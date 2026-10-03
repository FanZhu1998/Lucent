#pragma once

// Display-ready preview pixels: fitting a rendered image to a preview size in linear light,
// 8-bit sRGB encoding for the screen, and the histogram shown beside it (blueprint section 2,
// "Detailed individual editing": fit / 100% zoom, histogram).
//
// Colour: the engine hands over display-referred linear Rec.2020 (engine::display_linear_rec2020).
// Downscaling happens on those linear values (area averaging of light, never of encoded values);
// the result is then converted to sRGB (IEC 61966-2-1) and quantised to 8 bits with the same
// colorimetry as the exporter's output conversion (exporter::encode_for_output, Little CMS,
// relative colorimetric): the source-to-sRGB matrix (Bradford adaptation when the whites differ),
// per-channel clipping of out-of-gamut values to [0, 1], the sRGB curve, and round-half-up
// quantisation. encode_display_srgb8 evaluates this directly (a 3 x 3 matrix and an exact
// table-assisted quantiser per channel, rows in parallel): 0.15 s for the 45 MP Canon EOS R5
// render on the reference laptop, against 2.1-2.3 s for Little CMS's floating-point transform.
// It is validated against encode_for_output: every code value agrees within 1; 0.16 % of the R5
// render's samples and 1.2 % of a dense synthetic colour cube differ by 1
// (tests/display_image_test.cpp and the fixture tests), because the exporter evaluates the
// embedded ICC profile's s15Fixed16-quantised colorants, adaptation and curve parameters in
// float32 (about 1e-5 relative), which rounds samples lying that close to a quantisation boundary
// the other way. A preview therefore matches a JPEG export of the same pixels to within one code
// value before JPEG compression.
//
// Thread-safety: pure functions; reentrant.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/camera_color.hpp>
#include <lucent/image/color_encoding.hpp>
#include <lucent/image/geometry.hpp>

namespace lucent::app {

// 8-bit sRGB pixels for the screen. Plain value; copyable.
struct DisplayImage {
    std::int32_t width = 0;
    std::int32_t height = 0;
    // width * height * 3 samples: rows tightly packed (stride width * 3), top row first, R G B,
    // sRGB-encoded code values 0..255, upright (orientation already applied by the render).
    std::vector<std::uint8_t> rgb;

    [[nodiscard]] static constexpr image::ColorEncoding encoding() noexcept {
        return image::ColorEncoding::srgb_display();
    }
    [[nodiscard]] std::size_t byte_size() const noexcept { return rgb.size(); }
};

// Histograms of a DisplayImage's code values. `luminance` bins the luma of the encoded values,
// round(0.2126 R' + 0.7152 G' + 0.0722 B') (Rec. 709 coefficients on sRGB-encoded values), the
// convention of photo editors' display histograms; it is not scene luminance.
struct Histogram {
    static constexpr std::size_t kBins = 256;
    std::array<std::uint32_t, kBins> red{};
    std::array<std::uint32_t, kBins> green{};
    std::array<std::uint32_t, kBins> blue{};
    std::array<std::uint32_t, kBins> luminance{};
    std::uint64_t pixels = 0;
};

// The size that fits `size` within a square of `long_edge` pixels, keeping the aspect ratio: the
// longer side becomes long_edge and the shorter one round(short * long_edge / long) (at least 1).
// Images that already fit are returned unchanged (never enlarged). An empty size, or long_edge
// < 1, returns `size`.
[[nodiscard]] image::Size fit_within(image::Size size, std::int32_t long_edge) noexcept;

// Area-average (box) downscale of a 3-channel linear image to `target`: each output pixel is the
// mean of the source area it covers, with fractional coverage at its edges (exact for any
// ratio, no aliasing beyond the box filter's). Computed in double precision per output row;
// rows are independent, so the result does not depend on `parallel`.
// Errors ("app.display.downscale"): InvalidArgument when the image is empty, not 3-channel RGB,
// not linear (TransferFunction::Linear), or `target` is empty or larger than the source on
// either axis; OutOfMemory; Cancelled.
[[nodiscard]] Expected<engine::ColorImage> downscale_area(const engine::ColorImage& source,
                                                          image::Size target,
                                                          const core::CancellationToken& cancel = {},
                                                          bool parallel = true);

// Converts a display-referred RGB image (for example the render's display_linear_rec2020 output)
// to 8-bit sRGB (see the file comment). Linear sources with RGB primaries take the direct path;
// any other display-referred encoding is converted by exporter::encode_for_output itself. The
// source contract is encode_for_output's. Errors ("app.display.encode"): InvalidArgument for an
// empty or non-3-channel image, a scene-referred, non-RGB or invalid encoding, or a non-finite
// sample (the message names the first one in row-major order); LimitExceeded above
// exporter::kMaxOutputDimension; OutOfMemory; Cancelled.
[[nodiscard]] Expected<DisplayImage> encode_display_srgb8(const engine::ColorImage& image,
                                                          const core::CancellationToken& cancel = {},
                                                          bool parallel = true);

[[nodiscard]] Histogram compute_histogram(const DisplayImage& image) noexcept;

// Clipping overlay of a DisplayImage (blueprint section 2, "clipping overlays"): per pixel, whether
// any channel reached the top code value (255: a highlight clipped in the displayed encoding, by
// the base look's white point or by the conversion to sRGB) or the bottom one (0: a shadow or an
// out-of-gamut component clipped to black). These are the pixels counted in the histogram's end
// bins, so the overlay and the histogram agree.
struct ClippingMask {
    static constexpr std::uint8_t kHighlight = 1;  // some channel is 255
    static constexpr std::uint8_t kShadow = 2;     // some channel is 0
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<std::uint8_t> flags;  // width * height, row-major, top row first: kHighlight | kShadow
    std::uint64_t highlight_pixels = 0;
    std::uint64_t shadow_pixels = 0;

    [[nodiscard]] std::size_t byte_size() const noexcept { return flags.size(); }
};

// The clipping mask of `image`. OutOfMemory ("app.display.clipping").
[[nodiscard]] Expected<ClippingMask> compute_clipping(const DisplayImage& image);

// compute_histogram() and compute_clipping() in one pass over the pixels, rows in parallel when
// asked (the result does not depend on it): what an interactive frame needs. OutOfMemory
// ("app.display.clipping").
struct DisplayAnalysis {
    Histogram histogram;
    ClippingMask clipping;
};
[[nodiscard]] Expected<DisplayAnalysis> analyze_display(const DisplayImage& image, bool parallel = true);

}  // namespace lucent::app
