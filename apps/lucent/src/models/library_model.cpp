#include "models/library_model.hpp"

#include <algorithm>
#include <utility>

#include <QSet>

#include "imaging/image_id.hpp"
#include "models/presentation.hpp"

namespace lucent::ui {

LibraryModel::LibraryModel(LibraryService& service, std::shared_ptr<GenerationRegistry> registry,
                           QObject* parent)
    : QAbstractListModel(parent), registry_(std::move(registry)) {
    connect(&service, &LibraryService::items_added, this, &LibraryModel::add_items);
    connect(&service, &LibraryService::item_changed, this, &LibraryModel::update_item);
    add_items(service.current_items());
}

namespace {

bool is_missing(const LibraryItem& item) {
    return has_flag(item.flags, ItemFlag::MissingSource);
}

}  // namespace

QList<ItemId> LibraryModel::missing_items() const {
    QList<ItemId> ids;
    for (const LibraryItem& item : items_) {
        if (is_missing(item)) {
            ids << item.id;
        }
    }
    return ids;
}

QList<ItemId> LibraryModel::selected_items() const {
    QList<ItemId> ids;
    for (const LibraryItem& item : items_) {
        if (selected_.contains(item.id)) {
            ids << item.id;
        }
    }
    return ids;
}

QList<ItemId> LibraryModel::all_items() const {
    QList<ItemId> ids;
    ids.reserve(static_cast<qsizetype>(items_.size()));
    for (const LibraryItem& item : items_) {
        ids << item.id;
    }
    return ids;
}

void LibraryModel::set_selection(QSet<ItemId> selection) {
    if (selection == selected_) {
        return;
    }
    const QSet<ItemId> before = std::exchange(selected_, std::move(selection));
    // Only the rows whose state changed.
    for (std::size_t row = 0; row < items_.size(); ++row) {
        const ItemId id = items_[row].id;
        if (before.contains(id) != selected_.contains(id)) {
            const QModelIndex changed = index(static_cast<int>(row));
            emit dataChanged(changed, changed, {InSelectionRole});
        }
    }
    emit selectionChanged();
}

void LibraryModel::selectOnly(int row) {
    const LibraryItem* item = item_at(row);
    if (item == nullptr) {
        return;
    }
    anchor_ = item->id;
    set_selection({item->id});
}

void LibraryModel::toggleSelected(int row) {
    const LibraryItem* item = item_at(row);
    if (item == nullptr) {
        return;
    }
    anchor_ = item->id;
    QSet<ItemId> selection = selected_;
    if (!selection.remove(item->id)) {
        selection.insert(item->id);
    }
    set_selection(std::move(selection));
}

void LibraryModel::selectRange(int row) {
    const LibraryItem* item = item_at(row);
    if (item == nullptr) {
        return;
    }
    const std::optional<int> anchor = anchor_ != 0 ? row_of(anchor_) : std::nullopt;
    if (!anchor) {
        selectOnly(row);
        return;
    }
    QSet<ItemId> selection;
    for (int r = std::min(*anchor, row); r <= std::max(*anchor, row); ++r) {
        selection.insert(items_[static_cast<std::size_t>(r)].id);
    }
    set_selection(std::move(selection));
}

void LibraryModel::selectAll() {
    QSet<ItemId> selection;
    for (const LibraryItem& item : items_) {
        selection.insert(item.id);
    }
    set_selection(std::move(selection));
}

void LibraryModel::clearSelection() {
    anchor_ = 0;
    set_selection({});
}

void LibraryModel::set_missing(int missing) {
    if (missing != missing_) {
        missing_ = missing;
        emit missingCountChanged();
    }
}

int LibraryModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : count();
}

