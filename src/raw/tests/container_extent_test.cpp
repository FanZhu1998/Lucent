// Unit tests for the private container completeness check (src/container_extent.hpp) on synthetic
// CR3 box sequences, RAF headers and TIFF-structured vendor containers.

#include "container_extent.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/raw/decoded_raw.hpp>
#include <lucent/raw/dng_tags.hpp>
#include <lucent/raw/tiff_inspector.hpp>

#include "tiff_test_writer.hpp"

namespace lucent::raw::detail {
namespace {

namespace ts = test_support;

void put_be(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        bytes.at(offset + i) = static_cast<std::byte>((value >> (8 * (size - 1 - i))) & 0xFF);
    }
}

// Appends an ISO base media box of `size` bytes (header included); size 1 writes a 64-bit size.
void append_box(std::vector<std::byte>& bytes, std::string_view type, std::uint64_t size,
                bool large = false) {
    const std::size_t at = bytes.size();
    const std::size_t header = large ? 16 : 8;
    bytes.resize(at + std::max<std::size_t>(header, static_cast<std::size_t>(size)));
    put_be(bytes, at, large ? 1 : size, 4);
    std::memcpy(bytes.data() + at + 4, type.data(), 4);
    if (large) {
        put_be(bytes, at + 8, size, 8);
    }
}

// ftyp "crx ", moov and mdat, like a CR3.
std::vector<std::byte> cr3_file() {
    std::vector<std::byte> bytes;
    append_box(bytes, "ftyp", 24);
    std::memcpy(bytes.data() + 8, "crx ", 4);
    append_box(bytes, "moov", 400);
    append_box(bytes, "mdat", 4000);
    return bytes;
}

Expected<ImageDataExtent> check(const std::vector<std::byte>& bytes, ContainerFormat container) {
    MemorySource source(bytes);
    return find_container_overruns(source, container);
}

TEST(ContainerExtent, CompleteCr3Passes) {
    const std::vector<std::byte> bytes = cr3_file();
    auto result = check(bytes, ContainerFormat::Cr3);
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_TRUE(result->overruns.empty());
    EXPECT_TRUE(result->warnings.empty());
}

TEST(ContainerExtent, TruncatedCr3BoxIsAnOverrun) {
    std::vector<std::byte> bytes = cr3_file();
    for (const std::size_t cut : {std::size_t{1}, std::size_t{3000}}) {
        SCOPED_TRACE(cut);
        std::vector<std::byte> truncated(bytes.begin(), bytes.end() - static_cast<std::ptrdiff_t>(cut));
        auto result = check(truncated, ContainerFormat::Cr3);
        ASSERT_TRUE(result) << result.error().describe();
        ASSERT_EQ(result->overruns.size(), 1u);
        EXPECT_NE(result->overruns[0].ifd_path.find("mdat"), std::string::npos)
            << result->overruns[0].ifd_path;
        EXPECT_EQ(result->overruns[0].declared_end, bytes.size());
    }
    // Cut inside the moov box (a copy stopped after 4096 bytes of a real file looks like this).
    std::vector<std::byte> head(bytes.begin(), bytes.begin() + 100);
    auto result = check(head, ContainerFormat::Cr3);
    ASSERT_TRUE(result);
    ASSERT_EQ(result->overruns.size(), 1u);
    EXPECT_NE(result->overruns[0].ifd_path.find("moov"), std::string::npos);
    // Cut inside a box header.
    std::vector<std::byte> header_cut(bytes.begin(), bytes.begin() + 24 + 5);
    result = check(header_cut, ContainerFormat::Cr3);
    ASSERT_TRUE(result);
    ASSERT_EQ(result->overruns.size(), 1u);
    EXPECT_NE(result->overruns[0].detail.find("header"), std::string::npos);
}

TEST(ContainerExtent, Cr3SizeZeroAndLargeSizeBoxes) {
    std::vector<std::byte> bytes;
    append_box(bytes, "ftyp", 24);
    append_box(bytes, "moov", 100, /*large=*/true);
    append_box(bytes, "mdat", 64);
    put_be(bytes, 124, 0, 4);  // mdat size 0: extends to the end of the file
    bytes.resize(bytes.size() + 1000);
    auto result = check(bytes, ContainerFormat::Cr3);
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_TRUE(result->overruns.empty());

    // A 64-bit size beyond the end.
    std::vector<std::byte> large;
    append_box(large, "ftyp", 24);
    append_box(large, "mdat", 64, /*large=*/true);
    put_be(large, 24 + 8, std::uint64_t{1} << 40, 8);
    result = check(large, ContainerFormat::Cr3);
    ASSERT_TRUE(result);
    ASSERT_EQ(result->overruns.size(), 1u);
}

TEST(ContainerExtent, MalformedCr3BoxIsCorrupt) {
    std::vector<std::byte> bytes = cr3_file();
    put_be(bytes, 24, 4, 4);  // moov declares 4 bytes: smaller than its own header
    auto result = check(bytes, ContainerFormat::Cr3);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::Corrupt);
}

