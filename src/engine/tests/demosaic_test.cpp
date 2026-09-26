// Unit tests for the demosaicing routes (demosaic.hpp) on synthetic data.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/core/cancellation.hpp>
#include <lucent/engine/demosaic.hpp>
#include <lucent/engine/sensor.hpp>

#include "sensor_test_support.hpp"

namespace lucent::engine {
namespace {

namespace ts = test_support;
using image::BayerLayout;
using image::CfaColor;
using image::CfaPattern;
using image::PixelRect;

constexpr std::array<BayerLayout, 4> kBayerLayouts = {BayerLayout::Rggb, BayerLayout::Bggr, BayerLayout::Grbg,
                                                      BayerLayout::Gbrg};

// The Fujifilm X-Trans tile (X-T4, visible origin).
CfaPattern xtrans_tile() {
    const std::array<CfaColor, 36> tile = {
        CfaColor::Green, CfaColor::Green, CfaColor::Red,   CfaColor::Green, CfaColor::Green, CfaColor::Blue,
        CfaColor::Green, CfaColor::Green, CfaColor::Blue,  CfaColor::Green, CfaColor::Green, CfaColor::Red,
        CfaColor::Blue,  CfaColor::Red,   CfaColor::Green, CfaColor::Red,   CfaColor::Blue,  CfaColor::Green,
        CfaColor::Green, CfaColor::Green, CfaColor::Blue,  CfaColor::Green, CfaColor::Green, CfaColor::Red,
        CfaColor::Green, CfaColor::Green, CfaColor::Red,   CfaColor::Green, CfaColor::Green, CfaColor::Blue,
        CfaColor::Red,   CfaColor::Blue,  CfaColor::Green, CfaColor::Blue,  CfaColor::Red,   CfaColor::Green};
    return *CfaPattern::create(6, 6, tile);
}

std::int32_t rgb_index(CfaColor color) {
    return color == CfaColor::Red ? 0 : (color == CfaColor::Blue ? 2 : 1);
}

image::ImageBuffer<float> make_image(std::int32_t width, std::int32_t height, std::int32_t channels) {
    auto buffer = image::ImageBuffer<float>::create(width, height, channels);
    EXPECT_TRUE(buffer);
    return std::move(*buffer);
}

// A mosaic whose sites hold colour[channel of the site]: a uniform colour field.
image::ImageBuffer<float> flat_mosaic(std::int32_t width, std::int32_t height, const CfaPattern& cfa,
                                      const std::array<float, 3>& colour) {
    auto mosaic = make_image(width, height, 1);
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            mosaic.at(x, y, 0) = colour[static_cast<std::size_t>(rgb_index(cfa.color_at(y, x)))];
        }
    }
    return mosaic;
}

image::ImageBuffer<float> random_mosaic(std::int32_t width, std::int32_t height, std::uint32_t seed) {
    std::mt19937 engine(seed);  // fixed seed: deterministic
    std::uniform_real_distribution<float> distribution(-0.05f, 1.2f);
    auto mosaic = make_image(width, height, 1);
    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            mosaic.at(x, y, 0) = distribution(engine);
        }
    }
    return mosaic;
}

// ---- Independent Malvar-He-Cutler reference: the paper's 5 x 5 kernels as matrices, correlated in
// double with whole-sample-symmetric borders, and colour-demosaicing's selection rules. -----------

using Kernel = std::array<std::array<double, 5>, 5>;
constexpr Kernel kGreenAtRb = {{{0, 0, -1, 0, 0}, {0, 0, 2, 0, 0}, {-1, 2, 4, 2, -1}, {0, 0, 2, 0, 0}, {0, 0, -1, 0, 0}}};
constexpr Kernel kRowKernel = {
    {{0, 0, 0.5, 0, 0}, {0, -1, 0, -1, 0}, {-1, 4, 5, 4, -1}, {0, -1, 0, -1, 0}, {0, 0, 0.5, 0, 0}}};
constexpr Kernel kDiagonal = {
    {{0, 0, -1.5, 0, 0}, {0, 2, 0, 2, 0}, {-1.5, 0, 6, 0, -1.5}, {0, 2, 0, 2, 0}, {0, 0, -1.5, 0, 0}}};

Kernel transpose(const Kernel& k) {
    Kernel t{};
    for (std::size_t r = 0; r < 5; ++r) {
        for (std::size_t c = 0; c < 5; ++c) {
            t[c][r] = k[r][c];
        }
    }
    return t;
}

