#include <array>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/image/cfa.hpp>

namespace lucent::image {
namespace {

using enum CfaColor;

constexpr std::array<BayerLayout, 4> kAllBayerLayouts = {BayerLayout::Rggb, BayerLayout::Bggr,
                                                         BayerLayout::Grbg, BayerLayout::Gbrg};

// LibRaw's FC() (libraw/libraw.h:312-315), reproduced independently of the implementation.
unsigned libraw_fc(std::uint32_t filters, int row, int col) {
    return filters >> ((((row) << 1 & 14) | ((col) & 1)) << 1) & 3;
}

// Encodes a pattern with period dividing 8 x 2 into a LibRaw filters word, using LibRaw's
// colour indices for cdesc "RGBG" (0 R, 1 G, 2 B, 3 second G).
std::uint32_t to_libraw_filters(const CfaPattern& pattern) {
    std::uint32_t filters = 0;
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 2; ++col) {
            const auto index = static_cast<std::uint32_t>(pattern.color_at(row, col));  // R0 G1 B2 G2=3
            filters |= index << ((((row) << 1 & 14) | ((col) & 1)) << 1);
        }
    }
    return filters;
}

// LibRaw's identify() rewrite of a 3-colour filters word so the green sharing a row with blue
// becomes index 3 (LibRaw 0.22.2 src/metadata/identify.cpp:1283-1285).
std::uint32_t libraw_four_colour(std::uint32_t filters) {
    return filters | (((filters >> 2 & 0x22222222) | (filters << 2 & 0x88888888)) & filters << 1);
}

struct LibRawBayerConstants {
    BayerLayout layout;
    std::uint32_t three_colour;  // as parsed from the file (greens both index 1)
    std::uint32_t four_colour;   // after identify(): Green2 = index 3
};

// The three-colour words are dcraw/LibRaw's well-known Bayer constants.
constexpr LibRawBayerConstants kLibRawBayer[] = {
    {BayerLayout::Rggb, 0x94949494u, 0xB4B4B4B4u},
    {BayerLayout::Bggr, 0x16161616u, 0x1E1E1E1Eu},
    {BayerLayout::Grbg, 0x61616161u, 0xE1E1E1E1u},
    {BayerLayout::Gbrg, 0x49494949u, 0x4B4B4B4Bu},
};

// Fujifilm X-Trans tile as LibRaw stores it in imgdata.idata.xtrans (0 R, 1 G, 2 B).
constexpr char kXTrans[6][6] = {
    {1, 1, 0, 1, 1, 2},  //
    {1, 1, 2, 1, 1, 0},  //
    {2, 0, 1, 0, 2, 1},  //
    {1, 1, 2, 1, 1, 0},  //
    {1, 1, 0, 1, 1, 2},  //
    {0, 2, 1, 2, 0, 1},  //
};

TEST(CfaColor, PersistedValuesAndNames) {
    static_assert(static_cast<int>(Red) == 0 && static_cast<int>(Green) == 1 && static_cast<int>(Blue) == 2 &&
                  static_cast<int>(Green2) == 3 && static_cast<int>(Cyan) == 4 &&
                  static_cast<int>(Magenta) == 5 && static_cast<int>(Yellow) == 6 &&
                  static_cast<int>(White) == 7);
    for (int value = 0; value <= 7; ++value) {
        auto color = cfa_color_from_value(value);
        ASSERT_TRUE(color);
        EXPECT_EQ(static_cast<int>(*color), value);
    }
    EXPECT_FALSE(cfa_color_from_value(8));
    EXPECT_FALSE(cfa_color_from_value(-1));
    EXPECT_EQ(to_string(Green2), "green2");
    EXPECT_EQ(to_letter(Green2), 'G');
    EXPECT_TRUE(is_green(Green2));
    EXPECT_FALSE(is_green(Cyan));
}

