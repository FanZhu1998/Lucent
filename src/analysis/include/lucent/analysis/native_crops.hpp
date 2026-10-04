#pragma once

// Deterministic native-resolution crops (blueprint section 6, step 4: "Recheck only the leading
// three at analysis resolution and on deterministic native-resolution crops covering detail,
// shadows, highlights, and strong edges"; metric NativeDetail).
//
// Selection (select_native_crops), on the analysis image's stage one: the image is divided into
// square tiles that correspond to kNativeCropSize full-resolution pixels; per tile, from the scene
// signal (balanced green) and the value norm against the as-shot clip level:
//   Detail      the largest mean |grad log2 signal| among tiles with < 5 % clipped and < 20 % dark;
//   Shadows     the lowest mean log2 signal among tiles with < 1 % clipped and < 50 % dark;
//   Highlights  the most pixels within 3 EV below the clip among tiles with < 50 % clipped;
//   Edges       the largest |grad log2 signal| among tiles with < 50 % clipped;
// in this order, each excluding the tiles already chosen, ties to the lowest tile index; a kind
// without an eligible tile is skipped. The crop is centred on its tile in full-resolution upright
// coordinates (clamped inside the image). Clipped: value norm >= 0.98 clip or a saturated site;
// dark: signal below 4 sigma of the noise model or 2^-12.
//
// Rendering: each crop is cut from the full-resolution stage one with a halo of
// 2 local_tone_radius(full, 1) + kProfileReach + 2 pixels (the guided filter's two box passes need
// 2r, the edge profiles reach kProfileReach beyond the evaluation region), so stage two renders the
// evaluation region exactly as it renders the full image wherever the halo lies inside the image
// (where the crop meets the image border, the border is the image's own). Every measurement below
// reads only the evaluation region plus kProfileReach + 2 pixels (NativeCrop::measured: the edge
// profiles' farthest bilinear sample and the 3 x 3 neighbourhoods), and is computed from that
// region's pixels alone, in coordinates relative to it. Stage two is per pixel except for the
// local-tone guided filter (develop.hpp), so settings without local tone (highlights and shadows
// both 0) can be rendered on the `measured` rectangle alone ("halo-free" crop) and measure exactly
// as on the full crop; settings with local tone need the halo.
//
// Measurements (measure_crop) on the evaluation region, in display lightness l = L*/100 of the
// luminance (L* units where stated):
//   texture         mean |l - mean3x3(l)|, L* units (the detail crop's local detail);
//   noise           1.4826 median |l - mean3x3(l)|, L* units (the shadow crop's texture as a noise
//                   indicator: it includes fine texture, so only its change is meaningful);
//   halo            on the baseline's strong edges (Sobel magnitude of l >= max(kMinEdgeSobel, p99), at most
//                   1024 by a fixed stride), the mean lightness near the edge (2, 3, 4 px along the
//                   gradient, bilinear) and far from it (16, 24, 32 px) on both sides; an edge counts
//                   when its near sides differ by at least kMinEdgeContrast and at least one side is a
//                   plateau in the baseline (near and far within kPlateauTolerance: thin lines and gaps
//                   are not). Per edge, the largest |(near - near_base) - (far - far_base)| over its
//                   plateau sides: how much more (or less) the edit changed the lightness next to the
//                   edge than away from it. Averaged over edges, in L* units. It is 0 for the
//                   baseline and close to 0 for any per-pixel tone change (a plateau side's near and
//                   far lightness differ by at most kPlateauTolerance), and sensor noise cancels
//                   because both renders share the pixels; local tone (guided filter) halos and
//                   gradient reversals show as a positive value;
//   clipping        pixels at output code 255 (metric_contract.hpp), and newly clipped vs baseline.
//
// Thread-safety: reentrant.

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <lucent/analysis/render_metrics.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/camera_color.hpp>
#include <lucent/engine/develop.hpp>
#include <lucent/image/geometry.hpp>

