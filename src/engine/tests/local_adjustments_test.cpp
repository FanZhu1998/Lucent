// Unit tests for masks and local adjustments (local_adjustments.hpp): rasterisation against the
// definitions (radial, linear, brush with hardness, flow, erase and non-accumulating strokes),
// invert and opacity, proxy against full-resolution rasters, local controls against their
// definitions (exposure, saturation, white shift, contrast, highlights/shadows against the global
// operation), untouched pixels outside masks, overlapping sums, the raster cache, outlines,
// determinism and failures.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numbers>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/core/cancellation.hpp>
#include <lucent/engine/develop.hpp>
#include <lucent/engine/display_rendering.hpp>
#include <lucent/engine/local_adjustments.hpp>

#include "develop_test_support.hpp"

namespace lucent::engine {
namespace {

namespace ts = test_support;
constexpr const char* kWhere = "engine.develop.masks";

double smoothstep(double lo, double hi, double x) {
    const double u = std::clamp((x - lo) / (hi - lo), 0.0, 1.0);
    return u * u * (3.0 - 2.0 * u);
}

template <class F>
ColorImage make_image(std::int32_t w, std::int32_t h, F f) {
    auto buffer = image::ImageBuffer<float>::create(w, h, 3);
    EXPECT_TRUE(buffer);
    for (std::int32_t y = 0; y < h; ++y) {
        for (std::int32_t x = 0; x < w; ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                buffer->at(x, y, c) = static_cast<float>(f(x, y, c));
            }
        }
    }
    return ColorImage{std::move(*buffer), image::ColorEncoding::scene_linear_rec2020_d65()};
}

ColorImage clone(const ColorImage& image) {
    auto pixels = image.pixels.clone();
    EXPECT_TRUE(pixels);
    return ColorImage{std::move(*pixels), image.encoding};
}

// A textured, coloured scene-linear image (no pixel at or below 0).
ColorImage textured(std::int32_t w, std::int32_t h) {
    return make_image(w, h, [](std::int32_t x, std::int32_t y, std::int32_t c) {
        const double base = 0.05 + 0.3 * (0.5 + 0.5 * std::sin(0.11 * x + 0.07 * y));
        return base * (c == 0 ? 1.2 : c == 1 ? 1.0 : 0.7) * (1.0 + 0.05 * ((x * 3 + y * 5) % 7) / 7.0);
    });
}

LocalAdjustment adjustment(std::uint32_t id, MaskShape shape) {
    LocalAdjustment a;
    a.id = id;
    a.shape = std::move(shape);
    return a;
}

BrushStroke stroke(std::vector<MaskPoint> points, double radius, double hardness, double flow,
                   BrushMode mode = BrushMode::Add) {
    BrushStroke s;
    s.points = std::move(points);
    s.radius = radius;
    s.hardness = hardness;
    s.flow = flow;
    s.mode = mode;
    return s;
}

image::ImageBuffer<float> weights(const LocalAdjustment& a, image::Size size) {
    auto w = mask_weights(a, size);
    EXPECT_TRUE(w) << w.error().describe();
    return std::move(*w);
}

// ---- Rasterisation ------------------------------------------------------------------------------------

TEST(MaskRaster, RadialCoverageFollowsTheDefinition) {
    const image::Size size{200, 100};  // long edge 200
    RadialGradientMask m;
    m.center = {0.5, 0.5};
    m.radius_x = 0.2;  // 40 px
    m.radius_y = 0.1;  // 20 px
    m.feather = 0.5;
    for (const double angle : {0.0, 30.0, -90.0}) {
        m.angle_degrees = angle;
        auto raster = rasterize_mask(m, size);
        ASSERT_TRUE(raster) << raster.error().describe();
        const double a = angle * std::numbers::pi / 180.0;
        double worst = 0.0;
        for (std::int32_t y = 0; y < size.height; ++y) {
            for (std::int32_t x = 0; x < size.width; ++x) {
                const double dx = x + 0.5 - 100.0;
                const double dy = y + 0.5 - 50.0;
                // Counter-clockwise as displayed: the first axis is (cos a, -sin a).
                const double u = dx * std::cos(a) - dy * std::sin(a);
                const double v = dx * std::sin(a) + dy * std::cos(a);
                const double rho = std::hypot(u / 40.0, v / 20.0);
                const double expected = 1.0 - smoothstep(0.5, 1.0, rho);
                worst = std::max(worst, std::abs(raster->at(x, y) - expected));
            }
        }
        EXPECT_LT(worst, 1e-6) << angle;
    }
    // The bounds hold the whole support and the raster stores only them.
    m.angle_degrees = 0.0;
    auto raster = rasterize_mask(m, size);
    ASSERT_TRUE(raster);
    EXPECT_LE(raster->bounds.width, 84);
    EXPECT_LE(raster->bounds.height, 44);
    EXPECT_EQ(raster->coverage.size(), raster->bounds.size());
    EXPECT_EQ(raster->at(0, 0), 0.0f);
}

TEST(MaskRaster, LinearCoverageFollowsTheDefinition) {
    const image::Size size{200, 100};
    LinearGradientMask m;
    m.start = {0.25, 0.5};  // x = 50 px
    m.end = {0.75, 0.5};    // x = 150 px
    m.feather = 1.0;
    auto raster = rasterize_mask(m, size);
    ASSERT_TRUE(raster);
    for (std::int32_t x = 0; x < size.width; ++x) {
        const double t = (x + 0.5 - 50.0) / 100.0;
        ASSERT_NEAR(raster->at(x, 37), 1.0 - smoothstep(0.0, 1.0, t), 1e-6) << x;
    }
    EXPECT_EQ(raster->at(10, 5), 1.0f);  // the start side
    EXPECT_EQ(raster->at(190, 5), 0.0f);
    // A hard edge at the middle line, one pixel wide.
    m.feather = 0.0;
    raster = rasterize_mask(m, size);
    ASSERT_TRUE(raster);
    EXPECT_EQ(raster->at(98, 50), 1.0f);
    EXPECT_NEAR(raster->at(99, 50), 1.0 - smoothstep(-0.5, 0.5, -0.5), 1e-6);
    EXPECT_NEAR(raster->at(100, 50), 1.0 - smoothstep(-0.5, 0.5, 0.5), 1e-6);
    EXPECT_EQ(raster->at(101, 50), 0.0f);
    // A diagonal gradient is perpendicular in pixels (the image is not square).
    m.start = {0.0, 0.0};
    m.end = {0.5, 1.0};  // (100, 100) px: the band runs at 45 degrees
    m.feather = 1.0;
    raster = rasterize_mask(m, size);
    ASSERT_TRUE(raster);
    EXPECT_NEAR(raster->at(60, 20), raster->at(20, 60), 1e-6);
}

TEST(MaskRaster, BrushStrokesFollowHardnessFlowAndMode) {
    const image::Size size{100, 100};  // radius 0.05 = 5 px
    BrushMask brush;
    brush.strokes.push_back(stroke({{0.2, 0.505}, {0.8, 0.505}}, 0.05, 0.5, 1.0));  // y = 50.5: row 50's centre
    auto raster = rasterize_mask(brush, size);
    ASSERT_TRUE(raster) << raster.error().describe();
    EXPECT_FLOAT_EQ(raster->at(50, 50), 1.0f);  // on the line
    EXPECT_FLOAT_EQ(raster->at(50, 48), 1.0f);  // 2 px: inside h R = 2.5
    // 4 px from the line: 1 - smoothstep(2.5, 5, 4).
    EXPECT_NEAR(raster->at(50, 54), 1.0 - smoothstep(2.5, 5.0, 4.0), 1e-6);
    EXPECT_EQ(raster->at(50, 56), 0.0f);  // beyond R
    EXPECT_EQ(raster->at(10, 50), 0.0f);  // beyond the end cap
    EXPECT_NEAR(raster->at(15, 50), 1.0 - smoothstep(2.5, 5.0, 4.5), 1e-6);  // round cap
    // Flow scales the alpha.
    brush.strokes[0].flow = 0.5;
    raster = rasterize_mask(brush, size);
    ASSERT_TRUE(raster);
    EXPECT_FLOAT_EQ(raster->at(50, 50), 0.5f);
    // Segments of one stroke do not accumulate: a stroke going back and forth over itself.
    brush.strokes[0].points = {{0.2, 0.505}, {0.8, 0.505}, {0.3, 0.505}, {0.7, 0.505}};
    raster = rasterize_mask(brush, size);
    ASSERT_TRUE(raster);
    EXPECT_FLOAT_EQ(raster->at(50, 50), 0.5f);
    // A second stroke over it adds: 0.5 + 0.5 (1 - 0.5) = 0.75.
    brush.strokes.push_back(brush.strokes[0]);
    raster = rasterize_mask(brush, size);
    ASSERT_TRUE(raster);
    EXPECT_FLOAT_EQ(raster->at(50, 50), 0.75f);
    // An erase stroke across it removes coverage: 0.75 (1 - 1) = 0.
    brush.strokes.push_back(stroke({{0.505, 0.2}, {0.505, 0.8}}, 0.03, 1.0, 1.0, BrushMode::Erase));
    raster = rasterize_mask(brush, size);
    ASSERT_TRUE(raster);
    EXPECT_EQ(raster->at(50, 50), 0.0f);
    EXPECT_FLOAT_EQ(raster->at(40, 50), 0.75f);  // away from the erase stroke
    // A single point is a round dab.
    BrushMask dab{{stroke({{0.5, 0.5}}, 0.1, 0.0, 1.0)}};
    raster = rasterize_mask(dab, size);
    ASSERT_TRUE(raster);
    EXPECT_NEAR(raster->at(55, 49), 1.0 - smoothstep(0.0, 10.0, std::hypot(5.5, 0.5)), 1e-6);
    EXPECT_NEAR(raster->at(49, 55), raster->at(55, 49), 1e-6);
    // An empty brush and an erase-only brush cover nothing.
    raster = rasterize_mask(BrushMask{}, size);
    ASSERT_TRUE(raster);
    EXPECT_TRUE(raster->bounds.empty());
    raster = rasterize_mask(BrushMask{{stroke({{0.5, 0.5}}, 0.1, 0.5, 1.0, BrushMode::Erase)}}, size);
    ASSERT_TRUE(raster);
    EXPECT_TRUE(raster->bounds.empty());
}

TEST(MaskRaster, HardEdgesAreAntialiasedOverOnePixel) {
    const image::Size size{100, 100};
    // Brush of hardness 1 and radius 10 px: the ramp is (9.5, 10.5).
    BrushMask brush{{stroke({{0.505, 0.505}}, 0.1, 1.0, 1.0)}};
    auto raster = rasterize_mask(brush, size);
    ASSERT_TRUE(raster);
    EXPECT_EQ(raster->at(59, 50), 1.0f);                                 // d = 9
    EXPECT_NEAR(raster->at(60, 50), 0.5, 1e-6);                          // d = 10
    EXPECT_EQ(raster->at(61, 50), 0.0f);                                 // d = 11
    // A radial mask of feather 0: the ramp is one pixel along the minor semi-axis.
    RadialGradientMask r;
    r.center = {0.505, 0.505};
    r.radius_x = 0.1;
    r.radius_y = 0.1;
    r.feather = 0.0;
    raster = rasterize_mask(r, size);
    ASSERT_TRUE(raster);
    EXPECT_EQ(raster->at(59, 50), 1.0f);
    EXPECT_NEAR(raster->at(60, 50), 0.5, 1e-6);
    EXPECT_EQ(raster->at(61, 50), 0.0f);
}

TEST(MaskRaster, InvertOpacityAndDisabledShapeTheWeight) {
    const image::Size size{60, 40};
    LocalAdjustment a = adjustment(1, RadialGradientMask{});
    auto raster = rasterize_mask(a.shape, size);
    ASSERT_TRUE(raster);
    a.opacity = 0.6;
    a.invert = true;
    const image::ImageBuffer<float> w = weights(a, size);
    for (std::int32_t y = 0; y < size.height; y += 3) {
        for (std::int32_t x = 0; x < size.width; x += 3) {
            ASSERT_NEAR(w.at(x, y, 0), 0.6 * (1.0 - raster->at(x, y)), 1e-7);
        }
    }
    a.enabled = false;
    const image::ImageBuffer<float> off = weights(a, size);
    for (std::int32_t y = 0; y < size.height; ++y) {
        for (std::int32_t x = 0; x < size.width; ++x) {
            ASSERT_EQ(off.at(x, y, 0), 0.0f);
        }
    }
}

struct RasterDifference {
    double mean = 0.0;
    double max = 0.0;
};

// The proxy weights against the box-downscaled full-resolution weights.
RasterDifference proxy_difference(const LocalAdjustment& a, image::Size full, image::Size proxy) {
    const image::ImageBuffer<float> wf = weights(a, full);
    const image::ImageBuffer<float> wp = weights(a, proxy);
    auto reduced = image::ImageBuffer<float>::create(proxy.width, proxy.height, 1);
    EXPECT_TRUE(reduced);
    EXPECT_TRUE(box_downscale(wf.const_view(), reduced->view()));
    RasterDifference d;
    for (std::int32_t y = 0; y < proxy.height; ++y) {
        for (std::int32_t x = 0; x < proxy.width; ++x) {
            const double e = std::abs(reduced->at(x, y, 0) - wp.at(x, y, 0));
            d.mean += e;
            d.max = std::max(d.max, e);
        }
    }
    d.mean /= static_cast<double>(proxy.area());
    return d;
}

TEST(MaskRaster, ProxyRastersMatchTheDownscaledFullResolutionRaster) {
    // Tolerances stated in local_adjustments.hpp and ADR 0015: a proxy raster is the shape
    // evaluated at the proxy's pixel centres, not an average over the full-resolution pixels it
    // covers; the two differ by the smoothstep's curvature over a proxy pixel (feathered masks) and
    // by the one-pixel antialiasing ramp's scale (hard edges).
    const image::Size full{1200, 800};
    const image::Size proxy{300, 200};
    RadialGradientMask radial;
    radial.center = {0.4, 0.55};
    radial.radius_x = 0.15;
    radial.radius_y = 0.1;
    radial.angle_degrees = 25.0;
    radial.feather = 0.5;
    LinearGradientMask linear{{0.2, 0.3}, {0.6, 0.7}, 1.0};
    BrushMask brush;
    brush.strokes.push_back(stroke({{0.1, 0.2}, {0.3, 0.35}, {0.5, 0.3}, {0.7, 0.5}}, 0.03, 0.3, 0.8));
    brush.strokes.push_back(stroke({{0.2, 0.6}, {0.6, 0.25}}, 0.02, 0.6, 1.0, BrushMode::Erase));
    for (const MaskShape& shape : {MaskShape{radial}, MaskShape{linear}, MaskShape{brush}}) {
        const RasterDifference d = proxy_difference(adjustment(1, shape), full, proxy);
        std::cout << "feathered " << to_string(kind_of(shape)) << ": mean " << d.mean << " max " << d.max << "\n";
        EXPECT_LT(d.mean, 1e-3) << to_string(kind_of(shape));
        EXPECT_LT(d.max, 0.04) << to_string(kind_of(shape));
    }
    radial.feather = 0.0;
    linear.feather = 0.0;
    for (BrushStroke& s : brush.strokes) {
        s.hardness = 1.0;
    }
    for (const MaskShape& shape : {MaskShape{radial}, MaskShape{linear}, MaskShape{brush}}) {
        const RasterDifference d = proxy_difference(adjustment(1, shape), full, proxy);
        std::cout << "hard " << to_string(kind_of(shape)) << ": mean " << d.mean << " max " << d.max << "\n";
        EXPECT_LT(d.mean, 1e-3) << to_string(kind_of(shape));
        EXPECT_LT(d.max, 0.2) << to_string(kind_of(shape));
    }
}

TEST(MaskRaster, OutlinesFollowTheShapes) {
    const image::Size size{300, 200};
    RadialGradientMask r;
    r.center = {0.4, 0.6};
    r.radius_x = 0.2;
    r.radius_y = 0.1;
    r.angle_degrees = 35.0;
    r.feather = 0.4;
    const MaskOutline outline = mask_outline(r, size, 32);
    ASSERT_EQ(outline.polylines.size(), 2u);
    ASSERT_EQ(outline.polylines[0].size(), 33u);
    EXPECT_EQ(outline.polylines[0].front(), outline.polylines[0].back());
    const double a = 35.0 * std::numbers::pi / 180.0;
    for (std::size_t ring = 0; ring < 2; ++ring) {
        const double expected = ring == 0 ? 1.0 : 0.6;
        for (const MaskPoint& p : outline.polylines[ring]) {
            const double dx = p.x * 300.0 - 120.0;
            const double dy = p.y * 200.0 - 120.0;
            const double u = dx * std::cos(a) - dy * std::sin(a);
            const double v = dx * std::sin(a) + dy * std::cos(a);
            EXPECT_NEAR(std::hypot(u / 60.0, v / 30.0), expected, 1e-9);
        }
    }
    const MaskOutline lines = mask_outline(LinearGradientMask{{0.2, 0.2}, {0.4, 0.5}, 1.0}, size);
    ASSERT_EQ(lines.polylines.size(), 3u);
    for (const auto& line : lines.polylines) {
        ASSERT_EQ(line.size(), 2u);
        // Perpendicular to start -> end in pixels.
        const double lx = (line[1].x - line[0].x) * 300.0;
        const double ly = (line[1].y - line[0].y) * 200.0;
        EXPECT_NEAR(lx * 60.0 + ly * 60.0, 0.0, 1e-6);  // start -> end is (60, 60) pixels
    }
    BrushMask brush{{stroke({{0.1, 0.1}, {0.2, 0.3}}, 0.01, 0.5, 1.0)}};
    EXPECT_EQ(mask_outline(brush, size).polylines.front(), brush.strokes[0].points);
}

// ---- Local controls -----------------------------------------------------------------------------------

LocalAdjustmentsParameters parameters_for(double white = 1.0, std::int32_t radius = 4) {
    LocalAdjustmentsParameters p;
    p.white = white;
    p.tone_radius = radius;
    return p;
}

// A radial mask whose core (weight 1) covers the centre of a w x h image and whose support ends
// well inside it.
LocalAdjustment core_mask(std::uint32_t id, LocalControls controls) {
    RadialGradientMask r;
    r.center = {0.5, 0.5};
    r.radius_x = 0.2;
    r.radius_y = 0.2;
    r.feather = 0.5;
    LocalAdjustment a = adjustment(id, r);
    a.controls = controls;
    return a;
}

TEST(LocalAdjustments, ChangeOnlyPixelsInsideTheirMasks) {
    const ColorImage source = textured(80, 60);
    ColorImage image = clone(source);
    LocalControls c;
    c.exposure_ev = 1.0;
    const std::vector<LocalAdjustment> list = {core_mask(1, c)};
    auto record = apply_local_adjustments(image, list, parameters_for());
    ASSERT_TRUE(record) << record.error().describe();
    EXPECT_EQ(record->masks, 1u);
    const image::ImageBuffer<float> w = weights(list[0], source.pixels.size());
    std::uint64_t inside = 0;
    for (std::int32_t y = 0; y < 60; ++y) {
        for (std::int32_t x = 0; x < 80; ++x) {
            const double wx = w.at(x, y, 0);
            for (std::int32_t ch = 0; ch < 3; ++ch) {
                const float before = source.pixels.at(x, y, ch);
                const float after = image.pixels.at(x, y, ch);
                if (wx == 0.0) {
                    ASSERT_EQ(after, before) << x << "," << y;  // bit-exact outside the mask
                } else {
                    ASSERT_NEAR(after, before * std::exp2(wx), 1e-6 * before) << x << "," << y;
                }
            }
            inside += wx > 0.0 ? 1u : 0u;
        }
    }
    EXPECT_EQ(record->changed_pixels, inside);
    EXPECT_GT(inside, 0u);
    // In the core the exposure is exact: x 2.
    EXPECT_EQ(image.pixels.at(40, 30, 1), 2.0f * source.pixels.at(40, 30, 1));
}

TEST(LocalAdjustments, SaturationKeepsTheLuminance) {
    const ColorImage source = textured(40, 40);
    ColorImage image = clone(source);
    LocalControls c;
    c.saturation = 0.5;
    ASSERT_TRUE(apply_local_adjustments(image, std::vector<LocalAdjustment>{core_mask(1, c)}, parameters_for()));
    const float* p = source.pixels.row(20) + 3 * 20;
    const float* q = image.pixels.row(20) + 3 * 20;
    const double y0 = kRec2020Luminance[0] * p[0] + kRec2020Luminance[1] * p[1] + kRec2020Luminance[2] * p[2];
    const double y1 = kRec2020Luminance[0] * q[0] + kRec2020Luminance[1] * q[1] + kRec2020Luminance[2] * q[2];
    EXPECT_NEAR(y1, y0, 1e-6 * y0);
    for (std::int32_t ch = 0; ch < 3; ++ch) {
        EXPECT_NEAR(q[ch] - y0, 1.5 * (p[ch] - y0), 1e-6);
    }
}

TEST(LocalAdjustments, WhiteShiftsWarmTowardsAmberAndTintTowardsMagenta) {
    auto axes = local_white_axes();
    ASSERT_TRUE(axes) << axes.error().describe();
    EXPECT_GT(axes->temperature[0], 0.0);  // warmer: more red ...
    EXPECT_LT(axes->temperature[2], 0.0);  // ... less blue
    EXPECT_LT(axes->tint[1], axes->tint[0]);  // towards magenta: less green than red and blue
    EXPECT_LT(axes->tint[1], axes->tint[2]);
    const ColorImage grey = make_image(30, 30, [](std::int32_t, std::int32_t, std::int32_t) { return 0.25; });
    for (const bool tint : {false, true}) {
        ColorImage image = clone(grey);
        LocalControls c;
        (tint ? c.tint : c.temperature) = 1.0;
        ASSERT_TRUE(apply_local_adjustments(image, std::vector<LocalAdjustment>{core_mask(1, c)}, parameters_for()));
        const float* q = image.pixels.row(15) + 3 * 15;
        const double y = kRec2020Luminance[0] * q[0] + kRec2020Luminance[1] * q[1] + kRec2020Luminance[2] * q[2];
        EXPECT_NEAR(y, 0.25, 1e-7);  // a neutral keeps its luminance
        if (tint) {
            EXPECT_LT(q[1], q[0]);
            EXPECT_LT(q[1], q[2]);
        } else {
            EXPECT_GT(q[0], q[2]);
        }
    }
}

TEST(LocalAdjustments, ContrastIsABoundedSCurveAroundMiddleGrey) {
    auto look = NeutralV1Curve::create(1.0);
    ASSERT_TRUE(look);
    const double pivot = look->inverse(kLocalContrastPivotDisplay);
    // Columns: the pivot, 4x and 1/4x the pivot (neutral pixels).
    const std::vector<double> levels = {pivot, 4.0 * pivot, 0.25 * pivot};
    const ColorImage source = make_image(30, 30, [&](std::int32_t x, std::int32_t, std::int32_t) {
        return levels[static_cast<std::size_t>(x % 3)];
    });
    for (const double contrast : {1.0, -1.0, 0.4}) {
        ColorImage image = clone(source);
        LocalControls c;
        c.contrast = contrast;
        ASSERT_TRUE(
            apply_local_adjustments(image, std::vector<LocalAdjustment>{core_mask(1, c)}, parameters_for()));
        for (std::int32_t x = 15; x < 18; ++x) {
            const double before = source.pixels.at(x, 15, 1);
            const double xv = std::log2(before / pivot);
            const double expected = before * std::exp2(contrast * kLocalContrastStrength * kLocalContrastRangeEv *
                                                       std::tanh(xv / kLocalContrastRangeEv));
            EXPECT_NEAR(image.pixels.at(x, 15, 1), expected, 1e-6 * expected) << contrast << " at " << x;
        }
    }
}

TEST(LocalAdjustments, HighlightsAndShadowsMatchTheGlobalOperationUnderAFullMask) {
    // A mask of weight 1 everywhere with shadows/highlights equals the global local tone operation
    // (same base layer, radius and epsilon); the global one tabulates its factor, the local one
    // its weights, both at 1/256 EV.
    const ColorImage source = make_image(96, 64, [](std::int32_t x, std::int32_t y, std::int32_t c) {
        const double v = (x < 48 ? 0.02 : 0.6) * (1.0 + 0.2 * std::sin(0.3 * y)) + 0.001 * x;
        return v * (c == 2 ? 0.8 : 1.0);
    });
    RadialGradientMask everywhere;
    everywhere.radius_x = kMaxRadialRadius;
    everywhere.radius_y = kMaxRadialRadius;
    everywhere.feather = 0.0;
    LocalAdjustment a = adjustment(1, everywhere);
    a.controls.shadows = 0.6;
    a.controls.highlights = -0.4;
    ColorImage local = clone(source);
    auto record = apply_local_adjustments(local, std::vector<LocalAdjustment>{a}, parameters_for(1.0, 6));
    ASSERT_TRUE(record) << record.error().describe();
    EXPECT_EQ(record->tone_radius, 6);
    EXPECT_EQ(record->tone_region, image::PixelRect::from_size(source.pixels.size()));
    ColorImage global = clone(source);
    LocalToneParameters p;
    p.shadows = 0.6;
    p.highlights = -0.4;
    p.white = 1.0;
    p.radius = 6;
    ASSERT_TRUE(apply_local_tone(global, p));
    double worst = 0.0;
    for (std::int32_t y = 0; y < 64; ++y) {
        for (std::int32_t x = 0; x < 96; ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                const double g = global.pixels.at(x, y, c);
                worst = std::max(worst, std::abs(local.pixels.at(x, y, c) - g) / g);
            }
        }
    }
    EXPECT_LT(worst, 1e-5);
}

