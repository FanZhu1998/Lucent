#pragma once

// Private: the minimal TIFF/EXIF structure Lucent writes into a JPEG APP1 segment.
//
// Layout (little-endian, offsets relative to the "II" header as EXIF requires):
//   "II" 42 8 | IFD 0 + its values | Exif IFD + values | GPS IFD + values
// Entries are sorted by tag, values are word-aligned, IFD 0's next-IFD offset is 0 (no IFD 1, so
// no thumbnail). The tags and their forms are listed in lucent/export/metadata.hpp.

#include <cstdint>
#include <vector>

#include <lucent/core/error.hpp>
#include <lucent/export/metadata.hpp>
#include <lucent/export/output_profile.hpp>

namespace lucent::exporter::detail {

// Largest APP1 payload: a marker segment length is 16 bits and includes its own two bytes.
inline constexpr std::size_t kMaxApp1Payload = 65533;

// Image facts recorded alongside the metadata.
struct ExifImageInfo {
    std::int32_t width = 0;
    std::int32_t height = 0;
    OutputProfile profile = OutputProfile::Srgb;
};

// The complete APP1 payload ("Exif\0\0" + TIFF structure) for `prepared` (the output of
// prepare_metadata); empty when prepared.empty(). Errors (export.metadata.write): LimitExceeded
// when the payload would exceed kMaxApp1Payload (unreachable within the documented text limits);
// Internal when `prepared` violates prepare_metadata's guarantees.
[[nodiscard]] Expected<std::vector<std::uint8_t>> build_exif_app1(const ExportMetadata& prepared,
                                                                  const ExifImageInfo& image);

}  // namespace lucent::exporter::detail
