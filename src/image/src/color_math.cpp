#include "lucent/image/color_math.hpp"

#include <cmath>
#include <format>
#include <initializer_list>

namespace lucent::image {

namespace {

// Relative singularity threshold for inverse(); see the header.
constexpr double kSingularTolerance = 1e-12;

double norm(const Vec3d& v) noexcept {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

bool is_finite(const Vec3d& v) noexcept {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

// Cone-response matrices (XYZ -> cone space).
// Bradford: Lam (1985) as linearised by Lindbloom ("Chromatic Adaptation", brucelindbloom.com)
// and recommended by the ICC (ICC.1:2010).
constexpr Matrix3d kBradford{{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}};
// CAT02: CIE 159:2004 (CIECAM02), M_CAT02.
constexpr Matrix3d kCat02{{0.7328, 0.4296, -0.1624}, {-0.7036, 1.6975, 0.0061}, {0.0030, 0.0136, 0.9834}};
// Von Kries: Hunt-Pointer-Estevez cone fundamentals normalised to D65 (Lindbloom,
// "Chromatic Adaptation").
constexpr Matrix3d kVonKries{{0.4002400, 0.7076000, -0.0808100},
                             {-0.2263000, 1.1653200, 0.0457000},
                             {0.0000000, 0.0000000, 0.9182200}};

}  // namespace

bool is_finite(const Matrix3d& m) noexcept {
    return is_finite(m.row(0)) && is_finite(m.row(1)) && is_finite(m.row(2));
}

Expected<Matrix3d> inverse(const Matrix3d& m) {
    constexpr const char* kWhere = "image.color.inverse";
    if (!is_finite(m)) {
        return fail(ErrorCode::InvalidArgument, "matrix has a non-finite entry", kWhere);
    }
    const double det = determinant(m);
    const double scale = norm(m.row(0)) * norm(m.row(1)) * norm(m.row(2));
    if (!(std::abs(det) > kSingularTolerance * scale)) {
        return fail(ErrorCode::InvalidArgument, std::format("matrix is singular (determinant {})", det),
                    kWhere);
    }
    const double inv_det = 1.0 / det;
    // Adjugate (transposed cofactors) divided by the determinant.
    return Matrix3d{
        {(m(1, 1) * m(2, 2) - m(1, 2) * m(2, 1)) * inv_det, (m(0, 2) * m(2, 1) - m(0, 1) * m(2, 2)) * inv_det,
         (m(0, 1) * m(1, 2) - m(0, 2) * m(1, 1)) * inv_det},
        {(m(1, 2) * m(2, 0) - m(1, 0) * m(2, 2)) * inv_det, (m(0, 0) * m(2, 2) - m(0, 2) * m(2, 0)) * inv_det,
         (m(0, 2) * m(1, 0) - m(0, 0) * m(1, 2)) * inv_det},
        {(m(1, 0) * m(2, 1) - m(1, 1) * m(2, 0)) * inv_det, (m(0, 1) * m(2, 0) - m(0, 0) * m(2, 1)) * inv_det,
         (m(0, 0) * m(1, 1) - m(0, 1) * m(1, 0)) * inv_det}};
}

Expected<RgbPrimaries> rgb_primaries(ColorPrimaries primaries) {
    switch (primaries) {
        case ColorPrimaries::Rec709Srgb:
            return kRec709Primaries;
        case ColorPrimaries::Rec2020:
            return kRec2020Primaries;
        case ColorPrimaries::DisplayP3:
            return kDisplayP3Primaries;
        case ColorPrimaries::ProPhotoRomm:
            return kProPhotoRommPrimaries;
        case ColorPrimaries::AdobeRgb1998:
            return kAdobeRgb1998Primaries;
        case ColorPrimaries::CameraNative:
            return fail(
                ErrorCode::InvalidArgument,
                "camera-native primaries have no nominal chromaticities; a camera profile is required",
                "image.color.primaries");
        case ColorPrimaries::CieXyz:
            return fail(ErrorCode::InvalidArgument, "CIE XYZ is not an RGB space", "image.color.primaries");
    }
    return fail(ErrorCode::InvalidArgument,
                std::format("unknown colour primaries {}", static_cast<int>(primaries)),
                "image.color.primaries");
}

Expected<Chromaticity> white_chromaticity(const ColorEncoding& encoding) {
    if (auto status = encoding.validate(); !status) {
        return Unexpected(std::move(status.error()));
    }
    if (encoding.primaries == ColorPrimaries::CameraNative) {
        return fail(ErrorCode::InvalidArgument, "camera-native encodings have no defined white point",
                    "image.color.white");
    }
    switch (encoding.white_point) {
        case WhitePoint::D50:
            return kD50White;
        case WhitePoint::D65:
            return kD65White;
        case WhitePoint::Custom:
            return encoding.custom_white;  // checked by validate()
    }
    return fail(ErrorCode::Internal, "validated encoding has an unknown white point", "image.color.white");
}

Expected<Vec3d> xy_to_XYZ(Chromaticity xy, double Y) {
    if (!std::isfinite(xy.x) || !std::isfinite(xy.y) || !std::isfinite(Y) || !(xy.y > 0.0)) {
        return fail(
            ErrorCode::InvalidArgument,
            std::format("chromaticity ({}, {}) with Y = {} cannot be converted to XYZ", xy.x, xy.y, Y),
            "image.color.xy_to_XYZ");
    }
    return Vec3d{xy.x * Y / xy.y, Y, (1.0 - xy.x - xy.y) * Y / xy.y};
}

Expected<Chromaticity> XYZ_to_xy(const Vec3d& XYZ) {
    const double sum = XYZ[0] + XYZ[1] + XYZ[2];
    if (!is_finite(XYZ) || sum == 0.0 || !std::isfinite(sum)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("XYZ ({}, {}, {}) has no chromaticity", XYZ[0], XYZ[1], XYZ[2]),
                    "image.color.XYZ_to_xy");
    }
    return Chromaticity{XYZ[0] / sum, XYZ[1] / sum};
}

Expected<Matrix3d> rgb_to_xyz(const RgbPrimaries& primaries, Chromaticity white) {
    constexpr const char* kWhere = "image.color.rgb_to_xyz";
    const auto red = xy_to_XYZ(primaries.red);
    const auto green = xy_to_XYZ(primaries.green);
    const auto blue = xy_to_XYZ(primaries.blue);
    const auto white_XYZ = xy_to_XYZ(white);
    if (!red || !green || !blue || !white_XYZ) {
        return fail(ErrorCode::InvalidArgument, "primary or white chromaticity is invalid", kWhere);
    }
    // Columns are the primaries' XYZ at unit luminance.
    const Matrix3d p = transpose(Matrix3d{*red, *green, *blue});
    const auto p_inverse = inverse(p);
    if (!p_inverse) {
        return fail(ErrorCode::InvalidArgument, "primaries are collinear", kWhere);
    }
    const Vec3d scale = *p_inverse * *white_XYZ;
    return p * Matrix3d::diagonal(scale);
}

Expected<Matrix3d> xyz_to_rgb(const RgbPrimaries& primaries, Chromaticity white) {
    auto forward = rgb_to_xyz(primaries, white);
    if (!forward) {
        return forward;
    }
    return inverse(*forward);
}

Expected<Matrix3d> to_xyz_matrix(const ColorEncoding& encoding) {
    auto white = white_chromaticity(encoding);  // validates the encoding, rejects CameraNative
    if (!white) {
        return Unexpected(std::move(white.error()));
    }
    if (encoding.primaries == ColorPrimaries::CieXyz) {
        return Matrix3d::identity();
    }
    auto primaries = rgb_primaries(encoding.primaries);
    if (!primaries) {
        return Unexpected(std::move(primaries.error()));
    }
    return rgb_to_xyz(*primaries, *white);
}

Expected<Matrix3d> cone_response_matrix(ChromaticAdaptationMethod method) {
    switch (method) {
        case ChromaticAdaptationMethod::Bradford:
            return kBradford;
        case ChromaticAdaptationMethod::Cat02:
            return kCat02;
        case ChromaticAdaptationMethod::VonKries:
            return kVonKries;
        case ChromaticAdaptationMethod::XyzScaling:
            return Matrix3d::identity();
    }
    return fail(ErrorCode::InvalidArgument,
                std::format("unknown chromatic adaptation method {}", static_cast<int>(method)),
                "image.color.adaptation");
}

Expected<Matrix3d> chromatic_adaptation(const Vec3d& src_white_XYZ, const Vec3d& dst_white_XYZ,
                                        ChromaticAdaptationMethod method) {
    constexpr const char* kWhere = "image.color.adaptation";
    auto cone = cone_response_matrix(method);
    if (!cone) {
        return cone;
    }
    if (!is_finite(src_white_XYZ) || !is_finite(dst_white_XYZ)) {
        return fail(ErrorCode::InvalidArgument, "white point XYZ is not finite", kWhere);
    }
    const Vec3d src_cone = *cone * src_white_XYZ;
    const Vec3d dst_cone = *cone * dst_white_XYZ;
    for (std::size_t i = 0; i < 3; ++i) {
        if (!(src_cone[i] > 0.0) || !(dst_cone[i] > 0.0)) {
            return fail(ErrorCode::InvalidArgument, "white point has a non-positive cone response", kWhere);
        }
    }
    if (src_white_XYZ == dst_white_XYZ) {
        return Matrix3d::identity();  // exact no-op rather than identity plus rounding noise
    }
    const auto cone_inverse = inverse(*cone);
    if (!cone_inverse) {
        return fail(ErrorCode::Internal, "cone response matrix is singular", kWhere);
    }
    const Vec3d gain{dst_cone[0] / src_cone[0], dst_cone[1] / src_cone[1], dst_cone[2] / src_cone[2]};
    return *cone_inverse * (Matrix3d::diagonal(gain) * *cone);
}

Expected<Matrix3d> conversion_matrix(const ColorEncoding& src, const ColorEncoding& dst,
                                     ChromaticAdaptationMethod method) {
    constexpr const char* kWhere = "image.color.conversion_matrix";
    for (const ColorEncoding* e : {&src, &dst}) {
        if (auto status = e->validate(); !status) {
            return Unexpected(std::move(status.error()));
        }
        if (e->primaries == ColorPrimaries::CameraNative) {
            return fail(ErrorCode::InvalidArgument,
                        "camera-native RGB is not convertible without a camera calibration", kWhere);
        }
        if (e->layout != ChannelLayout::Rgb && e->layout != ChannelLayout::Rgba) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("colour matrices need RGB or RGBA data (got {})", to_string(e->layout)),
                        kWhere);
        }
    }
    auto src_white = white_chromaticity(src);
    auto dst_white = white_chromaticity(dst);
    if (!src_white || !dst_white) {
        return Unexpected(std::move(src_white ? dst_white.error() : src_white.error()));
    }
    const bool same_white = *src_white == *dst_white;
    if (same_white && src.primaries == dst.primaries) {
        return Matrix3d::identity();  // identical colorimetry: exact no-op
    }

