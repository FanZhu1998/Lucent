#pragma once

// Conversion of rendered float pixels into quantised output-profile code values (blueprint
// section 5 step 7: "Convert into the selected output profile and quantize only at encoding").
//
// Namespace lucent::exporter: see output_profile.hpp.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/export/output_profile.hpp>
#include <lucent/image/color_encoding.hpp>
#include <lucent/image/pixel_buffer.hpp>

namespace lucent::exporter {

// Quantised, interleaved RGB pixels in an output profile, ready for an encoder.
//
// Layout: rows are tightly packed (stride = width * 3 samples, no padding), top row first,
// samples in R, G, B order. Exactly one of samples8 / samples16 holds width * height * 3
// samples, selected by bit_depth; the other is empty. 16-bit samples are native (little-endian)
// uint16 with full-range scaling (code = round(value * 65535)); 8-bit samples use
// code = round(value * 255). Orientation is always "top-left" (row 0 is the top of the image);
// the render pipeline applies orientation before export.
//
// A plain value type: copyable, movable. Concurrent reads are safe.
struct OutputImage {
    static constexpr std::int32_t kChannels = 3;

    std::int32_t width = 0;
    std::int32_t height = 0;
    BitDepth bit_depth = BitDepth::Eight;
    OutputProfile profile = OutputProfile::Srgb;

    std::vector<std::uint8_t> samples8;    // bit_depth == Eight
    std::vector<std::uint16_t> samples16;  // bit_depth == Sixteen

    // Samples per row (width * 3).
    [[nodiscard]] std::size_t row_samples() const noexcept {
        return static_cast<std::size_t>(width) * static_cast<std::size_t>(kChannels);
    }

    // InvalidArgument unless width and height >= 1, bit_depth and profile are known values, the
    // buffer selected by bit_depth holds exactly width * height * 3 samples and the other buffer
    // is empty.
    [[nodiscard]] Status validate() const;
};

// Options for the Little CMS output conversion.
struct OutputTransformOptions {
    RenderingIntent intent = RenderingIntent::RelativeColorimetric;
    // Black-point compensation (Adobe BPC as implemented by Little CMS). Off by default: both
    // sides of the conversion are matrix/TRC spaces with a true zero black, where it would be a
    // no-op at best.
    bool black_point_compensation = false;
};

// Largest accepted width or height. Matches image::AllocationLimits::kDefaultMaxDimension; the
// JPEG writer has its own lower limit (65500).
inline constexpr std::int32_t kMaxOutputDimension = 65535;

// Largest accepted size of the quantised result, in bytes (2 GiB, the same ceiling as
// image::AllocationLimits::kDefaultMaxBytes).
inline constexpr std::size_t kMaxOutputBytes = std::size_t{2} << 30;

// Converts linear RGB pixels to `profile` and quantises them to `depth` bits.
//
// Source contract (checked, InvalidArgument otherwise):
// - `rgb` is non-empty with exactly 3 channels (RGB; alpha is not supported by the W2 writers);
// - `source` is valid, has ChannelLayout::Rgb, RGB primaries (not CameraNative or CieXyz) and
//   ImageReference::Display. The render pipeline's hand-off is display-referred linear Rec.2020
//   / D65; any primaries, white point and transfer function that lucent_image understands are
//   accepted. Scene-referred data must be rendered for display (tone-mapped) first: exporting it
//   directly would silently clip highlights, so it is rejected rather than guessed at.
// - every sample is finite. NaN or +/-Inf anywhere is InvalidArgument (the message names the
//   first offending pixel in row-major order); nothing is written or clamped silently.
//
// Conversion: the source transfer function is decoded with lucent_image's TransferCurve
// (identity for Linear), then Little CMS converts from a float matrix/TRC input profile built
// from the source primaries and white point (linear curves) to the cached output profile of
// icc_profile(profile), with `options.intent` and optional BPC. The transform is evaluated in
// floating point end to end (no 16-bit precalculated tables). White points are adapted with
// Bradford through the D50 PCS.
//
// Clipping and rounding (done by the Little CMS output formatter, the only place in the
// exporter where clipping happens; upstream render stages may already have limited values): each encoded channel value v is mapped to code = floor(v * (2^depth - 1) + 0.5)
// clamped to [0, 2^depth - 1], i.e. round-half-up. Values that are out of gamut (negative after
// the matrix) become 0 and values above 1 become the maximum code, per channel (no gamut
// mapping or hue preservation).
//
// Other errors: InvalidArgument for an unknown profile, depth or intent; LimitExceeded when a
// side exceeds kMaxOutputDimension or the result would exceed kMaxOutputBytes; OutOfMemory;
// Cancelled (checked between row bands); Internal when Little CMS cannot build the transform.
//
// Determinism: results depend only on the inputs and the library versions; the work is split
// into row bands processed on up to 8 threads, and each pixel is computed independently, so the
// thread count never affects the output.
//
// Thread-safety: safe to call concurrently. `rgb` must not be modified during the call.
[[nodiscard]] Expected<OutputImage> encode_for_output(image::ConstImageView<float> rgb,
                                                      const image::ColorEncoding& source,
                                                      OutputProfile profile, BitDepth depth,
                                                      const OutputTransformOptions& options = {},
                                                      core::CancellationToken cancel = {});

}  // namespace lucent::exporter
