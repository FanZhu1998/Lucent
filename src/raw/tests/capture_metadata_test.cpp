// Unit tests for read_capture_metadata() on synthetic TIFF, JPEG, CR3 and RAF streams, and for
// complete_from_decoder() and ExifRational::approximate().

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/raw/capture_metadata.hpp>
#include <lucent/raw/dng_tags.hpp>
#include <lucent/raw/tiff_inspector.hpp>

#include "tiff_test_writer.hpp"

namespace lucent::raw {
namespace {

namespace ts = test_support;
using ts::ascii_entry;
using ts::Built;
using ts::Document;
using ts::Endian;
using ts::Entry;
using ts::Format;
using ts::Ifd;
using ts::long_entry;
using ts::rational_entry;
using ts::short_entry;
using ts::srational_entry;

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> out;
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return out;
}

// ASCII entry with exactly these value bytes (embedded NULs and non-ASCII bytes allowed).
Entry ascii_bytes(std::uint16_t tag, std::string_view text) {
    std::vector<std::byte> bytes = bytes_of(text);
    const std::uint64_t count = bytes.size();
    return ts::raw_entry(tag, ts::type_code(TiffFieldType::Ascii), count, std::move(bytes));
}

Ifd ifd0_rights() {
    Ifd ifd;
    ifd.entries = {ascii_entry(271, "Canon"), ascii_entry(272, "Canon EOS R5"),
                   ascii_entry(315, "Jane Doe"), ascii_bytes(33432, std::string_view("(c) Jane Doe\0Editor\0", 20)),
                   short_entry(274, {1})};
    return ifd;
}

Ifd exif_ifd() {
    Ifd exif;
    exif.entries = {
        rational_entry(33434, {{1, 250}}),
        rational_entry(33437, {{28, 10}}),
        short_entry(34855, {400}),
        ascii_entry(36867, "2026:05:17 14:03:59"),
        ascii_entry(36881, "+09:30"),
        srational_entry(37380, {{-2, 3}}),
        rational_entry(37386, {{50, 1}}),
        ascii_entry(37521, "07"),
        short_entry(41989, {75}),
        ascii_entry(42035, "Canon"),
        ascii_entry(42036, "RF50mm F1.8 STM"),
        // Never read: maker note and a serial number.
        ts::undefined_entry(37500, bytes_of("MAKERNOTE")),
        ascii_entry(42033, "123456"),
    };
    return exif;
}

Ifd gps_ifd() {
    Ifd gps;
    gps.entries = {
        ts::byte_entry(0, {2, 3, 0, 0}),
        ascii_entry(1, "S"),
        rational_entry(2, {{33, 1}, {51, 1}, {3540, 100}}),  // 33 deg 51' 35.4"
        ascii_entry(3, "E"),
        rational_entry(4, {{151, 1}, {12, 1}, {3, 2}}),  // 151 deg 12' 1.5"
        ts::byte_entry(5, {1}),
        rational_entry(6, {{125, 10}}),
        rational_entry(7, {{4, 1}, {3, 1}, {595, 10}}),
        ascii_entry(29, "2026:05:17"),
    };
    return gps;
}

Built full_tiff(Endian endian = Endian::Little, Format format = Format::Classic) {
    Document doc;
    doc.endian = endian;
    doc.format = format;
    Ifd ifd0 = ifd0_rights();
    ifd0.exif.push_back(exif_ifd());
    ifd0.gps.push_back(gps_ifd());
    doc.chain.push_back(std::move(ifd0));
    return ts::build(doc);
}

Expected<CaptureMetadata> read(std::span<const std::byte> bytes, const CaptureMetadataLimits& limits = {}) {
    MemorySource source(bytes);
    return read_capture_metadata(source, limits);
}

CaptureMetadata read_ok(std::span<const std::byte> bytes, const CaptureMetadataLimits& limits = {}) {
    auto result = read(bytes, limits);
    EXPECT_TRUE(result) << result.error().describe();
    return result ? std::move(*result) : CaptureMetadata{};
}

void expect_full(const CaptureMetadata& m) {
    EXPECT_EQ(m.make, "Canon");
    EXPECT_EQ(m.model, "Canon EOS R5");
    EXPECT_EQ(m.artist, "Jane Doe");
    EXPECT_EQ(m.copyright, "(c) Jane Doe");
    EXPECT_EQ(m.lens_make, "Canon");
    EXPECT_EQ(m.lens_model, "RF50mm F1.8 STM");
    EXPECT_EQ(m.exposure_time_s, (ExifRational{1, 250}));
    EXPECT_EQ(m.f_number, (ExifRational{28, 10}));
    EXPECT_EQ(m.iso, 400u);
    EXPECT_EQ(m.focal_length_mm, (ExifRational{50, 1}));
    EXPECT_EQ(m.focal_length_35mm_mm, 75);
    EXPECT_EQ(m.exposure_bias_ev, (ExifSignedRational{-2, 3}));
    ASSERT_TRUE(m.capture_time);
    EXPECT_EQ(*m.capture_time, (ExifDateTime{2026, 5, 17, 14, 3, 59, "07", 570}));
    ASSERT_TRUE(m.gps);
    // Exact: these sums are computed identically here and in the reader.
    EXPECT_DOUBLE_EQ(m.gps->latitude_deg, -(33.0 + 51.0 / 60.0 + 35.4 / 3600.0));
    EXPECT_DOUBLE_EQ(m.gps->longitude_deg, 151.0 + 12.0 / 60.0 + 1.5 / 3600.0);
    ASSERT_TRUE(m.gps->altitude_m);
    EXPECT_DOUBLE_EQ(*m.gps->altitude_m, -12.5);
    ASSERT_TRUE(m.gps->utc);
    EXPECT_EQ(*m.gps->utc, (GpsTimestamp{2026, 5, 17, 4, 3, 59.5}));
}

TEST(CaptureMetadata, ReadsEveryFieldFromLittleEndianTiff) {
    const Built built = full_tiff();
    const CaptureMetadata m = read_ok(built.span());
    expect_full(m);
    EXPECT_TRUE(m.warnings.empty()) << m.warnings.front();
}

TEST(CaptureMetadata, ReadsBigEndianAndBigTiff) {
    expect_full(read_ok(full_tiff(Endian::Big).span()));
    expect_full(read_ok(full_tiff(Endian::Little, Format::BigTiff).span()));
}

TEST(CaptureMetadata, FileWithoutExifIsEmptyNotAnError) {
    Document doc;
    Ifd ifd0;
    ifd0.entries = {short_entry(256, {8})};
    doc.chain.push_back(ifd0);
    const CaptureMetadata m = read_ok(ts::build(doc).span());
    EXPECT_TRUE(m.make.empty());
    EXPECT_FALSE(m.capture_time);
    EXPECT_FALSE(m.gps);
    EXPECT_TRUE(m.warnings.empty());
}

TEST(CaptureMetadata, GpsIfdWithoutPositionYieldsNoLocation) {
    Document doc;
    Ifd ifd0 = ifd0_rights();
    Ifd gps;
    gps.entries = {ts::byte_entry(0, {2, 3, 0, 0})};
    ifd0.gps.push_back(gps);
    doc.chain.push_back(ifd0);
    const CaptureMetadata m = read_ok(ts::build(doc).span());
    EXPECT_FALSE(m.gps);
    EXPECT_TRUE(m.warnings.empty());
}

TEST(CaptureMetadata, ZeroPositionAndEmptyValuesAreAbsent) {
    Document doc;
    Ifd ifd0;
    ifd0.entries = {ascii_entry(271, "DJI"), ts::raw_entry(315, ts::type_code(TiffFieldType::Ascii), 0, {})};
    Ifd gps;
    gps.entries = {ascii_entry(1, "N"), rational_entry(2, {{0, 1}, {0, 1}, {0, 1}}), ascii_entry(3, "E"),
                   rational_entry(4, {{0, 1}, {0, 1}, {0, 1}})};
    ifd0.gps.push_back(gps);
    doc.chain.push_back(ifd0);
    const CaptureMetadata m = read_ok(ts::build(doc).span());
    EXPECT_TRUE(m.artist.empty());
    EXPECT_FALSE(m.gps);
    ASSERT_EQ(m.warnings.size(), 1u);  // only the 0,0 position is reported
    EXPECT_NE(m.warnings.front().find("no fix"), std::string::npos);
}

TEST(CaptureMetadata, MalformedFieldsAreDroppedWithWarnings) {
    Document doc;
    Ifd ifd0 = ifd0_rights();
    Ifd exif;
    exif.entries = {
        rational_entry(33434, {{1, 0}}),               // zero denominator
        short_entry(33437, {4}),                        // wrong type
        ascii_entry(36867, "2026:02:30 10:00:00"),     // no 30 February
        rational_entry(37386, {{0, 0}}),               // "unknown": silently absent
        short_entry(34855, {0}),                        // ISO 0: unknown
    };
    ifd0.exif.push_back(exif);
    Ifd gps;
    gps.entries = {rational_entry(2, {{10, 1}, {0, 1}, {0, 1}}), rational_entry(4, {{10, 1}, {0, 1}, {0, 1}})};
    ifd0.gps.push_back(gps);  // no references: the sign is unknown
    doc.chain.push_back(ifd0);

    const CaptureMetadata m = read_ok(ts::build(doc).span());
    EXPECT_EQ(m.make, "Canon");  // the rest still reads
    EXPECT_FALSE(m.exposure_time_s);
    EXPECT_FALSE(m.f_number);
    EXPECT_FALSE(m.capture_time);
    EXPECT_FALSE(m.focal_length_mm);
    EXPECT_FALSE(m.iso);
    EXPECT_FALSE(m.gps);
    EXPECT_EQ(m.warnings.size(), 4u);
}

TEST(CaptureMetadata, InvalidOffsetAndSubsecKeepTheTime) {
    Document doc;
    Ifd ifd0;
    ifd0.entries = {ascii_entry(271, "X")};
    Ifd exif;
    exif.entries = {ascii_entry(36867, "2024:02:29 23:59:59"), ascii_entry(36881, "+15:00"),
                    ascii_entry(37521, "12a")};
    ifd0.exif.push_back(exif);
    doc.chain.push_back(ifd0);
    const CaptureMetadata m = read_ok(ts::build(doc).span());
    ASSERT_TRUE(m.capture_time);
    EXPECT_EQ(*m.capture_time, (ExifDateTime{2024, 2, 29, 23, 59, 59, "", std::nullopt}));
    EXPECT_EQ(m.warnings.size(), 2u);
}

TEST(CaptureMetadata, BlankTimeIsUnknownWithoutWarning) {
    Document doc;
    Ifd ifd0;
    ifd0.entries = {ascii_entry(271, "X")};
    Ifd exif;
    exif.entries = {ascii_entry(36867, "    :  :     :  :  ")};
    ifd0.exif.push_back(exif);
    doc.chain.push_back(ifd0);
    const CaptureMetadata m = read_ok(ts::build(doc).span());
    EXPECT_FALSE(m.capture_time);
    EXPECT_TRUE(m.warnings.empty());
}

TEST(CaptureMetadata, IsoAboveSixtyFiveThousandUsesThePreciseTag) {
    Document doc;
    Ifd ifd0;
    ifd0.entries = {ascii_entry(271, "X")};
    Ifd exif;
    exif.entries = {short_entry(34855, {65535}), short_entry(34864, {2}), long_entry(34866, {102400})};
    ifd0.exif.push_back(exif);
    doc.chain.push_back(ifd0);
    EXPECT_EQ(read_ok(ts::build(doc).span()).iso, 102400u);
}

TEST(CaptureMetadata, TextIsSanitisedTrimmedAndUnicodePreserved) {
    Document doc;
    Ifd ifd0;
    ifd0.entries = {
        ascii_bytes(271, std::string_view("  Ricoh  \0junk", 14)),
        ascii_bytes(272, "Model\x01X"),                        // control character
        ascii_bytes(315, "Zo\xC3\xAB \xE5\xBC\xA0"),           // UTF-8 "Zoë 张"
        ascii_bytes(33432, std::string_view(" \0Agency Ltd\0", 13)),  // editor only
        ascii_bytes(42035, "x"),
    };
    Ifd exif;
    exif.entries = {ascii_bytes(42036, "Lens \xE9"),  // Latin-1 e-acute: not UTF-8
                    ascii_entry(42035, std::string(64, ' '))};  // padded blank
    ifd0.exif.push_back(exif);
    doc.chain.push_back(ifd0);
    const CaptureMetadata m = read_ok(ts::build(doc).span());
    EXPECT_EQ(m.make, "Ricoh");
    EXPECT_EQ(m.model, "Model X");
    EXPECT_EQ(m.artist, "Zo\xC3\xAB \xE5\xBC\xA0");
    EXPECT_EQ(m.copyright, "Agency Ltd");
    EXPECT_EQ(m.lens_model, "Lens \xEF\xBF\xBD");  // U+FFFD
    EXPECT_TRUE(m.lens_make.empty());
}

TEST(CaptureMetadata, OversizedValueIsDroppedWithWarning) {
    Document doc;
    Ifd ifd0;
    ifd0.entries = {ascii_entry(271, "Canon"), ascii_entry(315, std::string(5000, 'a'))};
    doc.chain.push_back(ifd0);
    CaptureMetadataLimits limits;
    limits.max_value_bytes = 4096;
    const CaptureMetadata m = read_ok(ts::build(doc).span(), limits);
    EXPECT_EQ(m.make, "Canon");
    EXPECT_TRUE(m.artist.empty());
    EXPECT_EQ(m.warnings.size(), 1u);
}

TEST(CaptureMetadata, BrokenSubIfdIsAWarningButBrokenIfd0IsCorrupt) {
    Built built = full_tiff();
    // Point the Exif IFD beyond the end.
    built.put(built.value("ifd0", 34665), built.bytes.size() + 100, 4);
    const CaptureMetadata m = read_ok(built.span());
    EXPECT_EQ(m.make, "Canon");
    EXPECT_FALSE(m.exposure_time_s);
    EXPECT_TRUE(m.gps);
    EXPECT_EQ(m.warnings.size(), 1u);

    Built truncated = full_tiff();
    truncated.put(4, truncated.bytes.size() + 8, 4);  // IFD0 offset beyond the end
    auto result = read(truncated.span());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::Corrupt);
}

