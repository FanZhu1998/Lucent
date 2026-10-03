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
//     item. Duplicates of library items (same canonical path or same file version, and same
//     subimage; Library's identity rules) and repeated paths within one import are skipped and
//     reported with the existing item. A path that is not valid Unicode (NTFS allows unpaired
//     surrogates in names) is skipped as InvalidName: the library cannot store it, and the rest
//     of the import goes on.
//   * Files found again. A library file at its own path with the library's identity that the
//     library had marked Missing or Changed is Present again (a card that came back; listed in
//     ImportReport::restored, otherwise it is a duplicate). The library's version of a file found
//     at ANOTHER path (the same volume serial, file id, size and modification time) while its own
//     path no longer has it (moved or renamed within its volume, or a card back under another
//     drive letter) moves the item there (Library::relocate_source, verified by that identity:
//     ids, flags and hash kept; listed in restored). While its own path still has it, the file is
//     another path to it (a hard link) and a duplicate.
//   * Output directories (the folders exports were written to) are not scanned into: a folder or
//     file reached while scanning an import folder is skipped (InOutputDirectory) when it is, or
//     lies inside, an output directory that does not also contain the import folder. So an output
//     folder nested in the input, or reached through a junction, is never re-imported, while
//     exporting next to the originals or into a parent folder never hides the originals: the
//     import folder itself and explicitly selected files are never skipped for this reason, and
//     exported JPEG and TIFF files are ignored by extension anyway.
//   * Never imported: anything inside Lucent's own directories (caches, temporaries), the
//     exporter's publication temporaries "~lucent-*.tmp" and the temporary siblings of atomic
//     writes and verified copies (core::is_temporary_file_name, ".lucent-<hex>.tmp").
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
//   * Copy into library folder (ImportRequest::copy_to, blueprint section 2: "lets the user select
//     a destination, verifies each copy before registering it, and leaves the source intact").
//     Each file to import is copied with core::copy_file_verified (the copy's bytes, read back
//     from the volume, must hash like a stable read of the source) into the destination, below a
//     folder named after the import folder and mirroring the subfolders under it (selected files
//     go to the destination itself; a name that is taken gets "_2", "_3", ...). Only the verified
//     copy is probed and registered: ImportMode::Copied, origin_path and origin_identity = the
//     original and its identity during the copy, sha256 = the verified hash. A selected file with
//     a non-RAW extension is probed before it is copied and copied only when it is a RAW photo.
//     Files already inside the destination are imported where they are.
//       - A referenced original already in the library (at its path with its identity, or its
//         version found at another path as above) is copied and the library's item moves to the
//         copy (Library::relocate_source, keeping its id and flags; the copy's hash must be the
//         item's, or, for an item never hashed, the original's identity during the copy must be the
//         item's). A referenced original that CHANGED (another version at the item's path, e.g. a
//         reused card) is refreshed in place first, then copied, and the item moves to the copy:
//         after a copy was asked for, no item is left referencing the original.
//       - An original copied before is skipped as a duplicate only when the earlier copy holds
//         the photo it holds now: the identity recorded with that copy equals the original's
//         identity, or (a copy found by origin path and size, Library::find_copied_from) the
//         original's SHA-256 equals the copy's. Otherwise it is a new photo (cameras restart their
//         numbering and many RAW formats have a fixed size) and is copied. An earlier copy that is
//         Missing is replaced: the original is copied again and the copy's item moves there.
//       - A file that is itself a library copy is not copied again; a library without
//         relocation (the session library) refreshes changed files in place and skips unchanged
//         ones as duplicates.
//       - A copy that fails (disk full, a source that changed while it was read, a copy that does
//         not verify) is reported as CopyFailed with the reason; nothing is registered for it and
//         no partial copy remains. A verified copy that does not become a library file (its probe
//         finds no RAW photo, the import is cancelled while it is probed, the library refuses its
//         batch) is removed again (core::remove_verified_copy), so the destination holds only
//         registered copies (and copies whose removal failed, which is logged).
//   * Storage. Every registered file records the kind of volume it is on (core::storage_kind);
//     items on removable media are counted in the report with the volumes they are on, so the
//     photographer can copy or keep the card until they are relinked (blueprint section 2).
//   * Records. When ImportRequest::record is set, the import is recorded in the library's import
//     journal (Library::begin_import / finish_import) with its roots, options and counts.
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
#include <lucent/core/file_copy.hpp>
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
    // "Copy into library folder": an existing folder the files are copied into before they are
    // registered (see the file comment); std::nullopt imports in place.
    std::optional<std::filesystem::path> copy_to;
    core::CopyOptions copy_options;  // of every copy (chunk size, name policy, free-space reserve)
    // Record the import in the library's journal (Library::begin_import). Maintenance re-reads of
    // changed files set it to false.
    bool record = true;
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
    CopyFailed = 11,          // "Copy into library folder" could not make a verified copy (see error)
    InvalidName = 12,         // a path that is not valid Unicode, which the library cannot store
};