std::int32_t reflect(std::int32_t i, std::int32_t n) { return i < 0 ? -i : (i >= n ? 2 * (n - 1) - i : i); }

double correlate(const image::ImageBuffer<float>& m, const Kernel& k, std::int32_t x, std::int32_t y) {
    double sum = 0.0;
    for (std::int32_t dy = -2; dy <= 2; ++dy) {
        for (std::int32_t dx = -2; dx <= 2; ++dx) {
            sum += k[static_cast<std::size_t>(dy + 2)][static_cast<std::size_t>(dx + 2)] *
                   m.at(reflect(x + dx, m.width()), reflect(y + dy, m.height()), 0);
        }
    }
    return sum / 8.0;
}

std::array<double, 3> reference_mhc(const image::ImageBuffer<float>& m, const CfaPattern& cfa, std::int32_t x,
                                    std::int32_t y) {
    const CfaColor colour = cfa.color_at(y, x);
    const bool red_row = cfa.color_at(y, 0) == CfaColor::Red || cfa.color_at(y, 1) == CfaColor::Red;
    const double centre = m.at(x, y, 0);
    const Kernel column_kernel = transpose(kRowKernel);
    if (colour == CfaColor::Red) {
        return {centre, correlate(m, kGreenAtRb, x, y), correlate(m, kDiagonal, x, y)};
    }
    if (colour == CfaColor::Blue) {
        return {correlate(m, kDiagonal, x, y), correlate(m, kGreenAtRb, x, y), centre};
    }
    const double row = correlate(m, kRowKernel, x, y);
    const double column = correlate(m, column_kernel, x, y);
    return red_row ? std::array<double, 3>{row, centre, column} : std::array<double, 3>{column, centre, row};
}

// ---------------------------------------------------------------------------------------------

TEST(MalvarHeCutler, UniformColourFieldsAreExactEverywhereInAllPhases) {
    const std::array<float, 3> colour = {0.25f, 0.5f, 0.75f};
    for (const BayerLayout layout : kBayerLayouts) {
        SCOPED_TRACE(image::to_string(layout));
        const CfaPattern cfa = CfaPattern::bayer(layout);
        for (const auto [w, h] : {std::pair{9, 7}, std::pair{3, 3}, std::pair{4, 5}}) {
            const auto mosaic = flat_mosaic(w, h, cfa, colour);
            auto rgb = make_image(w, h, 3);
            ASSERT_TRUE(demosaic_bayer_mhc(mosaic.const_view(), cfa, rgb.view()));
            for (std::int32_t y = 0; y < h; ++y) {
                for (std::int32_t x = 0; x < w; ++x) {
                    for (std::int32_t c = 0; c < 3; ++c) {
                        ASSERT_EQ(rgb.at(x, y, c), colour[static_cast<std::size_t>(c)])
                            << w << "x" << h << " at " << x << "," << y << " channel " << c;
                    }
                }
            }
        }
    }
}

TEST(MalvarHeCutler, MatchesTheKernelDefinitionIncludingBorders) {
    // Tolerance 1e-5: Lucent sums at most 13 float products with coefficients of magnitude <= 6/8;
    // for values in [-0.05, 1.2] the float32 rounding error is below about 2.5e-6.
    for (const BayerLayout layout : kBayerLayouts) {
        SCOPED_TRACE(image::to_string(layout));
        const CfaPattern cfa = CfaPattern::bayer(layout);
        const auto mosaic = random_mosaic(17, 12, 1234u + static_cast<std::uint32_t>(layout));
        auto rgb = make_image(17, 12, 3);
        ASSERT_TRUE(demosaic_bayer_mhc(mosaic.const_view(), cfa, rgb.view()));
        double worst = 0.0;
        for (std::int32_t y = 0; y < 12; ++y) {
            for (std::int32_t x = 0; x < 17; ++x) {
                const auto expected = reference_mhc(mosaic, cfa, x, y);
                for (std::int32_t c = 0; c < 3; ++c) {
                    worst = std::max(worst, std::abs(rgb.at(x, y, c) - expected[static_cast<std::size_t>(c)]));
                }
            }
        }
        EXPECT_LE(worst, 1e-5);
    }
}

