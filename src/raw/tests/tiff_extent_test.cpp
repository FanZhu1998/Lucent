// Unit tests for the private image-data extent check (src/tiff_extent.hpp).

#include "tiff_extent.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/raw/dng_tags.hpp>
#include <lucent/raw/tiff_inspector.hpp>

#include "tiff_test_writer.hpp"

namespace lucent::raw::detail {
namespace {

namespace ts = test_support;
namespace t = tiff_tag;

constexpr std::uint16_t kTileOffsets = 324;
constexpr std::uint16_t kTileByteCounts = 325;

Expected<ImageDataExtent> scan(const std::vector<std::byte>& bytes) {
    MemorySource source(bytes);
    return find_image_data_overruns(source);
}

// A classic TIFF whose IFD 0 declares two strips at explicit offsets, padded to `file_size`.
std::vector<std::byte> strips_file(std::uint64_t first, std::uint64_t second, std::uint64_t count,
                                   std::size_t file_size, ts::Endian endian = ts::Endian::Little) {
    ts::Ifd ifd;
    ifd.entries = {ts::long_entry(t::kImageWidth, {16}), ts::long_entry(t::kImageLength, {16}),
                   ts::long_entry(t::kStripOffsets, {first, second}),
                   ts::long_entry(t::kStripByteCounts, {count, count})};
    ts::Document doc;
    doc.endian = endian;
    doc.chain = {ifd};
    std::vector<std::byte> bytes = ts::build(doc).bytes;
    bytes.resize(std::max(bytes.size(), file_size));
    return bytes;
}

TEST(TiffImageExtent, DataInsideTheFileIsComplete) {
    for (const ts::Endian endian : {ts::Endian::Little, ts::Endian::Big}) {
        auto result = scan(strips_file(1000, 1500, 500, 2000, endian));
        ASSERT_TRUE(result) << result.error().describe();
        EXPECT_TRUE(result->overruns.empty());
    }
}

TEST(TiffImageExtent, StripBeyondTheEndIsReported) {
    auto result = scan(strips_file(1000, 1500, 500, 1999));
    ASSERT_TRUE(result) << result.error().describe();
    ASSERT_EQ(result->overruns.size(), 1u);
    EXPECT_EQ(result->overruns[0].ifd_path, "ifd0");
    EXPECT_EQ(result->overruns[0].declared_end, 2000u);
    EXPECT_NE(result->overruns[0].detail.find("strip data ends at byte 2000"), std::string::npos);
}

TEST(TiffImageExtent, OverflowingStripIsReported) {
    auto result = scan(strips_file(0xFFFFFFF0u, 0, 0x100, 4096));
    ASSERT_TRUE(result) << result.error().describe();
    ASSERT_EQ(result->overruns.size(), 1u);
}

TEST(TiffImageExtent, TilesInSubIfdsAreChecked) {
    ts::Ifd raw;
    raw.entries = {ts::long_entry(t::kImageWidth, {32}), ts::long_entry(t::kImageLength, {32}),
                   ts::long_entry(kTileOffsets, {3000, 3100, 3200, 3300}),
                   ts::long_entry(kTileByteCounts, {100, 100, 100, 100})};
    ts::Ifd ifd0;
    ifd0.entries = {ts::long_entry(t::kImageWidth, {8}), ts::long_entry(t::kImageLength, {8})};
    ifd0.sub_ifds = {raw};
    ts::Document doc;
    doc.chain = {ifd0};
    std::vector<std::byte> bytes = ts::build(doc).bytes;

    bytes.resize(3400);
    auto complete = scan(bytes);
    ASSERT_TRUE(complete) << complete.error().describe();
    EXPECT_TRUE(complete->overruns.empty());

    bytes.resize(3350);
    auto truncated = scan(bytes);
    ASSERT_TRUE(truncated) << truncated.error().describe();
    ASSERT_EQ(truncated->overruns.size(), 1u);
    EXPECT_EQ(truncated->overruns[0].ifd_path, "ifd0.subifd0");
    EXPECT_EQ(truncated->overruns[0].declared_end, 3400u);
    EXPECT_NE(truncated->overruns[0].detail.find("tile"), std::string::npos);
}

TEST(TiffImageExtent, StripTableBeyondTheEndIsReported) {
    ts::Ifd ifd;
    ifd.entries = {ts::long_entry(t::kImageWidth, {16}), ts::long_entry(t::kImageLength, {16}),
                   ts::long_entry(t::kStripOffsets, {100, 200, 300}),
                   ts::long_entry(t::kStripByteCounts, {10, 10, 10})};
    // Point the StripOffsets table (12 bytes, out of line) past the end of the file.
    ifd.entries[2].offset_override = 1'000'000;
    ts::Document doc;
    doc.chain = {ifd};
    auto result = scan(ts::build(doc).bytes);
    ASSERT_TRUE(result) << result.error().describe();
    ASSERT_EQ(result->overruns.size(), 1u);
    EXPECT_NE(result->overruns[0].detail.find("tables lie beyond"), std::string::npos);
}

TEST(TiffImageExtent, NonTiffDataIsUnsupported) {
    const std::vector<std::byte> text(64, std::byte{'x'});
    auto result = scan(text);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::Unsupported);
}

TEST(TiffImageExtent, IfdLoopsTerminate) {
    ts::Document doc;
    doc.chain = {ts::Ifd{{ts::long_entry(t::kImageWidth, {1})}},
                 ts::Ifd{{ts::long_entry(t::kImageWidth, {1})}}};
    ts::Built built = ts::build(doc);
    // Make IFD 1 point back to IFD 0.
    built.put(built.next_field("ifd1"), built.ifd("ifd0"), 4);
    auto result = scan(built.bytes);
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_TRUE(result->overruns.empty());
}

// A two-IFD chain (IFD 0 with complete strip data, IFD 1 a small thumbnail IFD) for the chain tests.
ts::Built chained_file() {
    ts::Ifd ifd0;
    ifd0.entries = {ts::long_entry(t::kImageWidth, {16}), ts::long_entry(t::kImageLength, {16}),
                    ts::long_entry(t::kStripOffsets, {8}), ts::long_entry(t::kStripByteCounts, {8})};
    ts::Ifd ifd1;
    ifd1.entries = {ts::long_entry(t::kImageWidth, {4}), ts::long_entry(t::kImageLength, {4})};
    ts::Document doc;
    doc.chain = {ifd0, ifd1};
    return ts::build(doc);
}

TEST(TiffImageExtent, ChainIfdBeyondTheEndIsAnOverrun) {
    ts::Built built = chained_file();
    built.put(built.next_field("ifd0"), 1'000'000, 4);
    auto result = scan(built.bytes);
    ASSERT_TRUE(result) << result.error().describe();
    ASSERT_EQ(result->overruns.size(), 1u);
    EXPECT_EQ(result->overruns[0].ifd_path, "ifd1");
    EXPECT_TRUE(result->warnings.empty());
}

TEST(TiffImageExtent, ChainIfdCutInsideItsTableIsAnOverrun) {
    const ts::Built built = chained_file();
    // Keep IFD 1's entry count and half of its first entry: the table is cut off.
    std::vector<std::byte> bytes = built.bytes;
    bytes.resize(static_cast<std::size_t>(built.ifd("ifd1")) + 2 + 6);
    auto result = scan(bytes);
    ASSERT_TRUE(result) << result.error().describe();
    ASSERT_EQ(result->overruns.size(), 1u);
    EXPECT_EQ(result->overruns[0].ifd_path, "ifd1");
}

TEST(TiffImageExtent, MalformedChainIfdInsideTheFileIsOnlyAWarning) {
    // A writer defect, not truncation: IFD 1 lies inside the file but declares no entries (or an
    // implausible number of them). The chain ends there and the file is not called truncated.
    for (const std::uint64_t count : {std::uint64_t{0}, std::uint64_t{0xFFFF}}) {
        SCOPED_TRACE(count);
        ts::Built built = chained_file();
        built.put(built.ifd("ifd1"), count, 2);
        auto result = scan(built.bytes);
        ASSERT_TRUE(result) << result.error().describe();
        EXPECT_TRUE(result->overruns.empty());
        ASSERT_EQ(result->warnings.size(), 1u);
        EXPECT_NE(result->warnings[0].find("ifd1"), std::string::npos) << result->warnings[0];
    }
}

TEST(TiffImageExtent, MalformedFirstIfdIsAnError) {
    ts::Built built = chained_file();
    built.put(built.ifd("ifd0"), 0, 2);
    auto result = scan(built.bytes);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::Corrupt);
}

}  // namespace
}  // namespace lucent::raw::detail

