// make_jpeg_thumbnail: reduced size and aspect, colour kept (area averaging), small inputs returned
// unchanged, ICC profiles carried over, determinism, and invalid or damaged input.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/core/cancellation.hpp>
#include <lucent/export/encode.hpp>
#include <lucent/export/export.hpp>
#include <lucent/export/thumbnail.hpp>

#include "codecs.hpp"

namespace lucent::exporter {
namespace {

// An 8-bit image: a horizontal red ramp, green constant, blue a vertical ramp.
OutputImage ramp_image(std::int32_t width, std::int32_t height) {
    OutputImage image;
    image.width = width;
    image.height = height;
    image.bit_depth = BitDepth::Eight;
    image.samples8.resize(static_cast<std::size_t>(width) * height * 3);
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            std::uint8_t* p = image.samples8.data() + (static_cast<std::size_t>(y) * width + x) * 3;
            p[0] = static_cast<std::uint8_t>(255 * x / (width - 1));
            p[1] = 128;
            p[2] = static_cast<std::uint8_t>(255 * y / (height - 1));
        }
    }
    return image;
}

std::vector<std::uint8_t> encode(const OutputImage& image, std::span<const std::uint8_t> icc = {}) {
    auto encoded = detail::encode_jpeg(image, JpegSettings{95}, icc);
    EXPECT_TRUE(encoded) << encoded.error().describe();
    return encoded ? *encoded : std::vector<std::uint8_t>{};
}

// The mean of each channel of a decoded stream.
std::vector<double> channel_means(const detail::DecodedJpegStream& decoded) {
    std::vector<double> sums(3, 0.0);
    for (std::size_t i = 0; i < decoded.rgb.size(); ++i) {
        sums[i % 3] += decoded.rgb[i];
    }
    const double pixels = static_cast<double>(decoded.rgb.size() / 3);
    for (double& sum : sums) {
        sum /= pixels;
    }
    return sums;
}

JpegThumbnailOptions options(std::int32_t max_long_edge, std::int32_t quality) {
    JpegThumbnailOptions o;
    o.max_long_edge = max_long_edge;
    o.quality = quality;
    return o;
}

TEST(JpegThumbnail, ReducesTheLongEdgeAndKeepsTheAspectAndTheColours) {
    const OutputImage source = ramp_image(3000, 2000);
    const std::vector<std::uint8_t> large = encode(source);
    auto thumbnail = make_jpeg_thumbnail(large, options(1024, 85));
    ASSERT_TRUE(thumbnail) << thumbnail.error().describe();
    EXPECT_TRUE(thumbnail->reduced);
    EXPECT_EQ(thumbnail->width, 1024);
    EXPECT_EQ(thumbnail->height, 683);  // 2000 * 1024 / 3000 = 682.7
    EXPECT_LT(thumbnail->jpeg.size(), large.size() / 4);

    auto decoded = detail::decode_jpeg_stream(thumbnail->jpeg, 1);
    ASSERT_TRUE(decoded) << decoded.error().describe();
    EXPECT_EQ(decoded->width, 1024);
    EXPECT_EQ(decoded->height, 683);
    // Area averaging keeps the mean of every channel (tolerance: JPEG quantisation at q85 and q95).
    auto original = detail::decode_jpeg_stream(large, 1);
    ASSERT_TRUE(original);
    const std::vector<double> before = channel_means(*original);
    const std::vector<double> after = channel_means(*decoded);
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(after[static_cast<std::size_t>(c)], before[static_cast<std::size_t>(c)], 1.5) << c;
    }
    // The ramps run the same way: the left edge is dark red, the right edge bright red.
    EXPECT_LT(decoded->rgb[0], 20);
    EXPECT_GT(decoded->rgb[(1023) * 3], 235);

    // The same input gives the same bytes.
    auto again = make_jpeg_thumbnail(large, options(1024, 85));
    ASSERT_TRUE(again);
    EXPECT_EQ(again->jpeg, thumbnail->jpeg);
}

