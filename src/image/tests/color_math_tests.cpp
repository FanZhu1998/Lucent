#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/image/color_math.hpp>

namespace lucent::image {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

::testing::AssertionResult matrices_near(const Matrix3d& actual, const Matrix3d& expected, double tolerance) {
    double worst = 0.0;
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            worst = std::max(worst, std::abs(actual(r, c) - expected(r, c)));
        }
    }
    if (worst <= tolerance) {
        return ::testing::AssertionSuccess();
    }
    std::string text =
        "max |difference| " + std::to_string(worst) + " > " + std::to_string(tolerance) + "; actual:";
    for (std::size_t r = 0; r < 3; ++r) {
        text += "\n  ";
        for (std::size_t c = 0; c < 3; ++c) {
            char cell[32];
            std::snprintf(cell, sizeof(cell), "%.9f ", actual(r, c));
            text += cell;
        }
    }
    return ::testing::AssertionFailure() << text;
}

::testing::AssertionResult vectors_near(const Vec3d& actual, const Vec3d& expected, double tolerance) {
    for (std::size_t i = 0; i < 3; ++i) {
        if (!(std::abs(actual[i] - expected[i]) <= tolerance)) {
            return ::testing::AssertionFailure()
                   << "component " << i << ": " << actual[i] << " vs " << expected[i];
        }
    }
    return ::testing::AssertionSuccess();
}

ColorEncoding encoding(ColorPrimaries primaries, WhitePoint white) {
    ColorEncoding e;
    e.primaries = primaries;
    e.white_point = white;
    return e;
}

// Chromaticity of an XYZ white, for reproducing tables computed from XYZ whites.
Chromaticity xy_of(const Vec3d& XYZ) {
    return *XYZ_to_xy(XYZ);
}

// Bruce Lindbloom's white points (brucelindbloom.com, "Chromatic Adaptation" and "RGB/XYZ
// Matrices"), from which his published matrices were computed.
const Vec3d kLindbloomD65{0.95047, 1.00000, 1.08883};
const Vec3d kLindbloomD50{0.96422, 1.00000, 0.82521};

// ---------------------------------------------------------------------------------------------
// Matrix3d

TEST(Matrix3d, MultiplicationFollowsColumnVectorConvention) {
    const Matrix3d a{{1, 2, 3}, {4, 5, 6}, {7, 8, 10}};
    const Matrix3d b{{2, 0, 1}, {1, 3, 0}, {0, 1, 4}};
    EXPECT_EQ(a * Vec3d(1, 0, -1), Vec3d(-2, -2, -3));
    EXPECT_EQ(a * b, (Matrix3d{{4, 9, 13}, {13, 21, 28}, {22, 34, 47}}));
    // (a * b) * v == a * (b * v)
    const Vec3d v{0.5, -2.0, 3.0};
    EXPECT_EQ((a * b) * v, a * (b * v));
    EXPECT_EQ(transpose(a), (Matrix3d{{1, 4, 7}, {2, 5, 8}, {3, 6, 10}}));
    EXPECT_EQ(Matrix3d::identity() * a, a);
    EXPECT_EQ(Matrix3d::diagonal(Vec3d(2, 3, 4)) * Vec3d(1, 1, 1), Vec3d(2, 3, 4));
    EXPECT_EQ(Matrix3d{}, (Matrix3d{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}));
}

TEST(Matrix3d, Determinant) {
    EXPECT_EQ(determinant(Matrix3d::identity()), 1.0);
    EXPECT_EQ(determinant(Matrix3d{{1, 2, 3}, {4, 5, 6}, {7, 8, 10}}), -3.0);
    EXPECT_EQ(determinant(Matrix3d{{1, 2, 3}, {4, 5, 6}, {7, 8, 9}}), 0.0);
}

TEST(Matrix3d, InverseRoundTripsToIdentity) {
    const Matrix3d m{{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}};
    auto inv = inverse(m);
    ASSERT_TRUE(inv);
    // Double-precision rounding of a well-conditioned 3x3 inverse: errors of order 1e-16.
    EXPECT_TRUE(matrices_near(m * *inv, Matrix3d::identity(), 1e-14));
    EXPECT_TRUE(matrices_near(*inv * m, Matrix3d::identity(), 1e-14));
    auto exact = inverse(Matrix3d{{1, 2, 3}, {4, 5, 6}, {7, 8, 10}});
    ASSERT_TRUE(exact);
    EXPECT_TRUE(matrices_near(
        *exact, Matrix3d{{-2.0 / 3.0, -4.0 / 3.0, 1.0}, {-2.0 / 3.0, 11.0 / 3.0, -2.0}, {1.0, -2.0, 1.0}},
        1e-14));
    // Scale independence: a tiny but well-conditioned matrix is invertible.
    auto tiny = inverse(Matrix3d::diagonal(Vec3d(1e-30, 2e-30, 3e-30)));
    ASSERT_TRUE(tiny);
    EXPECT_NEAR((*tiny)(2, 2), 1.0 / 3e-30, 1e-14 / 3e-30);
}

