#pragma once

// Backend: the application services one Lucent.exe session runs on (docs/adr/0007, 0008).
//
// It owns, in construction order: the decoder registry, the catalog (W4; when a catalog path is
// given), the library, the recipe store and recipe service (W6: the catalog's revisions, or memory
// for a session library), the on-disk thumbnail cache (when a folder is given), the three-lane task
// executor (with its memory admission), the renderer, the preview service, the export service,
// the edit-session service (W6, develop_preview.hpp) and the maintenance service (W4: presence
// checks, hashing, relinking). The UI adapters
// (app_library_service.hpp, app_image_source.hpp, app_export_service.hpp) share ownership of one
// Backend.
//
// The library. With BackendOptions::catalog_path the library is the persistent CatalogLibrary over
// that catalog: it is opened (created on first use, migrated after a verified backup when older)
// and loaded at creation, before any source file is read. When the catalog cannot be opened (it is
// open in another Lucent, damaged, on a network drive, ...), the session falls back to an
// in-memory SessionLibrary and library_problem() says why, so Lucent still starts and the user
// is told that changes are not saved; a catalog written by a newer Lucent is opened read-only
// (library_problem() set as well). Without a catalog path the library is a SessionLibrary (the
// W3 behaviour, used by tests).
//
// W8: the AutoService (auto_service.hpp, ADR 0013) runs the editor's "Auto" in process on the
// executor's Background lane, over the recipe service; it is created after the executor and
// destroyed before it (its destructor cancels the runs in progress and waits for them).
//
// W7: with a writable persistent library the Backend also opens the durable BatchService
// (batch_service.hpp, ADR 0012) after the recipe service; it reconciles the job journal of the
// previous session, continues running batches with lucent-worker.exe processes, and the export
// service sends single exports through it (batches of one). It pauses admission while interactive
// editing waits for a render (InteractiveActivity). When it cannot be opened, exports stay
// in-process and batches() is null. The shell reaches it through AppBatchService
// (app_batch_service.hpp).
//
// Destruction order is the reverse: the maintenance, export and preview services cancel their
// outstanding work and wait for its callbacks, the executor shuts down (queued tasks run once with
// cancelled tokens, then the threads are joined), and only then are the library, the catalog
// (which checkpoints its WAL on close), the thumbnail cache and the registry released. Work
// running on the executor may therefore use the library, the registry and the catalog by
// reference: they outlive every task.
//
// Rule for users: never let an executor task or service callback own a Backend (no
// std::shared_ptr<Backend> captured in work). The last owner must release it on a thread that
// is not an executor worker (the UI thread), because destruction joins those workers.
//
// Thread-safety: the accessors may be called from any thread; the services are thread-safe.
// OutputFolders, InteractiveActivity and the maintenance event relay are thread-safe.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <lucent/application/auto_service.hpp>
#include <lucent/application/batch_service.hpp>
#include <lucent/application/develop_preview.hpp>
#include <lucent/application/export_service.hpp>
#include <lucent/application/library.hpp>
#include <lucent/application/maintenance_service.hpp>
#include <lucent/application/preview_service.hpp>
#include <lucent/application/recipe_service.hpp>
#include <lucent/application/recipe_store.hpp>
#include <lucent/application/source_renderer.hpp>
#include <lucent/application/task_executor.hpp>
#include <lucent/application/thumbnail_cache.hpp>
#include <lucent/catalog/catalog.hpp>
#include <lucent/core/error.hpp>
#include <lucent/raw/decoder_registry.hpp>

namespace lucent::ui::adapters {

struct BackendOptions {
    app::ExecutorOptions executor = app::ExecutorOptions::for_this_machine();
    app::PreviewServiceOptions previews;
    app::EngineRendererOptions renderer;
    // Replaces the engine renderer (tests use scripted renderers to control timing).
    std::shared_ptr<const app::SourceRenderer> renderer_override;

