#pragma once

// Value types shared by the UI-side service interfaces (library_service.hpp, image_source.hpp,
// export_service.hpp).
//
// These interfaces are the seam between the Qt Quick shell (Lucent.exe) and the application
// services (src/application). The shell is written against them; service_factory.cpp binds them
// to the adapters in adapters/ (docs/adr/0007-application-shell.md). They use Qt value types
// because they belong to the UI layer; nothing below the UI depends on them
// (docs/architecture/overview.md).
//
// Strings are user-facing unless stated otherwise. Paths are absolute; the shell accepts either
// separator and shows native ones.

#include <array>
#include <cstdint>
#include <memory>

#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace lucent::ui {

// Identity of a library item: unique and stable for the lifetime of the service instance; with the
// catalog-backed library (W4) the catalog's item id, stable across restarts. Zero is never valid.
using ItemId = std::uint64_t;

// Image generation of an item's thumbnail or preview. It increases whenever the pixels that image
// shows change (the embedded camera JPEG replaced by Lucent's render, a new recipe revision, ...).
// kNoImage means no image is available yet. The shell encodes item id and generation in every
// image request so a late result for an older generation is never shown (blueprint section 4).
//
// Thumbnails: generation 1 is the camera's embedded JPEG (thumbnail_is_camera_preview set);
// generations from 2 on are Lucent's own render. Previews: the generation is the recipe
// revision shown (1 until W6 adds editing).
using Generation = std::uint64_t;
inline constexpr Generation kNoImage = 0;
inline constexpr Generation kCameraPreviewGeneration = 1;

// Blueprint section 3, "User-visible compatibility states". Numeric values are stable.
enum class SupportStatus : std::uint8_t {
    Unknown = 0,               // not yet determined
    Verified = 1,              // model and capture mode passed Lucent's qualification tests
    CompatibleUnverified = 2,  // decodes and interprets, without representative quality fixtures
    Limited = 3,               // an optional feature or profile is unavailable
    Unsupported = 4,           // decoding or a mandatory interpretation step is unavailable
};

// Where an item is in Lucent's own processing. Numeric values are stable.
enum class ProcessingStatus : std::uint8_t {
    NotProcessed = 0,  // nothing, or only the embedded camera preview, has been shown
    Queued = 1,
    Processing = 2,
    Processed = 3,  // Lucent has rendered the photo
    Failed = 4,
};

// Item flags, combined in an ItemFlags bit mask. Bit positions are stable.
enum class ItemFlag : std::uint32_t {
    ReviewRecommended = 1u << 0,  // a low-confidence automatic decision; review before sharing
    MissingSource = 1u << 1,      // the source file is not at its recorded location (relink)
    RemovableSource = 1u << 2,    // the source is on removable media (copy or relink before removal)
    DuplicateContent = 1u << 3,   // identical content to another library item
    ChangedSource = 1u << 4,      // the file changed on disk; Lucent re-reads it (stale until then)
};

using ItemFlags = std::uint32_t;

[[nodiscard]] constexpr ItemFlags flag_bit(ItemFlag flag) noexcept {
    return static_cast<ItemFlags>(flag);
}

[[nodiscard]] constexpr bool has_flag(ItemFlags flags, ItemFlag flag) noexcept {
    return (flags & flag_bit(flag)) != 0;
}

// The photographer's pick of an item (blueprint section 2: duplicates are flagged and the choice is
// kept per item; nothing is merged or deleted automatically). Numeric values are stable (the
// catalog's).
enum class PickState : std::uint8_t {
    None = 0,
    Picked = 1,
    Rejected = 2,
};

// What Lucent's most recent render of an item produced, for the Edit view's histogram and
// details. Immutable once published; shared (RenderInfoPtr) so snapshots stay cheap to copy.
struct RenderInfo {
    static constexpr int kBins = 256;

    // Histograms of the displayed 8-bit sRGB preview image (not of the full-resolution render):
    // code-value counts per channel, and of the Rec. 709 luma of the encoded values.
    std::array<std::uint32_t, kBins> red{};
    std::array<std::uint32_t, kBins> green{};
    std::array<std::uint32_t, kBins> blue{};
    std::array<std::uint32_t, kBins> luma{};
    std::uint64_t pixels = 0;
    int preview_width = 0;  // the preview image the histograms describe
    int preview_height = 0;
    // W6 edit frames: pixels with some channel at 255 (highlights) or at 0 (shadows), the pixels the
    // clipping overlay marks; when not known, the luma end bins stand in for them.
    bool clipping_counted = false;
    std::uint64_t clipped_highlights = 0;
    std::uint64_t clipped_shadows = 0;

