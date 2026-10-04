#pragma once

// Deterministic proposals of the automatic settings (blueprint section 6, "Analysis and candidate
// search" step 2): the histogram/headroom exposure proposal and evidence-based white-balance
// proposals, each with its evidence and confidence. Proposals seed the bounded search
// (auto_settings.hpp); the Natural v1 objective (objective.hpp) then decides.
//
// Scene measurements use the analysis image's stage one: white-balanced (as shot) camera RGB,
// scene-linear, highlights not clipped (StageOneKind::Interactive), upright.
//
// Exposure / headroom (ToneProposal), the Natural tone policy (policy version 2):
//   * evidence: pixels whose scene signal exceeds 3 sigma of the noise model; at least
//     kMinSignalPixels and 10 % of the image, else no exposure proposal and signal evidence 0.
//     The signal evidence (clamp((signal fraction - 0.10) / 0.30)) says whether the frame carries a
//     measurable signal at all; it is NOT a certainty about the key or the size of the edit;
//   * the key, from the baseline's luminance lightness quantiles and the scene luminance, without
//     semantics:
//       low_key  = clamp((p99 - 0.75) / 0.15) x clamp((0.30 - p50) / 0.15): bright content near
//                  white with a dark median (silhouettes, night scenes with lights);
//       dimness  = clamp((10 - scene_ev) / 8) from ShotFeatures::scene_ev (consistency.hpp), the
//                  luminance of the scene's median in EV100: 0 for daylight (10 EV and above), 1 for
//                  night (2 EV and below); 0 without exposure metadata. Dim scenes are rendered
//                  darker than daylight ones (an ND filter reads as dim: the safe direction);
//       high_key = clamp((p50 - 0.70) / 0.15) x clamp((0.20 - IQR) / 0.10): a bright, low-contrast
//                  frame (snow, fog, white backgrounds);
//     band_low = max(0.02, 0.40 - 0.38 low_key - 0.15 dimness), band_high = 0.85 + 0.08 high_key;
//   * the deficit (tone_deficit()), in EV of display luminance (display_lightness_inverse: the base
//     look's mid-tones are a pure gain, so an exposure change moves it one to one): the SMALLER of
//     the median's distance below band_low and the bright content's (p90) distance below
//     kBrightContentFloor. A frame counts as too dark only when its median AND its bright content
//     are dark: a dark median under well-exposed bright content is a low-key composition (a lit
//     subject before a dark ground), not underexposure, and keeps its key without depending on
//     near-white highlights. The excess is the median's distance above band_high;
//   * the exposure that removes the deficit (or the excess), limited upwards by the scene headroom
//     minus 0.3 EV (p99.5 of the value norm against the stage-two clip level) and downwards to
//     -1 EV; what the headroom withholds becomes half as much shadow lift (up to 0.75 EV, not for
//     low-key scenes); a brightening (more than 0.25 EV) of an image with highlights near white
//     (more than 1 % / 3 % of pixels) proposes -0.25 / -0.5 EV of local highlight compression;
//   * key_ambiguous: a key cue (low key, dimness, high key, the bright-content floor) reduced the
//     correction a plain [0.40, 0.85] median band would ask for by at least kKeyAmbiguityEv (half a
//     brightness scale unit) - the interpretation of the key, not the measurement, decides the
//     edit's size (the controller's policy confidence, auto_settings.hpp).
//
// White balance (WhiteBalanceProposal), in the as-shot balanced camera RGB of the image it is given
// (the controller passes the search proxy: area averaging attenuates demosaicing artefacts), on
// valid pixels (every channel above 8 sigma and 2^-11, below 0.9 of the clip level, no saturated
// sensor site). Each estimator returns the colour cast of neutrals relative to as shot as camera
// log ratios (log r/g, log b/g):
//   * GreyEdge: the first-order grey-edge hypothesis with Minkowski norm p = 6 (J. van de Weijer,
//     T. Gevers, A. Gijsenij, "Edge-Based Color Constancy", IEEE TIP 16(9), 2007) on Sobel
//     gradients of a 3x3 binomial smoothing (about a Gaussian derivative of sigma 1) at pixels
//     whose 3x3 neighbourhood is valid and whose green gradient exceeds the noise; confidence
//     min(1, n / 4000);
//   * BrightNeutral: median log ratios of the brightest 20 % of valid pixels within the plausible
//     cast window (|log ratio| <= 0.5); confidence min(1, n / 1000) x (1 - dispersion / 0.15);
//   * NeutralGreyWorld: luminance-weighted grey world restricted to the cast window; a weak
//     estimator, confidence 0.5 x min(1, n / 4000).
//   The cast is the component-wise median of the available estimates; the agreement factor
//   exp(-(spread / (0.05 + 0.25 |cast|))^2) (spread = largest distance between two estimates, |cast|
//   the combined cast's log norm; 0.4 with a single estimator) times the mean estimator confidence
//   is the proposal's confidence.
//   Natural v1 corrects small casts and preserves intentional illumination: the cast is split into
//   warmth w = (u_r - u_b) / 2 (warm > 0) and tint t = (u_r + u_b) / 2 (magenta > 0); the corrected
//   amounts are min(|w|, 0.03) + 0.25 clamp(|w| - 0.03, 0, 0.06) (at most 0.045, so a warm sunset or
//   a blue hour keeps nearly all of its colour) and min(|t|, 0.05) + 0.5 clamp(|t| - 0.05, 0, 0.06)
//   (at most 0.08), scaled by the confidence, and the resulting red and blue gains relative to as
//   shot are bounded to 1 +- kWhiteBalanceResidualBound.
//
// The constants are uncalibrated Natural v1 defaults (ADR 0011).
//
// Thread-safety: reentrant.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/analysis/render_metrics.hpp>
#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/develop.hpp>

