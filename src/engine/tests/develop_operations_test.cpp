// Unit tests for the per-pixel develop operations (develop_operations.hpp): base look, local
// highlights/shadows (including halos on step edges), the display-lightness curves and
// saturation/vibrance, with identity, range, hue, neutral-stability, finiteness, determinism and
// failure-path checks.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/core/cancellation.hpp>
#include <lucent/engine/develop_operations.hpp>
#include <lucent/engine/display_rendering.hpp>

#include "develop_test_support.hpp"

namespace lucent::engine {
namespace {

namespace ts = test_support;
using Rgb = std::array<float, 3>;

ColorImage scene_image(std::int32_t w, std::int32_t h, const std::function<Rgb(std::int32_t, std::int32_t)>& f) {
    ColorImage image;
    image.pixels = *image::ImageBuffer<float>::create(w, h, 3);
    image.encoding = image::ColorEncoding::scene_linear_rec2020_d65();
    for (std::int32_t y = 0; y < h; ++y) {
        for (std::int32_t x = 0; x < w; ++x) {
            const Rgb v = f(x, y);
            for (std::int32_t c = 0; c < 3; ++c) {
                image.pixels.at(x, y, c) = v[static_cast<std::size_t>(c)];
            }
        }
    }
    return image;
}

ColorImage clone(const ColorImage& image) {
    return ColorImage{*image.pixels.clone(), image.encoding};
}

float luminance(const ColorImage& image, std::int32_t x, std::int32_t y) {
    return static_cast<float>(kRec2020Luminance[0] * image.pixels.at(x, y, 0) +
                              kRec2020Luminance[1] * image.pixels.at(x, y, 1) +
                              kRec2020Luminance[2] * image.pixels.at(x, y, 2));
}

// ---- Base look ------------------------------------------------------------------------------------

TEST(BaseLook, RendersTheNormWithTheCurveAndKeepsChannelRatios) {
    auto curve = NeutralV1Curve::create(1.0);
    ASSERT_TRUE(curve);
    const std::vector<Rgb> values = {{0.09f, 0.09f, 0.09f}, {0.4f, 0.2f, 0.1f},  {2.0f, 1.0f, 0.5f},
                                     {0.5f, -0.1f, 0.2f},   {-0.2f, -0.1f, -0.3f}, {0.0f, 0.0f, 0.0f}};
    ColorImage image = scene_image(static_cast<std::int32_t>(values.size()), 1,
                                   [&](std::int32_t x, std::int32_t) { return values[static_cast<std::size_t>(x)]; });
    auto record = apply_base_look(image, *curve);
    ASSERT_TRUE(record) << record.error().describe();
    EXPECT_EQ(image.encoding, display_linear_rec2020());
    EXPECT_EQ(record->limited_pixels, 1u);  // the 2.0 pixel
    EXPECT_EQ(record->negative_samples, 4u);
    EXPECT_EQ(record->regime, NeutralV1Regime::Shoulder);
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto x = static_cast<std::int32_t>(i);
        const Rgb& in = values[i];
        const float m = std::max({in[0], in[1], in[2]});
        if (m <= 0.0f) {
            for (std::int32_t c = 0; c < 3; ++c) {
                EXPECT_EQ(image.pixels.at(x, 0, c), in[static_cast<std::size_t>(c)]);
            }
            continue;
        }
        const double scale = curve->evaluate(m) / m;
        for (std::int32_t c = 0; c < 3; ++c) {
            EXPECT_NEAR(image.pixels.at(x, 0, c), in[static_cast<std::size_t>(c)] * scale, 1e-6) << i;
        }
        EXPECT_LE(std::max({image.pixels.at(x, 0, 0), image.pixels.at(x, 0, 1), image.pixels.at(x, 0, 2)}),
                  1.0f + 1e-6f);
    }
    ColorImage wrong = scene_image(2, 2, [](std::int32_t, std::int32_t) { return Rgb{0.1f, 0.1f, 0.1f}; });
    wrong.encoding = display_linear_rec2020();
    ts::expect_error(apply_base_look(wrong, *curve), ErrorCode::InvalidArgument, "engine.develop.base_look");
}

// ---- Local tone -------------------------------------------------------------------------------------

LocalToneParameters local(double h, double s, std::int32_t radius = 4) {
    LocalToneParameters p;
    p.highlights = h;
    p.shadows = s;
    p.white = 1.0;
    p.radius = radius;
    return p;
}

TEST(LocalTone, FlatRegionsGetTheirFullGainAndKeepRatios) {
    const Rgb dark{0.004f, 0.003f, 0.002f};   // about -8 EV: full shadow weight
    const Rgb bright{1.0f, 0.95f, 0.9f};      // at white: full highlight weight
    const Rgb grey{0.125f, 0.125f, 0.125f};   // metered grey: untouched by shadows
    for (const auto& [value, h, s, expected_ev] :
         std::vector<std::tuple<Rgb, double, double, double>>{{dark, 0.0, 1.0, kShadowsMaxEv},
                                                              {dark, 0.0, -0.5, -0.5 * kShadowsMaxEv},
                                                              {bright, -1.0, 0.0, -kHighlightsMaxEv},
                                                              {bright, 0.5, 0.0, 0.5 * kHighlightsMaxEv},
                                                              {grey, 0.0, 1.0, 0.0}}) {
        ColorImage image = scene_image(24, 18, [&](std::int32_t, std::int32_t) { return value; });
        auto record = apply_local_tone(image, local(h, s));
        ASSERT_TRUE(record) << record.error().describe();
        const double gain = std::exp2(expected_ev);
        for (std::int32_t y = 0; y < 18; y += 5) {
            for (std::int32_t x = 0; x < 24; x += 5) {
                for (std::int32_t c = 0; c < 3; ++c) {
                    EXPECT_NEAR(image.pixels.at(x, y, c), value[static_cast<std::size_t>(c)] * gain,
                                1e-4 * value[static_cast<std::size_t>(c)] * gain)
                        << h << " " << s;
                }
            }
        }
        EXPECT_EQ(record->radius, 4);
        EXPECT_EQ(record->epsilon, kLocalToneEpsilon);
    }
}

TEST(LocalTone, StepEdgesGetNoHalo) {
    // Dark mountain (-7 EV) against a bright sky (-0.3 EV), both with fine texture.
    const std::int32_t w = 160, h = 40, edge = 80, r = 12;
    ColorImage image = scene_image(w, h, [&](std::int32_t x, std::int32_t y) {
        const float base = x < edge ? 0.0078f : 0.81f;
        const float t = 1.0f + 0.04f * static_cast<float>(((x * 7 + y * 3) % 5) - 2);
        return Rgb{base * t, base * t * 0.97f, base * t * 0.9f};
    });
    const ColorImage original = clone(image);
    auto record = apply_local_tone(image, local(-1.0, 1.0, r));
    ASSERT_TRUE(record) << record.error().describe();
    // Gain per pixel (EV), from the green channel.
    const auto gain = [&](std::int32_t x, std::int32_t y) {
        return std::log2(image.pixels.at(x, y, 1) / original.pixels.at(x, y, 1));
    };
    const double dark_plateau = gain(10, 20);
    const double bright_plateau = gain(w - 10, 20);
    EXPECT_NEAR(dark_plateau, kShadowsMaxEv, 0.01);
    EXPECT_NEAR(bright_plateau, -kHighlightsMaxEv, 0.01);  // display EV estimate 0: full weight
    for (std::int32_t y = 0; y < h; ++y) {
        for (std::int32_t x = 0; x < w; ++x) {
            const double g = gain(x, y);
            if (x < edge) {
                // No darkening halo on the dark side next to the bright sky: within 2 % of the plateau.
                EXPECT_NEAR(g, dark_plateau, 0.03) << x << "," << y;
            } else {
                // No brightening halo on the bright side next to the dark mountain.
                EXPECT_NEAR(g, bright_plateau, 0.03) << x << "," << y;
            }
            for (std::int32_t c = 0; c < 3; ++c) {
                ASSERT_TRUE(std::isfinite(image.pixels.at(x, y, c)));
            }
        }
    }
    // Texture survives: the ratio between neighbours is unchanged inside each plateau.
    EXPECT_NEAR(image.pixels.at(30, 10, 1) / image.pixels.at(31, 10, 1),
                original.pixels.at(30, 10, 1) / original.pixels.at(31, 10, 1), 1e-3);
}

TEST(LocalTone, ScaledRadiusOnAProxyAgreesWithFullResolution) {
    // A smooth gradient with a step, at full size and at 1/4 size with the radius scaled by 1/4.
    const auto scene = [](double u, double v) {
        const double ev = -9.0 + 8.5 * u + (v > 0.5 ? 1.0 : 0.0);
        const double y = std::exp2(ev);
        return Rgb{static_cast<float>(y), static_cast<float>(y), static_cast<float>(y)};
    };
    const std::int32_t w = 256, h = 128;
    ColorImage full = scene_image(w, h, [&](std::int32_t x, std::int32_t y) {
        return scene((x + 0.5) / w, (y + 0.5) / h);
    });
    ColorImage small = scene_image(w / 4, h / 4, [&](std::int32_t x, std::int32_t y) {
        return scene((x + 0.5) / (w / 4), (y + 0.5) / (h / 4));
    });
    ASSERT_TRUE(apply_local_tone(full, local(-0.8, 0.8, 16)));
    ASSERT_TRUE(apply_local_tone(small, local(-0.8, 0.8, 4)));
    double worst = 0.0;
    for (std::int32_t y = 2; y < h / 4 - 2; ++y) {
        for (std::int32_t x = 2; x < w / 4 - 2; ++x) {
            const double a = std::log2(small.pixels.at(x, y, 1));
            const double b = std::log2(full.pixels.at(4 * x + 2, 4 * y + 2, 1));
            if (std::abs(4 * y + 2 - h / 2) > 8) {  // away from the step, where sampling differs
                worst = std::max(worst, std::abs(a - b));
            }
        }
    }
    EXPECT_LT(worst, 0.05);  // EV
}

TEST(LocalTone, IsDeterministicScheduleIndependentAndChecksItsInput) {
    ColorImage a = scene_image(97, 61, [](std::int32_t x, std::int32_t y) {
        const float v = std::exp2(-10.0f + 0.1f * static_cast<float>(x) + 0.05f * static_cast<float>(y % 7));
        return Rgb{v, 0.8f * v, 0.6f * v};
    });
    ColorImage b = clone(a);
    ColorImage c = clone(a);
    ASSERT_TRUE(apply_local_tone(a, local(-0.7, 0.6, 5), {}, {}, true));
    ASSERT_TRUE(apply_local_tone(b, local(-0.7, 0.6, 5), {}, {}, true));
    ASSERT_TRUE(apply_local_tone(c, local(-0.7, 0.6, 5), {}, {}, false));
    EXPECT_TRUE(ts::bit_identical(a.pixels, b.pixels));
    EXPECT_TRUE(ts::bit_identical(a.pixels, c.pixels));

    ColorImage zero = clone(b);
    auto identity = apply_local_tone(zero, local(0.0, 0.0, 5));
    ASSERT_TRUE(identity);
    EXPECT_TRUE(ts::bit_identical(zero.pixels, b.pixels));
    EXPECT_EQ(identity->min_gain_ev, 0.0);
    EXPECT_EQ(identity->max_gain_ev, 0.0);

    constexpr const char* kWhere = "engine.develop.local_tone";
    ts::expect_error(apply_local_tone(a, local(1.5, 0.0)), ErrorCode::InvalidArgument, kWhere);
    ts::expect_error(apply_local_tone(a, local(0.0, 0.0, 0)), ErrorCode::InvalidArgument, kWhere);
    LocalToneParameters bad = local(0.5, 0.5);
    bad.white = 0.0;
    ts::expect_error(apply_local_tone(a, bad), ErrorCode::InvalidArgument, kWhere);
    ColorImage display = clone(a);
    display.encoding = display_linear_rec2020();
    ts::expect_error(apply_local_tone(display, local(0.5, 0.5)), ErrorCode::InvalidArgument, kWhere);
    core::CancellationSource source;
    source.cancel();
    auto cancelled = apply_local_tone(a, local(0.5, 0.5), {}, source.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::Cancelled);
}

// ---- Lightness curves -----------------------------------------------------------------------------

TEST(LightnessCurves, DisplayedLightnessOfNeutralsFollowsTheToneCurveExactly) {
    ToneCurveSetting setting;
    setting.points = {{0.0, 0.08}, {0.3, 0.22}, {0.6, 0.7}, {1.0, 0.96}};
    auto tone = ToneCurve::create(setting);
    ASSERT_TRUE(tone);
    for (const double white : {0.5, 1.0, 3.0}) {
        auto base = NeutralV1Curve::create(white);
        ASSERT_TRUE(base);
        const std::int32_t n = 200;
        const auto value = [&](std::int32_t x) {
            return static_cast<float>(white * std::exp2(-12.0 + 12.5 * x / (n - 1.0)));  // to above white
        };
        ColorImage image = scene_image(n, 1, [&](std::int32_t x, std::int32_t) {
            const float v = value(x);
            return Rgb{v, v, v};
        });
        ASSERT_TRUE(apply_lightness_curves(image, *base, nullptr, &*tone));
        auto rendered = clone(image);
        ASSERT_TRUE(apply_base_look(rendered, *base));
        for (std::int32_t x = 0; x < n; ++x) {
            const double before = display_lightness(base->evaluate(value(x)));
            const double after = display_lightness(rendered.pixels.at(x, 0, 1));
            // Tabulated chain (relative error below 1e-5) and float pixels.
            EXPECT_NEAR(after, tone->evaluate(before), 5e-6) << white << " at " << x;
            EXPECT_EQ(image.pixels.at(x, 0, 0), image.pixels.at(x, 0, 1));
            EXPECT_EQ(image.pixels.at(x, 0, 1), image.pixels.at(x, 0, 2));
        }
    }
}

TEST(LightnessCurves, TabulationMatchesTheExactChain) {
    ToneCurveSetting setting;
    setting.points = {{0.0, 0.0}, {0.2, 0.12}, {0.5, 0.58}, {0.8, 0.9}, {1.0, 1.0}};
    auto tone = ToneCurve::create(setting);
    for (const double c : {-1.0, 0.0, 0.6, 1.0}) {
        auto contrast = ContrastCurve::create(c);
        ASSERT_TRUE(tone && contrast);
        for (const double white : {0.3, 1.0, 5.0}) {
            auto base = NeutralV1Curve::create(white);
            ASSERT_TRUE(base);
            const std::int32_t n = 4000;
            const auto value = [&](std::int32_t x) {
                return static_cast<float>(white * std::exp2(-25.0 + 25.0 * x / (n - 1.0)));
            };
            ColorImage image = scene_image(n, 1, [&](std::int32_t x, std::int32_t) {
                const float v = value(x);
                return Rgb{v, 0.5f * v, 0.25f * v};
            });
            ASSERT_TRUE(apply_lightness_curves(image, *base, c == 0.0 ? nullptr : &*contrast, &*tone));
            double worst = 0.0;
            for (std::int32_t x = 0; x < n; ++x) {
                const double m = value(x);
                double l = display_lightness(base->evaluate(m));
                l = tone->evaluate(c == 0.0 ? l : contrast->evaluate(l));
                const double exact = base->inverse(display_lightness_inverse(l));
                worst = std::max(worst, std::abs(image.pixels.at(x, 0, 0) / exact - 1.0));
            }
            // Measured worst case 4.1e-5 (contrast -1 with a lift-regime white point).
            EXPECT_LT(worst, 1e-4) << "contrast " << c << ", white " << white;
        }
    }
}

TEST(LightnessCurves, ContrastKeepsBlackWhiteAndHue) {
    auto base = NeutralV1Curve::create(1.0);
    auto contrast = ContrastCurve::create(0.7);
    ASSERT_TRUE(base && contrast);
    ColorImage image = scene_image(4, 1, [](std::int32_t x, std::int32_t) {
        switch (x) {
            case 0:
                return Rgb{0.0f, 0.0f, 0.0f};
            case 1:
                return Rgb{1.0f, 1.0f, 1.0f};  // white point
            case 2:
                return Rgb{0.3f, 0.12f, 0.05f};
            default:
                return Rgb{0.05f, 0.04f, 0.02f};
        }
    });
    const ColorImage original = clone(image);
    auto record = apply_lightness_curves(image, *base, &*contrast, nullptr);
    ASSERT_TRUE(record);
    EXPECT_EQ(image.pixels.at(0, 0, 0), 0.0f);
    EXPECT_NEAR(image.pixels.at(1, 0, 1), 1.0f, 1e-6f);
    for (std::int32_t x = 2; x < 4; ++x) {
        EXPECT_NEAR(image.pixels.at(x, 0, 0) / image.pixels.at(x, 0, 1),
                    original.pixels.at(x, 0, 0) / original.pixels.at(x, 0, 1), 1e-5);
        EXPECT_NEAR(image.pixels.at(x, 0, 2) / image.pixels.at(x, 0, 1),
                    original.pixels.at(x, 0, 2) / original.pixels.at(x, 0, 1), 1e-5);
    }
    // Brighter than the pivot gets brighter, darker gets darker.
    EXPECT_GT(image.pixels.at(2, 0, 0), original.pixels.at(2, 0, 0));
    EXPECT_LT(image.pixels.at(3, 0, 0), original.pixels.at(3, 0, 0));
    EXPECT_GE(record->changed_pixels, 2u);
}

// ---- Saturation and vibrance ----------------------------------------------------------------------

TEST(ColorAdjust, NeutralsStayExactlyNeutralAndLuminanceIsKept) {
    const std::vector<Rgb> values = {{0.2f, 0.2f, 0.2f}, {1.3f, 1.3f, 1.3f}, {0.0f, 0.0f, 0.0f},
                                     {0.4f, 0.2f, 0.1f}, {0.1f, 0.3f, 0.6f}, {0.5f, -0.05f, 0.1f}};
    for (const auto& [sat, vib] : std::vector<std::pair<double, double>>{{1.0, 0.0}, {-1.0, 0.0}, {0.0, 1.0},
                                                                         {0.0, -1.0}, {0.6, 0.6}, {-0.3, 0.9}}) {
        ColorImage image = scene_image(static_cast<std::int32_t>(values.size()), 1, [&](std::int32_t x, std::int32_t) {
            return values[static_cast<std::size_t>(x)];
        });
        const ColorImage original = clone(image);
        ASSERT_TRUE(apply_color(image, sat, vib));
        for (std::int32_t x = 0; x < 2; ++x) {  // neutrals
            EXPECT_EQ(image.pixels.at(x, 0, 0), image.pixels.at(x, 0, 1));
            EXPECT_EQ(image.pixels.at(x, 0, 1), image.pixels.at(x, 0, 2));
            EXPECT_NEAR(image.pixels.at(x, 0, 0), original.pixels.at(x, 0, 0), 1e-6f);
        }
        for (std::int32_t x = 0; x < static_cast<std::int32_t>(values.size()); ++x) {
            // Y' = Y + k (Y(p) - Y): the float rounding of Y, at most 4x amplified.
            EXPECT_NEAR(luminance(image, x, 0), luminance(original, x, 0), 1e-6) << sat << " " << vib;
        }
    }
    ColorImage grey = scene_image(1, 1, [](std::int32_t, std::int32_t) { return Rgb{0.4f, 0.2f, 0.1f}; });
    const float y = luminance(grey, 0, 0);
    ASSERT_TRUE(apply_color(grey, -1.0, 0.0));
    for (std::int32_t c = 0; c < 3; ++c) {
        EXPECT_NEAR(grey.pixels.at(0, 0, c), y, 1e-7f);
    }
}

TEST(ColorAdjust, VibranceFavoursMutedColoursAndChromaIsMonotone) {
    const auto chroma_gain = [](Rgb value, double sat, double vib) {
        ColorImage image = scene_image(1, 1, [&](std::int32_t, std::int32_t) { return value; });
        const float y = luminance(image, 0, 0);
        const float before = value[0] - y;
        EXPECT_TRUE(apply_color(image, sat, vib));
        return (image.pixels.at(0, 0, 0) - y) / before;
    };
    const Rgb muted{0.30f, 0.25f, 0.24f};
    const Rgb vivid{0.60f, 0.10f, 0.05f};
    EXPECT_GT(chroma_gain(muted, 0.0, 0.8), chroma_gain(vivid, 0.0, 0.8));
    EXPECT_GT(chroma_gain(vivid, 0.0, 0.8), 1.0);
    EXPECT_NEAR(chroma_gain(vivid, 0.5, 0.0), 1.5, 1e-5);
    // For a fixed hue, output chroma is non-decreasing in input chroma (vibrance +-1).
    for (const double vib : {-1.0, 1.0}) {
        double previous = -1.0;
        for (float k = 0.0f; k <= 1.0f; k += 0.01f) {
            const Rgb v{0.3f + 0.3f * k, 0.3f, 0.3f - 0.25f * k};
            ColorImage image = scene_image(1, 1, [&](std::int32_t, std::int32_t) { return v; });
            const float y = luminance(image, 0, 0);
            ASSERT_TRUE(apply_color(image, 0.0, vib));
            const double chroma = image.pixels.at(0, 0, 0) - y;
            EXPECT_GE(chroma, previous - 1e-7) << vib << " at " << k;
            previous = chroma;
        }
    }
    ColorImage image = scene_image(1, 1, [](std::int32_t, std::int32_t) { return Rgb{0.1f, 0.1f, 0.1f}; });
    ts::expect_error(apply_color(image, 1.2, 0.0), ErrorCode::InvalidArgument, "engine.develop.color");
    ts::expect_error(apply_color(image, 0.0, std::nan("")), ErrorCode::InvalidArgument, "engine.develop.color");
}

}  // namespace
}  // namespace lucent::engine
