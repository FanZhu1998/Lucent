#pragma once

// Display rendering "Neutral v0" (blueprint section 5, steps 6-7): the provisional, parameter-free
// look that turns scene-linear working-space data into display-referred linear Rec.2020 for the
// export hand-off, until the W6 editor provides real photographic operations. It is deliberately
// minimal and is marked provisional in every RenderReport.
//
// For every pixel (R, G, B) of scene-linear Rec.2020 / D65:
//   1. Exposure: multiply by gain = 2^(exposure_ev + baseline_exposure_ev). baseline_exposure_ev
//      is the DNG BaselineExposure when the file has one (0 otherwise); exposure_ev is the user's
//      (0 by default). At gain 1 a neutral that brings the first camera channel to its white
//      level has Y = 1 (camera_color.hpp, "Exposure zero point").
//   2. Highlight mapping on m = max(R, G, B), hue preserving: the pixel is scaled by f(m) / m, so
//      channel ratios (hue and saturation in the linear sense) are kept. The white point
//          W = gain * scene_white
//      is the display-linear level of a fully clipped neutral after the gain (scene_white: the
//      white-balance clip level times the calibration's neutral_scale, 1 when highlights were not
//      clipped). f maps W to 1, so fully clipped highlights are display white whatever the
//      exposure, and nothing is ever brighter than them. Three branches (NeutralV0Curve):
//        * Shoulder (W > 1, positive exposure), knee k (0 < k < 1):
//            f(m) = m                                                     for m <= k
//            f(m) = k + (1 - k) * (1 - (1 - (m - k) / (W - k))^p)         for k < m < W,
//                   p = (W - k) / (1 - k)
//          continuous with a continuous first derivative (slope 1 at k, 0 at W), strictly
//          increasing and concave on [k, W].
//        * Identity (W = 1): f(m) = m; pixels with m < 1 are left bit-exact.
//        * Lift (W < 1, negative exposure, for example a negative DNG BaselineExposure): the
//          Adobe DNG SDK's exposure tone curve (dng_render.cpp, dng_function_exposure_tone: "if
//          there is any negative exposure compensation to perform ... we fake this by darkening
//          the tone curve"), with slope s = W on u = m / W (u = 1 is the clip level, as the SDK's
//          input 1.0 is):
//            f(m) = m                                                     for m <= W / 4
//            f(m) = a u^2 + b u + c                                       for W / 4 < m < W,
//                   a = 16/9 (1 - W), b = W - a / 2, c = 1 - a - b
//          so the plain gain applies up to two stops below the clip point and the top two stops
//          are lifted to reach white; continuous with a continuous first derivative (slope 1 at
//          W / 4), strictly increasing and convex on [W / 4, W]. With scene_white = 1 (the
//          default highlight policy) s = 2^(exposure), exactly the SDK's curve.
//        * f(m) = 1 for m >= W (limited) on every branch.
//      W tends to 1 from either side the curve tends to the identity, so the rendering is
//      continuous in the exposure.
//   3. Pixels with m <= 0 are left unchanged; negative components are kept (scaled with their
//      pixel): they are outside the Rec.2020 gamut and are clipped per channel by the output
//      encoder (export encode_for_output), not here.
// The output is display-referred linear Rec.2020 / D65 (display_linear_rec2020()): max(R, G, B)
// <= 1, components may be negative.
//
// Provisional: the knee, the shoulder and lift shapes and the absence of any tone curve, contrast
// or saturation handling are placeholders for the W6 recipe operations.
//
// Thread-safety: reentrant; rows run in parallel when requested with schedule-independent
// results.

#include <array>
#include <cstdint>
#include <string_view>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/camera_color.hpp>
#include <lucent/image/color_encoding.hpp>

namespace lucent::engine {

// Version of the Neutral v0 semantics (bump when the same input renders differently).
//   1: W = max(1, gain * scene_white): with W clamped to 1, clipped highlights stayed at
//      gain * scene_white (grey) whenever that was below 1.
//   2: W = gain * scene_white on every branch; the Lift branch for W < 1.
inline constexpr std::uint32_t kNeutralV0Version = 2;
// Default shoulder knee (display-linear value where compression starts).
inline constexpr double kNeutralV0DefaultKnee = 0.8;
// Lift branch: fraction of W up to which the plain gain applies (two stops below the clip
// point, the DNG SDK's crossover).
inline constexpr double kNeutralV0LiftBreak = 0.25;

// Display-referred linear Rec.2020 / D65: the encoding handed to the exporter. RGB layout,
// linear transfer, unbounded range (out-of-gamut components may be negative), display reference.
[[nodiscard]] constexpr image::ColorEncoding display_linear_rec2020() noexcept {
    image::ColorEncoding e = image::ColorEncoding::scene_linear_rec2020_d65();
    e.reference = image::ImageReference::Display;
    return e;
}

// Branch of the highlight mapping that a render used (persisted; append only).
enum class NeutralV0Curve : std::uint8_t {
    Identity = 1,  // W = 1
    Shoulder = 2,  // W > 1
    Lift = 3,      // W < 1
};

[[nodiscard]] std::string_view to_string(NeutralV0Curve curve) noexcept;  // "identity", "shoulder", "lift"

struct NeutralV0Parameters {
    double exposure_ev = 0.0;             // user exposure, EV, finite, within [-16, 16]
    double baseline_exposure_ev = 0.0;    // DNG BaselineExposure applied, EV, finite, within [-16, 16]
    double knee = kNeutralV0DefaultKnee;  // 0 < knee < 1
    double scene_white = 1.0;             // scene-linear level of a clipped neutral, finite, > 0
};

// What render_neutral_v0() did.
struct NeutralV0Record {
    std::uint32_t version = kNeutralV0Version;
    NeutralV0Parameters parameters;
    double gain = 1.0;   // 2^(exposure_ev + baseline_exposure_ev)
    double white = 1.0;  // W = gain * scene_white (display-linear level mapped to 1)
    NeutralV0Curve curve = NeutralV0Curve::Identity;
    double exponent = 1.0;              // p (Shoulder); 1 otherwise
    std::array<double, 3> lift{};       // a, b, c (Lift); 0 otherwise
    std::uint64_t compressed_pixels = 0;  // Shoulder: k < m < W
    std::uint64_t lifted_pixels = 0;      // Lift: W / 4 < m < W
    std::uint64_t limited_pixels = 0;     // m >= W (scaled to max 1)
    std::uint64_t negative_samples = 0;   // components below 0 in the output (out of Rec.2020 gamut)
};

// The highlight mapping f(m) for white point W (see the file comment; the branch follows from
// W). Preconditions: 0 < knee < 1; white finite and > 0; m finite.
[[nodiscard]] double neutral_v0_curve(double m, double knee, double white) noexcept;

// InvalidArgument (engine.render.neutral) unless the parameters are finite and within the ranges
// documented on NeutralV0Parameters.
[[nodiscard]] Status validate(const NeutralV0Parameters& parameters);

// Renders `image` (scene_linear_rec2020_d65, 3 channels) in place and sets its encoding to
// display_linear_rec2020(). Errors (engine.render.neutral): InvalidArgument for an empty image,
// another encoding, invalid parameters or a white point that is not a finite positive number;
// Internal when a non-finite value is produced (never expected: the input of the pipeline is
// finite); Cancelled (the image is then partially rendered and must be discarded).
[[nodiscard]] Expected<NeutralV0Record> render_neutral_v0(ColorImage& image,
                                                          const NeutralV0Parameters& parameters,
                                                          const core::CancellationToken& cancel = {},
                                                          bool parallel = true);

}  // namespace lucent::engine
