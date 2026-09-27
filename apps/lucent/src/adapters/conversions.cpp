#include "adapters/conversions.hpp"

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>

namespace lucent::ui::adapters {
namespace {

QString tr_text(const char* text) {
    return QCoreApplication::translate("lucent::ui::adapters", text);
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

// `stored` scaled down so that its upright form fits `bounding` (non-positive sides are
// unconstrained); returned in stored orientation. Never enlarged.
QSize fitted_stored_size(QSize stored, image::Orientation orientation, QSize bounding) {
    const bool swap = image::swaps_axes(orientation);
    const QSize upright = swap ? stored.transposed() : stored;
    constexpr double kUnconstrained = std::numeric_limits<double>::infinity();
    const double sx = bounding.width() > 0 ? double(bounding.width()) / upright.width() : kUnconstrained;
    const double sy = bounding.height() > 0 ? double(bounding.height()) / upright.height() : kUnconstrained;
    const double scale = std::min(sx, sy);
    if (!(scale < 1.0)) {
        return stored;
    }
    const QSize fitted(std::max(1, static_cast<int>(std::lround(upright.width() * scale))),
                       std::max(1, static_cast<int>(std::lround(upright.height() * scale))));
    return swap ? fitted.transposed() : fitted;
}

QString skip_message(const app::SkippedPath& skipped) {
    const QString path = QDir::toNativeSeparators(to_qstring(skipped.path));
    switch (skipped.reason) {
        case app::SkipReason::NotFound:
            return tr_text("%1 was not found.").arg(path);
        case app::SkipReason::NotAFileOrFolder:
            return tr_text("%1 is not a file or folder.").arg(path);
        case app::SkipReason::DepthLimit:
            return tr_text("%1 is nested too deeply and was not scanned.").arg(path);
        case app::SkipReason::Inaccessible:
            return tr_text("%1 could not be read: %2")
                .arg(path, skipped.error ? describe_error(*skipped.error) : tr_text("access failed."));
        case app::SkipReason::InOutputDirectory:
            return tr_text("%1 was not searched: Lucent exported photos into it in this session.").arg(path);
        case app::SkipReason::NotRawPhoto:
            return tr_text("%1 is not a RAW photo, so it was not added.").arg(path);
        case app::SkipReason::Duplicate:
        case app::SkipReason::InLucentDirectory:
        case app::SkipReason::LucentTemporaryFile:
        case app::SkipReason::DirectoryCycle:
            break;
    }
    return {};
}

}  // namespace

// --- Paths -----------------------------------------------------------------------------------

QString to_qstring(const std::filesystem::path& path) {
    return QString::fromStdWString(path.native());
}

std::filesystem::path to_path(const QString& path) {
    return std::filesystem::path(QDir::toNativeSeparators(path).toStdWString());
}

// --- Library items ---------------------------------------------------------------------------

SupportStatus to_ui_support(app::SupportStatus status) noexcept {
    switch (status) {
        case app::SupportStatus::Supported:
            return SupportStatus::CompatibleUnverified;
        case app::SupportStatus::Limited:
            return SupportStatus::Limited;
        case app::SupportStatus::Unsupported:
            return SupportStatus::Unsupported;
    }
    return SupportStatus::Unsupported;
}

QString describe_issue(raw::InterpretationIssueKind kind) {
    using K = raw::InterpretationIssueKind;
    switch (kind) {
        case K::MandatoryOperationUnsupported:
            return tr_text("A processing step this file requires is not available in Lucent yet.");
        case K::OptionalOperationUnsupported:
            return tr_text(
                "An optional processing step is not available; the photo is developed without it.");
        case K::DngVersionUnsupported:
            return tr_text("This DNG needs a newer DNG version than Lucent reads.");
        case K::DngInspectionFailed:
            return tr_text("Lucent could not inspect the structure of this DNG.");
        case K::DngRawImageAmbiguous:
            return tr_text("Lucent cannot tell which image of this DNG to develop.");
        case K::DecoderUnsupportedFormat:
            return tr_text("The RAW data is stored in a format Lucent cannot decode.");
        case K::UnsupportedSensorLayout:
            return tr_text("This camera's sensor layout is not supported.");
        case K::UnsupportedChannelLayout:
            return tr_text("This file's colour channel layout is not supported.");
        case K::SubimageNotAPhotograph:
            return tr_text(
                "This image is a component of another photograph (for example a Dual Pixel frame).");
        case K::SubimageFramesNotCombined:
            return tr_text(
                "One frame of a multi-shot capture; Lucent develops it on its own without combining frames.");
        case K::SubimageRoleUnknown:
            return tr_text("Lucent cannot tell what this image of a multi-image file is.");
        case K::MultiViewSource:
            return tr_text("Unstitched multi-lens (360°) images are not supported.");
        case K::UnrepresentedBlackLevelCorrection:
            return tr_text("The file's black-level corrections cannot be applied exactly.");
    }
    return tr_text("Lucent cannot interpret part of this file.");
}

namespace {

QString describe_status(raw::InterpretationStatus status) {
    switch (status) {
        case raw::InterpretationStatus::Complete:
            return {};
        case raw::InterpretationStatus::OptionalOperationsSkipped:
            return describe_issue(raw::InterpretationIssueKind::OptionalOperationUnsupported);
        case raw::InterpretationStatus::RequiresUnsupportedOperation:
            return describe_issue(raw::InterpretationIssueKind::MandatoryOperationUnsupported);
        case raw::InterpretationStatus::Ambiguous:
            return tr_text("Lucent cannot determine how this file must be processed.");
        case raw::InterpretationStatus::NotDecodable:
            return tr_text("The RAW data in this file cannot be decoded.");
    }
    return tr_text("Lucent cannot interpret part of this file.");
}

QString describe_reason(const app::SupportReason& reason) {
    switch (reason.kind) {
        case app::SupportReasonKind::InterpretationIssue:
            if (reason.issue) {
                return describe_issue(*reason.issue);
            }
            break;
        case app::SupportReasonKind::InterpretationStatus:
            break;
        case app::SupportReasonKind::ProbeFailed: {
            Error error;
            error.code = reason.error_code.value_or(ErrorCode::Unknown);
            error.where = "raw.probe";
            return describe_error(error);
        }
    }
    return {};
}

}  // namespace

QString support_detail(const app::SupportAssessment& support) {
    if (support.status == app::SupportStatus::Supported) {
        return tr_text(
            "Lucent decodes this file, but has not verified this camera model with its quality tests yet.");
    }
    QStringList sentences;
    for (const app::SupportReason& reason : support.reasons) {
        QString sentence = describe_reason(reason);
        if (sentence.isEmpty() && reason.kind == app::SupportReasonKind::InterpretationStatus &&
            support.interpretation) {
            sentence = describe_status(*support.interpretation);
        }
        if (!sentence.isEmpty() && !sentences.contains(sentence)) {
            sentences << sentence;
        }
    }
    if (sentences.isEmpty() && support.interpretation) {
        sentences << describe_status(*support.interpretation);
    }
    if (sentences.isEmpty()) {
        sentences << tr_text("Lucent cannot interpret part of this file.");
    }
    return sentences.join(u' ');
}

LibraryItem to_library_item(const app::SourceItem& source) {
    LibraryItem item;
    item.id = source.id.value;
    item.file_path = QDir::toNativeSeparators(to_qstring(source.path));
    item.file_size_bytes =
        source.identity.size_bytes > 0 ? static_cast<qint64>(source.identity.size_bytes) : qint64{-1};
    item.subimage = static_cast<int>(source.subimage);
    item.subimage_count = static_cast<int>(std::max<std::uint32_t>(1, source.subimage_count));
    if (source.probe) {
        const app::ProbeSummary& probe = *source.probe;
        item.camera_make = QString::fromStdString(probe.camera_make);
        item.camera_model = QString::fromStdString(probe.camera_model);
        item.lens = QString::fromStdString(probe.lens);
        if (probe.capture_time_local) {
            // Camera wall-clock time without a zone: shown as recorded (local time).
            item.capture_time =
                QDateTime::fromString(QString::fromStdString(*probe.capture_time_local), Qt::ISODate);
        }
        item.pixel_width = probe.upright_size.width;
        item.pixel_height = probe.upright_size.height;
        item.iso = probe.iso.value_or(0.0f);
        item.exposure_time_s = probe.exposure_time_s.value_or(0.0f);
        item.f_number = probe.f_number.value_or(0.0f);
        item.focal_length_mm = probe.focal_length_mm.value_or(0.0f);
    }
    item.support = to_ui_support(source.support.status);
    item.support_detail = support_detail(source.support);
    item.thumbnail_generation = source.probe ? kCameraPreviewGeneration : kNoImage;
    item.thumbnail_is_camera_preview = source.probe.has_value();
    item.preview_generation = 1;
    return item;
}

void merge_library_facts(LibraryItem& current, const LibraryItem& fresh) {
    current.file_path = fresh.file_path;
    current.file_size_bytes = fresh.file_size_bytes;
    current.subimage = fresh.subimage;
    current.subimage_count = fresh.subimage_count;
    current.camera_make = fresh.camera_make;
    current.camera_model = fresh.camera_model;
    current.lens = fresh.lens;
    current.capture_time = fresh.capture_time;
    if (!current.render) {
        // Once rendered, the render's size is authoritative.
        current.pixel_width = fresh.pixel_width;
        current.pixel_height = fresh.pixel_height;
    }
    current.iso = fresh.iso;
    current.exposure_time_s = fresh.exposure_time_s;
    current.f_number = fresh.f_number;
    current.focal_length_mm = fresh.focal_length_mm;
    current.support = fresh.support;
    current.support_detail = fresh.support_detail;
    current.flags = fresh.flags;
}

// --- Errors ----------------------------------------------------------------------------------

QString describe_error(const Error& error) {
    // Where the failure happened: at the destination (the export service's destination checks
    // and the exporter's writing), in the services' own item checks, or reading the source.
    const std::string_view where = error.where;
    const bool destination = starts_with(where, "app.export.destination") ||
                             starts_with(where, "app.export.space") || starts_with(where, "export");
    switch (error.code) {
        case ErrorCode::NotFound:
            if (destination) {
                return tr_text("The output folder does not exist.");
            }
            if (where == "app.export" || where == "app.preview" || where == "app.preview.camera") {
                return tr_text("This photo is no longer in the library.");
            }
            return tr_text("The file was not found. It may have been moved, renamed or deleted.");
        case ErrorCode::AlreadyExists:
            return tr_text("A file with the chosen name already exists; nothing was overwritten.");
        case ErrorCode::PermissionDenied:
            return destination ? tr_text("Windows denied access to the output folder.")
                               : tr_text("Windows denied access to the file.");
        case ErrorCode::IoError:
            return destination ? tr_text("Writing the file failed (a disk or network error).")
                               : tr_text("Reading the file failed (a disk or network error).");
        case ErrorCode::DiskFull:
            return tr_text("There is not enough free space on the destination drive.");
        case ErrorCode::Corrupt:
            return destination ? tr_text("The written file did not pass verification and was removed.")
                               : tr_text("The file is damaged or incomplete.");
        case ErrorCode::Unsupported:
            return starts_with(where, "app.export.support")
                       ? tr_text("This photo is Unsupported, so Lucent does not export it.")
                       : tr_text("Lucent cannot decode this file.");
        case ErrorCode::LimitExceeded:
            return tr_text("The photo is too large for Lucent's memory or size limits.");
        case ErrorCode::OutOfMemory:
            return tr_text("There is not enough memory. Close other programs and try again.");
        case ErrorCode::Cancelled:
            return tr_text("Cancelled.");
        case ErrorCode::Timeout:
            return tr_text("The operation took too long and was stopped.");
        case ErrorCode::SourceChanged:
            return tr_text("The file changed after it was added. Add it again to update it.");
        case ErrorCode::VersionMismatch:
            return tr_text("The file needs a newer version of Lucent.");
        case ErrorCode::InvalidArgument:
        case ErrorCode::Internal:
        case ErrorCode::Unknown:
            break;
    }
    const std::string_view code = to_string(error.code);
    return tr_text("Lucent ran into an internal problem (%1).")
        .arg(QString::fromUtf8(code.data(), static_cast<qsizetype>(code.size())));
}

// --- Import ----------------------------------------------------------------------------------

ImportProgress to_ui_progress(const app::ImportProgress& progress) {
    const auto clamp_int = [](std::uint64_t value) {
        return static_cast<int>(std::min<std::uint64_t>(value, std::numeric_limits<int>::max()));
    };
    ImportProgress ui;
    ui.folders_scanned = clamp_int(progress.directories_scanned);
    ui.files_seen = clamp_int(progress.files_seen);
    ui.candidates = clamp_int(progress.candidates);
    ui.checked = clamp_int(progress.probed);
    ui.added = clamp_int(progress.added);
    ui.current = QDir::toNativeSeparators(to_qstring(progress.current));
    return ui;
}

ImportSummary summarize_import(const QStringList& sources, const app::ImportReport& report) {
    ImportSummary summary;
    summary.sources = sources;
    summary.added = static_cast<int>(report.added.size());
    summary.limited = static_cast<int>(report.limited);
    summary.unsupported = static_cast<int>(report.unsupported);
    summary.updated = static_cast<int>(report.refreshed.size());
    summary.ignored = static_cast<int>(report.ignored_non_raw);
    summary.cancelled = report.cancelled;
    for (const app::SkippedPath& skipped : report.skipped) {
        switch (skipped.reason) {
            case app::SkipReason::Duplicate:
                ++summary.already_in_library;
                break;
            case app::SkipReason::NotRawPhoto:
                ++summary.ignored;
                summary.notes << skip_message(skipped);
                break;
            case app::SkipReason::InOutputDirectory:
                summary.notes << skip_message(skipped);  // a folder (or a link into one), not a file
                break;
            case app::SkipReason::InLucentDirectory:
            case app::SkipReason::LucentTemporaryFile:
                ++summary.ignored;
                break;
            case app::SkipReason::DirectoryCycle:
                break;  // the folder was scanned once already
            case app::SkipReason::DepthLimit:
            case app::SkipReason::Inaccessible:
            case app::SkipReason::NotFound:
            case app::SkipReason::NotAFileOrFolder:
                ++summary.failed;
                summary.errors << skip_message(skipped);
                break;
        }
    }
    return summary;
}

// --- Previews --------------------------------------------------------------------------------

app::PreviewQuality preview_quality_for(QSize bounding, int pixel_width, int pixel_height) noexcept {
    const int box_width = bounding.width();
    const int box_height = bounding.height();
    if (box_width <= 0 && box_height <= 0) {
        return app::PreviewQuality::full();
    }
    double long_edge = 0.0;
    if (pixel_width > 0 && pixel_height > 0) {
        const bool fits_width = box_width <= 0 || box_width >= pixel_width;
        const bool fits_height = box_height <= 0 || box_height >= pixel_height;
        if (fits_width && fits_height) {
            return app::PreviewQuality::full();
        }
        constexpr double kUnconstrained = std::numeric_limits<double>::infinity();
        const double sx = box_width > 0 ? double(box_width) / pixel_width : kUnconstrained;
        const double sy = box_height > 0 ? double(box_height) / pixel_height : kUnconstrained;
        long_edge = std::ceil(std::max(pixel_width, pixel_height) * std::min(sx, sy) - 1e-9);
    } else {
        long_edge = std::max(box_width, box_height);
    }
    const auto edge = static_cast<std::int32_t>(std::clamp(
        long_edge, double(app::PreviewQuality::kMinLongEdge), double(app::PreviewQuality::kMaxLongEdge)));
    return app::PreviewQuality::fit(edge);
}

QImage to_qimage(const app::DisplayImage& image) {
    if (image.width <= 0 || image.height <= 0 ||
        image.rgb.size() != std::size_t(image.width) * std::size_t(image.height) * 3) {
        return {};
    }
    QImage out(image.width, image.height, QImage::Format_RGB32);
    if (out.isNull()) {
        return {};  // allocation failed
    }
    const std::uint8_t* source = image.rgb.data();
    for (int y = 0; y < image.height; ++y) {
        auto* row = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < image.width; ++x, source += 3) {
            row[x] = qRgb(source[0], source[1], source[2]);
        }
    }
    return out;
}

QImage apply_orientation(const QImage& image, image::Orientation orientation) {
    // Rotations by multiples of 90 degrees and mirrors are exact pixel permutations in QImage.
    switch (orientation) {
        case image::Orientation::Normal:
            return image;
        case image::Orientation::FlipHorizontal:
            return image.flipped(Qt::Horizontal);
        case image::Orientation::Rotate180:
            return image.flipped(Qt::Horizontal | Qt::Vertical);
        case image::Orientation::FlipVertical:
            return image.flipped(Qt::Vertical);
        case image::Orientation::Transpose:
            return image.transformed(QTransform().rotate(90)).flipped(Qt::Horizontal);
        case image::Orientation::Rotate90Cw:
            return image.transformed(QTransform().rotate(90));
        case image::Orientation::Transverse:
            return image.transformed(QTransform().rotate(90)).flipped(Qt::Vertical);
        case image::Orientation::Rotate90Ccw:
            return image.transformed(QTransform().rotate(270));
    }
    return image;
}

QImage decode_camera_preview(const raw::EmbeddedPreview& preview, QSize bounding, QString* error) {
    const auto fail = [error](const QString& message) {
        if (error != nullptr) {
            *error = message;
        }
        return QImage();
    };
    if (preview.bytes.empty() || preview.width <= 0 || preview.height <= 0 ||
        preview.bytes.size() > std::size_t(std::numeric_limits<int>::max())) {
        return fail(tr_text("The camera preview is empty."));
    }
    // No copy: the buffer only reads the preview's bytes, which outlive this call.
    const QByteArray bytes = QByteArray::fromRawData(reinterpret_cast<const char*>(preview.bytes.data()),
                                                     static_cast<qsizetype>(preview.bytes.size()));
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return fail(tr_text("The camera preview could not be read."));
    }
    QImageReader reader(&buffer, "jpeg");
    // The RAW's orientation is the only one to apply (raw/embedded_preview.hpp).
    reader.setAutoTransform(false);
    const QSize stored(preview.width, preview.height);
    const QSize target = fitted_stored_size(stored, preview.orientation, bounding);
    if (target != stored) {
        reader.setScaledSize(target);  // libjpeg's scaled IDCT, then a smooth resize
    }
    QImage decoded = reader.read();
    if (decoded.isNull()) {
        return fail(tr_text("The camera preview is damaged and cannot be shown."));
    }
    return apply_orientation(decoded, preview.orientation);
}

