#include "sqlite.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include <sqlite3.h>

#include "lucent/core/log.hpp"
#include "lucent/core/text.hpp"

namespace lucent::catalog::detail {
namespace {

// SQLite copies the bound value before sqlite3_bind_* returns (SQLITE_TRANSIENT). Spelled without
// the macro, whose C-style cast of -1 to a function pointer draws conversion warnings.
const sqlite3_destructor_type kTransient =
    reinterpret_cast<sqlite3_destructor_type>(static_cast<std::intptr_t>(-1));

// Paths at least this long (UTF-16 code units) are handed to SQLite in "\\?\" form. SQLite's
// default Windows VFS limits names to MAX_PATH; the "win32-longpath" VFS accepts verbatim paths up
// to the NT limit. The margin leaves room for the "-journal" and "-wal" suffixes SQLite appends.
constexpr std::size_t kVerbatimThreshold = 240;

// Upper bound of cached statements per connection. The catalog's statement texts form a small
// fixed set (a few dozen, plus one per list_items filter combination); the bound only guards
// against unbounded growth.
constexpr std::size_t kMaxCachedStatements = 256;

Expected<std::string> sqlite_file_name(const std::filesystem::path& path, std::string_view where) {
    const std::wstring& native = path.native();
    if (native.empty() || native.find(L'\0') != std::wstring::npos) {
        return fail(ErrorCode::InvalidArgument, "empty path or path with NUL characters", std::string(where));
    }
    std::wstring name = native;
    if (name.size() >= kVerbatimThreshold && !name.starts_with(L"\\\\?\\") && !name.starts_with(L"\\\\.\\")) {
        std::error_code error;
        const std::filesystem::path absolute = std::filesystem::absolute(path, error);
        if (error) {
            return fail(ErrorCode::InvalidArgument,
                        "cannot make the catalog path absolute: " + error.message(), std::string(where));
        }
        const std::wstring& full = absolute.native();
        if (full.starts_with(L"\\\\")) {
            name = L"\\\\?\\UNC\\" + full.substr(2);
        } else {
            name = L"\\\\?\\" + full;
        }
    }
    return core::wide_to_utf8(name);
}

bool is_transient(int rc) noexcept {
    const int primary = rc & 0xFF;
    return primary == SQLITE_BUSY || primary == SQLITE_LOCKED;
}

}  // namespace

ErrorCode error_code_from_sqlite(int extended_code) noexcept {
    switch (extended_code) {
        case SQLITE_CONSTRAINT_UNIQUE:
        case SQLITE_CONSTRAINT_PRIMARYKEY:
            return ErrorCode::AlreadyExists;
        case SQLITE_CONSTRAINT_FOREIGNKEY:
            return ErrorCode::NotFound;
        case SQLITE_IOERR_NOMEM:
            return ErrorCode::OutOfMemory;
        default:
            break;
    }
    switch (extended_code & 0xFF) {
        case SQLITE_OK:
            return ErrorCode::Internal;  // reporting success as an error is a Lucent defect
        case SQLITE_BUSY:
        case SQLITE_LOCKED:
            return ErrorCode::IoError;
        case SQLITE_CORRUPT:
        case SQLITE_NOTADB:
            return ErrorCode::Corrupt;
        case SQLITE_FULL:
            return ErrorCode::DiskFull;
        case SQLITE_NOMEM:
            return ErrorCode::OutOfMemory;
        case SQLITE_READONLY:
        case SQLITE_PERM:
        case SQLITE_AUTH:
            return ErrorCode::PermissionDenied;
        case SQLITE_CONSTRAINT:
            return ErrorCode::InvalidArgument;
        case SQLITE_TOOBIG:
            return ErrorCode::LimitExceeded;
        case SQLITE_INTERRUPT:
            return ErrorCode::Cancelled;
        case SQLITE_ERROR:
        case SQLITE_MISUSE:
        case SQLITE_RANGE:
        case SQLITE_MISMATCH:
        case SQLITE_SCHEMA:
        case SQLITE_INTERNAL:
            return ErrorCode::Internal;
        default:
            return ErrorCode::IoError;  // IOERR, CANTOPEN, PROTOCOL, ...
    }
}

// ---------------------------------------------------------------------------------------------
// Statement
// ---------------------------------------------------------------------------------------------

Statement::Statement(Connection* connection, sqlite3_stmt* statement, bool* in_use,
                     std::string_view where) noexcept
    : connection_(connection), statement_(statement), in_use_(in_use), where_(where) {}

Statement::~Statement() {
    release();
}

Statement::Statement(Statement&& other) noexcept
    : connection_(std::exchange(other.connection_, nullptr)),
      statement_(std::exchange(other.statement_, nullptr)),
      in_use_(std::exchange(other.in_use_, nullptr)),
      where_(other.where_),
      bind_error_(std::exchange(other.bind_error_, 0)) {}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        release();
        connection_ = std::exchange(other.connection_, nullptr);
        statement_ = std::exchange(other.statement_, nullptr);
        in_use_ = std::exchange(other.in_use_, nullptr);
        where_ = other.where_;
        bind_error_ = std::exchange(other.bind_error_, 0);
    }
    return *this;
}