TEST(JpegThumbnail, PortraitAndDctScalingBoundaries) {
    // 1000 x 4100: 1/4 would give 1025 >= 1024, so the decoder reduces by 4 and the area step by
    // a hair; the short side is rounded.
    auto portrait = make_jpeg_thumbnail(encode(ramp_image(1000, 4100)), options(1024, 85));
    ASSERT_TRUE(portrait) << portrait.error().describe();
    EXPECT_EQ(portrait->height, 1024);
    EXPECT_EQ(portrait->width, 250);  // 1000 * 1024 / 4100 = 249.8
    // Just above the target: decoded at full size, reduced by area averaging only.
    auto barely = make_jpeg_thumbnail(encode(ramp_image(1030, 500)), options(1024, 85));
    ASSERT_TRUE(barely);
    EXPECT_EQ(barely->width, 1024);
    EXPECT_EQ(barely->height, 497);
}

TEST(JpegThumbnail, SmallInputsAreReturnedUnchangedUnlessTheyAreTooLarge) {
    const std::vector<std::uint8_t> small = encode(ramp_image(640, 427));
    auto thumbnail = make_jpeg_thumbnail(small, options(1024, 85));
    ASSERT_TRUE(thumbnail);
    EXPECT_FALSE(thumbnail->reduced);
    EXPECT_EQ(thumbnail->jpeg, small);
    EXPECT_EQ(thumbnail->width, 640);
    EXPECT_EQ(thumbnail->height, 427);

    // Within the pixel limit but over the byte limit: re-encoded at its own size.
    JpegThumbnailOptions bounded = options(1024, 50);
    bounded.max_unreduced_bytes = small.size() - 1;
    auto reencoded = make_jpeg_thumbnail(small, bounded);
    ASSERT_TRUE(reencoded) << reencoded.error().describe();
    EXPECT_TRUE(reencoded->reduced);
    EXPECT_EQ(reencoded->width, 640);
    EXPECT_EQ(reencoded->height, 427);
    EXPECT_LT(reencoded->jpeg.size(), small.size());
}

TEST(JpegThumbnail, TheIccProfileIsCarriedOver) {
    std::vector<std::uint8_t> icc(600);
    for (std::size_t i = 0; i < icc.size(); ++i) {
        icc[i] = static_cast<std::uint8_t>(i * 7);
    }
    auto thumbnail = make_jpeg_thumbnail(encode(ramp_image(2048, 1536), icc), options(512, 80));
    ASSERT_TRUE(thumbnail);
    auto decoded = detail::decode_jpeg_stream(thumbnail->jpeg, 1);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->icc, icc);
    EXPECT_EQ(decoded->width, 512);
    EXPECT_EQ(decoded->height, 384);
}

TEST(JpegThumbnail, InvalidOrDamagedInputIsReported) {
    const std::vector<std::uint8_t> good = encode(ramp_image(2000, 1000));
    EXPECT_EQ(make_jpeg_thumbnail(good, options(0, 85)).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(make_jpeg_thumbnail(good, options(1024, 0)).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(make_jpeg_thumbnail(good, options(1024, 101)).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(make_jpeg_thumbnail({}, options(1024, 85)).error().code, ErrorCode::InvalidArgument);
    const std::vector<std::uint8_t> garbage(1000, 0x5A);
    EXPECT_EQ(make_jpeg_thumbnail(garbage, options(1024, 85)).error().code, ErrorCode::Corrupt);
    // A stream cut in the middle of its scan data still has a header but cannot be decoded fully.
    std::vector<std::uint8_t> truncated(good.begin(),
                                        good.begin() + static_cast<std::ptrdiff_t>(good.size() / 3));
    auto cut = make_jpeg_thumbnail(truncated, options(256, 85));
    if (!cut) {
        EXPECT_EQ(cut.error().code, ErrorCode::Corrupt);
    }  // libjpeg may also pad a truncated scan with grey (a warning, not an error)
    core::CancellationSource cancel;
    cancel.cancel();
    auto cancelled = make_jpeg_thumbnail(good, options(256, 85), cancel.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::Cancelled);
}

}  // namespace
}  // namespace lucent::exporter
