// The first render path: stage orchestration, records and memory estimate (render.hpp).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <lucent/engine/orientation.hpp>
#include <lucent/engine/render.hpp>

namespace lucent::engine {

namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kWhereSettings = "engine.render.settings";
constexpr const char* kWhereMemory = "engine.render.memory";
constexpr const char* kWhereEdge = "engine.render.edge";
constexpr const char* kWhereStage = "engine.render.stage";
constexpr double kMaxExposureEv = 16.0;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

std::vector<double> list(const auto& values, std::size_t count) {
    std::vector<double> out;
    for (std::size_t i = 0; i < count && i < values.size(); ++i) {
        out.push_back(static_cast<double>(values[i]));
    }
    return out;
}

std::vector<double> matrix_list(const image::Matrix3d& m) {
    std::vector<double> out;
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            out.push_back(m(i, j));
        }
    }
    return out;
}

StageParameter param(std::string name, ParameterValue value) {
    return StageParameter{std::move(name), std::move(value)};
}

image::ColorEncoding encoding_of(const raw::RawImage& image) {
    return std::visit([](const auto& frame) { return frame.encoding; }, image);
}

image::Size size_of(const raw::RawImage& image) {
    const raw::PixelsShape shape = raw::shape_of(raw::pixels_of(image));
    return image::Size{shape.width, shape.height};
}

// Appends a stage after checking the encoding edge to its predecessor.
Status push_stage(RenderReport& report, StageRecord record, const image::ColorEncoding& expected_output) {
    if (record.output != expected_output) {
        return fail(ErrorCode::Internal,
                    std::format("stage {} produced {} but declares {}", to_string(record.stage),
                                record.output.to_string(), expected_output.to_string()),
                    kWhereEdge);
    }
    if (!report.stages.empty()) {
        const StageRecord& previous = report.stages.back();
        if (!record.input || *record.input != previous.output) {
            return fail(ErrorCode::Internal,
                        std::format("stage {} expects {} but stage {} produced {}", to_string(record.stage),
                                    record.input ? record.input->to_string() : std::string("no input"),
                                    to_string(previous.stage), previous.output.to_string()),
                        kWhereEdge);
        }
    }
    report.stages.push_back(std::move(record));
    return ok();
}

Expected<std::uint64_t> buffer_bytes(std::int64_t width, std::int64_t height, std::int32_t channels,
                                     std::size_t sample_size, const image::AllocationLimits& limits,
                                     const char* what) {
    if (width > std::numeric_limits<std::int32_t>::max() ||
        height > std::numeric_limits<std::int32_t>::max()) {
        return fail(ErrorCode::LimitExceeded, std::format("the {} is {}x{} pixels", what, width, height),
                    kWhereMemory);
    }
    auto layout = image::detail::plan_buffer_layout(
        static_cast<std::int32_t>(width), static_cast<std::int32_t>(height), channels, sample_size, limits);
    if (!layout) {
        return fail(layout.error().code,
                    std::format("the {} cannot be allocated: {}", what, layout.error().message),
                    kWhereMemory);
    }
    return static_cast<std::uint64_t>(layout->byte_size);
}

