// Unit tests for DevelopSettings (develop_settings.hpp): defaults, ranges and their boundaries,
// non-finite and non-monotone input, versions, base look, locks and equality; version 2's chroma
// denoising, local adjustments and geometry (bounds, the version-1 conversion, revisions and the
// orientation helpers).

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/engine/develop_settings.hpp>
#include <lucent/image/geometry.hpp>

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
        {{0.0, 0.0}, {1e-310, 0.75}, {1.0, 1.0}},    // x closer than kMinToneCurveGap
        {{0.0, 0.0}, {0.5, 0.5}, {0.5 + 0.5 * kMinToneCurveGap, 0.6}, {1.0, 1.0}},
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
    // Points exactly kMinToneCurveGap apart are valid.
    s.tone_curve.points = {{0.0, 0.0}, {kMinToneCurveGap, 0.5}, {2.0 * kMinToneCurveGap, 1.0}};
    EXPECT_TRUE(s.validate());
}

TEST(DevelopSettings, RejectsOtherVersionsAndUnknownLooks) {
    DevelopSettings s;
    s.version = kDevelopSettingsVersion + 1;
    ts::expect_error(s.validate(), ErrorCode::VersionMismatch, kWhere);
    s.version = 0;
    ts::expect_error(s.validate(), ErrorCode::VersionMismatch, kWhere);
    s.version = kOldestDevelopSettingsVersion;  // version 1 is still read
    EXPECT_TRUE(s.validate());

    for (std::uint32_t DevelopOperationVersions::* field :
         {&DevelopOperationVersions::white_balance, &DevelopOperationVersions::exposure,
          &DevelopOperationVersions::local_tone, &DevelopOperationVersions::contrast,
          &DevelopOperationVersions::tone_curve, &DevelopOperationVersions::color,
          &DevelopOperationVersions::chroma_denoise, &DevelopOperationVersions::local_adjustments}) {
        DevelopSettings t;
        t.operations.*field = 2;
        ts::expect_error(t.validate(), ErrorCode::VersionMismatch, kWhere);
        EXPECT_TRUE(t.is_as_shot());  // recorded versions do not change "as shot"
    }
    // The geometry: versions 1 (the whole visible area) and 2 (the camera's image area) are read.
    for (const std::uint32_t version : {0u, 3u}) {
        DevelopSettings t;
        t.operations.geometry = version;
        ts::expect_error(t.validate(), ErrorCode::VersionMismatch, kWhere);
    }
    for (const std::uint32_t version : {kOldestGeometryOperationVersion, kGeometryOperationVersion}) {
        DevelopSettings t;
        t.operations.geometry = version;
        EXPECT_TRUE(t.validate()) << version;
        EXPECT_TRUE(t.is_as_shot()) << version;
        EXPECT_EQ(geometry_operation_version(t), version);
    }
    EXPECT_EQ(kGeometryOperationVersion, 2u);
    EXPECT_EQ(DevelopSettings{}.operations.geometry, 2u);  // new settings use the camera's image area

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
    std::vector<DevelopSettings> variants(16);
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
    variants[12].chroma_denoise.radius = 4.0;
    variants[13].local_adjustments.emplace_back();
    variants[14].geometry.crop.left = 0.1;
    variants[15].version = kOldestDevelopSettingsVersion;
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

// ---- Version 2 ------------------------------------------------------------------------------------

LocalAdjustment radial_adjustment(std::uint32_t id) {
    LocalAdjustment a;
    a.id = id;
    a.shape = RadialGradientMask{};
    a.controls.exposure_ev = 0.5;
    return a;
}

BrushStroke stroke_with(std::size_t points) {
    BrushStroke s;
    for (std::size_t i = 0; i < points; ++i) {
        s.points.push_back({0.1 + 0.8 * static_cast<double>(i) / static_cast<double>(points), 0.5});
    }
    return s;
}

TEST(DevelopSettingsV2, DefaultsHoldNoDenoiseNoMasksAndNoGeometry) {
    const DevelopSettings s;
    EXPECT_EQ(kDevelopSettingsVersion, 2u);
    EXPECT_EQ(kOldestDevelopSettingsVersion, 1u);
    EXPECT_TRUE(s.chroma_denoise.is_identity());
    EXPECT_EQ(s.chroma_denoise.radius, kDefaultChromaDenoiseRadius);
    EXPECT_TRUE(s.local_adjustments.empty());
    EXPECT_TRUE(s.geometry.is_identity());
    EXPECT_TRUE(s.geometry.crop.is_full());
    EXPECT_EQ(s.operations.chroma_denoise, kChromaDenoiseOperationVersion);
    EXPECT_EQ(s.operations.local_adjustments, kLocalAdjustmentsOperationVersion);
    EXPECT_EQ(s.operations.geometry, kGeometryOperationVersion);
}

TEST(DevelopSettingsV2, Version1ValuesAreValidAndUpgradeWithoutTheNewMembers) {
    DevelopSettings v1;
    v1.version = 1;
    v1.exposure_ev = 0.7;
    v1.white_balance = KelvinWhiteBalance{4300.0, 6.0};
    v1.tone_curve.points = {{0.0, 0.0}, {0.5, 0.6}, {1.0, 1.0}};
    v1.locks = {DevelopControl::Exposure};
    EXPECT_TRUE(v1.validate());
    auto upgraded = upgrade_develop_settings(v1);
    ASSERT_TRUE(upgraded) << upgraded.error().describe();
    EXPECT_EQ(upgraded->version, kDevelopSettingsVersion);
    EXPECT_TRUE(upgraded->validate());
    DevelopSettings expected = v1;
    expected.version = kDevelopSettingsVersion;
    // Version-1 settings rendered the whole visible area: they keep geometry version 1 (ADR 0017).
    expected.operations.geometry = kOldestGeometryOperationVersion;
    EXPECT_EQ(*upgraded, expected);  // every version-1 field kept, the new ones at their defaults
    EXPECT_TRUE(upgraded->chroma_denoise.is_identity());
    EXPECT_TRUE(upgraded->local_adjustments.empty());
    EXPECT_TRUE(upgraded->geometry.is_identity());
    EXPECT_EQ(geometry_operation_version(v1), 1u);  // whatever its unused operations.geometry holds
    EXPECT_EQ(geometry_operation_version(*upgraded), 1u);

    // A default version-1 value becomes the default version-2 value with geometry version 1.
    DevelopSettings plain;
    plain.version = 1;
    auto plain_upgraded = upgrade_develop_settings(plain);
    ASSERT_TRUE(plain_upgraded);
    DevelopSettings plain_expected;
    plain_expected.operations.geometry = 1;
    EXPECT_EQ(*plain_upgraded, plain_expected);
    // The upgraded value is itself a valid version-1 value again when written back as version 1.
    DevelopSettings again = *plain_upgraded;
    again.version = 1;
    EXPECT_TRUE(again.validate());

    // Current values pass unchanged.
    DevelopSettings current;
    current.geometry.quarter_turns = 1;
    auto same = upgrade_develop_settings(current);
    ASSERT_TRUE(same);
    EXPECT_EQ(*same, current);

    // A version-1 value cannot hold version 2's members.
    std::vector<DevelopSettings> bad(7, v1);
    bad[0].chroma_denoise.strength = 0.5;
    bad[1].chroma_denoise.radius = 3.0;
    bad[2].local_adjustments.push_back(radial_adjustment(1));
    bad[3].geometry.straighten_degrees = 1.0;
    bad[4].operations.chroma_denoise = 7;
    bad[5].operations.local_adjustments = 7;
    bad[6].operations.geometry = 7;
    for (const DevelopSettings& b : bad) {
        ts::expect_error(b.validate(), ErrorCode::InvalidArgument, kWhere);
        ts::expect_error(upgrade_develop_settings(b), ErrorCode::InvalidArgument, kWhere);
    }
    // Versions this build does not read.
    for (const std::uint32_t version : {0u, 3u, 99u}) {
        DevelopSettings other;
        other.version = version;
        ts::expect_error(upgrade_develop_settings(other), ErrorCode::VersionMismatch, kWhere);
    }
}

TEST(DevelopSettingsV2, ValidatesChromaDenoise) {
    DevelopSettings s;
    for (const double strength : {0.0, 0.25, 1.0}) {
        s.chroma_denoise.strength = strength;
        EXPECT_TRUE(s.validate()) << strength;
    }
    EXPECT_FALSE(s.is_as_shot());
    s.chroma_denoise.strength = 0.0;
    s.chroma_denoise.radius = kMaxChromaDenoiseRadius;
    EXPECT_TRUE(s.validate());
    EXPECT_TRUE(s.is_as_shot());  // the radius does not matter at strength 0
    s.chroma_denoise.radius = kMinChromaDenoiseRadius;
    EXPECT_TRUE(s.validate());
    for (const double bad : {-1e-9, 1.0 + 1e-9, kNaN, kInf}) {
        DevelopSettings t;
        t.chroma_denoise.strength = bad;
        ts::expect_error(t.validate(), ErrorCode::InvalidArgument, kWhere);
    }
    for (const double bad : {0.5, kMaxChromaDenoiseRadius + 0.5, kNaN, -kInf}) {
        DevelopSettings t;
        t.chroma_denoise.radius = bad;
        ts::expect_error(t.validate(), ErrorCode::InvalidArgument, kWhere);
    }
}

TEST(DevelopSettingsV2, ValidatesGeometry) {
    DevelopSettings s;
    s.geometry.straighten_degrees = -kMaxStraightenDegrees;
    s.geometry.crop = CropRect{0.0, 0.25, 0.5, 1.0};
    s.geometry.quarter_turns = 3;
    s.geometry.flip_horizontal = true;
    EXPECT_TRUE(s.validate());
    EXPECT_FALSE(s.is_as_shot());
    s.geometry.crop = CropRect{0.5, 0.5, 0.5 + kMinCropExtent, 0.5 + kMinCropExtent};
    EXPECT_TRUE(s.validate());

    std::vector<GeometrySetting> bad(10);
    bad[0].straighten_degrees = kMaxStraightenDegrees + 1e-9;
    bad[1].straighten_degrees = kNaN;
    bad[2].crop = CropRect{-1e-9, 0.0, 1.0, 1.0};
    bad[3].crop = CropRect{0.0, 0.0, 1.0 + 1e-9, 1.0};
    bad[4].crop = CropRect{0.6, 0.0, 0.4, 1.0};  // not ordered
    bad[5].crop = CropRect{0.5, 0.0, 0.5 + 0.5 * kMinCropExtent, 1.0};
    bad[6].crop = CropRect{0.0, 0.3, 1.0, 0.3};
    bad[7].crop = CropRect{0.0, 0.0, kInf, 1.0};
    bad[8].quarter_turns = 4;
    bad[9].crop = CropRect{0.0, kNaN, 1.0, 1.0};
    for (std::size_t i = 0; i < bad.size(); ++i) {
        DevelopSettings t;
        t.geometry = bad[i];
        ts::expect_error(t.validate(), ErrorCode::InvalidArgument, kWhere);
    }
    // Any single change makes the geometry non-identity.
    std::vector<GeometrySetting> changes(4);
    changes[0].straighten_degrees = 0.1;
    changes[1].crop.bottom = 0.9;
    changes[2].quarter_turns = 2;
    changes[3].flip_horizontal = true;
    for (const GeometrySetting& g : changes) {
        EXPECT_FALSE(g.is_identity());
    }
}

TEST(DevelopSettingsV2, ValidatesLocalAdjustmentBounds) {
    DevelopSettings s;
    for (std::uint32_t id = 1; id <= kMaxLocalAdjustments; ++id) {
        s.local_adjustments.push_back(radial_adjustment(id));
    }
    EXPECT_TRUE(s.validate());
    s.local_adjustments.push_back(radial_adjustment(1000));
    ts::expect_error(s.validate(), ErrorCode::InvalidArgument, kWhere);

    const auto with = [](const LocalAdjustment& a) {
        DevelopSettings t;
        t.local_adjustments.push_back(a);
        return t;
    };
    // Ids: at least 1 and unique.
    {
        DevelopSettings t;
        t.local_adjustments = {radial_adjustment(3), radial_adjustment(3)};
        ts::expect_error(t.validate(), ErrorCode::InvalidArgument, kWhere);
        ts::expect_error(with(radial_adjustment(0)).validate(), ErrorCode::InvalidArgument, kWhere);
    }
    // Opacity and controls.
    {
        LocalAdjustment a = radial_adjustment(1);
        a.opacity = 1.0 + 1e-9;
        ts::expect_error(with(a).validate(), ErrorCode::InvalidArgument, kWhere);
        a = radial_adjustment(1);
        a.controls.exposure_ev = kMaxLocalExposureEv;
        EXPECT_TRUE(with(a).validate());
        a.controls.exposure_ev = kMaxLocalExposureEv + 1e-9;
        ts::expect_error(with(a).validate(), ErrorCode::InvalidArgument, kWhere);
        using Control = double LocalControls::*;
        for (const Control c : {&LocalControls::contrast, &LocalControls::highlights, &LocalControls::shadows,
                                &LocalControls::saturation, &LocalControls::temperature, &LocalControls::tint}) {
            LocalAdjustment b = radial_adjustment(1);
            b.controls.*c = -1.0;
            EXPECT_TRUE(with(b).validate());
            b.controls.*c = 1.0 + 1e-9;
            ts::expect_error(with(b).validate(), ErrorCode::InvalidArgument, kWhere);
            b.controls.*c = kNaN;
            ts::expect_error(with(b).validate(), ErrorCode::InvalidArgument, kWhere);
        }
    }
    // Radial shapes.
    {
        const auto radial = [&](const RadialGradientMask& m) {
            LocalAdjustment a = radial_adjustment(1);
            a.shape = m;
            return with(a);
        };
        RadialGradientMask m;
        m.center = {-kMaskPositionMargin, 1.0 + kMaskPositionMargin};
        m.radius_x = kMinRadialRadius;
        m.radius_y = kMaxRadialRadius;
        m.angle_degrees = -kMaxAngleDegrees;
        m.feather = 0.0;
        EXPECT_TRUE(radial(m).validate());
        std::vector<RadialGradientMask> bad(6);
        bad[0].center.x = -kMaskPositionMargin - 1e-9;
        bad[1].radius_x = 0.5 * kMinRadialRadius;
        bad[2].radius_y = kMaxRadialRadius + 1e-9;
        bad[3].angle_degrees = kMaxAngleDegrees + 1e-9;
        bad[4].feather = 1.0 + 1e-9;
        bad[5].center.y = kNaN;
        for (const RadialGradientMask& b : bad) {
            ts::expect_error(radial(b).validate(), ErrorCode::InvalidArgument, kWhere);
        }
    }
    // Linear shapes: the ends must differ.
    {
        LocalAdjustment a = radial_adjustment(1);
        a.shape = LinearGradientMask{{0.0, 0.0}, {0.0, kMinGradientLength}, 0.0};
        EXPECT_TRUE(with(a).validate());
        a.shape = LinearGradientMask{{0.0, 0.0}, {0.5 * kMinGradientLength, 0.0}, 0.5};
        ts::expect_error(with(a).validate(), ErrorCode::InvalidArgument, kWhere);
        a.shape = LinearGradientMask{{0.3, 0.3}, {2.5, 0.3}, 0.5};
        ts::expect_error(with(a).validate(), ErrorCode::InvalidArgument, kWhere);
        a.shape = LinearGradientMask{{0.3, 0.3}, {0.6, 0.3}, -0.1};
        ts::expect_error(with(a).validate(), ErrorCode::InvalidArgument, kWhere);
    }
    // Brush shapes: strokes, points, radii and modes.
    {
        LocalAdjustment a = radial_adjustment(1);
        a.shape = BrushMask{};  // a brush mask nothing was painted with yet
        EXPECT_TRUE(with(a).validate());
        BrushMask brush;
        brush.strokes.push_back(stroke_with(kMaxStrokePoints));
        a.shape = brush;
        EXPECT_TRUE(with(a).validate());
        brush.strokes[0].points.push_back({0.5, 0.5});
        a.shape = brush;
        ts::expect_error(with(a).validate(), ErrorCode::InvalidArgument, kWhere);
        brush.strokes[0].points.clear();
        a.shape = brush;
        ts::expect_error(with(a).validate(), ErrorCode::InvalidArgument, kWhere);

        BrushMask many;
        many.strokes.assign(kMaxBrushStrokes, stroke_with(1));
        a.shape = many;
        EXPECT_TRUE(with(a).validate());
        many.strokes.push_back(stroke_with(1));
        a.shape = many;
        ts::expect_error(with(a).validate(), ErrorCode::InvalidArgument, kWhere);

        std::vector<BrushStroke> strokes(6, stroke_with(2));
        strokes[0].radius = 0.5 * kMinBrushRadius;
        strokes[1].radius = kMaxBrushRadius + 1e-9;
        strokes[2].hardness = -0.1;
        strokes[3].flow = 1.5;
        strokes[4].mode = static_cast<BrushMode>(9);
        strokes[5].points[1] = {0.5, 1.0 + kMaskPositionMargin + 1e-9};
        for (const BrushStroke& s0 : strokes) {
            a.shape = BrushMask{{s0}};
            ts::expect_error(with(a).validate(), ErrorCode::InvalidArgument, kWhere);
        }
    }
    // The points of all brush masks together are bounded.
    {
        DevelopSettings t;
        const std::size_t per_mask = kMaxBrushPoints / 4;
        for (std::uint32_t id = 1; id <= 4; ++id) {
            LocalAdjustment a = radial_adjustment(id);
            BrushMask brush;
            for (std::size_t k = 0; k < per_mask / kMaxStrokePoints; ++k) {
                brush.strokes.push_back(stroke_with(kMaxStrokePoints));
            }
            a.shape = brush;
            t.local_adjustments.push_back(a);
        }
        EXPECT_TRUE(t.validate());
        LocalAdjustment extra = radial_adjustment(5);
        extra.shape = BrushMask{{stroke_with(1)}};
        t.local_adjustments.push_back(extra);
        ts::expect_error(t.validate(), ErrorCode::InvalidArgument, kWhere);
    }
}

TEST(DevelopSettingsV2, AsShotIgnoresMasksWithoutEffect) {
    DevelopSettings s;
    LocalAdjustment a = radial_adjustment(1);
    a.controls = LocalControls{};
    s.local_adjustments.push_back(a);  // no control set
    EXPECT_TRUE(s.is_as_shot());
    s.local_adjustments[0].controls.tint = 0.2;
    EXPECT_FALSE(s.is_as_shot());
    s.local_adjustments[0].enabled = false;
    EXPECT_TRUE(s.is_as_shot());
    s.local_adjustments[0].enabled = true;
    s.local_adjustments[0].opacity = 0.0;
    EXPECT_TRUE(s.is_as_shot());
    EXPECT_FALSE(s.local_adjustments[0].has_effect());
    EXPECT_EQ(s.local_adjustments[0].kind(), MaskKind::RadialGradient);
}

TEST(DevelopSettingsV2, NamesOfMaskKindsAndBrushModes) {
    for (const MaskKind kind : {MaskKind::Brush, MaskKind::LinearGradient, MaskKind::RadialGradient}) {
        const auto parsed = parse_mask_kind(to_string(kind));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, kind);
    }
    EXPECT_EQ(to_string(MaskKind::LinearGradient), "linear");
    EXPECT_EQ(to_string(static_cast<MaskKind>(0)), "invalid");
    EXPECT_FALSE(parse_mask_kind("Brush").has_value());
    for (const BrushMode mode : {BrushMode::Add, BrushMode::Erase}) {
        const auto parsed = parse_brush_mode(to_string(mode));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, mode);
    }
    EXPECT_FALSE(parse_brush_mode("paint").has_value());
    EXPECT_EQ(kind_of(MaskShape{BrushMask{}}), MaskKind::Brush);
    EXPECT_EQ(kind_of(MaskShape{LinearGradientMask{}}), MaskKind::LinearGradient);
}