TEST(Matrix3d, InverseRejectsSingularAndNonFiniteMatrices) {
    const Matrix3d cases[] = {
        Matrix3d{},                                         // zero
        Matrix3d{{1, 2, 3}, {4, 5, 6}, {7, 8, 9}},          // rank 2
        Matrix3d{{1, 2, 3}, {2, 4 + 1e-14, 6}, {0, 0, 1}},  // numerically singular (det ~1e-14)
        Matrix3d{{1, 0, 0}, {0, kNaN, 0}, {0, 0, 1}},
        Matrix3d{{1, 0, 0}, {0, std::numeric_limits<double>::infinity(), 0}, {0, 0, 1}},
    };
    for (const Matrix3d& m : cases) {
        auto inv = inverse(m);
        ASSERT_FALSE(inv);
        EXPECT_EQ(inv.error().code, ErrorCode::InvalidArgument);
    }
    EXPECT_FALSE(is_finite(cases[3]));
    EXPECT_TRUE(is_finite(cases[1]));
}

// ---------------------------------------------------------------------------------------------
// Chromaticity and XYZ

TEST(Chromaticity, XyAndXyzRoundTrip) {
    auto XYZ = xy_to_XYZ(kD65White);
    ASSERT_TRUE(XYZ);
    EXPECT_DOUBLE_EQ((*XYZ)[0], 0.3127 / 0.3290);
    EXPECT_DOUBLE_EQ((*XYZ)[1], 1.0);
    EXPECT_DOUBLE_EQ((*XYZ)[2], (1.0 - 0.3127 - 0.3290) / 0.3290);
    auto xy = XYZ_to_xy(*XYZ);
    ASSERT_TRUE(xy);
    EXPECT_NEAR(xy->x, 0.3127, 1e-15);
    EXPECT_NEAR(xy->y, 0.3290, 1e-15);
    auto scaled = xy_to_XYZ(kD50White, 0.5);
    ASSERT_TRUE(scaled);
    EXPECT_DOUBLE_EQ((*scaled)[1], 0.5);
}

TEST(Chromaticity, RejectsDegenerateInput) {
    EXPECT_FALSE(xy_to_XYZ(Chromaticity{0.3, 0.0}));
    EXPECT_FALSE(xy_to_XYZ(Chromaticity{0.3, -0.1}));
    EXPECT_FALSE(xy_to_XYZ(Chromaticity{kNaN, 0.3}));
    EXPECT_FALSE(xy_to_XYZ(kD65White, kNaN));
    EXPECT_FALSE(XYZ_to_xy(Vec3d(0, 0, 0)));
    EXPECT_FALSE(XYZ_to_xy(Vec3d(1, -1, 0)));
    EXPECT_FALSE(XYZ_to_xy(Vec3d(kNaN, 1, 1)));
}

TEST(ColorMath, PrimariesAndWhitesForEncodings) {
    EXPECT_EQ(*rgb_primaries(ColorPrimaries::Rec709Srgb), kRec709Primaries);
    EXPECT_EQ(*rgb_primaries(ColorPrimaries::Rec2020), kRec2020Primaries);
    EXPECT_EQ(*rgb_primaries(ColorPrimaries::DisplayP3), kDisplayP3Primaries);
    EXPECT_EQ(*rgb_primaries(ColorPrimaries::ProPhotoRomm), kProPhotoRommPrimaries);
    EXPECT_EQ(*rgb_primaries(ColorPrimaries::AdobeRgb1998), kAdobeRgb1998Primaries);
    for (ColorPrimaries p :
         {ColorPrimaries::CameraNative, ColorPrimaries::CieXyz, static_cast<ColorPrimaries>(42)}) {
        auto result = rgb_primaries(p);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
    }
    EXPECT_EQ(*white_chromaticity(encoding(ColorPrimaries::Rec2020, WhitePoint::D65)), kD65White);
    EXPECT_EQ(*white_chromaticity(encoding(ColorPrimaries::ProPhotoRomm, WhitePoint::D50)), kD50White);
    ColorEncoding custom = encoding(ColorPrimaries::CieXyz, WhitePoint::Custom);
    custom.custom_white = {0.4476, 0.4074};
    EXPECT_EQ(*white_chromaticity(custom), (Chromaticity{0.4476, 0.4074}));
    EXPECT_FALSE(white_chromaticity(ColorEncoding::camera_native_linear()));
}

// ---------------------------------------------------------------------------------------------
// RGB <-> XYZ

TEST(ColorMath, SrgbToXyzMatchesIec61966) {
    // IEC 61966-2-1:1999 publishes the sRGB (D65) matrix to 4 decimals; the
    // tolerance is half a unit in the 4th decimal plus 1e-5 margin for double rounding.
    const Matrix3d iec{{0.4124, 0.3576, 0.1805}, {0.2126, 0.7152, 0.0722}, {0.0193, 0.1192, 0.9505}};
    auto m = rgb_to_xyz(kRec709Primaries, kD65White);
    ASSERT_TRUE(m);
    EXPECT_TRUE(matrices_near(*m, iec, 6e-5));
}

TEST(ColorMath, SrgbToXyzMatchesLindbloom) {
    const Matrix3d lindbloom{{0.4124564, 0.3575761, 0.1804375},
                             {0.2126729, 0.7151522, 0.0721750},
                             {0.0193339, 0.1191920, 0.9503041}};
    // With the white Lindbloom used (XYZ 0.95047, 1, 1.08883) the derivation reproduces his
    // 7-decimal table to its rounding (observed 4.9e-8), so 1e-6 is a real check of the NPM.
    auto exact = rgb_to_xyz(kRec709Primaries, xy_of(kLindbloomD65));
    ASSERT_TRUE(exact);
    EXPECT_TRUE(matrices_near(*exact, lindbloom, 1e-6));
    // With the 4-decimal D65 of BT.709/IEC 61966-2-1 the entries move by up to 2.3e-4 (the
    // blue-column Z entry), so the 2e-4 suggested for this comparison is too tight; 3e-4 bounds
    // the white-point rounding effect.
    auto canonical = rgb_to_xyz(kRec709Primaries, kD65White);
    ASSERT_TRUE(canonical);
    EXPECT_TRUE(matrices_near(*canonical, lindbloom, 3e-4));
}

