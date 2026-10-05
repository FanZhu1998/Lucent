#pragma once

// Guided chroma denoising (DevelopSettings::chroma_denoise, kChromaDenoiseOperationVersion 1;
// blueprint section 5, "Noise, masks, and geometry"; ADR 0015). Lucent engineering built on the
// guided-filter primitive (guided_filter.hpp, the OpenCV conventions), not a pretrained feature.
//
// Place: stage two, right after camera colour, the working space and exposure (scene-linear
// Rec.2020 / D65), before local tone, masks, curves, the base look and any later learned or
// sharpening stage (develop.hpp).
//
// Definition, per pixel p = (R, G, B), with W the white point (DevelopReport::white):
//   Y  = 0.2627 R + 0.6780 G + 0.0593 B             luminance (kRec2020Luminance)
//   D  = max(Y, W 2^kChromaDenoiseFloorEv)          deep shadows and negative luminance use the floor
//   L  = log2 D                                     log luminance, in EV
//   I  = median3x3(L)                               the guide (edge pixels repeated)
//   D_c = max(D, kChromaScaleFraction max(|R|, |G|, |B|))   the brightness the chroma is relative to
//   c  = ((R - Y) / D_c, (B - Y) / D_c)             two colour-difference planes
//   q  = guided filter of c with guide I            K. He, J. Sun and X. Tang, Algorithm 1, as
//                                                   guided_filter.hpp defines it (box means over
//                                                   (2 radius + 1)^2 windows, borders mirrored,
//                                                   eps = epsilon in EV^2), the box means of each
//                                                   step computed jointly for both planes
//   c' = c + strength (q - c)                       the conservative blend
//   R' = Y + c'_r D_c,  B' = Y + c'_b D_c,  G' = (Y - 0.2627 R' - 0.0593 B') / 0.6780
// so the luminance of the result is Y (to the float rounding of G', about 1e-7 of the largest
// channel): this control removes colour noise and does not claim to remove luminance noise.
// Measuring the guide in EV and the chroma relative to the luminance makes the filter behave the
// same at every brightness and exposure: an edge between two tones is kept when its contrast
// (in stops) is well above sqrt(epsilon), a surface of one hue keeps its colour across a
// luminance edge exactly (its relative chroma does not change), and colour noise is averaged over
// the window. The guide is the 3 x 3 median of the log luminance rather than the log luminance
// itself: a pixel's chroma noise is correlated with its own luminance noise (both come from the
// same channel noise), and with a noisy guide the filter's regression would explain that part of
// the chroma noise by the guide and keep it (measured: chroma noise reduced 5 to 8 times instead
// of 23 on a flat field); the median removes most of the guide's noise and its coupling to
// the pixel's own noise while keeping step edges where they are (it removes luminance details one
// pixel wide, along which colour is then averaged too). A colour edge without a luminance edge
// (isoluminant) is smoothed over the radius: the limitation of luminance-guided chroma filtering.
// Next to a strong colour and luminance edge the luminance texture of either side is copied into
// the chroma in proportion to the edge's chroma/luminance slope (the guided filter's local linear
// model), about 1 % of the edge's chroma step for a 2.7 EV edge in the unit tests.
//
// Units: the radius is in pixels of the image being filtered; DevelopSettings stores it in
// full-image pixels and chroma_denoise_radius() scales it for proxies (at least 1). Downsampling
// and filtering do not commute: the proxy result is compared with the full-resolution one in
// develop_fixture_test.cpp (CIEDE2000 statistics stated there and in ADR 0015).
// The colour differences are relative to D_c rather than to the luminance alone: proportional to a colour's brightness like Y (so a hue still keeps
// its colour across luminance edges), but it does not collapse for saturated colours of low
// luminance (a pure blue has Y = 0.0593 B), which keeps the relative differences bounded (|c| <= 8)
// and stops one saturated pixel from tinting its neighbours (measured on the R5: the largest
// proxy-against-export CIEDE2000 fell from 29 to 13).
//
// Memory: processed in horizontal bands of `band_rows` output rows (kChromaDenoiseBandRows, at
// least 4 halos) with a halo of 2 radius + 1 rows above and below (the guided filter is two box
// passes of the radius and the median reaches one row further, so the halo makes each band's
// inner rows equal to a whole-image filter); the eight working planes (log luminance, guide and
// six for the guided filter's joint box means) cover one band and its halos
// (chroma_denoise_working_bytes()), not the whole image. A band's result equals the whole-image
// filter up to the float rounding of the running box sums, which start at each region's top; the
// band height is a constant, so results are deterministic.
//
// Thread-safety: reentrant; rows run in parallel when requested, with schedule-independent
// results.

