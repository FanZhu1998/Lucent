// encode_for_output: known code values, cross-check of the Little CMS conversion against
// lucent_image's conversion_matrix + transfer functions, clipping, determinism and invalid input.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/core/cancellation.hpp>
#include <lucent/export/encode.hpp>
#include <lucent/image/color_math.hpp>

#include "export_test_support.hpp"

namespace lucent::exporter {
namespace {

using test_support::make_rgb;

image::ColorEncoding display_linear(image::ColorPrimaries primaries) {
    image::ColorEncoding e;
    e.primaries = primaries;
    e.reference = image::ImageReference::Display;
    return e;
}

const image::ColorEncoding kRec2020Display = display_linear(image::ColorPrimaries::Rec2020);
const image::ColorEncoding kLinearSrgbDisplay = display_linear(image::ColorPrimaries::Rec709Srgb);

// One pixel through encode_for_output.
std::vector<int> encode_pixel(float r, float g, float b, const image::ColorEncoding& source, OutputProfile profile,
                              BitDepth depth, const OutputTransformOptions& options = {}) {
    auto buffer = make_rgb(1, 1, [&](std::int32_t, std::int32_t, std::int32_t c) { return c == 0 ? r : c == 1 ? g : b; });
    auto out = encode_for_output(buffer.const_view(), source, profile, depth, options);
    EXPECT_TRUE(out) << out.error().describe();
    if (!out) {
        return {};
    }
    if (depth == BitDepth::Eight) {
        return {out->samples8[0], out->samples8[1], out->samples8[2]};
    }
    return {out->samples16[0], out->samples16[1], out->samples16[2]};
}

// Independent reference: lucent_image matrix (Bradford) and sRGB curve, clip, round half up.
std::vector<int> reference_pixel(float r, float g, float b, const image::ColorEncoding& source, OutputProfile profile,
                                 BitDepth depth) {
    image::ColorEncoding dst = source;
    dst.primaries = profile == OutputProfile::Srgb ? image::ColorPrimaries::Rec709Srgb : image::ColorPrimaries::DisplayP3;
    dst.white_point = image::WhitePoint::D65;
    auto m = image::conversion_matrix(source, dst);
    EXPECT_TRUE(m);
    const image::Vec3d linear = *m * image::Vec3d{r, g, b};
    const double max_code = depth == BitDepth::Eight ? 255.0 : 65535.0;
    std::vector<int> codes;
    for (std::size_t c = 0; c < 3; ++c) {
        const double encoded = std::clamp(image::srgb_encode(linear[c]), 0.0, 1.0);
        codes.push_back(static_cast<int>(std::floor(encoded * max_code + 0.5)));
    }
    return codes;
}

TEST(EncodeForOutput, MiddleGreyMapsToKnownCodeValues) {
    // Linear 0.18 encodes to sRGB 0.461356 (IEC 61966-2-1): 8-bit 117.65 -> 118,
    // 16-bit 30235.0 -> 30235. Neutral stays neutral in every source space with a D65 white.
    const double encoded = image::srgb_encode(0.18);
    EXPECT_NEAR(encoded, 0.461356, 1e-6);
    for (const auto& source : {kLinearSrgbDisplay, kRec2020Display}) {
        for (const OutputProfile profile : {OutputProfile::Srgb, OutputProfile::DisplayP3}) {
            EXPECT_EQ(encode_pixel(0.18f, 0.18f, 0.18f, source, profile, BitDepth::Eight),
                      (std::vector<int>{118, 118, 118}))
                << source.to_string() << " -> " << to_string(profile);
            // 16-bit: +/-2 codes for the s15Fixed16 colorants of the stored profile.
            const auto codes = encode_pixel(0.18f, 0.18f, 0.18f, source, profile, BitDepth::Sixteen);
            ASSERT_EQ(codes.size(), 3u);
            for (const int code : codes) {
                EXPECT_NEAR(code, 30235, 2) << source.to_string() << " -> " << to_string(profile);
            }
        }
    }
}

TEST(EncodeForOutput, BlackAndWhiteHitTheEndpoints) {
    EXPECT_EQ(encode_pixel(0, 0, 0, kRec2020Display, OutputProfile::Srgb, BitDepth::Sixteen),
              (std::vector<int>{0, 0, 0}));
    const auto white = encode_pixel(1, 1, 1, kRec2020Display, OutputProfile::Srgb, BitDepth::Sixteen);
    for (const int code : white) {
        EXPECT_GE(code, 65533);  // within s15Fixed16 rounding of full scale
    }
    EXPECT_EQ(encode_pixel(1, 1, 1, kRec2020Display, OutputProfile::DisplayP3, BitDepth::Eight),
              (std::vector<int>{255, 255, 255}));
}

TEST(EncodeForOutput, Rec2020PrimariesClipPerChannelInSrgb) {
    // Rec.2020 primaries lie outside sRGB: in linear BT.709 red is (1.66, -0.12, -0.02), green
    // (-0.59, 1.13, -0.10), blue (-0.07, -0.01, 1.12). Clipping is per channel.
    EXPECT_EQ(encode_pixel(1, 0, 0, kRec2020Display, OutputProfile::Srgb, BitDepth::Eight),
              (std::vector<int>{255, 0, 0}));
    EXPECT_EQ(encode_pixel(0, 1, 0, kRec2020Display, OutputProfile::Srgb, BitDepth::Eight),
              (std::vector<int>{0, 255, 0}));
    EXPECT_EQ(encode_pixel(0, 0, 1, kRec2020Display, OutputProfile::Srgb, BitDepth::Eight),
              (std::vector<int>{0, 0, 255}));
    // Values above 1 and below 0 clip to the endpoints.
    EXPECT_EQ(encode_pixel(4.0f, -0.5f, 0.18f, kLinearSrgbDisplay, OutputProfile::Srgb, BitDepth::Eight),
              (std::vector<int>{255, 0, 118}));
}

TEST(EncodeForOutput, MatchesLucentImageColourMath) {
    // A grid of in-gamut, out-of-gamut and over-range colours, through both profiles and depths.
    // Tolerances. 8-bit: +/-1 code (rounding ties). 16-bit: compared in linear light, 1.5e-4 of
    // diffuse white. The conversion goes through the stored output profile, whose colorants are
    // s15Fixed16 (up to 1.5e-5 per matrix entry); with channel values up to 1.3 and inverse-matrix
    // gains near 3 that is up to about 6e-5 in linear light, plus half a 16-bit code step
    // (at most 1.9e-5 near white). Near black the sRGB slope of 12.92 turns this into tens of
    // codes, which is why the comparison is done in linear light.
    const std::vector<float> levels = {-0.2f, 0.0f, 0.001f, 0.01f, 0.05f, 0.18f, 0.5f, 0.9f, 1.0f, 1.3f};
    const image::ColorEncoding sources[] = {kRec2020Display, kLinearSrgbDisplay,
                                            display_linear(image::ColorPrimaries::ProPhotoRomm)};
    for (const auto& source : sources) {
        image::ColorEncoding src = source;
        if (src.primaries == image::ColorPrimaries::ProPhotoRomm) {
            src.white_point = image::WhitePoint::D50;  // exercises chromatic adaptation
        }
        const auto n = static_cast<std::int32_t>(levels.size());
        auto buffer = make_rgb(n * n, n, [&](std::int32_t x, std::int32_t y, std::int32_t c) {
            const std::int32_t index = c == 0 ? x % n : c == 1 ? x / n : y;
            return levels[static_cast<std::size_t>(index)];
        });
        for (const OutputProfile profile : {OutputProfile::Srgb, OutputProfile::DisplayP3}) {
            for (const BitDepth depth : {BitDepth::Eight, BitDepth::Sixteen}) {
                auto out = encode_for_output(buffer.const_view(), src, profile, depth);
                ASSERT_TRUE(out) << out.error().describe();
                const bool eight = depth == BitDepth::Eight;
                const double max_code = eight ? 255.0 : 65535.0;
                const double tolerance = eight ? 1.0 : 1.5e-4;
                double worst = 0.0;
                std::string worst_where;
                for (std::int32_t y = 0; y < buffer.height(); ++y) {
                    for (std::int32_t x = 0; x < buffer.width(); ++x) {
                        const auto expected = reference_pixel(buffer.at(x, y, 0), buffer.at(x, y, 1),
                                                              buffer.at(x, y, 2), src, profile, depth);
                        for (std::int32_t c = 0; c < 3; ++c) {
                            const std::size_t i = (static_cast<std::size_t>(y) * buffer.width() + x) * 3 + c;
                            const int actual = eight ? out->samples8[i] : out->samples16[i];
                            const int wanted = expected[static_cast<std::size_t>(c)];
                            const double diff =
                                eight ? std::abs(actual - wanted)
                                      : std::abs(image::srgb_decode(actual / max_code) -
                                                 image::srgb_decode(wanted / max_code));
                            if (diff > worst) {
                                worst = diff;
                                worst_where = "input (" + std::to_string(buffer.at(x, y, 0)) + ", " +
                                              std::to_string(buffer.at(x, y, 1)) + ", " +
                                              std::to_string(buffer.at(x, y, 2)) + ") channel " +
                                              std::to_string(c) + ": " + std::to_string(actual) + " vs " +
                                              std::to_string(expected[static_cast<std::size_t>(c)]);
                            }
                        }
                    }
                }
                EXPECT_LE(worst, tolerance) << src.to_string() << " -> " << to_string(profile) << " "
                                            << to_string(depth) << "-bit, worst at " << worst_where;
            }
        }
    }
}

TEST(EncodeForOutput, EncodedSourcesAreDecodedFirst) {
    image::ColorEncoding srgb_encoded = image::ColorEncoding::srgb_display();
    const auto encoded = static_cast<float>(image::srgb_encode(0.18));
    EXPECT_EQ(encode_pixel(encoded, encoded, encoded, srgb_encoded, OutputProfile::Srgb, BitDepth::Eight),
              (std::vector<int>{118, 118, 118}));
}

TEST(EncodeForOutput, OutputLayoutAndStridedViews) {
    auto buffer = test_support::make_gradient(37, 11);
    auto sub = buffer.const_view().view(image::PixelRect{3, 2, 20, 5});
    ASSERT_TRUE(sub);
    auto out = encode_for_output(*sub, kRec2020Display, OutputProfile::Srgb, BitDepth::Sixteen);
    ASSERT_TRUE(out) << out.error().describe();
    EXPECT_EQ(out->width, 20);
    EXPECT_EQ(out->height, 5);
    EXPECT_EQ(out->bit_depth, BitDepth::Sixteen);
    EXPECT_EQ(out->profile, OutputProfile::Srgb);
    EXPECT_EQ(out->samples16.size(), 20u * 5u * 3u);
    EXPECT_TRUE(out->samples8.empty());
    EXPECT_TRUE(out->validate());

    // The sub-view's pixel (0, 0) is the parent's (3, 2).
    auto single = make_rgb(1, 1, [&](std::int32_t, std::int32_t, std::int32_t c) { return buffer.at(3, 2, c); });
    auto one = encode_for_output(single.const_view(), kRec2020Display, OutputProfile::Srgb, BitDepth::Sixteen);
    ASSERT_TRUE(one);
    EXPECT_EQ(std::vector<std::uint16_t>(out->samples16.begin(), out->samples16.begin() + 3), one->samples16);
}

TEST(EncodeForOutput, ThreadedResultEqualsRowByRowResult) {
    // Large enough for several threads; every row is then encoded alone (one thread) and must
    // match exactly.
    auto buffer = test_support::make_gradient(1024, 640);
    auto whole = encode_for_output(buffer.const_view(), kRec2020Display, OutputProfile::DisplayP3, BitDepth::Sixteen);
    ASSERT_TRUE(whole) << whole.error().describe();
    for (std::int32_t y = 0; y < buffer.height(); y += 37) {
        auto row = buffer.const_view().view(image::PixelRect{0, y, buffer.width(), 1});
        ASSERT_TRUE(row);
        auto alone = encode_for_output(*row, kRec2020Display, OutputProfile::DisplayP3, BitDepth::Sixteen);
        ASSERT_TRUE(alone);
        EXPECT_TRUE(std::equal(alone->samples16.begin(), alone->samples16.end(),
                               whole->samples16.begin() + static_cast<std::ptrdiff_t>(whole->row_samples()) * y))
            << "row " << y;
    }
}

TEST(EncodeForOutput, NonFiniteSamplesAreRejectedWithTheirLocation) {
    for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity()}) {
        auto buffer = test_support::make_gradient(8, 6);
        buffer.at(3, 2, 1) = bad;
        buffer.at(5, 4, 0) = bad;  // later in row-major order: not the one reported
        auto out = encode_for_output(buffer.const_view(), kRec2020Display, OutputProfile::Srgb, BitDepth::Eight);
        ASSERT_FALSE(out);
        EXPECT_EQ(out.error().code, ErrorCode::InvalidArgument);
        EXPECT_NE(out.error().message.find("(3, 2) channel 1"), std::string::npos) << out.error().message;
    }
}

