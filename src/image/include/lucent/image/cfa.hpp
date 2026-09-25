#pragma once

// Colour-filter-array (CFA) descriptors.
//
// A CfaPattern is the repeating tile of colour filters over a mosaic sensor, stated relative to
// the origin of a particular pixel frame: color_at(row, col) is the filter over pixel (col, row)
// of that frame, and the tile repeats in both directions (negative coordinates included).
// Whoever stores a pattern must also record which frame it refers to (for example the full raw
// frame or the active/visible area); shifted() converts between frames.
//
// Both green sites of a Bayer tile are kept distinct (Green and Green2), because the two greens
// can differ in black level, gain and crosstalk and must stay separate until the processing
// route decides to combine them (blueprint section 3, "Adapter boundary").
//
// CfaPattern is a small value type (about 260 bytes): copyable, immutable after construction
// apart from assignment, and safe to share between threads for reading.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <lucent/core/error.hpp>

namespace lucent::image {

// Filter colour of one CFA site. Values are persisted and must never change; append only.
enum class CfaColor : std::uint8_t {
    Red = 0,
    Green = 1,  // Bayer: the green site sharing its row with red ("Gr")
    Blue = 2,
    Green2 = 3,  // Bayer: the green site sharing its row with blue ("Gb")
    Cyan = 4,
    Magenta = 5,
    Yellow = 6,
    White = 7,  // unfiltered / panchromatic site
};

// Parses a persisted CfaColor value (0..7); anything else fails with InvalidArgument.
[[nodiscard]] Expected<CfaColor> cfa_color_from_value(int value);

// Stable lowercase identifier, e.g. "green2".
[[nodiscard]] std::string_view to_string(CfaColor color) noexcept;

// Single-letter code used by CfaPattern::to_string: R, G, B, C, M, Y, W. Green and Green2 both
// print as 'G'.
[[nodiscard]] char to_letter(CfaColor color) noexcept;

[[nodiscard]] constexpr bool is_green(CfaColor color) noexcept {
    return color == CfaColor::Green || color == CfaColor::Green2;
}

// The four 2x2 Bayer arrangements, named by their top-left 2x2 tile in row-major order.
// Values are persisted and must never change.
enum class BayerLayout : std::uint8_t {
    Rggb = 1,
    Bggr = 2,
    Grbg = 3,
    Gbrg = 4,
};

// "RGGB", "BGGR", "GRBG" or "GBRG".
[[nodiscard]] std::string_view to_string(BayerLayout layout) noexcept;

class CfaPattern {
public:
    // Largest supported tile in either direction (LibRaw's largest native tile is Leaf's 16x16).
    static constexpr std::int32_t kMaxPeriod = 16;

    // Empty pattern (width() == height() == 0), used for "no CFA" placeholders. Only empty(),
    // width(), height(), comparison and to_string() may be used on it.
    constexpr CfaPattern() noexcept = default;

    // Builds a pattern from a row-major tile of width x height colours (1..kMaxPeriod each).
    // The tile is reduced to its smallest period along each axis, so for example a 4x4 tile of
    // repeated RGGB yields the 2x2 RGGB pattern and equal patterns always compare equal.
    // InvalidArgument: bad dimensions, colours.size() != width * height, or an undefined colour.
    [[nodiscard]] static Expected<CfaPattern> create(std::int32_t width, std::int32_t height,
                                                     std::span<const CfaColor> colors);

    // A 2x2 Bayer pattern with the two greens distinguished following the LibRaw/dcraw
    // convention: Green shares its row with Red and Green2 shares its row with Blue. For
    // example bayer(BayerLayout::Rggb) is  R  G  / G2 B, and bayer(BayerLayout::Gbrg) is
    // G2 B / R G. This is the assignment LibRaw makes when it marks the second green as colour
    // index 3 (see from_libraw_filters), so LibRaw-decoded Bayer data maps onto it one-to-one.
    [[nodiscard]] static CfaPattern bayer(BayerLayout layout) noexcept;

