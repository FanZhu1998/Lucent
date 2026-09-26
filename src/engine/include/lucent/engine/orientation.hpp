#pragma once

// Baking the capture orientation into pixels (blueprint section 5, step 4: "normalize orientation
// consistently"). The exporter writes top-left images only, so the render pipeline applies the
// EXIF/TIFF orientation (image::Orientation, geometry.hpp) as the last pixel stage.

#include <cstdint>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/image/pixel_buffer.hpp>

namespace lucent::engine {

// Returns the upright image for a stored image `stored` with `orientation`: output pixel p is
// stored pixel image::map_from_oriented(p, stored.size(), orientation), all channels copied
// exactly. Orientation::Normal returns `stored` itself (moved, no copy); every other orientation
// allocates the output (image::oriented_size) under `limits` and releases `stored` on success.
// Errors (engine.orientation): InvalidArgument for an empty buffer or an invalid orientation
// value; LimitExceeded / OutOfMemory; Cancelled.
[[nodiscard]] Expected<image::ImageBuffer<float>> apply_orientation(
    image::ImageBuffer<float>&& stored, image::Orientation orientation,
    const image::AllocationLimits& limits = {}, const core::CancellationToken& cancel = {},
    bool parallel = true);

}  // namespace lucent::engine