namespace lucent::analysis {

// Residual white-balance bound: red and blue gains relative to as shot within 1 +- this.
inline constexpr double kWhiteBalanceResidualBound = 0.10;

// ---- Scene measurements ------------------------------------------------------------------------

// Stage two's clip level for the as-shot balance of `stage_one` (camera units): the lowest
// white-balanced saturation level of the channels in use (sensor.hpp, ClipToLowestSaturation).
[[nodiscard]] double as_shot_clip_level(const engine::StageOneImage& stage_one) noexcept;

struct SceneStats {
    double clip_level = 1.0;
    double headroom_ev = 0.0;       // log2(clip / p99.5 of the value norm); 0 when that reaches the clip
    double clipped_fraction = 0.0;  // pixels whose value norm reaches the clip level
    std::uint64_t pixels = 0;
    std::uint64_t signal_pixels = 0;  // green signal >= 3 sigma
    [[nodiscard]] double signal_fraction() const noexcept {
        return pixels == 0 ? 0.0 : static_cast<double>(signal_pixels) / static_cast<double>(pixels);
    }
};

// Errors (analysis.proposals): InvalidArgument for an empty image or a context of another size;
// Cancelled.
[[nodiscard]] Expected<SceneStats> measure_scene(const engine::StageOneImage& analysis,
                                                 const ImageContext& context, bool parallel = true,
                                                 const core::CancellationToken& cancel = {});

// ---- Exposure / headroom -----------------------------------------------------------------------

// The tone policy's constants (see the file comment; uncalibrated).
inline constexpr double kBandLow = 0.40;             // darkest accepted median, normal key, daylight
inline constexpr double kBandHigh = 0.85;            // brightest accepted median, normal key
inline constexpr double kLowKeyBandDrop = 0.38;      // band_low drop at low_key 1
inline constexpr double kDimBandDrop = 0.15;         // band_low drop at dimness 1
inline constexpr double kDaylightSceneEv = 10.0;     // dimness 0 at and above
inline constexpr double kNightSceneEv = 2.0;         // dimness 1 at and below
inline constexpr double kHighKeyBandRise = 0.08;     // band_high rise at high_key 1
inline constexpr double kBrightContentFloor = 0.62;  // p90 lightness below which bright content is dark
inline constexpr double kKeyAmbiguityEv = 0.25;

struct ToneProposal {
    bool evidence = false;
    double signal_evidence = 0.0;  // [0, 1]: scene signal above the noise (see the file comment)
    LightnessQuantiles baseline;   // the baseline render's quantiles (analysis image)
    std::optional<double> scene_ev;
    double low_key = 0.0;
    double dimness = 0.0;
    double high_key = 0.0;
    double band_low = kBandLow;  // key-aware band of the median luminance lightness
    double band_high = kBandHigh;
    double bright_floor = kBrightContentFloor;
    // The baseline's distances (EV, positive when on the wrong side; tone_deficit()).
    double median_deficit_ev = 0.0;
    double bright_deficit_ev = 0.0;
    double excess_ev = 0.0;
    bool key_ambiguous = false;
    double needed_ev = 0.0;  // the deficit (> 0) or minus the excess (< 0), before the headroom limit
    double headroom_ev = 0.0;
    double exposure_ev = 0.0;
    double shadows_ev = 0.0;     // EV-equivalent local lift (DevelopSettings::shadows x kShadowsMaxEv)
    double highlights_ev = 0.0;  // EV-equivalent local compression (<= 0)
    std::string reason;          // developer-facing English
};

// The distances of a render's quantiles from the policy's band and floor, in EV of display
// luminance: median_ev = log2(Y(band_low) / Y(p50)), bright_ev = log2(Y(bright_floor) / Y(p90)),
// deficit = max(0, min(median_ev, bright_ev)), excess = max(0, log2(Y(p50) / Y(band_high))), with
// Y = engine::display_lightness_inverse of the lightness (at least 0.001).
struct ToneDeficit {
    double median_ev = 0.0;
    double bright_ev = 0.0;
    double deficit_ev = 0.0;
    double excess_ev = 0.0;
};
[[nodiscard]] ToneDeficit tone_deficit(const LightnessQuantiles& quantiles, const ToneProposal& policy) noexcept;

// `baseline` is the baseline render's statistics on the analysis image; `scene_ev` the scene
// luminance (ShotFeatures::scene_ev), when the file has exposure metadata.
[[nodiscard]] ToneProposal propose_tone(const RenderStats& baseline, const SceneStats& scene,
                                        std::optional<double> scene_ev = std::nullopt);

// ---- White balance -----------------------------------------------------------------------------

// Persisted (append only).
enum class WhiteBalanceEstimator : std::uint8_t {
    GreyEdge = 1,
    BrightNeutral = 2,
    NeutralGreyWorld = 3,
};

[[nodiscard]] std::string_view to_string(WhiteBalanceEstimator estimator) noexcept;

struct WhiteBalanceEstimate {
    WhiteBalanceEstimator method = WhiteBalanceEstimator::GreyEdge;
    bool available = false;
    std::uint64_t samples = 0;
    double log_rg = 0.0;  // cast of neutrals relative to as shot: log(r / g)
    double log_bg = 0.0;  // log(b / g)
    double confidence = 0.0;
    std::string detail;
};

struct WhiteBalanceProposal {
    std::vector<WhiteBalanceEstimate> estimates;
    bool evidence = false;  // at least one estimator was available
    double cast_log_rg = 0.0;
    double cast_log_bg = 0.0;
    double warmth = 0.0;  // (log_rg - log_bg) / 2: warm > 0
    double tint = 0.0;    // (log_rg + log_bg) / 2: magenta > 0
    double spread = 0.0;
    double confidence = 0.0;
    // Proposed multipliers relative to as shot for camera red and blue (green 1), in
    // [1 - kWhiteBalanceResidualBound, 1 + kWhiteBalanceResidualBound].
    double red_gain = 1.0;
    double blue_gain = 1.0;
    double preserved_fraction = 1.0;  // of the cast (log norm) the proposal leaves
    std::string reason;
};

// Estimates on the analysis image (3 channels; monochrome data has no white balance and is
// InvalidArgument). Errors (analysis.proposals): InvalidArgument; OutOfMemory; Cancelled.
[[nodiscard]] Expected<WhiteBalanceProposal> propose_white_balance(
    const engine::StageOneImage& analysis, const ImageContext& context, double clip_level,
    bool parallel = true, const core::CancellationToken& cancel = {});

// The surfaces the white-balance evidence treats as neutral: valid pixels (as for the estimators)
// whose camera log ratios lie within `radius` of the estimated cast. Row-major, 1 = evidence, the
// size of `image` (3 channels). The white-balance hard check measures how far a candidate moves
// these surfaces in the render (render_metrics.hpp, ImageContext::neutral_evidence): under a strong
// cast nothing renders near neutral, yet the evidence still identifies the neutral surfaces. The
// controller uses the mask only when the proposal's confidence is at least
// kNeutralEvidenceMinConfidence: a cast the estimators do not agree on names arbitrary surfaces, so
// the check then falls back to the baseline's near-neutral pixels (C*ab < 5).
inline constexpr double kNeutralEvidenceRadius = 0.08;
inline constexpr double kNeutralEvidenceMinConfidence = 0.3;
[[nodiscard]] Expected<std::vector<std::uint8_t>> neutral_evidence_mask(
    const engine::StageOneImage& image, const ImageContext& context, double clip_level, double cast_log_rg,
    double cast_log_bg, double radius = kNeutralEvidenceRadius);

// The Natural v1 correction of a cast (see the file comment): the red and blue gains for a cast of
// (log_rg, log_bg) at `confidence`.
struct WhiteBalanceCorrection {
    double red_gain = 1.0;
    double blue_gain = 1.0;
    double preserved_fraction = 1.0;
};
[[nodiscard]] WhiteBalanceCorrection natural_v1_white_balance_correction(double log_rg, double log_bg,
                                                                         double confidence) noexcept;

}  // namespace lucent::analysis