TEST(DevelopSettingsV2, RevisionsIdentifyValues) {
    GeometrySetting a;
    GeometrySetting b;
    EXPECT_EQ(geometry_revision(a), geometry_revision(b));
    b.straighten_degrees = -0.0;  // -0 is 0
    EXPECT_EQ(geometry_revision(a), geometry_revision(b));
    std::vector<GeometrySetting> changed(5);
    changed[0].straighten_degrees = 1.0;
    changed[1].crop.left = 0.01;
    changed[2].crop.bottom = 0.99;
    changed[3].quarter_turns = 1;
    changed[4].flip_horizontal = true;
    for (const GeometrySetting& g : changed) {
        EXPECT_NE(geometry_revision(g), geometry_revision(a));
    }
    BrushMask brush{{stroke_with(5)}};
    const std::uint64_t r0 = mask_shape_revision(brush);
    EXPECT_EQ(r0, mask_shape_revision(BrushMask{{stroke_with(5)}}));
    brush.strokes[0].points[2].y += 1e-12;
    EXPECT_NE(mask_shape_revision(brush), r0);
    brush = BrushMask{{stroke_with(5)}};
    brush.strokes[0].mode = BrushMode::Erase;
    EXPECT_NE(mask_shape_revision(brush), r0);
    EXPECT_NE(mask_shape_revision(LinearGradientMask{}), mask_shape_revision(RadialGradientMask{}));
    RadialGradientMask radial;
    const std::uint64_t rr = mask_shape_revision(radial);
    radial.feather = 0.4;
    EXPECT_NE(mask_shape_revision(radial), rr);
}

