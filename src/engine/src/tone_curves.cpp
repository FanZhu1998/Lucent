// Scalar curves of the develop operations: display lightness, Fritsch-Carlson tone curve,
// contrast and the "neutral" v1 base look (develop_operations.hpp).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <string_view>
#include <vector>

#include <lucent/engine/develop_operations.hpp>

namespace lucent::engine {

namespace {

// CIE 1976 lightness constants: (6/29)^3 and the slope of the linear segment, 1.16 * (29/6)^2 / 3.
constexpr double kLabEpsilon = 216.0 / 24389.0;
constexpr double kLabLinearSlope = 1.16 * (841.0 / 108.0);  // 1.16 * (1/3) * (29/6)^2
constexpr double kLabBreak = 6.0 / 29.0;

// Below this steepness the contrast curve differs from the identity by less than the rounding
// of a double (its deviation is O(b^2)); smaller values would also reach subnormal numbers.
constexpr double kMinContrastSteepness = 1e-150;

// Positive root t of c t^2 + t - delta = 0 (delta >= 0 or c small), computed stably.
double quadratic_root(double c, double delta) noexcept {
    const double disc = std::max(0.0, 1.0 + 4.0 * c * delta);
    return 2.0 * delta / (1.0 + std::sqrt(disc));
}

}  // namespace

// ---- Display lightness ----------------------------------------------------------------------------

double display_lightness(double d) noexcept {
    if (d > kLabEpsilon) {
        return 1.16 * std::cbrt(d) - 0.16;
    }
    return kLabLinearSlope * d;
}

double display_lightness_inverse(double l) noexcept {
    const double f = (l + 0.16) / 1.16;
    if (f > kLabBreak) {
        return f * f * f;
    }
    return l / kLabLinearSlope;
}

// ---- Tone curve -----------------------------------------------------------------------------------

Expected<ToneCurve> ToneCurve::create(const ToneCurveSetting& setting) {
    constexpr const char* kWhere = "engine.develop.tone_curve";
    const auto& p = setting.points;
    if (p.size() < kMinToneCurvePoints || p.size() > kMaxToneCurvePoints) {
        return fail(ErrorCode::InvalidArgument, std::format("a tone curve needs 2 to 32 points (got {})", p.size()),
                    kWhere);
    }
    for (std::size_t i = 0; i < p.size(); ++i) {
        if (!std::isfinite(p[i].x) || !std::isfinite(p[i].y) || p[i].x < 0.0 || p[i].x > 1.0 || p[i].y < 0.0 ||
            p[i].y > 1.0 || (i > 0 && (!(p[i].x - p[i - 1].x >= kMinToneCurveGap) || p[i].y < p[i - 1].y))) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("tone-curve point {} ({}, {}) breaks the monotone-curve rules", i, p[i].x, p[i].y),
                        kWhere);
        }
    }
    ToneCurve curve;
    curve.identity_ = setting.is_identity();
    const std::size_t n = p.size();
    curve.x_.resize(n);
    curve.y_.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        curve.x_[i] = p[i].x;
        curve.y_[i] = p[i].y;
    }
    std::vector<double> d(n - 1);
    for (std::size_t k = 0; k + 1 < n; ++k) {
        d[k] = (curve.y_[k + 1] - curve.y_[k]) / (curve.x_[k + 1] - curve.x_[k]);
    }
    std::vector<double>& m = curve.m_;
    m.assign(n, 0.0);
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (std::size_t k = 1; k + 1 < n; ++k) {
        m[k] = (d[k - 1] == 0.0 || d[k] == 0.0) ? 0.0 : 0.5 * (d[k - 1] + d[k]);
    }
    for (std::size_t k = 0; k + 1 < n; ++k) {
        if (d[k] == 0.0) {
            m[k] = 0.0;
            m[k + 1] = 0.0;
            continue;
        }
        // alpha^2 + beta^2 > 9 with alpha = m_k / d_k, beta = m_{k+1} / d_k, and the scaling
        // tau = 3 / sqrt(alpha^2 + beta^2), written without the ratios: they overflow when d_k is
        // tiny next to a steep neighbour (a nearly flat interval beside a nearly vertical one),
        // and tau * alpha would then be 0 * inf = NaN.
        const double norm = std::hypot(m[k], m[k + 1]);
        if (norm > 3.0 * d[k]) {
            const double s = 3.0 * d[k] / norm;
            m[k] *= s;
            m[k + 1] *= s;
        }
    }
    return curve;
}

