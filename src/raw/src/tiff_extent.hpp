#pragma once

// Private: checks that the image data a TIFF-structured file declares lies inside the file.
//
// LibRaw tolerates missing image data for several formats (tiled DNG, Sony ARW, Canon sRAW, OM
// System ORF, ...): reads past the end of the file return nothing and the decoder fills the rest
// of the image with whatever its buffers hold, reporting success. A truncated copy of a
// photograph must not become a silently damaged image, so the LibRaw adapter compares the strip
// and tile ranges declared by every IFD (IFD 0 chain and SubIFD trees) with the file size, for
// TIFF, DNG and the TIFF-structured vendor containers ORF and RW2.

#include <cstdint>
#include <string>
#include <vector>

#include <lucent/core/error.hpp>
#include <lucent/raw/tiff_inspector.hpp>

#include "tiff_reader.hpp"

namespace lucent::raw::detail {

// One IFD whose image data (or whose strip/tile tables, or the IFD itself) extend beyond the end
// of the source.
struct ImageDataOverrun {
    std::string ifd_path;            // "ifd0", "ifd0.subifd1", ...
    std::uint64_t declared_end = 0;  // first byte after the furthest declared strip, tile or table
    std::string detail;              // developer-facing English
};

struct ImageDataExtent {
    // Every IFD whose declared data reaches past the end of the source; empty when all image
    // data that could be checked is inside.
    std::vector<ImageDataOverrun> overruns;
    // Parts of the structure that could not be checked, and irregularities that did not prevent
    // the rest of the check (developer-facing English), e.g. a malformed SubIFD inside the file.
    std::vector<std::string> warnings;
};

// Scans the IFD 0 chain and the SubIFD trees of `source` (Exif IFDs carry no image data) for
// StripOffsets/StripByteCounts and TileOffsets/TileByteCounts that reach past the end of the
// source. `accepted` selects whether ORF/RW2 signatures are read (TiffSignatures).
//
// An IFD that cannot be read counts as an overrun when it, or its plausibly sized entry table,
// does not fit in the source (the usual shape of a truncated copy); this includes IFD 0. An IFD
// after IFD 0 that lies inside the source but is malformed is skipped with a warning (for a
// main-chain IFD the chain is not followed further): LibRaw accepted the file, and such an IFD is
// a writer defect rather than missing data. Likewise a strip/tile table or a SubIFDs list that
// cannot be interpreted is skipped with a warning. Overruns found before or after such a problem
// are always kept.
//
// Returned errors: the header errors of TiffReader::open (Unsupported for other signatures,
// Corrupt), Corrupt for an IFD 0 that lies inside the source but is malformed, and source read
// failures unchanged. The walk is bounded by `limits`: at most max_ifds IFDs are visited (a
// SubIFDs list longer than that is cut to its first max_ifds entries with a warning), SubIFDs
// deeper than max_ifd_depth and values above max_value_bytes are skipped with a warning, and each
// IFD is scanned once, so loops terminate.
[[nodiscard]] Expected<ImageDataExtent> find_image_data_overruns(
    RandomAccessSource& source, const TiffInspectLimits& limits = {},
    TiffSignatures accepted = TiffSignatures::Standard);

}  // namespace lucent::raw::detail
