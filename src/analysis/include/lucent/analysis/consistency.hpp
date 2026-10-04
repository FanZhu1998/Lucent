#pragma once

// Related-shot consistency, part 1: what is compared (blueprint section 6, "Related-shot
// consistency": "Group conservatively using capture time and visual similarity ... Treat
// consistency as a soft penalty on appearance, never a shared exposure setting for the whole
// folder. Large illumination changes split a group. Within a known camera/calibration domain,
// residual WB changes may be compared; across cameras compare color-managed neutral/chromaticity
// and rendered appearance instead of averaging camera gains"; ADR 0011, phase 2).
//
// Appearance (Appearance, per evaluated candidate and resolution, from the render's statistics):
//   lightness       median display lightness L*/100 of the luminance (RenderStats p50);
//   u, v            CIE 1976 u'v' of the display XYZ summed over the baseline's mid-tones
//                   (0.15 < l_base < 0.9, unclipped in the baseline and the candidate): the rendered
//                   colour balance, colour-managed (display-linear Rec.2020 / D65) and, being a ratio
//                   of sums, independent of exposure;
//   log_red/blue    log of the absolute camera multipliers (green = 1) of the settings' white
//                   balance: meaningful ONLY between images of one camera/calibration domain.
//
// Shot features (ShotFeatures, per image, from its metadata, the analysis image and its as-shot
// render), the inputs of grouping (related_shots.hpp):
//   capture time    the camera's wall clock (CaptureSettings::capture_time_local), in seconds;
//   camera / calibration domain
//                   "make model" and a key that also names the colour calibration route
//                   (engine::CalibrationPath) and a fingerprint of the calibration matrices the
//                   route uses (decoder XYZ->camera matrix, or the DNG ColorMatrix /
//                   ForwardMatrix / CameraCalibration / AnalogBalance tags and illuminants): two
//                   images share a domain only when the same sensor data meets the same
//                   calibration, so camera multipliers are comparable;
//   scene_ev        relative scene luminance, EV: log2(N^2 / t) - log2(ISO / 100)
//                   + log2(median scene signal / 0.127) (the ISO 12232 mid-grey response), from
//                   the exposure metadata and the median balanced-green signal; absent without
//                   f-number, exposure time or ISO;
//   white           the as-shot white (the camera's estimate or setting), colour-managed through
//                   the file's calibration (engine::WhiteBalanceModel::white_for), as CIE 1960 uv
//                   and correlated colour temperature; absent when Kelvin is unavailable;
//   rendered u'v'   the as-shot render's mid-tone colour balance (as Appearance);
//   structure       an 8 x 8 grid over the upright analysis image of log2 of the mean scene signal
//                   per cell minus the median cell (exposure-normalised layout, EV);
//   colour cells    u'v' of each cell of the as-shot render minus the whole image's (content colour
//                   layout, independent of the white balance);
//   tones           a 16-bin histogram of log2(signal / median signal) over [-8, +4) EV.
//
// Consistency term (consistency_term(), soft: it is added to the Natural v1 objective of feasible
// candidates and never gates feasibility; hard checks and locks are untouched), for a candidate's
// appearance `a` and a member's target `t` (dz = a dead zone, s = the scale of one visible
// inconsistency):
//   lightness       ((max(0, |a.lightness - t.lightness| - 0.01)) / 0.04)^2;
//   colour          ((max(0, |a.uv - t.uv| - 0.002)) / 0.004)^2, when the target carries a
//                   colour-managed colour (its group spans more than one calibration domain);
//   white balance   ((max(0, |a.log - t.log| - 0.005)) / 0.015)^2 (log-gain distance of the
//                   absolute camera multipliers), when the target carries the member's own
//                   domain's camera white balance;
//   total = weight x (lightness + colour + white balance).
// The constants are uncalibrated Natural v1 defaults (ADR 0011).
//
// Thread-safety: value types and pure functions.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <lucent/core/error.hpp>
#include <lucent/engine/camera_color.hpp>
#include <lucent/engine/white_balance_model.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/raw/decoded_raw.hpp>

namespace lucent::analysis {

// Version of the grouping rules, the consistency targets and the consistency term (bump when any
// of them changes).
inline constexpr std::uint32_t kConsistencyVersion = 1;

// ---- Appearance --------------------------------------------------------------------------------

inline constexpr double kMidtoneLow = 0.15;   // baseline lightness of the mid-tone mask
inline constexpr double kMidtoneHigh = 0.90;
inline constexpr std::uint64_t kMinMidtonePixels = 1024;

struct Appearance {
    bool valid = false;           // measured (an unevaluated candidate has none)
    double lightness = 0.0;       // median display lightness, L*/100
    bool colour = false;          // u, v measured (colour data with enough mid-tone pixels)
    double u = 0.0;               // CIE 1976 u' of the mid-tones
    double v = 0.0;               // CIE 1976 v'
    std::uint64_t midtone_pixels = 0;
    bool camera_gains = false;    // log_red/log_blue known (colour data with a white-balance model)
    double log_red = 0.0;         // log(r / g) of the absolute camera multipliers
    double log_blue = 0.0;        // log(b / g)

