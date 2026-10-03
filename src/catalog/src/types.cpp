#include "lucent/catalog/types.hpp"

#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

#include "lucent/catalog/catalog.hpp"

#include "codec.hpp"

namespace lucent::catalog {

Timestamp current_time() noexcept {
    return std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
}

std::string_view to_string(ImportMode value) noexcept {
    switch (value) {
        case ImportMode::Referenced:
            return "referenced";
        case ImportMode::Copied:
            return "copied";
    }
    return "invalid";
}

std::string_view to_string(SourceState value) noexcept {
    switch (value) {
        case SourceState::Present:
            return "present";
        case SourceState::Missing:
            return "missing";
        case SourceState::Changed:
            return "changed";
    }
    return "invalid";
}

std::string_view to_string(SupportStatus value) noexcept {
    switch (value) {
        case SupportStatus::Supported:
            return "supported";
        case SupportStatus::Limited:
            return "limited";
        case SupportStatus::Unsupported:
            return "unsupported";
    }
    return "invalid";
}

std::string_view to_string(PickFlag value) noexcept {
    switch (value) {
        case PickFlag::None:
            return "none";
        case PickFlag::Picked:
            return "picked";
        case PickFlag::Rejected:
            return "rejected";
    }
    return "invalid";
}

std::string_view to_string(ImportOutcome value) noexcept {
    switch (value) {
        case ImportOutcome::Running:
            return "running";
        case ImportOutcome::Completed:
            return "completed";
        case ImportOutcome::Cancelled:
            return "cancelled";
        case ImportOutcome::Failed:
            return "failed";
        case ImportOutcome::Interrupted:
            return "interrupted";
    }
    return "invalid";
}

std::string_view to_string(SourceUpsertOutcome value) noexcept {
    switch (value) {
        case SourceUpsertOutcome::Inserted:
            return "inserted";
        case SourceUpsertOutcome::Unchanged:
            return "unchanged";
        case SourceUpsertOutcome::Restored:
            return "restored";
        case SourceUpsertOutcome::Replaced:
            return "replaced";
    }
    return "invalid";
}

std::string_view to_string(BackupKind kind) noexcept {
    switch (kind) {
        case BackupKind::Manual:
            return "manual";
        case BackupKind::PreMigration:
            return "pre-migration";
        case BackupKind::PreRestore:
            return "pre-restore";
    }
    return "invalid";
}

Status ItemFlags::validate() const {
    if (pick != PickFlag::None && pick != PickFlag::Picked && pick != PickFlag::Rejected) {
        return fail(ErrorCode::InvalidArgument, "unknown pick flag value", "catalog.item.flags");
    }
    if (rating > 5) {
        return fail(ErrorCode::InvalidArgument, "rating must be 0..5", "catalog.item.flags");
    }
    return ok();
}

namespace detail {
namespace {

template <class Enum, std::size_t N>
std::optional<Enum> parse_with(std::string_view text, const Enum (&values)[N]) noexcept {
    for (const Enum value : values) {
        if (to_string(value) == text) {
            return value;
        }
    }
    return std::nullopt;
}

bool digits(std::string_view text, std::size_t first, std::size_t count) noexcept {
    for (std::size_t i = first; i < first + count; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
    }
    return true;
}

int number(std::string_view text, std::size_t first, std::size_t count) noexcept {
    int value = 0;
    for (std::size_t i = first; i < first + count; ++i) {
        value = value * 10 + (text[i] - '0');
    }
    return value;
}

}  // namespace

std::optional<ImportMode> parse_import_mode(std::string_view text) noexcept {
    static constexpr ImportMode kValues[] = {ImportMode::Referenced, ImportMode::Copied};
    return parse_with(text, kValues);
}

std::string_view storage_kind_text(StorageKind kind) noexcept {
    // Persisted spellings, owned by the catalog schema (they equal core::to_string() today, but a
    // change there must not change stored data).
    switch (kind) {
        case StorageKind::Unknown:
            return "unknown";
        case StorageKind::Fixed:
            return "fixed";
        case StorageKind::Removable:
            return "removable";
        case StorageKind::Network:
            return "network";
    }
    return "invalid";
}

std::optional<StorageKind> parse_storage_kind(std::string_view text) noexcept {
    static constexpr StorageKind kValues[] = {StorageKind::Unknown, StorageKind::Fixed,
                                              StorageKind::Removable, StorageKind::Network};
    for (const StorageKind value : kValues) {
        if (storage_kind_text(value) == text) {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<SourceState> parse_source_state(std::string_view text) noexcept {
    static constexpr SourceState kValues[] = {SourceState::Present, SourceState::Missing,
                                              SourceState::Changed};
    return parse_with(text, kValues);
}

std::optional<SupportStatus> parse_support_status(std::string_view text) noexcept {
    static constexpr SupportStatus kValues[] = {SupportStatus::Supported, SupportStatus::Limited,
                                                SupportStatus::Unsupported};
    return parse_with(text, kValues);
}

std::optional<PickFlag> parse_pick_flag(std::string_view text) noexcept {
    static constexpr PickFlag kValues[] = {PickFlag::None, PickFlag::Picked, PickFlag::Rejected};
    return parse_with(text, kValues);
}

std::optional<ImportOutcome> parse_import_outcome(std::string_view text) noexcept {
    static constexpr ImportOutcome kValues[] = {ImportOutcome::Running, ImportOutcome::Completed,
                                                ImportOutcome::Cancelled, ImportOutcome::Failed,
                                                ImportOutcome::Interrupted};
    return parse_with(text, kValues);
}

std::optional<BackupKind> parse_backup_kind(std::string_view text) noexcept {
    static constexpr BackupKind kValues[] = {BackupKind::Manual, BackupKind::PreMigration,
                                             BackupKind::PreRestore};
    return parse_with(text, kValues);
}

bool is_valid(ImportMode value) noexcept {
    return parse_import_mode(to_string(value)).has_value();
}
bool is_valid(StorageKind value) noexcept {
    return storage_kind_text(value) != "invalid";
}
bool is_valid(SourceState value) noexcept {
    return parse_source_state(to_string(value)).has_value();
}
bool is_valid(SupportStatus value) noexcept {
    return parse_support_status(to_string(value)).has_value();
}
bool is_valid(ImportOutcome value) noexcept {
    return parse_import_outcome(to_string(value)).has_value();
}

bool is_valid_capture_time(std::string_view text) noexcept {
    // 0123456789012345678
    // YYYY-MM-DDTHH:MM:SS
    if (text.size() != 19 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
        text[16] != ':') {
        return false;
    }
    if (!digits(text, 0, 4) || !digits(text, 5, 2) || !digits(text, 8, 2) || !digits(text, 11, 2) ||
        !digits(text, 14, 2) || !digits(text, 17, 2)) {
        return false;
    }
    const int month = number(text, 5, 2);
    const int day = number(text, 8, 2);
    const int hour = number(text, 11, 2);
    const int minute = number(text, 14, 2);
    const int second = number(text, 17, 2);
    // Seconds up to 60 for leap seconds; day validity per month is not checked (camera clocks).
    return month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour <= 23 && minute <= 59 && second <= 60;
}

std::string json_string(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (byte < 0x20) {
                    char escape[8];
                    std::snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned>(byte));
                    out += escape;
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace detail
}  // namespace lucent::catalog
