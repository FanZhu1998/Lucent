#pragma once

// Start-up of the Qt Quick shell, shared by Lucent.exe (main.cpp) and the UI tests.

class QQmlApplicationEngine;
class QQuickWindow;

namespace lucent::ui {

class AppController;

// Process-wide settings: application identity (name, organisation, version; used by QSettings
// and window titles), high-DPI rounding (pass-through, so 125 % and 150 % displays get exact
// scale factors), and the Qt Quick Controls style (Fusion, themed dark by Main.qml). Call once,
// before constructing QGuiApplication.
void configure_application();

// Registers the controller's image providers with `engine` and creates the window from the
// "Main" component of the Lucent QML module with `controller` as its required property. Returns
// the window, or nullptr when loading failed (the engine reports the QML errors as warnings).
// `controller` must outlive `engine`.
[[nodiscard]] QQuickWindow* load_main_window(QQmlApplicationEngine& engine, AppController& controller);

}  // namespace lucent::ui
