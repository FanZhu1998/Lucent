// Unit tests for the Neutral v0 display rendering (display_rendering.hpp).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/core/cancellation.hpp>
#include <lucent/engine/display_rendering.hpp>

#include "sensor_test_support.hpp"

namespace lucent::engine {
namespace {

namespace ts = test_support;

ColorImage scene(const std::vector<std::array<float, 3>>& pixels) {
    ColorImage img;
    img.pixels = *image::ImageBuffer<float>::create(static_cast<std::int32_t>(pixels.size()), 1, 3);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        for (std::int32_t c = 0; c < 3; ++c) {
            img.pixels.at(static_cast<std::int32_t>(i), 0, c) = pixels[i][static_cast<std::size_t>(c)];
        }
    }
    img.encoding = image::ColorEncoding::scene_linear_rec2020_d65();
    return img;
}

TEST(NeutralV0Curve, IsIdentityBelowTheKneeAndReachesWhite) {
    for (const double white : {1.0, 1.5, 2.5, 8.0}) {
        for (const double m : {0.0, 0.1, 0.18, 0.5, 0.79, 0.8}) {
            EXPECT_EQ(neutral_v0_curve(m, 0.8, white), m);
        }
        EXPECT_NEAR(neutral_v0_curve(white, 0.8, white), 1.0, 1e-15);
        EXPECT_EQ(neutral_v0_curve(white * 2.0, 0.8, white), 1.0);
    }
}

TEST(NeutralV0Curve, WithUnitWhiteOnlyLimitsValuesAboveOne) {
    for (const double m : {0.81, 0.9, 0.99, 1.0}) {
        EXPECT_NEAR(neutral_v0_curve(m, 0.8, 1.0), m, 1e-15);
    }
    EXPECT_EQ(neutral_v0_curve(1.2, 0.8, 1.0), 1.0);
}

TEST(NeutralV0Curve, IsMonotoneConcaveAndContinuouslyDifferentiable) {
    const double knee = 0.75;
    const double white = 3.0;
    const double h = 1e-7;
    // Slope 1 at the knee, 0 at the white point.
    EXPECT_NEAR((neutral_v0_curve(knee + h, knee, white) - neutral_v0_curve(knee, knee, white)) / h, 1.0,
                1e-5);
    EXPECT_NEAR((neutral_v0_curve(white, knee, white) - neutral_v0_curve(white - h, knee, white)) / h, 0.0,
                1e-5);
    double previous = neutral_v0_curve(knee, knee, white);
    for (double m = knee + 0.01; m < white; m += 0.01) {
        const double f = neutral_v0_curve(m, knee, white);
        EXPECT_GE(f, previous) << m;  // strictly increasing, but f rounds to 1 near W
        EXPECT_LE(f, m) << m;
        EXPECT_LE(f, 1.0) << m;
        previous = f;
    }
}

TEST(NeutralV0, LeavesDarkPixelsAndHuesUnchanged) {
    ColorImage img = scene({{0.1f, 0.2f, 0.05f},
                            {1.6f, 0.8f, 0.4f},
                            {-0.1f, 0.3f, 0.2f},
                            {-0.2f, -0.1f, -0.3f},
                            {0.93f, 0.85f, 0.3f}});
    auto record = render_neutral_v0(img, NeutralV0Parameters{});
    ASSERT_TRUE(record) << record.error().describe();
    EXPECT_EQ(img.encoding, display_linear_rec2020());
    EXPECT_EQ(record->gain, 1.0);
    EXPECT_EQ(record->white, 1.0);
    EXPECT_EQ(record->curve, NeutralV0Curve::Identity);
    // Dark pixel: unchanged.
    EXPECT_EQ(img.pixels.at(0, 0, 0), 0.1f);
    EXPECT_EQ(img.pixels.at(0, 0, 1), 0.2f);
    // Bright saturated pixel: max limited to 1, ratios kept.
    EXPECT_FLOAT_EQ(img.pixels.at(1, 0, 0), 1.0f);
    EXPECT_FLOAT_EQ(img.pixels.at(1, 0, 1), 0.5f);
    EXPECT_FLOAT_EQ(img.pixels.at(1, 0, 2), 0.25f);
    // Out-of-gamut components stay negative (the exporter clips them).
    EXPECT_EQ(img.pixels.at(2, 0, 0), -0.1f);
    EXPECT_EQ(img.pixels.at(3, 0, 2), -0.3f);
    // Above the knee but below W = 1: the identity branch leaves it bit-exact.
    EXPECT_EQ(img.pixels.at(4, 0, 0), 0.93f);
    EXPECT_EQ(img.pixels.at(4, 0, 1), 0.85f);
    EXPECT_EQ(record->limited_pixels, 1u);
    EXPECT_EQ(record->compressed_pixels, 0u);
    EXPECT_EQ(record->negative_samples, 4u);
}

TEST(NeutralV0, BaselineExposureRaisesTheWhitePointSoClippedNeutralsReachWhite) {
    ColorImage img = scene({{0.25f, 0.25f, 0.25f}, {1.0f, 1.0f, 1.0f}, {0.6f, 0.5f, 0.4f}});
    NeutralV0Parameters p;
    p.baseline_exposure_ev = 1.0;
    auto record = render_neutral_v0(img, p);
    ASSERT_TRUE(record);
    EXPECT_EQ(record->gain, 2.0);
    EXPECT_EQ(record->white, 2.0);
    EXPECT_EQ(record->curve, NeutralV0Curve::Shoulder);
    EXPECT_EQ(record->version, kNeutralV0Version);
    EXPECT_FLOAT_EQ(img.pixels.at(0, 0, 0), 0.5f);  // 0.25 * 2, below the knee
    for (std::int32_t c = 0; c < 3; ++c) {
        EXPECT_FLOAT_EQ(img.pixels.at(1, 0, c), 1.0f);  // clipped neutral -> display white
    }
    // Compressed pixel keeps its ratios.
    const float r = img.pixels.at(2, 0, 0);
    const float g = img.pixels.at(2, 0, 1);
    EXPECT_NEAR(g / r, 0.5f / 0.6f, 1e-6f);
    EXPECT_NEAR(r, static_cast<float>(neutral_v0_curve(1.2, 0.8, 2.0)), 1e-6f);
    EXPECT_EQ(record->compressed_pixels, 1u);
    EXPECT_EQ(record->limited_pixels, 1u);
}

TEST(NeutralV0Curve, LiftMatchesTheDngSdkExposureToneAndReachesWhite) {
    // dng_function_exposure_tone (Adobe DNG SDK, dng_render.cpp) for slope s on x in [0, 1]:
    // x * s up to 0.25, then a x^2 + b x + c with a = 16/9 (1 - s), b = s - a / 2, c = 1 - a - b.
    const auto sdk = [](double x, double s) {
        const double a = 16.0 / 9.0 * (1.0 - s);
        const double b = s - 0.5 * a;
        const double c = 1.0 - a - b;
        return x <= 0.25 ? x * s : (a * x + b) * x + c;
    };
    for (const double white : {0.9, 0.5824, 0.25, 0.01}) {
        for (double u = 0.0; u <= 1.0; u += 1.0 / 64.0) {
            EXPECT_NEAR(neutral_v0_curve(u * white, 0.8, white), sdk(u, white), 1e-12) << white << " " << u;
        }
        // Plain gain up to two stops below the clip point, white at the clip point and above.
        EXPECT_EQ(neutral_v0_curve(0.25 * white, 0.8, white), 0.25 * white);
        EXPECT_EQ(neutral_v0_curve(white, 0.8, white), 1.0);
        EXPECT_EQ(neutral_v0_curve(white * 1.5, 0.8, white), 1.0);
        // C1 at the break (slope 1 in m) and strictly increasing up to 1 at W.
        const double h = 1e-8 * white;
        const double at = 0.25 * white;
        EXPECT_NEAR((neutral_v0_curve(at + h, 0.8, white) - neutral_v0_curve(at, 0.8, white)) / h, 1.0, 1e-5);
        double previous = neutral_v0_curve(at, 0.8, white);
        for (double m = at + white / 100.0; m < white; m += white / 100.0) {
            const double f = neutral_v0_curve(m, 0.8, white);
            EXPECT_GT(f, previous) << m;
            EXPECT_GE(f, m) << m;  // lifted, never darkened
            EXPECT_LT(f, 1.0) << m;
            previous = f;
        }
    }
    // Continuous in W at 1 from below: the lift tends to the identity.
    EXPECT_NEAR(neutral_v0_curve(0.6, 0.8, 1.0 - 1e-9), 0.6, 1e-8);
}

TEST(NeutralV0, NegativeBaselineExposureStillRendersClippedNeutralsWhite) {
    // Leica M10: BaselineExposure -0.78 EV. The clipped neutral (scene 1.0) must be display white,
    // mid-tones take the plain gain, and the top two stops are lifted (COLR-01).
    ColorImage img = scene({{1.0f, 1.0f, 1.0f}, {0.18f, 0.18f, 0.18f}, {0.6f, 0.3f, 0.15f}, {1.3f, 0.9f, 0.4f}});
    NeutralV0Parameters p;
    p.baseline_exposure_ev = -0.78;
    auto record = render_neutral_v0(img, p);
    ASSERT_TRUE(record) << record.error().describe();
    const double gain = std::exp2(-0.78);
    EXPECT_DOUBLE_EQ(record->gain, gain);
    EXPECT_DOUBLE_EQ(record->white, gain);
    EXPECT_EQ(record->curve, NeutralV0Curve::Lift);
    EXPECT_EQ(record->exponent, 1.0);
    EXPECT_DOUBLE_EQ(record->lift[0], 16.0 / 9.0 * (1.0 - gain));
    for (std::int32_t c = 0; c < 3; ++c) {
        // Clipped neutral -> display white (the gain is applied in float, so m may round to just
        // below W: within a few ulps of 1).
        EXPECT_NEAR(img.pixels.at(0, 0, c), 1.0f, 1e-6f) << c;
    }
    // 0.18 * gain = 0.105 <= W / 4 = 0.146: plain gain, bit-exact.
    EXPECT_EQ(img.pixels.at(1, 0, 1), 0.18f * static_cast<float>(gain));
    // A saturated colour below the clip point is lifted with its ratios kept.
    const float r = img.pixels.at(2, 0, 0);
    EXPECT_NEAR(img.pixels.at(2, 0, 1) / r, 0.5f, 1e-6f);
    EXPECT_NEAR(r, neutral_v0_curve(0.6 * static_cast<float>(gain), 0.8, gain), 1e-6);
    EXPECT_GT(r, 0.6f * static_cast<float>(gain));
    // Above the clip level (camera matrix overshoot): limited to max 1, ratios kept.
    EXPECT_FLOAT_EQ(img.pixels.at(3, 0, 0), 1.0f);
    EXPECT_NEAR(img.pixels.at(3, 0, 2), 0.4f / 1.3f, 1e-6f);
    EXPECT_GE(record->limited_pixels, 1u);  // pixel 3 (pixel 0 sits at W up to rounding)
    EXPECT_EQ(record->limited_pixels + record->lifted_pixels, 3u);
    EXPECT_EQ(record->compressed_pixels, 0u);
}

TEST(NeutralV0, SceneWhiteBelowOneMapsTheClipLevelToWhite) {
    // A clip level below 1 (for example a custom highlight policy) must still reach display
    // white; nothing is brighter than a clipped neutral.
    ColorImage img = scene({{0.85f, 0.85f, 0.85f}, {0.1f, 0.1f, 0.1f}});
    NeutralV0Parameters p;
    p.scene_white = 0.85;
    auto record = render_neutral_v0(img, p);
    ASSERT_TRUE(record);
    EXPECT_DOUBLE_EQ(record->white, 0.85);
    EXPECT_EQ(record->curve, NeutralV0Curve::Lift);
    for (std::int32_t c = 0; c < 3; ++c) {
        EXPECT_NEAR(img.pixels.at(0, 0, c), 1.0f, 1e-6f);
    }
    EXPECT_EQ(img.pixels.at(1, 0, 0), 0.1f);  // below 0.85 / 4: unchanged
}

TEST(NeutralV0, ClippedNeutralIsDisplayWhiteForEveryGainAndSceneWhite) {
    for (const double ev : {-3.0, -0.78, -0.1, 0.0, 0.1, 1.0, 2.5}) {
        for (const double scene_white : {0.85, 1.0, 1.25}) {
            const auto level = static_cast<float>(scene_white);
            ColorImage img = scene({{level, level, level}});
            NeutralV0Parameters p;
            p.exposure_ev = ev;
            p.scene_white = scene_white;
            auto record = render_neutral_v0(img, p);
            ASSERT_TRUE(record) << record.error().describe();
            for (std::int32_t c = 0; c < 3; ++c) {
                EXPECT_NEAR(img.pixels.at(0, 0, c), 1.0f, 1e-6f) << ev << " " << scene_white;
            }
        }
    }
}

TEST(NeutralV0, ParallelAndSerialAgree) {
    ColorImage a;
    a.pixels = *image::ImageBuffer<float>::create(37, 23, 3);
    for (std::int32_t y = 0; y < 23; ++y) {
        for (std::int32_t x = 0; x < 37; ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                a.pixels.at(x, y, c) = static_cast<float>(std::sin(0.3 * x + 0.7 * y + c) * 1.5 + 0.6);
            }
        }
    }
    a.encoding = image::ColorEncoding::scene_linear_rec2020_d65();
    ColorImage b{*a.pixels.clone(), a.encoding};
    NeutralV0Parameters p;
    p.exposure_ev = 0.7;
    ASSERT_TRUE(render_neutral_v0(a, p, {}, true));
    ASSERT_TRUE(render_neutral_v0(b, p, {}, false));
    for (std::int32_t y = 0; y < 23; ++y) {
        for (std::int32_t x = 0; x < 37; ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                ASSERT_EQ(a.pixels.at(x, y, c), b.pixels.at(x, y, c));
                ASSERT_LE(std::max({a.pixels.at(x, y, 0), a.pixels.at(x, y, 1), a.pixels.at(x, y, 2)}), 1.0f);
            }
        }
    }
}

