#pragma once

// Thin RAII wrapper over the SQLite C API, private to lucent_catalog. <sqlite3.h> stays in
// sqlite.cpp: this header only forward-declares the SQLite handle types, so the catalog's tests
// can use the wrapper without the SQLite headers.
//
// Threading: SQLite connections are opened in multi-thread mode (SQLITE_OPEN_NOMUTEX). A
// Connection and its Statements must be used by one thread at a time; the catalog guarantees it
// (one writer under a mutex, read connections leased from a pool).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "lucent/core/error.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace lucent::catalog::detail {

// SQLite (extended) result code -> Lucent category; see the table in catalog.hpp.
[[nodiscard]] ErrorCode error_code_from_sqlite(int extended_code) noexcept;

class Connection;

// A prepared statement leased from a Connection's cache (or owned, when the cached one is busy).
// Parameters are 1-based, columns 0-based, as in SQLite. Bind failures are sticky and reported by
// the next step(). Destruction resets the statement and returns it to the cache.
class Statement {
public:
    Statement() noexcept = default;
    ~Statement();
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Statement& bind_null(int index) noexcept;
    Statement& bind_int64(int index, std::int64_t value) noexcept;
    // SQLite stores a NaN as NULL; callers validate finiteness first.
    Statement& bind_double(int index, double value) noexcept;
    Statement& bind_optional_double(int index, const std::optional<double>& value) noexcept;
    Statement& bind_text(int index, std::string_view value) noexcept;  // copied by SQLite
    Statement& bind_blob(int index, std::span<const std::uint8_t> value) noexcept;
    Statement& bind_optional_text(int index, const std::optional<std::string>& value) noexcept;

    // Advances: true when a row is available, false when the statement is done.
    [[nodiscard]] Expected<bool> step();
    // Steps until done (rows are ignored).
    [[nodiscard]] Status run();

    [[nodiscard]] bool is_null(int column) const noexcept;
    [[nodiscard]] std::int64_t column_int64(int column) const noexcept;
    [[nodiscard]] double column_double(int column) const noexcept;
    // The view is valid until the next step(), reset or destruction.
    [[nodiscard]] std::string_view column_text(int column) const noexcept;
    [[nodiscard]] std::span<const std::uint8_t> column_blob(int column) const noexcept;

    // Makes the statement runnable again with the same bindings cleared.
    void reset() noexcept;

private:
    friend class Connection;
    Statement(Connection* connection, sqlite3_stmt* statement, bool* in_use, std::string_view where) noexcept;
    void release() noexcept;

    Connection* connection_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
    bool* in_use_ = nullptr;  // cache slot flag; nullptr for an owned (uncached) statement
    std::string_view where_;  // static location string for errors
    int bind_error_ = 0;      // first failing sqlite3_bind_* result, 0 while healthy
};

enum class Access : std::uint8_t {
    ReadOnly = 1,         // SQLITE_OPEN_READONLY
    ReadWrite = 2,        // existing file only
    ReadWriteCreate = 3,  // create when missing
};

class Connection {
public:
    // Opens `path` with the "win32-longpath" VFS (long paths are passed in "\\?\" form), extended
    // result codes, defensive mode, untrusted schema, no double-quoted strings and no extension
    // loading, then sets busy_timeout. Never interprets the name as a URI. `where` is static.
    [[nodiscard]] static Expected<std::unique_ptr<Connection>> open(const std::filesystem::path& path,
                                                                    Access access,
                                                                    std::chrono::milliseconds busy_timeout,
                                                                    std::string_view where);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Runs one or more SQL statements without parameters (schema and pragmas only).
    [[nodiscard]] Status exec(std::string_view sql, std::string_view where);

    // A statement for `sql` (one statement), from the cache when available. `where` is static.
    [[nodiscard]] Expected<Statement> prepare(std::string_view sql, std::string_view where);

    // Runs a query returning one integer in the first column of the first row (NotFound-free:
    // Internal when there is no row).
    [[nodiscard]] Expected<std::int64_t> query_int64(std::string_view sql, std::string_view where);
    // Runs a query returning one text value in the first column of the first row.
    [[nodiscard]] Expected<std::string> query_text(std::string_view sql, std::string_view where);

    // An Error for result `rc` with SQLite's current message for this connection.
    [[nodiscard]] Error make_error(int rc, std::string_view where, std::string_view context = {}) const;

    [[nodiscard]] std::int64_t last_insert_rowid() const noexcept;
    [[nodiscard]] std::int64_t changes() const noexcept;
    [[nodiscard]] bool in_transaction() const noexcept;  // !sqlite3_get_autocommit
    [[nodiscard]] sqlite3* handle() const noexcept { return db_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    // Finalizes cached statements (they are prepared again on demand).
    void clear_statement_cache() noexcept;

    // Whether closing this connection, when it is the last one, checkpoints the WAL into the
    // database file (SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE). Disabled while a catalog that must not be
    // written is inspected.
    [[nodiscard]] Status set_checkpoint_on_close(bool enabled, std::string_view where);

private:
    Connection(sqlite3* db, std::filesystem::path path) noexcept;

    struct CachedStatement {
        sqlite3_stmt* statement = nullptr;
        bool in_use = false;
    };

    sqlite3* db_ = nullptr;
    std::filesystem::path path_;
    std::map<std::string, CachedStatement, std::less<>> cache_;
};

// Rolls back on destruction unless committed. Use one per transaction or savepoint.
class TransactionGuard {
public:
    enum class Kind : std::uint8_t {
        Deferred = 1,   // BEGIN (read transactions: the snapshot starts at the first read)
        Immediate = 2,  // BEGIN IMMEDIATE (write transactions: take the write lock now)
        Savepoint = 3,  // SAVEPOINT inside an open transaction (atomic multi-statement calls)
    };

    [[nodiscard]] static Expected<TransactionGuard> begin(Connection& connection, Kind kind,
                                                          std::string_view where);

    TransactionGuard(TransactionGuard&& other) noexcept;
    TransactionGuard& operator=(TransactionGuard&&) = delete;
    TransactionGuard(const TransactionGuard&) = delete;
    TransactionGuard& operator=(const TransactionGuard&) = delete;
    ~TransactionGuard();

    [[nodiscard]] Status commit();
    // Rolls back now (idempotent); errors are logged, never reported: SQLite may already have
    // rolled back after an I/O, busy or out-of-memory error.
    void rollback() noexcept;

private:
    TransactionGuard(Connection* connection, Kind kind, std::string_view where) noexcept
        : connection_(connection), kind_(kind), where_(where) {}

    Connection* connection_ = nullptr;  // nullptr once committed or rolled back
    Kind kind_ = Kind::Deferred;
    std::string_view where_;
};

// Online backup of the main database of `source` into the main database of `destination`
// (sqlite3_backup_init/step(-1)/finish): one pass on one snapshot of the source.
[[nodiscard]] Status copy_database(Connection& source, Connection& destination, std::string_view where);

// sqlite3_libversion(), for logs.
[[nodiscard]] std::string_view sqlite_version() noexcept;

}  // namespace lucent::catalog::detail
