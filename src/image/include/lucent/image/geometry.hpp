#pragma once

// Pixel-grid geometry and image orientation.
//
// Coordinate conventions (used throughout lucent::image):
// - x grows to the right (columns) and y grows downwards (rows); (0, 0) is the top-left pixel of
//   the frame the coordinates refer to.
// - Integer coordinates (Point, PixelRect) index whole pixels.
// - Continuous coordinates (PointD) place pixel (i, j) over the unit square [i, i+1) x [j, j+1),
//   so its centre is (i + 0.5, j + 0.5). Masks and other resolution-independent geometry use
//   this convention.
// - "Stored" means the order in which the decoder delivers pixels (sensor order); "oriented"
//   means the upright image the photographer sees after applying the EXIF orientation.
//
// All types here are plain values: cheap to copy and safe to use from any thread.

#include <cstdint>
#include <string_view>

#include <lucent/core/error.hpp>

namespace lucent::image {

// Integer pixel coordinate.
struct Point {
    std::int32_t x = 0;
    std::int32_t y = 0;

    friend constexpr bool operator==(const Point&, const Point&) = default;
};

// Continuous (sub-pixel) coordinate; see the file comment for the pixel-square convention.
struct PointD {
    double x = 0.0;
    double y = 0.0;

    friend constexpr bool operator==(const PointD&, const PointD&) = default;
};

// Image dimensions in pixels. A size with a non-positive width or height is empty.
struct Size {
    std::int32_t width = 0;
    std::int32_t height = 0;

    [[nodiscard]] constexpr bool empty() const noexcept { return width <= 0 || height <= 0; }

    // Number of pixels, computed in 64 bits; 0 for an empty size.
    [[nodiscard]] constexpr std::int64_t area() const noexcept {
        return empty() ? 0 : std::int64_t{width} * std::int64_t{height};
    }

    friend constexpr bool operator==(const Size&, const Size&) = default;
};

// Axis-aligned rectangle of whole pixels: columns [x, x + width) and rows [y, y + height).
// A rectangle with a non-positive width or height is empty. Edge arithmetic is done in 64 bits,
// so querying a malformed rectangle never overflows; use validate() to reject malformed input.
struct PixelRect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;

    // The rectangle covering a whole image of the given size.
    [[nodiscard]] static constexpr PixelRect from_size(Size size) noexcept {
        return PixelRect{0, 0, size.width, size.height};
    }

    [[nodiscard]] constexpr bool empty() const noexcept { return width <= 0 || height <= 0; }

    // Exclusive right and bottom edges.
    [[nodiscard]] constexpr std::int64_t right() const noexcept { return std::int64_t{x} + width; }
    [[nodiscard]] constexpr std::int64_t bottom() const noexcept { return std::int64_t{y} + height; }

    [[nodiscard]] constexpr Size size() const noexcept { return Size{width, height}; }

    // True when the pixel lies inside this rectangle. An empty rectangle contains no pixel.
    [[nodiscard]] constexpr bool contains(Point p) const noexcept {
        return !empty() && p.x >= x && p.y >= y && p.x < right() && p.y < bottom();
    }

    // True when every pixel of `other` lies inside this rectangle. Returns false when either
    // rectangle is empty, so an empty rectangle is never reported as contained.
    [[nodiscard]] constexpr bool contains(const PixelRect& other) const noexcept {
        return !empty() && !other.empty() && other.x >= x && other.y >= y && other.right() <= right() &&
               other.bottom() <= bottom();
    }

    // The overlapping region, or the default (empty, zero) rectangle when the two do not overlap
    // or either is empty. Never overflows: the result lies within both inputs.
    [[nodiscard]] constexpr PixelRect intersect(const PixelRect& other) const noexcept {
        if (empty() || other.empty()) {
            return PixelRect{};
        }
        const std::int32_t left = x > other.x ? x : other.x;
        const std::int32_t top = y > other.y ? y : other.y;
        const std::int64_t r = right() < other.right() ? right() : other.right();
        const std::int64_t b = bottom() < other.bottom() ? bottom() : other.bottom();
        if (r <= left || b <= top) {
            return PixelRect{};
        }
        return PixelRect{left, top, static_cast<std::int32_t>(r - left), static_cast<std::int32_t>(b - top)};
    }

    // Structural check: width and height are non-negative and right()/bottom() are representable
    // as int32. Negative x/y are allowed (for example halo regions around an image).
    // Fails with InvalidArgument.
    [[nodiscard]] Status validate() const;

    // validate(), plus: the rectangle is non-empty and lies entirely inside
    // [0, bounds.width) x [0, bounds.height). Fails with InvalidArgument.
    [[nodiscard]] Status validate_within(Size bounds) const;

    friend constexpr bool operator==(const PixelRect&, const PixelRect&) = default;
};

// Image orientation. The numeric values ARE the EXIF/TIFF Orientation tag values (TIFF 6.0,
// tag 274; the EXIF Orientation tag) and are persisted, so they must never change.
//
// Each value describes how the stored image must be transformed for upright display. The TIFF
// names in the comments give where stored row 0 / column 0 appear on the upright image.
enum class Orientation : std::uint8_t {
    Normal = 1,          // TopLeft:     no change
    FlipHorizontal = 2,  // TopRight:    mirror left-right
    Rotate180 = 3,       // BottomRight: rotate 180 degrees
    FlipVertical = 4,    // BottomLeft:  mirror top-bottom
    Transpose = 5,       // LeftTop:     mirror about the main diagonal (x <-> y)
    Rotate90Cw = 6,      // RightTop:    rotate 90 degrees clockwise
    Transverse = 7,      // RightBottom: mirror about the anti-diagonal
    Rotate90Ccw = 8,     // LeftBottom:  rotate 90 degrees counter-clockwise
};

