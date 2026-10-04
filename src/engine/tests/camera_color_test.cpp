// Unit tests for camera colour interpretation (camera_color.hpp): colour temperature, the DNG
// profile evaluation, calibration choice and the pixel stages. Synthetic data only.
//
// Reference values: Robertson CCTs of the CIE illuminants are the published ~6504 K (D65),
// ~5003 K (D50) and ~2856 K (A). Matrices are checked against independent computations written
// here in the DNG specification's own terms (chapter 6) and against dcraw's cam_xyz_coeff().

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/core/cancellation.hpp>
#include <lucent/engine/camera_color.hpp>
#include <lucent/image/color_math.hpp>

#include "sensor_test_support.hpp"

namespace lucent::engine {
namespace {

namespace ts = test_support;
using image::Chromaticity;
using image::Matrix3d;
using image::Vec3d;

// Canon EOS R5 XYZ(D65) -> camera matrix as LibRaw 0.22 reports it (a realistic camera).
const Matrix3d kCamXyz{{0.9766, -0.2953, -0.1254}, {-0.4276, 1.2116, 0.2433}, {-0.0437, 0.1336, 0.5131}};
// Adobe-style dual calibration (Canon EOS 5D Mark III, DNG converter): illuminant A and D65.
const Matrix3d kCmA{{0.7234, -0.1413, -0.0600}, {-0.3631, 1.1150, 0.2850}, {-0.0382, 0.1335, 0.6437}};
const Matrix3d kCmD65{{0.6722, -0.0635, -0.0963}, {-0.4287, 1.2460, 0.2028}, {-0.0908, 0.2162, 0.5668}};
const Matrix3d kFmA{{0.7868, 0.0092, 0.1683}, {0.2291, 0.8615, -0.0906}, {0.0027, -0.4752, 1.2976}};
const Matrix3d kFmD65{{0.7637, 0.0805, 0.1201}, {0.2649, 0.9179, -0.1828}, {0.0137, -0.2456, 1.0570}};

Vec3d xyz_of(Chromaticity xy) {
    return *image::xy_to_XYZ(xy);
}

Chromaticity xy_of(const Vec3d& xyz) {
    return *image::XYZ_to_xy(xyz);
}

void expect_vec_near(const Vec3d& a, const Vec3d& b, double tol) {
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_NEAR(a[i], b[i], tol) << "component " << i;
    }
}

void expect_matrix_near(const Matrix3d& a, const Matrix3d& b, double tol) {
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            EXPECT_NEAR(a(i, j), b(i, j), tol) << "(" << i << ", " << j << ")";
        }
    }
}

Matrix3d bradford(Chromaticity from, Chromaticity to) {
    return *image::chromatic_adaptation(xyz_of(from), xyz_of(to), image::ChromaticAdaptationMethod::Bradford);
}

raw::DngMatrix dng_matrix(const Matrix3d& m) {
    raw::DngMatrix d;
    d.rows = 3;
    d.cols = 3;
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            d.values.push_back(m(i, j));
        }
    }
    return d;
}

WhiteBalanceRecord wb_record(std::array<double, 4> multipliers,
                             WhiteBalanceSource source = WhiteBalanceSource::AsShot) {
    WhiteBalanceRecord r;
    r.applied.source = source;
    r.applied.multipliers = multipliers;
    return r;
}

raw::RawMetadata bayer_metadata() {
    raw::RawMetadata m;
    m.representation.kind = raw::RawRepresentation::Mosaic;
    m.representation.channel_colors = ts::kBayerChannels;
    return m;
}

void set_decoder_matrix(raw::RawMetadata& m, const Matrix3d& cam_xyz) {
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            m.color.xyz_to_camera[i][j] = static_cast<float>(cam_xyz(i, j));
        }
    }
    m.color.xyz_to_camera[3] = m.color.xyz_to_camera[1];
}

// ---------------------------------------------------------------------------------------------
// Colour temperature

TEST(CorrelatedColorTemperature, MatchesPublishedValuesForCieIlluminants) {
    EXPECT_NEAR(*correlated_color_temperature(image::kD65White), 6504.0, 4.0);
    EXPECT_NEAR(*correlated_color_temperature(image::kD50White), 5003.0, 4.0);
    EXPECT_NEAR(*correlated_color_temperature(Chromaticity{0.44757, 0.40745}), 2856.0, 4.0);
}

TEST(CorrelatedColorTemperature, InvertsThePlanckianLocus) {
    // planckian_xy() is Kim et al.'s spline (about 1e-3 in xy), so allow 1.5 %.
    for (const double kelvin : {2000.0, 2500.0, 3000.0, 4000.0, 5000.0, 6500.0, 8000.0, 12000.0}) {
        const double cct = *correlated_color_temperature(*image::planckian_xy(kelvin));
        EXPECT_NEAR(cct, kelvin, kelvin * 0.015) << kelvin;
    }
}