namespace lucent::raw::detail {
namespace {

namespace ts = test_support;
namespace t = tiff_tag;

// IFD 0 (a small preview with complete strips) with two raw SubIFDs whose strips fill the file.
ts::Built subifd_file() {
    ts::Ifd raw0;
    raw0.entries = {ts::long_entry(t::kImageWidth, {16}), ts::long_entry(t::kImageLength, {16}),
                    ts::long_entry(t::kStripOffsets, {2000}), ts::long_entry(t::kStripByteCounts, {500})};
    ts::Ifd raw1 = raw0;
    raw1.entries[2] = ts::long_entry(t::kStripOffsets, {2500});
    ts::Ifd ifd0;
    ifd0.entries = {ts::long_entry(t::kImageWidth, {8}), ts::long_entry(t::kImageLength, {8}),
                    ts::long_entry(t::kStripOffsets, {8}), ts::long_entry(t::kStripByteCounts, {8})};
    ifd0.sub_ifds = {raw0, raw1};
    ts::Document doc;
    doc.chain = {ifd0};
    ts::Built built = ts::build(doc);
    built.bytes.resize(3000);
    return built;
}

TEST(TiffImageExtentReview, FirstIfdBeyondTheEndIsAnOverrun) {
    // A DNG that stores its IFDs after the image data, cut before IFD 0 (review finding ROBU-4).
    ts::Built built = subifd_file();
    built.put(4, 1'000'000, 4);  // header: first IFD offset
    auto result = scan(built.bytes);
    ASSERT_TRUE(result) << result.error().describe();
    ASSERT_EQ(result->overruns.size(), 1u);
    EXPECT_EQ(result->overruns[0].ifd_path, "ifd0");
}

TEST(TiffImageExtentReview, SubIfdBeyondTheEndIsAnOverrunAndOthersAreStillChecked) {
    ts::Built built = subifd_file();
    // SubIFD 0 now points past the end; SubIFD 1 still declares strips beyond a shortened file.
    const std::uint64_t list = built.value("ifd0", t::kSubIfds);
    built.put(list, 1'000'000, 4);
    std::vector<std::byte> bytes = built.bytes;
    bytes.resize(2900);
    auto result = scan(bytes);
    ASSERT_TRUE(result) << result.error().describe();
    ASSERT_EQ(result->overruns.size(), 2u);
    EXPECT_EQ(result->overruns[0].ifd_path, "ifd0.subifd0");
    EXPECT_EQ(result->overruns[1].ifd_path, "ifd0.subifd1");
}

TEST(TiffImageExtentReview, MalformedSubIfdIsSkippedAndEarlierOverrunsAreKept) {
    ts::Built built = subifd_file();
    built.put(built.ifd("ifd0.subifd1"), 0, 2);  // SubIFD 1 declares no entries: a writer defect
    std::vector<std::byte> bytes = built.bytes;
    bytes.resize(2400);  // SubIFD 0's strip (2000..2500) is cut
    auto result = scan(bytes);
    ASSERT_TRUE(result) << result.error().describe();
    ASSERT_EQ(result->overruns.size(), 1u);
    EXPECT_EQ(result->overruns[0].ifd_path, "ifd0.subifd0");
    ASSERT_EQ(result->warnings.size(), 1u);
    EXPECT_NE(result->warnings[0].find("ifd0.subifd1"), std::string::npos) << result->warnings[0];
}

TEST(TiffImageExtentReview, UninterpretableStripTableIsAWarningNotAnAbort) {
    ts::Built built = subifd_file();
    // SubIFD 0's StripOffsets becomes type ASCII; SubIFD 1's strip is still checked.
    built.put(built.entry("ifd0.subifd0", t::kStripOffsets) + 2, 2, 2);
    std::vector<std::byte> bytes = built.bytes;
    bytes.resize(2900);
    auto result = scan(bytes);
    ASSERT_TRUE(result) << result.error().describe();
    ASSERT_EQ(result->overruns.size(), 1u);
    EXPECT_EQ(result->overruns[0].ifd_path, "ifd0.subifd1");
    ASSERT_EQ(result->warnings.size(), 1u);
    EXPECT_NE(result->warnings[0].find("not verified"), std::string::npos) << result->warnings[0];
}

TEST(TiffImageExtentReview, HugeSubIfdListIsBoundedByTheIfdLimit) {
    // Review finding ROBU-3: a SubIFDs count of millions must not be read or queued. Only the
    // first max_ifds pointers are visited; the others are reported as not verified.
    ts::Built built = subifd_file();
    const std::uint64_t entry = built.entry("ifd0", t::kSubIfds);
    built.put(entry + 4, 3'702'016, 4);  // count; the pointer table stays where it was
    std::vector<std::byte> bytes = built.bytes;
    bytes.resize(3'702'016 * 4 + 4096);  // large enough for the declared table to be in bounds
    TiffInspectLimits limits;
    limits.max_ifds = 8;
    MemorySource source(bytes);
    auto result = find_image_data_overruns(source, limits);
    ASSERT_TRUE(result) << result.error().describe();
    bool bounded = false;
    for (const std::string& warning : result->warnings) {
        bounded = bounded || warning.find("only the first 8") != std::string::npos;
    }
    EXPECT_TRUE(bounded);
}

}  // namespace
}  // namespace lucent::raw::detail
