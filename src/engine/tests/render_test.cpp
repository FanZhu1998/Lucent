// Unit tests for the render pipeline (render.hpp) on synthetic decoded data: stage order and
// encoding edges, neutral reproduction end to end, orientation, memory budget, settings
// validation, cancellation and failure propagation.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/core/cancellation.hpp>
#include <lucent/engine/render.hpp>
#include <lucent/image/color_math.hpp>

#include "sensor_test_support.hpp"

namespace lucent::engine {
namespace {

namespace ts = test_support;
using image::BayerLayout;
using image::CfaPattern;
using image::PixelRect;

// Levels chosen so that every synthetic sample is an exact integer: white - black = 16384.
constexpr double kBlack = 512.0;
constexpr double kWhite = 16896.0;
// As-shot multipliers (R, G, B, G2) with exact reciprocals.
constexpr std::array<double, 4> kMultipliers = {2.0, 1.0, 4.0, 1.0};

const image::Matrix3d kCamXyz{
    {0.9766, -0.2953, -0.1254}, {-0.4276, 1.2116, 0.2433}, {-0.0437, 0.1336, 0.5131}};

void set_camera_color(raw::RawMetadata& m) {
    for (std::size_t c = 0; c < 4; ++c) {
        m.color.as_shot_multipliers[c] = static_cast<float>(kMultipliers[c]);
    }
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            m.color.xyz_to_camera[i][j] = static_cast<float>(kCamXyz(i, j));
        }
    }
    m.color.xyz_to_camera[3] = m.color.xyz_to_camera[1];
}

// A Bayer frame of a neutral surface whose white-balanced level is `grey` (1 = white level), with
// the columns x >= clip_from at the white level (clipped highlights).
raw::DecodedRaw neutral_mosaic(double grey, std::int32_t clip_from = 1000, std::int32_t width = 16,
                               std::int32_t height = 12) {
    const CfaPattern cfa = CfaPattern::bayer(BayerLayout::Rggb);
    auto decoded =
        ts::make_mosaic(width, height, PixelRect{0, 0, width, height}, cfa, ts::simple_levels(kBlack, kWhite),
                        [&](std::int32_t x, std::int32_t y, std::int32_t) {
                            if (x >= clip_from) {
                                return kWhite;
                            }
                            const auto c = static_cast<std::size_t>(cfa.color_at(y, x));
                            return kBlack + grey / kMultipliers[c] * (kWhite - kBlack);
                        });
    set_camera_color(decoded.metadata);
    return decoded;
}

template <class T>
const T* parameter(const StageRecord& stage, const std::string& name) {
    for (const StageParameter& p : stage.parameters) {
        if (p.name == name) {
            return std::get_if<T>(&p.value);
        }
    }
    return nullptr;
}

TEST(RenderPipeline, RunsTheStagesInOrderWithMatchingEncodings) {
    auto result = render_decoded(neutral_mosaic(0.25), RenderSettings{});
    ASSERT_TRUE(result) << result.error().describe();
    const RenderReport& report = result->report;
    const std::vector<RenderStage> expected = {RenderStage::Normalize,    RenderStage::WhiteBalance,
                                               RenderStage::Demosaic,     RenderStage::CameraColor,
                                               RenderStage::WorkingSpace, RenderStage::DisplayRendering,
                                               RenderStage::Orientation};
    ASSERT_EQ(report.stages.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(report.stages[i].stage, expected[i]);
        ASSERT_TRUE(report.stages[i].input.has_value());
        if (i > 0) {
            EXPECT_EQ(*report.stages[i].input, report.stages[i - 1].output) << to_string(expected[i]);
        }
        EXPECT_FALSE(report.stages[i].algorithm.empty());
        EXPECT_GE(report.stages[i].seconds, 0.0);
    }
    EXPECT_EQ(report.stages[0].input, image::ColorEncoding::camera_native_mosaic());
    EXPECT_EQ(report.stages[3].input, image::ColorEncoding::camera_native_linear());
    EXPECT_EQ(report.stages[4].output, image::ColorEncoding::scene_linear_rec2020_d65());
    EXPECT_TRUE(report.stages[5].provisional);
    EXPECT_EQ(report.stages[6].output, display_linear_rec2020());
    EXPECT_EQ(result->display.encoding, display_linear_rec2020());
    EXPECT_EQ(report.pipeline_version, kRenderPipelineVersion);
    EXPECT_EQ(report.settings.schema, kRenderSettingsVersion);
    EXPECT_EQ(report.color.path, CalibrationPath::DecoderMatrix);
    EXPECT_EQ(report.demosaic.route, DemosaicRoute::BayerMalvarHeCutler);
    EXPECT_EQ(report.output_size, (image::Size{16, 12}));
    EXPECT_FALSE(result->scene_linear.has_value());
    EXPECT_GT(report.total_seconds, 0.0);
    const auto* route = parameter<std::string>(report.stages[2], "route");
    ASSERT_NE(route, nullptr);
    EXPECT_EQ(*route, "bayer_malvar_he_cutler");
}

