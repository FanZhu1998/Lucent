#pragma once

// Sensor normalisation and pre-demosaic white balance (blueprint section 5, "Sensor
// interpretation and rendering", step 2, and the white-balance part of step 5 that the
// demosaicing routes need before reconstruction).
//
// normalize_sensor() turns decoded RAW data (raw::DecodedRaw: Mosaic, LinearColor or Monochrome)
// into a SensorImage: float32 samples of the visible area, black-subtracted and scaled so that
// the white level of each channel maps to 1.0, together with the CFA re-expressed at the visible
// origin, a per-site saturation mask and a record of the levels used. Nothing is clipped:
// samples below black stay negative and samples above white stay above 1.0 (conventions.md,
// "Numerics and images").
//
// apply_white_balance() then multiplies each channel by its white-balance multiplier (green = 1)
// and applies the highlight policy. The SensorImage before that call is the pre-white-balance
// data for analysis callers; after it, SensorImage::white_balance records what was applied.
//
// Normalisation (per sample, in double precision, stored as float):
//     value = (raw - black(site)) / (white - black_max[c])
// where
//   * c is the sample's channel index (RepresentationInfo::channel_colors): the CFA colour's
//     index for a mosaic, the interleaved channel for LinearColor, 0 for Monochrome;
//   * black(site) = raw::SensorLevels::black_level_at(c, vx, vy) at visible-area position
//     (vx, vy): common + per-channel + spatial pattern + column delta + row delta, exactly the
//     contract's definition (LibRaw's cblack model plus DNG BlackLevelDeltaH/V);
//   * white = SensorLevels::white for every channel;
//   * black_max[c] = the largest black level of any site of channel c: the common and channel
//     terms, the largest pattern value at a channel-c site (over the joint period of CFA and
//     pattern), plus the largest column delta and the largest row delta. With deltas it is an
//     upper bound (DNG 1.7.1.0 chapter 5 scales by WhiteLevel minus the maximum computed black
//     level). So the white level maps to 1.0 at the sites of channel c with the highest black
//     level, and to slightly more elsewhere.
//
// Saturation (blueprint: "Track source saturation before resampling"): a sample is saturated
// when its RAW value (before black subtraction) is at or above the white level:
//     threshold[c] = white   for every channel c.
// This is where dcraw and LibRaw clip (their `maximum`, with LibRaw's image-dependent
// adjust_maximum disabled). The decoder's per-channel linear_max is deliberately NOT used (LibRaw's
// processing does not use it either): LibRaw fills it from sources of different meaning (Canon
// SpecularWhiteLevel, Panasonic LinearityLimit lowered by 64, an approximation from the DNG
// LinearResponseLimit, and for Nikon files without one an estimate of maximum / 1.07), and on
// the fixtures real data continues above it up to the white level (Canon R5: linear_max 14008,
// samples at the white level 16383; Nikon Z 6: linear_max 15311, 724 samples at 16383), so
// clipping there would discard valid highlight data.
// The normalised saturation level of channel c,
//     saturation_level[c] = (threshold[c] - black_max[c]) / (white - black_max[c])  (= 1),
// is the lowest normalised value a saturated channel-c sample can have. The per-channel fields
// stay in NormalizationRecord so that a later version can carry per-channel white levels.
//
// Units: pixel coordinates are visible-area pixels in stored (unrotated) orientation; levels
// and thresholds in NormalizationRecord are in stored-sample units unless named "level"
// (normalised units, 1.0 = white).
//
// Thread-safety: all functions are reentrant and may run concurrently on different images. A
// SensorImage has no internal synchronisation (see image/pixel_buffer.hpp). The functions use
// the C++17 parallel algorithms internally (std::execution::par over rows); results do not
// depend on scheduling.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/image/cfa.hpp>
#include <lucent/image/color_encoding.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/image/pixel_buffer.hpp>
#include <lucent/raw/decoded_raw.hpp>

