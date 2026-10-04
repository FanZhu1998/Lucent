#pragma once

// DNG opcode lists (DNG 1.7.1.0, chapter 7 "Opcode List Processing").
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
// This header identifies opcodes and their requirements (parse_dng_opcode_list), decodes the
// parameters of the opcodes Lucent implements (parse_dng_opcode_parameters, "Opcode parameters"
// below) and states which opcodes Lucent's engine applies at which stage (assess_dng_opcode).
// It does not apply them: lucent::engine does (engine/dng_opcode_processing.hpp). A mandatory
// opcode that is not applied means the image cannot be reported as verified.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
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

// The DNG version in which a known opcode ID was defined (1.3.0.0 for IDs 1-13, 1.6.0.0 for
// WarpRectilinear2); std::nullopt for unknown IDs.
[[nodiscard]] std::optional<DngVersion> dng_opcode_defined_version(std::uint32_t id) noexcept;

// ---------------------------------------------------------------------------------------------
// Opcode parameters (DNG 1.7.1.0 chapter 7, one section per opcode)
// ---------------------------------------------------------------------------------------------
//
// Coordinates are pixels of the image the opcode list processes (the "stage image": the whole
// stored raw image for OpcodeList1; the ActiveArea for OpcodeList2 and OpcodeList3, as in the DNG
// SDK's processing model), with (0, 0) at its top-left pixel. Parameters are decoded exactly as
// stored (LONG = uint32, SHORT = uint16, FLOAT = IEEE single, DOUBLE = IEEE double, all
// big-endian); the checks are listed with parse_dng_opcode_parameters.

// Half-open rectangle: rows [top, bottom), columns [left, right).
struct DngOpcodeRect {
    std::uint32_t top = 0;
    std::uint32_t left = 0;
    std::uint32_t bottom = 0;
    std::uint32_t right = 0;

    [[nodiscard]] constexpr bool empty() const noexcept { return bottom <= top || right <= left; }

    friend bool operator==(const DngOpcodeRect&, const DngOpcodeRect&) = default;
};

// The area, plane range and pitch shared by MapTable, MapPolynomial, GainMap and the
// per-row/per-column opcodes: rows top, top + row_pitch, ... below bottom and columns left,
// left + col_pitch, ... below right, in planes plane .. plane + planes - 1. Rows, columns and
// planes outside the image are not affected (as in the DNG SDK); an empty rectangle affects
// nothing.
struct DngOpcodeArea {
    DngOpcodeRect rect;
    std::uint32_t plane = 0;
    std::uint32_t planes = 1;     // >= 1
    std::uint32_t row_pitch = 1;  // >= 1
    std::uint32_t col_pitch = 1;  // >= 1

    // Rows / columns the area affects: ceil(height / row_pitch), ceil(width / col_pitch); 0 for an
    // empty rectangle.
    [[nodiscard]] std::uint64_t affected_rows() const noexcept;
    [[nodiscard]] std::uint64_t affected_columns() const noexcept;

    friend bool operator==(const DngOpcodeArea&, const DngOpcodeArea&) = default;
};

// One coefficient set of a rectilinear warp, written as the WarpRectilinear2 radial polynomial
//     f(r) = sum over p = 0..14 of radial[p] * r^p, evaluated at clamp(r, min_valid_radius,
//                                                                      max_valid_radius),
// with the tangential coefficients {kt0, kt1}. A WarpRectilinear opcode (f = kr0 + kr1 r^2 +
// kr2 r^4 + kr3 r^6) is stored with radial[0, 2, 4, 6] = kr0..kr3 and the valid radius [0, 1],
// which contains every pixel of the image, so the clamp never changes it.
struct DngWarpPlane {
    std::array<double, 15> radial{};
    std::array<double, 2> tangential{};
    double min_valid_radius = 0.0;
    double max_valid_radius = 1.0;

    friend bool operator==(const DngWarpPlane&, const DngWarpPlane&) = default;
};