TEST(ColorMath, Rec2020ToXyzMatchesPublished) {
    // Rec.2020 (D65) RGB -> XYZ published to 6 decimals (as derived from ITU-R BT.2020-2 Table 3;
    // cf. SMPTE RP 177 derivations). Observed difference 3.1e-7, i.e. within 6-decimal rounding;
    // tolerance 1e-6 (tighter than the 2e-4 requested).
    const Matrix3d published{
        {0.636958, 0.144617, 0.168881}, {0.262700, 0.677998, 0.059302}, {0.000000, 0.028073, 1.060985}};
    auto m = rgb_to_xyz(kRec2020Primaries, kD65White);
    ASSERT_TRUE(m);
    EXPECT_TRUE(matrices_near(*m, published, 1e-6));
}

TEST(ColorMath, RgbToXyzRowSumsEqualTheWhite) {
    const RgbPrimaries all[] = {kRec709Primaries, kRec2020Primaries, kDisplayP3Primaries,
                                kProPhotoRommPrimaries, kAdobeRgb1998Primaries};
    for (const RgbPrimaries& primaries : all) {
        for (Chromaticity white : {kD65White, kD50White, Chromaticity{0.44757, 0.40745}}) {
            auto m = rgb_to_xyz(primaries, white);
            ASSERT_TRUE(m);
            // RGB (1, 1, 1) is the white with Y = 1; exact up to double rounding.
            EXPECT_TRUE(vectors_near(*m * Vec3d(1, 1, 1), *xy_to_XYZ(white), 1e-12));
            auto back = xyz_to_rgb(primaries, white);
            ASSERT_TRUE(back);
            EXPECT_TRUE(matrices_near(*back * *m, Matrix3d::identity(), 1e-12));
        }
    }
}

TEST(ColorMath, RgbToXyzRejectsDegeneratePrimaries) {
    const RgbPrimaries collinear{{0.2, 0.2}, {0.3, 0.3}, {0.4, 0.4}};
    auto m = rgb_to_xyz(collinear, kD65White);
    ASSERT_FALSE(m);
    EXPECT_EQ(m.error().code, ErrorCode::InvalidArgument);
    const RgbPrimaries zero_y{{0.64, 0.33}, {0.3, 0.0}, {0.15, 0.06}};
    EXPECT_FALSE(rgb_to_xyz(zero_y, kD65White));
    EXPECT_FALSE(rgb_to_xyz(kRec709Primaries, Chromaticity{0.3, 0.0}));
}

// ---------------------------------------------------------------------------------------------
// Chromatic adaptation

TEST(ColorMath, BradfordD65ToD50MatchesLindbloom) {
    const Matrix3d lindbloom{{1.0478112, 0.0228866, -0.0501270},
                             {0.0295424, 0.9904844, -0.0170491},
                             {-0.0092345, 0.0150436, 0.7521316}};
    // Lindbloom's own whites reproduce the table to its 7-decimal rounding (observed 4.4e-8).
    auto exact = chromatic_adaptation(kLindbloomD65, kLindbloomD50, ChromaticAdaptationMethod::Bradford);
    ASSERT_TRUE(exact);
    EXPECT_TRUE(matrices_near(*exact, lindbloom, 1e-6));
    // The 4-decimal CIE/ITU whites shift entries by up to 2.6e-4 (row 3), hence 3e-4 here.
    auto canonical = chromatic_adaptation(*xy_to_XYZ(kD65White), *xy_to_XYZ(kD50White));
    ASSERT_TRUE(canonical);
    EXPECT_TRUE(matrices_near(*canonical, lindbloom, 3e-4));
}

TEST(ColorMath, XyzScalingIsTheWhiteRatio) {
    auto m = chromatic_adaptation(kLindbloomD65, kLindbloomD50, ChromaticAdaptationMethod::XyzScaling);
    ASSERT_TRUE(m);
    const Matrix3d expected =
        Matrix3d::diagonal(Vec3d(0.96422 / 0.95047, 1.0, 0.82521 / 1.08883));  // closed form
    EXPECT_TRUE(matrices_near(*m, expected, 1e-15));
}

TEST(ColorMath, AdaptationMapsSourceWhiteToDestinationWhite) {
    const Vec3d illuminant_a = *xy_to_XYZ(Chromaticity{0.44757, 0.40745});
    const Vec3d whites[] = {*xy_to_XYZ(kD65White), *xy_to_XYZ(kD50White), illuminant_a};
    for (auto method : {ChromaticAdaptationMethod::Bradford, ChromaticAdaptationMethod::Cat02,
                        ChromaticAdaptationMethod::VonKries, ChromaticAdaptationMethod::XyzScaling}) {
        for (const Vec3d& src : whites) {
            for (const Vec3d& dst : whites) {
                auto m = chromatic_adaptation(src, dst, method);
                ASSERT_TRUE(m) << static_cast<int>(method);
                EXPECT_TRUE(vectors_near(*m * src, dst, 1e-12)) << static_cast<int>(method);
                auto back = chromatic_adaptation(dst, src, method);
                ASSERT_TRUE(back);
                EXPECT_TRUE(matrices_near(*back * *m, Matrix3d::identity(), 1e-12));
            }
        }
    }
    // Identical whites give exactly the identity, not identity plus rounding noise.
    EXPECT_EQ(*chromatic_adaptation(whites[0], whites[0]), Matrix3d::identity());
}

