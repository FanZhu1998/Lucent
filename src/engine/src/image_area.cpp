// The camera's image area in the engine's images (image_area.hpp).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <utility>

#include <lucent/engine/image_area.hpp>

namespace lucent::engine {

namespace {

constexpr const char* kWhere = "engine.image_area";

// Round_int32 of the DNG SDK for the non-negative values used here; halves away from zero.
std::int64_t round_whole(double v) noexcept {
    return static_cast<std::int64_t>(std::llround(v));
}

std::int32_t narrow(std::int64_t v) noexcept {
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(v, INT32_MIN, INT32_MAX));
}

}  // namespace

std::optional<ImageAreaPlacement> place_image_area(const raw::RawMetadata& metadata, image::Size image,
                                                   const raw::DngOpcodeFrame* stage3) {
    const raw::ImageArea& area = metadata.image_area;
    if (area.source == raw::ImageAreaSource::Unknown || !area.rect || image.empty()) {
        return std::nullopt;
    }
    ImageAreaPlacement placement;
    placement.source = area.source;
    placement.provenance = area.provenance;
    placement.basis = area.basis;
    const raw::AreaRect& r = *area.rect;

    // The rectangle relative to the stage image's origin, whole pixels.
    std::int64_t left = round_whole(r.x - area.reference.x);
    std::int64_t top = round_whole(r.y - area.reference.y);
    const std::int64_t width = round_whole(r.width);
    const std::int64_t height = round_whole(r.height);
    // The stage image and Lucent's buffer origin in it.
    std::int64_t stage_width = area.reference.width;
    std::int64_t stage_height = area.reference.height;
    std::int64_t offset_x = std::int64_t{metadata.geometry.visible_area.x} - area.reference.x;
    std::int64_t offset_y = std::int64_t{metadata.geometry.visible_area.y} - area.reference.y;
    const bool dng = area.source == raw::ImageAreaSource::DngDefaultCrop ||
                     area.source == raw::ImageAreaSource::DngDefaultWholeImage;
    if (dng) {
        const raw::DngOpcodeFrame frame =
            stage3 != nullptr ? *stage3 : raw::dng_opcode_frame(metadata, raw::DngOpcodeListId::List3);
        if (frame.image.width > 0 && frame.image.height > 0) {
            stage_width = frame.image.width;
            stage_height = frame.image.height;
            offset_x = frame.offset_x;
            offset_y = frame.offset_y;
        }
        // dng_negative::DefaultCropArea(): an area overrunning the stage image moves back inside.
        if (left + width > stage_width) {
            left -= left + width - stage_width;
            placement.moved_inside = true;
        }
        if (top + height > stage_height) {
            top -= top + height - stage_height;
            placement.moved_inside = true;
        }
    }
    // In Lucent's buffer, cut to it.
    const std::int64_t x0 = left - offset_x;
    const std::int64_t y0 = top - offset_y;
    const std::int64_t x1 = x0 + width;
    const std::int64_t y1 = y0 + height;
    const std::int64_t cx0 = std::clamp<std::int64_t>(x0, 0, image.width);
    const std::int64_t cy0 = std::clamp<std::int64_t>(y0, 0, image.height);
    const std::int64_t cx1 = std::clamp<std::int64_t>(x1, 0, image.width);
    const std::int64_t cy1 = std::clamp<std::int64_t>(y1, 0, image.height);
    placement.clipped = {narrow(cx0 - x0), narrow(cy0 - y0), narrow(x1 - cx1), narrow(y1 - cy1)};
    if (cx1 > cx0 && cy1 > cy0) {
        placement.rect = image::PixelRect{narrow(cx0), narrow(cy0), narrow(cx1 - cx0), narrow(cy1 - cy0)};
    }
    return placement;
}

ImageAreaPlacement oriented_placement(ImageAreaPlacement placement, image::Size stored,
                                      image::Orientation orientation) noexcept {
    if (orientation == image::Orientation::Normal) {
        return placement;
    }
    // The declared rectangle (the placed one plus what was cut off) and the placed one, turned.
    const auto& c = placement.clipped;
    const image::PixelRect placed = placement.rect;
    if (placed.empty()) {
        return placement;
    }
    const image::PixelRect declared{placed.x - c[0], placed.y - c[1], placed.width + c[0] + c[2],
                                    placed.height + c[1] + c[3]};
    const image::PixelRect a = image::map_to_oriented(placed, stored, orientation);
    const image::PixelRect d = image::map_to_oriented(declared, stored, orientation);
    placement.rect = a;
    placement.clipped = {narrow(std::int64_t{a.x} - d.x), narrow(std::int64_t{a.y} - d.y),
                         narrow(d.right() - a.right()), narrow(d.bottom() - a.bottom())};
    return placement;
}

image::PixelRect scale_area(const image::PixelRect& area, image::Size full, image::Size render) noexcept {
    if (area.empty() || full.empty() || render.empty()) {
        return image::PixelRect::from_size(render);
    }
    if (full == render) {
        const image::PixelRect inside = area.intersect(image::PixelRect::from_size(render));
        return inside.empty() ? image::PixelRect::from_size(render) : inside;
    }
    const double sx = static_cast<double>(render.width) / full.width;
    const double sy = static_cast<double>(render.height) / full.height;
    const auto span = [](double lo, double hi, std::int32_t n, std::int32_t& first, std::int32_t& count) {
        auto a = static_cast<std::int32_t>(std::clamp<std::int64_t>(round_whole(lo), 0, n - 1));
        auto b = static_cast<std::int32_t>(std::clamp<std::int64_t>(round_whole(hi), a + 1, n));
        first = a;
        count = b - a;
    };
    image::PixelRect out;
    span(area.x * sx, static_cast<double>(area.right()) * sx, render.width, out.x, out.width);
    span(area.y * sy, static_cast<double>(area.bottom()) * sy, render.height, out.y, out.height);
    return out;
}

Status crop_to_area(image::ImageBuffer<float>& pixels, const image::PixelRect& rect,
                    const image::AllocationLimits& limits, const core::CancellationToken& cancel) {
    if (pixels.empty() || !image::PixelRect::from_size(pixels.size()).contains(rect)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the image area {}x{}+{}+{} is not inside the {} x {} image", rect.width,
                                rect.height, rect.x, rect.y, pixels.width(), pixels.height()),
                    kWhere);
    }
    if (rect == image::PixelRect::from_size(pixels.size())) {
        return ok();
    }
    auto out =
        image::ImageBuffer<float>::create_uninitialized(rect.width, rect.height, pixels.channels(), limits);
    if (!out) {
        return fail(out.error().code, "image area: " + out.error().message, kWhere);
    }
    const std::size_t row_bytes =
        static_cast<std::size_t>(rect.width) * static_cast<std::size_t>(pixels.channels()) * sizeof(float);
    for (std::int32_t y = 0; y < rect.height; ++y) {
        if ((y & 255) == 0) {
            if (auto cancelled = cancel.check(kWhere); !cancelled) {
                return cancelled;
            }
        }
        std::memcpy(out->row(y),
                    pixels.row(rect.y + y) + static_cast<std::ptrdiff_t>(rect.x) * pixels.channels(),
                    row_bytes);
    }
    pixels = std::move(*out);
    return ok();
}

}  // namespace lucent::engine
