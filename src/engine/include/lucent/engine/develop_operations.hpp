#pragma once

// Photographic develop operations (blueprint section 5, steps 6-7; DevelopSettings v1). Each
// operation works on a ColorImage in a declared encoding, has its own semantic version
// (develop_settings.hpp, base look below), is deterministic and leaves the image bit-exact when
// its settings are the identity (callers skip it then; develop.hpp does).
//
// Order (develop.hpp, render_stage_two):
//   1. white-balance correction relative to stage one, camera colour for the target white,
//      working space and exposure: one fused 3 x 3 pass (develop.hpp)  -> scene-linear Rec.2020
//   2. local highlights/shadows (apply_local_tone)                      -> scene-linear Rec.2020
//   3. contrast and 4. tone curve (apply_lightness_curves)              -> scene-linear Rec.2020
//   5. saturation and vibrance (apply_color)                            -> scene-linear Rec.2020
//   6. base look "neutral" v1 and display hand-off (apply_base_look)    -> display-linear Rec.2020
//
// Shared definitions:
//   * Luminance Y of scene- or display-linear Rec.2020 / D65: ITU-R BT.2020 coefficients
//     Y = 0.2627 R + 0.6780 G + 0.0593 B (kRec2020Luminance).
//   * Value norm m = max(R, G, B): the base look and the lightness curves scale a pixel by
//     f(m) / m, which keeps channel ratios (hue and linear saturation) exactly; pixels with
//     m <= 0 are left unchanged.
//   * Display lightness l of a display-linear value d (relative to display white): CIE 1976
//     L* / 100, l = 1.16 d^(1/3) - 0.16 above (6/29)^3, linear below (display_lightness()).
//
// Thread-safety: reentrant; rows run in parallel when requested, with schedule-independent
// results.

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/camera_color.hpp>
#include <lucent/engine/develop_settings.hpp>
#include <lucent/image/pixel_buffer.hpp>

namespace lucent::engine {

// ITU-R BT.2020 luminance coefficients of linear R, G, B (they sum to 1).
inline constexpr std::array<double, 3> kRec2020Luminance = {0.2627, 0.6780, 0.0593};

// ---- Display lightness ------------------------------------------------------------------------

// CIE 1976 lightness / 100 of a display-linear value `d` relative to white (d = 1 -> 1,
// d = 0.18419 -> 0.5, d = 0 -> 0). The linear segment below (6/29)^3 is extended to negative
// values, so the function is odd-symmetric around 0 only there; it is strictly increasing on
// the whole real line.
[[nodiscard]] double display_lightness(double d) noexcept;
// Exact inverse of display_lightness().
[[nodiscard]] double display_lightness_inverse(double l) noexcept;

// ---- Tone curve -------------------------------------------------------------------------------

// The monotone piecewise-cubic Hermite interpolant of F. N. Fritsch and R. E. Carlson, "Monotone
// Piecewise Cubic Interpolation", SIAM J. Numer. Anal. 17(2), 1980: secants d_k; tangents
// m_k = (d_{k-1} + d_k) / 2 inside, one-sided at the ends, 0 next to a flat interval; then, for
// every interval in order, when alpha^2 + beta^2 > 9 (alpha = m_k / d_k, beta = m_{k+1} / d_k)
// both tangents are scaled by 3 / sqrt(alpha^2 + beta^2). The result passes through every
// point, is C1 and non-decreasing, and stays within [y_k, y_{k+1}] on each interval. Constant
// beyond the first and last points.
class ToneCurve {
public:
    // InvalidArgument (engine.develop.tone_curve) unless the setting satisfies ToneCurveSetting's
    // rules (DevelopSettings::validate()).
    [[nodiscard]] static Expected<ToneCurve> create(const ToneCurveSetting& setting);

