#pragma once

// Camera colour interpretation (blueprint section 5, step 5 "Complete white balance and camera
// colour interpretation", and "Color and model contracts"): white-balanced camera RGB -> CIE XYZ
// -> Lucent's working space, scene-linear Rec.2020 / D65.
//
// Calibration paths (CalibrationPath), chosen from the file's metadata, never guessed:
//   * DNG files whose raw IFD Lucent inspected and whose IFD 0 carries ColorMatrix1 follow the
//     DNG 1.7.1.0 specification, chapter 6 "Mapping Camera Color Space to CIE XYZ Space", as the
//     Adobe DNG SDK implements it (dng_color_spec):
//       - AnalogBalance (AB, default unity) and CameraCalibration1/2 (CC, default identity) are
//         folded into the colour matrices: XYZtoCamera_i = AB * CC_i * ColorMatrix_i.
//       - Two calibrations are interpolated linearly in inverse correlated colour temperature
//         (weight g of calibration 1: 1 at or below T1, 0 at or above T2). The temperatures come
//         from the CalibrationIlluminant codes (calibration_illuminant_temperature()). With one
//         matrix, an unknown illuminant (0 or 255) or equal temperatures, ColorMatrix1 alone is
//         used (the SDK rule). A third illuminant (DNG 1.6) is not implemented and is ignored
//         with a note.
//       - The white is found from the camera neutral iteratively (the spec's "Translating
//         Camera Neutral Coordinates to White Balance xy Coordinates"): start at D50, compute
//         the matrix for the current white's temperature (Robertson's method,
//         correlated_color_temperature()), map the neutral to xy, repeat until |dx| + |dy| <
//         1e-7 (at most 30 passes; an oscillation is resolved by averaging the last two
//         estimates, as the SDK does).
//       - With a ForwardMatrix (normalised so that camera (1,1,1) maps to the D50 white):
//             CameraToXYZ_D50 = FM * inverse(diag(ReferenceNeutral)) * inverse(AB * CC),
//             ReferenceNeutral = inverse(AB * CC) * CameraNeutral       (DngForwardMatrix);
//         otherwise (DngColorMatrix):
//             CameraToXYZ_D50 = Bradford(white -> D50) * inverse(XYZtoCamera),
//         scaled so that the camera neutral maps to XYZ with Y = 1.
//     The result is XYZ relative to D50 (the DNG profile connection space).
//   * Other files use the decoder's XYZ(D65) -> camera matrix (raw::CameraColorData::
//     xyz_to_camera, LibRaw cam_xyz; usually LibRaw's built-in per-model table) with the
//     dcraw/LibRaw normalisation: each row is divided by its product with the D65 white, so
//     that white-balanced camera (1,1,1) maps to the D65 white; the camera -> XYZ(D65) matrix is
//     the inverse (DecoderMatrix). This reproduces LibRaw's cam_xyz_coeff() / rgb_cam.
//   * Monochrome data is neutral: XYZ = value * D65 white (MonochromeNeutral).
// A file with no usable calibration, or whose calibration is singular or maps the neutral outside
// positive XYZ, is an error (Unsupported / Corrupt), never a silent identity matrix.
//
// White balance: the engine balances before demosaicing (sensor.hpp), so the transform applies
// to white-balanced camera RGB. The DNG paths need the camera neutral; it is derived from the
// multipliers that were actually applied (WhiteBalanceRecord): neutral_c ~ 1 / multiplier_c,
// normalised to a maximum of 1 (entries pinned to at least 0.001, as the SDK does). When the
// decoder had already balanced the data (PriorProcessingKind::WhiteBalanceApplied), the file's
// AsShotNeutral supplies the balance the data carries. The matrix handed out is then
//     camera_to_xyz = CameraToXYZ * diag(1 / multiplier),
// so the applied white maps exactly to the calibration white.
//
// Exposure zero point: scene-linear Y = 1 is the level at which a neutral surface brings the
// first camera channel to the white level (the channel with the smallest multiplier). This is
// the DNG SDK's convention ("PCS white can just be reached when the first camera channel
// saturates") and dcraw/LibRaw's with highlight clipping (multipliers divided by their minimum).
// Hence neutral_scale = 1 / min(multiplier_R, multiplier_G, multiplier_B) (green-normalised
// multipliers), folded into camera_to_xyz. White-balanced camera (1,1,1) maps to the white with
// Y = neutral_scale.
//
// Camera RGB order: channel 0, 1, 2 of the demosaiced image are the camera's red, green and blue
// (demosaic.hpp). DNG colour planes are matched to them through CFAPlaneColor (default 0, 1, 2);
// the decoder matrix rows through RepresentationInfo::channel_colors.
//
// Thread-safety: all functions are reentrant; images have no internal synchronisation.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/demosaic.hpp>
#include <lucent/engine/sensor.hpp>
#include <lucent/image/color_encoding.hpp>
#include <lucent/image/color_math.hpp>
#include <lucent/image/pixel_buffer.hpp>
#include <lucent/raw/decoded_raw.hpp>

