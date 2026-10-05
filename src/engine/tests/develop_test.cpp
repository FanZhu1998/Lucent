// Unit tests for the develop pipeline (develop.hpp) on synthetic decoded data: stage one records
// and orientation, the defaults through the base look, interactive versus export renders (exact
// without clipping, local differences at clipped edges), Kelvin and camera-neutral white balance,
// proxies and the box downscale, determinism, extreme settings, records, memory and failures.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/core/cancellation.hpp>
#include <lucent/engine/develop.hpp>
#include <lucent/engine/display_rendering.hpp>

#include "develop_test_support.hpp"

namespace lucent::engine {
namespace {

namespace ts = test_support;
using image::Vec3d;

constexpr std::array<double, 3> kAsShot = {2.0, 1.0, 1.5};

// A neutral scene of reflectance `rho(x, y)` lit by the as-shot white (balanced response = rho).
raw::DecodedRaw grey_scene(std::int32_t w, std::int32_t h, const std::function<double(std::int32_t, std::int32_t)>& rho) {
    return ts::scene_mosaic(w, h, [&](std::int32_t x, std::int32_t y) {
        const double v = rho(x, y);
        return std::array<double, 3>{v / kAsShot[0], v / kAsShot[1], v / kAsShot[2]};
    });
}

// A textured, coloured scene without saturated sites.
raw::DecodedRaw textured_scene(std::int32_t w = 48, std::int32_t h = 36) {
    return ts::scene_mosaic(w, h, [](std::int32_t x, std::int32_t y) {
        const double base = 0.02 + 0.4 * x / 48.0 + 0.05 * ((x * 3 + y * 5) % 7) / 7.0;
        return std::array<double, 3>{0.6 * base, base * (0.8 + 0.2 * (y % 3)), 0.3 * base};
    });
}

StageOneImage interactive(raw::DecodedRaw&& decoded, std::int32_t proxy = 0) {
    StageOneRequest request;
    request.proxy_long_edge = proxy;
    auto one = render_stage_one(std::move(decoded), request);
    EXPECT_TRUE(one) << one.error().describe();
    return std::move(*one);
}

TEST(DevelopPipeline, StageOneRecordsItsStagesAndBakesTheOrientation) {
    auto decoded = textured_scene(24, 16);
    decoded.metadata.geometry.orientation = image::Orientation::Rotate90Cw;
    StageOneImage one = interactive(std::move(decoded));
    EXPECT_EQ(one.kind, StageOneKind::Interactive);
    EXPECT_EQ(one.full_size, (image::Size{16, 24}));
    EXPECT_EQ(one.pixels.size(), (image::Size{16, 24}));
    EXPECT_EQ(one.pixels.channels(), 3);
    EXPECT_EQ(one.encoding, image::ColorEncoding::camera_native_linear());
    EXPECT_DOUBLE_EQ(one.scale, 1.0);
    EXPECT_EQ(one.orientation, image::Orientation::Rotate90Cw);
    ASSERT_EQ(one.stages.size(), 4u);
    EXPECT_EQ(one.stages[0].stage, RenderStage::Normalize);
    EXPECT_EQ(one.stages[1].stage, RenderStage::WhiteBalance);
    EXPECT_EQ(one.stages[2].stage, RenderStage::Demosaic);
    EXPECT_EQ(one.stages[3].stage, RenderStage::Orientation);
    EXPECT_EQ(one.white_balance.highlights, HighlightPolicy::None);
    EXPECT_EQ(one.white_balance.applied.source, WhiteBalanceSource::AsShot);
    for (std::size_t c = 0; c < 3; ++c) {
        EXPECT_NEAR(one.sample_multipliers[c], kAsShot[c], 1e-7);
    }
    EXPECT_FALSE(one.downscale.has_value());
    EXPECT_EQ(one.byte_size(), one.pixels.byte_size());
    EXPECT_GE(one.seconds, 0.0);
}

TEST(DevelopPipeline, DefaultsRenderNeutralsThroughTheNeutralBaseLook) {
    auto result = render_develop(grey_scene(16, 12, [](std::int32_t x, std::int32_t) { return 0.03 + 0.06 * x; }),
                                 DevelopSettings{});
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_EQ(result->display.encoding, display_linear_rec2020());
    auto curve = NeutralV1Curve::create(1.0);
    ASSERT_TRUE(curve);
    EXPECT_DOUBLE_EQ(result->report.white, 1.0);
    for (std::int32_t y = 2; y < 10; ++y) {
        for (std::int32_t x = 2; x < 14; ++x) {
            const double expected = curve->evaluate(0.03 + 0.06 * x);
            for (std::int32_t c = 0; c < 3; ++c) {
                ASSERT_NEAR(result->display.pixels.at(x, y, c), expected, 2e-6) << x << "," << y << "," << c;
            }
        }
    }
}

TEST(DevelopPipeline, InteractiveAndExportRendersAreIdenticalWithoutClipping) {
    for (const bool proxy_free : {true}) {
        (void)proxy_free;
        StageOneImage one = interactive(textured_scene());
        for (DevelopSettings settings : {DevelopSettings{}, [] {
                                             DevelopSettings s;
                                             s.exposure_ev = 0.7;
                                             s.contrast = 0.4;
                                             s.highlights = -0.6;
                                             s.shadows = 0.5;
                                             s.saturation = 0.2;
                                             s.vibrance = 0.3;
                                             s.tone_curve.points = {{0.0, 0.02}, {0.5, 0.55}, {1.0, 1.0}};
                                             return s;
                                         }()}) {
            auto preview = render_stage_two(one, settings);
            auto exported = render_develop(textured_scene(), settings);
            ASSERT_TRUE(preview) << preview.error().describe();
            ASSERT_TRUE(exported) << exported.error().describe();
            EXPECT_TRUE(ts::bit_identical(preview->display.pixels, exported->display.pixels));
            EXPECT_EQ(exported->report.stage_one_kind, StageOneKind::Export);
            EXPECT_EQ(preview->report.stage_one_kind, StageOneKind::Interactive);
        }
    }
}

TEST(DevelopPipeline, ClippedHighlightsDifferOnlyNextToClippedSites) {
    const std::int32_t w = 40, h = 24, clip_from = 24;
    const auto scene = [&](std::int32_t x, std::int32_t) {
        // Columns >= clip_from saturate every channel (response above the white level).
        return x >= clip_from ? 1.2 : 0.05 + 0.02 * x;
    };
    StageOneImage one = interactive(grey_scene(w, h, scene));
    auto preview = render_stage_two(one, DevelopSettings{});
    auto exported = render_develop(grey_scene(w, h, scene), DevelopSettings{});
    ASSERT_TRUE(preview && exported);
    for (std::int32_t y = 0; y < h; ++y) {
        for (std::int32_t x = 0; x < w; ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                const float a = preview->display.pixels.at(x, y, c);
                const float b = exported->display.pixels.at(x, y, c);
                ASSERT_TRUE(std::isfinite(a) && std::isfinite(b));
                if (x < clip_from - 3) {
                    ASSERT_EQ(a, b) << x << "," << y;  // Malvar-He-Cutler reaches 2 pixels
                }
                if (x >= clip_from + 3) {
                    ASSERT_NEAR(a, 1.0f, 1e-6f);  // fully clipped: display white on both paths
                    ASSERT_NEAR(b, 1.0f, 1e-6f);
                }
                ASSERT_LE(a, 1.0f + 1e-6f);
            }
        }
    }
}

TEST(DevelopPipeline, KelvinBalancesANeutralLitByThatIlluminant) {
    // Grey patches lit by illuminant A; the camera's as-shot balance is daylight-like (wrong).
    const Vec3d neutral_a = ts::kR5CamXyz * *image::xy_to_XYZ(ts::kIlluminantA);
    const auto scene = [&](std::int32_t x, std::int32_t) { return ts::neutral_response(neutral_a, 0.04 + 0.012 * x); };
    auto tt = temperature_tint_from_xy(ts::kIlluminantA);
    ASSERT_TRUE(tt);
    EXPECT_NEAR(tt->temperature_k, 2856.0, 3.0);
    DevelopSettings kelvin;
    kelvin.white_balance = KelvinWhiteBalance{tt->temperature_k, tt->tint};

    StageOneImage one = interactive(ts::scene_mosaic(32, 16, scene));
    auto preview = render_stage_two(one, kelvin);
    auto exported = render_develop(ts::scene_mosaic(32, 16, scene), kelvin);
    auto as_shot = render_stage_two(one, DevelopSettings{});
    ASSERT_TRUE(preview) << preview.error().describe();
    ASSERT_TRUE(exported) << exported.error().describe();
    ASSERT_TRUE(as_shot);
    ASSERT_TRUE(preview->report.target_temperature.has_value());
    EXPECT_NEAR(preview->report.target_temperature->temperature_k, tt->temperature_k, 0.05);
    for (const auto* r : {&*preview, &*exported}) {
        for (std::int32_t y = 3; y < 13; ++y) {
            for (std::int32_t x = 3; x < 29; ++x) {
                const float g = r->display.pixels.at(x, y, 1);
                EXPECT_NEAR(r->display.pixels.at(x, y, 0), g, 1e-4f * g) << x;
                EXPECT_NEAR(r->display.pixels.at(x, y, 2), g, 1e-4f * g) << x;
            }
        }
    }
    // Without the correction the patches are far from neutral (orange).
    EXPECT_GT(as_shot->display.pixels.at(16, 8, 0), 1.2f * as_shot->display.pixels.at(16, 8, 2));
    // Camera-neutral multipliers equal to the Kelvin ones render identically.
    auto model = WhiteBalanceModel::create(one.metadata);
    ASSERT_TRUE(model);
    auto m = model->multipliers_for(*tt);
    ASSERT_TRUE(m);
    DevelopSettings neutral;
    neutral.white_balance = CameraNeutralWhiteBalance{(*m)[0], (*m)[1], (*m)[2]};
    auto by_neutral = render_stage_two(one, neutral);
    ASSERT_TRUE(by_neutral);
    EXPECT_TRUE(ts::bit_identical(by_neutral->display.pixels, preview->display.pixels));
}

TEST(DevelopPipeline, RefinedInteractiveStageOneMatchesExportForAnyWhiteBalance) {
    DevelopSettings s;
    s.white_balance = KelvinWhiteBalance{3600.0, -12.0};
    s.shadows = 0.4;
    s.contrast = 0.2;
    // Quick: the as-shot cache corrected in stage two differs at edges (demosaicing saw another
    // balance); refine: the cache rebuilt at the settings' white balance renders exactly the
    // export (no clipped sites in this scene).
    StageOneImage quick = interactive(textured_scene());
    StageOneRequest refine;
    refine.white_balance = s.white_balance;
    auto refined = render_stage_one(textured_scene(), refine);
    ASSERT_TRUE(refined) << refined.error().describe();
    EXPECT_EQ(refined->white_balance.applied.source, WhiteBalanceSource::Custom);
    EXPECT_EQ(refined->white_balance.highlights, HighlightPolicy::None);
    auto exported = render_develop(textured_scene(), s);
    auto from_quick = render_stage_two(quick, s);
    auto from_refined = render_stage_two(*refined, s);
    ASSERT_TRUE(exported && from_quick && from_refined);
    EXPECT_TRUE(ts::bit_identical(from_refined->display.pixels, exported->display.pixels));
    EXPECT_EQ(from_refined->report.correction, (RgbMultipliers{1.0, 1.0, 1.0}));
    EXPECT_FALSE(ts::bit_identical(from_quick->display.pixels, exported->display.pixels));
    // This scene changes colour every pixel (the worst case for demosaicing differences).
    double sum = 0.0;
    const image::ImageBuffer<float>& q = from_quick->display.pixels;
    for (std::int32_t y = 0; y < q.height(); ++y) {
        for (std::int32_t x = 0; x < q.width(); ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                sum += std::abs(q.at(x, y, c) - exported->display.pixels.at(x, y, c));
            }
        }
    }
    EXPECT_LT(sum / (3.0 * static_cast<double>(q.size().area())), 0.02);
}

