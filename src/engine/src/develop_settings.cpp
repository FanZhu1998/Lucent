// DevelopSettings v1 validation and helpers (develop_settings.hpp).

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

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

Status DevelopSettings::validate() const {
    if (version != kDevelopSettingsVersion) {
        return fail(ErrorCode::VersionMismatch,
                    std::format("{} settings version {} cannot be read by this build (version {})",
                                kDevelopSettingsSchema, version, kDevelopSettingsVersion),
                    kWhere);
    }
    const std::array<std::pair<std::uint32_t, std::pair<std::uint32_t, std::string_view>>, 6> versions = {{
        {operations.white_balance, {kWhiteBalanceOperationVersion, "white-balance"}},
        {operations.exposure, {kExposureOperationVersion, "exposure"}},
        {operations.local_tone, {kLocalToneOperationVersion, "local tone (highlights/shadows)"}},
        {operations.contrast, {kContrastOperationVersion, "contrast"}},
        {operations.tone_curve, {kToneCurveOperationVersion, "tone-curve"}},
        {operations.color, {kColorOperationVersion, "colour (saturation/vibrance)"}},
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
    return ok();
}

bool DevelopSettings::is_as_shot() const noexcept {
    const DevelopSettings defaults;
    return white_balance == defaults.white_balance && exposure_ev == 0.0 && contrast == 0.0 && highlights == 0.0 &&
           shadows == 0.0 && tone_curve.is_identity() && saturation == 0.0 && vibrance == 0.0 &&
           base_look == defaults.base_look;
}

}  // namespace lucent::engine
