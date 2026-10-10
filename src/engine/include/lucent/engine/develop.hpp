#pragma once

// The develop pipeline (blueprint section 5 and section 4 "quick preview, then refine"):
// DevelopSettings (v3, and v1 and v2 values) rendered from a decoded RAW image, split for
// interactive editing into a cached stage one and a fast stage two.
//
// Stage one (render_stage_one), done once per image and cached by the caller:
//     decode [-> DNG OpcodeList1] -> normalise [-> DNG OpcodeList2] -> white balance -> demosaic
//     [-> DNG OpcodeList3] -> orientation [-> proxy downscale]
// (the DNG opcode stages run when the file has those lists: dng_opcode_processing.hpp, ADR 0014;
// every kind of stage one applies the same opcodes, so previews and exports agree)
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
// Stage two (render_stage_two), per settings change, in this order (lens_correction.hpp,
// develop_operations.hpp, chroma_denoise.hpp, local_adjustments.hpp, develop_geometry.hpp):
//   0. lens-profile corrections (DevelopSettings::lens_correction, operation version 1; ADR 0023),
//      fused into the first pass: in linear camera RGB, each output pixel's camera channels are
//      read at their own source positions (distortion and lateral chromatic aberration, Lensfun's
//      model, Catmull-Rom) and divided by the vignetting there; the plan comes from
//      DevelopOptions::lens_database (none: nothing is corrected) and is recorded in
//      DevelopReport::lens_correction. Corrections the file carries are never applied again;
//      settings older than version 3 correct nothing;
//   1. white balance, camera colour, working space, exposure (one fused pass):
//        c = min(v * d, clip) per camera channel, rgb = gain * M_ws * M_cam(target) * c, where
//        d = target sample multipliers / stage one's (1 for the export kind), clip = the target's
//        lowest white-balanced saturation level (sensor.hpp), M_cam = resolve_camera_color() for
//        the target white (DNG: calibration interpolated at the target's temperature),
//        M_ws = XYZ -> Rec.2020 / D65 (Bradford), gain = 2^(exposure_ev + DNG BaselineExposure);
//   2. guided chroma denoising (luminance-preserving; radius in full-image pixels, scaled with the
//      proxy: chroma_denoise_radius());
//   3. local highlights/shadows (guided filter; radius in full-image pixels, scaled with the
//      proxy: local_tone_radius());
//   4. local adjustments: brush, linear and radial masks rasterised at the render scale from their
//      normalised definitions, with their local controls;
//   5. contrast and 6. tone curve (display-lightness curves, conjugated with the base look);
//   7. saturation and vibrance;
//   8. base look "neutral" v1 with white point W = gain * clip * neutral_scale (a fully clipped
//      neutral is display white at every exposure) -> display-linear Rec.2020 / D65
//      (display_linear_rec2020(), the exporter's input);
//   9. geometry: straighten, crop, quarter turns and flip within the geometry's frame: the
//      camera's image area for geometry version 2 (StageOneImage::image_area, image_area.hpp; the
//      identity geometry renders the area), the whole upright image for version 1 or when the area
//      is unknown (DevelopReport::output_size is the result's size, DevelopReport::upright_size the
//      size of the stage-one image).
// Operations 2-8 work on the whole upright image (stage one's frame, the camera's border pixels
// included; after a lens correction, the corrected canvas of the same size), so the crop, the
// rotation and the geometry version never move a mask relative to the content. Masks are defined
// in stage one's (uncorrected) frame and evaluated through the lens correction's point mapping, so
// enabling or disabling a correction does not move them either; GeometryMapping
// (develop_geometry.hpp, geometry_mapping() below) maps mask coordinates to output pixels for
// overlays, through the lens correction when it is given one.
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
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/camera_color.hpp>
#include <lucent/engine/chroma_denoise.hpp>
#include <lucent/engine/demosaic.hpp>
#include <lucent/engine/develop_geometry.hpp>
#include <lucent/engine/develop_operations.hpp>
#include <lucent/engine/develop_settings.hpp>
#include <lucent/engine/dng_opcode_processing.hpp>
#include <lucent/engine/guided_filter.hpp>
#include <lucent/engine/image_area.hpp>
#include <lucent/engine/lens_correction.hpp>
#include <lucent/engine/local_adjustments.hpp>
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
//   2: the DNG opcode stages (opcode processing version kDngOpcodeProcessingVersion 1).
//   3: chroma denoising, local adjustments and geometry in stage two (DevelopSettings v2, ADR 0015).
//   4: lens-profile corrections fused into stage two's first pass (DevelopSettings v3, ADR 0023).
inline constexpr std::uint32_t kDevelopPipelineVersion = 4;
// Version of the proxy downscale semantics.
inline constexpr std::uint32_t kBoxDownscaleVersion = 1;