TEST(CfaPattern, BayerLayoutsPlaceGreenBesideRedAndGreen2BesideBlue) {
    struct Expected2x2 {
        BayerLayout layout;
        std::array<CfaColor, 4> tile;  // row-major
        const char* text;
    };
    const Expected2x2 cases[] = {
        {BayerLayout::Rggb, {Red, Green, Green2, Blue}, "RGGB"},
        {BayerLayout::Bggr, {Blue, Green2, Green, Red}, "BGGR"},
        {BayerLayout::Grbg, {Green, Red, Blue, Green2}, "GRBG"},
        {BayerLayout::Gbrg, {Green2, Blue, Red, Green}, "GBRG"},
    };
    for (const auto& c : cases) {
        const CfaPattern p = CfaPattern::bayer(c.layout);
        EXPECT_EQ(p.width(), 2);
        EXPECT_EQ(p.height(), 2);
        EXPECT_EQ(p.color_at(0, 0), c.tile[0]);
        EXPECT_EQ(p.color_at(0, 1), c.tile[1]);
        EXPECT_EQ(p.color_at(1, 0), c.tile[2]);
        EXPECT_EQ(p.color_at(1, 1), c.tile[3]);
        EXPECT_TRUE(p.is_bayer());
        EXPECT_FALSE(p.is_xtrans());
        EXPECT_EQ(p.bayer_layout(), c.layout);
        EXPECT_EQ(p.to_string(), c.text);
        EXPECT_EQ(to_string(c.layout), c.text);
        // The documented green convention, checked structurally.
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 2; ++col) {
                const CfaColor other = p.color_at(row, col + 1);
                if (p.color_at(row, col) == Green) {
                    EXPECT_EQ(other, Red);
                }
                if (p.color_at(row, col) == Green2) {
                    EXPECT_EQ(other, Blue);
                }
            }
        }
    }
}

TEST(CfaPattern, ColorAtRepeatsForAnyCoordinates) {
    const CfaPattern p = CfaPattern::bayer(BayerLayout::Rggb);
    for (int row = -7; row <= 7; ++row) {
        for (int col = -7; col <= 7; ++col) {
            EXPECT_EQ(p.color_at(row, col), p.color_at(row & 1, col & 1)) << row << "," << col;
        }
    }
    constexpr std::int32_t kMin = std::numeric_limits<std::int32_t>::min();
    constexpr std::int32_t kMax = std::numeric_limits<std::int32_t>::max();
    EXPECT_EQ(p.color_at(kMin, kMin), Red);   // both even
    EXPECT_EQ(p.color_at(kMax, kMax), Blue);  // both odd
    EXPECT_EQ(p.color_at(kMin, kMax), Green);
}

TEST(CfaPattern, ShiftsBetweenBayerLayouts) {
    const CfaPattern rggb = CfaPattern::bayer(BayerLayout::Rggb);
    EXPECT_EQ(rggb.shifted(1, 0), CfaPattern::bayer(BayerLayout::Grbg));
    EXPECT_EQ(rggb.shifted(0, 1), CfaPattern::bayer(BayerLayout::Gbrg));
    EXPECT_EQ(rggb.shifted(1, 1), CfaPattern::bayer(BayerLayout::Bggr));
    // Even shifts are no-ops; odd shifts in either direction are equivalent.
    EXPECT_EQ(rggb.shifted(2, -4), rggb);
    EXPECT_EQ(rggb.shifted(-1, 0), rggb.shifted(1, 0));
    EXPECT_EQ(rggb.shifted(3, 5), rggb.shifted(1, 1));
    EXPECT_EQ(rggb.shifted(std::numeric_limits<std::int32_t>::min(), 1), rggb.shifted(0, 1));
    // Shifting every layout reproduces the defining relation for all coordinates.
    for (BayerLayout layout : kAllBayerLayouts) {
        const CfaPattern p = CfaPattern::bayer(layout);
        for (int dy = -3; dy <= 3; ++dy) {
            for (int dx = -3; dx <= 3; ++dx) {
                const CfaPattern s = p.shifted(dx, dy);
                EXPECT_TRUE(s.is_bayer());
                for (int row = 0; row < 4; ++row) {
                    for (int col = 0; col < 4; ++col) {
                        EXPECT_EQ(s.color_at(row, col), p.color_at(row + dy, col + dx));
                    }
                }
            }
        }
    }
}