TEST(RenderPipeline, ReproducesANeutralSurfaceAsEqualRec2020Values) {
    auto result = render_decoded(neutral_mosaic(0.25), RenderSettings{});
    ASSERT_TRUE(result) << result.error().describe();
    const image::ImageBuffer<float>& px = result->display.pixels;
    for (std::int32_t y = 0; y < px.height(); ++y) {
        for (std::int32_t x = 0; x < px.width(); ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                // Float matrices (camera -> XYZ -> Rec.2020): a few ulps around 0.25.
                ASSERT_NEAR(px.at(x, y, c), 0.25f, 2e-6f) << x << "," << y << "," << c;
            }
        }
    }
    EXPECT_EQ(result->report.white_balance.applied.source, WhiteBalanceSource::AsShot);
    EXPECT_DOUBLE_EQ(result->report.color.neutral_scale, 1.0);
}

TEST(RenderPipeline, ClippedHighlightsReachDisplayWhite) {
    auto result = render_decoded(neutral_mosaic(0.25, 8), RenderSettings{});
    ASSERT_TRUE(result) << result.error().describe();
    const image::ImageBuffer<float>& px = result->display.pixels;
    // Columns well inside the clipped area (away from the demosaic kernel at x = 8).
    for (std::int32_t y = 0; y < px.height(); ++y) {
        for (std::int32_t x = 11; x < px.width(); ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                ASSERT_NEAR(px.at(x, y, c), 1.0f, 2e-6f);
            }
        }
    }
    EXPECT_GT(result->report.white_balance.clipped_samples, 0u);
    EXPECT_DOUBLE_EQ(result->report.rendering.parameters.scene_white, 1.0);
}

