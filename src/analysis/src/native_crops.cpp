#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <optional>

#include <lucent/analysis/native_crops.hpp>
#include <lucent/analysis/proposals.hpp>
#include <lucent/engine/develop_operations.hpp>
#include <lucent/engine/display_rendering.hpp>
#include <lucent/image/color_math.hpp>

namespace lucent::analysis {

namespace {

constexpr const char* kWhere = "analysis.native";

struct TileStats {
    double mean_log = 0.0;
    double gradient_energy = 0.0;
    double gradient_max = 0.0;
    double clipped = 0.0;
    double dark = 0.0;
    double near_clip = 0.0;
};

constexpr std::array<double, 3> kNearDistances{2, 3, 4};
constexpr std::array<double, 3> kFarDistances{16, 24, 32};
static_assert(kFarDistances.back() == kProfileReach);

// The lightness of NativeCrop::measured, row-major, read with coordinates clamped to it. Where the
// measured region meets the image border its border is the image's, so clamping here equals
// clamping to any larger render of the crop; elsewhere no read leaves the region.
struct Plane {
    std::vector<float> lightness;
    std::int32_t width = 0;
    std::int32_t height = 0;

    [[nodiscard]] double at(std::int32_t x, std::int32_t y) const noexcept {
        x = std::clamp(x, 0, width - 1);
        y = std::clamp(y, 0, height - 1);
        return static_cast<double>(
            lightness[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)]);
    }
};

// Bilinear sample at (ox + rx, oy + ry) for an integer origin (ox, oy) and a relative position
// (rx, ry). The integer and fractional parts come from the relative position alone, so a position
// relative to the evaluation region samples the same pixels with the same weights whatever the
// region's place in the plane. Positions beyond the plane take its border (the weight towards the
// outside is 0), as clamping the position would.
double bilinear(const Plane& p, std::int32_t ox, std::int32_t oy, double rx, double ry) {
    const double fx0 = std::floor(rx);
    const double fy0 = std::floor(ry);
    std::int64_t x0 = static_cast<std::int64_t>(ox) + static_cast<std::int64_t>(fx0);
    std::int64_t y0 = static_cast<std::int64_t>(oy) + static_cast<std::int64_t>(fy0);
    double fx = rx - fx0;
    double fy = ry - fy0;
    if (x0 < 0) {
        x0 = 0;
        fx = 0.0;
    } else if (x0 >= p.width - 1) {
        x0 = p.width - 1;
        fx = 0.0;
    }
    if (y0 < 0) {
        y0 = 0;
        fy = 0.0;
    } else if (y0 >= p.height - 1) {
        y0 = p.height - 1;
        fy = 0.0;
    }
    const auto x = static_cast<std::int32_t>(x0);
    const auto y = static_cast<std::int32_t>(y0);
    const std::int32_t x1 = std::min(p.width - 1, x + 1);
    const std::int32_t y1 = std::min(p.height - 1, y + 1);
    return (1.0 - fy) * ((1.0 - fx) * p.at(x, y) + fx * p.at(x1, y)) +
           fy * ((1.0 - fx) * p.at(x, y1) + fx * p.at(x1, y1));
}

// Mean lightness near (2-4 px) and far (16-32 px) from an edge on both sides of its normal; (ox, oy)
// is the evaluation region's origin in the plane.
struct SideMeans {
    double near_plus = 0.0;
    double far_plus = 0.0;
    double near_minus = 0.0;
    double far_minus = 0.0;
};

SideMeans side_means(const Plane& plane, std::int32_t ox, std::int32_t oy, const EdgeSample& e) {
    const auto mean = [&](const std::array<double, 3>& distances, double sign) {
        double s = 0.0;
        for (const double d : distances) {
            s += bilinear(plane, ox, oy, e.x + sign * d * e.nx, e.y + sign * d * e.ny);
        }
        return s / static_cast<double>(distances.size());
    };
    return SideMeans{mean(kNearDistances, 1.0), mean(kFarDistances, 1.0), mean(kNearDistances, -1.0),
                     mean(kFarDistances, -1.0)};
}

image::PixelRect measured_rect(const NativeCrop& crop) noexcept {
    if (!crop.measured.empty()) {
        return crop.measured;
    }
    const image::PixelRect e = crop.evaluation;
    return image::PixelRect{e.x - kNativeMeasureMargin, e.y - kNativeMeasureMargin,
                            e.width + 2 * kNativeMeasureMargin, e.height + 2 * kNativeMeasureMargin}
        .intersect(crop.rendered);
}

}  // namespace

