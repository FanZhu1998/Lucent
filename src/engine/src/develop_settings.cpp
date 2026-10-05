// DevelopSettings v2 validation, the v1 -> v2 conversion and helpers (develop_settings.hpp).

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <lucent/engine/develop_settings.hpp>

namespace lucent::engine {

namespace {

constexpr const char* kWhere = "engine.develop.settings";

constexpr std::array<std::pair<DevelopControl, std::string_view>, 9> kControlNames = {{
    {DevelopControl::WhiteBalance, "white_balance"},
    {DevelopControl::Exposure, "exposure"},
    {DevelopControl::Contrast, "contrast"},
    {DevelopControl::Highlights, "highlights"},
    {DevelopControl::Shadows, "shadows"},
    {DevelopControl::ToneCurve, "tone_curve"},
    {DevelopControl::Saturation, "saturation"},
    {DevelopControl::Vibrance, "vibrance"},
    {DevelopControl::BaseLook, "base_look"},
}};

Status check_range(double value, double low, double high, std::string_view name) {
    if (!std::isfinite(value) || value < low || value > high) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("{} is {}; it must be finite and within [{}, {}]", name, value, low, high), kWhere);
    }
    return ok();
}

Status check_operation_version(std::uint32_t found, std::uint32_t supported, std::string_view name) {
    if (found != supported) {
        return fail(ErrorCode::VersionMismatch,
                    std::format("the {} operation has version {}; this build implements version {}", name, found,
                                supported),
                    kWhere);
    }
    return ok();
}

Status check_white_balance(const WhiteBalanceSetting& setting) {
    if (setting.valueless_by_exception()) {
        return fail(ErrorCode::InvalidArgument, "the white-balance setting holds no value", kWhere);
    }
    if (const auto* kelvin = std::get_if<KelvinWhiteBalance>(&setting)) {
        if (auto status = check_range(kelvin->temperature_k, kMinTemperatureK, kMaxTemperatureK,
                                      "the white-balance temperature (K)");
            !status) {
            return status;
        }
        return check_range(kelvin->tint, -kMaxTint, kMaxTint, "the white-balance tint");
    }
    if (const auto* neutral = std::get_if<CameraNeutralWhiteBalance>(&setting)) {
        const std::array<double, 3> m = {neutral->r, neutral->g, neutral->b};
        for (std::size_t c = 0; c < 3; ++c) {
            if (!std::isfinite(m[c]) || !(m[c] > 0.0)) {
                return fail(ErrorCode::InvalidArgument,
                            std::format("camera-neutral multiplier {} is {}; multipliers must be finite and positive",
                                        "rgb"[c], m[c]),
                            kWhere);
            }
        }
        for (std::size_t c = 0; c < 3; ++c) {
            const double ratio = m[c] / m[1];
            if (!(ratio >= 1.0 / kMaxMultiplierRatio) || !(ratio <= kMaxMultiplierRatio)) {
                return fail(ErrorCode::InvalidArgument,
                            std::format("camera-neutral multiplier {} is {} times the green one; the ratio must lie "
                                        "within [1/{}, {}]",
                                        "rgb"[c], ratio, kMaxMultiplierRatio, kMaxMultiplierRatio),
                            kWhere);
            }
        }
    }
    return ok();
}

Status check_tone_curve(const ToneCurveSetting& curve) {
    const auto& p = curve.points;
    if (p.size() < kMinToneCurvePoints || p.size() > kMaxToneCurvePoints) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the tone curve has {} points; it needs {} to {}", p.size(), kMinToneCurvePoints,
                                kMaxToneCurvePoints),
                    kWhere);
    }
    for (std::size_t i = 0; i < p.size(); ++i) {
        const ToneCurvePoint& q = p[i];
        if (!std::isfinite(q.x) || !std::isfinite(q.y) || q.x < 0.0 || q.x > 1.0 || q.y < 0.0 || q.y > 1.0) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("tone-curve point {} ({}, {}) must be finite and within [0, 1]", i, q.x, q.y),
                        kWhere);
        }
        if (i > 0 && !(q.x - p[i - 1].x >= kMinToneCurveGap)) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("tone-curve x values must increase by at least {} (point {} has x {} "
                                    "after {})",
                                    kMinToneCurveGap, i, q.x, p[i - 1].x),
                        kWhere);
        }
        if (i > 0 && q.y < p[i - 1].y) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("tone-curve y values must not decrease (point {} has y {} after {})", i, q.y,
                                    p[i - 1].y),
                        kWhere);
        }
    }
    return ok();
}

}  // namespace

