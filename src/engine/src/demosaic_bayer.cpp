// Malvar-He-Cutler Bayer demosaicing (demosaic.hpp).
//
// H. S. Malvar, L.-W. He and R. Cutler, "High-quality linear interpolation for demosaicing of
// Bayer-patterned color images", ICASSP 2004. The four 5 x 5 filters (coefficients / 8) are
// those of the paper's Figure 2, in the arrangement of colour-demosaicing's
// demosaicing_CFA_Bayer_Malvar2004 (BSD-3-Clause), the numerical reference of this route:
//   G at R or B sites                    GR_GB
//   R (B) at G sites in R (B) rows       Rg_RB_Bg_BR  ("row" kernel: 4s left and right)
//   R (B) at G sites in B (R) rows       its transpose ("column" kernel: 4s above and below)
//   B at R sites and R at B sites        Rb_BB_Br_RR  ("diagonal" kernel)
// The kernels are symmetric, so correlation (used here) equals scipy's convolution.

#include <cstddef>
#include <cstdint>
#include <string>

#include <lucent/engine/demosaic.hpp>

#include "parallel_rows.hpp"

namespace lucent::engine {

namespace {

constexpr const char* kWhereInput = "engine.demosaic.input";
constexpr const char* kWhereRun = "engine.demosaic.run";

// Whole-sample-symmetric reflection for offsets of at most 2 (requires n >= 3): -k -> k and
// n - 1 + k -> n - 1 - k. It preserves the index parity and hence the Bayer phase.
constexpr std::int32_t mirror(std::int32_t i, std::int32_t n) noexcept {
    return i < 0 ? -i : (i >= n ? 2 * (n - 1) - i : i);
}

// The 5 x 5 neighbourhood of one site: rows[k] is the mosaic row at offset k - 2 (already
// mirrored), and xm2..xp2 the column indices at offsets -2..2 (already mirrored).
struct Neighbourhood {
    const float* const* rows;
    std::int32_t xm2, xm1, x, xp1, xp2;

    [[nodiscard]] float at(std::int32_t dy, std::int32_t column) const noexcept { return rows[dy + 2][column]; }
    [[nodiscard]] float centre() const noexcept { return at(0, x); }
    [[nodiscard]] float diagonals() const noexcept {
        return (at(-1, xm1) + at(-1, xp1)) + (at(1, xm1) + at(1, xp1));
    }

