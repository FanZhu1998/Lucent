#include "lucent/image/geometry.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <format>
#include <limits>

namespace lucent::image {

namespace {

constexpr std::int64_t kInt32Max = std::numeric_limits<std::int32_t>::max();

// Every orientation decomposed as "mirror columns (flip_x), mirror rows (flip_y), then
// transpose", applied to the stored image. The three flags are exactly LibRaw's flip bits
// (bit 0 = flip_x, bit 1 = flip_y, bit 2 = transpose); see from_libraw_flip in the header.
struct Decomposition {
    bool flip_x = false;
    bool flip_y = false;
    bool transpose = false;
};

constexpr int flip_code(Decomposition d) noexcept {
    return (d.transpose ? 4 : 0) | (d.flip_y ? 2 : 0) | (d.flip_x ? 1 : 0);
}

constexpr Decomposition from_flip_code(int code) noexcept {
    return Decomposition{(code & 1) != 0, (code & 2) != 0, (code & 4) != 0};
}

// Index = LibRaw flip code, value = orientation. Mirrors LibRaw's "50132467" EXIF -> flip table.
constexpr Orientation kOrientationByFlip[8] = {
    Orientation::Normal,          // 0
    Orientation::FlipHorizontal,  // 1: flip columns
    Orientation::FlipVertical,    // 2: flip rows
    Orientation::Rotate180,       // 3: flip rows and columns
    Orientation::Transpose,       // 4: transpose
    Orientation::Rotate90Ccw,     // 5: flip columns, then transpose
    Orientation::Rotate90Cw,      // 6: flip rows, then transpose
    Orientation::Transverse,      // 7: flip both, then transpose
};

[[maybe_unused]] bool is_valid(Orientation orientation) noexcept {
    const auto value = static_cast<int>(orientation);
    return value >= 1 && value <= 8;
}

Decomposition decompose(Orientation orientation) noexcept {
    assert(is_valid(orientation) && "Orientation must be converted with from_exif/from_libraw_flip");
    for (int code = 0; code < 8; ++code) {
        if (kOrientationByFlip[code] == orientation) {
            return from_flip_code(code);
        }
    }
    return Decomposition{};  // invalid value: documented release-build fallback (Normal)
}

Orientation recompose(Decomposition d) noexcept {
    return kOrientationByFlip[flip_code(d)];
}

std::int32_t narrow(std::int64_t value) noexcept {
    // Documented precondition: coordinates stay within +/-2^30 of the image, so this is exact.
    assert(value >= std::numeric_limits<std::int32_t>::min() && value <= kInt32Max);
    return static_cast<std::int32_t>(value);
}

}  // namespace

Status PixelRect::validate() const {
    if (width < 0 || height < 0) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("rectangle has a negative size ({}x{})", width, height),
                    "image.rect.validate");
    }
    if (right() > kInt32Max || bottom() > kInt32Max) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("rectangle ({}, {}, {}x{}) extends beyond the 32-bit coordinate range", x, y,
                                width, height),
                    "image.rect.validate");
    }
    return ok();
}

Status PixelRect::validate_within(Size bounds) const {
    if (auto status = validate(); !status) {
        return status;
    }
    if (empty()) {
        return fail(ErrorCode::InvalidArgument, std::format("rectangle is empty ({}x{})", width, height),
                    "image.rect.validate");
    }
    if (x < 0 || y < 0 || right() > bounds.width || bottom() > bounds.height) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("rectangle ({}, {}, {}x{}) is not inside the {}x{} image", x, y, width,
                                height, bounds.width, bounds.height),
                    "image.rect.validate");
    }
    return ok();
}

Expected<Orientation> from_exif(int value) {
    if (value < 1 || value > 8) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("EXIF orientation {} is outside the valid range 1..8", value),
                    "image.orientation.from_exif");
    }
    return static_cast<Orientation>(value);
}

Expected<Orientation> from_libraw_flip(int flip) {
    if (flip < 0 || flip > 7) {
        return fail(ErrorCode::InvalidArgument, std::format("LibRaw flip code {} is outside 0..7", flip),
                    "image.orientation.from_libraw_flip");
    }
    return kOrientationByFlip[flip];
}

int to_libraw_flip(Orientation orientation) noexcept {
    return flip_code(decompose(orientation));
}

bool swaps_axes(Orientation orientation) noexcept {
    return decompose(orientation).transpose;
}

Orientation inverse(Orientation orientation) noexcept {
    const Decomposition d = decompose(orientation);
    if (!d.transpose) {
        return recompose(d);  // pure mirrors are involutions
    }
    // T * F(fx, fy) = F(fy, fx) * T, so (T * F(fx, fy))^-1 = F(fx, fy) * T = T * F(fy, fx).
    return recompose(Decomposition{d.flip_y, d.flip_x, true});
}

