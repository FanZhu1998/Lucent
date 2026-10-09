#pragma once

// Bounds-checked TIFF / BigTIFF / DNG metadata inspection.
//
// inspect_tiff() walks the IFD structure of a TIFF-based file (IFD0 chain, SubIFD trees and the
// Exif IFD) and summarises the image-structure, DNG raw-IFD, colour-profile and opcode-list tags
// that decide how a DNG must be interpreted. It never decodes pixels and never trusts a stored
// offset or count: every read is checked against the source size with overflow-safe arithmetic,
// the walk is bounded by TiffInspectLimits, and IFD loops are detected.
//
// Validation policy
//   * A tag that Lucent interprets and that is present but malformed (wrong field type, wrong
//     count, value outside the source, zero denominator, non-finite number, inconsistent with
//     a related tag) fails the inspection with ErrorCode::Corrupt: such a file cannot be
//     interpreted faithfully, and pretending otherwise would let it become a "verified" image.
//   * A tag that is absent is reported as absent (std::nullopt / empty). Deciding whether its
//     absence is acceptable belongs to the caller (for example the decoder adapter's
//     capability check), which sees exactly what the file contains.
//   * An entry with a field type that TIFF does not define is skipped, as TIFF 6.0 requires of
//     readers, and recorded in TiffSummary::warnings. So is an entry that Lucent does not
//     interpret and whose value lies outside the source.
//
// The DNG semantics follow the DNG 1.7.1.0 specification (kDngSpecificationVersion).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/core/error.hpp>
#include <lucent/raw/dng_opcodes.hpp>
#include <lucent/raw/dng_version.hpp>

namespace lucent::raw {

// ---------------------------------------------------------------------------------------------
// Byte sources
// ---------------------------------------------------------------------------------------------

// Random-access, read-only view of the bytes of one source file or buffer.
//
// Contract for implementations:
//   * size() is constant for the lifetime of the object.
//   * read_at(offset, destination) copies bytes starting at `offset` and returns how many were
//     copied, which is exactly min(destination.size(), size() - offset) when offset < size(),
//     and 0 when offset >= size(). A short count therefore only ever means "end of source".
//   * Failures (I/O errors, a file that changed while being read) are returned as an Error;
//     implementations do not throw.
//   * Thread-safety is implementation-defined; callers that share one source between threads
//     must check the implementation. The inspector uses a source from a single thread.
class RandomAccessSource {
public:
    virtual ~RandomAccessSource() = default;

    RandomAccessSource(const RandomAccessSource&) = delete;
    RandomAccessSource& operator=(const RandomAccessSource&) = delete;

    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;

    [[nodiscard]] virtual Expected<std::size_t> read_at(std::uint64_t offset,
                                                        std::span<std::byte> destination) = 0;

protected:
    RandomAccessSource() = default;
    RandomAccessSource(RandomAccessSource&&) = default;
    RandomAccessSource& operator=(RandomAccessSource&&) = default;
};

// RandomAccessSource over caller-owned memory. The memory is neither copied nor modified and
// must outlive the source. read_at never fails. Safe for concurrent use from several threads.
class MemorySource final : public RandomAccessSource {
public:
    explicit MemorySource(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] std::uint64_t size() const noexcept override { return bytes_.size(); }

    [[nodiscard]] Expected<std::size_t> read_at(std::uint64_t offset,
                                                std::span<std::byte> destination) override;

private:
    std::span<const std::byte> bytes_;
};

// ---------------------------------------------------------------------------------------------
// Inspection limits
// ---------------------------------------------------------------------------------------------

// Bounds on the work and memory of one inspection. Conforming camera files stay far below the
// defaults. Exceeding a structural bound (IFD count, nesting depth, entries per IFD) is reported
// as ErrorCode::Corrupt because no plausible camera file approaches them. Exceeding a value
// budget (value bytes, opcodes per list) is reported as ErrorCode::LimitExceeded because large
// values, such as long bad-pixel lists, are legitimate but must not be loaded without bound.
//
// Memory accounting: every tag value that is read is charged its in-memory size, which is the
// larger of its stored byte size and 8 bytes per decoded number. An opcode list is charged twice
// its byte size: the payload read, and the decoded opcode parameters kept in the summary (which
// are at most about as large). Tags reported only by location (LinearizationTable,
// ProfileGainTableMap, IlluminantData) are not read and cost nothing. Peak
// memory is therefore about max_total_value_bytes plus the IFD entry tables, whose size is
// bounded by max_ifds and max_entries_per_ifd (about 56 bytes per entry, one IFD at a time).
struct TiffInspectLimits {
    // Total IFDs visited: the IFD0 chain, all SubIFDs and Exif IFDs together. At least 1.
    std::uint32_t max_ifds = 64;

