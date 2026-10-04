#pragma once

// Private: Lucent's own strict reader for the EXIF it writes, and the verification of a written
// file's metadata against what was intended (write_verified step 5).
//
// The reader is deliberately independent of the writers (it shares only the tag numbers in
// exif_format.hpp): it walks the JPEG marker segments or the TIFF header itself, follows IFD 0's
// Exif and GPS pointers, and decodes every tag Lucent writes with its exact type and count. It is
// strict because it only ever reads Lucent's own output: anything malformed is Corrupt.
//
// Thread-safety: pure functions over their own file handles.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <lucent/core/error.hpp>
#include <lucent/export/export.hpp>
#include <lucent/export/metadata.hpp>

#include "exif_writer.hpp"

namespace lucent::exporter::detail {

enum class ExifContainer : std::uint8_t { Jpeg, Tiff };

// Everything the reader found.
struct ParsedExif {
    ExifContainer container = ExifContainer::Jpeg;
    ExportMetadata metadata;
    bool has_tiff_structure = false;  // JPEG: an APP1 "Exif" segment was found; TIFF: always
    std::int32_t exif_segments = 0;   // JPEG: number of APP1 "Exif" segments before the scan
    std::uint32_t ifd0_next = 0;      // IFD 0's next-IFD offset (0: no IFD 1 / thumbnail)
    std::optional<std::uint16_t> orientation;
    bool has_exif_ifd = false;
    bool has_gps_ifd = false;
    std::vector<std::uint16_t> ifd0_tags;  // in file order
    std::vector<std::uint16_t> exif_tags;
    std::vector<std::uint16_t> gps_tags;
    // Exif IFD structure fields.
    std::string exif_version;  // 4 characters, empty when absent
    std::optional<std::uint16_t> color_space;
    std::optional<std::uint32_t> pixel_x;
    std::optional<std::uint32_t> pixel_y;
};

// Reads the EXIF of a JPEG or classic TIFF file. Errors (export.metadata.read): file-open errors
// from core::ReadOnlyFile; Unsupported for another format; Corrupt as described above.
[[nodiscard]] Expected<ParsedExif> read_exif_file(const std::filesystem::path& path);

// First difference between `actual` and `expected`, or std::nullopt when they match. Strings,
// integers, dates and rationals (numerator and denominator) are compared exactly; GPS values,
// which are rebuilt from their fractions, within binary rounding (1e-9 degree, 1e-6 m, 1e-6 s).
[[nodiscard]] std::optional<std::string> metadata_difference(const ExportMetadata& actual,
                                                             const ExportMetadata& expected);

// Reads `path` back and checks it against `expected` (prepare_metadata's result): the values
// match (metadata_difference); the Exif IFD exists exactly when a capture field
// is expected and the GPS IFD exactly when a position is; only the documented tags occur in each
// IFD (so no maker note, no serial number); the Exif IFD records ExifVersion 0232, the ColorSpace
// of `image.profile` and the image size. JPEG: no APP1 "Exif" segment when nothing is expected,
// otherwise exactly one, with Orientation 1 and no IFD 1 (no thumbnail). TIFF: IFD 0 has no
// SubIFDs and no JPEG thumbnail pointers. Corrupt (export.<format>.verify) on any mismatch.
[[nodiscard]] Status verify_metadata(const std::filesystem::path& path, ExportFormat format,
                                     const ExportMetadata& expected, const ExifImageInfo& image);

}  // namespace lucent::exporter::detail
