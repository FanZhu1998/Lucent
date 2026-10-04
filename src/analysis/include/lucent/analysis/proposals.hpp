#pragma once

// Deterministic proposals of the automatic settings (blueprint section 6, "Analysis and candidate
// search" step 2): the histogram/headroom exposure proposal and evidence-based white-balance
// proposals, each with its evidence and confidence. Proposals seed the bounded search
// (auto_settings.hpp); the Natural v1 objective (objective.hpp) then decides.
//
// Scene measurements use the analysis image's stage one: white-balanced (as shot) camera RGB,
// scene-linear, highlights not clipped (StageOneKind::Interactive), upright.
//
// Exposure / headroom (ToneProposal):
//   * evidence: pixels whose scene signal exceeds 3 sigma of the noise model; at least
//     kMinSignalPixels and 10 % of the image, else no exposure proposal and confidence 0;
//   * key: low_key = clamp((p99 - 0.75) / 0.15) x clamp((0.30 - p50) / 0.15) of the baseline's
//     luminance lightness - bright content near white together with a dark median (silhouettes,
//     night scenes, low-key portraits) - widens the band downwards: band_low = 0.40 - 0.38 low_key
//     (a fully low-key composition is not brightened at all), band_high = 0.85 (snow, fog and white
//     backgrounds keep most of their key);
//   * the exposure that moves the median lightness into the band, exactly in display luminance
//     (log2 of the ratio of display_lightness_inverse values: the base look's mid-tones are a pure
//     gain), limited upwards by the scene headroom minus 0.3 EV (p99.5 of the value norm against
//     the stage-two clip level) and downwards to -1 EV; what the headroom withholds becomes half
//     as much shadow lift (up to 0.75 EV, not for low-key scenes); a brightening (more than
//     0.25 EV) of an image with highlights near white (more than 1 % / 3 % of pixels) proposes
//     -0.25 / -0.5 EV of local highlight compression to protect them.
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

struct ToneProposal {
    bool evidence = false;
    double confidence = 0.0;
    double low_key = 0.0;
    double band_low = 0.40;  // key-aware band of the median luminance lightness
    double band_high = 0.85;
    double needed_ev = 0.0;  // to reach the band, before the headroom limit
    double headroom_ev = 0.0;
    double exposure_ev = 0.0;
    double shadows_ev = 0.0;     // EV-equivalent local lift (DevelopSettings::shadows x kShadowsMaxEv)
    double highlights_ev = 0.0;  // EV-equivalent local compression (<= 0)
    std::string reason;          // developer-facing English
};

// `baseline` is the baseline render's statistics on the analysis image.
[[nodiscard]] ToneProposal propose_tone(const RenderStats& baseline, const SceneStats& scene);

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
// cast nothing renders near neutral, yet the evidence still identifies the neutral surfaces.
inline constexpr double kNeutralEvidenceRadius = 0.08;
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