TEST(ColorMath, AdaptationRejectsInvalidInput) {
    const Vec3d d65 = *xy_to_XYZ(kD65White);
    for (const Vec3d& bad : {Vec3d(0, 0, 0), Vec3d(-1, -1, -1), Vec3d(kNaN, 1, 1)}) {
        auto m = chromatic_adaptation(d65, bad);
        ASSERT_FALSE(m);
        EXPECT_EQ(m.error().code, ErrorCode::InvalidArgument);
        EXPECT_FALSE(chromatic_adaptation(bad, d65));
    }
    EXPECT_FALSE(chromatic_adaptation(d65, d65, static_cast<ChromaticAdaptationMethod>(9)));
    EXPECT_FALSE(cone_response_matrix(static_cast<ChromaticAdaptationMethod>(0)));
    EXPECT_EQ(*cone_response_matrix(ChromaticAdaptationMethod::XyzScaling), Matrix3d::identity());
}

// ---------------------------------------------------------------------------------------------
// Encoding-to-encoding conversion

TEST(ColorMath, ConversionRec709ToRec2020MatchesBt2087) {
    // ITU-R BT.2087-0 (linear BT.709 -> BT.2020 RGB, same D65 white), published to 4 decimals.
    const Matrix3d bt2087{{0.6274, 0.3293, 0.0433}, {0.0691, 0.9195, 0.0114}, {0.0164, 0.0880, 0.8956}};
    auto m = conversion_matrix(ColorEncoding::linear_srgb(), ColorEncoding::scene_linear_rec2020_d65());
    ASSERT_TRUE(m);
    EXPECT_TRUE(matrices_near(*m, bt2087, 6e-5));
    // Same white: white maps to white and each row sums to 1.
    EXPECT_TRUE(vectors_near(*m * Vec3d(1, 1, 1), Vec3d(1, 1, 1), 1e-12));
}

TEST(ColorMath, ConversionAdaptsWhitePoints) {
    // XYZ relative to D50 (the ICC PCS) into the D65 working space: the D50 white must become
    // RGB (1, 1, 1).
    const ColorEncoding pcs = encoding(ColorPrimaries::CieXyz, WhitePoint::D50);
    auto m = conversion_matrix(pcs, ColorEncoding::scene_linear_rec2020_d65());
    ASSERT_TRUE(m);
    EXPECT_TRUE(vectors_near(*m * *xy_to_XYZ(kD50White), Vec3d(1, 1, 1), 1e-12));

    // sRGB (D65) white -> ProPhoto (D50) white.
    auto to_prophoto = conversion_matrix(ColorEncoding::linear_srgb(),
                                         encoding(ColorPrimaries::ProPhotoRomm, WhitePoint::D50));
    ASSERT_TRUE(to_prophoto);
    EXPECT_TRUE(vectors_near(*to_prophoto * Vec3d(1, 1, 1), Vec3d(1, 1, 1), 1e-12));

    // The composition order is XYZ->dst * CAT * src->XYZ.
    const Matrix3d expected = *xyz_to_rgb(kProPhotoRommPrimaries, kD50White) *
                              *chromatic_adaptation(*xy_to_XYZ(kD65White), *xy_to_XYZ(kD50White)) *
                              *rgb_to_xyz(kRec709Primaries, kD65White);
    EXPECT_TRUE(matrices_near(*to_prophoto, expected, 1e-15));
}

TEST(ColorMath, ConversionRoundTripsToIdentity) {
    std::vector<ColorEncoding> spaces;
    for (auto primaries :
         {ColorPrimaries::CieXyz, ColorPrimaries::Rec709Srgb, ColorPrimaries::Rec2020,
          ColorPrimaries::DisplayP3, ColorPrimaries::ProPhotoRomm, ColorPrimaries::AdobeRgb1998}) {
        for (auto white : {WhitePoint::D50, WhitePoint::D65, WhitePoint::Custom}) {
            ColorEncoding e = encoding(primaries, white);
            e.custom_white = {0.44757, 0.40745};  // CIE illuminant A
            spaces.push_back(e);
        }
    }
    for (const ColorEncoding& a : spaces) {
        for (const ColorEncoding& b : spaces) {
            auto ab = conversion_matrix(a, b);
            auto ba = conversion_matrix(b, a);
            ASSERT_TRUE(ab && ba) << a.to_string() << " <-> " << b.to_string();
            // Products of two inverted, well-conditioned 3x3 matrices: rounding of order 1e-15.
            EXPECT_TRUE(matrices_near(*ba * *ab, Matrix3d::identity(), 1e-12))
                << a.to_string() << " -> " << b.to_string();
        }
    }
}

TEST(ColorMath, ConversionBetweenIdenticalColorimetryIsExactIdentity) {
    // Transfer, range and reference do not enter the matrix.
    EXPECT_EQ(*conversion_matrix(ColorEncoding::linear_srgb(), ColorEncoding::srgb_display()),
              Matrix3d::identity());
    ColorEncoding rgba = ColorEncoding::scene_linear_rec2020_d65();
    rgba.layout = ChannelLayout::Rgba;
    EXPECT_EQ(*conversion_matrix(rgba, ColorEncoding::scene_linear_rec2020_d65()), Matrix3d::identity());
}