void Statement::release() noexcept {
    if (statement_ == nullptr) {
        return;
    }
    if (in_use_ != nullptr) {
        ::sqlite3_reset(statement_);
        ::sqlite3_clear_bindings(statement_);
        *in_use_ = false;
    } else {
        ::sqlite3_finalize(statement_);
    }
    statement_ = nullptr;
    in_use_ = nullptr;
    connection_ = nullptr;
}

Statement& Statement::bind_null(int index) noexcept {
    const int rc = ::sqlite3_bind_null(statement_, index);
    if (rc != SQLITE_OK && bind_error_ == 0) {
        bind_error_ = rc;
    }
    return *this;
}

Statement& Statement::bind_int64(int index, std::int64_t value) noexcept {
    const int rc = ::sqlite3_bind_int64(statement_, index, value);
    if (rc != SQLITE_OK && bind_error_ == 0) {
        bind_error_ = rc;
    }
    return *this;
}

Statement& Statement::bind_double(int index, double value) noexcept {
    const int rc = ::sqlite3_bind_double(statement_, index, value);
    if (rc != SQLITE_OK && bind_error_ == 0) {
        bind_error_ = rc;
    }
    return *this;
}

Statement& Statement::bind_optional_double(int index, const std::optional<double>& value) noexcept {
    return value.has_value() ? bind_double(index, *value) : bind_null(index);
}

Statement& Statement::bind_text(int index, std::string_view value) noexcept {
    // A non-null pointer even for empty text, so the value is '' and not NULL.
    static constexpr char kEmpty[] = "";
    const char* data = value.empty() ? kEmpty : value.data();
    const int rc = ::sqlite3_bind_text64(statement_, index, data, static_cast<sqlite3_uint64>(value.size()),
                                         kTransient, SQLITE_UTF8);
    if (rc != SQLITE_OK && bind_error_ == 0) {
        bind_error_ = rc;
    }
    return *this;
}

Statement& Statement::bind_blob(int index, std::span<const std::uint8_t> value) noexcept {
    static constexpr std::uint8_t kEmpty[1] = {0};
    const void* data =
        value.empty() ? static_cast<const void*>(kEmpty) : static_cast<const void*>(value.data());
    const int rc =
        ::sqlite3_bind_blob64(statement_, index, data, static_cast<sqlite3_uint64>(value.size()), kTransient);
    if (rc != SQLITE_OK && bind_error_ == 0) {
        bind_error_ = rc;
    }
    return *this;
}

Statement& Statement::bind_optional_text(int index, const std::optional<std::string>& value) noexcept {
    return value.has_value() ? bind_text(index, *value) : bind_null(index);
}

Expected<bool> Statement::step() {
    if (statement_ == nullptr || connection_ == nullptr) {
        return fail(ErrorCode::Internal, "step on an empty statement", std::string(where_));
    }
    if (bind_error_ != 0) {
        return Unexpected(connection_->make_error(bind_error_, where_, "binding a parameter"));
    }
    const int rc = ::sqlite3_step(statement_);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    return Unexpected(connection_->make_error(rc, where_));
}