TEST(LocalAdjustments, TheBaseLayerRegionCoversTheMaskWithItsHalo) {
    // A small shadows mask: the base layer is computed on its support plus 2 radius pixels only,
    // and equals the whole-image base layer where the weight is not 0.
    const ColorImage source = textured(120, 90);
    LocalControls c;
    c.shadows = 0.8;
    LocalAdjustment small = core_mask(1, c);
    std::get<RadialGradientMask>(small.shape).radius_x = 0.08;
    std::get<RadialGradientMask>(small.shape).radius_y = 0.08;
    ColorImage image = clone(source);
    auto record = apply_local_adjustments(image, std::vector<LocalAdjustment>{small}, parameters_for(1.0, 5));
    ASSERT_TRUE(record);
    EXPECT_LT(record->tone_region.width, 120);
    EXPECT_LT(record->tone_region.height, 90);
    // Where the weight is 1 the result equals the global operation, whose base layer covers the
    // whole image.
    ColorImage global = clone(source);
    LocalToneParameters p;
    p.shadows = 0.8;
    p.white = 1.0;
    p.radius = 5;
    ASSERT_TRUE(apply_local_tone(global, p));
    const image::ImageBuffer<float> w = weights(small, source.pixels.size());
    double worst = 0.0;
    for (std::int32_t y = 0; y < 90; ++y) {
        for (std::int32_t x = 0; x < 120; ++x) {
            if (w.at(x, y, 0) != 1.0f) {
                continue;
            }
            const double g = global.pixels.at(x, y, 1);
            worst = std::max(worst, std::abs(image.pixels.at(x, y, 1) - g) / g);
        }
    }
    EXPECT_LT(worst, 2e-5);
}

