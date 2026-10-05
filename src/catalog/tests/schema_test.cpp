// Opening, creating and migrating catalogs; newer schemas; foreign and damaged databases.

#include "schema.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include "lucent/core/hash.hpp"

#include "backup.hpp"
#include "catalog_impl.hpp"
#include "catalog_test_support.hpp"

namespace lucent::catalog {
namespace {

using namespace test;

// The builtin migrations plus a synthetic next version (kSchemaVersion + 1) that adds a column.
constexpr std::int32_t kNext = kSchemaVersion + 1;

Status add_note_column(detail::Connection& connection) {
    return connection.exec("ALTER TABLE items ADD COLUMN note TEXT NOT NULL DEFAULT ''", "test.migrate");
}

Status create_then_fail(detail::Connection& connection) {
    if (Status s = connection.exec("CREATE TABLE half_done (x INTEGER) STRICT", "test.migrate"); !s) {
        return s;
    }
    return fail(ErrorCode::IoError, "simulated failure in the middle of a migration", "test.migrate");
}

Status orphan_item(detail::Connection& connection) {
    return connection.exec(
        "INSERT INTO items (source_id, subimage_index, make, model, lens, width, height, orientation, "
        "format, "
        "support_status, support_reasons_json, decoder_id, decoder_version, probed_at) "
        "VALUES (999999, 0, '', '', '', 0, 0, 1, 'dng', 'supported', '[]', 'x', '1', 0)",
        "test.migrate");
}

std::vector<detail::Migration> with_next(Status (*apply)(detail::Connection&),
                                         std::int32_t min_reader = kSchemaVersion) {
    std::vector<detail::Migration> migrations(detail::builtin_migrations().begin(),
                                              detail::builtin_migrations().end());
    migrations.push_back(detail::Migration{kNext, min_reader, "synthetic next version for tests", apply});
    return migrations;
}

std::string file_digest(const std::filesystem::path& path) {
    Expected<core::FileHashResult> hash = core::hash_file(path, core::CancellationToken{});
    return hash ? core::to_hex(hash->digest) : hash.error().describe();
}

// Marks a closed catalog as written by a future Lucent with schema `version`.
void pretend_newer(const std::filesystem::path& path, int version, int min_reader) {
    std::unique_ptr<detail::Connection> raw = raw_connection(path);
    ASSERT_NE(raw, nullptr);
    LUCENT_ASSERT_OK(raw->exec(std::format("PRAGMA user_version = {}", version), "test"));
    LUCENT_ASSERT_OK(
        raw->exec(std::format("UPDATE catalog_meta SET value = '{}' WHERE key = 'schema_version';"
                              "UPDATE catalog_meta SET value = '{}' WHERE key = 'min_reader_version';"
                              "UPDATE catalog_meta SET value = '9.0.0' WHERE key = 'written_by';",
                              version, min_reader),
                  "test"));
}

TEST(CatalogSchema, CreatesANewCatalogAtTheCurrentSchema) {
    TempDir dir;
    const std::filesystem::path path = dir / L"Lucent" / L"Catalog" / L"lucent.catalog";  // folders created
    {
        std::unique_ptr<Catalog> catalog = open_or_fail(path);
        ASSERT_NE(catalog, nullptr);
        const SchemaStatus& schema = catalog->schema();
        EXPECT_TRUE(schema.created);
        EXPECT_EQ(schema.found_version, 0);
        EXPECT_EQ(schema.version, kSchemaVersion);
        EXPECT_EQ(schema.supported_version, kSchemaVersion);
        EXPECT_EQ(schema.migrations_applied, (std::vector<std::int32_t>{1, 2, 3, 4, 5}));
        EXPECT_FALSE(schema.pre_migration_backup.has_value());  // nothing to back up
        EXPECT_EQ(schema.created_by, "0.1.0-test");
        EXPECT_EQ(schema.written_by, "0.1.0-test");
        EXPECT_FALSE(catalog->is_read_only());
        EXPECT_EQ(catalog->storage_kind(), StorageKind::Fixed);
        EXPECT_EQ(catalog->path(), path);
        EXPECT_TRUE(std::filesystem::exists(path));
        EXPECT_TRUE(std::filesystem::exists(path.parent_path() / L"lucent.catalog.lock"));
    }
    EXPECT_FALSE(std::filesystem::exists(path.parent_path() / L"lucent.catalog.lock"));  // deleted on close
    EXPECT_FALSE(std::filesystem::exists(path.parent_path() / L"backups"));

    std::unique_ptr<detail::Connection> raw = raw_connection(path, detail::Access::ReadOnly);
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(query_int(*raw, "PRAGMA application_id"), detail::kApplicationId);
    EXPECT_EQ(query_int(*raw, "PRAGMA user_version"), kSchemaVersion);
    EXPECT_EQ(query_text(*raw, "PRAGMA journal_mode"), "wal");  // persistent
    EXPECT_EQ(query_text(*raw, "SELECT value FROM catalog_meta WHERE key = 'schema_version'"),
              std::to_string(kSchemaVersion));
    // v2's recipes change what renders mean: schema-v1 readers must refuse it. v3 (the batch
    // journal) is additive and keeps the requirement at 2.
    EXPECT_EQ(query_text(*raw, "SELECT value FROM catalog_meta WHERE key = 'min_reader_version'"), "2");
    EXPECT_EQ(query_text(*raw, "SELECT value FROM catalog_meta WHERE key = 'created_by'"), "0.1.0-test");
    EXPECT_EQ(query_int(*raw,
                        "SELECT count(*) FROM schema_history WHERE version IN (1, 2) AND app_version = "
                        "'0.1.0-test'"),
              2);
    for (const char* table :
         {"sources", "items", "imports", "catalog_meta", "schema_history", "recipe_revisions", "batches", "jobs",
          "job_events", "publication_journal", "receipts"}) {
        EXPECT_EQ(
            query_int(*raw,
                      std::format("SELECT count(*) FROM sqlite_schema WHERE type = 'table' AND name = '{}'",
                                  table)),
            1)
            << table;
    }
    EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM sqlite_schema WHERE name = 'sources_by_path_key'"), 1);
    EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM sqlite_schema WHERE name = 'sources_by_sha256'"), 1);
}

TEST(CatalogSchema, ReopeningACurrentCatalogAppliesNothing) {
    TempDir dir;
    const std::filesystem::path path = dir / L"lucent.catalog";
    {
        ASSERT_NE(open_or_fail(path), nullptr);
    }
    std::unique_ptr<Catalog> catalog = open_or_fail(path);
    ASSERT_NE(catalog, nullptr);
    EXPECT_FALSE(catalog->schema().created);
    EXPECT_EQ(catalog->schema().found_version, kSchemaVersion);
    EXPECT_TRUE(catalog->schema().migrations_applied.empty());
    EXPECT_FALSE(catalog->schema().pre_migration_backup.has_value());
}

TEST(CatalogSchema, MigratesAnExistingEmptyFileFromV0) {
    TempDir dir;
    const std::filesystem::path path = dir / L"empty.catalog";
    write_bytes(path, {});  // a zero-byte file is an empty SQLite database
    std::unique_ptr<Catalog> catalog = open_or_fail(path);
    ASSERT_NE(catalog, nullptr);
    EXPECT_TRUE(catalog->schema().created);
    EXPECT_EQ(catalog->schema().version, kSchemaVersion);
}

TEST(CatalogSchema, RefusesAForeignSqliteDatabaseWithoutChangingIt) {
    TempDir dir;
    const std::filesystem::path path = dir / L"other.db";
    {
        std::unique_ptr<detail::Connection> raw = raw_connection(path, detail::Access::ReadWriteCreate);
        ASSERT_NE(raw, nullptr);
        LUCENT_ASSERT_OK(
            raw->exec("CREATE TABLE notes (x TEXT); INSERT INTO notes VALUES ('keep me')", "test"));
    }
    const std::string before = file_digest(path);
    Expected<std::unique_ptr<Catalog>> catalog = Catalog::open(options_for(path));
    ASSERT_FALSE(catalog);
    EXPECT_EQ(catalog.error().code, ErrorCode::Unsupported) << catalog.error().describe();
    EXPECT_EQ(file_digest(path), before);
}

TEST(CatalogSchema, RefusesAFileThatIsNotADatabase) {
    TempDir dir;
    const std::filesystem::path path = dir / L"text.catalog";
    write_bytes(path, std::vector<char>(4096, 'x'));
    Expected<std::unique_ptr<Catalog>> catalog = Catalog::open(options_for(path));
    ASSERT_FALSE(catalog);
    EXPECT_EQ(catalog.error().code, ErrorCode::Corrupt) << catalog.error().describe();
}

TEST(CatalogSchema, MissingFileWithoutCreateIsNotFound) {
    TempDir dir;
    OpenOptions options = options_for(dir / L"absent" / L"lucent.catalog");
    options.create_if_missing = false;
    Expected<std::unique_ptr<Catalog>> catalog = Catalog::open(options);
    ASSERT_FALSE(catalog);
    EXPECT_EQ(catalog.error().code, ErrorCode::NotFound);
    options.mode = OpenMode::ReadOnly;
    options.create_if_missing = true;  // ignored for ReadOnly
    catalog = Catalog::open(options);
    ASSERT_FALSE(catalog);
    EXPECT_EQ(catalog.error().code, ErrorCode::NotFound);
    EXPECT_FALSE(std::filesystem::exists(dir / L"absent"));
}

TEST(CatalogSchema, InvalidOptionsAreRejected) {
    TempDir dir;
    const auto expect_invalid = [&](OpenOptions options) {
        Expected<std::unique_ptr<Catalog>> catalog = Catalog::open(options);
        ASSERT_FALSE(catalog);
        EXPECT_EQ(catalog.error().code, ErrorCode::InvalidArgument) << catalog.error().describe();
    };
    OpenOptions options = options_for(dir / L"lucent.catalog");
    options.max_readers = 0;
    expect_invalid(options);
    options = options_for(dir / L"lucent.catalog");
    options.backups_to_keep = 0;
    expect_invalid(options);
    options = options_for(dir / L"lucent.catalog");
    options.busy_timeout = std::chrono::milliseconds(-1);
    expect_invalid(options);
    options = options_for(dir / L"lucent.catalog");
    options.app_version = "\xFF";
    expect_invalid(options);
    expect_invalid(options_for(L""));
    expect_invalid(options_for(dir.path()));  // a folder
}

TEST(CatalogSchema, BacksUpBeforeMigratingAnOlderCatalog) {
    TempDir dir;
    const std::filesystem::path path = dir / L"lucent.catalog";
    {
        std::unique_ptr<Catalog> catalog = open_or_fail(path);
        ASSERT_NE(catalog, nullptr);
        LUCENT_ASSERT_OK(populate(*catalog, dir / L"photos", 1, 25));
    }
    const std::vector<detail::Migration> migrations = with_next(&add_note_column);
    Expected<std::unique_ptr<Catalog>> migrated = detail::CatalogAccess::open(options_for(path), migrations);
    LUCENT_ASSERT_OK(migrated);
    const SchemaStatus& schema = (*migrated)->schema();
    EXPECT_EQ(schema.found_version, kSchemaVersion);
    EXPECT_EQ(schema.version, kNext);
    EXPECT_EQ(schema.supported_version, kNext);
    EXPECT_EQ(schema.migrations_applied, std::vector<std::int32_t>{kNext});
    ASSERT_TRUE(schema.pre_migration_backup.has_value());
    const BackupInfo backup = *schema.pre_migration_backup;  // a copy: the catalog is closed below
    EXPECT_EQ(backup.kind, BackupKind::PreMigration);
    EXPECT_EQ(backup.schema_version, kSchemaVersion);
    EXPECT_EQ(backup.path.parent_path(), dir / L"backups");
    EXPECT_TRUE(std::filesystem::exists(backup.path));
    EXPECT_GT(backup.size_bytes, 0u);
    // The data survived the migration.
    Expected<std::uint64_t> count =
        (*migrated)->query([](ReadTransaction& tx) { return tx.count_sources(); });
    LUCENT_ASSERT_OK(count);
    EXPECT_EQ(*count, 25u);
    Expected<std::vector<BackupInfo>> listed = (*migrated)->list_backups();
    LUCENT_ASSERT_OK(listed);
    ASSERT_EQ(listed->size(), 1u);
    EXPECT_EQ(listed->front().path, backup.path);
    migrated->reset();

    // The migrated file: the next version with history and the new column.
    {
        std::unique_ptr<detail::Connection> raw = raw_connection(path, detail::Access::ReadOnly);
        ASSERT_NE(raw, nullptr);
        EXPECT_EQ(query_int(*raw, "PRAGMA user_version"), kNext);
        EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM schema_history"), kNext);
        EXPECT_EQ(query_text(*raw, "SELECT value FROM catalog_meta WHERE key = 'schema_version'"),
                  std::to_string(kNext));
        EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM items WHERE note = ''"), 25);
    }
    // The backup: a self-contained catalog of the previous version with the pre-migration data,
    // openable by this build.
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(backup.path.native() + L"-wal")));
    OpenOptions read_backup = options_for(backup.path);
    read_backup.mode = OpenMode::ReadOnly;
    read_backup.integrity_check = IntegrityCheck::Full;
    Expected<std::unique_ptr<Catalog>> old = Catalog::open(read_backup);
    LUCENT_ASSERT_OK(old);
    EXPECT_EQ((*old)->schema().version, kSchemaVersion);
    Expected<std::uint64_t> old_count = (*old)->query([](ReadTransaction& tx) { return tx.count_sources(); });
    LUCENT_ASSERT_OK(old_count);
    EXPECT_EQ(*old_count, 25u);
}

