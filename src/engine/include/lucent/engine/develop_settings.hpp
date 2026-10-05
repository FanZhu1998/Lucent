#pragma once

// DevelopSettings v2: the photographic settings of one developed image (blueprint section 5,
// step 6 "Apply photographic operations", section 5 "Noise, masks, and geometry", and section 8
// "Recipes, assets, and forward compatibility"). Shared contract "lucent.develop", version 2
// (ADR 0010, ADR 0015): the engine owns this C++ type and its validation; JSON serialisation and
// persistence belong to the recipe layer.
//
// Every value is a plain number, enum or list, so the type can be copied, compared exactly
// (operator==) and serialised without loss. Default-constructed settings are "as shot": the
// camera's white balance, no adjustment, the identity tone curve, the "neutral" v1 base look, no
// chroma denoising, no local adjustment and no geometry change.
//
// Versions. Version 2 adds chroma_denoise, local_adjustments and geometry (each with its own
// operation version) to version 1's fields, whose meaning is unchanged. A version-1 value is still
// a valid input: validate() accepts it when it holds none of version 2's members (they keep their
// defaults), and it renders exactly as its conversion upgrade_develop_settings() does.
//
// Semantics (the operations are implemented in develop_operations.hpp, chroma_denoise.hpp,
// local_adjustments.hpp and develop_geometry.hpp and run in this order by develop.hpp; each one
// carries its own semantic version, recorded in `operations`):
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
//   chroma_denoise guided chroma denoising of the linear working image (ChromaDenoiseSetting).
//   highlights     local luminance change of the bright tones, -1..+1: negative compresses
//                  (darkens) them, positive brightens them (up to 1.5 EV at full weight).
//   shadows        local luminance change of the dark tones, -1..+1: positive lifts them,
//                  negative deepens them (up to 2 EV at full weight). Both are driven by a
//                  guided-filter base layer of the log luminance (edge aware, no halos).
//   local_adjustments  brush, linear-gradient and radial masks with local controls
//                  (LocalAdjustment), in list order.
//   contrast       S-curve in display lightness around L* = 50, -1..+1 (0 = identity).
//   tone_curve     monotone control points in display lightness (see ToneCurveSetting).
//   saturation     chroma scale around the pixel's luminance, -1..+1 (factor 1 + saturation).
//   vibrance       chroma scale weighted towards muted colours, -1..+1.
//   base_look      the scene-to-display rendering, id + version ("neutral" v1: a hue-preserving
//                  filmic tone curve, develop_operations.hpp).
//   geometry       straighten, crop, quarter turns and horizontal flip of the rendered image
//                  (GeometrySetting), the last operation.
//   locks          controls the automatic optimiser (W8) must not change; stored, not used here.
//
// Coordinates (ADR 0015). Masks and the crop are resolution independent:
//   * The upright frame U is the image stage one produces (develop.hpp): after the DNG opcodes
//     (warps, trims; ADR 0014) and the camera orientation, before any geometry setting. A
//     position is normalised per axis: (x, y) = (column / width, row / height) in continuous
//     pixel coordinates (image/geometry.hpp: pixel (i, j) covers [i, i + 1) x [j, j + 1)), so
//     (0, 0) is the top-left corner and (1, 1) the bottom-right corner of the image, at any
//     render scale. Lengths (brush and radial radii) are fractions of the image's long edge.
//   * Mask shapes are defined in U, before straightening and cropping: changing the crop, the
//     straighten angle or the quarter turns never moves a mask relative to the image content;
//     develop_geometry.hpp maps mask positions to output pixels (and back) for overlays.
//   * The crop rectangle is normalised in the straightened frame S (U rotated about its centre,
//     same canvas); with no straightening S = U.
//
// Thread-safety: a value type; concurrent reads are safe.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <lucent/core/error.hpp>
#include <lucent/image/geometry.hpp>

