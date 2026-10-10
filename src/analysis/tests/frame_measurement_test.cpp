// The band measurement of full-resolution renders (frame_measurement.hpp) against the whole-image
// measurement it replaces in the final check: make_image_context() of the stage one with the full
// grid's coverage map and the neutral-evidence mask, mapped through the geometry, measure_render()
// of the baseline with stored features, then of a candidate against them. Every statistic must be
// bit-identical (local contrast is not measured by bands), whatever the geometry, the baseline's
// operations and the band size.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/analysis/frame_measurement.hpp>
#include <lucent/analysis/proposals.hpp>
#include <lucent/analysis/render_metrics.hpp>
#include <lucent/analysis/source_diagnostics.hpp>
#include <lucent/engine/develop.hpp>
#include <lucent/engine/develop_geometry.hpp>
#include <lucent/engine/display_rendering.hpp>

#include "analysis_test_support.hpp"

namespace lucent::analysis {
namespace {

namespace ts = test_support;

// The chart with a clipped highlight (sensor saturation), deep shadows and a dark gradient.
ts::Scene scene() {
    const ts::Scene chart = ts::chart_scene(1.0, 1.08, 0.95);
    return [chart](std::int32_t x, std::int32_t y) -> std::array<double, 3> {
        if (x >= 820 && x < 900 && y >= 500 && y < 600) {
            return {1.6, 1.4, 1.2};  // beyond the white level in every channel
        }
        if (x >= 820 && x < 900 && y >= 380 && y < 440) {
            return ts::neutral(0.002 + 0.0001 * (x - 820));  // near black
        }
        return chart(x, y);
    };
}

struct Prepared {
    engine::StageOneImage stage_one;
    SourceAnalysis source;
    std::optional<NeutralEvidenceRule> rule;
};

Prepared prepare(image::Orientation orientation = image::Orientation::Normal) {
    ts::SceneOptions options;
    options.orientation = orientation;
    raw::DecodedRaw decoded = ts::make_scene(scene(), options);
    const image::Size upright =
        image::oriented_size(decoded.metadata.geometry.visible_area.size(), decoded.metadata.geometry.orientation);
    const std::array<image::Size, 1> grids{upright};
    auto source = analyze_source(decoded, grids, true, {}, true);
    EXPECT_TRUE(source) << source.error().describe();
    engine::StageOneRequest request;
    request.kind = engine::StageOneKind::Interactive;
    request.white_balance = engine::AsShotWhiteBalance{};
    auto one = engine::render_stage_one(std::move(decoded), request, engine::DevelopOptions{});
    EXPECT_TRUE(one) << one.error().describe();
    Prepared p;
    p.stage_one = std::move(*one);
    p.source = std::move(*source);
    NeutralEvidenceRule rule;
    rule.clip_level = as_shot_clip_level(p.stage_one);
    rule.cast_log_rg = std::log(1.08);
    rule.cast_log_bg = std::log(0.95);
    p.rule = rule;
    return p;
}

void expect_same(const RenderStats& a, const RenderStats& b, const std::string& what) {
    SCOPED_TRACE(what);
    EXPECT_EQ(a.size, b.size);
    EXPECT_EQ(a.pixels, b.pixels);
    EXPECT_EQ(a.nonfinite_samples, b.nonfinite_samples);
    EXPECT_EQ(a.quantiles, b.quantiles);
    EXPECT_EQ(a.mean_lightness, b.mean_lightness);
    EXPECT_EQ(a.signal_pixels, b.signal_pixels);
    EXPECT_EQ(a.near_white, b.near_white);
    EXPECT_EQ(a.dark, b.dark);
    EXPECT_EQ(a.highlight_clipped, b.highlight_clipped);
    EXPECT_EQ(a.shadow_blocked, b.shadow_blocked);
    EXPECT_EQ(a.out_of_gamut, b.out_of_gamut);
    EXPECT_EQ(a.new_highlight_clipped, b.new_highlight_clipped);
    EXPECT_EQ(a.new_shadow_blocked, b.new_shadow_blocked);
    EXPECT_EQ(a.new_out_of_gamut, b.new_out_of_gamut);
    EXPECT_EQ(a.source_saturated, b.source_saturated);
    EXPECT_EQ(a.saturated_greying, b.saturated_greying);
    EXPECT_EQ(a.chromatic_pixels, b.chromatic_pixels);
    EXPECT_EQ(a.saturation_log_ratio, b.saturation_log_ratio);
    EXPECT_EQ(a.hue_shift_degrees, b.hue_shift_degrees);
    EXPECT_EQ(a.mean_chroma, b.mean_chroma);
    EXPECT_EQ(a.neutral_pixels, b.neutral_pixels);
    EXPECT_EQ(a.neutral_shift_de, b.neutral_shift_de);
    EXPECT_EQ(a.noise_model_available, b.noise_model_available);
    EXPECT_EQ(a.shadow_pixels, b.shadow_pixels);
    EXPECT_EQ(a.shadow_noise_lstar, b.shadow_noise_lstar);
    EXPECT_EQ(a.midtone_pixels, b.midtone_pixels);
    EXPECT_EQ(a.midtone_x, b.midtone_x);
    EXPECT_EQ(a.midtone_y, b.midtone_y);
    EXPECT_EQ(a.midtone_z, b.midtone_z);
}

struct Case {
    std::string name;
    engine::DevelopSettings baseline;
    engine::DevelopSettings candidate;
    bool expect_streamed = true;
    std::int32_t band_rows = 64;
    bool with_rule = true;
    bool with_classes = true;
    std::optional<image::PixelRect> image_area;
};

// The whole-image measurement of the final check before W12: (baseline stats, candidate stats).
std::pair<RenderStats, RenderStats> whole_image(const Prepared& p, const Case& c) {
    const SaturationClasses* classes = c.with_classes ? &p.source.full_resolution : nullptr;
    auto context = make_image_context(p.stage_one, c.with_classes ? &p.source.coverage[0] : nullptr,
                                      p.source.diagnostics.noise);
    EXPECT_TRUE(context) << context.error().describe();
    (void)classes;
    if (c.with_rule && p.rule) {
        auto mask = neutral_evidence_mask(p.stage_one, *context, p.rule->clip_level, p.rule->cast_log_rg,
                                          p.rule->cast_log_bg);
        EXPECT_TRUE(mask);
        context->neutral_evidence = std::move(*mask);
    }
    auto mapping = engine::geometry_mapping(c.baseline, p.stage_one);
    EXPECT_TRUE(mapping);
    if (!mapping->identity()) {
        auto mapped = map_image_context(*context, *mapping);
        EXPECT_TRUE(mapped) << mapped.error().describe();
        *context = std::move(*mapped);
    }
    auto base_render = engine::render_stage_two(p.stage_one, c.baseline, engine::DevelopOptions{});
    EXPECT_TRUE(base_render) << base_render.error().describe();
    BaselineFeatures features;
    auto base = measure_render(base_render->display, *context, nullptr, &features);
    EXPECT_TRUE(base) << base.error().describe();
    auto candidate_render = engine::render_stage_two(p.stage_one, c.candidate, engine::DevelopOptions{});
    EXPECT_TRUE(candidate_render) << candidate_render.error().describe();
    auto candidate = measure_render(candidate_render->display, *context, &features, nullptr);
    EXPECT_TRUE(candidate) << candidate.error().describe();
    return {*base, *candidate};
}

void run_case(Prepared& p, const Case& c) {
    SCOPED_TRACE(c.name);
    p.stage_one.image_area.reset();
    if (c.image_area) {
        engine::ImageAreaPlacement area;
        area.source = raw::ImageAreaSource::DngDefaultCrop;
        area.rect = *c.image_area;
        p.stage_one.image_area = area;
    }
    const auto [base, candidate] = whole_image(p, c);
    ASSERT_GT(candidate.pixels, 0u);

    FrameMeasurementInputs inputs;
    inputs.stage_one = &p.stage_one;
    inputs.saturation = c.with_classes ? &p.source.full_resolution : nullptr;
    inputs.noise = p.source.diagnostics.noise;
    if (c.with_rule) {
        inputs.neutral = p.rule;
    }
    inputs.baseline = c.baseline;
    inputs.band_rows = c.band_rows;
    auto measurement = FrameMeasurement::create(inputs);
    ASSERT_TRUE(measurement) << measurement.error().describe();
    EXPECT_EQ(measurement->baseline_streamed(), c.expect_streamed);
    if (!measurement->baseline_streamed()) {
        auto rendered = engine::render_stage_two(p.stage_one, c.baseline, engine::DevelopOptions{});
        ASSERT_TRUE(rendered);
        ASSERT_TRUE(measurement->keep_baseline(std::move(rendered->display)));
        EXPECT_GT(measurement->kept_bytes(), 0u);
    }
    auto candidate_render = engine::render_stage_two(p.stage_one, c.candidate, engine::DevelopOptions{});
    ASSERT_TRUE(candidate_render);
    RenderStats band_base;
    auto band_candidate = measurement->measure(candidate_render->display, &band_base);
    ASSERT_TRUE(band_candidate) << band_candidate.error().describe();
    expect_same(base, band_base, "baseline, joint pass");
    expect_same(candidate, *band_candidate, "candidate");
    EXPECT_EQ(band_candidate->local_contrast_ev, 0.0);
    EXPECT_EQ(band_candidate->local_contrast_radius, 0);
    // Alone, and a second candidate measurement against the baseline stats already known.
    auto alone = measurement->measure_baseline();
    ASSERT_TRUE(alone);
    expect_same(base, *alone, "baseline alone");
    auto again = measurement->measure(candidate_render->display);
    ASSERT_TRUE(again);
    expect_same(candidate, *again, "candidate again");
    // The neutral evidence of the whole upright frame, as the mask counts it.
    if (c.with_rule && p.rule) {
        auto context = make_image_context(p.stage_one, &p.source.coverage[0], p.source.diagnostics.noise);
        ASSERT_TRUE(context);
        auto mask = neutral_evidence_mask(p.stage_one, *context, p.rule->clip_level, p.rule->cast_log_rg,
                                          p.rule->cast_log_bg);
        ASSERT_TRUE(mask);
        const auto expected = static_cast<std::uint64_t>(std::count(mask->begin(), mask->end(), std::uint8_t{1}));
        EXPECT_GT(expected, 0u);
        auto counted = measurement->upright_neutral_pixels();
        ASSERT_TRUE(counted);
        EXPECT_EQ(*counted, expected);
    }
}

engine::DevelopSettings candidate_settings() {
    engine::DevelopSettings s;
    s.exposure_ev = 0.6;
    s.shadows = 0.25;
    s.highlights = -0.2;
    s.saturation = 0.12;
    s.white_balance = engine::CameraNeutralWhiteBalance{2.0 * 0.97, 1.0, 1.5 * 1.04};
    return s;
}

TEST(FrameMeasurement, SaturationClassesAreTheFullGridsCoverage) {
    for (const image::Orientation orientation : {image::Orientation::Normal, image::Orientation::Rotate90Cw}) {
        SCOPED_TRACE(static_cast<int>(orientation));
        const Prepared p = prepare(orientation);
        const CoverageMap& coverage = p.source.coverage[0];
        const SaturationClasses& classes = p.source.full_resolution;
        ASSERT_EQ(classes.size, coverage.size);
        ASSERT_EQ(classes.classes.size(), coverage.fraction.size());
        std::uint64_t saturated = 0;
        for (std::size_t i = 0; i < coverage.fraction.size(); ++i) {
            const float f = coverage.fraction[i];
            const std::uint8_t expected = f <= 0.0f ? kNotSaturated
                                          : f >= kSaturatedCoverage ? kMostlySaturated
                                                                    : kPartlySaturated;
            ASSERT_EQ(classes.classes[i], expected) << i;
            saturated += expected != kNotSaturated ? 1 : 0;
            const float as_coverage = saturation_class_coverage(classes.classes[i]);
            EXPECT_EQ(as_coverage >= kSaturatedCoverage, f >= kSaturatedCoverage);
            EXPECT_EQ(as_coverage <= 0.0f, f <= 0.0f);
        }
        EXPECT_GT(saturated, 1000u);  // the clipped patch
    }
    // Not requested: none.
    raw::DecodedRaw decoded = ts::make_scene(scene());
    const std::array<image::Size, 1> grids{image::Size{960, 640}};
    auto source = analyze_source(decoded, grids);
    ASSERT_TRUE(source);
    EXPECT_TRUE(source->full_resolution.empty());
}

TEST(FrameMeasurement, TheNeutralEvidenceRuleIsTheMasksRule) {
    const Prepared p = prepare();
    auto context = make_image_context(p.stage_one, &p.source.coverage[0], p.source.diagnostics.noise);
    ASSERT_TRUE(context);
    auto mask = neutral_evidence_mask(p.stage_one, *context, p.rule->clip_level, p.rule->cast_log_rg,
                                      p.rule->cast_log_bg);
    ASSERT_TRUE(mask);
    std::uint64_t neutral = 0;
    for (std::int32_t y = 0; y < p.stage_one.pixels.height(); ++y) {
        const float* row = p.stage_one.pixels.row(y);
        for (std::int32_t x = 0; x < p.stage_one.pixels.width(); ++x, row += 3) {
            const std::size_t i = static_cast<std::size_t>(y) * 960 + static_cast<std::size_t>(x);
            const bool expected = (*mask)[i] != 0;
            ASSERT_EQ(p.rule->neutral(row[0], row[1], row[2], context->saturation[i] > 0.0f,
                                      p.source.diagnostics.noise),
                      expected)
                << x << "," << y;
            neutral += expected ? 1 : 0;
        }
    }
    EXPECT_GT(neutral, 10000u);
}

TEST(FrameMeasurement, BandsMeasureExactlyAsTheWholeImage) {
    Prepared p = prepare();
    const engine::DevelopSettings as_shot;
    std::vector<Case> cases;
    cases.push_back({"identity, streamed", as_shot, candidate_settings()});
    cases.push_back({"bands of 96 rows (a remainder)", as_shot, candidate_settings(), true, 96});
    cases.push_back({"one band", as_shot, candidate_settings(), true, 1024});
    {
        Case c{"crop, streamed", as_shot, candidate_settings()};
        c.baseline.geometry.crop = engine::CropRect{0.1, 0.05, 0.8, 0.9};
        c.candidate.geometry = c.baseline.geometry;
        cases.push_back(c);
    }
    {
        Case c{"camera image area and crop, streamed", as_shot, candidate_settings()};
        c.image_area = image::PixelRect{12, 8, 920, 610};
        c.baseline.geometry.crop = engine::CropRect{0.0, 0.1, 0.9, 1.0};
        c.candidate.geometry = c.baseline.geometry;
        cases.push_back(c);
    }
    {
        Case c{"straightened, kept", as_shot, candidate_settings(), false};
        c.baseline.geometry.straighten_degrees = 2.5;
        c.baseline.geometry.crop = engine::CropRect{0.1, 0.1, 0.9, 0.9};
        c.candidate.geometry = c.baseline.geometry;
        cases.push_back(c);
    }
    {
        Case c{"quarter turn and flip, kept", as_shot, candidate_settings(), false};
        c.baseline.geometry.quarter_turns = 1;
        c.baseline.geometry.flip_horizontal = true;
        c.candidate.geometry = c.baseline.geometry;
        cases.push_back(c);
    }
    {
        Case c{"baseline with locked local tone, kept", as_shot, candidate_settings(), false};
        c.baseline.shadows = 0.3;
        c.baseline.locks.insert(engine::DevelopControl::Shadows);
        c.candidate.shadows = 0.3;
        c.candidate.locks = c.baseline.locks;
        cases.push_back(c);
    }
    {
        Case c{"baseline with chroma denoising, kept", as_shot, candidate_settings(), false};
        c.baseline.chroma_denoise.strength = 0.5;
        c.candidate.chroma_denoise = c.baseline.chroma_denoise;
        cases.push_back(c);
    }
    {
        Case c{"no neutral rule, no saturation classes", as_shot, candidate_settings()};
        c.with_rule = false;
        c.with_classes = false;
        cases.push_back(c);
    }
    for (const Case& c : cases) {
        run_case(p, c);
    }
}

TEST(FrameMeasurement, ARotatedFileMeasuresAsTheWholeImage) {
    Prepared p = prepare(image::Orientation::Rotate90Ccw);
    Case c{"rotated file", engine::DevelopSettings{}, candidate_settings()};
    c.band_rows = 128;
    run_case(p, c);
}

TEST(FrameMeasurement, StreamsOnlyPerPixelBaselines) {
    engine::DevelopSettings s;
    EXPECT_TRUE(streams_baseline(s, false));
    s.geometry.crop = engine::CropRect{0.1, 0.1, 0.9, 0.9};
    EXPECT_TRUE(streams_baseline(s, false));
    s.geometry.straighten_degrees = 1.0;
    EXPECT_FALSE(streams_baseline(s, false));
    s = {};
    s.geometry.quarter_turns = 2;
    EXPECT_FALSE(streams_baseline(s, false));
    s = {};
    s.highlights = -0.1;
    EXPECT_FALSE(streams_baseline(s, false));
    s = {};
    s.chroma_denoise.strength = 0.4;
    EXPECT_FALSE(streams_baseline(s, false));
    EXPECT_TRUE(streams_baseline(s, true));  // monochrome data has no chroma to filter
    // The band's memory does not depend on the number of rows beyond one band (the partials aside).
    const std::uint64_t tall = frame_measurement_band_bytes(image::Size{8000, 60000}, 8000, true, 256);
    const std::uint64_t short_ = frame_measurement_band_bytes(image::Size{8000, 6000}, 8000, true, 256);
    EXPECT_LT(tall - short_, std::uint64_t{64} << 20);
    EXPECT_LT(short_, std::uint64_t{120} << 20);
    // By default a band holds at most 4 Mi pixels (32 rows at least), whatever the photo's width:
    // 50 bytes per pixel of context, features, stage one and band render, and the partials.
    for (const std::int32_t width : {4000, 12000, 30000, 60000, 131072}) {
        const std::uint64_t bytes = frame_measurement_band_bytes(image::Size{width, 100000}, width, true);
        EXPECT_LE(bytes, (std::uint64_t{9 + 17 + 2 * 12} << 22) + (std::uint64_t{1} << 20)) << width;
    }
}

TEST(FrameMeasurement, RefusesWhatItCannotMeasure) {
    Prepared p = prepare();
    FrameMeasurementInputs inputs;
    EXPECT_EQ(FrameMeasurement::create(inputs).error().code, ErrorCode::InvalidArgument);  // no stage one
    inputs.stage_one = &p.stage_one;
    SaturationClasses wrong;
    wrong.size = image::Size{10, 10};
    wrong.classes.assign(100, 0);
    inputs.saturation = &wrong;
    EXPECT_EQ(FrameMeasurement::create(inputs).error().code, ErrorCode::InvalidArgument);
    inputs.saturation = &p.source.full_resolution;
    inputs.baseline.geometry.straighten_degrees = 3.0;
    inputs.baseline.geometry.crop = engine::CropRect{0.1, 0.1, 0.9, 0.9};
    auto kept = FrameMeasurement::create(inputs);
    ASSERT_TRUE(kept);
    ASSERT_FALSE(kept->baseline_streamed());
    // Nothing is measured before the baseline is kept, and only renders of the output size are.
    auto render = engine::render_stage_two(p.stage_one, inputs.baseline, engine::DevelopOptions{});
    ASSERT_TRUE(render);
    EXPECT_EQ(kept->measure(render->display).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(kept->measure_baseline().error().code, ErrorCode::InvalidArgument);
    auto whole = engine::render_stage_two(p.stage_one, engine::DevelopSettings{}, engine::DevelopOptions{});
    ASSERT_TRUE(whole);
    auto other = engine::render_stage_two(p.stage_one, engine::DevelopSettings{}, engine::DevelopOptions{});
    ASSERT_TRUE(other);
    EXPECT_EQ(kept->keep_baseline(std::move(other->display)).error().code,
              ErrorCode::InvalidArgument);  // not the output size
    ASSERT_TRUE(kept->keep_baseline(std::move(render->display)));
    EXPECT_EQ(kept->measure(whole->display).error().code, ErrorCode::InvalidArgument);
    auto streamed = FrameMeasurement::create([&] {
        FrameMeasurementInputs plain;
        plain.stage_one = &p.stage_one;
        return plain;
    }());
    ASSERT_TRUE(streamed);
    EXPECT_EQ(streamed->keep_baseline(std::move(whole->display)).error().code, ErrorCode::InvalidArgument);
}

}  // namespace
}  // namespace lucent::analysis
