// Schema v2 (W6): migrating a populated W4 (v1) catalog, and the forward-compatibility contract
// (min_reader_version 2: a schema-v1 build refuses the catalog) seen from a schema-v1 build.

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

static_assert(kSchemaVersion == 2, "these tests describe the v1 -> v2 migration");

// The migrations of a schema-v1 (W4) build.
std::span<const detail::Migration> v1_build() {
    return detail::builtin_migrations().first(1);
}

std::string file_digest(const std::filesystem::path& path) {
    Expected<core::FileHashResult> hash = core::hash_file(path, core::CancellationToken{});
    return hash ? core::to_hex(hash->digest) : hash.error().describe();
}

// Every v1 column of every row of a v1 table, as text (quote() keeps types and NULLs apart).
std::string dump_v1_table(detail::Connection& connection, std::string_view table) {
    std::string_view columns;
    if (table == "sources") {
        columns =
            "id, path, path_key, volume_serial, file_id, size, mtime, sha256, import_mode, origin_path, "
            "origin_volume_serial, origin_file_id, origin_size, origin_mtime, storage_kind, state, "
            "first_seen, last_seen";
    } else if (table == "items") {
        columns =
            "id, source_id, subimage_index, make, model, lens, capture_time, width, height, orientation, "
            "format, support_status, support_reasons_json, decoder_id, decoder_version, probed_at, pick, "
            "rating";
    } else {
        columns =
            "id, started_at, finished_at, outcome, roots_json, options_json, files_seen, added, refreshed, "
            "unchanged, duplicates, skipped, failed";
    }
    std::string row_expression;
    std::size_t start = 0;
    while (start < columns.size()) {
        std::size_t end = columns.find(',', start);
        if (end == std::string_view::npos) {
            end = columns.size();
        }
        std::string_view column = columns.substr(start, end - start);
        while (column.starts_with(' ')) {
            column.remove_prefix(1);
        }
        if (!row_expression.empty()) {
            row_expression += " || '|' || ";
        }
        row_expression += std::format("quote({})", column);
        start = end + 1;
    }
    return query_text(connection,
                      std::format("SELECT ifnull(group_concat(r, char(10)), '') FROM (SELECT {} AS r "
                                  "FROM {} ORDER BY id)",
                                  row_expression, table));
}

