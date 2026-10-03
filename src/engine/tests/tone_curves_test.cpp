// Unit tests for the scalar curves of the develop operations (develop_operations.hpp): display
// lightness, the Fritsch-Carlson tone curve, contrast, the "neutral" v1 base look and the local
// tone gain field.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/engine/develop_operations.hpp>

#include "sensor_test_support.hpp"

namespace lucent::engine {
namespace {

namespace ts = test_support;

// ---- Display lightness ------------------------------------------------------------------------------

TEST(DisplayLightness, IsCieLightnessOverOneHundred) {
    EXPECT_DOUBLE_EQ(display_lightness(0.0), 0.0);
    EXPECT_NEAR(display_lightness(1.0), 1.0, 1e-15);
    EXPECT_NEAR(display_lightness(0.184186518), 0.5, 1e-9);  // L* = 50: Y = (66 / 116)^3
    // Continuous at (6/29)^3 and strictly increasing on both segments.
    const double e = 216.0 / 24389.0;
    EXPECT_NEAR(display_lightness(e * (1.0 - 1e-12)), display_lightness(e * (1.0 + 1e-12)), 1e-12);
    double previous = display_lightness(-0.01);
    for (double d = -0.01; d <= 1.5; d += 1e-4) {
        const double l = display_lightness(d);
        EXPECT_GT(l, previous - 1e-15);
        previous = l;
        EXPECT_NEAR(display_lightness_inverse(l), d, 1e-12 * std::max(1.0, std::abs(d)));
    }
}

// ---- Tone curve -------------------------------------------------------------------------------------

TEST(ToneCurve, IdentityAndInterpolation) {
    auto identity = ToneCurve::create(ToneCurveSetting{});
    ASSERT_TRUE(identity);
    EXPECT_TRUE(identity->identity());
    for (double x = 0.0; x <= 1.0; x += 0.01) {
        EXPECT_NEAR(identity->evaluate(x), x, 1e-15);
    }
    ToneCurveSetting s;
    s.points = {{0.0, 0.05}, {0.25, 0.2}, {0.5, 0.55}, {0.75, 0.85}, {1.0, 0.97}};
    auto curve = ToneCurve::create(s);
    ASSERT_TRUE(curve);
    EXPECT_FALSE(curve->identity());
    for (const ToneCurvePoint& p : s.points) {
        EXPECT_NEAR(curve->evaluate(p.x), p.y, 1e-15);
    }
    // Constant beyond the end points.
    ToneCurveSetting inner;
    inner.points = {{0.2, 0.1}, {0.8, 0.9}};
    auto c2 = ToneCurve::create(inner);
    ASSERT_TRUE(c2);
    EXPECT_DOUBLE_EQ(c2->evaluate(0.0), 0.1);
    EXPECT_DOUBLE_EQ(c2->evaluate(0.1), 0.1);
    EXPECT_DOUBLE_EQ(c2->evaluate(0.95), 0.9);
    EXPECT_NEAR(c2->evaluate(0.5), 0.5, 1e-15);  // linear between two points
}

TEST(ToneCurve, IsMonotoneWithoutOvershootAndC1) {
    // Data that makes an ordinary cubic spline overshoot: a plateau next to steep steps.
    ToneCurveSetting s;
    s.points = {{0.0, 0.0}, {0.1, 0.0}, {0.2, 0.6}, {0.3, 0.62}, {0.6, 0.63}, {0.65, 1.0}, {1.0, 1.0}};
    auto curve = ToneCurve::create(s);
    ASSERT_TRUE(curve);
    double previous = -1.0;
    for (double x = 0.0; x <= 1.0; x += 1e-4) {
        const double y = curve->evaluate(x);
        EXPECT_GE(y, previous) << x;
        EXPECT_GE(y, 0.0);
        EXPECT_LE(y, 1.0);
        previous = y;
    }
    // Flat interval stays exactly flat.
    for (double x = 0.0; x <= 0.1; x += 0.01) {
        EXPECT_EQ(curve->evaluate(x), 0.0);
    }
    // Fritsch-Carlson condition alpha^2 + beta^2 <= 9 on every interval.
    const auto& m = curve->tangents();
    for (std::size_t k = 0; k + 1 < s.points.size(); ++k) {
        const double d = (s.points[k + 1].y - s.points[k].y) / (s.points[k + 1].x - s.points[k].x);
        if (d > 0.0) {
            const double a = m[k] / d;
            const double b = m[k + 1] / d;
            EXPECT_LE(a * a + b * b, 9.0 + 1e-9) << k;
        }
    }
    // C1 at the knots: one-sided difference quotients agree.
    for (std::size_t k = 1; k + 1 < s.points.size(); ++k) {
        const double x = s.points[k].x;
        const double h = 1e-7;
        const double left = (curve->evaluate(x) - curve->evaluate(x - h)) / h;
        const double right = (curve->evaluate(x + h) - curve->evaluate(x)) / h;
        EXPECT_NEAR(left, right, 1e-4 + 1e-4 * std::abs(left)) << k;
    }
}

TEST(ToneCurve, RejectsInvalidPoints) {
    ToneCurveSetting s;
    s.points = {{0.0, 0.5}, {0.5, 0.4}};
    ts::expect_error(ToneCurve::create(s), ErrorCode::InvalidArgument, "engine.develop.tone_curve");
    s.points = {{0.5, 0.5}};
    ts::expect_error(ToneCurve::create(s), ErrorCode::InvalidArgument, "engine.develop.tone_curve");
}

// ---- Contrast ---------------------------------------------------------------------------------------

TEST(ContrastCurve, FixesBlackPivotAndWhiteAndIsSymmetricInItsSign) {
    for (const double c : {-1.0, -0.4, 0.0, 0.3, 1.0}) {
        auto curve = ContrastCurve::create(c);
        ASSERT_TRUE(curve);
        EXPECT_NEAR(curve->evaluate(0.0), 0.0, 1e-12) << c;
        EXPECT_NEAR(curve->evaluate(kContrastPivot), kContrastPivot, 1e-12) << c;
        EXPECT_NEAR(curve->evaluate(1.0), 1.0, 1e-12) << c;
        double previous = -1.0;
        for (double l = 0.0; l <= 1.0; l += 1e-3) {
            const double v = curve->evaluate(l);
            EXPECT_GT(v, previous) << c << " at " << l;
            previous = v;
        }
        auto opposite = ContrastCurve::create(-c);
        ASSERT_TRUE(opposite);
        for (double l = 0.0; l <= 1.0; l += 0.05) {
            EXPECT_NEAR(opposite->evaluate(curve->evaluate(l)), l, 1e-12) << c;
        }
        // Slope at the pivot: (b/4) / tanh(b/4) for c > 0, its reciprocal for c < 0.
        const double b = kContrastMaxSteepness * std::abs(c);
        const double expected = c == 0.0 ? 1.0 : (b / 4.0) / std::tanh(b / 4.0);
        EXPECT_NEAR(curve->slope_at_pivot(), c >= 0.0 ? expected : 1.0 / expected, 1e-12);
        const double h = 1e-6;
        EXPECT_NEAR((curve->evaluate(0.5 + h) - curve->evaluate(0.5 - h)) / (2.0 * h), curve->slope_at_pivot(),
                    1e-6);
    }
    EXPECT_TRUE(ContrastCurve::create(0.0)->identity());
    EXPECT_NEAR(ContrastCurve::create(1.0)->slope_at_pivot(), 2.0 / std::tanh(2.0), 1e-12);  // 2.0746
    ts::expect_error(ContrastCurve::create(1.01), ErrorCode::InvalidArgument, "engine.develop.contrast");
    ts::expect_error(ContrastCurve::create(std::nan("")), ErrorCode::InvalidArgument, "engine.develop.contrast");
}

// ---- Neutral v1 -------------------------------------------------------------------------------------

TEST(NeutralV1Curve, DefaultWhiteHasAOneStopShoulderAndHalfStopMidTones) {
    auto curve = NeutralV1Curve::create(1.0);
    ASSERT_TRUE(curve);
    EXPECT_EQ(curve->regime(), NeutralV1Regime::Shoulder);
    EXPECT_DOUBLE_EQ(curve->top_start_ev(), -1.0);
    EXPECT_DOUBLE_EQ(curve->end_slope(), 0.0);
    EXPECT_DOUBLE_EQ(curve->toe_end_ev(), kNeutralV1ToeEndEv);
    EXPECT_DOUBLE_EQ(curve->toe_start_ev(), kNeutralV1ToeEndEv - kNeutralV1ToeLengthEv);
    // Mid-tones +0.5 EV: an ISO 12232 metered grey (0.18 / sqrt 2) lands at display 0.18.
    EXPECT_NEAR(curve->evaluate(0.18 / std::sqrt(2.0)), 0.18, 1e-12);
    EXPECT_NEAR(curve->evaluate(0.5), std::sqrt(0.5), 1e-12);
    EXPECT_NEAR(curve->evaluate(0.75), 0.94205, 1e-5);
    EXPECT_DOUBLE_EQ(curve->evaluate(1.0), 1.0);
    EXPECT_DOUBLE_EQ(curve->evaluate(5.0), 1.0);
    // Deep shadows keep their scene-linear value.
    EXPECT_DOUBLE_EQ(curve->evaluate(1e-3), 1e-3);
    EXPECT_DOUBLE_EQ(curve->evaluate(0.0), 0.0);
    EXPECT_DOUBLE_EQ(curve->evaluate(-0.2), -0.2);
}

TEST(NeutralV1Curve, IsMonotoneC1InvertibleAndContinuousInTheWhitePoint) {
    for (const double white : {1.0 / 32.0, 0.25, 0.58, 0.99, 1.0, 1.01, 1.41, 4.0, 32.0, 300.0}) {
        SCOPED_TRACE(white);
        auto curve = NeutralV1Curve::create(white);
        ASSERT_TRUE(curve);
        EXPECT_EQ(curve->regime(), white >= 1.0 ? NeutralV1Regime::Shoulder : NeutralV1Regime::Lift);
        double previous = 0.0;
        double previous_slope = 1.0;
        const double step = 1.0 / 256.0;  // EV
        for (double x = std::log2(white) - 16.0; x < std::log2(white) - step; x += step) {
            const double n = std::exp2(x);
            const double d = curve->evaluate(n);
            EXPECT_GT(d, previous) << x;
            EXPECT_LE(d, 1.0);
            // Exact inverse.
            EXPECT_NEAR(curve->inverse(d), n, 1e-9 * n) << x;
            // C1 in log space: the log-log slope changes by at most O(step) per step.
            const double slope = (std::log2(curve->evaluate(std::exp2(x + 1e-6))) - std::log2(d)) / 1e-6;
            if (x > std::log2(white) - 15.0) {
                EXPECT_NEAR(slope, previous_slope, 0.25) << x;
            }
            EXPECT_GT(slope, 0.0);
            previous_slope = slope;
            previous = d;
        }
        EXPECT_DOUBLE_EQ(curve->evaluate(white), 1.0);
        EXPECT_DOUBLE_EQ(curve->inverse(1.0), white);
        EXPECT_DOUBLE_EQ(curve->inverse(-0.5), -0.5);
        // Continuity in W: a 1e-9 relative change of the white point moves the curve by ~1e-9.
        auto nearby = NeutralV1Curve::create(white * (1.0 + 1e-9));
        ASSERT_TRUE(nearby);
        for (double x = std::log2(white) - 10.0; x < std::log2(white); x += 0.37) {
            EXPECT_NEAR(nearby->evaluate(std::exp2(x)), curve->evaluate(std::exp2(x)), 1e-7) << x;
        }
    }
    ts::expect_error(NeutralV1Curve::create(0.0), ErrorCode::InvalidArgument, "engine.develop.base_look");
    ts::expect_error(NeutralV1Curve::create(std::nan("")), ErrorCode::InvalidArgument, "engine.develop.base_look");
}

TEST(NeutralV1Curve, ShoulderRollsIntoWhiteWithZeroSlope) {
    for (const double white : {1.0, 2.0, 16.0}) {
        auto curve = NeutralV1Curve::create(white);
        ASSERT_TRUE(curve);
        const double h = white * 1e-4;
        const double slope = (curve->evaluate(white) - curve->evaluate(white - h)) / h;  // linear units
        EXPECT_LT(slope, 1e-3) << white;
    }
    auto lift = NeutralV1Curve::create(0.5);
    ASSERT_TRUE(lift);
    EXPECT_EQ(lift->regime(), NeutralV1Regime::Lift);
    EXPECT_NEAR(lift->end_slope(), 1.0 - 2.0 * (-1.0 + kNeutralV1MidOffsetEv), 1e-12);
}

// ---- Local tone gain ----------------------------------------------------------------------------------

TEST(LocalToneGain, WeightsAndBoundsAndKeepsTheBaseLayerOrdered) {
    // Identity at zero.
    EXPECT_EQ(local_tone_gain_ev(-3.0, 0.0, 0.0, 0.0), 0.0);
    // Full weights at white and in deep shadow.
    EXPECT_NEAR(local_tone_gain_ev(0.0, 0.0, -1.0, 0.0), -kHighlightsMaxEv, 1e-15);
    EXPECT_NEAR(local_tone_gain_ev(-9.0, 0.0, 0.0, 1.0), kShadowsMaxEv, 1e-15);
    // Mid grey (display -2.47 EV): no shadow weight, almost no highlight weight.
    EXPECT_EQ(local_tone_gain_ev(-2.97, 0.0, 0.0, 1.0), 0.0);
    EXPECT_LT(std::abs(local_tone_gain_ev(-2.97, 0.0, -1.0, 0.0)), 0.15);
    // For every extreme setting and white point, x + gain(x) is strictly increasing: the smoothed
    // tones never swap order (no tonal reversal).
    for (const double white_ev : {-3.0, -1.0, 0.0, 0.5, 3.0}) {
        for (const double h : {-1.0, 1.0}) {
            for (const double s : {-1.0, 1.0}) {
                double previous = -1e9;
                for (double x = white_ev - 14.0; x <= white_ev + 2.0; x += 1e-3) {
                    const double mapped = x + local_tone_gain_ev(x, white_ev, h, s);
                    EXPECT_GT(mapped, previous) << white_ev << " " << h << " " << s << " at " << x;
                    previous = mapped;
                }
            }
        }
    }
}

}  // namespace
}  // namespace lucent::engine
