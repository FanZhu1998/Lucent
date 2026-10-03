#include "schema.hpp"

#include <charconv>
#include <format>
#include <iterator>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

#include "lucent/core/log.hpp"
#include "lucent/core/text.hpp"

#include "codec.hpp"

namespace lucent::catalog::detail {
namespace {

constexpr std::string_view kWhereMigrate = "catalog.migrate";

// ---------------------------------------------------------------------------------------------
// Version 1 (W4): metadata, imports, sources and items.
//
// Conventions of every table:
//   * STRICT: column types are enforced;
//   * timestamps are INTEGER milliseconds since 1970-01-01 UTC, file times INTEGER FILETIME ticks;
//   * enumerations are TEXT spellings (types.hpp); their value sets are validated by the catalog
//     code, not by CHECK constraints, so a later version can add values without rebuilding tables;
//   * unsigned 64-bit identity fields are stored bit-for-bit in INTEGER columns;
//   * sources.origin_* hold the identity of the original of a Copied source while it was copied
//     (all four NULL when unknown or for Referenced sources);
//   * items_by_capture_time orders unknown capture times last with the sentinel '~' (U+007E),
//     which sorts after every valid capture time ("YYYY-..." starts with a digit). A single
//     expression column lets keyset paging seek in the index instead of scanning from the start.
// Later migrations add W6 recipes (v2) and W7 job tables; they reference items/sources by id.
// ---------------------------------------------------------------------------------------------
constexpr std::string_view kSchemaV1 = R"sql(
CREATE TABLE catalog_meta (
    key   TEXT NOT NULL PRIMARY KEY,
    value TEXT NOT NULL
) STRICT, WITHOUT ROWID;

CREATE TABLE schema_history (
    version     INTEGER NOT NULL PRIMARY KEY,
    applied_at  INTEGER NOT NULL,
    app_version TEXT    NOT NULL
) STRICT;

CREATE TABLE imports (
    id           INTEGER PRIMARY KEY,
    started_at   INTEGER NOT NULL,
    finished_at  INTEGER,
    outcome      TEXT    NOT NULL,
    roots_json   TEXT    NOT NULL CHECK (json_valid(roots_json) AND json_type(roots_json) = 'array'),
    options_json TEXT    NOT NULL CHECK (json_valid(options_json)),
    files_seen   INTEGER NOT NULL DEFAULT 0 CHECK (files_seen >= 0),
    added        INTEGER NOT NULL DEFAULT 0 CHECK (added >= 0),
    refreshed    INTEGER NOT NULL DEFAULT 0 CHECK (refreshed >= 0),
    unchanged    INTEGER NOT NULL DEFAULT 0 CHECK (unchanged >= 0),
    duplicates   INTEGER NOT NULL DEFAULT 0 CHECK (duplicates >= 0),
    skipped      INTEGER NOT NULL DEFAULT 0 CHECK (skipped >= 0),
    failed       INTEGER NOT NULL DEFAULT 0 CHECK (failed >= 0)
) STRICT;

CREATE TABLE sources (
    id            INTEGER PRIMARY KEY,
    path          TEXT    NOT NULL,
    path_key      TEXT    NOT NULL,
    volume_serial INTEGER NOT NULL,
    file_id       BLOB    NOT NULL CHECK (length(file_id) = 16),
    size          INTEGER NOT NULL,
    mtime         INTEGER NOT NULL,
    sha256        BLOB    CHECK (sha256 IS NULL OR length(sha256) = 32),
    import_mode   TEXT    NOT NULL,
    origin_path   TEXT,
    origin_volume_serial INTEGER,
    origin_file_id       BLOB    CHECK (origin_file_id IS NULL OR length(origin_file_id) = 16),
    origin_size          INTEGER,
    origin_mtime         INTEGER,
    storage_kind  TEXT    NOT NULL,
    state         TEXT    NOT NULL,
    first_seen    INTEGER NOT NULL,
    last_seen     INTEGER NOT NULL
) STRICT;

CREATE UNIQUE INDEX sources_by_path_key ON sources (path_key);
CREATE INDEX sources_by_sha256 ON sources (sha256);
CREATE INDEX sources_by_file_id ON sources (volume_serial, file_id);
CREATE INDEX sources_by_state ON sources (state);

CREATE TABLE items (
    id                   INTEGER PRIMARY KEY,
    source_id            INTEGER NOT NULL REFERENCES sources (id) ON DELETE CASCADE,
    subimage_index       INTEGER NOT NULL CHECK (subimage_index >= 0),
    make                 TEXT    NOT NULL,
    model                TEXT    NOT NULL,
    lens                 TEXT    NOT NULL,
    capture_time         TEXT,
    width                INTEGER NOT NULL CHECK (width >= 0),
    height               INTEGER NOT NULL CHECK (height >= 0),
    orientation          INTEGER NOT NULL CHECK (orientation BETWEEN 1 AND 8),
    format               TEXT    NOT NULL,
    support_status       TEXT    NOT NULL,
    support_reasons_json TEXT    NOT NULL CHECK (json_valid(support_reasons_json)),
    decoder_id           TEXT    NOT NULL,
    decoder_version      TEXT    NOT NULL,
    probed_at            INTEGER NOT NULL,
    pick                 TEXT    NOT NULL DEFAULT 'none',
    rating               INTEGER NOT NULL DEFAULT 0 CHECK (rating BETWEEN 0 AND 5)
) STRICT;

CREATE UNIQUE INDEX items_by_source ON items (source_id, subimage_index);
CREATE INDEX items_by_capture_time ON items (ifnull(capture_time, '~'), id);
CREATE INDEX items_by_support ON items (support_status);
)sql";

Status migrate_to_v1(Connection& connection) {
    return connection.exec(kSchemaV1, kWhereMigrate);
}

// ---------------------------------------------------------------------------------------------
// Version 2 (W6): edit revisions and the capture fields schema v1 lacked. Only adds tables,
// columns, indexes and triggers, so min_reader_version stays 1: a v1 build opens a v2 catalog
// read-only (catalog.hpp) and reads every v1 column unchanged.
//
// recipe_revisions: the immutable develop-settings history of an item (blueprint section 8).
//   * item_id: the item edited. ON DELETE CASCADE: removing an item (or its source) removes its
//     revisions with it; nothing else ever deletes a revision (trigger recipe_revisions_are_kept,
//     "prune nothing").
//   * revision_number: 1, 2, 3, ... per item in creation order, never reused (trigger
//     recipe_revisions_are_numbered: each insert must be the item's highest number + 1).
//   * parent_revision_id: the revision that was current when this one was created (NULL: it was
//     created from "as shot"); always a revision of the same item (trigger
//     recipe_revisions_parent_check). Undo moves items.current_revision_id along these links, and
//     an edit after an undo creates a new branch: earlier branches are kept.
//   * origin: "user", "auto", "reset", "paste" or "import" (types.hpp; validated by the code, no
//     CHECK, so later versions can append values).
//   * schema_name / schema_version: the settings document's own contract (for example
//     "lucent.develop" v1); settings_json is opaque to the catalog apart from being valid JSON,
//     settings_sha256 is the SHA-256 of its UTF-8 bytes (verified when it is read).
//   * engine_version: the version of the engine that produced or validated the settings.
//   * No row is ever updated (trigger recipe_revisions_are_immutable).
// items.current_revision_id: the revision the item shows now; NULL means "as shot". Must be a
//   revision of the same item (trigger items_current_revision_check).
// items.iso, exposure_time_s, f_number, focal_length_mm: capture settings (positive, NULL when
//   unknown); items.image_count: the images of the item's file (>= 1). All NULL for items
//   catalogued before v2 until they are probed again.
// ---------------------------------------------------------------------------------------------
constexpr std::string_view kSchemaV2 = R"sql(
CREATE TABLE recipe_revisions (
    id                 INTEGER PRIMARY KEY,
    item_id            INTEGER NOT NULL REFERENCES items (id) ON DELETE CASCADE,
    revision_number    INTEGER NOT NULL CHECK (revision_number >= 1),
    parent_revision_id INTEGER REFERENCES recipe_revisions (id),
    created_at         INTEGER NOT NULL,
    origin             TEXT    NOT NULL,
    schema_name        TEXT    NOT NULL CHECK (length(schema_name) > 0),
    schema_version     INTEGER NOT NULL CHECK (schema_version >= 1),
    settings_json      TEXT    NOT NULL CHECK (json_valid(settings_json)),
    settings_sha256    BLOB    NOT NULL CHECK (length(settings_sha256) = 32),
    engine_version     TEXT    NOT NULL,
    note               TEXT    NOT NULL DEFAULT ''
) STRICT;

CREATE UNIQUE INDEX recipe_revisions_by_item ON recipe_revisions (item_id, revision_number);
CREATE INDEX recipe_revisions_by_parent ON recipe_revisions (parent_revision_id);

ALTER TABLE items ADD COLUMN current_revision_id INTEGER REFERENCES recipe_revisions (id);
ALTER TABLE items ADD COLUMN iso REAL CHECK (iso IS NULL OR iso > 0);
ALTER TABLE items ADD COLUMN exposure_time_s REAL CHECK (exposure_time_s IS NULL OR exposure_time_s > 0);
ALTER TABLE items ADD COLUMN f_number REAL CHECK (f_number IS NULL OR f_number > 0);
ALTER TABLE items ADD COLUMN focal_length_mm REAL CHECK (focal_length_mm IS NULL OR focal_length_mm > 0);
ALTER TABLE items ADD COLUMN image_count INTEGER CHECK (image_count IS NULL OR image_count >= 1);

-- Foreign-key lookups when revisions are deleted with their item.
CREATE INDEX items_by_current_revision ON items (current_revision_id);

CREATE TRIGGER recipe_revisions_are_immutable BEFORE UPDATE ON recipe_revisions
BEGIN
    SELECT RAISE(ABORT, 'recipe revisions are immutable');
END;

-- Deletion only by cascade from the item (its row is gone when the cascade runs).
CREATE TRIGGER recipe_revisions_are_kept BEFORE DELETE ON recipe_revisions
WHEN EXISTS (SELECT 1 FROM items WHERE id = OLD.item_id)
BEGIN
    SELECT RAISE(ABORT, 'recipe revisions are kept while their item exists');
END;

CREATE TRIGGER recipe_revisions_are_numbered BEFORE INSERT ON recipe_revisions
WHEN NEW.revision_number IS NOT (SELECT ifnull(max(revision_number), 0) + 1 FROM recipe_revisions
                                 WHERE item_id = NEW.item_id)
BEGIN
    SELECT RAISE(ABORT, 'revision numbers increase by one per item');
END;

CREATE TRIGGER recipe_revisions_parent_check BEFORE INSERT ON recipe_revisions
WHEN NEW.parent_revision_id IS NOT NULL AND NOT EXISTS (
    SELECT 1 FROM recipe_revisions WHERE id = NEW.parent_revision_id AND item_id = NEW.item_id)
BEGIN
    SELECT RAISE(ABORT, 'the parent revision belongs to another item');
END;

CREATE TRIGGER items_current_revision_check BEFORE UPDATE OF current_revision_id ON items
WHEN NEW.current_revision_id IS NOT NULL AND NOT EXISTS (
    SELECT 1 FROM recipe_revisions WHERE id = NEW.current_revision_id AND item_id = NEW.id)
BEGIN
    SELECT RAISE(ABORT, 'the current revision belongs to another item');
END;
)sql";

