#pragma once

// Natural v1: the hard checks and the normalised heuristic objective of the automatic settings
// (blueprint section 6: "Natural v1: small plausible corrections, restrained color, protected
// highlight gradients, and limited noise amplification"; "Do not ship an arbitrary weighted sum of
// unrelated unnormalized quantities").
//
// Every term is dimensionless: a measured quantity divided by a declared scale, the amount of that
// quantity that Natural v1 treats as one clearly visible defect, mostly squared so that small
// deviations are cheap. Terms are summed with unit weights except the edit-size term. The scales
// are engineering judgement, NOT calibrated: blueprint section 10 requires blind preference and
// correction-effort tests before they are frozen (ADR 0011). Lower is better. Policy "natural"
// version 2 (ADR 0011, "Amendment: W8 core closure").
//
//   brightness  huber(x) = x^2 for x <= 1, 2x - 1 beyond, x = (deficit + excess) / kBrightnessScaleEv
//               with the deficit and excess of the candidate's quantiles against the tone
//               proposal's key-aware band and bright-content floor (proposals.hpp, tone_deficit():
//               EV of display luminance; too dark only when the median AND the bright content are
//               dark). Linear beyond one scale unit, so a large deficit cannot outweigh every other
//               term. 0 when exposure is locked: the locked exposure is the user's key, and local
//               tone is not used to get around the lock. Without brightness evidence:
//               kMissingEvidencePenalty x (|dE| / 1.5 + |dS| / 0.75).
//   highlights  0.5 (max(0, near_white - near_white_base) / 0.05)^2 + (new_clipped / 0.0025)^2
//               + min(1, saturated / 0.02) (greying / 0.05)^2  (highlights newly crowded into the
//               shoulder, new clipping, clipped areas turned grey by compression: no invented
//               recovery). Only increases over the baseline count, so high-key scenes keep their key.
//   shadows     (new_blocked / 0.0025)^2 + 0.5 (1 - low_key) (max(0, dark - dark_base) / 0.30)^2:
//               only near-black area the candidate adds counts; the scene's own deep shadows are
//               the scene's (blueprint: "preserve ... deep shadows").
//   noise       (max(0, sigma - max(sigma_base, kVisibleNoiseLStar)) / 1 L*)^2 of the predicted
//               shadow noise. Without a noise model: 0.75 kMissingEvidencePenalty x
//               (max(0, dE) / 1.5 + |dS| / 0.75).
//   contrast    (max(0, |log(R / R_base)| - 0.05) / 0.10)^2 with R the interquartile range of log2
//               display luminance, log2(Y(p75) / Y(p25)) (flattening or harshening of the tonal
//               range; 0 for flat baselines, R_base < 0.1 EV). Exposure-invariant: a pure exposure
//               change leaves R unchanged in the base look's linear segment, while shadow lift and
//               highlight compression shrink it.
//   local contr (max(0, -log(C / C_base) - 0.05) / 0.10)^2 with C the local contrast at the
//               local-tone scale (render_metrics.hpp; 0 when C_base < 0.01 EV): only flattening
//               counts (blueprint, shadows: "Reject ... flat, unnatural local contrast"). Measured on
//               the proxy too, it is the search's surrogate for what local tone costs before the
//               native crops are checked.
//   color       (max(0, |log s| - 0.03) / 0.08)^2 + (new_out_of_gamut / 0.005)^2 with s the edit's
//               chroma factor: the saturation control scales linear-light chroma by exactly s
//               (render_metrics.hpp), while the measured chroma ratio also carries the intended
//               chroma change of a white-balance correction, which the white-balance term governs.
//               Hue changes are a hard check and a diagnostic, not a preference (a white-balance
//               correction moves hues by design). Too few chromatic pixels: kMissingEvidencePenalty
//               x |s - 1| / 0.15 instead of the first part. Monochrome: 0. Colour the scene already
//               has outside the output gamut is the scene's: only new gamut damage counts.
//   white_bal.  ((log(r / r*))^2 + (log(b / b*))^2) / 0.02^2 against the proposal's gains r*, b*
//               (1 without evidence, so any residual then costs).
//   native      recheck only: (halo / 1.5 L*)^2 (native_crops.hpp; with kMinEdgeSamples edges, and
//               only when NativeComparison::halo_applies: a candidate without local tone cannot
//               create a halo)
//               + (max(0, sigma_tex - max(sigma_tex_base, kVisibleNoiseLStar)) / 1 L*)^2
//               + (max(0, 0.85 - detail_ratio) / 0.15)^2 (only flattening counts: more local detail
//               is mostly texture a tone change reveals from the shoulder or toe; noise is the
//               shadow texture's concern) + (new_clip_highlight_crop / 0.01)^2. Too few edge
//               samples with a local-tone edit: kMissingEvidencePenalty x (|dS| + |dH|) / 0.75.
//   edit        kEditWeight x edit_size(): the smaller edit wins small differences.
// Selection (select_best()): among feasible candidates, those within kTieTolerance of the lowest
// total are tied and the smallest edit_size() among them wins, then the lowest index (blueprint:
// "Prefer the smaller edit when scores are effectively tied"). The winner is therefore never more
// than kTieTolerance above the best total, whatever the evaluation order.
//
// Hard checks (evaluate_candidate): numerical validity; new highlight clipping, new blocked
// shadows, new out-of-gamut pixels, hue shift and neutral shift above their thresholds
// (metric_contract.hpp); shadow noise above kMaxShadowNoiseLStar AND more than
// kMaxShadowNoiseAmplification times the baseline's; on native crops, a halo above kMaxHaloLStar
// (when it applies),
// shadow texture above kMaxNativeNoiseLStar AND kMaxNativeNoiseAmplification times the baseline's,
// and more than kMaxNativeNewClipFraction of the highlight crop newly clipped. A hard check without
// its minimum evidence makes the candidate infeasible
// when the candidate changes what the check guards (any change of an image below kMinPixels, a
// white-balance residual without kMinNeutralPixels near-neutral pixels); the shadow-noise, colour
// and native checks without evidence instead apply the missing-evidence penalties above. The
// baseline is the reference of every relative check and stays feasible unless numerically invalid.
//
// Thread-safety: pure functions.

