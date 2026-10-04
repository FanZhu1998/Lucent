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

// Quantiles of a render: the median, the bright content (p90), the near-white tail (p99) and the
// quartiles.
RenderStats key_stats(double p25, double p50, double p75, double p90, double p99) {
    RenderStats s;
    s.pixels = 100000;
    s.quantiles.p25 = p25;
    s.quantiles.p50 = p50;
    s.quantiles.p75 = p75;
    s.quantiles.p90 = p90;
    s.quantiles.p99 = p99;
    s.quantiles.p995 = p99;
    return s;
}

double ev_between(double from, double to) {
    return std::log2(engine::display_lightness_inverse(to) / engine::display_lightness_inverse(from));
}

TEST(ToneProposal, BrightensAnUnderexposedImageWithinTheBounds) {
    // Everything dark, the bright content too: underexposure.
    const ToneProposal t = propose_tone(key_stats(0.10, 0.15, 0.18, 0.20, 0.40), scene(3.0));
    EXPECT_TRUE(t.evidence);
    EXPECT_DOUBLE_EQ(t.low_key, 0.0);
    EXPECT_DOUBLE_EQ(t.dimness, 0.0);  // no scene luminance: no dimness
    EXPECT_NEAR(t.needed_ev, ev_between(0.15, 0.40), 1e-12);  // the median's deficit is the smaller
    EXPECT_FALSE(t.key_ambiguous);
    EXPECT_DOUBLE_EQ(t.exposure_ev, 1.5);  // the search bound
    EXPECT_DOUBLE_EQ(t.signal_evidence, 1.0);
}

TEST(ToneProposal, ADarkMedianUnderBrightContentIsLowKeyNotUnderexposure) {
    // A lit subject (p90 L* 70) before a dark ground (median L* 20), no near-white highlights: the
    // phase-1 low-key cue (p99 > 0.75) misses it, the bright-content floor does not.
    const ToneProposal lit = propose_tone(key_stats(0.10, 0.20, 0.45, 0.70, 0.74), scene(2.0));
    EXPECT_DOUBLE_EQ(lit.low_key, 0.0);
    EXPECT_LT(lit.bright_deficit_ev, 0.0);
    EXPECT_GT(lit.median_deficit_ev, 1.0);
    EXPECT_EQ(lit.needed_ev, 0.0);
    EXPECT_EQ(lit.exposure_ev, 0.0);
    EXPECT_EQ(lit.shadows_ev, 0.0);
    EXPECT_TRUE(lit.key_ambiguous);  // a plain median band would have brightened it

    // The same frame with dim bright content: brightened by the smaller of the two deficits.
    const ToneProposal dim = propose_tone(key_stats(0.10, 0.20, 0.30, 0.42, 0.50), scene(3.0));
    EXPECT_NEAR(dim.needed_ev, std::min(ev_between(0.20, 0.40), ev_between(0.42, kBrightContentFloor)), 1e-12);
    EXPECT_GT(dim.exposure_ev, 0.5);
}

TEST(ToneProposal, DimScenesKeepADarkerKey) {
    // The same tones at night-time and daylight scene luminance.
    const RenderStats tones = key_stats(0.15, 0.25, 0.33, 0.38, 0.45);
    const ToneProposal day = propose_tone(tones, scene(3.0), 12.0);
    const ToneProposal interior = propose_tone(tones, scene(3.0), 4.0);
    EXPECT_DOUBLE_EQ(day.dimness, 0.0);
    EXPECT_DOUBLE_EQ(day.band_low, kBandLow);
    EXPECT_NEAR(interior.dimness, 0.75, 1e-12);
    EXPECT_NEAR(interior.band_low, kBandLow - kDimBandDrop * 0.75, 1e-12);
    EXPECT_GT(day.needed_ev, interior.needed_ev + 0.4);
    EXPECT_FALSE(day.key_ambiguous);
    EXPECT_TRUE(interior.key_ambiguous);
    // Without exposure metadata the scene counts as daylight (no key cue).
    EXPECT_DOUBLE_EQ(propose_tone(tones, scene(3.0)).needed_ev, day.needed_ev);
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

    // Snow or fog: bright and low in contrast - kept.
    const ToneProposal snow = propose_tone(key_stats(0.86, 0.92, 0.95, 0.96, 0.99), scene(0.4));
    EXPECT_DOUBLE_EQ(snow.high_key, 1.0);
    EXPECT_NEAR(snow.band_high, kBandHigh + kHighKeyBandRise, 1e-12);
    EXPECT_EQ(snow.exposure_ev, 0.0);

    // A bright frame with a wide range of tones is pulled to the band's top (L* 85), not towards
    // mid-grey.
    const ToneProposal bright = propose_tone(key_stats(0.55, 0.92, 0.97, 0.98, 0.99), scene(0.4));
    EXPECT_DOUBLE_EQ(bright.high_key, 0.0);
    EXPECT_LT(bright.exposure_ev, 0.0);
    EXPECT_GT(bright.exposure_ev, -0.5);
    EXPECT_NEAR(bright.needed_ev, -ev_between(kBandHigh, 0.92), 1e-12);
}

TEST(ToneProposal, DeficitIsTheSmallerOfTheMedianAndTheBrightContent) {
    ToneProposal policy;
    LightnessQuantiles q;
    q.p50 = 0.25;
    q.p90 = 0.50;
    ToneDeficit d = tone_deficit(q, policy);
    EXPECT_NEAR(d.median_ev, ev_between(0.25, kBandLow), 1e-12);
    EXPECT_NEAR(d.bright_ev, ev_between(0.50, kBrightContentFloor), 1e-12);
    EXPECT_DOUBLE_EQ(d.deficit_ev, std::min(d.median_ev, d.bright_ev));
    EXPECT_EQ(d.excess_ev, 0.0);
    // An exposure change of E EV in the base look's linear segment moves both by E: the deficit is
    // measured in EV of display luminance.
    q.p90 = 0.75;  // bright content above its floor: no deficit
    d = tone_deficit(q, policy);
    EXPECT_EQ(d.deficit_ev, 0.0);
    q.p50 = 0.90;
    d = tone_deficit(q, policy);
    EXPECT_NEAR(d.excess_ev, ev_between(kBandHigh, 0.90), 1e-12);
}

TEST(ToneProposal, NoProposalWithoutSignal) {
    const ToneProposal t = propose_tone(tone_stats(0.01, 0.02), scene(8.0, 100));
    EXPECT_FALSE(t.evidence);
    EXPECT_EQ(t.signal_evidence, 0.0);
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