TEST(CatalogSchema, AFailedMigrationRollsBackAndKeepsTheBackup) {
    TempDir dir;
    const std::filesystem::path path = dir / L"lucent.catalog";
    {
        std::unique_ptr<Catalog> catalog = open_or_fail(path);
        ASSERT_NE(catalog, nullptr);
        LUCENT_ASSERT_OK(populate(*catalog, dir / L"photos", 1, 10));
    }
    const std::vector<detail::Migration> migrations = with_next(&create_then_fail);
    Expected<std::unique_ptr<Catalog>> migrated = detail::CatalogAccess::open(options_for(path), migrations);
    ASSERT_FALSE(migrated);
    EXPECT_EQ(migrated.error().code, ErrorCode::IoError);
    EXPECT_EQ(migrated.error().where, "catalog.migrate");
    EXPECT_NE(migrated.error().message.find(std::format("v{}", kNext)), std::string::npos)
        << migrated.error().message;

    {
        std::unique_ptr<detail::Connection> raw = raw_connection(path, detail::Access::ReadOnly);
        ASSERT_NE(raw, nullptr);
        EXPECT_EQ(query_int(*raw, "PRAGMA user_version"), kSchemaVersion);
        EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM sqlite_schema WHERE name = 'half_done'"), 0);
        EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM schema_history"), kSchemaVersion);
        EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM sources"), 10);
    }
    Expected<std::vector<BackupInfo>> backups = detail::list_backups_in(dir / L"backups", path, "test");
    LUCENT_ASSERT_OK(backups);
    ASSERT_EQ(backups->size(), 1u);
    EXPECT_EQ(backups->front().kind, BackupKind::PreMigration);

    // The current build still opens it.
    std::unique_ptr<Catalog> reopened = open_or_fail(path);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->schema().version, kSchemaVersion);
}

