// Guided chroma denoising (chroma_denoise.hpp).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <new>
#include <vector>

#include <lucent/engine/chroma_denoise.hpp>
#include <lucent/engine/develop_operations.hpp>
#include <lucent/engine/guided_filter.hpp>

#include "parallel_rows.hpp"

namespace lucent::engine {

namespace {

constexpr const char* kWhere = "engine.develop.chroma_denoise";

constexpr double kLr = kRec2020Luminance[0];
constexpr double kLg = kRec2020Luminance[1];
constexpr double kLb = kRec2020Luminance[2];

// Halo rows above and below a band: 2 r for the guided filter's two box passes and 1 for the 3 x 3
// median that makes its guide.
std::int32_t halo_rows(std::int32_t height, std::int32_t radius) noexcept {
    return static_cast<std::int32_t>(std::min<std::int64_t>(2 * std::int64_t{radius} + 1, height));
}

// Output rows per band for an image of `height` rows and a halo of `halo` rows (the whole image
// when band_rows is 0 or the bands would cover it anyway).
std::int32_t band_height(std::int32_t height, std::int32_t halo, std::int32_t band_rows) noexcept {
    if (band_rows <= 0) {
        return height;
    }
    const std::int64_t rows = std::max<std::int64_t>(band_rows, 4 * static_cast<std::int64_t>(halo));
    return rows >= height ? height : static_cast<std::int32_t>(rows);
}

std::uint64_t plane_bytes(std::int32_t width, std::int32_t rows, std::int32_t channels) noexcept {
    return static_cast<std::uint64_t>(std::max(width, 0)) * static_cast<std::uint64_t>(std::max(rows, 0)) *
           static_cast<std::uint64_t>(channels) * sizeof(float);
}

// The brightness that normalises the colour differences: max(Y, kChromaScaleFraction x the largest
// channel magnitude, the floor). It is proportional to a colour's brightness, like Y, but does not
// collapse for saturated colours of low luminance (a pure blue has Y = 0.0593 B), so the relative
// colour differences stay bounded (|c| <= 2 / kChromaScaleFraction).
inline double chroma_scale(const float* s, double y, double floor_value) noexcept {
    const float m = std::max(std::max(std::fabs(s[0]), std::fabs(s[1])), std::fabs(s[2]));
    return std::max(std::max(y, kChromaScaleFraction * static_cast<double>(m)), floor_value);
}

inline void sort2(float& a, float& b) noexcept {
    const float lo = std::min(a, b);
    b = std::max(a, b);
    a = lo;
}

// Median of nine values: the 19 compare-exchanges of the classic optimal network (Paeth; Devillard,
// "Fast median search: an ANSI C implementation", 1998).
inline float median9(float p[9]) noexcept {
    sort2(p[1], p[2]);
    sort2(p[4], p[5]);
    sort2(p[7], p[8]);
    sort2(p[0], p[1]);
    sort2(p[3], p[4]);
    sort2(p[6], p[7]);
    sort2(p[1], p[2]);
    sort2(p[4], p[5]);
    sort2(p[7], p[8]);
    sort2(p[0], p[3]);
    sort2(p[5], p[8]);
    sort2(p[4], p[7]);
    sort2(p[3], p[6]);
    sort2(p[1], p[4]);
    sort2(p[2], p[5]);
    sort2(p[4], p[7]);
    sort2(p[4], p[2]);
    sort2(p[6], p[4]);
    sort2(p[4], p[2]);
    return p[4];
}

}  // namespace

std::int32_t chroma_denoise_radius(double radius_full, double scale) noexcept {
    // The window (2 r + 1 pixels) scales with the image: r' = ((2 r + 1) scale - 1) / 2.
    const double r = 0.5 * ((2.0 * radius_full + 1.0) * scale - 1.0);
    if (!std::isfinite(r) || r < 1.0) {
        return 1;
    }
    return static_cast<std::int32_t>(std::lround(std::min(r, 1e6)));
}

std::uint64_t chroma_denoise_working_bytes(image::Size size, std::int32_t radius, std::int32_t band_rows) noexcept {
    if (size.empty() || radius < 1) {
        return 0;
    }
    const std::int32_t halo = halo_rows(size.height, radius);
    const std::int32_t band = band_height(size.height, halo, band_rows);
    const std::int32_t region = static_cast<std::int32_t>(std::min<std::int64_t>(
        size.height, static_cast<std::int64_t>(band) + 2 * static_cast<std::int64_t>(halo)));
    // Log luminance (1), guide (1), the guided filter's joint planes (4 + 2), and the stash of
    // original rows for the next band's upper halo (3).
    const std::uint64_t planes = plane_bytes(size.width, region, 1 + 1 + 4 + 2);
    const std::uint64_t stash = band < size.height ? plane_bytes(size.width, halo, 3) : 0;
    return planes + stash;
}

Expected<ChromaDenoiseRecord> apply_chroma_denoise(ColorImage& image, const ChromaDenoiseParameters& p,
                                                   const image::AllocationLimits& limits,
                                                   const core::CancellationToken& cancel, bool parallel) {
    if (image.pixels.empty() || image.pixels.channels() != 3) {
        return fail(ErrorCode::InvalidArgument, "chroma denoising needs a non-empty 3-channel image", kWhere);
    }
    if (image.encoding != image::ColorEncoding::scene_linear_rec2020_d65()) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("chroma denoising works on scene-linear Rec.2020 / D65 (got {})",
                                image.encoding.to_string()),
                    kWhere);
    }
    if (!std::isfinite(p.strength) || p.strength < 0.0 || p.strength > 1.0 || p.radius < 1 ||
        !std::isfinite(p.white) || !(p.white > 0.0) || !std::isfinite(p.epsilon) || !(p.epsilon > 0.0) ||
        p.band_rows < 0) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("invalid chroma-denoise parameters (strength {}, radius {}, white {}, eps {}, band "
                                "rows {})",
                                p.strength, p.radius, p.white, p.epsilon, p.band_rows),
                    kWhere);
    }
    ChromaDenoiseRecord record;
    record.strength = p.strength;
    record.radius = p.radius;
    record.epsilon = p.epsilon;
    if (p.strength == 0.0) {
        return record;
    }
    const std::int32_t width = image.pixels.width();
    const std::int32_t height = image.pixels.height();
    const std::int32_t halo = halo_rows(height, p.radius);
    const std::int32_t band = band_height(height, halo, p.band_rows);
    const std::int32_t region_max = static_cast<std::int32_t>(
        std::min<std::int64_t>(height, static_cast<std::int64_t>(band) + 2 * static_cast<std::int64_t>(halo)));
    record.band_rows = band;
    record.halo_rows = halo;
    record.working_bytes = chroma_denoise_working_bytes(image.pixels.size(), p.radius, p.band_rows);

    const auto plane = [&](std::int32_t channels, const char* what) -> Expected<image::ImageBuffer<float>> {
        auto buffer = image::ImageBuffer<float>::create_uninitialized(width, region_max, channels, limits);
        if (!buffer) {
            return fail(buffer.error().code, std::format("chroma-denoise {}: {}", what, buffer.error().message),
                        kWhere);
        }
        return buffer;
    };
    // L: log luminance; G: the guide (median of L, minus the shift); P: (g, g^2, c_r, c_b), then their
    // box means, then (a_r, a_b, b_r, b_b) and their box means; Q: (g c_r, g c_b), then their box means.
    auto l_buffer = plane(1, "log-luminance plane");
    if (!l_buffer) {
        return Unexpected(std::move(l_buffer.error()));
    }
    auto g_buffer = plane(1, "guide plane");
    if (!g_buffer) {
        return Unexpected(std::move(g_buffer.error()));
    }
    auto p_buffer = plane(4, "guided-filter planes");
    if (!p_buffer) {
        return Unexpected(std::move(p_buffer.error()));
    }
    auto q_buffer = plane(2, "guided-filter planes");
    if (!q_buffer) {
        return Unexpected(std::move(q_buffer.error()));
    }
    image::ImageBuffer<float> stash;
    if (band < height) {
        auto buffer = image::ImageBuffer<float>::create_uninitialized(width, halo, 3, limits);
        if (!buffer) {
            return fail(buffer.error().code, "chroma-denoise halo rows: " + buffer.error().message, kWhere);
        }
        stash = std::move(*buffer);
    }

    const image::ImageView<float> rgb = image.pixels.view();
    const double floor_value = p.white * std::exp2(kChromaDenoiseFloorEv);
    // The guide is shifted by a constant near the middle of its range (log2 W - 6 EV) before its
    // squares are formed, so the variance keeps its precision (the result is mathematically the
    // same; a constant keeps bands and whole images alike).
    const float shift = static_cast<float>(std::log2(p.white) + 0.5 * kChromaDenoiseFloorEv);
    const double strength = p.strength;
    const double eps = p.epsilon;
    std::atomic<std::uint64_t> non_finite{0};

    for (std::int32_t y0 = 0; y0 < height; y0 += band) {
        const std::int32_t y1 = std::min(height, y0 + band);
        const std::int32_t r0 = std::max(0, y0 - halo);
        const std::int32_t r1 = std::min(height, y1 + halo);
        const std::int32_t rows = r1 - r0;
        const image::PixelRect area{0, 0, width, rows};
        auto lv = l_buffer->view(area);
        auto gv = g_buffer->view(area);
        auto pv = p_buffer->view(area);
        auto qv = q_buffer->view(area);
        if (!lv || !gv || !pv || !qv) {
            return fail(ErrorCode::Internal, "chroma-denoise band views", kWhere);
        }
        const image::ImageView<float> lp = *lv;
        const image::ImageView<float> gp = *gv;
        const image::ImageView<float> pp = *pv;
        const image::ImageView<float> qp = *qv;
        // Rows above y0 were already written: their original values are in the stash, which holds
        // rows [y0 - halo, y0) (r0 == y0 - halo whenever y0 > 0, because band >= halo).
        const auto source_row = [&](std::int32_t y) -> const float* {
            return y < y0 ? stash.row(y - r0) : rgb.row(y);
        };
        // Log luminance and the chroma planes.
        auto filled = detail::for_each_row(rows, parallel, cancel, kWhere, [&](std::int32_t k) {
            const float* s = source_row(r0 + k);
            float* li = lp.row(k);
            float* pi = pp.row(k);
            for (std::int32_t x = 0; x < width; ++x, s += 3, pi += 4) {
                const double y = kLr * s[0] + kLg * s[1] + kLb * s[2];
                const double inv = 1.0 / chroma_scale(s, y, floor_value);
                li[x] = std::log2(static_cast<float>(std::max(y, floor_value)));
                pi[2] = static_cast<float>((s[0] - y) * inv);
                pi[3] = static_cast<float>((s[2] - y) * inv);
            }
        });
        if (!filled) {
            return Unexpected(std::move(filled.error()));
        }
        // The guide g: the 3 x 3 median of the log luminance (edge rows and columns of the band region
        // repeated: exact at the image's edges, the inner region edges lie in the halo), shifted; and
        // the products the guided filter averages.
        auto guided = detail::for_each_row(rows, parallel, cancel, kWhere, [&](std::int32_t k) {
            const float* above = lp.row(std::max(0, k - 1));
            const float* row = lp.row(k);
            const float* below = lp.row(std::min(rows - 1, k + 1));
            float* gi = gp.row(k);
            float* pi = pp.row(k);
            float* qi = qp.row(k);
            for (std::int32_t x = 0; x < width; ++x, pi += 4, qi += 2) {
                const std::int32_t xl = std::max(0, x - 1);
                const std::int32_t xr = std::min(width - 1, x + 1);
                float v[9] = {above[xl], above[x], above[xr], row[xl], row[x], row[xr], below[xl], below[x], below[xr]};
                const float g = median9(v) - shift;
                gi[x] = g;
                pi[0] = g;
                pi[1] = g * g;
                qi[0] = g * pi[2];
                qi[1] = g * pi[3];
            }
        });
        if (!guided) {
            return Unexpected(std::move(guided.error()));
        }
        // He, Sun and Tang, Algorithm 1, the box means of each step computed jointly.
        for (const image::ImageView<float> v : {pp, qp}) {
            if (auto status = box_mean(v, p.radius, cancel, parallel); !status) {
                return Unexpected(Error{status.error().code, "chroma denoise: " + status.error().message,
                                        status.error().where, status.error().native_code});
            }
        }
        auto coefficients = detail::for_each_row(rows, parallel, cancel, kWhere, [&](std::int32_t k) {
            float* pi = pp.row(k);
            const float* qi = qp.row(k);
            for (std::int32_t x = 0; x < width; ++x, pi += 4, qi += 2) {
                const double mean_g = pi[0];
                const double var = static_cast<double>(pi[1]) - mean_g * mean_g;
                const double mean_r = pi[2];
                const double mean_b = pi[3];
                const double inv = 1.0 / (var + eps);
                const double a_r = (static_cast<double>(qi[0]) - mean_g * mean_r) * inv;
                const double a_b = (static_cast<double>(qi[1]) - mean_g * mean_b) * inv;
                pi[0] = static_cast<float>(a_r);
                pi[1] = static_cast<float>(a_b);
                pi[2] = static_cast<float>(mean_r - a_r * mean_g);
                pi[3] = static_cast<float>(mean_b - a_b * mean_g);
            }
        });
        if (!coefficients) {
            return Unexpected(std::move(coefficients.error()));
        }
        if (auto status = box_mean(pp, p.radius, cancel, parallel); !status) {
            return Unexpected(Error{status.error().code, "chroma denoise: " + status.error().message,
                                    status.error().where, status.error().native_code});
        }
        if (y1 < height) {
            // The next band's upper halo: rows [y1 - halo, y1), still original (written below).
            for (std::int32_t k = 0; k < halo; ++k) {
                std::memcpy(stash.row(k), rgb.row(y1 - halo + k), static_cast<std::size_t>(width) * 3 * sizeof(float));
            }
        }
        auto written = detail::for_each_row(y1 - y0, parallel, cancel, kWhere, [&](std::int32_t k) {
            const std::int32_t y = y0 + k;
            float* s = rgb.row(y);
            const float* ab = pp.row(y - r0);
            const float* gi = gp.row(y - r0);
            std::uint64_t bad = 0;
            for (std::int32_t x = 0; x < width; ++x, s += 3, ab += 4) {
                const double g = gi[x];
                const double q_r = static_cast<double>(ab[0]) * g + static_cast<double>(ab[2]);
                const double q_b = static_cast<double>(ab[1]) * g + static_cast<double>(ab[3]);
                const double lum = kLr * s[0] + kLg * s[1] + kLb * s[2];
                const double d = chroma_scale(s, lum, floor_value);
                const double inv = 1.0 / d;
                const double cr = (s[0] - lum) * inv;
                const double cb = (s[2] - lum) * inv;
                const auto rf = static_cast<float>(lum + (cr + strength * (q_r - cr)) * d);
                const auto bf = static_cast<float>(lum + (cb + strength * (q_b - cb)) * d);
                const auto gf = static_cast<float>((lum - kLr * rf - kLb * bf) / kLg);
                s[0] = rf;
                s[1] = gf;
                s[2] = bf;
                bad += static_cast<std::uint64_t>(!std::isfinite(rf) || !std::isfinite(gf) || !std::isfinite(bf));
            }
            if (bad != 0) {
                non_finite.fetch_add(bad, std::memory_order_relaxed);
            }
        });
        if (!written) {
            return Unexpected(std::move(written.error()));
        }
        ++record.bands;
    }
    if (non_finite.load() != 0) {
        return fail(ErrorCode::Internal, std::format("chroma denoising produced {} non-finite pixels", non_finite.load()),
                    kWhere);
    }
    return record;
}

}  // namespace lucent::engine
