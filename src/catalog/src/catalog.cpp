#include "lucent/catalog/catalog.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <format>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "lucent/core/log.hpp"
#include "lucent/core/storage.hpp"
#include "lucent/core/text.hpp"
#include "lucent/core/version.hpp"

#include "backup.hpp"
#include "catalog_impl.hpp"
#include "codec.hpp"
#include "files.hpp"
#include "schema.hpp"
#include "sqlite.hpp"

namespace lucent::catalog {

using detail::Access;
using detail::Connection;
using detail::TransactionGuard;

namespace {

constexpr std::string_view kWhereOpen = "catalog.open";
constexpr std::string_view kWhereRead = "catalog.read";
constexpr std::string_view kWhereWrite = "catalog.write";
constexpr std::string_view kWhereBackup = "catalog.backup";
constexpr std::string_view kWhereRestore = "catalog.restore";

constexpr std::chrono::milliseconds kMaxBusyTimeout{10 * 60 * 1000};
constexpr std::uint32_t kMaxReaders = 64;
constexpr std::uint32_t kMaxBackupsToKeep = 1000;
constexpr std::int64_t kJournalSizeLimitBytes = std::int64_t{64} << 20;

std::string quoted(const std::filesystem::path& path) {
    return "'" + core::path_to_utf8_lossy(path) + "'";
}

Error with_where(Error error, std::string_view where) {
    error.where = std::string(where);
    return error;
}

Expected<std::filesystem::path> absolute_path(const std::filesystem::path& path, std::string_view where,
                                              std::string_view what) {
    if (path.empty() || path.native().find(L'\0') != std::wstring::npos) {
        return fail(ErrorCode::InvalidArgument, std::format("{} is empty or contains NUL characters", what),
                    std::string(where));
    }
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("cannot make the {} absolute: {}", what, error.message()),
                    std::string(where));
    }
    absolute = absolute.lexically_normal();
    if (absolute.filename().empty()) {
        return fail(ErrorCode::InvalidArgument, std::format("the {} {} names a folder", what, quoted(path)),
                    std::string(where));
    }
    return absolute;
}

// Runs a user callback, converting exceptions at the module boundary.
template <class Tx>
Status invoke_guarded(const std::function<Status(Tx&)>& work, Tx& transaction, std::string_view where) {
    try {
        return work(transaction);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory in a catalog transaction", std::string(where));
    } catch (const std::exception& exception) {
        return fail(
            ErrorCode::Internal,
            std::string("exception in a catalog transaction: ") + core::sanitize_utf8(exception.what()),
            std::string(where));
    } catch (...) {
        return fail(ErrorCode::Internal, "unknown exception in a catalog transaction", std::string(where));
    }
}

Status configure_writer(Connection& connection, Durability durability) {
    if (Status s = connection.exec("PRAGMA foreign_keys = ON", kWhereOpen); !s) {
        return s;
    }
    Expected<std::string> mode = connection.query_text("PRAGMA journal_mode = WAL", kWhereOpen);
    if (!mode) {
        return Unexpected(std::move(mode.error()));
    }
    if (*mode != "wal") {
        return fail(ErrorCode::IoError, "cannot enable write-ahead logging (journal mode is '" + *mode + "')",
                    std::string(kWhereOpen));
    }
    const std::string_view synchronous =
        durability == Durability::Full ? "PRAGMA synchronous = FULL" : "PRAGMA synchronous = NORMAL";
    if (Status s = connection.exec(synchronous, kWhereOpen); !s) {
        return s;
    }
    return connection.exec(std::format("PRAGMA journal_size_limit = {}", kJournalSizeLimitBytes), kWhereOpen);
}