    [[nodiscard]] double evaluate(double x) const noexcept;
    // True when the setting was the identity (ToneCurveSetting::is_identity()).
    [[nodiscard]] bool identity() const noexcept { return identity_; }
    [[nodiscard]] const std::vector<double>& tangents() const noexcept { return m_; }

private:
    std::vector<double> x_;
    std::vector<double> y_;
    std::vector<double> m_;
    bool identity_ = false;
};

// ---- Contrast ---------------------------------------------------------------------------------

// Pivot of the contrast curve in display lightness (L* = 50, display-linear 0.184).
inline constexpr double kContrastPivot = 0.5;
// Logistic steepness at contrast +-1: the slope at the pivot is (b/4) / tanh(b/4) = 2.07 at +1
// and its reciprocal, 0.48, at -1.
inline constexpr double kContrastMaxSteepness = 8.0;

// The contrast curve on display lightness l in [0, 1] (values outside are clamped): for
// contrast c > 0, the logistic s(z) = 1 / (1 + e^-z) with steepness b = kContrastMaxSteepness |c|
// normalised to fix 0, the pivot p and 1:
//     f(l) = (s(b (l - p)) - s(-b p)) / (s(b (1 - p)) - s(-b p));
// for c < 0, the exact inverse of the c > 0 curve with the same b (so +c and -c cancel):
//     f(l) = p + logit(s(-b p) + l (s(b (1 - p)) - s(-b p))) / b.
// c = 0 is the identity. Strictly increasing and smooth for every c.
class ContrastCurve {
public:
    // InvalidArgument (engine.develop.contrast) unless contrast is finite and within [-1, 1].
    [[nodiscard]] static Expected<ContrastCurve> create(double contrast);

    [[nodiscard]] double evaluate(double l) const noexcept;
    [[nodiscard]] double contrast() const noexcept { return contrast_; }
    [[nodiscard]] bool identity() const noexcept { return contrast_ == 0.0; }
    // d f / d l at the pivot.
    [[nodiscard]] double slope_at_pivot() const noexcept;

private:
    double contrast_ = 0.0;
    double steepness_ = 0.0;
    double low_ = 0.0;    // s(-b p)
    double range_ = 1.0;  // s(b (1 - p)) - s(-b p)
};

// ---- Base look "neutral" v1 -------------------------------------------------------------------

// Mid-tone placement: scene-linear values between the toe and the top are displayed 0.5 EV
// brighter. ISO 12232 saturation-based speed places a metered 18 % grey at 0.18 / sqrt(2) =
// 12.7 % of saturation (scene-linear 0.127 here, where 1 is the clip level); +0.5 EV puts it at
// display-linear 0.18 (L* 49.6), display middle grey, as camera renderings do.
inline constexpr double kNeutralV1MidOffsetEv = 0.5;
// The toe ends (full offset reached) at most 4.5 EV below scene level 1 and spans 3 EV; below
// it the curve is the identity, so the darkest tones keep their scene-linear values.
inline constexpr double kNeutralV1ToeEndEv = -4.5;
inline constexpr double kNeutralV1ToeLengthEv = 3.0;
// Shortest top segment (EV): below this the top segment becomes a lift (see NeutralV1Curve).
inline constexpr double kNeutralV1MinTopEv = 1.0;

// Branch of the top segment (persisted; append only).
enum class NeutralV1Regime : std::uint8_t {
    Shoulder = 1,  // a roll-off with zero slope at the white point (filmic shoulder)
    Lift = 2,      // headroom too small for a shoulder (strong negative exposure): convex lift
};

[[nodiscard]] std::string_view to_string(NeutralV1Regime regime) noexcept;  // "shoulder", "lift"

// The "neutral" v1 tone curve B_W: scene-linear norm n -> display-linear d, for white point W
// (the scene-linear level of a fully clipped neutral after exposure). Defined in log2 space,
// x = log2 n, y = log2 d, with o = kNeutralV1MidOffsetEv:
//   top length  S = max(kNeutralV1MinTopEv, 2 (log2 W + o)), top start x_s = log2 W - S
//   toe         x_t1 = min(kNeutralV1ToeEndEv, x_s), x_t0 = x_t1 - kNeutralV1ToeLengthEv,
//               h = kNeutralV1ToeLengthEv / 2, k = o / h
//   y = x                                          x <= x_t0             (shadows: identity)
//   y = x + k (x - x_t0)^2 / (2h)                  x_t0 < x <= x_t0 + h  (toe, slope 1 -> 1 + k)
//   y = x + o - k (x_t1 - x)^2 / (2h)              x_t0 + h < x <= x_t1  (toe, slope 1 + k -> 1)
//   y = x + o                                      x_t1 < x <= x_s       (mid-tones: +o EV)
//   y = y_s + t + a t^2, t = x - x_s,              x_s < x < log2 W      (top)
//       y_s = x_s + o, a = -(y_s + S) / S^2
//   y = 0 (d = 1)                                  x >= log2 W           (clipped: display white)
// The curve is continuous and strictly increasing on (0, W), C1 everywhere except at W in the
// Lift regime, and continuous in W. The top slope (in log2/log2 units) falls linearly from 1 to
// e = 1 - 2 (log2 W + o) / S: e = 0 when S = 2 (log2 W + o) (Shoulder: the highlights roll off
// into white with zero slope, the filmic shoulder), e > 0 otherwise (Lift: W below
// 2^(S_min / 2 - o) = 1, a convex top that still reaches white, as the DNG SDK does for negative
// exposure). At the default W = 1 the shoulder spans the top stop: scene 0.5 -> 0.707,
// 0.75 -> 0.94, 1 -> 1. Hue preserving: applied to the value norm (apply_base_look).
class NeutralV1Curve {
public:
    // InvalidArgument (engine.develop.base_look) unless `white` is finite and positive.
    [[nodiscard]] static Expected<NeutralV1Curve> create(double white);

