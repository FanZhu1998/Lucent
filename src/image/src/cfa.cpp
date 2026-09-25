#include "lucent/image/cfa.hpp"

#include <cassert>
#include <format>

namespace lucent::image {

namespace {

constexpr int kColorValueMax = static_cast<int>(CfaColor::White);

// Non-negative remainder for any divisor >= 1 (C++ % truncates towards zero).
constexpr std::int32_t floor_mod(std::int32_t value, std::int32_t divisor) noexcept {
    const std::int32_t r = value % divisor;
    return r < 0 ? r + divisor : r;
}

bool is_defined(CfaColor color) noexcept {
    return static_cast<int>(color) <= kColorValueMax;
}

// Base colour for a LibRaw cdesc letter; std::nullopt when Lucent has no such filter colour.
std::optional<CfaColor> color_from_letter(char letter) noexcept {
    switch (letter) {
        case 'R':
            return CfaColor::Red;
        case 'G':
            return CfaColor::Green;
        case 'B':
            return CfaColor::Blue;
        case 'C':
            return CfaColor::Cyan;
        case 'M':
            return CfaColor::Magenta;
        case 'Y':
            return CfaColor::Yellow;
        case 'W':
            return CfaColor::White;
        default:
            return std::nullopt;
    }
}

// LibRaw's FC(): the 2-bit colour index of (row, col) in the 8-row x 2-column `filters` tile.
// Verbatim from libraw/libraw.h:312-315 (LibRaw 0.22.2), with `filters` as a parameter.
constexpr unsigned libraw_fc(std::uint32_t filters, int row, int col) noexcept {
    return (filters >> ((((row) << 1 & 14) | ((col) & 1)) << 1) & 3);
}

}  // namespace

Expected<CfaColor> cfa_color_from_value(int value) {
    if (value < 0 || value > kColorValueMax) {
        return fail(ErrorCode::InvalidArgument, std::format("CFA colour value {} is outside 0..7", value),
                    "image.cfa.color");
    }
    return static_cast<CfaColor>(value);
}

std::string_view to_string(CfaColor color) noexcept {
    switch (color) {
        case CfaColor::Red:
            return "red";
        case CfaColor::Green:
            return "green";
        case CfaColor::Blue:
            return "blue";
        case CfaColor::Green2:
            return "green2";
        case CfaColor::Cyan:
            return "cyan";
        case CfaColor::Magenta:
            return "magenta";
        case CfaColor::Yellow:
            return "yellow";
        case CfaColor::White:
            return "white";
    }
    return "invalid";
}

char to_letter(CfaColor color) noexcept {
    switch (color) {
        case CfaColor::Red:
            return 'R';
        case CfaColor::Green:
            return 'G';
        case CfaColor::Blue:
            return 'B';
        case CfaColor::Green2:
            return 'G';
        case CfaColor::Cyan:
            return 'C';
        case CfaColor::Magenta:
            return 'M';
        case CfaColor::Yellow:
            return 'Y';
        case CfaColor::White:
            return 'W';
    }
    return '?';
}

std::string_view to_string(BayerLayout layout) noexcept {
    switch (layout) {
        case BayerLayout::Rggb:
            return "RGGB";
        case BayerLayout::Bggr:
            return "BGGR";
        case BayerLayout::Grbg:
            return "GRBG";
        case BayerLayout::Gbrg:
            return "GBRG";
    }
    return "invalid";
}

Expected<CfaPattern> CfaPattern::create(std::int32_t width, std::int32_t height,
                                        std::span<const CfaColor> colors) {
    constexpr const char* kWhere = "image.cfa.create";
    if (width < 1 || width > kMaxPeriod || height < 1 || height > kMaxPeriod) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("CFA tile must be 1..{} sites in each direction (got {}x{})", kMaxPeriod,
                                width, height),
                    kWhere);
    }
    const auto count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (colors.size() != count) {
        return fail(
            ErrorCode::InvalidArgument,
            std::format("CFA tile {}x{} needs {} colours (got {})", width, height, count, colors.size()),
            kWhere);
    }
    CfaPattern pattern;
    for (std::size_t i = 0; i < count; ++i) {
        if (!is_defined(colors[i])) {
            return fail(
                ErrorCode::InvalidArgument,
                std::format("undefined CFA colour value {} at index {}", static_cast<int>(colors[i]), i),
                kWhere);
        }
        pattern.colors_[i] = colors[i];
    }
    pattern.width_ = static_cast<std::uint8_t>(width);
    pattern.height_ = static_cast<std::uint8_t>(height);
    pattern.reduce_to_minimal_period();
    return pattern;
}