TEST(CorrelatedColorTemperature, IsMonotoneAlongTheDaylightLocus) {
    double previous = 0.0;
    for (double kelvin = 4000.0; kelvin <= 25000.0; kelvin += 500.0) {
        const double cct = *correlated_color_temperature(*image::cie_daylight_xy(kelvin));
        EXPECT_GT(cct, previous) << kelvin;
        previous = cct;
    }
}

TEST(CorrelatedColorTemperature, RejectsInvalidInput) {
    ts::expect_error(correlated_color_temperature(Chromaticity{std::nan(""), 0.3}),
                     ErrorCode::InvalidArgument, "engine.color.cct");
    ts::expect_error(correlated_color_temperature(Chromaticity{2.0, 0.0}), ErrorCode::InvalidArgument,
                     "engine.color.cct");
}

TEST(CalibrationIlluminantTemperature, FollowsTheDngSdkTable) {
    EXPECT_EQ(calibration_illuminant_temperature(17), 2850.0);
    EXPECT_EQ(calibration_illuminant_temperature(3), 2850.0);
    EXPECT_EQ(calibration_illuminant_temperature(24), 3200.0);
    EXPECT_EQ(calibration_illuminant_temperature(23), 5000.0);
    EXPECT_EQ(calibration_illuminant_temperature(20), 5500.0);
    EXPECT_EQ(calibration_illuminant_temperature(21), 6500.0);
    EXPECT_EQ(calibration_illuminant_temperature(22), 7500.0);
    EXPECT_EQ(calibration_illuminant_temperature(12), 6400.0);
    EXPECT_EQ(calibration_illuminant_temperature(14), 4200.0);
    EXPECT_EQ(calibration_illuminant_temperature(0), 0.0);
    EXPECT_EQ(calibration_illuminant_temperature(255), 0.0);
    EXPECT_EQ(calibration_illuminant_temperature(99), 0.0);
}

// ---------------------------------------------------------------------------------------------
// DNG profile evaluation

TEST(DngProfile, SingleColorMatrixAdaptsInXyz) {
    DngProfile profile;
    profile.illuminants = {21, 0};
    profile.color_matrix[0] = kCamXyz;
    const Chromaticity white{0.40, 0.39};
    const Vec3d neutral = kCamXyz * xyz_of(white);
    auto record = evaluate_dng_profile(profile, neutral);
    ASSERT_TRUE(record) << record.error().describe();
    EXPECT_FALSE(record->interpolated);
    EXPECT_FALSE(record->forward_matrix);
    EXPECT_NEAR(record->white_xy.x, white.x, 1e-7);
    EXPECT_NEAR(record->white_xy.y, white.y, 1e-7);
    // Independent: CameraToXYZ_D50 = Bradford(white -> D50) * inverse(ColorMatrix), scaled so the
    // normalised neutral maps to Y = 1 (DNG 1.7.1.0 chapter 6).
    const double max_n = std::max({neutral[0], neutral[1], neutral[2]});
    const Vec3d n{neutral[0] / max_n, neutral[1] / max_n, neutral[2] / max_n};
    Matrix3d expected = bradford(white, image::kD50White) * *image::inverse(kCamXyz);
    const double y = (expected * n)[1];
    expected =
        Matrix3d{(1.0 / y) * expected.row(0), (1.0 / y) * expected.row(1), (1.0 / y) * expected.row(2)};
    expect_matrix_near(record->camera_to_xyz_d50, expected, 1e-9);
    expect_vec_near(record->camera_to_xyz_d50 * n, xyz_of(image::kD50White), 1e-9);
}

TEST(DngProfile, DualCalibrationsInterpolateInInverseTemperature) {
    DngProfile profile;
    profile.illuminants = {17, 21};  // 2850 K and 6500 K
    profile.color_matrix = {kCmA, kCmD65};
    const Chromaticity white{0.38, 0.38};  // about 4100 K
    const double cct = *correlated_color_temperature(white);
    const double g = (1.0 / cct - 1.0 / 6500.0) / (1.0 / 2850.0 - 1.0 / 6500.0);
    Matrix3d cm;
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            cm(i, j) = g * kCmA(i, j) + (1.0 - g) * kCmD65(i, j);
        }
    }
    const Vec3d neutral = cm * xyz_of(white);
    auto record = evaluate_dng_profile(profile, neutral);
    ASSERT_TRUE(record) << record.error().describe();
    EXPECT_TRUE(record->interpolated);
    EXPECT_TRUE(record->converged);
    EXPECT_GT(record->iterations, 1u);
    EXPECT_NEAR(record->white_xy.x, white.x, 1e-6);
    EXPECT_NEAR(record->white_xy.y, white.y, 1e-6);
    EXPECT_NEAR(record->weight_matrix1, g, 1e-5);
    EXPECT_NEAR(record->white_cct_kelvin, cct, 0.5);
    EXPECT_EQ(record->temperatures[0], 2850.0);
    EXPECT_EQ(record->temperatures[1], 6500.0);
}

