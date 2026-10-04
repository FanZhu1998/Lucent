#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include <gtest/gtest.h>

#include <lucent/analysis/source_diagnostics.hpp>
#include <lucent/engine/develop.hpp>

#include "analysis_test_support.hpp"

namespace lucent::analysis {
namespace {

using test_support::make_scene;
using test_support::SceneOptions;

TEST(SourceDiagnostics, CountsSaturatedSitesPerSensorChannel) {
    SceneOptions o;
    o.width = 64;
    o.height = 48;
    // Red saturated on the left half, every other channel at a third of the white level.
    auto decoded = make_scene(
        [](std::int32_t x, std::int32_t) -> std::array<double, 3> { return {x < 32 ? 1.5 : 0.3, 0.3, 0.3}; },
        o);
    const std::array<image::Size, 0> none{};
    auto analysis = analyze_source(decoded, none);
    ASSERT_TRUE(analysis) << analysis.error().describe();
    const SourceDiagnostics& d = analysis->diagnostics;
    ASSERT_EQ(d.channels.size(), 4u);
    EXPECT_EQ(d.channels[0].color, image::CfaColor::Red);
    EXPECT_EQ(d.channels[0].sites, 32u * 24u);
    EXPECT_EQ(d.channels[0].saturated, 16u * 24u);
    EXPECT_DOUBLE_EQ(d.channels[0].fraction(), 0.5);
    for (std::size_t c = 1; c < 4; ++c) {
        EXPECT_EQ(d.channels[c].sites, 32u * 24u) << c;
        EXPECT_EQ(d.channels[c].saturated, 0u) << c;
    }
    EXPECT_EQ(d.sites, 64u * 48u);
    EXPECT_EQ(d.saturated_sites, 16u * 24u);
    EXPECT_DOUBLE_EQ(d.max_channel_fraction(), 0.5);
    EXPECT_EQ(d.channels[0].threshold, test_support::kWhite);
}

// The coverage map must land where stage one puts the pixels, for every orientation.
class SourceCoverage : public ::testing::TestWithParam<image::Orientation> {};

TEST_P(SourceCoverage, AgreesWithTheEngineOrientationAndProxy) {
    SceneOptions o;
    o.width = 64;
    o.height = 48;
    o.orientation = GetParam();
    const auto scene = [](std::int32_t x, std::int32_t y) -> std::array<double, 3> {
        const double v = (x < 32 && y < 24) ? 1.6 : 0.2;
        return {v, v, v};
    };
    const image::Size upright = image::oriented_size(image::Size{64, 48}, o.orientation);
    const image::Size grid = engine::proxy_size(upright, 16);
    auto decoded = make_scene(scene, o);
    const std::array<image::Size, 1> grids{grid};
    auto analysis = analyze_source(decoded, grids);
    ASSERT_TRUE(analysis) << analysis.error().describe();
    ASSERT_EQ(analysis->coverage.size(), 1u);
    const CoverageMap& map = analysis->coverage[0];
    ASSERT_EQ(map.size, grid);
    EXPECT_EQ(analysis->diagnostics.upright_size, upright);

    engine::StageOneRequest request;
    auto one = engine::render_stage_one(make_scene(scene, o), request);
    ASSERT_TRUE(one) << one.error().describe();
    auto proxy = engine::make_proxy(*one, 16);
    ASSERT_TRUE(proxy) << proxy.error().describe();
    ASSERT_EQ(proxy->pixels.size(), grid);
    std::uint64_t saturated_cells = 0;
    for (std::int32_t y = 0; y < grid.height; ++y) {
        for (std::int32_t x = 0; x < grid.width; ++x) {
            const bool bright = proxy->pixels.at(x, y, 1) > 0.9f;
            EXPECT_EQ(map.at(x, y) > 0.5f, bright) << "cell " << x << ", " << y;
            saturated_cells += map.at(x, y) == 1.0f ? 1 : 0;
        }
    }
    EXPECT_EQ(saturated_cells, static_cast<std::uint64_t>(grid.area() / 4));
}

INSTANTIATE_TEST_SUITE_P(Orientations, SourceCoverage,
                         ::testing::Values(image::Orientation::Normal, image::Orientation::Rotate90Cw,
                                           image::Orientation::Rotate180, image::Orientation::Transverse),
                         [](const ::testing::TestParamInfo<image::Orientation>& info) {
                             std::string name(image::to_string(info.param));
                             std::replace(name.begin(), name.end(), '-', '_');
                             return name;
                         });

TEST(SourceDiagnostics, MeasuresReadNoiseOnTheOpticalBlack) {
    SceneOptions o;
    o.width = 256;
    o.height = 192;
    o.left_margin = 64;
    o.read_sigma = 5.0e-4;
    o.iso = 400.0f;
    auto decoded = make_scene([](std::int32_t, std::int32_t) { return test_support::neutral(0.2); }, o);
    auto analysis = analyze_source(decoded, {});
    ASSERT_TRUE(analysis) << analysis.error().describe();
    const SensorNoiseModel& noise = analysis->diagnostics.noise;
    EXPECT_EQ(noise.source, NoiseModelSource::MaskedAreaAndIso);
    ASSERT_TRUE(noise.masked_read_sigma.has_value());
    // The synthetic noise is a sum of four uniforms (unit variance): its MAD-based estimate lies
    // within a few per cent of the generator's sigma; 8 % allows for that and the sampling.
    EXPECT_NEAR(*noise.masked_read_sigma, 5.0e-4, 0.08 * 5.0e-4);
    EXPECT_GE(noise.masked_samples, kMinMaskedSamples / 2);
    EXPECT_DOUBLE_EQ(noise.shot_scale, 400.0 / (100.0 * kAssumedFullWellAtIso100));
    EXPECT_DOUBLE_EQ(noise.confidence, 0.9);

    // Without ISO the measured read noise is used alone.
    o.iso.reset();
    auto no_iso = analyze_source(
        make_scene([](std::int32_t, std::int32_t) { return test_support::neutral(0.2); }, o), {});
    ASSERT_TRUE(no_iso);
    EXPECT_EQ(no_iso->diagnostics.noise.source, NoiseModelSource::MaskedAreaOnly);
}

TEST(SourceDiagnostics, NoiseModelSources) {
    raw::RawMetadata m;
    // DNG NoiseProfile: plane 1 of three is the green plane.
    m.noise = raw::NoiseProfile{};
    m.noise->planes = {{1e-4, 1e-7}, {2e-4, 2e-7}, {3e-4, 3e-7}};
    SensorNoiseModel p = make_noise_model(m, std::nullopt, 0);
    EXPECT_EQ(p.source, NoiseModelSource::DngNoiseProfile);
    EXPECT_DOUBLE_EQ(p.shot_scale, 2e-4);
    EXPECT_DOUBLE_EQ(p.read_variance, 2e-7);
    EXPECT_DOUBLE_EQ(p.sigma(0.5), std::sqrt(2e-7 + 2e-4 * 0.5));
    EXPECT_DOUBLE_EQ(p.sigma(-1.0), std::sqrt(2e-7));

    // ISO assumption.
    raw::RawMetadata iso;
    iso.capture.iso = 400.0f;
    SensorNoiseModel a = make_noise_model(iso, std::nullopt, 0);
    EXPECT_EQ(a.source, NoiseModelSource::IsoAssumption);
    EXPECT_DOUBLE_EQ(a.read_variance, std::pow(kAssumedReadNoiseAtIso100 * 2.0, 2.0));
    EXPECT_DOUBLE_EQ(a.shot_scale, 400.0 / (100.0 * kAssumedFullWellAtIso100));

    // Nothing to go on.
    SensorNoiseModel none = make_noise_model(raw::RawMetadata{}, std::nullopt, 0);
    EXPECT_EQ(none.source, NoiseModelSource::Unavailable);
    EXPECT_FALSE(none.available());
    EXPECT_EQ(none.confidence, 0.0);
}

TEST(SourceDiagnostics, RejectsGridsLargerThanTheImage) {
    SceneOptions o;
    o.width = 32;
    o.height = 32;
    auto decoded = make_scene([](std::int32_t, std::int32_t) { return test_support::neutral(0.2); }, o);
    const std::array<image::Size, 1> grids{image::Size{64, 64}};
    auto analysis = analyze_source(decoded, grids);
    ASSERT_FALSE(analysis);
    EXPECT_EQ(analysis.error().code, ErrorCode::InvalidArgument);
}

}  // namespace
}  // namespace lucent::analysis