CfaPattern CfaPattern::bayer(BayerLayout layout) noexcept {
    using enum CfaColor;
    // Row-major 2x2 tiles; Green shares its row with Red, Green2 with Blue.
    std::array<CfaColor, 4> tile{Red, Green, Green2, Blue};  // RGGB
    switch (layout) {
        case BayerLayout::Rggb:
            tile = {Red, Green, Green2, Blue};
            break;
        case BayerLayout::Bggr:
            tile = {Blue, Green2, Green, Red};
            break;
        case BayerLayout::Grbg:
            tile = {Green, Red, Blue, Green2};
            break;
        case BayerLayout::Gbrg:
            tile = {Green2, Blue, Red, Green};
            break;
    }
    CfaPattern pattern;
    for (std::size_t i = 0; i < tile.size(); ++i) {
        pattern.colors_[i] = tile[i];
    }
    pattern.width_ = 2;
    pattern.height_ = 2;
    return pattern;
}

Expected<CfaPattern> CfaPattern::from_libraw_filters(std::uint32_t filters, int colors,
                                                     std::string_view cdesc) {
    constexpr const char* kWhere = "image.cfa.from_libraw_filters";
    if (colors < 1 || colors > 4) {
        return fail(ErrorCode::InvalidArgument, std::format("LibRaw colour count {} is outside 1..4", colors),
                    kWhere);
    }
    if (filters == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "filters == 0 describes full-colour or monochrome data, not a CFA", kWhere);
    }
    if (filters == 9) {
        return fail(
            ErrorCode::InvalidArgument,
            "filters == 9 denotes X-Trans; build the pattern from imgdata.idata.xtrans with from_xtrans",
            kWhere);
    }
    if (filters == 1) {
        return fail(ErrorCode::Unsupported, "filters == 1 (Leaf CatchLight 16x16 CFA) is not supported",
                    kWhere, static_cast<std::int64_t>(filters));
    }
    if (filters < 1000) {
        return fail(ErrorCode::Unsupported, std::format("reserved LibRaw filters value {}", filters), kWhere,
                    static_cast<std::int64_t>(filters));
    }

    // Resolve each colour index 0..3 on first use; a letter repeating an earlier letter of
    // cdesc is the second site of that colour (LibRaw's "RGBG": index 3 is the second green).
    std::array<std::optional<CfaColor>, 4> by_index{};
    auto resolve = [&](unsigned index) -> Expected<CfaColor> {
        if (by_index[index]) {
            return *by_index[index];
        }
        if (index >= cdesc.size()) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("cdesc \"{}\" has no letter for colour index {}", cdesc, index), kWhere);
        }
        const char letter = cdesc[index];
        const std::optional<CfaColor> base = color_from_letter(letter);
        if (!base) {
            return fail(ErrorCode::Unsupported,
                        std::format("CFA colour '{}' (cdesc \"{}\") has no Lucent equivalent", letter, cdesc),
                        kWhere);
        }
        const auto earlier = cdesc.substr(0, index).find(letter);
        CfaColor color = *base;
        if (earlier != std::string_view::npos) {
            const bool third =
                cdesc.substr(earlier + 1, index - earlier - 1).find(letter) != std::string_view::npos;
            if (letter != 'G' || third) {
                return fail(
                    ErrorCode::Unsupported,
                    std::format("cdesc \"{}\" repeats colour '{}' in an unsupported way", cdesc, letter),
                    kWhere);
            }
            color = CfaColor::Green2;
        }
        by_index[index] = color;
        return color;
    };

    constexpr std::int32_t kTileWidth = 2;
    constexpr std::int32_t kTileHeight = 8;
    std::array<CfaColor, static_cast<std::size_t>(kTileWidth * kTileHeight)> tile{};
    for (int row = 0; row < kTileHeight; ++row) {
        for (int col = 0; col < kTileWidth; ++col) {
            auto color = resolve(libraw_fc(filters, row, col));
            if (!color) {
                return Unexpected(std::move(color.error()));
            }
            tile[static_cast<std::size_t>(row * kTileWidth + col)] = *color;
        }
    }
    return create(kTileWidth, kTileHeight, tile);
}

