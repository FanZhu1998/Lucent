#pragma once

// Embedded camera previews (blueprint section 2, "Import and organize": "An embedded camera JPEG
// may appear immediately for browsing, labeled until replaced by Lucent's own render. It must not
// be used as the RAW optimization input or imply exact preview/export agreement.").
//
// extract_embedded_preview() returns a usable JPEG preview that the camera (or the software that
// wrote the file) stored inside a RAW file, exactly as stored: the largest one, or the smallest
// one that is large enough for a requested size (see "Selection"). It identifies the
// file with the LibRaw adapter (the same read-only datastream as raw::RawDecoder, ADR 0005) and
// never unpacks the RAW pixel data: only the file's metadata is parsed, the header of every
// listed preview is read to learn its true dimensions, and the chosen preview's bytes are read
// with LibRaw::unpack_thumb_ex.
//
// What a preview is, and is not:
//   * A camera rendering (the camera's own white balance, tone curve, picture style, noise
//     reduction and often in-camera lens corrections), typically sRGB or Adobe RGB depending on
//     a camera setting that is not reported here. It is for browsing only and must be labelled
//     as a camera preview in the interface.
//   * Never analysis or optimisation input, never a stand-in for Lucent's render, and never
//     evidence that a file can be developed: a file whose RAW data Lucent cannot interpret may
//     still have a perfect preview.
//   * Its bytes are not decoded here. The JPEG marker structure up to the frame header is
//     validated; the entropy-coded data is not, so a display decoder can still fail on a
//     damaged preview and must handle that.
//
// Orientation rule: camera previews are stored in the sensor's stored orientation, like the RAW
// data. EmbeddedPreview::orientation is the RAW image's orientation as the decoder reports it
// (the value raw::RawMetadata::geometry.orientation carries) and is the ONLY orientation to apply
// for upright display. Any EXIF Orientation inside the JPEG stream itself must be ignored (for
// example QImageReader::setAutoTransform(false)); applying both could rotate twice. The pixels
// are returned unrotated.
//
// Selection: among the images the decoder lists (LibRaw's thumbnail list: TIFF IFDs, CR3
// preview tracks, RAF/ORF/... embedded JPEGs; at most 8), candidates must be JPEG streams that
// lie wholly inside the file, start with SOI, have a displayable frame header (baseline,
// extended or progressive Huffman, 8-bit, 1 or 3 components; see PreviewProblem) and respect
// EmbeddedPreviewLimits. Formats that are not JPEG (bitmaps, JPEG XL, Canon HEIF/H.265) are listed
// with PreviewProblem::NotJpeg and never returned. Among usable candidates:
//   * Without a requested size (EmbeddedPreviewOptions::min_long_edge == 0), the candidate with
//     the most pixels wins (ties: more bytes, then the earlier list position).
//   * With a requested size, the candidate with the fewest pixels whose long edge is at least
//     min_long_edge wins (ties: more bytes, then the earlier list position), so a thumbnail does
//     not read and decode a full-sensor JPEG when the file also carries a 1-2 MP preview. Only
//     candidates whose aspect ratio (long edge / short edge) is within kAspectTolerance of the
//     largest candidate's qualify: letterboxed or cropped thumbnails are never preferred. When no
//     candidate qualifies, the largest wins as without a size.
// If the decoder refuses the winner, the next candidate in the same order is tried.
//
// Thread-safety: safe to call concurrently (each call uses its own file handle and LibRaw
// instance).

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/core/file.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/raw/raw_decoder.hpp>

namespace lucent::raw {

// Encoding of a returned preview (append only).
enum class EmbeddedPreviewFormat : std::uint8_t {
    Jpeg = 1,  // a complete JPEG/JFIF/EXIF stream
};

// Why a listed image was not returned (append only).
enum class PreviewProblem : std::uint8_t {
    NotJpeg = 1,          // not a JPEG stream (bitmap, JPEG XL, H.265, unknown); see detail
    OutsideFile = 2,      // its declared byte range is empty or extends beyond the end of the file
    TooLarge = 3,         // more bytes or pixels than EmbeddedPreviewLimits allow
    InvalidJpeg = 4,      // no SOI, a malformed marker structure, or the decoder rejected it
    UnsupportedJpeg = 5,  // a valid JPEG that display decoders do not handle (lossless, 12-bit, ...)
};

[[nodiscard]] std::string_view to_string(EmbeddedPreviewFormat format) noexcept;  // "jpeg"
[[nodiscard]] std::string_view to_string(PreviewProblem problem) noexcept;        // "not_jpeg", ...

// Bounds that reject hostile or unexpectedly large previews before memory is committed. Camera
// previews are typically 0.2-8 MB and at most the sensor size (under 100 MP today).
struct EmbeddedPreviewLimits {
    static constexpr std::uint64_t kDefaultMaxBytes = std::uint64_t{64} << 20;  // 64 MiB
    static constexpr std::uint64_t kDefaultMaxPixels = std::uint64_t{1} << 27;  // 134 MP

