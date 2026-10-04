#pragma once

// Copies of stage-one images: a full copy (when the image is already smaller than a requested
// proxy) and a native-resolution crop, both carrying every record of the source so that stage two
// renders them exactly as it renders the source.

#include <algorithm>
#include <cstddef>
#include <utility>

#include <lucent/core/error.hpp>
#include <lucent/engine/develop.hpp>
#include <lucent/image/geometry.hpp>

namespace lucent::analysis::detail {

// Everything but the pixels.
[[nodiscard]] inline engine::StageOneImage copy_stage_one_records(const engine::StageOneImage& source) {
    engine::StageOneImage out;
    out.encoding = source.encoding;
    out.kind = source.kind;
    out.full_size = source.full_size;
    out.scale = source.scale;
    out.metadata = source.metadata;
    out.normalization = source.normalization;
    out.white_balance = source.white_balance;
    out.sample_multipliers = source.sample_multipliers;
    out.demosaic = source.demosaic;
    out.orientation = source.orientation;
    out.stages = source.stages;
    out.downscale = source.downscale;
    out.warnings = source.warnings;
    out.seconds = source.seconds;
    return out;
}

[[nodiscard]] inline Expected<engine::StageOneImage> clone_stage_one(const engine::StageOneImage& source) {
    auto pixels = source.pixels.clone();
    if (!pixels) {
        return Unexpected(std::move(pixels.error()));
    }
    engine::StageOneImage out = copy_stage_one_records(source);
    out.pixels = std::move(*pixels);
    return out;
}

// The pixels of `rect` (inside the full-resolution source) as a stage-one image of that size whose
// full_size and scale (1) are the source's, so stage two uses the full image's local-tone radius.
[[nodiscard]] inline Expected<engine::StageOneImage> crop_stage_one(const engine::StageOneImage& source,
                                                                    const image::PixelRect& rect,
                                                                    const image::AllocationLimits& limits) {
    auto view = source.pixels.view(rect);
    if (!view) {
        return Unexpected(std::move(view.error()));
    }
    const std::int32_t channels = source.pixels.channels();
    auto buffer = image::ImageBuffer<float>::create_uninitialized(rect.width, rect.height, channels, limits);
    if (!buffer) {
        return Unexpected(std::move(buffer.error()));
    }
    for (std::int32_t y = 0; y < rect.height; ++y) {
        const float* src = view->row(y);
        float* dst = buffer->row(y);
        std::copy(src, src + static_cast<std::ptrdiff_t>(rect.width) * channels, dst);
    }
    engine::StageOneImage out = copy_stage_one_records(source);
    out.pixels = std::move(*buffer);
    out.scale = 1.0;
    out.downscale.reset();
    return out;
}

}  // namespace lucent::analysis::detail
