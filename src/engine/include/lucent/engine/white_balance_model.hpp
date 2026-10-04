#pragma once

// White balance as a photographic control (blueprint section 5, step 5, and DevelopSettings v1):
// conversions between correlated colour temperature / tint, CIE xy whites and the camera's
// white-balance multipliers, through the camera's own calibration.
//
// Temperature and tint follow the Adobe DNG SDK's dng_temperature exactly (Set_xy_coord and
// Get_xy_coord): Robertson's isotemperature lines (Wyszecki & Stiles table 1(3.11)) in CIE 1960
// uv; the temperature is interpolated in reciprocal temperature between the two lines that
// bracket the point, and the tint is the signed distance along the interpolated isotemperature
// direction times kTintScale = -3000: a positive tint is a white above the Planckian locus
// (greenish light, which the balance corrects towards magenta), a negative tint one below it;
// D65 is 6504 K / +9.8 and D50 5001 K / +9.6, the values Adobe's raw editors show. Temperatures
// beyond the table (above 100000 K or below 1667 K) are clamped to its end lines. The two
// conversions are inverse to within 1.1e-5 (relative temperature) and 2.7e-5 (tint) over 2000
// to 50000 K and tints of +-150 (they interpolate the line directions slightly differently).
//
// The mapping between a white and the multipliers that make a neutral under it neutral depends on
// the calibration path that camera_color.hpp chooses for the file:
//   * DNG profile (ColorMatrix1 present): the SDK's rule. White -> camera neutral is
//     dng_camera_neutral() (XYZtoCamera interpolated at the white's temperature, times XYZ);
//     camera neutral -> white is the white search of evaluate_dng_profile(). Dual-illuminant
//     profiles interpolate the calibration with the temperature; single-matrix profiles use
//     ColorMatrix1 throughout.
//   * Decoder matrix (LibRaw cam_xyz, an XYZ(D65)-to-camera matrix): camera neutral =
//     cam_xyz * XYZ(white), exactly how dcraw/LibRaw derive their daylight multipliers (pre_mul)
//     from the same matrix for the D65 white; camera neutral -> white is its inverse. One matrix
//     serves every illuminant, so whites far from D65 are an approximation (stated in basis()).
//   In both paths the camera neutral is scaled to a maximum of 1 and its entries pinned to
//   [0.001, 1], as the SDK does, so the extreme corners of the DevelopSettings range (for
//   example 2000 K with tint +150, beyond the spectral locus, which a matrix can map to a
//   negative blue response) give finite, very large multipliers rather than an error; once an
//   entry is pinned, settings further into that corner give the same multipliers (on a Canon
//   EOS R5 every tint from about +70 to +150 at 2000 K renders alike), as in Adobe's raw
//   editors. Rendering such whites never fails either: the DNG ColorMatrix path adapts to them
//   with the SDK's guarded Bradford adaptation (evaluate_dng_profile()).
//   * Monochrome data, or no usable calibration: no mapping; Kelvin is unavailable
//     (kelvin_available() is false and kelvin_unavailable_reason() says why).
// Multipliers are always for camera red, green and blue (RgbMultipliers, green = 1) and
// "absolute": relative to the camera's native, unbalanced values. When the decoder had already
// balanced the samples (PriorProcessingKind::WhiteBalanceApplied), prior_balance() is that
// balance (DNG AsShotNeutral, otherwise the as-shot multipliers) and the multipliers to apply to
// the samples are absolute / prior (sample_multipliers()).
//
// Thread-safety: an immutable value after create(); concurrent reads are safe.

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include <lucent/core/error.hpp>
#include <lucent/engine/camera_color.hpp>
#include <lucent/engine/develop_settings.hpp>
#include <lucent/engine/sensor.hpp>
#include <lucent/image/color_encoding.hpp>
#include <lucent/image/color_math.hpp>
#include <lucent/raw/decoded_raw.hpp>

namespace lucent::engine {

// The DNG SDK's tint scale (dng_temperature.cpp, kTintScale).
inline constexpr double kTintScale = -3000.0;

struct TemperatureTint {
    double temperature_k = 0.0;  // correlated colour temperature, kelvin
    double tint = 0.0;           // DNG SDK units

    friend bool operator==(const TemperatureTint&, const TemperatureTint&) = default;
};

// dng_temperature::Set_xy_coord. InvalidArgument (engine.develop.white_balance) for a
// non-finite chromaticity or one without CIE 1960 coordinates (1.5 - x + 6y <= 0).
[[nodiscard]] Expected<TemperatureTint> temperature_tint_from_xy(image::Chromaticity xy);

// dng_temperature::Get_xy_coord. InvalidArgument (engine.develop.white_balance) for a
// non-finite value or a temperature that is not positive; Corrupt when the result has no xy
// coordinates (u - 4v + 2 <= 0, x <= 0 or y <= 0: tints far beyond the supported +-150). Within
// the DevelopSettings ranges the result can lie outside the spectral locus (x + y >= 1; for
// example 2000 K with tint +150), which the camera mapping accepts.
[[nodiscard]] Expected<image::Chromaticity> xy_from_temperature_tint(const TemperatureTint& value);

// White-balance multipliers of camera red, green and blue, normalised to green = 1.
using RgbMultipliers = std::array<double, 3>;

class WhiteBalanceModel {
public:
    // Builds the model of a decoded image's metadata (probe or decode metadata after unpack).
    // Errors: those of resolve_white_balance() for the as-shot request (a representation with
    // colours other than red, green and blue is Unsupported). A missing calibration is not an
    // error: the model then reports Kelvin as unavailable.
    [[nodiscard]] static Expected<WhiteBalanceModel> create(const raw::RawMetadata& metadata);

