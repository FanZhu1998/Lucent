// Unit tests for sensor normalisation (sensor.hpp) on synthetic decoded-RAW data.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
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

// A varied 12-bit test signal.
double varied(std::int32_t x, std::int32_t y, std::int32_t s) {
    return static_cast<double>(100 + (x * 131 + y * 71 + s * 997 + x * y * 13) % 3900);
}

// Largest black level of any visible site of channel `c` (brute force over the visible area).
double brute_black_max(const raw::SensorLevels& levels, const SensorImage& image, std::size_t c) {
    double best = -std::numeric_limits<double>::infinity();
    for (std::int32_t y = 0; y < image.pixels.height(); ++y) {
        for (std::int32_t x = 0; x < image.pixels.width(); ++x) {
            const std::int32_t site = image.representation == raw::RawRepresentation::Mosaic
                                          ? image.channel_of_site(x, y)
                                          : static_cast<std::int32_t>(c);
            if (site == static_cast<std::int32_t>(c)) {
                best = std::max(best, levels.black_level_at(c, x, y));
            }
        }
    }
    return best;
}

// Checks every normalised sample against (raw - black(site)) / (white - black_max[c]), computed
// independently in double and rounded once to float.
void expect_normalized(const raw::DecodedRaw& decoded, const SensorImage& image,
                       const std::array<double, 4>& black_max) {
    const raw::RawMetadata& m = decoded.metadata;
    const PixelRect& v = m.geometry.visible_area;
    const auto& frame = std::get<image::ImageBuffer<std::uint16_t>>(raw::pixels_of(decoded.image));
    const std::int32_t channels = image.pixels.channels();
    std::size_t mismatches = 0;
    for (std::int32_t y = 0; y < v.height; ++y) {
        for (std::int32_t x = 0; x < v.width; ++x) {
            for (std::int32_t s = 0; s < channels; ++s) {
                const auto c = static_cast<std::size_t>(channels == 1 && image.representation ==
                                                                             raw::RawRepresentation::Mosaic
                                                            ? image.channel_of_site(x, y)
                                                            : s);
                const double raw_value = frame.at(v.x + x, v.y + y, s);
                const double black = m.levels.black_level_at(c, x, y);
                const float expected = static_cast<float>((raw_value - black) / (m.levels.white - black_max[c]));
                if (image.pixels.at(x, y, s) != expected) {
                    ++mismatches;
                }
            }
        }
    }
    EXPECT_EQ(mismatches, 0u);
}

TEST(SensorNormalization, SubtractsBlackAndScalesPerChannelWithoutClipping) {
    raw::SensorLevels levels = ts::simple_levels(64.0, 4000.0);
    levels.channel_black = {1.0, 2.0, 3.0, 4.0};
    // 10 x 8 frame with the visible area at (3, 1); GBRG at the visible origin.
    const PixelRect visible{3, 1, 6, 5};
    const CfaPattern cfa = CfaPattern::bayer(BayerLayout::Gbrg);
    const auto decoded = ts::make_mosaic(10, 8, visible, cfa, levels, [](std::int32_t x, std::int32_t y, std::int32_t) {
        // Below black at one site, above white at another.
        if (x == 4 && y == 2) {
            return 10.0;
        }
        if (x == 5 && y == 3) {
            return 4090.0;
        }
        return varied(x, y, 0);
    });
    auto image = normalize_sensor(decoded);
    ASSERT_TRUE(image) << image.error().describe();
    EXPECT_EQ(image->representation, raw::RawRepresentation::Mosaic);
    EXPECT_EQ(image->encoding, image::ColorEncoding::camera_native_mosaic());
    EXPECT_EQ(image->cfa, cfa);
    EXPECT_EQ(image->pixels.width(), 6);
    EXPECT_EQ(image->pixels.height(), 5);
    EXPECT_EQ(image->pixels.channels(), 1);
    EXPECT_EQ(image->channel_colors, ts::kBayerChannels);
    EXPECT_FALSE(image->white_balance);

    const NormalizationRecord& r = image->normalization;
    EXPECT_EQ(r.version, kSensorNormalizationVersion);
    EXPECT_EQ(r.source_area, visible);
    EXPECT_EQ(r.levels_provenance, raw::Provenance::Decoder);
    EXPECT_DOUBLE_EQ(r.white, 4000.0);
    EXPECT_EQ(r.black_max, (std::array<double, 4>{65.0, 66.0, 67.0, 68.0}));
    EXPECT_FALSE(r.black_pattern_applied);
    EXPECT_FALSE(r.column_deltas_applied);
    EXPECT_FALSE(r.row_deltas_applied);
    expect_normalized(decoded, *image, r.black_max);

    // Nothing is clipped: below black stays negative, above white stays above 1.
    EXPECT_LT(image->pixels.at(1, 1, 0), 0.0f);
    EXPECT_GT(image->pixels.at(2, 2, 0), 1.0f);
}