TEST(NeutralV0, RejectsInvalidInputs) {
    ColorImage img = scene({{0.1f, 0.1f, 0.1f}});
    NeutralV0Parameters bad;
    bad.knee = 1.0;
    ts::expect_error(render_neutral_v0(img, bad), ErrorCode::InvalidArgument, "engine.render.neutral");
    bad = {};
    bad.exposure_ev = std::nan("");
    ts::expect_error(render_neutral_v0(img, bad), ErrorCode::InvalidArgument, "engine.render.neutral");
    bad = {};
    bad.baseline_exposure_ev = 17.0;
    ts::expect_error(render_neutral_v0(img, bad), ErrorCode::InvalidArgument, "engine.render.neutral");
    bad = {};
    bad.scene_white = 0.0;
    ts::expect_error(render_neutral_v0(img, bad), ErrorCode::InvalidArgument, "engine.render.neutral");

    ColorImage display = scene({{0.1f, 0.1f, 0.1f}});
    display.encoding = display_linear_rec2020();
    ts::expect_error(render_neutral_v0(display, {}), ErrorCode::InvalidArgument, "engine.render.neutral");
    ColorImage empty;
    empty.encoding = image::ColorEncoding::scene_linear_rec2020_d65();
    ts::expect_error(render_neutral_v0(empty, {}), ErrorCode::InvalidArgument, "engine.render.neutral");
}

TEST(NeutralV0, ReportsNonFiniteOutputAsInternal) {
    ColorImage img = scene({{std::nanf(""), 0.1f, 0.1f}});
    ts::expect_error(render_neutral_v0(img, {}), ErrorCode::Internal, "engine.render.neutral");
}

TEST(NeutralV0, HonoursCancellation) {
    ColorImage img = scene({{0.1f, 0.1f, 0.1f}, {0.2f, 0.2f, 0.2f}});
    core::CancellationSource source;
    source.cancel();
    ts::expect_error(render_neutral_v0(img, {}, source.token()), ErrorCode::Cancelled,
                     "engine.render.neutral");
}

}  // namespace
}  // namespace lucent::engine