    // d = B_W(n); n <= 0 returns n.
    [[nodiscard]] double evaluate(double n) const noexcept;
    // n with B_W(n) = d for 0 < d < 1; W for d >= 1; d for d <= 0.
    [[nodiscard]] double inverse(double d) const noexcept;

    [[nodiscard]] double white() const noexcept { return white_; }
    [[nodiscard]] NeutralV1Regime regime() const noexcept { return regime_; }
    [[nodiscard]] double top_start_ev() const noexcept { return x_s_; }
    [[nodiscard]] double top_length_ev() const noexcept { return length_; }
    [[nodiscard]] double end_slope() const noexcept { return end_slope_; }
    [[nodiscard]] double toe_start_ev() const noexcept { return x_t0_; }
    [[nodiscard]] double toe_end_ev() const noexcept { return x_t1_; }

private:
    [[nodiscard]] double evaluate_log(double x) const noexcept;  // y(x)
    [[nodiscard]] double inverse_log(double y) const noexcept;   // x(y), y < 0

    double white_ = 1.0;
    double x_w_ = 0.0;
    double x_s_ = 0.0;
    double length_ = 1.0;
    double a_ = 0.0;
    double end_slope_ = 0.0;
    double x_t0_ = 0.0;
    double x_t1_ = 0.0;
    double toe_c_ = 0.0;  // k / (2h)
    // Linear-domain thresholds of the segments that need no logarithm: n <= n_t0_ (identity) and
    // n_t1_ < n <= n_s_ (mid-tones, d = n * mid_gain_); d_t1_ / d_s_ are their images.
    double n_t0_ = 0.0;
    double n_t1_ = 0.0;
    double n_s_ = 0.0;
    double d_t1_ = 0.0;
    double d_s_ = 0.0;
    double mid_gain_ = 1.0;
    NeutralV1Regime regime_ = NeutralV1Regime::Shoulder;
};

// What apply_base_look() did.
struct BaseLookRecord {
    std::string_view id = kNeutralLookId;
    std::uint32_t version = kNeutralLookVersion;
    double white = 1.0;  // W
    NeutralV1Regime regime = NeutralV1Regime::Shoulder;
    double top_start_ev = 0.0;
    double end_slope = 0.0;
    double toe_start_ev = 0.0;
    double toe_end_ev = 0.0;
    std::uint64_t limited_pixels = 0;    // m >= W: mapped to display white
    std::uint64_t negative_samples = 0;  // components below 0 in the output (outside Rec.2020)
};

// Renders `image` (scene_linear_rec2020_d65(), 3 channels) in place with `curve` on the value
// norm and sets its encoding to display_linear_rec2020() (display_rendering.hpp): max(R, G, B)
// <= 1 afterwards, negative components are kept for the exporter. Errors
// (engine.develop.base_look): InvalidArgument for an empty image or another encoding; Internal
// when a non-finite value is produced; Cancelled (the image must then be discarded).
[[nodiscard]] Expected<BaseLookRecord> apply_base_look(ColorImage& image, const NeutralV1Curve& curve,
                                                       const core::CancellationToken& cancel = {},
                                                       bool parallel = true);

// ---- Local highlights and shadows -------------------------------------------------------------

// Largest local gain (EV) at full weight: highlights -1..+1 -> -1.5..+1.5 EV, shadows -1..+1 ->
// -2..+2 EV. The search bounds of blueprint section 6 (highlights -0.75..0, shadows 0..+0.75)
// lie inside.
inline constexpr double kHighlightsMaxEv = 1.5;
inline constexpr double kShadowsMaxEv = 2.0;
// Guided-filter regularisation in EV^2 (log2 luminance squared): local variations with a
// standard deviation well below 0.25 EV are texture and keep their local contrast, edges well
// above it bound the adjustment regions.
inline constexpr double kLocalToneEpsilon = 0.0625;
// Guided-filter radius in full-image pixels: the full image's long edge / 64 (128 px at 8192 px),
// at least 1; proxies scale it (develop.hpp).
inline constexpr double kLocalToneRadiusFraction = 1.0 / 64.0;
// The log luminance is floored 12 EV below the white point, so sensor noise in the deepest
// shadows (where the shadows weight is constant) does not steer the filter.
inline constexpr double kLocalToneFloorEv = -12.0;
// Weight transitions in estimated display EV (0 = display white, -2.47 = display grey 0.18):
// highlights rise from 0 at -3 EV to 1 at 0 EV, shadows fall from 1 at -6.5 EV to 0 at -2.5 EV.
inline constexpr std::array<double, 2> kHighlightsWeightEv = {-3.0, 0.0};
inline constexpr std::array<double, 2> kShadowsWeightEv = {-6.5, -2.5};

// Gain in EV that the local tone operation applies to a pixel whose guided-filter base layer is
// `base_ev` (log2 of the smoothed luminance) with white point log2 W = `white_ev`:
//   e = min(0, max(base_ev + o, base_ev - white_ev))  (estimated display EV under neutral v1:
//       mid-tones are placed +o EV, the white point is display white),
//   gain = kHighlightsMaxEv * highlights * smoothstep(kHighlightsWeightEv, e)
//        + kShadowsMaxEv * shadows * (1 - smoothstep(kShadowsWeightEv, e)),
// smoothstep(lo, hi, e) = t^2 (3 - 2t), t = clamp((e - lo) / (hi - lo), 0, 1). The slope of the
// gain in base_ev stays above -1 for every setting, so the smoothed tones keep their order.
[[nodiscard]] double local_tone_gain_ev(double base_ev, double white_ev, double highlights, double shadows) noexcept;

struct LocalToneParameters {
    double highlights = 0.0;  // [-1, 1]
    double shadows = 0.0;     // [-1, 1]
    double white = 1.0;       // W, scene-linear, finite and positive
    std::int32_t radius = 1;  // guided-filter radius in pixels of this image, >= 1
    double epsilon = kLocalToneEpsilon;
};

struct LocalToneRecord {
    std::uint32_t version = kLocalToneOperationVersion;
    std::int32_t radius = 0;
    double epsilon = 0.0;
    double min_gain_ev = 0.0;  // range of the applied gains
    double max_gain_ev = 0.0;
};

// Applies highlights/shadows to `image` (scene_linear_rec2020_d65(), 3 channels) in place:
// L = log2(max(Y, W 2^kLocalToneFloorEv)); base = guided_filter(L, L, radius, epsilon)
// (self-guided, edge-preserving); every pixel is multiplied by 2^local_tone_gain_ev(base, ...),
// a bounded exposure change that keeps its channel ratios and local detail (L - base). The factor
// is tabulated at 1/256 EV of the base value and interpolated linearly (relative error below
// 2e-6). One float plane plus the filter's two-channel working plane are allocated under
// `limits`.
// Errors (engine.develop.local_tone): InvalidArgument for an empty image, another encoding or
// invalid parameters; LimitExceeded / OutOfMemory; Cancelled.
[[nodiscard]] Expected<LocalToneRecord> apply_local_tone(ColorImage& image, const LocalToneParameters& parameters,
                                                         const image::AllocationLimits& limits = {},
                                                         const core::CancellationToken& cancel = {},
                                                         bool parallel = true);

// ---- Contrast and tone curve (display-lightness curves) ---------------------------------------

struct LightnessCurvesRecord {
    std::uint64_t changed_pixels = 0;  // pixels whose value norm changed
};

// Applies the contrast curve and then the tone curve, both defined on display lightness, to
// scene-linear `image` (scene_linear_rec2020_d65(), 3 channels) in place, by conjugation with
// the base look `base`: for the value norm m > 0,
//     l = display_lightness(base.evaluate(m)), l' = tone(contrast(l)),
//     m' = base.inverse(display_lightness_inverse(l')),
// and the pixel is scaled by m' / m. After the base look the norm's display lightness is
// l' (for neutrals: the displayed L* follows the curves), so both curves act in the
// display-referred lightness domain while running at their place in the scene-linear order.
// m -> m' is evaluated exactly at 512 nodes per octave over the 27 octaves below the white point
// (built per call, in parallel) and interpolated linearly between them, which keeps it monotone;
// norms further below use the exact chain, norms at or above the white point the white's value.
// The interpolation deviates from the exact chain by less than 1e-4 relative (0.00015 EV)
// wherever the curves are smooth (measured worst case 4.1e-5, contrast -1 with a lift-regime
// white point; develop_operations_test.cpp), more only within one node of a slope
// discontinuity (the ends of a tone curve that does not span [0, 1]).
// Either curve may be null (skipped). Errors (engine.develop.lightness): InvalidArgument for an
// empty image or another encoding; Internal for a non-finite result; OutOfMemory; Cancelled.
[[nodiscard]] Expected<LightnessCurvesRecord> apply_lightness_curves(ColorImage& image, const NeutralV1Curve& base,
                                                                     const ContrastCurve* contrast,
                                                                     const ToneCurve* tone,
                                                                     const core::CancellationToken& cancel = {},
                                                                     bool parallel = true);

// ---- Saturation and vibrance ------------------------------------------------------------------

struct ColorAdjustRecord {
    std::uint32_t version = kColorOperationVersion;
    std::uint64_t negative_samples = 0;  // components below 0 after the operation
};

// Chroma scale around the pixel's luminance Y (kRec2020Luminance), in place on scene-linear
// Rec.2020: p' = Y + k (p - Y) with
//     k = (1 + saturation) * (1 + vibrance * (1 - s)^2),
//     s = clamp((max - min) / max, 0, 1) (0 when max <= 0),
// so vibrance acts mostly on muted colours and not at all on fully saturated ones. Neutrals
// (R = G = B) stay exactly neutral (every channel gets the same arithmetic); Y is unchanged up to
// rounding; for a fixed hue the output chroma is non-decreasing in the input chroma.
// Errors (engine.develop.color): InvalidArgument for an empty image, another encoding or values
// outside [-1, 1]; Cancelled.
[[nodiscard]] Expected<ColorAdjustRecord> apply_color(ColorImage& image, double saturation, double vibrance,
                                                      const core::CancellationToken& cancel = {},
                                                      bool parallel = true);

}  // namespace lucent::engine
