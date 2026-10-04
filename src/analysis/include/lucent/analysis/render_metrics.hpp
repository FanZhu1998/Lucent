#pragma once

// Measurements of rendered candidates (blueprint section 6, metric table; metric_contract.hpp).
// A candidate is the develop pipeline's stage two on a cached stage one (the analysis image or the
// search proxy); its output is display-linear Rec.2020 / D65 (engine::display_linear_rec2020(),
// max(R, G, B) <= 1, negative components out of gamut). Everything is measured per pixel and
// aggregated deterministically (fixed row bands, integer histograms, sums merged in band order).
//
// Per pixel:
//   * display luminance Y (BT.2020 coefficients) and its lightness l = L*/100
//     (engine::display_lightness()); CIELAB a*, b* relative to the D65 display white (hue, neutral
//     shift, the chromatic and neutral masks);
//   * linear-light chroma |d - Y (1, 1, 1)| / Y of the display-linear pixel d: exactly invariant to
//     every ratio-preserving operation (exposure, local tone, the lightness curves without a lifted
//     black, the base look) and exactly scaled by the saturation slider's factor, so the colour
//     metric's saturation change measures colour edits and white balance only (CIELAB chroma
//     relative to lightness would also move with exposure in the dark linear segment);
//   * the declared output encoding (metric_contract.hpp): linear sRGB s = M(Rec.2020 -> BT.709) d,
//     highlight clipped when max(s) >= clip_high_linear() (code 255), blocked when max(s) <=
//     block_low_linear() (every channel at code 0), out of gamut when min(s) < -kGamutTolerance
//     max(s) or max(s) > 1 + kGamutTolerance;
//   * the predicted shadow noise: with the scene signal x (stage one's balanced green, 1 = white
//     level) and the noise model's sigma(x), the local display gain k = Y / x carries the sensor
//     noise into display luminance and the lightness slope l'(Y) into L* units:
//         sigma_L = 100 l'(Y) k sigma(x)        (native-resolution noise, not the proxy's average);
//     the secant gain is exact for the scene-linear gains of exposure, local tone and white balance
//     and a first-order estimate through the base look and curves.
// Masks (all relative to the baseline's own features, so candidates are compared on the same
// pixels): shadows 0.02 < l_base < 0.35 with no saturated site and x > 0; chromatic C*_base >= 8 and
// 0.25 < l_base < 0.95, unclipped in both renders; neutral: the white-balance evidence's neutral
// surfaces (ImageContext::neutral_evidence) or, without evidence, C*_base < 5, in both cases with
// 0.15 < l_base < 0.9 and unclipped in the baseline; source saturated: saturation coverage >= 0.5.
//
// Thread-safety: reentrant.

#include <cstdint>
#include <vector>

#include <lucent/analysis/metric_contract.hpp>
#include <lucent/analysis/source_diagnostics.hpp>
#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/camera_color.hpp>
#include <lucent/engine/develop.hpp>
#include <lucent/image/geometry.hpp>

namespace lucent::analysis {

// ---- Mask and class levels ---------------------------------------------------------------------

inline constexpr double kNearWhiteLightness = 0.93;  // value-norm lightness: crowded into the shoulder
inline constexpr double kDarkLightness = 0.10;       // luminance lightness: near black (L* 10)
inline constexpr double kShadowMaskLow = 0.02;
inline constexpr double kShadowMaskHigh = 0.35;
inline constexpr double kChromaticChroma = 8.0;
// Chromatic pixels start at L* 25: below, CIELAB's linear segment and sensor noise make C*/(L*+16)
// depend on exposure and noise rather than on colour.
inline constexpr double kChromaticLightnessLow = 0.25;
inline constexpr double kNeutralChroma = 5.0;
inline constexpr float kSaturatedCoverage = 0.5f;

// Linear sRGB levels of the 8-bit code boundaries (sRGB curve decoded at codes 254.5 and 0.5).
[[nodiscard]] double clip_high_linear() noexcept;
[[nodiscard]] double block_low_linear() noexcept;

// Output-encoding classes of a pixel (bit flags).
inline constexpr std::uint8_t kFlagHighlightClipped = 1;
inline constexpr std::uint8_t kFlagShadowBlocked = 2;
inline constexpr std::uint8_t kFlagOutOfGamut = 4;
inline constexpr std::uint8_t kFlagNonFinite = 8;

// ---- Context and features ----------------------------------------------------------------------

// Per-pixel data of one resolution that does not depend on the candidate.
struct ImageContext {
    image::Size size;
    // Fraction of saturated sensor sites per pixel (row-major); empty when unavailable.
    std::vector<float> saturation;
    // Scene signal per pixel: stage one's balanced green (monochrome: its channel), normalised.
    std::vector<float> signal;
    // The surfaces the white-balance evidence treats as neutral (proposals.hpp,
    // neutral_evidence_mask()); empty: the baseline render's near-neutral pixels are used instead.
    std::vector<std::uint8_t> neutral_evidence;
    SensorNoiseModel noise;
};

// The context of a stage-one image (camera RGB or monochrome, any kind). `coverage` must have the
// image's size when given. Errors (analysis.metrics): InvalidArgument for an empty image, a
// coverage map of another size; OutOfMemory.
[[nodiscard]] Expected<ImageContext> make_image_context(const engine::StageOneImage& stage_one,
                                                        const CoverageMap* coverage,
                                                        const SensorNoiseModel& noise);

// Per-pixel features of the baseline render, the reference of every comparison.
struct BaselineFeatures {
    image::Size size;
    std::vector<float> lightness;  // l = L*/100 of the display luminance
    std::vector<float> a;          // CIELAB a* (D65)
    std::vector<float> b;          // CIELAB b* (D65)
    std::vector<float> chroma;     // linear-light chroma |d - Y| / Y (0 for Y <= 0)
    std::vector<std::uint8_t> flags;
};

struct LightnessQuantiles {
    double p01 = 0.0;
    double p05 = 0.0;
    double p10 = 0.0;
    double p25 = 0.0;
    double p50 = 0.0;
    double p75 = 0.0;
    double p90 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double p995 = 0.0;

