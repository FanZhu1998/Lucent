// Unit tests for pre-demosaic white balance and the highlight policy (sensor.hpp).

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/core/cancellation.hpp>
#include <lucent/engine/sensor.hpp>

#include "sensor_test_support.hpp"

namespace lucent::engine {
namespace {

namespace ts = test_support;
using image::BayerLayout;
using image::CfaColor;
using image::CfaPattern;
using image::PixelRect;

raw::RawMetadata bayer_metadata(std::array<float, 4> as_shot, std::array<float, 4> daylight) {
    raw::RawMetadata m;
    m.representation.kind = raw::RawRepresentation::Mosaic;
    m.representation.channel_colors = ts::kBayerChannels;
    m.color.as_shot_multipliers = as_shot;
    m.color.daylight_multipliers = daylight;
    return m;
}

TEST(WhiteBalance, AsShotMultipliersAreNormalisedToGreen) {
    // Canon-style: four multipliers in 1/1024 units.
    auto wb = resolve_white_balance(bayer_metadata({1659.0f, 1024.0f, 2315.0f, 1024.0f}, {}));
    ASSERT_TRUE(wb) << wb.error().describe();
    EXPECT_EQ(wb->source, WhiteBalanceSource::AsShot);
    EXPECT_DOUBLE_EQ(wb->multipliers[0], 1659.0 / 1024.0);
    EXPECT_DOUBLE_EQ(wb->multipliers[1], 1.0);
    EXPECT_DOUBLE_EQ(wb->multipliers[2], 2315.0 / 1024.0);
    EXPECT_DOUBLE_EQ(wb->multipliers[3], 1.0);
    EXPECT_NE(wb->basis.find("as-shot"), std::string::npos) << wb->basis;

    // DNG-style: the fourth multiplier is 0 and takes Green's value.
    wb = resolve_white_balance(bayer_metadata({2.0f, 0.5f, 1.5f, 0.0f}, {}));
    ASSERT_TRUE(wb);
    EXPECT_EQ(wb->multipliers, (std::array<double, 4>{4.0, 1.0, 3.0, 1.0}));
}

TEST(WhiteBalance, FallsBackToDaylightThenToUnity) {
    auto wb = resolve_white_balance(bayer_metadata({0.0f, 0.0f, 0.0f, 0.0f}, {2.0f, 1.0f, 1.5f, 0.0f}));
    ASSERT_TRUE(wb);
    EXPECT_EQ(wb->source, WhiteBalanceSource::Daylight);
    EXPECT_EQ(wb->multipliers, (std::array<double, 4>{2.0, 1.0, 1.5, 1.0}));
    EXPECT_NE(wb->basis.find("as-shot multipliers unusable"), std::string::npos) << wb->basis;

    wb = resolve_white_balance(bayer_metadata({1.0f, 1.0f, std::numeric_limits<float>::quiet_NaN(), 1.0f},
                                              {0.0f, 0.0f, 0.0f, 0.0f}));
    ASSERT_TRUE(wb);
    EXPECT_EQ(wb->source, WhiteBalanceSource::Unavailable);
    EXPECT_EQ(wb->multipliers, (std::array<double, 4>{1.0, 1.0, 1.0, 1.0}));
    EXPECT_NE(wb->basis.find("not white balanced"), std::string::npos) << wb->basis;

    WhiteBalanceOptions daylight;
    daylight.request = WhiteBalanceRequest::Daylight;
    wb = resolve_white_balance(bayer_metadata({3.0f, 1.0f, 2.0f, 1.0f}, {2.0f, 1.0f, 1.25f, 1.0f}), daylight);
    ASSERT_TRUE(wb);
    EXPECT_EQ(wb->source, WhiteBalanceSource::Daylight);
    EXPECT_DOUBLE_EQ(wb->multipliers[2], 1.25);
}

TEST(WhiteBalance, CustomMultipliersAreValidated) {
    WhiteBalanceOptions custom;
    custom.request = WhiteBalanceRequest::Custom;
    custom.custom_multipliers = {4.0, 2.0, 3.0, 0.0};
    auto wb = resolve_white_balance(bayer_metadata({}, {}), custom);
    ASSERT_TRUE(wb);
    EXPECT_EQ(wb->source, WhiteBalanceSource::Custom);
    EXPECT_EQ(wb->multipliers, (std::array<double, 4>{2.0, 1.0, 1.5, 1.0}));

    custom.custom_multipliers = {4.0, 2.0, -3.0, 2.0};
    ts::expect_error(resolve_white_balance(bayer_metadata({}, {}), custom), ErrorCode::InvalidArgument,
                     "engine.white_balance.resolve");
}

TEST(WhiteBalance, MonochromeAndPreBalancedDataAreNotBalancedAgain) {
    raw::RawMetadata mono;
    mono.representation.kind = raw::RawRepresentation::Monochrome;
    mono.representation.channel_colors = {CfaColor::White};
    auto wb = resolve_white_balance(mono);
    ASSERT_TRUE(wb);
    EXPECT_EQ(wb->source, WhiteBalanceSource::NotApplicable);
    EXPECT_EQ(wb->multipliers, (std::array<double, 4>{1.0, 0.0, 0.0, 0.0}));

    raw::RawMetadata applied = bayer_metadata({2.0f, 1.0f, 1.5f, 1.0f}, {});
    applied.representation.kind = raw::RawRepresentation::LinearColor;
    applied.representation.channel_colors = ts::kRgbChannels;
    applied.prior_processing.push_back({raw::PriorProcessingKind::WhiteBalanceApplied, "sRAW"});
    wb = resolve_white_balance(applied);
    ASSERT_TRUE(wb);
    EXPECT_EQ(wb->source, WhiteBalanceSource::AlreadyApplied);
    EXPECT_EQ(wb->multipliers, (std::array<double, 4>{1.0, 1.0, 1.0, 0.0}));

    // An explicit custom balance is honoured and says that it acts on top of the applied one.
    WhiteBalanceOptions custom;
    custom.request = WhiteBalanceRequest::Custom;
    custom.custom_multipliers = {1.1, 1.0, 0.9, 0.0};
    wb = resolve_white_balance(applied, custom);
    ASSERT_TRUE(wb);
    EXPECT_EQ(wb->source, WhiteBalanceSource::Custom);
    EXPECT_NE(wb->basis.find("relative"), std::string::npos) << wb->basis;
}

TEST(WhiteBalance, ColoursWithoutAModelAreUnsupported) {
    raw::RawMetadata m;
    m.representation.kind = raw::RawRepresentation::Mosaic;
    m.representation.channel_colors = {CfaColor::Cyan, CfaColor::Magenta, CfaColor::Yellow, CfaColor::Green};
    ts::expect_error(resolve_white_balance(m), ErrorCode::Unsupported, "engine.white_balance.resolve");
    m.representation.channel_colors = {CfaColor::Red, CfaColor::Blue};
    ts::expect_error(resolve_white_balance(m), ErrorCode::InvalidArgument, "engine.white_balance.resolve");
}

// A 4 x 4 RGGB mosaic: black 0, white 1000; the left half is saturated in every channel, the
// right half holds 100 (and one negative sample).
SensorImage bayer_sensor() {
    const auto decoded = ts::make_mosaic(4, 4, PixelRect{0, 0, 4, 4}, CfaPattern::bayer(BayerLayout::Rggb),
                                         ts::simple_levels(0.0, 1000.0),
                                         [](std::int32_t x, std::int32_t, std::int32_t) { return x < 2 ? 1000.0 : 100.0; });
    auto image = normalize_sensor(decoded);
    EXPECT_TRUE(image);
    image->pixels.at(3, 3, 0) = -0.05f;
    return std::move(*image);
}

TEST(WhiteBalance, ClipToLowestSaturationKeepsClippedHighlightsNeutral) {
    SensorImage image = bayer_sensor();
    WhiteBalance wb;
    wb.source = WhiteBalanceSource::AsShot;
    wb.multipliers = {2.0, 1.0, 1.5, 1.0};
    ASSERT_TRUE(apply_white_balance(image, wb));
    ASSERT_TRUE(image.white_balance);
    const WhiteBalanceRecord& r = *image.white_balance;
    EXPECT_EQ(r.version, kWhiteBalanceVersion);
    EXPECT_EQ(r.highlights, HighlightPolicy::ClipToLowestSaturation);
    EXPECT_EQ(r.applied.source, WhiteBalanceSource::AsShot);
    // Every channel saturates at normalised 1.0; the lowest balanced saturation is green's 1.0.
    EXPECT_FLOAT_EQ(r.clip_level, 1.0f);
    // The saturated half becomes exactly 1.0 in every channel (red 2.0 and blue 1.5 are limited).
    for (std::int32_t y = 0; y < 4; ++y) {
        for (std::int32_t x = 0; x < 2; ++x) {
            EXPECT_EQ(image.pixels.at(x, y, 0), 1.0f) << x << "," << y;
        }
    }
    // Unsaturated samples are only multiplied: red (2, 0) 0.1 * 2, blue (3, 1) 0.1 * 1.5.
    EXPECT_FLOAT_EQ(image.pixels.at(2, 0, 0), 0.2f);
    EXPECT_FLOAT_EQ(image.pixels.at(3, 1, 0), 0.15f);
    EXPECT_FLOAT_EQ(image.pixels.at(3, 0, 0), 0.1f);
    // Negative values are balanced, never clipped.
    EXPECT_FLOAT_EQ(image.pixels.at(3, 3, 0), -0.05f * 1.5f);
    // Red and blue saturated sites were limited: 2 red + 2 blue sites in the left half.
    EXPECT_EQ(r.clipped_samples, 4u);
    // The saturation mask is untouched.
    EXPECT_EQ(image.saturation.count_flagged(), 8u);
}

TEST(WhiteBalance, NoHighlightPolicyKeepsBalancedValues) {
    SensorImage image = bayer_sensor();
    WhiteBalance wb;
    wb.multipliers = {2.0, 1.0, 1.5, 1.0};
    ASSERT_TRUE(apply_white_balance(image, wb, HighlightPolicy::None));
    EXPECT_EQ(image.pixels.at(0, 0, 0), 2.0f);  // saturated red, balanced and kept
    EXPECT_EQ(image.pixels.at(1, 1, 0), 1.5f);  // saturated blue
    EXPECT_TRUE(std::isinf(image.white_balance->clip_level));
    EXPECT_EQ(image.white_balance->clipped_samples, 0u);
}

TEST(WhiteBalance, ClipLevelUsesEachChannelsSaturationLevel) {
    // apply_white_balance() takes each channel's saturation level from the normalisation record.
    // normalize_sensor() sets them all to 1 (the white level); a record with a lower green2
    // level (0.75) makes that channel's balanced saturation the lowest.
    const auto decoded =
        ts::make_mosaic(4, 4, PixelRect{0, 0, 4, 4}, CfaPattern::bayer(BayerLayout::Rggb),
                        ts::simple_levels(0.0, 1000.0), [](std::int32_t, std::int32_t, std::int32_t) { return 900.0; });
    auto image = normalize_sensor(decoded);
    ASSERT_TRUE(image);
    image->normalization.saturation_level[3] = 0.75f;
    WhiteBalance wb;
    wb.multipliers = {2.0, 1.0, 1.5, 1.0};
    ASSERT_TRUE(apply_white_balance(*image, wb));
    EXPECT_FLOAT_EQ(image->white_balance->clip_level, 0.75f);
    for (std::int32_t y = 0; y < 4; ++y) {
        for (std::int32_t x = 0; x < 4; ++x) {
            EXPECT_EQ(image->pixels.at(x, y, 0), 0.75f);  // every channel limited to the same level
        }
    }
}

TEST(WhiteBalance, ClipsAtTheWhiteLevelTimesTheSmallestMultiplierLikeDcraw) {
    // A linear_max below white (as LibRaw reports for Canon, Nikon, Sony) must not lower the
    // clip: balanced values up to the smallest multiplier (green, 1) survive, and the clipped
    // neutral is exactly 1 in every channel.
    raw::SensorLevels levels = ts::simple_levels(0.0, 1000.0);
    levels.linear_max = {850.0, 850.0, 850.0, 850.0};
    const auto decoded = ts::make_mosaic(4, 4, PixelRect{0, 0, 4, 4}, CfaPattern::bayer(BayerLayout::Rggb), levels,
                                         [](std::int32_t x, std::int32_t, std::int32_t) {
                                             return x < 2 ? 900.0 : 1000.0;  // valid data, then white
                                         });
    auto image = normalize_sensor(decoded);
    ASSERT_TRUE(image);
    WhiteBalance wb;
    wb.multipliers = {2.0, 1.0, 1.5, 1.0};
    ASSERT_TRUE(apply_white_balance(*image, wb));
    EXPECT_EQ(image->white_balance->clip_level, 1.0f);
    EXPECT_FLOAT_EQ(image->pixels.at(1, 0, 0), 0.9f);  // green at 900: kept, not clipped at 0.85
    EXPECT_FLOAT_EQ(image->pixels.at(0, 1, 0), 0.9f);  // green2
    EXPECT_EQ(image->pixels.at(0, 0, 0), 1.0f);        // red 0.9 * 2 limited to 1
    for (std::int32_t y = 0; y < 4; ++y) {
        for (std::int32_t x = 2; x < 4; ++x) {
            EXPECT_EQ(image->pixels.at(x, y, 0), 1.0f);  // saturated sites: neutral at 1
        }
    }
}

TEST(WhiteBalance, LinearColorAndMonochromeAreBalancedPerChannel) {
    const auto linear = ts::make_linear(2, 2, PixelRect{0, 0, 2, 2}, ts::simple_levels(0.0, 1000.0),
                                        [](std::int32_t, std::int32_t, std::int32_t) { return 200.0; });
    auto image = normalize_sensor(linear);
    ASSERT_TRUE(image);
    WhiteBalance wb;
    wb.multipliers = {2.0, 1.0, 3.0, 0.0};
    ASSERT_TRUE(apply_white_balance(*image, wb));
    EXPECT_FLOAT_EQ(image->pixels.at(1, 1, 0), 0.4f);
    EXPECT_FLOAT_EQ(image->pixels.at(1, 1, 1), 0.2f);
    EXPECT_FLOAT_EQ(image->pixels.at(1, 1, 2), 0.6f);

    const auto mono = ts::make_monochrome(2, 2, PixelRect{0, 0, 2, 2}, ts::simple_levels(0.0, 1000.0),
                                          [](std::int32_t, std::int32_t, std::int32_t) { return 1000.0; });
    auto mono_image = normalize_sensor(mono);
    ASSERT_TRUE(mono_image);
    raw::RawMetadata mono_metadata = mono.metadata;
    ASSERT_TRUE(white_balance_sensor(*mono_image, mono_metadata));
    EXPECT_EQ(mono_image->white_balance->applied.source, WhiteBalanceSource::NotApplicable);
    EXPECT_EQ(mono_image->pixels.at(0, 0, 0), 1.0f);
}

TEST(WhiteBalance, ApplyRejectsInvalidRequestsAndIsNeverAppliedTwice) {
    SensorImage image = bayer_sensor();
    WhiteBalance wb;
    wb.multipliers = {2.0, 1.0, 0.0, 1.0};  // blue unusable
    ts::expect_error(apply_white_balance(image, wb), ErrorCode::InvalidArgument, "engine.white_balance.apply");
    EXPECT_FALSE(image.white_balance);

    wb.multipliers = {2.0, 1.0, 1.5, 1.0};
    ASSERT_TRUE(apply_white_balance(image, wb));
    ts::expect_error(apply_white_balance(image, wb), ErrorCode::InvalidArgument, "engine.white_balance.apply");

    SensorImage empty;
    ts::expect_error(apply_white_balance(empty, wb), ErrorCode::InvalidArgument, "engine.white_balance.apply");

    SensorImage cancelled = bayer_sensor();
    core::CancellationSource source;
    source.cancel();
    ts::expect_error(apply_white_balance(cancelled, wb, HighlightPolicy::ClipToLowestSaturation, source.token()),
                     ErrorCode::Cancelled, "engine.white_balance.apply");
}

TEST(WhiteBalance, ResolveAndApplyUseTheMetadata) {
    const auto decoded = ts::make_mosaic(4, 4, PixelRect{0, 0, 4, 4}, CfaPattern::bayer(BayerLayout::Grbg),
                                         ts::simple_levels(0.0, 1000.0),
                                         [](std::int32_t, std::int32_t, std::int32_t) { return 100.0; });
    raw::RawMetadata metadata = decoded.metadata;
    metadata.color.as_shot_multipliers = {3.0f, 1.5f, 2.25f, 1.5f};
    auto image = normalize_sensor(decoded);
    ASSERT_TRUE(image);
    ASSERT_TRUE(white_balance_sensor(*image, metadata));
    EXPECT_EQ(image->white_balance->applied.source, WhiteBalanceSource::AsShot);
    // GRBG: (1, 0) is red (x2), (0, 1) blue (x1.5), (0, 0) green.
    EXPECT_FLOAT_EQ(image->pixels.at(1, 0, 0), 0.2f);
    EXPECT_FLOAT_EQ(image->pixels.at(0, 1, 0), 0.15f);
    EXPECT_FLOAT_EQ(image->pixels.at(0, 0, 0), 0.1f);
}

TEST(WhiteBalance, NamesAreStable) {
    EXPECT_EQ(to_string(WhiteBalanceSource::AsShot), "as_shot");
    EXPECT_EQ(to_string(WhiteBalanceSource::Unavailable), "unavailable");
    EXPECT_EQ(to_string(HighlightPolicy::ClipToLowestSaturation), "clip_to_lowest_saturation");
    EXPECT_EQ(to_string(HighlightPolicy::None), "none");
    EXPECT_EQ(static_cast<int>(WhiteBalanceSource::Unavailable), 6);
    EXPECT_EQ(static_cast<int>(HighlightPolicy::None), 2);
}

}  // namespace
}  // namespace lucent::engine
