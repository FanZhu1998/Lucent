#pragma once

// Capture metadata of a source photograph: the EXIF facts an export may carry forward
// (blueprint section 7, "Preserve useful capture EXIF and copyright; provide an explicit GPS
// include/remove option").
//
// read_capture_metadata() is Lucent's own bounded EXIF reader; no third-party EXIF library is
// involved (Exiv2 is GPL and not permitted, ADR 0003). It reads, per container:
//   TIFF family (TIFF, BigTIFF, DNG, CR2, NEF, ARW, PEF, ... and the ORF / RW2 header variants)
//         IFD 0 and the Exif and GPS IFDs it points to;
//   CR3   the Canon metadata boxes of moov/uuid{85c0b687-820f-11e0-8111-f4ce462b6a48}:
//         CMT1 (IFD 0), CMT2 (Exif IFD) and CMT4 (GPS IFD), each a small TIFF stream;
//   RAF   the APP1 "Exif" segment of the JPEG preview named by the RAF header;
//   JPEG  the APP1 "Exif" segment (also used for camera JPEGs and to read exports back).
// Other containers are Unsupported; complete_from_decoder() then supplies what the decoder knows.
//
// Only the fields below are read. Maker notes, thumbnails (IFD 1), serial numbers and every
// other tag are never read or retained.
//
// Validation policy: metadata is descriptive, so a malformed or implausible field is dropped
// and a warning recorded (CaptureMetadata::warnings) instead of failing the whole read. The read
// fails only when the container itself cannot be walked (see read_capture_metadata).
//
// Text: EXIF ASCII values are cut at the first NUL (Copyright: the photographer part before the
// first NUL), decoded as UTF-8 with ill-formed sequences replaced by U+FFFD
// (core::sanitize_utf8; cameras and editors commonly store UTF-8 in ASCII tags), stripped of
// control characters and trimmed of surrounding spaces. A value that is empty afterwards is
// treated as absent (Canon, for example, pads an unset Artist with spaces).
//
// Thread-safety: plain value types; the functions are safe to call concurrently on different
// sources.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <lucent/core/error.hpp>
#include <lucent/raw/decoded_raw.hpp>
#include <lucent/raw/tiff_inspector.hpp>

namespace lucent::raw {

// EXIF RATIONAL: two unsigned 32-bit integers. A valid value has denominator != 0.
struct ExifRational {
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 1;

    [[nodiscard]] double value() const noexcept {
        return static_cast<double>(numerator) / static_cast<double>(denominator);
    }

    // Closest fraction to `value` (finite, >= 0, at most 2^32 - 1) whose denominator does not
    // exceed `max_denominator` (continued fractions). Values below 1 whose reciprocal is within
    // 0.5 % of an integer become 1/n, the way cameras record exposure times. std::nullopt for a
    // negative, non-finite or too large value.
    [[nodiscard]] static std::optional<ExifRational> approximate(double value,
                                                                 std::uint32_t max_denominator = 10000);

    friend bool operator==(const ExifRational&, const ExifRational&) = default;
};

// EXIF SRATIONAL: two signed 32-bit integers. A valid value has denominator != 0.
struct ExifSignedRational {
    std::int32_t numerator = 0;
    std::int32_t denominator = 1;

    [[nodiscard]] double value() const noexcept {
        return static_cast<double>(numerator) / static_cast<double>(denominator);
    }

    friend bool operator==(const ExifSignedRational&, const ExifSignedRational&) = default;
};

// A camera wall-clock time: DateTimeOriginal (36867) "YYYY:MM:DD HH:MM:SS" with its
// SubSecTimeOriginal (37521) and OffsetTimeOriginal (36881).
struct ExifDateTime {
    std::int32_t year = 0;    // 1..9999
    std::int32_t month = 0;   // 1..12
    std::int32_t day = 0;     // 1..31 (checked against the month, leap years included)
    std::int32_t hour = 0;    // 0..23
    std::int32_t minute = 0;  // 0..59
    std::int32_t second = 0;  // 0..59
    // Fraction of the second as recorded: 1 to 9 ASCII digits ("07" means 0.07 s); empty when
    // absent.
    std::string subsec;
    // Offset of the local time from UTC in minutes (OffsetTimeOriginal "+09:00" is 540),
    // -14 h .. +14 h; std::nullopt when the file does not record it.
    std::optional<std::int32_t> utc_offset_minutes;

    friend bool operator==(const ExifDateTime&, const ExifDateTime&) = default;
};

// UTC time of a GPS fix: GPSDateStamp (29) and GPSTimeStamp (7).
struct GpsTimestamp {
    std::int32_t year = 0;   // 1..9999
    std::int32_t month = 0;  // 1..12
    std::int32_t day = 0;    // 1..31
    std::int32_t hour = 0;   // 0..23
    std::int32_t minute = 0;  // 0..59
    double second = 0.0;     // [0, 60)

