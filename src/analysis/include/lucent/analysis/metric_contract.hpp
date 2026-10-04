#pragma once

// The metric contract of Lucent's automatic settings (blueprint section 6, "Metric contract: hard
// checks before aesthetic ranking"; ADR 0011). Every metric declares the domain and encoding it is
// measured in, the mask of samples that count, its units, the baseline it is compared with, its
// thresholds, the minimum number of valid samples, and what happens when there are fewer:
//
//   * Hard checks gate feasibility. A candidate that fails one is never ranked, whatever its
//     preference score (numerical validity, render-induced clipping, blocked shadows, gamut and
//     hue damage, implausible neutral shifts, harmful noise amplification, halos).
//   * Preference features feed the normalised Natural v1 objective (objective.hpp).
//   * Too little evidence never scores well: a metric below its valid-sample minimum is
//     InsufficientEvidence. A hard check without evidence makes a candidate infeasible when the
//     candidate changes what the check guards (any change of an image too small to measure, a
//     white-balance residual without neutral evidence); the baseline, the reference itself, stays
//     feasible. Where the contract says so (shadow noise without a noise model, colour without
//     chromatic pixels, halos without strong edges) and for every preference term, missing evidence
//     is replaced by a penalty proportional to the edit it would govern (objective.hpp), so the
//     search makes the smaller edit, and the result carries a review flag.
//
// Source clipping (sensor saturation), total output clipping and newly induced clipping are
// reported separately. Propagated sensor masks support interpretation (source saturation, the
// greying of clipped areas) but never exempt pixels from the render-induced checks.
//
// The thresholds and scales below are Natural v1 engineering defaults (policy version 1). They are
// UNCALIBRATED: blueprint section 10 requires calibration on a qualification corpus (blind
// preference and correction-effort tests, split by camera and trip) before they are frozen; ADR
// 0011 records the plan. Do not cite them as measured limits.
//
// Thread-safety: the specification table is immutable; MetricResult is a value type.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lucent::analysis {

// Version of the metric definitions below (bump when a metric is measured differently).
inline constexpr std::uint32_t kMetricContractVersion = 1;

// ---- Declared output encoding ------------------------------------------------------------------

// Clipping, blocked shadows and gamut are judged in sRGB (IEC 61966-2-1: BT.709 primaries, D65,
// the sRGB curve) quantised to 8 bits, the default export and screen encoding. The render's
// display-linear Rec.2020 output is converted to linear sRGB by the colorimetric matrix (same D65
// white, no adaptation); a sample is clipped high when it would encode to code 255 (linear value
// at or above the decoded level of code 254.5), blocked when every channel encodes to code 0
// (linear value at or below the decoded level of code 0.5), and out of gamut when, before the
// encoder clips it, a channel lies below -kGamutTolerance times the pixel's largest channel (a
// chromaticity outside the sRGB triangle, whatever the pixel's level) or above 1 + kGamutTolerance.
inline constexpr std::string_view kOutputEncodingName = "srgb-8bit";
inline constexpr double kGamutTolerance = 1e-3;

// ---- Natural v1 thresholds (uncalibrated) ------------------------------------------------------

// Hard checks.
inline constexpr double kMaxNewHighlightClipFraction = 0.005;  // pixels newly at code 255 vs baseline
inline constexpr double kMaxNewShadowBlockFraction = 0.005;    // pixels newly at code 0 vs baseline
inline constexpr double kMaxNewGamutFraction = 0.02;           // pixels newly outside the sRGB gamut
inline constexpr double kMaxHueShiftDegrees = 6.0;             // chroma-weighted mean CIELAB hue change
inline constexpr double kMaxNeutralShiftDeltaE = 8.0;          // mean a*b* shift of baseline neutrals
inline constexpr double kMaxShadowNoiseLStar = 4.0;            // predicted shadow noise, L* units, AND
inline constexpr double kMaxShadowNoiseAmplification = 1.5;    //   more than this times the baseline's
inline constexpr double kMaxHaloLStar = 4.0;                   // native-crop halo next to strong edges, L*
inline constexpr double kMaxNativeNoiseLStar = 4.0;            // native-crop shadow texture, L* units, AND
inline constexpr double kMaxNativeNoiseAmplification = 1.5;    //   more than this times the baseline's

