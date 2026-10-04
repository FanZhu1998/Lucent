// Schema v3 (W7): migrating a populated W6 (v2) catalog to the batch journal (and on to v4), and the
// forward-compatibility contract seen from a schema-v2 build (min_reader_version stays 2: it opens
// a v3 catalog read-only and never writes it).

#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "lucent/core/hash.hpp"

#include "backup.hpp"
#include "catalog_impl.hpp"
#include "catalog_test_support.hpp"
#include "schema.hpp"

namespace lucent::catalog {
namespace {

using namespace test;

static_assert(kSchemaVersion >= 3, "these tests describe the v2 -> v3 migration");

// The migrations of a schema-v2 (W6) build.
std::span<const detail::Migration> v2_build() {
    return detail::builtin_migrations().first(2);
}

std::vector<std::int32_t> versions_after_v2() {
    std::vector<std::int32_t> versions;
    for (std::int32_t v = 3; v <= kSchemaVersion; ++v) {
        versions.push_back(v);
    }
    return versions;
}

std::string file_digest(const std::filesystem::path& path) {
    Expected<core::FileHashResult> hash = core::hash_file(path, core::CancellationToken{});
    return hash ? core::to_hex(hash->digest) : hash.error().describe();
}

// Every row of a table, every column, as text (quote() keeps types and NULLs apart).
std::string dump_table(detail::Connection& connection, std::string_view table) {
    const std::string columns = query_text(
        connection,
        std::format("SELECT group_concat('quote(' || name || ')', ' || ''|'' || ') FROM pragma_table_info('{}')",
                    table));
    return query_text(connection, std::format("SELECT ifnull(group_concat(r, char(10)), '') FROM (SELECT {} AS r "
                                              "FROM {} ORDER BY rowid)",
                                              columns, table));
}

constexpr std::string_view kV2Tables[] = {"sources", "items", "imports", "recipe_revisions", "catalog_meta"};
constexpr std::string_view kV3Tables[] = {"batches", "jobs", "job_events", "publication_journal", "receipts"};

// A W6 catalog written by a schema-v2 build: 30 hashed sources with one item each, capture
// fields, a revision history with an undo and a branch on two items, and an import record.
void make_populated_v2_catalog(const std::filesystem::path& path, const std::filesystem::path& photos) {
    Expected<std::unique_ptr<Catalog>> catalog = detail::CatalogAccess::open(options_for(path), v2_build());
    LUCENT_ASSERT_OK(catalog);
    ASSERT_EQ((*catalog)->schema().version, 2);
    const Status written = (*catalog)->write([&](WriteTransaction& tx) -> Status {
        for (std::uint64_t n = 1; n <= 30; ++n) {
            SourceObservation o = observation(photos / std::format(L"Фото_{:03}.CR3", n), n);
            o.sha256 = digest(static_cast<std::uint8_t>(n));
            Expected<SourceUpsertResult> source = tx.upsert_source(o);
            if (!source) {
                return Unexpected(source.error());
            }
            ItemMetadata metadata = item();
            metadata.iso = 100.0 * static_cast<double>(n);
            metadata.f_number = 4.0;
            metadata.image_count = 1;
            if (Expected<std::vector<ItemId>> ids = tx.upsert_items(source->id, std::span(&metadata, 1)); !ids) {
                return Unexpected(ids.error());
            }
        }
        for (const std::int64_t item_id : {3, 7}) {
            for (int i = 0; i < 3; ++i) {
                NewRevision revision;
                revision.schema_name = "lucent.develop";
                revision.schema_version = 1;
                revision.settings_json = std::format(R"({{"exposure_ev":{}}})", i);
                revision.created_at = at(1'790'000'000'000 + i);
                if (Expected<RevisionInfo> r = tx.append_revision(ItemId{item_id}, revision); !r) {
                    return Unexpected(r.error());
                }
                if (i == 1) {
                    if (Expected<std::optional<RevisionId>> undone = tx.undo_revision(ItemId{item_id}); !undone) {
                        return Unexpected(undone.error());
                    }
                }
            }
        }
        return tx.begin_import(ImportStart{at(1'790'000'000'000), {{photos, true}}, "{}"}).map([](ImportId) {});
    });
    LUCENT_ASSERT_OK(written);
}

TEST(CatalogSchemaV3, MigratesAPopulatedV2CatalogAfterABackup) {
    TempDir dir;
    const std::filesystem::path path = dir / L"lucent.catalog";
    make_populated_v2_catalog(path, dir / L"photos");
    if (HasFatalFailure()) {
        return;
    }
    std::vector<std::string> before;
    {
        std::unique_ptr<detail::Connection> raw = raw_connection(path, detail::Access::ReadOnly);
        ASSERT_NE(raw, nullptr);
        ASSERT_EQ(query_int(*raw, "PRAGMA user_version"), 2);
        ASSERT_EQ(query_int(*raw, "SELECT count(*) FROM recipe_revisions"), 6);
        for (const std::string_view table : kV2Tables) {
            if (table != "catalog_meta") {
                before.push_back(dump_table(*raw, table));
            }
        }
    }

    std::filesystem::path backup_path;
    {
        std::unique_ptr<Catalog> catalog = open_or_fail(path);
        ASSERT_NE(catalog, nullptr);
        const SchemaStatus& schema = catalog->schema();
        EXPECT_EQ(schema.found_version, 2);
        EXPECT_EQ(schema.version, kSchemaVersion);
        EXPECT_EQ(schema.migrations_applied, versions_after_v2());
        ASSERT_TRUE(schema.pre_migration_backup.has_value());
        EXPECT_EQ(schema.pre_migration_backup->kind, BackupKind::PreMigration);
        EXPECT_EQ(schema.pre_migration_backup->schema_version, 2);
        backup_path = schema.pre_migration_backup->path;

        // The migrated catalog is fully usable: a batch over migrated items and revisions.
        Expected<std::optional<RecipeRevision>> current =
            catalog->query([](ReadTransaction& tx) { return tx.current_revision(ItemId{3}); });
        LUCENT_ASSERT_OK(current);
        ASSERT_TRUE(current->has_value());
        std::vector<NewJob> jobs(3);
        jobs[0].item = ItemId{3};
        jobs[0].revision = (*current)->info.id;
        jobs[0].settings_sha256 = (*current)->info.settings_sha256;
        jobs[1].item = ItemId{4};
        jobs[1].settings_sha256 = digest(77);
        jobs[2].item = ItemId{30};
        jobs[2].settings_sha256 = digest(77);
        NewBatch spec;
        spec.name = "after migration";
        spec.destination_json = "{}";
        spec.export_settings_json = R"({"format":"tiff16"})";
        spec.renderer_version = "engine";
        spec.created_at = at(1'800'000'000'000);
        spec.start = true;
        Expected<CreatedBatch> created =
            catalog->transact([&](WriteTransaction& tx) { return tx.create_batch(spec, jobs); });
        LUCENT_ASSERT_OK(created);
        EXPECT_EQ(created->jobs.size(), 3u);
    }

    {
        std::unique_ptr<detail::Connection> raw = raw_connection(path, detail::Access::ReadOnly);
        ASSERT_NE(raw, nullptr);
        EXPECT_EQ(query_int(*raw, "PRAGMA user_version"), kSchemaVersion);
        std::size_t i = 0;
        for (const std::string_view table : kV2Tables) {
            if (table != "catalog_meta") {
                EXPECT_EQ(dump_table(*raw, table), before[i++]) << table;  // unchanged, column by column
            }
        }
        for (const std::string_view table : kV3Tables) {
            EXPECT_EQ(query_int(*raw, std::format("SELECT count(*) FROM sqlite_schema WHERE type = 'table' AND "
                                                  "name = '{}'",
                                                  table)),
                      1)
                << table;
        }
        EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM jobs"), 3);
        EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM schema_history"), kSchemaVersion);
        EXPECT_EQ(query_text(*raw, "SELECT value FROM catalog_meta WHERE key = 'schema_version'"),
                  std::to_string(kSchemaVersion));
        // Additive: a schema-v2 reader may still read it.
        EXPECT_EQ(query_text(*raw, "SELECT value FROM catalog_meta WHERE key = 'min_reader_version'"), "2");
        EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM pragma_foreign_key_check"), 0);
        EXPECT_EQ(query_text(*raw, "PRAGMA integrity_check"), "ok");
    }

    // The pre-migration backup is the v2 catalog as it was.
    ASSERT_TRUE(std::filesystem::exists(backup_path));
    std::unique_ptr<detail::Connection> raw = raw_connection(backup_path, detail::Access::ReadOnly);
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(query_int(*raw, "PRAGMA user_version"), 2);
    EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM sqlite_schema WHERE name = 'jobs'"), 0);
    EXPECT_EQ(dump_table(*raw, "recipe_revisions"), before[3]);
}

TEST(CatalogSchemaV3, AFailingV3MigrationLeavesTheV2CatalogIntact) {
    TempDir dir;
    const std::filesystem::path path = dir / L"lucent.catalog";
    make_populated_v2_catalog(path, dir / L"photos");
    if (HasFatalFailure()) {
        return;
    }
    // Occupy a name the v3 migration creates, simulating an unexpected failure half-way.
    {
        std::unique_ptr<detail::Connection> raw = raw_connection(path);
        ASSERT_NE(raw, nullptr);
        LUCENT_ASSERT_OK(raw->exec("CREATE TABLE receipts (x INTEGER) STRICT", "test"));
    }
    std::string items_before;
    {
        std::unique_ptr<detail::Connection> raw = raw_connection(path, detail::Access::ReadOnly);
        ASSERT_NE(raw, nullptr);
        items_before = dump_table(*raw, "items");
    }
    Expected<std::unique_ptr<Catalog>> catalog = Catalog::open(options_for(path));
    ASSERT_FALSE(catalog);
    EXPECT_EQ(catalog.error().where, "catalog.migrate");
    EXPECT_NE(catalog.error().message.find("v3"), std::string::npos) << catalog.error().message;

    std::unique_ptr<detail::Connection> raw = raw_connection(path, detail::Access::ReadOnly);
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(query_int(*raw, "PRAGMA user_version"), 2);
    EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM sqlite_schema WHERE name IN ('batches', 'jobs')"), 0);
    EXPECT_EQ(dump_table(*raw, "items"), items_before);
    Expected<std::vector<BackupInfo>> backups = detail::list_backups_in(dir / L"backups", path, "test");
    LUCENT_ASSERT_OK(backups);
    ASSERT_EQ(backups->size(), 1u);
    EXPECT_EQ(backups->front().schema_version, 2);
}

TEST(CatalogSchemaV3, AV2BuildReadsAV3CatalogReadOnlyAndNeverWritesIt) {
    TempDir dir;
    const std::filesystem::path path = dir / L"lucent.catalog";
    {
        std::unique_ptr<Catalog> catalog = open_or_fail(path);
        ASSERT_NE(catalog, nullptr);
        LUCENT_ASSERT_OK(populate(*catalog, dir / L"photos", 1, 3));
        const Status written = catalog->write([&](WriteTransaction& tx) -> Status {
            if (Status s = tx.set_source_hash(SourceId{1}, digest(1), identity(1)); !s) {
                return s;
            }
            NewJob job;
            job.item = ItemId{1};
            job.settings_sha256 = digest(9);
            NewBatch spec;
            spec.destination_json = "{}";
            spec.export_settings_json = "{}";
            spec.renderer_version = "engine";
            spec.start = true;
            return tx.create_batch(spec, std::span(&job, 1)).map([](CreatedBatch&&) {});
        });
        LUCENT_ASSERT_OK(written);
    }
    const std::string before = file_digest(path);

    Expected<std::unique_ptr<Catalog>> old = detail::CatalogAccess::open(options_for(path), v2_build());
    LUCENT_ASSERT_OK(old);
    EXPECT_TRUE((*old)->is_read_only());
    ASSERT_TRUE((*old)->schema().read_only_reason.has_value());
    EXPECT_EQ((*old)->schema().read_only_reason->code, ErrorCode::VersionMismatch);
    Expected<std::uint64_t> items = (*old)->query([](ReadTransaction& tx) { return tx.count_items(ItemFilter{}); });
    LUCENT_ASSERT_OK(items);
    EXPECT_EQ(*items, 3u);
    const Status write = (*old)->write([](WriteTransaction& tx) {
        return tx.set_item_flags(ItemId{1}, ItemFlags{PickFlag::Picked, 5});
    });
    ASSERT_FALSE(write);
    EXPECT_EQ(write.error().code, ErrorCode::VersionMismatch);
    old->reset();

    EXPECT_EQ(file_digest(path), before);  // never written, never migrated
}

// ---------------------------------------------------------------------------------------------
// Schema v4 (W7): interruption counts and report emission, added to a populated v3 journal.
// ---------------------------------------------------------------------------------------------

TEST(CatalogSchemaV4, KeepsAV3JournalAndCountsItsFinishedBatchesAsReported) {
    TempDir dir;
    const std::filesystem::path path = dir / L"lucent.catalog";
    make_populated_v2_catalog(path, dir / L"photos");
    if (HasFatalFailure()) {
        return;
    }
    BatchId finished;
    BatchId running;
    {
        // A schema-v3 build: two batches, one of them cancelled (finished).
        Expected<std::unique_ptr<Catalog>> v3 =
            detail::CatalogAccess::open(options_for(path), detail::builtin_migrations().first(3));
        LUCENT_ASSERT_OK(v3);
        ASSERT_EQ((*v3)->schema().version, 3);
        std::vector<NewJob> jobs(2);
        jobs[0].item = ItemId{1};
        jobs[0].settings_sha256 = digest(77);
        jobs[1].item = ItemId{2};
        jobs[1].settings_sha256 = digest(77);
        NewBatch spec;
        spec.name = "v3 batch";
        spec.destination_json = "{}";
        spec.export_settings_json = R"({"format":"jpeg"})";
        spec.renderer_version = "engine";
        spec.created_at = at(1'800'000'000'000);
        spec.start = true;
        for (BatchId* id : {&finished, &running}) {
            Expected<CreatedBatch> created =
                (*v3)->transact([&](WriteTransaction& tx) { return tx.create_batch(spec, jobs); });
            LUCENT_ASSERT_OK(created);
            *id = created->id;
        }
        LUCENT_ASSERT_OK((*v3)->transact(
            [&](WriteTransaction& tx) { return tx.cancel_batch(finished, at(1'800'000'000'500)); }));
    }
    std::unique_ptr<Catalog> catalog = open_or_fail(path);
    ASSERT_NE(catalog, nullptr);
    EXPECT_EQ(catalog->schema().found_version, 3);
    EXPECT_EQ(catalog->schema().migrations_applied, (std::vector<std::int32_t>{4}));
    Expected<std::optional<BatchRecord>> done = catalog->query([&](ReadTransaction& tx) { return tx.find_batch(finished); });
    LUCENT_ASSERT_OK(done);
    ASSERT_TRUE(done->has_value());
    EXPECT_EQ((*done)->reported_at, (*done)->finished_at) << "batches finished before v4 are not reported again";
    Expected<std::optional<BatchRecord>> open = catalog->query([&](ReadTransaction& tx) { return tx.find_batch(running); });
    LUCENT_ASSERT_OK(open);
    ASSERT_TRUE(open->has_value());
    EXPECT_FALSE((*open)->reported_at.has_value());
    Expected<std::vector<BatchRecord>> unreported =
        catalog->query([](ReadTransaction& tx) { return tx.list_unreported_batches(10); });
    LUCENT_ASSERT_OK(unreported);
    EXPECT_TRUE(unreported->empty());
    JobQuery query;
    query.batch = running;
    Expected<std::vector<JobRecord>> jobs = catalog->query([&](ReadTransaction& tx) { return tx.list_jobs(query); });
    LUCENT_ASSERT_OK(jobs);
    ASSERT_EQ(jobs->size(), 2u);
    for (const JobRecord& job : *jobs) {
        EXPECT_EQ(job.interruptions, 0u);
    }
    std::unique_ptr<detail::Connection> raw = raw_connection(path, detail::Access::ReadOnly);
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(query_text(*raw, "SELECT value FROM catalog_meta WHERE key = 'min_reader_version'"), "2");
    EXPECT_EQ(query_text(*raw, "PRAGMA integrity_check"), "ok");
}

}  // namespace
}  // namespace lucent::catalog
