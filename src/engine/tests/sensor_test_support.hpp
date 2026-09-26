#pragma once

// Helpers shared by the engine tests: synthetic decoded-RAW data that satisfies the contract,
// fixture lookup and small decoding utilities.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/core/error.hpp>
#include <lucent/image/cfa.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/image/pixel_buffer.hpp>
#include <lucent/raw/decoded_raw.hpp>

namespace lucent::engine::test_support {

inline const std::vector<image::CfaColor> kBayerChannels = {image::CfaColor::Red, image::CfaColor::Green,
                                                            image::CfaColor::Blue, image::CfaColor::Green2};
inline const std::vector<image::CfaColor> kRgbChannels = {image::CfaColor::Red, image::CfaColor::Green,
                                                          image::CfaColor::Blue};

// Sample value (stored units) of raw-frame position (x, y), channel s.
using SampleFn = std::function<double(std::int32_t x, std::int32_t y, std::int32_t s)>;

// Metadata common to every synthetic frame: stage AfterUnpack, one subimage, levels as given.
inline raw::RawMetadata base_metadata(std::int32_t raw_width, std::int32_t raw_height, image::PixelRect visible,
                                      const raw::SensorLevels& levels) {
    raw::RawMetadata m;
    m.stage = raw::MetadataStage::AfterUnpack;
    m.geometry.raw_width = static_cast<std::uint32_t>(raw_width);
    m.geometry.raw_height = static_cast<std::uint32_t>(raw_height);
    m.geometry.visible_area = visible;
    m.levels = levels;
    return m;
}

template <class T>
image::ImageBuffer<T> make_buffer(std::int32_t width, std::int32_t height, std::int32_t channels,
                                  const SampleFn& sample) {
    auto buffer = image::ImageBuffer<T>::create(width, height, channels);
    EXPECT_TRUE(buffer);
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            for (std::int32_t s = 0; s < channels; ++s) {
                buffer->at(x, y, s) = static_cast<T>(sample(x, y, s));
            }
        }
    }
    return std::move(*buffer);
}

// A decoded mosaic: `cfa_visible` is the CFA at the visible-area origin (as the engine sees it);
// the contract's raw-frame CFA is derived from it.
template <class T = std::uint16_t>
raw::DecodedRaw make_mosaic(std::int32_t raw_width, std::int32_t raw_height, image::PixelRect visible,
                            const image::CfaPattern& cfa_visible, const raw::SensorLevels& levels,
                            const SampleFn& sample, std::vector<image::CfaColor> channels = kBayerChannels) {
    raw::DecodedRaw d;
    d.metadata = base_metadata(raw_width, raw_height, visible, levels);
    raw::RepresentationInfo& rep = d.metadata.representation;
    rep.kind = raw::RawRepresentation::Mosaic;
    rep.sample_encoding = std::is_same_v<T, float> ? raw::SampleEncoding::Float32 : raw::SampleEncoding::UInt16;
    rep.channels = 1;
    rep.color_count = static_cast<std::int32_t>(channels.size() == 4 && channels[3] == image::CfaColor::Green2
                                                    ? 3
                                                    : channels.size());
    rep.color_description = "RGBG";
    rep.channel_colors = std::move(channels);
    rep.cfa = cfa_visible.shifted(-visible.x, -visible.y);
    raw::MosaicFrame frame;
    frame.pixels = make_buffer<T>(raw_width, raw_height, 1, sample);
    frame.cfa = rep.cfa;
    d.image = std::move(frame);
    return d;
}

