// The develop pipeline with DevelopSettings v2 (develop.hpp; ADR 0015) on synthetic decoded data:
// the operation records, version-1 settings, masks that stay attached to the content when the
// crop, the straighten angle or the quarter turns change, masks that change only their area,
// proxy against full resolution with masks and denoising, the brush raster and stage-two caches,
// memory estimates and monochrome data.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/engine/develop.hpp>
#include <lucent/engine/display_rendering.hpp>
#include <lucent/engine/orientation.hpp>

#include "develop_test_support.hpp"

namespace lucent::engine {
namespace {

namespace ts = test_support;

constexpr std::array<double, 3> kAsShot = {2.0, 1.0, 1.5};

raw::DecodedRaw grey_scene(std::int32_t w, std::int32_t h, const std::function<double(std::int32_t, std::int32_t)>& rho) {
    return ts::scene_mosaic(w, h, [&](std::int32_t x, std::int32_t y) {
        const double v = rho(x, y);
        return std::array<double, 3>{v / kAsShot[0], v / kAsShot[1], v / kAsShot[2]};
    });
}

// A textured, coloured scene without saturated sites (deterministic "noise" included).
raw::DecodedRaw textured_scene(std::int32_t w, std::int32_t h) {
    return ts::scene_mosaic(w, h, [](std::int32_t x, std::int32_t y) {
        const double base = 0.03 + 0.25 * (0.5 + 0.5 * std::sin(0.07 * x) * std::cos(0.05 * y)) +
                            0.01 * ((x * 7 + y * 13) % 5);
        const double n = 0.03 * std::sin(12.9898 * x + 78.233 * y);
        return std::array<double, 3>{0.6 * base * (1.0 + n), base, 0.35 * base * (1.0 - n)};
    });
}

StageOneImage interactive(raw::DecodedRaw&& decoded, std::int32_t proxy = 0) {
    StageOneRequest request;
    request.proxy_long_edge = proxy;
    auto one = render_stage_one(std::move(decoded), request);
    EXPECT_TRUE(one) << one.error().describe();
    return std::move(*one);
}

BrushStroke stroke(std::vector<MaskPoint> points, double radius, double hardness, double flow) {
    BrushStroke s;
    s.points = std::move(points);
    s.radius = radius;
    s.hardness = hardness;
    s.flow = flow;
    return s;
}

// Three masks of every kind with several controls, and chroma denoising.
DevelopSettings masked_settings() {
    DevelopSettings s;
    s.chroma_denoise.strength = 0.8;
    s.chroma_denoise.radius = 8.0;
    LocalAdjustment radial;
    radial.id = 1;
    RadialGradientMask r;
    r.center = {0.35, 0.45};
    r.radius_x = 0.2;
    r.radius_y = 0.12;
    r.angle_degrees = 20.0;
    r.feather = 0.6;
    radial.shape = r;
    radial.controls.exposure_ev = 0.7;
    radial.controls.saturation = 0.3;
    LocalAdjustment linear;
    linear.id = 2;
    linear.shape = LinearGradientMask{{0.5, 0.0}, {0.5, 0.45}, 1.0};
    linear.controls.highlights = -0.5;
    linear.controls.temperature = 0.3;
    LocalAdjustment brush;
    brush.id = 7;
    brush.shape = BrushMask{{stroke({{0.1, 0.8}, {0.4, 0.7}, {0.8, 0.85}}, 0.06, 0.4, 0.9)}};
    brush.controls.shadows = 0.4;
    brush.controls.exposure_ev = -0.3;
    brush.controls.contrast = 0.25;
    s.local_adjustments = {radial, linear, brush};
    return s;
}

TEST(DevelopPipelineV2, RecordsTheVersion2OperationsInOrder) {
    StageOneImage one = interactive(textured_scene(64, 48));
    auto defaults = render_stage_two(one, DevelopSettings{});
    ASSERT_TRUE(defaults) << defaults.error().describe();
    const std::vector<DevelopOperation> order = {
        DevelopOperation::WhiteBalance,  DevelopOperation::CameraColor, DevelopOperation::WorkingSpace,
        DevelopOperation::Exposure,      DevelopOperation::ChromaDenoise, DevelopOperation::LocalTone,
        DevelopOperation::LocalAdjustments, DevelopOperation::Contrast,  DevelopOperation::ToneCurve,
        DevelopOperation::Color,         DevelopOperation::BaseLook,    DevelopOperation::Geometry};
    const DevelopReport& report = defaults->report;
    ASSERT_EQ(report.operations.size(), order.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        EXPECT_EQ(report.operations[i].operation, order[i]) << i;
    }
    for (const std::size_t skipped : {4u, 6u, 11u}) {
        EXPECT_FALSE(report.operations[skipped].applied) << to_string(order[skipped]);
    }
    EXPECT_FALSE(report.chroma_denoise.has_value());
    EXPECT_FALSE(report.local_adjustments.has_value());
    EXPECT_FALSE(report.geometry.has_value());
    EXPECT_EQ(report.upright_size, one.pixels.size());
    EXPECT_EQ(report.output_size, one.pixels.size());
    EXPECT_EQ(report.pipeline_version, 3u);
    EXPECT_EQ(to_string(DevelopOperation::ChromaDenoise), "chroma_denoise");
    EXPECT_EQ(to_string(DevelopOperation::LocalAdjustments), "local_adjustments");
    EXPECT_EQ(to_string(DevelopOperation::Geometry), "geometry");

    DevelopSettings all = masked_settings();
    all.geometry.straighten_degrees = 4.0;
    all.geometry.crop = largest_inside_crop(4.0, one.pixels.size());
    all.geometry.quarter_turns = 1;
    auto applied = render_stage_two(one, all);
    ASSERT_TRUE(applied) << applied.error().describe();
    for (const std::size_t index : {4u, 6u, 11u}) {
        EXPECT_TRUE(applied->report.operations[index].applied) << to_string(order[index]);
    }
    ASSERT_TRUE(applied->report.chroma_denoise.has_value());
    EXPECT_EQ(applied->report.chroma_denoise->radius, 8);
    ASSERT_TRUE(applied->report.local_adjustments.has_value());
    EXPECT_EQ(applied->report.local_adjustments->masks, 3u);
    EXPECT_EQ(applied->report.local_adjustments->rasterized, 1u);
    EXPECT_EQ(applied->report.local_adjustments->tone_radius, local_tone_radius(one.full_size, 1.0));
    ASSERT_TRUE(applied->report.geometry.has_value());
    EXPECT_TRUE(applied->report.geometry->resampled);
    EXPECT_EQ(applied->report.geometry->uncovered_pixels, 0u);
    const auto mapping = GeometryMapping::create(all.geometry, applied->report.upright_size);
    ASSERT_TRUE(mapping);
    EXPECT_EQ(applied->report.output_size, mapping->output_size());
    EXPECT_EQ(applied->display.pixels.size(), mapping->output_size());
    EXPECT_EQ(applied->report.upright_size, one.pixels.size());
    EXPECT_EQ(applied->display.encoding, display_linear_rec2020());
    EXPECT_TRUE(ts::all_finite(applied->display.pixels));
}

TEST(DevelopPipelineV2, Version1SettingsRenderAsTheirUpgrade) {
    StageOneImage one = interactive(textured_scene(48, 36));
    DevelopSettings v1;
    v1.version = 1;
    v1.exposure_ev = 0.4;
    v1.shadows = 0.3;
    v1.tone_curve.points = {{0.0, 0.0}, {0.5, 0.55}, {1.0, 1.0}};
    auto upgraded = upgrade_develop_settings(v1);
    ASSERT_TRUE(upgraded);
    auto a = render_stage_two(one, v1);
    auto b = render_stage_two(one, *upgraded);
    ASSERT_TRUE(a && b) << (a ? b.error().describe() : a.error().describe());
    EXPECT_TRUE(ts::bit_identical(a->display.pixels, b->display.pixels));
    EXPECT_EQ(a->report.settings.version, 1u);  // the report keeps the settings as given
    auto exported = render_develop(textured_scene(48, 36), v1);
    ASSERT_TRUE(exported) << exported.error().describe();
    // A version-1 value with a version-2 member is refused before anything is rendered.
    v1.geometry.quarter_turns = 1;
    ts::expect_error(render_stage_two(one, v1), ErrorCode::InvalidArgument, "engine.develop.settings");
}

TEST(DevelopPipelineV2, MasksStayOnTheContentWhenTheCropChanges) {
    // Every operation before the geometry works on the upright image, so a crop selects pixels of
    // the same rendering: the cropped output is bit-identical to the matching part of the uncropped
    // one, masks, denoising and the guided-filter local tone included.
    StageOneImage one = interactive(textured_scene(96, 64));
    DevelopSettings s = masked_settings();
    s.shadows = 0.3;
    auto whole = render_stage_two(one, s);
    ASSERT_TRUE(whole) << whole.error().describe();
    s.geometry.crop = CropRect{0.25, 0.1, 0.9, 0.8};  // pixels 24..86 x 6..51
    auto cropped = render_stage_two(one, s);
    ASSERT_TRUE(cropped) << cropped.error().describe();
    ASSERT_EQ(cropped->display.pixels.size(), (image::Size{62, 45}));
    for (std::int32_t y = 0; y < 45; ++y) {
        for (std::int32_t x = 0; x < 62; ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                ASSERT_EQ(cropped->display.pixels.at(x, y, c), whole->display.pixels.at(x + 24, y + 6, c))
                    << x << "," << y;
            }
        }
    }
    // Turning the cropped image turns the same pixels.
    s.geometry.quarter_turns = 3;
    s.geometry.flip_horizontal = true;
    auto turned = render_stage_two(one, s);
    ASSERT_TRUE(turned);
    auto expected = apply_orientation(std::move(cropped->display.pixels), user_orientation(s.geometry));
    ASSERT_TRUE(expected);
    EXPECT_TRUE(ts::bit_identical(turned->display.pixels, *expected));
}

TEST(DevelopPipelineV2, MasksFollowStraighteningAndQuarterTurns) {
    // A small bright radial mask on a flat grey image: its centre in the output is where the
    // mapping puts the mask's normalised centre, for several geometries.
    StageOneImage one = interactive(grey_scene(160, 120, [](std::int32_t, std::int32_t) { return 0.18; }));
    DevelopSettings s;
    LocalAdjustment spot;
    RadialGradientMask r;
    r.center = {0.3, 0.4};
    r.radius_x = 0.04;
    r.radius_y = 0.04;
    r.feather = 0.5;
    spot.shape = r;
    spot.controls.exposure_ev = 2.0;
    s.local_adjustments = {spot};
    auto plain = render_stage_two(one, DevelopSettings{});
    ASSERT_TRUE(plain);
    const float background = plain->display.pixels.at(5, 5, 1);
    std::vector<GeometrySetting> geometries(4);
    geometries[1].straighten_degrees = 15.0;
    geometries[1].crop = largest_inside_crop(15.0, one.pixels.size());
    geometries[2] = geometries[1];
    geometries[2].quarter_turns = 1;
    geometries[2].flip_horizontal = true;
    geometries[3].straighten_degrees = -33.0;
    geometries[3].crop = largest_inside_crop(-33.0, one.pixels.size(), 1.0);
    geometries[3].quarter_turns = 2;
    for (const GeometrySetting& g : geometries) {
        SCOPED_TRACE(testing::Message() << g.straighten_degrees << " deg, " << g.quarter_turns << " turns, flip "
                                        << g.flip_horizontal);
        s.geometry = g;
        auto result = render_stage_two(one, s);
        ASSERT_TRUE(result) << result.error().describe();
        const image::ImageBuffer<float>& px = result->display.pixels;
        double sum = 0.0;
        double sx = 0.0;
        double sy = 0.0;
        for (std::int32_t y = 0; y < px.height(); ++y) {
            for (std::int32_t x = 0; x < px.width(); ++x) {
                const double excess = std::max(0.0, static_cast<double>(px.at(x, y, 1) - background) - 1e-4);
                sum += excess;
                sx += excess * (x + 0.5);
                sy += excess * (y + 0.5);
            }
        }
        ASSERT_GT(sum, 0.0);
        auto mapping = GeometryMapping::create(g, result->report.upright_size);
        ASSERT_TRUE(mapping);
        const image::PointD expected = mapping->output_from_normalized(r.center);
        EXPECT_NEAR(sx / sum, expected.x, 0.2);
        EXPECT_NEAR(sy / sum, expected.y, 0.2);
        // And back: the brightest output pixel maps to the mask centre.
        const MaskPoint back = mapping->normalized_from_output(image::PointD{sx / sum, sy / sum});
        EXPECT_NEAR(back.x, r.center.x, 0.2 / 160.0);
        EXPECT_NEAR(back.y, r.center.y, 0.2 / 120.0);
    }
}

TEST(DevelopPipelineV2, MasksChangeOnlyTheirArea) {
    StageOneImage one = interactive(textured_scene(80, 60));
    DevelopSettings s;
    s.contrast = 0.2;  // per-pixel operations around the mask
    s.saturation = 0.1;
    auto plain = render_stage_two(one, s);
    ASSERT_TRUE(plain);
    LocalAdjustment spot;
    RadialGradientMask r;
    r.center = {0.6, 0.4};
    r.radius_x = 0.1;
    r.radius_y = 0.15;
    spot.shape = r;
    spot.controls.exposure_ev = -1.0;
    spot.controls.tint = 0.5;
    s.local_adjustments = {spot};
    auto masked = render_stage_two(one, s);
    ASSERT_TRUE(masked);
    auto weights = mask_weights(spot, one.pixels.size());
    ASSERT_TRUE(weights);
    std::uint64_t changed = 0;
    for (std::int32_t y = 0; y < 60; ++y) {
        for (std::int32_t x = 0; x < 80; ++x) {
            const bool inside = weights->at(x, y, 0) > 0.0f;
            for (std::int32_t c = 0; c < 3; ++c) {
                if (!inside) {
                    ASSERT_EQ(masked->display.pixels.at(x, y, c), plain->display.pixels.at(x, y, c)) << x << "," << y;
                }
            }
            changed += inside && masked->display.pixels.at(x, y, 1) != plain->display.pixels.at(x, y, 1) ? 1u : 0u;
        }
    }
    EXPECT_GT(changed, 100u);
    std::uint64_t positive = 0;
    for (std::int32_t y = 0; y < 60; ++y) {
        for (std::int32_t x = 0; x < 80; ++x) {
            positive += weights->at(x, y, 0) > 0.0f ? 1u : 0u;
        }
    }
    EXPECT_EQ(masked->report.local_adjustments->changed_pixels, positive);
}

TEST(DevelopPipelineV2, ProxyFollowsTheFullRenderWithMasksAndDenoising) {
    // Proxy (scale 0.25) against the full-resolution render box-downscaled to the proxy grid, with
    // three masks and chroma denoising (radius 8 -> 2): downscaling does not commute with the
    // guided filters, the curves or the mask edges; display-linear differences stay small.
    StageOneImage full = interactive(textured_scene(512, 384));
    auto proxy = make_proxy(full, 128);
    ASSERT_TRUE(proxy);
    const DevelopSettings s = masked_settings();
    auto large = render_stage_two(full, s);
    auto small = render_stage_two(*proxy, s);
    ASSERT_TRUE(large && small);
    EXPECT_EQ(small->report.chroma_denoise->radius, 2);
    EXPECT_EQ(large->report.chroma_denoise->radius, 8);
    auto reduced = image::ImageBuffer<float>::create(128, 96, 3);
    ASSERT_TRUE(reduced);
    ASSERT_TRUE(box_downscale(large->display.pixels.const_view(), reduced->view()));
    std::vector<double> differences;
    for (std::int32_t y = 0; y < 96; ++y) {
        for (std::int32_t x = 0; x < 128; ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                differences.push_back(std::abs(reduced->at(x, y, c) - small->display.pixels.at(x, y, c)));
            }
        }
    }
    double mean = 0.0;
    for (const double d : differences) {
        mean += d;
    }
    mean /= static_cast<double>(differences.size());
    std::sort(differences.begin(), differences.end());
    const double p99 = differences[static_cast<std::size_t>(0.99 * static_cast<double>(differences.size() - 1))];
    std::cout << "proxy vs full (display-linear): mean " << mean << " p99 " << p99 << " max " << differences.back()
              << "\n";
    EXPECT_LT(mean, 0.005);
    EXPECT_LT(p99, 0.03);
}