    // Converts LibRaw's libraw_iparams_t {filters, colors, cdesc} to a pattern whose frame is
    // LibRaw's visible area: LibRaw evaluates colours at visible-area coordinates and reads
    // raw_image at (row + top_margin, col + left_margin)
    // (LibRaw 0.22.2 src/preprocessing/raw2image.cpp:284-289). For the raw frame, use
    // shifted(-left_margin, -top_margin).
    //
    // For filters >= 1000 the colour index of (row, col) is LibRaw's FC():
    //     (filters >> ((((row) << 1 & 14) | ((col) & 1)) << 1) & 3)
    // (installed header libraw/libraw.h:312-315, LibRaw::FC), i.e. an 8-row x 2-column tile of
    // 2-bit indices; the tile is then reduced to its smallest period. Each index i maps to
    // cdesc[i]: 'R' Red, 'G' Green, 'B' Blue, 'C' Cyan, 'M' Magenta, 'Y' Yellow, 'W' White. A
    // letter that repeats an earlier letter of cdesc denotes the second site of that colour;
    // for 'G' that is Green2. This covers LibRaw's standard case: for 3-colour cameras LibRaw
    // sets cdesc = "RGBG" and rewrites filters so that the green sharing a row with blue gets
    // index 3 (src/metadata/identify.cpp:1277-1285:
    //     filters |= ((filters >> 2 & 0x22222222) | (filters << 2 & 0x88888888)) & filters << 1;
    // ), so index 3 is Green2 exactly as in bayer().
    //
    // Errors:
    // - InvalidArgument: colors outside 1..4; filters == 0 (not a mosaic: full-colour or
    //   monochrome data); filters == 9 (X-Trans: use from_xtrans with imgdata.idata.xtrans);
    //   cdesc shorter than the largest index used.
    // - Unsupported: filters == 1 (Leaf CatchLight 16x16 table, which LibRaw applies in raw-frame
    //   coordinates internally), other reserved values below 1000, a cdesc letter without a
    //   CfaColor (e.g. 'E' emerald, 'T'), or a repeated letter other than 'G'.
    [[nodiscard]] static Expected<CfaPattern> from_libraw_filters(std::uint32_t filters, int colors,
                                                                  std::string_view cdesc);

    // Converts a Fujifilm X-Trans 6x6 tile as stored by LibRaw in libraw_iparams_t::xtrans
    // (relative to the visible area) or xtrans_abs (relative to the sensor edges), indexed
    // [row][col] with values 0 = Red, 1 = Green, 2 = Blue. X-Trans greens are all Green (LibRaw
    // does not distinguish them). The resulting pattern's frame is the frame of the array used.
    // InvalidArgument when a value is outside 0..2.
    [[nodiscard]] static Expected<CfaPattern> from_xtrans(const char (&xtrans)[6][6]);

    [[nodiscard]] constexpr std::int32_t width() const noexcept { return width_; }
    [[nodiscard]] constexpr std::int32_t height() const noexcept { return height_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return width_ == 0; }

    // Colour of the site at (row, col) for any coordinates, negative included (the tile repeats
    // indefinitely). Precondition: !empty() (asserted; release builds return Red rather than
    // invoking undefined behaviour).
    [[nodiscard]] CfaColor color_at(std::int32_t row, std::int32_t col) const noexcept;

    // The same physical pattern expressed in a frame whose origin lies at column dx, row dy of
    // this pattern's frame: result.color_at(r, c) == color_at(r + dy, c + dx). For example, a
    // crop starting at (left, top) uses shifted(left, top). Any integers are allowed.
    [[nodiscard]] CfaPattern shifted(std::int32_t dx, std::int32_t dy) const noexcept;

    // True for a 2x2 tile with one Red, one Blue on the other diagonal, and two green sites
    // (Green or Green2 in any combination).
    [[nodiscard]] bool is_bayer() const noexcept;

    // The Bayer arrangement when is_bayer(), otherwise std::nullopt.
    [[nodiscard]] std::optional<BayerLayout> bayer_layout() const noexcept;

    // True for a 6x6 tile with the X-Trans structure: only red, green and blue sites in the
    // proportion 8 : 20 : 8, and every row and every column containing all three colours. This
    // accepts all shifts of the Fujifilm tile and rejects Bayer and other smaller-period tiles.
    [[nodiscard]] bool is_xtrans() const noexcept;

    // Diagnostic text, not a serialisation format: a 2x2 tile is written as four letters in
    // row-major order ("RGGB"); other sizes as rows separated by '/' ("GGRGGB/GGBGGR/...").
    // Green and Green2 both print as 'G'. An empty pattern prints as "".
    [[nodiscard]] std::string to_string() const;

    friend bool operator==(const CfaPattern& a, const CfaPattern& b) noexcept;

private:
    static constexpr std::size_t kCapacity = static_cast<std::size_t>(kMaxPeriod) * kMaxPeriod;

    [[nodiscard]] CfaColor stored(std::int32_t row, std::int32_t col) const noexcept {
        return colors_[static_cast<std::size_t>(row) * static_cast<std::size_t>(width_) +
                       static_cast<std::size_t>(col)];
    }
    void reduce_to_minimal_period() noexcept;

    std::array<CfaColor, kCapacity> colors_{};  // row-major, width_ * height_ entries used
    std::uint8_t width_ = 0;
    std::uint8_t height_ = 0;
};

}  // namespace lucent::image
