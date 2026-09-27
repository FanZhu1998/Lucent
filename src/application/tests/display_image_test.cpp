// Fitting, linear-light area downscaling, sRGB display encoding and histograms.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/application/display_image.hpp>
#include <lucent/engine/display_rendering.hpp>
#include <lucent/export/encode.hpp>
#include <lucent/image/color_math.hpp>

namespace lucent::app {
namespace {

engine::ColorImage make_image(std::int32_t w, std::int32_t h, auto value) {
    auto pixels = image::ImageBuffer<float>::create(w, h, 3);
    EXPECT_TRUE(pixels);
    for (std::int32_t y = 0; y < h; ++y) {
        for (std::int32_t x = 0; x < w; ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                pixels->at(x, y, c) = static_cast<float>(value(x, y, c));
            }
        }
    }
    return engine::ColorImage{std::move(*pixels), engine::display_linear_rec2020()};
}

double mean(const engine::ColorImage& image, std::int32_t channel) {
    double sum = 0.0;
    for (std::int32_t y = 0; y < image.pixels.height(); ++y) {
        for (std::int32_t x = 0; x < image.pixels.width(); ++x) {
            sum += image.pixels.at(x, y, channel);
        }
    }
    return sum / static_cast<double>(image.pixels.size().area());
}

TEST(FitWithin, KeepsTheAspectAndNeverEnlarges) {
    EXPECT_EQ(fit_within({8192, 5464}, 2048), (image::Size{2048, 1366}));
    EXPECT_EQ(fit_within({5464, 8192}, 2048), (image::Size{1366, 2048}));
    EXPECT_EQ(fit_within({1000, 1000}, 256), (image::Size{256, 256}));
    EXPECT_EQ(fit_within({640, 480}, 2048), (image::Size{640, 480}));  // already fits
    EXPECT_EQ(fit_within({10000, 3}, 100), (image::Size{100, 1}));     // at least one pixel
    EXPECT_EQ(fit_within({0, 0}, 100), (image::Size{0, 0}));
}

TEST(DownscaleArea, AveragesExactBlocksForIntegerRatios) {
    const auto source = make_image(8, 6, [](int x, int y, int c) { return 0.01 * (x + 10 * y) + 0.3 * c; });
    auto out = downscale_area(source, {4, 3});
    ASSERT_TRUE(out) << out.error().describe();
    EXPECT_EQ(out->encoding, source.encoding);
    for (std::int32_t y = 0; y < 3; ++y) {
        for (std::int32_t x = 0; x < 4; ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                // Independent computation: the mean of the 2 x 2 block.
                double expected = 0.0;
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        expected += source.pixels.at(2 * x + dx, 2 * y + dy, c);
                    }
                }
                EXPECT_NEAR(out->pixels.at(x, y, c), expected / 4.0, 1e-6);
            }
        }
    }
}

TEST(DownscaleArea, PreservesTheMeanForFractionalRatiosAndKeepsNegativeValues) {
    const auto source = make_image(97, 61, [](int x, int y, int c) {
        return std::sin(0.3 * x) * std::cos(0.2 * y) + (c == 2 ? -0.05 : 0.2);  // some values negative
    });
    auto out = downscale_area(source, {40, 23});
    ASSERT_TRUE(out) << out.error().describe();
    for (std::int32_t c = 0; c < 3; ++c) {
        // Area averaging preserves the integral of light exactly (float rounding aside).
        EXPECT_NEAR(mean(*out, c), mean(source, c), 1e-5);
    }
    // Constant input stays constant, including out-of-gamut negative values.
    const auto flat = make_image(33, 17, [](int, int, int c) { return c == 0 ? -0.25 : 0.75; });
    auto flat_out = downscale_area(flat, {10, 7});
    ASSERT_TRUE(flat_out);
    for (std::int32_t y = 0; y < 7; ++y) {
        for (std::int32_t x = 0; x < 10; ++x) {
            EXPECT_NEAR(flat_out->pixels.at(x, y, 0), -0.25f, 1e-6);
            EXPECT_NEAR(flat_out->pixels.at(x, y, 1), 0.75f, 1e-6);
        }
    }
}

TEST(DownscaleArea, ParallelAndSerialResultsAreIdentical) {
    const auto source =
        make_image(300, 200, [](int x, int y, int c) { return 0.001 * ((x * 7 + y * 13 + c) % 997); });
    auto parallel = downscale_area(source, {123, 77}, {}, true);
    auto serial = downscale_area(source, {123, 77}, {}, false);
    ASSERT_TRUE(parallel && serial);
    for (std::int32_t y = 0; y < 77; ++y) {
        for (std::int32_t i = 0; i < 123 * 3; ++i) {
            ASSERT_EQ(parallel->pixels.row(y)[i], serial->pixels.row(y)[i]);
        }
    }
}

