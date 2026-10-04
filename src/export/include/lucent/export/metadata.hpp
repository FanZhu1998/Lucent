#pragma once

// Metadata written into exported files (blueprint section 7: "Preserve useful capture EXIF and
// copyright; provide an explicit GPS include/remove option. Do not copy embedded camera
// thumbnails as if they represented the edited result").
//
// Namespace lucent::exporter: see output_profile.hpp.
//
// Model
//   ExportMetadata    the facts about one photograph that an export may carry (Lucent value
//                     types; the application fills it from raw::CaptureMetadata and the user's
//                     rights settings).
//   MetadataPolicy    which groups of facts are written (part of ExportSettings, so it is
//                     recorded in every receipt).
//   prepare_metadata  applies the policy, sanitises text and normalises numbers. Its result is
//                     exactly what write_verified() writes and then verifies, and what the
//                     receipt records.
//
// What is written (EXIF 2.32 tag numbering; Lucent writes its own minimal TIFF/EXIF structure for
// JPEG and uses libtiff's custom-directory support for TIFF):
//   IFD 0     Make, Model, Artist, Copyright, Software; JPEG also Orientation = 1 (the pixels
//             are always upright), X/YResolution 300 dpi, ResolutionUnit and YCbCrPositioning = 1.
//             IFD 0 is the only main-chain IFD: there is never an IFD 1, so no thumbnail.
//   Exif IFD  (only when a capture field is written) ExifVersion "0232", ColorSpace (1 for sRGB,
//             0xFFFF "uncalibrated" otherwise; the ICC profile is authoritative),
//             PixelX/YDimension, ExposureTime, FNumber, PhotographicSensitivity (65535 plus
//             SensitivityType 3 and ISOSpeed above 65535), DateTimeOriginal,
//             SubSecTimeOriginal, OffsetTimeOriginal, ExposureBiasValue, FocalLength,
//             FocalLengthIn35mmFilm, LensMake, LensModel; JPEG also ComponentsConfiguration
//             and FlashpixVersion.
//   GPS IFD   (only when a position is written) GPSVersionID 2.3.0.0, latitude and longitude
//             with their references, altitude with its reference, time and date stamp.
// Never written: maker notes, thumbnails, serial numbers, the file-change DateTime (export time
// would make identical exports differ) and any tag not listed above.
//
// Text
//   Make, Model, LensMake, LensModel and Software are EXIF ASCII: after UTF-8 sanitising,
//   every control character becomes a space, every non-ASCII character becomes '?', the value is
//   trimmed and limited to kMaxMetadataAsciiBytes.
//   Artist and Copyright keep Unicode: they are well-formed UTF-8 (ill-formed input becomes
//   U+FFFD), control characters become spaces, the value is trimmed and limited to
//   kMaxMetadataTextBytes (cut at a character boundary). EXIF 2.32 has no Unicode type for these
//   tags; the UTF-8 bytes are stored in the ASCII-typed tags, the de facto convention of ExifTool
//   and Adobe applications. A strict 7-bit reader shows non-ASCII characters as garbage.
//   Every change prepare_metadata makes to a value (replacement, truncation, a dropped invalid
//   field) is reported in PreparedMetadata::notes; nothing is altered silently.
//
// Thread-safety: plain value types; all functions are safe to call concurrently.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <lucent/core/error.hpp>

namespace lucent::exporter {

// Which groups of metadata an export writes. Persisted with the export settings.
struct MetadataPolicy {
    // Camera make and model, lens, exposure time, f-number, ISO, exposure bias, focal length
    // (and 35 mm equivalent), capture time with sub-seconds and UTC offset.
    bool include_capture = true;
    // Artist and Copyright.
    bool include_copyright = true;
    // GPS position. Off by default: location is personal data and is written only on an
    // explicit choice (blueprint section 7).
    bool include_gps = false;
    // Software = software_name() ("Lucent <version>").
    bool include_software = true;

    friend bool operator==(const MetadataPolicy&, const MetadataPolicy&) = default;
};

// EXIF RATIONAL. Valid when denominator != 0.
struct ExportRational {
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 1;

    [[nodiscard]] double value() const noexcept {
        return static_cast<double>(numerator) / static_cast<double>(denominator);
    }
    friend bool operator==(const ExportRational&, const ExportRational&) = default;
};

// EXIF SRATIONAL. Valid when denominator != 0.
struct ExportSignedRational {
    std::int32_t numerator = 0;
    std::int32_t denominator = 1;

    [[nodiscard]] double value() const noexcept {
        return static_cast<double>(numerator) / static_cast<double>(denominator);
    }
    friend bool operator==(const ExportSignedRational&, const ExportSignedRational&) = default;
};

// Camera wall-clock capture time (DateTimeOriginal, SubSecTimeOriginal, OffsetTimeOriginal).
struct CaptureDateTime {
    std::int32_t year = 0;    // 1..9999
    std::int32_t month = 0;   // 1..12
    std::int32_t day = 0;     // 1..31, valid for the month
    std::int32_t hour = 0;    // 0..23
    std::int32_t minute = 0;  // 0..59
    std::int32_t second = 0;  // 0..59
    std::string subsec;       // 0 to 9 ASCII digits ("07" = 0.07 s)
    std::optional<std::int32_t> utc_offset_minutes;  // -840..840 (UTC-14:00..UTC+14:00)

