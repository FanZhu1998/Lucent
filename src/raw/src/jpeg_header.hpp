#pragma once

// Private: a minimal, bounds-checked walker over the marker segments of a JPEG stream (ITU-T T.81
// Annex B) that finds the frame header (SOFn) without decoding anything. The embedded-preview
// extractor (embedded_preview.hpp) uses it to learn the true dimensions of every listed preview,
// because decoders do not always report them (LibRaw leaves them 0 for several CR3 previews).
//
// Thread-safety: pure functions.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace lucent::raw::detail {

// The first frame header of a JPEG stream.
struct JpegFrame {
    std::uint8_t sof_marker = 0;  // 0xC0..0xCF (excluding 0xC4 DHT, 0xC8 JPG, 0xCC DAC)
    std::uint8_t precision = 0;   // sample precision in bits (P)
    std::uint16_t width = 0;      // samples per line (X)
    std::uint16_t height = 0;     // lines (Y); 0 means "defined later by a DNL marker"
    std::uint8_t components = 0;  // image components in the frame (Nf)
};

enum class JpegScanStatus : std::uint8_t {
    Frame,         // JpegScan::frame holds the first frame header
    NeedMoreData,  // the header structure continues beyond the bytes supplied (only when !complete)
    Invalid,       // not a JPEG stream, or its marker structure is malformed (JpegScan::message)
};

struct JpegScan {
    JpegScanStatus status = JpegScanStatus::Invalid;
    JpegFrame frame;
    std::string message;  // developer-facing English, for Invalid
};

// Walks the marker segments of `bytes`, which must start at the beginning of the stream (SOI),
// up to the first frame header. `complete` states that `bytes` is the whole stream, so running
// out of data is Invalid rather than NeedMoreData. Rules (T.81 B.1.1): fill bytes 0xFF before a
// marker are skipped; RSTm and TEM carry no length; SOS, EOI or a second SOI before a frame
// header, a zero byte where a marker is expected, a segment length below 2 or a frame header
// whose length disagrees with its component count are Invalid. At most 4096 segments are
// walked before the frame header.
[[nodiscard]] JpegScan scan_jpeg_header(std::span<const std::byte> bytes, bool complete);

// Empty when a display decoder can be expected to handle `frame`: baseline, extended sequential
// or progressive Huffman coding (SOF0, SOF1, SOF2), 8-bit samples, 1 or 3 components, and
// non-zero dimensions. Otherwise a short reason, e.g. "lossless JPEG (SOF3)".
[[nodiscard]] std::string displayable_jpeg_problem(const JpegFrame& frame);

}  // namespace lucent::raw::detail