Status Statement::run() {
    for (;;) {
        Expected<bool> row = step();
        if (!row) {
            return Unexpected(std::move(row.error()));
        }
        if (!*row) {
            return ok();
        }
    }
}

bool Statement::is_null(int column) const noexcept {
    return ::sqlite3_column_type(statement_, column) == SQLITE_NULL;
}

std::int64_t Statement::column_int64(int column) const noexcept {
    return ::sqlite3_column_int64(statement_, column);
}

double Statement::column_double(int column) const noexcept {
    return ::sqlite3_column_double(statement_, column);
}

std::string_view Statement::column_text(int column) const noexcept {
    const unsigned char* text = ::sqlite3_column_text(statement_, column);
    const int bytes = ::sqlite3_column_bytes(statement_, column);
    if (text == nullptr || bytes <= 0) {
        return {};
    }
    return {reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes)};
}

std::span<const std::uint8_t> Statement::column_blob(int column) const noexcept {
    const void* blob = ::sqlite3_column_blob(statement_, column);
    const int bytes = ::sqlite3_column_bytes(statement_, column);
    if (blob == nullptr || bytes <= 0) {
        return {};
    }
    return {static_cast<const std::uint8_t*>(blob), static_cast<std::size_t>(bytes)};
}

void Statement::reset() noexcept {
    if (statement_ != nullptr) {
        ::sqlite3_reset(statement_);
        ::sqlite3_clear_bindings(statement_);
        bind_error_ = 0;
    }
}

// ---------------------------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------------------------

Connection::Connection(sqlite3* db, std::filesystem::path path) noexcept : db_(db), path_(std::move(path)) {}

Connection::~Connection() {
    clear_statement_cache();
    if (db_ != nullptr) {
        const int rc = ::sqlite3_close_v2(db_);
        if (rc != SQLITE_OK) {
            core::log_error("catalog.sqlite", "closing the connection to {} failed with SQLite code {}",
                            core::path_to_utf8_lossy(path_), rc);
        }
    }
}

Expected<std::unique_ptr<Connection>> Connection::open(const std::filesystem::path& path, Access access,
                                                       std::chrono::milliseconds busy_timeout,
                                                       std::string_view where) {
    Expected<std::string> name = sqlite_file_name(path, where);
    if (!name) {
        return Unexpected(std::move(name.error()));
    }
    int flags = SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_PRIVATECACHE | SQLITE_OPEN_EXRESCODE;
    switch (access) {
        case Access::ReadOnly:
            flags |= SQLITE_OPEN_READONLY;
            break;
        case Access::ReadWrite:
            flags |= SQLITE_OPEN_READWRITE;
            break;
        case Access::ReadWriteCreate:
            flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
            break;
    }
    const char* vfs = ::sqlite3_vfs_find("win32-longpath") != nullptr ? "win32-longpath" : nullptr;

    sqlite3* db = nullptr;
    const int rc = ::sqlite3_open_v2(name->c_str(), &db, flags, vfs);
    if (rc != SQLITE_OK) {
        std::string message = "cannot open " + core::path_to_utf8_lossy(path);
        if (db != nullptr) {
            message += ": ";
            message += core::sanitize_utf8(::sqlite3_errmsg(db));
            ::sqlite3_close_v2(db);
        }
        return fail(error_code_from_sqlite(rc), std::move(message), std::string(where), rc);
    }
    std::unique_ptr<Connection> connection(new Connection(db, path));

    ::sqlite3_extended_result_codes(db, 1);
    // Hardening (https://www.sqlite.org/security.html): no writable schema or other foot-guns,
    // schema functions limited to innocuous ones, string literals only in single quotes, no
    // extension loading.
    const struct {
        int option;
        int value;
    } config[] = {
        {SQLITE_DBCONFIG_DEFENSIVE, 1},
        {SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0},
        {SQLITE_DBCONFIG_DQS_DML, 0},
        {SQLITE_DBCONFIG_DQS_DDL, 0},
        {SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0},
    };
    for (const auto& entry : config) {
        const int config_rc = ::sqlite3_db_config(db, entry.option, entry.value, static_cast<int*>(nullptr));
        if (config_rc != SQLITE_OK) {
            return Unexpected(connection->make_error(config_rc, where, "configuring the connection"));
        }
    }
    const auto timeout_ms = busy_timeout.count();
    ::sqlite3_busy_timeout(
        db, static_cast<int>(std::min<std::int64_t>(timeout_ms, std::numeric_limits<int>::max())));
    return connection;
}