TEST(ColorMath, ConversionRejectsUnconvertibleEncodings) {
    const ColorEncoding working = ColorEncoding::scene_linear_rec2020_d65();
    ColorEncoding mono = working;
    mono.layout = ChannelLayout::Mono;
    ColorEncoding broken = working;
    broken.transfer = static_cast<TransferFunction>(77);
    for (const ColorEncoding& bad :
         {ColorEncoding::camera_native_linear(), ColorEncoding::camera_native_mosaic(), mono, broken}) {
        auto forward = conversion_matrix(bad, working);
        ASSERT_FALSE(forward) << bad.to_string();
        EXPECT_EQ(forward.error().code, ErrorCode::InvalidArgument);
        auto backward = conversion_matrix(working, bad);
        ASSERT_FALSE(backward);
        EXPECT_EQ(backward.error().code, ErrorCode::InvalidArgument);
    }
    EXPECT_FALSE(to_xyz_matrix(ColorEncoding::camera_native_linear()));
    EXPECT_EQ(*to_xyz_matrix(encoding(ColorPrimaries::CieXyz, WhitePoint::D65)), Matrix3d::identity());
}

// ---------------------------------------------------------------------------------------------
// Transfer functions

// Linear-light sweep including zero, both knees, negatives and values above 1.
std::vector<double> linear_sweep() {
    std::vector<double> values = {0.0,       1e-9,       1e-6,   0.001, 0.0031308, 0.00313081, 0.0031309,
                                  1.0 / 512, 0.00195313, 0.0179, 0.018, 0.0181,    0.18,       0.5,
                                  0.9999,    1.0,        1.0001, 2.0,   4.0,       16.0,       1000.0};
    const std::size_t n = values.size();
    for (std::size_t i = 1; i < n; ++i) {
        values.push_back(-values[i]);
    }
    for (int i = 0; i <= 200; ++i) {
        values.push_back(-2.0 + 0.025 * i);
    }
    return values;
}

TEST(TransferFunction, SrgbReferenceValues) {
    // IEC 61966-2-1: V = 0.5 decodes to ((0.5 + 0.055) / 1.055)^2.4 = 0.21404114...
    EXPECT_NEAR(srgb_decode(0.5), 0.21404114, 1e-7);
    EXPECT_NEAR(srgb_decode(0.5f), 0.21404114f, 5e-7f);  // single precision
    EXPECT_EQ(srgb_encode(0.0), 0.0);
    EXPECT_EQ(srgb_decode(1.0), 1.0);
    EXPECT_NEAR(srgb_encode(1.0), 1.0, 1e-15);
    EXPECT_DOUBLE_EQ(srgb_encode(0.001), 0.01292);  // linear segment
    EXPECT_DOUBLE_EQ(srgb_decode(0.04), 0.04 / 12.92);
}

TEST(TransferFunction, Rec709AndProPhotoReferenceValues) {
    EXPECT_DOUBLE_EQ(rec709_oetf(0.01), 0.045);  // 4.5 L below 0.018
    EXPECT_DOUBLE_EQ(rec709_oetf(0.018), 1.099 * std::pow(0.018, 0.45) - 0.099);
    EXPECT_NEAR(rec709_oetf(1.0), 1.0, 1e-15);
    EXPECT_NEAR(rec709_inverse_oetf(1.0), 1.0, 1e-15);
    // ROMM segments meet at Et = 1/512 -> 16 Et = 1/32.
    EXPECT_EQ(prophoto_encode(1.0 / 1024), 16.0 / 1024);
    EXPECT_NEAR(prophoto_encode(1.0 / 512), 1.0 / 32, 1e-16);
    EXPECT_NEAR(prophoto_decode(1.0 / 32), 1.0 / 512, 1e-17);
    EXPECT_EQ(prophoto_decode(1.0), 1.0);
    EXPECT_DOUBLE_EQ(gamma_encode(0.25, 2.0), 0.5);
    EXPECT_DOUBLE_EQ(gamma_decode(0.5, 2.0), 0.25);
}

TEST(TransferFunction, OddExtensionForNegativeInputs) {
    for (double x : linear_sweep()) {
        EXPECT_EQ(srgb_encode(-x), -srgb_encode(x)) << x;
        EXPECT_EQ(srgb_decode(-x), -srgb_decode(x)) << x;
        EXPECT_EQ(rec709_oetf(-x), -rec709_oetf(x)) << x;
        EXPECT_EQ(rec709_inverse_oetf(-x), -rec709_inverse_oetf(x)) << x;
        EXPECT_EQ(prophoto_encode(-x), -prophoto_encode(x)) << x;
        EXPECT_EQ(prophoto_decode(-x), -prophoto_decode(x)) << x;
        EXPECT_EQ(gamma_encode(-x, 2.2), -gamma_encode(x, 2.2)) << x;
        EXPECT_EQ(gamma_decode(-x, 2.2), -gamma_decode(x, 2.2)) << x;
    }
    EXPECT_TRUE(std::isnan(srgb_encode(kNaN)));
}

