#pragma once

// The geometry operation (DevelopSettings::geometry, kGeometryOperationVersion 2, and version 1)
// and the point mapping between mask coordinates and output pixels (blueprint section 2, "crop,
// rotate, straighten", and section 5, "Noise, masks, and geometry"; ADR 0015, ADR 0017).
//
// Frames, in continuous pixel coordinates (image/geometry.hpp: pixel (i, j) covers [i, i + 1) x
// [j, j + 1), its centre is (i + 0.5, j + 0.5)), all at the render scale (the full-resolution
// image or a proxy of it):
//   U  upright: stage one's image (DNG opcodes and the camera orientation applied), W x H. Masks
//      are defined here, normalised per axis: a MaskPoint (x, y) is the pixel point (x W, y H),
//      and a normalised length l is l x max(W, H) pixels. U is the masks' frame for every
//      geometry version.
//   F  the geometry's frame, a whole-pixel rectangle of U with origin (Fx, Fy) and size Fw x Fh:
//      all of U for geometry version 1, the camera's image area for version 2 (image_area.hpp:
//      the area stage one placed in full-resolution pixels, scale_area() at a proxy; all of U when
//      the file declares none). F = U reduces every formula below to version 1's.
//   S  straightened: F rotated by straighten_degrees about its centre c = (Fx + Fw / 2,
//      Fy + Fh / 2) on a canvas of F's size whose origin is F's top-left corner; positive angles
//      turn the content counter-clockwise as displayed:
//          p_S = (Fw / 2, Fh / 2) + R (p_U - c),  R = [[cos a, sin a], [-sin a, cos a]]
//      (y grows downwards).
//   C  cropped: the crop rectangle of S rounded to whole render pixels,
//          [X0, X1) x [Y0, Y1),  X0 = round(left Fw), X1 = round(right Fw) (likewise Y with Fh),
//      rounding halves away from zero and keeping at least one pixel; p_C = p_S - (X0, Y0).
//      With straightening, when the straightened frame does not cover that rectangle but covers
//      the crop rounded inward, X0 = ceil(left Fw), X1 = floor(right Fw) (likewise Y; a bound
//      within 1e-6 pixels of a whole pixel is that pixel; the nearest pixels when that leaves
//      none), the crop rounds inward: a crop on the frame's edge (largest_inside_crop(), a crop
//      constrained to the frame), also at a proxy whose sides round its aspect ratio, has no
//      partially covered output pixel.
//   O  output: C with the user orientation (user_orientation(): flip_horizontal, then the
//      quarter turns clockwise), image::map_to_oriented(p_C, crop size, orientation).
// Every step is affine, so straight lines stay straight, ellipses stay ellipses and lengths are
// kept (the rotation and the orientation are isometries): a normalised length l is l x max(W, H)
// output pixels.
//
// Rendering (apply_geometry): every output pixel centre is mapped back to U and sampled there.
//   * No straightening: the crop is a whole-pixel rectangle of F, so the output is an exact copy
//     of U's pixels, permuted by the orientation (bit-identical values).
//   * Straightening: separable 4 x 4 Keys cubic convolution, a = -0.5 (Catmull-Rom), the same
//     kernel as the DNG warps (ADR 0014), taps beyond U clamped to its edge pixels (so near F's
//     edge the taps read U's real border pixels, which exist "to prevent interpolation artifacts
//     near the edges of the final image", DNG DefaultCropOrigin), sums in double; each channel of
//     the result is limited to the range of the four pixels nearest to the sample point (no
//     ringing: the output stays within the input's range, so a display image's max(R, G, B) <= 1
//     holds). Output pixels whose centre lies within half a pixel of F's edge or outside F are
//     weighted by their coverage clamp(0.5 + d, 0, 1), d the signed distance (pixels) from the
//     centre to F's edge, towards black (0): canvas areas the rotated frame does not cover are
//     black, with an antialiased border (a coverage within 1e-4 of 1 counts as 1).
//     largest_inside_crop() gives the largest crop that avoids them, at every render scale (the
//     inward rounding above).
// Proxies: positions scale per axis and lengths with the long edge, so a proxy's output shows the
// same content as the full-resolution output within the crop rounding (half a pixel of each; a
// pixel of each for a straightened crop on the frame's edge, which rounds inward); a version-2
// frame is itself rounded to the proxy's pixels (scale_area()), which adds up to half a pixel.
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
// size: stage one's image, full resolution or a proxy) and the frame F the geometry works in.
// develop.hpp's geometry_mapping() makes the one stage two renders with.
class GeometryMapping {
public:
    // F = the whole upright image (geometry version 1, or version 2 without a camera area).
    // InvalidArgument (engine.develop.geometry) for an empty size or a geometry that
    // DevelopSettings::validate() would reject (angle, crop, quarter turns).
    [[nodiscard]] static Expected<GeometryMapping> create(const GeometrySetting& geometry, image::Size upright_size);
    // F = `frame`, a non-empty rectangle inside the upright image (InvalidArgument otherwise).
    [[nodiscard]] static Expected<GeometryMapping> create(const GeometrySetting& geometry,
                                                          image::Size upright_size,
                                                          const image::PixelRect& frame);