TEST(DngProfile, ClampsTheInterpolationWeightOutsideTheCalibratedRange) {
    DngProfile profile;
    profile.illuminants = {21, 17};  // stored out of order: sorted by temperature internally
    profile.color_matrix = {kCmD65, kCmA};
    // Warmer than illuminant A: calibration A alone.
    auto warm = evaluate_dng_profile(profile, kCmA * xyz_of(*image::planckian_xy(2200.0)));
    ASSERT_TRUE(warm);
    EXPECT_EQ(warm->weight_matrix1, 1.0);
    EXPECT_EQ(warm->temperatures[0], 2850.0);
    // Cooler than D65: calibration D65 alone.
    auto cool = evaluate_dng_profile(profile, kCmD65 * xyz_of(*image::cie_daylight_xy(9000.0)));
    ASSERT_TRUE(cool);
    EXPECT_EQ(cool->weight_matrix1, 0.0);
}

TEST(DngProfile, UnknownIlluminantUsesColorMatrix1Only) {
    DngProfile profile;
    profile.illuminants = {0, 21};
    profile.color_matrix = {kCmA, kCmD65};
    auto record = evaluate_dng_profile(profile, kCmA * xyz_of(image::kD65White));
    ASSERT_TRUE(record);
    EXPECT_FALSE(record->interpolated);
    EXPECT_EQ(record->temperatures[0], 5000.0);
    EXPECT_EQ(record->temperatures[1], 5000.0);
    EXPECT_NEAR(record->white_xy.x, image::kD65White.x, 1e-7);
}

TEST(DngProfile, ForwardMatrixPathMapsTheNeutralToD50) {
    DngProfile profile;
    profile.illuminants = {17, 21};
    profile.color_matrix = {kCmA, kCmD65};
    profile.forward_matrix = {kFmA, kFmD65};
    const Matrix3d cc = Matrix3d::diagonal(Vec3d{1.002, 1.0, 1.0062});
    profile.camera_calibration = {cc, cc};
    profile.analog_balance = Vec3d{1.399522, 1.0, 2.677346};
    const Vec3d neutral{0.704273, 0.820688, 1.0};
    auto record = evaluate_dng_profile(profile, neutral);
    ASSERT_TRUE(record) << record.error().describe();
    EXPECT_TRUE(record->forward_matrix);
    EXPECT_TRUE(record->camera_calibration);
    EXPECT_TRUE(record->analog_balance);
    expect_vec_near(record->camera_to_xyz_d50 * neutral, xyz_of(image::kD50White), 1e-9);
    // Independent: FM (rows scaled so camera (1,1,1) -> D50), interpolated with the weight the
    // evaluation reports, times inverse(diag(ReferenceNeutral)) * inverse(AB * CC).
    const double g = record->weight_matrix1;
    const Vec3d d50 = xyz_of(image::kD50White);
    auto normalize = [&](const Matrix3d& fm) {
        Matrix3d r;
        for (std::size_t i = 0; i < 3; ++i) {
            const double sum = fm(i, 0) + fm(i, 1) + fm(i, 2);
            for (std::size_t j = 0; j < 3; ++j) {
                r(i, j) = fm(i, j) * d50[i] / sum;
            }
        }
        return r;
    };
    const Matrix3d fa = normalize(kFmA);
    const Matrix3d fd = normalize(kFmD65);
    Matrix3d fm;
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            fm(i, j) = g * fa(i, j) + (1.0 - g) * fd(i, j);
        }
    }
    const Matrix3d to_reference = *image::inverse(Matrix3d::diagonal(profile.analog_balance) * cc);
    const Vec3d ref = to_reference * neutral;
    const Matrix3d expected =
        fm * Matrix3d::diagonal(Vec3d{1.0 / ref[0], 1.0 / ref[1], 1.0 / ref[2]}) * to_reference;
    expect_matrix_near(record->camera_to_xyz_d50, expected, 1e-12);
}

