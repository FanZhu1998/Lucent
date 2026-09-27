#include "shell.hpp"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QVariantMap>

#include "app_controller.hpp"

#ifndef LUCENT_APP_VERSION
#error "LUCENT_APP_VERSION must be defined by the build (apps/lucent/CMakeLists.txt)"
#endif

namespace lucent::ui {

void configure_application() {
    QCoreApplication::setOrganizationName(QStringLiteral("Lucent"));
    QCoreApplication::setApplicationName(QStringLiteral("Lucent"));
    QCoreApplication::setApplicationVersion(QStringLiteral(LUCENT_APP_VERSION));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Lucent"));
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QQuickStyle::setStyle(QStringLiteral("Fusion"));
}

QQuickWindow* load_main_window(QQmlApplicationEngine& engine, AppController& controller) {
    controller.install_image_providers(engine);
    engine.setInitialProperties({{QStringLiteral("controller"), QVariant::fromValue(&controller)}});
    engine.loadFromModule("Lucent", "Main");
    const QList<QObject*> roots = engine.rootObjects();
    if (roots.isEmpty()) {
        return nullptr;
    }
    return qobject_cast<QQuickWindow*>(roots.front());
}

}  // namespace lucent::ui