TEST(CatalogSchema, AMigrationThatBreaksForeignKeysIsRolledBack) {
    TempDir dir;
    const std::filesystem::path path = dir / L"lucent.catalog";
    {
        ASSERT_NE(open_or_fail(path), nullptr);
    }
    const std::vector<detail::Migration> migrations = with_next(&orphan_item);
    Expected<std::unique_ptr<Catalog>> migrated = detail::CatalogAccess::open(options_for(path), migrations);
    ASSERT_FALSE(migrated);
    EXPECT_EQ(migrated.error().code, ErrorCode::Corrupt) << migrated.error().describe();
    std::unique_ptr<detail::Connection> raw = raw_connection(path, detail::Access::ReadOnly);
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(query_int(*raw, "PRAGMA user_version"), kSchemaVersion);
    EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM items"), 0);
}

TEST(CatalogSchema, ANewerSchemaOpensReadOnlyAndIsNeverWritten) {
    TempDir dir;
    const std::filesystem::path path = dir / L"lucent.catalog";
    {
        std::unique_ptr<Catalog> catalog = open_or_fail(path);
        ASSERT_NE(catalog, nullptr);
        LUCENT_ASSERT_OK(populate(*catalog, dir / L"photos", 1, 5));
    }
    pretend_newer(path, 7, 1);
    const std::string before = file_digest(path);
    {
        std::unique_ptr<Catalog> catalog = open_or_fail(path);
        ASSERT_NE(catalog, nullptr);
        EXPECT_TRUE(catalog->is_read_only());
        EXPECT_EQ(catalog->schema().found_version, 7);
        EXPECT_EQ(catalog->schema().version, 7);
        EXPECT_TRUE(catalog->schema().migrations_applied.empty());
        EXPECT_EQ(catalog->schema().written_by, "9.0.0");
        ASSERT_TRUE(catalog->schema().read_only_reason.has_value());
        EXPECT_EQ(catalog->schema().read_only_reason->code, ErrorCode::VersionMismatch);

        // Reads work.
        Expected<std::uint64_t> count =
            catalog->query([](ReadTransaction& tx) { return tx.count_sources(); });
        LUCENT_ASSERT_OK(count);
        EXPECT_EQ(*count, 5u);
        Expected<std::optional<SourceRecord>> found = catalog->query(
            [&](ReadTransaction& tx) { return tx.find_source_by_path(dir / L"photos" / L"IMG_00003.CR3"); });
        LUCENT_ASSERT_OK(found);
        EXPECT_TRUE(found->has_value());

        // Writes and checkpoints are refused with the recorded reason.
        const Status write = catalog->write([&](WriteTransaction& tx) -> Status {
            return tx.upsert_source(observation(dir / L"photos" / L"new.cr3", 99)).map([](auto&&) {});
        });
        ASSERT_FALSE(write);
        EXPECT_EQ(write.error().code, ErrorCode::VersionMismatch);
        EXPECT_FALSE(catalog->checkpoint());
        // No writer lock: a newer Lucent may still open it for writing.
        EXPECT_FALSE(std::filesystem::exists(dir / L"lucent.catalog.lock"));
    }
    EXPECT_EQ(file_digest(path), before);
    EXPECT_FALSE(std::filesystem::exists(dir / L"backups"));  // no migration, so no backup
    std::unique_ptr<detail::Connection> raw = raw_connection(path, detail::Access::ReadOnly);
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(query_int(*raw, "PRAGMA user_version"), 7);
}