TEST(SensorNormalization, SubtractsTheSpatialBlackPatternPerSite) {
    // A 1 x 3 pattern on a Bayer mosaic: the joint period is 2 x 6, so the channels see different
    // maxima (the range of each channel uses its own highest black).
    raw::SensorLevels levels = ts::simple_levels(100.0, 5000.0);
    levels.pattern_rows = 1;
    levels.pattern_cols = 3;
    levels.black_pattern = {0.0, 10.0, 20.0};
    levels.channel_black = {0.0, 5.0, 0.0, 7.0};
    const CfaPattern cfa = CfaPattern::bayer(BayerLayout::Rggb);
    const auto decoded = ts::make_mosaic(14, 9, PixelRect{2, 2, 12, 7}, cfa, levels, varied);
    auto image = normalize_sensor(decoded);
    ASSERT_TRUE(image) << image.error().describe();
    EXPECT_TRUE(image->normalization.black_pattern_applied);
    for (std::size_t c = 0; c < 4; ++c) {
        EXPECT_DOUBLE_EQ(image->normalization.black_max[c], brute_black_max(levels, *image, c)) << c;
    }
    expect_normalized(decoded, *image, image->normalization.black_max);
}

TEST(SensorNormalization, SubtractsRowAndColumnDeltas) {
    raw::SensorLevels levels = ts::simple_levels(200.0, 4095.0);
    const PixelRect visible{1, 1, 8, 6};
    for (std::int32_t x = 0; x < visible.width; ++x) {
        levels.column_black_delta.push_back(0.25 * x - 1.0);
    }
    for (std::int32_t y = 0; y < visible.height; ++y) {
        levels.row_black_delta.push_back(y % 2 == 0 ? 3.0 : -2.0);
    }
    const auto decoded = ts::make_mosaic(9, 7, visible, CfaPattern::bayer(BayerLayout::Bggr), levels, varied);
    auto image = normalize_sensor(decoded);
    ASSERT_TRUE(image) << image.error().describe();
    EXPECT_TRUE(image->normalization.column_deltas_applied);
    EXPECT_TRUE(image->normalization.row_deltas_applied);
    // black_max adds the largest column delta (0.75) and the largest row delta (3).
    for (std::size_t c = 0; c < 4; ++c) {
        EXPECT_DOUBLE_EQ(image->normalization.black_max[c], 200.0 + 0.75 + 3.0) << c;
    }
    expect_normalized(decoded, *image, image->normalization.black_max);
    // One site by hand: black = 200 + delta_h[2] (-0.5) + delta_v[1] (-2).
    const double raw_value = varied(visible.x + 2, visible.y + 1, 0);
    EXPECT_FLOAT_EQ(image->pixels.at(2, 1, 0),
                    static_cast<float>((raw_value - (200.0 - 0.5 - 2.0)) / (4095.0 - 203.75)));
}

