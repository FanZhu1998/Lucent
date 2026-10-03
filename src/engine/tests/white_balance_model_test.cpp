// Unit tests for temperature/tint and the camera white-balance model (white_balance_model.hpp):
// the DNG SDK conversions against Robertson's table and Adobe's published D65/D50 values, the
// decoder-matrix and DNG-profile mappings against independent computations, round trips,
// unavailable calibrations and decoder-balanced data.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <variant>

#include <gtest/gtest.h>

#include <lucent/engine/camera_color.hpp>
#include <lucent/engine/white_balance_model.hpp>
#include <lucent/image/color_math.hpp>

#include "develop_test_support.hpp"

namespace lucent::engine {
namespace {

namespace ts = test_support;
using image::Chromaticity;
using image::Vec3d;
constexpr const char* kWhere = "engine.develop.white_balance";

raw::RawMetadata decoder_metadata(const std::array<double, 3>& as_shot = {2.0, 1.0, 1.5}) {
    raw::RawMetadata m;
    m.representation.kind = raw::RawRepresentation::Mosaic;
    m.representation.channel_colors = ts::kBayerChannels;
    ts::set_decoder_matrix(m, ts::kR5CamXyz);
    ts::set_as_shot(m, as_shot);
    return m;
}

raw::RawMetadata dng_metadata(const std::array<double, 3>& as_shot = {2.1, 1.0, 1.6}) {
    raw::RawMetadata m = decoder_metadata(as_shot);
    ts::make_dual_illuminant_dng(m);
    return m;
}

void expect_rgb_near(const RgbMultipliers& a, const RgbMultipliers& b, double relative) {
    for (std::size_t c = 0; c < 3; ++c) {
        EXPECT_NEAR(a[c], b[c], relative * std::abs(b[c])) << "channel " << c;
    }
}

// Robertson line i as a chromaticity.
Chromaticity line_xy(double u, double v) {
    const double d = 2.0 * u - 8.0 * v + 4.0;
    return {3.0 * u / d, 2.0 * v / d};
}

// ---- Temperature and tint ------------------------------------------------------------------------

TEST(TemperatureTint, PointsOnTheIsotemperatureTableHaveZeroTint) {
    // Lines 1..29 of Wyszecki & Stiles table 1(3.11) (u, v of the Planckian locus).
    const std::array<std::array<double, 3>, 6> lines = {{{100.0, 0.19032, 0.29326},
                                                         {200.0, 0.21142, 0.32312},
                                                         {300.0, 0.24010, 0.34308},
                                                         {350.0, 0.25591, 0.34951},
                                                         {450.0, 0.28863, 0.35714},
                                                         {550.0, 0.32129, 0.36011}}};
    for (const auto& [mired, u, v] : lines) {
        auto tt = temperature_tint_from_xy(line_xy(u, v));
        ASSERT_TRUE(tt) << tt.error().describe();
        EXPECT_NEAR(tt->temperature_k, 1.0e6 / mired, 1e-6 * 1.0e6 / mired) << mired;
        EXPECT_NEAR(tt->tint, 0.0, 1e-6) << mired;
        auto xy = xy_from_temperature_tint({1.0e6 / mired, 0.0});
        ASSERT_TRUE(xy);
        EXPECT_NEAR(xy->x, line_xy(u, v).x, 1e-12);
        EXPECT_NEAR(xy->y, line_xy(u, v).y, 1e-12);
    }
}

TEST(TemperatureTint, MatchesAdobesValuesForD65AndD50) {
    // Adobe's raw editors show D65 as about 6500 K / +10 and D50 as 5000 K / +10.
    auto d65 = temperature_tint_from_xy(image::kD65White);
    auto d50 = temperature_tint_from_xy(image::kD50White);
    ASSERT_TRUE(d65 && d50);
    EXPECT_NEAR(d65->temperature_k, 6504.0, 1.0);
    EXPECT_NEAR(d65->tint, 9.77, 0.02);
    EXPECT_NEAR(d50->temperature_k, 5001.0, 1.0);
    EXPECT_NEAR(d50->tint, 9.56, 0.02);
    // The temperature agrees with Robertson's CCT search of camera_color.hpp.
    EXPECT_NEAR(d65->temperature_k, *correlated_color_temperature(image::kD65White), 1e-9);
}

TEST(TemperatureTint, PositiveTintIsAboveTheLocus) {
    auto greener = temperature_tint_from_xy({0.3127, 0.3390});
    auto redder = temperature_tint_from_xy({0.3127, 0.3190});
    ASSERT_TRUE(greener && redder);
    EXPECT_GT(greener->tint, 9.77);
    EXPECT_LT(redder->tint, 0.0);
}

TEST(TemperatureTint, ConversionsAreInverseWithinTheStatedTolerance) {
    for (const double kelvin : {2000.0, 2850.0, 3200.0, 4300.0, 5000.0, 5600.0, 6500.0, 9000.0, 20000.0, 50000.0}) {
        for (const double tint : {-150.0, -40.0, 0.0, 12.5, 150.0}) {
            auto xy = xy_from_temperature_tint({kelvin, tint});
            ASSERT_TRUE(xy) << kelvin << " " << tint;
            auto back = temperature_tint_from_xy(*xy);
            ASSERT_TRUE(back);
            // Measured over a 400 x 61 grid of this range: 1.1e-5 and 2.7e-5 at most.
            EXPECT_NEAR(back->temperature_k, kelvin, 1.5e-5 * kelvin) << kelvin << " " << tint;
            EXPECT_NEAR(back->tint, tint, 4e-5) << kelvin << " " << tint;
        }
    }
}

TEST(TemperatureTint, RejectsInvalidInput) {
    ts::expect_error(temperature_tint_from_xy({std::nan(""), 0.3}), ErrorCode::InvalidArgument, kWhere);
    ts::expect_error(temperature_tint_from_xy({2.0, 0.0}), ErrorCode::InvalidArgument, kWhere);
    ts::expect_error(xy_from_temperature_tint({0.0, 0.0}), ErrorCode::InvalidArgument, kWhere);
    ts::expect_error(xy_from_temperature_tint({5000.0, std::nan("")}), ErrorCode::InvalidArgument, kWhere);
    ts::expect_error(xy_from_temperature_tint({5000.0, 1.0e6}), ErrorCode::Corrupt, kWhere);
}

// ---- Decoder-matrix model ---------------------------------------------------------------------------

TEST(WhiteBalanceModel, DecoderMatrixMapsWhitesLikeDcraw) {
    auto model = WhiteBalanceModel::create(decoder_metadata());
    ASSERT_TRUE(model) << model.error().describe();
    EXPECT_TRUE(model->kelvin_available()) << model->kelvin_unavailable_reason();
    EXPECT_EQ(model->path(), CalibrationPath::DecoderMatrix);
    EXPECT_FALSE(model->basis().empty());
    expect_rgb_near(model->as_shot(), {2.0, 1.0, 1.5}, 1e-7);
    EXPECT_EQ(model->as_shot_source(), WhiteBalanceSource::AsShot);
    expect_rgb_near(model->prior_balance(), {1.0, 1.0, 1.0}, 0.0);

    // dcraw's pre_mul: 1 / (cam_xyz * white), green-normalised (cam_xyz as the decoder stores it,
    // in float).
    image::Matrix3d stored;
    const raw::RawMetadata metadata = decoder_metadata();
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            stored(i, j) = metadata.color.xyz_to_camera[i][j];
        }
    }
    for (const Chromaticity white : {image::kD65White, ts::kIlluminantA, Chromaticity{0.33, 0.35}}) {
        const Vec3d camera = stored * *image::xy_to_XYZ(white);
        auto m = model->multipliers_for_white(white);
        ASSERT_TRUE(m) << m.error().describe();
        expect_rgb_near(*m, {camera[1] / camera[0], 1.0, camera[1] / camera[2]}, 1e-12);
        auto back = model->white_for(*m);
        ASSERT_TRUE(back);
        EXPECT_NEAR(back->x, white.x, 1e-12);
        EXPECT_NEAR(back->y, white.y, 1e-12);
    }
    // Warmer light needs more blue and less red gain.
    auto warm = model->multipliers_for({3000.0, 0.0});
    auto cool = model->multipliers_for({7000.0, 0.0});
    ASSERT_TRUE(warm && cool);
    EXPECT_GT((*warm)[2], (*cool)[2]);
    EXPECT_LT((*warm)[0], (*cool)[0]);
}