TEST(RenderPipeline, ClippedHighlightsReachDisplayWhiteWithNegativeBaselineExposureAndLowLinearMax) {
    // COLR-01 / COLR-02 regression: a DNG with a negative BaselineExposure (Leica M10: -0.78 EV)
    // and a decoder linear_max below the white level (Canon R5: 0.85 of the range) must still
    // render clipped highlights as display white, and keep valid data above linear_max.
    // Neutral at 0.875 of the white level (green raw 0.875, above linear_max; every sample an
    // exact integer), clipped from x = 8.
    raw::DecodedRaw decoded = neutral_mosaic(0.875, 8);
    decoded.metadata.dng.emplace();
    decoded.metadata.dng->info.color.baseline_exposure = -0.78;
    const double linear_max = kBlack + 0.85 * (kWhite - kBlack);
    decoded.metadata.levels.linear_max = {linear_max, linear_max, linear_max, linear_max};
    RenderOptions options;
    options.keep_scene_linear = true;
    auto result = render_decoded(std::move(decoded), RenderSettings{}, options);
    ASSERT_TRUE(result) << result.error().describe();
    const RenderReport& report = result->report;
    EXPECT_EQ(report.normalization.saturation_level[0], 1.0f);
    EXPECT_EQ(report.white_balance.clip_level, 1.0f);
    EXPECT_DOUBLE_EQ(report.rendering.parameters.scene_white, 1.0);
    EXPECT_DOUBLE_EQ(report.rendering.gain, std::exp2(-0.78));
    EXPECT_DOUBLE_EQ(report.rendering.white, std::exp2(-0.78));
    EXPECT_EQ(report.rendering.curve, NeutralV0Curve::Lift);
    const image::ImageBuffer<float>& px = result->display.pixels;
    for (std::int32_t y = 0; y < px.height(); ++y) {
        for (std::int32_t x = 11; x < px.width(); ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                ASSERT_NEAR(px.at(x, y, c), 1.0f, 2e-6f) << x << "," << y << "," << c;
            }
        }
    }
    // The 0.875 neutral is kept (not clipped at linear_max) and lifted as the curve says.
    ASSERT_TRUE(result->scene_linear);
    const double lifted = neutral_v0_curve(0.875 * report.rendering.gain, report.rendering.parameters.knee,
                                           report.rendering.white);
    for (std::int32_t c = 0; c < 3; ++c) {
        EXPECT_NEAR(result->scene_linear->pixels.at(3, 3, c), 0.875f, 2e-6f) << c;
        EXPECT_NEAR(px.at(3, 3, c), lifted, 1e-5) << c;
    }
    EXPECT_GT(lifted, 0.875 * report.rendering.gain);
    EXPECT_LT(lifted, 1.0);
}

TEST(RenderPipeline, CancellationAfterAnyStageStopsTheRender) {
    const std::vector<RenderStage> stages = {RenderStage::Normalize,       RenderStage::WhiteBalance,
                                             RenderStage::Demosaic,        RenderStage::CameraColor,
                                             RenderStage::WorkingSpace,    RenderStage::DisplayRendering};
    for (const RenderStage stop_after : stages) {
        core::CancellationSource source;
        std::vector<RenderStage> seen;
        RenderOptions options;
        options.on_stage_completed = [&](const StageRecord& record) {
            seen.push_back(record.stage);
            if (record.stage == stop_after) {
                source.cancel();
            }
        };
        auto result = render_decoded(neutral_mosaic(0.25), RenderSettings{}, options, source.token());
        ASSERT_FALSE(result) << to_string(stop_after);
        EXPECT_EQ(result.error().code, ErrorCode::Cancelled) << to_string(stop_after);
        EXPECT_EQ(result.error().where, "engine.render.stage") << to_string(stop_after);
        ASSERT_FALSE(seen.empty());
        EXPECT_EQ(seen.back(), stop_after);  // no stage ran after the cancellation
    }
    // Without cancellation the observer sees every stage once, in order.
    std::vector<RenderStage> seen;
    RenderOptions options;
    options.on_stage_completed = [&](const StageRecord& record) { seen.push_back(record.stage); };
    ASSERT_TRUE(render_decoded(neutral_mosaic(0.25), RenderSettings{}, options));
    std::vector<RenderStage> all = stages;
    all.push_back(RenderStage::Orientation);
    EXPECT_EQ(seen, all);
}

TEST(RenderPipeline, KeepsTheSceneLinearImageAndAppliesOrientationToBoth) {
    raw::DecodedRaw decoded = neutral_mosaic(0.5);
    decoded.metadata.geometry.orientation = image::Orientation::Rotate90Cw;
    RenderOptions options;
    options.keep_scene_linear = true;
    auto result = render_decoded(std::move(decoded), RenderSettings{}, options);
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_EQ(result->display.pixels.size(), (image::Size{12, 16}));
    ASSERT_TRUE(result->scene_linear.has_value());
    EXPECT_EQ(result->scene_linear->pixels.size(), (image::Size{12, 16}));
    EXPECT_EQ(result->scene_linear->encoding, image::ColorEncoding::scene_linear_rec2020_d65());
    EXPECT_EQ(result->report.orientation, image::Orientation::Rotate90Cw);
    EXPECT_EQ(result->report.output_size, (image::Size{12, 16}));
    EXPECT_GT(result->report.memory.orientation_peak, result->report.memory.rendering_peak);
    EXPECT_NEAR(result->scene_linear->pixels.at(3, 5, 1), 0.5f, 2e-6f);
}

