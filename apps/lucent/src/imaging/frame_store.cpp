#include "imaging/frame_store.hpp"

#include <QQmlEngine>

#include "imaging/image_id.hpp"

namespace lucent::ui {

QString FrameStore::provider_id(Kind kind) {
    switch (kind) {
    case Kind::Frame:
        return QStringLiteral("frame");
    case Kind::Before:
        return QStringLiteral("before");
    case Kind::Clipping:
        return QStringLiteral("clipping");
    }
    return {};
}

QString FrameStore::url(Kind kind, ItemId item, quint64 sequence) {
    const QString id = image_id(item, sequence);
    if (id.isEmpty()) {
        return {};
    }
    return QStringLiteral("image://") + provider_id(kind) + u'/' + id;
}

FrameStore::Slot& FrameStore::slot(Entry& entry, Kind kind) {
    switch (kind) {
    case Kind::Before:
        return entry.before;
    case Kind::Clipping:
        return entry.clipping;
    case Kind::Frame:
        break;
    }
    return entry.frame;
}

const FrameStore::Slot& FrameStore::slot(const Entry& entry, Kind kind) {
    switch (kind) {
    case Kind::Before:
        return entry.before;
    case Kind::Clipping:
        return entry.clipping;
    case Kind::Frame:
        break;
    }
    return entry.frame;
}

void FrameStore::put(Kind kind, ItemId item, quint64 sequence, QImage image) {
    if (item == 0 || sequence == 0) {
        return;
    }
    const std::lock_guard lock(mutex_);
    Slot& target = slot(entries_[item], kind);
    if (sequence < target.sequence) {
        return;
    }
    target.sequence = sequence;
    target.image = std::move(image);
}

QImage FrameStore::find(Kind kind, ItemId item, quint64 sequence) const {
    const std::lock_guard lock(mutex_);
    const auto found = entries_.find(item);
    if (found == entries_.end()) {
        return {};
    }
    const Slot& kept = slot(found->second, kind);
    return kept.sequence >= sequence ? kept.image : QImage();
}

void FrameStore::remove(ItemId item) {
    const std::lock_guard lock(mutex_);
    entries_.erase(item);
}

void FrameStore::drop(Kind kind, ItemId item) {
    const std::lock_guard lock(mutex_);
    if (const auto found = entries_.find(item); found != entries_.end()) {
        slot(found->second, kind).image = QImage();
    }
}

void FrameStore::clear() {
    const std::lock_guard lock(mutex_);
    entries_.clear();
}

FrameImageProvider::FrameImageProvider(FrameStore::Kind kind, std::shared_ptr<const FrameStore> store)
    : QQuickImageProvider(QQuickImageProvider::Image, QQmlImageProviderBase::ForceAsynchronousImageLoading),
      kind_(kind),
      store_(std::move(store)) {}

QImage FrameImageProvider::requestImage(const QString& id, QSize* size, const QSize& requested_size) {
    Q_UNUSED(requested_size);  // the pixels are scaled on the GPU (the Image has no sourceSize)
    QImage image;
    if (const std::optional<ImageKey> key = parse_image_id(id); key) {
        image = store_->find(kind_, key->item, key->generation);
    }
    if (size != nullptr) {
        *size = image.size();
    }
    return image;
}

void install_frame_providers(QQmlEngine& engine, const std::shared_ptr<const FrameStore>& store) {
    for (const FrameStore::Kind kind : {FrameStore::Kind::Frame, FrameStore::Kind::Before, FrameStore::Kind::Clipping}) {
        engine.addImageProvider(FrameStore::provider_id(kind), new FrameImageProvider(kind, store));
    }
}

}  // namespace lucent::ui