Status validate_options(const OpenOptions& options) {
    const auto invalid = [](std::string message) {
        return fail(ErrorCode::InvalidArgument, std::move(message), std::string(kWhereOpen));
    };
    if (options.mode != OpenMode::ReadWrite && options.mode != OpenMode::ReadOnly) {
        return invalid("unknown open mode");
    }
    if (options.newer_schema != NewerSchemaPolicy::OpenReadOnly &&
        options.newer_schema != NewerSchemaPolicy::Refuse) {
        return invalid("unknown newer-schema policy");
    }
    if (options.integrity_check != IntegrityCheck::None && options.integrity_check != IntegrityCheck::Quick &&
        options.integrity_check != IntegrityCheck::Full) {
        return invalid("unknown integrity check");
    }
    if (options.durability != Durability::Normal && options.durability != Durability::Full) {
        return invalid("unknown durability");
    }
    if (options.busy_timeout.count() < 0 || options.busy_timeout > kMaxBusyTimeout) {
        return invalid("busy_timeout must be 0 ... 10 minutes");
    }
    if (options.max_readers < 1 || options.max_readers > kMaxReaders) {
        return invalid("max_readers must be 1 ... 64");
    }
    if (options.backups_to_keep < 1 || options.backups_to_keep > kMaxBackupsToKeep) {
        return invalid("backups_to_keep must be 1 ... 1000");
    }
    if (!core::is_valid_utf8(options.app_version)) {
        return invalid("app_version is not valid UTF-8");
    }
    return ok();
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Catalog::Impl
// ---------------------------------------------------------------------------------------------

struct Catalog::Impl {
    OpenOptions options;  // normalised: absolute path, app_version filled in
    std::filesystem::path path;
    std::filesystem::path backup_directory;
    StorageKind storage_kind = StorageKind::Unknown;
    SchemaStatus schema;
    bool read_only = false;
    bool newer_schema = false;
    std::optional<Error> write_refusal;  // returned by write() when read_only
    detail::CatalogLock writer_lock;     // ReadWrite catalogs only

    // Writer: one connection, one transaction at a time.
    std::mutex writer_mutex;
    std::atomic<std::thread::id> writer_thread{};
    std::unique_ptr<Connection> writer;
    // A newer-schema catalog opened read-only keeps its inspection connection open (query_only, no
    // checkpoint on close) so the WAL index stays initialised for the read-only pool.
    std::unique_ptr<Connection> anchor;

    // Read pool.
    mutable std::mutex pool_mutex;
    mutable std::condition_variable pool_available;
    mutable std::vector<std::unique_ptr<Connection>> idle;
    mutable std::uint32_t readers_open = 0;

    class ReaderLease {
    public:
        ReaderLease(const Impl& impl, std::unique_ptr<Connection> connection) noexcept
            : impl_(&impl), connection_(std::move(connection)) {}
        ReaderLease(ReaderLease&& other) noexcept
            : impl_(other.impl_), connection_(std::move(other.connection_)) {}
        ReaderLease& operator=(ReaderLease&&) = delete;
        ~ReaderLease() {
            if (connection_ == nullptr) {
                return;
            }
            {
                std::lock_guard lock(impl_->pool_mutex);
                impl_->idle.push_back(std::move(connection_));
            }
            impl_->pool_available.notify_one();
        }
        [[nodiscard]] Connection& connection() const noexcept { return *connection_; }

    private:
        const Impl* impl_;
        std::unique_ptr<Connection> connection_;
    };

    [[nodiscard]] Expected<ReaderLease> acquire_reader(std::string_view where) const {
        std::unique_lock lock(pool_mutex);
        pool_available.wait(lock, [&] { return !idle.empty() || readers_open < options.max_readers; });
        if (!idle.empty()) {
            std::unique_ptr<Connection> connection = std::move(idle.back());
            idle.pop_back();
            return ReaderLease(*this, std::move(connection));
        }
        ++readers_open;
        lock.unlock();
        Expected<std::unique_ptr<Connection>> connection =
            Connection::open(path, Access::ReadOnly, options.busy_timeout, where);
        if (!connection) {
            lock.lock();
            --readers_open;
            lock.unlock();
            pool_available.notify_one();
            return Unexpected(std::move(connection.error()));
        }
        return ReaderLease(*this, std::move(*connection));
    }

    // Holds the writer mutex and marks the owning thread, so a nested write() can be detected.
    class WriterScope {
    public:
        explicit WriterScope(Impl& impl) : impl_(impl), lock_(impl.writer_mutex) {
            impl_.writer_thread.store(std::this_thread::get_id());
        }
        ~WriterScope() { impl_.writer_thread.store(std::thread::id{}); }
        WriterScope(const WriterScope&) = delete;
        WriterScope& operator=(const WriterScope&) = delete;

    private:
        Impl& impl_;
        std::lock_guard<std::mutex> lock_;
    };

    [[nodiscard]] Status check_writable(std::string_view where) const {
        if (read_only) {
            Error error = write_refusal.value_or(
                Error{ErrorCode::PermissionDenied, "the catalog is read-only", std::string(where), 0});
            return Unexpected(std::move(error));
        }
        if (writer_thread.load() == std::this_thread::get_id()) {
            return fail(ErrorCode::Internal, "nested catalog write on the same thread", std::string(where));
        }
        return ok();
    }
};

// ---------------------------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------------------------

Expected<std::unique_ptr<Catalog>> detail::CatalogAccess::open(const OpenOptions& options,
                                                               std::span<const Migration> migrations) {
    if (Status valid = validate_options(options); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    if (Status valid = validate_migrations(migrations); !valid) {
        return Unexpected(with_where(std::move(valid.error()), kWhereOpen));
    }
    const std::int32_t supported = migrations.back().version;

    auto impl = std::make_unique<Catalog::Impl>();
    impl->options = options;
    Expected<std::filesystem::path> path = absolute_path(options.path, kWhereOpen, "catalog path");
    if (!path) {
        return Unexpected(std::move(path.error()));
    }
    impl->path = std::move(*path);
    if (options.backup_directory.empty()) {
        impl->backup_directory = default_backup_directory(impl->path);
    } else {
        std::error_code error;
        impl->backup_directory =
            std::filesystem::absolute(options.backup_directory, error).lexically_normal();
        if (error) {
            return fail(ErrorCode::InvalidArgument, "invalid backup directory: " + error.message(),
                        std::string(kWhereOpen));
        }
    }
    if (impl->options.app_version.empty()) {
        impl->options.app_version = std::string(core::build_info().version);
    }
    impl->options.path = impl->path;
    impl->schema.supported_version = supported;

    // Location: a local disk only (blueprint section 7).
    impl->storage_kind = core::storage_kind(impl->path);
    if (Status location = check_catalog_storage(impl->storage_kind, impl->path); !location) {
        return Unexpected(std::move(location.error()));
    }
    if (impl->storage_kind == StorageKind::Removable) {
        core::log_warn("catalog",
                       "the catalog {} is on a removable drive; keep the drive connected while Lucent runs",
                       quoted(impl->path));
    }

    Expected<std::optional<detail::FileFacts>> facts = detail::file_facts(impl->path, kWhereOpen);
    if (!facts) {
        return Unexpected(std::move(facts.error()));
    }
    if (facts->has_value() && (*facts)->directory) {
        return fail(ErrorCode::InvalidArgument, "the catalog path " + quoted(impl->path) + " is a folder",
                    std::string(kWhereOpen));
    }
    const bool exists = facts->has_value();
    const bool read_write = options.mode == OpenMode::ReadWrite;
    if (!exists) {
        if (!read_write || !options.create_if_missing) {
            return fail(ErrorCode::NotFound, "no catalog at " + quoted(impl->path), std::string(kWhereOpen));
        }
        if (Status created = detail::ensure_directory(impl->path.parent_path(), kWhereOpen); !created) {
            return Unexpected(std::move(created.error()));
        }
    }
    if (read_write) {
        Expected<detail::CatalogLock> lock = detail::CatalogLock::acquire(impl->path, kWhereOpen);
        if (!lock) {
            return Unexpected(std::move(lock.error()));
        }
        impl->writer_lock = std::move(*lock);
    }

    // Inspect before writing anything: nothing below writes until the decision is made, and the
    // connection does not checkpoint when it closes.
    const Access access =
        !read_write ? Access::ReadOnly : (exists ? Access::ReadWrite : Access::ReadWriteCreate);
    Expected<std::unique_ptr<Connection>> opened =
        Connection::open(impl->path, access, options.busy_timeout, kWhereOpen);
    if (!opened) {
        return Unexpected(std::move(opened.error()));
    }
    std::unique_ptr<Connection> connection = std::move(*opened);
    if (read_write) {
        if (Status s = connection->set_checkpoint_on_close(false, kWhereOpen); !s) {
            return Unexpected(std::move(s.error()));
        }
    }
    Expected<DatabaseHeader> header = read_database_header(*connection, kWhereOpen);
    if (!header) {
        Error error = std::move(header.error());
        error.message = "cannot read " + quoted(impl->path) + ": " + error.message;
        return Unexpected(std::move(error));
    }
    if (header->kind == DatabaseKind::Foreign) {
        return fail(ErrorCode::Unsupported,
                    std::format("{} is a SQLite database but not a Lucent catalog (application id {:#x})",
                                quoted(impl->path), static_cast<std::uint32_t>(header->application_id)),
                    std::string(kWhereOpen));
    }
    const std::int32_t found = header->kind == DatabaseKind::Empty ? 0 : header->user_version;
    impl->schema.found_version = found;
    if (header->kind == DatabaseKind::Lucent) {
        if (Status integrity = check_integrity(*connection, options.integrity_check, kWhereOpen);
            !integrity) {
            Error error = std::move(integrity.error());
            error.message = quoted(impl->path) + ": " + error.message;
            return Unexpected(std::move(error));
        }
    }

    if (found > supported) {
        // A newer Lucent wrote this catalog: never write or migrate it.
        Expected<CatalogMeta> meta = read_catalog_meta(*connection, found, kWhereOpen);
        if (!meta) {
            return Unexpected(std::move(meta.error()));
        }
        Error reason{
            ErrorCode::VersionMismatch,
            std::format("catalog {} has schema v{} (written by Lucent {}), newer than the v{} this build "
                        "supports",
                        quoted(impl->path), found, meta->written_by.empty() ? "unknown" : meta->written_by,
                        supported),
            std::string(kWhereOpen), 0};
        if (options.newer_schema == NewerSchemaPolicy::Refuse) {
            return Unexpected(std::move(reason));
        }
        if (meta->min_reader_version > supported) {
            reason.message +=
                std::format("; it needs a reader for schema v{} or later", meta->min_reader_version);
            return Unexpected(std::move(reason));
        }
        reason.message += "; opened read-only";
        core::log_warn("catalog", "{}", reason.message);
        impl->read_only = true;
        impl->newer_schema = true;
        impl->write_refusal = reason;
        impl->write_refusal->where = std::string(kWhereWrite);
        impl->schema.read_only_reason = std::move(reason);
        impl->schema.version = found;
        impl->schema.created_by = meta->created_by;
        impl->schema.written_by = meta->written_by;
        if (read_write) {
            if (Status s = connection->exec("PRAGMA query_only = 1", kWhereOpen); !s) {
                return Unexpected(std::move(s.error()));
            }
            impl->anchor = std::move(connection);
            impl->writer_lock.release();  // this process does not write it; a newer Lucent may
        }
    } else if (!read_write) {
        if (found < supported) {
            return fail(found == 0 ? ErrorCode::Unsupported : ErrorCode::VersionMismatch,
                        found == 0
                            ? quoted(impl->path) + " is an empty database, not a Lucent catalog"
                            : std::format("catalog {} has schema v{}, older than v{}; open it read-write to "
                                          "migrate it",
                                          quoted(impl->path), found, supported),
                        std::string(kWhereOpen));
        }
        Expected<CatalogMeta> meta = read_catalog_meta(*connection, found, kWhereOpen);
        if (!meta) {
            return Unexpected(std::move(meta.error()));
        }
        impl->read_only = true;
        impl->write_refusal = Error{ErrorCode::PermissionDenied, "the catalog was opened read-only",
                                    std::string(kWhereWrite), 0};
        impl->schema.version = found;
        impl->schema.created_by = meta->created_by;
        impl->schema.written_by = meta->written_by;
    } else {
        if (found > 0 && found < supported) {
            Expected<BackupInfo> backup = detail::write_backup_into(
                *connection, impl->path, impl->backup_directory, BackupKind::PreMigration, found,
                options.backups_to_keep, options.busy_timeout, kWhereOpen);
            if (!backup) {
                Error error = std::move(backup.error());
                error.message = "cannot back up the catalog before migrating it: " + error.message;
                return Unexpected(std::move(error));
            }
            impl->schema.pre_migration_backup = std::move(*backup);
        }
        if (Status s = connection->set_checkpoint_on_close(true, kWhereOpen); !s) {
            return Unexpected(std::move(s.error()));
        }
        if (Status configured = configure_writer(*connection, options.durability); !configured) {
            return Unexpected(std::move(configured.error()));
        }
        if (found < supported) {
            Expected<std::vector<std::int32_t>> applied = apply_migrations(
                *connection, migrations, found, MigrationContext{impl->options.app_version, current_time()});
            if (!applied) {
                return Unexpected(std::move(applied.error()));
            }
            impl->schema.migrations_applied = std::move(*applied);
            impl->schema.created = found == 0;
        }
        Expected<CatalogMeta> meta = read_catalog_meta(*connection, supported, kWhereOpen);
        if (!meta) {
            return Unexpected(std::move(meta.error()));
        }
        impl->schema.version = supported;
        impl->schema.created_by = meta->created_by;
        impl->schema.written_by = meta->written_by;
        impl->writer = std::move(connection);
    }

    core::log_info("catalog", "opened catalog {} (schema v{}, {}{}, SQLite {})", quoted(impl->path),
                   impl->schema.version, core::to_string(impl->storage_kind),
                   impl->read_only ? ", read-only" : "", detail::sqlite_version());
    return std::unique_ptr<Catalog>(new Catalog(std::move(impl)));
}

Expected<std::unique_ptr<Catalog>> Catalog::open(const OpenOptions& options) {
    return detail::CatalogAccess::open(options, detail::builtin_migrations());
}

Catalog::Catalog(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Catalog::~Catalog() {
    if (impl_ == nullptr) {
        return;
    }
    {
        std::lock_guard lock(impl_->pool_mutex);
        impl_->idle.clear();
    }
    if (impl_->writer != nullptr) {
        // Lets SQLite refresh query-planner statistics it found useful during this session.
        if (Status optimized = impl_->writer->exec("PRAGMA optimize", kWhereWrite); !optimized) {
            core::log_warn("catalog", "PRAGMA optimize failed: {}", optimized.error().describe());
        }
        impl_->writer.reset();  // the last connection checkpoints and removes the WAL
    }
    impl_->anchor.reset();
    impl_->writer_lock.release();
}

const std::filesystem::path& Catalog::path() const noexcept {
    return impl_->path;
}
StorageKind Catalog::storage_kind() const noexcept {
    return impl_->storage_kind;
}
bool Catalog::is_read_only() const noexcept {
    return impl_->read_only;
}
const SchemaStatus& Catalog::schema() const noexcept {
    return impl_->schema;
}
const std::filesystem::path& Catalog::backup_directory() const noexcept {
    return impl_->backup_directory;
}

// ---------------------------------------------------------------------------------------------
// Transactions
// ---------------------------------------------------------------------------------------------

Status Catalog::read(const std::function<Status(ReadTransaction&)>& work) const {
    if (!work) {
        return fail(ErrorCode::InvalidArgument, "empty read callback", std::string(kWhereRead));
    }
    Expected<Impl::ReaderLease> lease = impl_->acquire_reader(kWhereRead);
    if (!lease) {
        return Unexpected(std::move(lease.error()));
    }
    Connection& connection = lease->connection();
    Expected<TransactionGuard> transaction =
        TransactionGuard::begin(connection, TransactionGuard::Kind::Deferred, kWhereRead);
    if (!transaction) {
        return Unexpected(std::move(transaction.error()));
    }
    ReadTransaction view(connection, impl_->newer_schema);
    Status result = invoke_guarded(work, view, kWhereRead);
    view.connection_ = nullptr;
    if (!result) {
        transaction->rollback();
        return result;
    }
    return transaction->commit();
}

Status Catalog::write(const std::function<Status(WriteTransaction&)>& work) {
    if (!work) {
        return fail(ErrorCode::InvalidArgument, "empty write callback", std::string(kWhereWrite));
    }
    if (Status writable = impl_->check_writable(kWhereWrite); !writable) {
        return writable;
    }
    Impl::WriterScope scope(*impl_);
    Connection& connection = *impl_->writer;
    Expected<TransactionGuard> transaction =
        TransactionGuard::begin(connection, TransactionGuard::Kind::Immediate, kWhereWrite);
    if (!transaction) {
        return Unexpected(std::move(transaction.error()));
    }
    WriteTransaction changes(connection);
    Status result = invoke_guarded(work, changes, kWhereWrite);
    changes.connection_ = nullptr;
    if (!result) {
        transaction->rollback();
        return result;
    }
    return transaction->commit();
}

Status Catalog::checkpoint() {
    if (Status writable = impl_->check_writable(kWhereWrite); !writable) {
        return writable;
    }
    Impl::WriterScope scope(*impl_);
    Expected<detail::Statement> statement =
        impl_->writer->prepare("PRAGMA wal_checkpoint(TRUNCATE)", kWhereWrite);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    Expected<bool> row = statement->step();
    if (!row) {
        return Unexpected(std::move(row.error()));
    }
    if (*row && statement->column_int64(0) != 0) {
        core::log_info("catalog", "WAL checkpoint was partial: readers hold older snapshots");
    }
    return ok();
}

// ---------------------------------------------------------------------------------------------
// Backups
// ---------------------------------------------------------------------------------------------

Expected<BackupInfo> Catalog::backup_to(const std::filesystem::path& destination) const {
    Expected<std::filesystem::path> target = absolute_path(destination, kWhereBackup, "backup path");
    if (!target) {
        return Unexpected(std::move(target.error()));
    }
    Expected<Impl::ReaderLease> lease = impl_->acquire_reader(kWhereBackup);
    if (!lease) {
        return Unexpected(std::move(lease.error()));
    }
    const Timestamp started = current_time();
    Expected<std::uint64_t> size = detail::write_verified_copy(
        lease->connection(), *target, impl_->schema.version, impl_->options.busy_timeout, kWhereBackup);
    if (!size) {
        return Unexpected(std::move(size.error()));
    }
    core::log_info("catalog", "wrote catalog backup {}", quoted(*target));
    return BackupInfo{std::move(*target), BackupKind::Manual, started, impl_->schema.version, *size};
}

Expected<BackupInfo> Catalog::create_backup() const {
    Expected<Impl::ReaderLease> lease = impl_->acquire_reader(kWhereBackup);
    if (!lease) {
        return Unexpected(std::move(lease.error()));
    }
    return detail::write_backup_into(
        lease->connection(), impl_->path, impl_->backup_directory, BackupKind::Manual, impl_->schema.version,
        impl_->options.backups_to_keep, impl_->options.busy_timeout, kWhereBackup);
}

Expected<std::vector<BackupInfo>> Catalog::list_backups() const {
    return detail::list_backups_in(impl_->backup_directory, impl_->path, kWhereBackup);
}

Expected<RestoreReport> Catalog::restore(const std::filesystem::path& catalog_path,
                                         const std::filesystem::path& backup_path,
                                         const RestoreOptions& options) {
    if (options.busy_timeout.count() < 0 || options.busy_timeout > kMaxBusyTimeout ||
        options.backups_to_keep < 1 || options.backups_to_keep > kMaxBackupsToKeep) {
        return fail(ErrorCode::InvalidArgument, "invalid restore options", std::string(kWhereRestore));
    }
    Expected<std::filesystem::path> catalog = absolute_path(catalog_path, kWhereRestore, "catalog path");
    if (!catalog) {
        return Unexpected(std::move(catalog.error()));
    }
    Expected<std::filesystem::path> backup = absolute_path(backup_path, kWhereRestore, "backup path");
    if (!backup) {
        return Unexpected(std::move(backup.error()));
    }
    {
        Expected<std::string> catalog_key = make_path_key(*catalog);
        Expected<std::string> backup_key = make_path_key(*backup);
        if (catalog_key && backup_key && *catalog_key == *backup_key) {
            return fail(ErrorCode::InvalidArgument, "a catalog cannot be restored from itself",
                        std::string(kWhereRestore));
        }
    }
    if (Status location = check_catalog_storage(core::storage_kind(*catalog), *catalog); !location) {
        return Unexpected(std::move(location.error()));
    }

    // 1. The backup must be a sound Lucent catalog this build can use.
    Expected<std::optional<detail::FileFacts>> backup_facts = detail::file_facts(*backup, kWhereRestore);
    if (!backup_facts) {
        return Unexpected(std::move(backup_facts.error()));
    }
    if (!backup_facts->has_value() || (*backup_facts)->directory) {
        return fail(ErrorCode::NotFound, "no backup file at " + quoted(*backup), std::string(kWhereRestore));
    }
    Expected<detail::DatabaseCheck> check =
        detail::verify_database_file(*backup, IntegrityCheck::Full, options.busy_timeout, kWhereRestore);
    if (!check) {
        Error error = std::move(check.error());
        error.message = "the backup " + quoted(*backup) + " cannot be used: " + error.message;
        return Unexpected(std::move(error));
    }
    if (check->application_id != detail::kApplicationId || check->user_version < 1) {
        return fail(ErrorCode::Unsupported, quoted(*backup) + " is not a Lucent catalog backup",
                    std::string(kWhereRestore));
    }
    if (check->user_version > kSchemaVersion) {
        return fail(ErrorCode::VersionMismatch,
                    std::format("the backup has schema v{}, newer than the v{} this build supports",
                                check->user_version, kSchemaVersion),
                    std::string(kWhereRestore));
    }
    RestoreReport report;
    report.restored_schema_version = check->user_version;

    // The writer lock: refuses a catalog that is open, and keeps it closed until we are done.
    if (Status folder = detail::ensure_directory(catalog->parent_path(), kWhereRestore); !folder) {
        return Unexpected(std::move(folder.error()));
    }
    Expected<detail::CatalogLock> lock = detail::CatalogLock::acquire(*catalog, kWhereRestore);
    if (!lock) {
        return Unexpected(std::move(lock.error()));
    }

    // 2. Keep the catalog being replaced.
    const std::filesystem::path backup_directory =
        options.backup_directory.empty() ? default_backup_directory(*catalog) : options.backup_directory;
    Expected<std::optional<detail::FileFacts>> catalog_facts = detail::file_facts(*catalog, kWhereRestore);
    if (!catalog_facts) {
        return Unexpected(std::move(catalog_facts.error()));
    }
    const bool replacing = catalog_facts->has_value();
    if (replacing) {
        if ((*catalog_facts)->directory) {
            return fail(ErrorCode::InvalidArgument, "the catalog path " + quoted(*catalog) + " is a folder",
                        std::string(kWhereRestore));
        }
        std::optional<Error> backup_failure;
        {
            Expected<std::unique_ptr<Connection>> current =
                Connection::open(*catalog, Access::ReadWrite, options.busy_timeout, kWhereRestore);
            Expected<detail::DatabaseHeader> header =
                current ? detail::read_database_header(**current, kWhereRestore)
                        : Expected<detail::DatabaseHeader>(Unexpected(current.error()));
            if (header && header->kind == detail::DatabaseKind::Lucent) {
                Expected<BackupInfo> saved = detail::write_backup_into(
                    **current, *catalog, backup_directory, BackupKind::PreRestore, header->user_version,
                    options.backups_to_keep, options.busy_timeout, kWhereRestore);
                if (saved) {
                    report.pre_restore_backup = std::move(*saved);
                } else {
                    backup_failure = std::move(saved.error());
                }
            } else {
                backup_failure = header ? Error{ErrorCode::Unsupported, "not a Lucent catalog", "", 0}
                                        : std::move(header.error());
            }
        }  // closes the connection: a readable catalog is checkpointed and its WAL removed
        if (backup_failure) {
            core::log_warn("catalog", "cannot back up the catalog being restored over ({}); moving it aside",
                           backup_failure->describe());
            std::wstring stamp = std::format(L".{}.damaged", detail::random_hex_suffix());
            for (const wchar_t* suffix : {L"", L"-wal"}) {
                std::filesystem::path from = *catalog;
                from += suffix;
                Expected<std::optional<detail::FileFacts>> present = detail::file_facts(from, kWhereRestore);
                if (!present) {
                    return Unexpected(std::move(present.error()));
                }
                if (!present->has_value()) {
                    continue;
                }
                std::filesystem::path to = *catalog;
                to += stamp;
                to += suffix;
                if (Status moved = detail::rename_no_replace(from, to, kWhereRestore); !moved) {
                    return Unexpected(std::move(moved.error()));
                }
                report.moved_aside.push_back(std::move(to));
            }
        }
    }

    // 3. Stage a verified copy next to the catalog, then put it in place.
    std::filesystem::path staged = *catalog;
    staged += L"." + detail::random_hex_suffix() + L".restore";
    {
        Expected<std::unique_ptr<Connection>> source =
            Connection::open(*backup, Access::ReadOnly, options.busy_timeout, kWhereRestore);
        if (!source) {
            return Unexpected(std::move(source.error()));
        }
        Expected<std::uint64_t> copied = detail::write_verified_copy(**source, staged, check->user_version,
                                                                     options.busy_timeout, kWhereRestore);
        if (!copied) {
            return Unexpected(std::move(copied.error()));
        }
    }
    for (const wchar_t* suffix : {L"-wal", L"-shm"}) {
        std::filesystem::path stale = *catalog;
        stale += suffix;
        if (Status removed = detail::delete_file_if_exists(stale, kWhereRestore); !removed) {
            (void)detail::delete_file_if_exists(staged, kWhereRestore);
            return Unexpected(std::move(removed.error()));
        }
    }
    Expected<std::optional<detail::FileFacts>> still_there = detail::file_facts(*catalog, kWhereRestore);
    Status installed = still_there && still_there->has_value()
                           ? detail::rename_replace(staged, *catalog, kWhereRestore)
                           : detail::rename_no_replace(staged, *catalog, kWhereRestore);
    if (!installed) {
        (void)detail::delete_file_if_exists(staged, kWhereRestore);
        return Unexpected(std::move(installed.error()));
    }
    core::log_info("catalog", "restored catalog {} from {} (schema v{})", quoted(*catalog), quoted(*backup),
                   report.restored_schema_version);
    return report;
}

}  // namespace lucent::catalog