TEST(DevelopPipeline, ExposureMovesMidTonesAndKeepsClippedHighlightsWhite) {
    const auto scene = [](std::int32_t x, std::int32_t) { return x >= 20 ? 1.5 : 0.1; };
    StageOneImage one = interactive(grey_scene(28, 12, scene));
    for (const double ev : {-2.0, -1.0, 1.0, 2.5}) {
        DevelopSettings s;
        s.exposure_ev = ev;
        auto r = render_stage_two(one, s);
        ASSERT_TRUE(r) << r.error().describe();
        auto curve = NeutralV1Curve::create(std::exp2(ev));
        ASSERT_TRUE(curve);
        EXPECT_NEAR(r->report.white, std::exp2(ev), 1e-12);
        EXPECT_NEAR(r->display.pixels.at(5, 6, 1), curve->evaluate(0.1 * std::exp2(ev)), 2e-6) << ev;
        EXPECT_NEAR(r->display.pixels.at(26, 6, 1), 1.0f, 1e-6f) << ev;
        EXPECT_EQ(r->report.base_look.regime, ev < 0.0 ? NeutralV1Regime::Lift : NeutralV1Regime::Shoulder);
    }
}

TEST(DevelopPipeline, RecordsEveryOperationInOrder) {
    StageOneImage one = interactive(textured_scene());
    auto defaults = render_stage_two(one, DevelopSettings{});
    ASSERT_TRUE(defaults);
    const std::vector<DevelopOperation> order = {
        DevelopOperation::WhiteBalance,  DevelopOperation::CameraColor,      DevelopOperation::WorkingSpace,
        DevelopOperation::Exposure,      DevelopOperation::ChromaDenoise,    DevelopOperation::LocalTone,
        DevelopOperation::LocalAdjustments, DevelopOperation::Contrast,      DevelopOperation::ToneCurve,
        DevelopOperation::Color,         DevelopOperation::BaseLook,         DevelopOperation::Geometry};
    const DevelopReport& report = defaults->report;
    ASSERT_EQ(report.operations.size(), order.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        EXPECT_EQ(report.operations[i].operation, order[i]);
        EXPECT_FALSE(report.operations[i].algorithm.empty());
        EXPECT_GE(report.operations[i].version, 1u);
    }
    for (const std::size_t skipped : {3u, 4u, 5u, 6u, 7u, 8u, 9u, 11u}) {
        EXPECT_FALSE(report.operations[skipped].applied) << to_string(order[skipped]);
    }
    EXPECT_TRUE(report.operations[10].applied);
    EXPECT_EQ(report.pipeline_version, kDevelopPipelineVersion);
    EXPECT_EQ(report.settings, DevelopSettings{});
    EXPECT_EQ(report.color.path, CalibrationPath::DecoderMatrix);
    EXPECT_EQ(report.correction, (RgbMultipliers{1.0, 1.0, 1.0}));
    EXPECT_FLOAT_EQ(static_cast<float>(report.clip_level), 1.0f);
    EXPECT_EQ(report.stage_one_stages.size(), 4u);
    EXPECT_EQ(report.output_size, one.pixels.size());
    EXPECT_FALSE(report.local_tone.has_value());
    EXPECT_GE(report.stage_two_seconds, 0.0);

    DevelopSettings all;
    all.exposure_ev = 0.3;
    all.highlights = -0.4;
    all.contrast = 0.2;
    all.tone_curve.points = {{0.0, 0.0}, {0.5, 0.6}, {1.0, 1.0}};
    all.vibrance = 0.4;
    auto applied = render_stage_two(one, all);
    ASSERT_TRUE(applied);
    for (const std::size_t i : {3u, 5u, 7u, 8u, 9u, 10u}) {
        EXPECT_TRUE(applied->report.operations[i].applied) << to_string(order[i]);
    }
    ASSERT_TRUE(applied->report.local_tone.has_value());
    EXPECT_EQ(applied->report.local_tone->radius, local_tone_radius(one.full_size, 1.0));
    EXPECT_EQ(to_string(DevelopOperation::LocalTone), "local_tone");
    EXPECT_EQ(to_string(StageOneKind::Export), "export");
}

