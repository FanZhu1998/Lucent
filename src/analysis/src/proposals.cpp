#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>

#include <lucent/analysis/proposals.hpp>
#include <lucent/engine/develop_operations.hpp>

#include "parallel_bands.hpp"

namespace lucent::analysis {

namespace {

constexpr const char* kWhere = "analysis.proposals";

// Log2 histogram over [kLogLow, kLogHigh) with kLogBinsPerEv bins per EV.
constexpr double kLogLow = -24.0;
constexpr double kLogHigh = 2.0;
constexpr double kLogBinsPerEv = 64.0;
constexpr std::size_t kLogBins = static_cast<std::size_t>((kLogHigh - kLogLow) * kLogBinsPerEv);

std::size_t log_bin(double v) noexcept {
    if (!(v > 0.0)) {
        return 0;
    }
    const double b = (std::log2(v) - kLogLow) * kLogBinsPerEv;
    if (b <= 0.0) {
        return 0;
    }
    return std::min(kLogBins - 1, static_cast<std::size_t>(b));
}

double log_bin_value(double bin) noexcept {
    return std::exp2(kLogLow + bin / kLogBinsPerEv);
}

// The histogram value below which a fraction q of `total` lies (linear within the bin, log2 domain).
double log_quantile(const std::vector<std::uint64_t>& hist, std::uint64_t total, double q) {
    if (total == 0) {
        return 0.0;
    }
    const double target = q * static_cast<double>(total);
    double cumulative = 0.0;
    for (std::size_t i = 0; i < hist.size(); ++i) {
        const auto h = static_cast<double>(hist[i]);
        if (h > 0.0 && cumulative + h >= target) {
            return log_bin_value(static_cast<double>(i) + std::clamp((target - cumulative) / h, 0.0, 1.0));
        }
        cumulative += h;
    }
    return log_bin_value(static_cast<double>(hist.size()));
}

// Noise of a normalised sample: the model, or the ISO 100 assumption without one.
double noise_sigma(const ImageContext& context, double x) noexcept {
    if (context.noise.available()) {
        return context.noise.sigma(x);
    }
    return std::sqrt(kAssumedReadNoiseAtIso100 * kAssumedReadNoiseAtIso100 +
                     std::max(0.0, x) / kAssumedFullWellAtIso100);
}

// Linear histogram over [low, high) with `bins` bins: the median and the median absolute deviation.
struct LinearHistogram {
    double low = 0.0;
    double high = 1.0;
    std::vector<std::uint64_t> counts;
    std::uint64_t total = 0;

    LinearHistogram(double lo, double hi, std::size_t bins) : low(lo), high(hi), counts(bins, 0) {}

    void add(double v) {
        const double t = (v - low) / (high - low) * static_cast<double>(counts.size());
        const auto bin = static_cast<std::size_t>(std::clamp(t, 0.0, static_cast<double>(counts.size() - 1)));
        ++counts[bin];
        ++total;
    }