// Every semantic version of the pipeline's own algorithms that a recipe does not carry (a recipe
// carries its operation and base-look versions): the stage graph, sensor normalisation, white
// balance, the demosaic routes, camera colour, the working space, the guided filter, the proxy
// downscale, DNG opcode processing, the placement of the camera's image area (which geometry
// version 2 renders; ADR 0017) and the lens-profile matching rules (ADR 0023). Pixels persisted
// under a recipe identity (rendered thumbnails) are keyed by these as well, so a newer engine never
// shows an older engine's rendering; they must also be keyed by the lens database's identity
// (LensDatabaseInfo::identity), which is data, not a version of the engine. Append new versions
// here.
inline constexpr std::array<std::uint32_t, 13> kDevelopEngineVersions{
    kDevelopPipelineVersion, kSensorNormalizationVersion, kWhiteBalanceVersion,
    kMalvarHeCutlerVersion,  kXTransBaselineVersion,      kBypassVersion,
    kCameraColorVersion,     kWorkingSpaceVersion,        kGuidedFilterVersion,
    kBoxDownscaleVersion,    kDngOpcodeProcessingVersion, kImageAreaPlacementVersion,
    kLensProfileMatchVersion};

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

inline constexpr std::uint64_t kDefaultStageTwoCacheBytes = std::uint64_t{256} << 20;

// Stage two's scene-linear image after the fused pass and chroma denoising, kept between renders of
// one stage-one image (an edit session's quick frames), so that moving a control that comes after
// the denoising (local tone, masks, curves, colour, geometry) does not repeat it. Used only when
// chroma denoising is applied, by the non-consuming render_stage_two() of a StageOneImage with an
// id; keyed by that id, the white balance, the exposure, the chroma-denoise setting and the lens
// correction's plan (LensCorrectionPlan::revision(); everything the fused pass and the denoising
// depend on). One entry, replaced on every miss; an image larger
// than `max_bytes` is not kept (the default keeps a 2048-pixel proxy's 32 MiB, not a full-resolution
// image's). A hit copies the kept image and is bit-identical to a render without the cache. Its
// memory is the caller's (stats().bytes). Internally synchronised.
class StageTwoCache {
public:
    struct Key {
        std::uint64_t stage_one = 0;  // StageOneImage::id
        WhiteBalanceSetting white_balance = AsShotWhiteBalance{};
        double exposure_ev = 0.0;
        ChromaDenoiseSetting chroma_denoise;
        std::uint64_t lens_plan = 0;  // LensCorrectionPlan::revision() (0: nothing corrected)

        friend bool operator==(const Key&, const Key&) = default;
    };
    struct Entry {
        Key key;
        image::ImageBuffer<float> pixels;  // scene-linear Rec.2020 after chroma denoising
        ChromaDenoiseRecord record;
    };
    struct Stats {
        std::uint64_t bytes = 0;
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
    };

    explicit StageTwoCache(std::uint64_t max_bytes = kDefaultStageTwoCacheBytes) noexcept : max_bytes_(max_bytes) {}
    StageTwoCache(const StageTwoCache&) = delete;
    StageTwoCache& operator=(const StageTwoCache&) = delete;

    // The entry for `key`, or nullptr (counted as a hit or a miss).
    [[nodiscard]] std::shared_ptr<const Entry> find(const Key& key);
    // Replaces the entry (ignored when its pixels exceed max_bytes).
    void insert(std::shared_ptr<const Entry> entry) noexcept;
    [[nodiscard]] std::uint64_t max_bytes() const noexcept { return max_bytes_; }
    void clear() noexcept;
    [[nodiscard]] Stats stats() const;

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const Entry> entry_;
    std::uint64_t max_bytes_ = 0;
    std::uint64_t hits_ = 0;
    std::uint64_t misses_ = 0;
};

