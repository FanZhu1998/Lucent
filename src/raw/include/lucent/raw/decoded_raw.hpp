#pragma once

// The decoded-RAW data contract (blueprint section 3, "Adapter boundary"; section 5, step 1).
//
// A RawDecoder (raw_decoder.hpp) describes a RAW file as RawMetadata when probing it and delivers
// its sensor data as a DecodedRaw when decoding it. Everything downstream (engine, catalog,
// diagnostics) consumes these types and never a decoder library's own structures, so decoders can
// be added or replaced without touching the render path.
//
// Principles
//   * Decoder-agnostic data only: no third-party type appears here. Decoder-specific facts that
//     are useful for diagnostics live in DecoderInfo as plain values.
//   * Pixels are copied into Lucent-owned image::ImageBuffers. Sample values are exactly what the
//     decoder produced: no black subtraction, no scaling, no clipping, no white balance. Every
//     sensor channel is preserved; both Bayer greens stay distinct (image::CfaColor::Green and
//     Green2) until a processing route decides to combine them.
//   * Nothing is guessed. Absent values are std::nullopt or empty; values carry a Provenance; the
//     file extension is recorded as a hint only and never decides how data is interpreted.
//   * A file that holds several images is never silently reduced to its first one: metadata
//     states the subimage index, count and role (what the image is, e.g. the main image or a
//     Dual Pixel component frame), probing lists the subimages, and decoding requires an explicit
//     index when there is more than one (raw_decoder.hpp).
//   * A frame that holds several optical views (an unstitched dual-fisheye 360-degree source) is
//     identified as such (ProjectionInfo) and never reported as a complete planar photograph.
//   * Operations a file requires (for example DNG opcode lists) and whether Lucent can perform
//     them are data (RequiredOperation, Interpretation), not user-interface state. A file that
//     needs an unsupported mandatory operation can still be decoded, but its Interpretation says
//     so and it must never be reported as verified.
//
// Units: pixel coordinates and sizes are in stored (unrotated) sensor pixels; levels are in
// stored-sample units; times in seconds; focal lengths in millimetres.
//
// Versioning: the contract version is kDecodedRawContractVersion (lucent::core::ContractVersion).
// Every enum below has persisted numeric values that never change; new values are appended.
// Adding a field is a minor-version change; changing the meaning of a field or of pixel values
// is a major-version change. A reader that meets a newer major version must reject the data.
//
// Thread-safety: all types are plain values without internal synchronisation. Concurrent reads
// are safe; see image/pixel_buffer.hpp for pixel buffers.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <lucent/core/error.hpp>
#include <lucent/core/file.hpp>
#include <lucent/core/version.hpp>
#include <lucent/image/cfa.hpp>
#include <lucent/image/color_encoding.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/image/pixel_buffer.hpp>
#include <lucent/raw/dng_opcodes.hpp>
#include <lucent/raw/dng_version.hpp>
#include <lucent/raw/tiff_inspector.hpp>

