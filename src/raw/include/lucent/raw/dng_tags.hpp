#pragma once

// Numeric identifiers for the TIFF field types and the TIFF 6.0, TIFF/EP, Exif and DNG tags that
// Lucent's DNG inspection uses. Every value is fixed by its specification; DNG tag numbers were
// checked against the DNG 1.7.1.0 specification (September 2023).

#include <cstdint>
#include <string_view>

namespace lucent::raw {

// TIFF field types: TIFF 6.0 section 2 (1-12), the TIFF Technical Note 1 IFD type (13) and
// BigTIFF (16-18). BigTIFF types are valid only in BigTIFF files.
enum class TiffFieldType : std::uint16_t {
    Byte = 1,
    Ascii = 2,
    Short = 3,
    Long = 4,
    Rational = 5,
    SByte = 6,
    Undefined = 7,
    SShort = 8,
    SLong = 9,
    SRational = 10,
    Float = 11,
    Double = 12,
    Ifd = 13,
    Long8 = 16,
    SLong8 = 17,
    Ifd8 = 18,
};

// Bytes per value of field type `type`, or 0 when `type` is not a TIFF/BigTIFF field type.
// BigTIFF-only types report their size here; whether they are legal depends on the file variant.
[[nodiscard]] constexpr std::uint32_t tiff_field_type_size(std::uint16_t type) noexcept {
    switch (type) {
        case 1:  // BYTE
        case 2:  // ASCII
        case 6:  // SBYTE
        case 7:  // UNDEFINED
            return 1;
        case 3:  // SHORT
        case 8:  // SSHORT
            return 2;
        case 4:   // LONG
        case 9:   // SLONG
        case 11:  // FLOAT
        case 13:  // IFD
            return 4;
        case 5:   // RATIONAL
        case 10:  // SRATIONAL
        case 12:  // DOUBLE
        case 16:  // LONG8
        case 17:  // SLONG8
        case 18:  // IFD8
            return 8;
        default:
            return 0;
    }
}

// Specification name of a field type ("SRATIONAL"), or "unknown".
[[nodiscard]] std::string_view tiff_field_type_name(std::uint16_t type) noexcept;

// Tag numbers.
namespace tiff_tag {
// TIFF 6.0 baseline and extensions.
inline constexpr std::uint16_t kNewSubFileType = 254;
inline constexpr std::uint16_t kImageWidth = 256;
inline constexpr std::uint16_t kImageLength = 257;
inline constexpr std::uint16_t kBitsPerSample = 258;
inline constexpr std::uint16_t kCompression = 259;
inline constexpr std::uint16_t kPhotometricInterpretation = 262;
inline constexpr std::uint16_t kMake = 271;
inline constexpr std::uint16_t kModel = 272;
inline constexpr std::uint16_t kStripOffsets = 273;
inline constexpr std::uint16_t kOrientation = 274;
inline constexpr std::uint16_t kSamplesPerPixel = 277;
inline constexpr std::uint16_t kRowsPerStrip = 278;
inline constexpr std::uint16_t kStripByteCounts = 279;
inline constexpr std::uint16_t kPlanarConfiguration = 284;
inline constexpr std::uint16_t kPredictor = 317;
inline constexpr std::uint16_t kTileWidth = 322;
inline constexpr std::uint16_t kTileLength = 323;
inline constexpr std::uint16_t kTileOffsets = 324;
inline constexpr std::uint16_t kTileByteCounts = 325;
inline constexpr std::uint16_t kSubIfds = 330;
inline constexpr std::uint16_t kSampleFormat = 339;

// TIFF/EP.
inline constexpr std::uint16_t kCfaRepeatPatternDim = 33421;
inline constexpr std::uint16_t kCfaPattern = 33422;

// Exif IFD pointer.
inline constexpr std::uint16_t kExifIfd = 34665;

// DNG (DNG 1.7.1.0 chapter 4 "Tags").
inline constexpr std::uint16_t kDngVersion = 50706;
inline constexpr std::uint16_t kDngBackwardVersion = 50707;
inline constexpr std::uint16_t kUniqueCameraModel = 50708;
inline constexpr std::uint16_t kLocalizedCameraModel = 50709;
inline constexpr std::uint16_t kCfaPlaneColor = 50710;
inline constexpr std::uint16_t kCfaLayout = 50711;
inline constexpr std::uint16_t kLinearizationTable = 50712;
inline constexpr std::uint16_t kBlackLevelRepeatDim = 50713;
inline constexpr std::uint16_t kBlackLevel = 50714;
inline constexpr std::uint16_t kBlackLevelDeltaH = 50715;
inline constexpr std::uint16_t kBlackLevelDeltaV = 50716;
inline constexpr std::uint16_t kWhiteLevel = 50717;
inline constexpr std::uint16_t kDefaultScale = 50718;
inline constexpr std::uint16_t kDefaultCropOrigin = 50719;
inline constexpr std::uint16_t kDefaultCropSize = 50720;
inline constexpr std::uint16_t kColorMatrix1 = 50721;
inline constexpr std::uint16_t kColorMatrix2 = 50722;
inline constexpr std::uint16_t kCameraCalibration1 = 50723;
inline constexpr std::uint16_t kCameraCalibration2 = 50724;
inline constexpr std::uint16_t kReductionMatrix1 = 50725;
inline constexpr std::uint16_t kReductionMatrix2 = 50726;
inline constexpr std::uint16_t kAnalogBalance = 50727;
inline constexpr std::uint16_t kAsShotNeutral = 50728;
inline constexpr std::uint16_t kAsShotWhiteXY = 50729;
inline constexpr std::uint16_t kBaselineExposure = 50730;
inline constexpr std::uint16_t kCalibrationIlluminant1 = 50778;
inline constexpr std::uint16_t kCalibrationIlluminant2 = 50779;
inline constexpr std::uint16_t kActiveArea = 50829;
inline constexpr std::uint16_t kMaskedAreas = 50830;
inline constexpr std::uint16_t kForwardMatrix1 = 50964;
inline constexpr std::uint16_t kForwardMatrix2 = 50965;
inline constexpr std::uint16_t kOpcodeList1 = 51008;
inline constexpr std::uint16_t kOpcodeList2 = 51009;
inline constexpr std::uint16_t kOpcodeList3 = 51022;
inline constexpr std::uint16_t kNoiseProfile = 51041;
inline constexpr std::uint16_t kDefaultUserCrop = 51125;         // DNG 1.4, raw IFD
inline constexpr std::uint16_t kProfileGainTableMap = 52525;     // DNG 1.6, raw IFD
inline constexpr std::uint16_t kCalibrationIlluminant3 = 52529;  // DNG 1.6
inline constexpr std::uint16_t kCameraCalibration3 = 52530;      // DNG 1.6
inline constexpr std::uint16_t kColorMatrix3 = 52531;            // DNG 1.6
inline constexpr std::uint16_t kForwardMatrix3 = 52532;          // DNG 1.6
inline constexpr std::uint16_t kIlluminantData1 = 52533;         // DNG 1.6
inline constexpr std::uint16_t kIlluminantData2 = 52534;         // DNG 1.6
inline constexpr std::uint16_t kIlluminantData3 = 52535;         // DNG 1.6
inline constexpr std::uint16_t kProfileGainTableMap2 = 52544;    // DNG 1.7, IFD 0 or profile
inline constexpr std::uint16_t kJxlDistance = 52553;             // DNG 1.7
inline constexpr std::uint16_t kJxlEffort = 52554;               // DNG 1.7
inline constexpr std::uint16_t kJxlDecodeSpeed = 52555;          // DNG 1.7
}  // namespace tiff_tag

// Specification name of a tag listed in tiff_tag ("ColorMatrix1"), or "unknown".
[[nodiscard]] std::string_view tiff_tag_name(std::uint16_t tag) noexcept;

// Compression tag (259) values. DNG 1.7.1.0 chapter 3 "Compression" permits 1, 7, 8 (DNG 1.4),
// 34892 (DNG 1.4) and 52546 (DNG 1.7); LZW and PackBits are TIFF 6.0 codes listed for diagnostics.
namespace tiff_compression {
inline constexpr std::uint16_t kUncompressed = 1;
inline constexpr std::uint16_t kLzw = 5;
inline constexpr std::uint16_t kJpeg = 7;
inline constexpr std::uint16_t kDeflate = 8;
inline constexpr std::uint16_t kPackBits = 32773;
inline constexpr std::uint16_t kLossyJpeg = 34892;
inline constexpr std::uint16_t kJpegXl = 52546;
}  // namespace tiff_compression

// PhotometricInterpretation tag (262) values used by DNG (DNG 1.7.1.0 chapter 3).
namespace tiff_photometric {
inline constexpr std::uint16_t kWhiteIsZero = 0;
inline constexpr std::uint16_t kBlackIsZero = 1;
inline constexpr std::uint16_t kRgb = 2;
inline constexpr std::uint16_t kTransparencyMask = 4;
inline constexpr std::uint16_t kYCbCr = 6;
inline constexpr std::uint16_t kCfa = 32803;        // TIFF/EP colour filter array; raw IFD
inline constexpr std::uint16_t kLinearRaw = 34892;  // DNG; raw, reduced-resolution or enhanced IFD
inline constexpr std::uint16_t kDepth = 51177;
inline constexpr std::uint16_t kPhotometricMask = 52527;
}  // namespace tiff_photometric

// NewSubFileType tag (254) values assigned by DNG 1.7.1.0 chapter 3.
namespace dng_subfile_type {
inline constexpr std::uint32_t kMainImage = 0;  // highest-resolution raw image (the raw IFD)
inline constexpr std::uint32_t kPreview = 1;    // reduced-resolution image or primary preview
inline constexpr std::uint32_t kTransparencyMask = 4;
inline constexpr std::uint32_t kPreviewTransparencyMask = 5;
inline constexpr std::uint32_t kDepthMap = 8;
inline constexpr std::uint32_t kPreviewDepthMap = 9;
inline constexpr std::uint32_t kEnhancedImage = 16;
inline constexpr std::uint32_t kAlternatePreview = 0x10001;
inline constexpr std::uint32_t kSemanticMask = 0x10004;
}  // namespace dng_subfile_type

}  // namespace lucent::raw