    // Deepest SubIFD/Exif nesting below a main-chain IFD (a SubIFD of IFD0 has depth 1).
    std::uint32_t max_ifd_depth = 4;

    // Entries allowed in one IFD. At least 1.
    std::uint32_t max_entries_per_ifd = 4096;

    // Largest in-memory size charged for a single tag value (see above). At least 1.
    std::uint64_t max_value_bytes = std::uint64_t{16} << 20;

    // Sum of the sizes charged for all tag values of one inspection. At least 1.
    std::uint64_t max_total_value_bytes = std::uint64_t{64} << 20;

    // Opcodes accepted in one opcode list. Real files carry a handful; 0 rejects every non-empty
    // list.
    std::uint32_t max_opcodes_per_list = 4096;

    // Warnings kept in TiffSummary::warnings; later ones are only counted.
    std::uint32_t max_warnings = 64;
};

// ---------------------------------------------------------------------------------------------
// Summary types
// ---------------------------------------------------------------------------------------------

enum class TiffByteOrder : std::uint8_t {
    LittleEndian = 1,  // "II"
    BigEndian = 2,     // "MM"
};

enum class TiffVariant : std::uint8_t {
    Classic = 1,  // magic 42, 32-bit offsets
    BigTiff = 2,  // magic 43, 64-bit offsets
};

// How an IFD was reached.
enum class TiffIfdKind : std::uint8_t {
    Main = 1,    // IFD0 or a successor in the IFD0 next-IFD chain
    SubIfd = 2,  // listed by a SubIFDs tag (330)
    Exif = 3,    // referenced by an Exif IFD pointer tag (34665); its tags are not interpreted
};

// Whether stored data went through lossy compression. Part of the decoded-RAW contract
// (DataCompression), so the numeric values are persisted and never change; append only.
enum class Lossiness : std::uint8_t {
    Unknown = 0,  // cannot be determined (unrecognised code, JPEG XL without distance, unknown mode)
    Lossless = 1,
    Lossy = 2,
};

// Stable lowercase identifier: "unknown", "lossless" or "lossy".
[[nodiscard]] std::string_view to_string(Lossiness value) noexcept;

// Interpretation of a Compression tag value in the context of its IFD.
struct CompressionInfo {
    std::uint16_t code = 0;

    // Human-readable method name ("Lossless JPEG"); refers to static storage.
    std::string_view name = "Unrecognized";

    // The value is one that DNG 1.7.1.0 chapter 3 permits in a DNG.
    bool allowed_in_dng = false;

    Lossiness lossiness = Lossiness::Unknown;

    [[nodiscard]] constexpr bool is_lossy() const noexcept { return lossiness == Lossiness::Lossy; }

    friend bool operator==(const CompressionInfo&, const CompressionInfo&) = default;
};

// Classifies a Compression value following DNG 1.7.1.0 chapter 3:
//   1      Uncompressed                                          lossless
//   7      JPEG: baseline DCT (lossy) when PhotometricInterpretation is 6 (YCbCr) with 8-bit
//          samples or 1 (BlackIsZero) with one 8-bit sample; otherwise lossless Huffman JPEG
//   8      Deflate (DNG 1.4)                                     lossless
//   34892  Lossy JPEG (DNG 1.4)                                  lossy
//   52546  JPEG XL (DNG 1.7): lossless when JXLDistance is 0, lossy when > 0, unknown when absent
// LZW (5) and PackBits (32773) are named but not allowed in DNG. Other codes are unrecognised.
[[nodiscard]] CompressionInfo classify_compression(std::uint16_t compression,
                                                   std::optional<std::uint16_t> photometric,
                                                   std::span<const std::uint16_t> bits_per_sample,
                                                   std::optional<float> jxl_distance) noexcept;

// Name of a PhotometricInterpretation value ("CFA", "LinearRaw"), or "Unrecognized".
[[nodiscard]] std::string_view photometric_name(std::uint16_t photometric) noexcept;

// Location of a tag value inside the source, for values that are reported but not decoded.
// The range [offset, offset + byte_count) has been verified to lie within the source.
struct TiffValueLocation {
    std::uint16_t type = 0;        // TiffFieldType value
    std::uint64_t count = 0;       // number of values of `type`
    std::uint64_t offset = 0;      // absolute byte offset of the first value
    std::uint64_t byte_count = 0;  // count * size of `type`

