#include "backup.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <format>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include <windows.h>

#include "lucent/core/log.hpp"
#include "lucent/core/text.hpp"

#include "codec.hpp"
#include "files.hpp"
#include "schema.hpp"

namespace lucent::catalog::detail {
namespace {

constexpr std::wstring_view kBackupExtension = L".bak";

bool equal_ignoring_case(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    if (a.empty()) {
        return true;
    }
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()),
                                  TRUE) == CSTR_EQUAL;
}

std::wstring format_time(Timestamp time) {
    const auto days = std::chrono::floor<std::chrono::days>(time);
    const std::chrono::year_month_day date{days};
    const std::chrono::hh_mm_ss clock{time - days};
    return std::format(L"{:04}{:02}{:02}T{:02}{:02}{:02}{:03}Z", static_cast<int>(date.year()),
                       static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                       clock.hours().count(), clock.minutes().count(), clock.seconds().count(),
                       clock.subseconds().count());
}

std::optional<int> parse_digits(std::wstring_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    int value = 0;
    for (const wchar_t c : text) {
        if (c < L'0' || c > L'9' || value > 100'000'000) {
            return std::nullopt;
        }
        value = value * 10 + static_cast<int>(c - L'0');
    }
    return value;
}

std::optional<Timestamp> parse_time(std::wstring_view text) {
    // yyyymmddThhmmssfffZ
    if (text.size() != 19 || text[8] != L'T' || text[18] != L'Z') {
        return std::nullopt;
    }
    const auto year = parse_digits(text.substr(0, 4));
    const auto month = parse_digits(text.substr(4, 2));
    const auto day = parse_digits(text.substr(6, 2));
    const auto hour = parse_digits(text.substr(9, 2));
    const auto minute = parse_digits(text.substr(11, 2));
    const auto second = parse_digits(text.substr(13, 2));
    const auto milli = parse_digits(text.substr(15, 3));
    if (!year || !month || !day || !hour || !minute || !second || !milli || *hour > 23 || *minute > 59 ||
        *second > 59) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year(*year),
                                           std::chrono::month(static_cast<unsigned>(*month)),
                                           std::chrono::day(static_cast<unsigned>(*day))};
    if (!date.ok()) {
        return std::nullopt;
    }
    return Timestamp(std::chrono::sys_days(date)) + std::chrono::hours(*hour) +
           std::chrono::minutes(*minute) + std::chrono::seconds(*second) + std::chrono::milliseconds(*milli);
}

void remove_database_files(const std::filesystem::path& database) {
    for (const wchar_t* suffix : {L"", L"-journal", L"-wal", L"-shm"}) {
        std::filesystem::path file = database;
        file += suffix;
        if (Status removed = delete_file_if_exists(file, "catalog.backup"); !removed) {
            core::log_warn("catalog", "cannot remove temporary backup file: {}", removed.error().describe());
        }
    }
}

}  // namespace

std::wstring format_backup_name(const std::filesystem::path& catalog_file_name, const BackupName& name) {
    std::wstring kind;
    for (const char c : to_string(name.kind)) {
        kind.push_back(static_cast<wchar_t>(c));
    }
    return std::format(L"{}.{}.v{}.{}{}", catalog_file_name.native(), format_time(name.created_at),
                       name.schema_version, kind, kBackupExtension);
}