TEST(LocalAdjustments, OverlappingMasksAddAndAreLimitedToTheControlRange) {
    const ColorImage source = make_image(40, 40, [](std::int32_t, std::int32_t, std::int32_t) { return 0.01; });
    LocalControls c;
    c.exposure_ev = 3.0;
    ColorImage image = clone(source);
    ASSERT_TRUE(apply_local_adjustments(image, std::vector<LocalAdjustment>{core_mask(1, c), core_mask(2, c)},
                                        parameters_for()));
    EXPECT_FLOAT_EQ(image.pixels.at(20, 20, 0), 0.01f * 16.0f);  // 3 + 3 EV limited to 4 EV
    c.exposure_ev = 1.0;
    LocalControls d;
    d.exposure_ev = -1.0;
    image = clone(source);
    ASSERT_TRUE(apply_local_adjustments(image, std::vector<LocalAdjustment>{core_mask(1, c), core_mask(2, d)},
                                        parameters_for()));
    EXPECT_EQ(image.pixels.at(20, 20, 0), 0.01f);  // +1 - 1 = 0
}

TEST(LocalAdjustments, MasksWithoutEffectAreSkipped) {
    const ColorImage source = textured(40, 30);
    LocalControls c;
    c.exposure_ev = 1.0;
    LocalAdjustment disabled = core_mask(1, c);
    disabled.enabled = false;
    LocalAdjustment transparent = core_mask(2, c);
    transparent.opacity = 0.0;
    LocalAdjustment idle = core_mask(3, LocalControls{});
    ColorImage image = clone(source);
    auto record = apply_local_adjustments(image, std::vector<LocalAdjustment>{disabled, transparent, idle},
                                          parameters_for());
    ASSERT_TRUE(record);
    EXPECT_EQ(record->masks, 0u);
    EXPECT_TRUE(ts::bit_identical(image.pixels, source.pixels));
}