// Preconditions shared by the Orientation functions below: an Orientation argument holds one
// of the enumerators above. Values read from files or storage must be converted with from_exif()
// or from_libraw_flip(), never cast. Violations are caught by assert in debug builds; release
// builds treat an invalid value as Orientation::Normal rather than invoking undefined behaviour.

// Parses an EXIF/TIFF Orientation value (1..8). Anything else fails with InvalidArgument.
[[nodiscard]] Expected<Orientation> from_exif(int value);

// The EXIF/TIFF Orientation value (1..8).
[[nodiscard]] constexpr std::uint16_t to_exif(Orientation orientation) noexcept {
    return static_cast<std::uint16_t>(orientation);
}

// Converts LibRaw's libraw_image_sizes_t::flip (inherited from dcraw) to an Orientation.
//
// flip is a 3-bit field applied by LibRaw when it writes the upright image. LibRaw 0.22.2
// src/write/file_write.cpp:22-31 (LibRaw::flip_index) maps an upright pixel (row, col) back to
// the stored pixel as:
//     if (flip & 4) swap(row, col);            // transpose
//     if (flip & 2) row = iheight - 1 - row;   // flip rows (top <-> bottom)
//     if (flip & 1) col = iwidth - 1 - col;    // flip columns (left <-> right)
// so stored -> upright is "flip columns if bit 1, flip rows if bit 2, then transpose if bit 4".
// LibRaw's TIFF parser converts EXIF orientation v with "50132467"[v & 7]
// (src/metadata/tiff.cpp:631), and the documented values are 0 = none, 3 = 180 degrees,
// 5 = 90 degrees counter-clockwise, 6 = 90 degrees clockwise (LibRaw doc/API-datastruct.html,
// libraw_image_sizes_t::flip). Full table (flip -> EXIF):
//     0 -> 1, 1 -> 2, 2 -> 4, 3 -> 3, 4 -> 5, 5 -> 8, 6 -> 6, 7 -> 7.
// LibRaw converts rotation angles to these codes during identify
// (src/metadata/identify.cpp:1291-1305), so after open_file() flip is 0..7; other values fail
// with InvalidArgument.
[[nodiscard]] Expected<Orientation> from_libraw_flip(int flip);

// The LibRaw/dcraw flip code (0..7) equivalent to `orientation`.
[[nodiscard]] int to_libraw_flip(Orientation orientation) noexcept;

// True when the orientation exchanges width and height (EXIF values 5..8).
[[nodiscard]] bool swaps_axes(Orientation orientation) noexcept;

// The orientation that undoes `orientation`: map_to_oriented with inverse(o) applied to the
// oriented image restores the stored image. Rotate90Cw and Rotate90Ccw are each other's inverse;
// all other orientations are their own inverse.
[[nodiscard]] Orientation inverse(Orientation orientation) noexcept;

// The single orientation equivalent to applying `first` and then `second` (for example the
// camera's EXIF orientation followed by a user rotation).
[[nodiscard]] Orientation compose(Orientation first, Orientation second) noexcept;

// Size of the upright image for a stored image of size `stored`.
[[nodiscard]] Size oriented_size(Size stored, Orientation orientation) noexcept;

// Maps a stored pixel index to the upright pixel index. `stored` is the stored image size.
// The mapping is affine, so pixels outside the image (halos) map consistently; the caller must
// keep coordinates within the image extent plus/minus 2^30 so results fit in int32.
[[nodiscard]] Point map_to_oriented(Point stored_pixel, Size stored, Orientation orientation) noexcept;

// Inverse of map_to_oriented: maps an upright pixel index back to the stored pixel index.
// `stored` is still the STORED image size, so both directions take the same arguments.
[[nodiscard]] Point map_from_oriented(Point oriented_pixel, Size stored, Orientation orientation) noexcept;

// Continuous-coordinate versions (see the file comment): mirroring uses the image extent
// (x -> width - x) instead of the last pixel index, so pixel centres map to pixel centres.
[[nodiscard]] PointD map_to_oriented(PointD stored_point, Size stored, Orientation orientation) noexcept;
[[nodiscard]] PointD map_from_oriented(PointD oriented_point, Size stored, Orientation orientation) noexcept;

// Maps a stored rectangle to the upright rectangle covering exactly the same pixels, and back.
// Empty rectangles map to the default (empty) rectangle.
[[nodiscard]] PixelRect map_to_oriented(const PixelRect& stored_rect, Size stored,
                                        Orientation orientation) noexcept;
[[nodiscard]] PixelRect map_from_oriented(const PixelRect& oriented_rect, Size stored,
                                          Orientation orientation) noexcept;

// Stable lowercase identifier for logs, e.g. "rotate-90-cw".
[[nodiscard]] std::string_view to_string(Orientation orientation) noexcept;

}  // namespace lucent::image
