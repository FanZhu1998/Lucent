#pragma once

// Encoding, verification and no-clobber publication of exported files (blueprint section 5
// step 8 and section 7 "Export integrity and user-selected destinations").
//
// Namespace lucent::exporter: see output_profile.hpp.
//
// Publication protocol of write_verified():
//  1. Validate settings, image and destination (the parent directory must exist; a destination
//     that already exists is AlreadyExists before any work is done). Prepare the metadata:
//     prepare_metadata(metadata, settings.metadata) (metadata.hpp).
//  2. JPEG only: encode into memory (before any file exists). Marker order: SOI, APP0 (JFIF),
//     APP1 "Exif" (only when the prepared metadata is not empty), APP2 ICC profile, image data.
//  3. Reserve a temporary sibling "~lucent-<16 hex digits>.tmp" in the destination directory
//     (same filesystem, created with CREATE_NEW so it never replaces anything).
//  4. Write the JPEG bytes, or encode the TIFF, into the temporary file; flush the
//     operating-system buffers (FlushFileBuffers) and close it.
//  5. Re-open and verify the temporary file. JPEG: every byte must equal the encoder's output
//     (so any corruption of the entropy-coded data is caught, which a decode alone would miss),
//     then a full decode. TIFF: a full decode whose every row must equal the intended samples
//     (compared row by row, without a second full-size copy). In both cases the format,
//     dimensions, channel count, bit depth, orientation (TIFF), embedded ICC bytes (must equal
//     icc_profile(settings.profile) exactly) must match and the decoder must report no warnings.
//     Then the metadata is read back with Lucent's own EXIF parser and must match the prepared
//     metadata: values (exact, fractions included), the
//     presence of the Exif and GPS IFDs (GPS absent unless MetadataPolicy::include_gps), only
//     the documented tags (never a maker note or thumbnail) and, for JPEG, Orientation 1.
//  6. SHA-256 the temporary file with lucent::core::hash_file (stable read).
//  7. Publish with MoveFileExW(temp, destination, MOVEFILE_WRITE_THROUGH) without
//     MOVEFILE_REPLACE_EXISTING: if anything appeared at the destination meanwhile, the move
//     fails with AlreadyExists and the existing file is left untouched. Nothing is ever
//     overwritten.
// On every failure after step 3 the temporary file is deleted before returning. A process that
// is killed between steps 3 and 7 leaves its temporary file behind (W7's publication journal
// owns that cleanup).
//
// Memory beyond the caller's OutputImage: JPEG, the compressed stream (grown by doubling, about
// 1 to 2 bytes per pixel at quality 95) and one decoded row; TIFF, libtiff's strip buffers and
// two rows.
//
// Durability: the data is flushed before publication and the rename is write-through. Network
// and removable destinations may offer weaker guarantees; W7's publication journal owns
// reconciliation after interruptions.
//
// Thread-safety: all functions are safe to call concurrently (different destinations, or the
// same destination, in which case at most one call publishes and the others fail with
// AlreadyExists).

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/export/encode.hpp>
#include <lucent/export/metadata.hpp>
#include <lucent/export/output_profile.hpp>
#include <lucent/image/color_encoding.hpp>
#include <lucent/image/pixel_buffer.hpp>

namespace lucent::exporter {

// Version of the export path's semantics: the quantisation and output conversion (encode.hpp), the
// embedded ICC profiles (output_profile.hpp), the JPEG and TIFF writers' settings and the metadata
// written (metadata.hpp). Bump when the same rendered image and settings would be written with other
// samples, profile bytes or tags. It is part of the camera support database's qualification basis
// (ADR 0018): a Verified mode was checked through this export path.
inline constexpr std::uint32_t kExportVersion = 1;

// File format. Persisted (receipts, export presets); append only.
enum class ExportFormat : std::uint8_t {
    Jpeg = 1,  // baseline JPEG/JFIF, 8-bit YCbCr, libjpeg-turbo
    Tiff = 2,  // TIFF 6.0, 16-bit RGB, chunky (interleaved), libtiff
};

// JPEG chroma subsampling, derived from the quality (see jpeg_chroma_subsampling()).
// Persisted in receipts; append only.
enum class ChromaSubsampling : std::uint8_t {
    Yuv444 = 1,  // no chroma subsampling
    Yuv420 = 2,  // chroma halved horizontally and vertically
};

// TIFF compression. Persisted; append only.
enum class TiffCompression : std::uint8_t {
    None = 1,
    Deflate = 2,  // Adobe Deflate (compression tag 8), zlib level 6, horizontal predictor (2)
};

[[nodiscard]] std::string_view to_string(ExportFormat format) noexcept;
[[nodiscard]] std::string_view to_string(ChromaSubsampling subsampling) noexcept;
[[nodiscard]] std::string_view to_string(TiffCompression compression) noexcept;

// Bit depth written for a format: 8 for JPEG, 16 for TIFF. Eight for an unknown format.
[[nodiscard]] BitDepth bit_depth_for(ExportFormat format) noexcept;

inline constexpr std::int32_t kJpegDefaultQuality = 95;
// At or above this quality JPEG chroma is not subsampled (4:4:4); below it 4:2:0 is used.
// Subsampling artefacts on saturated edges are visible at high quality settings, where the
// file-size saving no longer justifies them.
inline constexpr std::int32_t kJpegFullChromaMinQuality = 90;
// Largest JPEG width or height (the JPEG/libjpeg limit JPEG_MAX_DIMENSION).
inline constexpr std::int32_t kJpegMaxDimension = 65500;

// Subsampling used for `quality` (Yuv444 when quality >= kJpegFullChromaMinQuality).
[[nodiscard]] ChromaSubsampling jpeg_chroma_subsampling(std::int32_t quality) noexcept;

struct JpegSettings {
    // libjpeg quality scale, 1..100 (IJG tables scaled by jpeg_set_quality, baseline-limited).
    // Encoding always uses optimised Huffman tables, the accurate integer DCT (JDCT_ISLOW, which
    // libjpeg-turbo computes bit-exactly with or without SIMD), a JFIF APP0 marker and the ICC
    // profile in APP2 markers (jpeg_write_icc_profile).
    std::int32_t quality = kJpegDefaultQuality;
};

struct TiffSettings {
    TiffCompression compression = TiffCompression::Deflate;
};

// Everything that determines the bytes of an exported file (besides the pixels).
struct ExportSettings {
    ExportFormat format = ExportFormat::Jpeg;
    OutputProfile profile = OutputProfile::Srgb;
    OutputTransformOptions transform;
    JpegSettings jpeg;  // used when format == Jpeg
    TiffSettings tiff;  // used when format == Tiff
    // Which metadata groups are written (GPS off by default). See metadata.hpp.
    MetadataPolicy metadata;