TEST(CfaPattern, LibRawBayerFiltersConstantsDerivedThroughFc) {
    for (const auto& k : kLibRawBayer) {
        const CfaPattern bayer = CfaPattern::bayer(k.layout);
        // Encoding our pattern with LibRaw's FC indexing reproduces LibRaw's 4-colour word...
        EXPECT_EQ(to_libraw_filters(bayer), k.four_colour) << to_string(k.layout);
        // ...which is exactly what LibRaw's identify() makes of the classic 3-colour word.
        EXPECT_EQ(libraw_four_colour(k.three_colour), k.four_colour) << to_string(k.layout);
        // FC over a region larger than the tile agrees with color_at.
        for (int row = 0; row < 16; ++row) {
            for (int col = 0; col < 4; ++col) {
                EXPECT_EQ(libraw_fc(k.four_colour, row, col),
                          static_cast<unsigned>(bayer.color_at(row, col)));
            }
        }
        // Converting LibRaw's data gives back the same pattern, second green included.
        auto from_four = CfaPattern::from_libraw_filters(k.four_colour, 3, "RGBG");
        ASSERT_TRUE(from_four) << from_four.error().describe();
        EXPECT_EQ(*from_four, bayer) << to_string(k.layout);
        // A 3-colour word (greens undistinguished) is still recognised as the same layout.
        auto from_three = CfaPattern::from_libraw_filters(k.three_colour, 3, "RGBG");
        ASSERT_TRUE(from_three);
        EXPECT_EQ(from_three->bayer_layout(), k.layout);
        EXPECT_NE(*from_three, bayer);  // no Green2 site
    }
}

TEST(CfaPattern, LibRawFourColourCmygSensor) {
    // Rows alternate indices (0 1) and (2 3); with cdesc "GMCY" that is G M / C Y.
    auto p = CfaPattern::from_libraw_filters(0xE4E4E4E4u, 4, "GMCY");
    ASSERT_TRUE(p) << p.error().describe();
    EXPECT_EQ(p->width(), 2);
    EXPECT_EQ(p->height(), 2);
    EXPECT_EQ(p->color_at(0, 0), Green);
    EXPECT_EQ(p->color_at(0, 1), Magenta);
    EXPECT_EQ(p->color_at(1, 0), Cyan);
    EXPECT_EQ(p->color_at(1, 1), Yellow);
    EXPECT_FALSE(p->is_bayer());
}

TEST(CfaPattern, LibRawFiltersWithFourRowPeriod) {
    // Row nibbles (row 0 in the lowest nibble): RG, GB, GR, BG, repeated -> period 2 x 4.
    auto p = CfaPattern::from_libraw_filters(0x61946194u, 3, "RGBG");
    ASSERT_TRUE(p) << p.error().describe();
    EXPECT_EQ(p->width(), 2);
    EXPECT_EQ(p->height(), 4);
    EXPECT_EQ(p->to_string(), "RG/GB/GR/BG");
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 2; ++col) {
            EXPECT_EQ(static_cast<unsigned>(p->color_at(row, col)), libraw_fc(0x61946194u, row, col));
        }
    }
}