QVariant LibraryModel::data(const QModelIndex& index, int role) const {
    const LibraryItem* item = index.isValid() ? item_at(index.row()) : nullptr;
    if (item == nullptr) {
        return {};
    }
    namespace p = presentation;
    switch (role) {
    case Qt::DisplayRole:
    case FileNameRole:
        return p::file_name(*item);
    case FilePathRole:
        return item->file_path;
    case FolderRole:
        return p::folder_path(*item);
    case CameraLabelRole:
        return p::camera_label(*item);
    case LensLabelRole:
        return p::lens_label(*item);
    case CaptureTimeRole:
        return item->capture_time;
    case CaptureTimeLabelRole:
        return p::capture_time_label(*item, locale_);
    case DimensionsLabelRole:
        return p::dimensions_label(*item, locale_);
    case ExposureLabelRole:
        return p::exposure_label(*item, locale_);
    case FileSizeLabelRole:
        return p::file_size_label(item->file_size_bytes, locale_);
    case PixelWidthRole:
        return item->pixel_width;
    case PixelHeightRole:
        return item->pixel_height;
    case SupportKeyRole:
        return p::support_key(item->support);
    case SupportLabelRole:
        return p::support_label(item->support);
    case SupportShortLabelRole:
        return p::support_short_label(item->support);
    case SupportDetailRole:
        return item->support_detail;
    case ProcessingLabelRole:
        return p::processing_label(*item);
    case ProcessingDetailRole:
        return p::processing_detail(*item);
    case ProcessingKeyRole:
        return p::processing_key(*item);
    case RenderIncompleteRole:
        return p::render_incomplete(*item);
    case FlagLabelsRole:
        return p::flag_labels(item->flags);
    case ThumbnailSourceRole:
        return image_url(ImageKind::Thumbnail, item->id, item->thumbnail_generation);
    case ThumbnailIsCameraPreviewRole:
        return item->thumbnail_generation != kNoImage && item->thumbnail_is_camera_preview;
    case PreviewSourceRole:
        return image_url(ImageKind::Preview, item->id, item->preview_generation);
    case AccessibleLabelRole:
        return p::accessible_label(*item, locale_);
    case HasRenderRole:
        return item->render != nullptr;
    case RenderInfoRole:
        return QVariant::fromValue(item->render);
    case RenderLabelRole:
        return item->render ? p::render_label(*item->render, locale_) : QString();
    case RenderNotesRole:
        return item->render ? item->render->notes : QStringList();
    case FlagKeysRole:
        return p::flag_keys(item->flags);
    case PickKeyRole:
        return p::pick_key(item->pick);
    case PickLabelRole:
        return p::pick_label(item->pick);
    case SourceNoteRole:
        return p::source_note(*item);
    case OriginPathRole:
        return item->origin_path;
    case DuplicatePathsRole:
        return item->duplicate_paths;
    case MissingRole:
        return is_missing(*item);
    case EditedRole:
        return item->edited;
    case InSelectionRole:
        return selected_.contains(item->id);
    case FlagBadgesRole: {
        QVariantList badges;
        for (const QString& key : p::flag_keys(item->flags)) {
            badges << QVariantMap{{QStringLiteral("key"), key},
                                  {QStringLiteral("label"), p::flag_badge_label(key)},
                                  {QStringLiteral("detail"), p::flag_badge_detail(key)}};
        }
        return badges;
    }
    default:
        return {};
    }
}