TEST(MalvarHeCutler, ShiftedCropsMatchTheFullImageInTheirInterior) {
    const CfaPattern cfa = CfaPattern::bayer(BayerLayout::Rggb);
    const auto mosaic = random_mosaic(24, 20, 99u);
    auto full = make_image(24, 20, 3);
    ASSERT_TRUE(demosaic_bayer_mhc(mosaic.const_view(), cfa, full.view()));
    for (const auto [x0, y0] : {std::pair{0, 0}, std::pair{1, 0}, std::pair{0, 1}, std::pair{3, 5}}) {
        SCOPED_TRACE(testing::Message() << "crop at " << x0 << "," << y0);
        const PixelRect crop{x0, y0, 15, 11};
        auto view = mosaic.view(crop);
        ASSERT_TRUE(view);
        auto rgb = make_image(crop.width, crop.height, 3);
        ASSERT_TRUE(demosaic_bayer_mhc(*view, cfa.shifted(x0, y0), rgb.view()));
        for (std::int32_t y = 2; y < crop.height - 2; ++y) {
            for (std::int32_t x = 2; x < crop.width - 2; ++x) {
                for (std::int32_t c = 0; c < 3; ++c) {
                    ASSERT_EQ(rgb.at(x, y, c), full.at(x0 + x, y0 + y, c));
                }
            }
        }
    }
}

TEST(MalvarHeCutler, ReproducesLinearRampsInTheInterior) {
    // Each kernel sums to 1 and is symmetric, so a grey linear ramp is reproduced exactly (up to
    // float rounding) wherever the kernels stay inside the image.
    const CfaPattern cfa = CfaPattern::bayer(BayerLayout::Grbg);
    auto mosaic = make_image(16, 12, 1);
    for (std::int32_t y = 0; y < 12; ++y) {
        for (std::int32_t x = 0; x < 16; ++x) {
            mosaic.at(x, y, 0) = 0.1f + 0.01f * static_cast<float>(x) + 0.02f * static_cast<float>(y);
        }
    }
    auto rgb = make_image(16, 12, 3);
    ASSERT_TRUE(demosaic_bayer_mhc(mosaic.const_view(), cfa, rgb.view()));
    for (std::int32_t y = 2; y < 10; ++y) {
        for (std::int32_t x = 2; x < 14; ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                EXPECT_NEAR(rgb.at(x, y, c), mosaic.at(x, y, 0), 1e-5);
            }
        }
    }
}

TEST(MalvarHeCutler, SerialAndParallelAreIdenticalAndCancellationStops) {
    const CfaPattern cfa = CfaPattern::bayer(BayerLayout::Bggr);
    const auto mosaic = random_mosaic(41, 37, 7u);
    auto a = make_image(41, 37, 3);
    auto b = make_image(41, 37, 3);
    ASSERT_TRUE(demosaic_bayer_mhc(mosaic.const_view(), cfa, a.view(), {}, true));
    ASSERT_TRUE(demosaic_bayer_mhc(mosaic.const_view(), cfa, b.view(), {}, false));
    for (std::int32_t y = 0; y < 37; ++y) {
        ASSERT_TRUE(std::equal(a.row(y), a.row(y) + 41 * 3, b.row(y)));
    }
    core::CancellationSource source;
    source.cancel();
    ts::expect_error(demosaic_bayer_mhc(mosaic.const_view(), cfa, a.view(), source.token()), ErrorCode::Cancelled,
                     "engine.demosaic.run");
}

TEST(MalvarHeCutler, RejectsInvalidInput) {
    const CfaPattern cfa = CfaPattern::bayer(BayerLayout::Rggb);
    const auto mosaic = random_mosaic(8, 8, 1u);
    auto rgb = make_image(8, 8, 3);
    ts::expect_error(demosaic_bayer_mhc(mosaic.const_view(), xtrans_tile(), rgb.view()), ErrorCode::InvalidArgument,
                     "engine.demosaic.input");
    ts::expect_error(demosaic_bayer_mhc(mosaic.const_view(), CfaPattern{}, rgb.view()), ErrorCode::InvalidArgument,
                     "engine.demosaic.input");
    const auto tiny = random_mosaic(2, 8, 1u);
    auto tiny_rgb = make_image(2, 8, 3);
    ts::expect_error(demosaic_bayer_mhc(tiny.const_view(), cfa, tiny_rgb.view()), ErrorCode::InvalidArgument,
                     "engine.demosaic.input");
    auto wrong = make_image(8, 7, 3);
    ts::expect_error(demosaic_bayer_mhc(mosaic.const_view(), cfa, wrong.view()), ErrorCode::InvalidArgument,
                     "engine.demosaic.input");
    auto mono = make_image(8, 8, 1);
    ts::expect_error(demosaic_bayer_mhc(mosaic.const_view(), cfa, mono.view()), ErrorCode::InvalidArgument,
                     "engine.demosaic.input");
}