Expected<CfaPattern> CfaPattern::from_xtrans(const char (&xtrans)[6][6]) {
    std::array<CfaColor, 36> tile{};
    for (std::size_t row = 0; row < 6; ++row) {
        for (std::size_t col = 0; col < 6; ++col) {
            const int value = xtrans[row][col];
            if (value < 0 || value > 2) {
                return fail(ErrorCode::InvalidArgument,
                            std::format("X-Trans colour index {} at row {}, column {} is outside 0..2", value,
                                        row, col),
                            "image.cfa.from_xtrans");
            }
            tile[row * 6 + col] = static_cast<CfaColor>(value);  // 0, 1, 2 = Red, Green, Blue
        }
    }
    return create(6, 6, tile);
}

CfaColor CfaPattern::color_at(std::int32_t row, std::int32_t col) const noexcept {
    assert(!empty() && "color_at on an empty CfaPattern");
    if (empty()) {
        return CfaColor::Red;
    }
    return stored(floor_mod(row, height_), floor_mod(col, width_));
}

CfaPattern CfaPattern::shifted(std::int32_t dx, std::int32_t dy) const noexcept {
    if (empty()) {
        return *this;
    }
    const std::int32_t sx = floor_mod(dx, width_);
    const std::int32_t sy = floor_mod(dy, height_);
    CfaPattern result;
    result.width_ = width_;
    result.height_ = height_;
    for (std::int32_t row = 0; row < height_; ++row) {
        for (std::int32_t col = 0; col < width_; ++col) {
            result.colors_[static_cast<std::size_t>(row * width_ + col)] = color_at(row + sy, col + sx);
        }
    }
    return result;
}

bool CfaPattern::is_bayer() const noexcept {
    return bayer_layout().has_value();
}

std::optional<BayerLayout> CfaPattern::bayer_layout() const noexcept {
    if (width_ != 2 || height_ != 2) {
        return std::nullopt;
    }
    int reds = 0;
    int blues = 0;
    int greens = 0;
    std::int32_t red_row = 0;
    std::int32_t red_col = 0;
    std::int32_t blue_row = 0;
    std::int32_t blue_col = 0;
    for (std::int32_t row = 0; row < 2; ++row) {
        for (std::int32_t col = 0; col < 2; ++col) {
            const CfaColor color = stored(row, col);
            if (color == CfaColor::Red) {
                ++reds;
                red_row = row;
                red_col = col;
            } else if (color == CfaColor::Blue) {
                ++blues;
                blue_row = row;
                blue_col = col;
            } else if (is_green(color)) {
                ++greens;
            }
        }
    }
    if (reds != 1 || blues != 1 || greens != 2 || red_row == blue_row || red_col == blue_col) {
        return std::nullopt;
    }
    if (red_row == 0) {
        return red_col == 0 ? BayerLayout::Rggb : BayerLayout::Grbg;
    }
    return red_col == 0 ? BayerLayout::Gbrg : BayerLayout::Bggr;
}

