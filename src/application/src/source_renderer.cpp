#include <format>
#include <new>
#include <string>
#include <utility>

#include <lucent/application/source_renderer.hpp>
#include <lucent/core/text.hpp>

namespace lucent::app {

namespace {

constexpr std::string_view kWhereSource = "app.render.source";

Status require_identity(const SourceItem& item, const core::FileIdentity& found) {
    if (found == item.identity) {
        return ok();
    }
    return fail(ErrorCode::SourceChanged,
                std::format("'{}' changed since it was imported (was {}, now {}); re-import it",
                            core::path_to_utf8_lossy(item.path), core::to_string(item.identity),
                            core::to_string(found)),
                std::string(kWhereSource));
}

class EngineRenderer final : public SourceRenderer {
public:
    EngineRenderer(std::shared_ptr<const raw::DecoderRegistry> registry, const EngineRendererOptions& options)
        : registry_(std::move(registry)), options_(options) {}

    Expected<std::uint64_t> estimate_memory(const SourceItem& item, const engine::RenderSettings& settings,
                                            const core::CancellationToken& cancel) const override {
        raw::ProbeOptions probe_options;
        probe_options.subimage = item.subimage;
        probe_options.limits = options_.decode_limits;
        probe_options.describe_subimages = false;
        auto selection = registry_->select(item.path, probe_options, cancel);
        if (!selection) {
            return Unexpected(std::move(selection.error()));
        }
        if (auto same = require_identity(item, selection->probe.metadata.source.identity); !same) {
            return Unexpected(std::move(same.error()));
        }
        auto estimate =
            engine::estimate_render_memory(selection->probe.metadata, settings, render_options({}));
        if (!estimate) {
            return Unexpected(std::move(estimate.error()));
        }
        return estimate->peak_bytes;
    }

    Expected<engine::RenderResult> render(const SourceItem& item, const engine::RenderSettings& settings,
                                          const StageObserver& on_stage,
                                          const core::CancellationToken& cancel) const override {
        auto result = engine::render_file(*registry_, item.path, item.subimage, settings,
                                          render_options(on_stage), cancel);
        if (!result) {
            return Unexpected(std::move(result.error()));
        }
        if (auto same = require_identity(item, result->metadata.source.identity); !same) {
            return Unexpected(std::move(same.error()));
        }
        return result;
    }

private:
    engine::RenderOptions render_options(const StageObserver& on_stage) const {
        engine::RenderOptions options;
        options.decode_limits = options_.decode_limits;
        options.memory_budget_bytes = options_.memory_budget_bytes;
        options.parallel = options_.parallel;
        options.on_stage_completed = on_stage;
        return options;
    }

    std::shared_ptr<const raw::DecoderRegistry> registry_;
    EngineRendererOptions options_;
};

}  // namespace

Expected<std::shared_ptr<const SourceRenderer>> create_engine_renderer(
    std::shared_ptr<const raw::DecoderRegistry> registry, const EngineRendererOptions& options) {
    if (!registry) {
        return fail(ErrorCode::InvalidArgument, "the decoder registry is null", "app.render");
    }
    try {
        return std::shared_ptr<const SourceRenderer>(
            std::make_shared<EngineRenderer>(std::move(registry), options));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot create the renderer", "app.render");
    }
}

}  // namespace lucent::app
