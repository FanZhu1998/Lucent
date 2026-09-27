#pragma once

// Library items (blueprint section 2, "Import and organize": a contact sheet with camera/model,
// capture time, support status and flags; section 3, "User-visible compatibility states").
//
// A SourceItem is one photograph of one source file: the file's canonical path, its subimage,
// the file identity captured when it was probed, a summary of the probe and the support status
// derived from the decoded-RAW contract's interpretation assessment (raw::Interpretation, ADR
// 0005).
//
// Support status. Lucent has no qualification data yet (blueprint section 10; W10), so no item is
// ever "Verified":
//   Supported    the decoder identified the file and the probe found nothing required missing
//                (Interpretation Complete): the blueprint's "Compatible, unverified" state.
//   Limited      only optional operations are unavailable (OptionalOperationsSkipped): a
//                conservative path exists; the reasons say what is missing.
//   Unsupported  a mandatory interpretation step is unavailable (RequiresUnsupportedOperation),
//                Lucent cannot tell what the file requires (Ambiguous), the data cannot be
//                decoded (NotDecodable), or the file could not be identified at all (probe
//                failure). Automatic processing must skip such items and show the reasons.
// The status comes from the probe (metadata stage AfterOpen); a render reports the final
// interpretation after unpacking in its RenderReport.
//
// All types are plain values: safe to copy and to read concurrently.

#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/core/error.hpp>
#include <lucent/core/file.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/raw/decoded_raw.hpp>

namespace lucent::app {

// Identifier of a library item. The session library assigns 1, 2, 3, ... in insertion order and
// never reuses a value within a process; 0 is never assigned. A future catalog-backed library
// assigns persistent values; callers treat the value as opaque.
struct ItemId {
    std::uint64_t value = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(ItemId, ItemId) noexcept = default;
    friend constexpr auto operator<=>(ItemId, ItemId) noexcept = default;
};

struct ItemIdHash {
    [[nodiscard]] std::size_t operator()(ItemId id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value);
    }
};

// Persisted values (a catalog stores them); append only.
enum class SupportStatus : std::uint8_t {
    Supported = 1,
    Limited = 2,
    Unsupported = 3,
};

[[nodiscard]] std::string_view to_string(
    SupportStatus status) noexcept;  // "supported", "limited", "unsupported"

// Where a support reason comes from (append only).
enum class SupportReasonKind : std::uint8_t {
    InterpretationIssue = 1,   // one raw::InterpretationIssue of the probed image
    InterpretationStatus = 2,  // only a status is known (an image described by a probe of another)
    ProbeFailed = 3,           // the file could not be identified; see error_code
};

struct SupportReason {
    SupportReasonKind kind = SupportReasonKind::InterpretationIssue;
    std::optional<raw::InterpretationIssueKind> issue;  // InterpretationIssue
    std::optional<ErrorCode> error_code;                // ProbeFailed
    std::string message;                                // developer-facing English
};

struct SupportAssessment {
    SupportStatus status = SupportStatus::Unsupported;
    // The contract's status; std::nullopt when the probe failed.
    std::optional<raw::InterpretationStatus> interpretation;
    // Empty exactly when status is Supported.
    std::vector<SupportReason> reasons;
};

// The status an interpretation status maps to (see the file comment). Unknown values (for example
// from a newer contract) are Unsupported.
[[nodiscard]] SupportStatus support_status_for(raw::InterpretationStatus status) noexcept;

// Assessment from a probe's interpretation: one reason per issue.
[[nodiscard]] SupportAssessment assess_interpretation(const raw::Interpretation& interpretation);

// Assessment when only the status of an image is known (raw::SubimageInfo::interpretation).
[[nodiscard]] SupportAssessment assess_interpretation_status(raw::InterpretationStatus status);

// Assessment of a file whose probe failed: Unsupported, one ProbeFailed reason.
[[nodiscard]] SupportAssessment assess_probe_failure(const Error& error);

// What the contact sheet shows about an image, taken from its probe.
struct ProbeSummary {
    raw::ContainerFormat container = raw::ContainerFormat::Unknown;
    std::string extension_hint;  // lower case, without the dot
    std::string decoder;         // adapter id, e.g. "libraw"
    // The decoder's normalised maker and model when it has them, otherwise as recorded.
    std::string camera_make;
    std::string camera_model;
    std::string lens;                               // "<make> <model>" as recorded, empty when unknown
    std::optional<std::string> capture_time_local;  // camera wall-clock time "YYYY-MM-DDTHH:MM:SS"
    std::optional<float> iso;
    std::optional<float> exposure_time_s;
    std::optional<float> f_number;
    std::optional<float> focal_length_mm;
    image::Size stored_size;  // visible area in stored (sensor) orientation
    image::Orientation orientation = image::Orientation::Normal;
    image::Size upright_size;  // stored_size after orientation
    raw::RawRepresentation representation = raw::RawRepresentation::Mosaic;
    raw::SubimageRole role = raw::SubimageRole::Primary;
    std::string compression;  // raw::DataCompression::name
};

[[nodiscard]] ProbeSummary summarize_probe(const raw::RawMetadata& metadata);

// User flags (append only).
enum class PickFlag : std::uint8_t {
    None = 0,
    Picked = 1,
    Rejected = 2,
};

struct ItemFlags {
    PickFlag pick = PickFlag::None;
    std::uint8_t rating = 0;  // 0 = unrated, 1..5 stars

    // InvalidArgument ("app.library.flags") for an unknown pick value or a rating above 5.
    [[nodiscard]] Status validate() const;
    friend bool operator==(const ItemFlags&, const ItemFlags&) = default;
};

struct SourceItem {
    ItemId id;  // assigned by the library; ignored by Library::add
    // Canonical absolute path (links resolved, on-disk spelling); see import.hpp.
    std::filesystem::path path;
    std::uint32_t subimage = 0;        // image of the file this item develops
    std::uint32_t subimage_count = 1;  // images in the file (>= 1)
    // Identity of the file object when it was probed (all zero when the file could not be opened).
    // Renders and exports refuse to continue (SourceChanged) when the file no longer has it.
    core::FileIdentity identity;
    std::optional<ProbeSummary> probe;  // std::nullopt when the probe failed
    SupportAssessment support;
    ItemFlags flags;
    // Assigned by the library: 1 when added, incremented by every change of the item.
    std::uint64_t revision = 0;
};

}  // namespace lucent::app
