// Per-pixel develop operations: base look, local tone, lightness curves and colour
// (develop_operations.hpp).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include <lucent/engine/develop_operations.hpp>
#include <lucent/engine/display_rendering.hpp>
#include <lucent/engine/guided_filter.hpp>

#include "octave_lut.hpp"
#include "parallel_rows.hpp"

namespace lucent::engine {

namespace {

constexpr float kLumR = static_cast<float>(kRec2020Luminance[0]);
constexpr float kLumG = static_cast<float>(kRec2020Luminance[1]);
constexpr float kLumB = static_cast<float>(kRec2020Luminance[2]);

Status check_scene_image(const ColorImage& image, const char* where, const char* what) {
    if (image.pixels.empty() || image.pixels.channels() != 3) {
        return fail(ErrorCode::InvalidArgument, std::format("{} needs a non-empty 3-channel image", what), where);
    }
    if (image.encoding != image::ColorEncoding::scene_linear_rec2020_d65()) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("{} works on scene-linear Rec.2020 / D65 (got {})", what, image.encoding.to_string()),
                    where);
    }
    return ok();
}

template <class T>
Expected<std::vector<T>> per_row(std::int32_t rows, const char* where) {
    try {
        return std::vector<T>(static_cast<std::size_t>(rows));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate per-row records", where);
    }
}