namespace lucent::raw {

// Version of this contract.
//   1.0: initial W1 contract.
//   1.1: DataCompression gains provenance and basis and is filled for every container; subimage
//        roles (SubimageRole); view layout (ProjectionInfo); NoiseProfile source IFD;
//        Provenance::LucentRule; interpretation issue kinds 9-13.
//   1.2: SensorLevels gains per-column and per-row black deltas (column_black_delta,
//        row_black_delta, delta_provenance), filled from DNG BlackLevelDeltaH/V. When they are
//        filled, `black` no longer contains the decoder's folded mean of the deltas, and
//        UnrepresentedBlackLevelCorrection is raised only when the deltas cannot be carried.
//   1.3: DNG opcodes carry their decoded parameters (DngOpcode::parameters, parameter_error, in
//        DngMetadata::raw_ifd); RequiredOperation::supported_by_lucent is set for the opcodes
//        Lucent's engine applies (ADR 0014) and RequiredOperation gains lucent_note and
//        superseded_by_warp_rectilinear2. Supported operations raise no interpretation issue, so
//        a DNG whose every mandatory opcode is supported can be Complete. Pixel values are
//        unchanged: the decoder still delivers the stored samples, and the engine applies the
//        opcodes (dng_opcode_frame() describes where).
inline constexpr core::ContractVersion kDecodedRawContractVersion{1, 3};

// ---------------------------------------------------------------------------------------------
// Enumerations (persisted values; append only)
// ---------------------------------------------------------------------------------------------

// How the sensor data is represented.
enum class RawRepresentation : std::uint8_t {
    Mosaic = 1,       // one sample per site behind a colour filter array (Bayer, X-Trans, ...)
    LinearColor = 2,  // already full-colour camera RGB (linear DNG, sRAW, YCC pseudo-RAW)
    Monochrome = 3,   // one unfiltered sample per pixel (monochrome sensors)
};

// Numeric type of the decoded samples.
enum class SampleEncoding : std::uint8_t {
    UInt16 = 1,   // unsigned 16-bit integers in the decoder's stored-sample units
    Float32 = 2,  // IEEE float (floating-point DNG)
};

// Container structure recognised from the file's leading bytes (never from its extension).
enum class ContainerFormat : std::uint8_t {
    Unknown = 1,  // no signature Lucent recognises (the decoder may still understand the file)
    Tiff = 2,     // TIFF / TIFF-EP / BigTIFF based (NEF, CR2, ARW, PEF, 3FR, ...) that is not DNG
    Dng = 3,      // TIFF-based and the decoder reports a DNG version
    Cr3 = 4,      // ISO base media file with the Canon "crx " brand
    Raf = 5,      // Fujifilm RAF ("FUJIFILM")
    Orf = 6,      // Olympus ORF ("IIRO", "IIRS", "MMOR")
    Rw2 = 7,      // Panasonic RW2 / RAW ("IIU\0")
    Crw = 8,      // Canon CIFF ("HEAPCCDR")
    X3f = 9,      // Sigma X3F ("FOVb")
};

// Where a value came from.
enum class Provenance : std::uint8_t {
    // Reported by the decoder library. The library does not say whether a value came from the
    // file's metadata, its built-in per-camera tables or its own computation (for example black
    // levels measured from masked border pixels during unpacking).
    Decoder = 1,
    // Read directly from a DNG tag by Lucent's TIFF/DNG inspector (tiff_inspector.hpp).
    DngTag = 2,
    // Derived by Lucent from other metadata with a documented rule (for example a table of
    // camera models); the accompanying `basis` text names the rule and its inputs.
    LucentRule = 3,
};

// What one image of a file is. Only a Primary or BurstFrame image is a photograph on its own.
enum class SubimageRole : std::uint8_t {
    Primary = 1,             // the file's only image, or its main image
    DualPixelComponent = 2,  // Canon Dual Pixel RAW: the frame of one photodiode (about half the signal)
    BurstFrame = 3,          // one complete exposure of a RAW burst stored in one file
    PixelShiftFrame = 4,     // one exposure of a pixel-shift set that is meant to be combined
    Unknown = 5,             // the file holds several images whose relation Lucent cannot determine
};

// Arrangement of optical views in the frame (blueprint section 3, "Insta360 and 360 imagery").
enum class ViewLayout : std::uint8_t {
    // One optical view. Assumed whenever nothing identifies another layout; the lens projection
    // itself (rectilinear or fisheye) is not asserted.
    SingleView = 1,
    // Two unstitched fisheye views of a two-lens 360-degree camera, side by side or stacked.
    // Developing such a frame as one planar image is never correct; stitching is required.
    DualFisheye360 = 2,
};

// When metadata values were captured. Some values (black levels, white level, the image
// dimensions of legacy full-colour decoders, the representation) are only final once the pixel
// data has been unpacked; a probe reports the values known after opening the file.
enum class MetadataStage : std::uint8_t {
    AfterOpen = 1,    // probe: file opened and identified, pixel data not unpacked
    AfterUnpack = 2,  // decode: values the decoder holds after unpacking the pixel data
};

// Processing that was applied to the data before Lucent receives it, by the camera, by the
// software that wrote the file, or by the decoder while unpacking.
enum class PriorProcessingKind : std::uint8_t {
    Demosaiced = 1,            // data is already full colour (linear DNG, sRAW, YCC pseudo-RAW)
    WhiteBalanceApplied = 2,   // as-shot white balance is already applied to the samples
    YCbCrConverted = 3,        // stored as YCbCr; the decoder converted it to camera RGB
    LinearizationApplied = 4,  // the decoder mapped stored values through a linearization table
    LossyCompression = 5,      // the stored data was lossy compressed
    ToneCurveApplied = 6,      // the decoder mapped values through a tone curve (lossy DNG)
};

// Kind of an operation that a file requires for faithful interpretation.
enum class RequiredOperationKind : std::uint8_t {
    DngOpcode = 1,            // an opcode from OpcodeList1/2/3 (DNG 1.7.1.0 chapter 7)
    ProfileGainTableMap = 2,  // DNG 1.6 ProfileGainTableMap / DNG 1.7 ProfileGainTableMap2
};

// Pipeline stage at which a required operation applies.
enum class OperationStage : std::uint8_t {
    StoredRaw = 1,         // on stored raw values, before linearization and black subtraction (List1)
    LinearRaw = 2,         // on linear reference values, after black/white normalisation (List2)
    AfterDemosaic = 3,     // on demosaiced camera RGB (List3)
    ProfileRendering = 4,  // while rendering through the camera profile
};

// Overall ability to interpret the file faithfully, derived from InterpretationIssues. Ordered
// from best to worst; derive_interpretation_status() picks the worst issue.
enum class InterpretationStatus : std::uint8_t {
    Complete = 1,                      // nothing required is missing
    OptionalOperationsSkipped = 2,     // only optional operations are unavailable
    RequiresUnsupportedOperation = 3,  // a mandatory operation is unavailable: never "verified"
    Ambiguous = 4,                     // Lucent cannot determine what the file requires
    NotDecodable = 5,                  // the pixel data cannot be decoded or represented
};

// Reason recorded in an InterpretationIssue.
enum class InterpretationIssueKind : std::uint8_t {
    MandatoryOperationUnsupported = 1,  // -> RequiresUnsupportedOperation
    OptionalOperationUnsupported = 2,   // -> OptionalOperationsSkipped
    DngVersionUnsupported = 3,     // DNGBackwardVersion newer than supported -> RequiresUnsupportedOperation
    DngInspectionFailed = 4,       // DNG structure could not be inspected -> Ambiguous
    DngRawImageAmbiguous = 5,      // decoded image cannot be matched to one DNG raw IFD -> Ambiguous
    DecoderUnsupportedFormat = 6,  // decoder recognises the file but cannot decode it -> NotDecodable
    UnsupportedSensorLayout = 7,   // e.g. Fujifilm SuperCCD rotated layout -> NotDecodable
    UnsupportedChannelLayout = 8,  // e.g. 4-colour full-colour data -> NotDecodable
    // The selected subimage is a component of a capture, not a photograph on its own (a Dual
    // Pixel frame) -> RequiresUnsupportedOperation
    SubimageNotAPhotograph = 9,
    // The selected subimage is one frame of a set Lucent does not combine (pixel shift); the frame
    // itself is a valid exposure -> OptionalOperationsSkipped
    SubimageFramesNotCombined = 10,
    // The file holds several images and Lucent cannot tell what the selected one is -> Ambiguous
    SubimageRoleUnknown = 11,
    // Unstitched multi-lens source (ViewLayout other than SingleView) -> RequiresUnsupportedOperation
    MultiViewSource = 12,
    // Black-level corrections the file provides that SensorLevels cannot carry: DNG
    // BlackLevelDeltaH/V that do not cover the visible area, or whose mean the decoder folded
    // into its common black level in a way Lucent cannot undo exactly (see SensorLevels)
    // -> RequiresUnsupportedOperation
    UnrepresentedBlackLevelCorrection = 13,
};

// Stable lowercase identifiers for reports and logs ("mosaic", "uint16", "cr3", ...).
[[nodiscard]] std::string_view to_string(RawRepresentation value) noexcept;
[[nodiscard]] std::string_view to_string(SampleEncoding value) noexcept;
[[nodiscard]] std::string_view to_string(ContainerFormat value) noexcept;
[[nodiscard]] std::string_view to_string(Provenance value) noexcept;
[[nodiscard]] std::string_view to_string(SubimageRole value) noexcept;
[[nodiscard]] std::string_view to_string(ViewLayout value) noexcept;
[[nodiscard]] std::string_view to_string(MetadataStage value) noexcept;
[[nodiscard]] std::string_view to_string(PriorProcessingKind value) noexcept;
[[nodiscard]] std::string_view to_string(RequiredOperationKind value) noexcept;
[[nodiscard]] std::string_view to_string(OperationStage value) noexcept;
[[nodiscard]] std::string_view to_string(InterpretationStatus value) noexcept;
[[nodiscard]] std::string_view to_string(InterpretationIssueKind value) noexcept;

// Recognises a container from the first bytes of a file (at least 16 bytes should be passed;
// fewer simply recognise less). The result never depends on the file name. A TIFF-based file is
// reported as Tiff; only a decoder can confirm that it is a DNG.
[[nodiscard]] ContainerFormat detect_container_format(std::span<const std::byte> leading_bytes) noexcept;

// ---------------------------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------------------------

// The file that was read.
struct SourceInfo {
    std::filesystem::path path;
    std::uint64_t size_bytes = 0;
    // Identity of the file object whose bytes were decoded, captured through the open handle.
    // The adapter verifies it is unchanged when it finishes (ErrorCode::SourceChanged otherwise).
    core::FileIdentity identity;
};

// Compression of the decoded image data, for every container.
//
// DNG: from the Compression tag of the decoded raw IFD (Provenance::DngTag). Other formats:
// from the decoder's codec and the camera's own record of its compression mode, as the decoder
// parsed it from the maker notes (Provenance::Decoder), for example Canon Quality (RAW or
// C-RAW), Nikon NEFCompression, Sony SonyRawFileType and the Fujifilm compressed-data header.
// When the lossiness is Lossy, RawMetadata::prior_processing also lists LossyCompression.
struct DataCompression {
    // TIFF Compression tag value of the decoded DNG raw IFD; std::nullopt for other containers.
    std::optional<std::uint16_t> tiff_code;
    // Stable English name of the method ("Lossless JPEG", "Canon CRX lossy (C-RAW)",
    // "Nikon NEF lossy (type 2)"); the names never change once released. "unknown" when the
    // method could not be identified.
    std::string name = "unknown";
    Lossiness lossiness = Lossiness::Unknown;
    Provenance provenance = Provenance::Decoder;
    // The evidence, developer-facing English ("DNG raw IFD ifd0.subifd0 Compression 7",
    // "Canon maker note Quality 7, decoder crxLoadRaw()"); empty when nothing was found.
    std::string basis;
};

struct FormatInfo {
    ContainerFormat container = ContainerFormat::Unknown;  // from content, see detect_container_format
    // Lower-case file extension without the dot ("cr3"), as a hint for diagnostics only.
    std::string extension_hint;
    std::optional<DngVersion> dng_version;  // DNGVersion reported by the decoder
    DataCompression compression;
};

// Camera identity. Strings are UTF-8, sanitised (lucent::core::sanitize_utf8) and trimmed; empty
// when the file does not provide them.
struct CameraIdentity {
    std::string make;                 // as recorded in the file
    std::string model;                // as recorded in the file
    std::string normalized_make;      // decoder's canonical maker name (e.g. "Sony")
    std::string normalized_model;     // decoder's canonical model name (e.g. "ILCE-7M4")
    std::string software;             // firmware or writing software
    std::string unique_camera_model;  // DNG UniqueCameraModel, empty for other formats
};

struct LensIdentity {
    std::string make;   // empty when unknown
    std::string model;  // empty when unknown
    std::optional<float> min_focal_length_mm;
    std::optional<float> max_focal_length_mm;
    std::optional<float> max_aperture_at_min_focal;  // f-number
    std::optional<float> max_aperture_at_max_focal;  // f-number
    std::optional<float> focal_length_35mm_equivalent_mm;
};

struct CaptureSettings {
    std::optional<float> iso;
    std::optional<float> exposure_time_s;
    std::optional<float> f_number;
    std::optional<float> focal_length_mm;
    // Camera wall-clock time "YYYY-MM-DDTHH:MM:SS" without a time zone, as recorded by the
    // camera. The value may be off by one hour for times inside a daylight-saving transition gap
    // of the machine's time zone (see ADR 0005).
    std::optional<std::string> capture_time_local;
};

// Sensor geometry in stored (unrotated) pixel coordinates of the full raw frame.
struct SensorGeometry {
    std::uint32_t raw_width = 0;  // full decoded frame including masked margins
    std::uint32_t raw_height = 0;
    // Area the decoder considers image data, in raw-frame coordinates: x = left margin,
    // y = top margin. Always non-empty and inside the raw frame.
    image::PixelRect visible_area;
    double pixel_aspect = 1.0;  // pixel width / height; 1.0 for square pixels
    image::Orientation orientation = image::Orientation::Normal;  // stored -> upright
};

struct RepresentationInfo {
    RawRepresentation kind = RawRepresentation::Mosaic;
    SampleEncoding sample_encoding = SampleEncoding::UInt16;
    // Samples per pixel in the decoded buffer: 1 for Mosaic and Monochrome, 3 for LinearColor.
    std::int32_t channels = 1;
    // Number of distinct colour channels the decoder uses (LibRaw idata.colors) and their
    // letters in channel-index order ("RGBG": index 3 is the second green).
    std::int32_t color_count = 0;
    std::string color_description;
    // Colour of each channel index in use. Per-channel arrays in this contract (channel_black,
    // linear_max, white-balance multipliers, camera matrix rows; always 4 entries) are indexed by
    // this index; entries at or beyond channel_colors.size() are unused. Mosaic: every index the
    // CFA uses (for LibRaw Bayer data R, G, B, G2: index 1 is Green, sharing rows with red, and
    // index 3 is Green2). LinearColor: the three colour channels. Monochrome: {White}.
    std::vector<image::CfaColor> channel_colors;
    // Mosaic only: the CFA relative to the ORIGIN OF THE RAW FRAME (not the visible area). Use
    // cfa_at_visible_origin() for the visible-area frame. Empty for other representations.
    image::CfaPattern cfa;
};

// Black and white levels in stored-sample units. The black level of a site is
//     black + channel_black[channel index of the site]
//           + black_pattern[(vy % pattern_rows) * pattern_cols + (vx % pattern_cols)]
//           + column_black_delta[vx] + row_black_delta[vy]
// where (vx, vy) are VISIBLE-AREA coordinates (raw-frame coordinates minus the visible-area
// origin); the pattern term is 0 when pattern_rows or pattern_cols is 0, and a delta term is 0
// when its vector is empty (black_level_at()). This is the DNG definition (DNG 1.7.1.0
// chapter 5, "Black Subtraction": BlackLevel + BlackLevelDeltaH + BlackLevelDeltaV) with the
// decoder's channel and pattern terms.
// Values are those after MetadataStage (see RawMetadata::stage): several decoders measure black
// from masked pixels while unpacking, so only AfterUnpack values are final.
//
// Per-column and per-row deltas (DNG BlackLevelDeltaH / BlackLevelDeltaV) are read by Lucent's
// DNG inspector from the decoded raw IFD. The tags are indexed from the DNG ActiveArea origin;
// here they are re-indexed to the visible area (LibRaw can start the visible area one row or
// column inside an ActiveArea with an odd margin). LibRaw adds the rounded mean of each delta
// tag to its common black level; when the deltas are filled, that amount has been removed from
// `black` again, so each correction is counted exactly once. When the deltas cannot be carried
// (they do not cover the visible area, or the decoder's folding cannot be undone exactly), the
// vectors stay empty, `black` keeps the decoder's value, the metadata carries an
// UnrepresentedBlackLevelCorrection issue, and DngMetadata::raw_ifd still holds the tag values.
struct SensorLevels {
    Provenance provenance = Provenance::Decoder;
    double black = 0.0;
    std::array<double, 4> channel_black{};
    std::uint32_t pattern_rows = 0;
    std::uint32_t pattern_cols = 0;
    std::vector<double> black_pattern;  // pattern_rows * pattern_cols values, row-major
    double white = 0.0;                 // saturation / maximum level of the stored data
    // The decoder's per-channel "linear maximum" (LibRaw color.linear_max; 0 when it has none).
    // Informational only: it is not necessarily read from the file. LibRaw 0.22 fills it from
    // the Canon SpecularWhiteLevel, Sony and Olympus maker-note levels, Panasonic LinearityLimit
    // tags (lowered by 64 when they equal 16383), an approximation from the DNG
    // LinearResponseLimit, and for Nikon files without one it invents maximum / 1.07. LibRaw's
    // own processing never uses it. Real data can lie between it and `white`, so it is not a
    // saturation level (the engine saturates at `white`).
    std::array<double, 4> linear_max{};
    // Per-column black correction, indexed by visible-area column: empty (no correction) or
    // exactly visible_area.width values.
    std::vector<double> column_black_delta;
    // Per-row black correction, indexed by visible-area row: empty (no correction) or exactly
    // visible_area.height values.
    std::vector<double> row_black_delta;
    // Source of the two delta vectors (DngTag for DNG BlackLevelDeltaH/V); meaningful only when
    // one of them is non-empty.
    Provenance delta_provenance = Provenance::DngTag;

