#pragma once

// FrameStore: the frames of the photo being edited (EditService::frame_ready) for Qt Quick, and
// the image provider that serves them (blueprint section 4: interactive previews never wait on the
// UI thread; ADR 0010, "quick, then refine").
//
// URLs: image://frame/<item>/<sequence> (the current settings), image://before/<item>/<sequence>
// (the as-shot comparison) and image://clipping/<item>/<sequence> (the clipping overlay of the
// frame with that sequence). Sequences increase per service, so every frame is a new URL and Qt
// Quick never shows an older frame for a newer URL.
//
// Only the newest image of each kind is kept per item (a 100 % frame of a 45 MP photo is about
// 180 MB). A request for an older sequence, which Qt Quick may still be loading when a newer frame
// arrives, gets the newest image of its kind: frames only ever move forward, never back.
//
// Thread-safety: put() and remove() on the UI thread; find() from Qt Quick's image-loader thread;
// all members are safe to call concurrently.

#include <memory>
#include <mutex>
#include <unordered_map>

#include <QImage>
#include <QQuickImageProvider>
#include <QString>

#include "services/service_types.hpp"

class QQmlEngine;

namespace lucent::ui {

class FrameStore {
public:
    enum class Kind { Frame, Before, Clipping };

    // Provider ids: "frame", "before", "clipping".
    [[nodiscard]] static QString provider_id(Kind kind);
    // "image://<provider>/<item>/<sequence>"; empty for item or sequence 0.
    [[nodiscard]] static QString url(Kind kind, ItemId item, quint64 sequence);

    // Keeps `image` as the newest of `kind` for `item` (an older sequence than the kept one is
    // ignored).
    void put(Kind kind, ItemId item, quint64 sequence, QImage image);
    // The image of `sequence`, or the newest of its kind when that is newer; null otherwise.
    [[nodiscard]] QImage find(Kind kind, ItemId item, quint64 sequence) const;
    void remove(ItemId item);
    // Drops the kept image of `kind` for `item` (its sequence stays: older ones remain refused).
    void drop(Kind kind, ItemId item);
    void clear();

private:
    struct Slot {
        quint64 sequence = 0;
        QImage image;
    };
    struct Entry {
        Slot frame;
        Slot before;
        Slot clipping;
    };

    static Slot& slot(Entry& entry, Kind kind);
    static const Slot& slot(const Entry& entry, Kind kind);

    mutable std::mutex mutex_;
    // Looked up by key only, never iterated for output.
    std::unordered_map<ItemId, Entry> entries_;
};

// Serves one kind of FrameStore image (synchronously, on the image-loader thread: the pixels are
// already in memory).
class FrameImageProvider final : public QQuickImageProvider {
public:
    FrameImageProvider(FrameStore::Kind kind, std::shared_ptr<const FrameStore> store);

    QImage requestImage(const QString& id, QSize* size, const QSize& requested_size) override;

private:
    FrameStore::Kind kind_;
    std::shared_ptr<const FrameStore> store_;
};

// Registers the frame, before and clipping providers with `engine` (which takes ownership).
void install_frame_providers(QQmlEngine& engine, const std::shared_ptr<const FrameStore>& store);

}  // namespace lucent::ui
