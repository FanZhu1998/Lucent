#pragma once

// The set of services the shell runs against. service_factory.cpp (Lucent.exe) creates it; tests
// create it from fakes or test doubles.

#include <memory>

#include <QString>

#include "services/export_service.hpp"
#include "services/image_source.hpp"
#include "services/library_service.hpp"

namespace lucent::ui {

struct Services {
    std::unique_ptr<LibraryService> library;
    // Shared because the QML engine's image providers keep it alive for in-flight requests.
    std::shared_ptr<ImageSource> images;
    std::unique_ptr<ExportService> exports;
    // Short user-visible note shown in the window header when the services are simulated (for
    // example "Simulated services"); empty for the real services.
    QString simulation_notice;

    [[nodiscard]] bool complete() const noexcept { return library && images && exports; }
};

}  // namespace lucent::ui