namespace lucent::engine {

// ---- Schema and operation versions ------------------------------------------------------------

// Shared contract identifier and version of DevelopSettings.
inline constexpr std::string_view kDevelopSettingsSchema = "lucent.develop";
inline constexpr std::uint32_t kDevelopSettingsVersion = 2;
// The oldest version this build accepts (upgrade_develop_settings() converts it).
inline constexpr std::uint32_t kOldestDevelopSettingsVersion = 1;

// Semantic versions of the develop operations (bump when the same settings would render
// differently). A recipe records the versions it was made with (DevelopSettings::operations);
// this build renders exactly these versions and rejects any other (VersionMismatch).
inline constexpr std::uint32_t kWhiteBalanceOperationVersion = 1;  // correction relative to stage one
inline constexpr std::uint32_t kExposureOperationVersion = 1;
inline constexpr std::uint32_t kLocalToneOperationVersion = 1;  // highlights and shadows
inline constexpr std::uint32_t kContrastOperationVersion = 1;
inline constexpr std::uint32_t kToneCurveOperationVersion = 1;
inline constexpr std::uint32_t kColorOperationVersion = 1;  // saturation and vibrance
// Added by version 2 (ADR 0015).
inline constexpr std::uint32_t kChromaDenoiseOperationVersion = 1;
// Masks (shape definitions, coordinate frame, rasterisation) and their local controls.
inline constexpr std::uint32_t kLocalAdjustmentsOperationVersion = 1;
// Straighten (resampling kernel), crop (pixel rounding), quarter turns and flip.
inline constexpr std::uint32_t kGeometryOperationVersion = 1;

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
// Tone curve: 2 to 32 control points, x at least kMinToneCurveGap apart (closer points would
// make the curve's secants overflow; the editor keeps them 0.01 apart).
inline constexpr std::size_t kMinToneCurvePoints = 2;
inline constexpr std::size_t kMaxToneCurvePoints = 32;
inline constexpr double kMinToneCurveGap = 1e-6;

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
// x increasing by at least kMinToneCurveGap, y non-decreasing. The default {(0, 0), (1, 1)} is
// the identity.
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

// ---- Chroma denoise (version 2) ---------------------------------------------------------------

// Radius bounds in full-image pixels (chroma_denoise.hpp scales the radius for proxies).
inline constexpr double kMinChromaDenoiseRadius = 1.0;
inline constexpr double kMaxChromaDenoiseRadius = 64.0;
inline constexpr double kDefaultChromaDenoiseRadius = 8.0;

// Guided chroma denoising (blueprint section 5, "Noise, masks, and geometry"; chroma_denoise.hpp):
// after camera colour, in linear working RGB, the two colour-difference planes are filtered with
// the luminance as guide and blended with weight `strength`; the luminance is preserved.
//   strength  [0, 1]: 0 (the default) is the identity (the operation is skipped), 1 replaces the
//             chroma by its guided-filtered value.
//   radius    guided-filter radius in full-image pixels, [kMinChromaDenoiseRadius,
//             kMaxChromaDenoiseRadius] (a proxy scales the window: chroma_denoise_radius()).
struct ChromaDenoiseSetting {
    double strength = 0.0;
    double radius = kDefaultChromaDenoiseRadius;

    [[nodiscard]] bool is_identity() const noexcept { return strength == 0.0; }