    friend bool operator==(const GpsTimestamp&, const GpsTimestamp&) = default;
};

// Position recorded by the camera (GPS IFD). Coordinates are as recorded; GPSMapDatum is not
// interpreted (in practice WGS-84). A position of exactly 0,0 is what cameras without a fix write
// and is treated as absent (with a warning).
struct GpsLocation {
    double latitude_deg = 0.0;   // [-90, 90], north positive
    double longitude_deg = 0.0;  // [-180, 180], east positive
    std::optional<double> altitude_m;  // metres above sea level, negative below
    // Present only when both the date and the time stamp are valid.
    std::optional<GpsTimestamp> utc;

    friend bool operator==(const GpsLocation&, const GpsLocation&) = default;
};

// The capture facts Lucent carries into exports. Every field is optional: empty strings and
// std::nullopt mean "not recorded" (or dropped as malformed, with a warning).
struct CaptureMetadata {
    // IFD 0.
    std::string make;   // Make (271)
    std::string model;  // Model (272)
    // Exif IFD.
    std::string lens_make;   // LensMake (42035)
    std::string lens_model;  // LensModel (42036)
    std::optional<ExifRational> exposure_time_s;  // ExposureTime (33434), > 0
    std::optional<ExifRational> f_number;         // FNumber (33437), > 0
    // PhotographicSensitivity (34855); when that holds 65535 (the EXIF "65535 or more" marker),
    // RecommendedExposureIndex (34866) or ISOSpeed (34867) when present. > 0.
    std::optional<std::uint32_t> iso;
    std::optional<ExifRational> focal_length_mm;         // FocalLength (37386), > 0
    std::optional<std::uint16_t> focal_length_35mm_mm;   // FocalLengthIn35mmFilm (41989), > 0
    std::optional<ExifSignedRational> exposure_bias_ev;  // ExposureBiasValue (37380)
    std::optional<ExifDateTime> capture_time;            // DateTimeOriginal and companions
    // IFD 0 rights.
    std::string artist;     // Artist (315)
    std::string copyright;  // Copyright (33432), photographer part
    // GPS IFD; std::nullopt when absent or without a valid latitude and longitude.
    std::optional<GpsLocation> gps;

    // Developer-facing English notes about fields that were dropped or container parts that
    // could not be read, e.g. "tiff.exif tag 33434: zero denominator".
    std::vector<std::string> warnings;
};

// Bounds on one read. Conforming files stay far below them.
struct CaptureMetadataLimits {
    std::uint32_t max_entries_per_ifd = 1024;   // an IFD with more entries is skipped (warning)
    std::uint64_t max_value_bytes = 64 * 1024;  // a larger tag value is dropped (warning)
    // Largest embedded TIFF stream (CR3 CMTn box, JPEG APP1 payload) that is loaded.
    std::uint64_t max_embedded_bytes = 1024 * 1024;
    std::uint32_t max_boxes = 4096;         // ISO BMFF boxes visited in a CR3
    std::uint32_t max_jpeg_segments = 256;  // JPEG marker segments walked before giving up
};

// Reads the capture metadata of `source` (see the file comment for the containers).
//
// Errors (Error::where in parentheses):
//   Unsupported  not one of the containers above (raw.exif.container)
//   Corrupt      the container's own structure cannot be walked: a TIFF header or IFD 0 that
//                is truncated or invalid, a CR3 box or JPEG segment that overruns the source
//                (raw.exif.container, raw.tiff.*)
//   Errors returned by `source` are passed through unchanged.
// Missing metadata is not an error: a file without EXIF yields an empty CaptureMetadata.
// Never throws (std::bad_alloc becomes OutOfMemory). Reads only within [0, source.size()).
[[nodiscard]] Expected<CaptureMetadata> read_capture_metadata(RandomAccessSource& source,
                                                              const CaptureMetadataLimits& limits = {});

// Opens `path` read-only (core::ReadOnlyFile, sharing other readers) and reads it as above.
// Additional errors: those of core::ReadOnlyFile::open; SourceChanged when the file shrinks
// while being read.
[[nodiscard]] Expected<CaptureMetadata> read_capture_metadata(const std::filesystem::path& path,
                                                              const CaptureMetadataLimits& limits = {});

// Fills fields that `metadata` lacks from what a decoder reported (RawMetadata, for example a
// LibRaw probe): make and model, lens make and model, exposure time, f-number, ISO, focal
// length (35 mm equivalent included) and the capture time (without a UTC offset, which decoders
// do not report). Decoder floats are converted with ExifRational::approximate. Present fields
// are never replaced, so values read from the file's own EXIF always win. Artist, copyright and
// GPS are not part of RawMetadata and stay as they are.
void complete_from_decoder(CaptureMetadata& metadata, const RawMetadata& decoded);

}  // namespace lucent::raw