    auto src_to_xyz = to_xyz_matrix(src);
    if (!src_to_xyz) {
        return src_to_xyz;
    }
    auto dst_to_xyz = to_xyz_matrix(dst);
    if (!dst_to_xyz) {
        return dst_to_xyz;
    }
    auto xyz_to_dst = inverse(*dst_to_xyz);
    if (!xyz_to_dst) {
        return xyz_to_dst;
    }
    Matrix3d adaptation = Matrix3d::identity();
    if (!same_white) {
        auto src_white_XYZ = xy_to_XYZ(*src_white);
        auto dst_white_XYZ = xy_to_XYZ(*dst_white);
        if (!src_white_XYZ || !dst_white_XYZ) {
            return fail(ErrorCode::Internal, "validated white point has no XYZ", kWhere);
        }
        auto cat = chromatic_adaptation(*src_white_XYZ, *dst_white_XYZ, method);
        if (!cat) {
            return cat;
        }
        adaptation = *cat;
    }
    return *xyz_to_dst * (adaptation * *src_to_xyz);
}

Expected<TransferCurve> TransferCurve::create(TransferFunction function, double gamma) {
    constexpr const char* kWhere = "image.color.transfer_curve";
    switch (function) {
        case TransferFunction::Linear:
        case TransferFunction::Srgb:
        case TransferFunction::ProPhotoRomm:
        case TransferFunction::Rec709:
            return TransferCurve(function, 1.0);
        case TransferFunction::Gamma:
            if (!std::isfinite(gamma) || gamma <= 0.0 || gamma > 10.0) {
                return fail(ErrorCode::InvalidArgument,
                            std::format("gamma exponent {} is outside (0, 10]", gamma), kWhere);
            }
            return TransferCurve(function, gamma);
    }
    return fail(ErrorCode::InvalidArgument,
                std::format("unknown transfer function {}", static_cast<int>(function)), kWhere);
}

