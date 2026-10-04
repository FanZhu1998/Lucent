#pragma once

// Synthetic scenes for the analysis tests: 14-bit RGGB mosaics of a camera-RGB scene with
// deterministic sensor noise (a fixed hash, no random state), optional optical-black margins, ISO
// metadata and the Canon EOS R5 decoder matrix.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>

#include <lucent/image/cfa.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/raw/decoded_raw.hpp>

#include "develop_test_support.hpp"
#include "sensor_test_support.hpp"

namespace lucent::analysis::test_support {

// Camera R, G, B response (normalised, unbalanced: 1 = white level) at visible pixel (x, y).
using Scene = std::function<std::array<double, 3>(std::int32_t x, std::int32_t y)>;

inline constexpr double kBlack = 512.0;
inline constexpr double kWhite = 16383.0;
// As-shot multipliers of the synthetic camera: a neutral under the as-shot illuminant responds as
// (1/2, 1, 1/1.5) times its level.
inline constexpr std::array<double, 3> kAsShot{2.0, 1.0, 1.5};
// ISO 12232 places 18 % grey at 12.7 % of saturation: the green response of reflectance rho at a
// correct exposure is rho x kProperGain.
inline constexpr double kProperGain = 0.127 / 0.18;

struct SceneOptions {
    std::int32_t width = 960;
    std::int32_t height = 640;
    std::int32_t left_margin = 0;  // optical-black columns left of the visible area
    std::optional<float> iso = 100.0f;
    double read_sigma = 3.0e-4;  // normalised
    double full_well = 20000.0;  // photo-electrons at the white level (shot noise)
    std::array<double, 3> as_shot = kAsShot;
    image::Orientation orientation = image::Orientation::Normal;
};

// splitmix64: a deterministic pseudo-random stream without state.
inline std::uint64_t hash64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Approximately standard normal (sum of four uniforms, Irwin-Hall, scaled to unit variance).
inline double gaussian(std::uint64_t key) {
    double s = 0.0;
    for (std::uint64_t k = 0; k < 4; ++k) {
        s += static_cast<double>(hash64(key * 4 + k) >> 11) * 0x1.0p-53;
    }
    return (s - 2.0) * std::sqrt(3.0);
}

// The camera response of a neutral of green level `v` under the as-shot illuminant, times a cast
// (camera red and blue factors).
inline std::array<double, 3> neutral(double v, double cast_r = 1.0, double cast_b = 1.0) {
    return {v * cast_r / kAsShot[0], v, v * cast_b / kAsShot[2]};
}

inline raw::DecodedRaw make_scene(const Scene& scene, const SceneOptions& o = {}) {
    const std::int32_t raw_width = o.width + o.left_margin;
    const image::PixelRect visible{o.left_margin, 0, o.width, o.height};
    const image::CfaPattern cfa = image::CfaPattern::bayer(image::BayerLayout::Rggb);
    const double range = kWhite - kBlack;
    auto decoded = engine::test_support::make_mosaic<std::uint16_t>(
        raw_width, o.height, visible, cfa, engine::test_support::simple_levels(kBlack, kWhite),
        [&](std::int32_t x, std::int32_t y, std::int32_t) {
            const std::uint64_t key =
                static_cast<std::uint64_t>(y) * 1000003ull + static_cast<std::uint64_t>(x);
            double v = 0.0;
            double sigma = o.read_sigma;
            if (x >= o.left_margin) {
                const std::int32_t vx = x - o.left_margin;
                const auto color = cfa.color_at(y, vx);
                const std::size_t c = color == image::CfaColor::Red    ? 0
                                      : color == image::CfaColor::Blue ? 2
                                                                       : 1;
                v = scene(vx, y)[c];
                sigma = std::sqrt(o.read_sigma * o.read_sigma + std::max(0.0, v) / o.full_well);
            }
            const double sample = kBlack + (v + sigma * gaussian(key)) * range;
            return std::clamp(std::round(sample), 0.0, kWhite);
        });
    engine::test_support::set_decoder_matrix(decoded.metadata, engine::test_support::kR5CamXyz);
    engine::test_support::set_as_shot(decoded.metadata, o.as_shot);
    decoded.metadata.capture.iso = o.iso;
    decoded.metadata.geometry.orientation = o.orientation;
    decoded.metadata.camera.make = "Synthetic";
    decoded.metadata.camera.model = "Scene";
    return decoded;
}

// Smooth deterministic texture in [-1, 1].
inline double texture(std::int32_t x, std::int32_t y) {
    return 0.5 * std::sin(0.11 * x + 0.07 * y) + 0.3 * std::sin(0.031 * x - 0.053 * y) +
           0.2 * std::sin(0.37 * x + 0.29 * y);
}

// A test chart for a 960 x 640 frame: a neutral background gradient with a 6 x 4 grid of neutral and
// coloured patches, at `exposure` times a correct exposure, every response multiplied by the cast.
inline Scene chart_scene(double exposure, double cast_r = 1.0, double cast_b = 1.0) {
    return [=](std::int32_t x, std::int32_t y) -> std::array<double, 3> {
        constexpr std::array<double, 6> greys{0.04, 0.09, 0.18, 0.36, 0.60, 0.85};
        constexpr std::array<std::array<double, 3>, 6> colours{{
            {1.9, 0.55, 0.45},  // red
            {0.6, 1.10, 0.50},  // green
            {0.45, 0.70, 2.0},  // blue
            {1.6, 1.25, 0.35},  // yellow
            {1.4, 0.55, 1.4},   // magenta
            {0.5, 1.05, 1.6},   // cyan
        }};
        const std::int32_t px = (x - 120) / 120;
        const std::int32_t py = (y - 80) / 120;
        const bool inside =
            x >= 120 && y >= 80 && px < 6 && py < 4 && (x - 120) % 120 > 12 && (y - 80) % 120 > 12;
        double rho = 0.14 + 0.08 * x / 960.0 + 0.01 * texture(x, y);
        std::array<double, 3> colour{1.0, 1.0, 1.0};
        if (inside) {
            if (py % 2 == 0) {
                rho = greys[static_cast<std::size_t>(px)] * (1.0 + 0.02 * texture(x, y));
            } else {
                rho = 0.25 * (1.0 + 0.02 * texture(x, y));
                colour = colours[static_cast<std::size_t>(px)];
            }
        }
        const double v = rho * kProperGain * exposure;
        const auto n = neutral(v, cast_r, cast_b);
        return {n[0] * colour[0], n[1] * colour[1], n[2] * colour[2]};
    };
}

}  // namespace lucent::analysis::test_support
