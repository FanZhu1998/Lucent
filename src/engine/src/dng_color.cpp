// DNG camera-profile evaluation and correlated colour temperature (camera_color.hpp).
//
// The algorithm follows the DNG 1.7.1.0 specification, chapter 6, and mirrors the structure of
// the Adobe DNG SDK's dng_color_spec (constructor, FindXYZtoCamera, NeutralToXY, SetWhiteXY)
// and dng_temperature::Set_xy_coord, so that results can be compared with Adobe's reference
// implementation step by step. Deviations are stated where they occur.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <utility>

#include <lucent/engine/camera_color.hpp>
#include <lucent/image/color_math.hpp>

namespace lucent::engine {

namespace {

constexpr const char* kWhereDng = "engine.color.dng";

// One isotemperature line of Robertson's table: reciprocal temperature (mired), CIE 1960 (u, v)
// of the Planckian locus and the slope t of the isotemperature line.
struct RobertsonLine {
    double mired;
    double u;
    double v;
    double t;
};

// Wyszecki & Stiles, Color Science, 2nd ed., table 1(3.11). The 325-mired entry uses
// u = 0.24792 (the value in Robertson's paper and the DNG SDK), not the misprint 0.24702.
constexpr std::array<RobertsonLine, 31> kRobertson = {{
    {0.0, 0.18006, 0.26352, -0.24341},   {10.0, 0.18066, 0.26589, -0.25479},
    {20.0, 0.18133, 0.26846, -0.26876},  {30.0, 0.18208, 0.27119, -0.28539},
    {40.0, 0.18293, 0.27407, -0.30470},  {50.0, 0.18388, 0.27709, -0.32675},
    {60.0, 0.18494, 0.28021, -0.35156},  {70.0, 0.18611, 0.28342, -0.37915},
    {80.0, 0.18740, 0.28668, -0.40955},  {90.0, 0.18880, 0.28997, -0.44278},
    {100.0, 0.19032, 0.29326, -0.47888}, {125.0, 0.19462, 0.30141, -0.58204},
    {150.0, 0.19962, 0.30921, -0.70471}, {175.0, 0.20525, 0.31647, -0.84901},
    {200.0, 0.21142, 0.32312, -1.0182},  {225.0, 0.21807, 0.32909, -1.2168},
    {250.0, 0.22511, 0.33439, -1.4512},  {275.0, 0.23247, 0.33904, -1.7298},
    {300.0, 0.24010, 0.34308, -2.0637},  {325.0, 0.24792, 0.34655, -2.4681},
    {350.0, 0.25591, 0.34951, -2.9641},  {375.0, 0.26400, 0.35200, -3.5814},
    {400.0, 0.27218, 0.35407, -4.3633},  {425.0, 0.28039, 0.35577, -5.3762},
    {450.0, 0.28863, 0.35714, -6.7262},  {475.0, 0.29685, 0.35823, -8.5955},
    {500.0, 0.30505, 0.35907, -11.324},  {525.0, 0.31320, 0.35968, -15.628},
    {550.0, 0.32129, 0.36011, -23.325},  {575.0, 0.32931, 0.36038, -40.770},
    {600.0, 0.33724, 0.36051, -116.45},
}};

constexpr std::uint32_t kMaxNeutralPasses = 30;
constexpr double kNeutralTolerance = 1e-7;  // |dx| + |dy|, as the SDK
constexpr double kMinNeutral = 0.001;       // the SDK pins camera white entries to [0.001, 1]

bool finite(const image::Vec3d& v) noexcept {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

image::Matrix3d scaled(const image::Matrix3d& m, double s) noexcept {
    image::Matrix3d r;
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            r(i, j) = m(i, j) * s;
        }
    }
    return r;
}

image::Matrix3d blend(const image::Matrix3d& a, const image::Matrix3d& b, double g) noexcept {
    image::Matrix3d r;
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            r(i, j) = g * a(i, j) + (1.0 - g) * b(i, j);
        }
    }
    return r;
}

