// The committed catalog fixtures of every historical schema version (data/migration; W12, ADR 0022,
// the release gate "Update compatibility": prior catalog fixtures migrate safely). Each fixture was
// written by driving the older migration state (tests/catalog_fixtures; data/migration/README.md).
// This build must:
//   * recognise each fixture's schema as exactly what that version's migrations create (a released
//     migration is never edited);
//   * migrate a copy of each, after a verified pre-migration backup that holds the fixture as it
//     was, without changing a single value of a column that existed, filling the added columns with
//     their documented defaults, and leave a catalog that passes its integrity and foreign-key checks,
//     reads completely through the API and accepts writes;
//   * open a catalog written by a newer Lucent read-only and say why, never writing or migrating it,
//     and refuse one whose minimum reader version it does not reach.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lucent/core/hash.hpp"

#include "backup.hpp"
#include "catalog_impl.hpp"
#include "catalog_test_support.hpp"
#include "schema.hpp"

namespace lucent::catalog {
namespace {

using namespace test;

std::filesystem::path fixture_folder() {
    return std::filesystem::path(LUCENT_SOURCE_DIR) / L"src" / L"catalog" / L"tests" / L"data" / L"migration";
}

std::filesystem::path fixture_path(int version) {
    return fixture_folder() / std::format(L"v{}.catalog", version);
}

std::string file_digest(const std::filesystem::path& path) {
    Expected<core::FileHashResult> hash = core::hash_file(path, core::CancellationToken{});
    return hash ? core::to_hex(hash->digest) : hash.error().describe();
}

std::vector<std::string> tables_of(detail::Connection& connection) {
    std::vector<std::string> tables;
    const std::string list = query_text(
        connection,
        "SELECT ifnull(group_concat(name, ','), '') FROM (SELECT name FROM sqlite_schema WHERE type = 'table' AND "
        "name NOT LIKE 'sqlite_%' ORDER BY name)");
    std::size_t start = 0;
    while (start < list.size()) {
        const std::size_t end = std::min(list.find(',', start), list.size());
        tables.push_back(list.substr(start, end - start));
        start = end + 1;
    }
    return tables;
}

std::vector<std::string> columns_of(detail::Connection& connection, std::string_view table) {
    std::vector<std::string> columns;
    const std::string list = query_text(
        connection, std::format("SELECT ifnull(group_concat(name, ','), '') FROM (SELECT name FROM "
                                "pragma_table_info('{}') ORDER BY cid)",
                                table));
    std::size_t start = 0;
    while (start < list.size()) {
        const std::size_t end = std::min(list.find(',', start), list.size());
        columns.push_back(list.substr(start, end - start));
        start = end + 1;
    }
    return columns;
}

// Every row of `table`, the given columns, as text (quote() keeps types and NULLs apart), in the
// order of that text (catalog_meta has no rowid).
std::string dump(detail::Connection& connection, std::string_view table, const std::vector<std::string>& columns) {
    std::string expression;
    for (const std::string& column : columns) {
        expression += (expression.empty() ? "" : " || '|' || ") + std::format("quote({})", column);
    }
    return query_text(connection,
                      std::format("SELECT ifnull(group_concat(r, char(10)), '') FROM (SELECT {} AS r FROM {} ORDER BY "
                                  "r)",
                                  expression, table));
}

// Every table of the catalog at `path` with its columns and their dump.
struct Snapshot {
    std::map<std::string, std::vector<std::string>> columns;
    std::map<std::string, std::string> rows;
};

Snapshot snapshot(const std::filesystem::path& path) {
    Snapshot result;
    std::unique_ptr<detail::Connection> raw = raw_connection(path, detail::Access::ReadOnly);
    if (raw == nullptr) {
        return result;
    }
    for (const std::string& table : tables_of(*raw)) {
        result.columns[table] = columns_of(*raw, table);
        result.rows[table] = dump(*raw, table, result.columns[table]);
    }
    return result;
}

std::string schema_text(detail::Connection& connection) {
    return query_text(connection,
                      "SELECT ifnull(group_concat(o, char(10)), '') FROM (SELECT type || ' ' || name || ' ' || tbl_name || "
                      "' ' || ifnull(sql, '') AS o FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%' ORDER BY type, "
                      "name)");
}

std::vector<std::int32_t> versions_after(int version) {
    std::vector<std::int32_t> versions;
    for (std::int32_t v = version + 1; v <= kSchemaVersion; ++v) {
        versions.push_back(v);
    }
    return versions;
}

class MigrationFixture : public ::testing::TestWithParam<int> {
protected:
    void SetUp() override {
        source = fixture_path(GetParam());
        ASSERT_TRUE(std::filesystem::exists(source)) << "a committed fixture is missing: " << source.string();
        copy = dir / L"lucent.catalog";
        std::filesystem::copy_file(source, copy);
    }