// Warnings that follow from the metadata alone.
void metadata_warnings(const raw::RawMetadata& metadata, const RenderSettings& settings,
                       std::vector<std::string>& warnings) {
    if (metadata.interpretation.status != raw::InterpretationStatus::Complete) {
        warnings.push_back(std::format("interpretation is '{}': this render is not a verified interpretation",
                                       raw::to_string(metadata.interpretation.status)));
        for (const raw::InterpretationIssue& issue : metadata.interpretation.issues) {
            warnings.push_back(std::format("{}: {}", raw::to_string(issue.kind), issue.message));
        }
    }
    if (metadata.geometry.pixel_aspect != 1.0) {
        warnings.push_back(
            std::format("non-square pixels (aspect {}) are not corrected; the image is distorted",
                        metadata.geometry.pixel_aspect));
    }
    if (metadata.dng && metadata.dng->raw_ifd.default_crop_origin &&
        metadata.dng->raw_ifd.default_crop_size) {
        const auto& origin = *metadata.dng->raw_ifd.default_crop_origin;
        const auto& size = *metadata.dng->raw_ifd.default_crop_size;
        const image::PixelRect& visible = metadata.geometry.visible_area;
        if (origin[0] != 0.0 || origin[1] != 0.0 || size[0] != static_cast<double>(visible.width) ||
            size[1] != static_cast<double>(visible.height)) {
            warnings.push_back(
                std::format("DNG DefaultCrop (origin {}, {}; size {} x {}) is not applied; the whole visible "
                            "area is rendered",
                            origin[0], origin[1], size[0], size[1]));
        }
    }
    if (!settings.apply_orientation && metadata.geometry.orientation != image::Orientation::Normal) {
        warnings.push_back(
            std::format("the file's orientation '{}' is not applied (settings); pixels stay in stored "
                        "order",
                        image::to_string(metadata.geometry.orientation)));
    }
}

class Pipeline {
public:
    Pipeline(const RenderSettings& settings, const RenderOptions& options,
             const core::CancellationToken& cancel)
        : settings_(settings), options_(options), cancel_(cancel) {}

