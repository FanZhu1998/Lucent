#pragma once

// Source diagnostics of the automatic settings (blueprint section 6, "Analysis and candidate
// search" step 1, and the metric "Source saturation"): measured on the decoded RAW samples before
// stage one, so before black subtraction, white balance, demosaicing and any resampling ("Track
// source saturation before resampling", section 5).
//
//   * Per-sensor-channel saturation: every visible site of each channel index (both Bayer greens
//     separately) whose stored value is at or above the white level (SensorLevels::white, the
//     engine's saturation threshold, sensor.hpp). It reports an acquisition limitation; it never
//     blames the edit.
//   * Saturation coverage of upright analysis grids: for every cell of a requested grid (the
//     analysis image, the search proxy) the fraction of its sensor sites that were saturated. A
//     visible site (vx, vy) belongs to stored cell (floor(vx gw / W), floor(vy gh / H)) of the
//     grid in stored orientation, which is then mapped to the upright grid with the file's
//     orientation; at cell boundaries the assignment can differ from the box downscale of the
//     upright image by one site.
//   * The sensor noise model, for the shadow-noise metric: the variance of a normalised green
//     sample x (0 = black, 1 = white level) is read_variance + shot_scale x, from
//       1. the DNG NoiseProfile (51041) when present: shot_scale = S, read_variance = O of the green
//          plane (plane 1 of three, otherwise plane 0) - the camera's own model;
//       2. otherwise the read noise measured on the optical-black margins of the raw frame (1.4826
//          x the median absolute deviation of the green-channel samples of the left and top
//          margins, at least kMinMaskedSamples samples, median within kMaskedBlackTolerance of the
//          black level) and the shot noise of an assumed full well (kAssumedFullWellAtIso100
//          photo-electrons at the white level at ISO 100, scaled by 100 / ISO);
//       3. without margins, the read noise also from the ISO assumption
//          (kAssumedReadNoiseAtIso100 x sqrt(max(1, ISO / 100)));
//       4. without ISO, the measured read noise alone (shot noise at the ISO 100 assumption), or no
//          model at all (Unavailable).
//     The assumptions are uncalibrated engineering defaults (ADR 0011); every model records its
//     source and a confidence (1, 0.9, 0.7, 0.6, 0) used by the confidence of the result.
//
// Thread-safety: reentrant.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/image/cfa.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/raw/decoded_raw.hpp>

namespace lucent::analysis {

// ---- Noise model assumptions (uncalibrated) ----------------------------------------------------

inline constexpr double kAssumedFullWellAtIso100 = 20000.0;  // photo-electrons at the white level
inline constexpr double kAssumedReadNoiseAtIso100 = 3.0e-4;  // of the white level (about 5 DN of 14 bits)
inline constexpr std::uint64_t kMinMaskedSamples = 4096;
inline constexpr double kMaskedBlackTolerance = 0.02;  // of (white - black): masked median vs black level

// ---- Saturation --------------------------------------------------------------------------------

struct ChannelSaturation {
    image::CfaColor color = image::CfaColor::Red;  // RepresentationInfo::channel_colors entry
    std::uint64_t sites = 0;                       // visible samples of this channel index
    std::uint64_t saturated = 0;                   // stored value >= threshold
    double threshold = 0.0;                        // stored units (SensorLevels::white)

    [[nodiscard]] double fraction() const noexcept {
        return sites == 0 ? 0.0 : static_cast<double>(saturated) / static_cast<double>(sites);
    }

    friend bool operator==(const ChannelSaturation&, const ChannelSaturation&) = default;
};

// Saturated fraction of the sensor sites of every cell of an upright grid, row-major.
struct CoverageMap {
    image::Size size;
    std::vector<float> fraction;

    [[nodiscard]] bool empty() const noexcept { return fraction.empty(); }
    // Precondition: inside the grid.
    [[nodiscard]] float at(std::int32_t x, std::int32_t y) const noexcept {
        return fraction[static_cast<std::size_t>(y) * static_cast<std::size_t>(size.width) +
                        static_cast<std::size_t>(x)];
    }
};

// The saturation of every pixel of the upright full-resolution grid (the visible area; one cell per
// sensor site) in the three classes measurements distinguish, one byte per pixel: what a CoverageMap
// of that grid holds in a float (render_metrics.hpp compares a coverage with 0 and with
// kSaturatedCoverage, the neutral evidence with 0). Used by the full-resolution final check
// (frame_measurement.hpp), so that it keeps 1 byte per pixel instead of 4.
inline constexpr std::uint8_t kNotSaturated = 0;      // no saturated sample (coverage 0)
inline constexpr std::uint8_t kPartlySaturated = 1;   // some, below half (0 < coverage < 0.5)
inline constexpr std::uint8_t kMostlySaturated = 2;   // half or more (coverage >= 0.5)

struct SaturationClasses {
    image::Size size;                   // the upright visible size
    std::vector<std::uint8_t> classes;  // row-major

