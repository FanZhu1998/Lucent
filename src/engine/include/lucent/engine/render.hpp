#pragma once

// The first render path (blueprint section 5, "One explicit image-processing pipeline"; ADR 0006):
// one RAW image -> display-referred linear Rec.2020 pixels ready for the exporter, with a
// complete record of what was done.
//
// Ordered stages (RenderStage), each a separate pass with a declared input and output
// ColorEncoding; the pipeline checks that every stage's input equals the previous stage's output
// (an edge mismatch is ErrorCode::Internal, "engine.render.edge"):
//   1. decode            raw::RawDecoder (render_file only)       -> camera-native stored samples
//      dng_opcode_list1  DNG OpcodeList1 (dng_opcode_processing)  -> same encoding (stored samples)
//   2. normalize         normalize_sensor (sensor.hpp)            -> camera-native, white = 1
//      dng_opcode_list2  DNG OpcodeList2                          -> same encoding
//   3. white_balance     resolve/apply_white_balance              -> camera-native, balanced
//   4. demosaic          demosaic (demosaic.hpp)                  -> camera-native linear RGB / mono
//      dng_opcode_list3  DNG OpcodeList3                          -> same encoding
//   5. camera_color      resolve/apply_camera_color               -> CIE XYZ (D50 or D65)
//   6. working_space     convert_to_working_space                 -> scene-linear Rec.2020 / D65
//   7. display_rendering render_neutral_v0 (provisional)          -> display-referred linear Rec.2020
//   8. orientation       apply_orientation                        -> same encoding, upright
// An opcode stage is present only when the file has that opcode list (ADR 0014); it records every
// opcode of the list, applied or not.
// The hand-off to lucent::exporter::encode_for_output is display_linear_rec2020(). Values are
// limited in three places: stage 3 clips raw highlights to the lowest white-balanced saturation
// level (default highlight policy), stage 7 limits max(R, G, B) to 1, and the exporter clips per
// channel whatever lies outside the output profile's range after its conversion (out-of-gamut
// components, negative or above 1).
//
// Not yet part of the path (reported, never silently assumed): DNG opcodes Lucent does not
// implement and other required operations (the metadata's Interpretation says what is missing),
// DNG DefaultCrop, lens-profile corrections, non-square pixels, noise reduction, sharpening,
// resizing.
//
// Memory: before decoding (from the probe) and again before normalising, the pipeline estimates
// the peak of its own pixel buffers (estimate_render_memory) and refuses with LimitExceeded
// ("engine.render.memory") when it exceeds RenderOptions::memory_budget_bytes. Buffers are
// released as soon as the next stage no longer needs them (the raw frame after normalisation,
// the mosaic after demosaicing, the saturation mask after demosaicing).
//
// Cancellation is checked between stages and inside every pixel stage; a cancelled render returns
// ErrorCode::Cancelled and no partial result.
//
// Thread-safety: reentrant; each call owns its buffers. Pixel stages use the C++ parallel
// algorithms per row unless RenderOptions::parallel is false; results do not depend on it.

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/core/version.hpp>
#include <lucent/engine/camera_color.hpp>
#include <lucent/engine/demosaic.hpp>
#include <lucent/engine/display_rendering.hpp>
#include <lucent/engine/dng_opcode_processing.hpp>
#include <lucent/engine/sensor.hpp>
#include <lucent/image/color_encoding.hpp>
#include <lucent/image/color_math.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/image/pixel_buffer.hpp>
#include <lucent/raw/decoded_raw.hpp>
#include <lucent/raw/decoder_registry.hpp>
#include <lucent/raw/raw_decoder.hpp>