    Expected<RenderResult> run(raw::DecodedRaw&& decoded, std::optional<StageRecord> decode_stage,
                               Clock::time_point start) {
        RenderResult result;
        RenderReport& report = result.report;
        report.settings = settings_;
        report.interpretation = decoded.metadata.interpretation;
        metadata_warnings(decoded.metadata, settings_, report.warnings);

        auto estimate = estimate_render_memory(decoded.metadata, settings_, options_);
        if (!estimate) {
            return Unexpected(std::move(estimate.error()));
        }
        report.memory = *estimate;
        if (auto budget = check_budget(*estimate); !budget) {
            return Unexpected(std::move(budget.error()));
        }
        if (decode_stage) {
            if (auto pushed = finish_stage(report, std::move(*decode_stage), encoding_of(decoded.image));
                !pushed) {
                return Unexpected(std::move(pushed.error()));
            }
        }
        if (auto cancelled = cancel_.check(kWhereStage); !cancelled) {
            return Unexpected(std::move(cancelled.error()));
        }

        // ---- 2. normalize -------------------------------------------------------------------------
        auto t = Clock::now();
        const image::ColorEncoding raw_encoding = encoding_of(decoded.image);
        const image::Size raw_size = size_of(decoded.image);
        NormalizeOptions normalize_options;
        normalize_options.limits = options_.limits;
        normalize_options.parallel = options_.parallel;
        auto sensor = normalize_sensor(decoded, normalize_options, cancel_);
        if (!sensor) {
            return Unexpected(std::move(sensor.error()));
        }
        decoded.image = raw::MosaicFrame{};  // release the raw frame
        {
            const NormalizationRecord& n = sensor->normalization;
            StageRecord s;
            s.stage = RenderStage::Normalize;
            s.algorithm = "black-white-normalisation";
            s.version = n.version;
            s.input = raw_encoding;
            s.output = sensor->encoding;
            s.input_size = raw_size;
            s.output_size = sensor->pixels.size();
            const std::size_t channels = sensor->channel_colors.size();
            s.parameters = {
                param("levels_provenance", std::string(raw::to_string(n.levels_provenance))),
                param("black_max", list(n.black_max, channels)),
                param("white", n.white),
                param("saturation_threshold", list(n.saturation_threshold, channels)),
                param("saturation_level", list(n.saturation_level, channels)),
                param("black_pattern_applied", n.black_pattern_applied),
                param("column_deltas_applied", n.column_deltas_applied),
                param("row_deltas_applied", n.row_deltas_applied),
                param("saturated_samples", static_cast<std::int64_t>(n.saturated_samples)),
                param("source_area", std::format("{},{} {}x{}", n.source_area.x, n.source_area.y,
                                                 n.source_area.width, n.source_area.height)),
            };
            s.seconds = seconds_since(t);
            report.normalization = n;
            const image::ColorEncoding expected = sensor->encoding;
            if (auto pushed = finish_stage(report, std::move(s), expected); !pushed) {
                return Unexpected(std::move(pushed.error()));
            }
        }
        if (auto cancelled = cancel_.check(kWhereStage); !cancelled) {
            return Unexpected(std::move(cancelled.error()));
        }

        // ---- 3. white balance ---------------------------------------------------------------------
        t = Clock::now();
        WhiteBalanceOptions wb_options;
        wb_options.request = settings_.white_balance;
        wb_options.custom_multipliers = settings_.custom_multipliers;
        wb_options.highlights = settings_.highlights;
        auto wb = resolve_white_balance(decoded.metadata, wb_options);
        if (!wb) {
            return Unexpected(std::move(wb.error()));
        }
        const image::ColorEncoding before_wb = sensor->encoding;
        if (auto applied =
                apply_white_balance(*sensor, *wb, settings_.highlights, cancel_, options_.parallel);
            !applied) {
            return Unexpected(std::move(applied.error()));
        }
        {
            const WhiteBalanceRecord& w = *sensor->white_balance;
            StageRecord s;
            s.stage = RenderStage::WhiteBalance;
            s.algorithm = "channel-multipliers";
            s.version = w.version;
            s.input = before_wb;
            s.output = sensor->encoding;
            s.input_size = sensor->pixels.size();
            s.output_size = sensor->pixels.size();
            s.parameters = {
                param("request", static_cast<std::int64_t>(settings_.white_balance)),
                param("source", std::string(to_string(w.applied.source))),
                param("multipliers", list(w.applied.multipliers, sensor->channel_colors.size())),
                param("basis", w.applied.basis),
                param("highlights", std::string(to_string(w.highlights))),
                param("clip_level", static_cast<double>(w.clip_level)),
            };
            s.clipped_samples = w.clipped_samples;
            s.seconds = seconds_since(t);
            report.white_balance = w;
            if (w.applied.source == WhiteBalanceSource::Unavailable) {
                report.warnings.push_back(
                    "no usable white balance in the file: the image is not white balanced");
            }
            if (auto pushed = finish_stage(report, std::move(s), before_wb); !pushed) {
                return Unexpected(std::move(pushed.error()));
            }
        }
        if (auto cancelled = cancel_.check(kWhereStage); !cancelled) {
            return Unexpected(std::move(cancelled.error()));
        }

        // ---- 4. demosaic --------------------------------------------------------------------------
        t = Clock::now();
        const image::ColorEncoding mosaic_encoding = sensor->encoding;
        const image::Size mosaic_size = sensor->pixels.size();
        DemosaicOptions demosaic_options;
        demosaic_options.limits = options_.limits;
        demosaic_options.parallel = options_.parallel;
        auto demosaiced = demosaic(std::move(*sensor), demosaic_options, cancel_);
        if (!demosaiced) {
            return Unexpected(std::move(demosaiced.error()));
        }
        demosaiced->saturation = SaturationMask{};  // counted in the normalisation record
        {
            const DemosaicRecord& d = demosaiced->demosaic;
            StageRecord s;
            s.stage = RenderStage::Demosaic;
            s.algorithm = std::string(d.algorithm);
            s.version = d.algorithm_version;
            s.input = mosaic_encoding;
            s.output = demosaiced->encoding;
            s.input_size = mosaic_size;
            s.output_size = demosaiced->pixels.size();
            s.parameters = {
                param("route", std::string(to_string(d.route))),
                param("maturity", std::string(to_string(d.maturity))),
                param("border_handling", std::string(d.border_handling)),
                param("cfa", d.cfa.empty() ? std::string() : d.cfa.to_string()),
            };
            s.seconds = seconds_since(t);
            report.demosaic = d;
            if (d.maturity == RouteMaturity::Baseline) {
                report.warnings.push_back(
                    std::format("demosaicing route '{}' is baseline quality (not quality-qualified): expect "
                                "softness and colour "
                                "artefacts on fine detail",
                                to_string(d.route)));
            }
            const image::ColorEncoding expected = demosaiced->encoding;
            if (auto pushed = finish_stage(report, std::move(s), expected); !pushed) {
                return Unexpected(std::move(pushed.error()));
            }
        }
        if (auto cancelled = cancel_.check(kWhereStage); !cancelled) {
            return Unexpected(std::move(cancelled.error()));
        }

        // ---- 5. camera colour ---------------------------------------------------------------------
        t = Clock::now();
        if (!demosaiced->white_balance) {
            return fail(ErrorCode::Internal, "the demosaiced image lost its white-balance record",
                        kWhereStage);
        }
        auto color = resolve_camera_color(decoded.metadata, *demosaiced->white_balance);
        if (!color) {
            return Unexpected(std::move(color.error()));
        }
        const image::ColorEncoding camera_encoding = demosaiced->encoding;
        const image::Size camera_size = demosaiced->pixels.size();
        ColorStageOptions color_options;
        color_options.limits = options_.limits;
        color_options.parallel = options_.parallel;
        auto xyz = apply_camera_color(std::move(*demosaiced), *color, color_options, cancel_);
        if (!xyz) {
            return Unexpected(std::move(xyz.error()));
        }
        {
            const CameraColorRecord& c = *color;
            StageRecord s;
            s.stage = RenderStage::CameraColor;
            s.algorithm = std::format("camera-to-xyz-{}", to_string(c.path));
            s.version = c.version;
            s.input = camera_encoding;
            s.output = xyz->encoding;
            s.input_size = camera_size;
            s.output_size = xyz->pixels.size();
            s.parameters = {
                param("calibration_path", std::string(to_string(c.path))),
                param("provenance", std::string(raw::to_string(c.provenance))),
                param("camera_to_xyz", matrix_list(c.camera_to_xyz)),
                param("xyz_white", std::string(image::to_string(c.xyz_white))),
                param("multipliers", list(c.multipliers, 3)),
                param("neutral_scale", c.neutral_scale),
                param("basis", c.basis),
            };
            if (c.dng) {
                s.parameters.push_back(
                    param("white_xy", std::vector<double>{c.dng->white_xy.x, c.dng->white_xy.y}));
                s.parameters.push_back(param("white_cct_kelvin", c.dng->white_cct_kelvin));
                s.parameters.push_back(param("weight_matrix1", c.dng->weight_matrix1));
            }
            s.seconds = seconds_since(t);
            report.color = c;
            const image::ColorEncoding expected = xyz_encoding(c.xyz_white);
            if (auto pushed = finish_stage(report, std::move(s), expected); !pushed) {
                return Unexpected(std::move(pushed.error()));
            }
        }
        if (auto cancelled = cancel_.check(kWhereStage); !cancelled) {
            return Unexpected(std::move(cancelled.error()));
        }

        // ---- 6. working space ---------------------------------------------------------------------
        t = Clock::now();
        const image::ColorEncoding xyz_enc = xyz->encoding;
        auto working = convert_to_working_space(*xyz, color_options, cancel_);
        if (!working) {
            return Unexpected(std::move(working.error()));
        }
        {
            StageRecord s;
            s.stage = RenderStage::WorkingSpace;
            s.algorithm = "xyz-to-rec2020-d65-bradford";
            s.version = kWorkingSpaceVersion;
            s.input = xyz_enc;
            s.output = xyz->encoding;
            s.input_size = xyz->pixels.size();
            s.output_size = xyz->pixels.size();
            s.parameters = {
                param("matrix", matrix_list(*working)),
                param("adaptation",
                      std::string(xyz_enc.white_point == image::WhitePoint::D65 ? "none" : "bradford")),
            };
            s.seconds = seconds_since(t);
            report.working_space_matrix = *working;
            if (auto pushed =
                    finish_stage(report, std::move(s), image::ColorEncoding::scene_linear_rec2020_d65());
                !pushed) {
                return Unexpected(std::move(pushed.error()));
            }
        }
        if (options_.keep_scene_linear) {
            auto copy = xyz->pixels.clone();
            if (!copy) {
                return Unexpected(std::move(copy.error()));
            }
            result.scene_linear = ColorImage{std::move(*copy), xyz->encoding};
        }
        if (auto cancelled = cancel_.check(kWhereStage); !cancelled) {
            return Unexpected(std::move(cancelled.error()));
        }

        // ---- 7. display rendering (Neutral v0) --------------------------------------------------------
        t = Clock::now();
        NeutralV0Parameters neutral;
        neutral.exposure_ev = settings_.exposure_ev;
        std::optional<double> baseline;
        if (decoded.metadata.dng) {
            baseline = decoded.metadata.dng->info.color.baseline_exposure;
        }
        neutral.baseline_exposure_ev = settings_.apply_baseline_exposure && baseline ? *baseline : 0.0;
        neutral.knee = settings_.highlight_knee;
        const WhiteBalanceRecord& w = report.white_balance;
        neutral.scene_white = w.highlights == HighlightPolicy::ClipToLowestSaturation &&
                                      std::isfinite(w.clip_level) && w.clip_level > 0.0f
                                  ? static_cast<double>(w.clip_level) * report.color.neutral_scale
                                  : 1.0;
        const image::ColorEncoding scene_encoding = xyz->encoding;
        auto rendered = render_neutral_v0(*xyz, neutral, cancel_, options_.parallel);
        if (!rendered) {
            return Unexpected(std::move(rendered.error()));
        }
        {
            const NeutralV0Record& r = *rendered;
            StageRecord s;
            s.stage = RenderStage::DisplayRendering;
            s.algorithm = "neutral-v0";
            s.version = r.version;
            s.provisional = true;
            s.input = scene_encoding;
            s.output = xyz->encoding;
            s.input_size = xyz->pixels.size();
            s.output_size = xyz->pixels.size();
            s.parameters = {
                param("exposure_ev", r.parameters.exposure_ev),
                param("baseline_exposure_ev", r.parameters.baseline_exposure_ev),
                param("baseline_exposure_in_file",
                      baseline ? ParameterValue(*baseline) : ParameterValue(false)),
                param("gain", r.gain),
                param("knee", r.parameters.knee),
                param("scene_white", r.parameters.scene_white),
                param("white", r.white),
                param("curve", std::string(to_string(r.curve))),
                param("exponent", r.exponent),
                param("lift", std::vector<double>(r.lift.begin(), r.lift.end())),
                param("compressed_pixels", static_cast<std::int64_t>(r.compressed_pixels)),
                param("lifted_pixels", static_cast<std::int64_t>(r.lifted_pixels)),
                param("limited_pixels", static_cast<std::int64_t>(r.limited_pixels)),
                param("negative_samples", static_cast<std::int64_t>(r.negative_samples)),
            };
            s.clipped_samples = r.limited_pixels * 3;
            s.seconds = seconds_since(t);
            report.rendering = r;
            if (auto pushed = finish_stage(report, std::move(s), display_linear_rec2020()); !pushed) {
                return Unexpected(std::move(pushed.error()));
            }
        }
        if (auto cancelled = cancel_.check(kWhereStage); !cancelled) {
            return Unexpected(std::move(cancelled.error()));
        }

        // ---- 8. orientation ---------------------------------------------------------------------
        t = Clock::now();
        const image::Orientation orientation =
            settings_.apply_orientation ? decoded.metadata.geometry.orientation : image::Orientation::Normal;
        const image::Size stored_size = xyz->pixels.size();
        auto upright = apply_orientation(std::move(xyz->pixels), orientation, options_.limits, cancel_,
                                         options_.parallel);
        if (!upright) {
            return Unexpected(std::move(upright.error()));
        }
        if (result.scene_linear) {
            auto scene_upright = apply_orientation(std::move(result.scene_linear->pixels), orientation,
                                                   options_.limits, cancel_, options_.parallel);
            if (!scene_upright) {
                return Unexpected(std::move(scene_upright.error()));
            }
            result.scene_linear->pixels = std::move(*scene_upright);
        }
        result.display = ColorImage{std::move(*upright), xyz->encoding};
        {
            StageRecord s;
            s.stage = RenderStage::Orientation;
            s.algorithm = "exif-orientation";
            s.version = 1;
            s.input = result.display.encoding;
            s.output = result.display.encoding;
            s.input_size = stored_size;
            s.output_size = result.display.pixels.size();
            s.parameters = {
                param("orientation", std::string(image::to_string(orientation))),
                param("orientation_exif", static_cast<std::int64_t>(image::to_exif(orientation))),
                param("file_orientation_exif",
                      static_cast<std::int64_t>(image::to_exif(decoded.metadata.geometry.orientation))),
                param("applied_to_scene_linear", result.scene_linear.has_value()),
            };
            s.seconds = seconds_since(t);
            report.orientation = orientation;
            report.output_size = result.display.pixels.size();
            if (auto pushed = finish_stage(report, std::move(s), display_linear_rec2020()); !pushed) {
                return Unexpected(std::move(pushed.error()));
            }
        }
        report.total_seconds = seconds_since(start);
        result.metadata = std::move(decoded.metadata);
        return result;
    }

