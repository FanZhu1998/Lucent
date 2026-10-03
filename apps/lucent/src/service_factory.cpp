#include "service_factory.hpp"

#include <lucent/catalog/location.hpp>
#include <lucent/core/app_paths.hpp>
#include <lucent/core/log.hpp>

#include "adapters/application_services.hpp"

namespace lucent::ui {

Expected<Services> create_services(const ServiceOptions& options) {
    adapters::BackendOptions backend;
    // Per-user folders (blueprint section 8): the catalog is application data, the thumbnails are a
    // disposable cache. Neither is ever imported from.
    Expected<core::AppDirectories> directories =
        options.data_root.empty()
            ? core::default_app_directories()
            : Expected<core::AppDirectories>(core::app_directories_at(options.data_root));
    Status created =
        directories ? core::create_app_directories(*directories) : Status(Unexpected(directories.error()));
    if (created) {
        backend.catalog_path = directories->data / catalog::default_catalog_file_name();
        backend.thumbnail_directory = directories->cache / L"thumbnails";
        backend.lucent_directories = {directories->root};
    } else {
        core::log_error("ui.services", "Lucent's application folders are unavailable: {}",
                        created.error().describe());
        backend.catalog_unavailable = created.error();
    }
    return adapters::create_application_services(backend);
}

}  // namespace lucent::ui