    TempDir dir;
    std::filesystem::path source;
    std::filesystem::path copy;
};

TEST_P(MigrationFixture, HasTheSchemaThatVersionsMigrationsCreate) {
    const int version = GetParam();
    std::unique_ptr<detail::Connection> fixture = raw_connection(source, detail::Access::ReadOnly);
    ASSERT_NE(fixture, nullptr);
    EXPECT_EQ(query_int(*fixture, "PRAGMA user_version"), version);
    EXPECT_EQ(query_int(*fixture, "PRAGMA application_id"), detail::kApplicationId);
    EXPECT_EQ(query_text(*fixture, "PRAGMA journal_mode"), "delete") << "a self-contained file";
    EXPECT_EQ(query_text(*fixture, "PRAGMA integrity_check"), "ok");
    EXPECT_EQ(query_int(*fixture, "SELECT count(*) FROM pragma_foreign_key_check"), 0);
    EXPECT_EQ(query_text(*fixture, "SELECT value FROM catalog_meta WHERE key = 'schema_version'"),
              std::to_string(version));
    EXPECT_EQ(query_int(*fixture, "SELECT count(*) FROM schema_history"), version);

    // The same schema objects, character for character, as this build's first `version` migrations
    // make: the released migrations were never edited.
    Expected<std::unique_ptr<Catalog>> fresh = detail::CatalogAccess::open(
        options_for(dir / L"fresh.catalog"), detail::builtin_migrations().first(static_cast<std::size_t>(version)));
    LUCENT_ASSERT_OK(fresh);
    fresh->reset();
    std::unique_ptr<detail::Connection> expected = raw_connection(dir / L"fresh.catalog", detail::Access::ReadOnly);
    ASSERT_NE(expected, nullptr);
    EXPECT_EQ(schema_text(*fixture), schema_text(*expected));
}

TEST_P(MigrationFixture, MigratesAfterAVerifiedBackupWithoutLosingAValue) {
    const int version = GetParam();
    const Snapshot before = snapshot(copy);
    ASSERT_FALSE(before.rows.empty());
    ASSERT_FALSE(before.rows.at("items").empty());

    std::filesystem::path backup_path;
    {
        std::unique_ptr<Catalog> catalog = open_or_fail(copy);
        ASSERT_NE(catalog, nullptr);
        const SchemaStatus& schema = catalog->schema();
        EXPECT_EQ(schema.found_version, version);
        EXPECT_EQ(schema.version, kSchemaVersion);
        EXPECT_FALSE(schema.created);
        EXPECT_FALSE(catalog->is_read_only());
        EXPECT_EQ(schema.migrations_applied, versions_after(version));
        EXPECT_EQ(schema.created_by, "0.1.0");
        if (version < kSchemaVersion) {
            ASSERT_TRUE(schema.pre_migration_backup.has_value());
            EXPECT_EQ(schema.pre_migration_backup->kind, BackupKind::PreMigration);
            EXPECT_EQ(schema.pre_migration_backup->schema_version, version);
            backup_path = schema.pre_migration_backup->path;
        } else {
            EXPECT_FALSE(schema.pre_migration_backup.has_value()) << "nothing to migrate, nothing to back up";
        }
    }

    // The backup is the fixture as it was: verified, at its version, every value equal.
    if (!backup_path.empty()) {
        Expected<detail::DatabaseCheck> verified =
            detail::verify_database_file(backup_path, IntegrityCheck::Full, std::chrono::milliseconds(5000), "test");
        LUCENT_ASSERT_OK(verified);
        EXPECT_EQ(verified->user_version, version);
        EXPECT_EQ(verified->application_id, detail::kApplicationId);
        const Snapshot saved = snapshot(backup_path);
        EXPECT_EQ(saved.rows, before.rows);
        // ... and restoring it gives back the fixture's catalog, which the next open migrates again.
        Expected<RestoreReport> restored = Catalog::restore(dir / L"restored.catalog", backup_path);
        LUCENT_ASSERT_OK(restored);
        EXPECT_EQ(restored->restored_schema_version, version);
        EXPECT_EQ(snapshot(dir / L"restored.catalog").rows, before.rows);
    }

    // Every column that existed keeps every value; the tables and columns added have their defaults.
    const Snapshot after = snapshot(copy);
    std::unique_ptr<detail::Connection> raw = raw_connection(copy, detail::Access::ReadOnly);
    ASSERT_NE(raw, nullptr);
    for (const auto& [table, columns] : before.columns) {
        std::vector<std::string> kept = columns;
        if (table == "catalog_meta" || table == "schema_history") {
            continue;  // checked below: the migration appends to them
        }
        EXPECT_EQ(dump(*raw, table, kept), before.rows.at(table)) << table;
    }
    EXPECT_EQ(query_int(*raw, "PRAGMA user_version"), kSchemaVersion);
    EXPECT_EQ(query_text(*raw, "PRAGMA integrity_check"), "ok");
    EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM pragma_foreign_key_check"), 0);
    EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM schema_history"), kSchemaVersion);
    EXPECT_EQ(query_int(*raw, std::format("SELECT count(*) FROM schema_history WHERE version <= {} AND app_version = "
                                          "'0.1.0' AND applied_at < 1800000000000",
                                          version)),
              version)
        << "the fixture's own history is kept";
    EXPECT_EQ(query_text(*raw, "SELECT value FROM catalog_meta WHERE key = 'created_at'"), "1791021600000");
    EXPECT_EQ(query_text(*raw, "SELECT value FROM catalog_meta WHERE key = 'min_reader_version'"), "2");
    if (version < 2) {
        EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM items WHERE iso IS NOT NULL OR exposure_time_s IS NOT NULL OR "
                                  "f_number IS NOT NULL OR focal_length_mm IS NOT NULL OR image_count IS NOT NULL OR "
                                  "current_revision_id IS NOT NULL"),
                  0);
        EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM recipe_revisions"), 0);
    }
    if (version < 3) {
        EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM batches"), 0);
        EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM jobs"), 0);
    }
    if (version == 3) {
        // v4: no job was interrupted yet; finished batches count as reported, unfinished ones not.
        EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM jobs WHERE interruptions <> 0"), 0);
        EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM batches WHERE reported_at IS NOT finished_at"), 0);
        EXPECT_GT(query_int(*raw, "SELECT count(*) FROM batches WHERE reported_at IS NOT NULL"), 0);
    }
    if (version < 5) {
        EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM batches WHERE auto_mode IS NOT NULL"), 0);
        EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM jobs WHERE auto_state IS NOT NULL OR analysis_json IS NOT NULL"),
                  0);
        EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM auto_analyses"), 0);
    }
    for (const auto& [table, rows] : after.rows) {
        if (!before.rows.contains(table)) {
            EXPECT_TRUE(rows.empty()) << "a table the migration added holds rows: " << table;
        }
    }
    raw.reset();

    // The migrated catalog reads completely through the API ...
    std::unique_ptr<Catalog> catalog = open_or_fail(copy);
    ASSERT_NE(catalog, nullptr);
    EXPECT_TRUE(catalog->schema().migrations_applied.empty());
    const Status read = catalog->read([&](ReadTransaction& tx) -> Status {
        ItemQuery query;
        query.order = ItemOrder::Added;
        query.limit = 1000;
        Expected<ItemPage> page = tx.list_items(query);
        if (!page) {
            return Unexpected(page.error());
        }
        EXPECT_EQ(page->items.size(), 10u);
        for (const ItemRecord& item : page->items) {
            Expected<RevisionHistory> history = tx.revision_history(item.id);
            if (!history) {
                return Unexpected(history.error());
            }
            for (const RevisionInfo& info : history->revisions) {
                Expected<std::optional<RecipeRevision>> revision = tx.find_revision(info.id);  // verifies the SHA-256
                if (!revision) {
                    return Unexpected(revision.error());
                }
                EXPECT_TRUE(revision->has_value());
            }
            EXPECT_EQ(history->current, item.current_revision);
            if (Expected<std::vector<AutoAnalysisRecord>> records = tx.list_auto_analyses(item.id); !records) {
                return Unexpected(records.error());
            }
        }
        Expected<std::vector<SourceRecord>> sources = tx.list_sources(SourceQuery{});
        if (!sources) {
            return Unexpected(sources.error());
        }
        EXPECT_EQ(sources->size(), 9u);
        Expected<std::vector<ImportRecord>> imports = tx.list_imports(100);
        if (!imports) {
            return Unexpected(imports.error());
        }
        EXPECT_EQ(imports->size(), 4u);
        Expected<std::vector<BatchRecord>> batches = tx.list_batches(100);
        if (!batches) {
            return Unexpected(batches.error());
        }
        for (const BatchRecord& batch : *batches) {
            JobQuery jobs;
            jobs.batch = batch.id;
            Expected<std::vector<JobRecord>> listed = tx.list_jobs(jobs);
            if (!listed) {
                return Unexpected(listed.error());
            }
            EXPECT_EQ(listed->size(), batch.job_count);
            for (const JobRecord& job : *listed) {
                if (Expected<std::vector<PublicationRecord>> journal = tx.publication_journal(job.id); !journal) {
                    return Unexpected(journal.error());
                }
                Expected<std::optional<ReceiptRecord>> receipt = tx.find_receipt(job.id);
                if (!receipt) {
                    return Unexpected(receipt.error());
                }
                // Every published output has its receipt; an analyse-only job completes without one.
                EXPECT_EQ(receipt->has_value(),
                          job.stage == JobStage::Complete && batch.automatic != AutoMode::AnalyzeOnly)
                    << "job " << job.id.value;
            }
            EventQuery events;
            events.batch = batch.id;
            if (Expected<std::vector<JobEvent>> listed_events = tx.list_job_events(events); !listed_events) {
                return Unexpected(listed_events.error());
            }
        }
        return ok();
    });
    LUCENT_EXPECT_OK(read);

    // ... and accepts writes: a new revision for the first item, and its flags.
    const Status written = catalog->write([&](WriteTransaction& tx) -> Status {
        NewRevision revision;
        revision.schema_name = "lucent.develop";
        revision.schema_version = 2;
        revision.settings_json = R"({"after":"migration"})";
        revision.created_at = at(1'800'000'000'000);
        if (Expected<RevisionInfo> r = tx.append_revision(ItemId{1}, revision); !r) {
            return Unexpected(r.error());
        }
        return tx.set_item_flags(ItemId{1}, ItemFlags{PickFlag::Picked, 4});
    });
    LUCENT_EXPECT_OK(written);
}