// ---- X-Trans baseline ----------------------------------------------------------------------------

TEST(XTransBaseline, UniformColourFieldsAreExactEverywhereForEveryTileShift) {
    const std::array<float, 3> colour = {0.25f, 0.5f, 0.75f};
    for (std::int32_t shift = 0; shift < 6; ++shift) {
        const CfaPattern cfa = xtrans_tile().shifted(shift, (shift * 5) % 6);
        ASSERT_TRUE(cfa.is_xtrans());
        const auto mosaic = flat_mosaic(13, 11, cfa, colour);
        auto rgb = make_image(13, 11, 3);
        ASSERT_TRUE(demosaic_xtrans_baseline(mosaic.const_view(), cfa, rgb.view()));
        for (std::int32_t y = 0; y < 11; ++y) {
            for (std::int32_t x = 0; x < 13; ++x) {
                for (std::int32_t c = 0; c < 3; ++c) {
                    ASSERT_EQ(rgb.at(x, y, c), colour[static_cast<std::size_t>(c)]) << x << "," << y << " " << c;
                }
            }
        }
    }
}

TEST(XTransBaseline, MissingColoursAreSameColourMeansOfTheWindow) {
    const CfaPattern cfa = xtrans_tile().shifted(2, 1);
    const auto mosaic = random_mosaic(14, 12, 5u);
    auto rgb = make_image(14, 12, 3);
    ASSERT_TRUE(demosaic_xtrans_baseline(mosaic.const_view(), cfa, rgb.view()));
    // Brute force: smallest window (clipped to the image) holding the colour, row-major sums.
    const auto expected_at = [&](std::int32_t x, std::int32_t y, std::int32_t channel) {
        for (std::int32_t radius = 1; radius <= 5; ++radius) {
            float sum = 0.0f;
            std::int32_t count = 0;
            for (std::int32_t yy = std::max(0, y - radius); yy <= std::min(11, y + radius); ++yy) {
                for (std::int32_t xx = std::max(0, x - radius); xx <= std::min(13, x + radius); ++xx) {
                    if (rgb_index(cfa.color_at(yy, xx)) == channel) {
                        sum += mosaic.at(xx, yy, 0);
                        ++count;
                    }
                }
            }
            if (count > 0) {
                return sum / static_cast<float>(count);
            }
        }
        return 0.0f;
    };
    for (std::int32_t y = 0; y < 12; ++y) {
        for (std::int32_t x = 0; x < 14; ++x) {
            const std::int32_t own = rgb_index(cfa.color_at(y, x));
            for (std::int32_t c = 0; c < 3; ++c) {
                const float expected = c == own ? mosaic.at(x, y, 0) : expected_at(x, y, c);
                ASSERT_FLOAT_EQ(rgb.at(x, y, c), expected) << x << "," << y << " " << c;
            }
        }
    }
}

TEST(XTransBaseline, RejectsInvalidInput) {
    const auto mosaic = random_mosaic(8, 8, 1u);
    auto rgb = make_image(8, 8, 3);
    ts::expect_error(demosaic_xtrans_baseline(mosaic.const_view(), CfaPattern::bayer(BayerLayout::Rggb), rgb.view()),
                     ErrorCode::InvalidArgument, "engine.demosaic.input");
    const auto small = random_mosaic(5, 8, 1u);
    auto small_rgb = make_image(5, 8, 3);
    ts::expect_error(demosaic_xtrans_baseline(small.const_view(), xtrans_tile(), small_rgb.view()),
                     ErrorCode::InvalidArgument, "engine.demosaic.input");
}

// ---- Routes ----------------------------------------------------------------------------------------

double varied(std::int32_t x, std::int32_t y, std::int32_t s) {
    return static_cast<double>(200 + (x * 37 + y * 101 + s * 1009 + x * y) % 3500);
}

SensorImage sensor_from(const raw::DecodedRaw& decoded) {
    auto image = normalize_sensor(decoded);
    EXPECT_TRUE(image) << image.error().describe();
    return std::move(*image);
}