std::optional<BackupName> parse_backup_name(const std::filesystem::path& catalog_file_name,
                                            std::wstring_view file_name) {
    const std::wstring prefix = catalog_file_name.native() + L".";
    if (file_name.size() <= prefix.size() + kBackupExtension.size() ||
        !equal_ignoring_case(file_name.substr(0, prefix.size()), prefix) ||
        !equal_ignoring_case(file_name.substr(file_name.size() - kBackupExtension.size()),
                             kBackupExtension)) {
        return std::nullopt;
    }
    std::wstring_view rest =
        file_name.substr(prefix.size(), file_name.size() - prefix.size() - kBackupExtension.size());
    // <time>.v<schema>.<kind>
    const std::size_t first = rest.find(L'.');
    if (first == std::wstring_view::npos) {
        return std::nullopt;
    }
    const std::size_t second = rest.find(L'.', first + 1);
    if (second == std::wstring_view::npos || rest.find(L'.', second + 1) != std::wstring_view::npos) {
        return std::nullopt;
    }
    const std::wstring_view time_text = rest.substr(0, first);
    const std::wstring_view version_text = rest.substr(first + 1, second - first - 1);
    const std::wstring_view kind_text = rest.substr(second + 1);

    const std::optional<Timestamp> time = parse_time(time_text);
    if (!time || version_text.size() < 2 || version_text[0] != L'v') {
        return std::nullopt;
    }
    const std::optional<int> version = parse_digits(version_text.substr(1));
    if (!version) {
        return std::nullopt;
    }
    std::string kind_ascii;
    for (const wchar_t c : kind_text) {
        if (c > 0x7F) {
            return std::nullopt;
        }
        kind_ascii.push_back(static_cast<char>(c));
    }
    const std::optional<BackupKind> kind = parse_backup_kind(kind_ascii);
    if (!kind) {
        return std::nullopt;
    }
    return BackupName{*time, static_cast<std::int32_t>(*version), *kind};
}

Expected<DatabaseCheck> verify_database_file(const std::filesystem::path& path, IntegrityCheck check,
                                             std::chrono::milliseconds busy_timeout, std::string_view where) {
    Expected<std::unique_ptr<Connection>> connection =
        Connection::open(path, Access::ReadOnly, busy_timeout, where);
    if (!connection) {
        return Unexpected(std::move(connection.error()));
    }
    Expected<DatabaseHeader> header = read_database_header(**connection, where);
    if (!header) {
        return Unexpected(std::move(header.error()));
    }
    if (Status integrity = check_integrity(**connection, check, where); !integrity) {
        return Unexpected(std::move(integrity.error()));
    }
    return DatabaseCheck{header->application_id, header->user_version};
}

Expected<std::uint64_t> write_verified_copy(Connection& source, const std::filesystem::path& destination,
                                            std::int32_t expected_version,
                                            std::chrono::milliseconds busy_timeout, std::string_view where) {
    Expected<std::optional<FileFacts>> existing = file_facts(destination, where);
    if (!existing) {
        return Unexpected(std::move(existing.error()));
    }
    if (existing->has_value()) {
        return fail(ErrorCode::AlreadyExists,
                    "backup destination '" + core::path_to_utf8_lossy(destination) + "' already exists",
                    std::string(where));
    }
    std::filesystem::path temporary = destination;
    temporary += L"." + random_hex_suffix() + L".partial";

    auto attempt = [&]() -> Expected<std::uint64_t> {
        {
            Expected<std::unique_ptr<Connection>> copy =
                Connection::open(temporary, Access::ReadWriteCreate, busy_timeout, where);
            if (!copy) {
                return Unexpected(std::move(copy.error()));
            }
            if (Status copied = copy_database(source, **copy, where); !copied) {
                return Unexpected(std::move(copied.error()));
            }
        }
        {
            // The copied header may carry the source's WAL flag; switching the reopened file to
            // rollback-journal mode makes the backup one self-contained file.
            Expected<std::unique_ptr<Connection>> copy =
                Connection::open(temporary, Access::ReadWrite, busy_timeout, where);
            if (!copy) {
                return Unexpected(std::move(copy.error()));
            }
            Expected<std::string> mode = (*copy)->query_text("PRAGMA journal_mode = DELETE", where);
            if (!mode) {
                return Unexpected(std::move(mode.error()));
            }
            if (*mode != "delete") {
                return fail(ErrorCode::IoError,
                            "cannot switch the backup to rollback-journal mode (" + *mode + ")",
                            std::string(where));
            }
        }
        Expected<DatabaseCheck> check =
            verify_database_file(temporary, IntegrityCheck::Full, busy_timeout, where);
        if (!check) {
            Error error = std::move(check.error());
            error.message = "backup verification failed: " + error.message;
            return Unexpected(std::move(error));
        }
        if (check->application_id != kApplicationId || check->user_version != expected_version) {
            return fail(
                ErrorCode::Corrupt,
                std::format("backup verification failed: application id {:#x}, schema v{} (expected v{})",
                            static_cast<std::uint32_t>(check->application_id), check->user_version,
                            expected_version),
                std::string(where));
        }
        if (Status renamed = rename_no_replace(temporary, destination, where); !renamed) {
            return Unexpected(std::move(renamed.error()));
        }
        Expected<std::optional<FileFacts>> facts = file_facts(destination, where);
        if (!facts || !facts->has_value()) {
            return fail(ErrorCode::IoError, "backup vanished after it was written", std::string(where));
        }
        return (*facts)->size_bytes;
    };

    Expected<std::uint64_t> result = attempt();
    // On success only the journal side files of the temporary name can remain; on failure the
    // temporary database too.
    remove_database_files(temporary);
    return result;
}