TEST(RenderPipeline, OrientationCanBeLeftUnapplied) {
    raw::DecodedRaw decoded = neutral_mosaic(0.5);
    decoded.metadata.geometry.orientation = image::Orientation::Rotate90Ccw;
    RenderSettings settings;
    settings.apply_orientation = false;
    auto result = render_decoded(std::move(decoded), settings);
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_EQ(result->display.pixels.size(), (image::Size{16, 12}));
    EXPECT_EQ(result->report.orientation, image::Orientation::Normal);
    EXPECT_FALSE(result->report.warnings.empty());
}

TEST(RenderPipeline, ExposureAndDngBaselineExposureScaleTheImage) {
    raw::DecodedRaw decoded = neutral_mosaic(0.125);
    decoded.metadata.dng.emplace();
    decoded.metadata.dng->info.color.baseline_exposure = 1.0;
    RenderSettings settings;
    settings.exposure_ev = 1.0;
    auto result = render_decoded(std::move(decoded), settings);
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_DOUBLE_EQ(result->report.rendering.gain, 4.0);
    EXPECT_NEAR(result->display.pixels.at(5, 5, 1), 0.5f, 2e-6f);
    // The DNG has no ColorMatrix1, so the decoder matrix is used and noted.
    EXPECT_EQ(result->report.color.path, CalibrationPath::DecoderMatrix);
    EXPECT_FALSE(result->report.color.notes.empty());

    raw::DecodedRaw again = neutral_mosaic(0.125);
    again.metadata.dng.emplace();
    again.metadata.dng->info.color.baseline_exposure = 1.0;
    settings.apply_baseline_exposure = false;
    auto without = render_decoded(std::move(again), settings);
    ASSERT_TRUE(without);
    EXPECT_DOUBLE_EQ(without->report.rendering.gain, 2.0);
}

TEST(RenderPipeline, RendersMonochromeAndLinearColorData) {
    auto mono = ts::make_monochrome(
        10, 8, PixelRect{1, 1, 8, 6}, ts::simple_levels(kBlack, kWhite),
        [](std::int32_t, std::int32_t, std::int32_t) { return kBlack + 0.25 * (kWhite - kBlack); });
    auto m = render_decoded(std::move(mono), RenderSettings{});
    ASSERT_TRUE(m) << m.error().describe();
    EXPECT_EQ(m->report.color.path, CalibrationPath::MonochromeNeutral);
    EXPECT_EQ(m->report.demosaic.route, DemosaicRoute::MonochromeBypass);
    EXPECT_EQ(m->display.pixels.size(), (image::Size{8, 6}));
    for (std::int32_t c = 0; c < 3; ++c) {
        EXPECT_NEAR(m->display.pixels.at(4, 3, c), 0.25f, 2e-6f);
    }

    auto linear = ts::make_linear(6, 4, PixelRect{0, 0, 6, 4}, ts::simple_levels(kBlack, kWhite),
                                  [](std::int32_t, std::int32_t, std::int32_t s) {
                                      return kBlack + 0.25 / kMultipliers[static_cast<std::size_t>(s)] *
                                                          (kWhite - kBlack);
                                  });
    set_camera_color(linear.metadata);
    auto l = render_decoded(std::move(linear), RenderSettings{});
    ASSERT_TRUE(l) << l.error().describe();
    EXPECT_EQ(l->report.demosaic.route, DemosaicRoute::LinearColorBypass);
    EXPECT_EQ(l->report.stages[2].input, image::ColorEncoding::camera_native_linear());
    for (std::int32_t c = 0; c < 3; ++c) {
        EXPECT_NEAR(l->display.pixels.at(2, 2, c), 0.25f, 2e-6f);
    }
}