    int rendered_width = 0;  // full-resolution upright size of the render
    int rendered_height = 0;
    QString rendering;   // base look, e.g. "Neutral v1"
    bool provisional = false;  // a provisional rendering stage was used
    // A mandatory interpretation step was not applied (the file is Unsupported, blueprint
    // section 3): the pixels are not an accurate rendering and must be presented as such.
    bool incomplete = false;
    double render_seconds = 0.0;   // the engine's render
    double preview_seconds = 0.0;  // request to finished preview (admission wait included)
    // User-facing notes: interpretation steps that were not applied, and similar.
    QStringList notes;
};

using RenderInfoPtr = std::shared_ptr<const RenderInfo>;

// One photograph in the library, as the shell presents it. A value snapshot: the service sends a
// complete new snapshot whenever anything changes (LibraryService::item_changed).
struct LibraryItem {
    ItemId id = 0;
    QString file_path;
    qint64 file_size_bytes = -1;  // -1 when unknown
    // Image of a multi-image file this item develops (0-based) and the file's image count.
    int subimage = 0;
    int subimage_count = 1;

    QString camera_make;   // e.g. "Canon"; empty when unknown
    QString camera_model;  // e.g. "EOS R5"; empty when unknown
    QString lens;          // empty when unknown
    QDateTime capture_time;  // invalid when unknown

    // Upright size of Lucent's full-resolution render, in pixels; 0 when unknown.
    int pixel_width = 0;
    int pixel_height = 0;

    // Exposure; 0 when unknown.
    double iso = 0.0;
    double exposure_time_s = 0.0;
    double f_number = 0.0;
    double focal_length_mm = 0.0;

    SupportStatus support = SupportStatus::Unknown;
    // Why the item has its status (W10, ADR 0018): the limitations or missing steps of a Limited or
    // Unsupported item, and for a Verified or Compatible, unverified one what Lucent's camera
    // support database says about its camera mode. May be empty.
    QString support_detail;
    PickState pick = PickState::None;
    int rating = 0;  // 0 = unrated, 1..5
    // The file is a verified copy Lucent made into a library folder; origin_path names the original.
    bool copied = false;
    QString origin_path;
    // Other library files with the same content (DuplicateContent), native separators.
    QStringList duplicate_paths;
    bool content_verified = false;  // the file's SHA-256 is recorded
    ProcessingStatus processing = ProcessingStatus::NotProcessed;
    QString processing_detail;  // why processing failed; may be empty
    ItemFlags flags = 0;

    Generation thumbnail_generation = kNoImage;
    // True while the thumbnail is the camera's embedded JPEG rather than Lucent's own render. The
    // shell labels such thumbnails (blueprint section 2): they are for browsing only.
    bool thumbnail_is_camera_preview = false;
    Generation preview_generation = kNoImage;

    // W6. The photo has develop settings other than as shot (its current recipe revision changes
    // something, or was made by a newer Lucent). Its thumbnail is then Lucent's render of that
    // recipe (generation 2+), never the camera preview.
    bool edited = false;

    // Lucent's most recent render of the item; null until one finished.
    RenderInfoPtr render;
};

// Progress of the running import request (LibraryService::import_progress).
struct ImportProgress {
    int folders_scanned = 0;
    int files_seen = 0;   // regular files found, including ones that are not photos
    int candidates = 0;   // files to check
    int checked = 0;      // files checked so far
    int added = 0;        // library items added so far
    bool copying = false;  // a file is being copied into the library folder and verified
    int copied = 0;        // files copied and verified so far
    QString current;      // folder or file being processed; empty when done
};

// Options of one import request (blueprint section 2: "Include subfolders"; "Copy into library
// folder" with a destination the user chooses).
struct ImportOptions {
    bool include_subfolders = false;
    // Absolute path of an existing folder to copy the photos into before they are added (each
    // copy verified); empty imports the photos where they are.
    QString copy_to;
};

// W10 (ADR 0018). The camera support database the session uses (Help > About).
struct CameraSupportInfo {
    bool available = false;  // a database is active
    QString version;         // its asset version, e.g. "1.0.0"
    QString qualified_on;    // the qualification run's date, "YYYY-MM-DD"
    QString decoder;         // the decoder and contract it was qualified with
    QString location;        // its folder (native separators)
    bool bundled = true;     // installed with Lucent (data packs are not accepted yet)
    int modes = 0;           // camera modes listed, and by state
    int verified = 0;
    int compatible_unverified = 0;
    int limited = 0;
    int unsupported = 0;
    // False when the database was qualified with another decoder or pipeline than this build's:
    // its Verified modes then count as Compatible, unverified.
    bool verified_entries_hold = true;
    QString problem;    // user-facing: why no database is active (empty when one is)
    QStringList notes;  // user-facing: data packs that were not used, and why
};

