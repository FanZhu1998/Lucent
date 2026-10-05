#pragma once

// Masks and local adjustments (DevelopSettings::local_adjustments, kLocalAdjustmentsOperationVersion
// 1; blueprint section 2, "brush, linear-gradient, and radial masks with feathering", and section
// 5, "Noise, masks, and geometry"; ADR 0015).
//
// Coordinates. Masks are defined in the normalised upright frame U (develop_settings.hpp,
// develop_geometry.hpp): a MaskPoint (x, y) is the pixel point (x W, y H) of the upright image
// at the render scale (W x H, the full image or a proxy) and a normalised length l is l x max(W,
// H) pixels. Shapes are therefore rasterised from their definitions at every render scale
// (never resampled from another scale), before the geometry operation: the crop, the straighten
// angle and the quarter turns never move a mask relative to the image content.
//
// Rasterisation (coverage m in [0, 1], evaluated at pixel centres (i + 0.5, j + 0.5), double
// precision, deterministic):
//   * Linear gradient: t = ((p - s) . (e - s)) / |e - s|^2 in pixels; m = 1 - smoothstep(0.5 -
//     h, 0.5 + h, t), h = max(feather / 2, 0.5 / |e - s|) (at least one pixel wide).
//   * Radial: rho as RadialGradientMask defines it; m = 1 - smoothstep(r0, r1, rho) with (r0, r1)
//     = (1 - feather, 1), widened about its middle to 1 / min(a, b) (one pixel along the minor
//     semi-axis) when narrower.
//   * Brush: per stroke, d = the distance from the pixel centre to the stroke's polyline (pixels)
//     and a = flow (1 - smoothstep(h R, R, d)), the band (h R, R) widened about its middle to one
//     pixel when narrower; strokes compose in order, Add m' = m + a (1 - m), Erase m' = m (1 - a).
//   smoothstep(lo, hi, x) = u^2 (3 - 2 u), u = clamp((x - lo) / (hi - lo), 0, 1).
// Feather widths are fractions of the shape's own size (normalised lengths), i.e. full-image
// pixels scaled with the render, so a proxy's mask is the full-resolution mask seen at the proxy's
// scale; only the one-pixel antialiasing minimum depends on the scale. Proxy against full
// resolution: a proxy raster is the shape evaluated at the proxy's pixel centres, not the mean
// over the full-resolution pixels each covers; it differs from the box-downscaled full-resolution
// raster by the smoothstep's curvature over a proxy pixel (feathered masks at scale 0.25: mean
// 3e-6 to 1.3e-4, max 1e-5 to 0.025, the largest for a 10-pixel erase band) and by the
// antialiasing ramp's scale (one-pixel hard edges: mean below 4e-4, max 0.06 to 0.14 on edge
// pixels only); local_adjustments_test.cpp gates them (mean 1e-3; max 0.04 and 0.2).
//
// Weight: w = opacity (invert ? 1 - m : m), 0 for a disabled adjustment.
//
// Local controls (apply_local_adjustments), in scene-linear Rec.2020 after the global local tone
// and before contrast, the tone curve, saturation and the base look (develop.hpp). For every
// pixel the weighted controls of all masks with an effect are summed (E = sum w_k exposure_k,
// likewise C, H, S, Sat, T, G), each sum limited to its control's range; a pixel where every
// weight is 0 is left untouched (bit-exact). Then, in this order:
//   1. white shift: p_c *= g_c / (Y . g), g_c = 2^(T a_c + G b_c), with the luminance-normalised
//      log2 gain axes of local_white_axes(): temperature +1 = the von Kries gains (in Rec.2020)
//      from a white kLocalTemperatureMired mired bluer than D65 to D65 (warmer), tint +1 = those
//      from a white kLocalTintUnits DNG tint units greener (towards magenta, as the white-balance
//      tint);
//   2. a luminance gain 2^ev keeping channel ratios, ev = E + tone + contrast:
//        tone     = kHighlightsMaxEv H hw(b) + kShadowsMaxEv S sw(b): the weights of the global
//                   highlights/shadows operation (local_tone_gain_ev(), develop_operations.hpp)
//                   at b = the guided-filter base layer of the image's log luminance (radius and
//                   epsilon of the global operation) plus E;
//        contrast = C kLocalContrastStrength kLocalContrastRangeEv tanh(x / kLocalContrastRangeEv),
//                   x = log2(Y_after / P) the pixel's EV relative to P = the scene level the base
//                   look displays as kLocalContrastPivotDisplay (middle grey), Y_after its
//                   luminance after the exposure and tone gains (floored at W 2^-12): a monotone
//                   S-curve in EV (slope 1 + 0.6 C at the pivot, at most +-1.2 EV far from it);
//   3. saturation: p = Y + (1 + Sat) (p - Y) around the pixel's luminance Y.
// The base layer is computed only when a mask uses highlights or shadows, over the union of those
// masks' supports plus a halo of 2 radius pixels (two box passes), so it equals a whole-image base
// layer wherever a weight is not 0 (up to float rounding).
//
// Memory: linear and radial masks are evaluated analytically (no plane); a brush mask is a float
// plane over its bounding box at the render scale (MaskRaster), cached by its shape and size in a
// MaskRasterCache when the caller provides one; the base layer needs three float planes over its
// region (local_adjustments_working_bytes() estimates both from the shapes' extents).
//
// Thread-safety: reentrant; a MaskRasterCache is internally synchronised.