TEST(RenderPipeline, ReportsInterpretationIssuesAsWarnings) {
    raw::DecodedRaw decoded = neutral_mosaic(0.25);
    decoded.metadata.interpretation.issues.push_back(
        {raw::InterpretationIssueKind::MandatoryOperationUnsupported,
         "GainMap (OpcodeList2) is not applied"});
    decoded.metadata.interpretation.status = raw::InterpretationStatus::RequiresUnsupportedOperation;
    decoded.metadata.geometry.pixel_aspect = 2.0;
    auto result = render_decoded(std::move(decoded), RenderSettings{});
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_EQ(result->report.interpretation.status, raw::InterpretationStatus::RequiresUnsupportedOperation);
    const auto& w = result->report.warnings;
    EXPECT_TRUE(std::any_of(w.begin(), w.end(),
                            [](const std::string& s) { return s.find("GainMap") != std::string::npos; }));
    EXPECT_TRUE(std::any_of(w.begin(), w.end(),
                            [](const std::string& s) { return s.find("non-square") != std::string::npos; }));
}

TEST(RenderPipeline, SerialAndParallelRendersAreIdentical) {
    const auto varied = [](std::int32_t x, std::int32_t y, std::int32_t) {
        return kBlack + (std::sin(0.7 * x) * 0.3 + 0.4 + 0.02 * y) * (kWhite - kBlack) * 0.5;
    };
    auto a = ts::make_mosaic(24, 18, PixelRect{0, 0, 24, 18}, CfaPattern::bayer(BayerLayout::Grbg),
                             ts::simple_levels(kBlack, kWhite), varied);
    auto b = ts::make_mosaic(24, 18, PixelRect{0, 0, 24, 18}, CfaPattern::bayer(BayerLayout::Grbg),
                             ts::simple_levels(kBlack, kWhite), varied);
    set_camera_color(a.metadata);
    set_camera_color(b.metadata);
    RenderOptions serial;
    serial.parallel = false;
    auto ra = render_decoded(std::move(a), RenderSettings{});
    auto rb = render_decoded(std::move(b), RenderSettings{}, serial);
    ASSERT_TRUE(ra && rb);
    for (std::int32_t y = 0; y < 18; ++y) {
        for (std::int32_t x = 0; x < 24; ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                ASSERT_EQ(ra->display.pixels.at(x, y, c), rb->display.pixels.at(x, y, c));
            }
        }
    }
}

TEST(RenderPipeline, RefusesToExceedTheMemoryBudget) {
    RenderOptions options;
    options.memory_budget_bytes = 1024;
    ts::expect_error(render_decoded(neutral_mosaic(0.25), RenderSettings{}, options),
                     ErrorCode::LimitExceeded, "engine.render.memory");
}

TEST(RenderPipeline, EstimatesMemoryFromTheGeometry) {
    raw::DecodedRaw decoded = neutral_mosaic(0.25, 1000, 64, 32);
    RenderOptions options;
    options.keep_scene_linear = true;
    auto e = estimate_render_memory(decoded.metadata, RenderSettings{}, options);
    ASSERT_TRUE(e) << e.error().describe();
    EXPECT_GE(e->raw_frame_bytes, 64u * 32u * 2u);
    EXPECT_GE(e->rgb_bytes, 64u * 32u * 12u);
    EXPECT_EQ(e->mask_bytes, 64u * 32u);
    EXPECT_EQ(e->demosaic_peak, e->sensor_bytes + e->mask_bytes + e->rgb_bytes);
    EXPECT_EQ(e->rendering_peak, 2 * e->rgb_bytes);
    EXPECT_EQ(e->orientation_peak, e->rendering_peak);  // Normal orientation: no copy
    EXPECT_EQ(e->peak_bytes, std::max({e->decode_peak, e->normalize_peak, e->demosaic_peak, e->color_peak,
                                       e->rendering_peak, e->orientation_peak}));

    image::AllocationLimits tiny;
    tiny.max_bytes = 64;
    options.limits = tiny;
    ts::expect_error(estimate_render_memory(decoded.metadata, RenderSettings{}, options),
                     ErrorCode::LimitExceeded, "engine.render.memory");
    raw::RawMetadata empty;
    ts::expect_error(estimate_render_memory(empty, RenderSettings{}, RenderOptions{}),
                     ErrorCode::InvalidArgument, "engine.render.memory");
}

