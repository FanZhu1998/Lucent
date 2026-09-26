#pragma once

// Reconstruction routes (blueprint section 5, step 3, "Choose the reconstruction route", and
// "Initial demosaicing decision"; Appendix A.3 and A.4).
//
// A route is chosen from the SensorImage's representation and CFA, never forced onto every
// layout:
//   Bayer mosaic      -> BayerMalvarHeCutler: native Malvar-He-Cutler (ICASSP 2004), all four
//                        Bayer phases, whole-sample-symmetric (mirror) borders that keep the CFA
//                        phase. Validated against colour-demosaicing's
//                        demosaicing_CFA_Bayer_Malvar2004 (BSD-3-Clause) on crops of camera
//                        fixtures (fixtures/reference/demosaic). A correctness baseline, not the
//                        final quality choice (AMaZE/RCD comparisons are still to come).
//   X-Trans mosaic    -> XTransBaseline: each missing colour is the mean of the same-colour
//                        samples in the 3 x 3 neighbourhood (the Fujifilm 6 x 6 tile puts every
//                        colour in every 3 x 3 window), a same-colour average over the tile's
//                        period. Correct but BASELINE QUALITY (RouteMaturity::Baseline): it blurs
//                        detail and shows colour artefacts on fine structure; it exists so that
//                        X-Trans files render faithfully until a qualified route is added.
//   LinearColor       -> LinearColorBypass: already full colour; samples pass through.
//   Monochrome        -> MonochromeBypass: no colour reconstruction; the single channel is kept
//                        (the result has ChannelLayout::Mono, not three equal channels).
//   anything else     -> Unsupported (for example a mosaic that is neither Bayer nor X-Trans).
//
// Output: interleaved float32 R, G, B in camera-native primaries (1 channel for monochrome),
// with ColorEncoding camera-native, linear, unbounded, scene-referred; white balanced when the
// input was. Both Bayer greens (Green, Green2) feed the single G output channel. Values are not
// clipped (the linear kernels overshoot at edges, giving values below 0 and above 1). The
// pixel grid is that of the SensorImage (visible area, stored orientation).
//
// Thread-safety: reentrant; rows are processed with std::execution::par when requested and the
// results do not depend on scheduling.

#include <cstdint>
#include <optional>
#include <string_view>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/sensor.hpp>
#include <lucent/image/cfa.hpp>
#include <lucent/image/color_encoding.hpp>
#include <lucent/image/pixel_buffer.hpp>