TEST(DemosaicRoutes, EachLayoutGetsItsOwnRoute) {
    const raw::SensorLevels levels = ts::simple_levels(64.0, 4095.0);
    const SensorImage bayer =
        sensor_from(ts::make_mosaic(8, 8, PixelRect{0, 0, 8, 8}, CfaPattern::bayer(BayerLayout::Gbrg), levels, varied));
    EXPECT_EQ(*select_demosaic_route(bayer), DemosaicRoute::BayerMalvarHeCutler);
    const SensorImage xtrans = sensor_from(
        ts::make_mosaic(12, 12, PixelRect{0, 0, 12, 12}, xtrans_tile(), levels, varied, ts::kRgbChannels));
    EXPECT_EQ(*select_demosaic_route(xtrans), DemosaicRoute::XTransBaseline);
    const SensorImage linear = sensor_from(ts::make_linear(4, 4, PixelRect{0, 0, 4, 4}, levels, varied));
    EXPECT_EQ(*select_demosaic_route(linear), DemosaicRoute::LinearColorBypass);
    const SensorImage mono = sensor_from(ts::make_monochrome(4, 4, PixelRect{0, 0, 4, 4}, levels, varied));
    EXPECT_EQ(*select_demosaic_route(mono), DemosaicRoute::MonochromeBypass);

    // An RGB mosaic that is neither Bayer nor X-Trans has no route.
    const std::array<CfaColor, 4> stripes = {CfaColor::Red, CfaColor::Green, CfaColor::Blue, CfaColor::Green};
    auto striped = CfaPattern::create(4, 1, stripes);
    ASSERT_TRUE(striped);
    const SensorImage odd = sensor_from(
        ts::make_mosaic(8, 8, PixelRect{0, 0, 8, 8}, *striped, levels, varied, ts::kRgbChannels));
    ts::expect_error(select_demosaic_route(odd), ErrorCode::Unsupported, "engine.demosaic.route");
    ts::expect_error(demosaic(odd), ErrorCode::Unsupported, "engine.demosaic.route");

    EXPECT_EQ(to_string(DemosaicRoute::XTransBaseline), "xtrans_baseline");
    EXPECT_EQ(to_string(RouteMaturity::Baseline), "baseline");
    EXPECT_EQ(static_cast<int>(DemosaicRoute::MonochromeBypass), 4);
}

TEST(DemosaicRoutes, BayerResultCarriesItsProvenance) {
    const auto decoded = ts::make_mosaic(12, 10, PixelRect{2, 1, 9, 8}, CfaPattern::bayer(BayerLayout::Grbg),
                                         ts::simple_levels(64.0, 4095.0), varied);
    SensorImage sensor = sensor_from(decoded);
    raw::RawMetadata metadata = decoded.metadata;
    metadata.color.as_shot_multipliers = {2.0f, 1.0f, 1.5f, 1.0f};
    ASSERT_TRUE(white_balance_sensor(sensor, metadata));
    auto result = demosaic(sensor);
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_EQ(result->demosaic.route, DemosaicRoute::BayerMalvarHeCutler);
    EXPECT_EQ(result->demosaic.maturity, RouteMaturity::Validated);
    EXPECT_EQ(result->demosaic.algorithm, "malvar-he-cutler-2004");
    EXPECT_EQ(result->demosaic.algorithm_version, kMalvarHeCutlerVersion);
    EXPECT_FALSE(result->demosaic.border_handling.empty());
    EXPECT_EQ(result->demosaic.cfa, CfaPattern::bayer(BayerLayout::Grbg));
    EXPECT_EQ(result->encoding, image::ColorEncoding::camera_native_linear());
    EXPECT_EQ(result->pixels.width(), 9);
    EXPECT_EQ(result->pixels.height(), 8);
    EXPECT_EQ(result->pixels.channels(), 3);
    EXPECT_EQ(result->normalization.source_area, (PixelRect{2, 1, 9, 8}));
    ASSERT_TRUE(result->white_balance);
    EXPECT_EQ(result->white_balance->applied.source, WhiteBalanceSource::AsShot);
    EXPECT_EQ(result->saturation.width(), 9);
    // The const overload leaves the sensor image intact and equals a direct kernel call.
    EXPECT_FALSE(sensor.pixels.empty());
    auto direct = make_image(9, 8, 3);
    ASSERT_TRUE(demosaic_bayer_mhc(sensor.pixels.const_view(), sensor.cfa, direct.view()));
    for (std::int32_t y = 0; y < 8; ++y) {
        ASSERT_TRUE(std::equal(direct.row(y), direct.row(y) + 27, result->pixels.row(y)));
    }
    // The consuming overload releases the mosaic.
    auto consumed = demosaic(std::move(sensor));
    ASSERT_TRUE(consumed);
    EXPECT_TRUE(sensor.pixels.empty());  // NOLINT(bugprone-use-after-move): documented behaviour
}