// WarpRectilinear (ID 1) and WarpRectilinear2 (ID 14).
struct DngWarpRectilinear {
    bool extended = false;  // true: WarpRectilinear2
    // N coefficient sets (1 <= N <= 4): one for all planes, or one per image plane.
    std::vector<DngWarpPlane> planes;
    double center_x = 0.5;  // normalised optical centre (c^x, c^y), each in [0, 1]
    double center_y = 0.5;
    // WarpRectilinear2 reciprocalRadial != 0: the radial term divides by f instead of multiplying.
    bool reciprocal_radial = false;

    friend bool operator==(const DngWarpRectilinear&, const DngWarpRectilinear&) = default;
};

// FixVignetteRadial (ID 3): gain g = 1 + k0 r^2 + k1 r^4 + k2 r^6 + k3 r^8 + k4 r^10.
struct DngFixVignetteRadial {
    std::array<double, 5> k{};
    double center_x = 0.5;  // each in [0, 1]
    double center_y = 0.5;

    friend bool operator==(const DngFixVignetteRadial&, const DngFixVignetteRadial&) = default;
};

// FixBadPixelsConstant (ID 4).
struct DngFixBadPixelsConstant {
    std::uint32_t constant = 0;     // the value that marks a bad pixel
    std::uint32_t bayer_phase = 0;  // 0..3, see DngBayerPhase

    friend bool operator==(const DngFixBadPixelsConstant&, const DngFixBadPixelsConstant&) = default;
};

struct DngBadPoint {
    std::uint32_t row = 0;
    std::uint32_t column = 0;

    friend bool operator==(const DngBadPoint&, const DngBadPoint&) = default;
};

// FixBadPixelsList (ID 5).
struct DngFixBadPixelsList {
    std::uint32_t bayer_phase = 0;  // 0..3
    std::vector<DngBadPoint> points;
    std::vector<DngOpcodeRect> rects;  // each non-empty

    friend bool operator==(const DngFixBadPixelsList&, const DngFixBadPixelsList&) = default;
};

// TrimBounds (ID 6).
struct DngTrimBounds {
    DngOpcodeRect bounds;  // non-empty

    friend bool operator==(const DngTrimBounds&, const DngTrimBounds&) = default;
};

// MapTable (ID 7): 1..65536 16-bit entries; values beyond the table map to its last entry.
struct DngMapTable {
    DngOpcodeArea area;
    std::vector<std::uint16_t> table;

    friend bool operator==(const DngMapTable&, const DngMapTable&) = default;
};

// MapPolynomial (ID 8): Degree + 1 coefficients (Degree <= 8), constant term first.
struct DngMapPolynomial {
    DngOpcodeArea area;
    std::vector<double> coefficients;

    friend bool operator==(const DngMapPolynomial&, const DngMapPolynomial&) = default;
};

// GainMap (ID 9): a points_v x points_h grid of map_planes gains, stored row by row with the
// planes of one point together. Spacing and origin are relative to the height / width of the
// image being modified (1.0 = the whole image).
struct DngGainMap {
    DngOpcodeArea area;
    std::uint32_t points_v = 1;  // >= 1
    std::uint32_t points_h = 1;  // >= 1
    double spacing_v = 0.0;      // > 0 when points_v > 1
    double spacing_h = 0.0;      // > 0 when points_h > 1
    double origin_v = 0.0;
    double origin_h = 0.0;
    std::uint32_t map_planes = 1;  // >= 1
    std::vector<float> gains;      // points_v * points_h * map_planes, finite

    // Gain of grid point (v, h) in map plane p. Precondition: indices within the grid.
    [[nodiscard]] float gain(std::uint32_t v, std::uint32_t h, std::uint32_t p) const noexcept {
        return gains[(static_cast<std::size_t>(v) * points_h + h) * map_planes + p];
    }

    friend bool operator==(const DngGainMap&, const DngGainMap&) = default;
};

// DeltaPerRow / DeltaPerColumn / ScalePerRow / ScalePerColumn (IDs 10-13): one value per
// affected row (or column) of the area, in order.
struct DngRowColumnValues {
    DngOpcodeArea area;
    std::vector<float> values;  // finite

    friend bool operator==(const DngRowColumnValues&, const DngRowColumnValues&) = default;
};