TEST(DevelopPipeline, StageTwoIsDeterministicAndLeavesStageOneUntouched) {
    StageOneImage one = interactive(textured_scene(64, 40));
    auto before = one.pixels.clone();
    ASSERT_TRUE(before);
    DevelopSettings s;
    s.highlights = -0.8;
    s.shadows = 0.7;
    s.contrast = -0.3;
    s.saturation = 0.25;
    s.white_balance = KelvinWhiteBalance{4300.0, 6.0};
    auto a = render_stage_two(one, s);
    auto b = render_stage_two(one, s);
    DevelopOptions serial;
    serial.parallel = false;
    auto c = render_stage_two(one, s, serial);
    ASSERT_TRUE(a && b && c) << (a ? "" : a.error().describe());
    EXPECT_TRUE(ts::bit_identical(a->display.pixels, b->display.pixels));
    EXPECT_TRUE(ts::bit_identical(a->display.pixels, c->display.pixels));
    EXPECT_TRUE(ts::bit_identical(one.pixels, *before));
    // The consuming overload renders the same pixels.
    auto consumed = render_stage_two(std::move(one), s);
    ASSERT_TRUE(consumed);
    EXPECT_TRUE(ts::bit_identical(consumed->display.pixels, a->display.pixels));
}

TEST(DevelopPipeline, ExtremeSettingsStayFiniteAndDisplayReferred) {
    StageOneImage one = interactive(textured_scene());
    std::vector<DevelopSettings> extremes(4);
    for (std::size_t i = 0; i < extremes.size(); ++i) {
        const double sign = (i % 2 == 0) ? 1.0 : -1.0;
        DevelopSettings& s = extremes[i];
        s.exposure_ev = sign * kMaxExposureEv;
        s.contrast = sign;
        s.highlights = sign;
        s.shadows = -sign;
        s.saturation = sign;
        s.vibrance = -sign;
        s.tone_curve.points = i < 2 ? std::vector<ToneCurvePoint>{{0.0, 0.0}, {0.05, 0.9}, {1.0, 1.0}}
                                    : std::vector<ToneCurvePoint>{{0.0, 0.3}, {0.95, 0.31}, {1.0, 1.0}};
        s.white_balance = i < 2 ? WhiteBalanceSetting{KelvinWhiteBalance{kMinTemperatureK, kMaxTint}}
                                : WhiteBalanceSetting{KelvinWhiteBalance{kMaxTemperatureK, -kMaxTint}};
    }
    for (const DevelopSettings& s : extremes) {
        auto r = render_stage_two(one, s);
        ASSERT_TRUE(r) << r.error().describe();
        EXPECT_TRUE(ts::all_finite(r->display.pixels));
        for (std::int32_t y = 0; y < r->display.pixels.height(); ++y) {
            for (std::int32_t x = 0; x < r->display.pixels.width(); ++x) {
                const float m = std::max({r->display.pixels.at(x, y, 0), r->display.pixels.at(x, y, 1),
                                          r->display.pixels.at(x, y, 2)});
                ASSERT_LE(m, 1.0f + 1e-5f);
            }
        }
    }
}