TEST(CfaPattern, LibRawFiltersErrors) {
    struct Case {
        std::uint32_t filters;
        int colors;
        const char* cdesc;
        ErrorCode code;
    };
    const Case cases[] = {
        {0u, 3, "RGBG", ErrorCode::InvalidArgument},           // full-colour / monochrome data
        {9u, 3, "RGBG", ErrorCode::InvalidArgument},           // X-Trans: needs the xtrans array
        {1u, 3, "RGBG", ErrorCode::Unsupported},               // Leaf CatchLight
        {500u, 3, "RGBG", ErrorCode::Unsupported},             // reserved
        {0xB4B4B4B4u, 0, "RGBG", ErrorCode::InvalidArgument},  // colour count
        {0xB4B4B4B4u, 5, "RGBG", ErrorCode::InvalidArgument},
        {0xB4B4B4B4u, 3, "RGB", ErrorCode::InvalidArgument},  // no letter for index 3
        {0xB4B4B4B4u, 4, "RGBE", ErrorCode::Unsupported},     // emerald has no CfaColor
        {0xB4B4B4B4u, 4, "RGBR", ErrorCode::Unsupported},     // repeated non-green
        {0xB4B4B4B4u, 4, "GGBG", ErrorCode::Unsupported},     // third green
    };
    for (const Case& c : cases) {
        auto p = CfaPattern::from_libraw_filters(c.filters, c.colors, c.cdesc);
        ASSERT_FALSE(p) << c.filters << " " << c.cdesc;
        EXPECT_EQ(p.error().code, c.code) << c.filters << " " << c.cdesc << ": " << p.error().describe();
        EXPECT_EQ(p.error().where, "image.cfa.from_libraw_filters");
    }
}

TEST(CfaPattern, XTransFromLibRaw) {
    auto p = CfaPattern::from_xtrans(kXTrans);
    ASSERT_TRUE(p) << p.error().describe();
    EXPECT_EQ(p->width(), 6);
    EXPECT_EQ(p->height(), 6);
    EXPECT_TRUE(p->is_xtrans());
    EXPECT_FALSE(p->is_bayer());
    EXPECT_EQ(p->to_string(), "GGRGGB/GGBGGR/BRGRBG/GGBGGR/GGRGGB/RBGBRG");
    for (int row = 0; row < 6; ++row) {
        for (int col = 0; col < 6; ++col) {
            EXPECT_EQ(static_cast<int>(p->color_at(row, col)), kXTrans[row][col]);
        }
    }
}

TEST(CfaPattern, XTransShiftPeriodicityIsSix) {
    const CfaPattern p = *CfaPattern::from_xtrans(kXTrans);
    EXPECT_EQ(p.shifted(6, 0), p);
    EXPECT_EQ(p.shifted(0, 6), p);
    EXPECT_EQ(p.shifted(-6, 12), p);
    EXPECT_EQ(p.shifted(7, -5), p.shifted(1, 1));
    // Besides multiples of 6, the X-Trans tile is invariant only under the diagonal shift (3, 3),
    // which exchanges its two 3x3 halves; no axis-aligned shift below 6 maps it onto itself.
    EXPECT_EQ(p.shifted(3, 3), p);
    for (int dy = 0; dy < 6; ++dy) {
        for (int dx = 0; dx < 6; ++dx) {
            const CfaPattern s = p.shifted(dx, dy);
            EXPECT_TRUE(s.is_xtrans()) << dx << "," << dy;
            const bool symmetry = (dx == 0 && dy == 0) || (dx == 3 && dy == 3);
            EXPECT_EQ(s == p, symmetry) << dx << "," << dy << ": " << s.to_string();
            for (int row = -6; row < 6; ++row) {
                for (int col = -6; col < 6; ++col) {
                    EXPECT_EQ(s.color_at(row, col), p.color_at(row + dy, col + dx));
                }
            }
        }
    }
}

TEST(CfaPattern, XTransRejectsInvalidValues) {
    char bad[6][6] = {};
    bad[2][3] = 3;
    auto p = CfaPattern::from_xtrans(bad);
    ASSERT_FALSE(p);
    EXPECT_EQ(p.error().code, ErrorCode::InvalidArgument);
    bad[2][3] = -1;
    EXPECT_FALSE(CfaPattern::from_xtrans(bad));
}