    Status check_budget(const MemoryEstimate& estimate) const {
        if (estimate.peak_bytes > options_.memory_budget_bytes) {
            return fail(
                ErrorCode::LimitExceeded,
                std::format(
                    "rendering needs about {} MiB of pixel memory at stage '{}', above the budget of {} "
                    "MiB",
                    estimate.peak_bytes >> 20, to_string(estimate.peak_stage),
                    options_.memory_budget_bytes >> 20),
                kWhereMemory);
        }
        return ok();
    }

private:
    // Appends a completed stage (checking its encoding edge) and reports it to the observer.
    Status finish_stage(RenderReport& report, StageRecord record, const image::ColorEncoding& expected_output) {
        if (auto pushed = push_stage(report, std::move(record), expected_output); !pushed) {
            return pushed;
        }
        if (options_.on_stage_completed) {
            options_.on_stage_completed(report.stages.back());
        }
        return ok();
    }

    const RenderSettings& settings_;
    const RenderOptions& options_;
    const core::CancellationToken& cancel_;
};

}  // namespace

std::string_view to_string(DisplayRendering rendering) noexcept {
    switch (rendering) {
        case DisplayRendering::NeutralV0:
            return "neutral_v0";
    }
    return "invalid";
}

std::string_view to_string(RenderStage stage) noexcept {
    switch (stage) {
        case RenderStage::Decode:
            return "decode";
        case RenderStage::Normalize:
            return "normalize";
        case RenderStage::WhiteBalance:
            return "white_balance";
        case RenderStage::Demosaic:
            return "demosaic";
        case RenderStage::CameraColor:
            return "camera_color";
        case RenderStage::WorkingSpace:
            return "working_space";
        case RenderStage::DisplayRendering:
            return "display_rendering";
        case RenderStage::Orientation:
            return "orientation";
    }
    return "invalid";
}

