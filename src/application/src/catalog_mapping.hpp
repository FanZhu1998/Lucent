#pragma once

// Private: conversions between the application's library values (source_item.hpp, library.hpp)
// and the catalog's records (lucent/catalog/types.hpp), used by CatalogLibrary.
//
// What the catalog stores of an item (schema v1, items table): the probe summary's camera, lens,
// capture time, stored size, orientation, format token and decoder; the support status; the
// support reasons as JSON in the application-owned format below; the user's flags. Not stored by
// schema v1, so not restored after a restart: ISO, exposure time, f-number, focal length, the
// container / representation / role / compression of the probe, and the file's image count
// (subimage_count is restored as the larger of the number of catalogued images of the file and
// the highest catalogued image index + 1).
//
// Support status column (items.support_status). The catalog knows three spellings ("supported",
// "limited", "unsupported"), and every catalog reader since schema v1 reads them; the four states
// of ADR 0018 are stored without a schema change, so a catalog this build writes stays readable by
// older builds (the reader-version contract: no new spelling, min_reader_version unchanged):
//   Verified, Compatible, unverified -> "supported"   (the "camera_support" element below says which)
//   Limited                         -> "limited"
//   Unsupported                     -> "unsupported"
// An older build shows a Verified item as it showed every supported item (Compatible, unverified):
// conservative, never better than the truth.
//
// Support reasons JSON (items.support_reasons_json). An array of objects, one per
// app::SupportReason in order, then (ADR 0018) one "camera_support" element when the assessment
// consulted the camera support database:
//   {"kind": "issue",        "issue":  "<raw::to_string(InterpretationIssueKind)>", "message": "..."}
//   {"kind": "status",       "status": "<raw::to_string(InterpretationStatus)>",    "message": "..."}
//   {"kind": "probe_failed", "error":  "<lucent::to_string(ErrorCode)>",             "message": "..."}
//   {"kind": "camera_limitation", "limitation": "<code>",                          "message": "..."}
//   {"kind": "camera_unsupported",                                                 "message": "..."}
//   {"kind": "engine_limitation", "limitation": "<code>",                          "message": "..."}
//   {"kind": "camera_support", "rules": 2, "state": "<assets::to_string(SupportState)>",
//    "interpretation": "<raw::to_string(InterpretationStatus)>", "mode_key": "mk1|...",
//    "image_area": "decoder_inset_crop 7008x4672", "dng_opcodes": ["list3:WarpRectilinear"],
//    "database": {"id": "camera-support", "version": "2.0.0", "sha256": "<manifest SHA-256 hex>",
//                 "qualified": "2026-10-08"} | null,
//    "basis_matches": true, "variant_matches": true, "listed": "<state>" | null,
//    "qualified_key": "...", "camera": "...", "mode": "...", "evidence": "...", "message": "..."}
// (rules 2 added "engine_limitation", "image_area", "dng_opcodes", "sha256" and "variant_matches";
// "listed" with a null "database" is the answer kept from the last database while none is active.)
// "message" is developer-facing English for the interpretation kinds and user-facing English for the
// limitation and camera kinds. Readers keep unknown kinds and spellings (from a newer Lucent) as
// reasons without a known issue or error, so the generic wording applies; the support status
// column stays authoritative: a "camera_support" element whose state does not fit the column
// (verified or compatible_unverified with "supported", limited with "limited", unsupported with
// "unsupported") is dropped with a log line, which leaves the item without an evaluation, so source
// maintenance probes it again (probe_outdated()). Older readers see the camera elements as
// unknown kinds: a "supported" item still shows as supported, a Limited or Unsupported one with
// its generic wording.
//
// The interpretation status of a reloaded assessment is the camera element's when there is one;
// otherwise it is derived: Complete for "supported", OptionalOperationsSkipped for Limited (or the
// issues' status when worse), and for Unsupported the recorded status reason, else the status the
// issues imply (at least RequiresUnsupportedOperation), or none after a probe failure.
//
// Thread-safety: pure functions.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/application/library.hpp>
#include <lucent/application/source_item.hpp>
#include <lucent/catalog/types.hpp>

namespace lucent::app::detail {

[[nodiscard]] catalog::SupportStatus to_catalog(SupportStatus status) noexcept;
[[nodiscard]] SupportStatus from_catalog(catalog::SupportStatus status) noexcept;
[[nodiscard]] catalog::SourceState to_catalog(SourceState state) noexcept;
[[nodiscard]] SourceState from_catalog(catalog::SourceState state) noexcept;
[[nodiscard]] catalog::ImportMode to_catalog(ImportMode mode) noexcept;
[[nodiscard]] ImportMode from_catalog(catalog::ImportMode mode) noexcept;
[[nodiscard]] catalog::ItemFlags to_catalog(const ItemFlags& flags) noexcept;
[[nodiscard]] ItemFlags from_catalog(const catalog::ItemFlags& flags) noexcept;
[[nodiscard]] catalog::ImportOutcome to_catalog(ImportJournalOutcome outcome) noexcept;

// The support reasons (and the camera support evaluation) in the JSON format of the file comment.
// Never fails (text is sanitised to UTF-8).
[[nodiscard]] std::string encode_support_reasons(const SupportAssessment& support);

// The assessment of a stored item. Malformed JSON yields one reason without detail (and a log
// line); it never fails.
[[nodiscard]] SupportAssessment decode_support(catalog::SupportStatus status, std::string_view reasons_json);

// True when the stored reasons say the item's probe failed (it has no probe summary).
[[nodiscard]] bool has_probe_failure(const SupportAssessment& support) noexcept;

// `text` when it has the catalog's capture-time form "YYYY-MM-DDTHH:MM:SS" with plausible fields,
// otherwise std::nullopt (decoders occasionally report other forms; they are not stored).
[[nodiscard]] std::optional<std::string> catalog_capture_time(const std::optional<std::string>& text);

// What the catalog stores of `item` (see the file comment). `probed_at` is recorded as is.
[[nodiscard]] catalog::ItemMetadata to_metadata(const SourceItem& item, catalog::Timestamp probed_at);

// The probe summary restored from stored metadata (std::nullopt when the probe had failed).
[[nodiscard]] std::optional<ProbeSummary> probe_from_metadata(const catalog::ItemMetadata& metadata,
                                                              const SupportAssessment& support);

// The JSON stored with an import record: {"include_subfolders": [...], "copy_to": "..."|null}.
[[nodiscard]] std::string encode_import_options(const ImportJournalStart& start);

}  // namespace lucent::app::detail