TEST(DevelopPipeline, ProxiesScaleTheRadiusAndFollowTheFullRender) {
    const auto scene = [](std::int32_t x, std::int32_t y) {
        const double v = 0.01 + 0.5 * (x / 256.0) * (x / 256.0) + (y > 96 ? 0.2 : 0.0);
        return std::array<double, 3>{0.5 * v, v, 0.6 * v};
    };
    StageOneImage full = interactive(ts::scene_mosaic(256, 192, scene));
    auto proxy = make_proxy(full, 64);
    ASSERT_TRUE(proxy) << proxy.error().describe();
    EXPECT_EQ(proxy->pixels.size(), (image::Size{64, 48}));
    EXPECT_DOUBLE_EQ(proxy->scale, 0.25);
    ASSERT_TRUE(proxy->downscale.has_value());
    EXPECT_EQ(proxy->downscale->from, (image::Size{256, 192}));
    // The same proxy straight from stage one.
    StageOneImage direct = interactive(ts::scene_mosaic(256, 192, scene), 64);
    EXPECT_TRUE(ts::bit_identical(direct.pixels, proxy->pixels));
    EXPECT_DOUBLE_EQ(direct.scale, 0.25);

    DevelopSettings s;
    s.highlights = -0.6;
    s.shadows = 0.6;
    s.contrast = 0.3;
    auto small = render_stage_two(*proxy, s);
    auto large = render_stage_two(full, s);
    ASSERT_TRUE(small && large);
    EXPECT_EQ(small->report.local_tone->radius, 1);  // 256 / 64 * 0.25
    EXPECT_EQ(large->report.local_tone->radius, 4);
    auto reduced = image::ImageBuffer<float>::create(64, 48, 3);
    ASSERT_TRUE(reduced);
    ASSERT_TRUE(box_downscale(large->display.pixels.const_view(), reduced->view()));
    double sum = 0.0;
    for (std::int32_t y = 0; y < 48; ++y) {
        for (std::int32_t x = 0; x < 64; ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                sum += std::abs(reduced->at(x, y, c) - small->display.pixels.at(x, y, c));
            }
        }
    }
    EXPECT_LT(sum / (64.0 * 48.0 * 3.0), 0.01);  // display-linear units
}