TEST(TransferFunction, CurvesContinueAboveOne) {
    EXPECT_DOUBLE_EQ(srgb_encode(4.0), 1.055 * std::pow(4.0, 1.0 / 2.4) - 0.055);
    EXPECT_DOUBLE_EQ(srgb_decode(2.0), std::pow((2.0 + 0.055) / 1.055, 2.4));
    EXPECT_DOUBLE_EQ(prophoto_encode(4.0), std::pow(4.0, 1.0 / 1.8));
    EXPECT_DOUBLE_EQ(rec709_oetf(2.0), 1.099 * std::pow(2.0, 0.45) - 0.099);
    // Strictly increasing across the whole sweep, so nothing is clipped or folded.
    std::vector<double> sorted = linear_sweep();
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        EXPECT_LT(srgb_encode(sorted[i - 1]), srgb_encode(sorted[i])) << sorted[i];
        EXPECT_LT(rec709_oetf(sorted[i - 1]), rec709_oetf(sorted[i])) << sorted[i];
        EXPECT_LT(prophoto_encode(sorted[i - 1]), prophoto_encode(sorted[i])) << sorted[i];
    }
}

TEST(TransferFunction, DoubleRoundTripsOverSweep) {
    for (double x : linear_sweep()) {
        // Relative double rounding through pow: well below 1e-12.
        const double tol = 1e-12 * std::max(1.0, std::abs(x));
        // sRGB: the standard's thresholds leave a gap of up to 2.4e-9 just above 0.0031308.
        const double srgb_tol = std::max(tol, 2.4e-9);
        EXPECT_NEAR(srgb_decode(srgb_encode(x)), x, srgb_tol) << x;
        EXPECT_NEAR(rec709_inverse_oetf(rec709_oetf(x)), x, tol) << x;
        EXPECT_NEAR(prophoto_decode(prophoto_encode(x)), x, tol) << x;
        EXPECT_NEAR(gamma_decode(gamma_encode(x, 2.2), 2.2), x, tol) << x;
        EXPECT_NEAR(gamma_decode(gamma_encode(x, 563.0 / 256.0), 563.0 / 256.0), x, tol) << x;
        // Encoded-domain round trips (x reinterpreted as an encoded value). sRGB: up to 3e-8
        // for encoded values in (0.0404499, 0.04045].
        EXPECT_NEAR(srgb_encode(srgb_decode(x)), x, std::max(tol, 3e-8)) << x;
        EXPECT_NEAR(rec709_oetf(rec709_inverse_oetf(x)), x, tol) << x;
        EXPECT_NEAR(prophoto_encode(prophoto_decode(x)), x, tol) << x;
    }
}

TEST(TransferFunction, FloatMatchesDoubleAndRoundTrips) {
    // Single-precision evaluation (float constants and pow) stays within a few ulp of the double
    // result: relative 2e-6 of the expected value (1e-5 for float round trips).
    const auto near = [](double actual, double expected, double relative) {
        return std::abs(actual - expected) <= relative * std::max(1.0, std::abs(expected));
    };
    for (double xd : linear_sweep()) {
        if (std::abs(xd) > 16.0) {
            continue;
        }
        const auto x = static_cast<float>(xd);
        const double xf = static_cast<double>(x);
        EXPECT_TRUE(near(srgb_encode(x), srgb_encode(xf), 2e-6)) << xd;
        EXPECT_TRUE(near(srgb_decode(x), srgb_decode(xf), 2e-6)) << xd;
        // BT.709's rounded constants make the OETF jump by 2.5e-4 at L = 0.018 (and leave encoded
        // values 0.081..0.0812 unused). float(0.018) < 0.018, so exactly at a knee the float and
        // double versions legitimately pick different segments; compare away from the knees.
        const bool near_knee = std::abs(std::abs(xf) - 0.018) < 1e-6 || std::abs(std::abs(xf) - 0.081) < 1e-6;
        if (!near_knee) {
            EXPECT_TRUE(near(rec709_oetf(x), rec709_oetf(xf), 2e-6)) << xd;
            EXPECT_TRUE(near(rec709_inverse_oetf(x), rec709_inverse_oetf(xf), 2e-6)) << xd;
        }
        EXPECT_TRUE(near(prophoto_encode(x), prophoto_encode(xf), 2e-6)) << xd;
        EXPECT_TRUE(near(prophoto_decode(x), prophoto_decode(xf), 2e-6)) << xd;
        EXPECT_TRUE(near(gamma_encode(x, 2.2f), gamma_encode(xf, static_cast<double>(2.2f)), 2e-6)) << xd;
        EXPECT_TRUE(near(gamma_decode(x, 2.2f), gamma_decode(xf, static_cast<double>(2.2f)), 2e-6)) << xd;
        EXPECT_TRUE(near(srgb_decode(srgb_encode(x)), xf, 1e-5)) << xd;
        EXPECT_TRUE(near(prophoto_decode(prophoto_encode(x)), xf, 1e-5)) << xd;
        EXPECT_TRUE(near(rec709_inverse_oetf(rec709_oetf(x)), xf, 1e-5)) << xd;
    }
}