bool CfaPattern::is_xtrans() const noexcept {
    if (width_ != 6 || height_ != 6) {
        return false;
    }
    int counts[3] = {0, 0, 0};  // red, green, blue
    // Bit masks of the colours seen in each row and each column (bit 0 red, 1 green, 2 blue).
    unsigned row_seen[6] = {};
    unsigned col_seen[6] = {};
    for (std::int32_t row = 0; row < 6; ++row) {
        for (std::int32_t col = 0; col < 6; ++col) {
            const CfaColor color = stored(row, col);
            int channel = 0;
            if (color == CfaColor::Red) {
                channel = 0;
            } else if (is_green(color)) {
                channel = 1;
            } else if (color == CfaColor::Blue) {
                channel = 2;
            } else {
                return false;
            }
            ++counts[channel];
            row_seen[row] |= 1u << channel;
            col_seen[col] |= 1u << channel;
        }
    }
    if (counts[0] != 8 || counts[1] != 20 || counts[2] != 8) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        if (row_seen[i] != 7u || col_seen[i] != 7u) {
            return false;
        }
    }
    return true;
}

std::string CfaPattern::to_string() const {
    std::string text;
    if (empty()) {
        return text;
    }
    const bool separate_rows = !(width_ == 2 && height_ == 2);
    for (std::int32_t row = 0; row < height_; ++row) {
        if (separate_rows && row > 0) {
            text += '/';
        }
        for (std::int32_t col = 0; col < width_; ++col) {
            text += to_letter(stored(row, col));
        }
    }
    return text;
}

bool operator==(const CfaPattern& a, const CfaPattern& b) noexcept {
    if (a.width_ != b.width_ || a.height_ != b.height_) {
        return false;
    }
    for (std::int32_t row = 0; row < a.height_; ++row) {
        for (std::int32_t col = 0; col < a.width_; ++col) {
            if (a.stored(row, col) != b.stored(row, col)) {
                return false;
            }
        }
    }
    return true;
}

void CfaPattern::reduce_to_minimal_period() noexcept {
    // The smallest period along an axis always divides the tile size (if p and the tile size
    // are both periods, so is their gcd), so only divisors need testing.
    auto column_period = [this]() {
        for (std::int32_t p = 1; p < width_; ++p) {
            if (width_ % p != 0) {
                continue;
            }
            bool periodic = true;
            for (std::int32_t row = 0; row < height_ && periodic; ++row) {
                for (std::int32_t col = 0; col < width_ && periodic; ++col) {
                    periodic = stored(row, col) == stored(row, (col + p) % width_);
                }
            }
            if (periodic) {
                return p;
            }
        }
        return static_cast<std::int32_t>(width_);
    };
    auto row_period = [this]() {
        for (std::int32_t p = 1; p < height_; ++p) {
            if (height_ % p != 0) {
                continue;
            }
            bool periodic = true;
            for (std::int32_t row = 0; row < height_ && periodic; ++row) {
                for (std::int32_t col = 0; col < width_ && periodic; ++col) {
                    periodic = stored(row, col) == stored((row + p) % height_, col);
                }
            }
            if (periodic) {
                return p;
            }
        }
        return static_cast<std::int32_t>(height_);
    };
    const std::int32_t new_width = column_period();
    const std::int32_t new_height = row_period();
    if (new_width == width_ && new_height == height_) {
        return;
    }
    std::array<CfaColor, kCapacity> reduced{};
    for (std::int32_t row = 0; row < new_height; ++row) {
        for (std::int32_t col = 0; col < new_width; ++col) {
            reduced[static_cast<std::size_t>(row * new_width + col)] = stored(row, col);
        }
    }
    colors_ = reduced;
    width_ = static_cast<std::uint8_t>(new_width);
    height_ = static_cast<std::uint8_t>(new_height);
}

}  // namespace lucent::image