    [[nodiscard]] double quantile(double q) const {
        if (total == 0) {
            return 0.0;
        }
        const double width = (high - low) / static_cast<double>(counts.size());
        const double target = q * static_cast<double>(total);
        double cumulative = 0.0;
        for (std::size_t i = 0; i < counts.size(); ++i) {
            const auto h = static_cast<double>(counts[i]);
            if (h > 0.0 && cumulative + h >= target) {
                return low +
                       (static_cast<double>(i) + std::clamp((target - cumulative) / h, 0.0, 1.0)) * width;
            }
            cumulative += h;
        }
        return high;
    }
};

constexpr double kCastWindow = 0.5;  // |log ratio| of plausible casts (a factor of 1.65)

// Estimator validity of a balanced camera pixel (see the header): every channel above 8 sigma and
// 2^-11, below 0.9 of the clip level, no saturated sensor site.
bool valid_pixel(const ImageContext& context, std::size_t i, double r, double g, double b,
                 double clip_level) {
    if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) {
        return false;
    }
    if (!context.saturation.empty() && context.saturation[i] > 0.0f) {
        return false;
    }
    const double lo = std::min({r, g, b});
    const double hi = std::max({r, g, b});
    return lo >= std::max(std::exp2(-11.0), 8.0 * noise_sigma(context, g)) && hi <= 0.9 * clip_level;
}

double median_of(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    if (n == 0) {
        return 0.0;
    }
    return n % 2 == 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

}  // namespace

std::string_view to_string(WhiteBalanceEstimator estimator) noexcept {
    switch (estimator) {
        case WhiteBalanceEstimator::GreyEdge:
            return "grey_edge";
        case WhiteBalanceEstimator::BrightNeutral:
            return "bright_neutral";
        case WhiteBalanceEstimator::NeutralGreyWorld:
            return "neutral_grey_world";
    }
    return "invalid";
}

double as_shot_clip_level(const engine::StageOneImage& stage_one) noexcept {
    double clip = std::numeric_limits<double>::infinity();
    const std::size_t channels =
        std::min<std::size_t>(4, stage_one.metadata.representation.channel_colors.size());
    const auto& multipliers = stage_one.white_balance.applied.multipliers;
    for (std::size_t c = 0; c < std::max<std::size_t>(1, channels); ++c) {
        const double level = stage_one.normalization.saturation_level[c];
        if (multipliers[c] > 0.0 && level > 0.0) {
            clip = std::min(clip, level * multipliers[c]);
        }
    }
    return std::isfinite(clip) ? clip : 1.0;
}

Expected<SceneStats> measure_scene(const engine::StageOneImage& analysis, const ImageContext& context,
                                   bool parallel, const core::CancellationToken& cancel) {
    const auto& px = analysis.pixels;
    if (px.empty() || px.size() != context.size) {
        return fail(ErrorCode::InvalidArgument, "measure_scene needs a stage-one image of the context's size",
                    kWhere);
    }
    SceneStats s;
    s.clip_level = as_shot_clip_level(analysis);
    const std::int32_t channels = px.channels();
    const std::int32_t width = px.width();
    struct Part {
        std::vector<std::uint64_t> hist = std::vector<std::uint64_t>(kLogBins, 0);
        std::uint64_t pixels = 0;
        std::uint64_t signal = 0;
        std::uint64_t clipped = 0;
    };
    std::vector<Part> parts(static_cast<std::size_t>(detail::band_count(px.height(), 64)));
    const double clip = s.clip_level;
    auto status = detail::for_each_band(
        px.height(), parallel, cancel, kWhere,
        [&](std::int32_t band, std::int32_t begin, std::int32_t end) {
            Part& p = parts[static_cast<std::size_t>(band)];
            for (std::int32_t y = begin; y < end; ++y) {
                const float* row = px.row(y);
                for (std::int32_t x = 0; x < width; ++x, row += channels) {
                    double m = row[0];
                    for (std::int32_t c = 1; c < channels; ++c) {
                        m = std::max(m, static_cast<double>(row[c]));
                    }
                    if (!std::isfinite(m)) {
                        continue;
                    }
                    ++p.pixels;
                    ++p.hist[log_bin(m / clip)];
                    if (m >= clip) {
                        ++p.clipped;
                    }
                    const double g =
                        context.signal[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                       static_cast<std::size_t>(x)];
                    const bool signal = context.noise.available()
                                            ? (g > 0.0 && g >= 3.0 * context.noise.sigma(g))
                                            : g >= 1.0 / 1024.0;
                    p.signal += signal ? 1 : 0;
                }
            }
        },
        64);
    if (!status) {
        return Unexpected(std::move(status.error()));
    }
    std::vector<std::uint64_t> hist(kLogBins, 0);
    for (const Part& p : parts) {
        for (std::size_t k = 0; k < kLogBins; ++k) {
            hist[k] += p.hist[k];
        }
        s.pixels += p.pixels;
        s.signal_pixels += p.signal;
        s.clipped_fraction += static_cast<double>(p.clipped);
    }
    s.clipped_fraction = s.pixels == 0 ? 0.0 : s.clipped_fraction / static_cast<double>(s.pixels);
    const double q995 = log_quantile(hist, s.pixels, 0.995);  // relative to the clip level
    s.headroom_ev = q995 >= 1.0 || !(q995 > 0.0) ? 0.0 : -std::log2(q995);
    return s;
}

ToneDeficit tone_deficit(const LightnessQuantiles& q, const ToneProposal& policy) noexcept {
    const auto luminance = [](double l) { return engine::display_lightness_inverse(std::max(l, 0.001)); };
    ToneDeficit d;
    const double median = luminance(q.p50);
    d.median_ev = std::log2(luminance(policy.band_low) / median);
    d.bright_ev = std::log2(luminance(policy.bright_floor) / luminance(q.p90));
    d.deficit_ev = std::max(0.0, std::min(d.median_ev, d.bright_ev));
    d.excess_ev = std::max(0.0, std::log2(median / luminance(policy.band_high)));
    return d;
}

ToneProposal propose_tone(const RenderStats& baseline, const SceneStats& scene, std::optional<double> scene_ev) {
    ToneProposal t;
    t.headroom_ev = scene.headroom_ev;
    const LightnessQuantiles& q = baseline.quantiles;
    t.baseline = q;
    t.scene_ev = scene_ev;
    t.evidence = scene.signal_pixels >= kMinSignalPixels && scene.signal_fraction() >= 0.10;
    t.signal_evidence = t.evidence ? std::clamp((scene.signal_fraction() - 0.10) / 0.30, 0.0, 1.0) : 0.0;
    t.low_key = std::clamp((q.p99 - 0.75) / 0.15, 0.0, 1.0) * std::clamp((0.30 - q.p50) / 0.15, 0.0, 1.0);
    if (scene_ev && std::isfinite(*scene_ev)) {
        t.dimness = std::clamp((kDaylightSceneEv - *scene_ev) / (kDaylightSceneEv - kNightSceneEv), 0.0, 1.0);
    }
    t.high_key = std::clamp((q.p50 - 0.70) / 0.15, 0.0, 1.0) * std::clamp((0.20 - (q.p75 - q.p25)) / 0.10, 0.0, 1.0);
    t.band_low = std::max(0.02, kBandLow - kLowKeyBandDrop * t.low_key - kDimBandDrop * t.dimness);
    t.band_high = kBandHigh + kHighKeyBandRise * t.high_key;
    const ToneDeficit d = tone_deficit(q, t);
    t.median_deficit_ev = d.median_ev;
    t.bright_deficit_ev = d.bright_ev;
    t.excess_ev = d.excess_ev;
    // What a plain median band would ask for, without any key cue.
    ToneProposal plain;
    plain.bright_floor = 0.0;
    const ToneDeficit p = tone_deficit(q, plain);
    const double plain_deficit = std::max(0.0, p.median_ev);
    if (!t.evidence) {
        t.reason =
            std::format("too little scene signal ({} of {} pixels above 3 sigma): no exposure proposal",
                        scene.signal_pixels, scene.pixels);
        return t;
    }
    t.key_ambiguous =
        plain_deficit >= d.deficit_ev + kKeyAmbiguityEv || p.excess_ev >= d.excess_ev + kKeyAmbiguityEv;
    t.needed_ev = d.deficit_ev > 0.0 ? d.deficit_ev : -d.excess_ev;
    if (t.needed_ev > 0.0) {
        t.exposure_ev = std::min(t.needed_ev, std::max(0.0, scene.headroom_ev - 0.3));
        if (t.low_key < 0.5) {
            t.shadows_ev = std::clamp(0.5 * (t.needed_ev - t.exposure_ev), 0.0, 0.75);
        }
    } else if (t.needed_ev < 0.0) {
        t.exposure_ev = std::max(t.needed_ev, -1.0);
    }
    t.exposure_ev = std::clamp(t.exposure_ev, -1.5, 1.5);
    // Highlight compression protects highlights that a brightening would crowd into the shoulder.
    const double crowding = baseline.fraction(baseline.near_white);
    if (t.exposure_ev > 0.25 && crowding > 0.03) {
        t.highlights_ev = -0.5;
    } else if (t.exposure_ev > 0.25 && crowding > 0.01) {
        t.highlights_ev = -0.25;
    }
    t.reason = std::format(
        "median lightness {:.3f}, band [{:.3f}, {:.3f}] (low key {:.2f}, dimness {:.2f}{}, high key {:.2f}); "
        "median {:+.2f} EV below band_low, bright content (p90 {:.3f}) {:+.2f} EV below its floor; needed {:+.2f} EV{} "
        "(plain band {:+.2f} EV); scene headroom {:.2f} EV, near-white {:.2f} %",
        q.p50, t.band_low, t.band_high, t.low_key, t.dimness,
        scene_ev ? std::format(" at {:.1f} EV", *scene_ev) : std::string(" (no scene luminance)"), t.high_key,
        d.median_ev, q.p90, d.bright_ev, t.needed_ev, t.key_ambiguous ? " (key ambiguous)" : "",
        plain_deficit > 0.0 ? plain_deficit : -p.excess_ev, scene.headroom_ev, 100.0 * crowding);
    return t;
}

Expected<std::vector<std::uint8_t>> neutral_evidence_mask(const engine::StageOneImage& image,
                                                          const ImageContext& context, double clip_level,
                                                          double cast_log_rg, double cast_log_bg,
                                                          double radius) {
    const auto& px = image.pixels;
    if (px.empty() || px.channels() != 3 || px.size() != context.size) {
        return fail(ErrorCode::InvalidArgument,
                    "the neutral evidence needs a 3-channel image of the context's size", kWhere);
    }
    std::vector<std::uint8_t> mask;
    try {
        mask.assign(static_cast<std::size_t>(context.size.area()), 0);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the neutral evidence", kWhere);
    }
    for (std::int32_t y = 0; y < px.height(); ++y) {
        const float* row = px.row(y);
        for (std::int32_t x = 0; x < px.width(); ++x, row += 3) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(px.width()) +
                                  static_cast<std::size_t>(x);
            if (!valid_pixel(context, i, row[0], row[1], row[2], clip_level)) {
                continue;
            }
            const double lr = std::log(static_cast<double>(row[0]) / row[1]);
            const double lb = std::log(static_cast<double>(row[2]) / row[1]);
            mask[i] = std::hypot(lr - cast_log_rg, lb - cast_log_bg) <= radius ? 1 : 0;
        }
    }
    return mask;
}

