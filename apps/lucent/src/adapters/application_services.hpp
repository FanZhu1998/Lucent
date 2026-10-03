#pragma once

// The shell's services bound to the application services (docs/adr/0007, 0010): one Backend shared
// by AppLibraryService, AppImageSource, AppExportService and AppEditService. Lucent.exe gets them through
// service_factory.cpp; tests create them with their own options (for example a scripted
// renderer).
//
// Ownership: the returned Services own the adapters; the adapters share the Backend, which is
// released with the last of them. Destroy the Services (and so the AppController holding them)
// on the UI thread, after the QML engine.

#include <lucent/core/error.hpp>

#include "adapters/app_image_source.hpp"
#include "adapters/backend.hpp"
#include "services/services.hpp"

namespace lucent::ui::adapters {

// Errors: those of Backend::create().
[[nodiscard]] Expected<Services> create_application_services(const BackendOptions& backend = {},
                                                             const ImageSourceOptions& images = {});

}  // namespace lucent::ui::adapters