    [[nodiscard]] const GeometrySetting& setting() const noexcept { return setting_; }
    [[nodiscard]] image::Size upright_size() const noexcept { return upright_; }
    // The frame F in U, whole render pixels; its size is what crop_inside_image() and
    // largest_inside_crop() take.
    [[nodiscard]] const image::PixelRect& frame() const noexcept { return frame_; }
    [[nodiscard]] image::Size frame_size() const noexcept { return frame_.size(); }
    // The crop rectangle in S (relative to F's top-left corner), whole render pixels.
    [[nodiscard]] image::PixelRect crop_pixels() const noexcept { return crop_; }
    [[nodiscard]] image::Orientation orientation() const noexcept { return orientation_; }
    [[nodiscard]] image::Size output_size() const noexcept { return output_; }
    // True when the output equals the upright image (F is all of U, and an identity geometry or a
    // crop that rounds to the whole image with no straightening and no orientation change).
    [[nodiscard]] bool identity() const noexcept;
    // True when straightening makes the output a resampling (false: an exact pixel copy).
    [[nodiscard]] bool resamples() const noexcept { return setting_.straighten_degrees != 0.0; }
    // Identifies the mapping (geometry_revision() of the setting, the upright size and, when it is
    // not all of U, the frame): overlays computed for one revision stay valid until it changes.
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
    image::PixelRect frame_;
    image::PixelRect crop_;
    image::Orientation orientation_ = image::Orientation::Normal;
    image::Size output_;
    double cos_ = 1.0;
    double sin_ = 0.0;
    std::uint64_t revision_ = 0;
};

// True when the straightened frame covers every point of the geometry's crop rectangle (no black
// canvas in the output), for a frame of `frame_size` pixels (GeometryMapping::frame_size(): the
// whole upright image for geometry version 1, the camera's image area for version 2; its aspect
// ratio matters). Points within 1e-9 of the edge count as covered. False for an empty size.
[[nodiscard]] bool crop_inside_image(const GeometrySetting& geometry, image::Size frame_size) noexcept;

// The largest crop, centred on the frame's centre, with aspect ratio `aspect` (output width /
// height in pixels before the orientation step; <= 0 or not finite: the frame's own) that the frame
// of `frame_size` pixels (see crop_inside_image()) straightened by `straighten_degrees` covers
// completely. The full rectangle for an empty size. The aspect constraint and any other placement
// are the editor's (this is a helper for it).
[[nodiscard]] CropRect largest_inside_crop(double straighten_degrees, image::Size frame_size,
                                           double aspect = 0.0) noexcept;

// What apply_geometry() did.
struct GeometryRecord {
    std::uint32_t version = kGeometryOperationVersion;  // the operation version rendered (stage two sets it)
    double straighten_degrees = 0.0;
    image::PixelRect frame;  // F in U at the render scale
    image::PixelRect crop;   // in S at the render scale
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
