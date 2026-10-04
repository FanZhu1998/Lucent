// Export metadata: prepare_metadata (policy, sanitising, limits), the JPEG APP1 and TIFF
// Exif/GPS writers, read-back verification, receipts and hashes.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/core/hash.hpp>
#include <lucent/core/text.hpp>
#include <lucent/export/export.hpp>
#include <lucent/export/metadata.hpp>

#include "codecs.hpp"
#include "exif_format.hpp"
#include "exif_reader.hpp"
#include "exif_writer.hpp"
#include "export_test_support.hpp"
#include "fault_injection.hpp"

namespace lucent::exporter {
namespace {

using test_support::list_directory;
using test_support::TempDir;

image::ColorEncoding rec2020_display() {
    image::ColorEncoding e = image::ColorEncoding::scene_linear_rec2020_d65();
    e.reference = image::ImageReference::Display;
    return e;
}

OutputImage quantise(std::int32_t width, std::int32_t height, const ExportSettings& settings) {
    auto rgb = test_support::make_gradient(width, height);
    auto out = encode_for_output(rgb.const_view(), rec2020_display(), settings.profile, bit_depth_for(settings.format),
                                 settings.transform);
    if (!out) {
        ADD_FAILURE() << out.error().describe();
        return {};
    }
    return std::move(*out);
}

// A photograph's metadata with every field set.
ExportMetadata full_metadata() {
    ExportMetadata m;
    m.camera_make = "Canon";
    m.camera_model = "Canon EOS R5";
    m.lens_make = "Canon";
    m.lens_model = "RF24-105mm F4 L IS USM";
    m.exposure_time_s = ExportRational{1, 250};
    m.f_number = ExportRational{28, 10};
    m.iso = 400;
    m.exposure_bias_ev = ExportSignedRational{-2, 3};
    m.focal_length_mm = ExportRational{105, 1};
    m.focal_length_35mm_mm = std::uint16_t{105};
    m.capture_time = CaptureDateTime{2026, 5, 17, 14, 3, 59, "07", 570};
    m.artist = "Jane Doe";
    m.copyright = "(c) 2026 Jane Doe";
    GpsPosition gps;
    gps.latitude_deg = -(33.0 + 51.0 / 60.0 + 35.4 / 3600.0);
    gps.longitude_deg = 151.0 + 12.0 / 60.0 + 1.5 / 3600.0;
    gps.altitude_m = -12.5;
    gps.utc = GpsTimestamp{2026, 5, 17, 4, 33, 59.5};
    m.gps = gps;
    return m;
}

ExportSettings with_gps(ExportSettings settings) {
    settings.metadata.include_gps = true;
    return settings;
}

std::vector<ExportSettings> both_formats() {
    return {ExportSettings::jpeg_srgb_q95(), ExportSettings::tiff16(OutputProfile::Srgb)};
}

bool is_jpeg(const ExportSettings& settings) {
    return settings.format == ExportFormat::Jpeg;
}

std::string bytes_of_file(const std::filesystem::path& path) {
    const std::vector<char> bytes = test_support::read_all(path);
    return std::string(bytes.begin(), bytes.end());
}

detail::ParsedExif parse(const std::filesystem::path& path) {
    auto parsed = detail::read_exif_file(path);
    EXPECT_TRUE(parsed) << parsed.error().describe();
    return parsed ? std::move(*parsed) : detail::ParsedExif{};
}

// Exact, including every fraction, in both formats; GPS doubles are rebuilt from fractions and
// compared within binary rounding by metadata_difference.
void expect_same(const ExportMetadata& actual, const ExportMetadata& expected) {
    ExportMetadata a = actual;
    ExportMetadata e = expected;
    a.gps.reset();
    e.gps.reset();
    EXPECT_EQ(a, e);
    const auto difference = detail::metadata_difference(actual, expected);
    EXPECT_FALSE(difference) << *difference;
}

// ---------------------------------------------------------------------------------------------
// prepare_metadata

TEST(ExportMetadataPrepare, PolicyGroupsAndDefaults) {
    const MetadataPolicy defaults;
    EXPECT_TRUE(defaults.include_capture);
    EXPECT_TRUE(defaults.include_copyright);
    EXPECT_FALSE(defaults.include_gps);  // location only on an explicit choice
    EXPECT_TRUE(defaults.include_software);

    const ExportMetadata source = full_metadata();
    const PreparedMetadata kept = prepare_metadata(source, defaults);
    EXPECT_FALSE(kept.metadata.gps);
    EXPECT_EQ(kept.metadata.camera_model, "Canon EOS R5");
    EXPECT_EQ(kept.metadata.artist, "Jane Doe");
    EXPECT_EQ(kept.metadata.software, software_name());
    EXPECT_TRUE(kept.metadata.software.starts_with("Lucent "));
    EXPECT_TRUE(kept.notes.empty());

    MetadataPolicy none{false, false, false, false};
    EXPECT_TRUE(prepare_metadata(source, none).metadata.empty());

    MetadataPolicy rights_only{false, true, false, false};
    const ExportMetadata rights = prepare_metadata(source, rights_only).metadata;
    EXPECT_FALSE(rights.has_capture());
    EXPECT_EQ(rights.copyright, "(c) 2026 Jane Doe");

    ExportMetadata ignored;
    ignored.software = "Something else";
    EXPECT_EQ(prepare_metadata(ignored, defaults).metadata.software, software_name());
    EXPECT_TRUE(prepare_metadata(ignored, none).metadata.software.empty());
}

TEST(ExportMetadataPrepare, AsciiTagsAndUnicodeText) {
    ExportMetadata source;
    source.camera_make = "  Caf\xC3\xA9\x01X  ";      // non-ASCII and a control character
    source.lens_model = "Lens\xFF";                    // ill-formed UTF-8
    source.artist = "Zo\xC3\xAB \xE5\xBC\xA0\tSmith";  // "Zoë 张<TAB>Smith"
    source.copyright = "\xC2\xA9 Agency";              // "© Agency"
    const PreparedMetadata p = prepare_metadata(source, MetadataPolicy{});
    EXPECT_EQ(p.metadata.camera_make, "Caf? X");
    EXPECT_EQ(p.metadata.lens_model, "Lens?");
    EXPECT_EQ(p.metadata.artist, "Zo\xC3\xAB \xE5\xBC\xA0 Smith");
    EXPECT_EQ(p.metadata.copyright, "\xC2\xA9 Agency");
    EXPECT_EQ(p.notes.size(), 3u);  // make, lens model, artist (tab)
}

TEST(ExportMetadataPrepare, SizeLimitsCutAtCharacterBoundaries) {
    ExportMetadata source;
    source.camera_model = std::string(300, 'M');
    std::string long_text;
    while (long_text.size() < 2600) {
        long_text += "\xE5\xBC\xA0";  // 3-byte character
    }
    source.copyright = long_text;
    const PreparedMetadata p = prepare_metadata(source, MetadataPolicy{});
    EXPECT_EQ(p.metadata.camera_model.size(), kMaxMetadataAsciiBytes);
    EXPECT_LE(p.metadata.copyright.size(), kMaxMetadataTextBytes);
    EXPECT_EQ(p.metadata.copyright.size() % 3, 0u);
    EXPECT_TRUE(core::is_valid_utf8(p.metadata.copyright));
    EXPECT_EQ(p.notes.size(), 2u);
}

TEST(ExportMetadataPrepare, InvalidValuesAreDroppedWithNotes) {
    ExportMetadata source;
    source.exposure_time_s = ExportRational{1, 0};
    source.f_number = ExportRational{0, 10};
    source.iso = 0;
    source.exposure_bias_ev = ExportSignedRational{1, 0};
    source.focal_length_35mm_mm = std::uint16_t{0};
    source.capture_time = CaptureDateTime{2023, 2, 29, 10, 0, 0, "", std::nullopt};  // not a leap year
    GpsPosition gps;
    gps.latitude_deg = 91.0;
    source.gps = gps;
    const PreparedMetadata p = prepare_metadata(source, MetadataPolicy{true, true, true, false});
    EXPECT_TRUE(p.metadata.empty());
    EXPECT_EQ(p.notes.size(), 7u);

    ExportMetadata partial;
    partial.capture_time = CaptureDateTime{2024, 2, 29, 23, 59, 59, "12a", 15 * 60};
    GpsPosition valid;
    valid.latitude_deg = 10.0;
    valid.longitude_deg = -20.0;
    valid.altitude_m = std::numeric_limits<double>::infinity();
    valid.utc = GpsTimestamp{2024, 13, 1, 0, 0, 0.0};
    partial.gps = valid;
    const PreparedMetadata q = prepare_metadata(partial, MetadataPolicy{true, true, true, false});
    ASSERT_TRUE(q.metadata.capture_time);
    EXPECT_EQ(*q.metadata.capture_time, (CaptureDateTime{2024, 2, 29, 23, 59, 59, "", std::nullopt}));
    ASSERT_TRUE(q.metadata.gps);
    EXPECT_FALSE(q.metadata.gps->altitude_m);
    EXPECT_FALSE(q.metadata.gps->utc);
    EXPECT_EQ(q.notes.size(), 4u);
}

TEST(ExportMetadataPrepare, NumbersAreNormalisedToTheirWrittenResolution) {
    ExportMetadata source;
    GpsPosition gps;
    gps.latitude_deg = 48.858370123456789;
    gps.longitude_deg = -2.294481987654321;
    gps.altitude_m = 35.12345;
    gps.utc = GpsTimestamp{2026, 1, 2, 3, 4, 59.99999};
    source.gps = gps;
    const PreparedMetadata p = prepare_metadata(source, MetadataPolicy{false, false, true, false});
    ASSERT_TRUE(p.metadata.gps);
    const GpsPosition& out = *p.metadata.gps;
    EXPECT_NEAR(out.latitude_deg, gps.latitude_deg, 0.5 / 3.6e9);
    EXPECT_NEAR(out.longitude_deg, gps.longitude_deg, 0.5 / 3.6e9);
    EXPECT_DOUBLE_EQ(*out.altitude_m, 35.123);
    EXPECT_DOUBLE_EQ(out.utc->second, 59.999);  // truncated, never 60
    // Idempotent: preparing the prepared value changes nothing.
    EXPECT_EQ(prepare_metadata(p.metadata, MetadataPolicy{false, false, true, false}).metadata, p.metadata);
}

// ---------------------------------------------------------------------------------------------
// APP1 structure

TEST(ExportMetadataJpeg, App1IsMinimalLittleEndianExif) {
    const PreparedMetadata p = prepare_metadata(full_metadata(), MetadataPolicy{true, true, true, true});
    auto app1 = detail::build_exif_app1(p.metadata, {40, 30, OutputProfile::Srgb});
    ASSERT_TRUE(app1) << app1.error().describe();
    ASSERT_GT(app1->size(), 14u);
    EXPECT_EQ(std::memcmp(app1->data(), "Exif\0\0II*\0\x08\0\0\0", 14), 0);
    EXPECT_LE(app1->size(), detail::kMaxApp1Payload);
    auto empty = detail::build_exif_app1(ExportMetadata{}, {40, 30, OutputProfile::Srgb});
    ASSERT_TRUE(empty);
    EXPECT_TRUE(empty->empty());

    // The largest metadata the limits allow still fits one APP1 segment.
    ExportMetadata big = full_metadata();
    big.camera_make = big.camera_model = big.lens_make = big.lens_model = std::string(1000, 'x');
    big.artist = big.copyright = std::string(5000, 'y');
    const PreparedMetadata limited = prepare_metadata(big, MetadataPolicy{true, true, true, true});
    auto largest = detail::build_exif_app1(limited.metadata, {40, 30, OutputProfile::Srgb});
    ASSERT_TRUE(largest) << largest.error().describe();
    EXPECT_LT(largest->size(), detail::kMaxApp1Payload);
}

TEST(ExportMetadataJpeg, MarkerOrderIsSoiJfifExifIcc) {
    TempDir dir;
    const ExportSettings settings = ExportSettings::jpeg_srgb_q95();
    ASSERT_TRUE(write_verified(quantise(32, 24, settings), dir / L"order.jpg", settings, full_metadata()));
    const std::string bytes = bytes_of_file(dir / L"order.jpg");
    ASSERT_GT(bytes.size(), 64u);
    std::size_t pos = 2;
    std::vector<int> markers;
    std::vector<std::string> signatures;
    while (pos + 4 <= bytes.size() && markers.size() < 4) {
        const int code = static_cast<unsigned char>(bytes[pos + 1]);
        const std::size_t length = (static_cast<unsigned char>(bytes[pos + 2]) << 8) |
                                   static_cast<unsigned char>(bytes[pos + 3]);
        markers.push_back(code);
        signatures.push_back(bytes.substr(pos + 4, 4));
        pos += 2 + length;
    }
    ASSERT_EQ(markers.size(), 4u);
    EXPECT_EQ(markers[0], 0xE0);
    EXPECT_EQ(signatures[0], "JFIF");
    EXPECT_EQ(markers[1], 0xE1);
    EXPECT_EQ(signatures[1], "Exif");
    EXPECT_EQ(markers[2], 0xE2);
    EXPECT_EQ(signatures[2], "ICC_");
}

// ---------------------------------------------------------------------------------------------
// Round trips

TEST(ExportMetadataRoundTrip, EveryFieldWithGps) {
    for (const ExportSettings& base : both_formats()) {
        SCOPED_TRACE(std::string(to_string(base.format)));
        TempDir dir;
        const ExportSettings settings = with_gps(base);
        const OutputImage image = quantise(48, 32, settings);
        const auto path = dir / (is_jpeg(settings) ? L"full.jpg" : L"full.tif");
        auto receipt = write_verified(image, path, settings, full_metadata());
        ASSERT_TRUE(receipt) << receipt.error().describe();

        const PreparedMetadata expected = prepare_metadata(full_metadata(), settings.metadata);
        EXPECT_EQ(receipt->metadata, expected.metadata);
        EXPECT_TRUE(receipt->metadata_notes.empty());
        EXPECT_EQ(receipt->settings.metadata, settings.metadata);
        ASSERT_TRUE(receipt->metadata.gps);

        auto read = read_export_metadata(path);
        ASSERT_TRUE(read) << read.error().describe();
        expect_same(*read, expected.metadata);

        const detail::ParsedExif parsed = parse(path);
        EXPECT_TRUE(parsed.has_exif_ifd);
        EXPECT_TRUE(parsed.has_gps_ifd);
        EXPECT_EQ(parsed.exif_version, "0232");
        EXPECT_EQ(parsed.color_space, std::optional<std::uint16_t>(std::uint16_t{1}));
        EXPECT_EQ(parsed.pixel_x, std::optional<std::uint32_t>(48));
        EXPECT_EQ(parsed.pixel_y, std::optional<std::uint32_t>(32));
        EXPECT_EQ(parsed.orientation, std::optional<std::uint16_t>(std::uint16_t{1}));
        EXPECT_EQ(parsed.ifd0_next, 0u);  // no IFD 1, so no thumbnail
        EXPECT_EQ(std::count(parsed.exif_tags.begin(), parsed.exif_tags.end(), detail::exif_tag::kMakerNote), 0);

        // The receipt's hash covers the metadata bytes.
        auto hashed = core::hash_file(path, {});
        ASSERT_TRUE(hashed);
        EXPECT_EQ(receipt->sha256, hashed->digest);
        EXPECT_EQ(receipt->byte_size, std::filesystem::file_size(test_support::verbatim(path)));
        EXPECT_NE(bytes_of_file(path).find("Jane Doe"), std::string::npos);
    }
}

TEST(ExportMetadataRoundTrip, GpsIsRemovedUnlessIncluded) {
    for (const ExportSettings& settings : both_formats()) {
        SCOPED_TRACE(std::string(to_string(settings.format)));
        TempDir dir;
        const auto path = dir / (is_jpeg(settings) ? L"nogps.jpg" : L"nogps.tif");
        auto receipt = write_verified(quantise(24, 16, settings), path, settings, full_metadata());
        ASSERT_TRUE(receipt) << receipt.error().describe();
        EXPECT_FALSE(receipt->metadata.gps);
        const detail::ParsedExif parsed = parse(path);
        EXPECT_FALSE(parsed.has_gps_ifd);
        EXPECT_FALSE(parsed.metadata.gps);
        EXPECT_TRUE(parsed.has_exif_ifd);
        EXPECT_EQ(std::count(parsed.ifd0_tags.begin(), parsed.ifd0_tags.end(), detail::exif_tag::kGpsIfd), 0);
    }
}

TEST(ExportMetadataRoundTrip, AbsentFieldsStayAbsent) {
    for (const ExportSettings& settings : both_formats()) {
        SCOPED_TRACE(std::string(to_string(settings.format)));
        TempDir dir;
        ExportMetadata rights;
        rights.artist = "Only An Artist";
        const auto path = dir / (is_jpeg(settings) ? L"artist.jpg" : L"artist.tif");
        auto receipt = write_verified(quantise(24, 16, settings), path, settings, rights);
        ASSERT_TRUE(receipt) << receipt.error().describe();
        const detail::ParsedExif parsed = parse(path);
        EXPECT_FALSE(parsed.has_exif_ifd);  // no capture field: no Exif IFD at all
        EXPECT_FALSE(parsed.has_gps_ifd);
        ExportMetadata expected;
        expected.artist = "Only An Artist";
        expected.software = software_name();
        EXPECT_EQ(parsed.metadata, expected);

        // A partial capture set writes only those tags.
        ExportMetadata partial;
        partial.iso = 100;
        const auto partial_path = dir / (is_jpeg(settings) ? L"iso.jpg" : L"iso.tif");
        ASSERT_TRUE(write_verified(quantise(24, 16, settings), partial_path, settings, partial));
        const detail::ParsedExif p = parse(partial_path);
        EXPECT_TRUE(p.has_exif_ifd);
        EXPECT_EQ(p.metadata.iso, std::optional<std::uint32_t>(100));
        EXPECT_FALSE(p.metadata.exposure_time_s);
        EXPECT_FALSE(p.metadata.capture_time);
        EXPECT_TRUE(p.metadata.camera_make.empty());
    }
}

TEST(ExportMetadataRoundTrip, WithoutMetadataOnlySoftwareIsWritten) {
    for (const ExportSettings& settings : both_formats()) {
        SCOPED_TRACE(std::string(to_string(settings.format)));
        TempDir dir;
        const auto path = dir / (is_jpeg(settings) ? L"plain.jpg" : L"plain.tif");
        auto receipt = write_verified(quantise(24, 16, settings), path, settings);
        ASSERT_TRUE(receipt) << receipt.error().describe();
        ExportMetadata expected;
        expected.software = software_name();
        EXPECT_EQ(receipt->metadata, expected);
        auto read = read_export_metadata(path);
        ASSERT_TRUE(read) << read.error().describe();
        EXPECT_EQ(*read, expected);
    }
}

TEST(ExportMetadataRoundTrip, PolicyOffWritesNoExifAtAll) {
    TempDir dir;
    ExportSettings settings = ExportSettings::jpeg_srgb_q95();
    settings.metadata = MetadataPolicy{false, false, false, false};
    const auto path = dir / L"bare.jpg";
    auto receipt = write_verified(quantise(24, 16, settings), path, settings, full_metadata());
    ASSERT_TRUE(receipt) << receipt.error().describe();
    EXPECT_TRUE(receipt->metadata.empty());
    EXPECT_EQ(bytes_of_file(path).find(std::string("Exif\0\0", 6)), std::string::npos);
    EXPECT_EQ(parse(path).exif_segments, 0);

    ExportSettings tiff = ExportSettings::tiff16(OutputProfile::Srgb);
    tiff.metadata = settings.metadata;
    ASSERT_TRUE(write_verified(quantise(24, 16, tiff), dir / L"bare.tif", tiff, full_metadata()));
    const detail::ParsedExif parsed = parse(dir / L"bare.tif");
    EXPECT_TRUE(parsed.metadata.empty());
    EXPECT_EQ(std::count(parsed.ifd0_tags.begin(), parsed.ifd0_tags.end(), detail::exif_tag::kSoftware), 0);
}

TEST(ExportMetadataRoundTrip, UnicodeArtistAndCopyright) {
    for (const ExportSettings& settings : both_formats()) {
        SCOPED_TRACE(std::string(to_string(settings.format)));
        TempDir dir;
        ExportMetadata source;
        source.artist = "Zo\xC3\xAB \xE5\xBC\xA0 \xF0\x9F\x93\xB7";       // "Zoë 张 📷"
        source.copyright = "\xC2\xA9 2026 Zo\xC3\xAB. All rights reserved.";  // "© 2026 Zoë. ..."
        source.camera_make = "M\xC3\xBCller";                               // "Müller": ASCII tag
        const auto path = dir / (is_jpeg(settings) ? L"unicode.jpg" : L"unicode.tif");
        auto receipt = write_verified(quantise(24, 16, settings), path, settings, source);
        ASSERT_TRUE(receipt) << receipt.error().describe();
        auto read = read_export_metadata(path);
        ASSERT_TRUE(read) << read.error().describe();
        EXPECT_EQ(read->artist, source.artist);
        EXPECT_EQ(read->copyright, source.copyright);
        EXPECT_EQ(read->camera_make, "M?ller");
        EXPECT_EQ(receipt->metadata_notes.size(), 1u);
    }
}

TEST(ExportMetadataRoundTrip, LongTextWithinLimits) {
    for (const ExportSettings& settings : both_formats()) {
        SCOPED_TRACE(std::string(to_string(settings.format)));
        TempDir dir;
        ExportMetadata source = full_metadata();
        source.copyright = std::string(6000, 'c');
        source.lens_model = std::string(400, 'l');
        const auto path = dir / (is_jpeg(settings) ? L"long.jpg" : L"long.tif");
        auto receipt = write_verified(quantise(24, 16, settings), path, settings, source);
        ASSERT_TRUE(receipt) << receipt.error().describe();
        EXPECT_EQ(receipt->metadata.copyright.size(), kMaxMetadataTextBytes);
        EXPECT_EQ(receipt->metadata.lens_model.size(), kMaxMetadataAsciiBytes);
        EXPECT_EQ(receipt->metadata_notes.size(), 2u);
        auto read = read_export_metadata(path);
        ASSERT_TRUE(read);
        EXPECT_EQ(read->copyright, receipt->metadata.copyright);
    }
}

TEST(ExportMetadataRoundTrip, HighIsoAndDisplayP3ColorSpace) {
    for (ExportSettings settings : both_formats()) {
        SCOPED_TRACE(std::string(to_string(settings.format)));
        settings.profile = OutputProfile::DisplayP3;
        TempDir dir;
        ExportMetadata source;
        source.iso = 204800;
        source.exposure_bias_ev = ExportSignedRational{0, 1};
        const auto path = dir / (is_jpeg(settings) ? L"iso.jpg" : L"iso.tif");
        ASSERT_TRUE(write_verified(quantise(24, 16, settings), path, settings, source));
        const detail::ParsedExif parsed = parse(path);
        EXPECT_EQ(parsed.metadata.iso, std::optional<std::uint32_t>(204800));
        EXPECT_EQ(parsed.color_space, std::optional<std::uint16_t>(std::uint16_t{0xFFFF}));
        ASSERT_TRUE(parsed.metadata.exposure_bias_ev);
        EXPECT_EQ(parsed.metadata.exposure_bias_ev->value(), 0.0);
    }
}

TEST(ExportMetadataRoundTrip, ExportImageCarriesMetadata) {
    TempDir dir;
    const auto rgb = test_support::make_gradient(20, 12);
    auto receipt = export_image(rgb.const_view(), rec2020_display(), dir / L"img.jpg",
                                ExportSettings::jpeg_srgb_q95(), full_metadata());
    ASSERT_TRUE(receipt) << receipt.error().describe();
    auto read = read_export_metadata(dir / L"img.jpg");
    ASSERT_TRUE(read);
    EXPECT_EQ(read->camera_model, "Canon EOS R5");
}

// ---------------------------------------------------------------------------------------------
// Receipts and hashes

TEST(ExportMetadataHashes, IdenticalInputsGiveIdenticalBytesAndMetadataChangesTheHash) {
    for (const ExportSettings& settings : both_formats()) {
        SCOPED_TRACE(std::string(to_string(settings.format)));
        TempDir dir;
        const OutputImage image = quantise(32, 20, settings);
        const wchar_t* ext = is_jpeg(settings) ? L".jpg" : L".tif";
        auto a = write_verified(image, dir / (std::wstring(L"a") + ext), settings, full_metadata());
        auto b = write_verified(image, dir / (std::wstring(L"b") + ext), settings, full_metadata());
        ExportMetadata other = full_metadata();
        other.artist = "Someone Else";
        auto c = write_verified(image, dir / (std::wstring(L"c") + ext), settings, other);
        auto d = write_verified(image, dir / (std::wstring(L"d") + ext), with_gps(settings), full_metadata());
        ASSERT_TRUE(a && b && c && d);
        EXPECT_EQ(a->sha256, b->sha256);
        EXPECT_NE(a->sha256, c->sha256);
        EXPECT_NE(a->sha256, d->sha256);
        EXPECT_EQ(test_support::read_all(dir / (std::wstring(L"a") + ext)),
                  test_support::read_all(dir / (std::wstring(L"b") + ext)));
    }
}

TEST(ExportMetadataHashes, TiffHoldsEachValueOnceWithoutStaleCopies) {
    TempDir dir;
    const ExportSettings settings = with_gps(ExportSettings::tiff16(OutputProfile::Srgb));
    ExportMetadata m = full_metadata();
    m.artist = "Artist-Marker";
    m.copyright = "Copyright-Marker";
    ASSERT_TRUE(write_verified(quantise(24, 16, settings), dir / L"once.tif", settings, m));
    const std::string bytes = bytes_of_file(dir / L"once.tif");
    for (const std::string_view marker : {"Artist-Marker", "Copyright-Marker", "Canon EOS R5"}) {
        const auto first = bytes.find(marker);
        ASSERT_NE(first, std::string::npos) << marker;
        EXPECT_EQ(bytes.find(marker, first + 1), std::string::npos) << marker;
    }
}

// ---------------------------------------------------------------------------------------------
// Verification failures

struct Tamper {
    std::string from;
    std::string to;
    bool done = false;
};

Status tamper_after_write(detail::ExportStage stage, const std::filesystem::path& temp, void* context) {
    auto* tamper = static_cast<Tamper*>(context);
    if (stage != detail::ExportStage::Written) {
        return ok();
    }
    std::string bytes = bytes_of_file(temp);
    const auto at = bytes.find(tamper->from);
    if (at != std::string::npos) {
        bytes.replace(at, tamper->to.size(), tamper->to);
        test_support::write_all(temp, bytes);
        tamper->done = true;
    }
    return ok();
}

class ScopedHook {
public:
    ScopedHook(detail::FaultHook hook, void* context) { detail::set_fault_hook(hook, context); }
    ~ScopedHook() { detail::set_fault_hook(nullptr, nullptr); }
    ScopedHook(const ScopedHook&) = delete;
    ScopedHook& operator=(const ScopedHook&) = delete;
};

TEST(ExportMetadataVerify, AlteredTiffMetadataFailsAndCleansUp) {
    TempDir dir;
    const ExportSettings settings = ExportSettings::tiff16(OutputProfile::Srgb);
    Tamper tamper{"Jane Doe", "Jane Dox"};
    ScopedHook hook(tamper_after_write, &tamper);
    auto receipt = write_verified(quantise(24, 16, settings), dir / L"t.tif", settings, full_metadata());
    ASSERT_TRUE(tamper.done);
    ASSERT_FALSE(receipt);
    EXPECT_EQ(receipt.error().code, ErrorCode::Corrupt) << receipt.error().describe();
    EXPECT_EQ(receipt.error().where, "export.tiff.verify");
    EXPECT_TRUE(list_directory(dir.path()).empty());
}

TEST(ExportMetadataVerify, DetectsUnexpectedOrMissingMetadata) {
    TempDir dir;
    for (const ExportSettings& base : both_formats()) {
        SCOPED_TRACE(std::string(to_string(base.format)));
        const ExportSettings settings = with_gps(base);
        const auto path = dir / (is_jpeg(settings) ? L"v.jpg" : L"v.tif");
        ASSERT_TRUE(write_verified(quantise(24, 16, settings), path, settings, full_metadata()));
        const detail::ExifImageInfo info{24, 16, OutputProfile::Srgb};
        const ExportMetadata written = prepare_metadata(full_metadata(), settings.metadata).metadata;
        EXPECT_TRUE(detail::verify_metadata(path, settings.format, written, info));

        ExportMetadata no_gps = written;
        no_gps.gps.reset();  // the file has GPS although none was to be written
        auto gps_status = detail::verify_metadata(path, settings.format, no_gps, info);
        ASSERT_FALSE(gps_status);
        EXPECT_EQ(gps_status.error().code, ErrorCode::Corrupt);

        ExportMetadata other_artist = written;
        other_artist.artist = "Somebody";
        EXPECT_FALSE(detail::verify_metadata(path, settings.format, other_artist, info));

        ExportMetadata other_exposure = written;
        other_exposure.exposure_time_s = ExportRational{1, 500};
        EXPECT_FALSE(detail::verify_metadata(path, settings.format, other_exposure, info));

        EXPECT_FALSE(detail::verify_metadata(path, settings.format, written, {25, 16, OutputProfile::Srgb}));
        EXPECT_FALSE(detail::verify_metadata(path, settings.format, written, {24, 16, OutputProfile::DisplayP3}));
    }
}

TEST(ExportMetadataRead, Errors) {
    TempDir dir;
    auto missing = read_export_metadata(dir / L"missing.jpg");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);

