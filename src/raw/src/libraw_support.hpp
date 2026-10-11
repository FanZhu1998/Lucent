#pragma once

// Private helpers of the LibRaw adapter: LibRaw-to-contract conversions that need LibRaw's
// types. Only libraw_*.cpp include this header.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <libraw.h>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/raw/decoded_raw.hpp>

#include "libraw_stream.hpp"

namespace lucent::raw::detail {

// Canon's AspectInfo maker note (tag 0x009a, five LONGs; ExifTool Canon::AspectInfo): the aspect
// ratio code and the cropped image's size and origin. LibRaw 0.22 reads the tag but folds the crop
// codes 12 (APS-H crop) and 13 (APS-C crop) into "3:2" (src/metadata/canon.cpp), so the adapter
// reads it itself through LibRaw's maker-note callback while LibRaw parses the maker notes.
struct CanonAspectInfo {
    std::uint32_t ratio_code = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t left = 0;
    std::uint32_t top = 0;
};

// State shared with LibRaw's C callbacks during one call. Callbacks never throw.
struct LibRawCallState {
    core::CancellationToken cancel;
    bool cancel_observed = false;
    std::uint32_t data_errors = 0;             // LibRaw data-error callback invocations
    std::int64_t first_data_error_offset = 0;  // -1: unexpected end of file
    // The first maker-note tag 0x009a of five LONGs LibRaw parsed (any maker: only meaningful for
    // Canon, which extract_metadata() checks).
    std::optional<CanonAspectInfo> canon_aspect;
};

// Installs the progress (cancellation), data-error and maker-note callbacks for `state` on `raw`.
void install_callbacks(LibRaw& raw, LibRawCallState& state) noexcept;

// Stage names used in Error::where ("raw.libraw.<stage>").
inline constexpr std::string_view kStageOpen = "open";
inline constexpr std::string_view kStageUnpack = "unpack";

// The error for a failed LibRaw call at `stage`, refined with what the stream and callbacks
// observed (see libraw_decoder.hpp, libraw_error_code).
[[nodiscard]] Error libraw_failure(int code, std::string_view stage, const LibRawFileStream& stream,
                                   const LibRawCallState& state);

// Names of bits in LibRaw::capabilities(), get_decoder_info() flags and process_warnings.
[[nodiscard]] std::vector<std::string> capability_names(unsigned capabilities);
[[nodiscard]] std::vector<std::string> decoder_flag_names(unsigned flags);
[[nodiscard]] std::vector<std::string> process_warning_names(unsigned warnings);

// Text from a fixed-size LibRaw character field: up to the first NUL, trimmed of surrounding
// white space and sanitised to UTF-8.
[[nodiscard]] std::string text_field(const char* field, std::size_t capacity);

// Which LibRaw buffer holds the unpacked pixels.
enum class LibRawBuffer : std::uint8_t {
    None,
    Raw,     // rawdata.raw_image: 1 x uint16
    Color3,  // rawdata.color3_image: 3 x uint16
    Color4,  // rawdata.color4_image: 4 x uint16
    Float1,  // rawdata.float_image
    Float3,  // rawdata.float3_image
    Float4,  // rawdata.float4_image
};
[[nodiscard]] LibRawBuffer unpacked_buffer(const LibRaw& raw) noexcept;

// LibRaw's inset crops (imgdata.sizes.raw_inset_crops, libraw_types.h; LibRaw 0.22 doc/
// API-datastruct.html: "item at [0] contains standard crop data parsed from camera metadata (DNG:
// DefaultCrop* tags, other formats: vendor specific tags)", "item at [1] is 'user crop'", set by DNG
// DefaultUserCrop or by raw aspect-ratio tags). Values are raw-frame pixels: cleft/ctop from the
// frame's top-left, an edge of 0xffff meaning "not recorded" (src/utils/init_close_utils.cpp).
struct InsetCrop {
    std::uint32_t left = 0xffff;
    std::uint32_t top = 0xffff;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Everything extract_metadata() needs besides LibRaw's own state.
struct ExtractionContext {
    MetadataStage stage = MetadataStage::AfterOpen;
    SourceInfo source;
    ContainerFormat sniffed_container = ContainerFormat::Unknown;
    std::uint32_t subimage_index = 0;
    const TiffSummary* tiff = nullptr;  // inspector result for TIFF-based DNGs, or nullptr
    std::optional<Error> tiff_error;    // why inspection failed, for DNGs
    // Raw-frame size LibRaw reported after opening (legacy full-colour decoders change it while
    // unpacking); 0 means "use the current size". Used to match the DNG raw IFD.
    std::uint32_t open_raw_width = 0;
    std::uint32_t open_raw_height = 0;
    // Fujifilm RAF: the crop the file declares, read by Lucent (read_raf_crop), or nothing.
    std::optional<InsetCrop> raf_crop;
    // The maker-note tag 0x009a the callbacks captured while LibRaw opened the file
    // (LibRawCallState::canon_aspect); used for Canon only.
    std::optional<CanonAspectInfo> canon_aspect;
};

// Converts LibRaw's state (after open_datastream(), or after unpack() for stage AfterUnpack)
// into contract metadata, including required operations, interpretation and reference parity.
// Fails only for states the contract cannot express (Internal) or allocation failure.
[[nodiscard]] Expected<RawMetadata> extract_metadata(LibRaw& raw, const ExtractionContext& context);

// rawpy-compatible parity values from LibRaw's state (see ReferenceParity).
[[nodiscard]] ReferenceParity extract_reference_parity(LibRaw& raw, MetadataStage stage,
                                                       const RepresentationInfo& representation);

// ---- Classification rules (pure functions of plain facts, so they are unit-testable) ---------

// What LibRaw knows about the compression of non-DNG data: its codec and the camera's own record
// of its compression mode. Fields hold LibRaw's "not recorded" value when absent.
struct CompressionFacts {
    std::string_view unpacker;             // get_decoder_info() name, e.g. "crxLoadRaw()"
    int canon_quality = -1;                // makernotes.canon.Quality (4 RAW, 7 C-RAW)
    unsigned nikon_nef_compression = 0;    // makernotes.nikon.NEFCompression (ExifTool 0x0093)
    unsigned sony_raw_file_type = 0xFFFF;  // makernotes.sony.SonyRawFileType
    int fuji_lossless = -1;                // RAF compressed-data header flag (1 lossless, 0 lossy)
    int panasonic_encoding = 0;            // RW2 encoding (tag 0x2D)
};

// Classifies the compression of non-DNG data (DataCompression, Provenance::Decoder). Codecs that
// are lossless or lossy by construction are classified by the codec alone; codecs that serve
// several modes (Canon CRX, Nikon NEF, Fujifilm compressed RAF) by the recorded mode. Anything
// else is named after the codec with Lossiness::Unknown, or "unknown" without a codec.
[[nodiscard]] DataCompression classify_vendor_compression(const CompressionFacts& facts);

// What LibRaw knows about the images of a multi-image file.
struct SubimageFacts {
    std::uint32_t count = 1;  // images LibRaw offers (raw_count)
    std::uint32_t index = 0;  // the selected one
    bool canon_cr3 = false;   // Canon CR3 container
    // CR3 only: RAW tracks of the largest frame size (LibRaw's image candidates) and the largest
    // number of frames one of them holds (> 1 for a RAW burst).
    std::uint32_t cr3_full_size_raw_tracks = 0;
    std::uint32_t cr3_max_frames_per_track = 0;
    bool pentax_pixel_shift = false;  // Pentax maker note Quality 7 or 8 (pixel shift)
};

struct SubimageClassification {
    SubimageRole role = SubimageRole::Primary;
    std::optional<InterpretationIssue> issue;  // for roles that are not a photograph on their own
};

[[nodiscard]] SubimageClassification classify_subimage(const SubimageFacts& facts);

struct InsetCropFacts {
    InsetCrop crop;       // raw_inset_crops[0] (or the Canon sensor area, see canon_aspect_crops())
    InsetCrop user_crop;  // raw_inset_crops[1] (or the Canon aspect-ratio crop)
    image::Size raw_frame;
    image::PixelRect visible;
    // LibRaw's inset-crop margin adjustment period (src/utils/open.cpp): 6 for Fujifilm X-Trans data
    // (maker Fujifilm, filters 9), 2 for other CFA data (filters >= 1000) unless LibRaw holds it in
    // the rotated Fuji layout (fuji_width), 0 otherwise.
    std::uint32_t margin_period = 0;
    // The same declaration as the file states it, read by Lucent's own parser (Fujifilm RAF
    // RawImageCropTopLeft / RawImageCroppedSize), when available.
    std::optional<InsetCrop> declared;
    std::string declared_name;  // e.g. "RAF RawImageCropTopLeft / RawImageCroppedSize"
    // Where `crop` and `user_crop` come from, for ImageArea::basis (canon_aspect_crops() names its
    // sources here; empty: LibRaw's raw_inset_crops[0] and [1]).
    std::string crop_name;
    std::string user_crop_name;
};

// Canon's maker notes as LibRaw 0.22 reads them, for canon_aspect_crops().
struct CanonCropFacts {
    bool canon_non_dng = false;  // maker Canon and not a DNG
    // imgdata.sizes.raw_aspect: LIBRAW_IMAGE_ASPECT_UNKNOWN (0) unless AspectInfo (0x009a) was
    // parsed; LIBRAW_IMAGE_ASPECT_3to2 (1500) for its 3:2, APS-H and APS-C crop values.
    std::uint32_t raw_aspect = 0;
    // makernotes.canon.DefaultCropAbsolute (SensorInfo 0x00e0, inclusive edges); left -1 when
    // SensorInfo was not parsed.
    std::int32_t sensor_left = -1;
    std::int32_t sensor_top = -1;
    std::int32_t sensor_right = -1;
    std::int32_t sensor_bottom = -1;
};

// Canon's in-camera sensor crop (contract 1.5, SensorCrop): AspectInfo's ratio code 12 (APS-H crop)
// or 13 (APS-C crop) on a non-DNG Canon file whose visible area is the AspectInfo rectangle (within 2 %
// or 8 pixels per side: the camera recorded the crop, as the EOS R5 does in its 1.6x crop mode, not a
// whole-sensor image with a crop attached) is a sensor crop with Canon's factor for it (1.3, 1.6);
// anything else is none.
[[nodiscard]] std::optional<SensorCrop> canon_sensor_crop(bool canon_non_dng, const std::optional<CanonAspectInfo>& info,
                                                          const image::PixelRect& visible);

// Canon's in-camera aspect-ratio setting (QUA-01). LibRaw 0.22 stores Canon's AspectInfo rectangle
// (maker note 0x009a; src/metadata/canon.cpp) in raw_inset_crops[0], offset by the SensorInfo origin
// (src/utils/open.cpp), and leaves [1] empty (its ratio matches raw_aspect). For a 1:1, 4:3, 16:9 or
// 4:5 setting [0] is therefore the aspect-ratio crop, while the camera records the whole 3:2 image
// ("RAW images are generated at 3:2, with information added about the specified aspect ratio",
// Canon EOS R5 manual) and SensorInfo's DefaultCropAbsolute, independent of the setting, is the
// sensor's image area (LibRaw sets the visible area from it, identify.cpp). When `canon` is a
// non-DNG Canon file whose AspectInfo names another ratio than 3:2 (or an unknown one) and whose
// SensorInfo area is recorded and lies inside the raw frame, `facts.crop` becomes the SensorInfo
// area and `facts.user_crop` the AspectInfo rectangle (exposed as the user crop, never applied,
// ADR 0017); otherwise `facts` is unchanged.
void canon_aspect_crops(InsetCropFacts& facts, const CanonCropFacts& canon);

// LibRaw's inset-crop margin adjustment (src/utils/open.cpp, "adjust crop_inset margins") applied to
// `crop` for `period`: a left (top) edge that is non-zero, recorded and not a multiple of the period
// moves inward to the next multiple when the width (height) exceeds the period, and the width
// (height) shrinks by the same amount.
[[nodiscard]] InsetCrop libraw_adjusted_inset_crop(InsetCrop crop, std::uint32_t period) noexcept;

// The image area of a non-DNG file from LibRaw's inset crops (ImageArea, decoded_raw.hpp): the
// crop when it is recorded, inside the raw frame and keeps at least kMinInsetCropFraction of the
// visible area's width and height (DecoderInsetCrop, Provenance::Decoder); otherwise Unknown with
// the reason. When the file's own declaration (`declared`) is known and LibRaw's crop is exactly
// its margin-adjusted form, the declaration is used instead (Provenance::LucentRule: the edges
// LibRaw moved are restored). The user crop is the second inset crop normalised to the first, when
// it lies inside it and is not the whole of it (for Canon's aspect-ratio setting, the AspectInfo
// rectangle inside the SensorInfo area: canon_aspect_crops()).
[[nodiscard]] ImageArea image_area_from_inset_crops(const InsetCropFacts& facts);

// Identifies unstitched two-lens 360-degree sources from the camera identity and the frame shape
// of the visible area: Insta360 (make "Arashi Vision" or "Insta360", or an "Insta360" model) and
// Ricoh THETA bodies store both fisheye views in one frame of exactly 2:1 or 1:2. Everything else
// is SingleView. Matching is case-insensitive.
[[nodiscard]] ProjectionInfo classify_projection(std::string_view make, std::string_view model,
                                                 std::uint32_t width, std::uint32_t height);

}  // namespace lucent::raw::detail