TEST(CatalogSchema, ANewerSchemaIsRefusedWhenThePolicySaysSo) {
    TempDir dir;
    const std::filesystem::path path = dir / L"lucent.catalog";
    {
        ASSERT_NE(open_or_fail(path), nullptr);
    }
    pretend_newer(path, kNext, 1);
    OpenOptions options = options_for(path);
    options.newer_schema = NewerSchemaPolicy::Refuse;
    Expected<std::unique_ptr<Catalog>> catalog = Catalog::open(options);
    ASSERT_FALSE(catalog);
    EXPECT_EQ(catalog.error().code, ErrorCode::VersionMismatch);
}

TEST(CatalogSchema, ANewerSchemaThatOlderReadersCannotReadIsRefused) {
    TempDir dir;
    const std::filesystem::path path = dir / L"lucent.catalog";
    {
        ASSERT_NE(open_or_fail(path), nullptr);
    }
    pretend_newer(path, kSchemaVersion + 2, kNext);  // needs at least a reader of the next version
    for (const OpenMode mode : {OpenMode::ReadWrite, OpenMode::ReadOnly}) {
        OpenOptions options = options_for(path);
        options.mode = mode;
        Expected<std::unique_ptr<Catalog>> catalog = Catalog::open(options);
        ASSERT_FALSE(catalog);
        EXPECT_EQ(catalog.error().code, ErrorCode::VersionMismatch);
        EXPECT_NE(catalog.error().message.find(std::format("v{}", kNext)), std::string::npos)
            << catalog.error().message;
    }
}