Status RenderSettings::validate() const {
    if (auto readable = core::check_readable(schema, kRenderSettingsVersion, "render settings"); !readable) {
        return fail(ErrorCode::VersionMismatch, readable.error().message, kWhereSettings);
    }
    const auto wb = static_cast<std::uint8_t>(white_balance);
    if (wb < static_cast<std::uint8_t>(WhiteBalanceRequest::AsShot) ||
        wb > static_cast<std::uint8_t>(WhiteBalanceRequest::Custom)) {
        return fail(ErrorCode::InvalidArgument, std::format("unknown white-balance request {}", wb),
                    kWhereSettings);
    }
    const auto hl = static_cast<std::uint8_t>(highlights);
    if (hl < static_cast<std::uint8_t>(HighlightPolicy::ClipToLowestSaturation) ||
        hl > static_cast<std::uint8_t>(HighlightPolicy::None)) {
        return fail(ErrorCode::InvalidArgument, std::format("unknown highlight policy {}", hl),
                    kWhereSettings);
    }
    if (rendering != DisplayRendering::NeutralV0) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("unknown display rendering {}", static_cast<std::uint32_t>(rendering)),
                    kWhereSettings);
    }
    if (!std::isfinite(exposure_ev) || std::abs(exposure_ev) > kMaxExposureEv) {
        return fail(
            ErrorCode::InvalidArgument,
            std::format("exposure {} EV must be finite and within +/-{} EV", exposure_ev, kMaxExposureEv),
            kWhereSettings);
    }
    if (!std::isfinite(highlight_knee) || !(highlight_knee > 0.0) || !(highlight_knee < 1.0)) {
        return fail(
            ErrorCode::InvalidArgument,
            std::format("the highlight knee must lie strictly between 0 and 1 (got {})", highlight_knee),
            kWhereSettings);
    }
    return ok();
}