    // Total black level at channel `channel_index` (0..3) and visible-area position (vx, vy).
    // Negative positions are wrapped into the pattern; a delta term applies only at positions
    // inside its vector (masked margins outside the visible area have no delta).
    // Precondition: channel_index < 4.
    [[nodiscard]] double black_level_at(std::size_t channel_index, std::int64_t vx,
                                        std::int64_t vy) const noexcept;

    // Upper bound of the black level of any site: black + the largest channel_black + the largest
    // black_pattern value + the largest column delta + the largest row delta (each delta term
    // counted only when its vector is non-empty). Decoded data always has white above it
    // (check_consistency).
    [[nodiscard]] double max_black_level() const noexcept;
};

// Camera colour data as the decoder reports it (LibRaw colour model). Matrices are row-major;
// rows or columns that correspond to camera channels are indexed by channel index. Zero-filled
// values mean "not available".
struct CameraColorData {
    Provenance provenance = Provenance::Decoder;
    // As-shot white-balance multipliers per channel, not normalised (LibRaw cam_mul).
    std::array<float, 4> as_shot_multipliers{};
    // Daylight white-balance multipliers per channel (LibRaw pre_mul).
    std::array<float, 4> daylight_multipliers{};
    // XYZ(D65) -> camera matrix, 4 camera rows x 3 XYZ columns (LibRaw cam_xyz, usually from the
    // decoder's built-in per-model table).
    std::array<std::array<float, 3>, 4> xyz_to_camera{};
    // Camera -> sRGB matrix computed from a matrix stored in the file (LibRaw cmatrix, 3 x 4).
    std::array<std::array<float, 4>, 3> camera_to_srgb_from_file{};
    // Camera -> sRGB matrix the decoder would use for rendering (LibRaw rgb_cam, 3 x 4).
    std::array<std::array<float, 4>, 3> camera_to_srgb{};
};

// DNG NoiseProfile (51041): the variance of a normalised signal x is scale * x + offset.
struct NoiseModel {
    double scale = 0.0;
    double offset = 0.0;