TEST(CatalogSchema, ReadOnlyModeRefusesWritesAndOlderSchemas) {
    TempDir dir;
    const std::filesystem::path path = dir / L"lucent.catalog";
    {
        std::unique_ptr<Catalog> catalog = open_or_fail(path);
        ASSERT_NE(catalog, nullptr);
        LUCENT_ASSERT_OK(populate(*catalog, dir / L"photos", 1, 3));
    }
    OpenOptions options = options_for(path);
    options.mode = OpenMode::ReadOnly;
    {
        Expected<std::unique_ptr<Catalog>> catalog = Catalog::open(options);
        LUCENT_ASSERT_OK(catalog);
        EXPECT_TRUE((*catalog)->is_read_only());
        EXPECT_FALSE((*catalog)->schema().read_only_reason.has_value());
        const Status write = (*catalog)->write([](WriteTransaction&) { return ok(); });
        ASSERT_FALSE(write);
        EXPECT_EQ(write.error().code, ErrorCode::PermissionDenied);
        Expected<std::uint64_t> count =
            (*catalog)->query([](ReadTransaction& tx) { return tx.count_sources(); });
        LUCENT_ASSERT_OK(count);
        EXPECT_EQ(*count, 3u);
    }
    // A current catalog opened read-only by a build that supports the next version cannot be
    // migrated: refused.
    const std::vector<detail::Migration> migrations = with_next(&add_note_column);
    Expected<std::unique_ptr<Catalog>> older = detail::CatalogAccess::open(options, migrations);
    ASSERT_FALSE(older);
    EXPECT_EQ(older.error().code, ErrorCode::VersionMismatch);
}