[[nodiscard]] std::string_view to_string(SkipReason reason) noexcept;

struct SkippedPath {
    std::filesystem::path path;  // as discovered (canonical when it could be resolved)
    SkipReason reason = SkipReason::Inaccessible;
    std::optional<ItemId> existing;  // Duplicate of a library item
    // Inaccessible, NotFound, CopyFailed, InvalidName; NotRawPhoto when the decoder refused it;
    // Duplicate when the library refused to refresh or relocate the existing item.
    std::optional<Error> error;
};

enum class ImportPhase : std::uint8_t {
    Scanning = 1,  // listing folders
    Probing = 2,   // identifying files
    Done = 3,
    Copying = 4,  // copying a file into the library folder (copy_to)
};

struct ImportProgress {
    ImportPhase phase = ImportPhase::Scanning;
    std::uint64_t directories_scanned = 0;
    std::uint64_t files_seen = 0;  // regular files encountered (including ignored ones)
    std::uint64_t candidates = 0;  // files to probe
    std::uint64_t probed = 0;
    std::uint64_t added = 0;
    std::uint64_t copied = 0;        // files copied and verified (copy_to)
    std::uint64_t bytes_copied = 0;  // of finished and running copies
    std::filesystem::path current;   // folder or file being processed; empty when Done
};

struct ImportReport {
    std::vector<ItemId> added;      // in discovery order
    std::vector<ItemId> refreshed;  // existing items whose changed file was probed again
    // Existing items whose file was found again (see the file comment): Present again at their
    // path, or moved to where their file version was found. Not counted as duplicates.
    std::vector<ItemId> restored;
    std::uint64_t supported = 0;
    std::uint64_t limited = 0;
    std::uint64_t unsupported = 0;
    std::vector<SkippedPath> skipped;
    std::uint64_t ignored_non_raw = 0;     // folder files whose extension or content is not RAW
    std::uint64_t unlisted_subimages = 0;  // images beyond the probe's description limit
    std::uint64_t directories_scanned = 0;
    std::uint64_t files_seen = 0;
    // copy_to: items registered from verified copies, new ones (also in `added`) and library items
    // moved to their copy (refreshed ones are also in `refreshed`); and the bytes copied.
    std::vector<ItemId> copied;
    std::uint64_t bytes_copied = 0;
    // Added items on removable media, and the distinct volume roots they are on ("E:\").
    std::uint64_t removable_items = 0;
    std::vector<std::filesystem::path> removable_locations;
    std::uint64_t record = 0;  // the library's import record (0: none)
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
// for batch_size 0, invalid probe limits or copy options; NotFound / InvalidArgument
// ("app.import.copy") when copy_to does not exist or is not a folder; OutOfMemory; library
// failures (the import record, when one was started, is then finished as Failed).
[[nodiscard]] Expected<ImportReport> import_sources(Library& library, const raw::DecoderRegistry& registry,
                                                    const ImportRequest& request,
                                                    const ImportProgressCallback& progress = {},
                                                    core::CancellationToken cancel = {});

}  // namespace lucent::app