// A RAF header: JPEG at 200 (100 bytes), CFA header at 300 (50), CFA data at 400 (600).
std::vector<std::byte> raf_file() {
    std::vector<std::byte> bytes(1000, std::byte{0});
    std::memcpy(bytes.data(), "FUJIFILMCCD-RAW 0201FF383501", 28);
    put_be(bytes, 84, 200, 4);
    put_be(bytes, 88, 100, 4);
    put_be(bytes, 92, 300, 4);
    put_be(bytes, 96, 50, 4);
    put_be(bytes, 100, 400, 4);
    put_be(bytes, 104, 600, 4);
    return bytes;
}

TEST(ContainerExtent, RafRangesMustFit) {
    std::vector<std::byte> bytes = raf_file();
    auto complete = check(bytes, ContainerFormat::Raf);
    ASSERT_TRUE(complete) << complete.error().describe();
    EXPECT_TRUE(complete->overruns.empty());

    bytes.resize(998);
    auto cut = check(bytes, ContainerFormat::Raf);
    ASSERT_TRUE(cut);
    ASSERT_EQ(cut->overruns.size(), 1u);
    EXPECT_EQ(cut->overruns[0].ifd_path, "RAF CFA data");
    EXPECT_EQ(cut->overruns[0].declared_end, 1000u);

    bytes.resize(60);  // the header itself is cut
    auto header = check(bytes, ContainerFormat::Raf);
    ASSERT_TRUE(header);
    ASSERT_EQ(header->overruns.size(), 1u);
    EXPECT_EQ(header->overruns[0].ifd_path, "RAF header");
}

TEST(ContainerExtent, OrfAndRw2AreReadAsTiffStructure) {
    namespace t = tiff_tag;
    ts::Ifd ifd;
    ifd.entries = {ts::long_entry(t::kImageWidth, {16}), ts::long_entry(t::kImageLength, {16}),
                   ts::long_entry(t::kStripOffsets, {1000}), ts::long_entry(t::kStripByteCounts, {500})};
    ts::Document doc;
    doc.chain = {ifd};
    std::vector<std::byte> bytes = ts::build(doc).bytes;
    bytes.resize(1500);
    for (const auto& [magic, container] :
         {std::pair<std::string_view, ContainerFormat>{"IIRO", ContainerFormat::Orf},
          std::pair<std::string_view, ContainerFormat>{"IIRS", ContainerFormat::Orf},
          std::pair<std::string_view, ContainerFormat>{std::string_view("IIU\0", 4), ContainerFormat::Rw2}}) {
        SCOPED_TRACE(std::string(magic.substr(0, 3)));
        std::vector<std::byte> vendor = bytes;
        std::memcpy(vendor.data(), magic.data(), 4);
        auto complete = check(vendor, container);
        ASSERT_TRUE(complete) << complete.error().describe();
        EXPECT_TRUE(complete->overruns.empty());
        // The regression of review finding ROBU-1: an ORF cut by a few bytes.
        vendor.resize(1498);
        auto cut = check(vendor, container);
        ASSERT_TRUE(cut) << cut.error().describe();
        ASSERT_EQ(cut->overruns.size(), 1u);
        EXPECT_EQ(cut->overruns[0].declared_end, 1500u);
        // The DNG inspector's reader still rejects vendor signatures.
        MemorySource source(vendor);
        auto standard = find_image_data_overruns(source);
        ASSERT_FALSE(standard);
        EXPECT_EQ(standard.error().code, ErrorCode::Unsupported);
    }
    // Big-endian ORF ("MMOR").
    ts::Document big = doc;
    big.endian = ts::Endian::Big;
    std::vector<std::byte> mm = ts::build(big).bytes;
    mm.resize(1499);
    std::memcpy(mm.data(), "MMOR", 4);
    auto result = check(mm, ContainerFormat::Orf);
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_EQ(result->overruns.size(), 1u);
}