std::string_view to_string(CropKind kind) noexcept {
    switch (kind) {
        case CropKind::Detail:
            return "detail";
        case CropKind::Shadows:
            return "shadows";
        case CropKind::Highlights:
            return "highlights";
        case CropKind::Edges:
            return "edges";
    }
    return "invalid";
}

std::int32_t native_crop_halo(image::Size full) noexcept {
    return 2 * engine::local_tone_radius(full, 1.0) + kProfileReach + 2;
}

std::vector<NativeCrop> select_native_crops(const engine::StageOneImage& analysis,
                                            const ImageContext& context, image::Size full,
                                            std::int32_t crop_size) {
    std::vector<NativeCrop> crops;
    const auto& px = analysis.pixels;
    if (px.empty() || px.size() != context.size || full.empty()) {
        return crops;
    }
    crop_size = std::min({crop_size, full.width, full.height});
    if (crop_size < 16) {
        return crops;
    }
    const double sx = static_cast<double>(px.width()) / static_cast<double>(full.width);
    const double sy = static_cast<double>(px.height()) / static_cast<double>(full.height);
    const std::int32_t tile =
        std::max<std::int32_t>(4, static_cast<std::int32_t>(std::lround(crop_size * sx)));
    const std::int32_t tiles_x = px.width() / tile;
    const std::int32_t tiles_y = px.height() / tile;
    if (tiles_x < 1 || tiles_y < 1) {
        return crops;
    }
    const double clip = as_shot_clip_level(analysis);
    const std::int32_t channels = px.channels();
    const std::int32_t width = px.width();
    const std::int32_t height = px.height();
    std::vector<float> log_plane(context.signal.size());
    for (std::size_t i = 0; i < log_plane.size(); ++i) {
        log_plane[i] = static_cast<float>(std::log2(std::max(static_cast<double>(context.signal[i]), 1e-6)));
    }
    const auto log_signal = [&](std::int32_t x, std::int32_t y) {
        return static_cast<double>(log_plane[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                             static_cast<std::size_t>(x)]);
    };
    std::vector<TileStats> stats(static_cast<std::size_t>(tiles_x) * static_cast<std::size_t>(tiles_y));
    for (std::int32_t ty = 0; ty < tiles_y; ++ty) {
        for (std::int32_t tx = 0; tx < tiles_x; ++tx) {
            TileStats& s = stats[static_cast<std::size_t>(ty) * static_cast<std::size_t>(tiles_x) +
                                 static_cast<std::size_t>(tx)];
            double count = 0.0;
            for (std::int32_t y = ty * tile; y < (ty + 1) * tile; ++y) {
                const float* row = px.row(y);
                for (std::int32_t x = tx * tile; x < (tx + 1) * tile; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                          static_cast<std::size_t>(x);
                    double m = row[static_cast<std::ptrdiff_t>(x) * channels];
                    for (std::int32_t c = 1; c < channels; ++c) {
                        m = std::max(m,
                                     static_cast<double>(row[static_cast<std::ptrdiff_t>(x) * channels + c]));
                    }
                    const double g = context.signal[i];
                    const double l = log_signal(x, y);
                    const double gx =
                        log_signal(std::min(width - 1, x + 1), y) - log_signal(std::max(0, x - 1), y);
                    const double gy =
                        log_signal(x, std::min(height - 1, y + 1)) - log_signal(x, std::max(0, y - 1));
                    const double grad = 0.5 * std::hypot(gx, gy);
                    const bool saturated = !context.saturation.empty() && context.saturation[i] > 0.0f;
                    const bool clipped = saturated || !(m < 0.98 * clip);
                    const double floor = std::max(
                        std::exp2(-12.0), context.noise.available() ? 4.0 * context.noise.sigma(g) : 0.0);
                    s.mean_log += l;
                    s.gradient_energy += grad;
                    s.gradient_max = std::max(s.gradient_max, grad);
                    s.clipped += clipped ? 1.0 : 0.0;
                    s.dark += g < floor ? 1.0 : 0.0;
                    s.near_clip += (!clipped && m >= clip / 8.0) ? 1.0 : 0.0;
                    count += 1.0;
                }
            }
            s.mean_log /= count;
            s.gradient_energy /= count;
            s.clipped /= count;
            s.dark /= count;
        }
    }

    std::vector<std::uint8_t> taken(stats.size(), 0);
    const auto choose = [&](CropKind kind, auto eligible, auto statistic, bool maximise) {
        std::size_t best = stats.size();
        double best_value = 0.0;
        for (std::size_t k = 0; k < stats.size(); ++k) {
            if (taken[k] != 0 || !eligible(stats[k])) {
                continue;
            }
            const double v = statistic(stats[k]);
            if (best == stats.size() || (maximise ? v > best_value : v < best_value)) {
                best = k;
                best_value = v;
            }
        }
        if (best == stats.size()) {
            return;
        }
        taken[best] = 1;
        const auto tx = static_cast<std::int32_t>(best % static_cast<std::size_t>(tiles_x));
        const auto ty = static_cast<std::int32_t>(best / static_cast<std::size_t>(tiles_x));
        const double cx = (tx + 0.5) * tile / sx;
        const double cy = (ty + 0.5) * tile / sy;
        NativeCrop crop;
        crop.kind = kind;
        crop.statistic = best_value;
        crop.evaluation.width = crop_size;
        crop.evaluation.height = crop_size;
        crop.evaluation.x = std::clamp(static_cast<std::int32_t>(std::lround(cx - crop_size / 2.0)), 0,
                                       full.width - crop_size);
        crop.evaluation.y = std::clamp(static_cast<std::int32_t>(std::lround(cy - crop_size / 2.0)), 0,
                                       full.height - crop_size);
        const std::int32_t halo = native_crop_halo(full);
        const image::PixelRect expanded{crop.evaluation.x - halo, crop.evaluation.y - halo,
                                        crop_size + 2 * halo, crop_size + 2 * halo};
        crop.rendered = expanded.intersect(image::PixelRect::from_size(full));
        const image::PixelRect margin{crop.evaluation.x - kNativeMeasureMargin,
                                      crop.evaluation.y - kNativeMeasureMargin,
                                      crop_size + 2 * kNativeMeasureMargin, crop_size + 2 * kNativeMeasureMargin};
        crop.measured = margin.intersect(crop.rendered);
        crops.push_back(crop);
    };
    choose(
        CropKind::Detail, [](const TileStats& s) { return s.clipped < 0.05 && s.dark < 0.2; },
        [](const TileStats& s) { return s.gradient_energy; }, true);
    choose(
        CropKind::Shadows, [](const TileStats& s) { return s.clipped < 0.01 && s.dark < 0.5; },
        [](const TileStats& s) { return s.mean_log; }, false);
    choose(
        CropKind::Highlights, [](const TileStats& s) { return s.clipped < 0.5 && s.near_clip > 0.0; },
        [](const TileStats& s) { return s.near_clip; }, true);
    choose(
        CropKind::Edges, [](const TileStats& s) { return s.clipped < 0.5; },
        [](const TileStats& s) { return s.gradient_max; }, true);
    return crops;
}