TEST(CaptureMetadata, UnknownContainerIsUnsupported) {
    const std::vector<std::byte> bytes = bytes_of("not a photograph at all");
    auto result = read(bytes);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::Unsupported);
    auto empty = read({});
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, ErrorCode::Unsupported);
}

// ---- JPEG, RAF and CR3 ---------------------------------------------------------------------

void put_be(std::vector<std::byte>& out, std::uint64_t value, int size) {
    for (int i = size - 1; i >= 0; --i) {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFF));
    }
}

void append(std::vector<std::byte>& out, std::span<const std::byte> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void segment(std::vector<std::byte>& out, std::uint8_t code, std::span<const std::byte> payload) {
    out.push_back(std::byte{0xFF});
    out.push_back(static_cast<std::byte>(code));
    put_be(out, payload.size() + 2, 2);
    append(out, payload);
}

// SOI, APP0 JFIF, APP1 XMP, APP1 Exif(tiff), SOS (data not needed), EOI.
std::vector<std::byte> jpeg_with_exif(std::span<const std::byte> tiff) {
    std::vector<std::byte> out = {std::byte{0xFF}, std::byte{0xD8}};
    segment(out, 0xE0, bytes_of(std::string_view("JFIF\0\1\1\0\0\1\0\1\0\0", 14)));
    segment(out, 0xE1, bytes_of(std::string_view("http://ns.adobe.com/xap/1.0/\0<x/>", 33)));
    std::vector<std::byte> exif = bytes_of(std::string_view("Exif\0\0", 6));
    append(exif, tiff);
    segment(out, 0xE1, exif);
    segment(out, 0xDA, bytes_of("\x01\x01\x00\x00\x3F\x00"));
    out.push_back(std::byte{0xFF});
    out.push_back(std::byte{0xD9});
    return out;
}

TEST(CaptureMetadata, ReadsJpegApp1AfterOtherSegments) {
    const Built tiff = full_tiff(Endian::Big);
    const CaptureMetadata m = read_ok(jpeg_with_exif(tiff.span()));
    expect_full(m);
}

TEST(CaptureMetadata, JpegWithoutExifIsEmptyAndOverrunIsCorrupt) {
    std::vector<std::byte> plain = {std::byte{0xFF}, std::byte{0xD8}};
    segment(plain, 0xE0, bytes_of("JFIF"));
    segment(plain, 0xDA, bytes_of("x"));
    EXPECT_TRUE(read_ok(plain).make.empty());

    std::vector<std::byte> overrun = {std::byte{0xFF}, std::byte{0xD8}, std::byte{0xFF}, std::byte{0xE1},
                                      std::byte{0x40}, std::byte{0x00}, std::byte{'E'}};
    auto result = read(overrun);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::Corrupt);
}