namespace lucent::engine {

// Schema of RenderSettings, the seed of the W6 recipe. Persisted with every render record.
//   1.0: W2 initial settings.
//   1.1: apply_dng_opcodes.
inline constexpr core::ContractVersion kRenderSettingsVersion{1, 1};

// Version of the ordered stage graph (bump when stages are added, removed or reordered).
//   2: the DNG opcode stages (dng_opcode_list1..3).
inline constexpr std::uint32_t kRenderPipelineVersion = 2;

// Default bound on the pipeline's own pixel memory (4 GiB): a 45 MP render with a retained
// scene-linear copy and a rotation needs about 2 GB. Process-wide admission control is W7's.
inline constexpr std::uint64_t kDefaultRenderMemoryBudget = std::uint64_t{4} << 30;

// Display rendering operation (persisted; append only).
enum class DisplayRendering : std::uint8_t {
    NeutralV0 = 1,  // display_rendering.hpp; provisional
};

[[nodiscard]] std::string_view to_string(DisplayRendering rendering) noexcept;  // "neutral_v0"

// Everything that decides the rendered pixels of a decoded image, besides the source and the
// build. A versioned value type: readers reject a newer schema (validate()).
struct RenderSettings {
    core::ContractVersion schema = kRenderSettingsVersion;
    // White balance (sensor.hpp): request, multipliers for Custom (per channel index), policy.
    WhiteBalanceRequest white_balance = WhiteBalanceRequest::AsShot;
    std::array<double, 4> custom_multipliers{};
    HighlightPolicy highlights = HighlightPolicy::ClipToLowestSaturation;
    // User exposure in EV on top of the file's baseline (NeutralV0Parameters::exposure_ev).
    double exposure_ev = 0.0;
    // Apply the DNG BaselineExposure tag when present.
    bool apply_baseline_exposure = true;
    DisplayRendering rendering = DisplayRendering::NeutralV0;
    double highlight_knee = kNeutralV0DefaultKnee;  // NeutralV0Parameters::knee
    // Bake the file's orientation into the pixels (false leaves stored orientation; reported).
    bool apply_orientation = true;
    // Apply the DNG opcodes Lucent supports (ADR 0014). false is for reference comparisons with
    // decoders that do not apply opcodes (LibRaw / rawpy): every opcode that would have been
    // applied is then recorded as disabled and becomes an interpretation issue of the report, so
    // such a render is never reported as complete.
    bool apply_dng_opcodes = true;

