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

// Identity of a library item: unique and stable for the lifetime of the service instance (the
// catalog id from W4 on). Zero is never a valid id.
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
};

using ItemFlags = std::uint32_t;

[[nodiscard]] constexpr ItemFlags flag_bit(ItemFlag flag) noexcept {
    return static_cast<ItemFlags>(flag);
}

[[nodiscard]] constexpr bool has_flag(ItemFlags flags, ItemFlag flag) noexcept {
    return (flags & flag_bit(flag)) != 0;
}

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

    int rendered_width = 0;  // full-resolution upright size of the render
    int rendered_height = 0;
    QString rendering;   // display rendering, e.g. "Neutral v0"
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
    QString support_detail;  // why the status is Limited/Unsupported; may be empty
    ProcessingStatus processing = ProcessingStatus::NotProcessed;
    QString processing_detail;  // why processing failed; may be empty
    ItemFlags flags = 0;

    Generation thumbnail_generation = kNoImage;
    // True while the thumbnail is the camera's embedded JPEG rather than Lucent's own render. The
    // shell labels such thumbnails (blueprint section 2): they are for browsing only.
    bool thumbnail_is_camera_preview = false;
    Generation preview_generation = kNoImage;

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
    QString current;      // folder or file being processed; empty when done
};

// Outcome of one LibraryService::import_paths() request.
struct ImportSummary {
    QStringList sources;          // the paths the request named
    int added = 0;                // new library items
    int limited = 0;              // of `added`: Limited items
    int unsupported = 0;          // of `added`: Unsupported items (listed, never auto-exported)
    int already_in_library = 0;   // files that were already library items
    int updated = 0;              // library items whose file had changed, read again
    int ignored = 0;              // files that are not photos, or in Lucent's own locations
    int failed = 0;               // files or locations that could not be read
    bool cancelled = false;       // the import stopped early; items added so far stay
    QStringList errors;           // user-facing explanations of the failures, one per path
    // User-facing per-path notes: Limited/Unsupported items, selected files that are not RAW
    // photos, folders not searched because exports were written there.
    QStringList notes;
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