TEST(CaptureMetadata, CorruptExifInsideJpegIsAWarning) {
    // IFD0 offset far outside the stream.
    std::vector<std::byte> broken = bytes_of(std::string_view("II*\0\xFF\xFF\xFF\x0F", 8));
    const CaptureMetadata m = read_ok(jpeg_with_exif(broken));
    EXPECT_TRUE(m.make.empty());
    EXPECT_EQ(m.warnings.size(), 1u);
}

TEST(CaptureMetadata, ReadsRafPreviewExif) {
    const Built tiff = full_tiff();
    const std::vector<std::byte> jpeg = jpeg_with_exif(tiff.span());
    std::vector<std::byte> raf = bytes_of("FUJIFILMCCD-RAW 0201FF383501");
    raf.resize(84, std::byte{0});
    put_be(raf, 100, 4);
    put_be(raf, jpeg.size(), 4);
    raf.resize(100, std::byte{0});
    append(raf, jpeg);
    expect_full(read_ok(raf));

    std::vector<std::byte> outside = raf;
    outside[88] = std::byte{0x7F};  // length far beyond the file
    auto result = read(outside);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::Corrupt);
}

std::vector<std::byte> box(std::string_view type, std::span<const std::byte> payload) {
    std::vector<std::byte> out;
    put_be(out, payload.size() + 8, 4);
    append(out, bytes_of(type));
    append(out, payload);
    return out;
}