    [[nodiscard]] bool empty() const noexcept { return classes.empty(); }
};

// A coverage that every comparison of the measurements treats as the class does: 0, 0.25, 1.
[[nodiscard]] constexpr float saturation_class_coverage(std::uint8_t saturation_class) noexcept {
    return saturation_class == kNotSaturated ? 0.0f : saturation_class == kPartlySaturated ? 0.25f : 1.0f;
}

// ---- Noise model -------------------------------------------------------------------------------

// Persisted (append only).
enum class NoiseModelSource : std::uint8_t {
    DngNoiseProfile = 1,   // the file's NoiseProfile
    MaskedAreaAndIso = 2,  // read noise measured on the optical black, shot noise from the ISO assumption
    IsoAssumption = 3,     // both from the ISO assumption
    MaskedAreaOnly = 4,    // read noise measured, shot noise assumed at ISO 100 (no ISO in the file)
    Unavailable = 5,       // no ISO, no profile, no usable optical black
};

[[nodiscard]] std::string_view to_string(NoiseModelSource source) noexcept;  // "dng_noise_profile", ...

struct SensorNoiseModel {
    NoiseModelSource source = NoiseModelSource::Unavailable;
    double read_variance = 0.0;  // normalised units squared
    double shot_scale = 0.0;     // variance per unit of normalised signal
    std::optional<double> iso;
    std::uint64_t masked_samples = 0;
    std::optional<double> masked_read_sigma;  // normalised units, when measured
    double confidence = 0.0;                  // see the file comment
    std::string basis;                        // developer-facing English

    [[nodiscard]] bool available() const noexcept { return source != NoiseModelSource::Unavailable; }
    // Standard deviation of a normalised green sample of mean `signal` (negative signals count as 0).
    [[nodiscard]] double sigma(double signal) const noexcept;
};

// The model for `metadata` (see the file comment); `masked_read_sigma` is the measured read noise in
// normalised units from `masked_samples` optical-black samples, if any.
[[nodiscard]] SensorNoiseModel make_noise_model(const raw::RawMetadata& metadata,
                                                std::optional<double> masked_read_sigma,
                                                std::uint64_t masked_samples);

// ---- Source analysis ---------------------------------------------------------------------------

struct SourceDiagnostics {
    raw::RawRepresentation representation = raw::RawRepresentation::Mosaic;
    std::vector<ChannelSaturation> channels;  // one per channel index in use
    std::uint64_t sites = 0;                  // all visible samples
    std::uint64_t saturated_sites = 0;
    std::uint64_t nonfinite_samples = 0;  // float data only
    image::Size stored_size;              // the visible area, stored orientation
    image::Orientation orientation = image::Orientation::Normal;
    image::Size upright_size;  // oriented_size(stored_size, orientation)
    SensorNoiseModel noise;
    double seconds = 0.0;

    // The largest saturated fraction of any channel.
    [[nodiscard]] double max_channel_fraction() const noexcept;
};

struct SourceAnalysis {
    SourceDiagnostics diagnostics;
    std::vector<CoverageMap> coverage;  // one per requested grid, in request order
    // With full_resolution_classes: the classes of the upright full-resolution grid (exactly the
    // classes of its CoverageMap, without the float map).
    SaturationClasses full_resolution;
};

// Measures the decoded samples (see the file comment). `coverage_grids` are upright grid sizes, each
// at most the upright visible size; `full_resolution_classes` adds SourceAnalysis::full_resolution.
// Errors (analysis.source): Internal for data that violates the decoded-RAW contract
// (raw::check_consistency); InvalidArgument for an empty or oversized grid; Unsupported for a mosaic
// whose CFA colours have no channel index; OutOfMemory; Cancelled.
[[nodiscard]] Expected<SourceAnalysis> analyze_source(const raw::DecodedRaw& decoded,
                                                      std::span<const image::Size> coverage_grids,
                                                      bool parallel = true,
                                                      const core::CancellationToken& cancel = {},
                                                      bool full_resolution_classes = false);

}  // namespace lucent::analysis