TEST(SensorNormalization, FlagsSaturationFromRawValuesBeforeScaling) {
    raw::SensorLevels levels = ts::simple_levels(100.0, 4000.0);
    // The decoder's linear_max never lowers the threshold (LibRaw invents it for some cameras and
    // real data continues above it): every channel saturates at the white level, including
    // green2, whose linear_max (3000) lies between black and white.
    levels.linear_max = {50.0, 0.0, 4100.0, 3000.0};
    const CfaPattern cfa = CfaPattern::bayer(BayerLayout::Rggb);
    const auto decoded = ts::make_mosaic(4, 4, PixelRect{0, 0, 4, 4}, cfa, levels,
                                         [](std::int32_t x, std::int32_t y, std::int32_t) {
                                             // Row 0: R G R G; row 1: G2 B G2 B.
                                             if (y == 0) {
                                                 return x == 0 ? 4000.0 : (x == 1 ? 3999.0 : 500.0);
                                             }
                                             if (y == 1) {
                                                 return x == 0 ? 3000.0 : (x == 2 ? 2999.0 : 4050.0);
                                             }
                                             return 1000.0;
                                         });
    auto image = normalize_sensor(decoded);
    ASSERT_TRUE(image) << image.error().describe();
    const NormalizationRecord& r = image->normalization;
    EXPECT_EQ(r.version, kSensorNormalizationVersion);
    EXPECT_EQ(r.saturation_threshold, (std::array<double, 4>{4000.0, 4000.0, 4000.0, 4000.0}));
    for (std::size_t c = 0; c < 4; ++c) {
        EXPECT_EQ(r.saturation_level[c], 1.0f) << c;
    }
    const SaturationMask& mask = image->saturation;
    ASSERT_EQ(mask.width(), 4);
    EXPECT_EQ(mask.at(0, 0), 1u << 0);  // red at white
    EXPECT_EQ(mask.at(1, 0), 0u);       // green just below white
    EXPECT_EQ(mask.at(0, 1), 0u);       // green2 at its linear_max: valid data, not saturated
    EXPECT_EQ(mask.at(2, 1), 0u);
    EXPECT_EQ(mask.at(1, 1), 1u << 2);  // blue above white
    EXPECT_EQ(mask.at(3, 1), 1u << 2);
    EXPECT_EQ(r.saturated_samples, 3u);
    EXPECT_EQ(mask.count_flagged(), 3u);
    // Values between linear_max and white keep their scaled value.
    EXPECT_FLOAT_EQ(image->pixels.at(0, 1, 0), static_cast<float>((3000.0 - 100.0) / (4000.0 - 100.0)));
    // Saturated samples keep their scaled value (above 1 for blue).
    EXPECT_GT(image->pixels.at(1, 1, 0), 1.0f);
}

TEST(SensorNormalization, LinearColorKeepsThreeChannelsAndFlagsEachChannel) {
    raw::SensorLevels levels = ts::simple_levels(0.0, 1000.0);
    levels.channel_black = {10.0, 20.0, 30.0, 0.0};
    const auto decoded = ts::make_linear(5, 4, PixelRect{1, 0, 4, 4}, levels,
                                         [](std::int32_t x, std::int32_t y, std::int32_t s) {
                                             return x == 2 && y == 1 && s == 1 ? 1000.0 : 100.0 * (s + 1) + x + y;
                                         });
    auto image = normalize_sensor(decoded);
    ASSERT_TRUE(image) << image.error().describe();
    EXPECT_EQ(image->representation, raw::RawRepresentation::LinearColor);
    EXPECT_EQ(image->encoding, image::ColorEncoding::camera_native_linear());
    EXPECT_TRUE(image->cfa.empty());
    EXPECT_EQ(image->pixels.channels(), 3);
    EXPECT_EQ(image->normalization.black_max, (std::array<double, 4>{10.0, 20.0, 30.0, 0.0}));
    expect_normalized(decoded, *image, image->normalization.black_max);
    EXPECT_EQ(image->saturation.at(1, 1), 1u << 1);  // raw (2, 1) green
    EXPECT_EQ(image->normalization.saturated_samples, 1u);
    EXPECT_EQ(image->channel_of_site(0, 0), -1);
}

