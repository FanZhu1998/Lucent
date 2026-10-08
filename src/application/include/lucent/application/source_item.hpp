#pragma once

// Library items (blueprint section 2, "Import and organize": a contact sheet with camera/model,
// capture time, support status and flags; section 3, "User-visible compatibility states").
//
// A SourceItem is one photograph of one source file: the file's canonical path, its subimage,
// the file identity captured when it was probed, a summary of the probe and its support status.
//
// Support status: the blueprint's four states (W10 phase 3, ADR 0018), assessed when the file is
// probed (metadata stage AfterOpen) by combining the decoded-RAW contract's interpretation
// (raw::Interpretation, ADR 0005) with the camera support database's entry for the probe's mode
// key (lucent/assets/camera_support.hpp; the rules are in camera_support.hpp of this module):
//   Verified              the exact model and capture mode passed Lucent's qualification, the
//                         database's basis is this build's, and nothing required is missing.
//   CompatibleUnverified  the decoder identified the file and nothing required is missing, but
//                         the mode has no quality evidence (not in the database, listed as
//                         Compatible, unverified, or qualified with another decoder or pipeline).
//   Limited               a known optional feature is unavailable: optional operations the file
//                         asks for (OptionalOperationsSkipped), or a limitation the database lists
//                         (X-Trans baseline demosaicing, an image area Lucent cannot read, ...); a
//                         conservative path exists and the reasons name what is missing.
//   Unsupported           a mandatory interpretation step is unavailable (RequiresUnsupportedOperation),
//                         Lucent cannot tell what the file requires (Ambiguous), the data cannot be
//                         decoded (NotDecodable), the file could not be identified at all (probe
//                         failure), or the database lists the mode as Unsupported. Automatic
//                         processing skips such items and shows the reasons.
// A render reports the final interpretation after unpacking in its RenderReport.
//
// Source facts (W4, blueprint sections 2 and 7: "File path alone is not image identity"). Besides
// the probe, an item carries what the library knows about its source file: the content hash
// (SHA-256 of the version `identity` describes, once it has been hashed in the background), the
// result of the last presence check (SourceState), the kind of volume it is on (a removable card
// stays visibly identified), whether it is the user's file referenced in place or a verified copy
// Lucent made into a library folder, and which other items have the same content. The session
// library stores these facts as given; the catalog library (catalog_library.hpp) persists them
// and maintains content_duplicates.
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

#include <lucent/assets/camera_support.hpp>
#include <lucent/core/error.hpp>
#include <lucent/core/file.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/core/storage.hpp>
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

// The blueprint's four states (see the file comment): the camera support database's states, in
// order of severity (a larger value is worse; assets::worse() combines two). Their spellings
// ("verified", "compatible_unverified", "limited", "unsupported"; assets::to_string()) appear in
// the application's records; the catalog's status column keeps the three spellings every catalog
// reader knows (catalog_mapping.hpp).
using SupportStatus = assets::SupportState;

// Version of the support rules (camera_support.hpp) an assessment was made with.
inline constexpr std::uint32_t kSupportRulesVersion = 1;

// Where a support reason comes from (append only).
enum class SupportReasonKind : std::uint8_t {
    InterpretationIssue = 1,   // one raw::InterpretationIssue of the probed image
    InterpretationStatus = 2,  // only a status is known (an image described by a probe of another)
    ProbeFailed = 3,           // the file could not be identified; see error_code
    CameraLimitation = 4,      // a limitation the camera support database lists for the mode
    CameraUnsupported = 5,     // the camera support database lists the mode as Unsupported
};

struct SupportReason {
    SupportReasonKind kind = SupportReasonKind::InterpretationIssue;
    std::optional<raw::InterpretationIssueKind> issue;  // InterpretationIssue
    std::optional<ErrorCode> error_code;                // ProbeFailed
    std::string limitation;                             // CameraLimitation: its stable code
    // Developer-facing English for the interpretation kinds. For the Camera kinds, the database's
    // user-facing English ("<summary>: <meaning>"), which reports and review notes show as is.
    std::string message;
};

// How the camera support database was consulted for an item (ADR 0018). Kept with the assessment
// (and in the catalog) so that another database version can assess the item again without reading
// its file (reassess_support()).
struct CameraSupportEvaluation {
    std::uint32_t rules = kSupportRulesVersion;
    // The mode key of the probe the item was assessed with (assets::derive_mode_key()).
    std::string mode_key;
    // The interpretation status of that probe: the live part of the assessment.
    raw::InterpretationStatus interpretation = raw::InterpretationStatus::Complete;
    // The database consulted: its asset id and version and its qualification date; all empty when
    // no database was active.
    std::string database_id;
    std::string database_version;
    std::string qualified_on;
    // The database's Verified entries held for the build that assessed (assets::CameraSupport::
    // verified_entries_hold()); false downgrades a Verified entry to Compatible, unverified.
    bool basis_matches = true;
    // The database's answer for mode_key; std::nullopt when the mode is not in the database.
    std::optional<SupportStatus> listed;
    // The matching entry's decoded key, labels and evidence (empty when not listed).
    std::string qualified_key;
    std::string camera;
    std::string mode;
    std::string evidence;