// A TIFF stream whose IFD 0 holds `ifd`'s entries (no pointers), as in CR3 CMT boxes.
std::vector<std::byte> single_ifd_tiff(Ifd ifd) {
    ifd.exif.clear();
    ifd.gps.clear();
    Document doc;
    doc.chain.push_back(std::move(ifd));
    return ts::build(doc).bytes;
}

std::vector<std::byte> cr3(std::uint32_t cmt2_size_override = 0) {
    std::vector<std::byte> canon;
    const std::uint8_t uuid[16] = {0x85, 0xc0, 0xb6, 0x87, 0x82, 0x0f, 0x11, 0xe0,
                                   0x81, 0x11, 0xf4, 0xce, 0x46, 0x2b, 0x6a, 0x48};
    for (const std::uint8_t b : uuid) {
        canon.push_back(static_cast<std::byte>(b));
    }
    append(canon, box("CNCV", bytes_of("CanonCR3_001/00.09.00/00.00.00")));
    append(canon, box("CMT1", single_ifd_tiff(ifd0_rights())));
    std::vector<std::byte> cmt2 = box("CMT2", single_ifd_tiff(exif_ifd()));
    if (cmt2_size_override != 0) {
        cmt2[0] = cmt2[1] = cmt2[2] = std::byte{0};
        cmt2[3] = static_cast<std::byte>(cmt2_size_override);
    }
    append(canon, cmt2);
    append(canon, box("CMT3", bytes_of("maker notes are never read")));
    append(canon, box("CMT4", single_ifd_tiff(gps_ifd())));

    std::vector<std::byte> moov = box("mvhd", bytes_of("header"));
    append(moov, box("uuid", canon));
    std::vector<std::byte> file = box("ftyp", bytes_of(std::string_view("crx \0\0\0\1crx isom", 16)));
    append(file, box("moov", moov));
    append(file, box("mdat", bytes_of("pixels")));
    return file;
}