    friend bool operator==(const TiffValueLocation&, const TiffValueLocation&) = default;
};

// Rectangle in raw-image pixel coordinates, half-open: rows [top, bottom), columns [left, right).
struct DngRect {
    std::uint32_t top = 0;
    std::uint32_t left = 0;
    std::uint32_t bottom = 0;
    std::uint32_t right = 0;

    friend bool operator==(const DngRect&, const DngRect&) = default;
};

// Row-major matrix of `rows` x `cols` doubles; multiplies column vectors (out = M * in).
struct DngMatrix {
    std::uint32_t rows = 0;
    std::uint32_t cols = 0;
    std::vector<double> values;  // rows * cols elements, row-major

    // Element at (row, col). Precondition: row < rows and col < cols.
    [[nodiscard]] double at(std::uint32_t row, std::uint32_t col) const noexcept {
        return values[static_cast<std::size_t>(row) * cols + col];
    }

    friend bool operator==(const DngMatrix&, const DngMatrix&) = default;
};

// One diagnostic about input that was tolerated rather than rejected.
struct TiffWarning {
    std::string where;    // stable dotted location, e.g. "raw.tiff.entry"
    std::string message;  // developer-facing English, including the IFD path and tag

    friend bool operator==(const TiffWarning&, const TiffWarning&) = default;
};

// Summary of one IFD. Every optional field is std::nullopt (every vector empty) when the tag is
// absent; TIFF/DNG default values are NOT substituted. Tags are interpreted for kinds Main and
// SubIfd; an Exif IFD reports only its identity fields.
struct TiffIfdSummary {
    // ---- Identity -------------------------------------------------------------------------
    TiffIfdKind kind = TiffIfdKind::Main;

    // Stable path: "ifd0", "ifd1", "ifd0.subifd0", "ifd0.subifd1.subifd0", "ifd0.exif".
    std::string path;

    std::uint64_t offset = 0;             // absolute offset of the IFD
    std::optional<std::uint32_t> parent;  // index into TiffSummary::ifds; nullopt for Main
    std::uint32_t depth = 0;              // 0 for Main, parent depth + 1 otherwise
    std::uint32_t entry_count = 0;

    // ---- Image structure (TIFF 6.0 / TIFF/EP) ---------------------------------------------
    std::optional<std::uint32_t> new_subfile_type;      // 254; see dng_subfile_type
    std::optional<std::uint32_t> width;                 // 256, pixels
    std::optional<std::uint32_t> height;                // 257, pixels
    std::vector<std::uint16_t> bits_per_sample;         // 258
    std::optional<std::uint16_t> compression;           // 259; see compression_info
    std::optional<std::uint16_t> photometric;           // 262; see tiff_photometric
    std::optional<std::uint16_t> orientation;           // 274
    std::optional<std::uint16_t> samples_per_pixel;     // 277
    std::optional<std::uint16_t> planar_configuration;  // 284
    std::vector<std::uint16_t> sample_format;           // 339 (1 unsigned int, 3 IEEE float)
    std::optional<float> jxl_distance;                  // 52553 (DNG 1.7)

    // Classification of `compression` (only meaningful when it is present).
    CompressionInfo compression_info;

    // ---- Colour filter array (TIFF/EP, DNG) -----------------------------------------------
    // CFARepeatPatternDim (33421): {rows, cols}, each >= 1.
    std::optional<std::array<std::uint16_t, 2>> cfa_repeat_dim;
    // CFAPattern (33422): rows * cols colour codes, row-major (0 red, 1 green, 2 blue, 3 cyan,
    // 4 magenta, 5 yellow, 6 white). Verified to match cfa_repeat_dim when both are present.
    std::vector<std::uint8_t> cfa_pattern;
    std::vector<std::uint8_t> cfa_plane_color;  // 50710, colour code of each colour plane
    std::optional<std::uint16_t> cfa_layout;    // 50711

