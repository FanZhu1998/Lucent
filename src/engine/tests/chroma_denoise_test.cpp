// Unit tests for guided chroma denoising (chroma_denoise.hpp): strength 0 is the identity, the
// luminance is preserved to float rounding, chroma noise on a flat field is reduced, colour does
// not bleed across a sharp luminance edge (and does across an isoluminant one: the documented
// limitation), bands equal the whole-image filter, proxy radius scaling, bounded working memory,
// determinism and failures.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/core/cancellation.hpp>
#include <lucent/engine/chroma_denoise.hpp>
#include <lucent/engine/develop_operations.hpp>
#include <lucent/engine/display_rendering.hpp>

#include "develop_test_support.hpp"

namespace lucent::engine {
namespace {

namespace ts = test_support;
constexpr const char* kWhere = "engine.develop.chroma_denoise";

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

double luminance(const float* p) {
    return kRec2020Luminance[0] * p[0] + kRec2020Luminance[1] * p[1] + kRec2020Luminance[2] * p[2];
}

// (R - Y) / Y and (B - Y) / Y.
std::array<double, 2> chroma(const float* p) {
    const double y = luminance(p);
    return {(p[0] - y) / y, (p[2] - y) / y};
}

// A flat colour with independent Gaussian noise of `sigma` per channel (fixed seed).
ColorImage noisy_flat(std::int32_t w, std::int32_t h, std::array<double, 3> colour, double sigma,
                      std::uint64_t seed = 20261005) {
    std::mt19937_64 random(seed);
    std::normal_distribution<double> noise(0.0, sigma);
    std::vector<double> values(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3);
    for (double& v : values) {
        v = noise(random);
    }
    return make_image(w, h, [&](std::int32_t x, std::int32_t y, std::int32_t c) {
        return colour[static_cast<std::size_t>(c)] +
               values[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 3 +
                      static_cast<std::size_t>(c)];
    });
}

ChromaDenoiseParameters parameters(double strength, std::int32_t radius, std::int32_t band_rows = kChromaDenoiseBandRows) {
    ChromaDenoiseParameters p;
    p.strength = strength;
    p.radius = radius;
    p.white = 1.0;
    p.band_rows = band_rows;
    return p;
}

double chroma_std(const ColorImage& image, std::size_t plane, std::int32_t x0, std::int32_t x1) {
    double sum = 0.0;
    double sum2 = 0.0;
    double n = 0.0;
    for (std::int32_t y = 0; y < image.pixels.height(); ++y) {
        for (std::int32_t x = x0; x < x1; ++x) {
            const double v = chroma(image.pixels.row(y) + 3 * x)[plane];
            sum += v;
            sum2 += v * v;
            n += 1.0;
        }
    }
    const double mean = sum / n;
    return std::sqrt(std::max(0.0, sum2 / n - mean * mean));
}

TEST(ChromaDenoise, StrengthZeroIsTheIdentity) {
    const ColorImage source = noisy_flat(40, 30, {0.2, 0.25, 0.18}, 0.02);
    ColorImage image = clone(source);
    auto record = apply_chroma_denoise(image, parameters(0.0, 4));
    ASSERT_TRUE(record) << record.error().describe();
    EXPECT_EQ(record->bands, 0);
    EXPECT_TRUE(ts::bit_identical(image.pixels, source.pixels));
}

TEST(ChromaDenoise, PreservesTheLuminanceExactly) {
    // Coloured, noisy, with negative (out-of-gamut) samples and a dark corner below the floor.
    ColorImage source = noisy_flat(64, 48, {0.12, 0.2, 0.3}, 0.05);
    for (std::int32_t y = 0; y < 8; ++y) {
        for (std::int32_t x = 0; x < 8; ++x) {
            source.pixels.at(x, y, 0) = 1e-6f * static_cast<float>(x - 3);
            source.pixels.at(x, y, 2) = 2e-6f;
        }
    }
    for (const double strength : {0.3, 1.0}) {
        ColorImage image = clone(source);
        ASSERT_TRUE(apply_chroma_denoise(image, parameters(strength, 5)));
        double worst = 0.0;
        bool changed = false;
        for (std::int32_t y = 0; y < 48; ++y) {
            for (std::int32_t x = 0; x < 64; ++x) {
                const float* p = source.pixels.row(y) + 3 * x;
                const float* q = image.pixels.row(y) + 3 * x;
                const double scale = std::max({std::abs(p[0]), std::abs(p[1]), std::abs(p[2]), std::abs(q[0]),
                                               std::abs(q[1]), std::abs(q[2])});
                worst = std::max(worst, std::abs(luminance(q) - luminance(p)) / scale);
                changed = changed || q[0] != p[0];
            }
        }
        EXPECT_TRUE(changed);
        // The reconstruction rounds R', B' and G' to float once each: about 1e-7 of the largest channel.
        EXPECT_LT(worst, 2e-7) << strength;
        std::cout << "strength " << strength << ": largest luminance change " << worst << " of the largest channel\n";
    }
}

TEST(ChromaDenoise, ReducesChromaNoiseOnAFlatField) {
    const ColorImage source = noisy_flat(128, 128, {0.2, 0.25, 0.18}, 0.02);
    ColorImage image = clone(source);
    auto record = apply_chroma_denoise(image, parameters(1.0, 8));
    ASSERT_TRUE(record);
    for (std::size_t plane = 0; plane < 2; ++plane) {
        const double before = chroma_std(source, plane, 0, 128);
        const double after = chroma_std(image, plane, 0, 128);
        std::cout << "chroma plane " << plane << ": noise " << before << " -> " << after << " (x"
                  << before / after << ")\n";
        // A 17 x 17 window averages 289 samples (a factor of 17 for uncorrelated noise); the
        // guided filter's second box pass and edge handling leave a little more.
        EXPECT_LT(after, before / 8.0) << plane;
    }
    // Half strength halves the chroma change.
    ColorImage half = clone(source);
    ASSERT_TRUE(apply_chroma_denoise(half, parameters(0.5, 8)));
    const std::array<double, 2> c0 = chroma(source.pixels.row(64) + 3 * 64);
    const std::array<double, 2> c1 = chroma(image.pixels.row(64) + 3 * 64);
    const std::array<double, 2> ch = chroma(half.pixels.row(64) + 3 * 64);
    EXPECT_NEAR(ch[0] - c0[0], 0.5 * (c1[0] - c0[0]), 1e-5);
}

TEST(ChromaDenoise, KeepsColourOnEachSideOfASharpLuminanceEdge) {
    // Bright red against dark blue (2.7 EV and a hue change), with chroma noise on both sides.
    const std::array<double, 3> red = {0.6, 0.15, 0.08};
    const std::array<double, 3> blue = {0.02, 0.04, 0.15};
    const ColorImage clean = make_image(96, 64, [&](std::int32_t x, std::int32_t, std::int32_t c) {
        return (x < 48 ? red : blue)[static_cast<std::size_t>(c)];
    });
    ColorImage image = clone(clean);
    ASSERT_TRUE(apply_chroma_denoise(image, parameters(1.0, 8)));
    double worst = 0.0;
    for (std::int32_t y = 0; y < 64; ++y) {
        for (std::int32_t x = 0; x < 96; ++x) {
            const auto a = chroma(clean.pixels.row(y) + 3 * x);
            const auto b = chroma(image.pixels.row(y) + 3 * x);
            worst = std::max({worst, std::abs(a[0] - b[0]), std::abs(a[1] - b[1])});
        }
    }
    const auto left = chroma(clean.pixels.row(0));
    const auto right = chroma(clean.pixels.row(0) + 3 * 50);
    const double step = std::abs(left[0] - right[0]);
    std::cout << "chroma step " << step << " across the edge; largest chroma change " << worst << " ("
              << 100.0 * worst / step << " % of the step)\n";
    // A box blur of the chroma would move the pixels next to the edge by half the step.
    EXPECT_LT(worst, 0.02 * step);

    // With noise: the noise is removed on both sides away from the edge, and the mean colour of
    // every column next to the edge stays its side's.
    std::mt19937_64 random(20261005);
    std::normal_distribution<double> noise(0.0, 0.04);
    std::vector<double> n(96 * 64 * 3);
    for (double& v : n) {
        v = noise(random);
    }
    const ColorImage noisy = make_image(96, 64, [&](std::int32_t x, std::int32_t y, std::int32_t c) {
        const std::size_t i = (static_cast<std::size_t>(y) * 96 + static_cast<std::size_t>(x)) * 3 +
                              static_cast<std::size_t>(c);
        return (x < 48 ? red : blue)[static_cast<std::size_t>(c)] * (1.0 + n[i]);
    });
    ColorImage denoised = clone(noisy);
    ASSERT_TRUE(apply_chroma_denoise(denoised, parameters(1.0, 8)));
    for (std::size_t plane = 0; plane < 2; ++plane) {
        const double left_before = chroma_std(noisy, plane, 0, 30);
        const double right_before = chroma_std(noisy, plane, 66, 96);
        const double left_after = chroma_std(denoised, plane, 0, 30);
        const double right_after = chroma_std(denoised, plane, 66, 96);
        std::cout << "edge scene, plane " << plane << ": noise left " << left_before << " -> " << left_after
                  << ", right " << right_before << " -> " << right_after << "\n";
        EXPECT_LT(left_after, left_before / 6.0);
        EXPECT_LT(right_after, right_before / 6.0);
    }
    double worst_column = 0.0;
    for (std::int32_t x = 40; x < 56; ++x) {
        double mean_r = 0.0;
        for (std::int32_t y = 0; y < 64; ++y) {
            mean_r += chroma(denoised.pixels.row(y) + 3 * x)[0] / 64.0;
        }
        worst_column = std::max(worst_column, std::abs(mean_r - (x < 48 ? left : right)[0]));
    }
    std::cout << "edge scene: largest column-mean chroma deviation next to the edge " << worst_column << " ("
              << 100.0 * worst_column / step << " % of the step)\n";
    EXPECT_LT(worst_column, 0.03 * step);
}

TEST(ChromaDenoise, SmoothsAnIsoluminantColourEdge) {
    // The documented limitation: with no luminance edge to guide it, a colour edge is averaged over
    // the window (here: red | green of equal luminance).
    const double y = 0.2627 * 0.3 + 0.6780 * 0.2 + 0.0593 * 0.2;
    const double g = (y - 0.2627 * 0.1 - 0.0593 * 0.2) / 0.6780;
    const ColorImage clean = make_image(64, 32, [&](std::int32_t x, std::int32_t, std::int32_t c) {
        const std::array<double, 3> left = {0.3, 0.2, 0.2};
        const std::array<double, 3> right = {0.1, g, 0.2};
        return (x < 32 ? left : right)[static_cast<std::size_t>(c)];
    });
    ColorImage image = clone(clean);
    ASSERT_TRUE(apply_chroma_denoise(image, parameters(1.0, 4)));
    const double step = chroma(clean.pixels.row(0))[0] - chroma(clean.pixels.row(0) + 3 * 40)[0];
    const double moved = chroma(clean.pixels.row(16) + 3 * 31)[0] - chroma(image.pixels.row(16) + 3 * 31)[0];
    EXPECT_GT(moved, 0.25 * step);
    // Beyond two radii (the two box passes) nothing changes.
    EXPECT_NEAR(chroma(image.pixels.row(16) + 3 * 20)[0], chroma(clean.pixels.row(16) + 3 * 20)[0], 1e-6);
}

TEST(ChromaDenoise, BandsEqualTheWholeImageFilter) {
    const ColorImage source = noisy_flat(64, 200, {0.3, 0.2, 0.1}, 0.03);
    ColorImage whole = clone(source);
    auto r0 = apply_chroma_denoise(whole, parameters(0.8, 3, 0));
    ASSERT_TRUE(r0);
    EXPECT_EQ(r0->bands, 1);
    ColorImage banded = clone(source);
    auto r1 = apply_chroma_denoise(banded, parameters(0.8, 3, 24));
    ASSERT_TRUE(r1);
    EXPECT_EQ(r1->band_rows, 28);  // four halos of 2 r + 1 = 7 rows
    EXPECT_EQ(r1->halo_rows, 7);
    EXPECT_EQ(r1->bands, 8);
    // The running box sums start at each band region's top: equal up to float rounding.
    EXPECT_LT(ts::max_abs_difference(whole.pixels, banded.pixels), 2e-6);
    // Bands are at least four halos high.
    ColorImage tall_halo = clone(source);
    auto r2 = apply_chroma_denoise(tall_halo, parameters(0.8, 5, 8));
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->band_rows, 44);
    EXPECT_EQ(r2->bands, 5);
}

TEST(ChromaDenoise, RadiusScalesWithTheProxyAndMemoryWithTheBand) {
    EXPECT_EQ(chroma_denoise_radius(8.0, 1.0), 8);
    EXPECT_EQ(chroma_denoise_radius(8.0, 0.25), 2);
    EXPECT_EQ(chroma_denoise_radius(8.0, 0.0625), 1);
    EXPECT_EQ(chroma_denoise_radius(5.0, 0.3), 1);  // (11 x 0.3 - 1) / 2 = 1.15
    EXPECT_EQ(chroma_denoise_radius(6.0, 0.25), 1);  // (13 x 0.25 - 1) / 2 = 1.125
    EXPECT_EQ(chroma_denoise_radius(6.5, 1.0), 7);   // halves round away from zero
    EXPECT_EQ(chroma_denoise_radius(64.0, 0.5), 32);
    const image::Size r5{8192, 5464};
    const std::uint64_t banded = chroma_denoise_working_bytes(r5, 16);
    const std::uint64_t whole = chroma_denoise_working_bytes(r5, 16, 0);
    EXPECT_EQ(whole, static_cast<std::uint64_t>(r5.area()) * 8 * sizeof(float));
    EXPECT_LT(banded, whole / 10);
    std::cout << "R5 working memory at radius 16: " << (banded >> 20) << " MiB in bands, " << (whole >> 20)
              << " MiB whole\n";
}

TEST(ChromaDenoise, ParallelAndSerialAreBitIdentical) {
    const ColorImage source = noisy_flat(77, 301, {0.15, 0.3, 0.2}, 0.04);
    ColorImage a = clone(source);
    ColorImage b = clone(source);
    ASSERT_TRUE(apply_chroma_denoise(a, parameters(0.7, 4, 32), {}, {}, true));
    ASSERT_TRUE(apply_chroma_denoise(b, parameters(0.7, 4, 32), {}, {}, false));
    EXPECT_TRUE(ts::bit_identical(a.pixels, b.pixels));
    EXPECT_TRUE(ts::all_finite(a.pixels));
}

TEST(ChromaDenoise, RejectsInvalidInput) {
    ColorImage image = noisy_flat(10, 10, {0.2, 0.2, 0.2}, 0.01);
    ts::expect_error(apply_chroma_denoise(image, parameters(1.5, 2)), ErrorCode::InvalidArgument, kWhere);
    ts::expect_error(apply_chroma_denoise(image, parameters(0.5, 0)), ErrorCode::InvalidArgument, kWhere);
    ChromaDenoiseParameters p = parameters(0.5, 2);
    p.white = std::numeric_limits<double>::quiet_NaN();
    ts::expect_error(apply_chroma_denoise(image, p), ErrorCode::InvalidArgument, kWhere);
    ColorImage display = clone(image);
    display.encoding = display_linear_rec2020();
    ts::expect_error(apply_chroma_denoise(display, parameters(0.5, 2)), ErrorCode::InvalidArgument, kWhere);
    ColorImage empty;
    ts::expect_error(apply_chroma_denoise(empty, parameters(0.5, 2)), ErrorCode::InvalidArgument, kWhere);
    core::CancellationSource source;
    source.cancel();
    auto cancelled = apply_chroma_denoise(image, parameters(0.5, 2), {}, source.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::Cancelled);
}

}  // namespace
}  // namespace lucent::engine