TEST(SensorNormalization, MonochromeKeepsOneChannel) {
    const auto decoded = ts::make_monochrome(6, 6, PixelRect{0, 0, 6, 6}, ts::simple_levels(64.0, 1023.0), varied);
    auto image = normalize_sensor(decoded);
    ASSERT_TRUE(image) << image.error().describe();
    EXPECT_EQ(image->representation, raw::RawRepresentation::Monochrome);
    EXPECT_EQ(image->encoding, raw::camera_native_monochrome());
    EXPECT_EQ(image->pixels.channels(), 1);
    EXPECT_EQ(image->channel_colors, std::vector<CfaColor>{CfaColor::White});
    expect_normalized(decoded, *image, image->normalization.black_max);
}

TEST(SensorNormalization, FloatSamplesAreNormalisedAndNonFiniteOnesAreCorrupt) {
    raw::SensorLevels levels = ts::simple_levels(0.0, 1.0);
    const CfaPattern cfa = CfaPattern::bayer(BayerLayout::Grbg);
    auto decoded = ts::make_mosaic<float>(4, 4, PixelRect{0, 0, 4, 4}, cfa, levels,
                                          [](std::int32_t x, std::int32_t y, std::int32_t) { return 0.125 * (x + y); });
    auto image = normalize_sensor(decoded);
    ASSERT_TRUE(image) << image.error().describe();
    EXPECT_FLOAT_EQ(image->pixels.at(3, 2, 0), 0.625f);
    EXPECT_EQ(image->saturation.at(3, 3), 0u);

    std::get<image::ImageBuffer<float>>(std::get<raw::MosaicFrame>(decoded.image).pixels).at(2, 1, 0) =
        std::numeric_limits<float>::quiet_NaN();
    ts::expect_error(normalize_sensor(decoded), ErrorCode::Corrupt, "engine.sensor.samples");
}

TEST(SensorNormalization, RejectsLevelsThatCannotBeNormalised) {
    const CfaPattern cfa = CfaPattern::bayer(BayerLayout::Rggb);
    {
        const auto decoded = ts::make_mosaic(4, 4, PixelRect{0, 0, 4, 4}, cfa, ts::simple_levels(500.0, 500.0), varied);
        ts::expect_error(normalize_sensor(decoded), ErrorCode::Corrupt, "engine.sensor.levels");
    }
    {
        raw::SensorLevels levels = ts::simple_levels(100.0, 4000.0);
        levels.channel_black = {0.0, 0.0, 3950.0, 0.0};  // blue black above white
        const auto decoded = ts::make_mosaic(4, 4, PixelRect{0, 0, 4, 4}, cfa, levels, varied);
        ts::expect_error(normalize_sensor(decoded), ErrorCode::Corrupt, "engine.sensor.levels");
    }
    {
        raw::SensorLevels levels = ts::simple_levels(std::numeric_limits<double>::quiet_NaN(), 4000.0);
        const auto decoded = ts::make_mosaic(4, 4, PixelRect{0, 0, 4, 4}, cfa, levels, varied);
        ts::expect_error(normalize_sensor(decoded), ErrorCode::Corrupt, "engine.sensor.levels");
    }
}

TEST(SensorNormalization, RejectsNewerContractsAndContractViolations) {
    const CfaPattern cfa = CfaPattern::bayer(BayerLayout::Rggb);
    auto decoded = ts::make_mosaic(4, 4, PixelRect{0, 0, 4, 4}, cfa, ts::simple_levels(0.0, 4095.0), varied);
    decoded.contract = core::ContractVersion{raw::kDecodedRawContractVersion.major,
                                             raw::kDecodedRawContractVersion.minor + 1};
    ts::expect_error(normalize_sensor(decoded), ErrorCode::VersionMismatch, "engine.sensor.contract");
    decoded.contract = core::ContractVersion{raw::kDecodedRawContractVersion.major + 1, 0};
    ts::expect_error(normalize_sensor(decoded), ErrorCode::VersionMismatch, "engine.sensor.contract");

    decoded.contract = raw::kDecodedRawContractVersion;
    decoded.metadata.geometry.raw_width = 5;  // the buffer is 4 wide: a decoder defect
    ts::expect_error(normalize_sensor(decoded), ErrorCode::Internal, "raw.contract");
}

