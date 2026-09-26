#pragma once

// Private per-pixel helpers shared by the colour and rendering stages.

#include <cmath>
#include <cstdint>
#include <string_view>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/image/color_math.hpp>
#include <lucent/image/pixel_buffer.hpp>

#include "parallel_rows.hpp"

namespace lucent::engine::detail {

// out = matrix * rgb for every pixel of a 3-channel view, in place. The matrix is converted to
// float once and every pixel uses the same evaluation order as image::apply_matrix, so results
// are identical to it and independent of the schedule. The caller has validated the view and
// the matrix.
[[nodiscard]] inline Status apply_matrix_rows(image::ImageView<float> rgb, const image::Matrix3d& matrix,
                                              const core::CancellationToken& cancel, bool parallel,
                                              std::string_view where) {
    const auto f = [&matrix](std::size_t r, std::size_t c) { return static_cast<float>(matrix(r, c)); };
    const float m00 = f(0, 0), m01 = f(0, 1), m02 = f(0, 2);
    const float m10 = f(1, 0), m11 = f(1, 1), m12 = f(1, 2);
    const float m20 = f(2, 0), m21 = f(2, 1), m22 = f(2, 2);
    const std::int32_t width = rgb.width();
    return for_each_row(rgb.height(), parallel, cancel, where, [&](std::int32_t y) {
        float* p = rgb.row(y);
        for (std::int32_t x = 0; x < width; ++x, p += 3) {
            const float r = p[0];
            const float g = p[1];
            const float b = p[2];
            p[0] = m00 * r + m01 * g + m02 * b;
            p[1] = m10 * r + m11 * g + m12 * b;
            p[2] = m20 * r + m21 * g + m22 * b;
        }
    });
}

}  // namespace lucent::engine::detail
