#pragma once

// Value types of the Lucent catalog: sources (photograph files), items (the photographs of a
// source), user flags and import records (blueprint section 2, "Import and organize"; section 7,
// "Job lifecycle"; section 8, "Recipes, assets, and forward compatibility").
//
// Vocabulary
//   Source  one file Lucent knows about: its canonical path, its file identity (volume serial,
//           file id, size, last write time), its optional SHA-256 content hash, how it entered the
//           library (referenced in place or copied) and whether it was present at the last check.
//   Item    one photograph of a source (a subimage of a multi-image file), with the summary of its
//           probe, its support status and the user's flags.
//   Import  one run of the importer: when it ran, which roots it scanned, the options it used and
//           what it counted.
//   Revision one immutable version of an item's develop settings (schema v2), linked to the
//           revision it was derived from; the item points at its current revision.
//
// Persistence. Every enumeration below is stored in the catalog as the text returned by its
// to_string() function ("present", "missing", ...). Those spellings and the numeric values are
// persisted contracts: they never change and new values are only appended. A reader that finds
// a spelling it does not know reports ErrorCode::Corrupt (or VersionMismatch when the catalog was
// written by a newer schema) instead of guessing.
//
// Text is UTF-8 and paths are std::filesystem::path; the catalog converts paths with the strict
// helpers of lucent/core/text.hpp, so a stored path always reopens the same file.
//
// All types are plain values: safe to copy and to read concurrently.

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lucent/core/error.hpp"
#include "lucent/core/file.hpp"
#include "lucent/core/hash.hpp"
#include "lucent/core/storage.hpp"

namespace lucent::catalog {

// Catalog timestamps: UTC, millisecond precision, stored as milliseconds since 1970-01-01.
using Timestamp = std::chrono::sys_time<std::chrono::milliseconds>;

// The current UTC time truncated to milliseconds.
[[nodiscard]] Timestamp current_time() noexcept;

// Row identifiers. The catalog assigns positive values that are never reused for another row of
// the same table while the catalog exists; 0 is never assigned ("no row").
struct SourceId {
    std::int64_t value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value > 0; }
    friend constexpr bool operator==(SourceId, SourceId) noexcept = default;
    friend constexpr auto operator<=>(SourceId, SourceId) noexcept = default;
};

struct ItemId {
    std::int64_t value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value > 0; }
    friend constexpr bool operator==(ItemId, ItemId) noexcept = default;
    friend constexpr auto operator<=>(ItemId, ItemId) noexcept = default;
};

struct ImportId {
    std::int64_t value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value > 0; }
    friend constexpr bool operator==(ImportId, ImportId) noexcept = default;
    friend constexpr auto operator<=>(ImportId, ImportId) noexcept = default;
};

// A recipe revision (schema v2). Unique across all items of the catalog.
struct RevisionId {
    std::int64_t value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value > 0; }
    friend constexpr bool operator==(RevisionId, RevisionId) noexcept = default;
    friend constexpr auto operator<=>(RevisionId, RevisionId) noexcept = default;
};

// How a source entered the library (blueprint section 2: "Default import references files in
// place. An optional Copy into library folder choice ...").
enum class ImportMode : std::uint8_t {
    Referenced = 1,  // the user's file, used where it is
    Copied = 2,      // a verified copy Lucent made into a library folder; origin_path names the original
};

// What kind of volume a path is on (core::storage_kind()). Removable sources stay visibly
// identified so the photographer can copy or relink them before the card is removed. Persisted as
// "unknown", "fixed", "removable", "network".
using StorageKind = core::StorageKind;

// Result of the last check of a source's file.
enum class SourceState : std::uint8_t {
    Present = 1,  // the file at `path` had the catalogued identity
    Missing = 2,  // nothing was found at `path`; the source keeps its items and can be relinked
    Changed = 3,  // a file exists at `path` but its identity differs: probe data is stale, reimport it
};

// Support status of an item (blueprint section 3, "User-visible compatibility states"). Same
// values as lucent::app::SupportStatus, which the application maps to and from.
enum class SupportStatus : std::uint8_t {
    Supported = 1,
    Limited = 2,
    Unsupported = 3,
};