TEST(SensorNormalization, LayoutsWithoutARouteAreUnsupported) {
    {
        // A 4-colour CMYG mosaic.
        const std::array<CfaColor, 4> tile = {CfaColor::Cyan, CfaColor::Magenta, CfaColor::Yellow, CfaColor::Green};
        auto cmyg = CfaPattern::create(2, 2, tile);
        ASSERT_TRUE(cmyg);
        const auto decoded = ts::make_mosaic(4, 4, PixelRect{0, 0, 4, 4}, *cmyg, ts::simple_levels(0.0, 4095.0),
                                             varied,
                                             {CfaColor::Cyan, CfaColor::Magenta, CfaColor::Yellow, CfaColor::Green});
        ts::expect_error(normalize_sensor(decoded), ErrorCode::Unsupported, "engine.sensor.layout");
    }
    {
        // Full-colour data whose channels are not R, G, B.
        auto decoded = ts::make_linear(4, 4, PixelRect{0, 0, 4, 4}, ts::simple_levels(0.0, 4095.0), varied);
        decoded.metadata.representation.channel_colors = {CfaColor::Cyan, CfaColor::Magenta, CfaColor::Yellow};
        ts::expect_error(normalize_sensor(decoded), ErrorCode::Unsupported, "engine.sensor.layout");
    }
    {
        // Data the decoder marked as not decodable.
        auto decoded = ts::make_mosaic(4, 4, PixelRect{0, 0, 4, 4}, CfaPattern::bayer(BayerLayout::Rggb),
                                       ts::simple_levels(0.0, 4095.0), varied);
        decoded.metadata.interpretation.issues.push_back(
            {raw::InterpretationIssueKind::UnsupportedSensorLayout, "SuperCCD"});
        decoded.metadata.interpretation.status = raw::InterpretationStatus::NotDecodable;
        ts::expect_error(normalize_sensor(decoded), ErrorCode::Unsupported, "engine.sensor.layout");
    }
}

TEST(SensorNormalization, OtherInterpretationIssuesDoNotStopNormalisation) {
    auto decoded = ts::make_mosaic(4, 4, PixelRect{0, 0, 4, 4}, CfaPattern::bayer(BayerLayout::Rggb),
                                   ts::simple_levels(0.0, 4095.0), varied);
    decoded.metadata.interpretation.issues.push_back(
        {raw::InterpretationIssueKind::MandatoryOperationUnsupported, "GainMap"});
    decoded.metadata.interpretation.status = raw::InterpretationStatus::RequiresUnsupportedOperation;
    EXPECT_TRUE(normalize_sensor(decoded));
}

TEST(SensorNormalization, HonoursCancellationAndLimits) {
    const auto decoded = ts::make_mosaic(8, 8, PixelRect{0, 0, 8, 8}, CfaPattern::bayer(BayerLayout::Rggb),
                                         ts::simple_levels(0.0, 4095.0), varied);
    core::CancellationSource source;
    source.cancel();
    ts::expect_error(normalize_sensor(decoded, {}, source.token()), ErrorCode::Cancelled, "engine.sensor.normalize");

    NormalizeOptions tight;
    tight.limits.max_bytes = 100;  // an 8 x 8 float image needs more
    auto limited = normalize_sensor(decoded, tight);
    ASSERT_FALSE(limited);
    EXPECT_EQ(limited.error().code, ErrorCode::LimitExceeded);
}