    friend bool operator==(const LightnessQuantiles&, const LightnessQuantiles&) = default;
};

// Aggregated measurements of one render, its own and relative to the baseline (zero for the
// baseline itself).
struct RenderStats {
    image::Size size;
    std::uint64_t pixels = 0;
    std::uint64_t nonfinite_samples = 0;

    // Tonal distribution (luminance lightness).
    LightnessQuantiles quantiles;
    double mean_lightness = 0.0;
    std::uint64_t signal_pixels = 0;  // scene signal >= 3 sigma (or >= 2^-10 without a noise model)
    std::uint64_t near_white = 0;     // value-norm lightness >= kNearWhiteLightness, not clipped
    std::uint64_t dark = 0;           // lightness < kDarkLightness

    // Output encoding.
    std::uint64_t highlight_clipped = 0;
    std::uint64_t shadow_blocked = 0;
    std::uint64_t out_of_gamut = 0;
    std::uint64_t new_highlight_clipped = 0;
    std::uint64_t new_shadow_blocked = 0;
    std::uint64_t new_out_of_gamut = 0;
    std::uint64_t source_saturated = 0;  // pixels with saturation coverage >= kSaturatedCoverage
    double saturated_greying = 0.0;      // mean max(0, l_base - l) over source-saturated pixels

    // Colour.
    std::uint64_t chromatic_pixels = 0;
    double saturation_log_ratio = 0.0;  // log(sum chroma / the baseline's sum) over chromatic pixels
    double hue_shift_degrees = 0.0;     // C*_base-weighted mean |hue change|
    double mean_chroma = 0.0;           // mean C*ab of every pixel
    std::uint64_t neutral_pixels = 0;
    double neutral_shift_de = 0.0;  // |mean (Delta a*, Delta b*)| over near-neutral baseline pixels

    // Noise.
    bool noise_model_available = false;  // the context's noise model (shadow_noise_lstar is 0 without one)
    std::uint64_t shadow_pixels = 0;
    double shadow_noise_lstar = 0.0;  // p75 of the predicted noise, L* units

    double seconds = 0.0;

    [[nodiscard]] double fraction(std::uint64_t count) const noexcept {
        return pixels == 0 ? 0.0 : static_cast<double>(count) / static_cast<double>(pixels);
    }
    // Interquartile range of the lightness.
    [[nodiscard]] double iqr() const noexcept { return quantiles.p75 - quantiles.p25; }
};

// Measures `display` (display-linear Rec.2020, 3 channels, the context's size). Without `baseline`
// the render is the baseline: its relative values are zero and `features` (when not null) receives
// its per-pixel features. Errors (analysis.metrics): InvalidArgument for another size, channel
// count or encoding, or baseline features of another size; OutOfMemory; Cancelled.
[[nodiscard]] Expected<RenderStats> measure_render(const engine::ColorImage& display,
                                                   const ImageContext& context,
                                                   const BaselineFeatures* baseline,
                                                   BaselineFeatures* features, bool parallel = true,
                                                   const core::CancellationToken& cancel = {});

}  // namespace lucent::analysis