// Pick / reject flag of an item.
enum class PickFlag : std::uint8_t {
    None = 0,
    Picked = 1,
    Rejected = 2,
};

// State of an import record.
enum class ImportOutcome : std::uint8_t {
    Running = 1,  // begun and not finished
    Completed = 2,
    Cancelled = 3,
    Failed = 4,
    Interrupted = 5,  // found still Running after a restart (mark_interrupted_imports)
};

// Persisted spellings: "referenced"/"copied"; "present"/"missing"/"changed";
// "supported"/"limited"/"unsupported"; "none"/"picked"/ "rejected";
// "running"/"completed"/"cancelled"/"failed"/"interrupted". An out-of-range value returns "invalid".
[[nodiscard]] std::string_view to_string(ImportMode value) noexcept;
[[nodiscard]] std::string_view to_string(SourceState value) noexcept;
[[nodiscard]] std::string_view to_string(SupportStatus value) noexcept;
[[nodiscard]] std::string_view to_string(PickFlag value) noexcept;
[[nodiscard]] std::string_view to_string(ImportOutcome value) noexcept;

// ---------------------------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------------------------

struct SourceRecord {
    SourceId id;
    // Canonical absolute path as the importer resolved it (on-disk spelling).
    std::filesystem::path path;
    // Comparison key of `path` (make_path_key in storage.hpp): unique in the catalog.
    std::string path_key;
    // Identity of the file object when it was last catalogued (volume serial, file id, size, last
    // write time). Compare with core::stat_file() to detect a changed or replaced file.
    core::FileIdentity identity;
    // SHA-256 of the content of the catalogued version, when it has been hashed. Not unique:
    // different files with identical content are separate sources that share a hash.
    std::optional<core::Digest256> sha256;
    ImportMode import_mode = ImportMode::Referenced;
    // The original a Copied source was copied from (std::nullopt for Referenced sources).
    std::optional<std::filesystem::path> origin_path;
    // Copied sources: the identity of the original while it was copied (the version whose bytes the
    // copy holds), when the importer recorded it. A later import compares it with the original's
    // current identity to recognise a file it already copied without reading it again; a path
    // and a size alone do not identify a photo (cameras reuse file names). std::nullopt for
    // Referenced sources and when unknown.
    std::optional<core::FileIdentity> origin_identity;
    StorageKind storage_kind = StorageKind::Unknown;
    SourceState state = SourceState::Present;
    Timestamp first_seen{};  // when the source was first catalogued
    Timestamp last_seen{};   // last time the file was found present with the catalogued identity
};

// A fresh observation of a file, as the importer makes it: the input of upsert_source().
struct SourceObservation {
    std::filesystem::path path;  // canonical absolute path (the importer resolves links first)
    core::FileIdentity identity;
    // Content hash of exactly this identity (core::hash_file's result), or std::nullopt when the
    // file has not been hashed (yet).
    std::optional<core::Digest256> sha256;
    ImportMode import_mode = ImportMode::Referenced;
    std::optional<std::filesystem::path> origin_path;   // required for Copied, forbidden otherwise
    std::optional<core::FileIdentity> origin_identity;  // optional for Copied, forbidden otherwise
    StorageKind storage_kind = StorageKind::Unknown;
    Timestamp seen_at{};
};

enum class SourceUpsertOutcome : std::uint8_t {
    Inserted = 1,   // no source had this path: a new source was created
    Unchanged = 2,  // the catalogued identity matched and the source was Present
    Restored = 3,   // the identity matched and the source was Missing or Changed: now Present again
    Replaced = 4,   // the file at this path differs from the catalogued one (identity or hash):
                    // the identity was replaced and the source's items must be probed again
};

[[nodiscard]] std::string_view to_string(SourceUpsertOutcome value) noexcept;

struct SourceUpsertResult {
    SourceId id;
    SourceUpsertOutcome outcome = SourceUpsertOutcome::Inserted;
};