TEST(DevelopPipelineV2, TheMaskCacheServesLaterRenders) {
    StageOneImage one = interactive(textured_scene(64, 48));
    DevelopOptions options;
    options.mask_cache = std::make_shared<MaskRasterCache>();
    DevelopSettings s = masked_settings();
    auto first = render_stage_two(one, s, options);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->report.local_adjustments->rasterized, 1u);
    s.local_adjustments[2].controls.exposure_ev = 0.6;  // a slider move: same shape
    auto second = render_stage_two(one, s, options);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->report.local_adjustments->rasterized, 0u);
    EXPECT_EQ(second->report.local_adjustments->cache_hits, 1u);
    auto uncached = render_stage_two(one, s);
    ASSERT_TRUE(uncached);
    EXPECT_TRUE(ts::bit_identical(second->display.pixels, uncached->display.pixels));
    EXPECT_GT(options.mask_cache->stats().bytes, 0u);
}

TEST(DevelopPipelineV2, TheStageTwoCacheReusesTheDenoisedImageExactly) {
    StageOneImage one = interactive(textured_scene(96, 64));
    EXPECT_NE(one.id, 0u);
    auto proxy = make_proxy(one, 48);
    ASSERT_TRUE(proxy);
    EXPECT_NE(proxy->id, 0u);
    EXPECT_NE(proxy->id, one.id);
    DevelopOptions options;
    options.stage_two_cache = std::make_shared<StageTwoCache>();
    const StageTwoCache& cache = *options.stage_two_cache;
    DevelopSettings s = masked_settings();
    auto first = render_stage_two(one, s, options);
    ASSERT_TRUE(first) << first.error().describe();
    EXPECT_EQ(cache.stats().misses, 1u);
    EXPECT_EQ(cache.stats().bytes, one.pixels.byte_size());
    // Controls after the denoising: a hit, and exactly the pixels of a render without the cache.
    s.contrast = 0.3;
    s.shadows = 0.2;
    s.local_adjustments[0].controls.exposure_ev = -0.2;
    s.geometry.quarter_turns = 1;
    auto second = render_stage_two(one, s, options);
    ASSERT_TRUE(second);
    EXPECT_EQ(cache.stats().hits, 1u);
    auto plain = render_stage_two(one, s);
    ASSERT_TRUE(plain);
    EXPECT_TRUE(ts::bit_identical(second->display.pixels, plain->display.pixels));
    ASSERT_TRUE(second->report.chroma_denoise.has_value());
    EXPECT_EQ(second->report.chroma_denoise->bands, plain->report.chroma_denoise->bands);
    // Inputs of the fused pass or the denoising: misses.
    std::vector<DevelopSettings> changed(3, s);
    changed[0].exposure_ev = 0.1;
    changed[1].white_balance = KelvinWhiteBalance{4500.0, 3.0};
    changed[2].chroma_denoise.radius = 3.0;
    for (const DevelopSettings& c : changed) {
        const std::uint64_t misses = cache.stats().misses;
        auto r = render_stage_two(one, c, options);
        ASSERT_TRUE(r);
        EXPECT_EQ(cache.stats().misses, misses + 1);
        auto reference = render_stage_two(one, c);
        ASSERT_TRUE(reference);
        EXPECT_TRUE(ts::bit_identical(r->display.pixels, reference->display.pixels));
    }
    // Another stage-one image is another key.
    const std::uint64_t misses = cache.stats().misses;
    ASSERT_TRUE(render_stage_two(*proxy, s, options));
    EXPECT_EQ(cache.stats().misses, misses + 1);
    // Not used: without denoising, for an image without an id, by the consuming overload.
    const StageTwoCache::Stats before = cache.stats();
    DevelopSettings quiet = s;
    quiet.chroma_denoise = ChromaDenoiseSetting{};
    ASSERT_TRUE(render_stage_two(one, quiet, options));
    StageOneImage anonymous = interactive(textured_scene(96, 64));
    anonymous.id = 0;
    ASSERT_TRUE(render_stage_two(anonymous, s, options));
    ASSERT_TRUE(render_stage_two(std::move(anonymous), s, options));
    EXPECT_EQ(cache.stats().hits, before.hits);
    EXPECT_EQ(cache.stats().misses, before.misses);
    // An image above the limit is not kept.
    DevelopOptions small;
    small.stage_two_cache = std::make_shared<StageTwoCache>(1024);
    ASSERT_TRUE(render_stage_two(one, s, small));
    EXPECT_EQ(small.stage_two_cache->stats().bytes, 0u);
    options.stage_two_cache->clear();
    EXPECT_EQ(cache.stats().bytes, 0u);
}