Expected<Chromaticity> cie_daylight_xy(double kelvin) {
    if (!std::isfinite(kelvin) || kelvin < 4000.0 || kelvin > 25000.0) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("CIE daylight is defined for 4000..25000 K (got {} K)", kelvin),
                    "image.color.cie_daylight_xy");
    }
    const double t = kelvin;
    const double t2 = t * t;
    const double t3 = t2 * t;
    // CIE 15:2004 daylight locus: x_D, with separate polynomials below and above 7000 K.
    const double x = t <= 7000.0 ? -4.6070e9 / t3 + 2.9678e6 / t2 + 0.09911e3 / t + 0.244063
                                 : -2.0064e9 / t3 + 1.9018e6 / t2 + 0.24748e3 / t + 0.237040;
    // CIE 15:2004 daylight locus: y_D from x_D.
    const double y = -3.000 * x * x + 2.870 * x - 0.275;
    return Chromaticity{x, y};
}

Expected<Chromaticity> planckian_xy(double kelvin) {
    if (!std::isfinite(kelvin) || kelvin < 1667.0 || kelvin > 25000.0) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the Planckian approximation is valid for 1667..25000 K (got {} K)", kelvin),
                    "image.color.planckian_xy");
    }
    const double t = kelvin;
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double x = t <= 4000.0 ? -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910
                                 : -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390;
    const double x2 = x * x;
    const double x3 = x2 * x;
    double y = 0.0;
    if (t <= 2222.0) {
        y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
    } else if (t <= 4000.0) {
        y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
    } else {
        y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
    }
    return Chromaticity{x, y};
}

