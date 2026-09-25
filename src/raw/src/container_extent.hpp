#pragma once

// Private: structural completeness check of a RAW container, used by the LibRaw adapter to
// reject truncated copies (Corrupt at raw.libraw.truncated) and to tell a damaged file of a
// recognised RAW container from a file LibRaw does not support.
//
// Only declared extents are compared with the source size; nothing is decoded. What is checked
// per container:
//   Tiff, Dng, Orf, Rw2  the IFD 0 chain, SubIFD trees and their strips and tiles
//                        (find_image_data_overruns with TiffSignatures::WithVendorRaw)
//   Cr3                  the top-level ISO base media file boxes (ftyp, moov, uuid, mdat, ...):
//                        each must end inside the source; a box of size 0 extends to its end
//   Raf                  the JPEG preview, CFA header and CFA data ranges of the RAF header
// Every other container has no structural check (Unsupported).

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

}  // namespace lucent::raw::detail