WhiteBalanceCorrection natural_v1_white_balance_correction(double log_rg, double log_bg,
                                                           double confidence) noexcept {
    WhiteBalanceCorrection out;
    const double w = 0.5 * (log_rg - log_bg);
    const double t = 0.5 * (log_rg + log_bg);
    const double aw = std::abs(w);
    const double at = std::abs(t);
    const double cw = std::min(aw, 0.03) + 0.25 * std::clamp(aw - 0.03, 0.0, 0.06);
    const double ct = std::min(at, 0.05) + 0.5 * std::clamp(at - 0.05, 0.0, 0.06);
    const double c = std::clamp(confidence, 0.0, 1.0);
    const double wc = std::copysign(cw, w) * c;
    const double tc = std::copysign(ct, t) * c;
    const double log_kr = -(wc + tc);
    const double log_kb = wc - tc;
    out.red_gain =
        std::clamp(std::exp(log_kr), 1.0 - kWhiteBalanceResidualBound, 1.0 + kWhiteBalanceResidualBound);
    out.blue_gain =
        std::clamp(std::exp(log_kb), 1.0 - kWhiteBalanceResidualBound, 1.0 + kWhiteBalanceResidualBound);
    const double cast = std::hypot(log_rg, log_bg);
    if (cast > 1e-12) {
        const double left = std::hypot(log_rg + std::log(out.red_gain), log_bg + std::log(out.blue_gain));
        out.preserved_fraction = std::clamp(left / cast, 0.0, 1.0);
    }
    return out;
}