Expected<CropMeasurement> measure_crop(const engine::ColorImage& display, const NativeCrop& crop,
                                       const CropFeatures* baseline, CropFeatures* features) {
    return measure_crop(display, crop.rendered, crop, baseline, features);
}

Expected<CropMeasurement> measure_crop(const engine::ColorImage& display, const image::PixelRect& rendered,
                                       const NativeCrop& crop, const CropFeatures* baseline,
                                       CropFeatures* features) {
    const auto& px = display.pixels;
    if (px.empty() || px.channels() != 3 || display.encoding != engine::display_linear_rec2020() ||
        px.size() != rendered.size()) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("a crop render must be display-linear Rec.2020 of {}x{}", rendered.width,
                                rendered.height),
                    kWhere);
    }
    const image::PixelRect region = measured_rect(crop);
    if (region.empty() || !rendered.contains(region) || !region.contains(crop.evaluation)) {
        return fail(ErrorCode::InvalidArgument,
                    "the crop render does not cover the measured region around the evaluation region", kWhere);
    }
    auto srgb = image::conversion_matrix(image::ColorEncoding::scene_linear_rec2020_d65(),
                                         image::ColorEncoding::linear_srgb());
    if (!srgb) {
        return fail(ErrorCode::Internal, "no Rec.2020 to sRGB matrix", kWhere);
    }
    const double hi_level = clip_high_linear();

    // Everything below works in the measured region's coordinates; (ex, ey) is the evaluation
    // region's origin there, (dx, dy) the measured region's origin in the render.
    const std::int32_t width = region.width;
    const std::int32_t height = region.height;
    const std::int32_t dx = region.x - rendered.x;
    const std::int32_t dy = region.y - rendered.y;
    const std::int32_t ex = crop.evaluation.x - region.x;
    const std::int32_t ey = crop.evaluation.y - region.y;
    const image::PixelRect eval{ex, ey, crop.evaluation.width, crop.evaluation.height};

    CropMeasurement out;
    out.kind = crop.kind;
    Plane plane;
    plane.width = width;
    plane.height = height;
    std::vector<std::uint8_t> clipped;
    try {
        plane.lightness.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0.0f);
        clipped.assign(static_cast<std::size_t>(eval.width) * static_cast<std::size_t>(eval.height), 0);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the crop planes", kWhere);
    }
    for (std::int32_t y = 0; y < height; ++y) {
        const float* row = px.row(y + dy) + static_cast<std::ptrdiff_t>(dx) * 3;
        for (std::int32_t x = 0; x < width; ++x, row += 3) {
            const double r = row[0];
            const double g = row[1];
            const double b = row[2];
            const std::size_t i =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
            if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) {
                ++out.nonfinite_samples;
                continue;
            }
            const double Y = engine::kRec2020Luminance[0] * r + engine::kRec2020Luminance[1] * g +
                             engine::kRec2020Luminance[2] * b;
            plane.lightness[i] = static_cast<float>(engine::display_lightness(Y));
            if (eval.contains(image::Point{x, y})) {
                const image::Matrix3d& m = *srgb;
                const double smax = std::max({m(0, 0) * r + m(0, 1) * g + m(0, 2) * b,
                                              m(1, 0) * r + m(1, 1) * g + m(1, 2) * b,
                                              m(2, 0) * r + m(2, 1) * g + m(2, 2) * b});
                const std::size_t e =
                    static_cast<std::size_t>(y - eval.y) * static_cast<std::size_t>(eval.width) +
                    static_cast<std::size_t>(x - eval.x);
                clipped[e] = smax >= hi_level ? 1 : 0;
            }
        }
    }

    // Local residual l - mean3x3(l) over the evaluation region.
    std::vector<double> residual;
    residual.reserve(static_cast<std::size_t>(eval.width) * static_cast<std::size_t>(eval.height));
    const auto L = [&](std::int32_t x, std::int32_t y) { return plane.at(x, y); };
    double texture = 0.0;
    for (std::int32_t y = eval.y; y < eval.y + eval.height; ++y) {
        for (std::int32_t x = eval.x; x < eval.x + eval.width; ++x) {
            double mean = 0.0;
            for (std::int32_t oy = -1; oy <= 1; ++oy) {
                for (std::int32_t ox = -1; ox <= 1; ++ox) {
                    mean += L(x + ox, y + oy);
                }
            }
            const double r = std::abs(L(x, y) - mean / 9.0);
            residual.push_back(r);
            texture += r;
            ++out.pixels;
        }
    }
    out.texture_lstar = out.pixels == 0 ? 0.0 : 100.0 * texture / static_cast<double>(out.pixels);
    if (!residual.empty()) {
        const auto mid = residual.begin() + static_cast<std::ptrdiff_t>(residual.size() / 2);
        std::nth_element(residual.begin(), mid, residual.end());
        out.noise_lstar = 100.0 * 1.4826 * *mid;
    }
    for (std::size_t k = 0; k < clipped.size(); ++k) {
        out.highlight_clipped += clipped[k];
        if (baseline != nullptr && k < baseline->clipped.size() && clipped[k] != 0 &&
            baseline->clipped[k] == 0) {
            ++out.new_highlight_clipped;
        }
    }

    // Strong edges of the baseline (positions relative to the evaluation region).
    std::vector<EdgeSample> own_edges;
    const std::vector<EdgeSample>* edges = baseline != nullptr ? &baseline->edges : &own_edges;
    if (baseline == nullptr) {
        std::vector<double> magnitudes;
        magnitudes.reserve(residual.size());
        struct Candidate {
            std::int32_t x;
            std::int32_t y;
            double gx;
            double gy;
            double magnitude;
        };
        std::vector<Candidate> all;
        all.reserve(residual.size());
        for (std::int32_t y = eval.y; y < eval.y + eval.height; ++y) {
            for (std::int32_t x = eval.x; x < eval.x + eval.width; ++x) {
                const double gx = (L(x + 1, y - 1) + 2.0 * L(x + 1, y) + L(x + 1, y + 1)) -
                                  (L(x - 1, y - 1) + 2.0 * L(x - 1, y) + L(x - 1, y + 1));
                const double gy = (L(x - 1, y + 1) + 2.0 * L(x, y + 1) + L(x + 1, y + 1)) -
                                  (L(x - 1, y - 1) + 2.0 * L(x, y - 1) + L(x + 1, y - 1));
                const double mag = std::hypot(gx, gy);
                magnitudes.push_back(mag);
                all.push_back(Candidate{x, y, gx, gy, mag});
            }
        }
        double threshold = kMinEdgeSobel;
        if (!magnitudes.empty()) {
            const auto p99 = magnitudes.begin() + static_cast<std::ptrdiff_t>(magnitudes.size() * 99 / 100);
            std::nth_element(magnitudes.begin(), p99, magnitudes.end());
            threshold = std::max(threshold, *p99);
        }
        std::vector<Candidate> strong;
        for (const Candidate& c : all) {
            if (c.magnitude >= threshold) {
                strong.push_back(c);
            }
        }
        const std::size_t stride = std::max<std::size_t>(1, strong.size() / 1024);
        for (std::size_t k = 0; k < strong.size(); k += stride) {
            const Candidate& c = strong[k];
            EdgeSample e;
            e.x = static_cast<float>(c.x - eval.x);
            e.y = static_cast<float>(c.y - eval.y);
            e.nx = static_cast<float>(c.gx / c.magnitude);
            e.ny = static_cast<float>(c.gy / c.magnitude);
            const SideMeans m = side_means(plane, eval.x, eval.y, e);
            e.near_plus = static_cast<float>(m.near_plus);
            e.far_plus = static_cast<float>(m.far_plus);
            e.near_minus = static_cast<float>(m.near_minus);
            e.far_minus = static_cast<float>(m.far_minus);
            e.plateau_plus = std::abs(m.near_plus - m.far_plus) <= kPlateauTolerance;
            e.plateau_minus = std::abs(m.near_minus - m.far_minus) <= kPlateauTolerance;
            // A step of at least kMinEdgeContrast with a plateau on one side at least; thin lines and
            // gaps have no plateau within the reach and are not halo evidence.
            if (std::abs(m.near_plus - m.near_minus) >= kMinEdgeContrast &&
                (e.plateau_plus || e.plateau_minus)) {
                own_edges.push_back(e);
            }
        }
    }
    // Halo: on each plateau side, how much more (or less) the render changed the lightness next to
    // the edge than further away, against the baseline. Zero for the baseline itself and for any
    // edit that acts on the lightness alone; sensor noise cancels (both renders share the pixels).
    double halo = 0.0;
    for (const EdgeSample& e : *edges) {
        const SideMeans m = side_means(plane, eval.x, eval.y, e);
        double h = 0.0;
        if (e.plateau_plus) {
            h = std::max(h, std::abs((m.near_plus - e.near_plus) - (m.far_plus - e.far_plus)));
        }
        if (e.plateau_minus) {
            h = std::max(h, std::abs((m.near_minus - e.near_minus) - (m.far_minus - e.far_minus)));
        }
        halo += h;
        ++out.edge_samples;
    }
    out.halo_lstar = out.edge_samples == 0 ? 0.0 : 100.0 * halo / static_cast<double>(out.edge_samples);
    if (features != nullptr) {
        features->kind = crop.kind;
        features->edges = std::move(own_edges);
        features->clipped = std::move(clipped);
    }
    return out;
}