Status apply_matrix(ImageView<float> rgb, const Matrix3d& matrix) {
    constexpr const char* kWhere = "image.color.apply_matrix";
    if (rgb.empty() || (rgb.channels() != 3 && rgb.channels() != 4)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("apply_matrix needs a non-empty 3- or 4-channel view (got {} channels)",
                                rgb.channels()),
                    kWhere);
    }
    if (!is_finite(matrix)) {
        return fail(ErrorCode::InvalidArgument, "colour matrix has a non-finite entry", kWhere);
    }
    const auto f = [&matrix](std::size_t r, std::size_t c) { return static_cast<float>(matrix(r, c)); };
    const float m00 = f(0, 0), m01 = f(0, 1), m02 = f(0, 2);
    const float m10 = f(1, 0), m11 = f(1, 1), m12 = f(1, 2);
    const float m20 = f(2, 0), m21 = f(2, 1), m22 = f(2, 2);
    const std::int32_t channels = rgb.channels();
    for (std::int32_t y = 0; y < rgb.height(); ++y) {
        float* p = rgb.row(y);
        for (std::int32_t x = 0; x < rgb.width(); ++x, p += channels) {
            const float r = p[0];
            const float g = p[1];
            const float b = p[2];
            p[0] = m00 * r + m01 * g + m02 * b;
            p[1] = m10 * r + m11 * g + m12 * b;
            p[2] = m20 * r + m21 * g + m22 * b;
        }
    }
    return ok();
}

}  // namespace lucent::image