TEST(CatalogSchema, OnlyOneWriterAtATime) {
    TempDir dir;
    const std::filesystem::path path = dir / L"lucent.catalog";
    std::unique_ptr<Catalog> first = open_or_fail(path);
    ASSERT_NE(first, nullptr);
    Expected<std::unique_ptr<Catalog>> second = Catalog::open(options_for(path));
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code, ErrorCode::AlreadyExists) << second.error().describe();
    // Different spelling of the same path: same lock file.
    Expected<std::unique_ptr<Catalog>> upper = Catalog::open(options_for(dir / L"LUCENT.CATALOG"));
    ASSERT_FALSE(upper);
    EXPECT_EQ(upper.error().code, ErrorCode::AlreadyExists);
    // Readers are welcome.
    OpenOptions read_only = options_for(path);
    read_only.mode = OpenMode::ReadOnly;
    Expected<std::unique_ptr<Catalog>> reader = Catalog::open(read_only);
    LUCENT_ASSERT_OK(reader);
    first.reset();
    std::unique_ptr<Catalog> again = open_or_fail(path);
    EXPECT_NE(again, nullptr);
}

TEST(CatalogSchema, UnicodeCatalogPath) {
    TempDir dir;
    const std::filesystem::path path = dir / L"Каталог 写真 é" / L"lucent ✓ 📷.catalog";
    {
        std::unique_ptr<Catalog> catalog = open_or_fail(path);
        ASSERT_NE(catalog, nullptr);
        LUCENT_ASSERT_OK(populate(*catalog, dir / L"Фото", 1, 3));
        Expected<BackupInfo> backup = catalog->create_backup();
        LUCENT_ASSERT_OK(backup);
        EXPECT_TRUE(std::filesystem::exists(backup->path));
        EXPECT_EQ(backup->path.parent_path(), path.parent_path() / L"backups");
    }
    std::unique_ptr<Catalog> reopened = open_or_fail(path);
    ASSERT_NE(reopened, nullptr);
    Expected<std::uint64_t> count = reopened->query([](ReadTransaction& tx) { return tx.count_sources(); });
    LUCENT_ASSERT_OK(count);
    EXPECT_EQ(*count, 3u);
}

TEST(CatalogSchema, CatalogPathLongerThanMaxPath) {
    TempDir dir;
    std::filesystem::path folder = dir.path();
    while (folder.native().size() < 300) {
        folder /= L"a-rather-long-folder-name-for-testing";
    }
    const std::filesystem::path path = folder / L"lucent.catalog";
    ASSERT_GT(path.native().size(), 260u);
    {
        std::unique_ptr<Catalog> catalog = open_or_fail(path);
        ASSERT_NE(catalog, nullptr);
        LUCENT_ASSERT_OK(populate(*catalog, dir / L"photos", 1, 3));
        LUCENT_ASSERT_OK(catalog->create_backup());
    }
    std::unique_ptr<Catalog> reopened = open_or_fail(path);
    ASSERT_NE(reopened, nullptr);
    EXPECT_FALSE(reopened->schema().created);
}

