// Orientation baking (orientation.hpp).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include <lucent/engine/orientation.hpp>

#include "parallel_rows.hpp"

namespace lucent::engine {

namespace {

constexpr const char* kWhere = "engine.orientation";

bool known(image::Orientation orientation) noexcept {
    const auto v = static_cast<std::uint8_t>(orientation);
    return v >= 1 && v <= 8;
}

}  // namespace

Expected<image::ImageBuffer<float>> apply_orientation(image::ImageBuffer<float>&& stored,
                                                      image::Orientation orientation,
                                                      const image::AllocationLimits& limits,
                                                      const core::CancellationToken& cancel, bool parallel) {
    if (stored.empty()) {
        return fail(ErrorCode::InvalidArgument, "cannot orient an empty image", kWhere);
    }
    if (!known(orientation)) {
        return fail(ErrorCode::InvalidArgument, "invalid orientation value", kWhere);
    }
    if (auto cancelled = cancel.check(kWhere); !cancelled) {
        return Unexpected(std::move(cancelled.error()));
    }
    if (orientation == image::Orientation::Normal) {
        return std::move(stored);
    }
    const image::Size size = stored.size();
    const image::Size out_size = image::oriented_size(size, orientation);
    const std::int32_t channels = stored.channels();
    auto out =
        image::ImageBuffer<float>::create_uninitialized(out_size.width, out_size.height, channels, limits);
    if (!out) {
        return Unexpected(std::move(out.error()));
    }
    // The mapping is affine: stored = origin + x * step (for output row y, column x).
    const image::ConstImageView<float> src = stored.const_view();
    const image::ImageView<float> dst = out->view();
    auto status = detail::for_each_row(out_size.height, parallel, cancel, kWhere, [&](std::int32_t y) {
        const image::Point first = image::map_from_oriented(image::Point{0, y}, size, orientation);
        const image::Point second =
            out_size.width > 1 ? image::map_from_oriented(image::Point{1, y}, size, orientation) : first;
        const std::int32_t dx = second.x - first.x;
        const std::int32_t dy = second.y - first.y;
        const auto bytes = static_cast<std::size_t>(channels) * sizeof(float);
        float* d = dst.row(y);
        std::int32_t sx = first.x;
        std::int32_t sy = first.y;
        for (std::int32_t x = 0; x < out_size.width; ++x, d += channels, sx += dx, sy += dy) {
            std::memcpy(d, src.pixel(sx, sy), bytes);
        }
    });
    if (!status) {
        return Unexpected(std::move(status.error()));
    }
    stored = {};
    return std::move(*out);
}

}  // namespace lucent::engine