INSTANTIATE_TEST_SUITE_P(EveryHistoricalSchemaVersion, MigrationFixture, ::testing::Range(1, kSchemaVersion + 1),
                         [](const ::testing::TestParamInfo<int>& info) { return std::format("v{}", info.param); });

TEST(MigrationFixtures, EveryHistoricalVersionHasItsDocumentedFixture) {
    std::ifstream in(fixture_folder() / L"fixtures.json", std::ios::binary);
    ASSERT_TRUE(in.good());
    const nlohmann::json manifest = nlohmann::json::parse(in, nullptr, false);
    ASSERT_FALSE(manifest.is_discarded());
    ASSERT_EQ(manifest.at("fixtures").size(), static_cast<std::size_t>(kSchemaVersion))
        << "a new schema version needs its fixture (tests/catalog_fixtures)";
    int version = 0;
    for (const nlohmann::json& entry : manifest.at("fixtures")) {
        ++version;
        EXPECT_EQ(entry.at("schema_version").get<int>(), version);
        const std::filesystem::path path = fixture_folder() / entry.at("file").get<std::string>();
        EXPECT_EQ(path, fixture_path(version));
        EXPECT_EQ(file_digest(path), entry.at("sha256").get<std::string>())
            << "committed fixtures never change: " << path.string();
        std::unique_ptr<detail::Connection> raw = raw_connection(path, detail::Access::ReadOnly);
        ASSERT_NE(raw, nullptr);
        for (const auto& [table, rows] : entry.at("rows").items()) {
            EXPECT_EQ(query_int(*raw, std::format("SELECT count(*) FROM {}", table)), rows.get<std::int64_t>())
                << path.string() << " " << table;
        }
    }
}