std::string_view to_string(WhiteBalanceMode mode) noexcept {
    switch (mode) {
        case WhiteBalanceMode::AsShot:
            return "as_shot";
        case WhiteBalanceMode::Kelvin:
            return "kelvin";
        case WhiteBalanceMode::CameraNeutral:
            return "camera_neutral";
    }
    return "invalid";
}

WhiteBalanceMode mode_of(const WhiteBalanceSetting& setting) noexcept {
    if (std::holds_alternative<KelvinWhiteBalance>(setting)) {
        return WhiteBalanceMode::Kelvin;
    }
    if (std::holds_alternative<CameraNeutralWhiteBalance>(setting)) {
        return WhiteBalanceMode::CameraNeutral;
    }
    return WhiteBalanceMode::AsShot;
}

std::string_view to_string(DevelopControl control) noexcept {
    for (const auto& [value, name] : kControlNames) {
        if (value == control) {
            return name;
        }
    }
    return "invalid";
}

std::optional<DevelopControl> parse_develop_control(std::string_view text) noexcept {
    for (const auto& [value, name] : kControlNames) {
        if (name == text) {
            return value;
        }
    }
    return std::nullopt;
}

bool ToneCurveSetting::is_identity() const noexcept {
    if (points.size() < kMinToneCurvePoints) {
        return false;
    }
    if (points.front() != ToneCurvePoint{0.0, 0.0} || points.back() != ToneCurvePoint{1.0, 1.0}) {
        return false;
    }
    for (const ToneCurvePoint& p : points) {
        if (p.x != p.y) {
            return false;
        }
    }
    return true;
}


// ---- Version 2 members ----------------------------------------------------------------------------

namespace {

Status check_position(const MaskPoint& p, std::string_view what) {
    constexpr double lo = -kMaskPositionMargin;
    constexpr double hi = 1.0 + kMaskPositionMargin;
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || p.x < lo || p.x > hi || p.y < lo || p.y > hi) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("{} ({}, {}) must be finite and within [{}, {}]", what, p.x, p.y, lo, hi), kWhere);
    }
    return ok();
}

Status check_brush(const BrushMask& brush, std::uint32_t id, std::size_t& points) {
    if (brush.strokes.size() > kMaxBrushStrokes) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("brush mask {} has {} strokes; at most {} are allowed", id, brush.strokes.size(),
                                kMaxBrushStrokes),
                    kWhere);
    }
    for (std::size_t k = 0; k < brush.strokes.size(); ++k) {
        const BrushStroke& stroke = brush.strokes[k];
        const std::string what = std::format("brush mask {} stroke {}", id, k);
        if (stroke.mode != BrushMode::Add && stroke.mode != BrushMode::Erase) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("{} has an unknown mode {}", what, static_cast<std::uint32_t>(stroke.mode)),
                        kWhere);
        }
        if (auto status = check_range(stroke.radius, kMinBrushRadius, kMaxBrushRadius, what + " radius"); !status) {
            return status;
        }
        if (auto status = check_range(stroke.hardness, 0.0, 1.0, what + " hardness"); !status) {
            return status;
        }
        if (auto status = check_range(stroke.flow, 0.0, 1.0, what + " flow"); !status) {
            return status;
        }
        if (stroke.points.empty() || stroke.points.size() > kMaxStrokePoints) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("{} has {} points; it needs 1 to {}", what, stroke.points.size(), kMaxStrokePoints),
                        kWhere);
        }
        points += stroke.points.size();
        if (points > kMaxBrushPoints) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("the brush masks hold more than {} points together", kMaxBrushPoints), kWhere);
        }
        for (const MaskPoint& p : stroke.points) {
            if (auto status = check_position(p, what + " point"); !status) {
                return status;
            }
        }
    }
    return ok();
}