#include <cstdint>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/camera_color.hpp>
#include <lucent/engine/develop_settings.hpp>
#include <lucent/image/pixel_buffer.hpp>

namespace lucent::engine {

// Guided-filter regularisation in EV^2 (log2 luminance squared): luminance edges with a local
// standard deviation well above 0.1 EV bound the chroma filtering.
inline constexpr double kChromaDenoiseEpsilon = 0.01;
// The luminance floor, in EV below the white point (as the local tone operation's).
inline constexpr double kChromaDenoiseFloorEv = -12.0;
// The colour differences are relative to max(Y, kChromaScaleFraction x max(|R|, |G|, |B|), floor).
inline constexpr double kChromaScaleFraction = 0.25;
// Output rows per band (more when four halos are larger).
inline constexpr std::int32_t kChromaDenoiseBandRows = 256;

// Guided-filter radius in pixels of an image at `scale` (its long edge / the full image's long
// edge) for a radius of `radius_full` full-image pixels: the window of 2 r + 1 pixels scales with
// the image, max(1, round(((2 radius_full + 1) scale - 1) / 2)) (radius_full at scale 1; 6 -> 1 and
// 8 -> 2 at scale 0.25, whose pixels each average 4 x 4 full-resolution pixels already).
[[nodiscard]] std::int32_t chroma_denoise_radius(double radius_full, double scale) noexcept;

struct ChromaDenoiseParameters {
    double strength = 0.0;    // [0, 1]; 0 leaves the image unchanged
    std::int32_t radius = 1;  // pixels of this image, >= 1
    double white = 1.0;       // W, finite and positive
    double epsilon = kChromaDenoiseEpsilon;
    // Output rows per band; 0 processes the whole image as one band (tests).
    std::int32_t band_rows = kChromaDenoiseBandRows;
};

struct ChromaDenoiseRecord {
    std::uint32_t version = kChromaDenoiseOperationVersion;
    double strength = 0.0;
    std::int32_t radius = 0;
    double epsilon = 0.0;
    std::int32_t band_rows = 0;  // as used (0: no band was processed)
    std::int32_t halo_rows = 0;
    std::int32_t bands = 0;
    std::uint64_t working_bytes = 0;  // the working planes of one band
};

// Working memory of apply_chroma_denoise() for an image of `size` and `radius` (bytes).
[[nodiscard]] std::uint64_t chroma_denoise_working_bytes(image::Size size, std::int32_t radius,
                                                         std::int32_t band_rows = kChromaDenoiseBandRows) noexcept;

// Applies the definition above to `image` (scene_linear_rec2020_d65(), 3 channels) in place.
// Errors (engine.develop.chroma_denoise): InvalidArgument for an empty image, another encoding or
// invalid parameters; Internal for a non-finite result; LimitExceeded / OutOfMemory; Cancelled
// (the image must then be discarded).
[[nodiscard]] Expected<ChromaDenoiseRecord> apply_chroma_denoise(ColorImage& image,
                                                                 const ChromaDenoiseParameters& parameters,
                                                                 const image::AllocationLimits& limits = {},
                                                                 const core::CancellationToken& cancel = {},
                                                                 bool parallel = true);

}  // namespace lucent::engine
