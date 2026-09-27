// Lucent.exe: the Qt Quick desktop application (blueprint sections 2, 4 and 9).
//
// A WIN32-subsystem program: launching it never opens a console window. The command line is
// described in developer_automation.hpp (positional paths are imported; every option other than
// --help and --version is developer-only).

#include <windows.h>

#include <utility>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QtQml/QQmlExtensionPlugin>

#include "app_controller.hpp"
#include "developer_automation.hpp"
#include "service_factory.hpp"
#include "shell.hpp"

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

    lucent::Expected<lucent::ui::Services> services = lucent::ui::create_services();
    if (!services) {
        show_text(QStringLiteral("Lucent could not start its processing services (%1).")
                      .arg(QString::fromStdString(services.error().describe())),
                  true);
        return 1;
    }
    // The controller must outlive the engine (declared first, destroyed last). It owns the
    // services; their executor threads are joined when it is destroyed.
    lucent::ui::AppController controller(std::move(*services));
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

    return QGuiApplication::exec();
}
