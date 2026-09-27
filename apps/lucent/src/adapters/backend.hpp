#pragma once

// Backend: the application services one Lucent.exe session runs on (docs/adr/0007).
//
// It owns, in construction order: the decoder registry, the session library, the two-lane task
// executor (with its memory admission), the renderer, the preview service and the export
// service. The UI adapters (app_library_service.hpp, app_image_source.hpp,
// app_export_service.hpp) share ownership of one Backend.
//
// Destruction order is the reverse: the export and preview services cancel their outstanding
// work and wait for its callbacks, the executor shuts down (queued tasks run once with cancelled
// tokens, then the threads are joined), and only then are the library and the registry
// released. So work running on the executor may use the library and the registry by reference:
// both outlive every task.
//
// Rule for users: never let an executor task or service callback own a Backend (no
// std::shared_ptr<Backend> captured in work). The last owner must release it on a thread that
// is not an executor worker (the UI thread), because destruction joins those workers.
//
// Thread-safety: the accessors may be called from any thread; the services are thread-safe.
// OutputFolders is thread-safe.

#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

#include <lucent/application/export_service.hpp>
#include <lucent/application/library.hpp>
#include <lucent/application/preview_service.hpp>
#include <lucent/application/source_renderer.hpp>
#include <lucent/application/task_executor.hpp>
#include <lucent/core/error.hpp>
#include <lucent/raw/decoder_registry.hpp>

namespace lucent::ui::adapters {

struct BackendOptions {
    app::ExecutorOptions executor = app::ExecutorOptions::for_this_machine();
    app::PreviewServiceOptions previews;
    app::EngineRendererOptions renderer;
    // Replaces the engine renderer (tests use scripted renderers to control timing).
    std::shared_ptr<const app::SourceRenderer> renderer_override;
};

// Output folders chosen for exports in this session. Imports do not scan into them when a scan
// reaches them below the folder being imported (blueprint section 2: an output folder nested
// under an input folder is never re-imported); see app::ImportRequest::output_directories.
class OutputFolders {
public:
    void add(const std::filesystem::path& folder);
    [[nodiscard]] std::vector<std::filesystem::path> snapshot() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::filesystem::path> folders_;
};

class Backend {
public:
    // Errors: those of raw::create_default_registry(), TaskExecutor::create() (InvalidArgument for
    // invalid executor options) and create_engine_renderer(); OutOfMemory.
    [[nodiscard]] static Expected<std::shared_ptr<Backend>> create(const BackendOptions& options = {});

    ~Backend();
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    [[nodiscard]] const raw::DecoderRegistry& registry() const noexcept { return *registry_; }
    [[nodiscard]] app::Library& library() noexcept { return *library_; }
    [[nodiscard]] app::TaskExecutor& executor() noexcept { return *executor_; }
    [[nodiscard]] app::PreviewService& previews() noexcept { return *previews_; }
    [[nodiscard]] app::ExportService& exports() noexcept { return *exports_; }
    [[nodiscard]] OutputFolders& output_folders() noexcept { return output_folders_; }

private:
    Backend() = default;

    // Declaration order is construction order; see the file comment for destruction.
    std::shared_ptr<const raw::DecoderRegistry> registry_;
    std::unique_ptr<app::SessionLibrary> library_;
    std::unique_ptr<app::TaskExecutor> executor_;
    std::shared_ptr<const app::SourceRenderer> renderer_;
    std::unique_ptr<app::PreviewService> previews_;
    std::unique_ptr<app::ExportService> exports_;
    OutputFolders output_folders_;
};

}  // namespace lucent::ui::adapters