// Decoded parameters of one opcode. std::monostate when they were not decoded: an ID whose
// parameters Lucent does not decode (WarpFisheye, unknown IDs), an opcode whose version is newer
// than kDngSpecificationVersion, or an opcode whose parameters are malformed.
using DngOpcodeParameters =
    std::variant<std::monostate, DngWarpRectilinear, DngFixVignetteRadial, DngFixBadPixelsConstant,
                 DngFixBadPixelsList, DngTrimBounds, DngMapTable, DngMapPolynomial, DngGainMap, DngRowColumnValues>;

// True for the opcode IDs whose parameters parse_dng_opcode_parameters decodes: every ID of
// DNG 1.7.1.0 except WarpFisheye (2).
[[nodiscard]] constexpr bool decodes_dng_opcode_parameters(std::uint32_t id) noexcept {
    return is_known_dng_opcode(id) && id != static_cast<std::uint32_t>(DngOpcodeId::WarpFisheye);
}

// Decodes the parameter area of an opcode with ID `id` (big-endian). For an ID that
// decodes_dng_opcode_parameters() rejects, returns std::monostate. The layout must fill the
// byte count exactly; any violation fails with ErrorCode::Corrupt at "raw.dng.opcode_parameters":
//   WarpRectilinear     LONG N (1..4), N x 6 DOUBLE, 2 DOUBLE centre: 4 + 48 N + 16 bytes
//   WarpRectilinear2    LONG N (1..4), N x (15 radial + 2 tangential + min/max valid radius)
//                       DOUBLE, 2 DOUBLE centre, LONG reciprocalRadial: 4 + 152 N + 20 bytes;
//                       0 <= min_valid_radius < max_valid_radius <= 1
//   FixVignetteRadial   7 DOUBLE (56 bytes)
//   FixBadPixelsConstant  2 LONG (8 bytes); BayerPhase <= 3
//   FixBadPixelsList    3 LONG + 8 per point + 16 per rectangle; BayerPhase <= 3; rectangles
//                       non-empty
//   TrimBounds          4 LONG (16 bytes); non-empty
//   area-based opcodes  8 LONG area (top <= bottom, left <= right; planes, pitches >= 1), then
//     MapTable          LONG TableSize (1..65536), TableSize SHORT
//     MapPolynomial     LONG Degree (0..8), Degree + 1 DOUBLE
//     GainMap           2 LONG points (>= 1), 4 DOUBLE spacing and origin (spacing > 0 where the
//                       points are > 1), LONG MapPlanes (>= 1), points_v x points_h x MapPlanes
//                       FLOAT
//     per row/column    LONG Count (= the area's affected rows or columns), Count FLOAT
// Every DOUBLE and FLOAT must be finite, and normalised centres must lie in [0, 1]. Memory is
// bounded by about the size of `parameters` plus a constant. Thread-safe.
[[nodiscard]] Expected<DngOpcodeParameters> parse_dng_opcode_parameters(std::uint32_t id,
                                                                        std::span<const std::byte> parameters);

// One opcode of an opcode list: its header and, once decode_dng_opcode_parameters() ran (the
// TIFF inspector does), its decoded parameters.
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

    // Decoded parameters (see DngOpcodeParameters for when they stay std::monostate).
    DngOpcodeParameters parameters;

    // Why the parameters could not be decoded: Error::describe() of the Corrupt error of
    // parse_dng_opcode_parameters (where "raw.dng.opcode_parameters"); empty otherwise. Lucent
    // cannot apply such an opcode: a mandatory one leaves the file unsupported, an optional one
    // is skipped.
    std::string parameter_error;

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

    // The opcode's version does not exceed the DNG specification this build implements (chapter
    // 7: "A DNG reader should never attempt to process an opcode with a version higher than DNG
    // specification it was written to support").
    [[nodiscard]] bool version_supported() const noexcept { return min_dng_version <= kDngSpecificationVersion; }

    [[nodiscard]] constexpr bool has_reserved_flags() const noexcept {
        return (flags & ~kDefinedFlagsMask) != 0;
    }

    [[nodiscard]] std::string_view name() const noexcept { return dng_opcode_name(id); }

    friend bool operator==(const DngOpcode&, const DngOpcode&) = default;
};

