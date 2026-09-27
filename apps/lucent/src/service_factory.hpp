#pragma once

// The binding point between the shell and the application services.
//
// Lucent.exe calls create_services() once at start-up. It returns the UI adapters over the
// application services (adapters/application_services.hpp; docs/adr/0007-application-shell.md):
// import, camera and Lucent previews, and export all run in-process on the application services'
// task executor. The simulated services the shell was first developed against remain a test
// double (tests/fakes).

#include <lucent/core/error.hpp>

#include "services/services.hpp"

namespace lucent::ui {

// Errors: those of adapters::create_application_services() (for example when the executor's
// threads cannot be started).
[[nodiscard]] Expected<Services> create_services();

}  // namespace lucent::ui
