#pragma once

// DNG opcode-list parsing (DNG 1.7.1.0, chapter 7 "Opcode List Processing").
//
// An opcode list is the payload of one of the tags OpcodeList1 (51008), OpcodeList2 (51009) or
// OpcodeList3 (51022) in a raw IFD. The specification states that opcode lists are always
// stored in big-endian byte order, whatever the byte order of the enclosing TIFF file, so that
// utilities can copy them between files without understanding them. Layout (all uint32, BE):
//
//     count
//     repeated `count` times:
//         opcode ID
//         DNG version in which the opcode ID was defined (packed four-tier version)
//         flags            bit 0: optional; bit 1: may be skipped for preview-quality output
//         parameter byte count N
//         N bytes of opcode-specific parameters
//
// This header only identifies opcodes and their requirements; it does not apply them. Callers
// use it to decide whether a file needs processing that the active decoder cannot perform: a
// mandatory opcode that is not applied means the image cannot be reported as verified.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include <lucent/core/error.hpp>
#include <lucent/raw/dng_version.hpp>

namespace lucent::raw {

// Opcode IDs defined by DNG 1.7.1.0 chapter 7. The comment gives the "DNG Version" that the
// specification lists for each opcode. Values are fixed by the specification.
enum class DngOpcodeId : std::uint32_t {
    WarpRectilinear = 1,       // 1.3.0.0
    WarpFisheye = 2,           // 1.3.0.0
    FixVignetteRadial = 3,     // 1.3.0.0
    FixBadPixelsConstant = 4,  // 1.3.0.0
    FixBadPixelsList = 5,      // 1.3.0.0
    TrimBounds = 6,            // 1.3.0.0
    MapTable = 7,              // 1.3.0.0
    MapPolynomial = 8,         // 1.3.0.0
    GainMap = 9,               // 1.3.0.0
    DeltaPerRow = 10,          // 1.3.0.0
    DeltaPerColumn = 11,       // 1.3.0.0
    ScalePerRow = 12,          // 1.3.0.0
    ScalePerColumn = 13,       // 1.3.0.0
    WarpRectilinear2 = 14,     // 1.6.0.0
};

// The three opcode lists, identified by their list number. Each runs at a different stage of
// the DNG processing model (DNG 1.7.1.0, tags OpcodeList1/2/3):
//   List1: raw values exactly as read from the file (before linearization and black subtraction)
//   List2: after mapping to linear reference values (after linearization, black and white level)
//   List3: after demosaicing
enum class DngOpcodeListId : std::uint8_t {
    List1 = 1,
    List2 = 2,
    List3 = 3,
};

// TIFF tag that carries the given opcode list (51008, 51009 or 51022).
[[nodiscard]] constexpr std::uint16_t dng_opcode_list_tag(DngOpcodeListId list) noexcept {
    switch (list) {
        case DngOpcodeListId::List1:
            return 51008;
        case DngOpcodeListId::List2:
            return 51009;
        case DngOpcodeListId::List3:
            return 51022;
    }
    return 0;
}

// True when `id` is one of the opcode IDs defined by DNG 1.7.1.0.
[[nodiscard]] constexpr bool is_known_dng_opcode(std::uint32_t id) noexcept {
    return id >= static_cast<std::uint32_t>(DngOpcodeId::WarpRectilinear) &&
           id <= static_cast<std::uint32_t>(DngOpcodeId::WarpRectilinear2);
}

// Specification name of an opcode ID ("GainMap"), or "Unknown" for IDs this build does not
// recognise. The returned view refers to static storage.
[[nodiscard]] std::string_view dng_opcode_name(std::uint32_t id) noexcept;

// One opcode header from an opcode list. Parameters are not decoded; their location is given
// so that a later processing stage can read them from the same payload.
struct DngOpcode {
    static constexpr std::uint32_t kFlagOptional = 1u << 0;
    static constexpr std::uint32_t kFlagPreviewSkippable = 1u << 1;
    static constexpr std::uint32_t kDefinedFlagsMask = kFlagOptional | kFlagPreviewSkippable;

    DngOpcodeListId list = DngOpcodeListId::List1;

    // Zero-based position of the opcode within its list; opcodes run in this order.
    std::uint32_t index = 0;

    // Opcode ID as stored; may be an ID this build does not know (see is_known()).
    std::uint32_t id = 0;

    // DNG version in which this opcode ID was defined, as written by the file. A reader must
    // not process an opcode whose version is newer than the specification it implements.
    DngVersion min_dng_version;

    // Raw flag word. Bits other than kDefinedFlagsMask are reserved (see has_reserved_flags()).
    std::uint32_t flags = 0;

    // Size of the opcode-specific parameter area in bytes.
    std::uint32_t parameter_bytes = 0;

    // Offset of the parameter area from the first byte of the opcode-list payload.
    std::uint64_t parameter_offset = 0;

    // True for a WarpRectilinear or WarpFisheye opcode that immediately follows an optional
    // WarpRectilinear2 opcode in the same list. DNG 1.7.1.0 chapter 7 requires a reader that
    // processes such a WarpRectilinear2 to skip this opcode; a reader that skips the
    // WarpRectilinear2 applies this one instead.
    bool follows_optional_warp_rectilinear2 = false;

    // Flag bit 0: the reader may decline to apply the opcode, e.g. when it does not know it.
    [[nodiscard]] constexpr bool is_optional() const noexcept { return (flags & kFlagOptional) != 0; }

    // Flag bit 1: the opcode may be skipped for preview-quality output only.
    [[nodiscard]] constexpr bool is_preview_skippable() const noexcept {
        return (flags & kFlagPreviewSkippable) != 0;
    }

    // Not optional: final-quality output that does not apply this opcode is not a faithful
    // rendering of the file.
    [[nodiscard]] constexpr bool is_mandatory() const noexcept { return !is_optional(); }

    [[nodiscard]] constexpr bool is_known() const noexcept { return is_known_dng_opcode(id); }

    [[nodiscard]] constexpr bool has_reserved_flags() const noexcept {
        return (flags & ~kDefinedFlagsMask) != 0;
    }

    [[nodiscard]] std::string_view name() const noexcept { return dng_opcode_name(id); }

    friend bool operator==(const DngOpcode&, const DngOpcode&) = default;
};

// Parses the opcode headers of one opcode-list payload (the bytes of an OpcodeListN tag).
//
// The payload is read as big-endian regardless of the enclosing file's byte order. It must hold
// the 4-byte count and exactly `count` opcodes; a count that exceeds the payload, a parameter
// area that runs past the end, a payload shorter than 4 bytes (including an empty one) and
// trailing bytes after the last opcode all fail with ErrorCode::Corrupt at "raw.dng.opcode_list".
// A structurally valid count above `max_opcodes` fails with ErrorCode::LimitExceeded there, before
// any allocation. An invalid `list` value fails with ErrorCode::InvalidArgument. Memory use is
// proportional to the number of opcodes, which is at most min(max_opcodes, payload.size() / 16).
//
// Thread-safe: a pure function of its arguments.
[[nodiscard]] Expected<std::vector<DngOpcode>> parse_dng_opcode_list(
    std::span<const std::byte> payload, DngOpcodeListId list,
    std::uint32_t max_opcodes = std::numeric_limits<std::uint32_t>::max());

}  // namespace lucent::raw
