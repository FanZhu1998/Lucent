#pragma once

// DevelopSettings v1: the photographic settings of one developed image (blueprint section 5,
// step 6 "Apply photographic operations", and section 8 "Recipes, assets, and forward
// compatibility"). Shared contract "lucent.develop", version 1: the engine owns this C++ type
// and its validation; JSON serialisation and persistence belong to the recipe layer.
//
// Every value is a plain number, enum or list, so the type can be copied, compared exactly
// (operator==) and serialised without loss. Default-constructed settings are "as shot": the
// camera's white balance, no adjustment, the identity tone curve and the "neutral" v1 base look.
//
// Semantics (the operations are implemented in develop_operations.hpp and run in this order by
// develop.hpp; each one carries its own semantic version, recorded in `operations`):
//   white_balance  AsShot: the camera's as-shot multipliers.
//                  Kelvin: correlated colour temperature in kelvin and tint in the DNG SDK's
//                  units (dng_temperature: tint = -3000 x the signed distance along the
//                  isotemperature line in CIE 1960 uv) of the white to balance to. As in Adobe's
//                  raw editors, a positive tint is a white above the Planckian locus (greenish
//                  light), which the balance corrects towards magenta; D65 is about 6504 K, +9.8.
//                  Only available when the camera calibration permits a CCT mapping
//                  (WhiteBalanceModel::kelvin_available(), white_balance_model.hpp); otherwise
//                  developing reports Unsupported.
//                  CameraNeutral: white-balance multipliers for camera red, green and blue,
//                  relative to the camera's native (unbalanced) values; only their ratios
//                  matter (they are normalised to green = 1).
//   exposure_ev    scene-linear gain 2^exposure_ev, on top of the file's DNG BaselineExposure.
//   highlights     local luminance change of the bright tones, -1..+1: negative compresses
//                  (darkens) them, positive brightens them (up to 1.5 EV at full weight).
//   shadows        local luminance change of the dark tones, -1..+1: positive lifts them,
//                  negative deepens them (up to 2 EV at full weight). Both are driven by a
//                  guided-filter base layer of the log luminance (edge aware, no halos).
//   contrast       S-curve in display lightness around L* = 50, -1..+1 (0 = identity).
//   tone_curve     monotone control points in display lightness (see ToneCurveSetting).
//   saturation     chroma scale around the pixel's luminance, -1..+1 (factor 1 + saturation).
//   vibrance       chroma scale weighted towards muted colours, -1..+1.
//   base_look      the scene-to-display rendering, id + version ("neutral" v1: a hue-preserving
//                  filmic tone curve, develop_operations.hpp).
//   locks          controls the automatic optimiser (W8) must not change; stored, not used here.
//
// Thread-safety: a value type; concurrent reads are safe.

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <lucent/core/error.hpp>

namespace lucent::engine {

// ---- Schema and operation versions ------------------------------------------------------------

// Shared contract identifier and version of DevelopSettings.
inline constexpr std::string_view kDevelopSettingsSchema = "lucent.develop";
inline constexpr std::uint32_t kDevelopSettingsVersion = 1;

// Semantic versions of the develop operations (bump when the same settings would render
// differently). A recipe records the versions it was made with (DevelopSettings::operations);
// this build renders exactly these versions and rejects any other (VersionMismatch).
inline constexpr std::uint32_t kWhiteBalanceOperationVersion = 1;  // correction relative to stage one
inline constexpr std::uint32_t kExposureOperationVersion = 1;
inline constexpr std::uint32_t kLocalToneOperationVersion = 1;  // highlights and shadows
inline constexpr std::uint32_t kContrastOperationVersion = 1;
inline constexpr std::uint32_t kToneCurveOperationVersion = 1;
inline constexpr std::uint32_t kColorOperationVersion = 1;  // saturation and vibrance

// The base looks this build implements.
inline constexpr std::string_view kNeutralLookId = "neutral";
inline constexpr std::uint32_t kNeutralLookVersion = 1;

// ---- Ranges -----------------------------------------------------------------------------------

inline constexpr double kMaxExposureEv = 5.0;  // exposure_ev in [-5, +5]
// contrast, highlights, shadows, saturation and vibrance lie in [-1, +1].
inline constexpr double kMaxAdjustment = 1.0;
// Kelvin white balance (the range of common raw editors' temperature and tint sliders; within
// the 1667 K .. infinity span of the Robertson table used for the mapping).
inline constexpr double kMinTemperatureK = 2000.0;
inline constexpr double kMaxTemperatureK = 50000.0;
inline constexpr double kMaxTint = 150.0;  // tint in [-150, +150]
// CameraNeutral: each multiplier divided by the green one must lie in [1/64, 64] (a sanity bound
// far outside real illuminants; it rejects corrupt values before they reach pixels).
inline constexpr double kMaxMultiplierRatio = 64.0;
// Tone curve: 2 to 32 control points.
inline constexpr std::size_t kMinToneCurvePoints = 2;
inline constexpr std::size_t kMaxToneCurvePoints = 32;

// ---- White balance ----------------------------------------------------------------------------

// Persisted white-balance mode identifiers (append only).
enum class WhiteBalanceMode : std::uint8_t {
    AsShot = 1,
    Kelvin = 2,
    CameraNeutral = 3,
};

[[nodiscard]] std::string_view to_string(WhiteBalanceMode mode) noexcept;  // "as_shot", "kelvin", "camera_neutral"

struct AsShotWhiteBalance {
    friend bool operator==(const AsShotWhiteBalance&, const AsShotWhiteBalance&) = default;
};

struct KelvinWhiteBalance {
    double temperature_k = 5000.0;  // [kMinTemperatureK, kMaxTemperatureK]
    double tint = 0.0;              // [-kMaxTint, kMaxTint], DNG SDK units

