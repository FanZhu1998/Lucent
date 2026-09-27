#include "adapters/backend.hpp"

#include <algorithm>
#include <new>
#include <utility>

namespace lucent::ui::adapters {

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

Expected<std::shared_ptr<Backend>> Backend::create(const BackendOptions& options) {
    try {
        std::shared_ptr<Backend> backend(new Backend());

        auto registry = raw::create_default_registry();
        if (!registry) {
            return Unexpected(std::move(registry.error()));
        }
        backend->registry_ = std::make_shared<const raw::DecoderRegistry>(std::move(*registry));
        backend->library_ = std::make_unique<app::SessionLibrary>();

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

        backend->previews_ = std::make_unique<app::PreviewService>(*backend->library_, *backend->executor_,
                                                                   backend->renderer_, options.previews);
        backend->exports_ =
            std::make_unique<app::ExportService>(*backend->library_, *backend->executor_, backend->renderer_);
        return backend;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot create the application services", "ui.backend");
    }
}

Backend::~Backend() {
    // Explicit, although member order already implies it: the services before the executor they
    // use, the executor (joining every task) before the library and registry those tasks use.
    exports_.reset();
    previews_.reset();
    if (executor_) {
        executor_->shutdown();
    }
    executor_.reset();
    renderer_.reset();
    library_.reset();
    registry_.reset();
}

}  // namespace lucent::ui::adapters