    // W4. The catalog file of the persistent library (see the file comment); empty: a session
    // library that keeps nothing.
    std::filesystem::path catalog_path;
    // Folder of the on-disk thumbnail cache; empty: no disk cache.
    std::filesystem::path thumbnail_directory;
    // Disk budget of the thumbnail cache: 1 GiB holds the reduced camera thumbnails of about
    // 5,000 to 10,000 photos (app::camera_thumbnail_entry); older entries are evicted first.
    std::uint64_t thumbnail_cache_bytes = std::uint64_t{1} << 30;
    // Lucent's own folders (caches, catalog): never imported.
    std::vector<std::filesystem::path> lucent_directories;
    app::MaintenanceServiceOptions maintenance;  // its observer is replaced by the backend's relay
    // Why no catalog could be configured (the per-user folders are unavailable): reported as the
    // library problem of the session library.
    std::optional<Error> catalog_unavailable;
    // W7. Open the durable batch service over a writable catalog (see the file comment). Its
    // interactive_busy, registry and lucent_directories are filled in by the Backend.
    bool batches = true;
    app::BatchServiceOptions batch;
    // W8. The interactive Auto (its registry is filled in by the Backend; its decode limits and
    // memory budget follow `renderer`). Tests set fail_final_checks.
    app::AutoServiceOptions automatic;
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

// Whether interactive editing is waiting for a render: the newest settings of an edit session have
// not been rendered yet (app_edit_service.hpp). Lucent thumbnail renders wait while it is busy, so
// they never hold the render admission a refine or a 100 % view needs (app_image_source.hpp).
class InteractiveActivity {
public:
    // Calls the idle listener (on this thread, outside the internal lock) when the activity changes
    // from busy to idle.
    void set_busy(bool busy);
    [[nodiscard]] bool busy() const;
    // How long the activity has been idle (zero while busy; since creation when it never was).
    [[nodiscard]] std::chrono::milliseconds idle_for() const;
    // Replaces the listener; an empty function detaches. When this returns, a previous listener is
    // no longer running. The listener must not call set_idle_listener().
    void set_idle_listener(std::function<void()> listener);

private:
    mutable std::mutex mutex_;
    bool busy_ = false;
    std::chrono::steady_clock::time_point idle_since_ = std::chrono::steady_clock::now();
    std::mutex listener_mutex_;  // held while the listener runs and while it is replaced
    std::function<void()> listener_;
};

class Backend {
public:
    // Errors: those of raw::create_default_registry(), TaskExecutor::create() (InvalidArgument for
    // invalid executor options) and create_engine_renderer(); OutOfMemory. A catalog or thumbnail
    // cache that cannot be opened is not an error (see the file comment).
    [[nodiscard]] static Expected<std::shared_ptr<Backend>> create(const BackendOptions& options = {});

    ~Backend();
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    [[nodiscard]] const raw::DecoderRegistry& registry() const noexcept { return *registry_; }
    [[nodiscard]] app::Library& library() noexcept { return *library_; }
    [[nodiscard]] app::TaskExecutor& executor() noexcept { return *executor_; }
    [[nodiscard]] app::PreviewService& previews() noexcept { return *previews_; }
    [[nodiscard]] app::ExportService& exports() noexcept { return *exports_; }
    // W7. The durable batch service (null without a writable catalog, or when it could not open).
    [[nodiscard]] app::BatchService* batches() noexcept { return batches_.get(); }
    // W6. The items' recipes and the edit sessions (used by the editor; ADR 0010).
    [[nodiscard]] app::RecipeService& recipes() noexcept { return *recipes_; }
    [[nodiscard]] app::DevelopPreviewService& develop() noexcept { return *develop_; }
    // W8. The editor's Auto (null when it could not be created).
    [[nodiscard]] app::AutoService* automatic() noexcept { return auto_.get(); }
    [[nodiscard]] OutputFolders& output_folders() noexcept { return output_folders_; }
    // The recipe store under recipes() (tests write revisions through it directly).
    [[nodiscard]] app::RecipeStore& recipe_store() noexcept { return *recipe_store_; }
    // W6. Interactive editing's render activity (see InteractiveActivity).
    [[nodiscard]] InteractiveActivity& interactive() noexcept { return interactive_; }

    // W4. The catalog of the persistent library (null for a session library).
    [[nodiscard]] const std::shared_ptr<catalog::Catalog>& catalog() const noexcept { return catalog_; }
    // The maintenance service (null when the library keeps no source facts).
    [[nodiscard]] app::MaintenanceService* maintenance() noexcept { return maintenance_.get(); }
    // Why the persistent library is unavailable or read-only (see the file comment).
    [[nodiscard]] const std::optional<Error>& library_problem() const noexcept { return library_problem_; }
    [[nodiscard]] const std::vector<std::filesystem::path>& lucent_directories() const noexcept {
        return lucent_directories_;
    }

    // Receives the maintenance service's events (on its worker thread). Replaces the previous
    // sink; an empty function detaches. When this returns, a previous sink is no longer running.
    void set_maintenance_sink(std::function<void(const app::MaintenanceEvent&)> sink);

private:
    Backend() = default;

    struct Relay {
        std::mutex mutex;
        std::function<void(const app::MaintenanceEvent&)> sink;
    };

    // Declaration order is construction order; see the file comment for destruction.
    std::shared_ptr<const raw::DecoderRegistry> registry_;
    std::shared_ptr<catalog::Catalog> catalog_;
    std::unique_ptr<app::Library> library_;
    std::optional<Error> library_problem_;
    std::shared_ptr<app::RecipeStore> recipe_store_;
    std::unique_ptr<app::RecipeService> recipes_;
    std::unique_ptr<app::BatchService> batches_;
    std::shared_ptr<app::ThumbnailCache> thumbnails_;
    std::unique_ptr<app::TaskExecutor> executor_;
    std::shared_ptr<const app::SourceRenderer> renderer_;
    std::unique_ptr<app::PreviewService> previews_;
    std::unique_ptr<app::ExportService> exports_;
    std::unique_ptr<app::DevelopPreviewService> develop_;
    std::unique_ptr<app::AutoService> auto_;
    std::shared_ptr<Relay> relay_ = std::make_shared<Relay>();
    std::unique_ptr<app::MaintenanceService> maintenance_;
    OutputFolders output_folders_;
    InteractiveActivity interactive_;
    std::vector<std::filesystem::path> lucent_directories_;
};

}  // namespace lucent::ui::adapters