#include <span>
#include <string>
#include <vector>

#include <lucent/analysis/metric_contract.hpp>
#include <lucent/analysis/native_crops.hpp>
#include <lucent/analysis/proposals.hpp>
#include <lucent/analysis/render_metrics.hpp>

namespace lucent::analysis {

// ---- Scales and weights (uncalibrated) ---------------------------------------------------------

inline constexpr double kTieTolerance = 0.02;
inline constexpr double kEditWeight = 0.15;
inline constexpr double kMissingEvidencePenalty = 2.0;
// Brightness: half a stop beyond the band (or below the bright-content floor) is one visible defect.
inline constexpr double kBrightnessScaleEv = 0.5;
// Contrast (log-luminance range) and local contrast: dead zone and scale of the log ratio.
inline constexpr double kContrastDeadZone = 0.05;
inline constexpr double kContrastScale = 0.10;
// Native highlight crop: 1 % newly clipped is one visible defect.
inline constexpr double kNativeClipScale = 0.01;

// Edit-size ranges: the search bounds of blueprint section 6.
inline constexpr double kExposureRangeEv = 1.5;
inline constexpr double kWhiteBalanceRange = kWhiteBalanceResidualBound;
inline constexpr double kShadowsRangeEv = 0.75;
inline constexpr double kHighlightsRangeEv = 0.75;
inline constexpr double kContrastRange = 0.15;
inline constexpr double kSaturationRange = 0.15;

// ---- Edits -------------------------------------------------------------------------------------

// A candidate's settings relative to the baseline (as shot for unlocked controls), in the
// search's units.
struct EditVector {
    double exposure_ev = 0.0;  // DevelopSettings::exposure_ev difference
    double red_gain = 1.0;     // white balance: camera multipliers relative to the baseline's
    double blue_gain = 1.0;
    double shadows_ev = 0.0;     // difference of DevelopSettings::shadows x kShadowsMaxEv
    double highlights_ev = 0.0;  // difference of DevelopSettings::highlights x kHighlightsMaxEv
    double contrast = 0.0;       // DevelopSettings::contrast difference
    double saturation = 1.0;     // ratio of the chroma factors (1 + saturation)