TEST(WhiteBalanceModel, PinsNonPositiveNeutralEntriesAtTheRangeCorners) {
    // 2000 K with tint +150 lies beyond the spectral locus; the R5 matrix maps it to a negative
    // blue response, pinned to 0.001 of the largest entry (blue gain 1000 / green response).
    auto model = WhiteBalanceModel::create(decoder_metadata());
    ASSERT_TRUE(model);
    auto xy = xy_from_temperature_tint({2000.0, 150.0});
    ASSERT_TRUE(xy);
    EXPECT_GT(xy->x + xy->y, 1.0);
    const Vec3d camera = ts::kR5CamXyz * *image::xy_to_XYZ(*xy);
    ASSERT_LT(camera[2], 0.0);
    auto m = model->multipliers_for({2000.0, 150.0});
    ASSERT_TRUE(m) << m.error().describe();
    const double max_entry = std::max({camera[0], camera[1], camera[2]});
    EXPECT_NEAR((*m)[2], (camera[1] / max_entry) / 0.001, 1e-3 * (*m)[2]);
    for (const double v : *m) {
        EXPECT_TRUE(std::isfinite(v));
    }
}

TEST(WhiteBalanceModel, KelvinRoundTripsTheAsShotBalance) {
    for (const auto& as_shot :
         {std::array<double, 3>{2.0, 1.0, 1.5}, {1.4, 1.0, 2.4}, {2.6, 1.0, 1.25}}) {
        for (const bool dng : {false, true}) {
            SCOPED_TRACE(testing::Message() << (dng ? "dng " : "decoder ") << as_shot[0] << "/" << as_shot[2]);
            auto model = WhiteBalanceModel::create(dng ? dng_metadata(as_shot) : decoder_metadata(as_shot));
            ASSERT_TRUE(model);
            ASSERT_TRUE(model->kelvin_available()) << model->kelvin_unavailable_reason();
            auto tt = model->temperature_for(model->as_shot());
            ASSERT_TRUE(tt) << tt.error().describe();
            auto m = model->multipliers_for(*tt);
            ASSERT_TRUE(m) << m.error().describe();
            // The temperature/tint pair is inverse to 6e-6 in temperature and 3e-5 in tint; the DNG
            // white search converges to |dx| + |dy| < 1e-7.
            expect_rgb_near(*m, model->as_shot(), 2e-5);
        }
    }
}

