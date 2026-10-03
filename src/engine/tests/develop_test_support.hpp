#pragma once

// Helpers shared by the develop-pipeline tests: realistic calibrations, synthetic metadata and
// float Bayer mosaics generated from a camera-RGB scene, and image comparisons.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/image/cfa.hpp>
#include <lucent/image/color_math.hpp>
#include <lucent/image/pixel_buffer.hpp>
#include <lucent/raw/decoded_raw.hpp>

#include "sensor_test_support.hpp"

namespace lucent::engine::test_support {

// Canon EOS R5 XYZ(D65) -> camera matrix as LibRaw 0.22 reports it.
inline const image::Matrix3d kR5CamXyz{
    {0.9766, -0.2953, -0.1254}, {-0.4276, 1.2116, 0.2433}, {-0.0437, 0.1336, 0.5131}};
// Adobe-style dual calibration (Canon EOS 5D Mark III, DNG converter): illuminant A and D65.
inline const image::Matrix3d kCmA{
    {0.7234, -0.1413, -0.0600}, {-0.3631, 1.1150, 0.2850}, {-0.0382, 0.1335, 0.6437}};
inline const image::Matrix3d kCmD65{
    {0.6722, -0.0635, -0.0963}, {-0.4287, 1.2460, 0.2028}, {-0.0908, 0.2162, 0.5668}};
// CIE standard illuminant A chromaticity.
inline constexpr image::Chromaticity kIlluminantA{0.44757, 0.40745};

// Levels with an exact float normalisation: white - black = 16384.
inline constexpr double kDevBlack = 512.0;
inline constexpr double kDevWhite = 16896.0;

inline raw::DngMatrix to_dng_matrix(const image::Matrix3d& m) {
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

inline void set_decoder_matrix(raw::RawMetadata& m, const image::Matrix3d& cam_xyz) {
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            m.color.xyz_to_camera[i][j] = static_cast<float>(cam_xyz(i, j));
        }
    }
    m.color.xyz_to_camera[3] = m.color.xyz_to_camera[1];
}

// As-shot multipliers for camera R, G, B (Green2 = G), stored as LibRaw cam_mul.
inline void set_as_shot(raw::RawMetadata& m, const std::array<double, 3>& rgb) {
    m.color.as_shot_multipliers = {static_cast<float>(rgb[0]), static_cast<float>(rgb[1]),
                                   static_cast<float>(rgb[2]), static_cast<float>(rgb[1])};
}

inline void make_dual_illuminant_dng(raw::RawMetadata& m) {
    m.dng.emplace();
    m.dng->info.color_planes = 3;
    m.dng->raw_ifd.path = "ifd0.subifd0";
    m.dng->info.color.calibration_illuminant = {std::uint16_t{17}, std::uint16_t{21}, std::nullopt};
    m.dng->info.color.color_matrix = {to_dng_matrix(kCmA), to_dng_matrix(kCmD65), std::nullopt};
}

// Camera R, G, B response (normalised units, unbalanced) at visible pixel (x, y).
using SceneFn = std::function<std::array<double, 3>(std::int32_t x, std::int32_t y)>;

// A float RGGB mosaic of `scene` (no margins), levels kDevBlack / kDevWhite, R5 decoder matrix,
// as-shot multipliers `as_shot`. Values are not clipped by the generator (a response above 1
// stays above the white level), so tests control saturation exactly.
inline raw::DecodedRaw scene_mosaic(std::int32_t width, std::int32_t height, const SceneFn& scene,
                                    const std::array<double, 3>& as_shot = {2.0, 1.0, 1.5}) {
    const image::CfaPattern cfa = image::CfaPattern::bayer(image::BayerLayout::Rggb);
    auto decoded = make_mosaic<float>(
        width, height, image::PixelRect{0, 0, width, height}, cfa, simple_levels(kDevBlack, kDevWhite),
        [&](std::int32_t x, std::int32_t y, std::int32_t) {
            const auto color = cfa.color_at(y, x);
            const std::size_t c = color == image::CfaColor::Red ? 0 : color == image::CfaColor::Blue ? 2 : 1;
            return kDevBlack + scene(x, y)[c] * (kDevWhite - kDevBlack);
        });
    set_decoder_matrix(decoded.metadata, kR5CamXyz);
    set_as_shot(decoded.metadata, as_shot);
    return decoded;
}

// Camera response of a neutral surface of reflectance `rho` lit by a white whose camera neutral
// is `neutral` (scaled so that its largest channel is rho).
inline std::array<double, 3> neutral_response(const image::Vec3d& neutral, double rho) {
    const double m = std::max({neutral[0], neutral[1], neutral[2]});
    return {rho * neutral[0] / m, rho * neutral[1] / m, rho * neutral[2] / m};
}

inline double max_abs_difference(const image::ImageBuffer<float>& a, const image::ImageBuffer<float>& b) {
    EXPECT_EQ(a.size(), b.size());
    EXPECT_EQ(a.channels(), b.channels());
    double m = 0.0;
    for (std::int32_t y = 0; y < std::min(a.height(), b.height()); ++y) {
        for (std::int32_t x = 0; x < std::min(a.width(), b.width()); ++x) {
            for (std::int32_t c = 0; c < std::min(a.channels(), b.channels()); ++c) {
                m = std::max(m, static_cast<double>(std::abs(a.at(x, y, c) - b.at(x, y, c))));
            }
        }
    }
    return m;
}

inline bool bit_identical(const image::ImageBuffer<float>& a, const image::ImageBuffer<float>& b) {
    if (a.size() != b.size() || a.channels() != b.channels()) {
        return false;
    }
    for (std::int32_t y = 0; y < a.height(); ++y) {
        for (std::int32_t k = 0; k < a.width() * a.channels(); ++k) {
            if (std::bit_cast<std::uint32_t>(a.row(y)[k]) != std::bit_cast<std::uint32_t>(b.row(y)[k])) {
                return false;
            }
        }
    }
    return true;
}

inline bool all_finite(const image::ImageBuffer<float>& a) {
    for (std::int32_t y = 0; y < a.height(); ++y) {
        for (std::int32_t k = 0; k < a.width() * a.channels(); ++k) {
            if (!std::isfinite(a.row(y)[k])) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace lucent::engine::test_support