// Execution options (they never change the pixels) and the lens database (data that does).
struct DevelopOptions {
    raw::DecodeLimits decode_limits;  // *_file functions only
    image::AllocationLimits limits;   // per buffer
    std::uint64_t memory_budget_bytes = kDefaultRenderMemoryBudget;
    bool parallel = true;
    // Brush-mask rasters kept between stage-two renders of one image (an edit session's); null:
    // rasterised on every render. Its memory is the caller's (MaskRasterCache::stats()).
    std::shared_ptr<MaskRasterCache> mask_cache;
    // The denoised image kept between stage-two renders of one stage-one image (an edit session's
    // proxy); null: no reuse. Its memory is the caller's (StageTwoCache::stats()).
    std::shared_ptr<StageTwoCache> stage_two_cache;
    // The Lensfun database the lens-profile corrections use (lens_correction.hpp; the verified
    // asset, lucent_assets). Unlike the members above it is data that changes pixels: without it
    // nothing is corrected (every plan says "no lens database"), and the report records which
    // database a correction came from. Callers that persist pixels under a recipe identity must key
    // them by its identity too.
    std::shared_ptr<const LensDatabase> lens_database;
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
    // Unique per stage-one image this process made (render_stage_one*, make_proxy), so that caches
    // can recognise its pixels (StageTwoCache); 0 means unknown and is never cached. Code that
    // changes `pixels` of an image with an id must set it to 0.
    std::uint64_t id = 0;
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
    // Every DNG opcode and the geometry changes it made, in stored orientation (ADR 0014).
    DngOpcodeReport dng_opcodes;
    // The camera's image area in the full-resolution upright image (full_size), placed after the
    // DNG opcodes and turned with the camera orientation (image_area.hpp); std::nullopt when the
    // file declares none, or when it cannot be applied (a warning says why). Geometry version 2
    // works within it. Code that cuts `pixels` to part of the image (not a proxy of all of it)
    // must reset it: stage two refuses an area on an image that is not the whole frame.
    std::optional<ImageAreaPlacement> image_area;
    std::optional<DownscaleRecord> downscale;                      // proxies only
    // Developer-facing English: everything that makes the result less than a faithful
    // interpretation (interpretation issues, baseline routes, unbalanced data, ...).
    std::vector<std::string> warnings;
    double seconds = 0.0;  // wall time of stage one (decode included)

    [[nodiscard]] std::uint64_t byte_size() const noexcept { return pixels.byte_size(); }
};

// Persisted develop-operation identifiers (append only). Reports list the operations in execution
// order: LensCorrection (13) first, then WhiteBalance (1) to Geometry (12).
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
    // DevelopSettings v2 (ADR 0015).
    ChromaDenoise = 10,
    LocalAdjustments = 11,  // masks and their local controls
    Geometry = 12,          // straighten, crop, quarter turns, flip
    // DevelopSettings v3 (ADR 0023).
    LensCorrection = 13,  // lens-profile distortion, lateral chromatic aberration and vignetting
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
    image::Size full_size;     // upright size of the full-resolution image
    image::Size upright_size;  // the stage-one image rendered (full_size or a proxy's), before geometry
    image::Size output_size;   // the result, after geometry
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
    // The lens-profile corrections: the plan (profile, match, every correction's decision) and what
    // the render did (always filled; its plan applies nothing for operation version 0, without a
    // database or without a profile).
    LensCorrectionRecord lens_correction;
    std::optional<ChromaDenoiseRecord> chroma_denoise;        // when applied
    std::optional<LocalToneRecord> local_tone;                // when applied
    std::optional<LocalAdjustmentsRecord> local_adjustments;  // when applied
    BaseLookRecord base_look;
    // The geometry operation version rendered (geometry_operation_version() of the settings) and
    // its frame F in the upright render (the camera's image area at the render scale for version 2
    // when the stage one has one, the whole upright image otherwise).
    std::uint32_t geometry_version = kGeometryOperationVersion;
    image::PixelRect geometry_frame;
    std::optional<ImageAreaPlacement> image_area;  // the stage one's (full-resolution upright pixels)
    std::optional<GeometryRecord> geometry;  // when applied
    std::vector<OperationRecord> operations;  // in execution order
    // Stage one, copied from the StageOneImage.
    NormalizationRecord normalization;
    WhiteBalanceRecord stage_one_white_balance;
    DemosaicRecord demosaic;
    image::Orientation orientation = image::Orientation::Normal;
    std::vector<StageRecord> stage_one_stages;
    DngOpcodeReport dng_opcodes;
    std::optional<DownscaleRecord> downscale;
    std::vector<std::string> warnings;
    double stage_one_seconds = 0.0;
    double stage_two_seconds = 0.0;
};

struct DevelopResult {
    // display_linear_rec2020(), upright, after the geometry (cropped and turned): the exporter's
    // input.
    ColorImage display;
    DevelopReport report;
};