// An older Panasonic RW2 / Leica RWL in the layout of raw.pixls.us samples 3755 (DMC-FZ70), 4963
// (Leica C (Typ 112)) and 5535 (DMC-TZ60): IFD 0 holds PanasonicRawVersion, the sensor size,
// RawFormat 4, StripOffsets with the placeholder 4294967295 and StripByteCounts 0, and RawDataOffset
// (0x0118) pointing at the raw data, the last bytes of the file (16 bytes per 14 pixels, rounded up
// to whole 16 KiB blocks as the cameras write it).
struct OlderRw2 {
    std::vector<std::byte> bytes;
    std::uint64_t data_offset = 0;
    std::uint64_t data_needed = 0;  // ceil(width x height / 14) x 16
};

OlderRw2 older_rw2(std::uint32_t width, std::uint32_t height, std::optional<std::uint64_t> raw_format = 4,
                   std::uint64_t strip_byte_counts = 0, bool with_raw_offset = true) {
    constexpr std::uint16_t kRawData = 65000;  // private tag holding the raw data bytes
    const std::uint64_t needed = (std::uint64_t{width} * height + 13) / 14 * 16;
    const std::uint64_t stored = (needed + 16383) / 16384 * 16384;
    ts::Ifd ifd;
    ifd.entries = {
        ts::undefined_entry(0x0001, {std::byte{'0'}, std::byte{'2'}, std::byte{'3'}, std::byte{'0'}}),
        ts::short_entry(0x0002, {width}),
        ts::short_entry(0x0003, {height}),
        ts::short_entry(0x0004, {8}),
        ts::short_entry(0x0005, {8}),
        ts::short_entry(0x0006, {height - 8}),
        ts::short_entry(0x0007, {width - 8}),
        ts::long_entry(tiff_tag::kStripOffsets, {0xFFFFFFFFu}),
        ts::short_entry(tiff_tag::kRowsPerStrip, {height}),
        ts::long_entry(tiff_tag::kStripByteCounts, {strip_byte_counts}),
        ts::undefined_entry(kRawData, ts::filler(static_cast<std::size_t>(stored))),
    };
    if (raw_format) {
        ifd.entries.push_back(ts::short_entry(0x002D, {*raw_format}));
    }
    if (with_raw_offset) {
        ifd.entries.push_back(ts::long_entry(0x0118, {0}));  // patched below
    }
    ts::Document doc;
    doc.chain = {ifd};
    ts::Built built = ts::build(doc);
    OlderRw2 out;
    out.data_offset = built.value("ifd0", kRawData);
    out.data_needed = needed;
    if (with_raw_offset) {
        built.put(built.value("ifd0", 0x0118), out.data_offset, 4);
    }
    out.bytes = std::move(built.bytes);
    std::memcpy(out.bytes.data(), "IIU\0", 4);
    EXPECT_EQ(out.data_offset + stored, out.bytes.size()) << "the raw data must be the file's last bytes";
    return out;
}

TEST(ContainerExtent, OlderRw2PlaceholderStripsAreNotTheRawData) {
    // The regression (W12 phase 1, ADR 0020): these files were refused as truncated ("ifd0: strip
    // data ends at byte 4294967295") although LibRaw decodes them from RawDataOffset.
    const OlderRw2 file = older_rw2(4816, 3472);  // the DMC-FZ70's sensor
    auto rw2 = check(file.bytes, ContainerFormat::Rw2);
    ASSERT_TRUE(rw2) << rw2.error().describe();
    EXPECT_TRUE(rw2->overruns.empty()) << rw2->overruns.front().detail;
    EXPECT_TRUE(rw2->warnings.empty());
    // Read with the strip rule (as a TIFF) the placeholder still is an overrun: the rule is the
    // container's, not a relaxation of the strip check.
    MemorySource source(file.bytes);
    auto as_tiff = find_image_data_overruns(source, {}, TiffSignatures::WithVendorRaw);
    ASSERT_TRUE(as_tiff) << as_tiff.error().describe();
    ASSERT_EQ(as_tiff->overruns.size(), 1u);
    EXPECT_EQ(as_tiff->overruns[0].declared_end, 0xFFFFFFFFu);
}

