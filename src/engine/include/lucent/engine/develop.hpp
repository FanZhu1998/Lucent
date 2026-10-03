#pragma once

// The develop pipeline (blueprint section 5 and section 4 "quick preview, then refine"):
// DevelopSettings v1 rendered from a decoded RAW image, split for interactive editing into a
// cached stage one and a fast stage two.
//
// Stage one (render_stage_one), done once per image and cached by the caller:
//     decode -> normalise -> white balance -> demosaic -> orientation [-> proxy downscale]
// producing white-balanced camera RGB (or monochrome), upright, at full resolution or as a proxy
// (make_proxy: area-average box downscale in linear camera RGB to a requested long edge).
// Two kinds (StageOneKind), both balanced with StageOneRequest::white_balance (as shot by
// default) before demosaicing:
//   * Interactive: highlights NOT clipped (HighlightPolicy::None), so that stage two can apply
//     any other white balance and clip at that balance's own level.
//   * Export: the default highlight clip (dcraw "highlight 0", sensor.hpp), exactly as the W2
//     render path does. The export path (render_develop) is this stage one with the settings'
//     white balance at full resolution, followed by stage two.
//
// Stage two (render_stage_two), per settings change, in this order (develop_operations.hpp):
//   1. white balance, camera colour, working space, exposure (one fused pass):
//        c = min(v * d, clip) per camera channel, rgb = gain * M_ws * M_cam(target) * c, where
//        d = target sample multipliers / stage one's (1 for the export kind), clip = the target's
//        lowest white-balanced saturation level (sensor.hpp), M_cam = resolve_camera_color() for
//        the target white (DNG: calibration interpolated at the target's temperature),
//        M_ws = XYZ -> Rec.2020 / D65 (Bradford), gain = 2^(exposure_ev + DNG BaselineExposure);
//   2. local highlights/shadows (guided filter; radius in full-image pixels, scaled with the
//      proxy: local_tone_radius());
//   3. contrast and 4. tone curve (display-lightness curves, conjugated with the base look);
//   5. saturation and vibrance;
//   6. base look "neutral" v1 with white point W = gain * clip * neutral_scale (a fully clipped
//      neutral is display white at every exposure) -> display-linear Rec.2020 / D65
//      (display_linear_rec2020(), the exporter's input).
// An operation whose settings are the identity is skipped (recorded with applied = false).
//
// Preview versus export (measured in develop_fixture_test.cpp and stated in the W6 report):
// the interactive path differs from the export render only where (a) demosaicing saw a
// different channel balance (edges, when the settings' white balance differs from the one the
// cached stage one was balanced with: Malvar-He-Cutler mixes channels), (b) the highlight clip
// happens after instead of before demosaicing (within 2 pixels of clipped sensor sites), and,
// for proxies, (c) downscaling does not commute with the non-linear operations (base look,
// curves, local tone at a scaled radius). Quick then refine (blueprint section 4): while the
// white balance is being dragged, stage two corrects the cached stage one (quick); once it
// settles, the caller rebuilds the interactive stage one at that white balance (refine), which
// removes (a). With the cache at the settings' white balance and at full resolution, only (b)
// remains.
//
// Errors: the stages' own errors pass through unchanged (sensor.hpp, demosaic.hpp,
// camera_color.hpp, develop_operations.hpp, white_balance_model.hpp); settings are validated
// (DevelopSettings::validate()); LimitExceeded (engine.develop.memory) when the estimate exceeds
// DevelopOptions::memory_budget_bytes; Cancelled between and inside stages.
//
// Thread-safety: reentrant. A StageOneImage may be read by several stage-two renders at once.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/camera_color.hpp>
#include <lucent/engine/demosaic.hpp>
#include <lucent/engine/develop_operations.hpp>
#include <lucent/engine/develop_settings.hpp>
#include <lucent/engine/render.hpp>
#include <lucent/engine/sensor.hpp>
#include <lucent/engine/white_balance_model.hpp>
#include <lucent/image/color_encoding.hpp>
#include <lucent/image/color_math.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/image/pixel_buffer.hpp>
#include <lucent/raw/decoded_raw.hpp>
#include <lucent/raw/decoder_registry.hpp>

