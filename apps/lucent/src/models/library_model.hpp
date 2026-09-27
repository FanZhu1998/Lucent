#pragma once

// LibraryModel: the contact sheet and filmstrip model. It mirrors the LibraryService's items in the
// order they were added and keeps the GenerationRegistry in step with them: the registry is
// updated before rows are inserted or dataChanged is emitted, so an image URL the model hands out
// is always current in the registry.
//
// QML-facing members use QML naming (lowerCamelCase); C++-only members use snake_case.
//
// Thread-safety: UI thread only (like every QAbstractItemModel). The registry it writes is
// thread-safe.

#include <memory>
#include <optional>
#include <vector>

#include <QAbstractListModel>
#include <QHash>
#include <QLocale>
#include <QtQmlIntegration/qqmlintegration.h>

#include "imaging/generation_registry.hpp"
#include "services/library_service.hpp"

namespace lucent::ui {

class LibraryModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("LibraryModel is provided by AppController.library.")
    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)

public:
    enum Role {
        FileNameRole = Qt::UserRole + 1,
        FilePathRole,
        FolderRole,
        CameraLabelRole,
        LensLabelRole,
        CaptureTimeRole,       // QDateTime (invalid when unknown)
        CaptureTimeLabelRole,
        DimensionsLabelRole,
        ExposureLabelRole,
        FileSizeLabelRole,
        PixelWidthRole,
        PixelHeightRole,
        SupportKeyRole,        // presentation::support_key
        SupportLabelRole,
        SupportShortLabelRole,
        SupportDetailRole,
        ProcessingLabelRole,
        ProcessingDetailRole,
        FlagLabelsRole,        // QStringList
        ThumbnailSourceRole,   // image URL or ""
        ThumbnailIsCameraPreviewRole,
        PreviewSourceRole,     // image URL or ""
        AccessibleLabelRole,
        HasRenderRole,         // Lucent has rendered the item (RenderInfo available)
        RenderInfoRole,        // QVariant holding a RenderInfoPtr (null when none)
        RenderLabelRole,       // presentation::render_label; empty when none
        RenderNotesRole,       // QStringList
        ProcessingKeyRole,     // presentation::processing_key
        RenderIncompleteRole,  // presentation::render_incomplete
    };
    Q_ENUM(Role)

    // `service` must outlive the model. `registry` must not be written by anyone else.
    LibraryModel(LibraryService& service, std::shared_ptr<GenerationRegistry> registry,
                 QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] int count() const noexcept { return static_cast<int>(items_.size()); }

    // The item at `row`, or nullptr when out of range. Valid until the model next changes.
    [[nodiscard]] const LibraryItem* item_at(int row) const noexcept;
    [[nodiscard]] std::optional<int> row_of(ItemId id) const;

    // Locale used for dates and numbers (default: QLocale()).
    void set_locale(const QLocale& locale);

signals:
    void countChanged();

private:
    void add_items(const QList<LibraryItem>& items);
    void update_item(const LibraryItem& item);
    void record_generations(const LibraryItem& item);

    std::shared_ptr<GenerationRegistry> registry_;
    std::vector<LibraryItem> items_;
    QHash<ItemId, int> rows_;
    QLocale locale_;
};

}  // namespace lucent::ui
