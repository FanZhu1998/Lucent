#pragma once

// Colorimetry primitives: 3x3 matrices, chromaticities, RGB <-> XYZ derivation, chromatic
// adaptation, transfer functions and colour-temperature loci.
//
// Conventions:
// - Matrices are row-major and multiply column vectors: out = M * in (docs/development/
//   conventions.md). Composition reads right to left: (B * A) * v applies A first.
// - Colour math is done in double. Pixel loops convert matrices to float once per call.
// - XYZ is relative colorimetry scaled so the white point has Y = 1.
// - Transfer functions are extended to all real inputs: odd symmetry for negative values
//   (f(-x) = -f(x)) and the curve's own formula continued above 1.0, so scene-linear data that
//   is negative or brighter than diffuse white round-trips instead of being clipped. NaN
//   propagates. Clipping belongs to output encodings (NumericRange::UnitClamped), not here.
//
// Everything here is a pure function or an immutable value: thread-safe.

#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>

#include <lucent/core/error.hpp>
#include <lucent/image/color_encoding.hpp>
#include <lucent/image/pixel_buffer.hpp>

namespace lucent::image {

// ---------------------------------------------------------------------------------------------
// Vectors and matrices

struct Vec3d {
    std::array<double, 3> v{};

    constexpr Vec3d() noexcept = default;
    constexpr Vec3d(double a, double b, double c) noexcept : v{a, b, c} {}

    [[nodiscard]] constexpr double& operator[](std::size_t i) noexcept { return v[i]; }
    [[nodiscard]] constexpr double operator[](std::size_t i) const noexcept { return v[i]; }