// A W4 catalog: 40 sources (some hashed, one copied, one missing), 41 items with flags (one file
// has two images) and two import records, written by a schema-v1 build.
void make_populated_v1_catalog(const std::filesystem::path& path, const std::filesystem::path& photos) {
    {
        Expected<std::unique_ptr<Catalog>> catalog =
            detail::CatalogAccess::open(options_for(path), v1_build());
        LUCENT_ASSERT_OK(catalog);
        ASSERT_EQ((*catalog)->schema().version, 1);
        const Status written = (*catalog)->write([&](WriteTransaction& tx) -> Status {
            for (std::uint64_t n = 1; n <= 40; ++n) {
                SourceObservation o = observation(photos / std::format(L"Фото_{:03}.CR3", n), n);
                if (n % 4 == 0) {
                    o.sha256 = digest(static_cast<std::uint8_t>(n));
                }
                if (n == 7) {
                    o.import_mode = ImportMode::Copied;
                    o.origin_path = photos / L"card" / L"IMG_0007.CR3";
                    o.origin_identity = identity(1007);
                }
                Expected<SourceUpsertResult> source = tx.upsert_source(o);
                if (!source) {
                    return Unexpected(source.error());
                }
            }
            if (Status s = tx.set_source_state(SourceId{9}, SourceState::Missing, at(1'700'000'100'000));
                !s) {
                return s;
            }
            Expected<ImportId> first =
                tx.begin_import(ImportStart{at(1'700'000'000'000), {{photos, true}}, "{}"});
            if (!first) {
                return Unexpected(first.error());
            }
            if (Status s =
                    tx.finish_import(*first, ImportFinish{at(1'700'000'001'000), ImportOutcome::Completed,
                                                          ImportCounts{40, 40, 0, 0, 0, 0, 0}});
                !s) {
                return s;
            }
            return tx
                .begin_import(
                    ImportStart{at(1'700'000'002'000), {{photos / L"more", false}}, "{\"copy\":false}"})
                .map([](ImportId) {});
        });
        LUCENT_ASSERT_OK(written);
    }
    // Items as the W4 build wrote them (its upsert_items knows only the v1 columns).
    std::unique_ptr<detail::Connection> raw = raw_connection(path);
    ASSERT_NE(raw, nullptr);
    LUCENT_ASSERT_OK(raw->exec(
        "INSERT INTO items (source_id, subimage_index, make, model, lens, capture_time, width, height, "
        "orientation, format, support_status, support_reasons_json, decoder_id, decoder_version, probed_at, "
        "pick, rating) "
        "SELECT id, 0, 'Canon', 'EOS R5', 'RF24-105mm F4 L IS USM', "
        "CASE WHEN id % 5 = 0 THEN NULL ELSE format('2026-09-%02dT10:00:00', 1 + id % 28) END, 8192, 5464, "
        "1 + id % 8, 'cr3', CASE WHEN id % 10 = 0 THEN 'limited' ELSE 'supported' END, "
        "CASE WHEN id % 10 = 0 THEN '[\"no-profile\"]' ELSE '[]' END, 'libraw', '0.22.0', 1700000500000 + "
        "id, "
        "CASE id % 3 WHEN 0 THEN 'picked' WHEN 1 THEN 'none' ELSE 'rejected' END, id % 6 "
        "FROM sources ORDER BY id;"
        "INSERT INTO items (source_id, subimage_index, make, model, lens, width, height, orientation, "
        "format, "
        "support_status, support_reasons_json, decoder_id, decoder_version, probed_at) "
        "VALUES (3, 1, 'Canon', 'EOS R5', '', 1920, 1080, 1, 'cr3', 'supported', '[]', 'libraw', '0.22.0', "
        "0);",
        "test"));
}