// Estimated peaks of the develop path's own pixel buffers, bytes.
struct DevelopMemoryEstimate {
    MemoryEstimate render;              // W2 per-stage estimate (decode .. orientation)
    std::uint64_t stage_one_peak = 0;   // render stage one (with the proxy downscale)
    std::uint64_t stage_one_bytes = 0;  // the StageOneImage kept (full or proxy)
    // New allocations of stage two at that size: its RGB image and the largest operation's working
    // memory (local tone, chroma denoising, local adjustments with each brush raster over its
    // strokes' extent, the geometry's output). Without settings: chroma denoising at
    // kMaxChromaDenoiseRadius and no brush raster.
    std::uint64_t stage_two_bytes = 0;
    // render_develop: max(stage one, stage one kept + stage two). A lens correction that resamples
    // (the plan for the settings, the metadata and DevelopOptions::lens_database; without settings,
    // any profile of the database may) cannot reuse stage one's buffer, so the RGB image is counted
    // next to it.
    std::uint64_t export_peak = 0;
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
// The same for known settings (their chroma-denoise radius, brush masks and highlight/shadow
// masks, and the lens correction planned with options.lens_database: a correction that resamples
// writes a new image and moves the masks' supports), which callers with local adjustments should
// use. Errors also those of plan_lens_correction().
[[nodiscard]] Expected<DevelopMemoryEstimate> estimate_develop_memory(const raw::RawMetadata& metadata,
                                                                      const StageOneRequest& request,
                                                                      const DevelopSettings& settings,
                                                                      const DevelopOptions& options = {});

// Stage one from decoded data, consuming it. Errors: Unsupported (engine.develop.white_balance)
// for a white balance the calibration cannot express (Kelvin without a CCT mapping, any colour
// balance on monochrome data); those of the stages.
[[nodiscard]] Expected<StageOneImage> render_stage_one(raw::DecodedRaw&& decoded, const StageOneRequest& request,
                                                       const DevelopOptions& options = {},
                                                       const core::CancellationToken& cancel = {});

// Selects a decoder, checks the memory estimate from its probe, decodes image `subimage` and
// runs stage one. The check knows no settings (estimate_develop_memory() without them: an export
// request is checked against its export_peak for the worst chroma denoising and no brush raster);
// render_develop_file() checks with its settings instead. Errors: those of the registry and
// decoder, and of render_stage_one().
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

// The geometry mapping stage two renders `settings` with on `image`: GeometryMapping::create() of
// the settings' geometry for the image's size and the frame of their geometry version (version 2
// and an image area: scale_area() of the area at the image's scale; otherwise the whole image).
// This overload knows no lens database, so its mapping has no lens correction (stage two's renders
// with DevelopOptions::lens_database may correct the lens: use the overload with options, or the
// one with the render's report).
// Errors: InvalidArgument (engine.develop.geometry) for an invalid geometry, or an image area on an
// image that is not the whole frame at its scale (its size is not proxy_size() of full_size).
[[nodiscard]] Expected<GeometryMapping> geometry_mapping(const DevelopSettings& settings,
                                                         const StageOneImage& image);

// The same with the lens correction stage two plans for `settings` on `image` with
// options.lens_database (none: as the overload above). Errors: those above and of
// plan_lens_correction().
[[nodiscard]] Expected<GeometryMapping> geometry_mapping(const DevelopSettings& settings, const StageOneImage& image,
                                                         const DevelopOptions& options);

// The mapping a stage-two render used, from its report: the settings, upright, full sizes, the
// image area and the lens correction's geometry (report.lens_correction.plan.geometry).
[[nodiscard]] Expected<GeometryMapping> geometry_mapping(const DevelopReport& report);

// True when `image` is the whole frame at its scale (the full-resolution image or a proxy of all of
// it, made by render_stage_one* or make_proxy): its size is proxy_size() of full_size for its long
// edge and its scale is that long edge over full_size's. A lens correction needs the whole frame
// (its model is centred on it): stage two applies none to a part (recorded as NotApplicable).
[[nodiscard]] bool is_whole_frame(const StageOneImage& image) noexcept;

// The same for an upright render of `render_size` pixels of an image whose full-resolution upright
// size is `full_size` and whose image area (full-resolution upright pixels) is `area` (e.g. from a
// DevelopReport: upright_size or output size, full_size and image_area).
[[nodiscard]] Expected<GeometryMapping> geometry_mapping(const DevelopSettings& settings,
                                                         image::Size render_size, image::Size full_size,
                                                         const std::optional<ImageAreaPlacement>& area);

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

// render_develop() for a file (decoder selection, decode): the probe's memory check is the one
// render_develop() makes, the export_peak of estimate_develop_memory() with `settings`.
[[nodiscard]] Expected<DevelopResult> render_develop_file(const raw::DecoderRegistry& registry,
                                                          const std::filesystem::path& path,
                                                          std::optional<std::uint32_t> subimage,
                                                          const DevelopSettings& settings,
                                                          const DevelopOptions& options = {},
                                                          const core::CancellationToken& cancel = {});

}  // namespace lucent::engine
