#include <cmath>
#include <limits>

#include <gtest/gtest.h>

#include <lucent/image/color_encoding.hpp>

namespace lucent::image {
namespace {

// Persisted values: changing any of these breaks stored recipes and catalogs.
static_assert(static_cast<int>(ColorPrimaries::CameraNative) == 1);
static_assert(static_cast<int>(ColorPrimaries::CieXyz) == 2);
static_assert(static_cast<int>(ColorPrimaries::Rec709Srgb) == 3);
static_assert(static_cast<int>(ColorPrimaries::Rec2020) == 4);
static_assert(static_cast<int>(ColorPrimaries::DisplayP3) == 5);
static_assert(static_cast<int>(ColorPrimaries::ProPhotoRomm) == 6);
static_assert(static_cast<int>(ColorPrimaries::AdobeRgb1998) == 7);
static_assert(static_cast<int>(WhitePoint::D50) == 1);
static_assert(static_cast<int>(WhitePoint::D65) == 2);
static_assert(static_cast<int>(WhitePoint::Custom) == 3);
static_assert(static_cast<int>(TransferFunction::Linear) == 1);
static_assert(static_cast<int>(TransferFunction::Srgb) == 2);
static_assert(static_cast<int>(TransferFunction::Gamma) == 3);
static_assert(static_cast<int>(TransferFunction::ProPhotoRomm) == 4);
static_assert(static_cast<int>(TransferFunction::Rec709) == 5);
static_assert(static_cast<int>(NumericRange::Unbounded) == 1);
static_assert(static_cast<int>(NumericRange::UnitClamped) == 2);
static_assert(static_cast<int>(ImageReference::Scene) == 1);
static_assert(static_cast<int>(ImageReference::Display) == 2);
static_assert(static_cast<int>(ChannelLayout::Mono) == 1);
static_assert(static_cast<int>(ChannelLayout::Rgb) == 2);
static_assert(static_cast<int>(ChannelLayout::Rgba) == 3);
static_assert(static_cast<int>(ChannelLayout::CfaMosaic) == 4);

void expect_invalid(const ColorEncoding& e) {
    const Status status = e.validate();
    ASSERT_FALSE(status) << e.to_string();
    EXPECT_EQ(status.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(status.error().where, "image.color_encoding.validate");
}

TEST(ColorEncoding, WorkingSpaceIsSceneLinearRec2020D65) {
    constexpr ColorEncoding e = ColorEncoding::scene_linear_rec2020_d65();
    EXPECT_EQ(e.layout, ChannelLayout::Rgb);
    EXPECT_EQ(e.primaries, ColorPrimaries::Rec2020);
    EXPECT_EQ(e.white_point, WhitePoint::D65);
    EXPECT_EQ(e.transfer, TransferFunction::Linear);
    EXPECT_EQ(e.range, NumericRange::Unbounded);
    EXPECT_EQ(e.reference, ImageReference::Scene);
    EXPECT_EQ(e, ColorEncoding{});  // the default is the working space
}

TEST(ColorEncoding, NamedConstructors) {
    const ColorEncoding linear = ColorEncoding::linear_srgb();
    EXPECT_EQ(linear.primaries, ColorPrimaries::Rec709Srgb);
    EXPECT_EQ(linear.white_point, WhitePoint::D65);
    EXPECT_EQ(linear.transfer, TransferFunction::Linear);
    EXPECT_EQ(linear.reference, ImageReference::Scene);

    const ColorEncoding display = ColorEncoding::srgb_display();
    EXPECT_EQ(display.primaries, ColorPrimaries::Rec709Srgb);
    EXPECT_EQ(display.white_point, WhitePoint::D65);
    EXPECT_EQ(display.transfer, TransferFunction::Srgb);
    EXPECT_EQ(display.range, NumericRange::UnitClamped);
    EXPECT_EQ(display.reference, ImageReference::Display);

    const ColorEncoding camera = ColorEncoding::camera_native_linear();
    EXPECT_EQ(camera.layout, ChannelLayout::Rgb);
    EXPECT_EQ(camera.primaries, ColorPrimaries::CameraNative);
    EXPECT_EQ(camera.transfer, TransferFunction::Linear);
    EXPECT_EQ(camera.reference, ImageReference::Scene);

    const ColorEncoding mosaic = ColorEncoding::camera_native_mosaic();
    EXPECT_EQ(mosaic.layout, ChannelLayout::CfaMosaic);
    EXPECT_EQ(mosaic.primaries, ColorPrimaries::CameraNative);

    for (const ColorEncoding& e :
         {ColorEncoding::scene_linear_rec2020_d65(), linear, display, camera, mosaic}) {
        EXPECT_TRUE(e.validate()) << e.to_string();
    }
    EXPECT_NE(linear, display);
    EXPECT_NE(camera, mosaic);
}

TEST(ColorEncoding, ValidateRejectsUnknownEnumValues) {
    ColorEncoding e;
    e.layout = static_cast<ChannelLayout>(0);
    expect_invalid(e);
    e = {};
    e.layout = static_cast<ChannelLayout>(5);
    expect_invalid(e);
    e = {};
    e.primaries = static_cast<ColorPrimaries>(8);
    expect_invalid(e);
    e = {};
    e.white_point = static_cast<WhitePoint>(4);
    expect_invalid(e);
    e = {};
    e.transfer = static_cast<TransferFunction>(6);
    expect_invalid(e);
    e = {};
    e.range = static_cast<NumericRange>(3);
    expect_invalid(e);
    e = {};
    e.reference = static_cast<ImageReference>(0);
    expect_invalid(e);
}

TEST(ColorEncoding, ValidateCustomWhite) {
    ColorEncoding e;
    e.white_point = WhitePoint::Custom;
    e.custom_white = {0.3127, 0.3290};
    EXPECT_TRUE(e.validate());
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    for (const Chromaticity bad :
         {Chromaticity{0.0, 0.3}, Chromaticity{0.3, 0.0}, Chromaticity{0.5, 0.5}, Chromaticity{0.6, 0.5},
          Chromaticity{-0.1, 0.3}, Chromaticity{kNaN, 0.3}, Chromaticity{0.3, kNaN}}) {
        e.custom_white = bad;
        expect_invalid(e);
    }
    // The custom chromaticity is irrelevant unless selected, and for camera-native data.
    e.white_point = WhitePoint::D50;
    EXPECT_TRUE(e.validate());
    e.white_point = WhitePoint::Custom;
    e.primaries = ColorPrimaries::CameraNative;
    EXPECT_TRUE(e.validate());
}

TEST(ColorEncoding, ValidateGammaExponent) {
    ColorEncoding e;
    e.transfer = TransferFunction::Gamma;
    e.gamma = 2.2;
    EXPECT_TRUE(e.validate());
    e.gamma = 563.0 / 256.0;  // Adobe RGB (1998)
    EXPECT_TRUE(e.validate());
    for (double bad : {0.0, -1.0, 10.5, std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity()}) {
        e.gamma = bad;
        expect_invalid(e);
    }
    e.transfer = TransferFunction::Srgb;  // gamma is ignored for other curves
    e.gamma = 0.0;
    EXPECT_TRUE(e.validate());
}

TEST(ColorEncoding, ValidateMosaicContract) {
    EXPECT_TRUE(ColorEncoding::camera_native_mosaic().validate());
    ColorEncoding e = ColorEncoding::camera_native_mosaic();
    e.primaries = ColorPrimaries::Rec2020;
    expect_invalid(e);
    e = ColorEncoding::camera_native_mosaic();
    e.transfer = TransferFunction::Srgb;
    expect_invalid(e);
    e = ColorEncoding::camera_native_mosaic();
    e.reference = ImageReference::Display;
    expect_invalid(e);
}

TEST(ColorEncoding, EqualityComparesOnlyMeaningfulFields) {
    ColorEncoding a;
    ColorEncoding b;
    b.custom_white = {0.4, 0.4};  // unused: white_point is D65
    b.gamma = 3.0;                // unused: transfer is Linear
    EXPECT_EQ(a, b);

    a.white_point = b.white_point = WhitePoint::Custom;
    a.custom_white = {0.3, 0.3};
    EXPECT_NE(a, b);
    a.custom_white = b.custom_white;
    EXPECT_EQ(a, b);

    a.transfer = b.transfer = TransferFunction::Gamma;
    a.gamma = 2.2;
    EXPECT_NE(a, b);
    b.gamma = 2.2;
    EXPECT_EQ(a, b);

    ColorEncoding c = ColorEncoding::camera_native_linear();
    ColorEncoding d = ColorEncoding::camera_native_linear();
    d.white_point = WhitePoint::D50;  // not meaningful for camera-native data
    EXPECT_EQ(c, d);

    ColorEncoding e;
    e.range = NumericRange::UnitClamped;
    EXPECT_NE(e, ColorEncoding{});
    e = {};
    e.reference = ImageReference::Display;
    EXPECT_NE(e, ColorEncoding{});
    e = {};
    e.layout = ChannelLayout::Rgba;
    EXPECT_NE(e, ColorEncoding{});
}

TEST(ColorEncoding, ToString) {
    EXPECT_EQ(ColorEncoding::scene_linear_rec2020_d65().to_string(),
              "rgb rec2020 d65 linear unbounded scene");
    EXPECT_EQ(ColorEncoding::srgb_display().to_string(), "rgb rec709-srgb d65 srgb unit-clamped display");
    EXPECT_EQ(ColorEncoding::camera_native_mosaic().to_string(),
              "cfa-mosaic camera-native linear unbounded scene");

    ColorEncoding adobe;
    adobe.primaries = ColorPrimaries::AdobeRgb1998;
    adobe.transfer = TransferFunction::Gamma;
    adobe.gamma = 563.0 / 256.0;
    adobe.range = NumericRange::UnitClamped;
    adobe.reference = ImageReference::Display;
    EXPECT_EQ(adobe.to_string(), "rgb adobe-rgb-1998 d65 gamma(2.19921875) unit-clamped display");

    ColorEncoding custom;
    custom.primaries = ColorPrimaries::CieXyz;
    custom.white_point = WhitePoint::Custom;
    custom.custom_white = {0.25, 0.5};
    EXPECT_EQ(custom.to_string(), "rgb cie-xyz custom(0.25, 0.5) linear unbounded scene");
}

}  // namespace
}  // namespace lucent::image