TEST(CatalogSchemaV2, MigratesAPopulatedV1CatalogAfterABackup) {
    TempDir dir;
    const std::filesystem::path path = dir / L"lucent.catalog";
    make_populated_v1_catalog(path, dir / L"photos");
    if (HasFatalFailure()) {
        return;
    }
    std::string before[3];
    {
        std::unique_ptr<detail::Connection> raw = raw_connection(path, detail::Access::ReadOnly);
        ASSERT_NE(raw, nullptr);
        ASSERT_EQ(query_int(*raw, "PRAGMA user_version"), 1);
        ASSERT_EQ(query_int(*raw, "SELECT count(*) FROM items"), 41);
        before[0] = dump_v1_table(*raw, "sources");
        before[1] = dump_v1_table(*raw, "items");
        before[2] = dump_v1_table(*raw, "imports");
    }
    ASSERT_FALSE(before[1].empty());

    std::filesystem::path backup_path;
    {
        std::unique_ptr<Catalog> catalog = open_or_fail(path);  // this build: v2
        ASSERT_NE(catalog, nullptr);
        const SchemaStatus& schema = catalog->schema();
        EXPECT_EQ(schema.found_version, 1);
        EXPECT_EQ(schema.version, 2);
        EXPECT_FALSE(schema.created);
        EXPECT_EQ(schema.migrations_applied, std::vector<std::int32_t>{2});
        ASSERT_TRUE(schema.pre_migration_backup.has_value());
        EXPECT_EQ(schema.pre_migration_backup->kind, BackupKind::PreMigration);
        EXPECT_EQ(schema.pre_migration_backup->schema_version, 1);
        backup_path = schema.pre_migration_backup->path;

        // Through the repository: every item, with its flags; new fields unknown; as shot.
        Expected<ItemPage> page = catalog->query([](ReadTransaction& tx) {
            ItemQuery query;
            query.order = ItemOrder::Added;
            query.limit = 100;
            return tx.list_items(query);
        });
        LUCENT_ASSERT_OK(page);
        ASSERT_EQ(page->items.size(), 41u);
        for (const ItemRecord& item : page->items) {
            EXPECT_FALSE(item.metadata.iso.has_value());
            EXPECT_FALSE(item.metadata.exposure_time_s.has_value());
            EXPECT_FALSE(item.metadata.f_number.has_value());
            EXPECT_FALSE(item.metadata.focal_length_mm.has_value());
            EXPECT_FALSE(item.metadata.image_count.has_value());
            EXPECT_FALSE(item.current_revision.has_value());
            if (item.metadata.subimage_index == 0) {
                EXPECT_EQ(item.flags.rating, item.source_id.value % 6);
            }
        }
        // The migrated catalog is fully usable: edits and re-probes.
        const ItemId first = page->items.front().id;
        NewRevision revision;
        revision.schema_name = "lucent.develop";
        revision.schema_version = 1;
        revision.settings_json = R"({"exposure_ev":0.5})";
        revision.created_at = at(1'800'000'000'000);
        Expected<RevisionInfo> appended =
            catalog->transact([&](WriteTransaction& tx) { return tx.append_revision(first, revision); });
        LUCENT_ASSERT_OK(appended);
        EXPECT_EQ(appended->revision_number, 1u);
    }

    // The v1 data is unchanged, column by column; the new columns are NULL.
    {
        std::unique_ptr<detail::Connection> raw = raw_connection(path, detail::Access::ReadOnly);
        ASSERT_NE(raw, nullptr);
        EXPECT_EQ(query_int(*raw, "PRAGMA user_version"), 2);
        EXPECT_EQ(dump_v1_table(*raw, "sources"), before[0]);
        EXPECT_EQ(dump_v1_table(*raw, "items"), before[1]);
        EXPECT_EQ(dump_v1_table(*raw, "imports"), before[2]);
        EXPECT_EQ(query_int(*raw,
                            "SELECT count(*) FROM items WHERE iso IS NULL AND exposure_time_s IS NULL AND "
                            "f_number IS NULL AND focal_length_mm IS NULL AND image_count IS NULL"),
                  41);
        EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM items WHERE current_revision_id IS NOT NULL"), 1);
        EXPECT_EQ(query_text(*raw,
                             "SELECT group_concat(version, ',') FROM (SELECT version FROM schema_history "
                             "ORDER BY version)"),
                  "1,2");
        EXPECT_EQ(query_text(*raw, "SELECT value FROM catalog_meta WHERE key = 'schema_version'"), "2");
        EXPECT_EQ(query_text(*raw, "SELECT value FROM catalog_meta WHERE key = 'min_reader_version'"), "2");
        EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM pragma_foreign_key_check"), 0);
        EXPECT_EQ(query_text(*raw, "PRAGMA integrity_check"), "ok");
    }

    // The pre-migration backup is the v1 catalog as it was.
    ASSERT_TRUE(std::filesystem::exists(backup_path));
    {
        std::unique_ptr<detail::Connection> raw = raw_connection(backup_path, detail::Access::ReadOnly);
        ASSERT_NE(raw, nullptr);
        EXPECT_EQ(query_int(*raw, "PRAGMA user_version"), 1);
        EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM sqlite_schema WHERE name = 'recipe_revisions'"), 0);
        EXPECT_EQ(dump_v1_table(*raw, "sources"), before[0]);
        EXPECT_EQ(dump_v1_table(*raw, "items"), before[1]);
        EXPECT_EQ(dump_v1_table(*raw, "imports"), before[2]);
    }
    // ... and restoring it gives back a v1 catalog that the next open migrates again.
    Expected<RestoreReport> restored = Catalog::restore(path, backup_path);
    LUCENT_ASSERT_OK(restored);
    EXPECT_EQ(restored->restored_schema_version, 1);
    std::unique_ptr<Catalog> again = open_or_fail(path);
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again->schema().migrations_applied, std::vector<std::int32_t>{2});
    Expected<std::uint64_t> count =
        again->query([](ReadTransaction& tx) { return tx.count_items(ItemFilter{}); });
    LUCENT_ASSERT_OK(count);
    EXPECT_EQ(*count, 41u);
}