#include <array>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/camera_color.hpp>
#include <lucent/engine/develop_operations.hpp>
#include <lucent/engine/develop_settings.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/image/pixel_buffer.hpp>

namespace lucent::engine {

// ---- Rasterisation ------------------------------------------------------------------------------

// The coverage of one mask shape on an upright image of `size` pixels.
struct MaskRaster {
    image::Size size;                    // the upright render size it was made for
    image::PixelRect bounds;             // coverage is 0 outside (empty: 0 everywhere)
    image::ImageBuffer<float> coverage;  // bounds.width x bounds.height, 1 channel

    // Coverage of pixel (x, y) of the upright image (0 outside the bounds).
    [[nodiscard]] float at(std::int32_t x, std::int32_t y) const noexcept {
        if (!bounds.contains(image::Point{x, y})) {
            return 0.0f;
        }
        return coverage.at(x - bounds.x, y - bounds.y, 0);
    }
    [[nodiscard]] std::uint64_t byte_size() const noexcept { return coverage.byte_size(); }
};

// The pixels of an upright image of `size` where the coverage of `shape` (valid) can be above 0,
// clipped to the image, computed without rasterising (memory estimates): the MaskRaster::bounds
// rasterize_mask() gives a brush (the boxes of its Add strokes: their points widened by the outer
// radius and a pixel) and the analytic shapes' bounds (radial: the ellipse's box; linear: the image
// clipped to the half-plane it covers). Empty for an empty size.
[[nodiscard]] image::PixelRect mask_support(const MaskShape& shape, image::Size size) noexcept;

// Rasterises `shape` (valid as DevelopSettings::validate() requires) on an upright image of
// `size`: the bounds are the shape's support (brush: its Add strokes' boxes; radial: the
// ellipse's box; linear: the box of the image clipped to the half-plane it covers), clipped to
// the image.
// Errors (engine.develop.masks): InvalidArgument for an empty size or an invalid shape;
// LimitExceeded / OutOfMemory; Cancelled.
[[nodiscard]] Expected<MaskRaster> rasterize_mask(const MaskShape& shape, image::Size size,
                                                  const image::AllocationLimits& limits = {},
                                                  const core::CancellationToken& cancel = {}, bool parallel = true);

// The weight plane of `adjustment` over the whole upright image of `size` (opacity and invert
// applied, 0 when disabled): the editor's mask overlay. Errors as rasterize_mask().
[[nodiscard]] Expected<image::ImageBuffer<float>> mask_weights(const LocalAdjustment& adjustment, image::Size size,
                                                               const image::AllocationLimits& limits = {},
                                                               const core::CancellationToken& cancel = {},
                                                               bool parallel = true);

// Outlines of a shape for the editor's overlay, in normalised upright coordinates (map them with
// GeometryMapping::output_from_normalized()): radial: the ellipse (rho = 1) and, for a feather
// strictly between 0 and 1, the inner ellipse (rho = 1 - feather), each closed (`segments` + 1
// points); linear: three lines perpendicular to start -> end, each long enough to cross the
// image, through the transition band's edges and its middle (t = 0.5 - h, 0.5 and 0.5 + h with h
// as the coverage defines it: the start and the end for feather 1, close to the middle for a hard
// edge); brush: every stroke's centre line. `size` gives the aspect ratio (ellipses and
// perpendiculars are defined in pixels).
struct MaskOutline {
    std::vector<std::vector<MaskPoint>> polylines;
};

[[nodiscard]] MaskOutline mask_outline(const MaskShape& shape, image::Size size, std::int32_t segments = 64);

// ---- Raster cache -------------------------------------------------------------------------------

inline constexpr std::uint64_t kDefaultMaskRasterCacheBytes = std::uint64_t{256} << 20;

// Brush rasters kept between renders (an edit session's slider changes reuse them; painting a new
// stroke changes the shape and makes a new entry). Keyed by mask_shape_revision(), the shape itself
// (compared on a hit, so a hash collision never returns another mask) and the render size; least
// recently used entries are evicted beyond the capacity. The caller accounts for the memory
// (stats().bytes).
class MaskRasterCache {
public:
    explicit MaskRasterCache(std::uint64_t capacity_bytes = kDefaultMaskRasterCacheBytes) noexcept
        : capacity_(capacity_bytes) {}
    MaskRasterCache(const MaskRasterCache&) = delete;
    MaskRasterCache& operator=(const MaskRasterCache&) = delete;