Status check_shape(const MaskShape& shape, std::uint32_t id, std::size_t& points) {
    if (shape.valueless_by_exception()) {
        return fail(ErrorCode::InvalidArgument, std::format("mask {} holds no shape", id), kWhere);
    }
    if (const auto* brush = std::get_if<BrushMask>(&shape)) {
        return check_brush(*brush, id, points);
    }
    if (const auto* linear = std::get_if<LinearGradientMask>(&shape)) {
        const std::string what = std::format("linear gradient {}", id);
        if (auto status = check_position(linear->start, what + " start"); !status) {
            return status;
        }
        if (auto status = check_position(linear->end, what + " end"); !status) {
            return status;
        }
        if (!(std::max(std::abs(linear->end.x - linear->start.x), std::abs(linear->end.y - linear->start.y)) >=
              kMinGradientLength)) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("{}: start and end must differ by at least {} in x or y", what, kMinGradientLength),
                        kWhere);
        }
        return check_range(linear->feather, 0.0, 1.0, what + " feather");
    }
    const auto& radial = std::get<RadialGradientMask>(shape);
    const std::string what = std::format("radial mask {}", id);
    if (auto status = check_position(radial.center, what + " centre"); !status) {
        return status;
    }
    if (auto status = check_range(radial.radius_x, kMinRadialRadius, kMaxRadialRadius, what + " radius_x"); !status) {
        return status;
    }
    if (auto status = check_range(radial.radius_y, kMinRadialRadius, kMaxRadialRadius, what + " radius_y"); !status) {
        return status;
    }
    if (auto status = check_range(radial.angle_degrees, -kMaxAngleDegrees, kMaxAngleDegrees, what + " angle");
        !status) {
        return status;
    }
    return check_range(radial.feather, 0.0, 1.0, what + " feather");
}

Status check_local_adjustments(const std::vector<LocalAdjustment>& adjustments) {
    if (adjustments.size() > kMaxLocalAdjustments) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("{} local adjustments; at most {} are allowed", adjustments.size(),
                                kMaxLocalAdjustments),
                    kWhere);
    }
    std::vector<std::uint32_t> ids;
    ids.reserve(adjustments.size());
    std::size_t points = 0;
    for (const LocalAdjustment& a : adjustments) {
        if (a.id == 0) {
            return fail(ErrorCode::InvalidArgument, "a local adjustment id must be at least 1", kWhere);
        }
        if (std::find(ids.begin(), ids.end(), a.id) != ids.end()) {
            return fail(ErrorCode::InvalidArgument, std::format("local adjustment id {} is repeated", a.id), kWhere);
        }
        ids.push_back(a.id);
        const std::string what = std::format("local adjustment {}", a.id);
        if (auto status = check_range(a.opacity, 0.0, 1.0, what + " opacity"); !status) {
            return status;
        }
        const LocalControls& c = a.controls;
        if (auto status =
                check_range(c.exposure_ev, -kMaxLocalExposureEv, kMaxLocalExposureEv, what + " exposure (EV)");
            !status) {
            return status;
        }
        const std::array<std::pair<double, std::string_view>, 6> unit = {{
            {c.contrast, "contrast"},
            {c.highlights, "highlights"},
            {c.shadows, "shadows"},
            {c.saturation, "saturation"},
            {c.temperature, "temperature"},
            {c.tint, "tint"},
        }};
        for (const auto& [value, name] : unit) {
            if (auto status = check_range(value, -kMaxAdjustment, kMaxAdjustment, std::format("{} {}", what, name));
                !status) {
                return status;
            }
        }
        if (auto status = check_shape(a.shape, a.id, points); !status) {
            return status;
        }
    }
    return ok();
}