Status migrate_to_v2(Connection& connection) {
    return connection.exec(kSchemaV2, kWhereMigrate);
}

constexpr Migration kBuiltinMigrations[] = {
    {1, 1, "catalog metadata, imports, sources and items (W4)", &migrate_to_v1},
    {2, 1, "recipe revisions and capture settings (W6)", &migrate_to_v2},
};
static_assert(std::size(kBuiltinMigrations) == static_cast<std::size_t>(kSchemaVersion),
              "every schema version needs exactly one migration");

Status set_meta(Connection& connection, std::string_view key, std::string_view value, bool replace) {
    const std::string_view sql = replace ? "INSERT INTO catalog_meta (key, value) VALUES (?1, ?2) "
                                           "ON CONFLICT (key) DO UPDATE SET value = excluded.value"
                                         : "INSERT INTO catalog_meta (key, value) VALUES (?1, ?2) "
                                           "ON CONFLICT (key) DO NOTHING";
    Expected<Statement> statement = connection.prepare(sql, kWhereMigrate);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    statement->bind_text(1, key).bind_text(2, value);
    return statement->run();
}

Status run_one(Connection& connection, const Migration& migration, const MigrationContext& context) {
    Expected<TransactionGuard> transaction =
        TransactionGuard::begin(connection, TransactionGuard::Kind::Immediate, kWhereMigrate);
    if (!transaction) {
        return Unexpected(std::move(transaction.error()));
    }
    if (Status applied = migration.apply(connection); !applied) {
        return applied;
    }

    // Foreign keys are not enforced during a migration (tables may be rebuilt), so verify them
    // before committing.
    {
        Expected<Statement> check = connection.prepare("PRAGMA foreign_key_check", kWhereMigrate);
        if (!check) {
            return Unexpected(std::move(check.error()));
        }
        Expected<bool> violation = check->step();
        if (!violation) {
            return Unexpected(std::move(violation.error()));
        }
        if (*violation) {
            return fail(
                ErrorCode::Corrupt,
                std::format("migration left a foreign key violation in table '{}'", check->column_text(0)),
                std::string(kWhereMigrate));
        }
    }

    // PRAGMA arguments cannot be bound; both values are integers produced here, not input.
    if (Status s = connection.exec(std::format("PRAGMA application_id = {}", kApplicationId), kWhereMigrate);
        !s) {
        return s;
    }
    if (Status s = connection.exec(std::format("PRAGMA user_version = {}", migration.version), kWhereMigrate);
        !s) {
        return s;
    }
    {
        Expected<Statement> history = connection.prepare(
            "INSERT INTO schema_history (version, applied_at, app_version) VALUES (?1, ?2, ?3)",
            kWhereMigrate);
        if (!history) {
            return Unexpected(std::move(history.error()));
        }
        history->bind_int64(1, migration.version)
            .bind_int64(2, to_db(context.now))
            .bind_text(3, context.app_version);
        if (Status s = history->run(); !s) {
            return s;
        }
    }
    const std::string version_text = std::to_string(migration.version);
    const std::string reader_text = std::to_string(migration.min_reader_version);
    const std::string now_text = std::to_string(to_db(context.now));
    for (const auto& [key, value, replace] : {
             std::tuple<std::string_view, std::string_view, bool>{"schema_version", version_text, true},
             {"min_reader_version", reader_text, true},
             {"written_by", context.app_version, true},
             {"created_by", context.app_version, false},
             {"created_at", now_text, false},
         }) {
        if (Status s = set_meta(connection, key, value, replace); !s) {
            return s;
        }
    }
    return transaction->commit();
}

