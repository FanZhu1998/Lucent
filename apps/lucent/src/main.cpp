// Lucent.exe: the Qt Quick desktop application (blueprint sections 2, 4 and 9).
//
// A WIN32-subsystem program: launching it never opens a console window. The command line is
// described in developer_automation.hpp (positional paths are imported; every option other than
// --help and --version is developer-only).

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSettings>
#include <QTimer>
#include <cmath>
#include <filesystem>
#include <memory>
#include <utility>

#include <QtQml/QQmlExtensionPlugin>
#include <windows.h>

#include "app_controller.hpp"
#include "developer_automation.hpp"
#include "service_factory.hpp"
#include "shell.hpp"
#include "volume_watcher.hpp"

// The Lucent QML module is a static plugin linked into this executable.
Q_IMPORT_QML_PLUGIN(LucentPlugin)

namespace {

// Shows `text` on stderr when the caller gave the process one (a redirected WIN32 program), and in
// a message box otherwise (double-click, shortcut, plain console launch).
void show_text(const QString& text, bool is_error) {
    const HANDLE error_handle = GetStdHandle(STD_ERROR_HANDLE);
    if (error_handle != nullptr && error_handle != INVALID_HANDLE_VALUE) {
        lucent::ui::report(text.trimmed());
        return;
    }
    const std::wstring message = text.toStdWString();
    MessageBoxW(nullptr, message.c_str(), L"Lucent", MB_OK | (is_error ? MB_ICONERROR : MB_ICONINFORMATION));
}

}  // namespace

int main(int argc, char* argv[]) {
    // As lucent-worker.exe (ADR 0013, item 10): the MSVC x64 runtime picks FMA3 or SSE2 code for exp,
    // log and cbrt at run time, so the editor's in-process Auto could resolve a near-tie differently
    // from an automatic batch's worker on the same photo. Both use the SSE2 code: they decide alike.
    _set_FMA3_enable(0);
    lucent::ui::configure_application();
    QGuiApplication app(argc, argv);

    const lucent::ui::CommandLine command_line = lucent::ui::parse_command_line(QCoreApplication::arguments());
    switch (command_line.action) {
    case lucent::ui::CommandLine::Action::ShowHelp:
    case lucent::ui::CommandLine::Action::ShowVersion:
        show_text(command_line.message, false);
        return 0;
    case lucent::ui::CommandLine::Action::Error:
        show_text(command_line.message, true);
        return 2;
    case lucent::ui::CommandLine::Action::Run:
        break;
    }

    lucent::ui::ServiceOptions service_options;
    service_options.log_to_file = true;  // <cache>\logs\lucent.log
    // Per-user settings (QSettings); with --data-root they live in that folder too, so a developer
    // session never changes the user's own settings.
    std::unique_ptr<QSettings> settings = std::make_unique<QSettings>();
    if (!command_line.options.data_root.isEmpty()) {
        const QString data_root = QDir::current().absoluteFilePath(command_line.options.data_root);
        service_options.data_root = std::filesystem::path(QDir::toNativeSeparators(data_root).toStdWString());
        settings = std::make_unique<QSettings>(QDir(data_root).filePath(QStringLiteral("settings.ini")),
                                               QSettings::IniFormat);
    }
    lucent::Expected<lucent::ui::Services> services = lucent::ui::create_services(service_options);
    if (!services) {
        show_text(QStringLiteral("Lucent could not start its processing services (%1).")
                      .arg(QString::fromStdString(services.error().describe())),
                  true);
        return 1;
    }
    // The controller must outlive the engine (declared first, destroyed last). It owns the
    // services; their executor threads are joined when it is destroyed.
    lucent::ui::AppController controller(std::move(*services));
    // The Import dialog's library folder is remembered between sessions (QSettings: per user).
    const QString destination_key = QStringLiteral("import/copyDestination");
    controller.set_copy_destination(settings->value(destination_key).toUrl());
    QObject::connect(&controller, &lucent::ui::AppController::importOptionsChanged, &controller,
                     [settings = settings.get(), &controller, destination_key] {
                         settings->setValue(destination_key, controller.copy_destination());
                     });
    // W7: the Export batch dialog remembers the output folder, name pattern, preset and the artist and
    // copyright text (never the GPS choice or a decision to replace files).
    lucent::ui::BatchSetupModel& batch_setup = *controller.batch_setup();
    batch_setup.restore(settings->value(QStringLiteral("batch/outputFolder")).toUrl(),
                        settings->value(QStringLiteral("batch/namePattern")).toString(),
                        settings->value(QStringLiteral("batch/preset")).toInt() == lucent::ui::BatchSetupModel::Tiff16
                            ? lucent::ui::BatchSetupModel::Tiff16
                            : lucent::ui::BatchSetupModel::Jpeg,
                        settings->value(QStringLiteral("batch/displayP3")).toBool(),
                        settings->value(QStringLiteral("batch/artist")).toString(),
                        settings->value(QStringLiteral("batch/copyright")).toString());
    QObject::connect(&batch_setup, &lucent::ui::BatchSetupModel::rememberedChanged, &controller,
                     [settings = settings.get(), &batch_setup] {
                         settings->setValue(QStringLiteral("batch/outputFolder"), batch_setup.output_folder());
                         settings->setValue(QStringLiteral("batch/namePattern"), batch_setup.name_pattern());
                         settings->setValue(QStringLiteral("batch/preset"), static_cast<int>(batch_setup.preset()));
                         settings->setValue(QStringLiteral("batch/displayP3"), batch_setup.display_p3());
                         settings->setValue(QStringLiteral("batch/artist"), batch_setup.artist());
                         settings->setValue(QStringLiteral("batch/copyright"), batch_setup.copyright());
                     });
    QQmlApplicationEngine engine;
    QQuickWindow* window = lucent::ui::load_main_window(engine, controller);
    if (window == nullptr) {
        show_text(QStringLiteral("The user interface could not be loaded."), true);
        return 1;
    }

    lucent::ui::DeveloperAutomation automation(command_line.options, controller, *window);
    QObject::connect(&automation, &lucent::ui::DeveloperAutomation::quitRequested, &app,
                     [](int exit_code) { QCoreApplication::exit(exit_code); });
    automation.start();
    // Background upkeep of the library (presence checks, hashing) starts once the window is up:
    // the contact sheet shows the catalog's items and cached thumbnails first.
    QTimer::singleShot(0, &controller, [&controller] { controller.start(); });
    // A card or drive that arrives or leaves while Lucent runs: check the library's files again
    // (Missing badges for a removed card, Present again when it returns).
    lucent::ui::VolumeWatcher volumes;
    app.installNativeEventFilter(&volumes);
    QObject::connect(&volumes, &lucent::ui::VolumeWatcher::volumes_changed, &controller,
                     [&controller] { controller.recheck_library(); });

    return QGuiApplication::exec();
}
