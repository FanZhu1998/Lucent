#include "volume_watcher.hpp"

// clang-format off
#include <windows.h>
#include <dbt.h>  // needs <windows.h> first
// clang-format on

namespace lucent::ui {

VolumeWatcher::VolumeWatcher(std::chrono::milliseconds settle, QObject* parent) : QObject(parent) {
    settle_.setSingleShot(true);
    settle_.setInterval(settle);
    connect(&settle_, &QTimer::timeout, this, &VolumeWatcher::volumes_changed);
}

bool VolumeWatcher::nativeEventFilter(const QByteArray& event_type, void* message, qintptr* result) {
    Q_UNUSED(result);
    if (message == nullptr || event_type != QByteArrayLiteral("windows_generic_MSG")) {
        return false;
    }
    const MSG* msg = static_cast<const MSG*>(message);
    if (is_volume_change(msg->message, static_cast<std::uintptr_t>(msg->wParam),
                         static_cast<std::intptr_t>(msg->lParam))) {
        settle_.start();  // restarts a running timer: a burst becomes one signal
    }
    return false;
}

bool VolumeWatcher::is_volume_change(std::uint32_t message, std::uintptr_t wparam,
                                     std::intptr_t lparam) noexcept {
    if (message != WM_DEVICECHANGE || (wparam != DBT_DEVICEARRIVAL && wparam != DBT_DEVICEREMOVECOMPLETE) ||
        lparam == 0) {
        return false;
    }
    const auto* header = reinterpret_cast<const DEV_BROADCAST_HDR*>(lparam);
    return header->dbch_devicetype == DBT_DEVTYP_VOLUME;
}

}  // namespace lucent::ui