// Moving a source to a new location after the caller verified that the file there is the same
// photograph (blueprint section 2: "Missing files remain in the catalog with a relink action").
// The file is the same when its content hash equals the catalogued hash, or when its identity
// equals the catalogued identity exactly (volume serial, file id, size and last write time: the
// same version of the same file object, moved within its volume or reached under another drive
// letter), which needs no hash.
struct RelinkRequest {
    std::filesystem::path new_path;  // canonical absolute path of the found file
    core::FileIdentity identity;     // its identity (as hashed or stat'ed)
    // Hash of that file, computed by the caller (core::hash_file); std::nullopt when the caller
    // relies on the identity alone.
    std::optional<core::Digest256> sha256;
    StorageKind storage_kind = StorageKind::Unknown;
    Timestamp seen_at{};
    // A file that cannot be verified (the source was never hashed, or no hash was supplied, and
    // the identity differs) is refused unless the caller explicitly accepts it without content
    // verification (for example after the user confirmed it); a supplied hash is then recorded.
    // A hash that contradicts the catalogued one is always refused.
    bool allow_unverified = false;
};

// Filter of list_sources(): sources in the given states (all when empty), in id order.
struct SourceQuery {
    std::vector<SourceState> states;
    SourceId after;              // return ids greater than this (0: from the start)
    std::uint32_t limit = 1000;  // 1 ... kMaxPageSize
};

// ---------------------------------------------------------------------------------------------
// Items
// ---------------------------------------------------------------------------------------------

// Largest page of list_items() / list_sources() / list_imports().
inline constexpr std::uint32_t kMaxPageSize = 10'000;

// Probe summary of one photograph, written by upsert_items().
struct ItemMetadata {
    std::uint32_t subimage_index = 0;  // image of the file; unique per source
    std::string make;                  // camera maker as the decoder normalised it ("" when unknown)
    std::string model;
    std::string lens;
    // Camera wall-clock capture time "YYYY-MM-DDTHH:MM:SS" (no time zone; cameras record local
    // time), or std::nullopt when unknown. The fixed format makes text order chronological.
    std::optional<std::string> capture_time;
    std::uint32_t width = 0;  // visible image size in stored (sensor) orientation, pixels
    std::uint32_t height = 0;
    std::uint8_t orientation = 1;  // EXIF orientation 1..8 (lucent::image::Orientation values)
    std::string format;            // short lower-case container/extension token, e.g. "cr3", "dng"
    SupportStatus support_status = SupportStatus::Unsupported;
    // JSON array describing why the item is Limited or Unsupported ("[]" when Supported). The
    // application owns its element format; the catalog only requires valid JSON.
    std::string support_reasons_json = "[]";
    std::string decoder_id;       // adapter id, e.g. "libraw"
    std::string decoder_version;  // version of that decoder build
    Timestamp probed_at{};
    // Capture settings (schema v2). std::nullopt when unknown, and for items catalogued by a
    // schema-v1 build until they are probed again. When set: finite and > 0 (InvalidArgument
    // otherwise). ISO is the exposure index the camera recorded; exposure time in seconds.
    std::optional<double> iso;
    std::optional<double> exposure_time_s;
    std::optional<double> f_number;
    std::optional<double> focal_length_mm;
    // Number of images in the item's file (schema v2): >= 1 and > subimage_index when set;
    // std::nullopt when unknown (items catalogued before v2).
    std::optional<std::uint32_t> image_count;
};

struct ItemFlags {
    PickFlag pick = PickFlag::None;
    std::uint8_t rating = 0;  // 0 = unrated, 1..5 stars

    // InvalidArgument ("catalog.item.flags") for an unknown pick value or a rating above 5.
    [[nodiscard]] Status validate() const;
    friend bool operator==(const ItemFlags&, const ItemFlags&) = default;
};

// An item as listed for the contact sheet: its own columns plus a summary of its source.
struct ItemRecord {
    ItemId id;
    SourceId source_id;
    ItemMetadata metadata;
    ItemFlags flags;
    std::filesystem::path source_path;
    SourceState source_state = SourceState::Present;
    StorageKind storage_kind = StorageKind::Unknown;
    bool source_hashed = false;  // the source has a content hash
    // The recipe revision the item shows now; std::nullopt: "as shot" (no edit, or undone to it).
    std::optional<RevisionId> current_revision;
};