TEST(DownscaleArea, RejectsInvalidInputAndHonoursCancellation) {
    const auto source = make_image(20, 10, [](int, int, int) { return 0.5; });
    EXPECT_EQ(downscale_area(source, {21, 10}).error().code, ErrorCode::InvalidArgument);  // enlarging
    EXPECT_EQ(downscale_area(source, {0, 5}).error().code, ErrorCode::InvalidArgument);
    engine::ColorImage encoded = make_image(4, 4, [](int, int, int) { return 0.5; });
    encoded.encoding = image::ColorEncoding::srgb_display();  // non-linear: averaging would be wrong
    EXPECT_EQ(downscale_area(encoded, {2, 2}).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(downscale_area(engine::ColorImage{}, {2, 2}).error().code, ErrorCode::InvalidArgument);
    core::CancellationSource cancel;
    cancel.cancel();
    EXPECT_EQ(downscale_area(source, {10, 5}, cancel.token()).error().code, ErrorCode::Cancelled);
}

TEST(EncodeDisplay, MatchesTheExportersSrgbConversion) {
    // Display-linear Rec.2020 neutrals map to sRGB neutrals: code = round(255 * sRGB_OETF(v)).
    const auto grey = make_image(3, 2, [](int x, int, int) { return x == 0 ? 0.0 : (x == 1 ? 0.5 : 1.0); });
    auto display = encode_display_srgb8(grey);
    ASSERT_TRUE(display) << display.error().describe();
    EXPECT_EQ(display->width, 3);
    EXPECT_EQ(display->height, 2);
    ASSERT_EQ(display->rgb.size(), 18u);
    const auto code = [](double linear) {
        const double encoded =
            linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
        return static_cast<int>(std::floor(encoded * 255.0 + 0.5));
    };
    EXPECT_EQ(display->rgb[0], code(0.0));
    EXPECT_NEAR(display->rgb[3], code(0.5), 1);  // Little CMS float pipeline vs the analytic curve
    EXPECT_EQ(display->rgb[4], display->rgb[3]);
    EXPECT_EQ(display->rgb[6], 255);
    EXPECT_EQ(DisplayImage::encoding(), image::ColorEncoding::srgb_display());

    engine::ColorImage scene = make_image(2, 2, [](int, int, int) { return 0.5; });
    scene.encoding = image::ColorEncoding::scene_linear_rec2020_d65();  // not display-referred
    EXPECT_EQ(encode_display_srgb8(scene).error().code, ErrorCode::InvalidArgument);
    engine::ColorImage camera = make_image(2, 2, [](int, int, int) { return 0.5; });
    camera.encoding = image::ColorEncoding::camera_native_linear();
    camera.encoding.reference = image::ImageReference::Display;
    EXPECT_EQ(encode_display_srgb8(camera).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(encode_display_srgb8(engine::ColorImage{}).error().code, ErrorCode::InvalidArgument);
}

// The direct conversion against exporter::encode_for_output (Little CMS) on a dense cube of
// display-linear Rec.2020 colours including out-of-gamut and above-white values, plus neutrals
// placed exactly on the quantisation thresholds. Tolerance: 1 code value, with at most 2 % of
// samples differing. The exporter evaluates the ICC profile's s15Fixed16-quantised colorants,
// adaptation matrix and curve parameters in float32, which moves values by about 1e-5 relative;
// samples within that distance of a quantisation boundary round the other way (measured: 1.19 %
// of this cube, which concentrates samples near boundaries more than photographs do).
TEST(EncodeDisplay, AgreesWithTheExportersConversion) {
    constexpr int kSteps = 48;
    const auto value = [](int i) { return -0.25 + 1.5 * i / (kSteps - 1); };
    auto cube = make_image(kSteps * kSteps, kSteps + 1, [&](int x, int y, int c) {
        if (y == kSteps) {  // last row: neutrals on the thresholds of every code value
            const double code = std::clamp(x % 256, 1, 255) - 0.5;
            const double v = image::srgb_decode(code / 255.0);
            return v;
        }
        const int r = x / kSteps;
        const int g = x % kSteps;
        return c == 0 ? value(r) : (c == 1 ? value(g) : value(y));
    });
    auto fast = encode_display_srgb8(cube);
    ASSERT_TRUE(fast) << fast.error().describe();
    auto reference = exporter::encode_for_output(cube.pixels.const_view(), cube.encoding,
                                                 exporter::OutputProfile::Srgb, exporter::BitDepth::Eight);
    ASSERT_TRUE(reference) << reference.error().describe();
    ASSERT_EQ(fast->rgb.size(), reference->samples8.size());
    std::size_t differing = 0;
    int worst = 0;
    for (std::size_t i = 0; i < fast->rgb.size(); ++i) {
        const int d = std::abs(int{fast->rgb[i]} - int{reference->samples8[i]});
        worst = std::max(worst, d);
        differing += d != 0 ? 1 : 0;
    }
    const double fraction = static_cast<double>(differing) / static_cast<double>(fast->rgb.size());
    std::printf("  direct vs Little CMS: %zu of %zu samples differ (%.4f %%), worst %d\n", differing,
                fast->rgb.size(), 100.0 * fraction, worst);
    EXPECT_LE(worst, 1);
    EXPECT_LT(fraction, 0.02);
}

// With sRGB primaries the matrix is the identity, so each code must be exactly the documented
// quantisation of the sRGB curve, floor(255 * srgb_encode(v) + 0.5), computed independently here.
TEST(EncodeDisplay, QuantisesTheSrgbCurveExactly) {
    constexpr int kWidth = 1000;
    constexpr int kSamples = kWidth * 200;
    engine::ColorImage ramp = make_image(
        kWidth, kSamples / kWidth, [](int x, int y, int) { return (y * kWidth + x) / double(kSamples - 1); });
    ramp.encoding = image::ColorEncoding::linear_srgb();
    ramp.encoding.reference = image::ImageReference::Display;
    auto display = encode_display_srgb8(ramp);
    ASSERT_TRUE(display) << display.error().describe();
    int checked = 0;
    for (int x = 0; x < kSamples; ++x) {
        const double v = static_cast<double>(static_cast<float>(x / double(kSamples - 1)));
        const double scaled = 255.0 * image::srgb_encode(v) + 0.5;
        if (std::abs(scaled - std::round(scaled)) < 1e-9) {
            continue;  // on a boundary to within the matrix's rounding: either answer is right
        }
        const int expected = std::clamp(static_cast<int>(std::floor(scaled)), 0, 255);
        ASSERT_EQ(display->rgb[static_cast<std::size_t>(x) * 3], expected) << "v = " << v;
        ++checked;
    }
    EXPECT_GT(checked, kSamples - 10);
}

TEST(EncodeDisplay, SerialParallelAndFallbackPaths) {
    const auto source = make_image(
        257, 131, [](int x, int y, int c) { return 0.004 * ((x * 3 + y * 5 + c * 7) % 300) - 0.1; });
    auto parallel = encode_display_srgb8(source, {}, true);
    auto serial = encode_display_srgb8(source, {}, false);
    ASSERT_TRUE(parallel && serial);
    EXPECT_EQ(parallel->rgb, serial->rgb);

    // A display-referred source with a non-linear transfer goes through the exporter itself.
    engine::ColorImage encoded = make_image(8, 4, [](int x, int, int) { return x / 7.0; });
    encoded.encoding = image::ColorEncoding::srgb_display();
    auto fallback = encode_display_srgb8(encoded);
    ASSERT_TRUE(fallback) << fallback.error().describe();
    auto reference = exporter::encode_for_output(encoded.pixels.const_view(), encoded.encoding,
                                                 exporter::OutputProfile::Srgb, exporter::BitDepth::Eight);
    ASSERT_TRUE(reference);
    EXPECT_EQ(fallback->rgb, reference->samples8);

    engine::ColorImage bad = make_image(5, 3, [](int, int, int) { return 0.5; });
    bad.pixels.at(3, 2, 1) = std::numeric_limits<float>::quiet_NaN();
    auto rejected = encode_display_srgb8(bad);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::InvalidArgument);
    EXPECT_NE(rejected.error().message.find("(3, 2)"), std::string::npos) << rejected.error().message;

    core::CancellationSource cancel;
    cancel.cancel();
    EXPECT_EQ(encode_display_srgb8(source, cancel.token()).error().code, ErrorCode::Cancelled);
}

TEST(Histogram, CountsCodeValuesAndLuma) {
    DisplayImage image;
    image.width = 3;
    image.height = 1;
    image.rgb = {255, 0, 0, 0, 255, 0, 10, 10, 10};
    const Histogram h = compute_histogram(image);
    EXPECT_EQ(h.pixels, 3u);
    EXPECT_EQ(h.red[255], 1u);
    EXPECT_EQ(h.red[0], 1u);
    EXPECT_EQ(h.red[10], 1u);
    EXPECT_EQ(h.green[255], 1u);
    EXPECT_EQ(h.blue[0], 2u);
    // Luma: round(0.2126 * 255) = 54, round(0.7152 * 255) = 182, grey 10 stays 10.
    EXPECT_EQ(h.luminance[54], 1u);
    EXPECT_EQ(h.luminance[182], 1u);
    EXPECT_EQ(h.luminance[10], 1u);
    EXPECT_EQ(std::accumulate(h.luminance.begin(), h.luminance.end(), 0u), 3u);
}

}  // namespace
}  // namespace lucent::app