    // Blueprint section 7 starting presets.
    // JPEG / sRGB / quality 95.
    [[nodiscard]] static ExportSettings jpeg_srgb_q95() noexcept { return ExportSettings{}; }
    // 16-bit TIFF / `profile` / Deflate.
    [[nodiscard]] static ExportSettings tiff16(OutputProfile profile) noexcept {
        ExportSettings settings;
        settings.format = ExportFormat::Tiff;
        settings.profile = profile;
        return settings;
    }

    // InvalidArgument for unknown enum values or a JPEG quality outside 1..100.
    [[nodiscard]] Status validate() const;
};

// Record of a published file (blueprint section 7: "Persist a receipt with output hash and
// settings"). Settings are recorded as requested; derived choices are recorded explicitly.
struct ExportReceipt {
    std::filesystem::path path;  // the published file (the destination as given)
    ExportFormat format = ExportFormat::Jpeg;
    OutputProfile profile = OutputProfile::Srgb;
    BitDepth bit_depth = BitDepth::Eight;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::uint64_t byte_size = 0;  // size of the published file
    core::Digest256 sha256{};     // SHA-256 of the published file's bytes
    core::Digest256 icc_sha256{};  // SHA-256 of the embedded ICC profile bytes
    ExportSettings settings;
    ChromaSubsampling chroma_subsampling = ChromaSubsampling::Yuv444;  // meaningful for JPEG only
    // Libraries that produced the file, e.g. "libjpeg-turbo 3.2.0; lcms 2.19".
    std::string encoder;
    // The metadata written (prepare_metadata's result, verified in the file) and the notes about
    // values prepare_metadata had to change or drop. Part of the hashed bytes.
    ExportMetadata metadata;
    std::vector<std::string> metadata_notes;
};

// Writes `image` with `metadata` (filtered by settings.metadata) to `destination` with the
// protocol described at the top of this file. The written bytes, and so the receipt's hash,
// include the metadata; identical inputs (image, metadata, settings, Lucent version) produce
// identical files.
//
// Preconditions (InvalidArgument otherwise): settings.validate() passes; image.validate()
// passes; image.bit_depth == bit_depth_for(settings.format); image.profile == settings.profile;
// `destination` is non-empty, names a file (has a filename) and contains no NUL.
//
// Errors: AlreadyExists when the destination exists (checked first, and again atomically at
// publication); NotFound when the destination directory does not exist; LimitExceeded when a
// JPEG side exceeds kJpegMaxDimension; PermissionDenied / DiskFull / IoError from the file
// system; Corrupt when the written file fails verification; Cancelled (checked between the
// protocol steps, every 64 scanlines while encoding and decoding, per 4 MiB while writing and
// comparing JPEG bytes, and while hashing); Internal for codec invariant failures.
[[nodiscard]] Expected<ExportReceipt> write_verified(const OutputImage& image,
                                                     const std::filesystem::path& destination,
                                                     const ExportSettings& settings,
                                                     const ExportMetadata& metadata,
                                                     core::CancellationToken cancel = {});

// write_verified() without photograph metadata (ExportMetadata{}): only what the policy adds by
// itself, i.e. Software when settings.metadata.include_software is set.
[[nodiscard]] Expected<ExportReceipt> write_verified(const OutputImage& image,
                                                     const std::filesystem::path& destination,
                                                     const ExportSettings& settings,
                                                     core::CancellationToken cancel = {});

// encode_for_output(rgb, source, settings.profile, bit_depth_for(settings.format),
// settings.transform) followed by write_verified(). The destination is checked before the
// (comparatively expensive) conversion, so an existing target fails fast.
[[nodiscard]] Expected<ExportReceipt> export_image(image::ConstImageView<float> rgb,
                                                   const image::ColorEncoding& source,
                                                   const std::filesystem::path& destination,
                                                   const ExportSettings& settings,
                                                   const ExportMetadata& metadata,
                                                   core::CancellationToken cancel = {});

// export_image() without photograph metadata (see the write_verified overload above).
[[nodiscard]] Expected<ExportReceipt> export_image(image::ConstImageView<float> rgb,
                                                   const image::ColorEncoding& source,
                                                   const std::filesystem::path& destination,
                                                   const ExportSettings& settings,
                                                   core::CancellationToken cancel = {});

}  // namespace lucent::exporter