TEST(RenderSettings, RejectsNewerSchemasAndInvalidValues) {
    RenderSettings s;
    EXPECT_TRUE(s.validate());
    s.schema = core::ContractVersion{1, 0};  // an older minor version is readable
    EXPECT_TRUE(s.validate());
    s.schema = core::ContractVersion{1, 2};
    ts::expect_error(s.validate(), ErrorCode::VersionMismatch, "engine.render.settings");
    s.schema = core::ContractVersion{2, 0};
    ts::expect_error(s.validate(), ErrorCode::VersionMismatch, "engine.render.settings");
    s = RenderSettings{};
    s.highlight_knee = 1.0;
    ts::expect_error(s.validate(), ErrorCode::InvalidArgument, "engine.render.settings");
    s = RenderSettings{};
    s.exposure_ev = std::nan("");
    ts::expect_error(s.validate(), ErrorCode::InvalidArgument, "engine.render.settings");
    s = RenderSettings{};
    s.white_balance = static_cast<WhiteBalanceRequest>(7);
    ts::expect_error(s.validate(), ErrorCode::InvalidArgument, "engine.render.settings");
    s = RenderSettings{};
    s.highlights = static_cast<HighlightPolicy>(0);
    ts::expect_error(s.validate(), ErrorCode::InvalidArgument, "engine.render.settings");
    s = RenderSettings{};
    s.rendering = static_cast<DisplayRendering>(2);
    ts::expect_error(s.validate(), ErrorCode::InvalidArgument, "engine.render.settings");
    // Invalid settings stop the render before any work.
    s = RenderSettings{};
    s.schema = core::ContractVersion{2, 0};
    ts::expect_error(render_decoded(neutral_mosaic(0.25), s), ErrorCode::VersionMismatch,
                     "engine.render.settings");
}

TEST(RenderPipeline, PropagatesStageFailures) {
    raw::DecodedRaw no_matrix = neutral_mosaic(0.25);
    no_matrix.metadata.color.xyz_to_camera = {};
    ts::expect_error(render_decoded(std::move(no_matrix), RenderSettings{}), ErrorCode::Unsupported,
                     "engine.color.calibration");

    RenderSettings custom;
    custom.white_balance = WhiteBalanceRequest::Custom;  // all-zero custom multipliers
    ts::expect_error(render_decoded(neutral_mosaic(0.25), custom), ErrorCode::InvalidArgument,
                     "engine.white_balance.resolve");

    raw::DecodedRaw broken = neutral_mosaic(0.25);
    broken.metadata.levels.white = kBlack;  // white not above black
    auto result = render_decoded(std::move(broken), RenderSettings{});
    ASSERT_FALSE(result);
}

TEST(RenderPipeline, CustomWhiteBalanceIsHonoured) {
    RenderSettings settings;
    settings.white_balance = WhiteBalanceRequest::Custom;
    settings.custom_multipliers = {2.0, 1.0, 4.0, 1.0};
    auto result = render_decoded(neutral_mosaic(0.25), settings);
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_EQ(result->report.white_balance.applied.source, WhiteBalanceSource::Custom);
    EXPECT_NEAR(result->display.pixels.at(4, 4, 0), 0.25f, 2e-6f);
}

TEST(RenderPipeline, HonoursCancellation) {
    core::CancellationSource source;
    source.cancel();
    auto result = render_decoded(neutral_mosaic(0.25), RenderSettings{}, RenderOptions{}, source.token());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::Cancelled);
}

}  // namespace
}  // namespace lucent::engine