    // G at an R or B site.
    [[nodiscard]] float green() const noexcept {
        const float near = (at(0, xm1) + at(0, xp1)) + (at(-1, x) + at(1, x));
        const float far = (at(0, xm2) + at(0, xp2)) + (at(-2, x) + at(2, x));
        return (4.0f * centre() + 2.0f * near - far) * 0.125f;
    }
    // The colour of the horizontal neighbours at a G site.
    [[nodiscard]] float row_colour() const noexcept {
        return (5.0f * centre() + 4.0f * (at(0, xm1) + at(0, xp1)) - (at(0, xm2) + at(0, xp2)) - diagonals() +
                0.5f * (at(-2, x) + at(2, x))) *
               0.125f;
    }
    // The colour of the vertical neighbours at a G site.
    [[nodiscard]] float column_colour() const noexcept {
        return (5.0f * centre() + 4.0f * (at(-1, x) + at(1, x)) - (at(-2, x) + at(2, x)) - diagonals() +
                0.5f * (at(0, xm2) + at(0, xp2))) *
               0.125f;
    }
    // The opposite colour (B at R, R at B).
    [[nodiscard]] float diagonal_colour() const noexcept {
        const float far = (at(0, xm2) + at(0, xp2)) + (at(-2, x) + at(2, x));
        return (6.0f * centre() + 2.0f * diagonals() - 1.5f * far) * 0.125f;
    }
};

enum class Site : std::uint8_t { Red, Blue, GreenInRedRow, GreenInBlueRow };

void reconstruct(const Neighbourhood& n, Site site, float* out) noexcept {
    switch (site) {
        case Site::Red:
            out[0] = n.centre();
            out[1] = n.green();
            out[2] = n.diagonal_colour();
            break;
        case Site::Blue:
            out[0] = n.diagonal_colour();
            out[1] = n.green();
            out[2] = n.centre();
            break;
        case Site::GreenInRedRow:
            out[0] = n.row_colour();
            out[1] = n.centre();
            out[2] = n.column_colour();
            break;
        case Site::GreenInBlueRow:
            out[0] = n.column_colour();
            out[1] = n.centre();
            out[2] = n.row_colour();
            break;
    }
}

Status check_shapes(image::ConstImageView<float> mosaic, image::ImageView<float> rgb, std::int32_t min_size,
                    const char* route) {
    if (mosaic.empty() || mosaic.channels() != 1) {
        return fail(ErrorCode::InvalidArgument, std::string(route) + " needs a single-channel mosaic", kWhereInput);
    }
    if (mosaic.width() < min_size || mosaic.height() < min_size) {
        return fail(ErrorCode::InvalidArgument,
                    std::string(route) + " needs a mosaic of at least " + std::to_string(min_size) + " x " +
                        std::to_string(min_size) + " pixels",
                    kWhereInput);
    }
    if (rgb.empty() || rgb.channels() != 3 || rgb.width() != mosaic.width() || rgb.height() != mosaic.height()) {
        return fail(ErrorCode::InvalidArgument, std::string(route) + " needs a 3-channel output of the mosaic's size",
                    kWhereInput);
    }
    return ok();
}

}  // namespace

Status demosaic_bayer_mhc(image::ConstImageView<float> mosaic, const image::CfaPattern& cfa, image::ImageView<float> rgb,
                          const core::CancellationToken& cancel, bool parallel) {
    if (cfa.empty() || !cfa.is_bayer()) {
        return fail(ErrorCode::InvalidArgument,
                    "Malvar-He-Cutler needs a Bayer pattern, not '" + cfa.to_string() + "'", kWhereInput);
    }
    if (auto shapes = check_shapes(mosaic, rgb, 3, "Malvar-He-Cutler"); !shapes) {
        return shapes;
    }
    const std::int32_t width = mosaic.width();
    const std::int32_t height = mosaic.height();

    // Site kind for each of the 2 x 2 phases.
    Site sites[2][2] = {};
    for (std::int32_t r = 0; r < 2; ++r) {
        const bool red_row = cfa.color_at(r, 0) == image::CfaColor::Red || cfa.color_at(r, 1) == image::CfaColor::Red;
        for (std::int32_t c = 0; c < 2; ++c) {
            const image::CfaColor color = cfa.color_at(r, c);
            sites[r][c] = color == image::CfaColor::Red    ? Site::Red
                          : color == image::CfaColor::Blue ? Site::Blue
                          : red_row                        ? Site::GreenInRedRow
                                                           : Site::GreenInBlueRow;
        }
    }

    const auto run_row = [&](std::int32_t y) noexcept {
        const float* rows[5];
        for (std::int32_t k = 0; k < 5; ++k) {
            rows[k] = mosaic.row(mirror(y + k - 2, height));
        }
        float* out = rgb.row(y);
        const Site even = sites[y & 1][0];
        const Site odd = sites[y & 1][1];
        const auto pixel = [&](std::int32_t x, std::int32_t xm2, std::int32_t xm1, std::int32_t xp1,
                               std::int32_t xp2) {
            const Neighbourhood n{rows, xm2, xm1, x, xp1, xp2};
            reconstruct(n, (x & 1) == 0 ? even : odd, out + static_cast<std::ptrdiff_t>(x) * 3);
        };
        const auto border = [&](std::int32_t x) {
            pixel(x, mirror(x - 2, width), mirror(x - 1, width), mirror(x + 1, width), mirror(x + 2, width));
        };
        const std::int32_t left = width < 2 ? width : 2;
        for (std::int32_t x = 0; x < left; ++x) {
            border(x);
        }
        for (std::int32_t x = 2; x < width - 2; ++x) {
            pixel(x, x - 2, x - 1, x + 1, x + 2);
        }
        for (std::int32_t x = width - 2 > left ? width - 2 : left; x < width; ++x) {
            border(x);
        }
    };
    return detail::for_each_row(height, parallel, cancel, kWhereRun, run_row);
}

}  // namespace lucent::engine
