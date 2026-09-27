// Test runner of lucent_ui_tests: GoogleTest inside a QGuiApplication.
//
// The tests run on Qt's offscreen platform with Qt Quick's software scene graph, so they need no
// display or GPU and behave the same on every machine. Dates and numbers use the en_US locale.

#include <gtest/gtest.h>

#include <QGuiApplication>
#include <QLocale>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QtQml/QQmlExtensionPlugin>

#include "shell.hpp"

Q_IMPORT_QML_PLUGIN(LucentPlugin)

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    // The offscreen platform renders text with FreeType from a font directory, which Qt does not
    // ship; use the system's fonts so text renders and Qt does not warn about the missing folder.
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR") && !qEnvironmentVariableIsEmpty("WINDIR")) {
        qputenv("QT_QPA_FONTDIR", (qEnvironmentVariable("WINDIR") + QStringLiteral("/Fonts")).toLocal8Bit());
    }
    ::testing::InitGoogleTest(&argc, argv);
    lucent::ui::configure_application();
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    QGuiApplication app(argc, argv);
    return RUN_ALL_TESTS();
}