namespace lucent::engine {

// Versions of the operation semantics, recorded with every result. Bump a version whenever the
// same input would produce different values.
//   normalisation 2: the saturation threshold is the white level of every channel (version 1
//   lowered it to the decoder's linear_max when that lay below white).
inline constexpr std::uint32_t kSensorNormalizationVersion = 2;
inline constexpr std::uint32_t kWhiteBalanceVersion = 1;

// ---------------------------------------------------------------------------------------------
// Saturation mask
// ---------------------------------------------------------------------------------------------

// One byte per pixel of an image: bit c is set when channel index c of that pixel was saturated
// in the RAW data. For a mosaic only the bit of the site's own channel can be set; for
// LinearColor any of bits 0-2; for Monochrome bit 0. Move-only (it can be tens of megabytes).
class SaturationMask {
public:
    SaturationMask() noexcept = default;
    SaturationMask(SaturationMask&&) noexcept = default;
    SaturationMask& operator=(SaturationMask&&) noexcept = default;
    SaturationMask(const SaturationMask&) = delete;
    SaturationMask& operator=(const SaturationMask&) = delete;
    ~SaturationMask() = default;

    // An all-clear mask. InvalidArgument when a dimension is below 1; LimitExceeded when
    // width * height exceeds limits.max_bytes or a dimension exceeds limits.max_dimension;
    // OutOfMemory when the allocation fails.
    [[nodiscard]] static Expected<SaturationMask> create(std::int32_t width, std::int32_t height,
                                                         const image::AllocationLimits& limits = {});

    // Deep copy. OutOfMemory when the allocation fails; an empty mask clones to an empty mask.
    [[nodiscard]] Expected<SaturationMask> clone() const;

    [[nodiscard]] std::int32_t width() const noexcept { return width_; }
    [[nodiscard]] std::int32_t height() const noexcept { return height_; }
    [[nodiscard]] bool empty() const noexcept { return bits_.empty(); }

    // Flags of pixel (x, y). Precondition (asserted in debug builds): inside the mask.
    [[nodiscard]] std::uint8_t at(std::int32_t x, std::int32_t y) const noexcept;
    // The width() flags of row y. Precondition: 0 <= y < height().
    [[nodiscard]] std::uint8_t* row(std::int32_t y) noexcept;
    [[nodiscard]] const std::uint8_t* row(std::int32_t y) const noexcept;