Expected<MemoryEstimate> estimate_render_memory(const raw::RawMetadata& metadata,
                                                const RenderSettings& settings,
                                                const RenderOptions& options) {
    const raw::SensorGeometry& g = metadata.geometry;
    const image::PixelRect& visible = g.visible_area;
    if (g.raw_width == 0 || g.raw_height == 0 || visible.empty()) {
        return fail(ErrorCode::InvalidArgument, "the metadata has empty geometry", kWhereMemory);
    }
    const raw::RepresentationInfo& rep = metadata.representation;
    const std::size_t raw_sample = rep.sample_encoding == raw::SampleEncoding::Float32 ? 4 : 2;
    const std::int32_t raw_channels = std::max<std::int32_t>(1, rep.channels);
    const std::int32_t sensor_channels = rep.kind == raw::RawRepresentation::LinearColor ? 3 : 1;

    MemoryEstimate e;
    e.budget_bytes = options.memory_budget_bytes;
    auto raw_bytes =
        buffer_bytes(g.raw_width, g.raw_height, raw_channels, raw_sample, options.limits, "raw frame");
    if (!raw_bytes) {
        return Unexpected(std::move(raw_bytes.error()));
    }
    auto sensor_bytes = buffer_bytes(visible.width, visible.height, sensor_channels, sizeof(float),
                                     options.limits, "sensor image");
    if (!sensor_bytes) {
        return Unexpected(std::move(sensor_bytes.error()));
    }
    auto rgb_bytes =
        buffer_bytes(visible.width, visible.height, 3, sizeof(float), options.limits, "RGB image");
    if (!rgb_bytes) {
        return Unexpected(std::move(rgb_bytes.error()));
    }
    e.raw_frame_bytes = *raw_bytes;
    e.sensor_bytes = *sensor_bytes;
    e.mask_bytes = static_cast<std::uint64_t>(visible.width) * static_cast<std::uint64_t>(visible.height);
    e.rgb_bytes = *rgb_bytes;
    const std::uint64_t scene = options.keep_scene_linear ? e.rgb_bytes : 0;
    const image::Orientation orientation =
        settings.apply_orientation ? g.orientation : image::Orientation::Normal;

    e.decode_peak = 2 * e.raw_frame_bytes;
    e.normalize_peak = e.raw_frame_bytes + e.sensor_bytes + e.mask_bytes;
    e.demosaic_peak =
        e.sensor_bytes + e.mask_bytes + (rep.kind == raw::RawRepresentation::Mosaic ? e.rgb_bytes : 0);
    e.color_peak = e.rgb_bytes + (rep.kind == raw::RawRepresentation::Monochrome ? e.sensor_bytes : 0);
    e.rendering_peak = e.rgb_bytes + scene;
    e.orientation_peak = e.rendering_peak + (orientation != image::Orientation::Normal ? e.rgb_bytes : 0);

    const std::array<std::pair<std::uint64_t, RenderStage>, 6> peaks = {{
        {e.decode_peak, RenderStage::Decode},
        {e.normalize_peak, RenderStage::Normalize},
        {e.demosaic_peak, RenderStage::Demosaic},
        {e.color_peak, RenderStage::CameraColor},
        {e.rendering_peak, RenderStage::DisplayRendering},
        {e.orientation_peak, RenderStage::Orientation},
    }};
    for (const auto& [bytes, stage] : peaks) {
        if (bytes > e.peak_bytes) {
            e.peak_bytes = bytes;
            e.peak_stage = stage;
        }
    }
    return e;
}