TEST(DevelopPipelineV2, MemoryEstimatesCoverTheNewOperations) {
    const raw::DecodedRaw decoded = textured_scene(320, 240);
    const StageOneRequest request;
    auto unknown = estimate_develop_memory(decoded.metadata, request);
    auto plain = estimate_develop_memory(decoded.metadata, request, DevelopSettings{});
    ASSERT_TRUE(unknown && plain);
    const std::uint64_t rgb = static_cast<std::uint64_t>(320 * 240) * 3 * sizeof(float);
    EXPECT_GE(plain->stage_two_bytes, rgb);
    EXPECT_LE(plain->stage_two_bytes, unknown->stage_two_bytes);  // no settings: the worst case
    DevelopSettings brushes;
    for (std::uint32_t id = 1; id <= 3; ++id) {
        LocalAdjustment a;
        a.id = id;
        a.shape = BrushMask{{stroke({{0.5, 0.5}}, 0.1, 0.5, 1.0)}};
        a.controls.shadows = 0.5;
        brushes.local_adjustments.push_back(a);
    }
    auto masked = estimate_develop_memory(decoded.metadata, request, brushes);
    ASSERT_TRUE(masked);
    EXPECT_GE(masked->export_peak, masked->stage_one_bytes);
    // An upper bound of what the render allocates: its brush rasters (over the strokes' extent, not
    // the image) and the base layer over the masks' support with its halo.
    StageOneImage one = interactive(textured_scene(320, 240));
    auto rendered = render_stage_two(one, brushes);
    ASSERT_TRUE(rendered) << rendered.error().describe();
    ASSERT_TRUE(rendered->report.local_adjustments);
    const LocalAdjustmentsRecord& record = *rendered->report.local_adjustments;
    EXPECT_GT(record.raster_bytes, 0u);
    EXPECT_LT(record.raster_bytes, 3 * static_cast<std::uint64_t>(320 * 240) * sizeof(float));
    EXPECT_GE(masked->stage_two_bytes,
              rgb + record.raster_bytes + 3 * static_cast<std::uint64_t>(record.tone_region.size().area()) * sizeof(float));
    EXPECT_LT(masked->stage_two_bytes, unknown->stage_two_bytes + rgb);
    DevelopOptions tight;
    tight.memory_budget_bytes = rgb + 1024;
    ts::expect_error(render_stage_two(one, brushes, tight), ErrorCode::LimitExceeded, "engine.develop.memory");
}