RenderInfoPtr to_render_info(const app::PreviewImage& preview) {
    auto info = std::make_shared<RenderInfo>();
    std::copy(preview.histogram.red.begin(), preview.histogram.red.end(), info->red.begin());
    std::copy(preview.histogram.green.begin(), preview.histogram.green.end(), info->green.begin());
    std::copy(preview.histogram.blue.begin(), preview.histogram.blue.end(), info->blue.begin());
    std::copy(preview.histogram.luminance.begin(), preview.histogram.luminance.end(), info->luma.begin());
    info->pixels = preview.histogram.pixels;
    info->preview_width = preview.image.width;
    info->preview_height = preview.image.height;

    const app::RenderSummary& summary = preview.summary;
    info->rendered_width = summary.rendered_size.width;
    info->rendered_height = summary.rendered_size.height;
    switch (summary.rendering) {
        case engine::DisplayRendering::NeutralV0:
            info->rendering = tr_text("Neutral v0");
            break;
    }
    info->provisional = summary.provisional;
    info->incomplete = app::support_status_for(summary.interpretation) == app::SupportStatus::Unsupported;
    info->render_seconds = summary.render_seconds;
    info->preview_seconds = preview.seconds;
    for (const raw::InterpretationIssue& issue : summary.issues) {
        const QString note = describe_issue(issue.kind);
        if (!info->notes.contains(note)) {
            info->notes << note;
        }
    }
    if (info->notes.isEmpty() && summary.interpretation != raw::InterpretationStatus::Complete) {
        info->notes << describe_status(summary.interpretation);
    }
    return info;
}

// --- Export ----------------------------------------------------------------------------------

exporter::ExportSettings to_export_settings(const ExportOptions& options) noexcept {
    const exporter::OutputProfile profile = options.color_space == ExportColorSpace::DisplayP3
                                                ? exporter::OutputProfile::DisplayP3
                                                : exporter::OutputProfile::Srgb;
    if (options.format == ExportFileFormat::Tiff16) {
        return exporter::ExportSettings::tiff16(profile);
    }
    exporter::ExportSettings settings = exporter::ExportSettings::jpeg_srgb_q95();
    settings.profile = profile;
    settings.jpeg.quality = std::clamp(options.jpeg_quality, 1, 100);
    return settings;
}

QString describe_export_stage(app::ExportStage stage) {
    switch (stage) {
        case app::ExportStage::Preparing:
            return tr_text("Preparing…");
        case app::ExportStage::Rendering:
            return tr_text("Rendering…");
        case app::ExportStage::Encoding:
            return tr_text("Encoding…");
        case app::ExportStage::Writing:
            return tr_text("Writing and verifying…");
        case app::ExportStage::Finished:
            return tr_text("Finishing…");
    }
    return {};
}

}  // namespace lucent::ui::adapters