// XYZ (Y = 1) of a chromaticity; the caller guarantees y > 0.
image::Vec3d xy_to_xyz(image::Chromaticity xy) noexcept {
    return image::Vec3d{xy.x / xy.y, 1.0, (1.0 - xy.x - xy.y) / xy.y};
}

// dng_camera_profile::NormalizeForwardMatrix: scale each row so that camera (1,1,1) maps to the
// D50 white (PCS).
Expected<image::Matrix3d> normalize_forward_matrix(const image::Matrix3d& fm, std::size_t which) {
    const image::Vec3d d50 = xy_to_xyz(image::kD50White);
    image::Matrix3d r;
    for (std::size_t i = 0; i < 3; ++i) {
        const double sum = fm(i, 0) + fm(i, 1) + fm(i, 2);
        if (!std::isfinite(sum) || sum <= 0.0) {
            return fail(ErrorCode::Corrupt,
                        std::format("ForwardMatrix{} row {} sums to {}; it cannot map camera white to D50",
                                    which + 1, i, sum),
                        kWhereDng);
        }
        for (std::size_t j = 0; j < 3; ++j) {
            r(i, j) = fm(i, j) * d50[i] / sum;
        }
    }
    return r;
}

// The calibrations of dng_color_spec after its constructor: colour matrices with AB * CC
// folded in, normalised forward matrices, sorted by temperature.
struct Calibrations {
    std::array<double, 2> temperature{};
    std::array<image::Matrix3d, 2> color;  // AB * CC_i * CM_i
    std::array<std::optional<image::Matrix3d>, 2> forward;
    std::array<image::Matrix3d, 2> calibration;  // CC_i
    bool interpolated = false;
};

// FindXYZtoCamera at a given white.
struct AtWhite {
    image::Matrix3d xyz_to_camera;
    std::optional<image::Matrix3d> forward;
    image::Matrix3d calibration;
    double cct = 0.0;
    double weight = 1.0;
};

Expected<AtWhite> find_xyz_to_camera(const Calibrations& cal, image::Chromaticity white) {
    auto cct = correlated_color_temperature(white);
    if (!cct) {
        return fail(ErrorCode::Corrupt,
                    "white chromaticity has no colour temperature: " + cct.error().message, kWhereDng);
    }
    AtWhite r;
    r.cct = *cct;
    double g = 1.0;
    if (r.cct <= cal.temperature[0]) {
        g = 1.0;
    } else if (r.cct >= cal.temperature[1]) {
        g = 0.0;
    } else {
        const double inv_t = 1.0 / r.cct;
        g = (inv_t - 1.0 / cal.temperature[1]) / (1.0 / cal.temperature[0] - 1.0 / cal.temperature[1]);
    }
    r.weight = g;
    const auto pick = [g](const image::Matrix3d& a, const image::Matrix3d& b) {
        if (g >= 1.0) {
            return a;
        }
        if (g <= 0.0) {
            return b;
        }
        return blend(a, b, g);
    };
    r.xyz_to_camera = pick(cal.color[0], cal.color[1]);
    r.calibration = pick(cal.calibration[0], cal.calibration[1]);
    if (cal.forward[0] && cal.forward[1]) {
        r.forward = pick(*cal.forward[0], *cal.forward[1]);
    } else if (cal.forward[0]) {
        r.forward = cal.forward[0];
    } else if (cal.forward[1]) {
        r.forward = cal.forward[1];
    }
    return r;
}

Expected<image::Chromaticity> xyz_to_xy_checked(const image::Vec3d& xyz, const char* what) {
    const double sum = xyz[0] + xyz[1] + xyz[2];
    if (!finite(xyz) || !(sum > 0.0) || !(xyz[1] > 0.0)) {
        return fail(
            ErrorCode::Corrupt,
            std::format("the camera neutral maps to XYZ ({}, {}, {}) {}; the calibration is not usable",
                        xyz[0], xyz[1], xyz[2], what),
            kWhereDng);
    }
    return image::Chromaticity{xyz[0] / sum, xyz[1] / sum};
}

}  // namespace

