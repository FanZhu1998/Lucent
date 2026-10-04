#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <numbers>

#include <lucent/analysis/consistency.hpp>
#include <lucent/analysis/render_metrics.hpp>
#include <lucent/engine/develop_operations.hpp>
#include <lucent/engine/display_rendering.hpp>
#include <lucent/image/color_math.hpp>

#include "parallel_bands.hpp"

namespace lucent::analysis {

namespace {

constexpr const char* kWhere = "analysis.metrics";
using Clock = std::chrono::steady_clock;

constexpr std::size_t kLightnessBins = 4096;  // over l in [0, 1]
constexpr std::size_t kNoiseBins = 2000;      // over sigma_L in [0, 40) L* units
constexpr double kNoiseBinWidth = 0.02;
constexpr double kLabEpsilon = 216.0 / 24389.0;
constexpr double kLabKappa = 24389.0 / 27.0;
constexpr double kNoSignalFloor = 1.0 / 1024.0;
constexpr std::int32_t kMetricBandRows = 32;

struct Matrices {
    image::Matrix3d to_xyz;   // display-linear Rec.2020 -> XYZ (D65, Y of white = 1)
    image::Matrix3d to_srgb;  // display-linear Rec.2020 -> linear sRGB
    image::Vec3d white;       // XYZ of RGB (1, 1, 1)
};

Expected<Matrices> matrices() {
    Matrices m;
    auto xyz = image::rgb_to_xyz(image::kRec2020Primaries, image::kD65White);
    if (!xyz) {
        return fail(ErrorCode::Internal, "no Rec.2020 matrix: " + xyz.error().message, kWhere);
    }
    auto srgb = image::conversion_matrix(image::ColorEncoding::scene_linear_rec2020_d65(),
                                         image::ColorEncoding::linear_srgb());
    if (!srgb) {
        return fail(ErrorCode::Internal, "no Rec.2020 to sRGB matrix: " + srgb.error().message, kWhere);
    }
    m.to_xyz = *xyz;
    m.to_srgb = *srgb;
    m.white = m.to_xyz * image::Vec3d{1.0, 1.0, 1.0};
    return m;
}

inline double lab_f(double t) noexcept {
    return t > kLabEpsilon ? std::cbrt(t) : (kLabKappa * t + 16.0) / 116.0;
}

// d l / d Y of the display lightness at luminance Y.
inline double lightness_slope(double y) noexcept {
    if (y > kLabEpsilon) {
        const double c = std::cbrt(y);
        return 1.16 / (3.0 * c * c);
    }
    return kLabKappa / 100.0;
}

struct Partial {
    std::vector<std::uint32_t> lightness_hist;
    std::vector<std::uint32_t> noise_hist;
    std::uint64_t nonfinite_samples = 0;
    std::uint64_t pixels = 0;
    std::uint64_t signal = 0;
    std::uint64_t near_white = 0;
    std::uint64_t dark = 0;
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
    std::uint64_t gamut = 0;
    std::uint64_t new_hi = 0;
    std::uint64_t new_lo = 0;
    std::uint64_t new_gamut = 0;
    std::uint64_t saturated = 0;
    std::uint64_t chromatic = 0;
    std::uint64_t neutral = 0;
    std::uint64_t shadow = 0;
    double sum_l = 0.0;
    double sum_grey = 0.0;
    double sum_sat_c = 0.0;
    double sum_sat_b = 0.0;
    double sum_hue = 0.0;
    double sum_hue_weight = 0.0;
    double sum_chroma = 0.0;
    double sum_da = 0.0;
    double sum_db = 0.0;
    std::uint64_t midtone = 0;
    double sum_mx = 0.0;
    double sum_my = 0.0;
    double sum_mz = 0.0;
};

template <class Hist>
double histogram_quantile(const Hist& hist, std::uint64_t total, double q, double width) {
    if (total == 0) {
        return 0.0;
    }
    const double target = q * static_cast<double>(total);
    double cumulative = 0.0;
    for (std::size_t i = 0; i < hist.size(); ++i) {
        const auto h = static_cast<double>(hist[i]);
        if (h > 0.0 && cumulative + h >= target) {
            const double frac = std::clamp((target - cumulative) / h, 0.0, 1.0);
            return (static_cast<double>(i) + frac) * width;
        }
        cumulative += h;
    }
    return static_cast<double>(hist.size()) * width;
}

}  // namespace

double clip_high_linear() noexcept {
    return image::srgb_decode(254.5 / 255.0);
}
double block_low_linear() noexcept {
    return image::srgb_decode(0.5 / 255.0);
}

Expected<ImageContext> make_image_context(const engine::StageOneImage& stage_one, const CoverageMap* coverage,
                                          const SensorNoiseModel& noise) {
    const auto& pixels = stage_one.pixels;
    if (pixels.empty() || (pixels.channels() != 3 && pixels.channels() != 1)) {
        return fail(ErrorCode::InvalidArgument, "the context needs a stage-one image of 1 or 3 channels",
                    kWhere);
    }
    ImageContext ctx;
    ctx.size = pixels.size();
    ctx.noise = noise;
    const auto n = static_cast<std::size_t>(ctx.size.area());
    if (coverage != nullptr && !coverage->empty()) {
        if (coverage->size != ctx.size) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("coverage map {}x{} does not match the image {}x{}", coverage->size.width,
                                    coverage->size.height, ctx.size.width, ctx.size.height),
                        kWhere);
        }
        ctx.saturation = coverage->fraction;
    }
    try {
        ctx.signal.resize(n);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the signal plane", kWhere);
    }
    const std::int32_t channels = pixels.channels();
    const std::int32_t green = channels == 3 ? 1 : 0;
    for (std::int32_t y = 0; y < ctx.size.height; ++y) {
        const float* row = pixels.row(y);
        float* out =
            ctx.signal.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(ctx.size.width);
        for (std::int32_t x = 0; x < ctx.size.width; ++x) {
            const float v = row[static_cast<std::ptrdiff_t>(x) * channels + green];
            out[x] = std::isfinite(v) ? v : 0.0f;
        }
    }
    return ctx;
}