// Contact-sheet filter. Empty vectors and unset optionals do not filter.
struct ItemFilter {
    std::vector<SourceState> source_states;
    std::vector<SupportStatus> support;
    // Only items whose source shares its content hash with at least one other source (content
    // duplicates under different paths; unhashed sources never match).
    bool duplicates_only = false;
    std::optional<SourceId> source;
    std::optional<PickFlag> pick;
    std::optional<std::uint8_t> min_rating;  // rating >= value (1..5)
};

// Sort orders of list_items(). Every order is total, so paging is stable.
enum class ItemOrder : std::uint8_t {
    Path = 1,         // source path_key, then subimage index
    CaptureTime = 2,  // capture time (unknown last), then item id
    Added = 3,        // item id: the order items were first catalogued
};

namespace detail {
struct ItemCursorAccess;
}  // namespace detail

// Position after the last item of a page. Opaque: obtain it from ItemPage::next and pass it back
// unchanged with the same order and filter. Keyset paging: the next page starts strictly after the
// last returned item's sort key, so rows inserted or removed between pages never cause an item to
// be repeated or skipped. Rows inserted before the cursor position are not returned, and an item
// whose sort key changes between pages (a re-probed capture time, a relinked path) may appear
// twice or not at all.
class ItemCursor {
public:
    ItemCursor() = default;
    friend bool operator==(const ItemCursor&, const ItemCursor&) = default;

private:
    friend struct detail::ItemCursorAccess;
    ItemOrder order_ = ItemOrder::Path;
    std::string text_;         // path_key (Path) or capture-time sort key (CaptureTime)
    std::int64_t number_ = 0;  // subimage index (Path) or item id (CaptureTime, Added)
};

struct ItemQuery {
    ItemFilter filter;
    ItemOrder order = ItemOrder::Path;
    std::uint32_t limit = 500;        // 1 ... kMaxPageSize
    std::optional<ItemCursor> after;  // continue after this position (must use the same order)
};

struct ItemPage {
    std::vector<ItemRecord> items;
    // Set exactly when more items follow this page; pass it as ItemQuery::after.
    std::optional<ItemCursor> next;
};

// ---------------------------------------------------------------------------------------------
// Imports
// ---------------------------------------------------------------------------------------------

struct ImportRoot {
    std::filesystem::path path;  // a selected file or folder
    bool include_subfolders = false;
    friend bool operator==(const ImportRoot&, const ImportRoot&) = default;
};

struct ImportCounts {
    std::uint64_t files_seen = 0;  // regular files encountered
    std::uint64_t added = 0;       // new items
    std::uint64_t refreshed = 0;   // existing items probed again because their file changed
    std::uint64_t unchanged = 0;   // files already catalogued with the same identity
    std::uint64_t duplicates = 0;  // skipped as duplicates of catalogued sources
    std::uint64_t skipped = 0;     // skipped for any other reason
    std::uint64_t failed = 0;      // files whose import failed
    friend bool operator==(const ImportCounts&, const ImportCounts&) = default;
};

struct ImportStart {
    Timestamp started_at{};
    std::vector<ImportRoot> roots;
    std::string options_json = "{}";  // the importer's options, any valid JSON value
};

struct ImportFinish {
    Timestamp finished_at{};
    ImportOutcome outcome = ImportOutcome::Completed;  // anything but Running
    ImportCounts counts;
};

struct ImportRecord {
    ImportId id;
    Timestamp started_at{};
    std::optional<Timestamp> finished_at;
    ImportOutcome outcome = ImportOutcome::Running;
    std::vector<ImportRoot> roots;
    std::string options_json;
    ImportCounts counts;
};