TEST(BoxDownscale, AveragesCoveredAreas) {
    // 5 -> 2 columns: weights 1, 1, 0.5 | 0.5, 1, 1 (over 2.5).
    auto src = image::ImageBuffer<float>::create(5, 3, 1);
    ASSERT_TRUE(src);
    for (std::int32_t y = 0; y < 3; ++y) {
        for (std::int32_t x = 0; x < 5; ++x) {
            src->at(x, y, 0) = static_cast<float>(1 + x + 10 * y);
        }
    }
    auto dst = image::ImageBuffer<float>::create(2, 1, 1);
    ASSERT_TRUE(dst);
    ASSERT_TRUE(box_downscale(src->const_view(), dst->view()));
    // Rows average to y = 1 (mean of 0, 1, 2): values 1 + x + 10.
    EXPECT_NEAR(dst->at(0, 0, 0), (11.0 + 12.0 + 0.5 * 13.0) / 2.5, 1e-5);
    EXPECT_NEAR(dst->at(1, 0, 0), (0.5 * 13.0 + 14.0 + 15.0) / 2.5, 1e-5);
    // Uniform stays uniform at any factor; integer factors give exact block means.
    auto uniform = image::ImageBuffer<float>::create(37, 23, 3);
    ASSERT_TRUE(uniform);
    for (std::int32_t y = 0; y < 23; ++y) {
        for (std::int32_t x = 0; x < 37; ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                uniform->at(x, y, c) = 0.25f + 0.25f * c;
            }
        }
    }
    auto small = image::ImageBuffer<float>::create(11, 7, 3);
    ASSERT_TRUE(small);
    ASSERT_TRUE(box_downscale(uniform->const_view(), small->view()));
    for (std::int32_t y = 0; y < 7; ++y) {
        for (std::int32_t x = 0; x < 11; ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                EXPECT_NEAR(small->at(x, y, c), 0.25f + 0.25f * c, 1e-6f);
            }
        }
    }
    EXPECT_EQ(proxy_size({8192, 5464}, 2048), (image::Size{2048, 1366}));
    EXPECT_EQ(proxy_size({5464, 8192}, 2048), (image::Size{1366, 2048}));
    EXPECT_EQ(proxy_size({100, 50}, 0), (image::Size{100, 50}));
    EXPECT_EQ(proxy_size({100, 50}, 200), (image::Size{100, 50}));
    EXPECT_EQ(local_tone_radius({8192, 5464}, 1.0), 128);
    EXPECT_EQ(local_tone_radius({8192, 5464}, 0.25), 32);
    EXPECT_EQ(local_tone_radius({32, 20}, 0.25), 1);
    ts::expect_error(box_downscale(small->const_view(), uniform->view()), ErrorCode::InvalidArgument,
                     "engine.develop.downscale");
}