Orientation compose(Orientation first, Orientation second) noexcept {
    const Decomposition a = decompose(first);
    const Decomposition b = decompose(second);
    // second * first = T^tb * F(b) * T^ta * F(a). Moving F(b) to the right of T^ta swaps its
    // axes when ta is set, after which the mirrors combine by XOR and the transposes cancel.
    const bool b_fx = a.transpose ? b.flip_y : b.flip_x;
    const bool b_fy = a.transpose ? b.flip_x : b.flip_y;
    return recompose(Decomposition{a.flip_x != b_fx, a.flip_y != b_fy, a.transpose != b.transpose});
}

Size oriented_size(Size stored, Orientation orientation) noexcept {
    return swaps_axes(orientation) ? Size{stored.height, stored.width} : stored;
}

Point map_to_oriented(Point stored_pixel, Size stored, Orientation orientation) noexcept {
    const Decomposition d = decompose(orientation);
    std::int64_t x = stored_pixel.x;
    std::int64_t y = stored_pixel.y;
    if (d.flip_x) {
        x = std::int64_t{stored.width} - 1 - x;
    }
    if (d.flip_y) {
        y = std::int64_t{stored.height} - 1 - y;
    }
    return d.transpose ? Point{narrow(y), narrow(x)} : Point{narrow(x), narrow(y)};
}

Point map_from_oriented(Point oriented_pixel, Size stored, Orientation orientation) noexcept {
    const Decomposition d = decompose(orientation);
    std::int64_t x = d.transpose ? oriented_pixel.y : oriented_pixel.x;
    std::int64_t y = d.transpose ? oriented_pixel.x : oriented_pixel.y;
    if (d.flip_x) {
        x = std::int64_t{stored.width} - 1 - x;
    }
    if (d.flip_y) {
        y = std::int64_t{stored.height} - 1 - y;
    }
    return Point{narrow(x), narrow(y)};
}

PointD map_to_oriented(PointD stored_point, Size stored, Orientation orientation) noexcept {
    const Decomposition d = decompose(orientation);
    const double x = d.flip_x ? stored.width - stored_point.x : stored_point.x;
    const double y = d.flip_y ? stored.height - stored_point.y : stored_point.y;
    return d.transpose ? PointD{y, x} : PointD{x, y};
}

PointD map_from_oriented(PointD oriented_point, Size stored, Orientation orientation) noexcept {
    const Decomposition d = decompose(orientation);
    double x = d.transpose ? oriented_point.y : oriented_point.x;
    double y = d.transpose ? oriented_point.x : oriented_point.y;
    if (d.flip_x) {
        x = stored.width - x;
    }
    if (d.flip_y) {
        y = stored.height - y;
    }
    return PointD{x, y};
}

namespace {

// Maps the rectangle's two opposite continuous corners and takes their bounding box; exact
// because every orientation is an axis-aligned isometry of the pixel grid.
template <class MapFn>
PixelRect map_rect(const PixelRect& rect, MapFn map) noexcept {
    if (rect.empty()) {
        return PixelRect{};
    }
    const PointD a = map(PointD{static_cast<double>(rect.x), static_cast<double>(rect.y)});
    const PointD b = map(PointD{static_cast<double>(rect.right()), static_cast<double>(rect.bottom())});
    // Corner coordinates are integers well inside the exactly-representable range of double.
    const auto left = static_cast<std::int64_t>(std::min(a.x, b.x));
    const auto top = static_cast<std::int64_t>(std::min(a.y, b.y));
    const auto right = static_cast<std::int64_t>(std::max(a.x, b.x));
    const auto bottom = static_cast<std::int64_t>(std::max(a.y, b.y));
    return PixelRect{narrow(left), narrow(top), narrow(right - left), narrow(bottom - top)};
}

}  // namespace

PixelRect map_to_oriented(const PixelRect& stored_rect, Size stored, Orientation orientation) noexcept {
    return map_rect(stored_rect, [&](PointD p) { return map_to_oriented(p, stored, orientation); });
}

PixelRect map_from_oriented(const PixelRect& oriented_rect, Size stored, Orientation orientation) noexcept {
    return map_rect(oriented_rect, [&](PointD p) { return map_from_oriented(p, stored, orientation); });
}

std::string_view to_string(Orientation orientation) noexcept {
    switch (orientation) {
        case Orientation::Normal:
            return "normal";
        case Orientation::FlipHorizontal:
            return "flip-horizontal";
        case Orientation::Rotate180:
            return "rotate-180";
        case Orientation::FlipVertical:
            return "flip-vertical";
        case Orientation::Transpose:
            return "transpose";
        case Orientation::Rotate90Cw:
            return "rotate-90-cw";
        case Orientation::Transverse:
            return "transverse";
        case Orientation::Rotate90Ccw:
            return "rotate-90-ccw";
    }
    return "invalid";
}

}  // namespace lucent::image