// Report thresholds (review flags; they never change a candidate's feasibility).
inline constexpr double kSourceSaturationFlagFraction = 0.001;  // any sensor channel
inline constexpr double kVisibleNoiseLStar = 2.0;               // predicted noise considered visible

// Valid-sample minimums.
inline constexpr std::uint64_t kMinPixels = 4096;           // whole-image metrics (output encoding, tones)
inline constexpr std::uint64_t kMinSignalPixels = 4096;     // pixels with scene signal above 3 sigma
inline constexpr std::uint64_t kMinShadowPixels = 1024;     // shadow-noise mask
inline constexpr std::uint64_t kMinChromaticPixels = 1024;  // hue/chroma mask
inline constexpr std::uint64_t kMinNeutralPixels = 512;     // neutral-drift mask
inline constexpr std::uint64_t kMinEdgeSamples = 64;        // native-crop edge pixels
inline constexpr std::uint64_t kMinCropPixels = 4096;       // native-crop evaluation region

// ---- Identifiers -------------------------------------------------------------------------------

// Persisted metric identifiers (append only).
enum class MetricId : std::uint8_t {
    NumericalValidity = 1,  // NaN/Inf, dimensions, valid and monotone transforms
    SourceSaturation = 2,   // per-sensor-channel saturation before reconstruction
    RenderClipping = 3,     // highlight clipping in the output encoding: source, total, newly induced
    ShadowBlocking = 4,     // pixels blocked to code 0: total and newly induced
    TonalDistribution = 5,  // luminance quantiles, headroom, near-white crowding, key
    WhiteBalance = 6,       // residual neutral change and evidence confidence
    ShadowNoise = 7,        // predicted noise amplification from the sensor noise model
    NativeDetail = 8,       // halos, local detail and shadow texture on native-resolution crops
    ColorGamut = 9,         // hue/chroma change, neutral drift, out-of-gamut fraction
};

// Result status of one metric for one candidate (persisted; append only).
enum class MetricStatus : std::uint8_t {
    Pass = 1,                  // enough evidence; within every hard threshold
    Fail = 2,                  // a hard threshold is exceeded: the candidate is infeasible
    InsufficientEvidence = 3,  // fewer valid samples than the minimum: never scores well
    NotApplicable = 4,         // the metric does not apply (colour metrics of monochrome data, ...)
};

[[nodiscard]] std::string_view to_string(MetricId id) noexcept;          // "numerical_validity", ...
[[nodiscard]] std::string_view to_string(MetricStatus status) noexcept;  // "pass", "fail", ...

// ---- Specification -----------------------------------------------------------------------------

struct MetricThreshold {
    std::string_view name;  // the measurement it applies to (a MetricResult value name)
    double value = 0.0;
    std::string_view meaning;  // developer-facing English
};

struct MetricSpec {
    MetricId id = MetricId::NumericalValidity;
    std::string_view name;
    bool hard_check = false;  // gates feasibility
    bool preference = false;  // feeds the objective
    std::string_view domain;  // where and in which encoding it is measured
    std::string_view mask;    // which samples count
    std::string_view units;
    std::string_view baseline;  // what it is compared with
    std::span<const MetricThreshold> thresholds;
    std::uint64_t min_valid_samples = 0;
    std::string_view missing_data;  // behaviour below min_valid_samples
};

// The specification of every metric, in MetricId order.
[[nodiscard]] std::span<const MetricSpec> metric_specs() noexcept;
// The specification of `id` (every enumerator has one).
[[nodiscard]] const MetricSpec& metric_spec(MetricId id) noexcept;

// ---- Results -----------------------------------------------------------------------------------

struct NamedValue {
    std::string name;
    double value = 0.0;

    friend bool operator==(const NamedValue&, const NamedValue&) = default;
};

// One metric measured for one candidate (or the source).
struct MetricResult {
    MetricId id = MetricId::NumericalValidity;
    MetricStatus status = MetricStatus::NotApplicable;
    std::uint64_t valid_samples = 0;
    // Named measurements; the names are listed in the metric's documentation (render_metrics.hpp,
    // native_crops.hpp) and are stable.
    std::vector<NamedValue> values;
    // Developer-facing English: the failed threshold, the missing evidence.
    std::string detail;

    // The value named `name`, if present.
    [[nodiscard]] std::optional<double> value(std::string_view name) const;

    friend bool operator==(const MetricResult&, const MetricResult&) = default;
};

}  // namespace lucent::analysis
