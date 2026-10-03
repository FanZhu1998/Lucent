#include "models/presentation.hpp"

#include <cmath>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace lucent::ui::presentation {
namespace {

QString tr_text(const char* text) {
    return QCoreApplication::translate("lucent::ui::presentation", text);
}

bool is_positive(double value) {
    return std::isfinite(value) && value > 0.0;
}

// `value` with at most one decimal, without a trailing ".0".
QString one_decimal(double value, const QLocale& locale) {
    const double rounded = std::round(value * 10.0) / 10.0;
    if (std::abs(rounded - std::round(rounded)) < 1e-9) {
        return locale.toString(static_cast<qlonglong>(std::llround(rounded)));
    }
    return locale.toString(rounded, 'f', 1);
}

}  // namespace

QString file_name(const LibraryItem& item) {
    const QString name = QFileInfo(item.file_path).fileName();
    if (item.subimage_count > 1) {
        return tr_text("%1 (image %2 of %3)")
            .arg(name, QString::number(item.subimage + 1), QString::number(item.subimage_count));
    }
    return name;
}

QString folder_path(const LibraryItem& item) {
    return QDir::toNativeSeparators(QFileInfo(item.file_path).path());
}

QString camera_label(const LibraryItem& item) {
    const QString make = item.camera_make.trimmed();
    const QString model = item.camera_model.trimmed();
    if (make.isEmpty() && model.isEmpty()) {
        return tr_text("Unknown camera");
    }
    if (make.isEmpty()) {
        return model;
    }
    if (model.isEmpty()) {
        return make;
    }
    // "NIKON CORPORATION" + "NIKON Z 6" and "Canon" + "Canon EOS R5" read as the model alone.
    const QString make_first_word = make.section(u' ', 0, 0);
    if (model.startsWith(make_first_word, Qt::CaseInsensitive)) {
        return model;
    }
    return make + u' ' + model;
}

QString lens_label(const LibraryItem& item) {
    return item.lens.trimmed();
}

QString capture_time_label(const LibraryItem& item, const QLocale& locale) {
    if (!item.capture_time.isValid()) {
        return tr_text("Capture time unknown");
    }
    return locale.toString(item.capture_time, QLocale::ShortFormat);
}

QString dimensions_label(const LibraryItem& item, const QLocale& locale) {
    if (item.pixel_width <= 0 || item.pixel_height <= 0) {
        return {};
    }
    const double megapixels = double(item.pixel_width) * double(item.pixel_height) / 1e6;
    return tr_text("%1 × %2 · %3 MP")
        .arg(QString::number(item.pixel_width), QString::number(item.pixel_height),
             locale.toString(megapixels, 'f', 1));
}

QString shutter_label(double seconds, const QLocale& locale) {
    if (!is_positive(seconds)) {
        return {};
    }
    if (seconds < 1.0) {
        const double reciprocal = 1.0 / seconds;
        const double nearest = std::round(reciprocal);
        if (nearest >= 2.0 && std::abs(reciprocal - nearest) <= 0.05 * nearest) {
            return tr_text("1/%1 s").arg(QString::number(static_cast<qlonglong>(nearest)));
        }
    }
    return tr_text("%1 s").arg(one_decimal(seconds, locale));
}

QString aperture_label(double f_number, const QLocale& locale) {
    if (!is_positive(f_number)) {
        return {};
    }
    return tr_text("f/%1").arg(one_decimal(f_number, locale));
}

QString exposure_label(const LibraryItem& item, const QLocale& locale) {
    QStringList parts;
    if (const QString shutter = shutter_label(item.exposure_time_s, locale); !shutter.isEmpty()) {
        parts << shutter;
    }
    if (const QString aperture = aperture_label(item.f_number, locale); !aperture.isEmpty()) {
        parts << aperture;
    }
    if (is_positive(item.iso)) {
        parts << tr_text("ISO %1").arg(QString::number(static_cast<qlonglong>(std::llround(item.iso))));
    }
    if (is_positive(item.focal_length_mm)) {
        parts << tr_text("%1 mm").arg(one_decimal(item.focal_length_mm, locale));
    }
    return parts.join(QStringLiteral(" · "));
}