    // VersionMismatch (engine.render.settings) when `schema` is not readable by this build
    // (core::check_readable); InvalidArgument for unknown enum values, a non-finite or
    // out-of-range exposure (|EV| <= 16) or a knee outside (0, 1). Custom multipliers are checked
    // when white balance is resolved.
    [[nodiscard]] Status validate() const;
};

struct StageRecord;

// Execution options: they never change the pixels.
struct RenderOptions {
    raw::DecodeLimits decode_limits;  // render_file only
    image::AllocationLimits limits;   // per buffer
    std::uint64_t memory_budget_bytes = kDefaultRenderMemoryBudget;
    // Also return the oriented scene-linear Rec.2020 image before display rendering
    // (RenderResult::scene_linear), for analysis and reference comparisons.
    bool keep_scene_linear = false;
    bool parallel = true;
    // Progress observer: called on the rendering thread after each stage has completed and its
    // record was appended to the report, before the pipeline checks cancellation and starts the
    // next stage. Must not throw. Cancelling the render's token from it stops the render before
    // the next stage (ErrorCode::Cancelled, no result).
    std::function<void(const StageRecord&)> on_stage_completed;
};

// Persisted stage identifiers (append only).
enum class RenderStage : std::uint8_t {
    Decode = 1,
    Normalize = 2,
    WhiteBalance = 3,
    Demosaic = 4,
    CameraColor = 5,
    WorkingSpace = 6,
    DisplayRendering = 7,
    Orientation = 8,
    DngOpcodeList1 = 9,   // DNG OpcodeList1 on the stored samples
    DngOpcodeList2 = 10,  // DNG OpcodeList2 on the normalised samples
    DngOpcodeList3 = 11,  // DNG OpcodeList3 on the demosaiced image
};

[[nodiscard]] std::string_view to_string(RenderStage stage) noexcept;  // "decode", "white_balance", ...

// A stage parameter: flag, integer, real, text, or a list of reals (per-channel values).
using ParameterValue = std::variant<bool, std::int64_t, double, std::string, std::vector<double>>;

struct StageParameter {
    std::string name;  // snake_case
    ParameterValue value;
};

// One executed stage.
struct StageRecord {
    RenderStage stage = RenderStage::Decode;
    std::string algorithm;      // stable identifier, e.g. "malvar-he-cutler-2004"
    std::uint32_t version = 0;  // semantic version of that algorithm
    bool provisional = false;   // placeholder semantics (Neutral v0)
    // Encoding of the stage's input; std::nullopt for decode, which reads a file.
    std::optional<image::ColorEncoding> input;
    image::ColorEncoding output;
    image::Size input_size;   // pixels of the input image (0 x 0 for decode)
    image::Size output_size;  // pixels of the output image
    std::vector<StageParameter> parameters;
    double seconds = 0.0;  // wall-clock time of the stage
    // Samples whose value the stage limited: white balance, samples above the clip level;
    // display rendering, 3 per pixel whose max(R, G, B) reached the white point. 0 elsewhere.
    std::uint64_t clipped_samples = 0;
};

// Estimated peak of the pipeline's own pixel buffers, in bytes (row padding included). Decoder
// scratch memory beyond one copy of the raw frame, the parallel runtime and the exporter are not
// included.
struct MemoryEstimate {
    std::uint64_t raw_frame_bytes = 0;   // decoded frame (Lucent's copy)
    std::uint64_t sensor_bytes = 0;      // normalised float sensor data
    std::uint64_t mask_bytes = 0;        // saturation mask
    std::uint64_t rgb_bytes = 0;         // one 3-channel float image of the visible area
    std::uint64_t decode_peak = 0;       // decoder buffer + Lucent's copy (2 x raw frame)
    std::uint64_t normalize_peak = 0;    // raw frame + sensor + mask
    std::uint64_t demosaic_peak = 0;     // sensor + mask + RGB (mosaic routes)
    std::uint64_t color_peak = 0;        // RGB (+ the monochrome input while it is expanded)
    std::uint64_t rendering_peak = 0;    // RGB + retained scene-linear copy
    std::uint64_t orientation_peak = 0;  // + one oriented copy (non-Normal orientations)
    // DNG opcodes: a List 2 TrimBounds copies the sensor image and mask, a List 3 warp or
    // TrimBounds one RGB image (0 without such opcodes or when opcode processing is off).
    std::uint64_t opcode_peak = 0;
    std::uint64_t peak_bytes = 0;        // maximum of the peaks above
    RenderStage peak_stage = RenderStage::Decode;
    std::uint64_t budget_bytes = 0;  // RenderOptions::memory_budget_bytes
};

// The complete record of one render (the forward-compatible seed of W6 render records).
struct RenderReport {
    std::uint32_t pipeline_version = kRenderPipelineVersion;
    RenderSettings settings;
    std::vector<StageRecord> stages;     // in execution order
    // Of the decoded image, copied from its metadata; with RenderSettings::apply_dng_opcodes
    // false, every disabled opcode is added as an issue.
    raw::Interpretation interpretation;
    // Developer-facing English: everything that makes this render less than a faithful
    // interpretation (missing operations, baseline routes, ignored tags, ...).
    std::vector<std::string> warnings;
    MemoryEstimate memory;
    NormalizationRecord normalization;
    WhiteBalanceRecord white_balance;
    DemosaicRecord demosaic;
    CameraColorRecord color;
    image::Matrix3d working_space_matrix;  // XYZ -> Rec.2020 as applied
    NeutralV0Record rendering;
    image::Orientation orientation = image::Orientation::Normal;  // baked into the pixels
    DngOpcodeReport dng_opcodes;  // every DNG opcode and geometry change (ADR 0014)
    image::Size output_size;
    double total_seconds = 0.0;
};

struct RenderResult {
    // display_linear_rec2020(), upright: the exporter's input.
    ColorImage display;
    // Scene-linear Rec.2020 / D65 before display rendering, upright; only with
    // RenderOptions::keep_scene_linear.
    std::optional<ColorImage> scene_linear;
    raw::RawMetadata metadata;  // of the decoded image (stage AfterUnpack)
    RenderReport report;
};

// Estimates the pipeline's pixel memory for an image described by `metadata` (probe or decode
// metadata). Errors (engine.render.memory): LimitExceeded when a buffer would exceed
// options.limits (as its allocation would); InvalidArgument for empty geometry.
[[nodiscard]] Expected<MemoryEstimate> estimate_render_memory(const raw::RawMetadata& metadata,
                                                              const RenderSettings& settings,
                                                              const RenderOptions& options);

// Renders decoded data (stages 2-8), consuming it. Errors: those of settings.validate(), of the
// memory check (LimitExceeded, engine.render.memory) and of every stage (see their headers),
// passed through unchanged; Internal for an encoding-edge mismatch.
[[nodiscard]] Expected<RenderResult> render_decoded(raw::DecodedRaw&& decoded, const RenderSettings& settings,
                                                    const RenderOptions& options = {},
                                                    const core::CancellationToken& cancel = {});

// Selects a decoder for `path` from `registry`, checks the memory estimate from its probe,
// decodes image `subimage` (required for multi-image files, see raw::DecodeOptions) and renders
// it (stages 1-8). Errors: those of the registry and decoder (raw_decoder.hpp) and of
// render_decoded().
[[nodiscard]] Expected<RenderResult> render_file(const raw::DecoderRegistry& registry,
                                                 const std::filesystem::path& path,
                                                 std::optional<std::uint32_t> subimage,
                                                 const RenderSettings& settings,
                                                 const RenderOptions& options = {},
                                                 const core::CancellationToken& cancel = {});

}  // namespace lucent::engine
