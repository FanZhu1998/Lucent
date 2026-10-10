#pragma once

// Private: structural completeness check of a RAW container, used by the LibRaw adapter to
// reject truncated copies (Corrupt at raw.libraw.truncated) and to tell a damaged file of a
// recognised RAW container from a file LibRaw does not support.
//
// Only declared extents are compared with the source size; nothing is decoded. What is checked
// per container:
//   Tiff, Dng, Orf, Rw2  the IFD 0 chain, SubIFD trees and their strips and tiles
//                        (find_image_data_overruns with TiffSignatures::WithVendorRaw); for Rw2
//                        (Panasonic RW2, Leica RWL) IFD 0's raw data at RawDataOffset instead of
//                        its strip table (RawDataRule::Panasonic)
//   Cr3                  the top-level ISO base media file boxes (ftyp, moov, uuid, mdat, ...):
//                        each must end inside the source; a box of size 0 extends to its end
//   Raf                  the JPEG preview, CFA header and CFA data ranges of the RAF header
// Every other container has no structural check (Unsupported).

#include <cstdint>
#include <optional>

#include <lucent/core/error.hpp>
#include <lucent/raw/decoded_raw.hpp>
#include <lucent/raw/tiff_inspector.hpp>

#include "tiff_extent.hpp"

namespace lucent::raw::detail {

// True for the containers find_container_overruns() can check.
[[nodiscard]] bool has_structural_check(ContainerFormat container) noexcept;

// Checks the structure of `source`, a file recognised as `container`, against its size.
// Overruns are reported in the result (never as an error). Errors: Unsupported for a container
// without a check (or data that does not carry the container's signature), Corrupt for a
// structure that is malformed where it lies inside the source (for example a box whose size is
// smaller than its header), and source read failures unchanged.
[[nodiscard]] Expected<ImageDataExtent> find_container_overruns(RandomAccessSource& source,
                                                                ContainerFormat container,
                                                                const TiffInspectLimits& limits = {});

// The crop a Fujifilm RAF declares in the metadata directory its header points to (the "CFA
// header" range, byte 92): RawImageCropTopLeft (tag 0x110: top, left) and RawImageCroppedSize
// (0x111: height, width), big-endian 16-bit values, as ExifTool's FujiFilm RAF table names them
// and LibRaw's parse_fuji() reads them (src/metadata/fuji.cpp). std::nullopt when the file is not
// a RAF, the directory is absent or malformed, or either tag is missing (the directory is read up
// to 512 entries and never beyond the source). Errors: source read failures only.
struct RafCrop {
    std::uint32_t top = 0;
    std::uint32_t left = 0;
    std::uint32_t height = 0;
    std::uint32_t width = 0;
};
[[nodiscard]] Expected<std::optional<RafCrop>> read_raf_crop(RandomAccessSource& source);

}  // namespace lucent::raw::detail