TEST(ContainerExtent, Rw2RawDataMustFitTheFile) {
    const OlderRw2 file = older_rw2(4144, 2688);  // the Leica C (Typ 112): exactly 777 blocks
    ASSERT_EQ(file.data_offset + file.data_needed, file.bytes.size());
    // A copy that lost its last bytes is truncated inside the raw data.
    for (const std::size_t cut : {std::size_t{1}, std::size_t{100000}}) {
        SCOPED_TRACE(cut);
        std::vector<std::byte> truncated(file.bytes.begin(), file.bytes.end() - static_cast<std::ptrdiff_t>(cut));
        auto result = check(truncated, ContainerFormat::Rw2);
        ASSERT_TRUE(result) << result.error().describe();
        ASSERT_EQ(result->overruns.size(), 1u);
        EXPECT_EQ(result->overruns[0].ifd_path, "ifd0");
        EXPECT_EQ(result->overruns[0].declared_end, file.data_offset + file.data_needed);
        EXPECT_NE(result->overruns[0].detail.find("16 bytes per 14 pixels"), std::string::npos)
            << result->overruns[0].detail;
    }
    // Cut before the raw data starts.
    std::vector<std::byte> head(file.bytes.begin(), file.bytes.begin() + static_cast<std::ptrdiff_t>(file.data_offset));
    auto result = check(head, ContainerFormat::Rw2);
    ASSERT_TRUE(result) << result.error().describe();
    ASSERT_EQ(result->overruns.size(), 1u);
    EXPECT_NE(result->overruns[0].detail.find("starts at byte"), std::string::npos) << result->overruns[0].detail;
}

TEST(ContainerExtent, Rw2RawDataLengthFollowsStripByteCountsOrTheEncoding) {
    // A declared StripByteCounts is the raw data's length (LibRaw's data size).
    OlderRw2 declared = older_rw2(64, 32, 4, 100000);
    auto result = check(declared.bytes, ContainerFormat::Rw2);
    ASSERT_TRUE(result) << result.error().describe();
    ASSERT_EQ(result->overruns.size(), 1u);
    EXPECT_EQ(result->overruns[0].declared_end, declared.data_offset + 100000);
    // RawFormat 8 (a variable-rate compression) declares no length: only the start is checked.
    OlderRw2 compressed = older_rw2(4816, 3472, 8);
    compressed.bytes.resize(static_cast<std::size_t>(compressed.data_offset + 1));
    result = check(compressed.bytes, ContainerFormat::Rw2);
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_TRUE(result->overruns.empty());
    // Without RawFormat (the oldest models) the fixed-rate minimum applies.
    OlderRw2 oldest = older_rw2(4816, 3472, std::nullopt);
    oldest.bytes.resize(oldest.bytes.size() - 20000);
    result = check(oldest.bytes, ContainerFormat::Rw2);
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_EQ(result->overruns.size(), 1u);
    // Without RawDataOffset LibRaw reads the strips, and so does the check.
    const OlderRw2 strips = older_rw2(64, 32, 4, 0, false);
    result = check(strips.bytes, ContainerFormat::Rw2);
    ASSERT_TRUE(result) << result.error().describe();
    ASSERT_EQ(result->overruns.size(), 1u);
    EXPECT_EQ(result->overruns[0].declared_end, 0xFFFFFFFFu);
}

TEST(ContainerExtent, ContainersWithoutACheckAreUnsupported) {
    const std::vector<std::byte> bytes(64, std::byte{0});
    for (const ContainerFormat container :
         {ContainerFormat::Unknown, ContainerFormat::Crw, ContainerFormat::X3f}) {
        EXPECT_FALSE(has_structural_check(container));
        auto result = check(bytes, container);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, ErrorCode::Unsupported);
    }
    for (const ContainerFormat container :
         {ContainerFormat::Tiff, ContainerFormat::Dng, ContainerFormat::Orf, ContainerFormat::Rw2,
          ContainerFormat::Cr3, ContainerFormat::Raf}) {
        EXPECT_TRUE(has_structural_check(container));
    }
}

}  // namespace
}  // namespace lucent::raw::detail