// Many small brush masks on a 45 MP photo (ADR 0015 memory): each raster is estimated over its
// strokes, so the estimate stays far below the default budget (a full plane per mask refused 18
// such masks with shadows).
TEST(DevelopPipelineV2, ManySmallBrushMasksFitTheDefaultBudgetOnALargePhoto) {
    raw::DecodedRaw decoded = textured_scene(64, 48);
    raw::RawMetadata metadata = decoded.metadata;
    metadata.geometry.raw_width = 8192;
    metadata.geometry.raw_height = 5464;
    metadata.geometry.visible_area = image::PixelRect{0, 0, 8192, 5464};
    DevelopSettings settings;
    settings.shadows = 0.2;
    settings.chroma_denoise.strength = 0.5;
    for (std::uint32_t id = 1; id <= kMaxLocalAdjustments; ++id) {
        const double x = 0.04 + 0.029 * id;
        LocalAdjustment a;
        a.id = id;
        a.shape = BrushMask{{stroke({{x, 0.5}, {x + 0.005, 0.505}}, 0.004, 0.5, 1.0)}};
        a.controls.shadows = 0.3;
        settings.local_adjustments.push_back(a);
    }
    ASSERT_TRUE(settings.validate());
    StageOneRequest interactive_request;
    auto interactive_estimate = estimate_develop_memory(metadata, interactive_request, settings);
    StageOneRequest export_request;
    export_request.kind = StageOneKind::Export;
    auto export_estimate = estimate_develop_memory(metadata, export_request, settings);
    ASSERT_TRUE(interactive_estimate && export_estimate);
    const DevelopOptions defaults;
    const std::uint64_t rgb = 8192ull * 5464ull * 3ull * sizeof(float);
    // Stage two: its RGB image plus the largest operation, here the global shadows' three planes:
    // the masks' rasters and base layer are smaller and do not raise it.
    DevelopSettings unmasked = settings;
    unmasked.local_adjustments.clear();
    auto unmasked_estimate = estimate_develop_memory(metadata, interactive_request, unmasked);
    ASSERT_TRUE(unmasked_estimate);
    EXPECT_EQ(interactive_estimate->stage_two_bytes, unmasked_estimate->stage_two_bytes);
    EXPECT_LE(interactive_estimate->stage_two_bytes, rgb + rgb);
    EXPECT_LT(export_estimate->export_peak, defaults.memory_budget_bytes / 2);
    // Without the global shadows the masks' own working memory shows: a fraction of one plane.
    DevelopSettings local_only = settings;
    local_only.shadows = 0.0;
    local_only.chroma_denoise = ChromaDenoiseSetting{};
    auto local_estimate = estimate_develop_memory(metadata, interactive_request, local_only);
    ASSERT_TRUE(local_estimate);
    EXPECT_LT(local_estimate->stage_two_bytes, rgb + rgb / 6);
}

TEST(DevelopPipelineV2, MonochromeDataSkipsChromaDenoising) {
    auto decoded = ts::make_monochrome(20, 14, image::PixelRect{0, 0, 20, 14}, ts::simple_levels(512.0, 16896.0),
                                       [](std::int32_t x, std::int32_t, std::int32_t) { return 512.0 + 400.0 * x; });
    StageOneImage one = interactive(std::move(decoded));
    DevelopSettings s;
    s.chroma_denoise.strength = 1.0;
    auto denoised = render_stage_two(one, s);
    auto plain = render_stage_two(one, DevelopSettings{});
    ASSERT_TRUE(denoised && plain);
    EXPECT_FALSE(denoised->report.operations[4].applied);
    EXPECT_TRUE(ts::bit_identical(denoised->display.pixels, plain->display.pixels));
}

}  // namespace
}  // namespace lucent::engine
