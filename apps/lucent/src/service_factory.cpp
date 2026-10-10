#include "service_factory.hpp"

#include <filesystem>
#include <mutex>

#include <lucent/catalog/location.hpp>
#include <lucent/core/app_paths.hpp>
#include <lucent/core/log.hpp>
#include <lucent/core/version.hpp>

#include "adapters/application_services.hpp"

namespace lucent::ui {

namespace {

// AST-04: Lucent.exe's log file (<cache>\logs\lucent.log, bounded rotation; blueprint section 8 puts
// bounded logs in the per-user cache), so the reasons the services log (a refused camera support
// database, an unavailable catalog) reach a file support can ask for. Installed once per process; a
// second Lucent process that finds the file in use logs without it.
void install_log_file(const std::filesystem::path& logs) {
    static std::once_flag once;
    std::call_once(once, [&] {
        core::FileSinkOptions file;
        file.path = logs / L"lucent.log";
        auto sink = core::FileSink::open(std::move(file));
        if (!sink) {
            return;
        }
        if (core::logger().add_sink(std::move(*sink))) {
            core::log_info("ui.services", "Lucent {} ({}) started", core::build_info().version,
                           core::build_info().git_commit);
        }
    });
}

}  // namespace

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
    if (created && options.log_to_file) {
        install_log_file(directories->logs);
    }
    if (created) {
        backend.catalog_path = directories->data / catalog::default_catalog_file_name();
        backend.thumbnail_directory = directories->cache / L"thumbnails";
        backend.lucent_directories = {directories->root};
        // W10: per-user data packs would live in <data>\assets (refused until they can be signed).
        backend.user_data_directory = directories->data;
        // W12: downloaded installers are disposable (the cache); Setup's log goes with Lucent's.
        backend.update_directory = directories->cache / L"updates";
        backend.log_directory = directories->logs;
    } else {
        core::log_error("ui.services", "Lucent's application folders are unavailable: {}",
                        created.error().describe());
        backend.catalog_unavailable = created.error();
    }
    return adapters::create_application_services(backend);
}

}  // namespace lucent::ui
