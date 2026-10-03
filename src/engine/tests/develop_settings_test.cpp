// Unit tests for DevelopSettings v1 (develop_settings.hpp): defaults, ranges and their
// boundaries, non-finite and non-monotone input, versions, base look, locks and equality.

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/engine/develop_settings.hpp>

#include "sensor_test_support.hpp"

namespace lucent::engine {
namespace {

namespace ts = test_support;
constexpr const char* kWhere = "engine.develop.settings";
const double kNaN = std::numeric_limits<double>::quiet_NaN();
const double kInf = std::numeric_limits<double>::infinity();

TEST(DevelopSettings, DefaultsAreValidAndAsShot) {
    const DevelopSettings s;
    EXPECT_TRUE(s.validate());
    EXPECT_TRUE(s.is_as_shot());
    EXPECT_EQ(s.version, kDevelopSettingsVersion);
    EXPECT_EQ(kDevelopSettingsSchema, "lucent.develop");
    EXPECT_EQ(mode_of(s.white_balance), WhiteBalanceMode::AsShot);
    EXPECT_TRUE(s.tone_curve.is_identity());
    EXPECT_EQ(s.base_look.id, "neutral");
    EXPECT_EQ(s.base_look.version, 1u);
    EXPECT_TRUE(s.locks.empty());
    EXPECT_EQ(s.operations, DevelopOperationVersions{});
}

TEST(DevelopSettings, AcceptsRangeBoundariesAndRejectsValuesBeyondThem) {
    using Field = double DevelopSettings::*;
    const std::vector<std::pair<Field, double>> fields = {
        {&DevelopSettings::exposure_ev, kMaxExposureEv}, {&DevelopSettings::contrast, 1.0},
        {&DevelopSettings::highlights, 1.0},             {&DevelopSettings::shadows, 1.0},
        {&DevelopSettings::saturation, 1.0},             {&DevelopSettings::vibrance, 1.0},
    };
    for (const auto& [field, limit] : fields) {
        for (const double ok_value : {-limit, limit, 0.5 * limit}) {
            DevelopSettings s;
            s.*field = ok_value;
            EXPECT_TRUE(s.validate()) << ok_value;
            EXPECT_FALSE(s.is_as_shot());
        }
        for (const double bad : {-limit - 1e-9, limit + 1e-9, kNaN, kInf, -kInf}) {
            DevelopSettings s;
            s.*field = bad;
            ts::expect_error(s.validate(), ErrorCode::InvalidArgument, kWhere);
        }
    }
}

TEST(DevelopSettings, ValidatesKelvinWhiteBalance) {
    DevelopSettings s;
    s.white_balance = KelvinWhiteBalance{kMinTemperatureK, -kMaxTint};
    EXPECT_TRUE(s.validate());
    s.white_balance = KelvinWhiteBalance{kMaxTemperatureK, kMaxTint};
    EXPECT_TRUE(s.validate());
    EXPECT_EQ(mode_of(s.white_balance), WhiteBalanceMode::Kelvin);
    EXPECT_FALSE(s.is_as_shot());
    for (const KelvinWhiteBalance bad : {KelvinWhiteBalance{1999.0, 0.0}, KelvinWhiteBalance{50001.0, 0.0},
                                         KelvinWhiteBalance{5000.0, 150.5}, KelvinWhiteBalance{5000.0, -151.0},
                                         KelvinWhiteBalance{kNaN, 0.0}, KelvinWhiteBalance{5000.0, kInf}}) {
        s.white_balance = bad;
        ts::expect_error(s.validate(), ErrorCode::InvalidArgument, kWhere);
    }
}

TEST(DevelopSettings, ValidatesCameraNeutralMultipliers) {
    DevelopSettings s;
    s.white_balance = CameraNeutralWhiteBalance{2.0, 1.0, 1.5};
    EXPECT_TRUE(s.validate());
    EXPECT_EQ(mode_of(s.white_balance), WhiteBalanceMode::CameraNeutral);
    s.white_balance = CameraNeutralWhiteBalance{128.0, 2.0, 2.0 / 64.0};  // ratios exactly 64 and 1/64
    EXPECT_TRUE(s.validate());
    for (const CameraNeutralWhiteBalance bad :
         {CameraNeutralWhiteBalance{0.0, 1.0, 1.0}, CameraNeutralWhiteBalance{1.0, -1.0, 1.0},
          CameraNeutralWhiteBalance{1.0, 1.0, kNaN}, CameraNeutralWhiteBalance{65.0, 1.0, 1.0},
          CameraNeutralWhiteBalance{1.0, 1.0, 0.01}}) {
        s.white_balance = bad;
        ts::expect_error(s.validate(), ErrorCode::InvalidArgument, kWhere);
    }
}

TEST(DevelopSettings, ValidatesTheToneCurve) {
    DevelopSettings s;
    s.tone_curve.points = {{0.0, 0.1}, {0.25, 0.2}, {0.5, 0.5}, {0.75, 0.9}, {1.0, 0.95}};
    EXPECT_TRUE(s.validate());
    EXPECT_FALSE(s.tone_curve.is_identity());
    s.tone_curve.points = {{0.2, 0.0}, {0.8, 1.0}};  // endpoints moved inwards: valid, not identity
    EXPECT_TRUE(s.validate());
    EXPECT_FALSE(s.tone_curve.is_identity());
    s.tone_curve.points = {{0.0, 0.0}, {0.3, 0.3}, {0.7, 0.7}, {1.0, 1.0}};  // collinear on the diagonal
    EXPECT_TRUE(s.validate());
    EXPECT_TRUE(s.tone_curve.is_identity());
    EXPECT_TRUE(s.is_as_shot());
    s.tone_curve.points = {{0.0, 0.0}, {0.5, 0.5}, {0.5, 0.6}, {1.0, 1.0}};  // flat y allowed, x repeated not
    ts::expect_error(s.validate(), ErrorCode::InvalidArgument, kWhere);

    const std::vector<std::vector<ToneCurvePoint>> bad = {
        {{0.0, 0.0}},                                // too few
        {{0.0, 0.0}, {0.6, 0.5}, {0.4, 0.6}},        // x decreasing
        {{0.0, 0.0}, {0.5, 0.6}, {1.0, 0.5}},        // y decreasing
        {{-0.1, 0.0}, {1.0, 1.0}},                   // x below 0
        {{0.0, 0.0}, {1.0, 1.1}},                    // y above 1
        {{0.0, kNaN}, {1.0, 1.0}},                   // not finite
    };
    for (const auto& points : bad) {
        s.tone_curve.points = points;
        ts::expect_error(s.validate(), ErrorCode::InvalidArgument, kWhere);
    }
    std::vector<ToneCurvePoint> many;
    for (std::size_t i = 0; i <= kMaxToneCurvePoints; ++i) {
        const double v = static_cast<double>(i) / static_cast<double>(kMaxToneCurvePoints);
        many.push_back({v, v});
    }
    s.tone_curve.points = many;
    ts::expect_error(s.validate(), ErrorCode::InvalidArgument, kWhere);
    many.pop_back();
    many.back() = {1.0, 1.0};
    s.tone_curve.points = many;
    EXPECT_TRUE(s.validate());

    // A flat curve (all y equal) is monotone and valid.
    s.tone_curve.points = {{0.0, 0.4}, {1.0, 0.4}};
    EXPECT_TRUE(s.validate());
}

TEST(DevelopSettings, RejectsOtherVersionsAndUnknownLooks) {
    DevelopSettings s;
    s.version = 2;
    ts::expect_error(s.validate(), ErrorCode::VersionMismatch, kWhere);
    s.version = 0;
    ts::expect_error(s.validate(), ErrorCode::VersionMismatch, kWhere);

    for (std::uint32_t DevelopOperationVersions::*field :
         {&DevelopOperationVersions::white_balance, &DevelopOperationVersions::exposure,
          &DevelopOperationVersions::local_tone, &DevelopOperationVersions::contrast,
          &DevelopOperationVersions::tone_curve, &DevelopOperationVersions::color}) {
        DevelopSettings t;
        t.operations.*field = 2;
        ts::expect_error(t.validate(), ErrorCode::VersionMismatch, kWhere);
        EXPECT_TRUE(t.is_as_shot());  // recorded versions do not change "as shot"
    }

    DevelopSettings look;
    look.base_look.id = "vivid";
    ts::expect_error(look.validate(), ErrorCode::Unsupported, kWhere);
    EXPECT_FALSE(look.is_as_shot());
    look.base_look = BaseLookSetting{};
    look.base_look.version = 2;
    ts::expect_error(look.validate(), ErrorCode::VersionMismatch, kWhere);
}

TEST(DevelopSettings, LocksAreStoredOrderedAndValidated) {
    DevelopSettings s;
    s.locks = {DevelopControl::Vibrance, DevelopControl::WhiteBalance, DevelopControl::Exposure};
    EXPECT_TRUE(s.validate());
    EXPECT_TRUE(s.is_as_shot());  // locks do not affect pixels
    std::vector<DevelopControl> order(s.locks.begin(), s.locks.end());
    EXPECT_EQ(order, (std::vector<DevelopControl>{DevelopControl::WhiteBalance, DevelopControl::Exposure,
                                                  DevelopControl::Vibrance}));
    s.locks.insert(static_cast<DevelopControl>(42));
    ts::expect_error(s.validate(), ErrorCode::InvalidArgument, kWhere);

    for (std::uint8_t v = 1; v <= 9; ++v) {
        const auto control = static_cast<DevelopControl>(v);
        const auto parsed = parse_develop_control(to_string(control));
        ASSERT_TRUE(parsed.has_value()) << static_cast<int>(v);
        EXPECT_EQ(*parsed, control);
    }
    EXPECT_EQ(to_string(DevelopControl::ToneCurve), "tone_curve");
    EXPECT_FALSE(parse_develop_control("Exposure").has_value());
    EXPECT_FALSE(parse_develop_control("").has_value());
    EXPECT_EQ(to_string(static_cast<DevelopControl>(0)), "invalid");
}

TEST(DevelopSettings, EqualityComparesEveryField) {
    const DevelopSettings base;
    EXPECT_EQ(base, DevelopSettings{});
    std::vector<DevelopSettings> variants(12);
    variants[0].white_balance = KelvinWhiteBalance{};
    variants[1].white_balance = CameraNeutralWhiteBalance{};
    variants[2].exposure_ev = 0.1;
    variants[3].contrast = -0.1;
    variants[4].highlights = -0.5;
    variants[5].shadows = 0.5;
    variants[6].tone_curve.points = {{0.0, 0.0}, {0.5, 0.5}, {1.0, 1.0}};
    variants[7].saturation = 0.2;
    variants[8].vibrance = 0.2;
    variants[9].base_look.version = 7;
    variants[10].locks.insert(DevelopControl::Shadows);
    variants[11].operations.color = 9;
    for (std::size_t i = 0; i < variants.size(); ++i) {
        EXPECT_NE(variants[i], base) << i;
    }
    DevelopSettings k1, k2;
    k1.white_balance = KelvinWhiteBalance{5600.0, 3.0};
    k2.white_balance = KelvinWhiteBalance{5600.0, 3.0};
    EXPECT_EQ(k1, k2);
    k2.white_balance = KelvinWhiteBalance{5600.0, 3.5};
    EXPECT_NE(k1, k2);
    EXPECT_EQ(to_string(WhiteBalanceMode::CameraNeutral), "camera_neutral");
}

}  // namespace
}  // namespace lucent::engine