TEST(LocalAdjustments, BrushRastersAreCachedByShapeAndSize) {
    const ColorImage source = textured(64, 48);
    auto cache = std::make_shared<MaskRasterCache>();
    LocalAdjustment brush = adjustment(1, BrushMask{{stroke({{0.2, 0.3}, {0.8, 0.6}}, 0.05, 0.5, 1.0)}});
    brush.controls.exposure_ev = 0.7;
    const std::vector<LocalAdjustment> list = {brush};
    ColorImage first = clone(source);
    auto r1 = apply_local_adjustments(first, list, parameters_for(), cache.get());
    ASSERT_TRUE(r1);
    EXPECT_EQ(r1->rasterized, 1u);
    EXPECT_EQ(r1->cache_hits, 0u);
    EXPECT_GT(r1->raster_bytes, 0u);
    ColorImage second = clone(source);
    auto r2 = apply_local_adjustments(second, list, parameters_for(), cache.get());
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->rasterized, 0u);
    EXPECT_EQ(r2->cache_hits, 1u);
    EXPECT_TRUE(ts::bit_identical(first.pixels, second.pixels));
    // The controls are not part of the key; the shape and the size are.
    std::vector<LocalAdjustment> other = list;
    other[0].controls.exposure_ev = -0.3;
    ColorImage third = clone(source);
    auto r3 = apply_local_adjustments(third, other, parameters_for(), cache.get());
    ASSERT_TRUE(r3);
    EXPECT_EQ(r3->cache_hits, 1u);
    std::get<BrushMask>(other[0].shape).strokes[0].points.push_back({0.9, 0.9});
    ColorImage fourth = clone(source);
    auto r4 = apply_local_adjustments(fourth, other, parameters_for(), cache.get());
    ASSERT_TRUE(r4);
    EXPECT_EQ(r4->rasterized, 1u);
    EXPECT_EQ(cache->stats().entries, 2u);
    EXPECT_EQ(cache->stats().hits, 2u);
    EXPECT_FALSE(cache->find(std::get<BrushMask>(other[0].shape), image::Size{65, 48}));  // another size
    // Least recently used entries are evicted beyond the capacity.
    MaskRasterCache small(r1->raster_bytes);
    auto raster = rasterize_mask(brush.shape, source.pixels.size());
    ASSERT_TRUE(raster);
    auto shared = std::make_shared<const MaskRaster>(std::move(*raster));
    small.insert(brush.shape, shared);
    EXPECT_EQ(small.stats().entries, 1u);
    small.insert(other[0].shape, shared);  // as large again: the first is evicted
    EXPECT_EQ(small.stats().entries, 1u);
    EXPECT_FALSE(small.find(brush.shape, source.pixels.size()));
    EXPECT_TRUE(small.find(other[0].shape, source.pixels.size()));
    small.clear();
    EXPECT_EQ(small.stats().bytes, 0u);
}