    friend bool operator==(const Appearance&, const Appearance&) = default;
};

// CIE 1976 u'v' of XYZ (0, 0 for a non-positive denominator).
struct UvPrime {
    double u = 0.0;
    double v = 0.0;
};
[[nodiscard]] UvPrime uv_prime(double X, double Y, double Z) noexcept;

// ---- Shot features -----------------------------------------------------------------------------

inline constexpr std::int32_t kShotGrid = 8;  // cells per side
inline constexpr std::size_t kShotCells = static_cast<std::size_t>(kShotGrid) * kShotGrid;
inline constexpr std::size_t kShotToneBins = 16;
inline constexpr double kShotToneLowEv = -8.0;   // histogram range of log2(signal / median)
inline constexpr double kShotToneHighEv = 4.0;
inline constexpr double kMidGreySignal = 0.127;  // ISO 12232: 18 % grey at 12.7 % of saturation

struct ShotFeatures {
    // Capture.
    std::optional<std::int64_t> capture_seconds;  // camera wall clock, seconds (no time zone)
    std::string capture_time;                     // as recorded ("YYYY-MM-DDTHH:MM:SS"), or empty
    std::string camera;                           // normalised "make model"
    std::string calibration_domain;               // see the file comment
    image::Size size;                             // upright full-resolution size
    bool color = true;

    // Illumination.
    std::optional<double> exposure_ev100;  // log2(N^2 / t) - log2(ISO / 100)
    double median_signal = 0.0;            // median balanced-green scene signal (1 = white level)
    std::optional<double> scene_ev;        // exposure_ev100 + log2(median_signal / kMidGreySignal)
    std::optional<double> white_u;         // CIE 1960 uv of the as-shot white
    std::optional<double> white_v;
    std::optional<double> white_kelvin;    // its correlated colour temperature
    bool rendered_colour = false;          // rendered_u/v measured
    double rendered_u = 0.0;               // u'v' of the as-shot render's mid-tones
    double rendered_v = 0.0;

    // Visual similarity.
    bool descriptors = false;
    std::array<float, kShotCells> structure{};  // EV, minus the median cell
    std::array<float, kShotCells> colour_u{};   // u' minus the image's
    std::array<float, kShotCells> colour_v{};
    std::array<float, kShotToneBins> tones{};   // fractions (sum 1)
};

// "YYYY-MM-DDTHH:MM:SS" (an optional fractional second or zone suffix is ignored) as seconds of a
// proleptic Gregorian calendar without time zone; nullopt when the text does not parse.
[[nodiscard]] std::optional<std::int64_t> parse_capture_time(std::string_view text) noexcept;

// The camera/calibration domain key of `metadata` (see the file comment); "monochrome:<camera>"
// for monochrome data.
[[nodiscard]] std::string calibration_domain(const raw::RawMetadata& metadata);

// Fills the capture, camera, exposure and white-point fields of `features` from the metadata and
// the file's white-balance model (null for monochrome data).
void describe_capture(const raw::RawMetadata& metadata, const engine::WhiteBalanceModel* model,
                      image::Size upright_full, ShotFeatures& features);

// Fills median_signal, scene_ev, the rendered colour and the similarity descriptors from the
// analysis image's scene signal (balanced green, row-major, `size`) and its as-shot render
// (display-linear Rec.2020, same size; the colour cells and rendered colour need 3 channels).
// Errors (analysis.consistency): InvalidArgument for mismatched sizes.
[[nodiscard]] Status describe_scene(std::span<const float> signal, image::Size size,
                                    const engine::ColorImage& as_shot_render, ShotFeatures& features);

// ---- Targets and the consistency term -----------------------------------------------------------

inline constexpr double kConsistencyLightnessDeadZone = 0.01;
inline constexpr double kConsistencyLightnessScale = 0.04;
inline constexpr double kConsistencyColourDeadZone = 0.002;
inline constexpr double kConsistencyColourScale = 0.004;
inline constexpr double kConsistencyWhiteBalanceDeadZone = 0.005;
inline constexpr double kConsistencyWhiteBalanceScale = 0.015;
// Above this consistency term the regularised result is flagged ConsistencyLimited.
inline constexpr double kConsistencyLimitedTerm = 1.0;

// The target one member of a related group is regularised towards (related_shots.hpp,
// consistency_targets()).
struct ConsistencyTarget {
    std::string group_id;
    std::uint32_t version = kConsistencyVersion;
    double weight = 1.0;            // multiplies the term
    std::size_t contributors = 0;   // members (or anchors) the target was formed from
    bool anchored = false;          // formed from caller-marked anchors only

    double lightness = 0.0;         // target median lightness, L*/100

    bool colour = false;            // colour-managed colour target (multi-domain groups)
    double u = 0.0;
    double v = 0.0;

    bool camera_white_balance = false;  // the member's own domain's camera white balance
    std::string domain;                 // that domain
    double log_red = 0.0;               // log(r / g), log(b / g) of absolute camera multipliers
    double log_blue = 0.0;
    std::size_t domain_contributors = 0;

    // The confidence a white-balance move towards this target carries for the confidence
    // restraint (auto_settings.hpp): 1 for anchors, 0.6 for a camera target of at least three
    // contributors, 0.5 otherwise.
    double white_balance_confidence = 0.5;
};

struct ConsistencyTerms {
    double lightness = 0.0;
    double colour = 0.0;
    double white_balance = 0.0;
    double total = 0.0;  // weight x (sum)

    friend bool operator==(const ConsistencyTerms&, const ConsistencyTerms&) = default;
};

// The consistency term of an appearance against a target (see the file comment). An invalid
// appearance, or a colour or camera comparison the appearance cannot make, contributes nothing to
// that part.
[[nodiscard]] ConsistencyTerms consistency_term(const Appearance& appearance,
                                                const ConsistencyTarget& target) noexcept;

}  // namespace lucent::analysis