    // Number of pixels with at least one flag set.
    [[nodiscard]] std::uint64_t count_flagged() const noexcept;

private:
    std::vector<std::uint8_t> bits_;
    std::int32_t width_ = 0;
    std::int32_t height_ = 0;
};

// ---------------------------------------------------------------------------------------------
// Normalisation
// ---------------------------------------------------------------------------------------------

// What normalize_sensor() used. Per-channel arrays are indexed by channel index; entries at or
// beyond SensorImage::channel_colors.size() are 0.
struct NormalizationRecord {
    std::uint32_t version = kSensorNormalizationVersion;
    // The visible area in raw-frame coordinates: pixel (0, 0) of the SensorImage is raw-frame
    // pixel (source_area.x, source_area.y).
    image::PixelRect source_area;
    raw::Provenance levels_provenance = raw::Provenance::Decoder;
    std::array<double, 4> black_max{};             // stored units; see the file comment
    double white = 0.0;                            // stored units
    std::array<double, 4> saturation_threshold{};  // stored units
    std::array<float, 4> saturation_level{};       // normalised units
    bool black_pattern_applied = false;            // a spatial black pattern was subtracted
    bool column_deltas_applied = false;            // per-column deltas were subtracted
    bool row_deltas_applied = false;               // per-row deltas were subtracted
    std::uint64_t saturated_samples = 0;           // samples at or above their threshold
};

// Result of white balancing (apply_white_balance), recorded on the image.
enum class WhiteBalanceSource : std::uint8_t {
    AsShot = 1,          // RawMetadata::color.as_shot_multipliers (LibRaw cam_mul)
    Daylight = 2,        // RawMetadata::color.daylight_multipliers (LibRaw pre_mul)
    Custom = 3,          // caller-supplied multipliers
    AlreadyApplied = 4,  // the data already carries the as-shot white balance (prior processing);
                         // unity multipliers, so it is never applied twice
    NotApplicable = 5,   // monochrome data: no colour channels to balance
    Unavailable = 6,     // no usable multipliers in the file: unity, the image is NOT balanced
};

enum class HighlightPolicy : std::uint8_t {
    // dcraw/LibRaw "highlight 0" (clip): after white balance every sample of every channel is
    // limited to the lowest white-balanced saturation level of the channels in use,
    //     clip_level = min over channels c of saturation_level[c] * multiplier[c],
    // so saturated sites of every channel end at the same value and clipped highlights stay
    // neutral instead of turning magenta. With saturation at the white level (normalize_sensor)
    // this is exactly dcraw's clip: every channel is limited to the white level times the
    // smallest multiplier. Values at or below clip_level are unchanged; negative values are never
    // touched. The default for rendering.
    ClipToLowestSaturation = 1,
    // No limit: white-balanced values are kept as computed (analysis, highlight reconstruction).
    None = 2,
};

[[nodiscard]] std::string_view to_string(WhiteBalanceSource value) noexcept;  // "as_shot", ...
[[nodiscard]] std::string_view to_string(HighlightPolicy value) noexcept;     // "clip_to_lowest_saturation", "none"

// Resolved white-balance multipliers.
struct WhiteBalance {
    WhiteBalanceSource source = WhiteBalanceSource::AsShot;
    // Per channel index, normalised so that the green channel (CfaColor::Green, else Green2) is
    // exactly 1; unused channel indices are 0. All used entries are finite and positive.
    std::array<double, 4> multipliers{};
    // Developer-facing English: where the values came from and why (including any fallback).
    std::string basis;
};

struct WhiteBalanceRecord {
    std::uint32_t version = kWhiteBalanceVersion;
    WhiteBalance applied;
    HighlightPolicy highlights = HighlightPolicy::ClipToLowestSaturation;
    // Normalised level all samples were limited to (ClipToLowestSaturation); +infinity for None.
    float clip_level = 0.0f;
    std::uint64_t clipped_samples = 0;  // samples that were above clip_level and were limited
};

// Normalised sensor data of the visible area.
struct SensorImage {
    raw::RawRepresentation representation = raw::RawRepresentation::Mosaic;
    // Visible area only. Mosaic and Monochrome: 1 channel; LinearColor: 3 interleaved channels
    // in channel-index order (R, G, B).
    image::ImageBuffer<float> pixels;
    // camera_native_mosaic (Mosaic), camera_native_linear (LinearColor) or
    // raw::camera_native_monochrome() (Monochrome). Camera-native, linear, unbounded, scene.
    image::ColorEncoding encoding = image::ColorEncoding::camera_native_mosaic();
    // Mosaic only: the CFA relative to pixel (0, 0) of `pixels` (the visible-area origin), i.e.
    // raw::cfa_at_visible_origin(). Empty for other representations.
    image::CfaPattern cfa;
    // Colour of each channel index (raw::RepresentationInfo::channel_colors).
    std::vector<image::CfaColor> channel_colors;
    SaturationMask saturation;  // same size as `pixels`
    NormalizationRecord normalization;
    // Set by apply_white_balance(); std::nullopt while the data is not white balanced.
    std::optional<WhiteBalanceRecord> white_balance;