TEST(CatalogSchemaV2, AFailingV2MigrationLeavesTheV1CatalogIntact) {
    TempDir dir;
    const std::filesystem::path path = dir / L"lucent.catalog";
    make_populated_v1_catalog(path, dir / L"photos");
    if (HasFatalFailure()) {
        return;
    }
    // A v1 catalog whose data violates a v2 CHECK cannot exist; simulate an unexpected failure
    // instead by occupying a name the v2 migration creates.
    {
        std::unique_ptr<detail::Connection> raw = raw_connection(path);
        ASSERT_NE(raw, nullptr);
        LUCENT_ASSERT_OK(raw->exec("CREATE TABLE recipe_revisions (x INTEGER) STRICT", "test"));
    }
    std::string items_before;
    {
        std::unique_ptr<detail::Connection> raw = raw_connection(path, detail::Access::ReadOnly);
        ASSERT_NE(raw, nullptr);
        items_before = dump_v1_table(*raw, "items");
    }
    Expected<std::unique_ptr<Catalog>> catalog = Catalog::open(options_for(path));
    ASSERT_FALSE(catalog);
    EXPECT_EQ(catalog.error().where, "catalog.migrate");
    EXPECT_NE(catalog.error().message.find("v2"), std::string::npos) << catalog.error().message;

    std::unique_ptr<detail::Connection> raw = raw_connection(path, detail::Access::ReadOnly);
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(query_int(*raw, "PRAGMA user_version"), 1);
    EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM pragma_table_info('items') WHERE name = 'iso'"), 0);
    EXPECT_EQ(dump_v1_table(*raw, "items"), items_before);
    Expected<std::vector<BackupInfo>> backups = detail::list_backups_in(dir / L"backups", path, "test");
    LUCENT_ASSERT_OK(backups);
    ASSERT_EQ(backups->size(), 1u);
    EXPECT_EQ(backups->front().kind, BackupKind::PreMigration);
    EXPECT_EQ(backups->front().schema_version, 1);
}

// A v2 catalog with items, capture fields and a revision history.
void make_v2_catalog(const std::filesystem::path& path, const std::filesystem::path& photos) {
    std::unique_ptr<Catalog> catalog = open_or_fail(path);
    ASSERT_NE(catalog, nullptr);
    LUCENT_ASSERT_OK(populate(*catalog, photos, 1, 12));
    const Status written = catalog->write([&](WriteTransaction& tx) -> Status {
        ItemMetadata probed = item();
        probed.iso = 400;
        probed.exposure_time_s = 1.0 / 250;
        probed.f_number = 5.6;
        probed.focal_length_mm = 50;
        probed.image_count = 1;
        if (Expected<std::vector<ItemId>> ids = tx.upsert_items(SourceId{1}, std::span(&probed, 1)); !ids) {
            return Unexpected(ids.error());
        }
        for (int i = 0; i < 3; ++i) {
            NewRevision revision;
            revision.schema_name = "lucent.develop";
            revision.schema_version = 1;
            revision.settings_json = std::format(R"({{"exposure_ev":{}}})", i);
            revision.created_at = at(1'800'000'000'000 + i);
            if (Expected<RevisionInfo> r = tx.append_revision(ItemId{1}, revision); !r) {
                return Unexpected(r.error());
            }
        }
        return tx.undo_revision(ItemId{1}).map([](auto&&) {});
    });
    LUCENT_ASSERT_OK(written);
}

TEST(CatalogSchemaV2, AV1BuildRefusesAV2CatalogAndNeverWritesIt) {
    TempDir dir;
    const std::filesystem::path path = dir / L"lucent.catalog";
    make_v2_catalog(path, dir / L"photos");
    if (HasFatalFailure()) {
        return;
    }
    const std::string before = file_digest(path);

    // min_reader_version is 2: a v1 build would show and export edited photos as shot (it knows no
    // recipes), so it must refuse the catalog rather than open it read-only.
    for (const OpenMode mode : {OpenMode::ReadWrite, OpenMode::ReadOnly}) {
        OpenOptions options = options_for(path);
        options.mode = mode;
        Expected<std::unique_ptr<Catalog>> catalog = detail::CatalogAccess::open(options, v1_build());
        ASSERT_FALSE(catalog);
        EXPECT_EQ(catalog.error().code, ErrorCode::VersionMismatch);
        EXPECT_NE(catalog.error().message.find("needs a reader for schema v2"), std::string::npos)
            << catalog.error().message;
    }
    // Also when the v1 build would refuse every newer schema anyway.
    OpenOptions refuse = options_for(path);
    refuse.newer_schema = NewerSchemaPolicy::Refuse;
    Expected<std::unique_ptr<Catalog>> refused = detail::CatalogAccess::open(refuse, v1_build());
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::VersionMismatch);

    EXPECT_EQ(file_digest(path), before);  // never written, never migrated
    EXPECT_FALSE(std::filesystem::exists(dir / L"backups"));
}