TEST(DngProfile, ColorMatrixAdaptsWhitesBeyondTheSConeLimitLikeTheSdk) {
    // 3000 K / tint +90 (DNG SDK units): x + y = 0.997, Bradford S-cone response below zero, which
    // an unguarded adaptation rejects.
    DngProfile profile;
    profile.illuminants = {21, 0};
    profile.color_matrix[0] = kCamXyz;
    const Chromaticity white{0.4878, 0.5091};
    const Matrix3d bradford_cone{{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}};
    ASSERT_LT((bradford_cone * xyz_of(white))[2], 0.0);
    const Vec3d neutral = kCamXyz * xyz_of(white);
    auto record = evaluate_dng_profile(profile, neutral);
    ASSERT_TRUE(record) << record.error().describe();
    EXPECT_NEAR(record->white_xy.x, white.x, 1e-7);
    EXPECT_NEAR(record->white_xy.y, white.y, 1e-7);
    // Independent: the SDK's MapWhiteMatrix (dng_color_spec.cpp) with its guards written out here:
    // cone responses below 0 count as 0, each cone scale is pinned to [0.1, 10].
    const Vec3d d50 = xyz_of(image::kD50White);
    const Vec3d w1 = bradford_cone * d50;
    const Vec3d w2 = bradford_cone * xyz_of(record->white_xy);
    Vec3d scale;
    for (std::size_t i = 0; i < 3; ++i) {
        scale[i] = std::clamp(std::max(w2[i], 0.0) / w1[i], 0.1, 10.0);
    }
    EXPECT_EQ(scale[2], 0.1);
    const Matrix3d map_white = *image::inverse(bradford_cone) * (Matrix3d::diagonal(scale) * bradford_cone);
    Matrix3d pcs_to_camera = kCamXyz * map_white;
    const Vec3d white_camera = pcs_to_camera * d50;
    const double s = std::max({white_camera[0], white_camera[1], white_camera[2]});
    pcs_to_camera = Matrix3d{(1.0 / s) * pcs_to_camera.row(0), (1.0 / s) * pcs_to_camera.row(1),
                             (1.0 / s) * pcs_to_camera.row(2)};
    expect_matrix_near(record->camera_to_xyz_d50, *image::inverse(pcs_to_camera), 1e-9);
    EXPECT_TRUE(image::is_finite(record->camera_to_xyz_d50));
}

TEST(DngProfile, PinsTinyNeutralEntriesLikeTheSdk) {
    DngProfile profile;
    profile.color_matrix[0] = kCamXyz;
    auto record = evaluate_dng_profile(profile, Vec3d{0.0009, 1.0, 1.0});
    ASSERT_TRUE(record) << record.error().describe();
    EXPECT_EQ(record->camera_neutral[0], 0.001);
    EXPECT_EQ(record->camera_neutral[1], 1.0);
}