Status Connection::exec(std::string_view sql, std::string_view where) {
    const std::string text(sql);
    char* message = nullptr;
    const int rc = ::sqlite3_exec(db_, text.c_str(), nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        std::string detail =
            message != nullptr ? core::sanitize_utf8(message) : std::string(::sqlite3_errstr(rc));
        ::sqlite3_free(message);
        return fail(error_code_from_sqlite(rc), std::move(detail), std::string(where), rc);
    }
    return ok();
}

Expected<Statement> Connection::prepare(std::string_view sql, std::string_view where) {
    if (sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return fail(ErrorCode::Internal, "SQL text too long", std::string(where));
    }
    auto found = cache_.find(sql);
    if (found != cache_.end() && !found->second.in_use) {
        found->second.in_use = true;
        return Statement(this, found->second.statement, &found->second.in_use, where);
    }

    sqlite3_stmt* statement = nullptr;
    const char* tail = nullptr;
    const bool cache = found == cache_.end() && cache_.size() < kMaxCachedStatements;
    const int rc = ::sqlite3_prepare_v3(db_, sql.data(), static_cast<int>(sql.size()),
                                        cache ? SQLITE_PREPARE_PERSISTENT : 0u, &statement, &tail);
    if (rc != SQLITE_OK) {
        return Unexpected(make_error(rc, where, "preparing a statement"));
    }
    if (statement == nullptr) {
        return fail(ErrorCode::Internal, "empty SQL statement", std::string(where));
    }
    const std::string_view rest(tail, static_cast<std::size_t>(sql.data() + sql.size() - tail));
    if (rest.find_first_not_of(" \t\r\n;") != std::string_view::npos) {
        ::sqlite3_finalize(statement);
        return fail(ErrorCode::Internal, "prepare() takes exactly one SQL statement", std::string(where));
    }
    if (!cache) {
        return Statement(this, statement, nullptr, where);
    }
    auto [slot, inserted] = cache_.emplace(std::string(sql), CachedStatement{statement, true});
    (void)inserted;
    return Statement(this, statement, &slot->second.in_use, where);
}

