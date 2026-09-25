// Unit tests for the private container completeness check (src/container_extent.hpp) on synthetic
// CR3 box sequences, RAF headers and TIFF-structured vendor containers.

#include "container_extent.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