TEST(DngProfile, RejectsInvalidProfilesAndNeutrals) {
    DngProfile none;
    ts::expect_error(evaluate_dng_profile(none, Vec3d{1, 1, 1}), ErrorCode::InvalidArgument,
                     "engine.color.dng");

    DngProfile profile;
    profile.color_matrix[0] = kCamXyz;
    ts::expect_error(evaluate_dng_profile(profile, Vec3d{1, 0, 1}), ErrorCode::InvalidArgument,
                     "engine.color.dng");
    ts::expect_error(evaluate_dng_profile(profile, Vec3d{1, std::nan(""), 1}), ErrorCode::InvalidArgument,
                     "engine.color.dng");

    DngProfile singular;
    singular.color_matrix[0] = Matrix3d{{1, 2, 3}, {2, 4, 6}, {0, 1, 0}};
    ts::expect_error(evaluate_dng_profile(singular, Vec3d{1, 1, 1}), ErrorCode::Corrupt, "engine.color.dng");

    DngProfile bad_forward = profile;
    bad_forward.forward_matrix[0] = Matrix3d{{-1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    ts::expect_error(evaluate_dng_profile(bad_forward, Vec3d{1, 1, 1}), ErrorCode::Corrupt,
                     "engine.color.dng");

    DngProfile bad_balance = profile;
    bad_balance.analog_balance = Vec3d{1, -1, 1};
    ts::expect_error(evaluate_dng_profile(bad_balance, Vec3d{1, 1, 1}), ErrorCode::Corrupt,
                     "engine.color.dng");

    DngProfile non_finite = profile;
    non_finite.camera_calibration[0] = Matrix3d{{std::nan(""), 0, 0}, {0, 1, 0}, {0, 0, 1}};
    ts::expect_error(evaluate_dng_profile(non_finite, Vec3d{1, 1, 1}), ErrorCode::Corrupt,
                     "engine.color.dng");

    // A matrix that maps every camera neutral to negative XYZ.
    DngProfile negative;
    negative.color_matrix[0] = Matrix3d{{-1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    ts::expect_error(evaluate_dng_profile(negative, Vec3d{1, 1, 1}), ErrorCode::Corrupt, "engine.color.dng");
}

// ---------------------------------------------------------------------------------------------
// Calibration choice

TEST(ResolveCameraColor, DecoderMatrixReproducesDcrawNormalisation) {
    raw::RawMetadata m = bayer_metadata();
    set_decoder_matrix(m, kCamXyz);
    auto record = resolve_camera_color(m, wb_record({2.0, 1.0, 1.5, 1.0}));
    ASSERT_TRUE(record) << record.error().describe();
    EXPECT_EQ(record->path, CalibrationPath::DecoderMatrix);
    EXPECT_EQ(record->xyz_white, image::WhitePoint::D65);
    EXPECT_EQ(record->neutral_scale, 1.0);
    const Vec3d d65 = xyz_of(image::kD65White);
    expect_vec_near(record->camera_to_xyz * Vec3d{1, 1, 1}, d65, 1e-6);

    // dcraw cam_xyz_coeff(): cam_rgb = cam_xyz * xyz_rgb, rows normalised to sum 1, rgb_cam its
    // inverse. It must equal XYZ->sRGB * camera_to_xyz (float matrix precision).
    const Matrix3d xyz_rgb = *image::rgb_to_xyz(image::kRec709Primaries, image::kD65White);
    Matrix3d cam_rgb = Matrix3d{};
    const Matrix3d product = kCamXyz * xyz_rgb;
    for (std::size_t i = 0; i < 3; ++i) {
        const double sum = product(i, 0) + product(i, 1) + product(i, 2);
        for (std::size_t j = 0; j < 3; ++j) {
            cam_rgb(i, j) = product(i, j) / sum;
        }
    }
    const Matrix3d rgb_cam = *image::inverse(cam_rgb);
    const Matrix3d ours =
        *image::xyz_to_rgb(image::kRec709Primaries, image::kD65White) * record->camera_to_xyz;
    expect_matrix_near(ours, rgb_cam, 2e-6);
}

TEST(ResolveCameraColor, NeutralScaleFollowsTheSmallestMultiplier) {
    raw::RawMetadata m = bayer_metadata();
    set_decoder_matrix(m, kCamXyz);
    auto record = resolve_camera_color(m, wb_record({1.2, 1.0, 0.8, 1.0}));
    ASSERT_TRUE(record);
    EXPECT_DOUBLE_EQ(record->neutral_scale, 1.25);
    expect_vec_near(record->camera_to_xyz * Vec3d{1, 1, 1}, 1.25 * xyz_of(image::kD65White), 1e-6);
}

TEST(ResolveCameraColor, FailsWithoutCalibration) {
    raw::RawMetadata m = bayer_metadata();
    ts::expect_error(resolve_camera_color(m, wb_record({2, 1, 1.5, 1})), ErrorCode::Unsupported,
                     "engine.color.calibration");
    // A DNG without ColorMatrix1 and without a decoder matrix.
    m.dng.emplace();
    ts::expect_error(resolve_camera_color(m, wb_record({2, 1, 1.5, 1})), ErrorCode::Unsupported,
                     "engine.color.calibration");
}

TEST(ResolveCameraColor, RejectsSingularOrInvalidDecoderMatrices) {
    raw::RawMetadata m = bayer_metadata();
    set_decoder_matrix(m, Matrix3d{{1, 1, 1}, {1, 1, 1}, {0.5, 0.2, 0.1}});
    ts::expect_error(resolve_camera_color(m, wb_record({2, 1, 1.5, 1})), ErrorCode::Corrupt,
                     "engine.color.calibration");
    set_decoder_matrix(m, Matrix3d{{-1, 0, 0}, {0, 1, 0}, {0, 0, 1}});
    ts::expect_error(resolve_camera_color(m, wb_record({2, 1, 1.5, 1})), ErrorCode::Corrupt,
                     "engine.color.calibration");
}

TEST(ResolveCameraColor, RejectsInvalidWhiteBalanceRecords) {
    raw::RawMetadata m = bayer_metadata();
    set_decoder_matrix(m, kCamXyz);
    ts::expect_error(resolve_camera_color(m, wb_record({0.0, 1, 1.5, 1})), ErrorCode::InvalidArgument,
                     "engine.color.calibration");
    raw::RawMetadata cmy = bayer_metadata();
    cmy.representation.channel_colors = {image::CfaColor::Cyan, image::CfaColor::Magenta,
                                         image::CfaColor::Yellow};
    ts::expect_error(resolve_camera_color(cmy, wb_record({1, 1, 1, 0})), ErrorCode::Unsupported,
                     "engine.color.calibration");
}

TEST(ResolveCameraColor, MonochromeIsNeutral) {
    raw::RawMetadata m;
    m.representation.kind = raw::RawRepresentation::Monochrome;
    m.representation.channel_colors = {image::CfaColor::White};
    auto record = resolve_camera_color(m, wb_record({1, 0, 0, 0}, WhiteBalanceSource::NotApplicable));
    ASSERT_TRUE(record);
    EXPECT_EQ(record->path, CalibrationPath::MonochromeNeutral);
    EXPECT_EQ(record->provenance, raw::Provenance::LucentRule);
    expect_vec_near(record->camera_to_xyz.column(0), xyz_of(image::kD65White), 1e-12);
    expect_vec_near(record->camera_to_xyz.column(1), Vec3d{0, 0, 0}, 0.0);
}

// A DNG with the dual calibration above; the neutral is the as-shot neutral of the multipliers.
raw::RawMetadata dng_metadata(const Matrix3d& cm1, const Matrix3d& cm2) {
    raw::RawMetadata m = bayer_metadata();
    m.dng.emplace();
    m.dng->info.color_planes = 3;
    m.dng->raw_ifd.path = "ifd0.subifd0";
    m.dng->info.color.calibration_illuminant = {std::uint16_t{17}, std::uint16_t{21}, std::nullopt};
    m.dng->info.color.color_matrix = {dng_matrix(cm1), dng_matrix(cm2), std::nullopt};
    m.dng->info.color.baseline_exposure = 0.5;
    return m;
}

TEST(ResolveCameraColor, DngPathMapsTheAppliedWhiteToD50) {
    raw::RawMetadata m = dng_metadata(kCmA, kCmD65);
    set_decoder_matrix(m, kCamXyz);  // present but not used: the DNG profile has priority
    const std::array<double, 4> multipliers{2.1, 1.0, 1.6, 1.0};
    auto record = resolve_camera_color(m, wb_record(multipliers));
    ASSERT_TRUE(record) << record.error().describe();
    EXPECT_EQ(record->path, CalibrationPath::DngColorMatrix);
    EXPECT_EQ(record->provenance, raw::Provenance::DngTag);
    EXPECT_EQ(record->xyz_white, image::WhitePoint::D50);
    ASSERT_TRUE(record->dng);
    EXPECT_TRUE(record->dng->interpolated);
    // White-balanced (1, 1, 1) is the calibration white with Y = neutral_scale (= 1 here).
    // The colour-matrix path maps the neutral to D50 to within the white search tolerance (1e-7 in xy).
    expect_vec_near(record->camera_to_xyz * Vec3d{1, 1, 1}, xyz_of(image::kD50White), 1e-7);
    // The white found is the chromaticity of the camera neutral 1 / multiplier.
    const Vec3d neutral{1.0 / 2.1, 1.0, 1.0 / 1.6};
    const Chromaticity white = record->dng->white_xy;
    // Check: interpolated CM at the found white maps XYZ(white) onto the neutral direction.
    const double g = record->dng->weight_matrix1;
    Matrix3d interp;
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            interp(i, j) = g * kCmA(i, j) + (1.0 - g) * kCmD65(i, j);
        }
    }
    const Vec3d camera_white = interp * xyz_of(white);
    for (std::size_t c = 0; c < 3; ++c) {
        EXPECT_NEAR(camera_white[c] / camera_white[1], neutral[c] / neutral[1], 1e-6) << c;
    }
    EXPECT_FALSE(record->notes.empty());
}

TEST(ResolveCameraColor, DngPlanesAreMatchedToCameraChannels) {
    // The same camera described with planes stored in B, G, R order.
    raw::RawMetadata rgb = dng_metadata(kCmA, kCmD65);
    raw::RawMetadata bgr = rgb;
    bgr.dng->raw_ifd.cfa_plane_color = {2, 1, 0};
    const auto swap_rows = [](const Matrix3d& cm) { return Matrix3d{cm.row(2), cm.row(1), cm.row(0)}; };
    bgr.dng->info.color.color_matrix = {dng_matrix(swap_rows(kCmA)), dng_matrix(swap_rows(kCmD65)),
                                        std::nullopt};
    auto a = resolve_camera_color(rgb, wb_record({2.1, 1.0, 1.6, 1.0}));
    auto b = resolve_camera_color(bgr, wb_record({2.1, 1.0, 1.6, 1.0}));
    ASSERT_TRUE(a && b);
    expect_matrix_near(a->camera_to_xyz, b->camera_to_xyz, 1e-12);

    raw::RawMetadata duplicate = rgb;
    duplicate.dng->raw_ifd.cfa_plane_color = {0, 1, 1};
    ts::expect_error(resolve_camera_color(duplicate, wb_record({2, 1, 1.5, 1})), ErrorCode::Unsupported,
                     "engine.color.calibration");
    raw::RawMetadata four = rgb;
    four.dng->info.color_planes = 4;
    ts::expect_error(resolve_camera_color(four, wb_record({2, 1, 1.5, 1})), ErrorCode::Unsupported,
                     "engine.color.calibration");
    raw::RawMetadata wrong_shape = rgb;
    wrong_shape.dng->info.color.color_matrix[0]->cols = 4;
    ts::expect_error(resolve_camera_color(wrong_shape, wb_record({2, 1, 1.5, 1})), ErrorCode::Corrupt,
                     "engine.color.calibration");
}

TEST(ResolveCameraColor, DngDataBalancedByTheDecoderUsesAsShotNeutral) {
    raw::RawMetadata m = dng_metadata(kCmA, kCmD65);
    m.prior_processing.push_back({raw::PriorProcessingKind::WhiteBalanceApplied, "test"});
    const auto unity = wb_record({1, 1, 1, 1}, WhiteBalanceSource::AlreadyApplied);
    ts::expect_error(resolve_camera_color(m, unity), ErrorCode::Unsupported, "engine.color.calibration");

    m.dng->info.color.as_shot_neutral = {1.0 / 2.1, 1.0, 1.0 / 1.6};
    auto balanced = resolve_camera_color(m, unity);
    ASSERT_TRUE(balanced) << balanced.error().describe();
    // Same white as balancing the native data with the multipliers of that neutral.
    raw::RawMetadata native = dng_metadata(kCmA, kCmD65);
    auto reference = resolve_camera_color(native, wb_record({2.1, 1.0, 1.6, 1.0}));
    ASSERT_TRUE(reference);
    EXPECT_NEAR(balanced->dng->white_xy.x, reference->dng->white_xy.x, 1e-12);
    expect_matrix_near(balanced->camera_to_xyz, reference->camera_to_xyz, 1e-12);
}

TEST(ResolveCameraColor, DngForwardMatrixPathIsChosenWhenPresent) {
    raw::RawMetadata m = dng_metadata(kCmA, kCmD65);
    m.dng->info.color.forward_matrix = {dng_matrix(kFmA), dng_matrix(kFmD65), std::nullopt};
    m.dng->info.color.analog_balance = {1.399522, 1.0, 2.677346};
    auto record = resolve_camera_color(m, wb_record({1.42, 1.22, 1.0, 1.22}));
    ASSERT_TRUE(record) << record.error().describe();
    EXPECT_EQ(record->path, CalibrationPath::DngForwardMatrix);
    EXPECT_DOUBLE_EQ(record->neutral_scale, 1.0);
    expect_vec_near(record->camera_to_xyz * Vec3d{1, 1, 1}, xyz_of(image::kD50White), 1e-9);
}

// ---------------------------------------------------------------------------------------------
// Pixel stages

DemosaicedImage rgb_image(std::int32_t w, std::int32_t h, const Vec3d& value) {
    DemosaicedImage d;
    d.pixels = *image::ImageBuffer<float>::create(w, h, 3);
    for (std::int32_t y = 0; y < h; ++y) {
        for (std::int32_t x = 0; x < w; ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                d.pixels.at(x, y, c) =
                    static_cast<float>(value[static_cast<std::size_t>(c)] * (1.0 + 0.1 * x + 0.01 * y));
            }
        }
    }
    d.encoding = image::ColorEncoding::camera_native_linear();
    return d;
}

TEST(ApplyCameraColor, TransformsRgbLikeImageApplyMatrix) {
    raw::RawMetadata m = bayer_metadata();
    set_decoder_matrix(m, kCamXyz);
    auto record = resolve_camera_color(m, wb_record({2.0, 1.0, 1.5, 1.0}));
    ASSERT_TRUE(record);
    DemosaicedImage input = rgb_image(7, 5, Vec3d{0.2, 0.5, 0.3});
    auto expected = input.pixels.clone();
    ASSERT_TRUE(expected);
    ASSERT_TRUE(image::apply_matrix(expected->view(), record->camera_to_xyz));
    auto xyz = apply_camera_color(std::move(input), *record);
    ASSERT_TRUE(xyz) << xyz.error().describe();
    EXPECT_EQ(xyz->encoding, xyz_encoding(image::WhitePoint::D65));
    for (std::int32_t y = 0; y < 5; ++y) {
        for (std::int32_t x = 0; x < 7; ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                EXPECT_EQ(xyz->pixels.at(x, y, c), expected->at(x, y, c));
            }
        }
    }
}

