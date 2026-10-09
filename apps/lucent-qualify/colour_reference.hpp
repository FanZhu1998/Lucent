#pragma once

// The independent colour reference of the qualification suite: check colour_reference (ADR 0020).
//
// For a fixture whose photograph shows a colour chart (charts.hpp), Lucent's develop path is
// measured against the chart manufacturer's published values, which depend on neither Lucent nor
// LibRaw:
//
// 1. Measurement (measure_chart()): the export render of the develop path at its as-shot defaults
//    (DevelopSettings{}: as-shot white balance, no exposure, geometry version 2), taken apart into
//    its two stages so that its scene-linear values are available before the base look: stage one
//    (kind Export: white balance before demosaicing, highlights clipped) and stage two's report,
//    whose operation 1 is out = gain * M_ws * camera_to_xyz * min(s * d, clip) (develop.hpp).
//    Every patch's box (in the camera-area frame, whose position the stage-two report gives) is
//    averaged in white-balanced camera RGB and mapped with that same operation, giving the
//    develop path's scene-linear Rec.2020 / D65 mean of the patch. Samples at or above the clip
//    level are counted.
// 2. Evaluation (evaluate_chart()), identical to research/tools/chart_reference.py:
//    * excluded: patches with more than kColourReferenceClippedFraction of their box's samples at
//      the clip level (their colour is not measured); the chart is not measurable when its
//      normalisation patch is excluded or fewer than kColourReferenceMinMeasured of its patches
//      remain;
//    * "as shot": XYZ (D65) adapted to D50 (Bradford), scaled so that the normalisation patch's Y
//      equals its reference Y (D50 white, Y = 1), CIELAB (D50), CIEDE2000 against the reference;
//    * "chart balanced" (gated): one Bradford von Kries transform that maps the measured
//      normalisation patch exactly onto its reference XYZ: exposure normalisation and chromatic
//      adaptation of the scene's white, as the chart records it, to the reference white. It
//      removes the as-shot white balance and exposure, which belong to the capture (the camera's
//      estimate of the scene's light), and leaves what the calibration and its application do to
//      colour: the camera matrix, Lucent's colour stages, the working space;
//    * statistics: mean, 90th percentile (linear interpolation between order statistics, as
//      numpy's default) and maximum of the CIEDE2000 values (kL = kC = kH = 1, Sharma et al. 2005).
// 3. Gate: the chart-balanced mean, 90th percentile and maximum must not exceed
//    kColourReferenceMeanLimit, kColourReferenceP90Limit and kColourReferenceMaxLimit (1.5 times
//    the largest value an independent rendering of a corpus chart reaches, rounded up to half a
//    unit; ADR 0020 gives the measurements, the literature and the simulated defects the gate
//    catches and misses). As-shot statistics are reported, not gated.

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <lucent/core/error.hpp>
#include <lucent/image/color_math.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/raw/decoder_registry.hpp>

#include "charts.hpp"

namespace lucent::qualify {

// The gate (chart-balanced CIEDE2000; ADR 0020).
inline constexpr double kColourReferenceMeanLimit = 4.5;
inline constexpr double kColourReferenceP90Limit = 7.5;
inline constexpr double kColourReferenceMaxLimit = 12.5;
// A patch is excluded when more than this fraction of its box's samples are clipped.
inline constexpr double kColourReferenceClippedFraction = 0.001;
// The chart is measurable when at least this fraction of its patches are measured.
inline constexpr double kColourReferenceMinMeasured = 0.75;

// One patch as measured.
struct ChartSample {
    std::string patch;
    image::Vec3d camera_mean{};   // white-balanced camera R, G, B (clipped as operation 1 clips)
    image::Vec3d working_mean{};  // the develop path's scene-linear Rec.2020 / D65 (operation 1)
    std::uint64_t pixels = 0;
    std::uint64_t clipped_samples = 0;  // of pixels * 3 samples, at or above the clip level

    [[nodiscard]] double clipped_fraction() const noexcept;
};

// What measure_chart() found besides the samples.
struct ChartMeasurement {
    std::vector<ChartSample> samples;  // in the chart fixture's box order
    image::PixelRect frame;            // the develop render's geometry frame in its upright image
    image::Matrix3d camera_to_working;  // gain * M_ws * camera_to_xyz of operation 1
    image::Vec3d correction{1.0, 1.0, 1.0};  // stage two's white-balance correction d
    double clip_level = 0.0;
    double gain = 1.0;
    std::string calibration_path;  // engine::CalibrationPath name
};

// Measures the chart of `chart` on the develop path of `file` (subimage 0). Errors: those of the
// engine's stages; InvalidArgument ("qualify.colour_reference") when the develop render's frame
// differs from the chart definition's frame (size and origin: the boxes would not locate the
// patches) or the image is not 3-channel colour.
[[nodiscard]] Expected<ChartMeasurement> measure_chart(const raw::DecoderRegistry& registry,
                                                       const std::filesystem::path& file,
                                                       const ChartFixture& chart);

struct ChartPatchResult {
    std::string patch;
    std::array<double, 3> reference_lab{};
    std::array<double, 3> as_shot_lab{};
    double as_shot_delta_e = 0.0;
    std::array<double, 3> balanced_lab{};
    double balanced_delta_e = 0.0;
};

struct ChartEvaluation {
    bool measurable = false;
    std::string problem;                       // why it is not measurable
    std::vector<ChartPatchResult> patches;     // measured patches, in sample order
    std::map<std::string, double> excluded;    // patch -> clipped fraction
    double normalisation_scale = 0.0;          // the as-shot exposure normalisation (after the adaptation)
    ChartStatistics as_shot;
    ChartStatistics balanced;                  // gated
    // Statistics against the fixture's alternate references (sensitivity, not gated).
    std::map<std::string, std::pair<ChartStatistics, ChartStatistics>> alternates;  // as shot, balanced
    bool passes = false;                       // measurable and within the gate

    [[nodiscard]] std::string summary() const;
};

// The gate on chart-balanced statistics.
[[nodiscard]] bool within_colour_reference_gate(const ChartStatistics& balanced) noexcept;

// q-th quantile (0 <= q <= 1) with linear interpolation between order statistics (numpy's
// default): position q (n - 1) of the sorted values. 0 for an empty set.
[[nodiscard]] double quantile(std::vector<double> values, double q);

// CIELAB of XYZ relative to `white` (XYZ of the white, Y = 1), and its inverse.
[[nodiscard]] std::array<double, 3> lab_from_xyz(const image::Vec3d& xyz, const image::Vec3d& white) noexcept;
[[nodiscard]] image::Vec3d xyz_from_lab(const std::array<double, 3>& lab, const image::Vec3d& white) noexcept;

// Evaluates `samples` of `chart` (their working_mean: linear RGB whose XYZ is working_to_xyz *
// working_mean, relative to the white `working_white`). Patches without a sample are not measured.
[[nodiscard]] ChartEvaluation evaluate_chart(const ChartDefinitions& charts, const ChartFixture& chart,
                                             const std::vector<ChartSample>& samples,
                                             const image::Matrix3d& working_to_xyz,
                                             image::Chromaticity working_white);

// working_to_xyz for the develop path's working space (Rec.2020 primaries, D65 white).
[[nodiscard]] const image::Matrix3d& rec2020_to_xyz();

}  // namespace lucent::qualify
