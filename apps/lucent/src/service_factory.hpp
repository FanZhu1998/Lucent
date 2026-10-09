#pragma once

// The binding point between the shell and the application services.
//
// Lucent.exe calls create_services() once at start-up. It returns the UI adapters over the
// application services (adapters/application_services.hpp; docs/adr/0007-application-shell.md,
// 0008-catalog-and-source-identity.md): the persistent library (the catalog in the per-user data
// folder), import, camera and Lucent previews (with the on-disk thumbnail cache in the per-user
// cache folder), background verification of the library's files, and export, all in-process on
// the application services' task executor. The simulated services the shell was first developed
// against remain a test double (tests/fakes).

#include <filesystem>

#include <lucent/core/error.hpp>

#include "services/services.hpp"

namespace lucent::ui {

struct ServiceOptions {
    // Root of Lucent's per-user folders (data, cache; core::AppDirectories); empty:
    // %LOCALAPPDATA%\Lucent. Developer-only (--data-root).
    std::filesystem::path data_root;
    // Attach the process's log file, <cache>\logs\lucent.log (core::FileSink, rotated at 8 MiB, 5
    // files), before the services start (AST-04: their warnings, such as a refused camera support
    // database, must reach a file). Lucent.exe sets it; tests leave it off.
    bool log_to_file = false;
};

// Errors: those of adapters::create_application_services() (for example when the executor's
// threads cannot be started). A catalog that cannot be opened is not an error: the session then
// runs on an in-memory library and LibraryService::library_notice() says so.
[[nodiscard]] Expected<Services> create_services(const ServiceOptions& options = {});

}  // namespace lucent::ui