namespace lucent::engine {

// Version of the camera-colour semantics (bump when the same input maps to different values).
inline constexpr std::uint32_t kCameraColorVersion = 1;
// Version of the working-space conversion semantics.
inline constexpr std::uint32_t kWorkingSpaceVersion = 1;

// A float image and the colour encoding of its samples: the unit handed between colour stages.
// Move-only (the buffer is).
struct ColorImage {
    image::ImageBuffer<float> pixels;
    image::ColorEncoding encoding;
};

// Persisted calibration path identifiers (append only).
enum class CalibrationPath : std::uint8_t {
    DngColorMatrix = 1,     // DNG ColorMatrix (+ CameraCalibration, AnalogBalance), adapt in XYZ
    DngForwardMatrix = 2,   // DNG ForwardMatrix path
    DecoderMatrix = 3,      // decoder XYZ(D65) -> camera matrix, dcraw/LibRaw row normalisation
    MonochromeNeutral = 4,  // monochrome sensor: neutral
};

[[nodiscard]] std::string_view to_string(CalibrationPath path) noexcept;  // "dng_color_matrix", ...

// Correlated colour temperature in kelvin of a chromaticity, by Robertson's method (A. R.
// Robertson, "Computation of correlated color temperature and distribution temperature",
// JOSA 58(11), 1968) with the 31 isotemperature lines of Wyszecki & Stiles, Color Science
// (2nd ed.), table 1(3.11) (u = 0.24792 at 325 mired), searched exactly as the DNG SDK's
// dng_temperature does. Results lie in [1666.7, 100000] K (points beyond the table ends are
// assigned the end lines). InvalidArgument for non-finite input or 1.5 - x + 6y <= 0.
[[nodiscard]] Expected<double> correlated_color_temperature(image::Chromaticity xy);

// Temperature in kelvin that the DNG SDK assigns to an EXIF LightSource code used as a DNG
// CalibrationIlluminant: 17 (standard A) and 3 (tungsten) 2850; 24 (ISO studio tungsten) 3200;
// 23 (D50) 5000; 20 (D55), 1 (daylight), 9 (fine weather), 4 (flash), 18 (standard B) 5500;
// 21 (D65), 19 (standard C), 10 (cloudy) 6500; 22 (D75), 11 (shade) 7500; fluorescent codes the
// midpoints of their EXIF 2.2 ranges: 12 (D, 5700-7100) 6400, 13 (N, 4600-5400) 5000,
// 14 (W, 3900-4500) and 2 (fluorescent) 4200, 15 (WW, 3200-3700) 3450, 16 (L, 2600-3250) 2925.
// 0 (unknown), 255 (other, defined by IlluminantData, not supported) and anything else: 0.
[[nodiscard]] double calibration_illuminant_temperature(std::uint16_t light_source) noexcept;

// A DNG camera profile re-expressed in camera R, G, B order (3 colour planes only).
struct DngProfile {
    // CalibrationIlluminant1/2 (0 when absent).
    std::array<std::uint16_t, 2> illuminants{};
    // ColorMatrix1/2: XYZ -> camera. color_matrix[0] is required.
    std::array<std::optional<image::Matrix3d>, 2> color_matrix;
    // CameraCalibration1/2 (identity when absent).
    std::array<std::optional<image::Matrix3d>, 2> camera_calibration;
    // ForwardMatrix1/2: camera -> XYZ(D50), as stored (normalised internally).
    std::array<std::optional<image::Matrix3d>, 2> forward_matrix;
    // AnalogBalance (unity when absent).
    image::Vec3d analog_balance{1.0, 1.0, 1.0};
};

// How a DNG calibration was evaluated.
struct DngCalibrationRecord {
    std::array<std::uint16_t, 2> illuminants{};
    // Temperatures used for interpolation (after the single-matrix rule both are 5000 K).
    std::array<double, 2> temperatures{};
    bool interpolated = false;        // two calibrations with distinct known temperatures
    bool forward_matrix = false;      // the ForwardMatrix path was used
    bool camera_calibration = false;  // a CameraCalibration tag was present
    bool analog_balance = false;      // AnalogBalance was present and not unity
    image::Chromaticity white_xy;     // white of the camera neutral
    double white_cct_kelvin = 0.0;    // its correlated colour temperature
    double weight_matrix1 = 1.0;      // interpolation weight g of calibration 1
    std::uint32_t iterations = 0;     // passes of the neutral -> xy search
    bool converged = true;            // false: the last two estimates were averaged
    // Camera -> XYZ(D50) for UNBALANCED camera values; maps camera_neutral to the D50 white.
    image::Matrix3d camera_to_xyz_d50;
    image::Vec3d camera_neutral;  // as used (maximum 1, entries >= 0.001)
};

// Evaluates a DNG profile for a camera neutral (camera R, G, B values of a neutral surface; any
// positive scale). Errors (engine.color.dng): InvalidArgument for a neutral that is not finite
// and positive or a profile without ColorMatrix1; Corrupt for a singular or non-finite matrix, a
// ForwardMatrix whose rows do not sum to positive values, or a neutral that maps outside positive
// XYZ.
[[nodiscard]] Expected<DngCalibrationRecord> evaluate_dng_profile(const DngProfile& profile,
                                                                  const image::Vec3d& camera_neutral);

// Camera neutral of a DNG profile for a white chromaticity: the camera R, G, B values of a neutral
// surface lit by `white`, XYZtoCamera(white) * XYZ(white) with the calibrations interpolated at
// the white's temperature, scaled to a maximum of 1 and pinned to [0.001, 1] (the DNG SDK's
// dng_color_spec::SetWhiteXY). The inverse of the white search of evaluate_dng_profile(): for a
// neutral n, dng_camera_neutral(profile, evaluate_dng_profile(profile, n)->white_xy) reproduces
// n (normalised) to the search tolerance. Errors (engine.color.dng): InvalidArgument for a
// profile without ColorMatrix1 or an invalid chromaticity (not finite, x <= 0 or y <= 0; whites
// beyond the spectral locus are accepted); Corrupt for invalid matrices or a white that maps to
// non-positive camera values.
[[nodiscard]] Expected<image::Vec3d> dng_camera_neutral(const DngProfile& profile, image::Chromaticity white);

// The DNG profile of `metadata` (IFD 0 colour tags of an inspected DNG) in camera R, G, B order,
// as resolve_camera_color() uses it; `notes` (optional) receives the tags that are present but
// ignored. Errors (engine.color.calibration): Unsupported when the metadata has no DNG
// ColorMatrix1, a profile with other than 3 colour planes or planes that are not red, green and
// blue; Corrupt for malformed matrices.
[[nodiscard]] Expected<DngProfile> dng_profile_from_metadata(const raw::RawMetadata& metadata,
                                                             std::vector<std::string>* notes = nullptr);

// What resolve_camera_color() chose and computed.
struct CameraColorRecord {
    std::uint32_t version = kCameraColorVersion;
    CalibrationPath path = CalibrationPath::DecoderMatrix;
    // DngTag for the DNG paths, Decoder for the decoder matrix, LucentRule for monochrome.
    raw::Provenance provenance = raw::Provenance::Decoder;
    // Applied to white-balanced camera R, G, B (monochrome: column 0 applies to the single
    // channel; the other columns are zero); produces XYZ relative to `xyz_white` (Y = 1 is the
    // white). neutral_scale is included.
    image::Matrix3d camera_to_xyz;
    image::WhitePoint xyz_white = image::WhitePoint::D65;  // D50 for the DNG paths
    // White-balance multipliers of camera R, G, B that the transform assumes (green = 1).
    std::array<double, 3> multipliers{1.0, 1.0, 1.0};
    double neutral_scale = 1.0;               // see the file comment
    std::optional<DngCalibrationRecord> dng;  // DNG paths only
    // Developer-facing English: which tags or tables were used and why.
    std::string basis;
    // Developer-facing English: tags that were present but not used, fallbacks taken.
    std::vector<std::string> notes;
};

// The encoding of camera-colour output: CIE XYZ relative to `white`, RGB layout (X, Y, Z),
// linear, unbounded, scene-referred.
[[nodiscard]] image::ColorEncoding xyz_encoding(image::WhitePoint white) noexcept;

// Chooses and evaluates the calibration for `metadata`, whose image was white balanced as
// `white_balance` records (see the file comment).
// Errors (engine.color.calibration): InvalidArgument when the record does not fit the metadata
// (multipliers not finite and positive for red, green and blue); Unsupported when there is no
// calibration (no DNG ColorMatrix1 and a zero decoder matrix), a DNG profile with other than 3
// colour planes or planes that are not red, green and blue, or data already balanced by the
// decoder without an AsShotNeutral in a DNG; Corrupt for singular or invalid matrices; plus the
// errors of evaluate_dng_profile().
[[nodiscard]] Expected<CameraColorRecord> resolve_camera_color(const raw::RawMetadata& metadata,
                                                               const WhiteBalanceRecord& white_balance);

struct ColorStageOptions {
    image::AllocationLimits limits;  // for the 3-channel output of monochrome data
    bool parallel = true;            // false runs on the calling thread (identical results)
};

// Maps a demosaiced image (camera_native_linear RGB, or camera_native_monochrome) to XYZ with
// `record`, consuming it: RGB is transformed in place, monochrome is expanded to 3 channels.
// Each pixel is multiplied by the matrix converted once to float, in a fixed order (as
// image::apply_matrix), so results do not depend on scheduling.
// Errors (engine.color.apply): InvalidArgument for an empty image, an encoding other than the two
// above, a monochrome image with a non-monochrome record or the reverse, or a non-finite matrix;
// LimitExceeded / OutOfMemory for the monochrome expansion; Cancelled.
[[nodiscard]] Expected<ColorImage> apply_camera_color(DemosaicedImage&& image,
                                                      const CameraColorRecord& record,
                                                      const ColorStageOptions& options = {},
                                                      const core::CancellationToken& cancel = {});

// Converts a 3-channel linear RGB-layout image (XYZ or RGB primaries, scene-referred) in place to
// scene-linear Rec.2020 / D65 (image::ColorEncoding::scene_linear_rec2020_d65()) with
// image::conversion_matrix (Bradford adaptation when the whites differ) and returns the matrix
// that was applied. Errors (engine.color.working): InvalidArgument for an empty image, a
// channel count other than 3, a non-linear transfer, a display-referred or camera-native
// encoding; Cancelled.
[[nodiscard]] Expected<image::Matrix3d> convert_to_working_space(ColorImage& image,
                                                                 const ColorStageOptions& options = {},
                                                                 const core::CancellationToken& cancel = {});

}  // namespace lucent::engine
