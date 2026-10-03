#pragma once

// Reduced JPEG thumbnails made from a larger JPEG, typically a camera's embedded preview, for the
// disposable thumbnail cache (blueprint section 7: bounded caches; section 8: thumbnails in the
// per-user cache directory). Many cameras embed only a tiny thumbnail and a full-size JPEG of
// several megabytes; storing the latter verbatim would let a cache of a few hundred megabytes hold
// only a few hundred photos.
//
// make_jpeg_thumbnail() works in memory and does no colour management: samples keep the
// preview's own encoding (typically sRGB) and an embedded ICC profile is carried over. It decodes
// with libjpeg's DCT-domain scaling (1/2, 1/4 or 1/8: the strongest reduction whose long edge is
// still at least max_long_edge), averages areas of the encoded values down to a long edge of
// exactly max_long_edge (aspect ratio kept, the short side rounded, at least 1 pixel) and encodes a
// baseline JPEG at `quality` (4:2:0 below kJpegFullChromaMinQuality, optimised Huffman tables, no
// EXIF: the caller keeps the orientation). A JPEG whose long edge is at most max_long_edge already
// is returned unchanged (its bytes, `reduced` false) when it has at most max_unreduced_bytes, and
// otherwise re-encoded at its own size. Deterministic: the same input and options give the same
// bytes with the same libjpeg-turbo build.
//
// Thread-safety: safe to call concurrently.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>

namespace lucent::exporter {

struct JpegThumbnailOptions {
    std::int32_t max_long_edge = 1024;  // >= 1
    std::int32_t quality = 85;          // 1..100
    // A JPEG that is small enough in pixels but larger than this is re-encoded at `quality`.
    std::size_t max_unreduced_bytes = std::numeric_limits<std::size_t>::max();
};

struct JpegThumbnail {
    std::vector<std::uint8_t> jpeg;
    std::int32_t width = 0;
    std::int32_t height = 0;
    bool reduced = false;  // false: the input is returned as it was
};

// See the file comment. Errors ("export.thumbnail" / "export.jpeg.decode"): InvalidArgument
// (max_long_edge < 1, quality outside 1..100, an empty input); Corrupt (not a JPEG stream libjpeg
// can decode); Unsupported (CMYK or YCCK); LimitExceeded (a side above kJpegMaxDimension);
// OutOfMemory; Cancelled.
[[nodiscard]] Expected<JpegThumbnail> make_jpeg_thumbnail(std::span<const std::uint8_t> jpeg,
                                                          const JpegThumbnailOptions& options,
                                                          const core::CancellationToken& cancel = {});

}  // namespace lucent::exporter
