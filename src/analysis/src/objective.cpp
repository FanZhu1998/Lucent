#include <algorithm>
#include <cmath>
#include <format>

#include <lucent/analysis/objective.hpp>
#include <lucent/engine/develop_operations.hpp>

namespace lucent::analysis {

namespace {

double sq(double v) noexcept {
    return v * v;
}

double huber(double x) noexcept {
    return x <= 1.0 ? x * x : 2.0 * x - 1.0;
}

// Interquartile range of log2 display luminance (EV).
double log_range(const LightnessQuantiles& q) noexcept {
    const double lo = engine::display_lightness_inverse(std::max(q.p25, 0.001));
    const double hi = engine::display_lightness_inverse(std::max(q.p75, 0.001));
    return lo > 0.0 && hi > 0.0 ? std::log2(hi / lo) : 0.0;
}

constexpr double kChangeEpsilon = 1e-9;

}  // namespace

double edit_size(const EditVector& e) noexcept {
    return std::abs(e.exposure_ev) / kExposureRangeEv +
           (std::abs(e.red_gain - 1.0) + std::abs(e.blue_gain - 1.0)) / kWhiteBalanceRange +
           std::abs(e.shadows_ev) / kShadowsRangeEv + std::abs(e.highlights_ev) / kHighlightsRangeEv +
           std::abs(e.contrast) / kContrastRange + std::abs(e.saturation - 1.0) / kSaturationRange;
}

bool changes_white_balance(const EditVector& e) noexcept {
    return std::abs(e.red_gain - 1.0) > kChangeEpsilon || std::abs(e.blue_gain - 1.0) > kChangeEpsilon;
}

bool changes_white_balance_materially(const EditVector& e) noexcept {
    return std::abs(e.red_gain - 1.0) > kMaterialWhiteBalanceChange ||
           std::abs(e.blue_gain - 1.0) > kMaterialWhiteBalanceChange;
}

bool changes_local_tone(const EditVector& e) noexcept {
    return std::abs(e.shadows_ev) > kChangeEpsilon || std::abs(e.highlights_ev) > kChangeEpsilon;
}

bool is_identity_edit(const EditVector& e) noexcept {
    return edit_size(e) <= kChangeEpsilon;
}

ObjectiveTerms natural_v1_objective(const RenderStats& c, const RenderStats& base, const EditVector& edit,
                                    const NaturalV1Context& ctx, const NativeComparison* native) {
    ObjectiveTerms t;
    const ToneProposal& tone = ctx.tone;

    // Brightness.
    if (tone.evidence) {
        if (!ctx.exposure_locked) {
            const ToneDeficit d = tone_deficit(c.quantiles, tone);
            t.brightness = huber((d.deficit_ev + d.excess_ev) / kBrightnessScaleEv);
        }
    } else {
        t.brightness = kMissingEvidencePenalty * (std::abs(edit.exposure_ev) / kExposureRangeEv +
                                                  std::abs(edit.shadows_ev) / kShadowsRangeEv);
    }

    // Highlights.
    const double saturated = c.fraction(c.source_saturated);
    t.highlights = 0.5 * sq(std::max(0.0, c.fraction(c.near_white) - base.fraction(base.near_white)) / 0.05) +
                   sq(c.fraction(c.new_highlight_clipped) / 0.0025) +
                   std::min(1.0, saturated / 0.02) * sq(c.saturated_greying / 0.05);

    // Shadows: new blocking, and near-black area the candidate adds.
    t.shadows = sq(c.fraction(c.new_shadow_blocked) / 0.0025) +
                0.5 * (1.0 - tone.low_key) * sq(std::max(0.0, c.fraction(c.dark) - base.fraction(base.dark)) / 0.30);

    // Noise (predicted from the sensor model).
    if (c.shadow_pixels < kMinShadowPixels) {
        t.noise = 0.0;  // nothing to amplify
    } else if (!c.noise_model_available) {
        t.noise = 0.75 * kMissingEvidencePenalty *
                  (std::max(0.0, edit.exposure_ev) / kExposureRangeEv +
                   std::abs(edit.shadows_ev) / kShadowsRangeEv);
    } else {
        t.noise =
            sq(std::max(0.0, c.shadow_noise_lstar - std::max(base.shadow_noise_lstar, kVisibleNoiseLStar)) /
               1.0);
    }

    // Contrast: the tonal range in log luminance (exposure-invariant), both directions.
    const double range_base = log_range(base.quantiles);
    const double range = log_range(c.quantiles);
    if (range_base >= 0.1 && range > 0.0) {
        t.contrast = sq(std::max(0.0, std::abs(std::log(range / range_base)) - kContrastDeadZone) / kContrastScale);
    }
    // Local contrast at the local-tone scale: flattening only.
    if (base.local_contrast_ev >= 0.01 && c.local_contrast_ev > 0.0) {
        t.local_contrast = sq(std::max(0.0, -std::log(c.local_contrast_ev / base.local_contrast_ev) - kContrastDeadZone) /
                              kContrastScale);
    }

    // Colour. The saturation control scales linear-light chroma by exactly its factor (the measured
    // saturation_log_ratio also carries the white balance's intended chroma change, which is the
    // white-balance term's concern), so its restraint uses the edit; gamut damage is measured.
    if (ctx.color) {
        if (c.chromatic_pixels >= kMinChromaticPixels) {
            t.color = sq(std::max(0.0, std::abs(std::log(edit.saturation)) - 0.03) / 0.08);
        } else {
            t.color = kMissingEvidencePenalty * std::abs(edit.saturation - 1.0) / kSaturationRange;
        }
        t.color += sq(c.fraction(c.new_out_of_gamut) / 0.005);
    }

    // White balance.
    if (ctx.color) {
        t.white_balance =
            (sq(std::log(edit.red_gain / ctx.red_target)) + sq(std::log(edit.blue_gain / ctx.blue_target))) /
            sq(0.02);
    }

    // Native crops (recheck only).
    if (native != nullptr && native->available) {
        t.native = native->halo_applies && native->edge_samples >= kMinEdgeSamples ? sq(native->halo_lstar / 1.5) : 0.0;
        if (native->shadow_available) {
            t.native +=
                sq(std::max(0.0, native->shadow_texture_lstar -
                                     std::max(native->baseline_shadow_texture_lstar, kVisibleNoiseLStar)) /
                   1.0);
        }
        if (native->detail_available) {
            // Only flattening counts: more local detail is usually texture the tone change reveals
            // (out of the shoulder or the toe); amplified noise is the shadow texture's concern.
            t.native += sq(std::max(0.0, 0.85 - native->detail_ratio) / 0.15);
        }
        if (native->highlight_available) {
            t.native += sq(native->highlight_new_clip_fraction / kNativeClipScale);
        }
        if (native->edge_samples < kMinEdgeSamples && changes_local_tone(edit)) {
            t.native += kMissingEvidencePenalty * (std::abs(edit.shadows_ev) + std::abs(edit.highlights_ev)) /
                        kShadowsRangeEv;
        }
    }

    t.edit = kEditWeight * edit_size(edit);
    t.total = objective_total(t);
    return t;
}

double objective_total(const ObjectiveTerms& t) noexcept {
    return t.brightness + t.highlights + t.shadows + t.noise + t.contrast + t.local_contrast + t.color +
           t.white_balance + t.native + t.edit + t.consistency;
}

CandidateEvaluation evaluate_candidate(const RenderStats& c, const RenderStats& base, const EditVector& edit,
                                       const NaturalV1Context& ctx, const NativeComparison* native,
                                       bool is_baseline, bool curves_monotone) {
    CandidateEvaluation out;
    const auto fail_with = [&](MetricResult& m, std::string reason) {
        m.status = MetricStatus::Fail;
        m.detail = reason;
        if (!is_baseline) {
            out.feasible = false;
        }
        out.failures.push_back(std::format("{}: {}", to_string(m.id), std::move(reason)));
    };
    const bool changed = !is_identity_edit(edit);
    const bool enough_pixels = c.pixels >= kMinPixels;

    // Numerical validity (also fails the baseline).
    {
        MetricResult m;
        m.id = MetricId::NumericalValidity;
        m.valid_samples = c.pixels;
        m.values = {{"nonfinite_samples", static_cast<double>(c.nonfinite_samples)},
                    {"nonmonotone_curves", curves_monotone ? 0.0 : 1.0},
                    {"pixels", static_cast<double>(c.pixels)}};
        m.status = MetricStatus::Pass;
        if (c.nonfinite_samples > 0 || c.pixels == 0 || !curves_monotone) {
            m.status = MetricStatus::Fail;
            m.detail = c.pixels == 0 ? "empty render"
                       : c.nonfinite_samples > 0
                           ? std::format("{} non-finite output samples", c.nonfinite_samples)
                           : "a contrast or tone curve is not non-decreasing";
            out.feasible = false;
            out.failures.push_back(std::format("numerical_validity: {}", m.detail));
        }
        out.metrics.push_back(std::move(m));
    }

    // Render-induced clipping.
    {
        MetricResult m;
        m.id = MetricId::RenderClipping;
        m.valid_samples = c.pixels;
        m.values = {{"total_fraction", c.fraction(c.highlight_clipped)},
                    {"new_fraction", c.fraction(c.new_highlight_clipped)},
                    {"baseline_fraction", base.fraction(base.highlight_clipped)},
                    {"source_fraction", c.fraction(c.source_saturated)},
                    {"near_white_fraction", c.fraction(c.near_white)},
                    {"saturated_greying", c.saturated_greying}};
        m.status = MetricStatus::Pass;
        if (!enough_pixels) {
            m.status = MetricStatus::InsufficientEvidence;
            m.detail = std::format("{} pixels (at least {} needed)", c.pixels, kMinPixels);
            if (changed && !is_baseline) {
                out.feasible = false;
                out.failures.push_back("render_clipping: too few pixels to verify a change");
            }
        } else if (c.fraction(c.new_highlight_clipped) > kMaxNewHighlightClipFraction) {
            fail_with(m, std::format("{:.3f} % of pixels newly clipped (limit {:.3f} %)",
                                     100.0 * c.fraction(c.new_highlight_clipped),
                                     100.0 * kMaxNewHighlightClipFraction));
        }
        out.metrics.push_back(std::move(m));
    }

    // Blocked shadows.
    {
        MetricResult m;
        m.id = MetricId::ShadowBlocking;
        m.valid_samples = c.pixels;
        m.values = {{"total_fraction", c.fraction(c.shadow_blocked)},
                    {"new_fraction", c.fraction(c.new_shadow_blocked)},
                    {"baseline_fraction", base.fraction(base.shadow_blocked)},
                    {"dark_fraction", c.fraction(c.dark)}};
        m.status = MetricStatus::Pass;
        if (!enough_pixels) {
            m.status = MetricStatus::InsufficientEvidence;
            m.detail = std::format("{} pixels (at least {} needed)", c.pixels, kMinPixels);
            if (changed && !is_baseline) {
                out.feasible = false;
                out.failures.push_back("shadow_blocking: too few pixels to verify a change");
            }
        } else if (c.fraction(c.new_shadow_blocked) > kMaxNewShadowBlockFraction) {
            fail_with(
                m, std::format("{:.3f} % of pixels newly blocked (limit {:.3f} %)",
                               100.0 * c.fraction(c.new_shadow_blocked), 100.0 * kMaxNewShadowBlockFraction));
        }
        out.metrics.push_back(std::move(m));
    }

    // Tonal distribution (preference only).
    {
        MetricResult m;
        m.id = MetricId::TonalDistribution;
        m.valid_samples = c.signal_pixels;
        const LightnessQuantiles& q = c.quantiles;
        const ToneDeficit d = tone_deficit(q, ctx.tone);
        m.values = {{"p01", q.p01},
                    {"p05", q.p05},
                    {"p10", q.p10},
                    {"p25", q.p25},
                    {"p50", q.p50},
                    {"p75", q.p75},
                    {"p90", q.p90},
                    {"p95", q.p95},
                    {"p99", q.p99},
                    {"p995", q.p995},
                    {"mean", c.mean_lightness},
                    {"iqr", c.iqr()},
                    {"baseline_p50", base.quantiles.p50},
                    {"headroom_lightness", 1.0 - q.p995},
                    {"baseline_headroom_lightness", 1.0 - base.quantiles.p995},
                    {"scene_headroom_ev", ctx.tone.headroom_ev},
                    {"band_low", ctx.tone.band_low},
                    {"band_high", ctx.tone.band_high},
                    {"bright_floor", ctx.tone.bright_floor},
                    {"low_key", ctx.tone.low_key},
                    {"dimness", ctx.tone.dimness},
                    {"high_key", ctx.tone.high_key},
                    {"median_deficit_ev", d.median_ev},
                    {"bright_deficit_ev", d.bright_ev},
                    {"deficit_ev", d.deficit_ev},
                    {"excess_ev", d.excess_ev},
                    {"log_range_ev", log_range(q)},
                    {"baseline_log_range_ev", log_range(base.quantiles)},
                    {"local_contrast_ev", c.local_contrast_ev},
                    {"baseline_local_contrast_ev", base.local_contrast_ev},
                    {"dark_fraction", c.fraction(c.dark)},
                    {"baseline_dark_fraction", base.fraction(base.dark)},
                    {"signal_fraction", c.fraction(c.signal_pixels)}};
        if (c.signal_pixels < kMinSignalPixels || !ctx.tone.evidence) {
            m.status = MetricStatus::InsufficientEvidence;
            m.detail = std::format("{} pixels with scene signal above 3 sigma (at least {} and 10 % needed)",
                                   c.signal_pixels, kMinSignalPixels);
        } else {
            m.status = MetricStatus::Pass;
        }
        out.metrics.push_back(std::move(m));
    }

    // White balance.
    {
        MetricResult m;
        m.id = MetricId::WhiteBalance;
        m.valid_samples = c.neutral_pixels;
        m.values = {{"red_gain", edit.red_gain},
                    {"blue_gain", edit.blue_gain},
                    {"red_target", ctx.red_target},
                    {"blue_target", ctx.blue_target},
                    {"confidence", ctx.white_balance_confidence},
                    {"neutral_shift_de", c.neutral_shift_de},
                    {"neutral_pixels", static_cast<double>(c.neutral_pixels)}};
        m.status = MetricStatus::Pass;
        if (!ctx.color) {
            m.status = MetricStatus::NotApplicable;
        } else if (changes_white_balance(edit)) {
            if (c.neutral_pixels < kMinNeutralPixels) {
                m.status = MetricStatus::InsufficientEvidence;
                m.detail = std::format(
                    "{} near-neutral pixels (at least {} needed) to verify a white-balance change",
                    c.neutral_pixels, kMinNeutralPixels);
                if (ctx.neutral_evidence_beyond_crop) {
                    m.detail += "; the neutral evidence of the whole frame lies outside the crop";
                } else if (!is_baseline) {
                    out.feasible = false;
                    out.failures.push_back("white_balance: no neutral evidence for a residual change");
                }
            } else if (c.neutral_shift_de > kMaxNeutralShiftDeltaE) {
                fail_with(m, std::format("neutral shift {:.2f} (limit {:.2f})", c.neutral_shift_de,
                                         kMaxNeutralShiftDeltaE));
            }
        }
        out.metrics.push_back(std::move(m));
    }

    // Shadow noise.
    {
        MetricResult m;
        m.id = MetricId::ShadowNoise;
        m.valid_samples = c.shadow_pixels;
        const double amplification =
            base.shadow_noise_lstar > 1e-9 ? c.shadow_noise_lstar / base.shadow_noise_lstar : 1.0;
        m.values = {{"predicted_sigma_lstar", c.shadow_noise_lstar},
                    {"baseline_sigma_lstar", base.shadow_noise_lstar},
                    {"amplification", amplification},
                    {"shadow_pixels", static_cast<double>(c.shadow_pixels)}};
        if (c.shadow_pixels < kMinShadowPixels) {
            m.status = MetricStatus::NotApplicable;
            m.detail = std::format("{} shadow pixels: nothing to amplify", c.shadow_pixels);
        } else if (!c.noise_model_available) {
            m.status = MetricStatus::InsufficientEvidence;
            m.detail = "no sensor noise model: shadow lifts are penalised instead";
        } else if (c.shadow_noise_lstar > kMaxShadowNoiseLStar &&
                   amplification > kMaxShadowNoiseAmplification) {
            fail_with(m, std::format("predicted shadow noise {:.2f} L* ({:.2f} x the baseline)",
                                     c.shadow_noise_lstar, amplification));
        } else {
            m.status = MetricStatus::Pass;
        }
        out.metrics.push_back(std::move(m));
    }

    // Native crops.
    {
        MetricResult m;
        m.id = MetricId::NativeDetail;
        if (native == nullptr || !native->available) {
            m.status = MetricStatus::NotApplicable;
            m.detail =
                native == nullptr ? "proxy evaluation (crops are measured at recheck)" : "no native crops";
        } else {
            m.valid_samples = native->edge_samples;
            m.values = {{"halo_lstar", native->halo_lstar},
                        {"halo_applies", native->halo_applies ? 1.0 : 0.0},
                        {"edge_samples", static_cast<double>(native->edge_samples)},
                        {"detail_ratio", native->detail_ratio},
                        {"shadow_texture_lstar", native->shadow_texture_lstar},
                        {"baseline_shadow_texture_lstar", native->baseline_shadow_texture_lstar},
                        {"shadow_texture_ratio", native->shadow_texture_ratio},
                        {"highlight_new_clip_fraction", native->highlight_new_clip_fraction},
                        {"nonfinite_samples", static_cast<double>(native->nonfinite_samples)}};
            m.status = MetricStatus::Pass;
            if (native->nonfinite_samples > 0) {
                m.status = MetricStatus::Fail;
                m.detail = std::format("{} non-finite samples in native crops", native->nonfinite_samples);
                out.feasible = false;
                out.failures.push_back(std::format("native_detail: {}", m.detail));
            } else if (native->halo_applies && native->edge_samples >= kMinEdgeSamples &&
                       native->halo_lstar > kMaxHaloLStar) {
                fail_with(m, std::format("halo of {:.2f} L* next to strong edges (limit {:.2f})",
                                         native->halo_lstar, kMaxHaloLStar));
            } else if (native->shadow_available && native->shadow_texture_lstar > kMaxNativeNoiseLStar &&
                       native->shadow_texture_ratio > kMaxNativeNoiseAmplification) {
                fail_with(m, std::format("native shadow texture {:.2f} L* ({:.2f} x the baseline)",
                                         native->shadow_texture_lstar, native->shadow_texture_ratio));
            } else if (native->highlight_available &&
                       native->highlight_new_clip_fraction > kMaxNativeNewClipFraction) {
                fail_with(m, std::format("{:.2f} % of the native highlight crop newly clipped (limit {:.2f} %)",
                                         100.0 * native->highlight_new_clip_fraction,
                                         100.0 * kMaxNativeNewClipFraction));
            } else if (native->edge_samples < kMinEdgeSamples) {
                m.status = MetricStatus::InsufficientEvidence;
                m.detail = std::format("{} edge samples (at least {} needed): halos not verified",
                                       native->edge_samples, kMinEdgeSamples);
            }
        }
        out.metrics.push_back(std::move(m));
    }

    // Colour and gamut.
    {
        MetricResult m;
        m.id = MetricId::ColorGamut;
        m.valid_samples = c.chromatic_pixels;
        m.values = {{"saturation_log_ratio", c.saturation_log_ratio},
                    {"hue_shift_degrees", c.hue_shift_degrees},
                    {"mean_chroma", c.mean_chroma},
                    {"out_of_gamut_fraction", c.fraction(c.out_of_gamut)},
                    {"new_out_of_gamut_fraction", c.fraction(c.new_out_of_gamut)},
                    {"baseline_out_of_gamut_fraction", base.fraction(base.out_of_gamut)},
                    {"chromatic_pixels", static_cast<double>(c.chromatic_pixels)}};
        m.status = MetricStatus::Pass;
        if (!ctx.color) {
            m.status = MetricStatus::NotApplicable;
        } else if (enough_pixels && c.fraction(c.new_out_of_gamut) > kMaxNewGamutFraction) {
            fail_with(m, std::format("{:.2f} % of pixels newly out of gamut (limit {:.2f} %)",
                                     100.0 * c.fraction(c.new_out_of_gamut), 100.0 * kMaxNewGamutFraction));
        } else if (c.chromatic_pixels >= kMinChromaticPixels && c.hue_shift_degrees > kMaxHueShiftDegrees) {
            fail_with(m, std::format("hue shift {:.2f} degrees (limit {:.2f})", c.hue_shift_degrees,
                                     kMaxHueShiftDegrees));
        } else if (c.chromatic_pixels < kMinChromaticPixels) {
            m.status = MetricStatus::InsufficientEvidence;
            m.detail = std::format("{} chromatic pixels (at least {} needed)", c.chromatic_pixels,
                                   kMinChromaticPixels);
        }
        out.metrics.push_back(std::move(m));
    }
    return out;
}

std::size_t select_best(std::span<const RankKey> keys) noexcept {
    const bool any_feasible = std::any_of(keys.begin(), keys.end(), [](const RankKey& k) { return k.feasible; });
    const auto considered = [&](const RankKey& k) { return k.feasible || !any_feasible; };
    double lowest = 0.0;
    bool first = true;
    for (const RankKey& k : keys) {
        if (considered(k) && (first || k.total < lowest)) {
            lowest = k.total;
            first = false;
        }
    }
    std::size_t best = keys.size();
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const RankKey& k = keys[i];
        if (!considered(k) || k.total > lowest + kTieTolerance) {
            continue;
        }
        if (best == keys.size() || k.edit_size < keys[best].edit_size ||
            (k.edit_size == keys[best].edit_size && k.index < keys[best].index)) {
            best = i;
        }
    }
    return best;
}

}  // namespace lucent::analysis