    friend bool operator==(const ChromaDenoiseSetting&, const ChromaDenoiseSetting&) = default;
};

// ---- Local adjustments: masks (version 2) -----------------------------------------------------

// Bounded sizes (validate()): masks per settings, strokes per brush mask, points per stroke and
// points of all strokes of all masks together (a recipe stays a few megabytes at most).
inline constexpr std::size_t kMaxLocalAdjustments = 32;
inline constexpr std::size_t kMaxBrushStrokes = 256;
inline constexpr std::size_t kMaxStrokePoints = 4096;
inline constexpr std::size_t kMaxBrushPoints = 65536;
// Normalised positions (points, centres, gradient ends) lie within [-kMaskPositionMargin,
// 1 + kMaskPositionMargin]: a shape may extend beyond the image by one image size.
inline constexpr double kMaskPositionMargin = 1.0;
// Lengths are fractions of the upright image's long edge.
inline constexpr double kMinBrushRadius = 1e-4;
inline constexpr double kMaxBrushRadius = 0.5;
inline constexpr double kMinRadialRadius = 1e-4;
inline constexpr double kMaxRadialRadius = 2.0;
// A linear gradient's start and end differ by at least this much in x or y.
inline constexpr double kMinGradientLength = 1e-4;
inline constexpr double kMaxAngleDegrees = 180.0;  // radial mask rotation in [-180, 180]
// Local controls: exposure in [-4, +4] EV, the others in [-1, +1].
inline constexpr double kMaxLocalExposureEv = 4.0;

// Persisted mask kinds (append only).
enum class MaskKind : std::uint8_t {
    Brush = 1,
    LinearGradient = 2,
    RadialGradient = 3,
};

[[nodiscard]] std::string_view to_string(MaskKind kind) noexcept;  // "brush", "linear", "radial"
[[nodiscard]] std::optional<MaskKind> parse_mask_kind(std::string_view text) noexcept;

// Persisted brush-stroke modes (append only).
enum class BrushMode : std::uint8_t {
    Add = 1,    // the stroke adds coverage: m' = m + a (1 - m)
    Erase = 2,  // the stroke removes coverage: m' = m (1 - a)
};

[[nodiscard]] std::string_view to_string(BrushMode mode) noexcept;  // "add", "erase"
[[nodiscard]] std::optional<BrushMode> parse_brush_mode(std::string_view text) noexcept;

// A position in the normalised upright frame (see "Coordinates" above).
struct MaskPoint {
    double x = 0.0;
    double y = 0.0;

    friend bool operator==(const MaskPoint&, const MaskPoint&) = default;
};

// One brush stroke: the polyline through `points` (one point is a dab) painted with a round brush.
// A pixel at distance d (pixels) from the polyline gets alpha a = flow x (1 - smoothstep(h R, R,
// d)), R = radius x the long edge, h = hardness; strokes are composed in order by their mode.
// Within one stroke overlapping segments do not accumulate (the distance to the whole polyline
// counts).
struct BrushStroke {
    BrushMode mode = BrushMode::Add;
    double radius = 0.02;   // [kMinBrushRadius, kMaxBrushRadius], fraction of the long edge
    double hardness = 0.5;  // [0, 1]: fraction of the radius at full alpha (1: a hard edge)
    double flow = 1.0;      // [0, 1]: the stroke's alpha at full coverage
    std::vector<MaskPoint> points;  // 1 .. kMaxStrokePoints

    friend bool operator==(const BrushStroke&, const BrushStroke&) = default;
};

// A painted mask: coverage starts at 0 and the strokes are applied in order. No stroke means no
// coverage (a brush mask the user has not painted yet).
struct BrushMask {
    std::vector<BrushStroke> strokes;  // 0 .. kMaxBrushStrokes

    friend bool operator==(const BrushMask&, const BrushMask&) = default;
};

// A linear gradient: coverage 1 on the side of `start`, 0 beyond `end`, falling across the band
// between the lines through start and end perpendicular to start -> end (in pixels). With t the
// position along start -> end (0 at start, 1 at end), coverage = 1 - smoothstep(0.5 - f / 2,
// 0.5 + f / 2, t), f = feather: 1 (the default) spreads the transition over the whole band, 0 is a
// hard edge at the middle line.
struct LinearGradientMask {
    MaskPoint start{0.5, 0.25};
    MaskPoint end{0.5, 0.75};
    double feather = 1.0;  // [0, 1]

    friend bool operator==(const LinearGradientMask&, const LinearGradientMask&) = default;
};

// A radial (elliptical) mask: coverage 1 inside the ellipse's core, falling to 0 at its boundary.
// With rho the normalised elliptical radius (1 on the ellipse, whose semi-axes are radius_x and
// radius_y times the long edge, the first rotated by angle_degrees counter-clockwise as
// displayed), coverage = 1 - smoothstep(1 - feather, 1, rho).
struct RadialGradientMask {
    MaskPoint center{0.5, 0.5};
    double radius_x = 0.25;  // [kMinRadialRadius, kMaxRadialRadius], fraction of the long edge
    double radius_y = 0.25;
    double angle_degrees = 0.0;  // [-kMaxAngleDegrees, kMaxAngleDegrees]
    double feather = 0.5;        // [0, 1]