TEST(SensorNormalization, ParallelAndSerialResultsAreIdentical) {
    raw::SensorLevels levels = ts::simple_levels(64.0, 4000.0);
    levels.pattern_rows = 2;
    levels.pattern_cols = 2;
    levels.black_pattern = {1.0, 2.0, 3.0, 4.0};
    const auto decoded = ts::make_mosaic(67, 45, PixelRect{3, 2, 61, 41}, CfaPattern::bayer(BayerLayout::Gbrg),
                                         levels, varied);
    NormalizeOptions serial;
    serial.parallel = false;
    auto a = normalize_sensor(decoded);
    auto b = normalize_sensor(decoded, serial);
    ASSERT_TRUE(a && b);
    for (std::int32_t y = 0; y < a->pixels.height(); ++y) {
        ASSERT_TRUE(std::equal(a->pixels.row(y), a->pixels.row(y) + a->pixels.width(), b->pixels.row(y)));
        ASSERT_TRUE(std::equal(a->saturation.row(y), a->saturation.row(y) + a->saturation.width(),
                               b->saturation.row(y)));
    }
    EXPECT_EQ(a->normalization.saturated_samples, b->normalization.saturated_samples);
}

TEST(SensorNormalization, CfaIsReexpressedAtTheVisibleOrigin) {
    // X-Trans tile at the raw origin; the visible area starts at (1, 2).
    const std::array<CfaColor, 36> xtrans = {
        CfaColor::Green, CfaColor::Green, CfaColor::Red,   CfaColor::Green, CfaColor::Green, CfaColor::Blue,
        CfaColor::Green, CfaColor::Green, CfaColor::Blue,  CfaColor::Green, CfaColor::Green, CfaColor::Red,
        CfaColor::Blue,  CfaColor::Red,   CfaColor::Green, CfaColor::Red,   CfaColor::Blue,  CfaColor::Green,
        CfaColor::Green, CfaColor::Green, CfaColor::Blue,  CfaColor::Green, CfaColor::Green, CfaColor::Red,
        CfaColor::Green, CfaColor::Green, CfaColor::Red,   CfaColor::Green, CfaColor::Green, CfaColor::Blue,
        CfaColor::Red,   CfaColor::Blue,  CfaColor::Green, CfaColor::Blue,  CfaColor::Red,   CfaColor::Green};
    auto raw_origin = CfaPattern::create(6, 6, xtrans);
    ASSERT_TRUE(raw_origin);
    const CfaPattern visible_origin = raw_origin->shifted(1, 2);
    const auto decoded = ts::make_mosaic(12, 12, PixelRect{1, 2, 10, 9}, visible_origin,
                                         ts::simple_levels(0.0, 4095.0), varied, ts::kRgbChannels);
    auto image = normalize_sensor(decoded);
    ASSERT_TRUE(image) << image.error().describe();
    EXPECT_EQ(image->cfa, visible_origin);
    EXPECT_TRUE(image->cfa.is_xtrans());
    EXPECT_EQ(image->cfa.color_at(0, 0), raw_origin->color_at(2, 1));
    EXPECT_EQ(image->channel_of_site(0, 0), 0);  // raw (1, 2) is red
    EXPECT_EQ(image->channel_of_site(1, 0), 1);  // raw (2, 2) is green
    expect_normalized(decoded, *image, image->normalization.black_max);
}

TEST(SaturationMask, CreationLimitsAndClone) {
    ts::expect_error(SaturationMask::create(0, 3), ErrorCode::InvalidArgument, "engine.sensor.mask");
    image::AllocationLimits limits;
    limits.max_bytes = 11;
    auto too_big = SaturationMask::create(4, 3, limits);
    ASSERT_FALSE(too_big);
    EXPECT_EQ(too_big.error().code, ErrorCode::LimitExceeded);
    limits.max_bytes = 12;
    limits.max_dimension = 3;
    auto too_wide = SaturationMask::create(4, 3, limits);
    ASSERT_FALSE(too_wide);
    EXPECT_EQ(too_wide.error().code, ErrorCode::LimitExceeded);

    auto mask = SaturationMask::create(4, 3);
    ASSERT_TRUE(mask);
    EXPECT_EQ(mask->count_flagged(), 0u);
    mask->row(2)[3] = 0b101;
    auto copy = mask->clone();
    ASSERT_TRUE(copy);
    EXPECT_EQ(copy->at(3, 2), 0b101u);
    EXPECT_EQ(copy->count_flagged(), 1u);
    EXPECT_TRUE(SaturationMask{}.empty());
}

}  // namespace
}  // namespace lucent::engine