TEST(CatalogSchemaV2, AnUnknownRevisionOriginIsListedAsUnknownInEveryCatalog) {
    TempDir dir;
    const std::filesystem::path path = dir / L"lucent.catalog";
    make_v2_catalog(path, dir / L"photos");
    if (HasFatalFailure()) {
        return;
    }
    {
        // A later Lucent appends an origin this build does not know (the column has no CHECK, so
        // later versions can add values without a new schema): the history must still list it.
        std::unique_ptr<detail::Connection> raw = raw_connection(path);
        ASSERT_NE(raw, nullptr);
        LUCENT_ASSERT_OK(raw->exec(
            "INSERT INTO recipe_revisions (item_id, revision_number, parent_revision_id, created_at, origin, "
            "schema_name, schema_version, settings_json, settings_sha256, engine_version) "
            "VALUES (2, 1, NULL, 0, 'dream', 'lucent.develop', 3, '{}', "
            "x'44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a', '9.0.0')",
            "test"));
    }
    const auto check = [&](const std::unique_ptr<Catalog>& catalog) {
        Expected<RevisionHistory> history =
            catalog->query([](ReadTransaction& tx) { return tx.revision_history(ItemId{2}); });
        LUCENT_ASSERT_OK(history);
        ASSERT_EQ(history->revisions.size(), 1u);
        EXPECT_EQ(history->revisions[0].origin, RevisionOrigin::Unknown);
        EXPECT_EQ(to_string(RevisionOrigin::Unknown), "unknown");
        // Reading the revision itself works too (its settings are the reader's business).
        Expected<std::optional<RecipeRevision>> revision = catalog->query(
            [&](ReadTransaction& tx) { return tx.find_revision(history->revisions[0].id); });
        LUCENT_ASSERT_OK(revision);
        ASSERT_TRUE(revision->has_value());
        EXPECT_EQ((*revision)->info.origin, RevisionOrigin::Unknown);
        // Other items are unaffected.
        Expected<RevisionHistory> fine =
            catalog->query([](ReadTransaction& tx) { return tx.revision_history(ItemId{1}); });
        LUCENT_ASSERT_OK(fine);
        EXPECT_EQ(fine->revisions.size(), 3u);
    };
    {
        std::unique_ptr<Catalog> catalog = open_or_fail(path);
        ASSERT_NE(catalog, nullptr);
        check(catalog);
        // It can never be written: an unknown origin is not a valid input.
        NewRevision unknown;
        unknown.origin = RevisionOrigin::Unknown;
        unknown.schema_name = "lucent.develop";
        unknown.schema_version = 1;
        unknown.settings_json = "{}";
        EXPECT_FALSE(unknown.validate());
    }
    {
        std::unique_ptr<detail::Connection> raw = raw_connection(path);
        ASSERT_NE(raw, nullptr);
        LUCENT_ASSERT_OK(
            raw->exec("PRAGMA user_version = 3;"
                      "UPDATE catalog_meta SET value = '3' WHERE key = 'schema_version';"
                      "UPDATE catalog_meta SET value = '9.0.0' WHERE key = 'written_by';",
                      "test"));
    }
    std::unique_ptr<Catalog> newer = open_or_fail(path);
    ASSERT_NE(newer, nullptr);
    EXPECT_TRUE(newer->is_read_only());
    check(newer);
}

}  // namespace
}  // namespace lucent::catalog