TEST(DevelopPipeline, MonochromeRendersNeutralAndRefusesColourWhiteBalance) {
    auto decoded = ts::make_monochrome(20, 14, image::PixelRect{0, 0, 20, 14}, ts::simple_levels(512.0, 16896.0),
                                       [](std::int32_t x, std::int32_t, std::int32_t) { return 512.0 + 400.0 * x; });
    StageOneImage one = interactive(std::move(decoded));
    EXPECT_EQ(one.pixels.channels(), 1);
    auto r = render_stage_two(one, DevelopSettings{});
    ASSERT_TRUE(r) << r.error().describe();
    EXPECT_EQ(r->report.color.path, CalibrationPath::MonochromeNeutral);
    for (std::int32_t x = 0; x < 20; ++x) {
        EXPECT_NEAR(r->display.pixels.at(x, 5, 0), r->display.pixels.at(x, 5, 1), 1e-6f);
        EXPECT_NEAR(r->display.pixels.at(x, 5, 2), r->display.pixels.at(x, 5, 1), 1e-6f);
    }
    DevelopSettings kelvin;
    kelvin.white_balance = KelvinWhiteBalance{};
    ts::expect_error(render_stage_two(one, kelvin), ErrorCode::Unsupported, "engine.develop.white_balance");
}