// A catalog the newest fixture's build wrote, then migrated by a later Lucent to the next schema
// version: what this build sees after the user went back to it.
void make_newer(const std::filesystem::path& path, int min_reader) {
    std::unique_ptr<detail::Connection> raw = raw_connection(path);
    ASSERT_NE(raw, nullptr);
    LUCENT_ASSERT_OK(raw->exec(
        std::format("PRAGMA user_version = {0};"
                    "UPDATE catalog_meta SET value = '{0}' WHERE key = 'schema_version';"
                    "UPDATE catalog_meta SET value = '{1}' WHERE key = 'min_reader_version';"
                    "UPDATE catalog_meta SET value = '9.0.0' WHERE key = 'written_by';"
                    "INSERT INTO schema_history (version, applied_at, app_version) VALUES ({0}, 1800000000000, '9.0.0');"
                    "CREATE TABLE future_table (id INTEGER PRIMARY KEY, note TEXT) STRICT;",
                    kSchemaVersion + 1, min_reader),
        "test"));
}

TEST(MigrationFixtures, ACatalogOfANewerLucentOpensReadOnlyAndSaysWhy) {
    TempDir dir;
    const std::filesystem::path path = dir / L"lucent.catalog";
    std::filesystem::copy_file(fixture_path(kSchemaVersion), path);
    make_newer(path, 2);  // additive: readers of schema v2 and later may still read it
    const std::string before = file_digest(path);

    std::unique_ptr<Catalog> catalog = open_or_fail(path);
    ASSERT_NE(catalog, nullptr);
    EXPECT_TRUE(catalog->is_read_only());
    ASSERT_TRUE(catalog->schema().read_only_reason.has_value());
    const Error& reason = *catalog->schema().read_only_reason;
    EXPECT_EQ(reason.code, ErrorCode::VersionMismatch);
    EXPECT_NE(reason.message.find(std::format("schema v{}", kSchemaVersion + 1)), std::string::npos) << reason.message;
    EXPECT_NE(reason.message.find("written by Lucent 9.0.0"), std::string::npos) << reason.message;
    EXPECT_NE(reason.message.find("opened read-only"), std::string::npos) << reason.message;
    EXPECT_EQ(catalog->schema().found_version, kSchemaVersion + 1);
    EXPECT_TRUE(catalog->schema().migrations_applied.empty());
    EXPECT_FALSE(catalog->schema().pre_migration_backup.has_value());
    // Everything this build knows is still listed ...
    Expected<std::uint64_t> items = catalog->query([](ReadTransaction& tx) { return tx.count_items(ItemFilter{}); });
    LUCENT_ASSERT_OK(items);
    EXPECT_EQ(*items, 10u);
    Expected<std::vector<BatchRecord>> batches = catalog->query([](ReadTransaction& tx) { return tx.list_batches(100); });
    LUCENT_ASSERT_OK(batches);
    EXPECT_FALSE(batches->empty());
    // ... and nothing is ever written: every write fails with the reason.
    const Status write = catalog->write([](WriteTransaction& tx) {
        return tx.set_item_flags(ItemId{1}, ItemFlags{PickFlag::Rejected, 1});
    });
    ASSERT_FALSE(write);
    EXPECT_EQ(write.error().code, ErrorCode::VersionMismatch);
    catalog.reset();
    EXPECT_EQ(file_digest(path), before);
    EXPECT_FALSE(std::filesystem::exists(dir / L"backups"));
}

TEST(MigrationFixtures, ACatalogThisBuildCannotReadIsRefusedAndNeverTouched) {
    TempDir dir;
    const std::filesystem::path path = dir / L"lucent.catalog";
    std::filesystem::copy_file(fixture_path(kSchemaVersion), path);
    make_newer(path, kSchemaVersion + 1);  // the newer schema changed what existing data means
    const std::string before = file_digest(path);
    for (const OpenMode mode : {OpenMode::ReadWrite, OpenMode::ReadOnly}) {
        OpenOptions options = options_for(path);
        options.mode = mode;
        Expected<std::unique_ptr<Catalog>> catalog = Catalog::open(options);
        ASSERT_FALSE(catalog);
        EXPECT_EQ(catalog.error().code, ErrorCode::VersionMismatch);
        EXPECT_NE(catalog.error().message.find(std::format("needs a reader for schema v{}", kSchemaVersion + 1)),
                  std::string::npos)
            << catalog.error().message;
    }
    EXPECT_EQ(file_digest(path), before);
    EXPECT_FALSE(std::filesystem::exists(dir / L"backups"));
}

}  // namespace
}  // namespace lucent::catalog