Expected<std::int32_t> parse_int32(std::string_view text) {
    std::int32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) {
        return fail(ErrorCode::Corrupt, std::format("catalog_meta holds a malformed number '{}'", text),
                    "catalog.meta");
    }
    return value;
}

}  // namespace

std::span<const Migration> builtin_migrations() noexcept {
    return kBuiltinMigrations;
}

Status validate_migrations(std::span<const Migration> migrations) {
    if (migrations.empty()) {
        return fail(ErrorCode::InvalidArgument, "no migrations", std::string(kWhereMigrate));
    }
    for (std::size_t i = 0; i < migrations.size(); ++i) {
        const Migration& migration = migrations[i];
        if (migration.version != static_cast<std::int32_t>(i + 1) || migration.apply == nullptr ||
            migration.min_reader_version < 1 || migration.min_reader_version > migration.version) {
            return fail(ErrorCode::InvalidArgument, std::format("invalid migration at position {}", i),
                        std::string(kWhereMigrate));
        }
    }
    return ok();
}

Expected<DatabaseHeader> read_database_header(Connection& connection, std::string_view where) {
    Expected<std::int64_t> application_id = connection.query_int64("PRAGMA application_id", where);
    if (!application_id) {
        return Unexpected(std::move(application_id.error()));
    }
    Expected<std::int64_t> user_version = connection.query_int64("PRAGMA user_version", where);
    if (!user_version) {
        return Unexpected(std::move(user_version.error()));
    }
    Expected<std::int64_t> objects = connection.query_int64("SELECT count(*) FROM sqlite_schema", where);
    if (!objects) {
        return Unexpected(std::move(objects.error()));
    }
    DatabaseHeader header;
    header.application_id = static_cast<std::int32_t>(*application_id);
    header.user_version = static_cast<std::int32_t>(*user_version);
    if (header.application_id == kApplicationId) {
        header.kind = DatabaseKind::Lucent;
    } else if (header.application_id == 0 && header.user_version == 0 && *objects == 0) {
        header.kind = DatabaseKind::Empty;
    } else {
        header.kind = DatabaseKind::Foreign;
    }
    return header;
}