TEST(DevelopPipeline, ReportsFailuresExplicitly) {
    StageOneImage one = interactive(textured_scene());
    DevelopSettings bad;
    bad.exposure_ev = 9.0;
    ts::expect_error(render_stage_two(one, bad), ErrorCode::InvalidArgument, "engine.develop.settings");
    ts::expect_error(render_develop(textured_scene(), bad), ErrorCode::InvalidArgument, "engine.develop.settings");

    // No calibration: Kelvin is unavailable on both paths.
    auto uncalibrated = textured_scene();
    uncalibrated.metadata.color.xyz_to_camera = {};
    DevelopSettings kelvin;
    kelvin.white_balance = KelvinWhiteBalance{5000.0, 0.0};
    ts::expect_error(render_develop(std::move(uncalibrated), kelvin), ErrorCode::Unsupported,
                     "engine.develop.white_balance");

    DevelopOptions tight;
    tight.memory_budget_bytes = 1024;
    ts::expect_error(render_develop(textured_scene(), DevelopSettings{}, tight), ErrorCode::LimitExceeded,
                     "engine.develop.memory");
    ts::expect_error(render_stage_one(textured_scene(), StageOneRequest{}, tight), ErrorCode::LimitExceeded,
                     "engine.develop.memory");
    DevelopSettings local;
    local.shadows = 0.5;
    ts::expect_error(render_stage_two(one, local, tight), ErrorCode::LimitExceeded, "engine.develop.memory");

    core::CancellationSource source;
    source.cancel();
    auto cancelled = render_stage_two(one, local, {}, source.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::Cancelled);
    auto cancelled_one = render_stage_one(textured_scene(), StageOneRequest{}, {}, source.token());
    ASSERT_FALSE(cancelled_one);
    EXPECT_EQ(cancelled_one.error().code, ErrorCode::Cancelled);

    ts::expect_error(make_proxy(one, 0), ErrorCode::InvalidArgument, "engine.develop.downscale");
    ts::expect_error(make_proxy(one, 48), ErrorCode::InvalidArgument, "engine.develop.downscale");
    StageOneImage empty;
    ts::expect_error(render_stage_two(empty, DevelopSettings{}), ErrorCode::InvalidArgument, "engine.develop.stage_two");
}

TEST(DevelopPipeline, MemoryEstimateCoversStageOneProxyAndExport) {
    const auto decoded = textured_scene(400, 300);
    StageOneRequest full_request;
    auto full = estimate_develop_memory(decoded.metadata, full_request);
    StageOneRequest proxy_request;
    proxy_request.proxy_long_edge = 100;
    auto proxy = estimate_develop_memory(decoded.metadata, proxy_request);
    ASSERT_TRUE(full && proxy);
    EXPECT_GE(full->stage_one_peak, full->stage_one_bytes);
    EXPECT_GE(full->stage_one_bytes, 400u * 300u * 3u * 4u);
    EXPECT_LT(proxy->stage_one_bytes, full->stage_one_bytes / 10);
    EXPECT_GE(proxy->stage_one_peak, full->render.rgb_bytes);
    EXPECT_GE(full->export_peak, full->stage_one_peak);
    EXPECT_GT(full->stage_two_bytes, 0u);
}

}  // namespace
}  // namespace lucent::engine