Expected<RenderStats> measure_render(const engine::ColorImage& display, const ImageContext& context,
                                     const BaselineFeatures* baseline, BaselineFeatures* features,
                                     bool parallel, const core::CancellationToken& cancel) {
    const auto start = Clock::now();
    const auto& px = display.pixels;
    if (px.empty() || px.channels() != 3 || display.encoding != engine::display_linear_rec2020() ||
        px.size() != context.size) {
        return fail(
            ErrorCode::InvalidArgument,
            std::format(
                "measure_render needs a display-linear Rec.2020 render of {}x{} (got {}x{}, {} channels, "
                "{})",
                context.size.width, context.size.height, px.width(), px.height(), px.channels(),
                display.encoding.to_string()),
            kWhere);
    }
    const auto n = static_cast<std::size_t>(context.size.area());
    if (context.signal.size() != n || (!context.saturation.empty() && context.saturation.size() != n) ||
        (!context.neutral_evidence.empty() && context.neutral_evidence.size() != n)) {
        return fail(ErrorCode::InvalidArgument, "the image context does not match its size", kWhere);
    }
    if (baseline != nullptr &&
        (baseline->size != context.size || baseline->lightness.size() != n || baseline->a.size() != n ||
         baseline->b.size() != n || baseline->chroma.size() != n || baseline->flags.size() != n)) {
        return fail(ErrorCode::InvalidArgument, "the baseline features do not match the render", kWhere);
    }
    auto mats = matrices();
    if (!mats) {
        return Unexpected(std::move(mats.error()));
    }
    if (features != nullptr) {
        try {
            features->size = context.size;
            features->lightness.assign(n, 0.0f);
            features->a.assign(n, 0.0f);
            features->b.assign(n, 0.0f);
            features->chroma.assign(n, 0.0f);
            features->flags.assign(n, 0);
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "cannot allocate the baseline features", kWhere);
        }
    }

    const Matrices& m = *mats;
    const double xn = m.white[0];
    const double yn = m.white[1];
    const double zn = m.white[2];
    const double hi_level = clip_high_linear();
    const double lo_level = block_low_linear();
    const SensorNoiseModel& noise = context.noise;
    const bool noise_available = noise.available();
    const std::int32_t width = context.size.width;
    const std::int32_t height = context.size.height;

    std::vector<Partial> partials;
    try {
        partials.resize(static_cast<std::size_t>(detail::band_count(height, kMetricBandRows)));
        for (Partial& p : partials) {
            p.lightness_hist.assign(kLightnessBins, 0);
            p.noise_hist.assign(kNoiseBins, 0);
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the metric histograms", kWhere);
    }

    auto status = detail::for_each_band(
        height, parallel, cancel, kWhere,
        [&](std::int32_t band, std::int32_t begin, std::int32_t end) {
            Partial& p = partials[static_cast<std::size_t>(band)];
            for (std::int32_t y = begin; y < end; ++y) {
                const float* row = px.row(y);
                for (std::int32_t x = 0; x < width; ++x, row += 3) {
                    const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                          static_cast<std::size_t>(x);
                    const double r = row[0];
                    const double g = row[1];
                    const double b = row[2];
                    if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) {
                        p.nonfinite_samples += static_cast<std::uint64_t>(!std::isfinite(r)) +
                                               static_cast<std::uint64_t>(!std::isfinite(g)) +
                                               static_cast<std::uint64_t>(!std::isfinite(b));
                        if (features != nullptr) {
                            features->flags[i] = kFlagNonFinite;
                        }
                        continue;
                    }
                    ++p.pixels;
                    const double X = m.to_xyz(0, 0) * r + m.to_xyz(0, 1) * g + m.to_xyz(0, 2) * b;
                    const double Y = m.to_xyz(1, 0) * r + m.to_xyz(1, 1) * g + m.to_xyz(1, 2) * b;
                    const double Z = m.to_xyz(2, 0) * r + m.to_xyz(2, 1) * g + m.to_xyz(2, 2) * b;
                    const double fx = lab_f(X / xn);
                    const double fy = lab_f(Y / yn);
                    const double fz = lab_f(Z / zn);
                    // Rounded to float as the baseline features are stored, so a render compared
                    // with itself differs by exactly nothing.
                    const double l = static_cast<float>(1.16 * fy - 0.16);
                    const double A = static_cast<float>(500.0 * (fx - fy));
                    const double B = static_cast<float>(200.0 * (fy - fz));
                    const double C = std::hypot(A, B);
                    const double deviation =
                        std::sqrt((r - Y) * (r - Y) + (g - Y) * (g - Y) + (b - Y) * (b - Y));
                    const double linear_chroma = Y > 0.0 ? static_cast<float>(deviation / Y) : 0.0f;

                    const double sr = m.to_srgb(0, 0) * r + m.to_srgb(0, 1) * g + m.to_srgb(0, 2) * b;
                    const double sg = m.to_srgb(1, 0) * r + m.to_srgb(1, 1) * g + m.to_srgb(1, 2) * b;
                    const double sb = m.to_srgb(2, 0) * r + m.to_srgb(2, 1) * g + m.to_srgb(2, 2) * b;
                    const double smax = std::max({sr, sg, sb});
                    const double smin = std::min({sr, sg, sb});
                    std::uint8_t flags = 0;
                    if (smax >= hi_level) {
                        flags |= kFlagHighlightClipped;
                    }
                    if (smax <= lo_level) {
                        flags |= kFlagShadowBlocked;
                    }
                    if (smin < -kGamutTolerance * std::max(smax, 0.0) || smax > 1.0 + kGamutTolerance) {
                        flags |= kFlagOutOfGamut;
                    }
                    const bool hi = (flags & kFlagHighlightClipped) != 0;
                    p.hi += hi ? 1 : 0;
                    p.lo += (flags & kFlagShadowBlocked) != 0 ? 1 : 0;
                    p.gamut += (flags & kFlagOutOfGamut) != 0 ? 1 : 0;

                    const double lm = engine::display_lightness(std::max({r, g, b}));
                    if (lm >= kNearWhiteLightness && !hi) {
                        ++p.near_white;
                    }
                    if (l < kDarkLightness) {
                        ++p.dark;
                    }
                    const double lc = std::clamp(l, 0.0, 1.0);
                    const auto bin =
                        std::min(kLightnessBins - 1, static_cast<std::size_t>(lc * kLightnessBins));
                    ++p.lightness_hist[bin];
                    p.sum_l += l;
                    p.sum_chroma += C;

                    const double s = context.signal[i];
                    if (noise_available ? (s > 0.0 && s >= 3.0 * noise.sigma(s)) : s >= kNoSignalFloor) {
                        ++p.signal;
                    }

                    const double bl = baseline != nullptr ? baseline->lightness[i] : l;
                    const double ba = baseline != nullptr ? baseline->a[i] : A;
                    const double bb = baseline != nullptr ? baseline->b[i] : B;
                    const double bchroma = baseline != nullptr ? baseline->chroma[i] : linear_chroma;
                    const std::uint8_t bflags = baseline != nullptr ? baseline->flags[i] : flags;
                    if (baseline != nullptr) {
                        p.new_hi += hi && (bflags & kFlagHighlightClipped) == 0 ? 1 : 0;
                        p.new_lo +=
                            (flags & kFlagShadowBlocked) != 0 && (bflags & kFlagShadowBlocked) == 0 ? 1 : 0;
                        p.new_gamut +=
                            (flags & kFlagOutOfGamut) != 0 && (bflags & kFlagOutOfGamut) == 0 ? 1 : 0;
                    }
                    const float coverage = context.saturation.empty() ? 0.0f : context.saturation[i];
                    if (coverage >= kSaturatedCoverage) {
                        ++p.saturated;
                        p.sum_grey += std::max(0.0, bl - l);
                    }
                    const double bc = std::hypot(ba, bb);
                    const bool base_hi = (bflags & kFlagHighlightClipped) != 0;
                    if (bc >= kChromaticChroma && bl > kChromaticLightnessLow && bl < 0.95 && !base_hi &&
                        !hi) {
                        ++p.chromatic;
                        p.sum_sat_c += linear_chroma;
                        p.sum_sat_b += bchroma;
                        const double dot = A * ba + B * bb;
                        const double cross = ba * B - bb * A;
                        p.sum_hue += bc * std::abs(std::atan2(cross, dot));
                        p.sum_hue_weight += bc;
                    }
                    const bool neutral_surface = context.neutral_evidence.empty()
                                                     ? bc < kNeutralChroma
                                                     : context.neutral_evidence[i] != 0;
                    if (neutral_surface && bl > 0.15 && bl < 0.9 && !base_hi) {
                        ++p.neutral;
                        p.sum_da += A - ba;
                        p.sum_db += B - bb;
                    }
                    if (bl > kMidtoneLow && bl < kMidtoneHigh && !base_hi && !hi) {
                        ++p.midtone;
                        p.sum_mx += X;
                        p.sum_my += Y;
                        p.sum_mz += Z;
                    }
                    if (bl > kShadowMaskLow && bl < kShadowMaskHigh && coverage <= 0.0f && s > 0.0) {
                        ++p.shadow;
                        if (noise_available && Y > 0.0) {
                            const double sigma_l = 100.0 * lightness_slope(Y) * (Y / s) * noise.sigma(s);
                            const auto nb =
                                std::min(kNoiseBins - 1,
                                         static_cast<std::size_t>(std::max(0.0, sigma_l) / kNoiseBinWidth));
                            ++p.noise_hist[nb];
                        }
                    }
                    if (features != nullptr) {
                        features->lightness[i] = static_cast<float>(l);
                        features->a[i] = static_cast<float>(A);
                        features->b[i] = static_cast<float>(B);
                        features->chroma[i] = static_cast<float>(linear_chroma);
                        features->flags[i] = flags;
                    }
                }
            }
        },
        kMetricBandRows);
    if (!status) {
        return Unexpected(std::move(status.error()));
    }

    // Merge in band order.
    std::vector<std::uint64_t> lightness_hist(kLightnessBins, 0);
    std::vector<std::uint64_t> noise_hist(kNoiseBins, 0);
    Partial t;
    for (const Partial& p : partials) {
        for (std::size_t k = 0; k < kLightnessBins; ++k) {
            lightness_hist[k] += p.lightness_hist[k];
        }
        for (std::size_t k = 0; k < kNoiseBins; ++k) {
            noise_hist[k] += p.noise_hist[k];
        }
        t.nonfinite_samples += p.nonfinite_samples;
        t.pixels += p.pixels;
        t.signal += p.signal;
        t.near_white += p.near_white;
        t.dark += p.dark;
        t.hi += p.hi;
        t.lo += p.lo;
        t.gamut += p.gamut;
        t.new_hi += p.new_hi;
        t.new_lo += p.new_lo;
        t.new_gamut += p.new_gamut;
        t.saturated += p.saturated;
        t.chromatic += p.chromatic;
        t.neutral += p.neutral;
        t.shadow += p.shadow;
        t.sum_l += p.sum_l;
        t.sum_grey += p.sum_grey;
        t.sum_sat_c += p.sum_sat_c;
        t.sum_sat_b += p.sum_sat_b;
        t.sum_hue += p.sum_hue;
        t.sum_hue_weight += p.sum_hue_weight;
        t.sum_chroma += p.sum_chroma;
        t.sum_da += p.sum_da;
        t.sum_db += p.sum_db;
        t.midtone += p.midtone;
        t.sum_mx += p.sum_mx;
        t.sum_my += p.sum_my;
        t.sum_mz += p.sum_mz;
    }

    RenderStats s;
    s.size = context.size;
    s.pixels = t.pixels;
    s.nonfinite_samples = t.nonfinite_samples;
    const double bin_width = 1.0 / static_cast<double>(kLightnessBins);
    s.quantiles.p01 = histogram_quantile(lightness_hist, t.pixels, 0.01, bin_width);
    s.quantiles.p05 = histogram_quantile(lightness_hist, t.pixels, 0.05, bin_width);
    s.quantiles.p10 = histogram_quantile(lightness_hist, t.pixels, 0.10, bin_width);
    s.quantiles.p25 = histogram_quantile(lightness_hist, t.pixels, 0.25, bin_width);
    s.quantiles.p50 = histogram_quantile(lightness_hist, t.pixels, 0.50, bin_width);
    s.quantiles.p75 = histogram_quantile(lightness_hist, t.pixels, 0.75, bin_width);
    s.quantiles.p90 = histogram_quantile(lightness_hist, t.pixels, 0.90, bin_width);
    s.quantiles.p95 = histogram_quantile(lightness_hist, t.pixels, 0.95, bin_width);
    s.quantiles.p99 = histogram_quantile(lightness_hist, t.pixels, 0.99, bin_width);
    s.quantiles.p995 = histogram_quantile(lightness_hist, t.pixels, 0.995, bin_width);
    const auto count = static_cast<double>(std::max<std::uint64_t>(1, t.pixels));
    s.mean_lightness = t.sum_l / count;
    s.signal_pixels = t.signal;
    s.near_white = t.near_white;
    s.dark = t.dark;
    s.highlight_clipped = t.hi;
    s.shadow_blocked = t.lo;
    s.out_of_gamut = t.gamut;
    s.new_highlight_clipped = t.new_hi;
    s.new_shadow_blocked = t.new_lo;
    s.new_out_of_gamut = t.new_gamut;
    s.source_saturated = t.saturated;
    s.saturated_greying = t.saturated == 0 ? 0.0 : t.sum_grey / static_cast<double>(t.saturated);
    s.chromatic_pixels = t.chromatic;
    s.saturation_log_ratio =
        (t.sum_sat_c > 0.0 && t.sum_sat_b > 0.0) ? std::log(t.sum_sat_c / t.sum_sat_b) : 0.0;
    s.hue_shift_degrees =
        t.sum_hue_weight > 0.0 ? t.sum_hue / t.sum_hue_weight * 180.0 / std::numbers::pi : 0.0;
    s.mean_chroma = t.sum_chroma / count;
    s.neutral_pixels = t.neutral;
    s.neutral_shift_de = t.neutral == 0 ? 0.0
                                        : std::hypot(t.sum_da / static_cast<double>(t.neutral),
                                                     t.sum_db / static_cast<double>(t.neutral));
    s.noise_model_available = noise_available;
    s.shadow_pixels = t.shadow;
    std::uint64_t noise_count = 0;
    for (const std::uint64_t h : noise_hist) {
        noise_count += h;
    }
    s.shadow_noise_lstar = histogram_quantile(noise_hist, noise_count, 0.75, kNoiseBinWidth);
    s.midtone_pixels = t.midtone;
    s.midtone_x = t.sum_mx;
    s.midtone_y = t.sum_my;
    s.midtone_z = t.sum_mz;
    s.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return s;
}

}  // namespace lucent::analysis
