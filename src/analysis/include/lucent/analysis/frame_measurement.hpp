#pragma once

// Measurements of full-resolution renders that hold no per-pixel plane of the image (blueprint
// section 6, "Metric contract"; ADR 0011, amendment "Full-resolution measurements by bands"; ADR
// 0013 item 2). Used by the application's full-resolution final check of the automatic settings
// (application/auto_develop.hpp).
//
// measure_render() (render_metrics.hpp) keeps per-pixel planes of the whole render: the image
// context (9 bytes per pixel), the baseline's features (17), the log luminance and the two box planes
// of local contrast (12). That is right for the analysis image and the search proxy, whose size
// AutoOptions bounds whatever the photo, but at full resolution those planes grew with the photo
// (26 bytes per pixel of context and features alone refused Auto above about 69 MP on a 4 GiB
// budget). FrameMeasurement measures a full-resolution render band by band instead, with memory
// bounded by one band of rows (frame_measurement_band_bytes(); by default at most 4 Mi pixels, about
// 200 MiB), whatever the photo's size:
//   * The context of every output pixel comes from its upright pixel, the nearest one through the
//     render's geometry (exactly as map_image_context() picks it; the identity geometry reads the
//     same pixel): the scene signal is stage one's balanced green (as make_image_context()), the
//     saturation its SaturationClasses entry (the CoverageMap of that grid, in one byte), the neutral
//     surfaces the white-balance evidence's per-pixel rule (NeutralEvidenceRule, the rule of
//     neutral_evidence_mask()), all computed for the band being measured.
//   * The baseline's features (lightness, a*, b*, linear chroma, flags) are computed for each band
//     from the baseline's own pixels: re-rendered for that band from the stage one when the
//     baseline's stage two is per pixel and its geometry only crops (baseline_streamed(): no local
//     tone, no chroma denoising, no local adjustments, no straightening, quarter turn or flip; a
//     band of full upright rows renders exactly the pixels of the whole render, which the crop then
//     only copies), otherwise from a kept full-resolution render of the baseline (keep_baseline()).
// Every statistic is bit-identical to measure_render() of the same render with make_image_context()
// (mapped with map_image_context()) and the baseline's stored features: the same per-pixel
// arithmetic in the same order, accumulated into the same partials of 32 rows and merged in order
// (render_kernel.hpp). Local contrast is not measured (local_contrast_ev and its radius are 0): it
// feeds the objective's preference terms, which a final check does not evaluate
// (evaluate_candidate() judges feasibility).
//
// Thread-safety: not thread-safe (one caller at a time); measurements run in parallel inside.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include <lucent/analysis/proposals.hpp>
#include <lucent/analysis/render_metrics.hpp>
#include <lucent/analysis/source_diagnostics.hpp>
#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/develop.hpp>
#include <lucent/engine/develop_geometry.hpp>
#include <lucent/engine/develop_settings.hpp>

namespace lucent::analysis {

struct FrameMeasurementInputs {
    // The full-resolution stage one every measured render is rendered from (the whole upright image
    // at scale 1, with its camera image area). Must outlive the measurement and stay unchanged.
    const engine::StageOneImage* stage_one = nullptr;
    // The saturation classes of its upright grid (analyze_source()); null or empty: no saturation
    // information (make_image_context() without a coverage map).
    const SaturationClasses* saturation = nullptr;
    SensorNoiseModel noise;
    // The white-balance evidence's neutral surfaces; none: the baseline's near-neutral pixels.
    std::optional<NeutralEvidenceRule> neutral;
    // The baseline's settings: every measured render shares their geometry (the output frame).
    engine::DevelopSettings baseline;
    engine::DevelopOptions develop;  // the baseline's band renders: limits, parallelism
    // Rows per band (a multiple of 32). 0: one partial of 32 rows per hardware thread (at most 12),
    // and at most 4 Mi pixels of the image's width (but 32 rows at least): the band's memory is then
    // bounded whatever the photo (frame_measurement_band_bytes()). Tests use small bands.
    std::int32_t band_rows = 0;
};

// True when the baseline `settings` can be rendered band by band from a stage one with the same
// pixels as its whole render: stage two per pixel (no local tone, no chroma denoising unless the
// data is monochrome, no local adjustment with an effect) and a geometry that only crops (no
// straightening, quarter turn or flip; the camera's image area and a crop are translations).
[[nodiscard]] bool streams_baseline(const engine::DevelopSettings& settings, bool monochrome) noexcept;

// Bytes one band of a measurement of a render `output` pixels large holds at most (the context and
// the baseline's features of the band, and for a streamed baseline its band of stage one and its
// band render, `upright_width` pixels wide), with `band_rows` rows (0: the default).
[[nodiscard]] std::uint64_t frame_measurement_band_bytes(image::Size output, std::int32_t upright_width,
                                                         bool streamed, std::int32_t band_rows = 0) noexcept;

class FrameMeasurement {
public:
    // Errors (analysis.frame): InvalidArgument for a missing or empty stage one, one that is not at
    // full resolution, saturation classes of another size or settings the geometry refuses.
    [[nodiscard]] static Expected<FrameMeasurement> create(FrameMeasurementInputs inputs);

    FrameMeasurement(FrameMeasurement&&) noexcept;
    FrameMeasurement& operator=(FrameMeasurement&&) noexcept;
    FrameMeasurement(const FrameMeasurement&) = delete;
    FrameMeasurement& operator=(const FrameMeasurement&) = delete;
    ~FrameMeasurement();

    // The geometry of the renders (every measured render has its output size).
    [[nodiscard]] const engine::GeometryMapping& mapping() const noexcept;
    // The baseline is re-rendered band by band (see streams_baseline()); otherwise keep_baseline()
    // must be given its full render before anything is measured.
    [[nodiscard]] bool baseline_streamed() const noexcept;

    // The neutral surfaces of the whole upright image (what neutral_evidence_mask() counts before
    // the geometry crops them); 0 without a rule. Errors: Cancelled.
    [[nodiscard]] Expected<std::uint64_t> upright_neutral_pixels(const core::CancellationToken& cancel = {}) const;

    // Keeps the baseline's full render (display-linear Rec.2020 of the output size). InvalidArgument
    // for another size or encoding, or when the baseline is streamed.
    [[nodiscard]] Status keep_baseline(engine::ColorImage baseline);

    // The baseline's own statistics (the reference: relative values 0), as measure_render() of its
    // render without a baseline.
    [[nodiscard]] Expected<RenderStats> measure_baseline(const core::CancellationToken& cancel = {});

    // `render` (display-linear Rec.2020 of the output size, rendered from the stage one with the
    // baseline's geometry) against the baseline, as measure_render() with the baseline's features;
    // with `baseline_stats` not null, the baseline's own statistics come from the same pass. Errors:
    // InvalidArgument (render, or a baseline neither streamed nor kept); the baseline band render's
    // errors; OutOfMemory; Cancelled.
    // `progress` (optional) is called with the fraction of the rows measured after each band.
    [[nodiscard]] Expected<RenderStats> measure(const engine::ColorImage& render, RenderStats* baseline_stats = nullptr,
                                                const core::CancellationToken& cancel = {},
                                                const std::function<void(double)>& progress = {});

    // Bytes held between measurements (the kept baseline render).
    [[nodiscard]] std::uint64_t kept_bytes() const noexcept;

    struct Impl;

private:
    explicit FrameMeasurement(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lucent::analysis
