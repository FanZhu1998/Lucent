#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <set>
#include <utility>

#include <gtest/gtest.h>

#include <lucent/image/geometry.hpp>

namespace lucent::image {
namespace {

constexpr std::array<Orientation, 8> kAllOrientations = {
    Orientation::Normal,    Orientation::FlipHorizontal, Orientation::Rotate180,  Orientation::FlipVertical,
    Orientation::Transpose, Orientation::Rotate90Cw,     Orientation::Transverse, Orientation::Rotate90Ccw,
};

// Deliberately non-square so that axis swaps are observable.
constexpr Size kStored{5, 3};

// ---------------------------------------------------------------------------------------------
// Size and PixelRect

TEST(Size, EmptyAndArea) {
    EXPECT_TRUE(Size{}.empty());
    EXPECT_TRUE((Size{0, 5}.empty()));
    EXPECT_TRUE((Size{-3, 5}.empty()));
    EXPECT_FALSE((Size{1, 1}.empty()));
    EXPECT_EQ((Size{-3, 5}.area()), 0);
    // 64-bit area: 65535^2 does not fit in int32.
    EXPECT_EQ((Size{65535, 65535}.area()), std::int64_t{65535} * 65535);
}

TEST(PixelRect, EdgesAndEmptiness) {
    const PixelRect r{2, 3, 4, 5};
    EXPECT_FALSE(r.empty());
    EXPECT_EQ(r.right(), 6);
    EXPECT_EQ(r.bottom(), 8);
    EXPECT_EQ(r.size(), (Size{4, 5}));
    EXPECT_TRUE((PixelRect{2, 3, 0, 5}.empty()));
    EXPECT_TRUE((PixelRect{2, 3, 4, -1}.empty()));
    EXPECT_EQ(PixelRect::from_size(Size{7, 9}), (PixelRect{0, 0, 7, 9}));
}

TEST(PixelRect, ContainsPoint) {
    const PixelRect r{2, 3, 4, 5};  // columns 2..5, rows 3..7
    EXPECT_TRUE(r.contains(Point{2, 3}));
    EXPECT_TRUE(r.contains(Point{5, 7}));
    EXPECT_FALSE(r.contains(Point{6, 7}));  // right edge is exclusive
    EXPECT_FALSE(r.contains(Point{5, 8}));  // bottom edge is exclusive
    EXPECT_FALSE(r.contains(Point{1, 3}));
    EXPECT_FALSE((PixelRect{2, 3, 0, 5}.contains(Point{2, 3})));
}

TEST(PixelRect, ContainsRect) {
    const PixelRect r{0, 0, 10, 10};
    EXPECT_TRUE(r.contains(PixelRect{0, 0, 10, 10}));
    EXPECT_TRUE(r.contains(PixelRect{3, 4, 2, 2}));
    EXPECT_FALSE(r.contains(PixelRect{9, 9, 2, 1}));
    EXPECT_FALSE(r.contains(PixelRect{-1, 0, 2, 2}));
    EXPECT_FALSE(r.contains(PixelRect{3, 3, 0, 0}));  // empty rectangles are never contained
}

TEST(PixelRect, Intersect) {
    const PixelRect a{0, 0, 10, 10};
    EXPECT_EQ(a.intersect(PixelRect{5, 6, 10, 10}), (PixelRect{5, 6, 5, 4}));
    EXPECT_EQ(a.intersect(PixelRect{-5, -5, 7, 8}), (PixelRect{0, 0, 2, 3}));
    EXPECT_EQ(a.intersect(PixelRect{2, 2, 3, 3}), (PixelRect{2, 2, 3, 3}));
    // Touching edges share no pixel.
    EXPECT_EQ(a.intersect(PixelRect{10, 0, 5, 5}), PixelRect{});
    EXPECT_EQ(a.intersect(PixelRect{20, 20, 5, 5}), PixelRect{});
    EXPECT_EQ(a.intersect(PixelRect{2, 2, 0, 3}), PixelRect{});
    EXPECT_TRUE(a.intersect(PixelRect{20, 20, 5, 5}).empty());
}

TEST(PixelRect, IntersectNearCoordinateLimitsDoesNotOverflow) {
    constexpr std::int32_t kMax = std::numeric_limits<std::int32_t>::max();
    const PixelRect a{kMax - 10, kMax - 10, 10, 10};
    const PixelRect b{kMax - 5, kMax - 20, 1000, 1000};  // malformed: right() exceeds int32
    EXPECT_EQ(a.intersect(b), (PixelRect{kMax - 5, kMax - 10, 5, 10}));
}

TEST(PixelRect, ValidateRejectsMalformedRectangles) {
    constexpr std::int32_t kMax = std::numeric_limits<std::int32_t>::max();
    EXPECT_TRUE((PixelRect{-4, -4, 8, 8}.validate()));  // negative origin is allowed
    EXPECT_TRUE((PixelRect{0, 0, 0, 0}.validate()));    // empty but well-formed
    for (const PixelRect& bad : {PixelRect{0, 0, -1, 1}, PixelRect{0, 0, 1, -1}, PixelRect{kMax, 0, 1, 1},
                                 PixelRect{0, kMax - 1, 1, 2}}) {
        const Status status = bad.validate();
        ASSERT_FALSE(status);
        EXPECT_EQ(status.error().code, ErrorCode::InvalidArgument);
        EXPECT_EQ(status.error().where, "image.rect.validate");
    }
}

TEST(PixelRect, ValidateWithin) {
    const Size bounds{8, 6};
    EXPECT_TRUE((PixelRect{0, 0, 8, 6}.validate_within(bounds)));
    EXPECT_TRUE((PixelRect{7, 5, 1, 1}.validate_within(bounds)));
    for (const PixelRect& bad : {PixelRect{0, 0, 9, 6}, PixelRect{-1, 0, 2, 2}, PixelRect{7, 5, 2, 1},
                                 PixelRect{3, 3, 0, 1}, PixelRect{0, 0, -2, 3}}) {
        const Status status = bad.validate_within(bounds);
        ASSERT_FALSE(status) << bad.x << "," << bad.y << " " << bad.width << "x" << bad.height;
        EXPECT_EQ(status.error().code, ErrorCode::InvalidArgument);
    }
}

// ---------------------------------------------------------------------------------------------
// Orientation: persisted values and conversions

TEST(Orientation, EnumValuesAreExifValues) {
    for (int value = 1; value <= 8; ++value) {
        const auto orientation = from_exif(value);
        ASSERT_TRUE(orientation) << value;
        EXPECT_EQ(to_exif(*orientation), value);
        EXPECT_EQ(static_cast<int>(*orientation), value);
    }
    static_assert(static_cast<int>(Orientation::Normal) == 1);
    static_assert(static_cast<int>(Orientation::Rotate90Cw) == 6);
    static_assert(static_cast<int>(Orientation::Rotate90Ccw) == 8);
}

TEST(Orientation, FromExifRejectsInvalidValues) {
    for (int value : {0, 9, -1, 255, 65535}) {
        const auto orientation = from_exif(value);
        ASSERT_FALSE(orientation) << value;
        EXPECT_EQ(orientation.error().code, ErrorCode::InvalidArgument);
    }
}

TEST(Orientation, LibRawFlipCodesMatchLibRawExifTable) {
    // LibRaw 0.22.2 src/metadata/tiff.cpp:631 converts EXIF orientation v to its flip code with
    // "50132467"[v & 7] - '0'. Reproduce that table and check that both paths agree.
    constexpr const char* kLibRawExifToFlip = "50132467";
    for (int exif = 1; exif <= 8; ++exif) {
        const int flip = kLibRawExifToFlip[exif & 7] - '0';
        const auto from_flip = from_libraw_flip(flip);
        ASSERT_TRUE(from_flip) << flip;
        EXPECT_EQ(to_exif(*from_flip), exif) << "flip " << flip;
    }
    // The commonly documented codes (LibRaw doc/API-datastruct.html, sizes.flip).
    EXPECT_EQ(*from_libraw_flip(0), Orientation::Normal);
    EXPECT_EQ(*from_libraw_flip(3), Orientation::Rotate180);
    EXPECT_EQ(*from_libraw_flip(5), Orientation::Rotate90Ccw);
    EXPECT_EQ(*from_libraw_flip(6), Orientation::Rotate90Cw);
}

TEST(Orientation, LibRawFlipRoundTripsForAllEight) {
    for (int flip = 0; flip < 8; ++flip) {
        const auto orientation = from_libraw_flip(flip);
        ASSERT_TRUE(orientation);
        EXPECT_EQ(to_libraw_flip(*orientation), flip);
    }
    for (Orientation o : kAllOrientations) {
        EXPECT_EQ(*from_libraw_flip(to_libraw_flip(o)), o) << to_string(o);
    }
}

TEST(Orientation, FromLibRawFlipRejectsInvalidCodes) {
    for (int flip : {-1, 8, 90, 270}) {
        const auto orientation = from_libraw_flip(flip);
        ASSERT_FALSE(orientation) << flip;
        EXPECT_EQ(orientation.error().code, ErrorCode::InvalidArgument);
    }
}

// ---------------------------------------------------------------------------------------------
// Orientation: pixel mapping

// LibRaw 0.22.2 src/write/file_write.cpp:22-31 (LibRaw::flip_index): index of the stored pixel
// that LibRaw writes at upright position (row, col). iwidth/iheight are the stored dimensions.
int libraw_flip_index(int flip, int row, int col, int iwidth, int iheight) {
    if (flip & 4) {
        std::swap(row, col);
    }
    if (flip & 2) {
        row = iheight - 1 - row;
    }
    if (flip & 1) {
        col = iwidth - 1 - col;
    }
    return row * iwidth + col;
}

TEST(Orientation, MappingMatchesLibRawFlipIndex) {
    for (int flip = 0; flip < 8; ++flip) {
        const Orientation o = *from_libraw_flip(flip);
        const Size upright = oriented_size(kStored, o);
        for (int row = 0; row < upright.height; ++row) {
            for (int col = 0; col < upright.width; ++col) {
                const Point stored = map_from_oriented(Point{col, row}, kStored, o);
                EXPECT_EQ(stored.y * kStored.width + stored.x,
                          libraw_flip_index(flip, row, col, kStored.width, kStored.height))
                    << "flip " << flip << " upright (" << col << ", " << row << ")";
                EXPECT_EQ(map_to_oriented(stored, kStored, o), (Point{col, row}));
            }
        }
    }
}

TEST(Orientation, MappingMatchesTiffDefinition) {
    // TIFF 6.0 Orientation: where stored row 0 / column 0 end up on the upright image. Checked
    // through the upright positions of stored pixel (0, 0) and of its right neighbour (1, 0).
    struct Case {
        Orientation orientation;
        Point origin;      // upright position of stored (0, 0)
        Point right_step;  // upright displacement for stored +x
    };
    const Size w = kStored;           // stored 5 x 3
    const Size t{w.height, w.width};  // transposed 3 x 5
    const Case cases[] = {
        {Orientation::Normal, {0, 0}, {1, 0}},                            // row 0 top, col 0 left
        {Orientation::FlipHorizontal, {w.width - 1, 0}, {-1, 0}},         // row 0 top, col 0 right
        {Orientation::Rotate180, {w.width - 1, w.height - 1}, {-1, 0}},   // row 0 bottom, col 0 right
        {Orientation::FlipVertical, {0, w.height - 1}, {1, 0}},           // row 0 bottom, col 0 left
        {Orientation::Transpose, {0, 0}, {0, 1}},                         // row 0 left, col 0 top
        {Orientation::Rotate90Cw, {t.width - 1, 0}, {0, 1}},              // row 0 right, col 0 top
        {Orientation::Transverse, {t.width - 1, t.height - 1}, {0, -1}},  // row 0 right, col 0 bottom
        {Orientation::Rotate90Ccw, {0, t.height - 1}, {0, -1}},           // row 0 left, col 0 bottom
    };
    for (const Case& c : cases) {
        const Point origin = map_to_oriented(Point{0, 0}, kStored, c.orientation);
        const Point next = map_to_oriented(Point{1, 0}, kStored, c.orientation);
        EXPECT_EQ(origin, c.origin) << to_string(c.orientation);
        EXPECT_EQ((Point{next.x - origin.x, next.y - origin.y}), c.right_step) << to_string(c.orientation);
    }
}

TEST(Orientation, OrientedSize) {
    for (Orientation o : kAllOrientations) {
        const Size expected = swaps_axes(o) ? Size{3, 5} : Size{5, 3};
        EXPECT_EQ(oriented_size(kStored, o), expected) << to_string(o);
        EXPECT_EQ(swaps_axes(o), to_exif(o) >= 5) << to_string(o);
    }
}

TEST(Orientation, PixelMappingIsABijectionAndRoundTrips) {
    for (Orientation o : kAllOrientations) {
        const Size upright = oriented_size(kStored, o);
        std::set<std::pair<int, int>> seen;
        for (int y = 0; y < kStored.height; ++y) {
            for (int x = 0; x < kStored.width; ++x) {
                const Point p{x, y};
                const Point q = map_to_oriented(p, kStored, o);
                EXPECT_TRUE(PixelRect::from_size(upright).contains(q)) << to_string(o);
                EXPECT_EQ(map_from_oriented(q, kStored, o), p) << to_string(o);
                seen.insert({q.x, q.y});
            }
        }
        EXPECT_EQ(static_cast<std::int64_t>(seen.size()), kStored.area()) << to_string(o);
    }
}

TEST(Orientation, ContinuousMappingKeepsPixelCentresAndRoundTrips) {
    for (Orientation o : kAllOrientations) {
        for (int y = 0; y < kStored.height; ++y) {
            for (int x = 0; x < kStored.width; ++x) {
                const Point q = map_to_oriented(Point{x, y}, kStored, o);
                const PointD centre = map_to_oriented(PointD{x + 0.5, y + 0.5}, kStored, o);
                EXPECT_EQ(centre, (PointD{q.x + 0.5, q.y + 0.5})) << to_string(o);
            }
        }
        // Arbitrary sub-pixel points, including outside the image, round-trip exactly (all
        // operations are exact in binary floating point for these values).
        for (const PointD p : {PointD{0.25, 2.75}, PointD{4.5, 0.125}, PointD{-1.5, 7.0}, PointD{5.0, 3.0}}) {
            EXPECT_EQ(map_from_oriented(map_to_oriented(p, kStored, o), kStored, o), p) << to_string(o);
        }
    }
    // The image corners map to the upright image corners.
    const PointD far_corner = map_to_oriented(PointD{5.0, 3.0}, kStored, Orientation::Rotate90Cw);
    EXPECT_EQ(far_corner, (PointD{0.0, 5.0}));
}

TEST(Orientation, InverseUndoesMapping) {
    EXPECT_EQ(inverse(Orientation::Rotate90Cw), Orientation::Rotate90Ccw);
    EXPECT_EQ(inverse(Orientation::Rotate90Ccw), Orientation::Rotate90Cw);
    for (Orientation o : kAllOrientations) {
        const Orientation inv = inverse(o);
        EXPECT_EQ(compose(o, inv), Orientation::Normal) << to_string(o);
        EXPECT_EQ(compose(inv, o), Orientation::Normal) << to_string(o);
        const Size upright = oriented_size(kStored, o);
        for (int y = 0; y < kStored.height; ++y) {
            for (int x = 0; x < kStored.width; ++x) {
                const Point q = map_to_oriented(Point{x, y}, kStored, o);
                EXPECT_EQ(map_to_oriented(q, upright, inv), (Point{x, y})) << to_string(o);
            }
        }
    }
}

TEST(Orientation, ComposeMatchesSequentialMapping) {
    for (Orientation first : kAllOrientations) {
        for (Orientation second : kAllOrientations) {
            const Orientation combined = compose(first, second);
            const Size intermediate = oriented_size(kStored, first);
            EXPECT_EQ(oriented_size(kStored, combined), oriented_size(intermediate, second));
            for (int y = 0; y < kStored.height; ++y) {
                for (int x = 0; x < kStored.width; ++x) {
                    const Point step =
                        map_to_oriented(map_to_oriented(Point{x, y}, kStored, first), intermediate, second);
                    EXPECT_EQ(map_to_oriented(Point{x, y}, kStored, combined), step)
                        << to_string(first) << " then " << to_string(second);
                }
            }
        }
    }
}

TEST(Orientation, ComposeRotations) {
    EXPECT_EQ(compose(Orientation::Rotate90Cw, Orientation::Rotate90Cw), Orientation::Rotate180);
    EXPECT_EQ(compose(Orientation::Rotate90Cw, Orientation::Rotate90Ccw), Orientation::Normal);
    EXPECT_EQ(compose(Orientation::Rotate180, Orientation::Rotate90Cw), Orientation::Rotate90Ccw);
    EXPECT_EQ(compose(Orientation::FlipHorizontal, Orientation::FlipVertical), Orientation::Rotate180);
    Orientation o = Orientation::Normal;
    for (int i = 0; i < 4; ++i) {
        o = compose(o, Orientation::Rotate90Cw);
    }
    EXPECT_EQ(o, Orientation::Normal);
}

TEST(Orientation, RectMappingCoversExactlyTheMappedPixels) {
    const Size stored{7, 4};
    const PixelRect rect{1, 1, 4, 2};
    for (Orientation o : kAllOrientations) {
        const PixelRect mapped = map_to_oriented(rect, stored, o);
        // Bounding box of the individually mapped pixels.
        std::int32_t min_x = std::numeric_limits<std::int32_t>::max();
        std::int32_t min_y = min_x;
        std::int32_t max_x = std::numeric_limits<std::int32_t>::min();
        std::int32_t max_y = max_x;
        for (int y = rect.y; y < rect.bottom(); ++y) {
            for (int x = rect.x; x < rect.right(); ++x) {
                const Point q = map_to_oriented(Point{x, y}, stored, o);
                EXPECT_TRUE(mapped.contains(q)) << to_string(o);
                min_x = std::min(min_x, q.x);
                min_y = std::min(min_y, q.y);
                max_x = std::max(max_x, q.x);
                max_y = std::max(max_y, q.y);
            }
        }
        EXPECT_EQ(mapped, (PixelRect{min_x, min_y, max_x - min_x + 1, max_y - min_y + 1})) << to_string(o);
        EXPECT_EQ(map_from_oriented(mapped, stored, o), rect) << to_string(o);
    }
    EXPECT_EQ(map_to_oriented(PixelRect{2, 2, 0, 3}, stored, Orientation::Rotate90Cw), PixelRect{});
}

TEST(Orientation, ToStringIsStable) {
    EXPECT_EQ(to_string(Orientation::Normal), "normal");
    EXPECT_EQ(to_string(Orientation::Rotate90Cw), "rotate-90-cw");
    EXPECT_EQ(to_string(Orientation::Rotate90Ccw), "rotate-90-ccw");
    EXPECT_EQ(to_string(Orientation::Transverse), "transverse");
}

}  // namespace
}  // namespace lucent::image
