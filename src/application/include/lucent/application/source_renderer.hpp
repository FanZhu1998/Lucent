#pragma once

// The seam between the application services and the engine's develop pipeline (develop.hpp, ADR
// 0010): renders library items. The preview, edit-session and export services use it; tests
// substitute a scripted implementation to control timing.
//
// Every render of an item checks that the file still has the item's identity (the one captured
// when it was imported): SourceChanged ("app.render.source") otherwise.
//
// render() is the export render, exactly engine::render_develop_file's composition: stage one of
// kind Export with the settings' white balance at full resolution, then stage two consuming it.
// The application runs the two steps itself only to check the decoded file's identity between them
// and to report progress; the pixels are those of render_develop_file (pinned by a test).
//
// Thread-safety: implementations are immutable and their const members may be called
// concurrently.

#include <cstdint>
#include <functional>
#include <memory>

#include <lucent/application/source_item.hpp>
#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/develop.hpp>
#include <lucent/engine/develop_settings.hpp>
#include <lucent/raw/decoded_raw.hpp>
#include <lucent/raw/decoder_registry.hpp>
#include <lucent/raw/raw_decoder.hpp>

namespace lucent::app {

// Called on the rendering thread with the fraction of a render that is done (0..1, increasing).
using RenderProgress = std::function<void(double fraction)>;

// The result of an export render.
struct DevelopRender {
    engine::ColorImage display;          // display_linear_rec2020(), upright: the exporter's input
    engine::DevelopReport report;        // stage one and stage two records
    raw::Interpretation interpretation;  // of the decoded image (after unpacking)
};

class SourceRenderer {
public:
    virtual ~SourceRenderer() = default;
    SourceRenderer(const SourceRenderer&) = delete;
    SourceRenderer& operator=(const SourceRenderer&) = delete;

    // The engine's memory estimate (engine::estimate_develop_memory from a probe of the file) for
    // stage one `request`: admit export_peak before render(), stage_one_peak before
    // render_stage_one(), and keep stage_one_bytes for a cached stage one.
    [[nodiscard]] virtual Expected<engine::DevelopMemoryEstimate> estimate(
        const SourceItem& item, const engine::StageOneRequest& request,
        const core::CancellationToken& cancel) const = 0;

    // Stage one of image item.subimage of item.path (engine::render_stage_one_file).
    [[nodiscard]] virtual Expected<engine::StageOneImage> render_stage_one(
        const SourceItem& item, const engine::StageOneRequest& request,
        const core::CancellationToken& cancel) const = 0;

    // Stage two on a cached stage one (engine::render_stage_two; `stage_one` is not modified).
    [[nodiscard]] virtual Expected<engine::DevelopResult> render_stage_two(
        const engine::StageOneImage& stage_one, const engine::DevelopSettings& settings,
        const core::CancellationToken& cancel) const = 0;

    // The export render (see the file comment). `progress` may be empty.
    [[nodiscard]] virtual Expected<DevelopRender> render(const SourceItem& item,
                                                         const engine::DevelopSettings& settings,
                                                         const RenderProgress& progress,
                                                         const core::CancellationToken& cancel) const = 0;

protected:
    SourceRenderer() = default;
};

struct EngineRendererOptions {
    raw::DecodeLimits decode_limits;
    // engine::DevelopOptions::memory_budget_bytes: the engine's own refusal threshold.
    std::uint64_t memory_budget_bytes = engine::kDefaultRenderMemoryBudget;
    bool parallel = true;
};

// The production renderer over the engine's develop pipeline. InvalidArgument ("app.render") for a
// null registry; OutOfMemory.
[[nodiscard]] Expected<std::shared_ptr<const SourceRenderer>> create_engine_renderer(
    std::shared_ptr<const raw::DecoderRegistry> registry, const EngineRendererOptions& options = {});

}  // namespace lucent::app