TEST(TransferCurve, DispatchesToTheSelectedCurve) {
    struct Case {
        TransferFunction function;
        double (*encode)(double);
        double (*decode)(double);
        float (*encode_f)(float);
        float (*decode_f)(float);
    };
    const Case cases[] = {
        {TransferFunction::Srgb, [](double v) { return srgb_encode(v); },
         [](double v) { return srgb_decode(v); }, [](float v) { return srgb_encode(v); },
         [](float v) { return srgb_decode(v); }},
        {TransferFunction::Rec709, [](double v) { return rec709_oetf(v); },
         [](double v) { return rec709_inverse_oetf(v); }, [](float v) { return rec709_oetf(v); },
         [](float v) { return rec709_inverse_oetf(v); }},
        {TransferFunction::ProPhotoRomm, [](double v) { return prophoto_encode(v); },
         [](double v) { return prophoto_decode(v); }, [](float v) { return prophoto_encode(v); },
         [](float v) { return prophoto_decode(v); }},
        {TransferFunction::Linear, [](double v) { return v; }, [](double v) { return v; },
         [](float v) { return v; }, [](float v) { return v; }},
    };
    for (const Case& c : cases) {
        auto curve = TransferCurve::create(c.function);
        ASSERT_TRUE(curve);
        EXPECT_EQ(curve->function(), c.function);
        for (double x : linear_sweep()) {
            EXPECT_EQ(curve->encode(x), c.encode(x));
            EXPECT_EQ(curve->decode(x), c.decode(x));
            const auto xf = static_cast<float>(x);
            EXPECT_EQ(curve->encode(xf), c.encode_f(xf));
            EXPECT_EQ(curve->decode(xf), c.decode_f(xf));
        }
    }
    auto gamma = TransferCurve::create(TransferFunction::Gamma, 2.2);
    ASSERT_TRUE(gamma);
    EXPECT_EQ(gamma->gamma(), 2.2);
    EXPECT_EQ(gamma->encode(0.5), gamma_encode(0.5, 2.2));
    EXPECT_EQ(gamma->decode(0.5f), gamma_decode(0.5f, 2.2f));

    ColorEncoding display = ColorEncoding::srgb_display();
    auto from_encoding = TransferCurve::from_encoding(display);
    ASSERT_TRUE(from_encoding);
    EXPECT_EQ(from_encoding->function(), TransferFunction::Srgb);
}

TEST(TransferCurve, RejectsInvalidParameters) {
    for (double bad : {0.0, -2.2, 10.01, kNaN}) {
        auto curve = TransferCurve::create(TransferFunction::Gamma, bad);
        ASSERT_FALSE(curve);
        EXPECT_EQ(curve.error().code, ErrorCode::InvalidArgument);
    }
    EXPECT_FALSE(TransferCurve::create(static_cast<TransferFunction>(0)));
    EXPECT_FALSE(TransferCurve::create(static_cast<TransferFunction>(6)));
    EXPECT_TRUE(TransferCurve::create(TransferFunction::Srgb, kNaN));  // gamma ignored
}

// ---------------------------------------------------------------------------------------------
// Colour temperature

TEST(ColorTemperature, CieDaylightReproducesD65AndD50) {
    // CIE D65 is daylight at 6504 K; D65 xy (0.3127, 0.3290) is its 4-decimal rounding. The
    // tolerance 5e-4 covers that rounding and the formula's own 4-5 digit coefficients
    // (observed differences 1.4e-5 in x and 1.2e-4 in y).
    auto d65 = cie_daylight_xy(6504.0);
    ASSERT_TRUE(d65);
    EXPECT_NEAR(d65->x, 0.3127, 5e-4);
    EXPECT_NEAR(d65->y, 0.3290, 5e-4);
    // D50 is daylight at 5003 K.
    auto d50 = cie_daylight_xy(5003.0);
    ASSERT_TRUE(d50);
    EXPECT_NEAR(d50->x, 0.3457, 5e-4);
    EXPECT_NEAR(d50->y, 0.3585, 5e-4);
}

TEST(ColorTemperature, CieDaylightRangeAndContinuity) {
    EXPECT_TRUE(cie_daylight_xy(4000.0));
    EXPECT_TRUE(cie_daylight_xy(25000.0));
    for (double bad : {3999.0, 25001.0, kNaN, std::numeric_limits<double>::infinity(), -6500.0}) {
        auto xy = cie_daylight_xy(bad);
        ASSERT_FALSE(xy) << bad;
        EXPECT_EQ(xy.error().code, ErrorCode::InvalidArgument);
    }
    // The two CIE polynomials meet at 7000 K to within 1e-5 (observed 4.6e-7).
    auto below = cie_daylight_xy(7000.0);
    auto above = cie_daylight_xy(7000.0 + 1e-6);
    EXPECT_NEAR(below->x, above->x, 1e-5);
    EXPECT_NEAR(below->y, above->y, 1e-5);
}

TEST(ColorTemperature, PlanckianMatchesIlluminantA) {
    // CIE illuminant A is a Planckian radiator at 2856 K, xy (0.44757, 0.40745) (CIE 15:2004).
    // Kim et al. quote an approximation error of about 1e-3; observed 5e-4 here.
    auto a = planckian_xy(2856.0);
    ASSERT_TRUE(a);
    EXPECT_NEAR(a->x, 0.44757, 1e-3);
    EXPECT_NEAR(a->y, 0.40745, 1e-3);
}