    friend constexpr bool operator==(const Vec3d&, const Vec3d&) = default;
};

[[nodiscard]] constexpr Vec3d operator+(const Vec3d& a, const Vec3d& b) noexcept {
    return Vec3d{a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
[[nodiscard]] constexpr Vec3d operator-(const Vec3d& a, const Vec3d& b) noexcept {
    return Vec3d{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
[[nodiscard]] constexpr Vec3d operator*(double s, const Vec3d& a) noexcept {
    return Vec3d{s * a[0], s * a[1], s * a[2]};
}

// Row-major 3x3 matrix of doubles. Default-constructed matrices are zero.
class Matrix3d {
public:
    constexpr Matrix3d() noexcept = default;
    // Rows in order: Matrix3d{{a, b, c}, {d, e, f}, {g, h, i}}.
    constexpr Matrix3d(const Vec3d& row0, const Vec3d& row1, const Vec3d& row2) noexcept
        : rows_{row0, row1, row2} {}

    [[nodiscard]] static constexpr Matrix3d identity() noexcept {
        return Matrix3d{{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
    }
    [[nodiscard]] static constexpr Matrix3d diagonal(const Vec3d& d) noexcept {
        return Matrix3d{{d[0], 0.0, 0.0}, {0.0, d[1], 0.0}, {0.0, 0.0, d[2]}};
    }

    [[nodiscard]] constexpr double& operator()(std::size_t row, std::size_t col) noexcept {
        return rows_[row][col];
    }
    [[nodiscard]] constexpr double operator()(std::size_t row, std::size_t col) const noexcept {
        return rows_[row][col];
    }
    [[nodiscard]] constexpr const Vec3d& row(std::size_t r) const noexcept { return rows_[r]; }
    [[nodiscard]] constexpr Vec3d column(std::size_t c) const noexcept {
        return Vec3d{rows_[0][c], rows_[1][c], rows_[2][c]};
    }

    friend constexpr bool operator==(const Matrix3d&, const Matrix3d&) = default;

private:
    std::array<Vec3d, 3> rows_{};
};

// out = m * v
[[nodiscard]] constexpr Vec3d operator*(const Matrix3d& m, const Vec3d& v) noexcept {
    return Vec3d{m(0, 0) * v[0] + m(0, 1) * v[1] + m(0, 2) * v[2],
                 m(1, 0) * v[0] + m(1, 1) * v[1] + m(1, 2) * v[2],
                 m(2, 0) * v[0] + m(2, 1) * v[1] + m(2, 2) * v[2]};
}

// (a * b) * v == a * (b * v): b is applied first.
[[nodiscard]] constexpr Matrix3d operator*(const Matrix3d& a, const Matrix3d& b) noexcept {
    Matrix3d r;
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            r(i, j) = a(i, 0) * b(0, j) + a(i, 1) * b(1, j) + a(i, 2) * b(2, j);
        }
    }
    return r;
}

[[nodiscard]] constexpr Matrix3d transpose(const Matrix3d& m) noexcept {
    return Matrix3d{m.column(0), m.column(1), m.column(2)};
}

[[nodiscard]] constexpr double determinant(const Matrix3d& m) noexcept {
    return m(0, 0) * (m(1, 1) * m(2, 2) - m(1, 2) * m(2, 1)) -
           m(0, 1) * (m(1, 0) * m(2, 2) - m(1, 2) * m(2, 0)) +
           m(0, 2) * (m(1, 0) * m(2, 1) - m(1, 1) * m(2, 0));
}

// True when all nine entries are finite.
[[nodiscard]] bool is_finite(const Matrix3d& m) noexcept;

// Inverse by the adjugate. InvalidArgument when an entry is not finite or the matrix is
// singular or numerically close to it: |det| <= 1e-12 * |row0| * |row1| * |row2| (Hadamard's
// bound makes this a scale-independent test).
[[nodiscard]] Expected<Matrix3d> inverse(const Matrix3d& m);

// ---------------------------------------------------------------------------------------------
// Chromaticities, white points and primaries

// CIE standard illuminant white points, 4-decimal xy.
// D65: ITU-R BT.709-6 Table 1 item 1.4 and ITU-R BT.2020-2 Table 3 (also IEC 61966-2-1 sRGB).
inline constexpr Chromaticity kD65White{0.3127, 0.3290};
// D50: ISO 22028-2:2013 (ROMM RGB reference white); CIE 15:2004 tabulates
// (0.34567, 0.35851), rounded here to the 4 decimals used by ROMM RGB and ICC workflows.
inline constexpr Chromaticity kD50White{0.3457, 0.3585};

struct RgbPrimaries {
    Chromaticity red;
    Chromaticity green;
    Chromaticity blue;

    friend constexpr bool operator==(const RgbPrimaries&, const RgbPrimaries&) = default;
};

// ITU-R BT.709-6 Table 1 item 1.3; identical to IEC 61966-2-1 (sRGB). White D65.
inline constexpr RgbPrimaries kRec709Primaries{{0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}};
// ITU-R BT.2020-2 Table 3 item 3.3. White D65.
inline constexpr RgbPrimaries kRec2020Primaries{{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}};
// SMPTE RP 431-2 (DCI-P3) primaries; Display P3 pairs them with D65 (SMPTE EG 432-1).
inline constexpr RgbPrimaries kDisplayP3Primaries{{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}};
// ISO 22028-2:2013 ROMM RGB (ProPhoto RGB); defined with the D50 white.
inline constexpr RgbPrimaries kProPhotoRommPrimaries{{0.7347, 0.2653}, {0.1596, 0.8404}, {0.0366, 0.0001}};
// Adobe RGB (1998) Color Image Encoding, version 2005-05. White D65.
inline constexpr RgbPrimaries kAdobeRgb1998Primaries{{0.6400, 0.3300}, {0.2100, 0.7100}, {0.1500, 0.0600}};

// Chromaticities of an RGB primaries enumerator. InvalidArgument for CameraNative (needs a
// camera profile) and CieXyz (not an RGB space), or an unknown value.
[[nodiscard]] Expected<RgbPrimaries> rgb_primaries(ColorPrimaries primaries);

// White chromaticity of an encoding: D50/D65 constants above or custom_white. InvalidArgument
// for CameraNative primaries (no defined white) or an invalid encoding.
[[nodiscard]] Expected<Chromaticity> white_chromaticity(const ColorEncoding& encoding);

// XYZ with luminance Y of chromaticity xy: X = x Y / y, Z = (1 - x - y) Y / y.
// InvalidArgument when y <= 0 or an input is not finite.
[[nodiscard]] Expected<Vec3d> xy_to_XYZ(Chromaticity xy, double Y = 1.0);

// Chromaticity of an XYZ triple: x = X / (X + Y + Z), y = Y / (X + Y + Z).
// InvalidArgument when X + Y + Z == 0 or an input is not finite.
[[nodiscard]] Expected<Chromaticity> XYZ_to_xy(const Vec3d& XYZ);

// Normalised primary matrix (SMPTE RP 177-1993): maps linear RGB to XYZ so that RGB (1, 1, 1)
// yields the white with Y = 1. Columns of P are the primaries' XYZ (Y = 1); the scale factors
// S = P^-1 * W_XYZ; the result is P * diag(S). InvalidArgument for degenerate primaries or an
// invalid white.
[[nodiscard]] Expected<Matrix3d> rgb_to_xyz(const RgbPrimaries& primaries, Chromaticity white);

// Inverse of rgb_to_xyz.
[[nodiscard]] Expected<Matrix3d> xyz_to_rgb(const RgbPrimaries& primaries, Chromaticity white);

// Matrix from the encoding's linear values to XYZ relative to its own white (Y_white = 1):
// rgb_to_xyz for RGB primaries, identity for CieXyz. InvalidArgument for CameraNative or an
// invalid encoding.
[[nodiscard]] Expected<Matrix3d> to_xyz_matrix(const ColorEncoding& encoding);

// ---------------------------------------------------------------------------------------------
// Chromatic adaptation

// Values are persisted (recipes may record the method) and must never change.
enum class ChromaticAdaptationMethod : std::uint8_t {
    Bradford = 1,    // Lam (1985); Lindbloom's linearised Bradford
    Cat02 = 2,       // CIECAM02 (CIE 159:2004)
    VonKries = 3,    // Hunt-Pointer-Estevez cone space, normalised to D65
    XyzScaling = 4,  // identity cone space (scales X, Y, Z independently)
};

// Cone-response matrix M_A of a method (XYZ -> cone space). InvalidArgument for unknown values.
[[nodiscard]] Expected<Matrix3d> cone_response_matrix(ChromaticAdaptationMethod method);

// Von Kries-style adaptation of XYZ from a source to a destination white:
//     M = M_A^-1 * diag(M_A * dst_white / M_A * src_white) * M_A,
// so M * src_white == dst_white. Whites are XYZ (typically Y = 1). InvalidArgument when a white
// is not finite or has a non-positive cone response.
[[nodiscard]] Expected<Matrix3d> chromatic_adaptation(
    const Vec3d& src_white_XYZ, const Vec3d& dst_white_XYZ,
    ChromaticAdaptationMethod method = ChromaticAdaptationMethod::Bradford);

// Matrix converting linear values of `src` to linear values of `dst`:
//     M = XYZ->dst * adaptation(src white -> dst white) * src->XYZ,
// with the adaptation omitted (identity) when both whites have identical chromaticity. Only the
// colorimetric fields participate: decode the source transfer function before applying M and
// encode the destination afterwards; range and reference are the caller's concern.
// InvalidArgument when either encoding is invalid, uses CameraNative primaries (a camera needs
// a calibrated profile, not a nominal matrix), or has a Mono or CfaMosaic layout.
[[nodiscard]] Expected<Matrix3d> conversion_matrix(
    const ColorEncoding& src, const ColorEncoding& dst,
    ChromaticAdaptationMethod method = ChromaticAdaptationMethod::Bradford);

// ---------------------------------------------------------------------------------------------
// Transfer functions ("encode" = linear -> stored, "decode" = stored -> linear)

namespace detail {

template <std::floating_point T, class F>
[[nodiscard]] inline T odd_extension(T x, F positive_branch) noexcept {
    return x < T(0) ? -positive_branch(-x) : positive_branch(x);
}

template <std::floating_point T>
[[nodiscard]] inline T srgb_encode(T x) noexcept {
    return odd_extension(x, [](T v) {
        return v <= T(0.0031308) ? T(12.92) * v : T(1.055) * std::pow(v, T(1) / T(2.4)) - T(0.055);
    });
}

template <std::floating_point T>
[[nodiscard]] inline T srgb_decode(T x) noexcept {
    return odd_extension(
        x, [](T v) { return v <= T(0.04045) ? v / T(12.92) : std::pow((v + T(0.055)) / T(1.055), T(2.4)); });
}

template <std::floating_point T>
[[nodiscard]] inline T rec709_oetf(T x) noexcept {
    return odd_extension(
        x, [](T v) { return v < T(0.018) ? T(4.5) * v : T(1.099) * std::pow(v, T(0.45)) - T(0.099); });
}

template <std::floating_point T>
[[nodiscard]] inline T rec709_inverse_oetf(T x) noexcept {
    return odd_extension(x, [](T v) {
        return v < T(0.081) ? v / T(4.5) : std::pow((v + T(0.099)) / T(1.099), T(1) / T(0.45));
    });
}

template <std::floating_point T>
[[nodiscard]] inline T prophoto_encode(T x) noexcept {
    return odd_extension(x, [](T v) { return v < T(1.0 / 512.0) ? T(16) * v : std::pow(v, T(1) / T(1.8)); });
}

template <std::floating_point T>
[[nodiscard]] inline T prophoto_decode(T x) noexcept {
    return odd_extension(x, [](T v) { return v < T(16.0 / 512.0) ? v / T(16) : std::pow(v, T(1.8)); });
}

template <std::floating_point T>
[[nodiscard]] inline T gamma_encode(T x, T gamma) noexcept {
    return odd_extension(x, [gamma](T v) { return std::pow(v, T(1) / gamma); });
}

template <std::floating_point T>
[[nodiscard]] inline T gamma_decode(T x, T gamma) noexcept {
    return odd_extension(x, [gamma](T v) { return std::pow(v, gamma); });
}

}  // namespace detail

// sRGB, IEC 61966-2-1:1999: encode 12.92 L for L <= 0.0031308, else
// 1.055 L^(1/2.4) - 0.055; decode V / 12.92 for V <= 0.04045, else ((V + 0.055) / 1.055)^2.4.
// The standard's two thresholds are not exact images of each other (the power segment gives
// 0.0404499 at L = 0.0031308), so decode(encode(L)) for L in (0.0031308, 0.0031308073) is off by
// at most 2.4e-9 and encode(decode(V)) for V in (0.0404499, 0.04045] by at most 3e-8; elsewhere
// both round trips are exact to rounding.
[[nodiscard]] inline double srgb_encode(double linear) noexcept {
    return detail::srgb_encode(linear);
}
[[nodiscard]] inline float srgb_encode(float linear) noexcept {
    return detail::srgb_encode(linear);
}
[[nodiscard]] inline double srgb_decode(double encoded) noexcept {
    return detail::srgb_decode(encoded);
}
[[nodiscard]] inline float srgb_decode(float encoded) noexcept {
    return detail::srgb_decode(encoded);
}

// ITU-R BT.709-6 Table 1 item 1.2 OETF: 4.5 L for L < 0.018, else 1.099 L^0.45 - 0.099.
// With the standard's rounded constants the curve jumps from 0.081 to 0.0812479 at L = 0.018.
// The inverse uses 0.081 = 4.5 * 0.018 as its threshold, so decode(encode(L)) is exact to
// rounding for every L; encoded values in [0.081, 0.0812479) never come out of the OETF and
// decode (power branch) to L just below 0.018. This is the camera OETF, not the BT.1886 EOTF.
[[nodiscard]] inline double rec709_oetf(double linear) noexcept {
    return detail::rec709_oetf(linear);
}
[[nodiscard]] inline float rec709_oetf(float linear) noexcept {
    return detail::rec709_oetf(linear);
}
[[nodiscard]] inline double rec709_inverse_oetf(double encoded) noexcept {
    return detail::rec709_inverse_oetf(encoded);
}
[[nodiscard]] inline float rec709_inverse_oetf(float encoded) noexcept {
    return detail::rec709_inverse_oetf(encoded);
}

// ROMM RGB, ISO 22028-2:2013: encode 16 L for L < 1/512 (Et), else L^(1/1.8); decode V / 16
// for V < 1/32 (16 Et), else V^1.8. The segments meet: (1/512)^(1/1.8) = 1/32 (to within one
// rounding step in floating point), so the curve is continuous.
[[nodiscard]] inline double prophoto_encode(double linear) noexcept {
    return detail::prophoto_encode(linear);
}
[[nodiscard]] inline float prophoto_encode(float linear) noexcept {
    return detail::prophoto_encode(linear);
}
[[nodiscard]] inline double prophoto_decode(double encoded) noexcept {
    return detail::prophoto_decode(encoded);
}
[[nodiscard]] inline float prophoto_decode(float encoded) noexcept {
    return detail::prophoto_decode(encoded);
}

// Pure power law with decoding exponent `gamma` (> 0): encode L^(1/gamma), decode V^gamma.
[[nodiscard]] inline double gamma_encode(double linear, double gamma) noexcept {
    return detail::gamma_encode(linear, gamma);
}
[[nodiscard]] inline float gamma_encode(float linear, float gamma) noexcept {
    return detail::gamma_encode(linear, gamma);
}
[[nodiscard]] inline double gamma_decode(double encoded, double gamma) noexcept {
    return detail::gamma_decode(encoded, gamma);
}
[[nodiscard]] inline float gamma_decode(float encoded, float gamma) noexcept {
    return detail::gamma_decode(encoded, gamma);
}

// A validated transfer function selected at run time (for example from a ColorEncoding).
class TransferCurve {
public:
    // InvalidArgument for an unknown function, or for Gamma with an exponent that is not finite
    // or not in (0, 10]. `gamma` is ignored for other functions.
    [[nodiscard]] static Expected<TransferCurve> create(TransferFunction function, double gamma = 1.0);

    // The curve of encoding.transfer / encoding.gamma (the rest of the encoding is not checked).
    [[nodiscard]] static Expected<TransferCurve> from_encoding(const ColorEncoding& encoding) {
        return create(encoding.transfer, encoding.gamma);
    }

    [[nodiscard]] TransferFunction function() const noexcept { return function_; }
    [[nodiscard]] double gamma() const noexcept { return gamma_; }

    [[nodiscard]] double encode(double linear) const noexcept { return encode_impl(linear); }
    [[nodiscard]] float encode(float linear) const noexcept { return encode_impl(linear); }
    [[nodiscard]] double decode(double encoded) const noexcept { return decode_impl(encoded); }
    [[nodiscard]] float decode(float encoded) const noexcept { return decode_impl(encoded); }

private:
    TransferCurve(TransferFunction function, double gamma) noexcept : function_(function), gamma_(gamma) {}

    template <std::floating_point T>
    [[nodiscard]] T encode_impl(T x) const noexcept {
        switch (function_) {
            case TransferFunction::Srgb:
                return detail::srgb_encode(x);
            case TransferFunction::Gamma:
                return detail::gamma_encode(x, static_cast<T>(gamma_));
            case TransferFunction::ProPhotoRomm:
                return detail::prophoto_encode(x);
            case TransferFunction::Rec709:
                return detail::rec709_oetf(x);
            case TransferFunction::Linear:
                break;
        }
        return x;
    }

    template <std::floating_point T>
    [[nodiscard]] T decode_impl(T x) const noexcept {
        switch (function_) {
            case TransferFunction::Srgb:
                return detail::srgb_decode(x);
            case TransferFunction::Gamma:
                return detail::gamma_decode(x, static_cast<T>(gamma_));
            case TransferFunction::ProPhotoRomm:
                return detail::prophoto_decode(x);
            case TransferFunction::Rec709:
                return detail::rec709_inverse_oetf(x);
            case TransferFunction::Linear:
                break;
        }
        return x;
    }

    TransferFunction function_ = TransferFunction::Linear;
    double gamma_ = 1.0;
};

// ---------------------------------------------------------------------------------------------
// Colour temperature

// Chromaticity of CIE daylight at correlated colour temperature `kelvin`, CIE 15:2004
// daylight-locus formulae (x_D polynomial in 10^3/T split at 7000 K; y_D = -3 x_D^2 + 2.870 x_D
// - 0.275). Valid for 4000 K <= T <= 25000 K; InvalidArgument outside or for non-finite input.
// Note CIE D65 corresponds to T = 6504 K (6500 K with the 1968 revision of c2).
[[nodiscard]] Expected<Chromaticity> cie_daylight_xy(double kelvin);

// Chromaticity of a Planckian (black-body) radiator at `kelvin`, using the cubic spline
// approximation of Kim et al., "Design of advanced color temperature control system for HDTV
// applications", Journal of the Korean Physical Society 41(6), 2002, pp. 865-871. Valid for
// 1667 K <= T <= 25000 K (accuracy about 1e-3 in xy); InvalidArgument outside or for
// non-finite input. At the shared segment boundaries (2222 K, 4000 K) the lower segment is used.
[[nodiscard]] Expected<Chromaticity> planckian_xy(double kelvin);

// ---------------------------------------------------------------------------------------------
// Pixel operations

// In place, for every pixel: rgb = matrix * rgb, on interleaved float data with 3 channels
// (RGB) or 4 channels (RGBA; alpha is left unchanged). The matrix is converted to float once
// and every pixel uses the same fixed evaluation order, so results are deterministic.
// InvalidArgument for other channel counts, an empty view, or a non-finite matrix.
[[nodiscard]] Status apply_matrix(ImageView<float> rgb, const Matrix3d& matrix);

}  // namespace lucent::image
