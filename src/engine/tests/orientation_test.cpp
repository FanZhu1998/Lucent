// Unit tests for orientation baking (orientation.hpp), checked against image::map_to_oriented.

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

#include <lucent/core/cancellation.hpp>
#include <lucent/engine/orientation.hpp>
#include <lucent/image/geometry.hpp>

#include "sensor_test_support.hpp"

namespace lucent::engine {
namespace {

namespace ts = test_support;
using image::Orientation;

constexpr std::array<Orientation, 8> kAll = {
    Orientation::Normal,    Orientation::FlipHorizontal, Orientation::Rotate180,  Orientation::FlipVertical,
    Orientation::Transpose, Orientation::Rotate90Cw,     Orientation::Transverse, Orientation::Rotate90Ccw};

// Sample (x, y, c) encodes its own position so that any misplacement is detected.
image::ImageBuffer<float> numbered(std::int32_t w, std::int32_t h, std::int32_t channels) {
    auto b = image::ImageBuffer<float>::create(w, h, channels);
    EXPECT_TRUE(b);
    for (std::int32_t y = 0; y < h; ++y) {
        for (std::int32_t x = 0; x < w; ++x) {
            for (std::int32_t c = 0; c < channels; ++c) {
                b->at(x, y, c) = static_cast<float>(y * 1000 + x * 10 + c);
            }
        }
    }
    return std::move(*b);
}

TEST(ApplyOrientation, MatchesTheGeometryMappingForEveryOrientation) {
    for (const std::int32_t channels : {1, 3}) {
        for (const Orientation o : kAll) {
            const image::Size stored{7, 4};
            auto out = apply_orientation(numbered(stored.width, stored.height, channels), o);
            ASSERT_TRUE(out) << out.error().describe();
            EXPECT_EQ(out->size(), image::oriented_size(stored, o)) << image::to_string(o);
            EXPECT_EQ(out->channels(), channels);
            for (std::int32_t y = 0; y < stored.height; ++y) {
                for (std::int32_t x = 0; x < stored.width; ++x) {
                    const image::Point p = image::map_to_oriented(image::Point{x, y}, stored, o);
                    for (std::int32_t c = 0; c < channels; ++c) {
                        ASSERT_EQ(out->at(p.x, p.y, c), static_cast<float>(y * 1000 + x * 10 + c))
                            << image::to_string(o) << " stored (" << x << ", " << y << ")";
                    }
                }
            }
        }
    }
}

TEST(ApplyOrientation, HandlesSingleColumnAndSingleRowImages) {
    for (const Orientation o : kAll) {
        for (const image::Size stored : {image::Size{1, 5}, image::Size{5, 1}, image::Size{1, 1}}) {
            auto out = apply_orientation(numbered(stored.width, stored.height, 3), o, {}, {}, false);
            ASSERT_TRUE(out);
            for (std::int32_t y = 0; y < stored.height; ++y) {
                for (std::int32_t x = 0; x < stored.width; ++x) {
                    const image::Point p = image::map_to_oriented(image::Point{x, y}, stored, o);
                    ASSERT_EQ(out->at(p.x, p.y, 1), static_cast<float>(y * 1000 + x * 10 + 1));
                }
            }
        }
    }
}

TEST(ApplyOrientation, NormalMovesTheBufferWithoutCopying) {
    image::ImageBuffer<float> b = numbered(4, 3, 3);
    const float* data = b.data();
    auto out = apply_orientation(std::move(b), Orientation::Normal);
    ASSERT_TRUE(out);
    EXPECT_EQ(out->data(), data);
}

TEST(ApplyOrientation, RejectsInvalidInput) {
    ts::expect_error(apply_orientation(image::ImageBuffer<float>{}, Orientation::Rotate90Cw),
                     ErrorCode::InvalidArgument, "engine.orientation");
    ts::expect_error(apply_orientation(numbered(2, 2, 3), static_cast<Orientation>(9)),
                     ErrorCode::InvalidArgument, "engine.orientation");
    image::AllocationLimits tiny;
    tiny.max_bytes = 16;
    auto limited = apply_orientation(numbered(8, 8, 3), Orientation::Rotate90Ccw, tiny);
    ASSERT_FALSE(limited);
    EXPECT_EQ(limited.error().code, ErrorCode::LimitExceeded);
}

TEST(ApplyOrientation, HonoursCancellation) {
    core::CancellationSource source;
    source.cancel();
    ts::expect_error(apply_orientation(numbered(4, 4, 3), Orientation::Rotate180, {}, source.token()),
                     ErrorCode::Cancelled, "engine.orientation");
}

}  // namespace
}  // namespace lucent::engine