Status check_integrity(Connection& connection, IntegrityCheck check, std::string_view where) {
    if (check == IntegrityCheck::None) {
        return ok();
    }
    const std::string_view sql =
        check == IntegrityCheck::Full ? "PRAGMA integrity_check(20)" : "PRAGMA quick_check(20)";
    Expected<Statement> statement = connection.prepare(sql, where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    std::string problems;
    for (;;) {
        Expected<bool> row = statement->step();
        if (!row) {
            return Unexpected(std::move(row.error()));  // a damaged file can fail the check itself
        }
        if (!*row) {
            break;
        }
        const std::string_view line = statement->column_text(0);
        if (line == "ok") {
            continue;
        }
        if (!problems.empty()) {
            problems += "; ";
        }
        problems.append(line);
    }
    if (!problems.empty()) {
        return fail(ErrorCode::Corrupt, "catalog integrity check failed: " + problems, std::string(where));
    }
    if (check == IntegrityCheck::Full) {
        Expected<Statement> foreign = connection.prepare("PRAGMA foreign_key_check", where);
        if (!foreign) {
            return Unexpected(std::move(foreign.error()));
        }
        Expected<bool> violation = foreign->step();
        if (!violation) {
            return Unexpected(std::move(violation.error()));
        }
        if (*violation) {
            return fail(
                ErrorCode::Corrupt,
                std::format("catalog foreign key check failed in table '{}'", foreign->column_text(0)),
                std::string(where));
        }
    }
    return ok();
}

Expected<std::vector<std::int32_t>> apply_migrations(Connection& connection,
                                                     std::span<const Migration> migrations, std::int32_t from,
                                                     const MigrationContext& context) {
    if (Status valid = validate_migrations(migrations); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    std::vector<std::int32_t> applied;
    for (const Migration& migration : migrations) {
        if (migration.version <= from) {
            continue;
        }
        if (Status off = connection.exec("PRAGMA foreign_keys = OFF", kWhereMigrate); !off) {
            return Unexpected(std::move(off.error()));
        }
        Status status = run_one(connection, migration, context);
        if (Status on = connection.exec("PRAGMA foreign_keys = ON", kWhereMigrate); !on && status) {
            status = std::move(on);
        }
        if (!status) {
            Error error = std::move(status.error());
            error.message = std::format("migration to schema v{} ({}) failed: {}", migration.version,
                                        migration.description, error.message);
            error.where = std::string(kWhereMigrate);
            core::log_error("catalog", "{}", error.describe());
            return Unexpected(std::move(error));
        }
        core::log_info("catalog", "catalog {} migrated to schema v{} ({})",
                       core::path_to_utf8_lossy(connection.path()), migration.version, migration.description);
        applied.push_back(migration.version);
    }
    return applied;
}

Expected<CatalogMeta> read_catalog_meta(Connection& connection, std::int32_t user_version,
                                        std::string_view where) {
    CatalogMeta meta;
    meta.schema_version = user_version;
    meta.min_reader_version = user_version;
    Expected<Statement> statement = connection.prepare("SELECT key, value FROM catalog_meta", where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    for (;;) {
        Expected<bool> row = statement->step();
        if (!row) {
            return Unexpected(std::move(row.error()));
        }
        if (!*row) {
            break;
        }
        const std::string_view key = statement->column_text(0);
        const std::string_view value = statement->column_text(1);
        if (key == "min_reader_version") {
            Expected<std::int32_t> number = parse_int32(value);
            if (!number) {
                return Unexpected(std::move(number.error()));
            }
            meta.min_reader_version = *number;
        } else if (key == "schema_version") {
            Expected<std::int32_t> number = parse_int32(value);
            if (!number) {
                return Unexpected(std::move(number.error()));
            }
            meta.schema_version = *number;
        } else if (key == "created_by") {
            meta.created_by = std::string(value);
        } else if (key == "written_by") {
            meta.written_by = std::string(value);
        }
    }
    return meta;
}

}  // namespace lucent::catalog::detail
