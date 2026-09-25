#pragma once

// Colour-encoding contract carried by every image edge (blueprint section 5, "Color and model
// contracts"): channel layout, primaries, white point, transfer function, numeric range and
// scene/display reference. Pixel values are meaningless without it, so it is data attached to
// the image, never inferred from context. Orientation is carried separately (geometry.hpp).
//
// All enum values are persisted (recipes, catalog, worker protocol) and must never change;
// append new values only. Readers that meet an unknown value must reject it (validate()), never
// guess.
//
// ColorEncoding is a small value type, safe to copy and to share between threads for reading.

#include <cstdint>
#include <string>
#include <string_view>

#include <lucent/core/error.hpp>

namespace lucent::image {

// CIE 1931 xy chromaticity coordinates.
struct Chromaticity {
    double x = 0.0;
    double y = 0.0;

    friend constexpr bool operator==(const Chromaticity&, const Chromaticity&) = default;
};

// Colour primaries of the RGB channels, or the XYZ / camera-native interpretation.
enum class ColorPrimaries : std::uint8_t {
    CameraNative = 1,  // camera RGB; colorimetry known only through a camera profile
    CieXyz = 2,        // CIE 1931 XYZ, relative to the encoding's white point (adopted white)
    Rec709Srgb = 3,    // ITU-R BT.709 / IEC 61966-2-1 sRGB primaries
    Rec2020 = 4,       // ITU-R BT.2020 primaries
    DisplayP3 = 5,     // SMPTE RP 431-2 (DCI-P3) primaries, as used by Display P3 with D65
    ProPhotoRomm = 6,  // ISO 22028-2 ROMM RGB primaries (natively D50)
    AdobeRgb1998 = 7,  // Adobe RGB (1998) primaries
};

// White point (adopted white) of the encoding.
enum class WhitePoint : std::uint8_t {
    D50 = 1,
    D65 = 2,
    Custom = 3,  // ColorEncoding::custom_white holds the chromaticity
};

// Transfer function mapping linear light to the stored (encoded) values. "Encode" is
// linear -> stored, "decode" is stored -> linear; see color_math.hpp for the exact curves.
enum class TransferFunction : std::uint8_t {
    Linear = 1,
    Srgb = 2,          // IEC 61966-2-1 piecewise curve
    Gamma = 3,         // pure power law: linear = stored ^ ColorEncoding::gamma
    ProPhotoRomm = 4,  // ISO 22028-2 ROMM curve (1.8 with a linear toe below 1/512)
    Rec709 = 5,        // ITU-R BT.709 OETF (camera encoding); decode is its exact inverse
};

// Whether stored values are confined to [0, 1].
enum class NumericRange : std::uint8_t {
    Unbounded = 1,    // any finite value; scene-linear data may be negative or above 1.0
    UnitClamped = 2,  // values lie in [0, 1] (display/output encodings)
};

// Whether values describe scene light or a rendered display image.
enum class ImageReference : std::uint8_t {
    Scene = 1,
    Display = 2,
};

// How samples are arranged into channels.
enum class ChannelLayout : std::uint8_t {
    Mono = 1,       // one channel
    Rgb = 2,        // three interleaved channels in R, G, B order
    Rgba = 3,       // R, G, B plus straight (non-premultiplied) linear coverage alpha in [0, 1]
    CfaMosaic = 4,  // one sample per site, colour given by a CfaPattern (see cfa.hpp)
};

struct ColorEncoding {
    ChannelLayout layout = ChannelLayout::Rgb;
    ColorPrimaries primaries = ColorPrimaries::Rec2020;

    // Adopted white. Not meaningful for CameraNative primaries (the camera profile defines the
    // white), where it is ignored by comparison, validation and to_string().
    WhitePoint white_point = WhitePoint::D65;
    // Used only when white_point == Custom; ignored (and not compared) otherwise.
    Chromaticity custom_white{};

    TransferFunction transfer = TransferFunction::Linear;
    // Exponent for TransferFunction::Gamma (decode: linear = stored ^ gamma, e.g. 2.2, or
    // 563/256 for Adobe RGB (1998)); ignored (and not compared) for other transfers.
    double gamma = 1.0;

    NumericRange range = NumericRange::Unbounded;
    ImageReference reference = ImageReference::Scene;

    // Lucent's common working space (blueprint section 5): scene-linear Rec.2020 / D65, RGB,
    // unbounded float values.
    [[nodiscard]] static constexpr ColorEncoding scene_linear_rec2020_d65() noexcept {
        return ColorEncoding{};
    }

    // Scene-linear RGB with sRGB/BT.709 primaries and D65 white, unbounded.
    [[nodiscard]] static constexpr ColorEncoding linear_srgb() noexcept {
        ColorEncoding e;
        e.primaries = ColorPrimaries::Rec709Srgb;
        return e;
    }

    // Display-referred sRGB (IEC 61966-2-1): sRGB primaries, D65, sRGB curve, values in [0, 1].
    [[nodiscard]] static constexpr ColorEncoding srgb_display() noexcept {
        ColorEncoding e;
        e.primaries = ColorPrimaries::Rec709Srgb;
        e.transfer = TransferFunction::Srgb;
        e.range = NumericRange::UnitClamped;
        e.reference = ImageReference::Display;
        return e;
    }

    // Demosaiced camera RGB: camera-native primaries, linear, unbounded, scene-referred.
    [[nodiscard]] static constexpr ColorEncoding camera_native_linear() noexcept {
        ColorEncoding e;
        e.primaries = ColorPrimaries::CameraNative;
        return e;
    }

    // Undemosaiced sensor data: a CFA mosaic in camera-native, linear, scene-referred values.
    [[nodiscard]] static constexpr ColorEncoding camera_native_mosaic() noexcept {
        ColorEncoding e = camera_native_linear();
        e.layout = ChannelLayout::CfaMosaic;
        return e;
    }

    // Checks the contract. InvalidArgument when:
    // - any enum holds a value not listed above (for example read from a newer file);
    // - white_point == Custom and custom_white is not finite with x > 0, y > 0, x + y < 1;
    // - transfer == Gamma and gamma is not finite or not in (0, 10];
    // - layout == CfaMosaic but the encoding is not camera-native, linear and scene-referred
    //   (a mosaic is undeveloped sensor data by definition).
    [[nodiscard]] Status validate() const;

    // Diagnostic text, e.g. "rgb rec2020 d65 linear unbounded scene" or
    // "rgb adobe-rgb-1998 d65 gamma(2.19921875) unit-clamped display". Not a serialisation
    // format; persist the enum values instead.
    [[nodiscard]] std::string to_string() const;

    // Semantic equality: fields that the documentation above says are ignored are not compared.
    friend bool operator==(const ColorEncoding& a, const ColorEncoding& b) noexcept;
};

// Stable lowercase identifiers for logs and diagnostics.
[[nodiscard]] std::string_view to_string(ColorPrimaries primaries) noexcept;
[[nodiscard]] std::string_view to_string(WhitePoint white_point) noexcept;
[[nodiscard]] std::string_view to_string(TransferFunction transfer) noexcept;
[[nodiscard]] std::string_view to_string(NumericRange range) noexcept;
[[nodiscard]] std::string_view to_string(ImageReference reference) noexcept;
[[nodiscard]] std::string_view to_string(ChannelLayout layout) noexcept;

}  // namespace lucent::image
