#pragma once

// The camera's intended image area in the engine's images (blueprint section 3, "Support files and
// capture modes", and section 5, step 2 "active-area rules"; ADR 0017).
//
// The decoded-RAW contract (raw::ImageArea, contract 1.4) states the area a file declares, in
// raw-frame coordinates and relative to its reference rectangle. The engine places it in the image
// it holds after the DNG opcodes (Lucent's buffer of the visible area, cut by any TrimBounds), in
// stored orientation, and both pipelines apply it by default:
//   * the develop pipeline through the geometry operation, version 2 (develop_geometry.hpp): the
//     user's crop and straighten are relative to the area, and the identity geometry renders it;
//   * the W2 render path as its camera_area stage (render.hpp, RenderSettings::apply_camera_area).
//
// Placement (place_image_area):
//   * DNG (DefaultCrop, or the specification's default without it): the stage-3 image of the DNG
//     SDK is the ActiveArea, re-based at the origin of every TrimBounds of OpcodeList2 and 3 that is
//     applied (dng_simple_image::Trim). The area relative to it is the declared origin and size,
//     each rounded to whole pixels (halves away from zero; dng_negative::DefaultCropArea() rounds
//     with Round_int32), and when its right or bottom edge overruns the stage image it is moved
//     back inside by the overrun (DefaultCropArea()'s rule; moved_inside). It is then mapped into
//     Lucent's buffer, whose origin lies at the stage frame's offset (one row or column inside an
//     odd ActiveArea margin, ADR 0005), and cut to the buffer (clipped).
//   * Decoder inset crops: whole raw-frame pixels, mapped into the visible area and cut to it.
// A known area of which no pixel lies inside the image cannot be applied: the placement then has an
// empty rect and callers treat the area as unknown (with a warning).
//
// Thread-safety: plain functions on values.

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/image/pixel_buffer.hpp>
#include <lucent/raw/decoded_raw.hpp>

namespace lucent::engine {

// Semantic version of the placement rules above (bump when the same metadata would place the area
// elsewhere).
inline constexpr std::uint32_t kImageAreaPlacementVersion = 1;

// The camera's image area placed in one image.
struct ImageAreaPlacement {
    std::uint32_t version = kImageAreaPlacementVersion;
    raw::ImageAreaSource source = raw::ImageAreaSource::Unknown;
    raw::Provenance provenance = raw::Provenance::LucentRule;
    // Whole pixels of the image it was placed on, inside it; empty when nothing of the area lies
    // inside the image (the area cannot be applied).
    image::PixelRect rect;
    // Rows or columns of the declared area beyond each edge of the image, cut off: left, top,
    // right, bottom (in the image's own orientation).
    std::array<std::int32_t, 4> clipped{};
    // DNG: the rounded area overran the stage image and was moved back inside it.
    bool moved_inside = false;
    std::string basis;  // raw::ImageArea::basis

    [[nodiscard]] bool applicable() const noexcept { return !rect.empty(); }

    friend bool operator==(const ImageAreaPlacement&, const ImageAreaPlacement&) = default;
};

// Places metadata.image_area in Lucent's image of the visible area after the DNG opcodes, stored
// orientation, `image` pixels (see "Placement"). `stage3` is the opcode processor's stage-3 frame
// (DngOpcodeProcessor::stage3_frame()) for a DNG; without it the frame raw::dng_opcode_frame()
// gives for OpcodeList3 is used (no trims). std::nullopt when the area is unknown.
[[nodiscard]] std::optional<ImageAreaPlacement> place_image_area(const raw::RawMetadata& metadata,
                                                                 image::Size image,
                                                                 const raw::DngOpcodeFrame* stage3 = nullptr);

// The placement after its image of `stored` pixels is turned by `orientation` (image/geometry.hpp:
// the rectangle covering the same pixels; the clipped edges follow their sides).
[[nodiscard]] ImageAreaPlacement oriented_placement(ImageAreaPlacement placement, image::Size stored,
                                                    image::Orientation orientation) noexcept;

// The full-resolution rectangle `area` of an image of `full` pixels at the scale of an image of
// `render` pixels (a proxy: positions scale per axis, as the proxy downscale does): every edge is
// scaled and rounded to the nearest pixel (halves away from zero), keeping at least one pixel
// inside the image. Equal to `area` (clipped to the image) when render == full.
[[nodiscard]] image::PixelRect scale_area(const image::PixelRect& area, image::Size full,
                                          image::Size render) noexcept;

// Replaces `pixels` (any channel count) by its rectangle `rect` (non-empty, inside it): a new
// buffer allocated under `limits`; the input is released on success. Errors
// (engine.image_area): InvalidArgument for a rectangle outside the image; LimitExceeded /
// OutOfMemory; Cancelled (the image is unchanged).
[[nodiscard]] Status crop_to_area(image::ImageBuffer<float>& pixels, const image::PixelRect& rect,
                                  const image::AllocationLimits& limits = {},
                                  const core::CancellationToken& cancel = {});

}  // namespace lucent::engine