    friend bool operator==(const NoiseModel&, const NoiseModel&) = default;
};

struct NoiseProfile {
    Provenance provenance = Provenance::DngTag;
    // One entry per colour plane, or a single entry that applies to all planes.
    std::vector<NoiseModel> planes;
    // Inspector path of the IFD the tag was read from: the decoded raw IFD ("ifd0.subifd0"), or
    // "ifd0" when only IFD 0 carries it (DNG 1.7.1.0 places NoiseProfile in the raw IFD, but
    // writers of earlier versions store it in IFD 0 only, and Adobe's converter writes both).
    std::string source_ifd;
};

// Optical layout of the frame and why it was chosen.
struct ProjectionInfo {
    ViewLayout layout = ViewLayout::SingleView;
    // LucentRule for a layout identified from the camera identity and frame shape.
    Provenance provenance = Provenance::LucentRule;
    std::string basis;  // developer-facing English; empty for the SingleView default
};

struct PriorProcessingStep {
    PriorProcessingKind kind = PriorProcessingKind::Demosaiced;
    std::string detail;  // developer-facing English
};

// One operation the file requires.
struct RequiredOperation {
    RequiredOperationKind kind = RequiredOperationKind::DngOpcode;
    OperationStage stage = OperationStage::StoredRaw;
    std::string name;                // "GainMap", "WarpRectilinear", "ProfileGainTableMap", "Unknown"
    std::uint32_t opcode_id = 0;     // DNG opcode ID for DngOpcode, 0 otherwise
    std::uint32_t index = 0;         // position within its opcode list
    DngVersion min_dng_version;      // version the opcode requires (DngOpcode)
    std::uint32_t flags = 0;         // raw opcode flag word (DngOpcode)
    bool mandatory = true;           // false when the file marks it optional
    bool preview_skippable = false;  // may be skipped for preview-quality output only
    // A WarpRectilinear/WarpFisheye that follows an optional WarpRectilinear2 (DNG 1.7.1.0 ch. 7):
    // skipped by readers that apply the WarpRectilinear2.
    bool follows_optional_warp_rectilinear2 = false;
    // Lucent's engine applies this operation at its stage (raw::assess_dng_opcode; ADR 0014). A
    // supported operation raises no interpretation issue. Never set together with
    // applied_by_decoder or superseded_by_warp_rectilinear2.
    bool supported_by_lucent = false;
    // follows_optional_warp_rectilinear2 and Lucent applies that WarpRectilinear2, so this opcode
    // is skipped as the specification requires (no issue either).
    bool superseded_by_warp_rectilinear2 = false;
    // The decoder applied this operation (or an approximation of it) while unpacking, e.g. LibRaw
    // applying MapPolynomial of a lossy DNG. Informational: it does not make the operation
    // supported, because Lucent has not verified the decoder's implementation, and Lucent never
    // applies it a second time.
    bool applied_by_decoder = false;
    // Developer-facing English: why Lucent does not apply the operation (empty when it does).
    std::string lucent_note;
};

struct InterpretationIssue {
    InterpretationIssueKind kind = InterpretationIssueKind::MandatoryOperationUnsupported;
    std::string message;  // developer-facing English
};

struct Interpretation {
    InterpretationStatus status = InterpretationStatus::Complete;
    std::vector<InterpretationIssue> issues;
};

// The status implied by a set of issues: the worst status any issue implies, or Complete.
[[nodiscard]] InterpretationStatus derive_interpretation_status(
    std::span<const InterpretationIssue> issues) noexcept;

// DNG structure as seen by Lucent's inspector, for DNG files whose decoded image could be
// matched to exactly one raw IFD.
struct DngMetadata {
    DngInfo info;                       // IFD 0: versions, camera profile colour tags, ...
    std::uint32_t raw_ifd_index = 0;    // index into the inspector's IFD list
    TiffIfdSummary raw_ifd;             // the raw IFD: compression, CFA, levels, crops, opcodes, noise
    std::vector<TiffWarning> warnings;  // inspector warnings (tolerated irregularities)
};

// LibRaw-model values with exactly the semantics of rawpy 0.27.1's properties, used only to
// compare Lucent's decoder with the rawpy reference (research/README.md, "Probe report
// contract"). Never used for processing. Values are taken at RawMetadata::stage.
struct ReferenceParity {
    std::int32_t raw_width = 0;
    std::int32_t raw_height = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t top_margin = 0;
    std::int32_t left_margin = 0;
    std::int32_t iwidth = 0;
    std::int32_t iheight = 0;
    std::int32_t flip = 0;
    std::int32_t num_colors = 0;
    std::string color_desc;
    // n x n colour indices (rawpy raw_pattern); nullopt for full-colour ("stack") data.
    std::optional<std::vector<std::vector<std::int32_t>>> raw_pattern;
    std::array<std::uint32_t, 4> black_level_per_channel{};
    std::uint32_t white_level = 0;
    std::optional<std::array<std::uint32_t, 4>> camera_white_level_per_channel;
    std::array<float, 4> camera_whitebalance{};
    std::array<float, 4> daylight_whitebalance{};
    std::array<std::array<float, 3>, 4> rgb_xyz_matrix{};
    std::array<std::array<float, 4>, 3> color_matrix{};  // LibRaw cmatrix (as rawpy), not rgb_cam
    std::string raw_type;                                // "flat" or "stack"
};

// The decoder that produced the metadata and what it reported about itself.
struct DecoderInfo {
    std::string adapter_id;                     // RawDecoder::id(), e.g. "libraw"
    std::string library;                        // "LibRaw"
    std::string library_version;                // e.g. "0.22.2-Release"
    std::vector<std::string> capabilities;      // decoder build capabilities, e.g. "zlib", "jpeg"
    std::string unpacker;                       // decoder routine for the pixel data, e.g. "crxLoadRaw()"
    std::vector<std::string> unpacker_flags;    // e.g. "hascurve", "adobecopypixel"
    std::vector<std::string> process_warnings;  // decoder warnings, e.g. "dng_images_reordered"
    std::vector<std::string> warnings;          // adapter warnings (developer-facing English)
    std::optional<ReferenceParity> reference_parity;
};

// Everything known about one image of a RAW file.
struct RawMetadata {
    MetadataStage stage = MetadataStage::AfterOpen;
    SourceInfo source;
    FormatInfo format;
    std::uint32_t subimage_index = 0;  // image of the file these values describe
    std::uint32_t subimage_count = 1;  // images the decoder can extract from the file (>= 1)
    // What that image is. Primary for single-image files. A role other than Primary or
    // BurstFrame comes with an InterpretationIssue, so such an image is never "verified".
    SubimageRole subimage_role = SubimageRole::Primary;
    CameraIdentity camera;
    LensIdentity lens;
    CaptureSettings capture;
    SensorGeometry geometry;
    // Layout of optical views; anything but SingleView comes with a MultiViewSource issue.
    ProjectionInfo projection;
    RepresentationInfo representation;
    SensorLevels levels;
    CameraColorData color;
    std::optional<NoiseProfile> noise;
    std::vector<PriorProcessingStep> prior_processing;
    std::vector<RequiredOperation> required_operations;
    Interpretation interpretation;
    std::optional<DngMetadata> dng;
    DecoderInfo decoder;

