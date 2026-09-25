#pragma once

// The RawDecoder interface (blueprint section 3, "Adapter boundary"): probe and decode with
// cancellation, limits and structured errors. Adapters convert a decoder library's state into
// the data contract of decoded_raw.hpp; nothing of the library leaks through this interface.
//
// Error model (lucent::Error). Every adapter reports failures with these codes; Error::where is
// "raw.<adapter>.<stage>" (for example "raw.libraw.unpack") and Error::native_code holds the
// library's own code when there is one:
//   NotFound          the file does not exist, or the requested subimage does not exist
//   PermissionDenied  the OS refused access
//   IoError           a read failed
//   Unsupported       not a format this adapter understands, or a mode it cannot decode
//   Corrupt           truncated or malformed data, or the decoder reported data errors
//   LimitExceeded     a DecodeLimits bound (or the decoder's own allocation ceiling) was hit
//   OutOfMemory       an allocation failed
//   Cancelled         the CancellationToken was cancelled
//   SourceChanged     the file changed while it was being read; re-import it
//   InvalidArgument   invalid options, including a missing subimage choice for a multi-image file
//   Internal          an adapter defect (an unexpected exception or broken invariant)

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/raw/decoded_raw.hpp>

namespace lucent::raw {

// Resource bounds for one probe or decode. They reject hostile or unexpectedly large inputs
// before memory is committed; they are not a scheduler budget (blueprint section 7).
//
// Defaults are chosen for the 12-16 GB reference laptops:
//   max_file_bytes   = 2 GiB: LibRaw's own ceiling for non-DNG files and far above current
//                      single-shot RAW files (about 150-400 MB for 100-150 MP medium format).
//   max_pixels       = 2^28 (268 million) raw-frame sites: above the 240 MP pixel-shift
//                      composites of current bodies, while a 16-bit mosaic of that size needs
//                      512 MB.
//   max_memory_bytes = 2 GiB: bound on the pixel memory of a decode, i.e. the decoder's own raw
//                      buffer plus Lucent's copy, estimated from the dimensions before any
//                      pixel is unpacked; also passed to the decoder as its allocation ceiling
//                      (LibRaw max_raw_memory_mb, whose default is the same 2048 MB). A 61 MP
//                      mosaic needs about 250 MB; a single decode can therefore never take more
//                      than a quarter of the 8 GB Lucent budget of section 7.
struct DecodeLimits {
    static constexpr std::uint64_t kDefaultMaxFileBytes = std::uint64_t{2} << 30;
    static constexpr std::uint64_t kDefaultMaxPixels = std::uint64_t{1} << 28;
    static constexpr std::uint64_t kDefaultMaxMemoryBytes = std::uint64_t{2} << 30;

    std::uint64_t max_file_bytes = kDefaultMaxFileBytes;
    std::uint64_t max_pixels = kDefaultMaxPixels;
    std::uint64_t max_memory_bytes = kDefaultMaxMemoryBytes;

    // InvalidArgument ("raw.limits") unless every bound is at least 1.
    [[nodiscard]] Status validate() const;
};

struct ProbeOptions {
    // Image whose metadata is reported; std::nullopt means index 0. Probing never selects an
    // image for processing: the result states subimage_count and lists all images.
    std::optional<std::uint32_t> subimage;

    // Probing enforces max_file_bytes (before opening) and passes max_memory_bytes to the
    // decoder's metadata parser. max_pixels is not enforced: a probe reports the dimensions so
    // that callers can explain why a decode would be refused.
    DecodeLimits limits;

    // Describe every image of a multi-image file in ProbeResult::subimages (one extra metadata
    // parse per additional image; nothing is unpacked). At most kMaxDescribedSubimages images are
    // described; the count is always reported.
    bool describe_subimages = true;
    static constexpr std::uint32_t kMaxDescribedSubimages = 16;
};

struct DecodeOptions {
    // Image to decode. Required when the file holds more than one image (otherwise the decode
    // fails with InvalidArgument at "raw.<adapter>.subimage"); may be omitted for single-image
    // files, where it means index 0. An index >= the image count fails with NotFound.
    std::optional<std::uint32_t> subimage;

    // All three bounds are enforced: max_file_bytes before opening, max_pixels and
    // max_memory_bytes after the file is identified and before any pixel data is unpacked.
    DecodeLimits limits;
};

// Interface of a RAW decoder adapter.
//
// Contract for implementations:
//   * probe() identifies the file and reports RawMetadata (stage AfterOpen) without unpacking
//     pixel data. decode() also unpacks and copies the pixels into Lucent-owned buffers and
//     reports the final metadata (stage AfterUnpack); check_consistency() holds for its result.
//   * Neither function writes to the source file; both read it with sharing that lets other
//     programs read, write, rename or delete it, and fail with SourceChanged when the file object
//     or its content version (core::FileIdentity: size and last write time) changed while it was
//     being read. Writes that leave both unchanged, such as writes through a memory-mapped view,
//     are not detected (see core::hash_file).
//   * The token is checked before work starts and at safe points during the call; a cancelled
//     call returns ErrorCode::Cancelled and never a partial result.
//   * No exception escapes (std::bad_alloc becomes OutOfMemory; anything else Internal).
//
// Thread-safety: implementations are immutable after construction and their const member
// functions may be called concurrently from any number of threads, each call using its own
// decoder state.
class RawDecoder {
public:
    virtual ~RawDecoder() = default;

    RawDecoder(const RawDecoder&) = delete;
    RawDecoder& operator=(const RawDecoder&) = delete;

    // Stable identifier of the adapter, e.g. "libraw". Lower-case ASCII; used in reports and
    // Error::where.
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;

    [[nodiscard]] virtual Expected<ProbeResult> probe(const std::filesystem::path& path,
                                                      const ProbeOptions& options,
                                                      core::CancellationToken cancel) const = 0;

    [[nodiscard]] virtual Expected<DecodedRaw> decode(const std::filesystem::path& path,
                                                      const DecodeOptions& options,
                                                      core::CancellationToken cancel) const = 0;

protected:
    RawDecoder() = default;
};

}  // namespace lucent::raw
