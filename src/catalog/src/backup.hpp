#pragma once

// Verified online backups of a catalog and the backup folder's naming and retention. Private to
// lucent_catalog.
//
// Backup file names: "<catalog file name>.<UTC yyyymmddThhmmssfffZ>.v<schema>.<kind>.bak", for
// example "lucent.catalog.20261003T104643123Z.v1.pre-migration.bak". Names sort chronologically
// per catalog; files that do not parse are never listed, pruned or deleted.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lucent/catalog/catalog.hpp"
#include "lucent/core/error.hpp"

#include "sqlite.hpp"

namespace lucent::catalog::detail {

struct BackupName {
    Timestamp created_at{};
    std::int32_t schema_version = 0;
    BackupKind kind = BackupKind::Manual;
};

[[nodiscard]] std::wstring format_backup_name(const std::filesystem::path& catalog_file_name,
                                              const BackupName& name);

// std::nullopt when `file_name` is not a backup name of `catalog_file_name` (the prefix is compared
// case-insensitively, like Windows file names).
[[nodiscard]] std::optional<BackupName> parse_backup_name(const std::filesystem::path& catalog_file_name,
                                                          std::wstring_view file_name);

struct DatabaseCheck {
    std::int32_t application_id = 0;
    std::int32_t user_version = 0;
};

// Opens `path` read-only and runs the requested integrity check. Corrupt when the file is not a
// database or the check fails.
[[nodiscard]] Expected<DatabaseCheck> verify_database_file(const std::filesystem::path& path,
                                                           IntegrityCheck check,
                                                           std::chrono::milliseconds busy_timeout,
                                                           std::string_view where);

// Writes a verified, self-contained (rollback-journal mode) copy of `source`'s database to
// `destination` through a temporary sibling, never replacing an existing file (AlreadyExists).
// Verification: integrity_check, foreign_key_check, application id and `expected_version`.
// Returns the size of the copy.
[[nodiscard]] Expected<std::uint64_t> write_verified_copy(Connection& source,
                                                          const std::filesystem::path& destination,
                                                          std::int32_t expected_version,
                                                          std::chrono::milliseconds busy_timeout,
                                                          std::string_view where);

// write_verified_copy() into `directory` (created when missing) under a new backup name, then
// prune_backups() for that kind.
[[nodiscard]] Expected<BackupInfo> write_backup_into(Connection& source,
                                                     const std::filesystem::path& catalog_path,
                                                     const std::filesystem::path& directory, BackupKind kind,
                                                     std::int32_t schema_version, std::uint32_t keep,
                                                     std::chrono::milliseconds busy_timeout,
                                                     std::string_view where);

// Backups of `catalog_path` in `directory`, newest first; empty when the folder does not exist.
[[nodiscard]] Expected<std::vector<BackupInfo>> list_backups_in(const std::filesystem::path& directory,
                                                                const std::filesystem::path& catalog_path,
                                                                std::string_view where);

// Deletes the oldest backups of `kind` beyond the newest `keep`. Failures are logged, not returned:
// a backup that was just written must not be reported as failed because an old one is locked.
void prune_backups(const std::filesystem::path& directory, const std::filesystem::path& catalog_path,
                   BackupKind kind, std::uint32_t keep);

}  // namespace lucent::catalog::detail