double ToneCurve::evaluate(double x) const noexcept {
    if (!(x > x_.front())) {
        return y_.front();
    }
    if (!(x < x_.back())) {
        return y_.back();
    }
    // Interval k with x_k <= x < x_{k+1}.
    const auto it = std::upper_bound(x_.begin(), x_.end(), x);
    const auto k = static_cast<std::size_t>(it - x_.begin()) - 1;
    const double h = x_[k + 1] - x_[k];
    const double t = (x - x_[k]) / h;
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
    const double h10 = t3 - 2.0 * t2 + t;
    const double h01 = -2.0 * t3 + 3.0 * t2;
    const double h11 = t3 - t2;
    const double y = h00 * y_[k] + h10 * h * m_[k] + h01 * y_[k + 1] + h11 * h * m_[k + 1];
    // Monotone cubic segments stay within their end values; clamp the last rounding.
    return std::clamp(y, y_[k], y_[k + 1]);
}

// ---- Contrast -------------------------------------------------------------------------------------

Expected<ContrastCurve> ContrastCurve::create(double contrast) {
    if (!std::isfinite(contrast) || std::abs(contrast) > kMaxAdjustment) {
        return fail(ErrorCode::InvalidArgument, std::format("contrast {} must be finite and within [-1, 1]", contrast),
                    "engine.develop.contrast");
    }
    ContrastCurve curve;
    curve.contrast_ = contrast;
    curve.steepness_ = kContrastMaxSteepness * std::abs(contrast);
    curve.linear_ = !(curve.steepness_ >= kMinContrastSteepness);
    if (!curve.linear_) {
        curve.t0_ = std::tanh(0.5 * curve.steepness_ * kContrastPivot);
        curve.span_ = curve.t0_ + std::tanh(0.5 * curve.steepness_ * (1.0 - kContrastPivot));
    }
    return curve;
}

// With s(z) - 1/2 = tanh(z / 2) / 2 the logistic forms of the header become
//   c > 0: f(l) = (tanh(b (l - p) / 2) + tanh(b p / 2)) / (tanh(b (1 - p) / 2) + tanh(b p / 2)),
//   c < 0: f(l) = p + 2 atanh(l (tanh(b (1 - p) / 2) + tanh(b p / 2)) - tanh(b p / 2)) / b,
// which keep full relative precision as b -> 0 (the differences of logistics they replace cancel
// to nothing for |c| below about 3e-17).
double ContrastCurve::evaluate(double l) const noexcept {
    if (contrast_ == 0.0 || linear_) {
        return l;
    }
    const double x = std::clamp(l, 0.0, 1.0);
    const double b = steepness_;
    if (contrast_ > 0.0) {
        return std::clamp((std::tanh(0.5 * b * (x - kContrastPivot)) + t0_) / span_, 0.0, 1.0);
    }
    const double u = std::clamp(x * span_ - t0_, -1.0, 1.0);
    return std::clamp(kContrastPivot + 2.0 * std::atanh(u) / b, 0.0, 1.0);
}

double ContrastCurve::slope_at_pivot() const noexcept {
    if (contrast_ == 0.0 || linear_) {
        return 1.0;
    }
    const double s = 0.5 * steepness_ / span_;  // (b / 2) / (tanh(b (1 - p) / 2) + tanh(b p / 2))
    return contrast_ > 0.0 ? s : 1.0 / s;
}

// ---- Neutral v1 -----------------------------------------------------------------------------------

std::string_view to_string(NeutralV1Regime regime) noexcept {
    switch (regime) {
        case NeutralV1Regime::Shoulder:
            return "shoulder";
        case NeutralV1Regime::Lift:
            return "lift";
    }
    return "invalid";
}

