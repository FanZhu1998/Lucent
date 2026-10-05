#pragma once

// The geometry operation (DevelopSettings::geometry, kGeometryOperationVersion 1) and the point
// mapping between mask coordinates and output pixels (blueprint section 2, "crop, rotate,
// straighten", and section 5, "Noise, masks, and geometry"; ADR 0015).
//
// Frames, in continuous pixel coordinates (image/geometry.hpp: pixel (i, j) covers [i, i + 1) x
// [j, j + 1), its centre is (i + 0.5, j + 0.5)), all at the render scale (the full-resolution
// image or a proxy of it):
//   U  upright: stage one's image (DNG opcodes and the camera orientation applied), W x H. Masks
//      are defined here, normalised per axis: a MaskPoint (x, y) is the pixel point (x W, y H),
//      and a normalised length l is l x max(W, H) pixels.
//   S  straightened: U rotated by straighten_degrees about its centre c = (W / 2, H / 2) on the
//      same W x H canvas; positive angles turn the content counter-clockwise as displayed:
//          p_S = c + R (p_U - c),  R = [[cos a, sin a], [-sin a, cos a]]  (y grows downwards).
//   C  cropped: the crop rectangle of S rounded to whole render pixels,
//          [X0, X1) x [Y0, Y1),  X0 = round(left W), X1 = round(right W) (likewise Y with H),
//      rounding halves away from zero and keeping at least one pixel; p_C = p_S - (X0, Y0).
//      With straightening, when the straightened image does not cover that rectangle but covers
//      the crop rounded inward, X0 = ceil(left W), X1 = floor(right W) (likewise Y; a bound within
//      1e-6 pixels of a whole pixel is that pixel; the nearest pixels when that leaves none), the
//      crop rounds inward: a crop on the image's edge (largest_inside_crop(), a crop constrained to
//      the image), also at a proxy whose sides round its aspect ratio, has no partially covered
//      output pixel.
//   O  output: C with the user orientation (user_orientation(): flip_horizontal, then the
//      quarter turns clockwise), image::map_to_oriented(p_C, crop size, orientation).
// Every step is affine, so straight lines stay straight, ellipses stay ellipses and lengths are
// kept (the rotation and the orientation are isometries): a normalised length l is l x max(W, H)
// output pixels.
//
// Rendering (apply_geometry): every output pixel centre is mapped back to U and sampled there.
//   * No straightening: the crop is a whole-pixel rectangle, so the output is an exact copy of
//     U's pixels, permuted by the orientation (bit-identical values).
//   * Straightening: separable 4 x 4 Keys cubic convolution, a = -0.5 (Catmull-Rom), the same
//     kernel as the DNG warps (ADR 0014), taps beyond the image clamped to its edge pixels, sums in
//     double; each channel of the result is limited to the range of the four pixels nearest to
//     the sample point (no ringing: the output stays within the input's range, so a display
//     image's max(R, G, B) <= 1 holds). Output pixels whose centre lies within half a pixel of the
//     image's edge or outside it are weighted by their coverage clamp(0.5 + d, 0, 1), d the signed
//     distance (pixels) from the centre to the image's edge, towards black (0): canvas areas the
//     rotated image does not cover are black, with an antialiased border (a coverage within 1e-4
//     of 1 counts as 1). largest_inside_crop() gives the largest crop that avoids them, at every
//     render scale (the inward rounding above).
// Proxies: positions scale per axis and lengths with the long edge, so a proxy's output shows the
// same content as the full-resolution output within the crop rounding (half a pixel of each; a
// pixel of each for a straightened crop on the image's edge, which rounds inward).
//
// Thread-safety: GeometryMapping is an immutable value; functions are reentrant.

#include <cstdint>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/camera_color.hpp>
#include <lucent/engine/develop_settings.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/image/pixel_buffer.hpp>

namespace lucent::engine {

// The mapping of one geometry setting for an upright image of `upright_size` pixels (the render
// size: stage one's image, full resolution or a proxy).
class GeometryMapping {
public:
    // InvalidArgument (engine.develop.geometry) for an empty size or a geometry that
    // DevelopSettings::validate() would reject (angle, crop, quarter turns).
    [[nodiscard]] static Expected<GeometryMapping> create(const GeometrySetting& geometry, image::Size upright_size);