    // ---- DNG raw-IFD tags ------------------------------------------------------------------
    std::optional<DngRect> active_area;  // 50829, verified non-empty and within width x height
    // DefaultCropOrigin / DefaultCropSize (50719 / 50720): {horizontal, vertical} in raw pixels
    // relative to the top-left of the active area. Size components are > 0.
    std::optional<std::array<double, 2>> default_crop_origin;
    std::optional<std::array<double, 2>> default_crop_size;
    // DefaultUserCrop (51125, DNG 1.4.0.0): {top, left, bottom, right} relative to the default crop,
    // 0 <= top < bottom <= 1 and 0 <= left < right <= 1. The tag is advisory (the initial value of
    // a user's crop), so a malformed one is skipped with a warning instead of failing the
    // inspection, as LibRaw also ignores it.
    std::optional<std::array<double, 4>> default_user_crop;
    // BlackLevelRepeatDim (50713) {rows, cols}. The single value 1, which some cameras write
    // instead of two values, is read as {1, 1} with a warning; other single values are Corrupt.
    std::optional<std::array<std::uint16_t, 2>> black_level_repeat_dim;
    // BlackLevel (50714): repeat rows * repeat cols * SamplesPerPixel values (defaults 1, 1, 1
    // when those tags are absent), in stored-sample units; verified against that count.
    std::vector<double> black_level;
    // WhiteLevel (50717): one value per sample (count verified against SamplesPerPixel).
    std::vector<std::uint32_t> white_level;
    // BlackLevelDeltaH / BlackLevelDeltaV (50715 / 50716): per-column / per-row differences from
    // the BlackLevel pattern, in stored-sample units, indexed from the ActiveArea origin. The
    // count is verified against the ActiveArea width / height (the image width / height when
    // there is no ActiveArea).
    std::vector<double> black_level_delta_h;
    std::vector<double> black_level_delta_v;
    // MaskedAreas (50830): rectangles of masked (optically black) pixels, each verified
    // non-empty and within width x height. Readers may measure black levels there.
    std::vector<DngRect> masked_areas;
    std::optional<TiffValueLocation> linearization_table;     // 50712, SHORT, count >= 1
    std::optional<TiffValueLocation> profile_gain_table_map;  // 52525 (DNG 1.6, raw IFD)
    // NoiseProfile (51041): pairs {S, O} per colour plane (or one pair for all planes).
    std::vector<double> noise_profile;

    // ---- Opcode lists (DNG 1.7.1.0 chapter 7) ----------------------------------------------
    // Location of OpcodeList1/2/3 (index 0..2) when present.
    std::array<std::optional<TiffValueLocation>, 3> opcode_lists;
    // Opcodes of all present lists, list 1 first, each list in processing order. The absolute
    // offset of an opcode's parameters is opcode_lists[list - 1]->offset + parameter_offset.
    // Parameters are decoded (decode_dng_opcode_parameters); a malformed parameter area is
    // recorded in DngOpcode::parameter_error (a Corrupt error at "raw.dng.opcode_parameters"),
    // and that opcode cannot be applied.
    std::vector<DngOpcode> opcodes;

    [[nodiscard]] bool has_mandatory_opcodes() const noexcept;

    friend bool operator==(const TiffIfdSummary&, const TiffIfdSummary&) = default;
};

// Camera-profile colour tags of IFD 0 (DNG 1.7.1.0 chapter 6). Array index i holds the tag for
// calibration illuminant i + 1 (color_matrix[0] is ColorMatrix1); the third illuminant's tags
// were added in DNG 1.6.0.0. Values are exactly as stored; nothing is normalised or defaulted.
struct DngColorInfo {
    std::array<std::optional<std::uint16_t>, 3> calibration_illuminant;  // EXIF LightSource codes
    std::array<std::optional<DngMatrix>, 3> color_matrix;                // XYZ -> camera: ColorPlanes x 3
    std::array<std::optional<DngMatrix>, 3> camera_calibration;          // ColorPlanes x ColorPlanes
    std::array<std::optional<DngMatrix>, 3> forward_matrix;              // camera -> XYZ D50: 3 x ColorPlanes
    std::array<std::optional<TiffValueLocation>, 3> illuminant_data;     // IlluminantData1..3
    std::vector<double> analog_balance;                                  // 50727, ColorPlanes values
    std::vector<double> as_shot_neutral;                                 // 50728, ColorPlanes values
    std::optional<std::array<double, 2>> as_shot_white_xy;               // 50729, CIE xy chromaticity
    std::optional<double> baseline_exposure;                             // 50730, EV