Expected<double> correlated_color_temperature(image::Chromaticity xy) {
    constexpr const char* kWhere = "engine.color.cct";
    if (!std::isfinite(xy.x) || !std::isfinite(xy.y)) {
        return fail(ErrorCode::InvalidArgument, "chromaticity is not finite", kWhere);
    }
    const double den = 1.5 - xy.x + 6.0 * xy.y;
    if (!(den > 0.0)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("chromaticity ({}, {}) has no CIE 1960 (u, v) coordinates", xy.x, xy.y),
                    kWhere);
    }
    const double u = 2.0 * xy.x / den;
    const double v = 3.0 * xy.y / den;
    double last_dt = 0.0;
    for (std::size_t index = 1; index < kRobertson.size(); ++index) {
        // Unit vector along the isotemperature line.
        double du = 1.0;
        double dv = kRobertson[index].t;
        const double len = std::sqrt(1.0 + dv * dv);
        du /= len;
        dv /= len;
        // Signed distance of (u, v) from the line; <= 0 once the point lies below it.
        const double uu = u - kRobertson[index].u;
        const double vv = v - kRobertson[index].v;
        double dt = -uu * dv + vv * du;
        if (dt <= 0.0 || index == kRobertson.size() - 1) {
            if (dt > 0.0) {
                dt = 0.0;
            }
            dt = -dt;
            const double f = index == 1 ? 0.0 : dt / (last_dt + dt);
            return 1.0e6 / (kRobertson[index - 1].mired * f + kRobertson[index].mired * (1.0 - f));
        }
        last_dt = dt;
    }
    return fail(ErrorCode::Internal, "Robertson search did not terminate", kWhere);  // unreachable
}

double calibration_illuminant_temperature(std::uint16_t light_source) noexcept {
    switch (light_source) {
        case 17:  // standard light A
        case 3:   // tungsten
            return 2850.0;
        case 24:  // ISO studio tungsten
            return 3200.0;
        case 23:  // D50
            return 5000.0;
        case 20:  // D55
        case 1:   // daylight
        case 9:   // fine weather
        case 4:   // flash
        case 18:  // standard light B
            return 5500.0;
        case 21:  // D65
        case 19:  // standard light C
        case 10:  // cloudy weather
            return 6500.0;
        case 22:  // D75
        case 11:  // shade
            return 7500.0;
        case 12:  // daylight fluorescent (D 5700 - 7100 K)
            return (5700.0 + 7100.0) * 0.5;
        case 13:  // day white fluorescent (N 4600 - 5400 K)
            return (4600.0 + 5400.0) * 0.5;
        case 14:  // cool white fluorescent (W 3900 - 4500 K)
        case 2:   // fluorescent
            return (3900.0 + 4500.0) * 0.5;
        case 15:  // white fluorescent (WW 3200 - 3700 K)
            return (3200.0 + 3700.0) * 0.5;
        case 16:  // warm white fluorescent (L 2600 - 3250 K)
            return (2600.0 + 3250.0) * 0.5;
        default:
            return 0.0;
    }
}