NativeComparison compare_native(std::span<const CropMeasurement> candidate,
                                std::span<const CropMeasurement> baseline) {
    NativeComparison c;
    if (candidate.empty() || candidate.size() != baseline.size()) {
        return c;
    }
    c.available = true;
    for (std::size_t k = 0; k < candidate.size(); ++k) {
        const CropMeasurement& m = candidate[k];
        const CropMeasurement& b = baseline[k];
        c.nonfinite_samples += m.nonfinite_samples;
        // Halos on every crop's edges: the worst crop with enough edges to be meaningful.
        c.edge_samples += m.edge_samples;
        if (m.edge_samples >= kMinCropEdgeSamples) {
            c.halo_lstar = std::max(c.halo_lstar, m.halo_lstar);
        }
        switch (m.kind) {
            case CropKind::Detail:
                if (b.texture_lstar > 0.05) {
                    c.detail_available = true;
                    c.detail_ratio = m.texture_lstar / b.texture_lstar;
                }
                break;
            case CropKind::Shadows:
                c.shadow_available = m.pixels >= kMinCropPixels;
                c.shadow_texture_lstar = m.noise_lstar;
                c.baseline_shadow_texture_lstar = b.noise_lstar;
                c.shadow_texture_ratio = b.noise_lstar > 1e-6 ? m.noise_lstar / b.noise_lstar : 1.0;
                break;
            case CropKind::Highlights:
                c.highlight_available = m.pixels >= kMinCropPixels;
                if (m.pixels > 0) {
                    c.highlight_new_clip_fraction =
                        static_cast<double>(m.new_highlight_clipped) / static_cast<double>(m.pixels);
                }
                break;
            case CropKind::Edges:
                break;
        }
    }
    return c;
}

}  // namespace lucent::analysis
