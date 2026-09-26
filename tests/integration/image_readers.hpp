#pragma once

// Minimal, independent readers for the files lucent::exporter publishes, used by the integration
// tests to check exported pixels against encode_for_output() without going through the
// exporter's own verification code. They decode row by row (no full-size copy) and hand each
// row to a callback. Test-only code.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>

namespace lucent::integration {

struct DecodedHeader {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t channels = 0;
    std::int32_t bits = 0;
};

// Decodes a 16-bit RGB TIFF (libtiff) row by row. Returns false with `error` set on any failure
// or when the layout is not 16-bit chunky RGB; `row` receives (y, width * 3 samples).
bool read_tiff16_rows(const std::filesystem::path& path, DecodedHeader& header, std::string& error,
                      const std::function<void(std::int32_t, std::span<const std::uint16_t>)>& row);

// Decodes a JPEG (libjpeg-turbo, islow DCT, RGB output) row by row. Returns false with `error`
// set on any failure or warning.
bool read_jpeg_rows(const std::filesystem::path& path, DecodedHeader& header, std::string& error,
                    const std::function<void(std::int32_t, std::span<const std::uint8_t>)>& row);

}  // namespace lucent::integration
