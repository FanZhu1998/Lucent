#include "jpeg_header.hpp"

#include <format>
#include <utility>

namespace lucent::raw::detail {

namespace {

constexpr std::uint8_t kMarkerPrefix = 0xFF;
constexpr std::uint8_t kSoi = 0xD8;
constexpr std::uint8_t kEoi = 0xD9;
constexpr std::uint8_t kSos = 0xDA;
constexpr std::uint8_t kTem = 0x01;
constexpr std::size_t kMaxSegments = 4096;

// Minimum frame-header payload: P (1), Y (2), X (2), Nf (1) after the 2-byte length.
constexpr std::size_t kFrameHeaderFixedBytes = 8;

bool is_frame_marker(std::uint8_t marker) noexcept {
    return marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
}

bool is_standalone_marker(std::uint8_t marker) noexcept {
    return marker == kTem || (marker >= 0xD0 && marker <= 0xD7);
}

std::uint8_t byte_at(std::span<const std::byte> bytes, std::size_t i) noexcept {
    return static_cast<std::uint8_t>(bytes[i]);
}

std::uint16_t be16(std::span<const std::byte> bytes, std::size_t i) noexcept {
    return static_cast<std::uint16_t>((byte_at(bytes, i) << 8) | byte_at(bytes, i + 1));
}

JpegScan invalid(std::string message) {
    JpegScan scan;
    scan.status = JpegScanStatus::Invalid;
    scan.message = std::move(message);
    return scan;
}

// Running out of bytes: the caller may supply more unless the stream is complete.
JpegScan out_of_data(bool complete, std::size_t offset) {
    if (complete) {
        return invalid(std::format("the stream ends at byte {} before a frame header", offset));
    }
    JpegScan scan;
    scan.status = JpegScanStatus::NeedMoreData;
    return scan;
}

}  // namespace

JpegScan scan_jpeg_header(std::span<const std::byte> bytes, bool complete) {
    if (bytes.size() < 2) {
        return out_of_data(complete, bytes.size());
    }
    if (byte_at(bytes, 0) != kMarkerPrefix || byte_at(bytes, 1) != kSoi) {
        return invalid("the stream does not start with a JPEG SOI marker (FF D8)");
    }
    std::size_t pos = 2;
    for (std::size_t segments = 0; segments < kMaxSegments; ++segments) {
        if (pos >= bytes.size()) {
            return out_of_data(complete, pos);
        }
        if (byte_at(bytes, pos) != kMarkerPrefix) {
            return invalid(
                std::format("expected a marker at byte {}, found 0x{:02X}", pos, byte_at(bytes, pos)));
        }
        while (pos < bytes.size() && byte_at(bytes, pos) == kMarkerPrefix) {
            ++pos;  // the marker prefix and any fill bytes
        }
        if (pos >= bytes.size()) {
            return out_of_data(complete, pos);
        }
        const std::uint8_t marker = byte_at(bytes, pos);
        const std::size_t marker_offset = pos - 1;
        ++pos;
        if (marker == 0x00) {
            return invalid(std::format("a stuffed zero byte appears where a marker is expected (byte {})",
                                       marker_offset));
        }
        if (marker == kSoi) {
            return invalid(std::format("a second SOI marker appears at byte {}", marker_offset));
        }
        if (marker == kEoi || marker == kSos) {
            return invalid(
                std::format("marker 0x{:02X} at byte {} precedes the frame header", marker, marker_offset));
        }
        if (is_standalone_marker(marker)) {
            continue;
        }
        if (pos + 2 > bytes.size()) {
            return out_of_data(complete, pos);
        }
        const std::size_t length = be16(bytes, pos);
        if (length < 2) {
            return invalid(std::format("marker 0x{:02X} at byte {} has the invalid segment length {}", marker,
                                       marker_offset, length));
        }
        if (is_frame_marker(marker)) {
            if (length < kFrameHeaderFixedBytes) {
                return invalid(std::format("the frame header at byte {} is too short ({} bytes)",
                                           marker_offset, length));
            }
            if (pos + kFrameHeaderFixedBytes > bytes.size()) {
                return out_of_data(complete, pos);
            }
            JpegScan scan;
            scan.frame.sof_marker = marker;
            scan.frame.precision = byte_at(bytes, pos + 2);
            scan.frame.height = be16(bytes, pos + 3);
            scan.frame.width = be16(bytes, pos + 5);
            scan.frame.components = byte_at(bytes, pos + 7);
            if (length != kFrameHeaderFixedBytes + 3 * std::size_t{scan.frame.components}) {
                return invalid(std::format("the frame header at byte {} has length {} for {} components",
                                           marker_offset, length, scan.frame.components));
            }
            scan.status = JpegScanStatus::Frame;
            return scan;
        }
        pos += length;
    }
    return invalid(std::format("more than {} marker segments precede the frame header", kMaxSegments));
}

std::string displayable_jpeg_problem(const JpegFrame& frame) {
    switch (frame.sof_marker) {
        case 0xC0:
        case 0xC1:
        case 0xC2:
            break;
        case 0xC3:
            return "lossless JPEG (SOF3)";
        case 0xC9:
        case 0xCA:
        case 0xCB:
            return std::format("arithmetic-coded JPEG (SOF{})", frame.sof_marker - 0xC0);
        default:
            return std::format("JPEG coding process SOF{} is not displayable", frame.sof_marker - 0xC0);
    }
    if (frame.precision != 8) {
        return std::format("{}-bit JPEG samples", frame.precision);
    }
    if (frame.components != 1 && frame.components != 3) {
        return std::format("{} JPEG components", frame.components);
    }
    if (frame.width == 0 || frame.height == 0) {
        return std::format("JPEG frame dimensions {} x {}", frame.width, frame.height);
    }
    return {};
}

}  // namespace lucent::raw::detail