TEST(ApplyCameraColor, ExpandsMonochromeToNeutralXyz) {
    raw::RawMetadata m;
    m.representation.kind = raw::RawRepresentation::Monochrome;
    m.representation.channel_colors = {image::CfaColor::White};
    auto record = resolve_camera_color(m, wb_record({1, 0, 0, 0}, WhiteBalanceSource::NotApplicable));
    ASSERT_TRUE(record);
    DemosaicedImage mono;
    mono.pixels = *image::ImageBuffer<float>::create(3, 2, 1);
    mono.pixels.at(1, 1, 0) = 0.5f;
    mono.encoding = raw::camera_native_monochrome();
    auto xyz = apply_camera_color(std::move(mono), *record);
    ASSERT_TRUE(xyz) << xyz.error().describe();
    EXPECT_EQ(xyz->pixels.channels(), 3);
    const Vec3d d65 = xyz_of(image::kD65White);
    EXPECT_FLOAT_EQ(xyz->pixels.at(1, 1, 0), static_cast<float>(0.5 * d65[0]));
    EXPECT_FLOAT_EQ(xyz->pixels.at(1, 1, 1), 0.5f);
    EXPECT_FLOAT_EQ(xyz->pixels.at(1, 1, 2), static_cast<float>(0.5 * d65[2]));
    EXPECT_EQ(xyz->pixels.at(0, 0, 1), 0.0f);
}