    friend bool operator==(const CameraSupportEvaluation&, const CameraSupportEvaluation&) = default;
};

struct SupportAssessment {
    SupportStatus status = SupportStatus::Unsupported;
    // The contract's status; std::nullopt when the probe failed.
    std::optional<raw::InterpretationStatus> interpretation;
    // Why the item is Limited or Unsupported (empty for Verified and Compatible, unverified):
    // interpretation reasons first, then the database's.
    std::vector<SupportReason> reasons;
    // How the camera support database was consulted; std::nullopt when the probe failed, and for
    // assessments made before the database existed (such items are probed again,
    // probe_outdated()).
    std::optional<CameraSupportEvaluation> camera;
};

// The status an interpretation status alone maps to: Complete is Compatible, unverified (nothing
// required is missing, but the interpretation alone proves no quality), OptionalOperationsSkipped
// Limited, the rest Unsupported. Unknown values (for example from a newer contract) are
// Unsupported.
[[nodiscard]] SupportStatus support_status_for(raw::InterpretationStatus status) noexcept;

// Assessment from a probe's interpretation alone: one reason per issue, no database consulted
// (camera stays empty). assess_support() (camera_support.hpp) is the import's assessment.
[[nodiscard]] SupportAssessment assess_interpretation(const raw::Interpretation& interpretation);

// Assessment when only the status of an image is known (raw::SubimageInfo::interpretation).
[[nodiscard]] SupportAssessment assess_interpretation_status(raw::InterpretationStatus status);

// Assessment of a file whose probe failed: Unsupported, one ProbeFailed reason.
[[nodiscard]] SupportAssessment assess_probe_failure(const Error& error);

// What the contact sheet shows about an image, taken from its probe.
struct ProbeSummary {
    raw::ContainerFormat container = raw::ContainerFormat::Unknown;
    std::string extension_hint;   // lower case, without the dot
    std::string decoder;          // adapter id, e.g. "libraw"
    std::string decoder_version;  // version of the decoder library, e.g. "0.22.2-Release"
    // The decoded-RAW contract (raw::kDecodedRawContractVersion) of the build that made the probe,
    // whose interpretation rules gave the support status; std::nullopt when unknown (items a
    // build before W9 catalogued). See probe_outdated().
    std::optional<core::ContractVersion> contract;
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

// The summary of a probe made by this build (contract = raw::kDecodedRawContractVersion).
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

// Result of the last check of an item's source file. Persisted values (the catalog stores them as
// "present", "missing", "changed"); append only.
enum class SourceState : std::uint8_t {
    Present = 1,  // the file at `path` had the item's identity when it was last checked or imported
    Missing = 2,  // nothing was found at `path`: the item stays listed and can be relinked
    Changed = 3,  // another version of the file is at `path`: the probe is stale until it is re-read
};

[[nodiscard]] std::string_view to_string(SourceState state) noexcept;  // "present", "missing", "changed"

// How the source entered the library (blueprint section 2). Persisted values ("referenced",
// "copied"); append only.
enum class ImportMode : std::uint8_t {
    Referenced = 1,  // the user's file, used where it is
    Copied = 2,      // a verified copy Lucent made into a library folder; origin_path names the original
};

[[nodiscard]] std::string_view to_string(ImportMode mode) noexcept;  // "referenced", "copied"

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

    // ---- The source file (see the file comment) ----------------------------------------------
    // Set by the library: Present when added or refreshed, then as background checks find it.
    SourceState source_state = SourceState::Present;
    // Kind of volume `path` was on when the file was last observed (core::storage_kind()).
    core::StorageKind storage = core::StorageKind::Unknown;
    ImportMode import_mode = ImportMode::Referenced;
    // Copied items: the original the copy was made from (never written by Lucent); std::nullopt
    // for Referenced items.
    std::optional<std::filesystem::path> origin_path;
    // Copied items: the identity of the original while it was copied (core::CopyReceipt::
    // source_identity), when known. An import of the same original version recognises it as
    // already copied without reading it (Library::find_copied_from).
    std::optional<core::FileIdentity> origin_identity;
    // SHA-256 of the content of the file version `identity` describes, once hashed (a stable read,
    // core::hash_file). Shared by every item of the file.
    std::optional<core::Digest256> sha256;
    // Set by the library (ignored by add()): the other items with the same content hash and the
    // same subimage, i.e. the same photograph under another file, in id order. Duplicates are
    // only flagged; they are never merged or removed automatically (blueprint section 2).
    std::vector<ItemId> content_duplicates;
};

// True when the item's support status came from an older interpretation than this build's: it
// has a probe whose decoded-RAW contract is unknown or older than raw::kDecodedRawContractVersion
// (a contract change can change what a file requires and what Lucent supports; W9 made DNG
// opcodes supported), or its assessment has no camera support evaluation (made before ADR 0018:
// without the probe's mode key it cannot be assessed against the database) or one of older support
// rules. Source maintenance probes such items again (refresh_sources). An item whose probe failed
// carries no interpretation and is not outdated.
[[nodiscard]] bool probe_outdated(const SourceItem& item) noexcept;

}  // namespace lucent::app
