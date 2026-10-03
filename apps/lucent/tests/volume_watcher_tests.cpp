// VolumeWatcher: which Windows messages count as a volume arriving or leaving, the merging of a
// burst into one signal, and that messages are never consumed.

#include <QSignalSpy>
#include <QTest>
#include <chrono>
#include <cstdint>

#include <gtest/gtest.h>

// clang-format off
#include <windows.h>
#include <dbt.h>  // needs <windows.h> first
// clang-format on

#include "volume_watcher.hpp"

namespace lucent::ui {
namespace {

using namespace std::chrono_literals;

DEV_BROADCAST_VOLUME volume_header() {
    DEV_BROADCAST_VOLUME volume{};
    volume.dbcv_size = sizeof(volume);
    volume.dbcv_devicetype = DBT_DEVTYP_VOLUME;
    volume.dbcv_unitmask = 1u << 4;  // E:
    return volume;
}

std::intptr_t as_lparam(const void* header) {
    return reinterpret_cast<std::intptr_t>(header);
}

TEST(VolumeWatcher, RecognisesVolumesArrivingAndLeavingOnly) {
    const DEV_BROADCAST_VOLUME volume = volume_header();
    DEV_BROADCAST_HDR port{};
    port.dbch_size = sizeof(port);
    port.dbch_devicetype = DBT_DEVTYP_PORT;
    EXPECT_TRUE(VolumeWatcher::is_volume_change(WM_DEVICECHANGE, DBT_DEVICEARRIVAL, as_lparam(&volume)));
    EXPECT_TRUE(
        VolumeWatcher::is_volume_change(WM_DEVICECHANGE, DBT_DEVICEREMOVECOMPLETE, as_lparam(&volume)));
    EXPECT_FALSE(VolumeWatcher::is_volume_change(WM_DEVICECHANGE, DBT_DEVICEQUERYREMOVE, as_lparam(&volume)));
    EXPECT_FALSE(VolumeWatcher::is_volume_change(WM_DEVICECHANGE, DBT_DEVICEARRIVAL, as_lparam(&port)));
    EXPECT_FALSE(VolumeWatcher::is_volume_change(WM_DEVICECHANGE, DBT_DEVNODES_CHANGED, 0));
    EXPECT_FALSE(VolumeWatcher::is_volume_change(WM_DEVICECHANGE, DBT_DEVICEARRIVAL, 0));
    EXPECT_FALSE(VolumeWatcher::is_volume_change(WM_PAINT, DBT_DEVICEARRIVAL, as_lparam(&volume)));
}

TEST(VolumeWatcher, MergesABurstIntoOneSignalAndNeverConsumesMessages) {
    VolumeWatcher watcher(20ms);
    QSignalSpy changed(&watcher, &VolumeWatcher::volumes_changed);
    const DEV_BROADCAST_VOLUME volume = volume_header();
    MSG removal{};
    removal.message = WM_DEVICECHANGE;
    removal.wParam = DBT_DEVICEREMOVECOMPLETE;
    removal.lParam = as_lparam(&volume);
    MSG arrival = removal;
    arrival.wParam = DBT_DEVICEARRIVAL;
    qintptr result = 0;
    EXPECT_FALSE(watcher.nativeEventFilter(QByteArrayLiteral("windows_generic_MSG"), &removal, &result));
    EXPECT_FALSE(watcher.nativeEventFilter(QByteArrayLiteral("windows_generic_MSG"), &arrival, &result));
    ASSERT_TRUE(changed.wait(5000));
    QTest::qWait(100);
    EXPECT_EQ(changed.count(), 1);

    // Other messages and event types do nothing.
    MSG paint{};
    paint.message = WM_PAINT;
    EXPECT_FALSE(watcher.nativeEventFilter(QByteArrayLiteral("windows_generic_MSG"), &paint, &result));
    EXPECT_FALSE(watcher.nativeEventFilter(QByteArrayLiteral("windows_dispatcher_MSG"), &arrival, &result));
    EXPECT_FALSE(watcher.nativeEventFilter(QByteArrayLiteral("windows_generic_MSG"), nullptr, &result));
    QTest::qWait(100);
    EXPECT_EQ(changed.count(), 1);
}

}  // namespace
}  // namespace lucent::ui