namespace lucent::engine {

// Version of the develop pipeline's stage graph (bump when stages are added, removed or
// reordered, or the split between the stages changes).
inline constexpr std::uint32_t kDevelopPipelineVersion = 1;
// Version of the proxy downscale semantics.
inline constexpr std::uint32_t kBoxDownscaleVersion = 1;

// How stage one balanced its samples (persisted; append only).
enum class StageOneKind : std::uint8_t {
    Interactive = 1,  // as shot, highlights not clipped; any white balance in stage two
    Export = 2,       // the recipe's white balance before demosaicing, highlights clipped
};

[[nodiscard]] std::string_view to_string(StageOneKind kind) noexcept;  // "interactive", "export"

struct StageOneRequest {
    StageOneKind kind = StageOneKind::Interactive;
    // The white balance applied before demosaicing: the settings' for Export; for Interactive, as
    // shot or the white balance the user settled on (see "Quick then refine" above).
    WhiteBalanceSetting white_balance = AsShotWhiteBalance{};
    // When > 0 and below the full image's long edge, the result is a proxy with this long edge
    // (the full-resolution image is released as soon as the proxy exists).
    std::int32_t proxy_long_edge = 0;
};

// Execution options: they never change the pixels.
struct DevelopOptions {
    raw::DecodeLimits decode_limits;  // *_file functions only
    image::AllocationLimits limits;   // per buffer
    std::uint64_t memory_budget_bytes = kDefaultRenderMemoryBudget;
    bool parallel = true;
};

// The proxy downscale (make_proxy).
struct DownscaleRecord {
    std::uint32_t version = kBoxDownscaleVersion;
    image::Size from;
    image::Size to;
    double seconds = 0.0;
};

// Stage one's output: white-balanced camera RGB (or monochrome), upright. Move-only.
struct StageOneImage {
    image::ImageBuffer<float> pixels;  // 3 channels (camera R, G, B) or 1 (monochrome)
    // camera_native_linear() or raw::camera_native_monochrome().
    image::ColorEncoding encoding = image::ColorEncoding::camera_native_linear();
    StageOneKind kind = StageOneKind::Interactive;
    image::Size full_size;  // upright size of the full-resolution image
    // Long edge of `pixels` divided by the long edge of full_size (1 at full resolution).
    double scale = 1.0;
    raw::RawMetadata metadata;  // of the decoded image (stage AfterUnpack)
    NormalizationRecord normalization;
    WhiteBalanceRecord white_balance;  // as applied to the samples (policy, clip level)
    RgbMultipliers sample_multipliers{1.0, 1.0, 1.0};  // camera R, G, B multipliers applied
    DemosaicRecord demosaic;
    image::Orientation orientation = image::Orientation::Normal;  // baked into the pixels
    std::vector<StageRecord> stages;                               // decode .. orientation
    std::optional<DownscaleRecord> downscale;                      // proxies only
    // Developer-facing English: everything that makes the result less than a faithful
    // interpretation (interpretation issues, baseline routes, unbalanced data, ...).
    std::vector<std::string> warnings;
    double seconds = 0.0;  // wall time of stage one (decode included)

