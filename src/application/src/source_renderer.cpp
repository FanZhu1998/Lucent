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

    Expected<engine::DevelopMemoryEstimate> estimate(const SourceItem& item,
                                                     const engine::StageOneRequest& request,
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
        return engine::estimate_develop_memory(selection->probe.metadata, request, develop_options());
    }

    Expected<engine::DevelopMemoryEstimate> estimate_for(const SourceItem& item,
                                                         const engine::StageOneRequest& request,
                                                         const engine::DevelopSettings& settings,
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
        return engine::estimate_develop_memory(selection->probe.metadata, request, settings, develop_options());
    }

    Expected<engine::DevelopMemoryEstimate> estimate_stage_two(
        const engine::StageOneImage& stage_one, const engine::StageOneRequest& request,
        const engine::DevelopSettings& settings) const override {
        return engine::estimate_develop_memory(stage_one.metadata, request, settings, develop_options());
    }

    Expected<engine::StageOneImage> render_stage_one(const SourceItem& item,
                                                     const engine::StageOneRequest& request,
                                                     const core::CancellationToken& cancel) const override {
        auto one = engine::render_stage_one_file(*registry_, item.path, item.subimage, request,
                                                 develop_options(), cancel);
        if (!one) {
            return Unexpected(std::move(one.error()));
        }
        if (auto same = require_identity(item, one->metadata.source.identity); !same) {
            return Unexpected(std::move(same.error()));
        }
        return one;
    }

    Expected<engine::DevelopResult> render_stage_two(const engine::StageOneImage& stage_one,
                                                     const engine::DevelopSettings& settings,
                                                     const core::CancellationToken& cancel) const override {
        return engine::render_stage_two(stage_one, settings, develop_options(), cancel);
    }

    Expected<engine::DevelopResult> render_stage_two_cached(const engine::StageOneImage& stage_one,
                                                            const engine::DevelopSettings& settings,
                                                            const StageTwoCaches& caches,
                                                            const core::CancellationToken& cancel) const override {
        engine::DevelopOptions options = develop_options();
        options.mask_cache = caches.masks;
        options.stage_two_cache = caches.stage_two;
        return engine::render_stage_two(stage_one, settings, options, cancel);
    }

    Expected<DevelopRender> render(const SourceItem& item, const engine::DevelopSettings& settings,
                                   const RenderProgress& progress,
                                   const core::CancellationToken& cancel) const override {
        if (auto valid = settings.validate(); !valid) {
            return Unexpected(std::move(valid.error()));
        }
        // engine::render_develop_file's composition (develop.cpp), with the identity check between
        // decoding and stage two.
        engine::StageOneRequest request;
        request.kind = engine::StageOneKind::Export;
        request.white_balance = settings.white_balance;
        auto one = render_stage_one(item, request, cancel);
        if (!one) {
            return Unexpected(std::move(one.error()));
        }
        if (progress) {
            progress(0.7);
        }
        DevelopRender out;
        out.interpretation = one->metadata.interpretation;
        auto two = engine::render_stage_two(std::move(*one), settings, develop_options(), cancel);
        if (!two) {
            return Unexpected(std::move(two.error()));
        }
        out.display = std::move(two->display);
        out.report = std::move(two->report);
        if (progress) {
            progress(1.0);
        }
        return out;
    }

    std::optional<engine::LensDatabaseIdentity> lens_database() const override {
        const std::shared_ptr<const engine::LensDatabase> database = lens_database_now();
        if (!database) {
            return std::nullopt;
        }
        return database->info().identity;
    }

private:
    // The database renders correct lenses with: the given one, or the loading source's once it has
    // loaded (this waits for it; see the file comment).
    std::shared_ptr<const engine::LensDatabase> lens_database_now() const {
        if (options_.lens_database || !options_.lens_database_source) {
            return options_.lens_database;
        }
        return options_.lens_database_source->database();
    }

    engine::DevelopOptions develop_options() const {
        engine::DevelopOptions options;
        options.decode_limits = options_.decode_limits;
        options.memory_budget_bytes = options_.memory_budget_bytes;
        options.parallel = options_.parallel;
        options.lens_database = lens_database_now();
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