Expected<RenderResult> render_decoded(raw::DecodedRaw&& decoded, const RenderSettings& settings,
                                      const RenderOptions& options, const core::CancellationToken& cancel) {
    const auto start = Clock::now();
    if (auto valid = settings.validate(); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    Pipeline pipeline(settings, options, cancel);
    return pipeline.run(std::move(decoded), std::nullopt, start);
}

Expected<RenderResult> render_file(const raw::DecoderRegistry& registry, const std::filesystem::path& path,
                                   std::optional<std::uint32_t> subimage, const RenderSettings& settings,
                                   const RenderOptions& options, const core::CancellationToken& cancel) {
    const auto start = Clock::now();
    if (auto valid = settings.validate(); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    if (auto cancelled = cancel.check(kWhereStage); !cancelled) {
        return Unexpected(std::move(cancelled.error()));
    }
    raw::ProbeOptions probe_options;
    probe_options.subimage = subimage;
    probe_options.limits = options.decode_limits;
    probe_options.describe_subimages = false;
    auto selection = registry.select(path, probe_options, cancel);
    if (!selection) {
        return Unexpected(std::move(selection.error()));
    }
    // Refuse before any pixel is unpacked when the estimate from the probe exceeds the budget.
    auto estimate = estimate_render_memory(selection->probe.metadata, settings, options);
    if (!estimate) {
        return Unexpected(std::move(estimate.error()));
    }
    Pipeline pipeline(settings, options, cancel);
    if (auto budget = pipeline.check_budget(*estimate); !budget) {
        return Unexpected(std::move(budget.error()));
    }
    raw::DecodeOptions decode_options;
    decode_options.subimage = subimage;
    decode_options.limits = options.decode_limits;
    const auto t = Clock::now();
    auto decoded = selection->decoder->decode(path, decode_options, cancel);
    if (!decoded) {
        return Unexpected(std::move(decoded.error()));
    }
    StageRecord s;
    s.stage = RenderStage::Decode;
    const raw::DecoderInfo& info = decoded->metadata.decoder;
    s.algorithm = std::format("{}-decoder", selection->decoder->id());
    s.version = 1;
    s.output = encoding_of(decoded->image);
    s.output_size = size_of(decoded->image);
    s.parameters = {
        param("adapter", std::string(selection->decoder->id())),
        param("library", info.library),
        param("library_version", info.library_version),
        param("unpacker", info.unpacker),
        param("contract", decoded->contract.to_string()),
        param("subimage", static_cast<std::int64_t>(decoded->metadata.subimage_index)),
        param("subimage_count", static_cast<std::int64_t>(decoded->metadata.subimage_count)),
        param("representation", std::string(raw::to_string(decoded->metadata.representation.kind))),
        param("sample_encoding",
              std::string(raw::to_string(decoded->metadata.representation.sample_encoding))),
    };
    s.seconds = seconds_since(t);
    return pipeline.run(std::move(*decoded), std::move(s), start);
}

}  // namespace lucent::engine