Status check_geometry(const GeometrySetting& g) {
    if (auto status = check_range(g.straighten_degrees, -kMaxStraightenDegrees, kMaxStraightenDegrees,
                                  "the straighten angle (degrees)");
        !status) {
        return status;
    }
    const CropRect& c = g.crop;
    if (!std::isfinite(c.left) || !std::isfinite(c.top) || !std::isfinite(c.right) || !std::isfinite(c.bottom) ||
        c.left < 0.0 || c.top < 0.0 || c.right > 1.0 || c.bottom > 1.0 || !(c.right - c.left >= kMinCropExtent) ||
        !(c.bottom - c.top >= kMinCropExtent)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the crop ({}, {}) - ({}, {}) must lie within [0, 1] with left < right, top < bottom "
                                "and keep at least {} of each side",
                                c.left, c.top, c.right, c.bottom, kMinCropExtent),
                    kWhere);
    }
    if (g.quarter_turns > 3) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("quarter turns must be 0 to 3 (got {})", g.quarter_turns), kWhere);
    }
    return ok();
}

Status check_chroma_denoise(const ChromaDenoiseSetting& d) {
    if (auto status = check_range(d.strength, 0.0, 1.0, "the chroma-denoise strength"); !status) {
        return status;
    }
    return check_range(d.radius, kMinChromaDenoiseRadius, kMaxChromaDenoiseRadius,
                       "the chroma-denoise radius (full-image pixels)");
}

// A version-1 value holds none of version 2's members.
Status check_version_1(const DevelopSettings& s) {
    const DevelopOperationVersions current;
    if (!(s.chroma_denoise == ChromaDenoiseSetting{}) || !s.local_adjustments.empty() ||
        !(s.geometry == GeometrySetting{}) || s.operations.chroma_denoise != current.chroma_denoise ||
        s.operations.local_adjustments != current.local_adjustments || s.operations.geometry != current.geometry) {
        return fail(ErrorCode::InvalidArgument,
                    "version-1 settings cannot hold chroma denoising, local adjustments or geometry (members of "
                    "version 2)",
                    kWhere);
    }
    return ok();
}

// ---- FNV-1a ---------------------------------------------------------------------------------------

class Fnv1a {
public:
    void add(std::uint64_t value) noexcept {
        for (int i = 0; i < 8; ++i) {
            hash_ ^= (value >> (8 * i)) & 0xFFu;
            hash_ *= 0x100000001b3ull;
        }
    }
    void add(double value) noexcept { add(std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value)); }
    void add(const MaskPoint& p) noexcept {
        add(p.x);
        add(p.y);
    }
    [[nodiscard]] std::uint64_t value() const noexcept { return hash_; }

private:
    std::uint64_t hash_ = 0xcbf29ce484222325ull;
};

image::Orientation quarter_turn_orientation(std::uint32_t turns) noexcept {
    switch (turns) {
        case 1:
            return image::Orientation::Rotate90Cw;
        case 2:
            return image::Orientation::Rotate180;
        case 3:
            return image::Orientation::Rotate90Ccw;
        default:
            return image::Orientation::Normal;
    }
}

}  // namespace

std::string_view to_string(MaskKind kind) noexcept {
    switch (kind) {
        case MaskKind::Brush:
            return "brush";
        case MaskKind::LinearGradient:
            return "linear";
        case MaskKind::RadialGradient:
            return "radial";
    }
    return "invalid";
}

std::optional<MaskKind> parse_mask_kind(std::string_view text) noexcept {
    for (const MaskKind kind : {MaskKind::Brush, MaskKind::LinearGradient, MaskKind::RadialGradient}) {
        if (to_string(kind) == text) {
            return kind;
        }
    }
    return std::nullopt;
}

std::string_view to_string(BrushMode mode) noexcept {
    switch (mode) {
        case BrushMode::Add:
            return "add";
        case BrushMode::Erase:
            return "erase";
    }
    return "invalid";
}

std::optional<BrushMode> parse_brush_mode(std::string_view text) noexcept {
    for (const BrushMode mode : {BrushMode::Add, BrushMode::Erase}) {
        if (to_string(mode) == text) {
            return mode;
        }
    }
    return std::nullopt;
}

MaskKind kind_of(const MaskShape& shape) noexcept {
    if (std::holds_alternative<BrushMask>(shape)) {
        return MaskKind::Brush;
    }
    if (std::holds_alternative<LinearGradientMask>(shape)) {
        return MaskKind::LinearGradient;
    }
    return MaskKind::RadialGradient;
}