Expected<WhiteBalanceProposal> propose_white_balance(const engine::StageOneImage& analysis,
                                                     const ImageContext& context, double clip_level,
                                                     bool parallel, const core::CancellationToken& cancel) {
    const auto& px = analysis.pixels;
    if (px.empty() || px.channels() != 3 || px.size() != context.size) {
        return fail(ErrorCode::InvalidArgument,
                    "white-balance estimation needs a 3-channel stage-one image of the context's size",
                    kWhere);
    }
    const std::int32_t width = px.width();
    const std::int32_t height = px.height();
    const auto n = static_cast<std::size_t>(context.size.area());
    const auto index = [width](std::int32_t x, std::int32_t y) {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
    };

    // ---- valid pixels ----------------------------------------------------------------------------
    std::vector<std::uint8_t> valid;
    try {
        valid.assign(n, 0);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the white-balance mask", kWhere);
    }
    auto status = detail::for_each_band(
        height, parallel, cancel, kWhere, [&](std::int32_t, std::int32_t begin, std::int32_t end) {
            for (std::int32_t y = begin; y < end; ++y) {
                const float* row = px.row(y);
                for (std::int32_t x = 0; x < width; ++x, row += 3) {
                    const std::size_t i = index(x, y);
                    valid[i] = valid_pixel(context, i, row[0], row[1], row[2], clip_level) ? 1 : 0;
                }
            }
        });
    if (!status) {
        return Unexpected(std::move(status.error()));
    }

    WhiteBalanceProposal proposal;

    // ---- grey edge (p = 6) ------------------------------------------------------------------------
    // Gradients of a 3x3 binomial smoothing (with the Sobel operator, about a Gaussian derivative of
    // sigma 1): demosaicing artefacts at sharp edges would otherwise dominate the p = 6 norm.
    auto smoothed = image::ImageBuffer<float>::create_uninitialized(width, height, 3);
    if (!smoothed) {
        return Unexpected(std::move(smoothed.error()));
    }
    {
        auto horizontal = image::ImageBuffer<float>::create_uninitialized(width, height, 3);
        if (!horizontal) {
            return Unexpected(std::move(horizontal.error()));
        }
        for (std::int32_t y = 0; y < height; ++y) {
            const float* row = px.row(y);
            float* out = horizontal->row(y);
            for (std::int32_t x = 0; x < width; ++x) {
                const std::int32_t xl = std::max(0, x - 1);
                const std::int32_t xr = std::min(width - 1, x + 1);
                for (std::int32_t c = 0; c < 3; ++c) {
                    out[x * 3 + c] =
                        0.25f * row[xl * 3 + c] + 0.5f * row[x * 3 + c] + 0.25f * row[xr * 3 + c];
                }
            }
        }
        for (std::int32_t y = 0; y < height; ++y) {
            const float* up = horizontal->row(std::max(0, y - 1));
            const float* mid = horizontal->row(y);
            const float* down = horizontal->row(std::min(height - 1, y + 1));
            float* out = smoothed->row(y);
            for (std::int32_t k = 0; k < width * 3; ++k) {
                out[k] = 0.25f * up[k] + 0.5f * mid[k] + 0.25f * down[k];
            }
        }
    }
    const image::ImageBuffer<float>& sm = *smoothed;
    {
        struct Part {
            std::array<double, 3> sum{};
            std::uint64_t count = 0;
        };
        std::vector<Part> parts(static_cast<std::size_t>(detail::band_count(height)));
        status = detail::for_each_band(
            height, parallel, cancel, kWhere, [&](std::int32_t band, std::int32_t begin, std::int32_t end) {
                Part& p = parts[static_cast<std::size_t>(band)];
                for (std::int32_t y = std::max(1, begin); y < std::min(height - 1, end); ++y) {
                    for (std::int32_t x = 1; x < width - 1; ++x) {
                        bool ok = true;
                        for (std::int32_t dy = -1; dy <= 1 && ok; ++dy) {
                            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                                if (valid[index(x + dx, y + dy)] == 0) {
                                    ok = false;
                                    break;
                                }
                            }
                        }
                        if (!ok) {
                            continue;
                        }
                        const float* up = sm.row(y - 1) + static_cast<std::ptrdiff_t>(x) * 3;
                        const float* mid = sm.row(y) + static_cast<std::ptrdiff_t>(x) * 3;
                        const float* down = sm.row(y + 1) + static_cast<std::ptrdiff_t>(x) * 3;
                        std::array<double, 3> magnitude{};
                        for (std::int32_t c = 0; c < 3; ++c) {
                            const double gx =
                                (static_cast<double>(up[3 + c]) + 2.0 * mid[3 + c] + down[3 + c]) -
                                (static_cast<double>(up[c - 3]) + 2.0 * mid[c - 3] + down[c - 3]);
                            const double gy =
                                (static_cast<double>(down[c - 3]) + 2.0 * down[c] + down[3 + c]) -
                                (static_cast<double>(up[c - 3]) + 2.0 * up[c] + up[3 + c]);
                            magnitude[static_cast<std::size_t>(c)] = std::hypot(gx, gy);
                        }
                        const double noise = 4.0 * std::sqrt(12.0) * noise_sigma(context, mid[1]);
                        if (magnitude[1] <= noise) {
                            continue;
                        }
                        ++p.count;
                        for (std::size_t c = 0; c < 3; ++c) {
                            const double m2 = magnitude[c] * magnitude[c];
                            p.sum[c] += m2 * m2 * m2;
                        }
                    }
                }
            });
        if (!status) {
            return Unexpected(std::move(status.error()));
        }
        WhiteBalanceEstimate e;
        e.method = WhiteBalanceEstimator::GreyEdge;
        std::array<double, 3> sum{};
        for (const Part& p : parts) {
            for (std::size_t c = 0; c < 3; ++c) {
                sum[c] += p.sum[c];
            }
            e.samples += p.count;
        }
        if (e.samples >= 1000 && sum[0] > 0.0 && sum[1] > 0.0 && sum[2] > 0.0) {
            e.available = true;
            e.log_rg = (std::log(sum[0]) - std::log(sum[1])) / 6.0;
            e.log_bg = (std::log(sum[2]) - std::log(sum[1])) / 6.0;
            e.confidence = std::min(1.0, static_cast<double>(e.samples) / 4000.0);
            e.detail = std::format("{} edge pixels", e.samples);
        } else {
            e.detail = std::format("{} edge pixels (at least 1000 needed)", e.samples);
        }
        proposal.estimates.push_back(std::move(e));
    }

    // ---- bright neutral and neutral grey world -----------------------------------------------------
    {
        std::vector<std::uint64_t> luminance(kLogBins, 0);
        std::uint64_t window = 0;
        std::array<double, 3> sums{};
        for (std::int32_t y = 0; y < height; ++y) {
            const float* row = px.row(y);
            for (std::int32_t x = 0; x < width; ++x, row += 3) {
                if (valid[index(x, y)] == 0) {
                    continue;
                }
                const double lr = std::log(static_cast<double>(row[0]) / row[1]);
                const double lb = std::log(static_cast<double>(row[2]) / row[1]);
                if (std::abs(lr) > kCastWindow || std::abs(lb) > kCastWindow) {
                    continue;
                }
                ++window;
                ++luminance[log_bin(row[1])];
                sums[0] += row[0];
                sums[1] += row[1];
                sums[2] += row[2];
            }
        }
        if (auto cancelled = cancel.check(kWhere); !cancelled) {
            return Unexpected(std::move(cancelled.error()));
        }
        const double bright = log_quantile(luminance, window, 0.80);
        LinearHistogram hr(-kCastWindow, kCastWindow, 500);
        LinearHistogram hb(-kCastWindow, kCastWindow, 500);
        for (std::int32_t y = 0; y < height; ++y) {
            const float* row = px.row(y);
            for (std::int32_t x = 0; x < width; ++x, row += 3) {
                if (valid[index(x, y)] == 0 || row[1] < bright) {
                    continue;
                }
                const double lr = std::log(static_cast<double>(row[0]) / row[1]);
                const double lb = std::log(static_cast<double>(row[2]) / row[1]);
                if (std::abs(lr) > kCastWindow || std::abs(lb) > kCastWindow) {
                    continue;
                }
                hr.add(lr);
                hb.add(lb);
            }
        }
        WhiteBalanceEstimate e;
        e.method = WhiteBalanceEstimator::BrightNeutral;
        e.samples = hr.total;
        if (hr.total >= 200) {
            const double mr = hr.quantile(0.5);
            const double mb = hb.quantile(0.5);
            LinearHistogram dr(0.0, 2.0 * kCastWindow, 500);
            LinearHistogram db(0.0, 2.0 * kCastWindow, 500);
            for (std::int32_t y = 0; y < height; ++y) {
                const float* row = px.row(y);
                for (std::int32_t x = 0; x < width; ++x, row += 3) {
                    if (valid[index(x, y)] == 0 || row[1] < bright) {
                        continue;
                    }
                    const double lr = std::log(static_cast<double>(row[0]) / row[1]);
                    const double lb = std::log(static_cast<double>(row[2]) / row[1]);
                    if (std::abs(lr) > kCastWindow || std::abs(lb) > kCastWindow) {
                        continue;
                    }
                    dr.add(std::abs(lr - mr));
                    db.add(std::abs(lb - mb));
                }
            }
            const double dispersion = 1.4826 * std::hypot(dr.quantile(0.5), db.quantile(0.5));
            e.available = true;
            e.log_rg = mr;
            e.log_bg = mb;
            e.confidence = std::min(1.0, static_cast<double>(e.samples) / 1000.0) *
                           std::clamp(1.0 - dispersion / 0.15, 0.0, 1.0);
            e.available = e.confidence > 0.0;
            e.detail = std::format("{} bright pixels, dispersion {:.3f}", e.samples, dispersion);
        } else {
            e.detail = std::format("{} bright pixels in the cast window (at least 200 needed)", e.samples);
        }
        proposal.estimates.push_back(std::move(e));

        WhiteBalanceEstimate w;
        w.method = WhiteBalanceEstimator::NeutralGreyWorld;
        w.samples = window;
        if (window >= 1000 && sums[0] > 0.0 && sums[1] > 0.0 && sums[2] > 0.0) {
            w.available = true;
            w.log_rg = std::log(sums[0] / sums[1]);
            w.log_bg = std::log(sums[2] / sums[1]);
            w.confidence = 0.5 * std::min(1.0, static_cast<double>(window) / 4000.0);
            w.detail = std::format("{} pixels in the cast window", window);
        } else {
            w.detail = std::format("{} pixels in the cast window (at least 1000 needed)", window);
        }
        proposal.estimates.push_back(std::move(w));
    }

    // ---- combination and the Natural v1 policy -----------------------------------------------------
    std::vector<double> rg;
    std::vector<double> bg;
    double confidence_sum = 0.0;
    for (const WhiteBalanceEstimate& e : proposal.estimates) {
        if (e.available) {
            rg.push_back(e.log_rg);
            bg.push_back(e.log_bg);
            confidence_sum += e.confidence;
        }
    }
    if (rg.empty()) {
        proposal.reason = "no neutral evidence: white balance stays as shot";
        return proposal;
    }
    proposal.evidence = true;
    proposal.cast_log_rg = median_of(rg);
    proposal.cast_log_bg = median_of(bg);
    for (std::size_t i = 0; i < rg.size(); ++i) {
        for (std::size_t j = i + 1; j < rg.size(); ++j) {
            proposal.spread = std::max(proposal.spread, std::hypot(rg[i] - rg[j], bg[i] - bg[j]));
        }
    }
    // Agreement tolerance grows with the cast: estimators that agree on a large cast's direction
    // but not its exact size still support the (small, bounded) correction Natural v1 makes.
    const double tolerance = 0.05 + 0.25 * std::hypot(proposal.cast_log_rg, proposal.cast_log_bg);
    const double agreement = rg.size() == 1 ? 0.4 : std::exp(-std::pow(proposal.spread / tolerance, 2.0));
    proposal.confidence = std::clamp(agreement * confidence_sum / static_cast<double>(rg.size()), 0.0, 1.0);
    proposal.warmth = 0.5 * (proposal.cast_log_rg - proposal.cast_log_bg);
    proposal.tint = 0.5 * (proposal.cast_log_rg + proposal.cast_log_bg);
    const WhiteBalanceCorrection correction =
        natural_v1_white_balance_correction(proposal.cast_log_rg, proposal.cast_log_bg, proposal.confidence);
    proposal.red_gain = correction.red_gain;
    proposal.blue_gain = correction.blue_gain;
    proposal.preserved_fraction = correction.preserved_fraction;
    proposal.reason = std::format(
        "cast log(r/g) {:+.3f}, log(b/g) {:+.3f} (warmth {:+.3f}, tint {:+.3f}) from {} estimator{}, spread "
        "{:.3f}, "
        "confidence {:.2f}: gains r {:.4f}, b {:.4f}, {:.0f} % of the cast kept",
        proposal.cast_log_rg, proposal.cast_log_bg, proposal.warmth, proposal.tint, rg.size(),
        rg.size() == 1 ? "" : "s", proposal.spread, proposal.confidence, proposal.red_gain,
        proposal.blue_gain, 100.0 * proposal.preserved_fraction);
    return proposal;
}

}  // namespace lucent::analysis