TEST(DevelopSettingsV2, OrientationHelpersActOnTheDisplayedImage) {
    // All eight states: turning or mirroring the displayed image composes its orientation with
    // the turn or mirror applied last.
    for (std::uint32_t q = 0; q < 4; ++q) {
        for (const bool flip : {false, true}) {
            GeometrySetting g;
            g.quarter_turns = q;
            g.flip_horizontal = flip;
            g.crop = CropRect{0.1, 0.2, 0.7, 0.9};
            g.straighten_degrees = 3.0;
            const image::Orientation o = user_orientation(g);
            EXPECT_EQ(user_orientation(rotated_clockwise(g)), image::compose(o, image::Orientation::Rotate90Cw));
            EXPECT_EQ(user_orientation(rotated_counter_clockwise(g)),
                      image::compose(o, image::Orientation::Rotate90Ccw));
            EXPECT_EQ(user_orientation(flipped_horizontally(g)), image::compose(o, image::Orientation::FlipHorizontal));
            EXPECT_EQ(flipped_horizontally(flipped_horizontally(g)), g);
            EXPECT_EQ(rotated_counter_clockwise(rotated_clockwise(g)), g);
            EXPECT_EQ(rotated_clockwise(g).crop, g.crop);  // the crop stays with the content
        }
    }
    GeometrySetting g;
    EXPECT_EQ(user_orientation(g), image::Orientation::Normal);
    g.quarter_turns = 1;
    EXPECT_EQ(user_orientation(g), image::Orientation::Rotate90Cw);
    g.flip_horizontal = true;  // mirror first, then turn
    EXPECT_EQ(user_orientation(g), image::compose(image::Orientation::FlipHorizontal, image::Orientation::Rotate90Cw));
}

}  // namespace
}  // namespace lucent::engine