TEST(WhiteBalanceModel, TargetMultipliersFollowTheSetting) {
    auto model = WhiteBalanceModel::create(decoder_metadata());
    ASSERT_TRUE(model);
    expect_rgb_near(*model->target_multipliers(AsShotWhiteBalance{}), model->as_shot(), 0.0);
    expect_rgb_near(*model->target_multipliers(CameraNeutralWhiteBalance{4.0, 2.0, 3.0}), {2.0, 1.0, 1.5}, 1e-15);
    auto kelvin = model->target_multipliers(KelvinWhiteBalance{4000.0, 5.0});
    ASSERT_TRUE(kelvin);
    expect_rgb_near(*kelvin, *model->multipliers_for({4000.0, 5.0}), 0.0);
    ts::expect_error(model->target_multipliers(KelvinWhiteBalance{1000.0, 0.0}), ErrorCode::InvalidArgument, kWhere);
    ts::expect_error(model->target_multipliers(CameraNeutralWhiteBalance{1.0, 0.0, 1.0}), ErrorCode::InvalidArgument,
                     kWhere);
    // Bayer RGBG: Green2 takes green.
    const auto channels = model->channel_multipliers({2.0, 1.0, 1.5});
    EXPECT_EQ(channels, (std::array<double, 4>{2.0, 1.0, 1.5, 1.0}));
    expect_rgb_near(model->sample_multipliers({2.0, 1.0, 1.5}), {2.0, 1.0, 1.5}, 0.0);
}

// ---- DNG-profile model -------------------------------------------------------------------------------

TEST(WhiteBalanceModel, DngProfileUsesTheInterpolatedCalibration) {
    const raw::RawMetadata metadata = dng_metadata();
    auto model = WhiteBalanceModel::create(metadata);
    ASSERT_TRUE(model) << model.error().describe();
    EXPECT_TRUE(model->kelvin_available()) << model->kelvin_unavailable_reason();
    EXPECT_EQ(model->path(), CalibrationPath::DngColorMatrix);
    auto profile = dng_profile_from_metadata(metadata);
    ASSERT_TRUE(profile);
    for (const Chromaticity white : {ts::kIlluminantA, image::kD65White, Chromaticity{0.40, 0.39}}) {
        auto m = model->multipliers_for_white(white);
        ASSERT_TRUE(m) << m.error().describe();
        // The SDK's white search finds the same white again from the neutral 1 / m.
        auto record = evaluate_dng_profile(*profile, Vec3d{1.0 / (*m)[0], 1.0 / (*m)[1], 1.0 / (*m)[2]});
        ASSERT_TRUE(record);
        EXPECT_NEAR(record->white_xy.x, white.x, 2e-7);
        EXPECT_NEAR(record->white_xy.y, white.y, 2e-7);
    }
    // Below the A calibration's 2850 K only that calibration applies (weight 1):
    // neutral = CM_A * XYZ(white).
    const auto warm = xy_from_temperature_tint({2500.0, 0.0});
    ASSERT_TRUE(warm);
    const Vec3d camera = ts::kCmA * *image::xy_to_XYZ(*warm);
    auto at_a = model->multipliers_for_white(*warm);
    ASSERT_TRUE(at_a);
    expect_rgb_near(*at_a, {camera[1] / camera[0], 1.0, camera[1] / camera[2]}, 1e-12);
}