bool LocalControls::is_identity() const noexcept {
    return exposure_ev == 0.0 && contrast == 0.0 && highlights == 0.0 && shadows == 0.0 && saturation == 0.0 &&
           temperature == 0.0 && tint == 0.0;
}

bool LocalAdjustment::has_effect() const noexcept { return enabled && opacity > 0.0 && !controls.is_identity(); }

bool GeometrySetting::is_identity() const noexcept {
    return straighten_degrees == 0.0 && crop.is_full() && quarter_turns == 0 && !flip_horizontal;
}

image::Orientation user_orientation(const GeometrySetting& geometry) noexcept {
    const image::Orientation flip =
        geometry.flip_horizontal ? image::Orientation::FlipHorizontal : image::Orientation::Normal;
    return image::compose(flip, quarter_turn_orientation(geometry.quarter_turns));
}

GeometrySetting rotated_clockwise(GeometrySetting geometry) noexcept {
    geometry.quarter_turns = (geometry.quarter_turns % 4 + 1) % 4;
    return geometry;
}

GeometrySetting rotated_counter_clockwise(GeometrySetting geometry) noexcept {
    geometry.quarter_turns = (geometry.quarter_turns % 4 + 3) % 4;
    return geometry;
}

GeometrySetting flipped_horizontally(GeometrySetting geometry) noexcept {
    // F R^q F^f = R^-q F^(1-f): a mirror conjugates a rotation into its inverse.
    geometry.quarter_turns = (4 - geometry.quarter_turns % 4) % 4;
    geometry.flip_horizontal = !geometry.flip_horizontal;
    return geometry;
}

std::uint64_t geometry_revision(const GeometrySetting& geometry) noexcept {
    Fnv1a h;
    h.add(std::uint64_t{kGeometryOperationVersion});
    h.add(geometry.straighten_degrees);
    h.add(geometry.crop.left);
    h.add(geometry.crop.top);
    h.add(geometry.crop.right);
    h.add(geometry.crop.bottom);
    h.add(std::uint64_t{geometry.quarter_turns});
    h.add(std::uint64_t{geometry.flip_horizontal ? 1u : 0u});
    return h.value();
}

std::uint64_t mask_shape_revision(const MaskShape& shape) noexcept {
    Fnv1a h;
    h.add(std::uint64_t{kLocalAdjustmentsOperationVersion});
    h.add(std::uint64_t{static_cast<std::uint8_t>(kind_of(shape))});
    if (const auto* brush = std::get_if<BrushMask>(&shape)) {
        h.add(std::uint64_t{brush->strokes.size()});
        for (const BrushStroke& s : brush->strokes) {
            h.add(std::uint64_t{static_cast<std::uint8_t>(s.mode)});
            h.add(s.radius);
            h.add(s.hardness);
            h.add(s.flow);
            h.add(std::uint64_t{s.points.size()});
            for (const MaskPoint& p : s.points) {
                h.add(p);
            }
        }
    } else if (const auto* linear = std::get_if<LinearGradientMask>(&shape)) {
        h.add(linear->start);
        h.add(linear->end);
        h.add(linear->feather);
    } else if (const auto* radial = std::get_if<RadialGradientMask>(&shape)) {
        h.add(radial->center);
        h.add(radial->radius_x);
        h.add(radial->radius_y);
        h.add(radial->angle_degrees);
        h.add(radial->feather);
    }
    return h.value();
}

// ---- Settings ---------------------------------------------------------------------------------------