    [[nodiscard]] std::uint64_t byte_size() const noexcept { return pixels.byte_size(); }
};

// Persisted develop-operation identifiers (append only).
enum class DevelopOperation : std::uint8_t {
    WhiteBalance = 1,  // correction relative to stage one and the highlight clip
    CameraColor = 2,   // camera -> XYZ for the target white
    WorkingSpace = 3,  // XYZ -> scene-linear Rec.2020 / D65
    Exposure = 4,
    LocalTone = 5,  // highlights and shadows
    Contrast = 6,
    ToneCurve = 7,
    Color = 8,  // saturation and vibrance
    BaseLook = 9,
};

[[nodiscard]] std::string_view to_string(DevelopOperation operation) noexcept;  // "white_balance", ...

struct OperationRecord {
    DevelopOperation operation = DevelopOperation::WhiteBalance;
    std::string algorithm;      // stable identifier, e.g. "guided-filter-local-tone"
    std::uint32_t version = 0;  // semantic version
    bool applied = false;       // false: the identity at these settings, skipped
    std::vector<StageParameter> parameters;
    double seconds = 0.0;  // operations 1-4 share one fused pass, recorded on WhiteBalance
};

// The complete record of one stage-two render (with the stage-one records it used).
struct DevelopReport {
    std::uint32_t pipeline_version = kDevelopPipelineVersion;
    DevelopSettings settings;
    StageOneKind stage_one_kind = StageOneKind::Interactive;
    image::Size full_size;
    image::Size output_size;
    double scale = 1.0;
    // White balance: absolute target multipliers (camera R, G, B, green = 1), those applied to
    // the samples overall, and stage two's correction factor d.
    RgbMultipliers target_multipliers{1.0, 1.0, 1.0};
    RgbMultipliers sample_multipliers{1.0, 1.0, 1.0};
    RgbMultipliers correction{1.0, 1.0, 1.0};
    std::optional<TemperatureTint> target_temperature;  // when the calibration permits it
    double clip_level = 0.0;                            // in target-balanced camera units
    CameraColorRecord color;                            // for the target white
    image::Matrix3d working_space_matrix;               // XYZ -> Rec.2020
    double baseline_exposure_ev = 0.0;                  // DNG BaselineExposure applied
    double gain = 1.0;                                  // 2^(exposure_ev + baseline)
    double scene_white = 1.0;                           // clip_level * neutral_scale
    double white = 1.0;                                 // W = gain * scene_white
    std::optional<LocalToneRecord> local_tone;
    BaseLookRecord base_look;
    std::vector<OperationRecord> operations;  // in execution order
    // Stage one, copied from the StageOneImage.
    NormalizationRecord normalization;
    WhiteBalanceRecord stage_one_white_balance;
    DemosaicRecord demosaic;
    image::Orientation orientation = image::Orientation::Normal;
    std::vector<StageRecord> stage_one_stages;
    std::optional<DownscaleRecord> downscale;
    std::vector<std::string> warnings;
    double stage_one_seconds = 0.0;
    double stage_two_seconds = 0.0;
};

struct DevelopResult {
    ColorImage display;  // display_linear_rec2020(), upright: the exporter's input
    DevelopReport report;
};

// Estimated peaks of the develop path's own pixel buffers, bytes.
struct DevelopMemoryEstimate {
    MemoryEstimate render;              // W2 per-stage estimate (decode .. orientation)
    std::uint64_t stage_one_peak = 0;   // render stage one (with the proxy downscale)
    std::uint64_t stage_one_bytes = 0;  // the StageOneImage kept (full or proxy)
    std::uint64_t stage_two_bytes = 0;  // new allocations of stage two at that size
    std::uint64_t export_peak = 0;      // render_develop: max(stage one, stage one kept + stage two)
};

// Size of a proxy whose long edge is `long_edge` (aspect ratio kept, each side rounded, at least
// 1); `full` itself when long_edge <= 0 or >= its long edge.
[[nodiscard]] image::Size proxy_size(image::Size full, std::int32_t long_edge) noexcept;

// Guided-filter radius of the local tone operation for an image whose upright full-resolution
// size is `full` rendered at `scale`: max(1, round(kLocalToneRadiusFraction * long edge * scale)).
[[nodiscard]] std::int32_t local_tone_radius(image::Size full, double scale) noexcept;

// Area-average downscale: output pixel (i, j) is the mean of the source over the rectangle
// [i fx, (i + 1) fx) x [j fy, (j + 1) fy), fx = src width / dst width, fy likewise, with
// fractional pixels weighted by their covered area (separable, sums in double). dst must not be
// larger than src in either dimension and must have the same channel count.
// Errors (engine.develop.downscale): InvalidArgument; OutOfMemory; Cancelled.
[[nodiscard]] Status box_downscale(image::ConstImageView<float> src, image::ImageView<float> dst,
                                   const core::CancellationToken& cancel = {}, bool parallel = true);

// Memory estimate for `metadata` (probe or decode metadata). Errors as estimate_render_memory().
[[nodiscard]] Expected<DevelopMemoryEstimate> estimate_develop_memory(const raw::RawMetadata& metadata,
                                                                      const StageOneRequest& request,
                                                                      const DevelopOptions& options = {});

// Stage one from decoded data, consuming it. Errors: Unsupported (engine.develop.white_balance)
// for a white balance the calibration cannot express (Kelvin without a CCT mapping, any colour
// balance on monochrome data); those of the stages.
[[nodiscard]] Expected<StageOneImage> render_stage_one(raw::DecodedRaw&& decoded, const StageOneRequest& request,
                                                       const DevelopOptions& options = {},
                                                       const core::CancellationToken& cancel = {});

// Selects a decoder, checks the memory estimate from its probe, decodes image `subimage` and
// runs stage one. Errors: those of the registry and decoder, and of render_stage_one().
[[nodiscard]] Expected<StageOneImage> render_stage_one_file(const raw::DecoderRegistry& registry,
                                                            const std::filesystem::path& path,
                                                            std::optional<std::uint32_t> subimage,
                                                            const StageOneRequest& request,
                                                            const DevelopOptions& options = {},
                                                            const core::CancellationToken& cancel = {});

// A proxy of `source` whose long edge is `long_edge` (box_downscale; records and metadata are
// copied, scale updated). InvalidArgument unless 1 <= long_edge < the source's long edge.
[[nodiscard]] Expected<StageOneImage> make_proxy(const StageOneImage& source, std::int32_t long_edge,
                                                 const DevelopOptions& options = {},
                                                 const core::CancellationToken& cancel = {});

// Stage two on a cached stage one (not modified; the result is a new image of its size).
[[nodiscard]] Expected<DevelopResult> render_stage_two(const StageOneImage& stage_one,
                                                       const DevelopSettings& settings,
                                                       const DevelopOptions& options = {},
                                                       const core::CancellationToken& cancel = {});

// Stage two consuming `stage_one` (its buffer becomes the result; on failure it may have been
// moved from).
[[nodiscard]] Expected<DevelopResult> render_stage_two(StageOneImage&& stage_one, const DevelopSettings& settings,
                                                       const DevelopOptions& options = {},
                                                       const core::CancellationToken& cancel = {});

// The export render: stage one of kind Export with the settings' white balance at full
// resolution, then stage two, consuming `decoded`.
[[nodiscard]] Expected<DevelopResult> render_develop(raw::DecodedRaw&& decoded, const DevelopSettings& settings,
                                                     const DevelopOptions& options = {},
                                                     const core::CancellationToken& cancel = {});

// render_develop() for a file (decoder selection, probe memory check, decode).
[[nodiscard]] Expected<DevelopResult> render_develop_file(const raw::DecoderRegistry& registry,
                                                          const std::filesystem::path& path,
                                                          std::optional<std::uint32_t> subimage,
                                                          const DevelopSettings& settings,
                                                          const DevelopOptions& options = {},
                                                          const core::CancellationToken& cancel = {});

}  // namespace lucent::engine