    // Channel index of mosaic site (x, y) (any coordinates; the CFA repeats). -1 when the image
    // is not a mosaic or the site's colour has no channel index.
    [[nodiscard]] std::int32_t channel_of_site(std::int32_t x, std::int32_t y) const noexcept;
};

struct NormalizeOptions {
    // Bounds for the float image and the mask (image::AllocationLimits defaults: 2 GiB each).
    image::AllocationLimits limits;
    // Use the parallel algorithms; false runs on the calling thread (identical results).
    bool parallel = true;
};

// Normalises decoded RAW data (see the file comment).
//
// Errors (Error::where in parentheses):
//   VersionMismatch  decoded.contract is not readable by this build (engine.sensor.contract)
//   Corrupt          levels are not finite, or the white level is not above the highest black
//                    level of some channel, or a floating-point sample is not finite
//                    (engine.sensor.levels / engine.sensor.samples)
//   Internal         the data violates the decoded-RAW contract (raw::check_consistency;
//                    raw.contract)
//   Unsupported      no normalisation route: the decoder marked the data NotDecodable, a mosaic
//                    uses colours other than red, green and blue (CMYG and other 4-colour
//                    filters), or full-colour data whose channels are not R, G, B in that order
//                    (engine.sensor.layout)
//   LimitExceeded / OutOfMemory  the output does not fit `options.limits` or memory
//   Cancelled        `cancel` was cancelled (engine.sensor.normalize)
// Interpretation issues other than NotDecodable (for example a mandatory DNG opcode Lucent
// cannot apply) do not stop normalisation; they stay in the caller's metadata, which must
// travel with any result.
[[nodiscard]] Expected<SensorImage> normalize_sensor(const raw::DecodedRaw& decoded,
                                                     const NormalizeOptions& options = {},
                                                     const core::CancellationToken& cancel = {});

// ---------------------------------------------------------------------------------------------
// White balance
// ---------------------------------------------------------------------------------------------

enum class WhiteBalanceRequest : std::uint8_t {
    AsShot = 1,    // as-shot multipliers; falls back to daylight, then to Unavailable (unity)
    Daylight = 2,  // daylight multipliers; falls back to Unavailable (unity)
    Custom = 3,    // WhiteBalanceOptions::custom_multipliers
};

struct WhiteBalanceOptions {
    WhiteBalanceRequest request = WhiteBalanceRequest::AsShot;
    // Per channel index, for Custom only; normalised to green = 1 on resolution. A zero Green2
    // entry takes the Green entry (and vice versa), as LibRaw does for its fourth multiplier.
    std::array<double, 4> custom_multipliers{};
    HighlightPolicy highlights = HighlightPolicy::ClipToLowestSaturation;
};

// Chooses the multipliers for `metadata` without touching pixels:
//   * Monochrome data: NotApplicable (multiplier 1 for channel 0).
//   * Data whose as-shot white balance is already applied (PriorProcessingKind::
//     WhiteBalanceApplied): AlreadyApplied (unity) for AsShot and Daylight requests. A Custom
//     request is honoured and then acts relative to the applied balance (stated in `basis`).
//   * AsShot: as_shot_multipliers when every used channel has a finite positive value (after
//     completing Green2 from Green); otherwise Daylight with the reason in `basis`; otherwise
//     Unavailable.
//   * Daylight: daylight_multipliers, otherwise Unavailable.
// Errors: InvalidArgument (engine.white_balance.resolve) for a Custom request whose multipliers
// are not finite and positive for every used channel, or metadata without a green channel for
// colour data; Unsupported for a representation without a white-balance model (colours other
// than R, G, B).
[[nodiscard]] Expected<WhiteBalance> resolve_white_balance(const raw::RawMetadata& metadata,
                                                           const WhiteBalanceOptions& options = {});

// Multiplies every sample by its channel's multiplier and applies `highlights`, in place, then
// sets image.white_balance (whose multipliers keep only the channel indices the image uses).
// Multipliers must be finite and positive for every channel index the image uses (every CFA
// colour of a mosaic, channels 0-2 of LinearColor, channel 0 of Monochrome).
// Errors (engine.white_balance.apply): InvalidArgument when the image is empty, already white
// balanced (white balance is never applied twice) or a multiplier is invalid; Cancelled when
// `cancel` was cancelled (the image is then partially modified and must be discarded).
[[nodiscard]] Status apply_white_balance(SensorImage& image, const WhiteBalance& white_balance,
                                         HighlightPolicy highlights = HighlightPolicy::ClipToLowestSaturation,
                                         const core::CancellationToken& cancel = {}, bool parallel = true);

// resolve_white_balance() followed by apply_white_balance().
[[nodiscard]] Status white_balance_sensor(SensorImage& image, const raw::RawMetadata& metadata,
                                          const WhiteBalanceOptions& options = {},
                                          const core::CancellationToken& cancel = {}, bool parallel = true);

}  // namespace lucent::engine