TEST(WhiteBalanceModel, DngDataBalancedByTheDecoderUsesAsShotNeutralAsPrior) {
    raw::RawMetadata m = dng_metadata();
    m.prior_processing.push_back({raw::PriorProcessingKind::WhiteBalanceApplied, "test"});
    m.dng->info.color.as_shot_neutral = {1.0 / 2.1, 1.0, 1.0 / 1.6};
    auto model = WhiteBalanceModel::create(m);
    ASSERT_TRUE(model) << model.error().describe();
    EXPECT_EQ(model->as_shot_source(), WhiteBalanceSource::AlreadyApplied);
    expect_rgb_near(model->prior_balance(), {2.1, 1.0, 1.6}, 1e-12);
    expect_rgb_near(model->as_shot(), {2.1, 1.0, 1.6}, 1e-12);
    expect_rgb_near(model->sample_multipliers({4.2, 1.0, 1.6}), {2.0, 1.0, 1.0}, 1e-12);
    EXPECT_TRUE(model->kelvin_available()) << model->kelvin_unavailable_reason();

    m.dng->info.color.as_shot_neutral.clear();
    auto unknown = WhiteBalanceModel::create(m);
    ASSERT_TRUE(unknown);
    EXPECT_FALSE(unknown->kelvin_available());
    ts::expect_error(unknown->multipliers_for({5000.0, 0.0}), ErrorCode::Unsupported, kWhere);
}

// ---- Unavailable ---------------------------------------------------------------------------------------

TEST(WhiteBalanceModel, ReportsKelvinUnavailableWithoutCalibration) {
    raw::RawMetadata none = decoder_metadata();
    none.color.xyz_to_camera = {};
    auto model = WhiteBalanceModel::create(none);
    ASSERT_TRUE(model);
    EXPECT_FALSE(model->kelvin_available());
    EXPECT_NE(model->kelvin_unavailable_reason().find("no camera calibration"), std::string::npos);
    ts::expect_error(model->multipliers_for({5000.0, 0.0}), ErrorCode::Unsupported, kWhere);
    ts::expect_error(model->temperature_for({2.0, 1.0, 1.5}), ErrorCode::Unsupported, kWhere);
    ts::expect_error(model->target_multipliers(KelvinWhiteBalance{}), ErrorCode::Unsupported, kWhere);
    // Camera-neutral multipliers need no calibration.
    EXPECT_TRUE(model->target_multipliers(CameraNeutralWhiteBalance{2.0, 1.0, 1.5}));
}

TEST(WhiteBalanceModel, MonochromeHasNoWhiteBalance) {
    raw::RawMetadata m;
    m.representation.kind = raw::RawRepresentation::Monochrome;
    m.representation.channel_colors = {image::CfaColor::White};
    auto model = WhiteBalanceModel::create(m);
    ASSERT_TRUE(model) << model.error().describe();
    EXPECT_TRUE(model->monochrome());
    EXPECT_FALSE(model->kelvin_available());
    EXPECT_EQ(model->path(), CalibrationPath::MonochromeNeutral);
    ts::expect_error(model->target_multipliers(KelvinWhiteBalance{}), ErrorCode::Unsupported, kWhere);
    ts::expect_error(model->target_multipliers(CameraNeutralWhiteBalance{}), ErrorCode::Unsupported, kWhere);
    EXPECT_EQ(model->channel_multipliers({1.0, 1.0, 1.0}), (std::array<double, 4>{1.0, 0.0, 0.0, 0.0}));
}

TEST(WhiteBalanceModel, RejectsRepresentationsWithoutRgb) {
    raw::RawMetadata m;
    m.representation.kind = raw::RawRepresentation::Mosaic;
    m.representation.channel_colors = {image::CfaColor::Cyan, image::CfaColor::Magenta, image::CfaColor::Yellow,
                                       image::CfaColor::Green};
    EXPECT_FALSE(WhiteBalanceModel::create(m));
}

}  // namespace
}  // namespace lucent::engine
