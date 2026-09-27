#pragma once

// CurrentItem: the photo shown in the Edit view, as bindable QML properties. It follows its item
// by identity, so the properties stay correct when the item changes (a new thumbnail or preview
// generation, a support status) or when rows move. All properties notify through changed().
//
// Thread-safety: UI thread only.

#include <QObject>
#include <QStringList>
#include <QVariant>
#include <QtQmlIntegration/qqmlintegration.h>

#include "models/library_model.hpp"

namespace lucent::ui {

class CurrentItem : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("CurrentItem is provided by AppController.current.")
    Q_PROPERTY(bool valid READ valid NOTIFY changed FINAL)
    Q_PROPERTY(int row READ row NOTIFY changed FINAL)
    Q_PROPERTY(QString fileName READ file_name NOTIFY changed FINAL)
    Q_PROPERTY(QString filePath READ file_path NOTIFY changed FINAL)
    Q_PROPERTY(QString folder READ folder NOTIFY changed FINAL)
    Q_PROPERTY(QString cameraLabel READ camera_label NOTIFY changed FINAL)
    Q_PROPERTY(QString lensLabel READ lens_label NOTIFY changed FINAL)
    Q_PROPERTY(QString captureTimeLabel READ capture_time_label NOTIFY changed FINAL)
    Q_PROPERTY(QString dimensionsLabel READ dimensions_label NOTIFY changed FINAL)
    Q_PROPERTY(QString exposureLabel READ exposure_label NOTIFY changed FINAL)
    Q_PROPERTY(QString fileSizeLabel READ file_size_label NOTIFY changed FINAL)
    Q_PROPERTY(int pixelWidth READ pixel_width NOTIFY changed FINAL)
    Q_PROPERTY(int pixelHeight READ pixel_height NOTIFY changed FINAL)
    Q_PROPERTY(QString supportKey READ support_key NOTIFY changed FINAL)
    Q_PROPERTY(QString supportLabel READ support_label NOTIFY changed FINAL)
    Q_PROPERTY(QString supportDetail READ support_detail NOTIFY changed FINAL)
    Q_PROPERTY(QString processingLabel READ processing_label NOTIFY changed FINAL)
    Q_PROPERTY(QString processingDetail READ processing_detail NOTIFY changed FINAL)
    Q_PROPERTY(QString processingKey READ processing_key NOTIFY changed FINAL)
    // Lucent's latest render lacks a mandatory interpretation step (not an accurate result).
    Q_PROPERTY(bool renderIncomplete READ render_incomplete NOTIFY changed FINAL)
    Q_PROPERTY(QStringList flagLabels READ flag_labels NOTIFY changed FINAL)
    Q_PROPERTY(QString thumbnailSource READ thumbnail_source NOTIFY changed FINAL)
    Q_PROPERTY(bool thumbnailIsCameraPreview READ thumbnail_is_camera_preview NOTIFY changed FINAL)
    Q_PROPERTY(QString previewSource READ preview_source NOTIFY changed FINAL)
    Q_PROPERTY(bool hasRender READ has_render NOTIFY changed FINAL)
    // RenderInfoPtr for HistogramPlot.histogram; opaque to QML.
    Q_PROPERTY(QVariant renderInfo READ render_info NOTIFY changed FINAL)
    Q_PROPERTY(QString renderLabel READ render_label NOTIFY changed FINAL)
    Q_PROPERTY(QStringList renderNotes READ render_notes NOTIFY changed FINAL)

public:
    // `model` must outlive this object.
    explicit CurrentItem(const LibraryModel& model, QObject* parent = nullptr);

    // Follows the item at `row` (none when out of range). Emits changed() when that differs.
    void set_row(int row);

    [[nodiscard]] bool valid() const noexcept { return row_ >= 0; }
    [[nodiscard]] int row() const noexcept { return row_; }
    [[nodiscard]] ItemId item_id() const noexcept { return id_; }

    [[nodiscard]] QString file_name() const { return text(LibraryModel::FileNameRole); }
    [[nodiscard]] QString file_path() const { return text(LibraryModel::FilePathRole); }
    [[nodiscard]] QString folder() const { return text(LibraryModel::FolderRole); }
    [[nodiscard]] QString camera_label() const { return text(LibraryModel::CameraLabelRole); }
    [[nodiscard]] QString lens_label() const { return text(LibraryModel::LensLabelRole); }
    [[nodiscard]] QString capture_time_label() const { return text(LibraryModel::CaptureTimeLabelRole); }
    [[nodiscard]] QString dimensions_label() const { return text(LibraryModel::DimensionsLabelRole); }
    [[nodiscard]] QString exposure_label() const { return text(LibraryModel::ExposureLabelRole); }
    [[nodiscard]] QString file_size_label() const { return text(LibraryModel::FileSizeLabelRole); }
    [[nodiscard]] int pixel_width() const { return value(LibraryModel::PixelWidthRole).toInt(); }
    [[nodiscard]] int pixel_height() const { return value(LibraryModel::PixelHeightRole).toInt(); }
    [[nodiscard]] QString support_key() const { return text(LibraryModel::SupportKeyRole); }
    [[nodiscard]] QString support_label() const { return text(LibraryModel::SupportLabelRole); }
    [[nodiscard]] QString support_detail() const { return text(LibraryModel::SupportDetailRole); }
    [[nodiscard]] QString processing_label() const { return text(LibraryModel::ProcessingLabelRole); }
    [[nodiscard]] QString processing_detail() const { return text(LibraryModel::ProcessingDetailRole); }
    [[nodiscard]] QString processing_key() const { return text(LibraryModel::ProcessingKeyRole); }
    [[nodiscard]] bool render_incomplete() const { return value(LibraryModel::RenderIncompleteRole).toBool(); }
    [[nodiscard]] QStringList flag_labels() const { return value(LibraryModel::FlagLabelsRole).toStringList(); }
    [[nodiscard]] QString thumbnail_source() const { return text(LibraryModel::ThumbnailSourceRole); }
    [[nodiscard]] bool thumbnail_is_camera_preview() const {
        return value(LibraryModel::ThumbnailIsCameraPreviewRole).toBool();
    }
    [[nodiscard]] QString preview_source() const { return text(LibraryModel::PreviewSourceRole); }
    [[nodiscard]] bool has_render() const { return value(LibraryModel::HasRenderRole).toBool(); }
    [[nodiscard]] QVariant render_info() const { return value(LibraryModel::RenderInfoRole); }
    [[nodiscard]] QString render_label() const { return text(LibraryModel::RenderLabelRole); }
    [[nodiscard]] QStringList render_notes() const {
        return value(LibraryModel::RenderNotesRole).toStringList();
    }

signals:
    void changed();

private:
    [[nodiscard]] QVariant value(int role) const;
    [[nodiscard]] QString text(int role) const { return value(role).toString(); }
    void relocate();

    const LibraryModel& model_;
    int row_ = -1;
    ItemId id_ = 0;
};

}  // namespace lucent::ui
