#pragma once

// Import: discovering photographs in user-selected files and folders and listing them in the
// library (blueprint section 2, "Import and organize").
//
// Rules:
//   * In folders ("Add folder", optionally with subfolders), the extension is a hint
//     (classify_extension): known RAW extensions are probed; known non-RAW extensions (JPEG,
//     TIFF, video, sidecars, ...) are counted as ignored; any other extension is probed when the
//     file's first bytes carry a RAW container signature (raw::detect_container_format).
//   * Files the user selects explicitly ("Add photos", drag and drop) are probed whatever their
//     extension, except that a file with a known non-RAW extension is skipped as NotRawPhoto
//     (not listed) when its first bytes carry no RAW container signature or the decoder does not
//     recognise it: a JPEG or XMP companion dropped with its RAW is not a photo Lucent develops,
//     while a DNG saved as ".tif" still is.
//   * Otherwise the probe decides: a file the decoder rejects is still listed, as Unsupported
//     with the probe's error as its reason.
//   * Paths are canonicalised (every junction, symbolic link and mount point resolved; the
//     on-disk spelling) before anything else, so one file reached through different paths is one
//     item. Duplicates of library items (same canonical path or same file object, and same
//     subimage) and repeated paths within one import are skipped and reported with the existing
//     item.
//   * Output directories (the folders exports were written to) are not scanned into: a folder or
//     file reached while scanning an import folder is skipped (InOutputDirectory) when it is, or
//     lies inside, an output directory that does not also contain the import folder. So an output
//     folder nested in the input, or reached through a junction, is never re-imported, while
//     exporting next to the originals or into a parent folder never hides the originals: the
//     import folder itself and explicitly selected files are never skipped for this reason, and
//     exported JPEG and TIFF files are ignored by extension anyway.
//   * Never imported: anything inside Lucent's own directories (caches, temporaries) and the
//     exporter's publication temporaries "~lucent-*.tmp".
//   * Changed files: a file that is already a library item, at the same canonical path, but whose
//     identity (file object, size, modification time) changed since it was imported is probed
//     again and its items are refreshed in place (Library::refresh, ImportReport::refreshed)
//     instead of being skipped as duplicates. New images of a multi-image file are added; items
//     of images the file no longer has keep their old identity (their renders report
//     SourceChanged).
//   * Loops: every folder is identified by its file object (volume serial and file id) before it
//     is entered, and a folder already scanned is not entered again, so junctions and symbolic
//     links can neither create scan loops nor scan a tree twice. max_depth bounds the recursion.
//   * Multi-image files: one item per photograph (raw::SubimageRole Primary, BurstFrame,
//     PixelShiftFrame, Unknown), each with its own support status; Dual Pixel component frames
//     are not listed (they belong to their Primary image). At most
//     raw::ProbeOptions::kMaxDescribedSubimages images per file are listed; the rest are counted.
//   * Nothing is ever written to a source: files are opened read-only with full sharing.
//   * Items are committed to the library in batches as they are probed (the library's observers
//     see them appear). Cancellation stops at the next file or folder; items already committed
//     stay in the library and the report says the import was cancelled.
//   * Enumeration order is deterministic: folders are listed sorted by case-folded name.
//
// Thread-safety: import_sources() is reentrant; concurrent imports into one library are safe
// (the library rejects duplicates atomically).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <lucent/application/library.hpp>
#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/raw/decoder_registry.hpp>
#include <lucent/raw/raw_decoder.hpp>