    friend bool operator==(const KelvinWhiteBalance&, const KelvinWhiteBalance&) = default;
};

// Multipliers for camera red, green and blue (finite, positive; ratios to green within
// [1/kMaxMultiplierRatio, kMaxMultiplierRatio]).
struct CameraNeutralWhiteBalance {
    double r = 1.0;
    double g = 1.0;
    double b = 1.0;

    friend bool operator==(const CameraNeutralWhiteBalance&, const CameraNeutralWhiteBalance&) = default;
};

using WhiteBalanceSetting = std::variant<AsShotWhiteBalance, KelvinWhiteBalance, CameraNeutralWhiteBalance>;

[[nodiscard]] WhiteBalanceMode mode_of(const WhiteBalanceSetting& setting) noexcept;

// ---- Tone curve -------------------------------------------------------------------------------

struct ToneCurvePoint {
    double x = 0.0;
    double y = 0.0;

    friend bool operator==(const ToneCurvePoint&, const ToneCurvePoint&) = default;
};

// A monotone tone curve in display-referred lightness: x and y are CIE 1976 lightness L* / 100
// of the display-linear value norm max(R, G, B) (for neutrals, the lightness of the displayed
// luminance relative to display white: 0 = black, 0.5 = L* 50 (display-linear 0.184), 1 = white).
// The curve is the Fritsch-Carlson monotone cubic through the points, constant beyond the first
// and last points (ToneCurve, develop_operations.hpp). Points: 2 to 32, finite, within [0, 1],
// x strictly increasing, y non-decreasing. The default {(0, 0), (1, 1)} is the identity.
struct ToneCurveSetting {
    std::vector<ToneCurvePoint> points{{0.0, 0.0}, {1.0, 1.0}};

    // True when the curve is exactly the identity on [0, 1]: the first point is (0, 0), the last
    // (1, 1) and every point has y == x (the monotone cubic through collinear points is linear).
    [[nodiscard]] bool is_identity() const noexcept;

    friend bool operator==(const ToneCurveSetting&, const ToneCurveSetting&) = default;
};

// ---- Base look --------------------------------------------------------------------------------

struct BaseLookSetting {
    std::string id{kNeutralLookId};
    std::uint32_t version = kNeutralLookVersion;

    friend bool operator==(const BaseLookSetting&, const BaseLookSetting&) = default;
};

// ---- Locks ------------------------------------------------------------------------------------

// Persisted identifiers of the user-facing controls (append only).
enum class DevelopControl : std::uint8_t {
    WhiteBalance = 1,
    Exposure = 2,
    Contrast = 3,
    Highlights = 4,
    Shadows = 5,
    ToneCurve = 6,
    Saturation = 7,
    Vibrance = 8,
    BaseLook = 9,
};

// "white_balance", "exposure", "contrast", "highlights", "shadows", "tone_curve", "saturation",
// "vibrance", "base_look"; "invalid" for other values.
[[nodiscard]] std::string_view to_string(DevelopControl control) noexcept;
// The control named by to_string(); std::nullopt for any other text.
[[nodiscard]] std::optional<DevelopControl> parse_develop_control(std::string_view text) noexcept;

// ---- Operation versions -----------------------------------------------------------------------

// The semantic version of every operation the settings were made for.
struct DevelopOperationVersions {
    std::uint32_t white_balance = kWhiteBalanceOperationVersion;
    std::uint32_t exposure = kExposureOperationVersion;
    std::uint32_t local_tone = kLocalToneOperationVersion;
    std::uint32_t contrast = kContrastOperationVersion;
    std::uint32_t tone_curve = kToneCurveOperationVersion;
    std::uint32_t color = kColorOperationVersion;

    friend bool operator==(const DevelopOperationVersions&, const DevelopOperationVersions&) = default;
};

// ---- Settings ---------------------------------------------------------------------------------

struct DevelopSettings {
    std::uint32_t version = kDevelopSettingsVersion;
    WhiteBalanceSetting white_balance = AsShotWhiteBalance{};
    double exposure_ev = 0.0;
    double contrast = 0.0;
    double highlights = 0.0;
    double shadows = 0.0;
    ToneCurveSetting tone_curve;
    double saturation = 0.0;
    double vibrance = 0.0;
    BaseLookSetting base_look;
    std::set<DevelopControl> locks;  // ordered: deterministic iteration and comparison
    DevelopOperationVersions operations;

    // Checks the whole value (Error::where "engine.develop.settings"):
    //   VersionMismatch  `version` or an operation version is not the one this build implements,
    //                    or the base look's version is not implemented (newer or retired);
    //   Unsupported      an unknown base-look id;
    //   InvalidArgument  an unknown enum value (white-balance mode, lock), a non-finite number,
    //                    a value outside its range (see the constants above) or a tone curve that
    //                    is not monotone as ToneCurveSetting requires.
    [[nodiscard]] Status validate() const;

    // True when every pixel-affecting value equals the default (locks and the recorded
    // operation versions are ignored): the image renders "as shot".
    [[nodiscard]] bool is_as_shot() const noexcept;

    friend bool operator==(const DevelopSettings&, const DevelopSettings&) = default;
};

}  // namespace lucent::engine