Expected<std::int64_t> Connection::query_int64(std::string_view sql, std::string_view where) {
    Expected<Statement> statement = prepare(sql, where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    Expected<bool> row = statement->step();
    if (!row) {
        return Unexpected(std::move(row.error()));
    }
    if (!*row) {
        return fail(ErrorCode::Internal, "query returned no row", std::string(where));
    }
    return statement->column_int64(0);
}

Expected<std::string> Connection::query_text(std::string_view sql, std::string_view where) {
    Expected<Statement> statement = prepare(sql, where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    Expected<bool> row = statement->step();
    if (!row) {
        return Unexpected(std::move(row.error()));
    }
    if (!*row) {
        return fail(ErrorCode::Internal, "query returned no row", std::string(where));
    }
    return std::string(statement->column_text(0));
}

Error Connection::make_error(int rc, std::string_view where, std::string_view context) const {
    std::string message;
    if (!context.empty()) {
        message.append(context);
        message.append(": ");
    }
    // sqlite3_errmsg describes the most recent failure of this connection; fall back to the
    // generic text when it is unrelated (for example a bind failure).
    const int current = db_ != nullptr ? ::sqlite3_extended_errcode(db_) : SQLITE_OK;
    if (db_ != nullptr && current == rc) {
        message.append(core::sanitize_utf8(::sqlite3_errmsg(db_)));
    } else {
        message.append(::sqlite3_errstr(rc));
    }
    return Error{error_code_from_sqlite(rc), std::move(message), std::string(where), rc};
}

std::int64_t Connection::last_insert_rowid() const noexcept {
    return ::sqlite3_last_insert_rowid(db_);
}

std::int64_t Connection::changes() const noexcept {
    return ::sqlite3_changes64(db_);
}

bool Connection::in_transaction() const noexcept {
    return ::sqlite3_get_autocommit(db_) == 0;
}

void Connection::clear_statement_cache() noexcept {
    for (auto& [sql, cached] : cache_) {
        (void)sql;
        ::sqlite3_finalize(cached.statement);
    }
    cache_.clear();
}

Status Connection::set_checkpoint_on_close(bool enabled, std::string_view where) {
    const int rc = ::sqlite3_db_config(db_, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, enabled ? 0 : 1,
                                       static_cast<int*>(nullptr));
    if (rc != SQLITE_OK) {
        return Unexpected(make_error(rc, where, "configuring checkpoint on close"));
    }
    return ok();
}

// ---------------------------------------------------------------------------------------------
// TransactionGuard
// ---------------------------------------------------------------------------------------------

Expected<TransactionGuard> TransactionGuard::begin(Connection& connection, Kind kind,
                                                   std::string_view where) {
    std::string_view sql;
    switch (kind) {
        case Kind::Deferred:
            sql = "BEGIN";
            break;
        case Kind::Immediate:
            sql = "BEGIN IMMEDIATE";
            break;
        case Kind::Savepoint:
            if (!connection.in_transaction()) {
                return fail(ErrorCode::Internal, "savepoint outside a transaction", std::string(where));
            }
            sql = "SAVEPOINT lucent_operation";
            break;
    }
    Status status = connection.exec(sql, where);
    if (!status) {
        return Unexpected(std::move(status.error()));
    }
    return TransactionGuard(&connection, kind, where);
}

TransactionGuard::TransactionGuard(TransactionGuard&& other) noexcept
    : connection_(std::exchange(other.connection_, nullptr)), kind_(other.kind_), where_(other.where_) {}

TransactionGuard::~TransactionGuard() {
    rollback();
}

Status TransactionGuard::commit() {
    if (connection_ == nullptr) {
        return fail(ErrorCode::Internal, "transaction already finished", std::string(where_));
    }
    if (!connection_->in_transaction()) {
        connection_ = nullptr;
        return fail(ErrorCode::IoError,
                    "the transaction was rolled back after an earlier error; nothing was committed",
                    std::string(where_));
    }
    const std::string_view sql = kind_ == Kind::Savepoint ? "RELEASE lucent_operation" : "COMMIT";
    Status status = connection_->exec(sql, where_);
    if (!status) {
        rollback();
        return status;
    }
    connection_ = nullptr;
    return ok();
}

void TransactionGuard::rollback() noexcept {
    Connection* connection = std::exchange(connection_, nullptr);
    if (connection == nullptr || !connection->in_transaction()) {
        return;  // already finished, or SQLite rolled back on its own after an error
    }
    const std::string_view sql =
        kind_ == Kind::Savepoint ? "ROLLBACK TO lucent_operation; RELEASE lucent_operation" : "ROLLBACK";
    Status status = connection->exec(sql, where_);
    if (!status) {
        core::log_error("catalog.sqlite", "rollback failed: {}", status.error().describe());
    }
}

// ---------------------------------------------------------------------------------------------
// Backup
// ---------------------------------------------------------------------------------------------

Status copy_database(Connection& source, Connection& destination, std::string_view where) {
    // A busy or locked source or destination is retried for up to about two seconds; with the
    // catalog's WAL sources this only happens while another process checkpoints.
    constexpr int kAttempts = 200;
    for (int attempt = 0;; ++attempt) {
        sqlite3_backup* backup = ::sqlite3_backup_init(destination.handle(), "main", source.handle(), "main");
        if (backup == nullptr) {
            return Unexpected(destination.make_error(::sqlite3_extended_errcode(destination.handle()), where,
                                                     "starting the backup"));
        }
        const int step = ::sqlite3_backup_step(backup, -1);
        const int finish = ::sqlite3_backup_finish(backup);
        if (step == SQLITE_DONE && finish == SQLITE_OK) {
            return ok();
        }
        const int rc = step != SQLITE_DONE ? step : finish;
        if (is_transient(rc) && attempt + 1 < kAttempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        return Unexpected(destination.make_error(rc, where, "copying the database"));
    }
}

std::string_view sqlite_version() noexcept {
    return ::sqlite3_libversion();
}

}  // namespace lucent::catalog::detail
