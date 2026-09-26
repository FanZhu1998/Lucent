#pragma once

// Private helpers shared by sensor normalisation, white balance and demosaicing.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <lucent/core/error.hpp>
#include <lucent/image/cfa.hpp>

namespace lucent::engine::detail {

// Channel index of every site of one CFA period, relative to the CFA's frame. A pattern-less
// image (LinearColor, Monochrome) uses a 1 x 1 table holding channel 0.
class ChannelTable {
public:
    // InvalidArgument (at `where`) when a CFA colour has no channel index in `channel_colors`.
    [[nodiscard]] static Expected<ChannelTable> create(const image::CfaPattern& cfa,
                                                       std::span<const image::CfaColor> channel_colors,
                                                       const char* where);

    [[nodiscard]] std::int32_t width() const noexcept { return width_; }
    [[nodiscard]] std::int32_t height() const noexcept { return height_; }

    // The width() channel indices of table row `row` (0 <= row < height()).
    [[nodiscard]] const std::uint8_t* row(std::int32_t row) const noexcept {
        return indices_.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(width_);
    }

    // Channel index at any site (x, y) of the frame (the table repeats).
    [[nodiscard]] std::uint8_t at(std::int64_t x, std::int64_t y) const noexcept {
        const std::int64_t r = ((y % height_) + height_) % height_;
        const std::int64_t c = ((x % width_) + width_) % width_;
        return indices_[static_cast<std::size_t>(r * width_ + c)];
    }

    // Bit c set for every channel index c that occurs in the table.
    [[nodiscard]] std::uint32_t used_channels() const noexcept { return used_; }

private:
    static constexpr std::size_t kCapacity =
        static_cast<std::size_t>(image::CfaPattern::kMaxPeriod) * image::CfaPattern::kMaxPeriod;
    std::array<std::uint8_t, kCapacity> indices_{};
    std::int32_t width_ = 1;
    std::int32_t height_ = 1;
    std::uint32_t used_ = 1;
};

// True for the colours that have a white-balance and demosaicing model: red, both greens, blue.
[[nodiscard]] constexpr bool is_rgb_color(image::CfaColor color) noexcept {
    return color == image::CfaColor::Red || color == image::CfaColor::Green || color == image::CfaColor::Green2 ||
           color == image::CfaColor::Blue;
}

}  // namespace lucent::engine::detail
