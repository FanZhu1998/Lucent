#pragma once

// Source maintenance (W4; blueprint section 2, "Import and organize"; section 7, "Detect source
// modification with stable-read checks before/after hashing ... reimport a changed source instead
// of using stale diagnostics. File path alone is not image identity"; ADR 0008).
//
// Three synchronous operations on a Library whose capabilities() include source_maintenance (the
// others fail with Unsupported, "app.maintenance"). Each runs on the calling thread (the
// MaintenanceService runs them on the executor's Background lane), reads source files only
// through read-only handles with full sharing, never writes to a source, and changes the library
// only through its Library interface. Work is grouped per source FILE: every item of a file shares
// its identity, hash and state.
//
// hash_sources(): content hashes and change detection.
//   For each file (Present, without a hash; with verify_hashed also those with one), in library
//   order: core::hash_file (a stable read: identity before and after, path re-checked).
//     * The identity it reports differs from the library's: the file changed since it was read
//       -> state Changed, its items are listed in changed_items (the caller re-reads them,
//       refresh_sources). The same when the file already had a hash and the new one differs
//       (content changed without the identity noticing, e.g. a memory-mapped writer).
//     * The file is gone, or its volume is absent (an empty card-reader slot or a
//       disconnected drive, core::is_volume_absent_win32_error) -> state Missing.
//     * Otherwise the hash is recorded (Library::set_content_hash), which updates the content
//       duplicates of every item with the same content. Then, with a thumbnail cache, the file's
//       embedded camera JPEG (chosen for kCachedCameraPreviewEdge, read while the file is still
//       in the system cache, reduced by camera_thumbnail_entry) is stored for each of its items,
//       so the next session shows the contact sheet without reading the file again.
//   Read errors (a file another program holds exclusively, an I/O error) leave the file unhashed
//   and are counted as failed; the next run retries it.
//
// refresh_sources(): presence checks at start-up (and on demand).
//   For each file: core::stat_file only (no content is read).
//     * Nothing at the path (NotFound), or its volume is absent (a card reader without the card,
//       a disconnected drive: core::is_volume_absent_win32_error) -> Missing (kept, with flags;
//       relink or reconnect the card).
//     * The library's identity -> Present (restores a Missing or Changed file that is back). When
//       an item of the file has an outdated probe (probe_outdated(): catalogued by a build with
//       an older decoded-RAW contract, e.g. before W9 made DNG opcodes supported) and
//       reprobe_outdated is set, the file is probed again like a changed one (below), which
//       renews its items' support status in place (ids, flags, hash and identity kept). Each
//       such file is re-read once: the new probe carries the current contract.
//     * Another identity -> Changed, then (reprobe_changed) the file is probed again through the
//       import pipeline (import_sources with that file, unrecorded), which refreshes its items in
//       place (Library::refresh: new identity, probe and support; hash cleared; caches of those
//       items invalidated by their observers). Its items are then listed in reprobed_items.
//     * Other errors (access denied, an I/O error) leave the state as it is and are counted as
//       failed.
//   unhashed_items lists the items of Present files without a hash, for hashing.
//
// relink_sources(): "Relink..." (blueprint section 2: "Missing files remain in the catalog with a
// relink action").
//   Scans the chosen folder (subfolders on request; junctions and links cannot loop, temporary
//   files are skipped) for files whose name equals a missing file's name (case-insensitively, as
//   the file system compares) and whose size equals its recorded size. A candidate that is the
//   missing file's own version (its identity equals the recorded one exactly: volume serial,
//   file id, size and modification time; a file moved within its volume, or a card that came
//   back under another drive letter) becomes its new location without being read. Otherwise each
//   candidate is hashed, and the first whose SHA-256 equals the missing file's recorded hash
//   becomes its new location (Library::relocate_source: path, identity, storage and state change;
//   ids, flags, the probe and the hash stay). A path changes ONLY on such proof: a file that was
//   never hashed and whose own version is not found cannot be confirmed and stays missing
//   (counted as unverifiable), as do files without a matching candidate (unmatched). A candidate
//   that is already another library file is not taken.
//
// Cancellation: checked before every file and inside every hash; a cancelled run stops at the
// next file with what it did so far recorded and `cancelled` set (not an error).
//
// Thread-safety: the functions are reentrant; concurrent runs on one library are safe (the
// library serialises its changes) but do redundant work.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <lucent/application/camera_support.hpp>
#include <lucent/application/library.hpp>
#include <lucent/application/thumbnail_cache.hpp>
#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/raw/decoder_registry.hpp>
#include <lucent/raw/embedded_preview.hpp>
#include <lucent/raw/raw_decoder.hpp>

