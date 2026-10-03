#pragma once

// Catalog schema: identification, integrity checks and ordered migrations. Private to
// lucent_catalog (and its white-box tests).
//
// Forward-compatibility contract (never changed by later schema versions, so that any Lucent can
// recognise any catalog and decide whether it may read it):
//   * PRAGMA application_id = kApplicationId identifies a Lucent catalog;
//   * PRAGMA user_version is the schema version;
//   * table catalog_meta(key TEXT PRIMARY KEY, value TEXT) holds at least the keys
//     "schema_version", "min_reader_version" (the oldest schema version whose readers can still
//     read this catalog read-only: a migration that only adds tables or columns keeps it, one that
//     renames, removes or reinterprets anything raises it), "created_by", "created_at" and
//     "written_by" (application versions as SemVer text; created_at in ms since 1970 UTC);
//   * table schema_history(version, applied_at, app_version) lists every applied migration.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lucent/catalog/catalog.hpp"
#include "lucent/catalog/types.hpp"
#include "lucent/core/error.hpp"

#include "sqlite.hpp"

namespace lucent::catalog::detail {

// PRAGMA application_id of Lucent catalogs: "LUCN" in ASCII.
inline constexpr std::int32_t kApplicationId = 0x4C55434E;

struct Migration {
    std::int32_t version = 0;             // schema version this migration produces (previous + 1)
    std::int32_t min_reader_version = 0;  // see the file comment; 1 ... version
    std::string_view description;
    // Applies the schema change inside the runner's transaction. Must not COMMIT, ROLLBACK or
    // change PRAGMA user_version / application_id; the runner does that.
    Status (*apply)(Connection& connection) = nullptr;
};

// The migrations of this build, versions 1 ... kSchemaVersion in order.
[[nodiscard]] std::span<const Migration> builtin_migrations() noexcept;

// InvalidArgument unless versions are 1, 2, ..., N, every apply is set and every
// min_reader_version is in 1 ... version.
[[nodiscard]] Status validate_migrations(std::span<const Migration> migrations);

enum class DatabaseKind : std::uint8_t {
    Empty = 1,    // no schema objects, application_id and user_version 0: a new catalog
    Lucent = 2,   // application_id matches
    Foreign = 3,  // some other SQLite database
};

struct DatabaseHeader {
    DatabaseKind kind = DatabaseKind::Empty;
    std::int32_t application_id = 0;
    std::int32_t user_version = 0;
};

// Reads application_id, user_version and the schema object count. A file that is not a SQLite
// database fails with Corrupt (SQLITE_NOTADB).
[[nodiscard]] Expected<DatabaseHeader> read_database_header(Connection& connection, std::string_view where);

// Runs the requested check; Corrupt with the first problems reported when it fails.
[[nodiscard]] Status check_integrity(Connection& connection, IntegrityCheck check, std::string_view where);

struct MigrationContext {
    std::string app_version;
    Timestamp now{};
};

// Applies every migration whose version is greater than `from`, in order, each in its own
// BEGIN IMMEDIATE transaction with foreign-key enforcement suspended and foreign_key_check run
// before commit; records schema_history and catalog_meta and sets user_version/application_id in
// the same transaction. Stops at the first failure (that migration is rolled back; earlier ones
// stay committed). Returns the versions reached. Where: "catalog.migrate".
[[nodiscard]] Expected<std::vector<std::int32_t>> apply_migrations(Connection& connection,
                                                                   std::span<const Migration> migrations,
                                                                   std::int32_t from,
                                                                   const MigrationContext& context);

struct CatalogMeta {
    std::int32_t schema_version = 0;
    std::int32_t min_reader_version = 0;
    std::string created_by;
    std::string written_by;
};

// Reads catalog_meta. A missing min_reader_version is taken as the schema version (readable by
// that version only); missing application versions are "".
[[nodiscard]] Expected<CatalogMeta> read_catalog_meta(Connection& connection, std::int32_t user_version,
                                                      std::string_view where);

}  // namespace lucent::catalog::detail
