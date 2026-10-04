#include <cmath>

#include <gtest/gtest.h>

#include <lucent/analysis/proposals.hpp>
#include <lucent/engine/develop_operations.hpp>

namespace lucent::analysis {
namespace {

// A balanced (as shot) stage-one image of neutral patches seen through a residual cast: every
// neutral appears as level x (cast_r, 1, cast_b). Saturation levels 1, multipliers (2, 1, 1.5): the
// clip level is 1.
engine::StageOneImage neutral_patches(double cast_r, double cast_b, double scale = 1.0) {
    engine::StageOneImage s;
    constexpr std::int32_t w = 400;
    constexpr std::int32_t h = 300;
    s.pixels = std::move(*image::ImageBuffer<float>::create(w, h, 3));
    s.encoding = image::ColorEncoding::camera_native_linear();
    s.full_size = image::Size{w, h};
    s.metadata.representation.channel_colors = {image::CfaColor::Red, image::CfaColor::Green,
                                                image::CfaColor::Blue, image::CfaColor::Green2};
    s.normalization.saturation_level = {1.0f, 1.0f, 1.0f, 1.0f};
    s.white_balance.applied.multipliers = {2.0, 1.0, 1.5, 1.0};
    for (std::int32_t y = 0; y < h; ++y) {
        for (std::int32_t x = 0; x < w; ++x) {
            const std::int32_t patch = (x / 25) * 7 + (y / 25) * 3;
            const double level =
                scale * (0.08 + 0.07 * (patch % 8)) * (1.0 + 0.05 * std::sin(0.2 * x + 0.1 * y));
            s.pixels.at(x, y, 0) = static_cast<float>(level * cast_r);
            s.pixels.at(x, y, 1) = static_cast<float>(level);
            s.pixels.at(x, y, 2) = static_cast<float>(level * cast_b);
        }
    }
    return s;
}

SensorNoiseModel clean_model() {
    SensorNoiseModel m;
    m.source = NoiseModelSource::DngNoiseProfile;
    m.read_variance = 9e-8;
    m.shot_scale = 5e-5;
    m.confidence = 1.0;
    return m;
}

TEST(WhiteBalanceProposal, EstimatorsRecoverAUniformCast) {
    const auto image = neutral_patches(1.06, 0.95);
    auto ctx = make_image_context(image, nullptr, clean_model());
    ASSERT_TRUE(ctx);
    auto proposal = propose_white_balance(image, *ctx, as_shot_clip_level(image));
    ASSERT_TRUE(proposal) << proposal.error().describe();
    ASSERT_EQ(proposal->estimates.size(), 3u);
    for (const WhiteBalanceEstimate& e : proposal->estimates) {
        ASSERT_TRUE(e.available) << to_string(e.method) << ": " << e.detail;
        // A uniform cast is exact for every estimator up to float storage and the median histogram
        // bins of BrightNeutral (0.002 wide).
        EXPECT_NEAR(e.log_rg, std::log(1.06), 2e-3) << to_string(e.method);
        EXPECT_NEAR(e.log_bg, std::log(0.95), 2e-3) << to_string(e.method);
    }
    EXPECT_TRUE(proposal->evidence);
    EXPECT_LT(proposal->spread, 3e-3);
    EXPECT_GT(proposal->confidence, 0.7);
    const WhiteBalanceCorrection expected = natural_v1_white_balance_correction(
        proposal->cast_log_rg, proposal->cast_log_bg, proposal->confidence);
    EXPECT_EQ(proposal->red_gain, expected.red_gain);
    EXPECT_EQ(proposal->blue_gain, expected.blue_gain);
    EXPECT_LT(proposal->red_gain, 1.0);   // too red: less red
    EXPECT_GT(proposal->blue_gain, 1.0);  // too little blue: more blue
}

TEST(WhiteBalanceProposal, NoEvidenceWithoutValidPixels) {
    // Everything below the noise floor.
    const auto image = neutral_patches(1.0, 1.0, 1e-4);
    auto ctx = make_image_context(image, nullptr, clean_model());
    ASSERT_TRUE(ctx);
    auto proposal = propose_white_balance(image, *ctx, 1.0);
    ASSERT_TRUE(proposal);
    EXPECT_FALSE(proposal->evidence);
    EXPECT_EQ(proposal->confidence, 0.0);
    EXPECT_EQ(proposal->red_gain, 1.0);
    EXPECT_EQ(proposal->blue_gain, 1.0);
}

TEST(WhiteBalanceProposal, NaturalV1CorrectsSmallCastsAndPreservesIllumination) {
    // No cast: nothing to do.
    const WhiteBalanceCorrection none = natural_v1_white_balance_correction(0.0, 0.0, 1.0);
    EXPECT_EQ(none.red_gain, 1.0);
    EXPECT_EQ(none.blue_gain, 1.0);
    // A small magenta tint is corrected completely at full confidence.
    const WhiteBalanceCorrection tint =
        natural_v1_white_balance_correction(std::log(1.04), std::log(1.04), 1.0);
    EXPECT_NEAR(tint.red_gain, 1.0 / 1.04, 1e-12);
    EXPECT_NEAR(tint.blue_gain, 1.0 / 1.04, 1e-12);
    EXPECT_LT(tint.preserved_fraction, 1e-9);
    // A warm sunset and a blue hour keep at least 85 % of their colour (log norm).
    const WhiteBalanceCorrection sunset =
        natural_v1_white_balance_correction(std::log(1.35), std::log(0.70), 1.0);
    EXPECT_GE(sunset.preserved_fraction, 0.85);
    EXPECT_LT(sunset.red_gain, 1.0);
    EXPECT_GT(sunset.blue_gain, 1.0);
    const WhiteBalanceCorrection blue =
        natural_v1_white_balance_correction(std::log(0.75), std::log(1.40), 1.0);
    EXPECT_GE(blue.preserved_fraction, 0.85);
    EXPECT_GT(blue.red_gain, 1.0);
    // Zero confidence changes nothing; gains always stay within the residual bound.
    const WhiteBalanceCorrection unsure =
        natural_v1_white_balance_correction(std::log(1.2), std::log(0.9), 0.0);
    EXPECT_EQ(unsure.red_gain, 1.0);
    EXPECT_EQ(unsure.blue_gain, 1.0);
    const WhiteBalanceCorrection huge =
        natural_v1_white_balance_correction(std::log(1.6), std::log(1.6), 1.0);
    EXPECT_GE(huge.red_gain, 1.0 - kWhiteBalanceResidualBound);
    EXPECT_GE(huge.blue_gain, 1.0 - kWhiteBalanceResidualBound);
}

RenderStats tone_stats(double p50, double p99) {
    RenderStats s;
    s.pixels = 100000;
    s.quantiles.p50 = p50;
    s.quantiles.p99 = p99;
    return s;
}

SceneStats scene(double headroom, std::uint64_t signal = 100000) {
    SceneStats s;
    s.pixels = 100000;
    s.signal_pixels = signal;
    s.headroom_ev = headroom;
    return s;
}

TEST(ToneProposal, BrightensAnUnderexposedImageWithinTheBounds) {
    const ToneProposal t = propose_tone(tone_stats(0.15, 0.40), scene(3.0));
    EXPECT_TRUE(t.evidence);
    EXPECT_DOUBLE_EQ(t.low_key, 0.0);
    const double needed =
        std::log2(engine::display_lightness_inverse(0.40) / engine::display_lightness_inverse(0.15));
    EXPECT_NEAR(t.needed_ev, needed, 1e-12);
    EXPECT_DOUBLE_EQ(t.exposure_ev, 1.5);  // the search bound
    EXPECT_DOUBLE_EQ(t.confidence, 1.0);
}

TEST(ToneProposal, HeadroomLimitsExposureAndTheRestBecomesShadowLift) {
    const ToneProposal t = propose_tone(tone_stats(0.25, 0.70), scene(0.5));
    EXPECT_NEAR(t.exposure_ev, 0.2, 1e-12);
    EXPECT_NEAR(t.shadows_ev, std::min(0.75, 0.5 * (t.needed_ev - 0.2)), 1e-12);
    EXPECT_GT(t.shadows_ev, 0.0);
}

TEST(ToneProposal, LowKeyAndHighKeyKeepTheirKey) {
    const ToneProposal low = propose_tone(tone_stats(0.08, 0.97), scene(0.1));
    EXPECT_DOUBLE_EQ(low.low_key, 1.0);
    EXPECT_NEAR(low.band_low, 0.02, 1e-12);
    EXPECT_EQ(low.exposure_ev, 0.0);
    EXPECT_EQ(low.shadows_ev, 0.0);

    const ToneProposal high = propose_tone(tone_stats(0.92, 0.99), scene(0.4));
    EXPECT_LT(high.exposure_ev, 0.0);
    EXPECT_GT(high.exposure_ev, -0.5);  // pulled to the band's top (L* 85), not towards mid-grey
}

TEST(ToneProposal, NoProposalWithoutSignal) {
    const ToneProposal t = propose_tone(tone_stats(0.01, 0.02), scene(8.0, 100));
    EXPECT_FALSE(t.evidence);
    EXPECT_EQ(t.confidence, 0.0);
    EXPECT_EQ(t.exposure_ev, 0.0);
    EXPECT_EQ(t.shadows_ev, 0.0);
}

TEST(SceneStats, HeadroomOfTheValueNorm) {
    // Max channel 0.25 x the clip level everywhere (a uniform image): 2 EV of headroom.
    auto image = neutral_patches(1.0, 1.0);
    for (std::int32_t y = 0; y < image.pixels.height(); ++y) {
        for (std::int32_t x = 0; x < image.pixels.width(); ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                image.pixels.at(x, y, c) = 0.25f;
            }
        }
    }
    auto ctx = make_image_context(image, nullptr, clean_model());
    ASSERT_TRUE(ctx);
    auto stats = measure_scene(image, *ctx);
    ASSERT_TRUE(stats);
    EXPECT_DOUBLE_EQ(stats->clip_level, 1.0);
    EXPECT_NEAR(stats->headroom_ev, 2.0, 2.0 / 64.0);  // the histogram resolution (1/64 EV bins)
    EXPECT_EQ(stats->clipped_fraction, 0.0);
    EXPECT_EQ(stats->signal_pixels, stats->pixels);
}

}  // namespace
}  // namespace lucent::analysis
