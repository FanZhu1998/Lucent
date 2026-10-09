#pragma once

// Independent read-back of exported files for the qualification suite's export round trip: the
// published JPEG and 16-bit TIFF are decoded with libjpeg-turbo and libtiff directly (not through
// the exporter's own verification code), row by row, and their embedded ICC profile is extracted.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace lucent::qualify {

struct ReadbackHeader {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t channels = 0;
    std::int32_t bits = 0;
    std::vector<std::uint8_t> icc;  // embedded ICC profile bytes (empty when absent)
};

// 8-bit JPEG decoded to RGB (islow DCT). False with `error` set on any failure or decoder warning.
bool read_jpeg(const std::filesystem::path& path, ReadbackHeader& header, std::string& error,
               const std::function<void(std::int32_t, std::span<const std::uint8_t>)>& row);

// 16-bit chunky RGB TIFF. False with `error` set on any failure or another layout.
bool read_tiff16(const std::filesystem::path& path, ReadbackHeader& header, std::string& error,
                 const std::function<void(std::int32_t, std::span<const std::uint16_t>)>& row);

}  // namespace lucent::qualify