    [[nodiscard]] const GeometrySetting& setting() const noexcept { return setting_; }
    [[nodiscard]] image::Size upright_size() const noexcept { return upright_; }
    // The crop rectangle in S, whole render pixels.
    [[nodiscard]] image::PixelRect crop_pixels() const noexcept { return crop_; }
    [[nodiscard]] image::Orientation orientation() const noexcept { return orientation_; }
    [[nodiscard]] image::Size output_size() const noexcept { return output_; }
    // True when the output equals the upright image (identity geometry, or a crop that rounds to
    // the whole image with no straightening and no orientation change).
    [[nodiscard]] bool identity() const noexcept;
    // True when straightening makes the output a resampling (false: an exact pixel copy).
    [[nodiscard]] bool resamples() const noexcept { return setting_.straighten_degrees != 0.0; }
    // Identifies the mapping (geometry_revision() of the setting and the upright size): overlays
    // computed for one revision stay valid until it changes.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    // Output (and upright) pixels per normalised length: max(W, H).
    [[nodiscard]] double pixels_per_unit_length() const noexcept;

    // Continuous upright pixel coordinates <-> continuous output pixel coordinates (inverse of
    // each other to rounding; points outside the image or the crop map as the affine maps do).
    [[nodiscard]] image::PointD output_from_upright(image::PointD upright) const noexcept;
    [[nodiscard]] image::PointD upright_from_output(image::PointD output) const noexcept;
    // Normalised mask coordinates <-> continuous output pixel coordinates.
    [[nodiscard]] image::PointD output_from_normalized(MaskPoint point) const noexcept;
    [[nodiscard]] MaskPoint normalized_from_output(image::PointD output) const noexcept;
    // True when an output point lies inside the output image.
    [[nodiscard]] bool inside_output(image::PointD output) const noexcept;

private:
    GeometrySetting setting_;
    image::Size upright_;
    image::PixelRect crop_;
    image::Orientation orientation_ = image::Orientation::Normal;
    image::Size output_;
    double cos_ = 1.0;
    double sin_ = 0.0;
    std::uint64_t revision_ = 0;
};

// True when the straightened image covers every point of the geometry's crop rectangle (no
// black canvas in the output), for an upright image of `upright_size` (its aspect ratio matters).
// Points within 1e-9 of the edge count as covered. False for an empty size.
[[nodiscard]] bool crop_inside_image(const GeometrySetting& geometry, image::Size upright_size) noexcept;

// The largest crop, centred on the image centre, with aspect ratio `aspect` (output width / height
// in pixels before the orientation step; <= 0 or not finite: the upright image's own) that the
// image straightened by `straighten_degrees` covers completely. The full rectangle for an empty
// size. The aspect constraint and any other placement are the editor's (this is a helper for it).
[[nodiscard]] CropRect largest_inside_crop(double straighten_degrees, image::Size upright_size,
                                           double aspect = 0.0) noexcept;

// What apply_geometry() did.
struct GeometryRecord {
    std::uint32_t version = kGeometryOperationVersion;
    double straighten_degrees = 0.0;
    image::PixelRect crop;  // in S at the render scale
    image::Orientation orientation = image::Orientation::Normal;
    image::Size input_size;
    image::Size output_size;
    bool resampled = false;  // Catmull-Rom (straightened); false: an exact pixel copy
    // Output pixels not wholly covered by the straightened image (weighted towards black).
    std::uint64_t uncovered_pixels = 0;
};

// Replaces `image` (1 to 4 channels, any encoding, which is kept) by its geometry output: a new
// buffer of mapping.output_size() allocated under `limits`; the input buffer is released on
// success. The identity mapping leaves the image untouched (callers skip it).
// Errors (engine.develop.geometry): InvalidArgument when the image is empty or its size is not
// mapping.upright_size(); LimitExceeded / OutOfMemory; Cancelled (the image is unchanged).
[[nodiscard]] Expected<GeometryRecord> apply_geometry(ColorImage& image, const GeometryMapping& mapping,
                                                      const image::AllocationLimits& limits = {},
                                                      const core::CancellationToken& cancel = {},
                                                      bool parallel = true);

}  // namespace lucent::engine