// Integrity check on open: a copy of a sound catalog with one damaged page is refused as Corrupt,
// before anything is written to it, while the original still opens.
TEST(CatalogSchema, IntegrityCheckRejectsADamagedCopy) {
    TempDir dir;
    const std::filesystem::path path = dir / L"lucent.catalog";
    {
        std::unique_ptr<Catalog> catalog = open_or_fail(path);
        ASSERT_NE(catalog, nullptr);
        LUCENT_ASSERT_OK(populate(*catalog, dir / L"photos", 1, 2000));
    }
    std::vector<char> bytes = read_bytes(path);
    constexpr std::size_t kPage = 4096;
    ASSERT_GT(bytes.size(), 20 * kPage);
    {
        std::unique_ptr<detail::Connection> raw = raw_connection(path, detail::Access::ReadOnly);
        ASSERT_NE(raw, nullptr);
        ASSERT_EQ(query_int(*raw, "PRAGMA page_size"), static_cast<std::int64_t>(kPage));
        ASSERT_EQ(query_int(*raw, "PRAGMA freelist_count"), 0);  // every page belongs to a b-tree
    }

    // Damage one page in the middle of the file.
    const std::filesystem::path damaged = dir / L"damaged.catalog";
    std::vector<char> copy = bytes;
    const std::size_t page = (copy.size() / kPage) / 2;
    std::fill(copy.begin() + static_cast<std::ptrdiff_t>(page * kPage),
              copy.begin() + static_cast<std::ptrdiff_t>((page + 1) * kPage), static_cast<char>(0xA5));
    write_bytes(damaged, copy);
    const std::string before = file_digest(damaged);
    for (const IntegrityCheck check : {IntegrityCheck::Quick, IntegrityCheck::Full}) {
        OpenOptions options = options_for(damaged);
        options.integrity_check = check;
        Expected<std::unique_ptr<Catalog>> catalog = Catalog::open(options);
        ASSERT_FALSE(catalog);
        EXPECT_EQ(catalog.error().code, ErrorCode::Corrupt) << catalog.error().describe();
        EXPECT_EQ(catalog.error().where, "catalog.open");
    }
    EXPECT_EQ(file_digest(damaged), before);  // nothing was written to the damaged copy

    // Damage the header: not recognisable as a database at all.
    const std::filesystem::path headless = dir / L"headless.catalog";
    copy = bytes;
    std::fill(copy.begin(), copy.begin() + 16, '\0');
    write_bytes(headless, copy);
    Expected<std::unique_ptr<Catalog>> catalog = Catalog::open(options_for(headless));
    ASSERT_FALSE(catalog);
    EXPECT_EQ(catalog.error().code, ErrorCode::Corrupt) << catalog.error().describe();

    std::unique_ptr<Catalog> original = open_or_fail(path);
    EXPECT_NE(original, nullptr);
}

TEST(CatalogSchema, MigrationListValidation) {
    LUCENT_EXPECT_OK(detail::validate_migrations(detail::builtin_migrations()));
    EXPECT_EQ(detail::builtin_migrations().size(), static_cast<std::size_t>(kSchemaVersion));
    std::vector<detail::Migration> gap = with_next(&add_note_column);
    gap.back().version = kNext + 1;
    EXPECT_FALSE(detail::validate_migrations(gap));
    std::vector<detail::Migration> reader = with_next(&add_note_column, kNext + 1);
    EXPECT_FALSE(detail::validate_migrations(reader));
    std::vector<detail::Migration> none = with_next(nullptr);
    EXPECT_FALSE(detail::validate_migrations(none));
    // A later version may not lower the reader requirement (schema v2 needs a v2 reader).
    std::vector<detail::Migration> lowered = with_next(&add_note_column, 1);
    EXPECT_FALSE(detail::validate_migrations(lowered));
    EXPECT_EQ(detail::builtin_migrations()[1].min_reader_version, 2);
    EXPECT_EQ(detail::builtin_migrations()[2].min_reader_version, 2);  // v3 is additive
}

}  // namespace
}  // namespace lucent::catalog