TEST(DemosaicRoutes, XTransResultIsMarkedBaseline) {
    const SensorImage sensor = sensor_from(ts::make_mosaic(13, 12, PixelRect{1, 0, 12, 12}, xtrans_tile().shifted(1, 0),
                                                           ts::simple_levels(0.0, 4095.0), varied, ts::kRgbChannels));
    auto result = demosaic(sensor);
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_EQ(result->demosaic.route, DemosaicRoute::XTransBaseline);
    EXPECT_EQ(result->demosaic.maturity, RouteMaturity::Baseline);
    EXPECT_EQ(result->demosaic.algorithm, "xtrans-3x3-same-colour-mean");
    EXPECT_EQ(result->pixels.channels(), 3);
}

TEST(DemosaicRoutes, BypassRoutesPassSamplesThroughAndMoveWhenConsumed) {
    const raw::SensorLevels levels = ts::simple_levels(0.0, 4095.0);
    SensorImage linear = sensor_from(ts::make_linear(5, 4, PixelRect{0, 0, 5, 4}, levels, varied));
    auto copied = demosaic(linear);
    ASSERT_TRUE(copied);
    EXPECT_EQ(copied->demosaic.route, DemosaicRoute::LinearColorBypass);
    EXPECT_EQ(copied->demosaic.maturity, RouteMaturity::Bypass);
    EXPECT_TRUE(copied->demosaic.cfa.empty());
    EXPECT_EQ(copied->encoding, image::ColorEncoding::camera_native_linear());
    EXPECT_NE(copied->pixels.data(), linear.pixels.data());
    for (std::int32_t y = 0; y < 4; ++y) {
        ASSERT_TRUE(std::equal(linear.pixels.row(y), linear.pixels.row(y) + 15, copied->pixels.row(y)));
    }
    const float* storage = linear.pixels.data();
    auto moved = demosaic(std::move(linear));
    ASSERT_TRUE(moved);
    EXPECT_EQ(moved->pixels.data(), storage);  // no copy

    SensorImage mono = sensor_from(ts::make_monochrome(4, 4, PixelRect{0, 0, 4, 4}, levels, varied));
    auto mono_result = demosaic(std::move(mono));
    ASSERT_TRUE(mono_result);
    EXPECT_EQ(mono_result->demosaic.route, DemosaicRoute::MonochromeBypass);
    EXPECT_EQ(mono_result->pixels.channels(), 1);
    EXPECT_EQ(mono_result->encoding, raw::camera_native_monochrome());
    EXPECT_EQ(mono_result->encoding.layout, image::ChannelLayout::Mono);
}

TEST(DemosaicRoutes, ClippedHighlightsStayNeutralThroughReconstruction) {
    // A saturated region (every channel at white) next to a mid-grey one, with strong red/blue
    // multipliers: after the highlight policy and MHC the saturated interior is exactly neutral.
    const auto decoded = ts::make_mosaic(16, 12, PixelRect{0, 0, 16, 12}, CfaPattern::bayer(BayerLayout::Rggb),
                                         ts::simple_levels(0.0, 1000.0),
                                         [](std::int32_t x, std::int32_t, std::int32_t) { return x < 8 ? 1000.0 : 300.0; });
    SensorImage sensor = sensor_from(decoded);
    WhiteBalance wb;
    wb.multipliers = {2.2, 1.0, 1.7, 1.0};
    ASSERT_TRUE(apply_white_balance(sensor, wb));
    auto result = demosaic(std::move(sensor));
    ASSERT_TRUE(result);
    for (std::int32_t y = 0; y < 12; ++y) {
        for (std::int32_t x = 0; x < 6; ++x) {
            EXPECT_EQ(result->pixels.at(x, y, 0), 1.0f);
            EXPECT_EQ(result->pixels.at(x, y, 1), 1.0f);
            EXPECT_EQ(result->pixels.at(x, y, 2), 1.0f);
        }
    }
    EXPECT_EQ(result->saturation.at(0, 0), 1u << 0);
}

TEST(DemosaicRoutes, EmptyInputIsRejected) {
    SensorImage empty;
    empty.representation = raw::RawRepresentation::LinearColor;
    ts::expect_error(demosaic(empty), ErrorCode::InvalidArgument, "engine.demosaic.input");
    SensorImage mosaic_without_cfa;
    ts::expect_error(demosaic(mosaic_without_cfa), ErrorCode::InvalidArgument, "engine.demosaic.route");
}

}  // namespace
}  // namespace lucent::engine