TEST(LocalAdjustments, ParallelAndSerialAreBitIdentical) {
    const ColorImage source = textured(97, 71);
    LocalControls c;
    c.exposure_ev = 0.5;
    c.contrast = 0.3;
    c.shadows = 0.4;
    c.saturation = -0.2;
    c.temperature = 0.3;
    LocalAdjustment radial = core_mask(1, c);
    LocalAdjustment linear = adjustment(2, LinearGradientMask{{0.1, 0.1}, {0.6, 0.9}, 0.7});
    linear.controls.highlights = -0.5;
    linear.invert = true;
    LocalAdjustment brush = adjustment(3, BrushMask{{stroke({{0.1, 0.8}, {0.9, 0.2}}, 0.04, 0.2, 0.9)}});
    brush.controls.tint = -0.4;
    brush.opacity = 0.7;
    const std::vector<LocalAdjustment> list = {radial, linear, brush};
    ColorImage a = clone(source);
    ColorImage b = clone(source);
    ASSERT_TRUE(apply_local_adjustments(a, list, parameters_for(1.0, 3), nullptr, {}, {}, true));
    ASSERT_TRUE(apply_local_adjustments(b, list, parameters_for(1.0, 3), nullptr, {}, {}, false));
    EXPECT_TRUE(ts::bit_identical(a.pixels, b.pixels));
    EXPECT_TRUE(ts::all_finite(a.pixels));
}