    friend bool operator==(const DngColorInfo&, const DngColorInfo&) = default;
};

// DNG-level information; present when IFD 0 carries DNGVersion.
struct DngInfo {
    DngVersion version;  // DNGVersion (50706)
    // DNGBackwardVersion (50707); when absent this holds the specification default, DNGVersion
    // with its last two tiers set to zero, and backward_version_present is false.
    DngVersion backward_version;
    bool backward_version_present = false;
    std::optional<std::string> unique_camera_model;  // 50708 (required by DNG; may be absent)

    // Number of colour planes of the primary raw IFD: the CFAPlaneColor count (default 3) for
    // a CFA image, SamplesPerPixel (default 1) for LinearRaw. nullopt without a primary raw IFD.
    // When known, the colour tags and the NoiseProfile of the raw IFD and of IFD 0 (where some
    // writers store it instead of, or as well as, the raw IFD) are verified against it.
    std::optional<std::uint32_t> color_planes;

    DngColorInfo color;
    std::optional<TiffValueLocation> profile_gain_table_map2;  // 52544 (DNG 1.7, IFD 0)

    friend bool operator==(const DngInfo&, const DngInfo&) = default;
};

struct TiffSummary {
    TiffByteOrder byte_order = TiffByteOrder::LittleEndian;
    TiffVariant variant = TiffVariant::Classic;
    std::uint64_t source_size = 0;

    // Every IFD visited, in depth-first pre-order: ifd0, its SubIFDs (recursively) in order,
    // its Exif IFD, then ifd1 and so on. Indices are stable for a given file.
    std::vector<TiffIfdSummary> ifds;

    std::optional<std::string> make;   // IFD 0 tag 271
    std::optional<std::string> model;  // IFD 0 tag 272

    std::optional<DngInfo> dng;

    // Indices of IFDs that qualify as the main raw image under DNG rules: kind Main or SubIfd,
    // NewSubFileType 0 (or absent), and PhotometricInterpretation CFA or LinearRaw.
    std::vector<std::uint32_t> raw_ifd_candidates;

    // The primary raw IFD: set only when there is exactly one candidate. With several
    // candidates it stays empty, so that no caller silently picks the first image.
    std::optional<std::uint32_t> primary_raw_ifd;

    std::vector<TiffWarning> warnings;
    std::uint32_t suppressed_warnings = 0;  // warnings beyond TiffInspectLimits::max_warnings

    [[nodiscard]] bool is_dng() const noexcept { return dng.has_value(); }

    // The primary raw IFD summary, or nullptr.
    [[nodiscard]] const TiffIfdSummary* primary_raw() const noexcept {
        return primary_raw_ifd ? &ifds[*primary_raw_ifd] : nullptr;
    }

    friend bool operator==(const TiffSummary&, const TiffSummary&) = default;
};

// ---------------------------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------------------------

// Inspects the TIFF structure of `source`.
//
// Errors (Error::where in parentheses):
//   Unsupported    the source does not start with a TIFF ("II*\0"/"MM\0*") or BigTIFF signature,
//                  including TIFF-like vendor variants such as ORF or RW2 (raw.tiff.header)
//   Corrupt        truncated or inconsistent structure or a malformed interpreted tag
//                  (raw.tiff.header, raw.tiff.ifd, raw.tiff.ifd_tree, raw.tiff.entry,
//                  raw.tiff.tag, raw.dng.opcode_list, raw.dng.color); malformed opcode
//                  parameters are recorded per opcode instead (DngOpcode::parameter_error)
//   LimitExceeded  a tag value exceeds max_value_bytes or max_total_value_bytes (raw.tiff.limits),
//                  or an opcode list exceeds max_opcodes_per_list (raw.dng.opcode_list)
//   InvalidArgument  `limits` violates its documented minimums (raw.tiff.limits)
//   OutOfMemory / Internal  allocation failure / unexpected exception (raw.tiff.inspect)
//   Errors returned by `source` are passed through unchanged; a source that returns fewer bytes
//   than its contract promises yields IoError (raw.tiff.source).
//
// Never throws. Reads only within [0, source.size()). Memory is bounded as described for
// TiffInspectLimits.
// Thread-safety: concurrent calls are safe when they use different sources (or a source that is
// itself safe for concurrent use).
[[nodiscard]] Expected<TiffSummary> inspect_tiff(RandomAccessSource& source,
                                                 const TiffInspectLimits& limits = {});

}  // namespace lucent::raw