TEST(CfaPattern, IsXTransRejectsLookalikes) {
    // A Bayer tile repeated to 6x6 reduces to 2x2 and is not X-Trans.
    std::vector<CfaColor> bayer6;
    for (int row = 0; row < 6; ++row) {
        for (int col = 0; col < 6; ++col) {
            bayer6.push_back(CfaPattern::bayer(BayerLayout::Rggb).color_at(row, col));
        }
    }
    auto tiled = CfaPattern::create(6, 6, bayer6);
    ASSERT_TRUE(tiled);
    EXPECT_EQ(*tiled, CfaPattern::bayer(BayerLayout::Rggb));
    EXPECT_FALSE(tiled->is_xtrans());

    // Swapping one red and one green keeps the counts but leaves a row without red.
    char swapped[6][6];
    for (int row = 0; row < 6; ++row) {
        for (int col = 0; col < 6; ++col) {
            swapped[row][col] = kXTrans[row][col];
        }
    }
    swapped[0][2] = 1;  // row 0 loses its only red
    swapped[1][0] = 0;  // row 1 gains a red
    auto broken = CfaPattern::from_xtrans(swapped);
    ASSERT_TRUE(broken);
    EXPECT_FALSE(broken->is_xtrans());
}

TEST(CfaPattern, CreateReducesToMinimalPeriod) {
    std::vector<CfaColor> tile;
    for (int row = 0; row < 16; ++row) {
        for (int col = 0; col < 16; ++col) {
            tile.push_back(CfaPattern::bayer(BayerLayout::Gbrg).color_at(row, col));
        }
    }
    auto big = CfaPattern::create(16, 16, tile);
    ASSERT_TRUE(big);
    EXPECT_EQ(*big, CfaPattern::bayer(BayerLayout::Gbrg));
    EXPECT_TRUE(big->is_bayer());

    // A 3-wide, 2-high tile with no smaller period stays as it is.
    const std::array<CfaColor, 6> odd = {Red, Green, Blue, Green, Blue, Red};
    auto irregular = CfaPattern::create(3, 2, odd);
    ASSERT_TRUE(irregular);
    EXPECT_EQ(irregular->width(), 3);
    EXPECT_EQ(irregular->height(), 2);
    EXPECT_EQ(irregular->to_string(), "RGB/GBR");

    const std::array<CfaColor, 4> uniform = {White, White, White, White};
    auto mono = CfaPattern::create(2, 2, uniform);
    ASSERT_TRUE(mono);
    EXPECT_EQ(mono->width(), 1);
    EXPECT_EQ(mono->height(), 1);
    EXPECT_EQ(mono->to_string(), "W");
}

TEST(CfaPattern, CreateValidatesArguments) {
    const std::array<CfaColor, 4> four = {Red, Green, Green2, Blue};
    for (auto [w, h] :
         {std::pair{0, 2}, std::pair{2, 0}, std::pair{17, 1}, std::pair{1, 17}, std::pair{2, 3}}) {
        auto p = CfaPattern::create(w, h, four);
        ASSERT_FALSE(p) << w << "x" << h;
        EXPECT_EQ(p.error().code, ErrorCode::InvalidArgument);
    }
    const std::array<CfaColor, 1> undefined = {static_cast<CfaColor>(9)};
    auto bad = CfaPattern::create(1, 1, undefined);
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, ErrorCode::InvalidArgument);
}

TEST(CfaPattern, EmptyDefault) {
    const CfaPattern p;
    EXPECT_TRUE(p.empty());
    EXPECT_EQ(p.width(), 0);
    EXPECT_EQ(p.height(), 0);
    EXPECT_EQ(p.to_string(), "");
    EXPECT_FALSE(p.is_bayer());
    EXPECT_FALSE(p.is_xtrans());
    EXPECT_EQ(p, CfaPattern{});
    EXPECT_NE(p, CfaPattern::bayer(BayerLayout::Rggb));
    EXPECT_TRUE(p.shifted(3, 1).empty());
}

}  // namespace
}  // namespace lucent::image
