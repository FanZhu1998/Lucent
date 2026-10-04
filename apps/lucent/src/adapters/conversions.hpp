#pragma once

// Conversions between the application services' types (lucent::app, lucent::raw, lucent::exporter)
// and the shell's UI types (services/service_types.hpp), and the user-facing wording of the
// services' errors and assessments (lucent::Error::message is developer-facing and is never shown
// verbatim; conventions and core/error.hpp).
//
// Everything here is a pure function of its arguments with no file-system access, safe to call
// from any thread (Qt value types are reentrant). Pixel conversions are meant for worker threads.

#include <QImage>
#include <QSize>
#include <QString>
#include <QStringList>
#include <filesystem>
#include <memory>

#include <lucent/application/export_service.hpp>
#include <lucent/application/import.hpp>
#include <lucent/application/preview.hpp>
#include <lucent/application/source_item.hpp>
#include <lucent/application/source_maintenance.hpp>
#include <lucent/core/error.hpp>
#include <lucent/export/export.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/raw/embedded_preview.hpp>

#include "services/service_types.hpp"

namespace lucent::ui::adapters {

// --- Paths -----------------------------------------------------------------------------------

[[nodiscard]] QString to_qstring(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path to_path(const QString& path);

// --- Library items ---------------------------------------------------------------------------

// Supported -> CompatibleUnverified (Lucent has no qualification data before W10, so nothing is
// Verified), Limited -> Limited, Unsupported -> Unsupported.
[[nodiscard]] SupportStatus to_ui_support(app::SupportStatus status) noexcept;

// One user-facing sentence per interpretation issue kind.
[[nodiscard]] QString describe_issue(raw::InterpretationIssueKind kind);

// Why an item has its status, for the badge tooltip and the details panel: the distinct reasons
// joined; for Supported items a note that the camera is not verified yet.
[[nodiscard]] QString support_detail(const app::SupportAssessment& support);

// The shell's flags for the source facts of `source` (W4): MissingSource, ChangedSource,
// RemovableSource (a file on removable media, a card or a USB disk) and DuplicateContent.
[[nodiscard]] ItemFlags source_flags(const app::SourceItem& source) noexcept;

// The item as the shell first shows it: thumbnail generation 1 (the camera's embedded JPEG,
// labelled) and preview generation 1, not processed. An item whose probe failed has no thumbnail
// (kNoImage): nothing can be extracted from a file the decoder does not recognise.
// `duplicate_paths` are the paths of source.content_duplicates (the caller looks them up).
[[nodiscard]] LibraryItem to_library_item(const app::SourceItem& source,
                                          const QStringList& duplicate_paths = {});

// Copies the facts the application library owns (file, camera, capture, support, source facts,
// pick) from `fresh` into `current`, keeping the shell's own state (generations, processing,
// render).
void merge_library_facts(LibraryItem& current, const LibraryItem& fresh);

// --- Errors ----------------------------------------------------------------------------------

// A user-facing sentence for `error`, chosen by its code and location. An unreadable recipe
// (where "app.recipe...", app::kRecipeUnreadableWhere) is described as the photo's edit, never as
// its file.
[[nodiscard]] QString describe_error(const Error& error);

// --- Import ----------------------------------------------------------------------------------

[[nodiscard]] ImportProgress to_ui_progress(const app::ImportProgress& progress);

// Counts, per-path failures and notes of a finished import. `sources` are the request's paths.
// Notes name selected files that are not RAW photos, folders not searched because exports were
// written there, and removable drives the new photos are on; files that could not be copied into
// the library folder are failures with their reason. Notes about Limited and Unsupported items
// are added by the caller, which knows the items.
[[nodiscard]] ImportSummary summarize_import(const QStringList& sources, const app::ImportReport& report);

// The shell's summary of a relink (`folder` as the user chose it).
[[nodiscard]] RelinkSummary summarize_relink(const QString& folder,
                                             const Expected<app::RelinkReport>& report);

// --- Previews --------------------------------------------------------------------------------

// The preview quality for a request box (ImageRequest::bounding_size) and the item's
// full-resolution upright size (0 when unknown): Full when the box does not constrain the image
// (both sides non-positive, or at least the image's size), otherwise Fit with the long edge of
// the image fitted into the box (rounded up), clamped to PreviewQuality's range.
[[nodiscard]] app::PreviewQuality preview_quality_for(QSize bounding, int pixel_width,
                                                      int pixel_height) noexcept;

// The preview's pixels as a QImage (Format_RGB32, opaque, the format Qt Quick uploads without
// conversion). A deep copy: the result does not refer to `preview`.
[[nodiscard]] QImage to_qimage(const app::DisplayImage& image);

// `image` transformed from stored orientation to upright (EXIF semantics; pixel-exact).
[[nodiscard]] QImage apply_orientation(const QImage& image, image::Orientation orientation);

// Decodes an embedded camera JPEG for display, upright, scaled down (never up) to fit
// `bounding` (non-positive sides unconstrained). The JPEG's own EXIF orientation is ignored and
// preview.orientation applied instead (raw/embedded_preview.hpp). A null image and `*error` set
// when it cannot be decoded.
[[nodiscard]] QImage decode_camera_preview(const raw::EmbeddedPreview& preview, QSize bounding,
                                           QString* error);

// What the shell shows about a finished preview (histograms, render facts, user-facing notes;
// `incomplete` when the render's interpretation maps to Unsupported).
[[nodiscard]] RenderInfoPtr to_render_info(const app::PreviewImage& preview);
// The same for any displayed image with its histogram and render summary (edit-session frames);
// `seconds` is the time from request to pixels.
[[nodiscard]] RenderInfoPtr to_render_info(const app::DisplayImage& image, const app::Histogram& histogram,
                                           const app::RenderSummary& summary, double seconds);

// --- Export ----------------------------------------------------------------------------------

[[nodiscard]] exporter::ExportSettings to_export_settings(const ExportOptions& options) noexcept;

// "Rendering…", "Encoding…", ...
[[nodiscard]] QString describe_export_stage(app::ExportStage stage);

}  // namespace lucent::ui::adapters