Expected<BackupInfo> write_backup_into(Connection& source, const std::filesystem::path& catalog_path,
                                       const std::filesystem::path& directory, BackupKind kind,
                                       std::int32_t schema_version, std::uint32_t keep,
                                       std::chrono::milliseconds busy_timeout, std::string_view where) {
    if (Status created = ensure_directory(directory, where); !created) {
        return Unexpected(std::move(created.error()));
    }
    const std::filesystem::path catalog_name = catalog_path.filename();
    Timestamp time = current_time();
    std::filesystem::path destination;
    for (int attempt = 0;; ++attempt) {
        destination = directory / format_backup_name(catalog_name, BackupName{time, schema_version, kind});
        Expected<std::optional<FileFacts>> existing = file_facts(destination, where);
        if (!existing) {
            return Unexpected(std::move(existing.error()));
        }
        if (!existing->has_value()) {
            break;
        }
        if (attempt >= 1000) {
            return fail(ErrorCode::AlreadyExists, "cannot find a free backup file name", std::string(where));
        }
        time += std::chrono::milliseconds(1);
    }
    Expected<std::uint64_t> size =
        write_verified_copy(source, destination, schema_version, busy_timeout, where);
    if (!size) {
        return Unexpected(std::move(size.error()));
    }
    core::log_info("catalog", "wrote {} backup {}", to_string(kind), core::path_to_utf8_lossy(destination));
    prune_backups(directory, catalog_path, kind, keep);
    return BackupInfo{destination, kind, time, schema_version, *size};
}

Expected<std::vector<BackupInfo>> list_backups_in(const std::filesystem::path& directory,
                                                  const std::filesystem::path& catalog_path,
                                                  std::string_view where) {
    Expected<std::vector<std::wstring>> names = list_file_names(directory, where);
    if (!names) {
        return Unexpected(std::move(names.error()));
    }
    const std::filesystem::path catalog_name = catalog_path.filename();
    std::vector<BackupInfo> backups;
    for (const std::wstring& name : *names) {
        const std::optional<BackupName> parsed = parse_backup_name(catalog_name, name);
        if (!parsed) {
            continue;
        }
        BackupInfo info{directory / name, parsed->kind, parsed->created_at, parsed->schema_version, 0};
        Expected<std::optional<FileFacts>> facts = file_facts(info.path, where);
        if (facts && facts->has_value()) {
            info.size_bytes = (*facts)->size_bytes;
        }
        backups.push_back(std::move(info));
    }
    std::sort(backups.begin(), backups.end(), [](const BackupInfo& a, const BackupInfo& b) {
        if (a.created_at != b.created_at) {
            return a.created_at > b.created_at;
        }
        return a.path.native() > b.path.native();
    });
    return backups;
}

void prune_backups(const std::filesystem::path& directory, const std::filesystem::path& catalog_path,
                   BackupKind kind, std::uint32_t keep) {
    Expected<std::vector<BackupInfo>> backups = list_backups_in(directory, catalog_path, "catalog.backup");
    if (!backups) {
        core::log_warn("catalog", "cannot list backups for pruning: {}", backups.error().describe());
        return;
    }
    std::uint32_t kept = 0;
    for (const BackupInfo& backup : *backups) {
        if (backup.kind != kind) {
            continue;
        }
        if (kept < keep) {
            ++kept;
            continue;
        }
        if (Status removed = delete_file_if_exists(backup.path, "catalog.backup"); !removed) {
            core::log_warn("catalog", "cannot delete old backup: {}", removed.error().describe());
        } else {
            core::log_info("catalog", "deleted old backup {}", core::path_to_utf8_lossy(backup.path));
        }
    }
}

}  // namespace lucent::catalog::detail
