#pragma once

// Private JPEG (libjpeg-turbo) and TIFF (libtiff) encoders and verification readers.
// Third-party headers stay in the .cpp files.
//
// JPEG is encoded into memory (encode_jpeg) and the bytes are written by the caller, so the
// written file can be compared byte for byte with the encoder's output. TIFF is written by
// libtiff straight to the (already reserved) temporary file; its verification compares every
// decoded row with the encoded samples. Paths go to the wide-character APIs (_wfopen_s,
// TIFFOpenWExt) through api_path(), so Unicode and long paths work.
//
// Cancellation: every encoder and reader checks its token every kCodecCancelRows scanlines and
// returns ErrorCode::Cancelled; the caller removes the temporary file.
//
// Memory: no function holds a second full-size copy of the image. encode_jpeg holds the
// compressed stream (grown by doubling); the readers decode one row at a time unless asked to
// keep the samples (tests).
//
// Thread-safety: all functions are safe to call concurrently on different files. libjpeg errors
// are captured per call (a jmp_buf per compress/decompress object); libtiff errors per handle
// (TIFFOpenOptions ExtR handlers), never through process-global handlers.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/export/encode.hpp>
#include <lucent/export/export.hpp>

namespace lucent::exporter::detail {

// Scanlines between two cancellation checks in the codecs.
inline constexpr std::int32_t kCodecCancelRows = 64;

// What a verification reader found in a file.
struct DecodedFile {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t channels = 0;
    std::int32_t bits_per_sample = 0;
    std::vector<std::uint8_t> icc;  // embedded profile, empty when absent

    // Interleaved, tightly packed samples, only when the reader was asked to keep them:
    // samples8 for 8-bit files, samples16 for 16-bit.
    std::vector<std::uint8_t> samples8;
    std::vector<std::uint16_t> samples16;

    // TIFF read with an expected image: whether every row was compared, and the first row whose
    // samples differ from it (-1 when all rows match).
    bool samples_compared = false;
    std::int64_t first_mismatched_row = -1;

    std::int32_t warnings = 0;  // decoder warnings (any warning fails verification)
    std::string first_warning;

    // JPEG only: luma sampling factors (1x1 for 4:4:4, 2x2 for 4:2:0); chroma is always 1x1.
    std::int32_t jpeg_luma_h_samp = 0;
    std::int32_t jpeg_luma_v_samp = 0;
    bool jpeg_ycbcr = false;  // stored colour space is YCbCr

    // TIFF only.
    std::int32_t tiff_directories = 0;
    std::int32_t tiff_photometric = 0;
    std::int32_t tiff_planar = 0;
    std::int32_t tiff_orientation = 0;
    std::int32_t tiff_sample_format = 0;
    std::int32_t tiff_compression = 0;
};

// Encodes an 8-bit OutputImage as a complete JPEG stream in memory, with the ICC profile in APP2
// markers (none when `icc` is empty), optimised Huffman tables, islow DCT, 4:4:4 from
// kJpegFullChromaMinQuality. Errors (export.jpeg.encode): InvalidArgument for a 16-bit image,
// LimitExceeded above kJpegMaxDimension, OutOfMemory, Internal for other libjpeg errors,
// Cancelled.
[[nodiscard]] Expected<std::vector<std::uint8_t>> encode_jpeg(const OutputImage& image, const JpegSettings& settings,
                                                              std::span<const std::uint8_t> icc,
                                                              const core::CancellationToken& cancel = {});

// Fully decodes a JPEG to 8-bit RGB, one row at a time. keep_samples fills samples8 with the
// whole image (tests); verification passes false. Corrupt when libjpeg reports an error;
// Cancelled (export.jpeg.verify).
[[nodiscard]] Expected<DecodedFile> read_jpeg(const std::filesystem::path& path,
                                              const core::CancellationToken& cancel = {}, bool keep_samples = true);

// Encodes a 16-bit OutputImage as a single-directory TIFF (RGB, chunky, orientation 1) into the
// existing file `path` (truncated). Errors (export.tiff.write): IoError / DiskFull /
// PermissionDenied for file failures, InvalidArgument for an 8-bit image, Internal when a field
// cannot be set, Cancelled.
[[nodiscard]] Status write_tiff(const std::filesystem::path& path, const OutputImage& image,
                                const TiffSettings& settings, std::span<const std::uint8_t> icc,
                                const core::CancellationToken& cancel = {});

// Decodes a TIFF's first directory. Only 16-bit unsigned chunky RGB is decoded; other layouts
// return the header fields with no samples (verification then rejects them). With `expected`
// (verification) every decoded row is compared with the matching row of expected->samples16
// through a one-row buffer (samples_compared, first_mismatched_row; reading stops at the first
// mismatch) and samples16 stays empty; without it samples16 receives the whole image. Corrupt
// when libtiff reports an error; Cancelled (export.tiff.verify).
[[nodiscard]] Expected<DecodedFile> read_tiff(const std::filesystem::path& path,
                                              const core::CancellationToken& cancel = {},
                                              const OutputImage* expected = nullptr);

// A JPEG stream in memory, decoded to 8-bit interleaved RGB (gray and YCbCr are converted by
// libjpeg; other colour spaces are refused).
struct DecodedJpegStream {
    std::int32_t width = 0;  // after DCT scaling
    std::int32_t height = 0;
    std::vector<std::uint8_t> rgb;  // width * height * 3
    std::vector<std::uint8_t> icc;  // embedded profile, empty when absent
};

// Size of the image in a JPEG stream (header only). Corrupt when libjpeg cannot read the header.
[[nodiscard]] Expected<std::pair<std::int32_t, std::int32_t>> read_jpeg_stream_size(
    std::span<const std::uint8_t> jpeg);

// Decodes a JPEG stream at the DCT scale 1 / scale_denom (1, 2, 4 or 8: libjpeg reduces while it
// decodes, much faster than decoding at full size). Errors (export.jpeg.decode): InvalidArgument
// for another scale; Corrupt when libjpeg reports an error; Unsupported for colour spaces other
// than gray, RGB and YCbCr (CMYK, YCCK); LimitExceeded above kJpegMaxDimension; OutOfMemory;
// Cancelled.
[[nodiscard]] Expected<DecodedJpegStream> decode_jpeg_stream(std::span<const std::uint8_t> jpeg,
                                                             std::int32_t scale_denom,
                                                             const core::CancellationToken& cancel = {});

// "libjpeg-turbo 3.2.0" / "libtiff 4.7.2" (versions of the headers built against).
[[nodiscard]] std::string jpeg_library_version();
[[nodiscard]] std::string tiff_library_version();

}  // namespace lucent::exporter::detail