TEST(ColorTemperature, PlanckianRangeAndSegmentJoins) {
    EXPECT_TRUE(planckian_xy(1667.0));
    EXPECT_TRUE(planckian_xy(25000.0));
    for (double bad : {1666.0, 25001.0, kNaN, 0.0}) {
        auto xy = planckian_xy(bad);
        ASSERT_FALSE(xy) << bad;
        EXPECT_EQ(xy.error().code, ErrorCode::InvalidArgument);
    }
    // The published segments join within the approximation's accuracy (observed <= 7e-5).
    for (double join : {2222.0, 4000.0}) {
        auto below = planckian_xy(join);
        auto above = planckian_xy(join + 1e-6);
        EXPECT_NEAR(below->x, above->x, 1e-3) << join;
        EXPECT_NEAR(below->y, above->y, 1e-3) << join;
    }
    // Lower temperatures are redder: x decreases monotonically with temperature.
    double previous = 1.0;
    for (double t = 1667.0; t <= 25000.0; t += 250.0) {
        const double x = planckian_xy(t)->x;
        EXPECT_LT(x, previous) << t;
        previous = x;
    }
}

// ---------------------------------------------------------------------------------------------
// apply_matrix

void fill(ImageView<float> view) {
    for (std::int32_t y = 0; y < view.height(); ++y) {
        for (std::int32_t x = 0; x < view.width(); ++x) {
            for (std::int32_t c = 0; c < view.channels(); ++c) {
                // Scene-linear style data: negatives and values above 1 included.
                view.at(x, y, c) = static_cast<float>(x - 2) * 0.37f + static_cast<float>(y) * 0.61f +
                                   static_cast<float>(c) * 0.9f - 0.5f;
            }
        }
    }
}

TEST(ApplyMatrix, TransformsEveryPixelLikeDoubleMath) {
    const Matrix3d m =
        *conversion_matrix(ColorEncoding::linear_srgb(), ColorEncoding::scene_linear_rec2020_d65());
    auto buffer = ImageBuffer<float>::create(7, 5, 3);
    ASSERT_TRUE(buffer);
    fill(buffer->view());
    auto original = buffer->clone();
    ASSERT_TRUE(original);
    ASSERT_TRUE(apply_matrix(buffer->view(), m));
    for (std::int32_t y = 0; y < 5; ++y) {
        for (std::int32_t x = 0; x < 7; ++x) {
            const Vec3d in{original->at(x, y, 0), original->at(x, y, 1), original->at(x, y, 2)};
            const Vec3d expected = m * in;
            for (std::int32_t c = 0; c < 3; ++c) {
                // float coefficients and arithmetic: relative error of a few 1e-7.
                EXPECT_NEAR(buffer->at(x, y, c), expected[static_cast<std::size_t>(c)],
                            2e-6 * std::max(1.0, std::abs(expected[static_cast<std::size_t>(c)])));
            }
        }
    }
}

TEST(ApplyMatrix, LeavesAlphaAndPixelsOutsideTheViewUntouched) {
    auto buffer = ImageBuffer<float>::create(6, 4, 4);
    ASSERT_TRUE(buffer);
    fill(buffer->view());
    auto original = buffer->clone();
    ASSERT_TRUE(original);
    auto sub = buffer->view(PixelRect{1, 1, 3, 2});
    ASSERT_TRUE(sub);
    const Matrix3d swap_rb{{0, 0, 1}, {0, 1, 0}, {1, 0, 0}};
    ASSERT_TRUE(apply_matrix(*sub, swap_rb));
    for (std::int32_t y = 0; y < 4; ++y) {
        for (std::int32_t x = 0; x < 6; ++x) {
            const bool inside = PixelRect{1, 1, 3, 2}.contains(Point{x, y});
            EXPECT_EQ(buffer->at(x, y, 0), original->at(x, y, inside ? 2 : 0));
            EXPECT_EQ(buffer->at(x, y, 1), original->at(x, y, 1));
            EXPECT_EQ(buffer->at(x, y, 2), original->at(x, y, inside ? 0 : 2));
            EXPECT_EQ(buffer->at(x, y, 3), original->at(x, y, 3));  // alpha
        }
    }
}

TEST(ApplyMatrix, IdentityIsBitExactAndResultsAreDeterministic) {
    auto a = ImageBuffer<float>::create(9, 3, 3);
    ASSERT_TRUE(a);
    fill(a->view());
    auto before = a->clone();
    ASSERT_TRUE(before);
    ASSERT_TRUE(apply_matrix(a->view(), Matrix3d::identity()));
    EXPECT_EQ(std::memcmp(a->data(), before->data(), a->byte_size()), 0);

    auto b = before->clone();
    ASSERT_TRUE(b);
    const Matrix3d m =
        *conversion_matrix(ColorEncoding::scene_linear_rec2020_d65(), ColorEncoding::linear_srgb());
    ASSERT_TRUE(apply_matrix(a->view(), m));
    ASSERT_TRUE(apply_matrix(b->view(), m));
    EXPECT_EQ(std::memcmp(a->data(), b->data(), a->byte_size()), 0);
}

TEST(ApplyMatrix, RejectsBadInput) {
    auto mono = ImageBuffer<float>::create(4, 4, 1);
    ASSERT_TRUE(mono);
    auto status = apply_matrix(mono->view(), Matrix3d::identity());
    ASSERT_FALSE(status);
    EXPECT_EQ(status.error().code, ErrorCode::InvalidArgument);
    EXPECT_FALSE(apply_matrix(ImageView<float>{}, Matrix3d::identity()));
    auto rgb = ImageBuffer<float>::create(4, 4, 3);
    ASSERT_TRUE(rgb);
    EXPECT_FALSE(apply_matrix(rgb->view(), Matrix3d{{1, 0, 0}, {0, kNaN, 0}, {0, 0, 1}}));
}

}  // namespace
}  // namespace lucent::image
