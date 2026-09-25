#include "lucent/image/color_encoding.hpp"

#include <cmath>
#include <format>

namespace lucent::image {

namespace {

constexpr const char* kWhere = "image.color_encoding.validate";

template <class Enum>
bool in_range(Enum value, int first, int last) noexcept {
    const auto raw = static_cast<int>(value);
    return raw >= first && raw <= last;
}

bool white_is_meaningful(const ColorEncoding& e) noexcept {
    return e.primaries != ColorPrimaries::CameraNative;
}

bool custom_white_is_used(const ColorEncoding& e) noexcept {
    return white_is_meaningful(e) && e.white_point == WhitePoint::Custom;
}

}  // namespace

Status ColorEncoding::validate() const {
    if (!in_range(layout, 1, 4)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("unknown channel layout {}", static_cast<int>(layout)), kWhere);
    }
    if (!in_range(primaries, 1, 7)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("unknown colour primaries {}", static_cast<int>(primaries)), kWhere);
    }
    if (!in_range(white_point, 1, 3)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("unknown white point {}", static_cast<int>(white_point)), kWhere);
    }
    if (!in_range(transfer, 1, 5)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("unknown transfer function {}", static_cast<int>(transfer)), kWhere);
    }
    if (!in_range(range, 1, 2)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("unknown numeric range {}", static_cast<int>(range)), kWhere);
    }
    if (!in_range(reference, 1, 2)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("unknown image reference {}", static_cast<int>(reference)), kWhere);
    }
    if (custom_white_is_used(*this)) {
        const Chromaticity& w = custom_white;
        if (!std::isfinite(w.x) || !std::isfinite(w.y) || w.x <= 0.0 || w.y <= 0.0 || w.x + w.y >= 1.0) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("custom white chromaticity ({}, {}) is not a physical colour", w.x, w.y),
                        kWhere);
        }
    }
    if (transfer == TransferFunction::Gamma && (!std::isfinite(gamma) || gamma <= 0.0 || gamma > 10.0)) {
        return fail(ErrorCode::InvalidArgument, std::format("gamma exponent {} is outside (0, 10]", gamma),
                    kWhere);
    }
    if (layout == ChannelLayout::CfaMosaic &&
        (primaries != ColorPrimaries::CameraNative || transfer != TransferFunction::Linear ||
         reference != ImageReference::Scene)) {
        return fail(ErrorCode::InvalidArgument,
                    "a CFA mosaic must be camera-native, linear and scene-referred sensor data", kWhere);
    }
    return ok();
}

std::string ColorEncoding::to_string() const {
    std::string text = std::format("{} {}", image::to_string(layout), image::to_string(primaries));
    if (white_is_meaningful(*this)) {
        if (white_point == WhitePoint::Custom) {
            text += std::format(" custom({}, {})", custom_white.x, custom_white.y);
        } else {
            text += std::format(" {}", image::to_string(white_point));
        }
    }
    if (transfer == TransferFunction::Gamma) {
        text += std::format(" gamma({})", gamma);
    } else {
        text += std::format(" {}", image::to_string(transfer));
    }
    text += std::format(" {} {}", image::to_string(range), image::to_string(reference));
    return text;
}

bool operator==(const ColorEncoding& a, const ColorEncoding& b) noexcept {
    if (a.layout != b.layout || a.primaries != b.primaries || a.transfer != b.transfer ||
        a.range != b.range || a.reference != b.reference) {
        return false;
    }
    if (white_is_meaningful(a)) {
        if (a.white_point != b.white_point) {
            return false;
        }
        if (a.white_point == WhitePoint::Custom && a.custom_white != b.custom_white) {
            return false;
        }
    }
    return a.transfer != TransferFunction::Gamma || a.gamma == b.gamma;
}

std::string_view to_string(ColorPrimaries primaries) noexcept {
    switch (primaries) {
        case ColorPrimaries::CameraNative:
            return "camera-native";
        case ColorPrimaries::CieXyz:
            return "cie-xyz";
        case ColorPrimaries::Rec709Srgb:
            return "rec709-srgb";
        case ColorPrimaries::Rec2020:
            return "rec2020";
        case ColorPrimaries::DisplayP3:
            return "display-p3";
        case ColorPrimaries::ProPhotoRomm:
            return "prophoto-romm";
        case ColorPrimaries::AdobeRgb1998:
            return "adobe-rgb-1998";
    }
    return "invalid";
}

std::string_view to_string(WhitePoint white_point) noexcept {
    switch (white_point) {
        case WhitePoint::D50:
            return "d50";
        case WhitePoint::D65:
            return "d65";
        case WhitePoint::Custom:
            return "custom";
    }
    return "invalid";
}

std::string_view to_string(TransferFunction transfer) noexcept {
    switch (transfer) {
        case TransferFunction::Linear:
            return "linear";
        case TransferFunction::Srgb:
            return "srgb";
        case TransferFunction::Gamma:
            return "gamma";
        case TransferFunction::ProPhotoRomm:
            return "prophoto-romm";
        case TransferFunction::Rec709:
            return "rec709";
    }
    return "invalid";
}

std::string_view to_string(NumericRange range) noexcept {
    switch (range) {
        case NumericRange::Unbounded:
            return "unbounded";
        case NumericRange::UnitClamped:
            return "unit-clamped";
    }
    return "invalid";
}

std::string_view to_string(ImageReference reference) noexcept {
    switch (reference) {
        case ImageReference::Scene:
            return "scene";
        case ImageReference::Display:
            return "display";
    }
    return "invalid";
}

std::string_view to_string(ChannelLayout layout) noexcept {
    switch (layout) {
        case ChannelLayout::Mono:
            return "mono";
        case ChannelLayout::Rgb:
            return "rgb";
        case ChannelLayout::Rgba:
            return "rgba";
        case ChannelLayout::CfaMosaic:
            return "cfa-mosaic";
    }
    return "invalid";
}

}  // namespace lucent::image
