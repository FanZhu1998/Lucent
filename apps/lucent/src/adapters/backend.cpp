#include "adapters/backend.hpp"

#include <algorithm>
#include <new>
#include <utility>

#include <lucent/application/catalog_library.hpp>
#include <lucent/core/log.hpp>
#include <lucent/core/text.hpp>

namespace lucent::ui::adapters {

namespace {

constexpr std::string_view kLog = "ui.backend";

}  // namespace

void OutputFolders::add(const std::filesystem::path& folder) {
    if (folder.empty()) {
        return;
    }
    const std::lock_guard lock(mutex_);
    if (std::find(folders_.begin(), folders_.end(), folder) == folders_.end()) {
        folders_.push_back(folder);
    }
}

std::vector<std::filesystem::path> OutputFolders::snapshot() const {
    const std::lock_guard lock(mutex_);
    return folders_;
}

void InteractiveActivity::set_busy(bool busy) {
    {
        const std::lock_guard lock(mutex_);
        const bool was_busy = busy_;
        busy_ = busy;
        if (busy || !was_busy) {
            return;
        }
        idle_since_ = std::chrono::steady_clock::now();
    }
    const std::lock_guard lock(listener_mutex_);
    if (listener_) {
        listener_();
    }
}

bool InteractiveActivity::busy() const {
    const std::lock_guard lock(mutex_);
    return busy_;
}

std::chrono::milliseconds InteractiveActivity::idle_for() const {
    const std::lock_guard lock(mutex_);
    if (busy_) {
        return std::chrono::milliseconds(0);
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - idle_since_);
}

void InteractiveActivity::set_idle_listener(std::function<void()> listener) {
    const std::lock_guard lock(listener_mutex_);  // waits for a listener that is running
    listener_ = std::move(listener);
}

Expected<std::shared_ptr<Backend>> Backend::create(const BackendOptions& options) {
    try {
        std::shared_ptr<Backend> backend(new Backend());

        auto registry = raw::create_default_registry();
        if (!registry) {
            return Unexpected(std::move(registry.error()));
        }
        backend->registry_ = std::make_shared<const raw::DecoderRegistry>(std::move(*registry));
        backend->lucent_directories_ = options.lucent_directories;

        // The library: the catalog when one is configured and opens, else a session library.
        if (!options.catalog_path.empty()) {
            catalog::OpenOptions open;
            open.path = options.catalog_path;
            auto catalog = catalog::Catalog::open(open);
            if (catalog) {
                backend->catalog_ = std::shared_ptr<catalog::Catalog>(std::move(*catalog));
                auto library = app::CatalogLibrary::open(backend->catalog_);
                if (library) {
                    backend->library_ = std::move(*library);
                    if (backend->catalog_->schema().read_only_reason) {
                        backend->library_problem_ = backend->catalog_->schema().read_only_reason;
                    }
                } else {
                    backend->library_problem_ = library.error();
                    backend->catalog_.reset();
                }
            } else {
                backend->library_problem_ = catalog.error();
            }
            if (backend->library_problem_) {
                core::log_error(kLog, "the catalog '{}' is not fully available: {}",
                                core::path_to_utf8_lossy(options.catalog_path),
                                backend->library_problem_->describe());
            }
            if (backend->catalog_) {
                // Never import the catalog's own folder (its backups, its lock file).
                backend->lucent_directories_.push_back(backend->catalog_->path().parent_path());
            }
        }
        if (!backend->library_) {
            backend->library_ = std::make_unique<app::SessionLibrary>();
            if (!backend->library_problem_ && options.catalog_unavailable) {
                backend->library_problem_ = options.catalog_unavailable;
            }
        }

        // Recipes: the catalog's revisions with the persistent library, memory otherwise.
        if (backend->catalog_) {
            auto store = app::create_catalog_recipe_store(backend->catalog_);
            if (!store) {
                return Unexpected(std::move(store.error()));
            }
            backend->recipe_store_ = std::move(*store);
        } else {
            backend->recipe_store_ = app::create_memory_recipe_store();
        }
        app::RecipeServiceOptions recipe_options;
        recipe_options.registry = backend->registry_;
        recipe_options.probe_limits = options.renderer.decode_limits;
        backend->recipes_ = std::make_unique<app::RecipeService>(*backend->library_, backend->recipe_store_,
                                                                 std::move(recipe_options));

        // W7: durable batches over the writable persistent library.
        if (options.batches && backend->catalog_ && !backend->catalog_->is_read_only() &&
            backend->library_->capabilities().persistent) {
            app::BatchServiceOptions batch = options.batch;
            batch.registry = backend->registry_;
            batch.lucent_directories = backend->lucent_directories_;
            if (!batch.interactive_busy) {
                batch.interactive_busy = [activity = &backend->interactive_] { return activity->busy(); };
            }
            auto opened = app::BatchService::open(*backend->library_, backend->catalog_, *backend->recipes_,
                                                  std::move(batch));
            if (opened) {
                backend->batches_ = std::move(*opened);
            } else {
                core::log_warn(kLog, "batches are unavailable; exports run in-process: {}",
                               opened.error().describe());
            }
        }

        if (!options.thumbnail_directory.empty()) {
            app::ThumbnailCacheOptions cache;
            cache.directory = options.thumbnail_directory;
            cache.capacity_bytes = std::max(options.thumbnail_cache_bytes, cache.max_entry_bytes);
            auto thumbnails = app::ThumbnailCache::open(cache);
            if (thumbnails) {
                backend->thumbnails_ = std::shared_ptr<app::ThumbnailCache>(std::move(*thumbnails));
                backend->lucent_directories_.push_back(options.thumbnail_directory);
            } else {
                // Disposable: carry on without it.
                core::log_warn(kLog, "the thumbnail cache is unavailable: {}", thumbnails.error().describe());
            }
        }

        auto executor = app::TaskExecutor::create(options.executor);
        if (!executor) {
            return Unexpected(std::move(executor.error()));
        }
        backend->executor_ = std::move(*executor);

        if (options.renderer_override) {
            backend->renderer_ = options.renderer_override;
        } else {
            auto renderer = app::create_engine_renderer(backend->registry_, options.renderer);
            if (!renderer) {
                return Unexpected(std::move(renderer.error()));
            }
            backend->renderer_ = std::move(*renderer);
        }

        app::PreviewServiceOptions previews = options.previews;
        previews.thumbnails = backend->thumbnails_;
        previews.recipes = backend->recipes_.get();
        backend->previews_ = std::make_unique<app::PreviewService>(*backend->library_, *backend->executor_,
                                                                   backend->renderer_, previews);
        backend->exports_ = std::make_unique<app::ExportService>(*backend->library_, *backend->executor_,
                                                                 backend->renderer_, backend->recipes_.get(),
                                                                 backend->batches_.get());
        app::DevelopPreviewOptions develop;
        develop.on_metadata = [recipes = backend->recipes_.get()](app::ItemId item,
                                                                  const raw::RawMetadata& metadata) {
            recipes->note_camera(item, metadata);
        };
        backend->develop_ = std::make_unique<app::DevelopPreviewService>(
            *backend->library_, *backend->executor_, backend->renderer_, std::move(develop));

        // W8: the editor's Auto. Without it the editor offers no Auto; nothing else depends on it.
        app::AutoServiceOptions automatic = options.automatic;
        automatic.registry = backend->registry_;
        automatic.run.analysis.develop.decode_limits = options.renderer.decode_limits;
        automatic.run.analysis.develop.memory_budget_bytes = options.renderer.memory_budget_bytes;
        automatic.run.analysis.develop.parallel = options.renderer.parallel;
        // A run waiting for memory: the edit sessions' cached full-resolution stage ones (a 100 %
        // view) are rebuilt on demand, so they make room for it (a 61 MP photo's run does not fit
        // beside its own 100 % view). The develop service outlives the runs (see ~Backend).
        automatic.make_room = [develop = backend->develop_.get()](app::ItemId) {
            for (const app::ItemId open : develop->open_items()) {
                develop->release_full_resolution(open);
            }
        };
        auto created = app::AutoService::create(*backend->library_, *backend->recipes_, *backend->executor_,
                                                std::move(automatic));
        if (created) {
            backend->auto_ = std::move(*created);
        } else {
            core::log_warn(kLog, "automatic settings are unavailable: {}", created.error().describe());
        }

        if (backend->library_->capabilities().source_maintenance) {
            app::MaintenanceServiceOptions maintenance = options.maintenance;
            maintenance.hashing.thumbnails = backend->thumbnails_;
            maintenance.observer = [relay = backend->relay_](const app::MaintenanceEvent& event) {
                const std::lock_guard lock(relay->mutex);
                if (relay->sink) {
                    relay->sink(event);
                }
            };
            backend->maintenance_ = std::make_unique<app::MaintenanceService>(
                *backend->library_, *backend->registry_, *backend->executor_, std::move(maintenance));
        }
        return backend;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot create the application services", "ui.backend");
    }
}

void Backend::set_maintenance_sink(std::function<void(const app::MaintenanceEvent&)> sink) {
    const std::lock_guard lock(relay_->mutex);  // waits for a sink that is running
    relay_->sink = std::move(sink);
}

Backend::~Backend() {
    // Explicit, although member order already implies it: the services before the executor they
    // use, the executor (joining every task) before the library, catalog and registry those tasks
    // use.
    maintenance_.reset();
    auto_.reset();  // cancels the runs in progress and waits for them (they use the executor)
    develop_.reset();
    exports_.reset();
    previews_.reset();
    if (executor_) {
        executor_->shutdown();
    }
    executor_.reset();
    renderer_.reset();
    // After the export service (its exports wait for their batches); running batch jobs are given
    // back and continue at the next start.
    batches_.reset();
    recipes_.reset();
    recipe_store_.reset();
    library_.reset();
    catalog_.reset();
    thumbnails_.reset();
    registry_.reset();
}

}  // namespace lucent::ui::adapters