// ---------------------------------------------------------------------------------------------
// Recipe revisions (schema v2; blueprint section 2, "Detailed individual editing", and section 8)
// ---------------------------------------------------------------------------------------------
//
// Every edit of an item's develop settings is stored as a new, immutable revision; nothing ever
// changes or deletes a revision (they go only with their item). Each item points at the revision
// it shows now (its current revision; none means "as shot"):
//   * an edit appends a revision whose parent is the current one and makes it current (one atomic
//     call, WriteTransaction::append_revision);
//   * undo / redo and "go back to an earlier recipe" only move the current pointer
//     (set_current_revision, undo_revision, redo_revision);
//   * an edit after an undo starts a new branch from the current revision; the undone branch
//     stays in the history and can be made current again.
// "Reset to as shot" is either a move to no revision (set_current_revision(item, std::nullopt):
// undoable by redo) or, when the reset itself should be a recorded step, a Reset revision whose
// settings are the as-shot defaults.
//
// The settings document is opaque to the catalog: it stores the JSON text, its schema name and
// version (for example "lucent.develop" v1) and the engine version, and verifies the text's
// SHA-256 when it is read back. Interpreting it, and refusing documents of a newer unknown schema
// version, is the reader's job (blueprint section 8: "never treat newer unknown versions as
// compatible").

// Why a revision was created. Persisted as "user", "auto", "reset", "paste", "import"; later
// versions may append values without a new schema (the column has no CHECK). Origins only label the
// history, so a value this build does not know is read as Unknown rather than as an error.
enum class RevisionOrigin : std::uint8_t {
    Unknown = 0,  // read only: an origin written by a newer Lucent; never written
    User = 1,     // the photographer changed controls
    Auto = 2,    // the automatic optimiser produced it ("Auto creates a new editable revision")
    Reset = 3,   // a recorded reset to as-shot settings
    Paste = 4,   // settings pasted from another item
    Import = 5,  // settings that arrived with the photograph (a sidecar or another catalog)
};

// "user", "auto", "reset", "paste", "import", "unknown"; "invalid" for out-of-range values.
[[nodiscard]] std::string_view to_string(RevisionOrigin value) noexcept;

// Largest settings document of a revision (UTF-8 bytes). A recipe with many brush strokes stays
// far below it; LimitExceeded above it.
inline constexpr std::size_t kMaxRevisionSettingsBytes = std::size_t{16} << 20;  // 16 MiB
// Largest note of a revision (UTF-8 bytes).
inline constexpr std::size_t kMaxRevisionNoteBytes = std::size_t{64} << 10;  // 64 KiB
// Largest schema name and engine version (UTF-8 bytes).
inline constexpr std::size_t kMaxRevisionLabelBytes = 256;

// The input of WriteTransaction::append_revision(). The parent and the revision number are
// assigned by the catalog.
struct NewRevision {
    RevisionOrigin origin = RevisionOrigin::User;
    std::string schema_name;           // settings schema, e.g. "lucent.develop"; 1 ... 256 bytes
    std::uint32_t schema_version = 0;  // settings schema version, >= 1
    std::string settings_json;         // the settings document: valid JSON, at most 16 MiB
    std::string engine_version;        // engine that produced / validated it (may be empty)
    std::string note;                  // free text for the history list (may be empty)
    Timestamp created_at{};

    // InvalidArgument ("catalog.revision") for an unknown origin, an empty or overlong schema
    // name, schema version 0, text that is not valid UTF-8 or an overlong engine version;
    // LimitExceeded for settings or a note above their limits. JSON validity is checked by the
    // catalog when the revision is stored.
    [[nodiscard]] Status validate() const;
};

// A revision as listed in an item's history (without its settings document).
struct RevisionInfo {
    RevisionId id;
    ItemId item;
    std::uint32_t revision_number = 0;  // 1, 2, 3, ... per item in creation order
    std::optional<RevisionId> parent;   // the revision it was derived from; none: from as shot
    Timestamp created_at{};
    RevisionOrigin origin = RevisionOrigin::User;
    std::string schema_name;
    std::uint32_t schema_version = 0;
    core::Digest256 settings_sha256{};  // SHA-256 of settings_json's UTF-8 bytes
    std::uint64_t settings_bytes = 0;   // size of settings_json
    std::string engine_version;
    std::string note;
};

// A revision with its settings document.
struct RecipeRevision {
    RevisionInfo info;
    std::string settings_json;
};

// The whole history of one item.
struct RevisionHistory {
    ItemId item;
    std::optional<RevisionId> current;    // none: the item shows "as shot"
    std::vector<RevisionInfo> revisions;  // every revision, by revision_number (oldest first)
};

}  // namespace lucent::catalog