namespace lucent::analysis {

inline constexpr std::int32_t kNativeCropSize = 256;  // evaluation region, full-resolution pixels
inline constexpr std::int32_t kProfileReach = 32;     // edge profiles, pixels
inline constexpr double kMinEdgeContrast = 0.10;      // lightness step between the near sides
// Sobel magnitude floor of an edge candidate (4 x the lightness step per pixel: 0.15 is 3.75 L* per
// pixel, which soft native-resolution edges of real lenses still reach).
inline constexpr double kMinEdgeSobel = 0.15;
inline constexpr double kPlateauTolerance = 0.03;         // near vs far lightness of a plateau side
inline constexpr std::uint64_t kMinCropEdgeSamples = 16;  // a crop's halo counts from this many edges

// Persisted (append only).
enum class CropKind : std::uint8_t {
    Detail = 1,
    Shadows = 2,
    Highlights = 3,
    Edges = 4,
};

[[nodiscard]] std::string_view to_string(CropKind kind) noexcept;  // "detail", ...

struct NativeCrop {
    CropKind kind = CropKind::Detail;
    image::PixelRect evaluation;  // full-resolution upright coordinates
    image::PixelRect rendered;    // the evaluation region with its halo, inside the image
    // The evaluation region with kNativeMeasureMargin pixels, inside the image: every pixel a
    // measurement reads (the halo-free render of settings without local tone).
    image::PixelRect measured;
    double statistic = 0.0;  // the selection statistic of its tile

    friend bool operator==(const NativeCrop&, const NativeCrop&) = default;
};

// Margin of NativeCrop::measured around the evaluation region.
inline constexpr std::int32_t kNativeMeasureMargin = kProfileReach + 2;

// The halo of a crop of an image whose upright full-resolution size is `full`.
[[nodiscard]] std::int32_t native_crop_halo(image::Size full) noexcept;

// Selects up to four crops (see the file comment). `analysis` is the analysis image's stage one,
// `context` its context, `full` the upright full-resolution size; `crop_size` is clamped to the
// image. Returns no crops when the image has no whole tile.
[[nodiscard]] std::vector<NativeCrop> select_native_crops(const engine::StageOneImage& analysis,
                                                          const ImageContext& context, image::Size full,
                                                          std::int32_t crop_size = kNativeCropSize);

struct EdgeSample {
    float x = 0.0f;  // relative to the evaluation region's origin
    float y = 0.0f;
    float nx = 0.0f;  // unit gradient direction
    float ny = 0.0f;
    // The baseline's mean lightness near (2-4 px) and far (16-32 px) on the + and - sides.
    float near_plus = 0.0f;
    float far_plus = 0.0f;
    float near_minus = 0.0f;
    float far_minus = 0.0f;
    bool plateau_plus = false;
    bool plateau_minus = false;
};

// The baseline's per-crop data that candidates are compared with.
struct CropFeatures {
    CropKind kind = CropKind::Detail;
    std::vector<EdgeSample> edges;
    std::vector<std::uint8_t> clipped;  // evaluation region, row-major
};

struct CropMeasurement {
    CropKind kind = CropKind::Detail;
    std::uint64_t pixels = 0;  // evaluation region
    double texture_lstar = 0.0;
    double noise_lstar = 0.0;
    std::uint64_t edge_samples = 0;
    double halo_lstar = 0.0;
    std::uint64_t highlight_clipped = 0;
    std::uint64_t new_highlight_clipped = 0;
    std::uint64_t nonfinite_samples = 0;
};

// Measures the render of `crop` (display-linear Rec.2020, the size of crop.rendered). Without
// `baseline` the render is the baseline's and `features` (when not null) receives its edges and
// clipping. Errors (analysis.native): InvalidArgument for another size or encoding; OutOfMemory.
[[nodiscard]] Expected<CropMeasurement> measure_crop(const engine::ColorImage& display,
                                                     const NativeCrop& crop, const CropFeatures* baseline,
                                                     CropFeatures* features);

// As above for a render of the rectangle `rendered` (full-resolution upright coordinates; the
// display's size), which must contain crop.measured: crop.rendered, crop.measured (the halo-free
// render) or anything between. The result depends only on the pixels of crop.measured.
[[nodiscard]] Expected<CropMeasurement> measure_crop(const engine::ColorImage& display,
                                                     const image::PixelRect& rendered, const NativeCrop& crop,
                                                     const CropFeatures* baseline, CropFeatures* features);

// A candidate's crops compared with the baseline's (both in crop order).
struct NativeComparison {
    bool available = false;          // at least one crop measured
    std::uint64_t edge_samples = 0;  // over every crop
    double halo_lstar = 0.0;         // the largest crop halo (crops with kMinCropEdgeSamples edges)
    bool detail_available = false;
    double detail_ratio = 1.0;  // texture, candidate / baseline
    bool shadow_available = false;
    double shadow_texture_lstar = 0.0;
    double baseline_shadow_texture_lstar = 0.0;
    double shadow_texture_ratio = 1.0;
    double highlight_new_clip_fraction = 0.0;
    std::uint64_t nonfinite_samples = 0;
};

[[nodiscard]] NativeComparison compare_native(std::span<const CropMeasurement> candidate,
                                              std::span<const CropMeasurement> baseline);

}  // namespace lucent::analysis