// Already full-colour camera RGB (3 interleaved channels).
template <class T = std::uint16_t>
raw::DecodedRaw make_linear(std::int32_t raw_width, std::int32_t raw_height, image::PixelRect visible,
                            const raw::SensorLevels& levels, const SampleFn& sample) {
    raw::DecodedRaw d;
    d.metadata = base_metadata(raw_width, raw_height, visible, levels);
    raw::RepresentationInfo& rep = d.metadata.representation;
    rep.kind = raw::RawRepresentation::LinearColor;
    rep.sample_encoding = std::is_same_v<T, float> ? raw::SampleEncoding::Float32 : raw::SampleEncoding::UInt16;
    rep.channels = 3;
    rep.color_count = 3;
    rep.color_description = "RGBG";
    rep.channel_colors = kRgbChannels;
    raw::LinearColorImage linear;
    linear.pixels = make_buffer<T>(raw_width, raw_height, 3, sample);
    d.image = std::move(linear);
    return d;
}

// Monochrome sensor data (1 channel, colour White).
inline raw::DecodedRaw make_monochrome(std::int32_t raw_width, std::int32_t raw_height, image::PixelRect visible,
                                       const raw::SensorLevels& levels, const SampleFn& sample) {
    raw::DecodedRaw d;
    d.metadata = base_metadata(raw_width, raw_height, visible, levels);
    raw::RepresentationInfo& rep = d.metadata.representation;
    rep.kind = raw::RawRepresentation::Monochrome;
    rep.channels = 1;
    rep.color_count = 1;
    rep.color_description = "GMCY";
    rep.channel_colors = {image::CfaColor::White};
    raw::MonochromeFrame mono;
    mono.pixels = make_buffer<std::uint16_t>(raw_width, raw_height, 1, sample);
    d.image = std::move(mono);
    return d;
}

inline raw::SensorLevels simple_levels(double black, double white) {
    raw::SensorLevels levels;
    levels.black = black;
    levels.white = white;
    return levels;
}

// Asserts that `result` failed with `code` at `where`.
template <class R>
void expect_error(const R& result, ErrorCode code, std::string_view where) {
    ASSERT_FALSE(result) << "expected " << to_string(code) << " at " << where;
    EXPECT_EQ(result.error().code, code) << result.error().describe();
    EXPECT_EQ(result.error().where, where) << result.error().describe();
}

// The cached file of camera fixture `id` (fixtures/cache/<id>/<file>), or std::nullopt when it has
// not been downloaded (tools\fetch-fixtures.ps1).
inline std::optional<std::filesystem::path> fixture_file(const std::string& id) {
    const std::filesystem::path dir = std::filesystem::path(LUCENT_FIXTURE_DIR) / "cache" / id;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return std::nullopt;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() != L".partial") {
            return entry.path();
        }
    }
    return std::nullopt;
}

// RFC 4648 base64 (standard alphabet, '=' padding); std::nullopt for malformed text.
inline std::optional<std::vector<std::byte>> base64_decode(std::string_view text) {
    const auto value_of = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    std::vector<std::byte> out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        int v[4];
        int padding = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            const char c = text[i + k];
            if (c == '=' && i + 4 == text.size() && k >= 2) {
                v[k] = 0;
                ++padding;
            } else if (padding > 0 || (v[k] = value_of(c)) < 0) {
                return std::nullopt;
            }
        }
        const std::uint32_t triple = (static_cast<std::uint32_t>(v[0]) << 18) | (static_cast<std::uint32_t>(v[1]) << 12) |
                                     (static_cast<std::uint32_t>(v[2]) << 6) | static_cast<std::uint32_t>(v[3]);
        out.push_back(static_cast<std::byte>((triple >> 16) & 0xFF));
        if (padding < 2) out.push_back(static_cast<std::byte>((triple >> 8) & 0xFF));
        if (padding < 1) out.push_back(static_cast<std::byte>(triple & 0xFF));
    }
    return out;
}

}  // namespace lucent::engine::test_support

// Skips the current test when camera fixture `id` is not cached.
#define LUCENT_ENGINE_REQUIRE_FIXTURE(var, id)                                                       \
    const auto var##_optional = ::lucent::engine::test_support::fixture_file(id);                    \
    if (!var##_optional) {                                                                           \
        GTEST_SKIP() << "fixture '" << (id) << "' is not downloaded; run tools\\fetch-fixtures.ps1"; \
    }                                                                                                \
    const std::filesystem::path var = *var##_optional
