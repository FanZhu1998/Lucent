#pragma once

// Conversions between catalog values and their stored form, private to lucent_catalog.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "lucent/catalog/catalog.hpp"
#include "lucent/catalog/jobs.hpp"
#include "lucent/catalog/types.hpp"

namespace lucent::catalog::detail {

// Inverses of the to_string() spellings (exact, case-sensitive); std::nullopt for anything else.
[[nodiscard]] std::optional<ImportMode> parse_import_mode(std::string_view text) noexcept;
[[nodiscard]] std::optional<StorageKind> parse_storage_kind(std::string_view text) noexcept;
[[nodiscard]] std::optional<SourceState> parse_source_state(std::string_view text) noexcept;
[[nodiscard]] std::optional<SupportStatus> parse_support_status(std::string_view text) noexcept;
[[nodiscard]] std::optional<PickFlag> parse_pick_flag(std::string_view text) noexcept;
[[nodiscard]] std::optional<ImportOutcome> parse_import_outcome(std::string_view text) noexcept;
[[nodiscard]] std::optional<RevisionOrigin> parse_revision_origin(std::string_view text) noexcept;
[[nodiscard]] std::optional<BackupKind> parse_backup_kind(std::string_view text) noexcept;
// Batch journal spellings (schema v3; defined in jobs.cpp).
[[nodiscard]] std::optional<BatchState> parse_batch_state(std::string_view text) noexcept;
[[nodiscard]] std::optional<JobStage> parse_job_stage(std::string_view text) noexcept;
[[nodiscard]] std::optional<FailureClass> parse_failure_class(std::string_view text) noexcept;
[[nodiscard]] std::optional<PublicationStep> parse_publication_step(std::string_view text) noexcept;
// A stored numeric lucent::ErrorCode; codes this build does not know read as ErrorCode::Unknown.
[[nodiscard]] ErrorCode error_code_from_db(std::int64_t value) noexcept;

// Persisted spelling of a storage kind; "invalid" for values outside the enumeration.
[[nodiscard]] std::string_view storage_kind_text(StorageKind kind) noexcept;

// Validity of enumeration values passed in by callers.
[[nodiscard]] bool is_valid(ImportMode value) noexcept;
[[nodiscard]] bool is_valid(StorageKind value) noexcept;
[[nodiscard]] bool is_valid(SourceState value) noexcept;
[[nodiscard]] bool is_valid(SupportStatus value) noexcept;
[[nodiscard]] bool is_valid(ImportOutcome value) noexcept;
[[nodiscard]] bool is_valid(RevisionOrigin value) noexcept;

[[nodiscard]] constexpr std::int64_t to_db(Timestamp time) noexcept {
    return time.time_since_epoch().count();
}
[[nodiscard]] constexpr Timestamp timestamp_from_db(std::int64_t milliseconds) noexcept {
    return Timestamp(std::chrono::milliseconds(milliseconds));
}

// SQLite integers are signed 64-bit; unsigned identity fields are stored bit-for-bit.
[[nodiscard]] constexpr std::int64_t to_db(std::uint64_t value) noexcept {
    return static_cast<std::int64_t>(value);
}
[[nodiscard]] constexpr std::uint64_t unsigned_from_db(std::int64_t value) noexcept {
    return static_cast<std::uint64_t>(value);
}

// "YYYY-MM-DDTHH:MM:SS" with digits in the digit positions and plausible field ranges.
[[nodiscard]] bool is_valid_capture_time(std::string_view text) noexcept;

// `text` as a JSON string literal including the quotes (RFC 8259 escaping of '"', '\' and
// control characters; other UTF-8 passes through).
[[nodiscard]] std::string json_string(std::string_view text);

}  // namespace lucent::catalog::detail
