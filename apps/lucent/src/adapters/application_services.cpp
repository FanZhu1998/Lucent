#include "adapters/application_services.hpp"

#include <new>
#include <utility>

#include "adapters/app_batch_service.hpp"
#include "adapters/app_edit_service.hpp"
#include "adapters/app_export_service.hpp"
#include "adapters/app_library_service.hpp"

namespace lucent::ui::adapters {

Expected<Services> create_application_services(const BackendOptions& backend_options,
                                               const ImageSourceOptions& image_options) {
    auto backend = Backend::create(backend_options);
    if (!backend) {
        return Unexpected(std::move(backend.error()));
    }
    try {
        auto library = std::make_unique<AppLibraryService>(*backend);
        auto images = std::make_shared<AppImageSource>(*backend, library->render_event_sink(), image_options);
        auto exports = std::make_unique<AppExportService>(*backend);
        auto edits = std::make_unique<AppEditService>(*backend, library->render_event_sink());
        auto batches = std::make_unique<AppBatchService>(*backend);
        Services services;
        services.library = std::move(library);
        services.images = std::move(images);
        services.exports = std::move(exports);
        services.edits = std::move(edits);
        services.batches = std::move(batches);
        return services;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot create the user-interface services", "ui.services");
    }
}

}  // namespace lucent::ui::adapters
