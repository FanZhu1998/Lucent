#pragma once

// The LibRaw adapter: a RawDecoder backed by LibRaw (the thread-safe libraw_r build, linked
// privately to lucent_raw). No LibRaw type appears in this header.
//
// Behaviour (details in docs/adr/0005-raw-decoder-boundary.md):
//   * One LibRaw instance per call, so concurrent calls never share decoder state.
//   * The file is opened once with lucent::core::ReadOnlyFile (share mode READ|WRITE|DELETE,
//     long and Unicode paths) and fed to LibRaw through a datastream over that handle, so the
//     bytes LibRaw decodes are exactly those of the FileIdentity captured through the handle.
//     The identity is re-checked through the handle and the path when the call finishes.
//   * Files LibRaw identifies as DNG are also inspected by Lucent's TIFF/DNG inspector
//     (tiff_inspector.hpp), which supplies DNG opcode lists (required operations), NoiseProfile
//     (from the raw IFD, else IFD 0), BlackLevelDeltaH/V, the camera-profile colour tags and the
//     compression of the raw IFD. For other formats the compression is classified from LibRaw's
//     codec and the camera's recorded mode (DataCompression).
//   * Subimage roles (Canon Dual Pixel, RAW burst, Pentax pixel shift) and dual-fisheye 360
//     sources are identified; see SubimageRole and ProjectionInfo.
//   * LibRaw options are LibRaw's defaults, except: the subimage (shot_select), the allocation
//     ceiling (max_raw_memory_mb from DecodeLimits::max_memory_bytes), and floating-point DNG
//     data is kept as float (LIBRAW_RAWOPTIONS_CONVERTFLOAT_TO_INT is cleared) rather than being
//     rescaled to integers. Nothing is post-processed: no black subtraction, no scaling.
//   * Cancellation is observed at LibRaw's progress callbacks and at every read of the file.
//   * Truncated copies are rejected as Corrupt (raw.libraw.truncated) wherever Lucent can see the
//     truncation, even when LibRaw itself reports success or calls the file unsupported:
//       - TIFF-structured files (TIFF, DNG, ORF, RW2): every IFD of the IFD 0 chain and the
//         SubIFD trees, and every strip and tile they declare, must lie inside the file;
//       - CR3: every top-level ISO base media box must end inside the file;
//       - RAF: the JPEG, CFA-header and CFA ranges of the RAF header must lie inside the file;
//       - all formats: unpacking may not keep reading at the end of the file (more than 16 reads
//         starting there).
//     A cut that only removes data none of these checks covers (for example trailing tag values
//     of a TIFF IFD, or trailing bytes of a format without a structural check) is not detected
//     when the pixel data is still complete (docs/adr/0005-raw-decoder-boundary.md).

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/core/error.hpp>
#include <lucent/raw/raw_decoder.hpp>

namespace lucent::raw {

inline constexpr std::string_view kLibRawDecoderId = "libraw";

// Creates the LibRaw adapter. Fails only with OutOfMemory.
[[nodiscard]] Expected<std::unique_ptr<RawDecoder>> create_libraw_decoder();

// Identity of the linked LibRaw build.
struct LibRawBuildInfo {
    std::string version;                    // e.g. "0.22.2-Release"
    std::uint32_t version_number = 0;       // (major << 16) | (minor << 8) | patch
    std::vector<std::string> capabilities;  // decoded LibRaw::capabilities(), e.g. "zlib", "jpeg"
};
[[nodiscard]] LibRawBuildInfo libraw_build_info();

// Maps a LibRaw return code (enum LibRaw_errors, or a positive errno value) to Lucent's error
// category:
//   LIBRAW_SUCCESS                                   -> (not an error; returns Internal)
//   LIBRAW_FILE_UNSUPPORTED, LIBRAW_NOT_IMPLEMENTED,
//   LIBRAW_UNSUPPORTED_THUMBNAIL                     -> Unsupported
//   LIBRAW_REQUEST_FOR_NONEXISTENT_IMAGE,
//   LIBRAW_NO_THUMBNAIL,
//   LIBRAW_REQUEST_FOR_NONEXISTENT_THUMBNAIL, ENOENT -> NotFound
//   LIBRAW_DATA_ERROR                                -> Corrupt
//   LIBRAW_IO_ERROR, other positive errno values     -> IoError
//   LIBRAW_UNSUFFICIENT_MEMORY                       -> OutOfMemory
//   LIBRAW_TOO_BIG, LIBRAW_MEMPOOL_OVERFLOW          -> LimitExceeded
//   LIBRAW_CANCELLED_BY_CALLBACK                     -> Cancelled
//   LIBRAW_OUT_OF_ORDER_CALL, LIBRAW_INPUT_CLOSED,
//   LIBRAW_BAD_CROP                                  -> Internal (the adapter misused LibRaw)
//   LIBRAW_UNSPECIFIED_ERROR and unknown codes       -> Unknown
// The adapter refines these with what it observed itself (its stream records every OS read
// failure): any failure after cancellation was observed becomes Cancelled; a failure caused by an
// OS read error carries that error (IoError, SourceChanged, ...); and a LIBRAW_IO_ERROR without
// an OS read error becomes Corrupt, because LibRaw reports both "ran out of data" and "rejected
// the structure" as LIBRAW_IO_ERROR. LibRaw data errors counted during a call that otherwise
// succeeded also become Corrupt (as rawpy treats them).
[[nodiscard]] ErrorCode libraw_error_code(int libraw_code) noexcept;

// LibRaw's own description of a return code (LibRaw::strerror), e.g. "Unsupported file format".
[[nodiscard]] std::string_view libraw_error_text(int libraw_code) noexcept;

}  // namespace lucent::raw
