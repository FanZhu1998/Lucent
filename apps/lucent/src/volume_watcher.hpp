#pragma once

// VolumeWatcher: notices volumes that arrive or leave (a memory card inserted into or removed from
// a reader, a USB drive plugged in or out), so the library can check its files again while Lucent
// runs: photos on a removed card become Missing, and Present again when the card returns
// (blueprint section 2: removable sources stay visibly identified, missing files remain with a
// relink action; ADR 0008).
//
// Windows broadcasts WM_DEVICECHANGE with DBT_DEVICEARRIVAL or DBT_DEVICEREMOVECOMPLETE and a
// DBT_DEVTYP_VOLUME header to every top-level window. Installed as the application's native event
// filter (QCoreApplication::installNativeEventFilter), the watcher sees those that reach Lucent's
// windows; it never consumes a message. Changes within `settle` of each other are merged into one
// volumes_changed() (a card can mount several volumes, a reader reports removal and arrival in
// quick succession), emitted `settle` after the last one.
//
// Lives on the UI thread; Qt calls nativeEventFilter() there.

#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QObject>
#include <QTimer>
#include <chrono>
#include <cstdint>

namespace lucent::ui {

class VolumeWatcher final : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT

public:
    explicit VolumeWatcher(std::chrono::milliseconds settle = std::chrono::milliseconds(1000),
                           QObject* parent = nullptr);

    // Restarts the settle timer when `message` (a Windows MSG) announces a volume change. Always
    // returns false: the message is processed as usual.
    bool nativeEventFilter(const QByteArray& event_type, void* message, qintptr* result) override;

    // True when the window message (message, wParam, lParam) announces a volume that arrived or
    // left: WM_DEVICECHANGE, DBT_DEVICEARRIVAL or DBT_DEVICEREMOVECOMPLETE, and lParam pointing to
    // a DEV_BROADCAST_HDR of type DBT_DEVTYP_VOLUME.
    [[nodiscard]] static bool is_volume_change(std::uint32_t message, std::uintptr_t wparam,
                                               std::intptr_t lparam) noexcept;

signals:
    void volumes_changed();

private:
    QTimer settle_;
};

}  // namespace lucent::ui
