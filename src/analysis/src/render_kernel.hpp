#pragma once

// Private: the per-pixel arithmetic and the per-band partial results of measure_render()
// (render_metrics.hpp), shared with the band measurement of full-resolution renders
// (frame_measurement.hpp), so that both give bit-identical statistics: the same operations in the
// same order on the same values, accumulated into partials of kMetricBandRows rows (pixels in
// row-major order within a band) and merged in band order.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <lucent/analysis/render_metrics.hpp>
#include <lucent/analysis/source_diagnostics.hpp>
#include <lucent/core/error.hpp>
#include <lucent/image/color_math.hpp>

namespace lucent::analysis::detail {

inline constexpr std::size_t kLightnessBins = 4096;  // over l in [0, 1]
inline constexpr std::size_t kNoiseBins = 2000;      // over sigma_L in [0, 40) L* units
inline constexpr double kNoiseBinWidth = 0.02;
inline constexpr std::int32_t kMetricBandRows = 32;

struct MetricMatrices {
    image::Matrix3d to_xyz;   // display-linear Rec.2020 -> XYZ (D65, Y of white = 1)
    image::Matrix3d to_srgb;  // display-linear Rec.2020 -> linear sRGB
    image::Vec3d white;       // XYZ of RGB (1, 1, 1)
};

[[nodiscard]] Expected<MetricMatrices> metric_matrices();

// One band's sums and histograms (integer histograms and in-order sums: schedule independent).
struct MetricPartial {
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

// `count` empty partials with their histograms. OutOfMemory (analysis.metrics).
[[nodiscard]] Status make_partials(std::vector<MetricPartial>& partials, std::size_t count);

// The context of one row of the measured frame: `width` values each; saturation null: no
// saturation information (coverage 0); neutral null: the baseline's near-neutral pixels.
struct ContextRow {
    const float* signal = nullptr;
    const float* saturation = nullptr;
    const std::uint8_t* neutral = nullptr;
};

// Per-pixel features of one row (BaselineFeatures' members): written by measure_row() and read as
// the baseline of another render's row.
struct FeatureRow {
    float* lightness = nullptr;
    float* a = nullptr;
    float* b = nullptr;
    float* chroma = nullptr;
    std::uint8_t* flags = nullptr;
};
struct BaselineRow {
    const float* lightness = nullptr;
    const float* a = nullptr;
    const float* b = nullptr;
    const float* chroma = nullptr;
    const std::uint8_t* flags = nullptr;
};

// The per-pixel measurement of measure_render().
class MetricKernel {
public:
    MetricKernel(const MetricMatrices& matrices, const SensorNoiseModel& noise) noexcept;

    // Accumulates the `width` pixels of `row` (display-linear Rec.2020 RGB, 3 floats each) into
    // `partial`. `baseline` null: the row is the baseline itself (its relative values are zero).
    // `features` (optional) receives the row's own features (a non-finite pixel: zeros and
    // kFlagNonFinite); `log_y` (optional) its log2 luminance for local contrast (left unchanged for
    // a non-finite pixel).
    void measure_row(MetricPartial& partial, const float* row, std::int32_t width, const ContextRow& context,
                     const BaselineRow* baseline, const FeatureRow* features, float* log_y) const noexcept;

    // The features of `row` only (what measure_row() writes to `features`, bit for bit).
    void features_row(const float* row, std::int32_t width, const FeatureRow& features) const noexcept;

private:
    const MetricMatrices& m_;
    const SensorNoiseModel& noise_;
    bool noise_available_ = false;
    double xn_ = 1.0;
    double yn_ = 1.0;
    double zn_ = 1.0;
    double hi_level_ = 1.0;
    double lo_level_ = 0.0;
    double floor_y_ = 0.0;
};

// The partials of a render merged in band order: merge() adds one band after another (the same
// additions in the same order however many bands are kept at a time).
struct MetricTotal {
    std::vector<std::uint64_t> lightness_hist = std::vector<std::uint64_t>(kLightnessBins, 0);
    std::vector<std::uint64_t> noise_hist = std::vector<std::uint64_t>(kNoiseBins, 0);
    MetricPartial sums;  // its histograms unused

    void merge(const MetricPartial& p) noexcept;
};

// The statistics of a `size` render from its merged partials; local contrast is the caller's (left
// 0, radius 0).
[[nodiscard]] RenderStats finish_stats(const MetricTotal& total, image::Size size, bool noise_model_available);
// finish_stats() of the partials merged in order.
[[nodiscard]] RenderStats finish_stats(const std::vector<MetricPartial>& partials, image::Size size,
                                       bool noise_model_available);

}  // namespace lucent::analysis::detail
