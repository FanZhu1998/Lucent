#include <cmath>
#include <functional>

#include <gtest/gtest.h>

#include <lucent/analysis/render_metrics.hpp>
#include <lucent/engine/develop_operations.hpp>
#include <lucent/engine/display_rendering.hpp>

namespace lucent::analysis {
namespace {

constexpr std::int32_t kSide = 100;

engine::ColorImage display_image(const std::function<std::array<double, 3>(std::int32_t i)>& pixel) {
    engine::ColorImage image;
    image.encoding = engine::display_linear_rec2020();
    auto buffer = image::ImageBuffer<float>::create(kSide, kSide, 3);
    EXPECT_TRUE(buffer);
    image.pixels = std::move(*buffer);
    for (std::int32_t y = 0; y < kSide; ++y) {
        for (std::int32_t x = 0; x < kSide; ++x) {
            const auto p = pixel(y * kSide + x);
            for (std::int32_t c = 0; c < 3; ++c) {
                image.pixels.at(x, y, c) = static_cast<float>(p[static_cast<std::size_t>(c)]);
            }
        }
    }
    return image;
}

ImageContext context(double signal = 0.1) {
    ImageContext ctx;
    ctx.size = image::Size{kSide, kSide};
    ctx.signal.assign(static_cast<std::size_t>(kSide * kSide), static_cast<float>(signal));
    return ctx;
}

// Neutral ramp whose display lightness is (i + 0.5) / N.
double ramp_value(std::int32_t i) {
    return engine::display_lightness_inverse((i + 0.5) / static_cast<double>(kSide * kSide));
}

TEST(RenderMetrics, QuantilesAndOutputClippingOfARamp) {
    const auto image = display_image([](std::int32_t i) {
        const double d = ramp_value(i);
        return std::array<double, 3>{d, d, d};
    });
    BaselineFeatures features;
    auto stats = measure_render(image, context(), nullptr, &features);
    ASSERT_TRUE(stats) << stats.error().describe();
    EXPECT_EQ(stats->pixels, static_cast<std::uint64_t>(kSide * kSide));
    // The histogram resolution is 1/4096 of lightness; the float storage adds about 1e-7.
    EXPECT_NEAR(stats->quantiles.p50, 0.5, 1.0 / 4096.0);
    EXPECT_NEAR(stats->quantiles.p25, 0.25, 1.0 / 4096.0);
    EXPECT_NEAR(stats->quantiles.p99, 0.99, 1.0 / 4096.0);
    EXPECT_NEAR(stats->mean_lightness, 0.5, 1e-6);

    std::uint64_t high = 0;
    std::uint64_t low = 0;
    for (std::int32_t i = 0; i < kSide * kSide; ++i) {
        const auto d = static_cast<double>(static_cast<float>(ramp_value(i)));
        high += d >= clip_high_linear() ? 1 : 0;
        low += d <= block_low_linear() ? 1 : 0;
    }
    EXPECT_GT(high, 0u);
    EXPECT_GT(low, 0u);
    EXPECT_EQ(stats->highlight_clipped, high);
    EXPECT_EQ(stats->shadow_blocked, low);
    EXPECT_EQ(stats->out_of_gamut, 0u);  // neutrals are inside every gamut
    EXPECT_EQ(stats->new_highlight_clipped, 0u);
    EXPECT_EQ(stats->neutral_shift_de, 0.0);
    EXPECT_EQ(features.lightness.size(), static_cast<std::size_t>(kSide * kSide));
}

TEST(RenderMetrics, NewClippingIsCountedAgainstTheBaseline) {
    const auto base_image = display_image([](std::int32_t i) {
        const double d = ramp_value(i);
        return std::array<double, 3>{d, d, d};
    });
    BaselineFeatures features;
    auto base = measure_render(base_image, context(), nullptr, &features);
    ASSERT_TRUE(base);
    const auto brighter = display_image([](std::int32_t i) {
        const double d = std::min(1.0, 1.25 * ramp_value(i));
        return std::array<double, 3>{d, d, d};
    });
    auto stats = measure_render(brighter, context(), &features, nullptr);
    ASSERT_TRUE(stats);
    std::uint64_t expected_new = 0;
    for (std::int32_t i = 0; i < kSide * kSide; ++i) {
        const auto b = static_cast<double>(static_cast<float>(ramp_value(i)));
        const auto c = static_cast<double>(static_cast<float>(std::min(1.0, 1.25 * ramp_value(i))));
        expected_new += (c >= clip_high_linear() && b < clip_high_linear()) ? 1 : 0;
    }
    EXPECT_GT(expected_new, 0u);
    EXPECT_EQ(stats->new_highlight_clipped, expected_new);
    EXPECT_EQ(stats->highlight_clipped, base->highlight_clipped + expected_new);
    EXPECT_EQ(stats->new_shadow_blocked, 0u);
    EXPECT_GT(stats->quantiles.p50, base->quantiles.p50);
}

TEST(RenderMetrics, ChromaAndHueAgainstTheBaseline) {
    // Chromatic pixels around a hue, scaled in chroma around their luminance by k.
    const auto colour = [](std::int32_t i, double k) {
        const double level = 0.05 + 0.4 * (i % 97) / 97.0;
        const std::array<double, 3> p{level * 1.6, level * 0.9, level * 0.5};
        const double Y = engine::kRec2020Luminance[0] * p[0] + engine::kRec2020Luminance[1] * p[1] +
                         engine::kRec2020Luminance[2] * p[2];
        return std::array<double, 3>{Y + k * (p[0] - Y), Y + k * (p[1] - Y), Y + k * (p[2] - Y)};
    };
    BaselineFeatures features;
    auto base = measure_render(display_image([&](std::int32_t i) { return colour(i, 1.0); }), context(),
                               nullptr, &features);
    ASSERT_TRUE(base);
    EXPECT_GT(base->chromatic_pixels, 5000u);
    auto same = measure_render(display_image([&](std::int32_t i) { return colour(i, 1.0); }), context(),
                               &features, nullptr);
    ASSERT_TRUE(same);
    EXPECT_EQ(same->saturation_log_ratio, 0.0);  // a render compared with itself: exactly nothing
    EXPECT_EQ(same->hue_shift_degrees, 0.0);
    EXPECT_EQ(same->neutral_shift_de, 0.0);
    auto more = measure_render(display_image([&](std::int32_t i) { return colour(i, 1.1); }), context(),
                               &features, nullptr);
    ASSERT_TRUE(more);
    // The linear-light chroma scales exactly with the factor (float storage: 1e-6).
    EXPECT_NEAR(more->saturation_log_ratio, std::log(1.1), 1e-6);
    EXPECT_LT(more->hue_shift_degrees, 2.0);  // CIELAB hue moves a little under a linear chroma scale
    // A ratio-preserving brightening (exposure, local tone, base look) leaves the chroma unchanged.
    auto brighter = measure_render(display_image([&](std::int32_t i) {
                                       auto p = colour(i, 1.0);
                                       return std::array<double, 3>{1.3 * p[0], 1.3 * p[1], 1.3 * p[2]};
                                   }),
                                   context(), &features, nullptr);
    ASSERT_TRUE(brighter);
    EXPECT_NEAR(brighter->saturation_log_ratio, 0.0, 1e-6);
}

TEST(RenderMetrics, PredictedShadowNoiseFollowsTheNoiseModel) {
    // Signal 0.01 with shot-noise variance 1e-4 x: sigma = 1e-3. Displayed at luminance 0.02 (local
    // gain 2): sigma_L = 100 x l'(0.02) x 2 x 1e-3 with l'(Y) = 1.16 / (3 Y^(2/3)).
    ImageContext ctx = context(0.01);
    ctx.noise.source = NoiseModelSource::DngNoiseProfile;
    ctx.noise.shot_scale = 1e-4;
    ctx.noise.read_variance = 0.0;
    const auto image = display_image([](std::int32_t) { return std::array<double, 3>{0.02, 0.02, 0.02}; });
    auto stats = measure_render(image, ctx, nullptr, nullptr);
    ASSERT_TRUE(stats);
    EXPECT_EQ(stats->shadow_pixels, static_cast<std::uint64_t>(kSide * kSide));
    EXPECT_TRUE(stats->noise_model_available);
    const double expected = 100.0 * 1.16 / (3.0 * std::pow(0.02, 2.0 / 3.0)) * 2.0 * 1e-3;
    EXPECT_NEAR(stats->shadow_noise_lstar, expected, 0.02);  // the histogram bin width
    // Every pixel has signal above 3 sigma (0.01 >= 3e-3).
    EXPECT_EQ(stats->signal_pixels, static_cast<std::uint64_t>(kSide * kSide));
}

TEST(RenderMetrics, RejectsMismatchedInput) {
    const auto image = display_image([](std::int32_t) { return std::array<double, 3>{0.2, 0.2, 0.2}; });
    ImageContext small = context();
    small.size = image::Size{10, 10};
    EXPECT_FALSE(measure_render(image, small, nullptr, nullptr));
    engine::ColorImage scene;
    scene.encoding = image::ColorEncoding::scene_linear_rec2020_d65();
    scene.pixels = std::move(*image::ImageBuffer<float>::create(kSide, kSide, 3));
    auto wrong = measure_render(scene, context(), nullptr, nullptr);
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().code, ErrorCode::InvalidArgument);
}

}  // namespace
}  // namespace lucent::analysis