    std::uint64_t max_bytes = kDefaultMaxBytes;    // size of the JPEG stream
    std::uint64_t max_pixels = kDefaultMaxPixels;  // width x height from the frame header

    // InvalidArgument ("raw.preview.limits") unless both bounds are at least 1.
    [[nodiscard]] Status validate() const;
};

struct EmbeddedPreviewOptions {
    // Relative difference of aspect ratios within which a smaller preview may stand in for the
    // largest one (preview dimensions are rounded, often to multiples of 8 or 16).
    static constexpr double kAspectTolerance = 0.02;

    EmbeddedPreviewLimits limits;
    // Long edge, in pixels, that the preview is wanted at (0: the largest preview). See the file
    // comment, "Selection". Must not be negative.
    std::int32_t min_long_edge = 0;
    // Files larger than this are refused (LimitExceeded) before the decoder sees them, as for
    // probing (raw::DecodeLimits::max_file_bytes).
    std::uint64_t max_file_bytes = DecodeLimits::kDefaultMaxFileBytes;
};

// One image the decoder lists, and whether it was usable.
struct PreviewCandidate {
    std::uint32_t list_index = 0;   // position in the decoder's list
    std::uint64_t file_offset = 0;  // declared byte range in the file
    std::uint64_t byte_length = 0;
    // Dimensions from the JPEG frame header when it was read, otherwise as the decoder declares
    // them (0 when it does not know).
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::optional<PreviewProblem> problem;  // std::nullopt: usable
    std::string detail;                     // developer-facing English; empty when usable
};

// The chosen preview. Plain value; safe to copy and share for reading.
struct EmbeddedPreview {
    EmbeddedPreviewFormat format = EmbeddedPreviewFormat::Jpeg;
    // The complete stream exactly as stored in the file (bytes [file_offset, file_offset +
    // bytes.size())), starting with SOI.
    std::vector<std::byte> bytes;
    // Stored (unrotated) pixel grid from the JPEG frame header.
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t components = 0;  // 1 (greyscale) or 3 (colour)
    // Orientation to apply for upright display (see the file comment); the pixels are unrotated.
    image::Orientation orientation = image::Orientation::Normal;
    std::uint32_t list_index = 0;
    std::uint64_t file_offset = 0;

    // Size of the upright image (width and height swapped for 90-degree orientations).
    [[nodiscard]] image::Size upright_size() const noexcept {
        return image::oriented_size(image::Size{width, height}, orientation);
    }
};

enum class EmbeddedPreviewStatus : std::uint8_t {
    Found = 1,       // EmbeddedPreviewResult::preview is set
    NoneInFile = 2,  // the decoder lists no embedded image at all
    NoneUsable = 3,  // images are listed but none is usable (see each candidate's problem)
};

[[nodiscard]] std::string_view to_string(EmbeddedPreviewStatus status) noexcept;  // "found", ...

struct EmbeddedPreviewResult {
    EmbeddedPreviewStatus status = EmbeddedPreviewStatus::NoneInFile;
    std::optional<EmbeddedPreview> preview;    // set exactly when status == Found
    std::vector<PreviewCandidate> candidates;  // every listed image in list order
    // Identity of the file the preview was read from; verified unchanged when the call finished.
    core::FileIdentity source_identity;
    std::string decoder;  // adapter that listed the images, e.g. "libraw"
};

// Extracts an embedded JPEG preview of the RAW file at `path`, chosen as the file comment's
// "Selection" describes. A file without a usable preview is not an error: the result says so explicitly
// (NoneInFile / NoneUsable) and lists every candidate's problem.
//
// Errors (Error::where "raw.preview.*" for Lucent's checks, "raw.libraw.<stage>" for LibRaw
// failures, mapped as for raw::RawDecoder): InvalidArgument for invalid options (limits below 1,
// a negative min_long_edge, max_file_bytes 0); NotFound,
// PermissionDenied, IoError when the file cannot be read; LimitExceeded when it is larger than
// max_file_bytes; Unsupported when the decoder does not recognise the file (an empty file
// included); Corrupt when its structure is rejected; Cancelled; SourceChanged when the file
// changed while it was being read; OutOfMemory; Internal for adapter defects. No exception
// escapes.
[[nodiscard]] Expected<EmbeddedPreviewResult> extract_embedded_preview(
    const std::filesystem::path& path, const EmbeddedPreviewOptions& options = {},
    core::CancellationToken cancel = {});

}  // namespace lucent::raw
