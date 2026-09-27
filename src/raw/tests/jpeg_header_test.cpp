// Unit tests of the private JPEG marker walker (src/jpeg_header.hpp) used by the embedded-preview
// extractor: real streams, every prefix of them, and malformed marker structures.

#include "jpeg_header.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "test_jpegs.hpp"

namespace lucent::raw::detail {
namespace {

namespace ts = test_support;

std::vector<std::byte> bytes_of(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    for (const int v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

// SOI + one APP0-like segment + a frame header for `components` components, as a prefix.
std::vector<std::byte> minimal_header(std::uint8_t sof, std::uint8_t precision, std::uint16_t height,
                                      std::uint16_t width, std::uint8_t components) {
    std::vector<std::byte> out = bytes_of({0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x12, 0x34});
    const std::uint16_t length = static_cast<std::uint16_t>(8 + 3 * components);
    for (const int v : {0xFF, int{sof}, length >> 8, length & 0xFF, int{precision}, height >> 8,
                        height & 0xFF, width >> 8, width & 0xFF, int{components}}) {
        out.push_back(static_cast<std::byte>(v));
    }
    for (std::uint8_t c = 0; c < components; ++c) {
        for (const int v : {c + 1, 0x11, 0}) {
            out.push_back(static_cast<std::byte>(v));
        }
    }
    return out;
}

TEST(JpegHeader, FindsTheFrameHeaderOfRealStreams) {
    struct Case {
        std::vector<std::byte> bytes;
        std::uint8_t sof;
        std::uint16_t width;
        std::uint16_t height;
        std::uint8_t components;
    };
    const std::vector<Case> cases = {
        {ts::jpeg_bytes(ts::kJpegRgb48x32), 0xC0, 48, 32, 3},
        {ts::jpeg_bytes(ts::kJpegRgb96x64), 0xC0, 96, 64, 3},
        {ts::jpeg_bytes(ts::kJpegGray40x24), 0xC0, 40, 24, 1},
        {ts::jpeg_bytes(ts::kJpegProgressive32x16), 0xC2, 32, 16, 3},
    };
    for (const Case& c : cases) {
        const JpegScan scan = scan_jpeg_header(c.bytes, true);
        ASSERT_EQ(scan.status, JpegScanStatus::Frame) << scan.message;
        EXPECT_EQ(scan.frame.sof_marker, c.sof);
        EXPECT_EQ(scan.frame.precision, 8);
        EXPECT_EQ(scan.frame.width, c.width);
        EXPECT_EQ(scan.frame.height, c.height);
        EXPECT_EQ(scan.frame.components, c.components);
        EXPECT_TRUE(displayable_jpeg_problem(scan.frame).empty());
    }
}

TEST(JpegHeader, EveryPrefixNeedsMoreDataUntilTheFrameHeaderIsComplete) {
    const std::vector<std::byte> jpeg = ts::jpeg_bytes(ts::kJpegRgb48x32);
    bool found = false;
    for (std::size_t n = 0; n <= jpeg.size(); ++n) {
        const std::span<const std::byte> prefix(jpeg.data(), n);
        const JpegScan open_ended = scan_jpeg_header(prefix, false);
        ASSERT_NE(open_ended.status, JpegScanStatus::Invalid) << "prefix " << n << ": " << open_ended.message;
        if (open_ended.status == JpegScanStatus::Frame) {
            found = true;
            EXPECT_EQ(open_ended.frame.width, 48);
        } else {
            ASSERT_FALSE(found) << "a longer prefix lost the frame header";
            // The same prefix declared complete is a truncated stream.
            EXPECT_EQ(scan_jpeg_header(prefix, true).status, JpegScanStatus::Invalid) << "prefix " << n;
        }
    }
    EXPECT_TRUE(found);
}

TEST(JpegHeader, SkipsFillBytesAndStandaloneMarkers) {
    // SOI, fill bytes, TEM, RST0, then the frame header.
    std::vector<std::byte> bytes = bytes_of({0xFF, 0xD8, 0xFF, 0xFF, 0xFF, 0x01, 0xFF, 0xD0});
    const std::vector<std::byte> header = minimal_header(0xC1, 8, 20, 30, 3);
    bytes.insert(bytes.end(), header.begin() + 2, header.end());
    const JpegScan scan = scan_jpeg_header(bytes, true);
    ASSERT_EQ(scan.status, JpegScanStatus::Frame) << scan.message;
    EXPECT_EQ(scan.frame.sof_marker, 0xC1);
    EXPECT_EQ(scan.frame.width, 30);
    EXPECT_EQ(scan.frame.height, 20);
}

TEST(JpegHeader, RejectsMalformedStructures) {
    // Not a JPEG at all.
    EXPECT_EQ(scan_jpeg_header(bytes_of({0x49, 0x49, 0x2A, 0x00}), true).status, JpegScanStatus::Invalid);
    // SOI but the next byte is not a marker.
    EXPECT_EQ(scan_jpeg_header(bytes_of({0xFF, 0xD8, 0x12}), true).status, JpegScanStatus::Invalid);
    // Scan data (SOS) before any frame header.
    EXPECT_EQ(scan_jpeg_header(bytes_of({0xFF, 0xD8, 0xFF, 0xDA, 0x00, 0x08}), false).status,
              JpegScanStatus::Invalid);
    // EOI before any frame header.
    EXPECT_EQ(scan_jpeg_header(bytes_of({0xFF, 0xD8, 0xFF, 0xD9}), false).status, JpegScanStatus::Invalid);
    // A second SOI.
    EXPECT_EQ(scan_jpeg_header(bytes_of({0xFF, 0xD8, 0xFF, 0xD8}), false).status, JpegScanStatus::Invalid);
    // A stuffed zero where a marker is expected.
    EXPECT_EQ(scan_jpeg_header(bytes_of({0xFF, 0xD8, 0xFF, 0x00}), false).status, JpegScanStatus::Invalid);
    // A segment length below 2.
    EXPECT_EQ(scan_jpeg_header(bytes_of({0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x01}), false).status,
              JpegScanStatus::Invalid);
    // A frame header whose length disagrees with its component count.
    std::vector<std::byte> header = minimal_header(0xC0, 8, 10, 10, 3);
    header[11] = std::byte{0x0C};  // length low byte: 12 instead of 17
    EXPECT_EQ(scan_jpeg_header(header, false).status, JpegScanStatus::Invalid);
    // A frame header shorter than its fixed part.
    EXPECT_EQ(
        scan_jpeg_header(bytes_of({0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x05, 0x08, 0x00, 0x10}), false).status,
        JpegScanStatus::Invalid);
}

TEST(JpegHeader, BoundsTheNumberOfSegments) {
    std::vector<std::byte> bytes = bytes_of({0xFF, 0xD8});
    for (int i = 0; i < 5000; ++i) {
        for (const int v : {0xFF, 0xFE, 0x00, 0x02}) {  // empty COM segments
            bytes.push_back(static_cast<std::byte>(v));
        }
    }
    const JpegScan scan = scan_jpeg_header(bytes, true);
    EXPECT_EQ(scan.status, JpegScanStatus::Invalid);
    EXPECT_NE(scan.message.find("segments"), std::string::npos) << scan.message;
}

TEST(JpegHeader, ClassifiesDisplayableCodingProcesses) {
    const auto problem = [](std::uint8_t sof, std::uint8_t precision, std::uint16_t h, std::uint16_t w,
                            std::uint8_t n) {
        const JpegScan scan = scan_jpeg_header(minimal_header(sof, precision, h, w, n), false);
        EXPECT_EQ(scan.status, JpegScanStatus::Frame) << scan.message;
        return displayable_jpeg_problem(scan.frame);
    };
    EXPECT_TRUE(problem(0xC0, 8, 16, 16, 3).empty());
    EXPECT_TRUE(problem(0xC1, 8, 16, 16, 1).empty());
    EXPECT_TRUE(problem(0xC2, 8, 16, 16, 3).empty());
    EXPECT_NE(problem(0xC3, 8, 16, 16, 3).find("lossless"), std::string::npos);
    EXPECT_NE(problem(0xC9, 8, 16, 16, 3).find("arithmetic"), std::string::npos);
    EXPECT_NE(problem(0xC5, 8, 16, 16, 3).find("SOF5"), std::string::npos);
    EXPECT_NE(problem(0xC0, 12, 16, 16, 3).find("12-bit"), std::string::npos);
    EXPECT_NE(problem(0xC0, 8, 16, 16, 4).find("4 JPEG components"), std::string::npos);
    EXPECT_NE(problem(0xC0, 8, 0, 16, 3).find("dimensions"), std::string::npos);
}

}  // namespace
}  // namespace lucent::raw::detail