TEST(ApplyCameraColor, RejectsMismatchedInputs) {
    raw::RawMetadata m = bayer_metadata();
    set_decoder_matrix(m, kCamXyz);
    auto rgb_record = resolve_camera_color(m, wb_record({2.0, 1.0, 1.5, 1.0}));
    ASSERT_TRUE(rgb_record);
    ts::expect_error(apply_camera_color(DemosaicedImage{}, *rgb_record), ErrorCode::InvalidArgument,
                     "engine.color.apply");
    DemosaicedImage mono;
    mono.pixels = *image::ImageBuffer<float>::create(2, 2, 1);
    mono.encoding = raw::camera_native_monochrome();
    ts::expect_error(apply_camera_color(std::move(mono), *rgb_record), ErrorCode::InvalidArgument,
                     "engine.color.apply");
    CameraColorRecord mono_record = *rgb_record;
    mono_record.path = CalibrationPath::MonochromeNeutral;
    ts::expect_error(apply_camera_color(rgb_image(2, 2, Vec3d{1, 1, 1}), mono_record),
                     ErrorCode::InvalidArgument, "engine.color.apply");
    DemosaicedImage wrong = rgb_image(2, 2, Vec3d{1, 1, 1});
    wrong.encoding = image::ColorEncoding::linear_srgb();
    ts::expect_error(apply_camera_color(std::move(wrong), *rgb_record), ErrorCode::InvalidArgument,
                     "engine.color.apply");
    CameraColorRecord nan_record = *rgb_record;
    nan_record.camera_to_xyz(0, 0) = std::nan("");
    ts::expect_error(apply_camera_color(rgb_image(2, 2, Vec3d{1, 1, 1}), nan_record),
                     ErrorCode::InvalidArgument, "engine.color.apply");
}