double smoothstep(double lo, double hi, double e) noexcept {
    const double t = std::clamp((e - lo) / (hi - lo), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

}  // namespace

// ---- Base look ------------------------------------------------------------------------------------

Expected<BaseLookRecord> apply_base_look(ColorImage& image, const NeutralV1Curve& curve,
                                         const core::CancellationToken& cancel, bool parallel) {
    constexpr const char* kWhere = "engine.develop.base_look";
    if (auto valid = check_scene_image(image, kWhere, "the base look"); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    struct Counts {
        std::uint64_t limited = 0;
        std::uint64_t negative = 0;
        std::uint64_t non_finite = 0;
    };
    auto counts = per_row<Counts>(image.pixels.height(), kWhere);
    if (!counts) {
        return Unexpected(std::move(counts.error()));
    }
    const image::ImageView<float> view = image.pixels.view();
    const std::int32_t width = view.width();
    const double white = curve.white();
    auto status = detail::for_each_row(view.height(), parallel, cancel, kWhere, [&](std::int32_t y) {
        Counts c;
        float* p = view.row(y);
        for (std::int32_t x = 0; x < width; ++x, p += 3) {
            const double m = std::max({p[0], p[1], p[2]});
            if (m > 0.0) {
                const float scale = static_cast<float>(curve.evaluate(m) / m);
                c.limited += static_cast<std::uint64_t>(m >= white);
                p[0] *= scale;
                p[1] *= scale;
                p[2] *= scale;
            }
            c.negative += static_cast<std::uint64_t>(p[0] < 0.0f) + static_cast<std::uint64_t>(p[1] < 0.0f) +
                          static_cast<std::uint64_t>(p[2] < 0.0f);
            c.non_finite += static_cast<std::uint64_t>(!std::isfinite(p[0]) || !std::isfinite(p[1]) ||
                                                       !std::isfinite(p[2]));
        }
        (*counts)[static_cast<std::size_t>(y)] = c;
    });
    if (!status) {
        return Unexpected(std::move(status.error()));
    }
    BaseLookRecord record;
    record.white = white;
    record.regime = curve.regime();
    record.top_start_ev = curve.top_start_ev();
    record.end_slope = curve.end_slope();
    record.toe_start_ev = curve.toe_start_ev();
    record.toe_end_ev = curve.toe_end_ev();
    std::uint64_t non_finite = 0;
    for (const Counts& c : *counts) {
        record.limited_pixels += c.limited;
        record.negative_samples += c.negative;
        non_finite += c.non_finite;
    }
    if (non_finite != 0) {
        return fail(ErrorCode::Internal, std::format("the base look produced {} non-finite pixels", non_finite), kWhere);
    }
    image.encoding = display_linear_rec2020();
    return record;
}

// ---- Local tone -----------------------------------------------------------------------------------

double local_tone_gain_ev(double base_ev, double white_ev, double highlights, double shadows) noexcept {
    const double e = std::min(0.0, std::max(base_ev + kNeutralV1MidOffsetEv, base_ev - white_ev));
    double gain = 0.0;
    if (highlights != 0.0) {
        gain += kHighlightsMaxEv * highlights * smoothstep(kHighlightsWeightEv[0], kHighlightsWeightEv[1], e);
    }
    if (shadows != 0.0) {
        gain += kShadowsMaxEv * shadows * (1.0 - smoothstep(kShadowsWeightEv[0], kShadowsWeightEv[1], e));
    }
    return gain;
}

Expected<LocalToneRecord> apply_local_tone(ColorImage& image, const LocalToneParameters& parameters,
                                           const image::AllocationLimits& limits, const core::CancellationToken& cancel,
                                           bool parallel) {
    constexpr const char* kWhere = "engine.develop.local_tone";
    if (auto valid = check_scene_image(image, kWhere, "local tone"); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    const LocalToneParameters& q = parameters;
    if (!std::isfinite(q.highlights) || !std::isfinite(q.shadows) || std::abs(q.highlights) > kMaxAdjustment ||
        std::abs(q.shadows) > kMaxAdjustment || !std::isfinite(q.white) || !(q.white > 0.0) || q.radius < 1 ||
        !std::isfinite(q.epsilon) || !(q.epsilon > 0.0)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("invalid local-tone parameters (highlights {}, shadows {}, white {}, radius {}, eps {})",
                                q.highlights, q.shadows, q.white, q.radius, q.epsilon),
                    kWhere);
    }
    const std::int32_t width = image.pixels.width();
    const std::int32_t height = image.pixels.height();
    auto log_plane = image::ImageBuffer<float>::create_uninitialized(width, height, 1, limits);
    if (!log_plane) {
        return fail(log_plane.error().code, "local tone log-luminance plane: " + log_plane.error().message, kWhere);
    }
    const image::ImageView<float> rgb = image.pixels.view();
    const image::ImageView<float> base = log_plane->view();
    const double white_ev = std::log2(q.white);
    const float floor_value = static_cast<float>(q.white * std::exp2(kLocalToneFloorEv));
    if (auto status = detail::for_each_row(height, parallel, cancel, kWhere,
                                           [&](std::int32_t y) {
                                               const float* p = rgb.row(y);
                                               float* l = base.row(y);
                                               for (std::int32_t x = 0; x < width; ++x, p += 3) {
                                                   const float lum = kLumR * p[0] + kLumG * p[1] + kLumB * p[2];
                                                   l[x] = std::log2(std::max(lum, floor_value));
                                               }
                                           });
        !status) {
        return Unexpected(std::move(status.error()));
    }
    if (auto status = guided_filter(base, base, base, q.radius, q.epsilon, limits, cancel, parallel); !status) {
        return Unexpected(Error{status.error().code, "local tone: " + status.error().message, status.error().where,
                                status.error().native_code});
    }
    // The gain is a function of the base value alone: tabulated at 1/256 EV over the span where it
    // varies (below it the shadow weight is 1, above it the highlight weight is 1), linear
    // interpolation (relative error below 2e-6, octave_lut.hpp).
    auto gain = detail::UniformLut::create(
        white_ev + kLocalToneFloorEv - 1.0, white_ev + 1.0,
        256 * static_cast<std::int32_t>(2.0 - kLocalToneFloorEv) + 1,
        [&](double base_ev) { return std::exp2(local_tone_gain_ev(base_ev, white_ev, q.highlights, q.shadows)); },
        kWhere);
    if (!gain) {
        return Unexpected(std::move(gain.error()));
    }
    struct Range {
        double min = std::numeric_limits<double>::infinity();
        double max = -std::numeric_limits<double>::infinity();
    };
    auto ranges = per_row<Range>(height, kWhere);
    if (!ranges) {
        return Unexpected(std::move(ranges.error()));
    }
    const detail::UniformLut& scale_of = *gain;
    if (auto status = detail::for_each_row(height, parallel, cancel, kWhere,
                                           [&](std::int32_t y) {
                                               float* p = rgb.row(y);
                                               const float* l = base.row(y);
                                               Range r;
                                               for (std::int32_t x = 0; x < width; ++x, p += 3) {
                                                   const double g = scale_of(l[x]);
                                                   r.min = std::min(r.min, g);
                                                   r.max = std::max(r.max, g);
                                                   if (g != 1.0) {
                                                       const auto scale = static_cast<float>(g);
                                                       p[0] *= scale;
                                                       p[1] *= scale;
                                                       p[2] *= scale;
                                                   }
                                               }
                                               (*ranges)[static_cast<std::size_t>(y)] = r;
                                           });
        !status) {
        return Unexpected(std::move(status.error()));
    }
    LocalToneRecord record;
    record.radius = q.radius;
    record.epsilon = q.epsilon;
    double min_scale = std::numeric_limits<double>::infinity();
    double max_scale = -std::numeric_limits<double>::infinity();
    for (const Range& r : *ranges) {
        min_scale = std::min(min_scale, r.min);
        max_scale = std::max(max_scale, r.max);
    }
    record.min_gain_ev = std::log2(min_scale);
    record.max_gain_ev = std::log2(max_scale);
    return record;
}

// ---- Lightness curves -----------------------------------------------------------------------------

Expected<LightnessCurvesRecord> apply_lightness_curves(ColorImage& image, const NeutralV1Curve& base,
                                                       const ContrastCurve* contrast, const ToneCurve* tone,
                                                       const core::CancellationToken& cancel, bool parallel) {
    constexpr const char* kWhere = "engine.develop.lightness";
    if (auto valid = check_scene_image(image, kWhere, "the lightness curves"); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    struct Counts {
        std::uint64_t changed = 0;
        std::uint64_t non_finite = 0;
    };
    auto counts = per_row<Counts>(image.pixels.height(), kWhere);
    if (!counts) {
        return Unexpected(std::move(counts.error()));
    }
    // The target norm as a function of the norm (the exact chain).
    const auto composite = [&](double m) noexcept {
        double l = display_lightness(base.evaluate(m));
        if (contrast != nullptr) {
            l = contrast->evaluate(l);
        }
        if (tone != nullptr) {
            l = tone->evaluate(l);
        }
        const double d = display_lightness_inverse(l);
        return d > 0.0 ? base.inverse(d) : 0.0;
    };
    // Tabulated over the 27 octaves below the white point (512 nodes per octave, octave_lut.hpp);
    // smaller norms (more than 26 EV below white) use the exact chain, norms at or above the
    // white point its constant value there.
    const double white = base.white();
    const std::int32_t e_top = std::ilogb(white) + 1;  // 2^e_top > white
    constexpr std::int32_t kOctaves = 27;
    auto table = detail::OctaveLut::create(e_top - kOctaves, kOctaves, composite, parallel, cancel, kWhere);
    if (!table) {
        return Unexpected(std::move(table.error()));
    }
    const detail::OctaveLut& lut = *table;
    // Lookups need normal floats.
    const double table_low = std::max(lut.lower(), 2.0 * static_cast<double>(std::numeric_limits<float>::min()));
    const double at_white = composite(white);
    const image::ImageView<float> view = image.pixels.view();
    const std::int32_t width = view.width();
    auto status = detail::for_each_row(view.height(), parallel, cancel, kWhere, [&](std::int32_t y) {
        Counts c;
        float* p = view.row(y);
        for (std::int32_t x = 0; x < width; ++x, p += 3) {
            const float mf = std::max({p[0], p[1], p[2]});
            const double m = mf;
            if (!(m > 0.0)) {
                continue;
            }
            const double target = m >= white ? at_white : (m >= table_low ? lut(mf) : composite(m));
            const auto scale = static_cast<float>(target / m);
            if (scale != 1.0f) {
                ++c.changed;
                p[0] *= scale;
                p[1] *= scale;
                p[2] *= scale;
            }
            c.non_finite += static_cast<std::uint64_t>(!std::isfinite(p[0]) || !std::isfinite(p[1]) ||
                                                       !std::isfinite(p[2]));
        }
        (*counts)[static_cast<std::size_t>(y)] = c;
    });
    if (!status) {
        return Unexpected(std::move(status.error()));
    }
    LightnessCurvesRecord record;
    std::uint64_t non_finite = 0;
    for (const Counts& c : *counts) {
        record.changed_pixels += c.changed;
        non_finite += c.non_finite;
    }
    if (non_finite != 0) {
        return fail(ErrorCode::Internal, std::format("the lightness curves produced {} non-finite pixels", non_finite),
                    kWhere);
    }
    return record;
}

// ---- Saturation and vibrance ----------------------------------------------------------------------

Expected<ColorAdjustRecord> apply_color(ColorImage& image, double saturation, double vibrance,
                                        const core::CancellationToken& cancel, bool parallel) {
    constexpr const char* kWhere = "engine.develop.color";
    if (auto valid = check_scene_image(image, kWhere, "saturation and vibrance"); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    if (!std::isfinite(saturation) || !std::isfinite(vibrance) || std::abs(saturation) > kMaxAdjustment ||
        std::abs(vibrance) > kMaxAdjustment) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("saturation {} and vibrance {} must be finite and within [-1, 1]", saturation, vibrance),
                    kWhere);
    }
    auto negatives = per_row<std::uint64_t>(image.pixels.height(), kWhere);
    if (!negatives) {
        return Unexpected(std::move(negatives.error()));
    }
    const image::ImageView<float> view = image.pixels.view();
    const std::int32_t width = view.width();
    const float sat_factor = static_cast<float>(1.0 + saturation);
    const float vib = static_cast<float>(vibrance);
    auto status = detail::for_each_row(view.height(), parallel, cancel, kWhere, [&](std::int32_t y) {
        std::uint64_t negative = 0;
        float* p = view.row(y);
        for (std::int32_t x = 0; x < width; ++x, p += 3) {
            const float r = p[0];
            const float g = p[1];
            const float b = p[2];
            const float hi = std::max({r, g, b});
            const float lo = std::min({r, g, b});
            const float s = hi > 0.0f ? std::clamp((hi - lo) / hi, 0.0f, 1.0f) : 0.0f;
            const float w = (1.0f - s) * (1.0f - s);
            const float k = sat_factor * (1.0f + vib * w);
            const float lum = kLumR * r + kLumG * g + kLumB * b;
            p[0] = lum + k * (r - lum);
            p[1] = lum + k * (g - lum);
            p[2] = lum + k * (b - lum);
            negative += static_cast<std::uint64_t>(p[0] < 0.0f) + static_cast<std::uint64_t>(p[1] < 0.0f) +
                        static_cast<std::uint64_t>(p[2] < 0.0f);
        }
        (*negatives)[static_cast<std::size_t>(y)] = negative;
    });
    if (!status) {
        return Unexpected(std::move(status.error()));
    }
    ColorAdjustRecord record;
    for (const std::uint64_t n : *negatives) {
        record.negative_samples += n;
    }
    return record;
}

}  // namespace lucent::engine