QHash<int, QByteArray> LibraryModel::roleNames() const {
    return {
        {FileNameRole, "fileName"},
        {FilePathRole, "filePath"},
        {FolderRole, "folder"},
        {CameraLabelRole, "cameraLabel"},
        {LensLabelRole, "lensLabel"},
        {CaptureTimeRole, "captureTime"},
        {CaptureTimeLabelRole, "captureTimeLabel"},
        {DimensionsLabelRole, "dimensionsLabel"},
        {ExposureLabelRole, "exposureLabel"},
        {FileSizeLabelRole, "fileSizeLabel"},
        {PixelWidthRole, "pixelWidth"},
        {PixelHeightRole, "pixelHeight"},
        {SupportKeyRole, "supportKey"},
        {SupportLabelRole, "supportLabel"},
        {SupportShortLabelRole, "supportShortLabel"},
        {SupportDetailRole, "supportDetail"},
        {ProcessingLabelRole, "processingLabel"},
        {ProcessingDetailRole, "processingDetail"},
        {FlagLabelsRole, "flagLabels"},
        {ThumbnailSourceRole, "thumbnailSource"},
        {ThumbnailIsCameraPreviewRole, "thumbnailIsCameraPreview"},
        {PreviewSourceRole, "previewSource"},
        {AccessibleLabelRole, "accessibleLabel"},
        {HasRenderRole, "hasRender"},
        {RenderInfoRole, "renderInfo"},
        {RenderLabelRole, "renderLabel"},
        {RenderNotesRole, "renderNotes"},
        {ProcessingKeyRole, "processingKey"},
        {RenderIncompleteRole, "renderIncomplete"},
        {FlagKeysRole, "flagKeys"},
        {PickKeyRole, "pickKey"},
        {PickLabelRole, "pickLabel"},
        {SourceNoteRole, "sourceNote"},
        {OriginPathRole, "originPath"},
        {DuplicatePathsRole, "duplicatePaths"},
        {MissingRole, "missing"},
        {FlagBadgesRole, "flagBadges"},
        {EditedRole, "edited"},
        {InSelectionRole, "inSelection"},
    };
}

const LibraryItem* LibraryModel::item_at(int row) const noexcept {
    if (row < 0 || row >= count()) {
        return nullptr;
    }
    return &items_[static_cast<std::size_t>(row)];
}

std::optional<int> LibraryModel::row_of(ItemId id) const {
    const auto found = rows_.constFind(id);
    if (found == rows_.cend()) {
        return std::nullopt;
    }
    return *found;
}

void LibraryModel::set_locale(const QLocale& locale) {
    if (locale == locale_) {
        return;
    }
    locale_ = locale;
    if (!items_.empty()) {
        emit dataChanged(index(0), index(count() - 1));
    }
}

void LibraryModel::record_generations(const LibraryItem& item) {
    registry_->set(item.id, item.thumbnail_generation, item.preview_generation);
}

void LibraryModel::add_items(const QList<LibraryItem>& items) {
    // Ids already present are updates (a service should not send them twice, but a duplicate must
    // never create a second row with the same identity).
    QList<LibraryItem> fresh;
    fresh.reserve(items.size());
    QSet<ItemId> seen;
    for (const LibraryItem& item : items) {
        if (item.id == 0) {
            continue;
        }
        if (rows_.contains(item.id)) {
            update_item(item);
        } else if (!seen.contains(item.id)) {
            seen.insert(item.id);
            fresh.push_back(item);
        }
    }
    if (fresh.isEmpty()) {
        return;
    }
    for (const LibraryItem& item : fresh) {
        record_generations(item);
    }
    const int first = count();
    int missing = missing_;
    beginInsertRows({}, first, first + static_cast<int>(fresh.size()) - 1);
    for (const LibraryItem& item : fresh) {
        rows_.insert(item.id, count());
        items_.push_back(item);
        missing += is_missing(item) ? 1 : 0;
    }
    endInsertRows();
    emit countChanged();
    set_missing(missing);
}

void LibraryModel::update_item(const LibraryItem& item) {
    const auto row = row_of(item.id);
    if (!row) {
        return;  // not (yet) in the library; nothing to show
    }
    record_generations(item);
    LibraryItem& slot = items_[static_cast<std::size_t>(*row)];
    const int missing = missing_ - (is_missing(slot) ? 1 : 0) + (is_missing(item) ? 1 : 0);
    slot = item;
    const QModelIndex changed = index(*row);
    emit dataChanged(changed, changed);
    set_missing(missing);
}

}  // namespace lucent::ui