namespace lucent::app {

// Long edge (pixels) the cached camera thumbnail is chosen for and reduced to: the smallest
// embedded JPEG that covers it, else the largest (raw::EmbeddedPreviewOptions::min_long_edge).
// Covers the contact sheet's 512-pixel cells up to 200 % display scale.
inline constexpr std::int32_t kCachedCameraPreviewEdge = 1024;

// JPEG quality of reduced camera thumbnails (about 100 to 250 KB at 1024 pixels).
inline constexpr std::int32_t kCachedCameraPreviewQuality = 85;

// A camera preview no larger than kCachedCameraPreviewEdge is cached as it is up to this size, and
// re-encoded at kCachedCameraPreviewQuality when it is larger.
inline constexpr std::size_t kCachedCameraPreviewMaxUnreducedBytes = std::size_t{256} << 10;

// Version of the bytes ThumbnailGenerator::EmbeddedJpeg entries hold. 1: the camera's JPEG chosen
// for kCachedCameraPreviewEdge, verbatim. 2: that JPEG reduced to kCachedCameraPreviewEdge
// (camera_thumbnail_entry). Bump when the selection or the reduction changes.
inline constexpr std::uint32_t kCameraThumbnailGeneratorVersion = 2;

// The disk-cache key of the camera thumbnail of image `subimage` of the file with content `sha256`.
[[nodiscard]] ThumbnailKey camera_thumbnail_key(const core::Digest256& sha256,
                                                std::uint32_t subimage) noexcept;

// The disk-cache entry for a camera preview: the JPEG reduced to a long edge of
// kCachedCameraPreviewEdge at kCachedCameraPreviewQuality (exporter::make_jpeg_thumbnail; many
// cameras embed only a full-size JPEG of several megabytes besides a tiny thumbnail), or unchanged
// when it is no larger in pixels and in bytes (kCachedCameraPreviewMaxUnreducedBytes). Pixels stay
// unrotated (the item's orientation applies). std::nullopt when the preview is not a JPEG libjpeg
// can decode, or on cancellation: nothing is cached then.
[[nodiscard]] std::optional<Thumbnail> camera_thumbnail_entry(const raw::EmbeddedPreview& preview,
                                                              const core::CancellationToken& cancel = {});

enum class MaintenancePhase : std::uint8_t {
    Checking = 1,   // refresh_sources: presence checks
    Reprobing = 2,  // refresh_sources: re-reading changed files
    Hashing = 3,    // hash_sources
    Scanning = 4,   // relink_sources: listing the chosen folder
    Matching = 5,   // relink_sources: hashing candidates
};

struct MaintenanceProgress {
    MaintenancePhase phase = MaintenancePhase::Checking;
    std::uint64_t files_done =
        0;  // source files (Checking, Hashing), candidates (Matching), folders (Scanning)
    std::uint64_t files_total = 0;  // 0 while unknown (Scanning)
    std::uint64_t items_done = 0;   // Checking, Hashing: items of the files done
    std::uint64_t items_total = 0;  // Checking, Hashing: items of the files to do
    std::uint64_t bytes_done = 0;   // Hashing and Matching: bytes hashed so far
    std::uint64_t bytes_total = 0;  // Hashing: bytes of the files to hash
    std::filesystem::path current;
};

// Called on the working thread (after every file and every hashed chunk); must be cheap and must
// not throw.
using MaintenanceProgressCallback = std::function<void(const MaintenanceProgress&)>;

// ---- Hashing ----------------------------------------------------------------------------------

struct HashOptions {
    std::size_t chunk_bytes = core::kDefaultHashChunkBytes;
    // Re-hash files that already have a hash and compare (a full content verification).
    bool verify_hashed = false;
    // Disk cache for the camera thumbnails of hashed files (null: none are stored).
    std::shared_ptr<ThumbnailCache> thumbnails;
    raw::EmbeddedPreviewOptions thumbnail_options;  // min_long_edge is set to kCachedCameraPreviewEdge
};

struct HashReport {
    std::uint64_t files = 0;     // files considered
    std::uint64_t hashed = 0;    // hashes recorded
    std::uint64_t verified = 0;  // verify_hashed: files whose content still matched
    std::uint64_t changed = 0;   // files found changed (state Changed)
    std::uint64_t missing = 0;   // files found missing (state Missing)
    std::uint64_t failed = 0;    // files that could not be read (left as they were)
    std::uint64_t bytes = 0;     // bytes hashed
    std::uint64_t thumbnails_cached = 0;
    std::vector<ItemId> changed_items;  // items of Changed files: re-read them (refresh_sources)
    bool cancelled = false;
};

// See the file comment. `items` selects the files (any item of a file selects it; repeated or
// unknown ids are ignored); empty selects every file of the library. Errors ("app.maintenance"):
// Unsupported (library without source maintenance); InvalidArgument (chunk_bytes); OutOfMemory.
[[nodiscard]] Expected<HashReport> hash_sources(Library& library, std::span<const ItemId> items,
                                                const HashOptions& options = {},
                                                const MaintenanceProgressCallback& progress = {},
                                                core::CancellationToken cancel = {});

// ---- Presence checks --------------------------------------------------------------------------

struct RefreshOptions {
    bool reprobe_changed = true;
    bool reprobe_outdated = true;  // probe Present files whose items' probe is outdated again
    raw::DecodeLimits probe_limits;
    // W10 (ADR 0018): the camera support database re-read items are assessed against (null: none).
    CameraSupportRef camera_support;
};

struct RefreshReport {
    std::uint64_t files = 0;     // files checked
    std::uint64_t present = 0;   // found with the library's identity
    std::uint64_t restored = 0;  // of present: were Missing or Changed
    std::uint64_t missing = 0;   // not found
    std::uint64_t changed = 0;   // found with another identity
    std::uint64_t outdated = 0;  // of present: files probed again for an outdated probe
    std::uint64_t failed = 0;    // could not be checked
    std::vector<ItemId> missing_items;
    // Items refreshed after their file changed or because their probe was outdated.
    std::vector<ItemId> reprobed_items;
    std::vector<ItemId> unhashed_items;  // items of Present files without a hash
    bool cancelled = false;
};

// See the file comment. `items` selects files as for hash_sources (empty: all). `registry` is
// used to re-read changed files. Errors: Unsupported; OutOfMemory; library failures of the
// re-read (import_sources).
[[nodiscard]] Expected<RefreshReport> refresh_sources(Library& library, const raw::DecoderRegistry& registry,
                                                      std::span<const ItemId> items,
                                                      const RefreshOptions& options = {},
                                                      const MaintenanceProgressCallback& progress = {},
                                                      core::CancellationToken cancel = {});

// ---- Relinking --------------------------------------------------------------------------------

struct RelinkRequest {
    std::filesystem::path folder;  // where to look (must exist)
    bool include_subfolders = true;
    std::uint32_t max_depth = 64;
    // Items whose files to relink (any item of a file selects it); empty: every Missing file.
    // Files that are not Missing are ignored.
    std::vector<ItemId> items;
    std::size_t chunk_bytes = core::kDefaultHashChunkBytes;
};

struct RelinkReport {
    std::uint64_t files = 0;         // missing files considered
    std::uint64_t relinked = 0;      // files moved to a verified match
    std::uint64_t unmatched = 0;     // files left missing (includes unverifiable)
    std::uint64_t unverifiable = 0;  // of unmatched: never hashed, so a match cannot be confirmed
    std::uint64_t candidates_hashed = 0;
    std::uint64_t directories_scanned = 0;
    std::uint64_t files_seen = 0;
    std::vector<ItemId> relinked_items;   // every item of the relinked files
    std::vector<ItemId> unmatched_items;  // every item of the files left missing
    bool cancelled = false;
};

// See the file comment. Errors ("app.maintenance.relink"): Unsupported; NotFound /
// InvalidArgument when the folder does not exist or is not a folder; OutOfMemory. Problems with
// single files or folders are not errors (they simply do not match).
[[nodiscard]] Expected<RelinkReport> relink_sources(Library& library, const RelinkRequest& request,
                                                    const MaintenanceProgressCallback& progress = {},
                                                    core::CancellationToken cancel = {});

}  // namespace lucent::app