QString file_size_label(qint64 bytes, const QLocale& locale) {
    if (bytes < 0) {
        return {};
    }
    return locale.formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

QString support_key(SupportStatus status) {
    switch (status) {
    case SupportStatus::Verified:
        return QStringLiteral("verified");
    case SupportStatus::CompatibleUnverified:
        return QStringLiteral("unverified");
    case SupportStatus::Limited:
        return QStringLiteral("limited");
    case SupportStatus::Unsupported:
        return QStringLiteral("unsupported");
    case SupportStatus::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

QString support_label(SupportStatus status) {
    switch (status) {
    case SupportStatus::Verified:
        return tr_text("Verified");
    case SupportStatus::CompatibleUnverified:
        return tr_text("Compatible, unverified");
    case SupportStatus::Limited:
        return tr_text("Limited");
    case SupportStatus::Unsupported:
        return tr_text("Unsupported");
    case SupportStatus::Unknown:
        break;
    }
    return tr_text("Not checked");
}

QString support_short_label(SupportStatus status) {
    if (status == SupportStatus::CompatibleUnverified) {
        return tr_text("Unverified");
    }
    return support_label(status);
}

QString processing_label(ProcessingStatus status) {
    switch (status) {
    case ProcessingStatus::NotProcessed:
        return tr_text("Not processed");
    case ProcessingStatus::Queued:
        return tr_text("Queued");
    case ProcessingStatus::Processing:
        return tr_text("Processing");
    case ProcessingStatus::Processed:
        return tr_text("Processed");
    case ProcessingStatus::Failed:
        return tr_text("Processing failed");
    }
    return {};
}

bool render_incomplete(const LibraryItem& item) {
    return item.processing == ProcessingStatus::Processed && item.render != nullptr && item.render->incomplete;
}

QString processing_key(const LibraryItem& item) {
    if (render_incomplete(item)) {
        return QStringLiteral("incomplete");
    }
    switch (item.processing) {
    case ProcessingStatus::NotProcessed:
        return QStringLiteral("notprocessed");
    case ProcessingStatus::Queued:
        return QStringLiteral("queued");
    case ProcessingStatus::Processing:
        return QStringLiteral("processing");
    case ProcessingStatus::Processed:
        return QStringLiteral("processed");
    case ProcessingStatus::Failed:
        return QStringLiteral("failed");
    }
    return QStringLiteral("notprocessed");
}

QString processing_label(const LibraryItem& item) {
    return render_incomplete(item) ? tr_text("Incomplete render") : processing_label(item.processing);
}

QString processing_detail(const LibraryItem& item) {
    if (!item.processing_detail.isEmpty() || !render_incomplete(item)) {
        return item.processing_detail;
    }
    return tr_text("Lucent rendered this photo without a processing step it requires, so the result is not "
                   "accurate. It is shown for reference only.");
}

QStringList flag_labels(ItemFlags flags) {
    QStringList labels;
    if (has_flag(flags, ItemFlag::ReviewRecommended)) {
        labels << tr_text("Review recommended");
    }
    if (has_flag(flags, ItemFlag::MissingSource)) {
        labels << tr_text("Missing file");
    }
    if (has_flag(flags, ItemFlag::RemovableSource)) {
        labels << tr_text("Removable media");
    }
    if (has_flag(flags, ItemFlag::DuplicateContent)) {
        labels << tr_text("Duplicate");
    }
    if (has_flag(flags, ItemFlag::ChangedSource)) {
        labels << tr_text("Changed on disk");
    }
    return labels;
}

QStringList flag_keys(ItemFlags flags) {
    QStringList keys;
    if (has_flag(flags, ItemFlag::MissingSource)) {
        keys << QStringLiteral("missing");
    }
    if (has_flag(flags, ItemFlag::ChangedSource)) {
        keys << QStringLiteral("changed");
    }
    if (has_flag(flags, ItemFlag::DuplicateContent)) {
        keys << QStringLiteral("duplicate");
    }
    if (has_flag(flags, ItemFlag::RemovableSource)) {
        keys << QStringLiteral("removable");
    }
    return keys;
}

QString flag_badge_label(const QString& key) {
    if (key == u"missing") {
        return tr_text("Missing");
    }
    if (key == u"changed") {
        return tr_text("Changed");
    }
    if (key == u"duplicate") {
        return tr_text("Duplicate");
    }
    if (key == u"removable") {
        return tr_text("Removable");
    }
    return {};
}

QString flag_badge_detail(const QString& key) {
    if (key == u"missing") {
        return tr_text(
            "The file is not at its last known location. Reconnect its drive, or use Relink to "
            "find it.");
    }
    if (key == u"changed") {
        return tr_text("The file changed on disk after it was added. Lucent reads it again before using it.");
    }
    if (key == u"duplicate") {
        return tr_text(
            "Another file in the library has exactly the same content. Both are kept; pick or "
            "reject each one.");
    }
    if (key == u"removable") {
        return tr_text(
            "The file is on removable media. Keep it connected, or copy it into a library folder "
            "before you remove it.");
    }
    return {};
}

QString pick_key(PickState pick) {
    switch (pick) {
        case PickState::Picked:
            return QStringLiteral("picked");
        case PickState::Rejected:
            return QStringLiteral("rejected");
        case PickState::None:
            break;
    }
    return QStringLiteral("none");
}

QString pick_label(PickState pick) {
    switch (pick) {
        case PickState::Picked:
            return tr_text("Picked");
        case PickState::Rejected:
            return tr_text("Rejected");
        case PickState::None:
            break;
    }
    return {};
}

QString source_note(const LibraryItem& item) {
    QStringList sentences;
    if (has_flag(item.flags, ItemFlag::MissingSource)) {
        sentences << tr_text(
            "The file is not at its last known location. Reconnect its drive, or use "
            "Relink to find it in another folder.");
    } else if (has_flag(item.flags, ItemFlag::ChangedSource)) {
        sentences << tr_text("The file changed on disk after it was added; Lucent reads it again.");
    }
    if (has_flag(item.flags, ItemFlag::RemovableSource)) {
        sentences << tr_text(
            "It is on removable media: keep it connected, or copy it into a library folder "
            "before you remove it.");
    }
    if (item.copied && !item.origin_path.isEmpty()) {
        sentences << tr_text("A verified copy of %1.").arg(item.origin_path);
    }
    if (!item.duplicate_paths.isEmpty()) {
        sentences << tr_text("Same content as %1. Both are kept; your pick decides.")
                         .arg(item.duplicate_paths.join(QStringLiteral(", ")));
    }
    return sentences.join(u' ');
}

QString accessible_label(const LibraryItem& item, const QLocale& locale) {
    QStringList parts{file_name(item), camera_label(item), capture_time_label(item, locale),
                      support_label(item.support)};
    parts << flag_labels(item.flags);
    if (item.thumbnail_generation != kNoImage && item.thumbnail_is_camera_preview) {
        parts << tr_text("camera preview");
    }
    return parts.join(QStringLiteral(", "));
}

QString color_space_label(ExportColorSpace color_space) {
    switch (color_space) {
    case ExportColorSpace::Srgb:
        return tr_text("sRGB");
    case ExportColorSpace::DisplayP3:
        return tr_text("Display P3");
    }
    return {};
}

QString export_options_label(const ExportOptions& options) {
    switch (options.format) {
    case ExportFileFormat::Jpeg:
        return tr_text("JPEG · %1 · quality %2")
            .arg(color_space_label(options.color_space), QString::number(options.jpeg_quality));
    case ExportFileFormat::Tiff16:
        return tr_text("TIFF 16-bit · %1").arg(color_space_label(options.color_space));
    }
    return {};
}

QString render_label(const RenderInfo& info, const QLocale& locale) {
    QString label = info.rendering.isEmpty() ? tr_text("Lucent render") : info.rendering;
    if (info.provisional) {
        label += u' ' + tr_text("(provisional)");
    }
    if (info.render_seconds > 0.0) {
        label += QStringLiteral(" · ") +
                 tr_text("rendered in %1 s").arg(locale.toString(info.render_seconds, 'f', 2));
    }
    return label;
}

QString job_state_key(ExportJobState state) {
    switch (state) {
    case ExportJobState::Queued:
        return QStringLiteral("queued");
    case ExportJobState::Running:
        return QStringLiteral("running");
    case ExportJobState::Completed:
        return QStringLiteral("completed");
    case ExportJobState::Failed:
        return QStringLiteral("failed");
    case ExportJobState::Cancelled:
        return QStringLiteral("cancelled");
    }
    return {};
}

QString job_state_label(ExportJobState state) {
    switch (state) {
    case ExportJobState::Queued:
        return tr_text("Queued");
    case ExportJobState::Running:
        return tr_text("Exporting");
    case ExportJobState::Completed:
        return tr_text("Exported");
    case ExportJobState::Failed:
        return tr_text("Failed");
    case ExportJobState::Cancelled:
        return tr_text("Cancelled");
    }
    return {};
}

}  // namespace lucent::ui::presentation
