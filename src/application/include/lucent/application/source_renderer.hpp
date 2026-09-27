#pragma once

// The seam between the application services and the engine: renders library items. The preview
// and export services use it; tests substitute a scripted implementation to control timing.
//
// Thread-safety: implementations are immutable and their const members may be called
// concurrently.

#include <cstdint>
#include <functional>
#include <memory>

#include <lucent/application/source_item.hpp>
#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/render.hpp>
#include <lucent/raw/decoder_registry.hpp>
#include <lucent/raw/raw_decoder.hpp>

namespace lucent::app {

// Called on the rendering thread after each render stage (engine::RenderOptions::on_stage_completed).
using StageObserver = std::function<void(const engine::StageRecord&)>;

class SourceRenderer {
public:
    virtual ~SourceRenderer() = default;
    SourceRenderer(const SourceRenderer&) = delete;
    SourceRenderer& operator=(const SourceRenderer&) = delete;

    // Bytes to admit (MemoryAdmission) before render(): the engine's estimate of its own peak
    // pixel memory for this item and settings. SourceChanged when the file no longer has the
    // item's identity.
    [[nodiscard]] virtual Expected<std::uint64_t> estimate_memory(
        const SourceItem& item, const engine::RenderSettings& settings,
        const core::CancellationToken& cancel) const = 0;

    // Renders image item.subimage of item.path. SourceChanged ("app.render.source") when the file
    // rendered does not have the item's identity (it was modified or replaced since import).
    [[nodiscard]] virtual Expected<engine::RenderResult> render(
        const SourceItem& item, const engine::RenderSettings& settings, const StageObserver& on_stage,
        const core::CancellationToken& cancel) const = 0;

protected:
    SourceRenderer() = default;
};

struct EngineRendererOptions {
    raw::DecodeLimits decode_limits;
    // engine::RenderOptions::memory_budget_bytes: the engine's own refusal threshold.
    std::uint64_t memory_budget_bytes = engine::kDefaultRenderMemoryBudget;
    bool parallel = true;
};

// The production renderer: raw::DecoderRegistry::select() + engine::estimate_render_memory for
// estimates, engine::render_file for renders. InvalidArgument ("app.render") for a null
// registry; OutOfMemory.
[[nodiscard]] Expected<std::shared_ptr<const SourceRenderer>> create_engine_renderer(
    std::shared_ptr<const raw::DecoderRegistry> registry, const EngineRendererOptions& options = {});

}  // namespace lucent::app
