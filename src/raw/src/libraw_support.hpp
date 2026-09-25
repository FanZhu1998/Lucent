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

// State shared with LibRaw's C callbacks during one call. Callbacks never throw.
struct LibRawCallState {
    core::CancellationToken cancel;
    bool cancel_observed = false;
    std::uint32_t data_errors = 0;             // LibRaw data-error callback invocations
    std::int64_t first_data_error_offset = 0;  // -1: unexpected end of file
};

// Installs the progress (cancellation) and data-error callbacks for `state` on `raw`.
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

// Identifies unstitched two-lens 360-degree sources from the camera identity and the frame shape
// of the visible area: Insta360 (make "Arashi Vision" or "Insta360", or an "Insta360" model) and
// Ricoh THETA bodies store both fisheye views in one frame of exactly 2:1 or 1:2. Everything else
// is SingleView. Matching is case-insensitive.
[[nodiscard]] ProjectionInfo classify_projection(std::string_view make, std::string_view model,
                                                 std::uint32_t width, std::uint32_t height);

}  // namespace lucent::raw::detail