TEST(ApplyCameraColor, HonoursCancellation) {
    raw::RawMetadata m = bayer_metadata();
    set_decoder_matrix(m, kCamXyz);
    auto record = resolve_camera_color(m, wb_record({2.0, 1.0, 1.5, 1.0}));
    ASSERT_TRUE(record);
    core::CancellationSource source;
    source.cancel();
    ts::expect_error(apply_camera_color(rgb_image(4, 4, Vec3d{1, 1, 1}), *record, {}, source.token()),
                     ErrorCode::Cancelled, "engine.color.apply");
}

TEST(WorkingSpace, MapsBothCalibrationWhitesToRec2020Unity) {
    for (const image::WhitePoint white : {image::WhitePoint::D65, image::WhitePoint::D50}) {
        ColorImage img;
        img.pixels = *image::ImageBuffer<float>::create(2, 1, 3);
        const Vec3d w = xyz_of(white == image::WhitePoint::D65 ? image::kD65White : image::kD50White);
        for (std::int32_t c = 0; c < 3; ++c) {
            img.pixels.at(0, 0, c) = static_cast<float>(w[static_cast<std::size_t>(c)]);
        }
        img.encoding = xyz_encoding(white);
        auto matrix = convert_to_working_space(img);
        ASSERT_TRUE(matrix) << matrix.error().describe();
        EXPECT_EQ(img.encoding, image::ColorEncoding::scene_linear_rec2020_d65());
        for (std::int32_t c = 0; c < 3; ++c) {
            EXPECT_NEAR(img.pixels.at(0, 0, c), 1.0f, 2e-6f);
            EXPECT_EQ(img.pixels.at(1, 0, c), 0.0f);
        }
    }
}

TEST(WorkingSpace, RejectsNonSceneOrCameraData) {
    ColorImage display;
    display.pixels = *image::ImageBuffer<float>::create(2, 2, 3);
    display.encoding = image::ColorEncoding::srgb_display();
    ts::expect_error(convert_to_working_space(display), ErrorCode::InvalidArgument, "engine.color.working");
    ColorImage camera;
    camera.pixels = *image::ImageBuffer<float>::create(2, 2, 3);
    camera.encoding = image::ColorEncoding::camera_native_linear();
    ts::expect_error(convert_to_working_space(camera), ErrorCode::InvalidArgument, "engine.color.working");
    ColorImage mono;
    mono.pixels = *image::ImageBuffer<float>::create(2, 2, 1);
    mono.encoding = xyz_encoding(image::WhitePoint::D65);
    ts::expect_error(convert_to_working_space(mono), ErrorCode::InvalidArgument, "engine.color.working");
    ColorImage empty;
    ts::expect_error(convert_to_working_space(empty), ErrorCode::InvalidArgument, "engine.color.working");
}

TEST(WorkingSpace, IdentityForRec2020InputStillHonoursCancellation) {
    ColorImage img;
    img.pixels = *image::ImageBuffer<float>::create(2, 2, 3);
    img.encoding = image::ColorEncoding::scene_linear_rec2020_d65();
    core::CancellationSource source;
    source.cancel();
    ts::expect_error(convert_to_working_space(img, {}, source.token()), ErrorCode::Cancelled,
                     "engine.color.working");
    auto matrix = convert_to_working_space(img);
    ASSERT_TRUE(matrix);
    EXPECT_EQ(*matrix, Matrix3d::identity());
}

}  // namespace
}  // namespace lucent::engine