    // The cached raster of `shape` at `size`, or nullptr.
    [[nodiscard]] std::shared_ptr<const MaskRaster> find(const MaskShape& shape, image::Size size);
    // Keeps `raster` (made for `shape` at raster->size), evicting older entries; a raster larger
    // than the capacity, or one that cannot be recorded for lack of memory, is not kept.
    void insert(const MaskShape& shape, std::shared_ptr<const MaskRaster> raster) noexcept;
    void clear() noexcept;

    struct Stats {
        std::size_t entries = 0;
        std::uint64_t bytes = 0;
        std::uint64_t capacity = 0;
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
    };
    [[nodiscard]] Stats stats() const;

private:
    struct Entry {
        std::uint64_t revision = 0;
        MaskShape shape;
        std::shared_ptr<const MaskRaster> raster;
    };
    mutable std::mutex mutex_;
    std::list<Entry> entries_;  // most recently used first
    std::uint64_t capacity_ = 0;
    std::uint64_t bytes_ = 0;
    std::uint64_t hits_ = 0;
    std::uint64_t misses_ = 0;
};

// ---- Local controls -----------------------------------------------------------------------------

// Local contrast (see the file comment): slope 1 + kLocalContrastStrength x C at the pivot, changes
// bounded by kLocalContrastStrength x kLocalContrastRangeEv EV.
inline constexpr double kLocalContrastStrength = 0.6;
inline constexpr double kLocalContrastRangeEv = 2.0;
// The pivot's displayed value: display-linear middle grey (L* 49.5).
inline constexpr double kLocalContrastPivotDisplay = 0.18;
// White shifts at +-1.
inline constexpr double kLocalTemperatureMired = 50.0;
inline constexpr double kLocalTintUnits = 50.0;

// log2 channel-gain axes of the local white shift in Rec.2020 (see the file comment): a pixel's
// gains are 2^(T temperature + G tint), normalised to unit luminance.
struct LocalWhiteAxes {
    std::array<double, 3> temperature{};
    std::array<double, 3> tint{};
};

// The axes (computed from xy_from_temperature_tint(), white_balance_model.hpp). Internal when the
// colour conversions fail (a build defect).
[[nodiscard]] Expected<LocalWhiteAxes> local_white_axes();

struct LocalAdjustmentsParameters {
    double white = 1.0;            // W (DevelopReport::white), finite and positive
    std::int32_t tone_radius = 1;  // highlights/shadows base layer: local_tone_radius() of the render
    double tone_epsilon = kLocalToneEpsilon;
};

struct LocalAdjustmentsRecord {
    std::uint32_t version = kLocalAdjustmentsOperationVersion;
    std::uint32_t masks = 0;       // adjustments with an effect
    std::uint32_t rasterized = 0;  // brush rasters made by this call
    std::uint32_t cache_hits = 0;  // brush rasters taken from the cache
    std::uint64_t raster_bytes = 0;
    std::int32_t tone_radius = 0;  // 0: no base layer was needed
    image::PixelRect tone_region;  // where the base layer was computed
    std::uint64_t changed_pixels = 0;  // pixels with a weight above 0
};

// Working memory of apply_local_adjustments() for an upright render of `size` whose base layer has
// the guided-filter radius `tone_radius` (LocalAdjustmentsParameters::tone_radius; 0 when it is
// not known), bytes, an upper bound: every brush raster over its mask_support() (the strokes'
// extent, so many small brush masks stay small), the base layer's three planes over the tone
// masks' supports (the whole image for an inverted one) expanded by 2 tone_radius (the whole image
// without a radius), and the per-row counters. A raster the cache already holds is counted too.
[[nodiscard]] std::uint64_t local_adjustments_working_bytes(std::span<const LocalAdjustment> adjustments,
                                                            image::Size size, std::int32_t tone_radius = 0) noexcept;

// Applies `adjustments` (valid, in order) to `image` (scene_linear_rec2020_d65(), 3 channels, the
// upright render) in place, as the file comment defines. Brush rasters come from `cache` when it
// holds them and are added to it (nullptr: no cache).
// Errors (engine.develop.masks): InvalidArgument for an empty image, another encoding or invalid
// parameters or adjustments; Internal for a non-finite result; LimitExceeded / OutOfMemory;
// Cancelled (the image must then be discarded).
[[nodiscard]] Expected<LocalAdjustmentsRecord> apply_local_adjustments(
    ColorImage& image, std::span<const LocalAdjustment> adjustments, const LocalAdjustmentsParameters& parameters,
    MaskRasterCache* cache = nullptr, const image::AllocationLimits& limits = {},
    const core::CancellationToken& cancel = {}, bool parallel = true);

}  // namespace lucent::engine