namespace lucent::engine {

// Persisted route identifiers (append only).
enum class DemosaicRoute : std::uint8_t {
    BayerMalvarHeCutler = 1,
    XTransBaseline = 2,
    LinearColorBypass = 3,
    MonochromeBypass = 4,
};

// How far a route's output can be trusted.
enum class RouteMaturity : std::uint8_t {
    // Reproduces a published algorithm with numerical parity against a reference implementation.
    Validated = 1,
    // A correct reconstruction of the layout, but not quality-qualified: expect softness and
    // colour artefacts on fine detail. Must be surfaced as such (never presented as final).
    Baseline = 2,
    // No reconstruction was needed; samples pass through unchanged.
    Bypass = 3,
};

[[nodiscard]] std::string_view to_string(DemosaicRoute route) noexcept;     // "bayer_malvar_he_cutler", ...
[[nodiscard]] std::string_view to_string(RouteMaturity maturity) noexcept;  // "validated", ...

// Algorithm versions (bump when output values change for the same input).
inline constexpr std::uint32_t kMalvarHeCutlerVersion = 1;
inline constexpr std::uint32_t kXTransBaselineVersion = 1;
inline constexpr std::uint32_t kBypassVersion = 1;

// What reconstructed an image.
struct DemosaicRecord {
    DemosaicRoute route = DemosaicRoute::BayerMalvarHeCutler;
    RouteMaturity maturity = RouteMaturity::Validated;
    // Stable algorithm identifier: "malvar-he-cutler-2004", "xtrans-3x3-same-colour-mean",
    // "linear-color-bypass", "monochrome-bypass".
    std::string_view algorithm;
    std::uint32_t algorithm_version = 0;
    // Border rule (developer-facing English), empty for bypass routes.
    std::string_view border_handling;
    // Mosaic routes: the CFA at the output origin; empty for bypass routes.
    image::CfaPattern cfa;
};

// The route for `sensor`, or Unsupported (engine.demosaic.route) when there is none.
[[nodiscard]] Expected<DemosaicRoute> select_demosaic_route(const SensorImage& sensor);

struct DemosaicOptions {
    image::AllocationLimits limits;  // bound for the output buffer
    bool parallel = true;            // false runs on the calling thread (identical results)
};

// A reconstructed camera-RGB (or monochrome) image and its provenance.
struct DemosaicedImage {
    image::ImageBuffer<float> pixels;  // 3 channels R, G, B, or 1 for MonochromeBypass
    // camera_native_linear() (Rgb layout) or raw::camera_native_monochrome() (Mono layout).
    image::ColorEncoding encoding = image::ColorEncoding::camera_native_linear();
    DemosaicRecord demosaic;
    NormalizationRecord normalization;                // carried from the SensorImage
    std::optional<WhiteBalanceRecord> white_balance;  // carried; nullopt when not balanced
    // The SensorImage's per-site RAW saturation flags, in the same pixel grid (for a Bayer or
    // X-Trans route, bit c marks that site's own channel c; reconstructed values near flagged
    // sites are also affected).
    SaturationMask saturation;
};

// Reconstructs `sensor` with the route select_demosaic_route() picks.
// Errors (Error::where in parentheses): Unsupported (engine.demosaic.route) without a route;
// InvalidArgument (engine.demosaic.input) for an empty image, a CFA the route cannot use, or an
// image smaller than the route's minimum (3 x 3 for Bayer, 6 x 6 for X-Trans); LimitExceeded /
// OutOfMemory for the output; Cancelled (engine.demosaic.run).
[[nodiscard]] Expected<DemosaicedImage> demosaic(const SensorImage& sensor, const DemosaicOptions& options = {},
                                                 const core::CancellationToken& cancel = {});

// As above, consuming `sensor`: bypass routes move its buffer instead of copying it, and mosaic
// routes release the mosaic as soon as the reconstruction is done. On failure `sensor` may have
// been moved from.
[[nodiscard]] Expected<DemosaicedImage> demosaic(SensorImage&& sensor, const DemosaicOptions& options = {},
                                                 const core::CancellationToken& cancel = {});

// ---- Low-level kernels (views; used by demosaic() and by parity tests) ------------------------

// Malvar-He-Cutler on a single-channel Bayer mosaic. `cfa` is the Bayer pattern relative to
// mosaic pixel (0, 0); `rgb` has the mosaic's size and 3 channels and must not overlap it.
// Pixels within 2 of an edge read mirrored neighbours (index -k -> k, n - 1 + k -> n - 1 - k),
// which keeps the CFA phase, so a uniform colour field is reproduced exactly everywhere.
// InvalidArgument (engine.demosaic.input): cfa not Bayer, mosaic smaller than 3 x 3, not one
// channel, or rgb of the wrong shape. Cancelled (engine.demosaic.run).
[[nodiscard]] Status demosaic_bayer_mhc(image::ConstImageView<float> mosaic, const image::CfaPattern& cfa,
                                        image::ImageView<float> rgb, const core::CancellationToken& cancel = {},
                                        bool parallel = true);

// X-Trans baseline on a single-channel mosaic whose `cfa` (relative to pixel (0, 0)) satisfies
// CfaPattern::is_xtrans(). Each missing colour is the mean (summed in row-major order, then
// divided by the count) of the same-colour samples in the smallest square window around the
// pixel, clipped to the image, that holds that colour: 3 x 3 everywhere inside the image, larger
// (up to 11 x 11) only where the image border cuts the window. The site's own colour is copied.
// InvalidArgument (engine.demosaic.input): cfa not X-Trans, mosaic smaller than 6 x 6, not one
// channel, or rgb of the wrong shape. Cancelled (engine.demosaic.run).
[[nodiscard]] Status demosaic_xtrans_baseline(image::ConstImageView<float> mosaic, const image::CfaPattern& cfa,
                                              image::ImageView<float> rgb,
                                              const core::CancellationToken& cancel = {}, bool parallel = true);

}  // namespace lucent::engine