    friend bool operator==(const EditVector&, const EditVector&) = default;
};

// Normalised L1 size: |dE| / 1.5 + (|r - 1| + |b - 1|) / 0.10 + |dS| / 0.75 + |dH| / 0.75
// + |dC| / 0.15 + |s - 1| / 0.15.
[[nodiscard]] double edit_size(const EditVector& edit) noexcept;

[[nodiscard]] bool changes_white_balance(const EditVector& edit) noexcept;
// A residual of more than kMaterialWhiteBalanceChange in either gain: what the confidence of the
// white-balance evidence applies to.
inline constexpr double kMaterialWhiteBalanceChange = 0.005;
[[nodiscard]] bool changes_white_balance_materially(const EditVector& edit) noexcept;
[[nodiscard]] bool changes_local_tone(const EditVector& edit) noexcept;
[[nodiscard]] bool is_identity_edit(const EditVector& edit) noexcept;

// ---- Per-image policy context ------------------------------------------------------------------

struct NaturalV1Context {
    ToneProposal tone;
    bool exposure_locked = false;  // the brightness term is then 0 (see the file comment)
    bool white_balance_evidence = false;
    double white_balance_confidence = 0.0;
    double red_target = 1.0;  // proposal gains (1 without evidence)
    double blue_target = 1.0;
    bool color = true;  // colour data (false for monochrome)
    // The final check measures inside the photographer's crop (application auto_develop.cpp): true
    // when the analysis' neutral evidence on the whole frame had at least kMinNeutralPixels pixels.
    // A white-balance change then stays feasible when the crop holds fewer near-neutral pixels
    // (InsufficientEvidence, not a failure): the analysis verified it on the whole frame, and the
    // outcome must not depend on the crop (ADR 0015).
    bool neutral_evidence_beyond_crop = false;
};

// ---- Objective ---------------------------------------------------------------------------------

struct ObjectiveTerms {
    double brightness = 0.0;
    double highlights = 0.0;
    double shadows = 0.0;
    double noise = 0.0;
    double contrast = 0.0;
    double local_contrast = 0.0;
    double color = 0.0;
    double white_balance = 0.0;
    double native = 0.0;
    double edit = 0.0;
    // Related-shot consistency (consistency.hpp): 0 unless the candidate is ranked against a group
    // target (AutoSession::regularise); natural_v1_objective() never sets it.
    double consistency = 0.0;
    double total = 0.0;

    friend bool operator==(const ObjectiveTerms&, const ObjectiveTerms&) = default;
};

// The total of `terms` in the objective's summation order (consistency last).
[[nodiscard]] double objective_total(const ObjectiveTerms& terms) noexcept;

// The Natural v1 objective of `candidate` against `baseline` (same resolution); `native` is the
// native-crop comparison at recheck (null during the proxy search).
[[nodiscard]] ObjectiveTerms natural_v1_objective(const RenderStats& candidate, const RenderStats& baseline,
                                                  const EditVector& edit, const NaturalV1Context& context,
                                                  const NativeComparison* native);

// ---- Hard checks -------------------------------------------------------------------------------

struct CandidateEvaluation {
    bool feasible = true;
    std::vector<std::string> failures;  // developer-facing English, one per failed check
    std::vector<MetricResult> metrics;  // every metric of the contract except SourceSaturation
};

// Evaluates the metric contract for one render. `is_baseline` marks the reference itself;
// `curves_monotone` is the numerical check of the candidate's contrast and tone curves.
[[nodiscard]] CandidateEvaluation evaluate_candidate(const RenderStats& candidate,
                                                     const RenderStats& baseline, const EditVector& edit,
                                                     const NaturalV1Context& context,
                                                     const NativeComparison* native, bool is_baseline,
                                                     bool curves_monotone);

// A candidate's ranking data.
struct RankKey {
    bool feasible = false;
    double total = 0.0;
    double edit_size = 0.0;
    std::size_t index = 0;
};

// The position in `keys` of the selected candidate (keys.size() when empty), in two steps: the
// candidates considered are the feasible ones (all of them when none is); among those whose total
// lies within kTieTolerance of the lowest, the smallest edit size wins, then the lowest index. The
// result does not depend on the order of `keys`.
[[nodiscard]] std::size_t select_best(std::span<const RankKey> keys) noexcept;

}  // namespace lucent::analysis
