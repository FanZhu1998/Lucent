#pragma once

// The set of services the shell runs against. service_factory.cpp (Lucent.exe) creates it; tests
// create it from fakes or test doubles.

#include <memory>

#include <QString>

#include "services/batch_service.hpp"
#include "services/edit_service.hpp"
#include "services/export_service.hpp"
#include "services/image_source.hpp"
#include "services/library_service.hpp"
#include "services/update_service.hpp"

namespace lucent::ui {

struct Services {
    std::unique_ptr<LibraryService> library;
    // Shared because the QML engine's image providers keep it alive for in-flight requests.
    std::shared_ptr<ImageSource> images;
    std::unique_ptr<ExportService> exports;
    // W6: detailed editing (recipes, revisions, edit sessions).
    std::unique_ptr<EditService> edits;
    // W7: durable batch exports (BatchService::is_available() is false when the session cannot
    // keep them; exports then run in-process through `exports` only).
    std::unique_ptr<BatchService> batches;
    // W12: "Check for updates" (ADR 0022). Optional: without it the shell offers no updates
    // (UnavailableUpdateService: "not configured in this build").
    std::unique_ptr<UpdateService> updates;
    // Short user-visible note shown in the window header when the services are simulated (for
    // example "Simulated services"); empty for the real services.
    QString simulation_notice;

    [[nodiscard]] bool complete() const noexcept { return library && images && exports && edits && batches; }
};

}  // namespace lucent::ui