    friend bool operator==(const CaptureDateTime&, const CaptureDateTime&) = default;
};

// UTC time of a GPS fix.
struct GpsTimestamp {
    std::int32_t year = 0;    // 1..9999
    std::int32_t month = 0;   // 1..12
    std::int32_t day = 0;     // valid for the month
    std::int32_t hour = 0;    // 0..23
    std::int32_t minute = 0;  // 0..59
    double second = 0.0;      // [0, 60); written with millisecond resolution

    friend bool operator==(const GpsTimestamp&, const GpsTimestamp&) = default;
};

// A GPS position. Resolution as written: 1 micro-arcsecond for coordinates (about 0.03 mm),
// 1 mm for the altitude.
struct GpsPosition {
    double latitude_deg = 0.0;   // [-90, 90], north positive
    double longitude_deg = 0.0;  // [-180, 180], east positive
    std::optional<double> altitude_m;  // |altitude| <= kMaxGpsAltitudeMetres; negative below sea level
    std::optional<GpsTimestamp> utc;

    friend bool operator==(const GpsPosition&, const GpsPosition&) = default;
};

// Facts about one photograph for its export. Empty strings and std::nullopt are "absent".
struct ExportMetadata {
    // ---- Capture (MetadataPolicy::include_capture) ------------------------------------------
    std::string camera_make;
    std::string camera_model;
    std::string lens_make;
    std::string lens_model;
    std::optional<ExportRational> exposure_time_s;       // > 0
    std::optional<ExportRational> f_number;              // > 0
    std::optional<std::uint32_t> iso;                    // > 0
    std::optional<ExportSignedRational> exposure_bias_ev;
    std::optional<ExportRational> focal_length_mm;       // > 0
    std::optional<std::uint16_t> focal_length_35mm_mm;   // > 0
    std::optional<CaptureDateTime> capture_time;

    // ---- Rights (MetadataPolicy::include_copyright) -----------------------------------------
    std::string artist;
    std::string copyright;

    // ---- Location (MetadataPolicy::include_gps) ---------------------------------------------
    std::optional<GpsPosition> gps;

    // ---- Writer (MetadataPolicy::include_software) ------------------------------------------
    // Set by prepare_metadata() (software_name() or empty); any input value is ignored.
    std::string software;

    // True when nothing would be written.
    [[nodiscard]] bool empty() const noexcept;
    // True when any capture field is present (the Exif IFD is written).
    [[nodiscard]] bool has_capture() const noexcept;

    friend bool operator==(const ExportMetadata&, const ExportMetadata&) = default;
};

inline constexpr std::size_t kMaxMetadataAsciiBytes = 255;  // Make, Model, lens, Software
inline constexpr std::size_t kMaxMetadataTextBytes = 2000;  // Artist, Copyright (UTF-8 bytes)
inline constexpr double kMaxGpsAltitudeMetres = 1.0e6;

// "Lucent <version>" (core::build_info().version).
[[nodiscard]] std::string software_name();

struct PreparedMetadata {
    ExportMetadata metadata;         // what will be written
    std::vector<std::string> notes;  // developer-facing English, one per change (see Text)
};

// Applies `policy` to `source` (excluded groups are cleared; software is set or cleared), then
// sanitises text (see the file comment) and checks numbers. Invalid values are dropped with a
// note rather than failing the export: a zero denominator or zero value where > 0 is required,
// an impossible date or time (a capture time with valid date and time but invalid sub-seconds
// or offset keeps the date and time), an out-of-range UTC offset, a GPS position outside the
// ranges above or not finite (the whole position is dropped), an invalid altitude or GPS time
// (only that part is dropped). Coordinates are rounded to 1 micro-arcsecond, the altitude to
// 1 mm and the GPS second down to 1 ms, so the result is exactly what the file will hold.
// Deterministic; never fails (std::bad_alloc aside).
[[nodiscard]] PreparedMetadata prepare_metadata(const ExportMetadata& source, const MetadataPolicy& policy);

// Reads back the metadata of a JPEG or TIFF written by Lucent with Lucent's own EXIF parser
// (no third-party EXIF code). For reconciliation (W7) and tests.
//
// Errors (export.metadata.read): NotFound and other core::ReadOnlyFile::open failures;
// Unsupported when the file is neither JPEG nor classic TIFF (BigTIFF is not written by Lucent);
// Corrupt when the EXIF structure is malformed, a known tag has an unexpected type or count, or
// a value is out of range. A file without EXIF yields an empty ExportMetadata.
[[nodiscard]] Expected<ExportMetadata> read_export_metadata(const std::filesystem::path& path);

}  // namespace lucent::exporter