    // Calibration the mapping uses (as resolve_camera_color() would choose it).
    [[nodiscard]] CalibrationPath path() const noexcept { return path_; }
    [[nodiscard]] bool monochrome() const noexcept { return path_ == CalibrationPath::MonochromeNeutral; }
    [[nodiscard]] bool kelvin_available() const noexcept { return unavailable_reason_.empty(); }
    // Developer-facing English; empty when Kelvin is available.
    [[nodiscard]] const std::string& kelvin_unavailable_reason() const noexcept { return unavailable_reason_; }
    // Developer-facing English: which calibration the mapping uses and its limits.
    [[nodiscard]] const std::string& basis() const noexcept { return basis_; }

    // The as-shot white balance (absolute multipliers) and where it came from (as
    // resolve_white_balance() with WhiteBalanceRequest::AsShot: as shot, else daylight, else
    // unity; AlreadyApplied data reports the decoder's balance).
    [[nodiscard]] const RgbMultipliers& as_shot() const noexcept { return as_shot_; }
    [[nodiscard]] WhiteBalanceSource as_shot_source() const noexcept { return as_shot_source_; }
    // The balance the decoder already applied to the samples; unity otherwise.
    [[nodiscard]] const RgbMultipliers& prior_balance() const noexcept { return prior_; }

    // Multipliers that turn a neutral lit by `white` into camera (1, 1, 1). Unsupported when
    // Kelvin is unavailable; InvalidArgument for a chromaticity that is not finite or has x <= 0
    // or y <= 0; Corrupt when the calibration maps the white to camera values without a positive
    // entry (other non-positive entries are pinned, see the file comment).
    [[nodiscard]] Expected<RgbMultipliers> multipliers_for_white(image::Chromaticity white) const;
    // multipliers_for_white(xy_from_temperature_tint(value)).
    [[nodiscard]] Expected<RgbMultipliers> multipliers_for(const TemperatureTint& value) const;
    // The white whose neutral `multipliers` balance (inverse of multipliers_for_white()).
    // Unsupported when Kelvin is unavailable; InvalidArgument for multipliers that are not
    // finite and positive; Corrupt when the calibration maps the neutral outside positive XYZ.
    [[nodiscard]] Expected<image::Chromaticity> white_for(const RgbMultipliers& multipliers) const;
    // temperature_tint_from_xy(white_for(multipliers)).
    [[nodiscard]] Expected<TemperatureTint> temperature_for(const RgbMultipliers& multipliers) const;

    // Absolute target multipliers of a DevelopSettings white balance: AsShot -> as_shot();
    // Kelvin -> multipliers_for() (Unsupported when unavailable); CameraNeutral -> the given
    // multipliers normalised to green (Unsupported for monochrome data). InvalidArgument for
    // values DevelopSettings::validate() rejects.
    [[nodiscard]] Expected<RgbMultipliers> target_multipliers(const WhiteBalanceSetting& setting) const;

    // Multipliers to apply to the samples for absolute multipliers: absolute / prior_balance(),
    // normalised to green = 1.
    [[nodiscard]] RgbMultipliers sample_multipliers(const RgbMultipliers& absolute) const noexcept;

    // Per channel index (RepresentationInfo::channel_colors; Green2 takes green), the layout of
    // WhiteBalance::multipliers. Monochrome: channel 0 = 1. Unused entries are 0.
    [[nodiscard]] std::array<double, 4> channel_multipliers(const RgbMultipliers& rgb) const noexcept;

private:
    WhiteBalanceModel() = default;

    // white_for() without the availability check; the calibration (dng_ or camera_to_xyz_) must
    // be set.
    [[nodiscard]] Expected<image::Chromaticity> compute_white(const RgbMultipliers& multipliers) const;

    CalibrationPath path_ = CalibrationPath::DecoderMatrix;
    std::optional<DngProfile> dng_;
    image::Matrix3d xyz_to_camera_;  // decoder matrix rows for camera R, G, B
    image::Matrix3d camera_to_xyz_;  // its inverse
    RgbMultipliers prior_{1.0, 1.0, 1.0};
    RgbMultipliers as_shot_{1.0, 1.0, 1.0};
    WhiteBalanceSource as_shot_source_ = WhiteBalanceSource::AsShot;
    std::array<std::int32_t, 4> channel_rgb_{-1, -1, -1, -1};  // channel index -> 0, 1, 2 (or -1)
    std::string unavailable_reason_;
    std::string basis_;
};

}  // namespace lucent::engine