// Parses the opcode headers of one opcode-list payload (the bytes of an OpcodeListN tag).
// Parameters are not decoded (decode_dng_opcode_parameters does that).
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

// Decodes the parameters of every opcode of `opcodes` (parsed from `payload`, the same list)
// whose ID decodes_dng_opcode_parameters() accepts and whose version_supported(). A malformed
// parameter area (parse_dng_opcode_parameters fails with Corrupt) is recorded in
// DngOpcode::parameter_error and the opcode keeps std::monostate parameters: the opcode cannot be
// applied, which makes a mandatory one an unsupported required operation (the file is never
// verified) without discarding the rest of the DNG's metadata. Other errors (OutOfMemory) are
// returned; InvalidArgument when an opcode's parameter area lies outside `payload`.
// Precondition: `opcodes` came from parse_dng_opcode_list(payload, ...).
[[nodiscard]] Status decode_dng_opcode_parameters(std::span<DngOpcode> opcodes, std::span<const std::byte> payload);

// ---------------------------------------------------------------------------------------------
// What Lucent applies
// ---------------------------------------------------------------------------------------------

// BayerPhase as DNG 1.7.1.0 defines it for FixBadPixelsConstant / FixBadPixelsList, from the
// colours of the 2 x 2 tile at the image's top-left pixel: 0 red (RGGB), 1 green in a green/red
// row (GRBG), 2 green in a green/blue row (GBRG), 3 blue (BGGR).
enum class DngBayerPhase : std::uint8_t {
    Red = 0,
    GreenRedRow = 1,
    GreenBlueRow = 2,
    Blue = 3,
};

// The image one opcode list of a file processes, as far as the support decision needs it.
struct DngOpcodeImage {
    DngOpcodeListId list = DngOpcodeListId::List1;
    std::uint32_t width = 0;   // stage image size in pixels
    std::uint32_t height = 0;
    std::uint32_t planes = 1;  // samples per pixel of the stage image
    bool mosaic = false;       // a CFA image (List1 and List2 of a CFA DNG)
    // Lists 1 and 2 of a 2 x 2 Bayer CFA: the arrangement at the stage image's top-left pixel.
    std::optional<DngBayerPhase> bayer_phase;
    // List 1 only: the stored samples are 16-bit integers (floating-point raw data is not
    // processed in List 1).
    bool integer_samples = true;
    // Empty when Lucent can run this list on the file at all; otherwise why not (for example the
    // decoder already linearised the stored values, so List 1 can no longer see them).
    std::string list_unavailable;
};

struct DngOpcodeSupport {
    bool supported = false;
    std::string reason;  // developer-facing English when not supported; empty otherwise
};

// Whether Lucent's engine applies `opcode` to `image` (the stage and kind table of ADR 0014):
//   List 1 (16-bit stored samples): FixBadPixelsConstant / FixBadPixelsList (2 x 2 Bayer CFA
//           whose green sites agree with BayerPhase), FixVignetteRadial, MapTable,
//           MapPolynomial, GainMap, Delta/ScalePerRow/Column.
//   List 2: FixBadPixelsList (Bayer, as above), FixVignetteRadial, TrimBounds, MapTable,
//           MapPolynomial, GainMap, Delta/ScalePerRow/Column.
//   List 3: WarpRectilinear, WarpRectilinear2 (coefficient sets 1 or `planes`), FixVignetteRadial,
//           TrimBounds (inside the image), MapTable, MapPolynomial, GainMap,
//           Delta/ScalePerRow/Column.
// Never supported: unknown IDs, versions newer than kDngSpecificationVersion, parameters that
// were not decoded, WarpFisheye, anything when image.list_unavailable is set. A TrimBounds
// rectangle must lie inside the stage image.
[[nodiscard]] DngOpcodeSupport assess_dng_opcode(const DngOpcode& opcode, const DngOpcodeImage& image);

}  // namespace lucent::raw