TEST(LocalAdjustments, RejectsInvalidInput) {
    ColorImage image = textured(20, 20);
    LocalControls c;
    c.exposure_ev = 1.0;
    LocalAdjustment bad = core_mask(0, c);  // id 0
    ts::expect_error(apply_local_adjustments(image, std::vector<LocalAdjustment>{bad}, parameters_for()),
                     ErrorCode::InvalidArgument, kWhere);
    ts::expect_error(apply_local_adjustments(image, std::vector<LocalAdjustment>{core_mask(1, c)},
                                             parameters_for(0.0)),
                     ErrorCode::InvalidArgument, kWhere);
    ColorImage display = textured(20, 20);
    display.encoding = display_linear_rec2020();
    ts::expect_error(apply_local_adjustments(display, std::vector<LocalAdjustment>{core_mask(1, c)}, parameters_for()),
                     ErrorCode::InvalidArgument, kWhere);
    ts::expect_error(rasterize_mask(RadialGradientMask{}, image::Size{}), ErrorCode::InvalidArgument, kWhere);
    RadialGradientMask r;
    r.radius_x = 0.0;
    ts::expect_error(rasterize_mask(r, image::Size{10, 10}), ErrorCode::InvalidArgument, kWhere);
    core::CancellationSource source;
    source.cancel();
    auto cancelled =
        apply_local_adjustments(image, std::vector<LocalAdjustment>{core_mask(1, c)}, parameters_for(), nullptr, {},
                                source.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::Cancelled);
}

}  // namespace
}  // namespace lucent::engine