Status DevelopSettings::validate() const {
    if (version != kDevelopSettingsVersion && version != kOldestDevelopSettingsVersion) {
        return fail(ErrorCode::VersionMismatch,
                    std::format("{} settings version {} cannot be read by this build (versions {} to {})",
                                kDevelopSettingsSchema, version, kOldestDevelopSettingsVersion,
                                kDevelopSettingsVersion),
                    kWhere);
    }
    if (version == kOldestDevelopSettingsVersion) {
        if (auto status = check_version_1(*this); !status) {
            return status;
        }
    }
    const std::array<std::pair<std::uint32_t, std::pair<std::uint32_t, std::string_view>>, 9> versions = {{
        {operations.white_balance, {kWhiteBalanceOperationVersion, "white-balance"}},
        {operations.exposure, {kExposureOperationVersion, "exposure"}},
        {operations.local_tone, {kLocalToneOperationVersion, "local tone (highlights/shadows)"}},
        {operations.contrast, {kContrastOperationVersion, "contrast"}},
        {operations.tone_curve, {kToneCurveOperationVersion, "tone-curve"}},
        {operations.color, {kColorOperationVersion, "colour (saturation/vibrance)"}},
        {operations.chroma_denoise, {kChromaDenoiseOperationVersion, "chroma-denoise"}},
        {operations.local_adjustments, {kLocalAdjustmentsOperationVersion, "local-adjustments (masks)"}},
        {operations.geometry, {kGeometryOperationVersion, "geometry"}},
    }};
    for (const auto& [found, expected] : versions) {
        if (auto status = check_operation_version(found, expected.first, expected.second); !status) {
            return status;
        }
    }
    if (base_look.id != kNeutralLookId) {
        return fail(ErrorCode::Unsupported, std::format("unknown base look '{}'", base_look.id), kWhere);
    }
    if (base_look.version != kNeutralLookVersion) {
        return fail(ErrorCode::VersionMismatch,
                    std::format("base look '{}' version {} is not implemented by this build (version {})",
                                base_look.id, base_look.version, kNeutralLookVersion),
                    kWhere);
    }
    if (auto status = check_white_balance(white_balance); !status) {
        return status;
    }
    const std::array<std::pair<double, std::pair<double, std::string_view>>, 6> ranges = {{
        {exposure_ev, {kMaxExposureEv, "exposure (EV)"}},
        {contrast, {kMaxAdjustment, "contrast"}},
        {highlights, {kMaxAdjustment, "highlights"}},
        {shadows, {kMaxAdjustment, "shadows"}},
        {saturation, {kMaxAdjustment, "saturation"}},
        {vibrance, {kMaxAdjustment, "vibrance"}},
    }};
    for (const auto& [value, range] : ranges) {
        if (auto status = check_range(value, -range.first, range.first, range.second); !status) {
            return status;
        }
    }
    if (auto status = check_tone_curve(tone_curve); !status) {
        return status;
    }
    for (const DevelopControl control : locks) {
        if (to_string(control) == "invalid") {
            return fail(ErrorCode::InvalidArgument,
                        std::format("unknown locked control {}", static_cast<std::uint32_t>(control)), kWhere);
        }
    }
    if (auto status = check_chroma_denoise(chroma_denoise); !status) {
        return status;
    }
    if (auto status = check_local_adjustments(local_adjustments); !status) {
        return status;
    }
    return check_geometry(geometry);
}

bool DevelopSettings::is_as_shot() const noexcept {
    const DevelopSettings defaults;
    const bool masks = std::any_of(local_adjustments.begin(), local_adjustments.end(),
                                   [](const LocalAdjustment& a) { return a.has_effect(); });
    return white_balance == defaults.white_balance && exposure_ev == 0.0 && contrast == 0.0 && highlights == 0.0 &&
           shadows == 0.0 && tone_curve.is_identity() && saturation == 0.0 && vibrance == 0.0 &&
           base_look == defaults.base_look && chroma_denoise.is_identity() && !masks && geometry.is_identity();
}

Expected<DevelopSettings> upgrade_develop_settings(DevelopSettings settings) {
    if (settings.version == kDevelopSettingsVersion) {
        return settings;
    }
    if (settings.version != kOldestDevelopSettingsVersion) {
        return fail(ErrorCode::VersionMismatch,
                    std::format("{} settings version {} cannot be converted by this build (versions {} to {})",
                                kDevelopSettingsSchema, settings.version, kOldestDevelopSettingsVersion,
                                kDevelopSettingsVersion),
                    kWhere);
    }
    if (auto status = check_version_1(settings); !status) {
        return Unexpected(std::move(status.error()));
    }
    // Version 1 -> 2: the new members keep their defaults (no denoising, no masks, no geometry) and
    // the operations version 2 added get this build's versions (check_version_1 ensured both).
    settings.version = kDevelopSettingsVersion;
    return settings;
}

}  // namespace lucent::engine