Expected<DngCalibrationRecord> evaluate_dng_profile(const DngProfile& profile,
                                                    const image::Vec3d& camera_neutral) {
    if (!profile.color_matrix[0]) {
        return fail(ErrorCode::InvalidArgument, "a DNG profile needs ColorMatrix1", kWhereDng);
    }
    if (!finite(camera_neutral) || !(camera_neutral[0] > 0.0) || !(camera_neutral[1] > 0.0) ||
        !(camera_neutral[2] > 0.0)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("camera neutral ({}, {}, {}) must be finite and positive", camera_neutral[0],
                                camera_neutral[1], camera_neutral[2]),
                    kWhereDng);
    }
    if (!finite(profile.analog_balance) || !(profile.analog_balance[0] > 0.0) ||
        !(profile.analog_balance[1] > 0.0) || !(profile.analog_balance[2] > 0.0)) {
        return fail(ErrorCode::Corrupt, "AnalogBalance must be finite and positive", kWhereDng);
    }
    for (std::size_t i = 0; i < 2; ++i) {
        for (const auto* m :
             {&profile.color_matrix[i], &profile.camera_calibration[i], &profile.forward_matrix[i]}) {
            if (*m && !image::is_finite(**m)) {
                return fail(ErrorCode::Corrupt,
                            std::format("calibration {} has a non-finite matrix entry", i + 1), kWhereDng);
            }
        }
    }

    DngCalibrationRecord record;
    record.illuminants = profile.illuminants;
    record.camera_calibration =
        profile.camera_calibration[0].has_value() || profile.camera_calibration[1].has_value();
    record.analog_balance = profile.analog_balance[0] != 1.0 || profile.analog_balance[1] != 1.0 ||
                            profile.analog_balance[2] != 1.0;

    // ---- dng_color_spec constructor ---------------------------------------------------------
    const image::Matrix3d ab = image::Matrix3d::diagonal(profile.analog_balance);
    Calibrations cal;
    cal.temperature = {calibration_illuminant_temperature(profile.illuminants[0]),
                       calibration_illuminant_temperature(profile.illuminants[1])};
    for (std::size_t i = 0; i < 2; ++i) {
        cal.calibration[i] = profile.camera_calibration[i].value_or(image::Matrix3d::identity());
        if (profile.forward_matrix[i]) {
            auto fm = normalize_forward_matrix(*profile.forward_matrix[i], i);
            if (!fm) {
                return Unexpected(std::move(fm.error()));
            }
            cal.forward[i] = *fm;
        }
    }
    cal.color[0] = ab * cal.calibration[0] * *profile.color_matrix[0];
    if (!profile.color_matrix[1] || cal.temperature[0] <= 0.0 || cal.temperature[1] <= 0.0 ||
        cal.temperature[0] == cal.temperature[1]) {
        cal.temperature = {5000.0, 5000.0};
        cal.color[1] = cal.color[0];
        cal.forward[1] = cal.forward[0];
        cal.calibration[1] = cal.calibration[0];
        cal.interpolated = false;
    } else {
        cal.color[1] = ab * cal.calibration[1] * *profile.color_matrix[1];
        cal.interpolated = true;
    }
    if (cal.temperature[0] > cal.temperature[1]) {
        std::swap(cal.temperature[0], cal.temperature[1]);
        std::swap(cal.color[0], cal.color[1]);
        std::swap(cal.forward[0], cal.forward[1]);
        std::swap(cal.calibration[0], cal.calibration[1]);
    }
    record.temperatures = cal.temperature;
    record.interpolated = cal.interpolated;

    // ---- Camera white (SetWhiteXY pins it to [0.001, 1] after normalising to a maximum of 1) ----
    const double max_neutral = std::max({camera_neutral[0], camera_neutral[1], camera_neutral[2]});
    image::Vec3d neutral;
    for (std::size_t c = 0; c < 3; ++c) {
        neutral[c] = std::clamp(camera_neutral[c] / max_neutral, kMinNeutral, 1.0);
    }
    record.camera_neutral = neutral;

    // ---- NeutralToXY: iterate white -> matrix -> white ------------------------------------------
    image::Chromaticity last = image::kD50White;
    bool done = false;
    for (std::uint32_t pass = 0; pass < kMaxNeutralPasses && !done; ++pass) {
        auto at = find_xyz_to_camera(cal, last);
        if (!at) {
            return Unexpected(std::move(at.error()));
        }
        auto inverse = image::inverse(at->xyz_to_camera);
        if (!inverse) {
            return fail(ErrorCode::Corrupt,
                        std::format("the interpolated XYZ-to-camera matrix at {:.0f} K is singular", at->cct),
                        kWhereDng);
        }
        auto next = xyz_to_xy_checked(*inverse * neutral, "while searching the white");
        if (!next) {
            return Unexpected(std::move(next.error()));
        }
        record.iterations = pass + 1;
        if (std::abs(next->x - last.x) + std::abs(next->y - last.y) < kNeutralTolerance) {
            last = *next;
            done = true;
            break;
        }
        if (pass == kMaxNeutralPasses - 1) {
            // Most likely a two-value oscillation: average the last two estimates (SDK rule).
            next->x = (last.x + next->x) * 0.5;
            next->y = (last.y + next->y) * 0.5;
            record.converged = false;
        }
        last = *next;
    }
    record.white_xy = last;

    // ---- SetWhiteXY ------------------------------------------------------------------------------
    auto at = find_xyz_to_camera(cal, last);
    if (!at) {
        return Unexpected(std::move(at.error()));
    }
    record.white_cct_kelvin = at->cct;
    record.weight_matrix1 = at->weight;
    const image::Vec3d d50 = xy_to_xyz(image::kD50White);
    if (!(last.y > 0.0)) {
        return fail(ErrorCode::Corrupt, "the white chromaticity has y <= 0", kWhereDng);
    }
    if (at->forward) {
        // CameraToPCS = FM * inverse(diag(ReferenceNeutral)) * inverse(AB * CC).
        auto individual_to_reference = image::inverse(ab * at->calibration);
        if (!individual_to_reference) {
            return fail(ErrorCode::Corrupt, "AnalogBalance * CameraCalibration is singular", kWhereDng);
        }
        const image::Vec3d reference_neutral = *individual_to_reference * neutral;
        if (!finite(reference_neutral) || !(reference_neutral[0] > 0.0) || !(reference_neutral[1] > 0.0) ||
            !(reference_neutral[2] > 0.0)) {
            return fail(ErrorCode::Corrupt, "the reference neutral is not positive", kWhereDng);
        }
        const image::Matrix3d d = image::Matrix3d::diagonal(
            image::Vec3d{1.0 / reference_neutral[0], 1.0 / reference_neutral[1], 1.0 / reference_neutral[2]});
        record.camera_to_xyz_d50 = *at->forward * d * *individual_to_reference;
        record.forward_matrix = true;
    } else {
        // PCStoCamera = CM * MapWhiteMatrix(D50 -> white), scaled so that the PCS white just reaches
        // the first camera channel at 1; CameraToPCS is its inverse.
        auto map_white =
            image::chromatic_adaptation(d50, xy_to_xyz(last), image::ChromaticAdaptationMethod::Bradford);
        if (!map_white) {
            return fail(ErrorCode::Corrupt,
                        "cannot adapt D50 to the camera white: " + map_white.error().message, kWhereDng);
        }
        image::Matrix3d pcs_to_camera = at->xyz_to_camera * *map_white;
        const image::Vec3d white_camera = pcs_to_camera * d50;
        const double scale = std::max({white_camera[0], white_camera[1], white_camera[2]});
        if (!std::isfinite(scale) || !(scale > 0.0)) {
            return fail(ErrorCode::Corrupt,
                        "the calibration maps the D50 white to non-positive camera values", kWhereDng);
        }
        pcs_to_camera = scaled(pcs_to_camera, 1.0 / scale);
        auto camera_to_pcs = image::inverse(pcs_to_camera);
        if (!camera_to_pcs) {
            return fail(ErrorCode::Corrupt, "the PCS-to-camera matrix is singular", kWhereDng);
        }
        record.camera_to_xyz_d50 = *camera_to_pcs;
    }
    if (!image::is_finite(record.camera_to_xyz_d50)) {
        return fail(ErrorCode::Corrupt, "the camera-to-XYZ matrix is not finite", kWhereDng);
    }
    return record;
}

}  // namespace lucent::engine