    friend bool operator==(const RadialGradientMask&, const RadialGradientMask&) = default;
};

using MaskShape = std::variant<BrushMask, LinearGradientMask, RadialGradientMask>;

[[nodiscard]] MaskKind kind_of(const MaskShape& shape) noexcept;

// Every transition (a brush edge, a gradient band, a radial falloff) is at least one pixel of the
// render wide, centred on its nominal position, so hard masks are antialiased at any scale
// (local_adjustments.hpp).

// The local controls of a mask, applied with the mask's weight w in [0, 1] (local_adjustments.hpp).
// Overlapping masks add their weighted amounts; each sum is limited to the control's range.
struct LocalControls {
    double exposure_ev = 0.0;  // [-kMaxLocalExposureEv, +kMaxLocalExposureEv]: gain 2^(w x ev)
    double contrast = 0.0;     // [-1, 1]: bounded S-curve in EV around display middle grey
    double highlights = 0.0;   // [-1, 1]: as DevelopSettings::highlights, on the masked area
    double shadows = 0.0;      // [-1, 1]: as DevelopSettings::shadows, on the masked area
    double saturation = 0.0;   // [-1, 1]: chroma scale 1 + w x saturation around the luminance
    double temperature = 0.0;  // [-1, 1]: positive warms (see local_adjustments.hpp)
    double tint = 0.0;         // [-1, 1]: positive towards magenta, as the white-balance tint

    [[nodiscard]] bool is_identity() const noexcept;

    friend bool operator==(const LocalControls&, const LocalControls&) = default;
};

// One local adjustment: a mask and its controls. The weight of a pixel is
//     w = opacity x (invert ? 1 - coverage : coverage)   (0 when disabled).
struct LocalAdjustment {
    std::uint32_t id = 1;  // >= 1, unique within the list: the editor's stable identity of the mask
    bool enabled = true;
    bool invert = false;
    double opacity = 1.0;  // [0, 1]
    MaskShape shape = RadialGradientMask{};
    LocalControls controls;

    [[nodiscard]] MaskKind kind() const noexcept { return kind_of(shape); }
    // True when the adjustment changes pixels: enabled, opacity above 0 and a control not 0.
    [[nodiscard]] bool has_effect() const noexcept;

    friend bool operator==(const LocalAdjustment&, const LocalAdjustment&) = default;
};

// ---- Geometry (version 2) ---------------------------------------------------------------------

inline constexpr double kMaxStraightenDegrees = 45.0;  // straighten_degrees in [-45, +45]
// The crop keeps at least this fraction of the width and of the height.
inline constexpr double kMinCropExtent = 0.001;

// A crop rectangle normalised in the straightened frame: 0 <= left < right <= 1, 0 <= top <
// bottom <= 1 (fractions of the width and height). The default keeps the whole image.
struct CropRect {
    double left = 0.0;
    double top = 0.0;
    double right = 1.0;
    double bottom = 1.0;

    [[nodiscard]] bool is_full() const noexcept {
        return left == 0.0 && top == 0.0 && right == 1.0 && bottom == 1.0;
    }

    friend bool operator==(const CropRect&, const CropRect&) = default;
};

// The geometry applied at the end of stage two (develop_geometry.hpp), in this order:
//   1. straighten: the upright image rotated by straighten_degrees about its centre
//      (counter-clockwise as displayed for positive angles) on a canvas of the same size,
//      resampled with the Keys cubic convolution kernel a = -0.5 (Catmull-Rom), each result
//      limited to the range of its four nearest pixels; canvas points the rotated image does not
//      cover are black (largest_inside_crop() gives a crop inside the image);
//   2. crop: the rectangle `crop` of that canvas, rounded to whole pixels of the render;
//   3. orientation: flip_horizontal (mirror left-right), then quarter_turns x 90 degrees
//      clockwise, on top of the camera orientation stage one already applied.
struct GeometrySetting {
    double straighten_degrees = 0.0;  // [-kMaxStraightenDegrees, kMaxStraightenDegrees]
    CropRect crop;
    std::uint32_t quarter_turns = 0;  // 0..3, clockwise
    bool flip_horizontal = false;