TEST(EncodeForOutput, InvalidSourcesAreRejected) {
    auto rgb = test_support::make_gradient(4, 4);
    const auto code_of = [](const auto& result) { return result ? ErrorCode::Unknown : result.error().code; };

    auto mono = image::ImageBuffer<float>::create(4, 4, 1);
    ASSERT_TRUE(mono);
    EXPECT_EQ(code_of(encode_for_output(mono->const_view(), kRec2020Display, OutputProfile::Srgb, BitDepth::Eight)),
              ErrorCode::InvalidArgument);
    auto rgba = image::ImageBuffer<float>::create(4, 4, 4);
    ASSERT_TRUE(rgba);
    EXPECT_EQ(code_of(encode_for_output(rgba->const_view(), kRec2020Display, OutputProfile::Srgb, BitDepth::Eight)),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(code_of(encode_for_output(image::ConstImageView<float>{}, kRec2020Display, OutputProfile::Srgb,
                                        BitDepth::Eight)),
              ErrorCode::InvalidArgument);

    // Scene-referred, camera-native and XYZ sources are not accepted.
    EXPECT_EQ(code_of(encode_for_output(rgb.const_view(), image::ColorEncoding::scene_linear_rec2020_d65(),
                                        OutputProfile::Srgb, BitDepth::Eight)),
              ErrorCode::InvalidArgument);
    image::ColorEncoding camera = image::ColorEncoding::camera_native_linear();
    camera.reference = image::ImageReference::Display;
    EXPECT_EQ(code_of(encode_for_output(rgb.const_view(), camera, OutputProfile::Srgb, BitDepth::Eight)),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(code_of(encode_for_output(rgb.const_view(), display_linear(image::ColorPrimaries::CieXyz),
                                        OutputProfile::Srgb, BitDepth::Eight)),
              ErrorCode::InvalidArgument);

    // Unknown enum values.
    EXPECT_EQ(code_of(encode_for_output(rgb.const_view(), kRec2020Display, static_cast<OutputProfile>(7),
                                        BitDepth::Eight)),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(code_of(encode_for_output(rgb.const_view(), kRec2020Display, OutputProfile::Srgb,
                                        static_cast<BitDepth>(10))),
              ErrorCode::InvalidArgument);
    OutputTransformOptions bad_intent;
    bad_intent.intent = static_cast<RenderingIntent>(99);
    EXPECT_EQ(code_of(encode_for_output(rgb.const_view(), kRec2020Display, OutputProfile::Srgb, BitDepth::Eight,
                                        bad_intent)),
              ErrorCode::InvalidArgument);
}

TEST(EncodeForOutput, CancellationStopsTheConversion) {
    core::CancellationSource source;
    source.cancel();
    auto rgb = test_support::make_gradient(16, 16);
    auto out = encode_for_output(rgb.const_view(), kRec2020Display, OutputProfile::Srgb, BitDepth::Eight, {},
                                 source.token());
    ASSERT_FALSE(out);
    EXPECT_EQ(out.error().code, ErrorCode::Cancelled);
}

TEST(EncodeForOutput, EveryIntentAndBpcProducesOutput) {
    for (const RenderingIntent intent : {RenderingIntent::Perceptual, RenderingIntent::RelativeColorimetric,
                                         RenderingIntent::Saturation, RenderingIntent::AbsoluteColorimetric}) {
        for (const bool bpc : {false, true}) {
            OutputTransformOptions options;
            options.intent = intent;
            options.black_point_compensation = bpc;
            // D65 -> D65 matrix/TRC: every intent is colorimetric here, so grey stays 118.
            EXPECT_EQ(encode_pixel(0.18f, 0.18f, 0.18f, kRec2020Display, OutputProfile::Srgb, BitDepth::Eight, options),
                      (std::vector<int>{118, 118, 118}))
                << to_string(intent) << " bpc=" << bpc;
        }
    }
}

TEST(OutputImage, ValidateChecksBufferConsistency) {
    OutputImage image;
    image.width = 2;
    image.height = 1;
    image.bit_depth = BitDepth::Eight;
    image.samples8.resize(6);
    EXPECT_TRUE(image.validate());
    image.samples16.resize(1);
    EXPECT_FALSE(image.validate());
    image.samples16.clear();
    image.samples8.resize(5);
    EXPECT_FALSE(image.validate());
    image.samples8.resize(6);
    image.width = 0;
    EXPECT_FALSE(image.validate());
}

}  // namespace
}  // namespace lucent::exporter