Expected<NeutralV1Curve> NeutralV1Curve::create(double white) {
    if (!std::isfinite(white) || !(white > 0.0)) {
        return fail(ErrorCode::InvalidArgument, std::format("the white point must be finite and positive (got {})", white),
                    "engine.develop.base_look");
    }
    constexpr double o = kNeutralV1MidOffsetEv;
    NeutralV1Curve c;
    c.white_ = white;
    c.x_w_ = std::log2(white);
    const double shoulder_length = 2.0 * (c.x_w_ + o);
    c.length_ = std::max(kNeutralV1MinTopEv, shoulder_length);
    c.regime_ = shoulder_length >= kNeutralV1MinTopEv ? NeutralV1Regime::Shoulder : NeutralV1Regime::Lift;
    c.x_s_ = c.x_w_ - c.length_;
    const double y_s = c.x_s_ + o;
    c.a_ = -(y_s + c.length_) / (c.length_ * c.length_);
    c.end_slope_ = c.regime_ == NeutralV1Regime::Shoulder ? 0.0 : 1.0 + 2.0 * c.a_ * c.length_;
    c.x_t1_ = std::min(kNeutralV1ToeEndEv, c.x_s_);
    c.x_t0_ = c.x_t1_ - kNeutralV1ToeLengthEv;
    const double h = 0.5 * kNeutralV1ToeLengthEv;
    c.toe_c_ = (o / h) / (2.0 * h);
    c.mid_gain_ = std::exp2(o);
    c.n_t0_ = std::exp2(c.x_t0_);
    c.n_t1_ = std::exp2(c.x_t1_);
    c.n_s_ = std::exp2(c.x_s_);
    c.d_t1_ = c.n_t1_ * c.mid_gain_;
    c.d_s_ = c.n_s_ * c.mid_gain_;
    return c;
}

double NeutralV1Curve::evaluate_log(double x) const noexcept {
    constexpr double o = kNeutralV1MidOffsetEv;
    const double h = 0.5 * kNeutralV1ToeLengthEv;
    if (x <= x_t0_) {
        return x;
    }
    if (x <= x_t0_ + h) {
        const double t = x - x_t0_;
        return x + toe_c_ * t * t;
    }
    if (x <= x_t1_) {
        const double u = x_t1_ - x;
        return x + o - toe_c_ * u * u;
    }
    if (x <= x_s_) {
        return x + o;
    }
    if (x < x_w_) {
        const double t = x - x_s_;
        return std::min(0.0, (x_s_ + o) + t + a_ * t * t);
    }
    return 0.0;
}

double NeutralV1Curve::inverse_log(double y) const noexcept {
    constexpr double o = kNeutralV1MidOffsetEv;
    const double h = 0.5 * kNeutralV1ToeLengthEv;
    if (y <= x_t0_) {
        return y;
    }
    const double y_mid = x_t0_ + h + 0.5 * o;  // y at the toe's midpoint
    if (y <= y_mid) {
        return x_t0_ + quadratic_root(toe_c_, y - x_t0_);
    }
    if (y <= x_t1_ + o) {
        return x_t1_ - quadratic_root(toe_c_, x_t1_ + o - y);
    }
    const double y_s = x_s_ + o;
    if (y <= y_s) {
        return y - o;
    }
    return std::min(x_w_, x_s_ + quadratic_root(a_, y - y_s));
}

// The identity and mid-tone segments are evaluated in the linear domain (no logarithm): the
// same values as the log-domain formulas to within rounding, and most pixels of an image lie
// there.
double NeutralV1Curve::evaluate(double n) const noexcept {
    if (!(n > 0.0)) {
        return n;
    }
    if (n >= white_) {
        return 1.0;
    }
    if (n <= n_t0_) {
        return n;
    }
    if (n > n_t1_ && n <= n_s_) {
        return n * mid_gain_;
    }
    return std::exp2(evaluate_log(std::log2(n)));
}

double NeutralV1Curve::inverse(double d) const noexcept {
    if (!(d > 0.0)) {
        return d;
    }
    if (d >= 1.0) {
        return white_;
    }
    if (d <= n_t0_) {
        return d;
    }
    if (d > d_t1_ && d <= d_s_) {
        return std::min(white_, d / mid_gain_);
    }
    return std::min(white_, std::exp2(inverse_log(std::log2(d))));
}

}  // namespace lucent::engine