    test_support::write_all(dir / L"text.jpg", "plain text, not an image");
    auto text = read_export_metadata(dir / L"text.jpg");
    ASSERT_FALSE(text);
    EXPECT_EQ(text.error().code, ErrorCode::Unsupported);

    // A JPEG whose APP1 segment claims more bytes than the file holds.
    test_support::write_all(dir / L"cut.jpg", std::string("\xFF\xD8\xFF\xE1\x40\x00" "Exif\0\0II", 14));
    auto cut = read_export_metadata(dir / L"cut.jpg");
    ASSERT_FALSE(cut);
    EXPECT_EQ(cut.error().code, ErrorCode::Corrupt);

    // An EXIF IFD 0 pointing outside the segment.
    std::string bad("\xFF\xD8\xFF\xE1\x00\x10" "Exif\0\0" "II*\0\xF0\0\0\0" "\xFF\xD9", 22);
    test_support::write_all(dir / L"bad.jpg", bad);
    auto broken = read_export_metadata(dir / L"bad.jpg");
    ASSERT_FALSE(broken);
    EXPECT_EQ(broken.error().code, ErrorCode::Corrupt);

    // A JPEG without EXIF reads as empty metadata.
    test_support::write_all(dir / L"none.jpg", std::string("\xFF\xD8\xFF\xDA\x00\x02\xFF\xD9", 8));
    auto none = read_export_metadata(dir / L"none.jpg");
    ASSERT_TRUE(none) << none.error().describe();
    EXPECT_TRUE(none->empty());
}

}  // namespace
}  // namespace lucent::exporter