namespace lucent::app {

struct ImportFolder {
    std::filesystem::path path;
    bool include_subfolders = false;
};

struct ImportRequest {
    std::vector<std::filesystem::path> files;  // "Add photos"
    std::vector<ImportFolder> folders;         // "Add folder"
    // Folders exports were written to: not scanned into when a scan reaches them below an import
    // folder they do not contain (see the file comment). Selected files and folders are never
    // skipped for being in one.
    std::vector<std::filesystem::path> output_directories;
    // Lucent's own directories (preview and analysis caches, temporary files).
    std::vector<std::filesystem::path> lucent_directories;
    raw::DecodeLimits probe_limits;  // file-size bound of each probe
    std::uint32_t max_depth = 64;    // folder levels below each import folder
    std::uint32_t batch_size = 32;   // items per library commit (>= 1)
};

// Why a path was not imported (append only).
enum class SkipReason : std::uint8_t {
    Duplicate = 1,            // already in the library, or already seen in this import
    InOutputDirectory = 2,    // inside an output directory
    InLucentDirectory = 3,    // inside one of Lucent's own directories
    LucentTemporaryFile = 4,  // "~lucent-*.tmp", a publication temporary
    DirectoryCycle = 5,       // a folder already scanned, reached again (link or another import folder)
    DepthLimit = 6,           // deeper than max_depth
    Inaccessible = 7,         // could not be opened, resolved or listed (see error)
    NotFound = 8,             // a selected file or folder does not exist
    NotAFileOrFolder = 9,     // a device or other special object
    NotRawPhoto = 10,         // a selected file with a non-RAW extension that is not a RAW file
};

[[nodiscard]] std::string_view to_string(SkipReason reason) noexcept;

struct SkippedPath {
    std::filesystem::path path;  // as discovered (canonical when it could be resolved)
    SkipReason reason = SkipReason::Inaccessible;
    std::optional<ItemId> existing;  // Duplicate of a library item
    std::optional<Error> error;      // Inaccessible, NotFound; NotRawPhoto when the decoder refused it
};

enum class ImportPhase : std::uint8_t {
    Scanning = 1,  // listing folders
    Probing = 2,   // identifying files
    Done = 3,
};

struct ImportProgress {
    ImportPhase phase = ImportPhase::Scanning;
    std::uint64_t directories_scanned = 0;
    std::uint64_t files_seen = 0;  // regular files encountered (including ignored ones)
    std::uint64_t candidates = 0;  // files to probe
    std::uint64_t probed = 0;
    std::uint64_t added = 0;
    std::filesystem::path current;  // folder or file being processed; empty when Done
};

struct ImportReport {
    std::vector<ItemId> added;      // in discovery order
    std::vector<ItemId> refreshed;  // existing items whose changed file was probed again
    std::uint64_t supported = 0;
    std::uint64_t limited = 0;
    std::uint64_t unsupported = 0;
    std::vector<SkippedPath> skipped;
    std::uint64_t ignored_non_raw = 0;     // folder files whose extension or content is not RAW
    std::uint64_t unlisted_subimages = 0;  // images beyond the probe's description limit
    std::uint64_t directories_scanned = 0;
    std::uint64_t files_seen = 0;
    bool cancelled = false;
    double seconds = 0.0;
};

// What a file name's extension suggests (append only).
enum class ExtensionHint : std::uint8_t {
    Raw = 1,      // a RAW extension (cr2, cr3, nef, arw, dng, raf, orf, rw2, pef, 3fr, iiq, x3f, ...)
    NotRaw = 2,   // an extension of other content (jpg, tif, png, heic, xmp, mov, mp4, insv, ...)
    Unknown = 3,  // anything else, including no extension
};

// Case-insensitive classification of `path`'s extension; a hint only (see the file comment).
[[nodiscard]] ExtensionHint classify_extension(const std::filesystem::path& path);

// The extensions classify_extension() reports as Raw: lower case, without the dot, sorted,
// without duplicates. For file-picker filters; the probe still decides (see the file comment).
[[nodiscard]] std::span<const std::wstring_view> raw_extensions() noexcept;

// Called on the importing thread; must not throw.
using ImportProgressCallback = std::function<void(const ImportProgress&)>;

// Runs an import on the calling thread (see the file comment). Problems with individual paths
// are reported in ImportReport::skipped, not as errors. Errors ("app.import"): InvalidArgument
// for batch_size 0 or invalid probe limits; OutOfMemory; library failures.
[[nodiscard]] Expected<ImportReport> import_sources(Library& library, const raw::DecoderRegistry& registry,
                                                    const ImportRequest& request,
                                                    const ImportProgressCallback& progress = {},
                                                    core::CancellationToken cancel = {});

}  // namespace lucent::app