TEST(CaptureMetadata, ReadsCr3CanonMetadataBoxes) {
    const CaptureMetadata m = read_ok(cr3());
    expect_full(m);
    EXPECT_TRUE(m.warnings.empty());
}

TEST(CaptureMetadata, Cr3BoxLimitsAndOverruns) {
    CaptureMetadataLimits small;
    small.max_embedded_bytes = 64;  // every CMT box is larger
    const CaptureMetadata skipped = read_ok(cr3(), small);
    EXPECT_TRUE(skipped.make.empty());
    EXPECT_EQ(skipped.warnings.size(), 3u);

    CaptureMetadataLimits few;
    few.max_boxes = 3;
    auto limited = read(cr3(), few);
    ASSERT_FALSE(limited);
    EXPECT_EQ(limited.error().code, ErrorCode::Corrupt);

    auto overrun = read(cr3(4));  // CMT2 size smaller than its header
    ASSERT_FALSE(overrun);
    EXPECT_EQ(overrun.error().code, ErrorCode::Corrupt);
}

// ---- Decoder fallback and rationals --------------------------------------------------------

TEST(CaptureMetadata, CompleteFromDecoderFillsOnlyMissingFields) {
    CaptureMetadata m;
    m.make = "FromExif";
    m.iso = 200;
    RawMetadata decoded;
    decoded.camera.make = "FromDecoder";
    decoded.camera.model = "Model";
    decoded.lens.model = "Lens";
    decoded.lens.focal_length_35mm_equivalent_mm = 52.4f;
    decoded.capture.iso = 6400.0f;
    decoded.capture.exposure_time_s = 1.0f / 8000.0f;
    decoded.capture.f_number = 5.6f;
    decoded.capture.focal_length_mm = 35.0f;
    decoded.capture.capture_time_local = "2025-12-31T23:59:58";
    complete_from_decoder(m, decoded);
    EXPECT_EQ(m.make, "FromExif");
    EXPECT_EQ(m.model, "Model");
    EXPECT_EQ(m.lens_model, "Lens");
    EXPECT_EQ(m.iso, 200u);
    EXPECT_EQ(m.exposure_time_s, (ExifRational{1, 8000}));
    EXPECT_EQ(m.f_number, (ExifRational{28, 5}));
    EXPECT_EQ(m.focal_length_mm, (ExifRational{35, 1}));
    EXPECT_EQ(m.focal_length_35mm_mm, 52);
    ASSERT_TRUE(m.capture_time);
    EXPECT_EQ(*m.capture_time, (ExifDateTime{2025, 12, 31, 23, 59, 58, "", std::nullopt}));
    EXPECT_FALSE(m.gps);
}

TEST(CaptureMetadata, RationalApproximation) {
    EXPECT_EQ(ExifRational::approximate(0.004), (ExifRational{1, 250}));
    EXPECT_EQ(ExifRational::approximate(1.0 / 3.0), (ExifRational{1, 3}));
    EXPECT_EQ(ExifRational::approximate(0.3), (ExifRational{3, 10}));
    EXPECT_EQ(ExifRational::approximate(2.8), (ExifRational{14, 5}));
    EXPECT_EQ(ExifRational::approximate(50.0), (ExifRational{50, 1}));
    EXPECT_EQ(ExifRational::approximate(0.0), (ExifRational{0, 1}));
    const auto pi = ExifRational::approximate(3.14159265358979, 1000);
    ASSERT_TRUE(pi);
    EXPECT_EQ(*pi, (ExifRational{355, 113}));
    EXPECT_FALSE(ExifRational::approximate(-1.0));
    EXPECT_FALSE(ExifRational::approximate(std::numeric_limits<double>::quiet_NaN()));
    EXPECT_FALSE(ExifRational::approximate(1e12));
}

}  // namespace
}  // namespace lucent::raw
