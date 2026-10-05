// Unit tests for the geometry operation and the point mapping (develop_geometry.hpp): whole-pixel
// crops are exact copies, quarter turns and flips equal apply_orientation(), straightening
// reproduces a bilinear field exactly (Catmull-Rom) and blackens only uncovered canvas, mapping
// round trips at full and proxy scale, the largest inside crop, determinism and failures.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/core/cancellation.hpp>
#include <lucent/engine/develop.hpp>
#include <lucent/engine/develop_geometry.hpp>
#include <lucent/engine/orientation.hpp>

#include "develop_test_support.hpp"

namespace lucent::engine {
namespace {

namespace ts = test_support;
constexpr const char* kWhere = "engine.develop.geometry";

// A 3-channel image whose sample (x, y, c) is f(x, y, c).
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

ColorImage pattern(std::int32_t w, std::int32_t h) {
    return make_image(w, h, [](std::int32_t x, std::int32_t y, std::int32_t c) { return x + 1000.0 * y + 0.25 * c; });
}

GeometryMapping mapping_of(const GeometrySetting& g, image::Size size) {
    auto m = GeometryMapping::create(g, size);
    EXPECT_TRUE(m) << m.error().describe();
    return *m;
}

ColorImage clone(const ColorImage& image) {
    auto pixels = image.pixels.clone();
    EXPECT_TRUE(pixels);
    return ColorImage{std::move(*pixels), image.encoding};
}

TEST(DevelopGeometry, IdentityLeavesTheImageAndPointsUnchanged) {
    const GeometryMapping m = mapping_of(GeometrySetting{}, image::Size{40, 30});
    EXPECT_TRUE(m.identity());
    EXPECT_FALSE(m.resamples());
    EXPECT_EQ(m.output_size(), (image::Size{40, 30}));
    EXPECT_EQ(m.crop_pixels(), (image::PixelRect{0, 0, 40, 30}));
    EXPECT_DOUBLE_EQ(m.pixels_per_unit_length(), 40.0);
    const image::PointD p = m.output_from_upright(image::PointD{3.25, 7.5});
    EXPECT_DOUBLE_EQ(p.x, 3.25);
    EXPECT_DOUBLE_EQ(p.y, 7.5);
    const image::PointD n = m.output_from_normalized(MaskPoint{0.5, 0.25});
    EXPECT_DOUBLE_EQ(n.x, 20.0);
    EXPECT_DOUBLE_EQ(n.y, 7.5);
    ColorImage image = pattern(40, 30);
    ColorImage before = clone(image);
    auto record = apply_geometry(image, m);
    ASSERT_TRUE(record) << record.error().describe();
    EXPECT_TRUE(ts::bit_identical(image.pixels, before.pixels));
    EXPECT_EQ(record->output_size, (image::Size{40, 30}));
}

TEST(DevelopGeometry, CropRoundsToWholePixelsAndCopiesExactly) {
    GeometrySetting g;
    g.crop = CropRect{0.1, 0.2, 0.6, 0.9};  // 4..24 x 6..27 of 40 x 30
    const GeometryMapping m = mapping_of(g, image::Size{40, 30});
    EXPECT_EQ(m.crop_pixels(), (image::PixelRect{4, 6, 20, 21}));
    EXPECT_EQ(m.output_size(), (image::Size{20, 21}));
    EXPECT_FALSE(m.identity());
    ColorImage image = pattern(40, 30);
    ColorImage before = clone(image);
    auto record = apply_geometry(image, m);
    ASSERT_TRUE(record) << record.error().describe();
    EXPECT_FALSE(record->resampled);
    EXPECT_EQ(record->uncovered_pixels, 0u);
    ASSERT_EQ(image.pixels.size(), (image::Size{20, 21}));
    for (std::int32_t y = 0; y < 21; ++y) {
        for (std::int32_t x = 0; x < 20; ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                ASSERT_EQ(image.pixels.at(x, y, c), before.pixels.at(x + 4, y + 6, c)) << x << "," << y;
            }
        }
    }
    // Halves round away from zero; a crop narrower than a pixel keeps one pixel.
    g.crop = CropRect{0.0125, 0.0, 0.0375, 1.0};  // 0.5 .. 1.5 px of 40
    EXPECT_EQ(mapping_of(g, image::Size{40, 30}).crop_pixels(), (image::PixelRect{1, 0, 1, 30}));
    g.crop = CropRect{0.5, 0.5, 0.5 + kMinCropExtent, 0.5 + kMinCropExtent};
    EXPECT_EQ(mapping_of(g, image::Size{40, 30}).crop_pixels(), (image::PixelRect{20, 15, 1, 1}));
    g.crop = CropRect{0.999, 0.0, 1.0, 1.0};
    EXPECT_EQ(mapping_of(g, image::Size{40, 30}).crop_pixels(), (image::PixelRect{39, 0, 1, 30}));
}

TEST(DevelopGeometry, QuarterTurnsAndFlipEqualTheOrientationStage) {
    const ColorImage source = pattern(23, 17);
    for (std::uint32_t q = 0; q < 4; ++q) {
        for (const bool flip : {false, true}) {
            SCOPED_TRACE(testing::Message() << q << " turns, flip " << flip);
            GeometrySetting g;
            g.crop = CropRect{0.2, 0.1, 0.9, 0.8};
            g.quarter_turns = q;
            g.flip_horizontal = flip;
            const GeometryMapping m = mapping_of(g, source.pixels.size());
            ColorImage image = clone(source);
            auto record = apply_geometry(image, m);
            ASSERT_TRUE(record) << record.error().describe();
            EXPECT_EQ(record->orientation, user_orientation(g));
            // The independent reference: the crop alone, then the camera-orientation stage.
            GeometrySetting crop_only;
            crop_only.crop = g.crop;
            ColorImage reference = clone(source);
            ASSERT_TRUE(apply_geometry(reference, mapping_of(crop_only, source.pixels.size())));
            auto oriented = apply_orientation(std::move(reference.pixels), user_orientation(g));
            ASSERT_TRUE(oriented);
            EXPECT_TRUE(ts::bit_identical(image.pixels, *oriented));
            EXPECT_EQ(m.output_size(), oriented->size());
            // Pixel centres map to pixel centres.
            for (std::int32_t y = 0; y < m.output_size().height; y += 3) {
                for (std::int32_t x = 0; x < m.output_size().width; x += 3) {
                    const image::PointD u = m.upright_from_output(image::PointD{x + 0.5, y + 0.5});
                    const auto ux = static_cast<std::int32_t>(std::floor(u.x));
                    const auto uy = static_cast<std::int32_t>(std::floor(u.y));
                    EXPECT_DOUBLE_EQ(u.x - ux, 0.5);
                    EXPECT_DOUBLE_EQ(u.y - uy, 0.5);
                    ASSERT_EQ(image.pixels.at(x, y, 0), source.pixels.at(ux, uy, 0));
                }
            }
        }
    }
}

TEST(DevelopGeometry, PointMappingRoundTripsAtEveryScale) {
    std::mt19937_64 random(20261005);  // fixed seed: deterministic
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const std::vector<image::Size> sizes = {{8192, 5464}, {2048, 1366}, {5464, 8192}, {37, 101}};
    for (int i = 0; i < 200; ++i) {
        GeometrySetting g;
        g.straighten_degrees = (unit(random) - 0.5) * 2.0 * kMaxStraightenDegrees;
        const double l = 0.4 * unit(random);
        const double t = 0.4 * unit(random);
        g.crop = CropRect{l, t, l + 0.1 + 0.5 * unit(random), t + 0.1 + 0.5 * unit(random)};
        g.quarter_turns = static_cast<std::uint32_t>(i % 4);
        g.flip_horizontal = (i / 4) % 2 == 1;
        const image::Size size = sizes[static_cast<std::size_t>(i) % sizes.size()];
        const GeometryMapping m = mapping_of(g, size);
        for (int k = 0; k < 10; ++k) {
            const image::PointD p{unit(random) * size.width, unit(random) * size.height};
            const image::PointD o = m.output_from_upright(p);
            const image::PointD back = m.upright_from_output(o);
            ASSERT_NEAR(back.x, p.x, 1e-9 * size.width);
            ASSERT_NEAR(back.y, p.y, 1e-9 * size.height);
            const MaskPoint n{unit(random), unit(random)};
            const MaskPoint n_back = m.normalized_from_output(m.output_from_normalized(n));
            ASSERT_NEAR(n_back.x, n.x, 1e-12);
            ASSERT_NEAR(n_back.y, n.y, 1e-12);
            // Lengths are kept (rotation and orientation are isometries).
            const image::PointD p2{p.x + 10.0, p.y - 7.0};
            const image::PointD o2 = m.output_from_upright(p2);
            ASSERT_NEAR(std::hypot(o2.x - o.x, o2.y - o.y), std::hypot(10.0, 7.0), 1e-9);
        }
    }
}

TEST(DevelopGeometry, StraighteningTurnsTheContentCounterClockwiseAboutTheCentre) {
    GeometrySetting g;
    g.straighten_degrees = 30.0;
    const GeometryMapping m = mapping_of(g, image::Size{200, 100});
    EXPECT_TRUE(m.resamples());
    const image::PointD centre = m.output_from_upright(image::PointD{100.0, 50.0});
    EXPECT_NEAR(centre.x, 100.0, 1e-12);
    EXPECT_NEAR(centre.y, 50.0, 1e-12);
    // A point right of the centre moves up (y grows downwards): counter-clockwise as displayed.
    const image::PointD right = m.output_from_upright(image::PointD{110.0, 50.0});
    const double a = 30.0 * std::numbers::pi / 180.0;
    EXPECT_NEAR(right.x, 100.0 + 10.0 * std::cos(a), 1e-12);
    EXPECT_NEAR(right.y, 50.0 - 10.0 * std::sin(a), 1e-12);
}

TEST(DevelopGeometry, StraighteningReproducesABilinearFieldExactly) {
    // Catmull-Rom interpolates polynomials of degree 2 per axis exactly, and a bilinear value lies
    // within its four nearest samples, so the anti-ringing limit does not act: every output pixel
    // inside the image equals the field at its source point (float rounding only).
    const auto field = [](double x, double y) { return 0.1 + 0.002 * x + 0.003 * y + 1e-5 * x * y; };
    ColorImage image = make_image(120, 90, [&](std::int32_t x, std::int32_t y, std::int32_t c) {
        return field(x, y) * (1.0 + 0.1 * c);
    });
    GeometrySetting g;
    g.straighten_degrees = 10.0;
    g.crop = largest_inside_crop(10.0, image.pixels.size());
    g.quarter_turns = 1;
    const GeometryMapping m = mapping_of(g, image.pixels.size());
    auto record = apply_geometry(image, m);
    ASSERT_TRUE(record) << record.error().describe();
    EXPECT_TRUE(record->resampled);
    double worst = 0.0;
    for (std::int32_t y = 0; y < image.pixels.height(); ++y) {
        for (std::int32_t x = 0; x < image.pixels.width(); ++x) {
            const image::PointD u = m.upright_from_output(image::PointD{x + 0.5, y + 0.5});
            // Inside by one pixel: the taps beyond the edge are clamped (not the field's values).
            if (u.x < 2.0 || u.y < 2.0 || u.x > 118.0 || u.y > 88.0) {
                continue;
            }
            for (std::int32_t c = 0; c < 3; ++c) {
                const double expected = field(u.x - 0.5, u.y - 0.5) * (1.0 + 0.1 * c);
                worst = std::max(worst, std::abs(image.pixels.at(x, y, c) - expected));
            }
        }
    }
    EXPECT_LT(worst, 2e-7);
}

TEST(DevelopGeometry, OnlyUncoveredCanvasTurnsBlack) {
    const ColorImage flat = make_image(80, 60, [](std::int32_t, std::int32_t, std::int32_t) { return 0.5; });
    GeometrySetting g;
    g.straighten_degrees = 20.0;  // full crop: the rotated image leaves the corners uncovered
    ColorImage full = clone(flat);
    auto record = apply_geometry(full, mapping_of(g, flat.pixels.size()));
    ASSERT_TRUE(record);
    EXPECT_GT(record->uncovered_pixels, 0u);
    EXPECT_FALSE(crop_inside_image(g, flat.pixels.size()));
    EXPECT_EQ(full.pixels.at(0, 0, 0), 0.0f);
    EXPECT_EQ(full.pixels.at(79, 59, 1), 0.0f);
    EXPECT_EQ(full.pixels.at(40, 30, 2), 0.5f);
    // The largest inside crop has no uncovered pixel, and a flat image stays exactly flat.
    g.crop = largest_inside_crop(20.0, flat.pixels.size());
    EXPECT_TRUE(crop_inside_image(g, flat.pixels.size()));
    ColorImage inside = clone(flat);
    record = apply_geometry(inside, mapping_of(g, flat.pixels.size()));
    ASSERT_TRUE(record);
    EXPECT_EQ(record->uncovered_pixels, 0u);
    for (std::int32_t y = 0; y < inside.pixels.height(); ++y) {
        for (std::int32_t x = 0; x < inside.pixels.width(); ++x) {
            ASSERT_EQ(inside.pixels.at(x, y, 0), 0.5f) << x << "," << y;
        }
    }
}

TEST(DevelopGeometry, LargestInsideCropIsInsideAndMaximal) {
    const std::vector<image::Size> sizes = {{6000, 4000}, {4000, 6000}, {1000, 1000}};
    for (const image::Size size : sizes) {
        for (const double angle : {-45.0, -12.5, -1.0, 0.0, 3.0, 30.0, 45.0}) {
            for (const double aspect : {0.0, 1.0, 1.5, 16.0 / 9.0, 0.8}) {
                SCOPED_TRACE(testing::Message() << size.width << "x" << size.height << " " << angle << " deg, aspect "
                                                << aspect);
                GeometrySetting g;
                g.straighten_degrees = angle;
                g.crop = largest_inside_crop(angle, size, aspect);
                EXPECT_TRUE(crop_inside_image(g, size));
                const double w = (g.crop.right - g.crop.left) * size.width;
                const double h = (g.crop.bottom - g.crop.top) * size.height;
                const double expected_aspect = aspect > 0.0 ? aspect : static_cast<double>(size.width) / size.height;
                EXPECT_NEAR(w / h, expected_aspect, 1e-9 * expected_aspect);
                EXPECT_NEAR(0.5 * (g.crop.left + g.crop.right), 0.5, 1e-12);
                // One per cent larger no longer fits (unless it is already the whole image).
                if (g.crop.left > 1e-6 && g.crop.top > 1e-6) {
                    GeometrySetting bigger = g;
                    const double gx = 0.01 * (g.crop.right - g.crop.left);
                    const double gy = 0.01 * (g.crop.bottom - g.crop.top);
                    bigger.crop = CropRect{g.crop.left - gx, g.crop.top - gy, g.crop.right + gx, g.crop.bottom + gy};
                    EXPECT_FALSE(crop_inside_image(bigger, size));
                }
            }
        }
    }
    EXPECT_TRUE(largest_inside_crop(0.0, image::Size{300, 200}).is_full());
}

TEST(DevelopGeometry, ProxyAndFullResolutionShowTheSameContent) {
    // A smooth scene at 400 x 300 and its 100 x 75 box proxy through the same geometry: the full
    // output, box-downscaled to the proxy output's size, matches the proxy output. Positions agree
    // within the crop rounding (half a pixel at each scale, 2 full-resolution pixels here).
    const auto scene = [](std::int32_t x, std::int32_t y, std::int32_t c) {
        return 0.2 + 0.15 * std::sin(0.031 * x + 0.5 * c) * std::cos(0.023 * y) + 0.0005 * x;
    };
    ColorImage full = make_image(400, 300, scene);
    auto proxy_pixels = image::ImageBuffer<float>::create(100, 75, 3);
    ASSERT_TRUE(proxy_pixels);
    ASSERT_TRUE(box_downscale(full.pixels.const_view(), proxy_pixels->view()));
    ColorImage proxy{std::move(*proxy_pixels), full.encoding};
    GeometrySetting g;
    g.straighten_degrees = 12.0;
    g.crop = CropRect{0.2, 0.15, 0.8, 0.85};
    g.quarter_turns = 3;
    g.flip_horizontal = true;
    const GeometryMapping mf = mapping_of(g, full.pixels.size());
    const GeometryMapping mp = mapping_of(g, proxy.pixels.size());
    for (const MaskPoint n : {MaskPoint{0.5, 0.5}, MaskPoint{0.3, 0.6}, MaskPoint{0.7, 0.2}}) {
        const image::PointD of = mf.output_from_normalized(n);
        const image::PointD op = mp.output_from_normalized(n);
        EXPECT_NEAR(op.x * 4.0, of.x, 2.0);
        EXPECT_NEAR(op.y * 4.0, of.y, 2.0);
    }
    ASSERT_TRUE(apply_geometry(full, mf));
    ASSERT_TRUE(apply_geometry(proxy, mp));
    auto reduced = image::ImageBuffer<float>::create(proxy.pixels.width(), proxy.pixels.height(), 3);
    ASSERT_TRUE(reduced);
    ASSERT_TRUE(box_downscale(full.pixels.const_view(), reduced->view()));
    double sum = 0.0;
    double worst = 0.0;
    for (std::int32_t y = 0; y < reduced->height(); ++y) {
        for (std::int32_t x = 0; x < reduced->width(); ++x) {
            const double d = std::abs(reduced->at(x, y, 1) - proxy.pixels.at(x, y, 1));
            sum += d;
            worst = std::max(worst, d);
        }
    }
    const double mean = sum / static_cast<double>(reduced->size().area());
    // The scene's gradient is at most 0.006 per full-resolution pixel: 2 pixels of misplacement
    // and the non-commuting interpolation stay well below these.
    EXPECT_LT(mean, 0.004);
    EXPECT_LT(worst, 0.02);
}

TEST(DevelopGeometry, ParallelAndSerialAreBitIdentical) {
    const ColorImage source = make_image(97, 61, [](std::int32_t x, std::int32_t y, std::int32_t c) {
        return std::sin(0.3 * x + c) * std::cos(0.2 * y) + 0.01 * ((x * 7 + y * 3) % 5);
    });
    GeometrySetting g;
    g.straighten_degrees = -7.25;
    g.crop = CropRect{0.05, 0.1, 0.95, 0.9};
    g.quarter_turns = 2;
    const GeometryMapping m = mapping_of(g, source.pixels.size());
    ColorImage a = clone(source);
    ColorImage b = clone(source);
    ASSERT_TRUE(apply_geometry(a, m, {}, {}, true));
    ASSERT_TRUE(apply_geometry(b, m, {}, {}, false));
    EXPECT_TRUE(ts::bit_identical(a.pixels, b.pixels));
    EXPECT_TRUE(ts::all_finite(a.pixels));
}

TEST(DevelopGeometry, RejectsInvalidInput) {
    ts::expect_error(GeometryMapping::create(GeometrySetting{}, image::Size{}), ErrorCode::InvalidArgument, kWhere);
    GeometrySetting bad;
    bad.straighten_degrees = 50.0;
    ts::expect_error(GeometryMapping::create(bad, image::Size{10, 10}), ErrorCode::InvalidArgument, kWhere);
    bad = GeometrySetting{};
    bad.crop = CropRect{0.5, 0.0, 0.4, 1.0};
    ts::expect_error(GeometryMapping::create(bad, image::Size{10, 10}), ErrorCode::InvalidArgument, kWhere);
    bad = GeometrySetting{};
    bad.quarter_turns = 4;
    ts::expect_error(GeometryMapping::create(bad, image::Size{10, 10}), ErrorCode::InvalidArgument, kWhere);

    GeometrySetting g;
    g.crop.right = 0.5;
    ColorImage image = pattern(20, 10);
    ts::expect_error(apply_geometry(image, mapping_of(g, image::Size{21, 10})), ErrorCode::InvalidArgument, kWhere);
    ColorImage empty;
    ts::expect_error(apply_geometry(empty, mapping_of(g, image::Size{21, 10})), ErrorCode::InvalidArgument, kWhere);
    core::CancellationSource source;
    source.cancel();
    ColorImage before = clone(image);
    auto cancelled = apply_geometry(image, mapping_of(g, image.pixels.size()), {}, source.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::Cancelled);
    EXPECT_TRUE(ts::bit_identical(image.pixels, before.pixels));  // unchanged
}

}  // namespace
}  // namespace lucent::engine
