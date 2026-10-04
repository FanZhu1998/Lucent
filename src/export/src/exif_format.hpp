#pragma once

// Private: the EXIF tags Lucent writes, shared by the JPEG APP1 builder (exif_writer.cpp), the
// libtiff writer (tiff_codec.cpp) and the read-back parser (exif_reader.cpp), plus the fixed-
// point forms in which numbers are written.

#include <array>
#include <cmath>
#include <cstdint>
#include <span>

#include <lucent/export/metadata.hpp>
#include <lucent/export/output_profile.hpp>

namespace lucent::exporter::detail {

namespace exif_tag {
// IFD 0.
inline constexpr std::uint16_t kMake = 271;
inline constexpr std::uint16_t kModel = 272;
inline constexpr std::uint16_t kOrientation = 274;
inline constexpr std::uint16_t kXResolution = 282;
inline constexpr std::uint16_t kYResolution = 283;
inline constexpr std::uint16_t kResolutionUnit = 296;
inline constexpr std::uint16_t kSoftware = 305;
inline constexpr std::uint16_t kArtist = 315;
inline constexpr std::uint16_t kSubIfds = 330;
inline constexpr std::uint16_t kJpegInterchangeFormat = 513;
inline constexpr std::uint16_t kJpegInterchangeFormatLength = 514;
inline constexpr std::uint16_t kYCbCrPositioning = 531;
inline constexpr std::uint16_t kCopyright = 33432;
inline constexpr std::uint16_t kExifIfd = 34665;
inline constexpr std::uint16_t kGpsIfd = 34853;
// Exif IFD.
inline constexpr std::uint16_t kExposureTime = 33434;
inline constexpr std::uint16_t kFNumber = 33437;
inline constexpr std::uint16_t kPhotographicSensitivity = 34855;
inline constexpr std::uint16_t kSensitivityType = 34864;
inline constexpr std::uint16_t kIsoSpeed = 34867;
inline constexpr std::uint16_t kExifVersion = 36864;
inline constexpr std::uint16_t kDateTimeOriginal = 36867;
inline constexpr std::uint16_t kOffsetTimeOriginal = 36881;
inline constexpr std::uint16_t kComponentsConfiguration = 37121;
inline constexpr std::uint16_t kExposureBias = 37380;
inline constexpr std::uint16_t kFocalLength = 37386;
inline constexpr std::uint16_t kMakerNote = 37500;
inline constexpr std::uint16_t kSubSecTimeOriginal = 37521;
inline constexpr std::uint16_t kFlashpixVersion = 40960;
inline constexpr std::uint16_t kColorSpace = 40961;
inline constexpr std::uint16_t kPixelXDimension = 40962;
inline constexpr std::uint16_t kPixelYDimension = 40963;
inline constexpr std::uint16_t kFocalLength35mm = 41989;
inline constexpr std::uint16_t kLensMake = 42035;
inline constexpr std::uint16_t kLensModel = 42036;
// GPS IFD.
inline constexpr std::uint16_t kGpsVersionId = 0;
inline constexpr std::uint16_t kGpsLatitudeRef = 1;
inline constexpr std::uint16_t kGpsLatitude = 2;
inline constexpr std::uint16_t kGpsLongitudeRef = 3;
inline constexpr std::uint16_t kGpsLongitude = 4;
inline constexpr std::uint16_t kGpsAltitudeRef = 5;
inline constexpr std::uint16_t kGpsAltitude = 6;
inline constexpr std::uint16_t kGpsTimeStamp = 7;
inline constexpr std::uint16_t kGpsDateStamp = 29;
}  // namespace exif_tag

namespace exif_type {
inline constexpr std::uint16_t kByte = 1;
inline constexpr std::uint16_t kAscii = 2;
inline constexpr std::uint16_t kShort = 3;
inline constexpr std::uint16_t kLong = 4;
inline constexpr std::uint16_t kRational = 5;
inline constexpr std::uint16_t kUndefined = 7;
inline constexpr std::uint16_t kSRational = 10;
inline constexpr std::uint16_t kIfd = 13;
}  // namespace exif_type

// Tags Lucent may write in each IFD of a JPEG's EXIF (the Exif and GPS lists also apply to TIFF;
// a TIFF's IFD 0 additionally holds libtiff's image-structure tags).
inline constexpr std::array<std::uint16_t, 12> kJpegIfd0Tags = {
    exif_tag::kMake,       exif_tag::kModel,    exif_tag::kOrientation,       exif_tag::kXResolution,
    exif_tag::kYResolution, exif_tag::kResolutionUnit, exif_tag::kSoftware, exif_tag::kArtist,
    exif_tag::kYCbCrPositioning, exif_tag::kCopyright, exif_tag::kExifIfd, exif_tag::kGpsIfd};
inline constexpr std::array<std::uint16_t, 19> kExifIfdTags = {
    exif_tag::kExposureTime,    exif_tag::kFNumber,          exif_tag::kPhotographicSensitivity,
    exif_tag::kSensitivityType, exif_tag::kIsoSpeed,         exif_tag::kExifVersion,
    exif_tag::kDateTimeOriginal, exif_tag::kOffsetTimeOriginal, exif_tag::kComponentsConfiguration,
    exif_tag::kExposureBias,    exif_tag::kFocalLength,      exif_tag::kSubSecTimeOriginal,
    exif_tag::kFlashpixVersion, exif_tag::kColorSpace,       exif_tag::kPixelXDimension,
    exif_tag::kPixelYDimension, exif_tag::kFocalLength35mm,  exif_tag::kLensMake,
    exif_tag::kLensModel};
inline constexpr std::array<std::uint16_t, 9> kGpsIfdTags = {
    exif_tag::kGpsVersionId, exif_tag::kGpsLatitudeRef, exif_tag::kGpsLatitude,
    exif_tag::kGpsLongitudeRef, exif_tag::kGpsLongitude, exif_tag::kGpsAltitudeRef,
    exif_tag::kGpsAltitude,  exif_tag::kGpsTimeStamp,   exif_tag::kGpsDateStamp};

inline constexpr char kExifVersion[4] = {'0', '2', '3', '2'};
inline constexpr char kFlashpixVersion[4] = {'0', '1', '0', '0'};
inline constexpr std::uint8_t kComponentsConfiguration[4] = {1, 2, 3, 0};  // Y, Cb, Cr
inline constexpr std::uint8_t kGpsVersion[4] = {2, 3, 0, 0};
inline constexpr std::uint32_t kExifResolutionDpi = 300;
inline constexpr std::uint16_t kResolutionUnitInch = 2;
inline constexpr std::uint16_t kYCbCrCentered = 1;
inline constexpr std::uint16_t kColorSpaceSrgb = 1;
inline constexpr std::uint16_t kColorSpaceUncalibrated = 0xFFFF;
inline constexpr std::uint16_t kSensitivityTypeIsoSpeed = 3;

// Denominators of the fixed-point forms below.
inline constexpr std::uint32_t kGpsSecondsDenominator = 1000000;  // micro-arcseconds
inline constexpr std::uint32_t kAltitudeDenominator = 1000;        // millimetres
inline constexpr std::uint32_t kTimeSecondsDenominator = 1000;     // milliseconds

// EXIF ColorSpace for an output profile: 1 (sRGB) or 0xFFFF (uncalibrated).
[[nodiscard]] inline std::uint16_t exif_color_space(OutputProfile profile) noexcept {
    return profile == OutputProfile::Srgb ? kColorSpaceSrgb : kColorSpaceUncalibrated;
}

// |degrees| as degrees/1, minutes/1, micro-arcseconds/1000000. Precondition: finite, |degrees|
// <= 180 and already rounded to a micro-arcsecond (prepare_metadata).
struct DmsRationals {
    std::uint32_t degrees = 0;
    std::uint32_t minutes = 0;
    std::uint32_t micro_seconds = 0;  // < 60 000 000
};
[[nodiscard]] inline DmsRationals to_dms(double degrees) noexcept {
    const auto total = static_cast<std::uint64_t>(std::llround(std::abs(degrees) * 3600.0 * kGpsSecondsDenominator));
    constexpr std::uint64_t kPerDegree = 3600ull * kGpsSecondsDenominator;
    constexpr std::uint64_t kPerMinute = 60ull * kGpsSecondsDenominator;
    return DmsRationals{static_cast<std::uint32_t>(total / kPerDegree),
                        static_cast<std::uint32_t>((total % kPerDegree) / kPerMinute),
                        static_cast<std::uint32_t>(total % kPerMinute)};
}

// Altitude in millimetres and a GPS second in milliseconds. prepare_metadata has already made
// both whole numbers of those units; rounding only absorbs binary floating-point error.
[[nodiscard]] inline std::uint32_t altitude_millimetres(double metres) noexcept {
    return static_cast<std::uint32_t>(std::llround(std::abs(metres) * kAltitudeDenominator));
}
[[nodiscard]] inline std::uint32_t second_milliseconds(double second) noexcept {
    return static_cast<std::uint32_t>(std::llround(second * kTimeSecondsDenominator));
}

// The exact fractions written for GPS values (both writers use these).
[[nodiscard]] inline std::array<ExportRational, 3> coordinate_rationals(double degrees) noexcept {
    const DmsRationals dms = to_dms(degrees);
    return {ExportRational{dms.degrees, 1}, ExportRational{dms.minutes, 1},
            ExportRational{dms.micro_seconds, kGpsSecondsDenominator}};
}
[[nodiscard]] inline ExportRational altitude_rational(double metres) noexcept {
    return ExportRational{altitude_millimetres(metres), kAltitudeDenominator};
}
[[nodiscard]] inline std::array<ExportRational, 3> time_rationals(const GpsTimestamp& t) noexcept {
    return {ExportRational{static_cast<std::uint32_t>(t.hour), 1}, ExportRational{static_cast<std::uint32_t>(t.minute), 1},
            ExportRational{second_milliseconds(t.second), kTimeSecondsDenominator}};
}

}  // namespace lucent::exporter::detail