    // True when the geometry changes nothing (no straighten, full crop, no turn, no flip).
    [[nodiscard]] bool is_identity() const noexcept;

    friend bool operator==(const GeometrySetting&, const GeometrySetting&) = default;
};

// The orientation step of `geometry` (flip_horizontal, then the quarter turns) as an
// image::Orientation of the cropped image (Normal for out-of-range quarter turns, which
// validate() rejects).
[[nodiscard]] image::Orientation user_orientation(const GeometrySetting& geometry) noexcept;

// The geometry after the user turns the displayed image by 90 degrees clockwise or
// counter-clockwise, or mirrors it left-right as displayed. Crop and straighten stay attached to
// the image content (they come before the orientation step); a mirror after an odd number of
// quarter turns therefore becomes (4 - quarter_turns) % 4 turns with flip_horizontal toggled.
[[nodiscard]] GeometrySetting rotated_clockwise(GeometrySetting geometry) noexcept;
[[nodiscard]] GeometrySetting rotated_counter_clockwise(GeometrySetting geometry) noexcept;
[[nodiscard]] GeometrySetting flipped_horizontally(GeometrySetting geometry) noexcept;

// ---- Revisions --------------------------------------------------------------------------------

// Content hashes (64-bit FNV-1a over the operation version and the values, -0 hashed as 0):
// equal values have equal revisions, so a revision identifies a geometry or a mask shape for
// caches (mask rasters, overlays) without a counter that would change the settings' identity.
// Not a cryptographic hash: caches compare the values themselves on a hit.
[[nodiscard]] std::uint64_t geometry_revision(const GeometrySetting& geometry) noexcept;
[[nodiscard]] std::uint64_t mask_shape_revision(const MaskShape& shape) noexcept;

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
    // Version 2.
    std::uint32_t chroma_denoise = kChromaDenoiseOperationVersion;
    std::uint32_t local_adjustments = kLocalAdjustmentsOperationVersion;
    std::uint32_t geometry = kGeometryOperationVersion;

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
    // Version 2.
    ChromaDenoiseSetting chroma_denoise;
    std::vector<LocalAdjustment> local_adjustments;  // applied in this order
    GeometrySetting geometry;

    // Checks the whole value (Error::where "engine.develop.settings"):
    //   VersionMismatch  `version` is neither kDevelopSettingsVersion nor an older version this
    //                    build reads (kOldestDevelopSettingsVersion), an operation version is not
    //                    the one this build implements, or the base look's version is not
    //                    implemented (newer or retired);
    //   Unsupported      an unknown base-look id;
    //   InvalidArgument  an unknown enum value (white-balance mode, lock, brush mode), a non-finite
    //                    number, a value outside its range (see the constants above), a tone curve
    //                    that is not monotone as ToneCurveSetting requires, a crop that is not
    //                    ordered or not inside [0, 1], a mask id that is 0 or repeated, a stroke
    //                    without points, more masks, strokes or points than the bounds allow, a
    //                    linear gradient whose ends coincide, or a version-1 value holding a
    //                    member of version 2 (anything but its default).
    [[nodiscard]] Status validate() const;

    // True when every pixel-affecting value equals the default (locks, the recorded operation
    // versions, the chroma-denoise radius at strength 0 and local adjustments without effect are
    // ignored): the image renders "as shot".
    [[nodiscard]] bool is_as_shot() const noexcept;

    friend bool operator==(const DevelopSettings&, const DevelopSettings&) = default;
};

// Converts settings of an older version this build reads to kDevelopSettingsVersion:
//   version 1 -> 2: no chroma denoising, no local adjustment, no geometry, and this build's
//   versions of the operations version 2 added. The result renders exactly as the version-1
//   value does.
// Current settings are returned unchanged. Errors (engine.develop.settings): VersionMismatch for
// a version this build does not read (0, or newer); InvalidArgument for a version-1 value that
// holds a member of version 2. The other fields are not checked (call validate()).
[[nodiscard]] Expected<DevelopSettings> upgrade_develop_settings(DevelopSettings settings);

}  // namespace lucent::engine
