#pragma once

// Minimal 32-bit floating-point RGB TIFF writer and reader for lucent-render's --dump-linear
// (reference comparisons, research/tools/render_reference.py). Developer-tool code, not part of
// any module's public API; exported images use lucent::exporter.
//
// File layout (TIFF 6.0 baseline structure with the SampleFormat extension, little-endian):
//   * one IFD: ImageWidth, ImageLength, BitsPerSample 32/32/32, Compression 1 (none),
//     PhotometricInterpretation 2 (RGB), ImageDescription (the colour encoding), StripOffsets,
//     SamplesPerPixel 3, RowsPerStrip, StripByteCounts, PlanarConfiguration 1 (chunky),
//     Software, SampleFormat 3/3/3 (IEEE floating point);
//   * pixel data after the IFD: rows top to bottom, R G B interleaved, IEEE-754 binary32
//     little-endian, strips of kRowsPerStrip rows stored contiguously.
// Classic TIFF offsets are 32-bit, so the file must stay below 4 GiB (about 350 megapixels).
// No ICC profile is embedded: the ImageDescription names the encoding (for example
// "lucent scene-linear rgb rec709-srgb d65 linear unbounded scene"); readers must not assume sRGB
// transfer.

#include <cstdint>
#include <filesystem>
#include <string_view>

#include <lucent/core/error.hpp>
#include <lucent/image/pixel_buffer.hpp>

namespace lucent::render_tool {

inline constexpr std::int32_t kRowsPerStrip = 16;

// Writes `rgb` (3 channels, finite or not; values are written as they are) to `path`, replacing
// an existing file. Errors (lucent-render.tiff): InvalidArgument for an empty or non-3-channel
// view; LimitExceeded when the file would reach 4 GiB; IoError when the file cannot be written.
[[nodiscard]] Status write_float_tiff(const std::filesystem::path& path, image::ConstImageView<float> rgb,
                                      std::string_view description);

// Reads a file in exactly the layout write_float_tiff() produces (any strip size): little-endian,
// uncompressed, chunky 3 x 32-bit IEEE float. Errors (lucent-render.tiff): NotFound / IoError;
// Unsupported for any other TIFF layout; Corrupt for truncated or inconsistent structure.
[[nodiscard]] Expected<image::ImageBuffer<float>> read_float_tiff(const std::filesystem::path& path);

}  // namespace lucent::render_tool