// Outcome of one LibraryService::start_import() request.
struct ImportSummary {
    QStringList sources;          // the paths the request named
    int added = 0;                // new library items
    int verified = 0;             // of `added`: Verified items
    int limited = 0;              // of `added`: Limited items
    int unsupported = 0;          // of `added`: Unsupported items (listed, never auto-exported)
    int already_in_library = 0;   // files that were already library items
    int updated = 0;              // library items whose file had changed, read again
    // Library items whose missing file was found again (back at its place, or moved, or a card
    // under another drive letter): Present again with their flags.
    int restored = 0;
    int ignored = 0;              // files that are not photos, or in Lucent's own locations
    int failed = 0;               // files or locations that could not be read (or copied)
    int copied = 0;               // photos now in the library folder as verified copies
    QString copy_to;              // the library folder of the copies; empty for an in-place import
    int removable = 0;            // of `added`: photos on removable media
    QStringList removable_locations;  // the removable drives they are on ("E:\\")
    bool cancelled = false;       // the import stopped early; items added so far stay
    QStringList errors;           // user-facing explanations of the failures, one per path
    // User-facing per-path notes: Limited/Unsupported items, selected files that are not RAW
    // photos, folders not searched because exports were written there.
    QStringList notes;
};

// Outcome of one LibraryService::relink() request (blueprint section 2: missing files remain in the
// library with a relink action; a path changes only when the content hash matches).
struct RelinkSummary {
    QString folder;        // where Lucent looked
    int missing = 0;       // missing files considered
    int relinked = 0;      // found and confirmed by content
    int unmatched = 0;     // left missing (includes unverifiable)
    int unverifiable = 0;  // of unmatched: never verified, so a match cannot be confirmed
    bool cancelled = false;
    QString error;  // user-facing reason when the relink could not run at all; empty otherwise
};

// Outcome of LibraryService::back_up_catalog().
struct BackupResult {
    bool ok = false;
    QString path;     // the backup file (native separators) when ok
    QString message;  // user-facing reason when not ok
};

// Background verification of the library's files (presence checks and content hashing).
struct VerificationProgress {
    bool active = false;
    int done = 0;   // photos verified in the current run
    int total = 0;  // photos to verify in the current run
};

using JobId = std::uint64_t;

// Export options offered by the shell (blueprint section 7: "JPEG / sRGB / quality 95" as the
// sharing preset, "16-bit TIFF / selected ICC profile" as the editing preset). Numeric values are
// stable.
enum class ExportFileFormat : std::uint8_t {
    Jpeg = 1,    // 8-bit baseline JPEG
    Tiff16 = 2,  // 16-bit TIFF, Deflate
};

enum class ExportColorSpace : std::uint8_t {
    Srgb = 1,
    DisplayP3 = 2,
};

inline constexpr int kDefaultJpegQuality = 95;

struct ExportOptions {
    ExportFileFormat format = ExportFileFormat::Jpeg;
    ExportColorSpace color_space = ExportColorSpace::Srgb;
    int jpeg_quality = kDefaultJpegQuality;  // 1..100; used for JPEG only

    friend bool operator==(const ExportOptions&, const ExportOptions&) = default;
};

enum class ExportJobState : std::uint8_t {
    Queued = 1,
    Running = 2,
    Completed = 3,
    Failed = 4,
    Cancelled = 5,
};

[[nodiscard]] constexpr bool is_active(ExportJobState state) noexcept {
    return state == ExportJobState::Queued || state == ExportJobState::Running;
}

struct ExportRequest {
    QList<ItemId> items;    // in the order the jobs should run
    QString output_folder;  // chosen by the user; must exist
    ExportOptions options;
};

// One export job, as a value snapshot (ExportService::job_changed sends a complete new one).
struct ExportJob {
    JobId id = 0;
    ItemId item = 0;
    QString source_name;       // file name of the source photo
    QString output_folder;     // the destination folder the user chose
    QString destination_path;  // the published file; empty until the export completed
    ExportOptions options;
    ExportJobState state = ExportJobState::Queued;
    double progress = 0.0;  // 0..1, meaningful while Running
    // Current stage while Running ("Rendering…"), the result when Completed, the reason when
    // Failed; may be empty.
    QString message;
    bool cancel_requested = false;  // cancellation was asked for and has not taken effect yet
};

}  // namespace lucent::ui

Q_DECLARE_METATYPE(lucent::ui::RenderInfoPtr)