    [[nodiscard]] bool has_prior_processing(PriorProcessingKind kind) const noexcept;
};

// The CFA relative to the visible-area origin (representation.cfa shifted by the visible-area
// origin). Empty unless the representation is Mosaic.
[[nodiscard]] image::CfaPattern cfa_at_visible_origin(const RawMetadata& metadata);

// The BayerPhase (DNG 1.7.1.0, FixBadPixelsConstant) of `cfa` at its origin; std::nullopt
// unless it is a 2 x 2 Bayer pattern.
[[nodiscard]] std::optional<DngBayerPhase> bayer_phase_of(const image::CfaPattern& cfa) noexcept;

// The image one opcode list of the decoded DNG raw IFD processes, and where Lucent's buffer for
// that stage lies in it (ADR 0014):
//   List 1: the whole stored raw image (raw IFD width x height); Lucent's buffer is the decoded
//           raw frame, which must have the same size. Unavailable when the decoder linearised
//           the stored values (LinearizationTable) or decoded a lossy DNG through its own tone
//           curve, since the list must see the stored values.
//   List 2: the ActiveArea (the whole image without one), after black and white mapping;
//           Lucent's buffer is the normalised visible area, which must lie inside it.
//           Unavailable when the decoder applied an opcode of this list while unpacking.
//   List 3: the same rectangle after demosaicing, with the colour planes of the camera profile
//           (3 for colour, 1 for monochrome), which must be red, green, blue in that order.
// Without DNG metadata (metadata.dng empty) every list is unavailable.
struct DngOpcodeFrame {
    DngOpcodeImage image;
    // Lucent's buffer pixel (0, 0) is stage-image pixel (offset_x, offset_y); >= 0.
    std::int32_t offset_x = 0;
    std::int32_t offset_y = 0;
};
[[nodiscard]] DngOpcodeFrame dng_opcode_frame(const RawMetadata& metadata, DngOpcodeListId list);

// The pipeline stage of an opcode list (List1 StoredRaw, List2 LinearRaw, List3 AfterDemosaic).
[[nodiscard]] OperationStage operation_stage_of(DngOpcodeListId list) noexcept;

// The RequiredOperation (kind DngOpcode) describing `opcode`, before assessment: support flags,
// applied_by_decoder and lucent_note are left for the decoder and assess_dng_opcode_operations.
[[nodiscard]] RequiredOperation required_operation_for(const DngOpcode& opcode);

// Decides, for every DNG-opcode entry of metadata.required_operations (matched by list, index and
// ID to metadata.dng->raw_ifd.opcodes), whether Lucent's engine applies it (assess_dng_opcode on
// the image dng_opcode_frame() describes, evaluated in processing order: a TrimBounds Lucent
// applies makes its rectangle the stage image of the following opcodes of its list, and in List 2
// of List 3, as the engine does, and one that keeps no pixel of the visible area is not
// supported; never an operation applied_by_decoder), applies the
// WarpRectilinear2 skip rule (DNG 1.7.1.0 chapter 7: an opcode that follows an optional
// WarpRectilinear2 Lucent applies is superseded_by_warp_rectilinear2), fills lucent_note, appends
// an interpretation issue (MandatoryOperationUnsupported or OptionalOperationUnsupported) for each
// opcode that is neither applied by Lucent nor superseded, and re-derives interpretation.status.
// Decoders call it once after filling metadata.dng and the representation.
void assess_dng_opcode_operations(RawMetadata& metadata);

// Summary of one image of a multi-image file, as listed by a probe.
struct SubimageInfo {
    std::uint32_t index = 0;
    // Set when this subimage could not be described; the other fields are then unset.
    std::optional<Error> error;
    SubimageRole role = SubimageRole::Unknown;                             // as RawMetadata::subimage_role
    InterpretationStatus interpretation = InterpretationStatus::Complete;  // of that image's metadata
    std::uint32_t raw_width = 0;
    std::uint32_t raw_height = 0;
    image::PixelRect visible_area;
    RawRepresentation representation = RawRepresentation::Mosaic;
    std::string unpacker;
};

struct ProbeResult {
    core::ContractVersion contract = kDecodedRawContractVersion;
    RawMetadata metadata;  // stage AfterOpen, for metadata.subimage_index
    // Every image of the file in index order when ProbeOptions::describe_subimages is set
    // (entry metadata.subimage_index is derived from `metadata`); empty otherwise.
    std::vector<SubimageInfo> subimages;
};

// ---------------------------------------------------------------------------------------------
// Pixel data
// ---------------------------------------------------------------------------------------------

// Decoded samples; the alternative matches RepresentationInfo::sample_encoding.
using RawPixels = std::variant<image::ImageBuffer<std::uint16_t>, image::ImageBuffer<float>>;

// The full raw frame (raw_width x raw_height, masked margins included), one sample per site.
struct MosaicFrame {
    RawPixels pixels;
    image::CfaPattern cfa;  // relative to the buffer origin (== RepresentationInfo::cfa)
    image::ColorEncoding encoding = image::ColorEncoding::camera_native_mosaic();
};

// Already full-colour camera RGB, 3 interleaved channels in channel-index order 0, 1, 2
// (R, G, B for RepresentationInfo::color_description "RGB?").
struct LinearColorImage {
    RawPixels pixels;
    image::ColorEncoding encoding = image::ColorEncoding::camera_native_linear();
};

// Camera-native, linear, scene-referred single-channel encoding of monochrome sensor data.
[[nodiscard]] constexpr image::ColorEncoding camera_native_monochrome() noexcept {
    image::ColorEncoding e = image::ColorEncoding::camera_native_linear();
    e.layout = image::ChannelLayout::Mono;
    return e;
}

// One unfiltered sample per pixel of the full raw frame.
struct MonochromeFrame {
    RawPixels pixels;
    image::ColorEncoding encoding = camera_native_monochrome();
};

using RawImage = std::variant<MosaicFrame, LinearColorImage, MonochromeFrame>;

struct DecodedRaw {
    core::ContractVersion contract = kDecodedRawContractVersion;
    RawMetadata metadata;  // stage AfterUnpack
    RawImage image;
};

// Width, height and channel count of decoded pixels (all zero for an empty buffer).
struct PixelsShape {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t channels = 0;
    SampleEncoding encoding = SampleEncoding::UInt16;
};
[[nodiscard]] PixelsShape shape_of(const RawPixels& pixels) noexcept;
[[nodiscard]] const RawPixels& pixels_of(const RawImage& image) noexcept;

// Checks the internal consistency of decoded data: the image alternative matches the
// representation; the buffer matches the geometry (raw_width x raw_height), channel count and
// sample encoding; the visible area lies inside the frame; a mosaic has a non-empty CFA equal to
// the metadata's, whose colours all appear in channel_colors; levels are finite with black below
// white; black deltas are empty or match the visible area's width / height; every operation
// marked supported_by_lucent is a decoded DNG opcode that assess_dng_opcode accepts at its stage;
// the contract version is readable. Fails with ErrorCode::Internal ("raw.contract") and a
// message naming the first violation. Adapters call it before returning; tests use it too.
[[nodiscard]] Status check_consistency(const DecodedRaw& decoded);

}  // namespace lucent::raw
