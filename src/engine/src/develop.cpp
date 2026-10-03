// The develop pipeline: stage one, proxies, stage two and the export render (develop.hpp).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <lucent/engine/develop.hpp>
#include <lucent/engine/display_rendering.hpp>
#include <lucent/engine/orientation.hpp>

#include "parallel_rows.hpp"
#include "render_warnings.hpp"

namespace lucent::engine {

namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kWhereStage = "engine.develop.stage";
constexpr const char* kWhereMemory = "engine.develop.memory";
constexpr const char* kWhereDownscale = "engine.develop.downscale";
constexpr const char* kWhereTwo = "engine.develop.stage_two";

double seconds_since(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }

StageParameter param(std::string name, ParameterValue value) { return StageParameter{std::move(name), std::move(value)}; }

std::vector<double> rgb_list(const RgbMultipliers& m) { return {m[0], m[1], m[2]}; }

std::vector<double> channel_list(const std::array<double, 4>& values, std::size_t count) {
    std::vector<double> out;
    for (std::size_t i = 0; i < count && i < values.size(); ++i) {
        out.push_back(values[i]);
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

std::int32_t long_edge(image::Size size) noexcept { return std::max(size.width, size.height); }

image::ColorEncoding encoding_of(const raw::RawImage& image) {
    return std::visit([](const auto& frame) { return frame.encoding; }, image);
}

image::Size size_of(const raw::RawImage& image) {
    const raw::PixelsShape shape = raw::shape_of(raw::pixels_of(image));
    return image::Size{shape.width, shape.height};
}

// Camera R, G, B entries of per-channel-index multipliers (Green2 stands in for a missing green).
RgbMultipliers rgb_of(const std::array<double, 4>& channel, const std::vector<image::CfaColor>& colors) {
    RgbMultipliers rgb{0.0, 0.0, 0.0};
    double green2 = 0.0;
    for (std::size_t i = 0; i < colors.size() && i < 4; ++i) {
        switch (colors[i]) {
            case image::CfaColor::Red:
                if (rgb[0] == 0.0) rgb[0] = channel[i];
                break;
            case image::CfaColor::Green:
                if (rgb[1] == 0.0) rgb[1] = channel[i];
                break;
            case image::CfaColor::Green2:
                if (green2 == 0.0) green2 = channel[i];
                break;
            case image::CfaColor::Blue:
                if (rgb[2] == 0.0) rgb[2] = channel[i];
                break;
            default:
                break;
        }
    }
    if (rgb[1] == 0.0) {
        rgb[1] = green2;
    }
    if (colors.size() == 1) {  // monochrome
        return {1.0, 1.0, 1.0};
    }
    return rgb;
}

// Lowest white-balanced saturation level (sensor.hpp, ClipToLowestSaturation) for multipliers per
// channel index.
double clip_level_for(const NormalizationRecord& normalization, const std::array<double, 4>& multipliers,
                      std::size_t channels) {
    double clip = std::numeric_limits<double>::infinity();
    for (std::size_t c = 0; c < channels && c < 4; ++c) {
        if (multipliers[c] > 0.0 && normalization.saturation_level[c] > 0.0f) {
            clip = std::min(clip, static_cast<double>(normalization.saturation_level[c]) * multipliers[c]);
        }
    }
    return clip;
}

Status check_budget(std::uint64_t bytes, std::uint64_t budget, const char* what) {
    if (bytes > budget) {
        return fail(ErrorCode::LimitExceeded,
                    std::format("{} needs about {} MiB of pixel memory, above the budget of {} MiB", what, bytes >> 20,
                                budget >> 20),
                    kWhereMemory);
    }
    return ok();
}

// Stage one alone (interactive) or the whole export render (export: stage two consumes it).
Status check_stage_one_budget(const DevelopMemoryEstimate& estimate, const StageOneRequest& request,
                              const DevelopOptions& options) {
    if (request.kind == StageOneKind::Export) {
        return check_budget(estimate.export_peak, options.memory_budget_bytes, "the export render");
    }
    return check_budget(estimate.stage_one_peak, options.memory_budget_bytes, "stage one");
}

std::uint64_t plane_bytes(image::Size size, std::int32_t channels, const image::AllocationLimits& limits) {
    auto layout = image::detail::plan_buffer_layout(std::max(1, size.width), std::max(1, size.height), channels,
                                                    sizeof(float), limits);
    return layout ? static_cast<std::uint64_t>(layout->byte_size)
                  : static_cast<std::uint64_t>(size.area()) * static_cast<std::uint64_t>(channels) * sizeof(float);
}

// The white balance stage one applies (WhiteBalance per channel index) and its policy.
struct StageOneBalance {
    WhiteBalance white_balance;
    HighlightPolicy highlights = HighlightPolicy::None;
};

Expected<StageOneBalance> stage_one_balance(const raw::RawMetadata& metadata, const StageOneRequest& request) {
    StageOneBalance balance;
    balance.highlights = request.kind == StageOneKind::Interactive ? HighlightPolicy::None
                                                                   : HighlightPolicy::ClipToLowestSaturation;
    if (mode_of(request.white_balance) == WhiteBalanceMode::AsShot) {
        auto resolved = resolve_white_balance(metadata, WhiteBalanceOptions{});
        if (!resolved) {
            return Unexpected(std::move(resolved.error()));
        }
        balance.white_balance = std::move(*resolved);
        return balance;
    }
    auto model = WhiteBalanceModel::create(metadata);
    if (!model) {
        return Unexpected(std::move(model.error()));
    }
    auto absolute = model->target_multipliers(request.white_balance);
    if (!absolute) {
        return Unexpected(std::move(absolute.error()));
    }
    const RgbMultipliers sample = model->sample_multipliers(*absolute);
    balance.white_balance.source = WhiteBalanceSource::Custom;
    balance.white_balance.multipliers = model->channel_multipliers(sample);
    balance.white_balance.basis =
        std::format("{} white balance: absolute camera multipliers ({:.6g}, {:.6g}, {:.6g}), applied to the samples "
                    "as ({:.6g}, {:.6g}, {:.6g})",
                    to_string(mode_of(request.white_balance)), (*absolute)[0], (*absolute)[1], (*absolute)[2],
                    sample[0], sample[1], sample[2]);
    return balance;
}

// Copies everything but the pixels.
StageOneImage copy_records(const StageOneImage& source) {
    StageOneImage out;
    out.encoding = source.encoding;
    out.kind = source.kind;
    out.full_size = source.full_size;
    out.scale = source.scale;
    out.metadata = source.metadata;
    out.normalization = source.normalization;
    out.white_balance = source.white_balance;
    out.sample_multipliers = source.sample_multipliers;
    out.demosaic = source.demosaic;
    out.orientation = source.orientation;
    out.stages = source.stages;
    out.downscale = source.downscale;
    out.warnings = source.warnings;
    out.seconds = source.seconds;
    return out;
}

// ---- Box downscale --------------------------------------------------------------------------------

// For each output index: the first source index, the number of taps and their weights.
struct Taps {
    std::vector<std::int32_t> first;
    std::vector<std::int32_t> count;
    std::vector<std::size_t> offset;
    std::vector<double> weight;
};

Expected<Taps> make_taps(std::int32_t source, std::int32_t target) {
    try {
        Taps taps;
        taps.first.resize(static_cast<std::size_t>(target));
        taps.count.resize(static_cast<std::size_t>(target));
        taps.offset.resize(static_cast<std::size_t>(target));
        const double factor = static_cast<double>(source) / static_cast<double>(target);
        for (std::int32_t j = 0; j < target; ++j) {
            const double a = static_cast<double>(j) * source / target;
            const double b = static_cast<double>(j + 1) * source / target;
            const auto i0 = static_cast<std::int32_t>(std::floor(a));
            const auto i1 = std::min(source, static_cast<std::int32_t>(std::ceil(b)));
            taps.first[static_cast<std::size_t>(j)] = i0;
            taps.offset[static_cast<std::size_t>(j)] = taps.weight.size();
            std::int32_t n = 0;
            for (std::int32_t i = i0; i < i1; ++i) {
                const double overlap = std::min(static_cast<double>(i + 1), b) - std::max(static_cast<double>(i), a);
                if (overlap > 0.0) {
                    taps.weight.push_back(overlap / factor);
                } else {
                    taps.weight.push_back(0.0);
                }
                ++n;
            }
            taps.count[static_cast<std::size_t>(j)] = n;
        }
        return taps;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the downscale taps", kWhereDownscale);
    }
}

// ---- Stage one ------------------------------------------------------------------------------------

Expected<StageOneImage> run_stage_one(raw::DecodedRaw&& decoded, const StageOneRequest& request,
                                      const DevelopOptions& options, const core::CancellationToken& cancel,
                                      std::optional<StageRecord> decode_stage, Clock::time_point start) {
    if (request.proxy_long_edge < 0) {
        return fail(ErrorCode::InvalidArgument, "the proxy long edge must not be negative", kWhereStage);
    }
    if (request.kind != StageOneKind::Interactive && request.kind != StageOneKind::Export) {
        return fail(ErrorCode::InvalidArgument, "unknown stage-one kind", kWhereStage);
    }
    StageOneImage out;
    out.kind = request.kind;
    detail::metadata_warnings(decoded.metadata, true, out.warnings);

    auto balance = stage_one_balance(decoded.metadata, request);
    if (!balance) {
        return Unexpected(std::move(balance.error()));
    }
    auto estimate = estimate_develop_memory(decoded.metadata, request, options);
    if (!estimate) {
        return Unexpected(std::move(estimate.error()));
    }
    if (auto budget = check_stage_one_budget(*estimate, request, options); !budget) {
        return Unexpected(std::move(budget.error()));
    }
    if (decode_stage) {
        out.stages.push_back(std::move(*decode_stage));
    }
    if (auto cancelled = cancel.check(kWhereStage); !cancelled) {
        return Unexpected(std::move(cancelled.error()));
    }

    // ---- normalise ---------------------------------------------------------------------------------
    auto t = Clock::now();
    const image::ColorEncoding raw_encoding = encoding_of(decoded.image);
    const image::Size raw_size = size_of(decoded.image);
    NormalizeOptions normalize_options;
    normalize_options.limits = options.limits;
    normalize_options.parallel = options.parallel;
    auto sensor = normalize_sensor(decoded, normalize_options, cancel);
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
        s.parameters = {
            param("white", n.white),
            param("black_max", channel_list(n.black_max, sensor->channel_colors.size())),
            param("saturated_samples", static_cast<std::int64_t>(n.saturated_samples)),
        };
        s.seconds = seconds_since(t);
        out.normalization = n;
        out.stages.push_back(std::move(s));
    }
    if (auto cancelled = cancel.check(kWhereStage); !cancelled) {
        return Unexpected(std::move(cancelled.error()));
    }

    // ---- white balance -----------------------------------------------------------------------------
    t = Clock::now();
    const image::ColorEncoding before_wb = sensor->encoding;
    if (auto applied = apply_white_balance(*sensor, balance->white_balance, balance->highlights, cancel,
                                           options.parallel);
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
            param("kind", std::string(to_string(request.kind))),
            param("source", std::string(to_string(w.applied.source))),
            param("multipliers", channel_list(w.applied.multipliers, sensor->channel_colors.size())),
            param("highlights", std::string(to_string(w.highlights))),
            param("clip_level", static_cast<double>(w.clip_level)),
            param("basis", w.applied.basis),
        };
        s.clipped_samples = w.clipped_samples;
        s.seconds = seconds_since(t);
        out.white_balance = w;
        out.sample_multipliers = rgb_of(w.applied.multipliers, sensor->channel_colors);
        if (w.applied.source == WhiteBalanceSource::Unavailable) {
            out.warnings.emplace_back("no usable white balance in the file: the image is not white balanced");
        }
        out.stages.push_back(std::move(s));
    }
    if (auto cancelled = cancel.check(kWhereStage); !cancelled) {
        return Unexpected(std::move(cancelled.error()));
    }

    // ---- demosaic ----------------------------------------------------------------------------------
    t = Clock::now();
    const image::ColorEncoding mosaic_encoding = sensor->encoding;
    const image::Size mosaic_size = sensor->pixels.size();
    DemosaicOptions demosaic_options;
    demosaic_options.limits = options.limits;
    demosaic_options.parallel = options.parallel;
    auto demosaiced = demosaic(std::move(*sensor), demosaic_options, cancel);
    if (!demosaiced) {
        return Unexpected(std::move(demosaiced.error()));
    }
    demosaiced->saturation = SaturationMask{};
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
        };
        s.seconds = seconds_since(t);
        out.demosaic = d;
        if (d.maturity == RouteMaturity::Baseline) {
            out.warnings.push_back(std::format("demosaicing route '{}' is baseline quality (not quality-qualified): "
                                               "expect softness and colour artefacts on fine detail",
                                               to_string(d.route)));
        }
        out.stages.push_back(std::move(s));
    }
    if (auto cancelled = cancel.check(kWhereStage); !cancelled) {
        return Unexpected(std::move(cancelled.error()));
    }

    // ---- orientation -------------------------------------------------------------------------------
    t = Clock::now();
    const image::Orientation orientation = decoded.metadata.geometry.orientation;
    const image::Size stored_size = demosaiced->pixels.size();
    auto upright =
        apply_orientation(std::move(demosaiced->pixels), orientation, options.limits, cancel, options.parallel);
    if (!upright) {
        return Unexpected(std::move(upright.error()));
    }
    out.encoding = demosaiced->encoding;
    out.pixels = std::move(*upright);
    out.full_size = out.pixels.size();
    out.orientation = orientation;
    {
        StageRecord s;
        s.stage = RenderStage::Orientation;
        s.algorithm = "exif-orientation";
        s.version = 1;
        s.input = out.encoding;
        s.output = out.encoding;
        s.input_size = stored_size;
        s.output_size = out.full_size;
        s.parameters = {param("orientation", std::string(image::to_string(orientation)))};
        s.seconds = seconds_since(t);
        out.stages.push_back(std::move(s));
    }

    // ---- proxy -------------------------------------------------------------------------------------
    if (request.proxy_long_edge > 0 && request.proxy_long_edge < long_edge(out.full_size)) {
        if (auto cancelled = cancel.check(kWhereStage); !cancelled) {
            return Unexpected(std::move(cancelled.error()));
        }
        t = Clock::now();
        const image::Size target = proxy_size(out.full_size, request.proxy_long_edge);
        auto proxy = image::ImageBuffer<float>::create_uninitialized(target.width, target.height,
                                                                     out.pixels.channels(), options.limits);
        if (!proxy) {
            return Unexpected(std::move(proxy.error()));
        }
        if (auto status = box_downscale(out.pixels.const_view(), proxy->view(), cancel, options.parallel); !status) {
            return Unexpected(std::move(status.error()));
        }
        out.downscale = DownscaleRecord{kBoxDownscaleVersion, out.full_size, target, seconds_since(t)};
        out.pixels = std::move(*proxy);
        out.scale = static_cast<double>(long_edge(target)) / static_cast<double>(long_edge(out.full_size));
    }
    out.metadata = std::move(decoded.metadata);
    out.seconds = seconds_since(start);
    return out;
}

// ---- Stage two ------------------------------------------------------------------------------------

Expected<DevelopResult> run_stage_two(const StageOneImage& one, image::ImageBuffer<float>* reuse,
                                      const DevelopSettings& settings, const DevelopOptions& options,
                                      const core::CancellationToken& cancel) {
    const auto start = Clock::now();
    if (auto valid = settings.validate(); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    const bool mono = one.pixels.channels() == 1;
    if (one.pixels.empty() || (one.pixels.channels() != 3 && !mono) ||
        (mono ? one.encoding != raw::camera_native_monochrome()
              : one.encoding != image::ColorEncoding::camera_native_linear())) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("stage two needs a stage-one image of camera RGB or monochrome samples (got {} with {} "
                                "channels)",
                                one.encoding.to_string(), one.pixels.channels()),
                    kWhereTwo);
    }
    if (!(one.scale > 0.0) || one.scale > 1.0 || one.full_size.empty()) {
        return fail(ErrorCode::InvalidArgument, "the stage-one image has an invalid scale or full size", kWhereTwo);
    }
    const image::Size size = one.pixels.size();
    const raw::RawMetadata& metadata = one.metadata;
    const bool local_tone = settings.highlights != 0.0 || settings.shadows != 0.0;
    {
        std::uint64_t bytes = local_tone ? 3 * plane_bytes(size, 1, options.limits) : 0;
        if (reuse == nullptr || mono) {
            bytes += plane_bytes(size, 3, options.limits);
        }
        if (auto budget = check_budget(bytes, options.memory_budget_bytes, "stage two"); !budget) {
            return Unexpected(std::move(budget.error()));
        }
    }

    DevelopResult result;
    DevelopReport& report = result.report;
    report.settings = settings;
    report.stage_one_kind = one.kind;
    report.full_size = one.full_size;
    report.output_size = size;
    report.scale = one.scale;
    report.normalization = one.normalization;
    report.stage_one_white_balance = one.white_balance;
    report.demosaic = one.demosaic;
    report.orientation = one.orientation;
    report.stage_one_stages = one.stages;
    report.downscale = one.downscale;
    report.warnings = one.warnings;
    report.stage_one_seconds = one.seconds;

    // ---- 1. white balance, camera colour, working space, exposure (fused) -----------------------
    auto t = Clock::now();
    auto model = WhiteBalanceModel::create(metadata);
    if (!model) {
        return Unexpected(std::move(model.error()));
    }
    const WhiteBalanceMode mode = mode_of(settings.white_balance);
    WhiteBalanceRecord target;
    target.highlights = HighlightPolicy::ClipToLowestSaturation;
    if (mode == WhiteBalanceMode::AsShot) {
        auto resolved = resolve_white_balance(metadata, WhiteBalanceOptions{});
        if (!resolved) {
            return Unexpected(std::move(resolved.error()));
        }
        target.applied = std::move(*resolved);
        report.target_multipliers = model->as_shot();
    } else {
        if (mono) {
            return fail(ErrorCode::Unsupported,
                        std::format("{} white balance does not apply to monochrome data", to_string(mode)),
                        "engine.develop.white_balance");
        }
        auto absolute = model->target_multipliers(settings.white_balance);
        if (!absolute) {
            return Unexpected(std::move(absolute.error()));
        }
        report.target_multipliers = *absolute;
        target.applied.source = WhiteBalanceSource::Custom;
        target.applied.multipliers = model->channel_multipliers(model->sample_multipliers(*absolute));
        target.applied.basis = std::format("{} white balance", to_string(mode));
    }
    const std::size_t channel_count = metadata.representation.channel_colors.size();
    report.sample_multipliers = rgb_of(target.applied.multipliers, metadata.representation.channel_colors);
    for (std::size_t c = 0; c < 3; ++c) {
        if (!(one.sample_multipliers[c] > 0.0) || !std::isfinite(one.sample_multipliers[c]) ||
            !(report.sample_multipliers[c] > 0.0) || !std::isfinite(report.sample_multipliers[c])) {
            return fail(ErrorCode::Internal, "white-balance multipliers of camera R, G, B must be finite and positive",
                        kWhereTwo);
        }
        report.correction[c] = mono ? 1.0 : report.sample_multipliers[c] / one.sample_multipliers[c];
    }
    if (model->kelvin_available()) {
        if (auto tt = model->temperature_for(report.target_multipliers)) {
            report.target_temperature = *tt;
        }
    }
    const double clip = clip_level_for(one.normalization, target.applied.multipliers, mono ? 1 : channel_count);
    if (!std::isfinite(clip) || !(clip > 0.0)) {
        return fail(ErrorCode::Internal, "no positive white-balanced saturation level", kWhereTwo);
    }
    const auto clip_f = static_cast<float>(clip);
    report.clip_level = static_cast<double>(clip_f);
    target.clip_level = clip_f;
    auto color = resolve_camera_color(metadata, target);
    if (!color) {
        return Unexpected(std::move(color.error()));
    }
    report.color = *color;
    auto working = image::conversion_matrix(xyz_encoding(color->xyz_white),
                                            image::ColorEncoding::scene_linear_rec2020_d65(),
                                            image::ChromaticAdaptationMethod::Bradford);
    if (!working) {
        return fail(ErrorCode::Internal, "no conversion to the working space: " + working.error().message, kWhereTwo);
    }
    report.working_space_matrix = *working;
    if (metadata.dng && metadata.dng->info.color.baseline_exposure) {
        report.baseline_exposure_ev = *metadata.dng->info.color.baseline_exposure;
    }
    report.gain = std::exp2(settings.exposure_ev + report.baseline_exposure_ev);
    report.scene_white = report.clip_level * color->neutral_scale;
    report.white = report.gain * report.scene_white;
    if (!std::isfinite(report.white) || !(report.white > 0.0)) {
        return fail(ErrorCode::Internal, "the white point is not a finite positive number", kWhereTwo);
    }
    const image::Matrix3d fused = *working * color->camera_to_xyz;
    image::Matrix3d m;
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            m(i, j) = fused(i, j) * report.gain;
        }
    }
    if (!image::is_finite(m)) {
        return fail(ErrorCode::Internal, "the stage-two matrix is not finite", kWhereTwo);
    }

    ColorImage& image = result.display;
    if (reuse != nullptr && !mono) {
        image.pixels = std::move(*reuse);
    } else {
        auto buffer = image::ImageBuffer<float>::create_uninitialized(size.width, size.height, 3, options.limits);
        if (!buffer) {
            return Unexpected(std::move(buffer.error()));
        }
        image.pixels = std::move(*buffer);
    }
    image.encoding = image::ColorEncoding::scene_linear_rec2020_d65();
    {
        const auto f = [&m](std::size_t r, std::size_t c) { return static_cast<float>(m(r, c)); };
        const float m00 = f(0, 0), m01 = f(0, 1), m02 = f(0, 2);
        const float m10 = f(1, 0), m11 = f(1, 1), m12 = f(1, 2);
        const float m20 = f(2, 0), m21 = f(2, 1), m22 = f(2, 2);
        const std::array<float, 3> d = {static_cast<float>(report.correction[0]),
                                        static_cast<float>(report.correction[1]),
                                        static_cast<float>(report.correction[2])};
        // Source rows: the stage-one buffer, or (in place) the output itself.
        const image::ConstImageView<float> src =
            (reuse != nullptr && !mono) ? image::ConstImageView<float>(image.pixels.view()) : one.pixels.const_view();
        const image::ImageView<float> dst = image.pixels.view();
        const std::int32_t width = size.width;
        auto status = detail::for_each_row(size.height, options.parallel, cancel, kWhereTwo, [&](std::int32_t y) {
            const float* s = src.row(y);
            float* o = dst.row(y);
            if (mono) {
                for (std::int32_t x = 0; x < width; ++x, o += 3) {
                    const float v = std::min(s[x], clip_f);
                    o[0] = m00 * v;
                    o[1] = m10 * v;
                    o[2] = m20 * v;
                }
                return;
            }
            for (std::int32_t x = 0; x < width; ++x, s += 3, o += 3) {
                const float r = std::min(s[0] * d[0], clip_f);
                const float g = std::min(s[1] * d[1], clip_f);
                const float b = std::min(s[2] * d[2], clip_f);
                o[0] = m00 * r + m01 * g + m02 * b;
                o[1] = m10 * r + m11 * g + m12 * b;
                o[2] = m20 * r + m21 * g + m22 * b;
            }
        });
        if (!status) {
            return Unexpected(std::move(status.error()));
        }
    }
    const double fused_seconds = seconds_since(t);
    {
        OperationRecord r;
        r.operation = DevelopOperation::WhiteBalance;
        r.algorithm = "camera-multiplier-correction";
        r.version = kWhiteBalanceOperationVersion;
        r.applied = true;
        r.parameters = {
            param("mode", std::string(to_string(mode))),
            param("target_multipliers", rgb_list(report.target_multipliers)),
            param("sample_multipliers", rgb_list(report.sample_multipliers)),
            param("correction", rgb_list(report.correction)),
            param("clip_level", report.clip_level),
            param("calibration_basis", model->basis()),
        };
        if (report.target_temperature) {
            r.parameters.push_back(param("temperature_k", report.target_temperature->temperature_k));
            r.parameters.push_back(param("tint", report.target_temperature->tint));
        }
        r.seconds = fused_seconds;
        report.operations.push_back(std::move(r));
    }
    {
        OperationRecord r;
        r.operation = DevelopOperation::CameraColor;
        r.algorithm = std::format("camera-to-xyz-{}", to_string(color->path));
        r.version = color->version;
        r.applied = true;
        r.parameters = {
            param("camera_to_xyz", matrix_list(color->camera_to_xyz)),
            param("xyz_white", std::string(image::to_string(color->xyz_white))),
            param("neutral_scale", color->neutral_scale),
            param("basis", color->basis),
        };
        report.operations.push_back(std::move(r));
    }
    {
        OperationRecord r;
        r.operation = DevelopOperation::WorkingSpace;
        r.algorithm = "xyz-to-rec2020-d65-bradford";
        r.version = kWorkingSpaceVersion;
        r.applied = true;
        r.parameters = {param("matrix", matrix_list(*working))};
        report.operations.push_back(std::move(r));
    }
    {
        OperationRecord r;
        r.operation = DevelopOperation::Exposure;
        r.algorithm = "scene-linear-gain";
        r.version = kExposureOperationVersion;
        r.applied = report.gain != 1.0;
        r.parameters = {
            param("exposure_ev", settings.exposure_ev),
            param("baseline_exposure_ev", report.baseline_exposure_ev),
            param("gain", report.gain),
        };
        report.operations.push_back(std::move(r));
    }
    if (auto cancelled = cancel.check(kWhereTwo); !cancelled) {
        return Unexpected(std::move(cancelled.error()));
    }

    // ---- 2. local highlights / shadows ------------------------------------------------------------
    {
        OperationRecord r;
        r.operation = DevelopOperation::LocalTone;
        r.algorithm = "guided-filter-local-tone";
        r.version = kLocalToneOperationVersion;
        r.applied = local_tone;
        r.parameters = {param("highlights", settings.highlights), param("shadows", settings.shadows)};
        if (local_tone) {
            t = Clock::now();
            LocalToneParameters p;
            p.highlights = settings.highlights;
            p.shadows = settings.shadows;
            p.white = report.white;
            p.radius = local_tone_radius(one.full_size, one.scale);
            auto applied = apply_local_tone(image, p, options.limits, cancel, options.parallel);
            if (!applied) {
                return Unexpected(std::move(applied.error()));
            }
            report.local_tone = *applied;
            r.parameters.push_back(param("radius", static_cast<std::int64_t>(applied->radius)));
            r.parameters.push_back(
                param("radius_full_image", kLocalToneRadiusFraction * static_cast<double>(long_edge(one.full_size))));
            r.parameters.push_back(param("epsilon", applied->epsilon));
            r.parameters.push_back(param("min_gain_ev", applied->min_gain_ev));
            r.parameters.push_back(param("max_gain_ev", applied->max_gain_ev));
            r.seconds = seconds_since(t);
        }
        report.operations.push_back(std::move(r));
    }
    if (auto cancelled = cancel.check(kWhereTwo); !cancelled) {
        return Unexpected(std::move(cancelled.error()));
    }

    // ---- 3./4. contrast and tone curve ------------------------------------------------------------
    auto base = NeutralV1Curve::create(report.white);
    if (!base) {
        return Unexpected(std::move(base.error()));
    }
    {
        auto contrast = ContrastCurve::create(settings.contrast);
        if (!contrast) {
            return Unexpected(std::move(contrast.error()));
        }
        auto tone = ToneCurve::create(settings.tone_curve);
        if (!tone) {
            return Unexpected(std::move(tone.error()));
        }
        const bool apply_contrast = !contrast->identity();
        const bool apply_tone = !tone->identity();
        t = Clock::now();
        if (apply_contrast || apply_tone) {
            auto applied = apply_lightness_curves(image, *base, apply_contrast ? &*contrast : nullptr,
                                                  apply_tone ? &*tone : nullptr, cancel, options.parallel);
            if (!applied) {
                return Unexpected(std::move(applied.error()));
            }
        }
        const double seconds = seconds_since(t);
        OperationRecord rc;
        rc.operation = DevelopOperation::Contrast;
        rc.algorithm = "lightness-logistic-contrast";
        rc.version = kContrastOperationVersion;
        rc.applied = apply_contrast;
        rc.parameters = {param("contrast", settings.contrast), param("pivot_lightness", kContrastPivot),
                         param("slope_at_pivot", contrast->slope_at_pivot())};
        rc.seconds = apply_contrast ? seconds : 0.0;
        report.operations.push_back(std::move(rc));
        OperationRecord rt;
        rt.operation = DevelopOperation::ToneCurve;
        rt.algorithm = "lightness-fritsch-carlson";
        rt.version = kToneCurveOperationVersion;
        rt.applied = apply_tone;
        std::vector<double> points;
        for (const ToneCurvePoint& p : settings.tone_curve.points) {
            points.push_back(p.x);
            points.push_back(p.y);
        }
        rt.parameters = {param("points_xy", std::move(points))};
        rt.seconds = apply_tone && !apply_contrast ? seconds : 0.0;
        report.operations.push_back(std::move(rt));
    }
    if (auto cancelled = cancel.check(kWhereTwo); !cancelled) {
        return Unexpected(std::move(cancelled.error()));
    }

    // ---- 5. saturation and vibrance ---------------------------------------------------------------
    {
        const bool apply = settings.saturation != 0.0 || settings.vibrance != 0.0;
        OperationRecord r;
        r.operation = DevelopOperation::Color;
        r.algorithm = "luminance-chroma-scale";
        r.version = kColorOperationVersion;
        r.applied = apply;
        r.parameters = {param("saturation", settings.saturation), param("vibrance", settings.vibrance)};
        if (apply) {
            t = Clock::now();
            auto applied = apply_color(image, settings.saturation, settings.vibrance, cancel, options.parallel);
            if (!applied) {
                return Unexpected(std::move(applied.error()));
            }
            r.parameters.push_back(param("negative_samples", static_cast<std::int64_t>(applied->negative_samples)));
            r.seconds = seconds_since(t);
        }
        report.operations.push_back(std::move(r));
    }
    if (auto cancelled = cancel.check(kWhereTwo); !cancelled) {
        return Unexpected(std::move(cancelled.error()));
    }

    // ---- 6. base look and display hand-off --------------------------------------------------------
    {
        t = Clock::now();
        auto applied = apply_base_look(image, *base, cancel, options.parallel);
        if (!applied) {
            return Unexpected(std::move(applied.error()));
        }
        report.base_look = *applied;
        OperationRecord r;
        r.operation = DevelopOperation::BaseLook;
        r.algorithm = "neutral-v1-filmic";
        r.version = kNeutralLookVersion;
        r.applied = true;
        r.parameters = {
            param("id", std::string(kNeutralLookId)),
            param("white", applied->white),
            param("regime", std::string(to_string(applied->regime))),
            param("top_start_ev", applied->top_start_ev),
            param("end_slope", applied->end_slope),
            param("toe_ev", std::vector<double>{applied->toe_start_ev, applied->toe_end_ev}),
            param("limited_pixels", static_cast<std::int64_t>(applied->limited_pixels)),
            param("negative_samples", static_cast<std::int64_t>(applied->negative_samples)),
        };
        r.seconds = seconds_since(t);
        report.operations.push_back(std::move(r));
    }
    report.stage_two_seconds = seconds_since(start);
    return result;
}

}  // namespace

// ---- Public functions -----------------------------------------------------------------------------

std::string_view to_string(StageOneKind kind) noexcept {
    switch (kind) {
        case StageOneKind::Interactive:
            return "interactive";
        case StageOneKind::Export:
            return "export";
    }
    return "invalid";
}

std::string_view to_string(DevelopOperation operation) noexcept {
    switch (operation) {
        case DevelopOperation::WhiteBalance:
            return "white_balance";
        case DevelopOperation::CameraColor:
            return "camera_color";
        case DevelopOperation::WorkingSpace:
            return "working_space";
        case DevelopOperation::Exposure:
            return "exposure";
        case DevelopOperation::LocalTone:
            return "local_tone";
        case DevelopOperation::Contrast:
            return "contrast";
        case DevelopOperation::ToneCurve:
            return "tone_curve";
        case DevelopOperation::Color:
            return "color";
        case DevelopOperation::BaseLook:
            return "base_look";
    }
    return "invalid";
}

image::Size proxy_size(image::Size full, std::int32_t edge) noexcept {
    const std::int32_t full_edge = long_edge(full);
    if (edge <= 0 || full.empty() || edge >= full_edge) {
        return full;
    }
    const double s = static_cast<double>(edge) / static_cast<double>(full_edge);
    const auto side = [&](std::int32_t v) {
        if (v == full_edge) {
            return edge;
        }
        return std::clamp(static_cast<std::int32_t>(std::lround(static_cast<double>(v) * s)), 1, edge);
    };
    return image::Size{side(full.width), side(full.height)};
}

std::int32_t local_tone_radius(image::Size full, double scale) noexcept {
    const double r = kLocalToneRadiusFraction * static_cast<double>(long_edge(full)) * scale;
    return std::max<std::int32_t>(1, static_cast<std::int32_t>(std::lround(r)));
}

Status box_downscale(image::ConstImageView<float> src, image::ImageView<float> dst,
                     const core::CancellationToken& cancel, bool parallel) {
    if (src.empty() || dst.empty() || src.channels() != dst.channels() || dst.width() > src.width() ||
        dst.height() > src.height()) {
        return fail(ErrorCode::InvalidArgument,
                    "the downscale needs non-empty views with equal channel counts and a destination no larger than "
                    "the source",
                    kWhereDownscale);
    }
    const std::int32_t channels = src.channels();
    auto horizontal = make_taps(src.width(), dst.width());
    if (!horizontal) {
        return Unexpected(std::move(horizontal.error()));
    }
    auto vertical = make_taps(src.height(), dst.height());
    if (!vertical) {
        return Unexpected(std::move(vertical.error()));
    }
    auto temp = image::ImageBuffer<float>::create_uninitialized(dst.width(), src.height(), channels);
    if (!temp) {
        return fail(temp.error().code, "downscale buffer: " + temp.error().message, kWhereDownscale);
    }
    const image::ImageView<float> tmp = temp->view();
    const Taps& hx = *horizontal;
    const Taps& vy = *vertical;
    if (auto status = detail::for_each_row(src.height(), parallel, cancel, kWhereDownscale,
                                           [&](std::int32_t y) {
                                               const float* s = src.row(y);
                                               float* o = tmp.row(y);
                                               std::array<double, 4> acc{};
                                               for (std::int32_t j = 0; j < dst.width(); ++j) {
                                                   const auto ju = static_cast<std::size_t>(j);
                                                   acc.fill(0.0);
                                                   const double* w = hx.weight.data() + hx.offset[ju];
                                                   const float* p = s + static_cast<std::ptrdiff_t>(hx.first[ju]) *
                                                                            channels;
                                                   for (std::int32_t k = 0; k < hx.count[ju]; ++k, p += channels) {
                                                       for (std::int32_t c = 0; c < channels; ++c) {
                                                           acc[static_cast<std::size_t>(c)] += w[k] * p[c];
                                                       }
                                                   }
                                                   for (std::int32_t c = 0; c < channels; ++c) {
                                                       o[static_cast<std::ptrdiff_t>(j) * channels + c] =
                                                           static_cast<float>(acc[static_cast<std::size_t>(c)]);
                                                   }
                                               }
                                           });
        !status) {
        return status;
    }
    const std::int32_t row_samples = dst.width() * channels;
    constexpr std::int32_t kChunk = 64;
    return detail::for_each_row(dst.height(), parallel, cancel, kWhereDownscale, [&](std::int32_t i) {
        const auto iu = static_cast<std::size_t>(i);
        float* o = dst.row(i);
        const double* w = vy.weight.data() + vy.offset[iu];
        std::array<double, kChunk> acc{};
        for (std::int32_t first = 0; first < row_samples; first += kChunk) {
            const std::int32_t count = std::min(kChunk, row_samples - first);
            acc.fill(0.0);
            for (std::int32_t t = 0; t < vy.count[iu]; ++t) {
                const float* r = tmp.row(vy.first[iu] + t) + first;
                const double wt = w[t];
                for (std::int32_t k = 0; k < count; ++k) {
                    acc[static_cast<std::size_t>(k)] += wt * r[k];
                }
            }
            for (std::int32_t k = 0; k < count; ++k) {
                o[first + k] = static_cast<float>(acc[static_cast<std::size_t>(k)]);
            }
        }
    });
}

Expected<DevelopMemoryEstimate> estimate_develop_memory(const raw::RawMetadata& metadata,
                                                        const StageOneRequest& request,
                                                        const DevelopOptions& options) {
    RenderSettings render_settings;
    RenderOptions render_options;
    render_options.limits = options.limits;
    render_options.memory_budget_bytes = options.memory_budget_bytes;
    auto render = estimate_render_memory(metadata, render_settings, render_options);
    if (!render) {
        return Unexpected(Error{render.error().code, render.error().message, kWhereMemory, render.error().native_code});
    }
    DevelopMemoryEstimate e;
    e.render = *render;
    const MemoryEstimate& r = *render;
    const image::Size full = image::oriented_size(
        image::Size{metadata.geometry.visible_area.width, metadata.geometry.visible_area.height},
        metadata.geometry.orientation);
    const std::int32_t channels = metadata.representation.kind == raw::RawRepresentation::Monochrome ? 1 : 3;
    const std::uint64_t full_bytes = plane_bytes(full, channels, options.limits);
    std::uint64_t one_peak = std::max({r.decode_peak, r.normalize_peak, r.demosaic_peak, r.orientation_peak});
    e.stage_one_bytes = full_bytes;
    if (request.proxy_long_edge > 0 && request.proxy_long_edge < long_edge(full)) {
        const image::Size proxy = proxy_size(full, request.proxy_long_edge);
        const std::uint64_t proxy_bytes = plane_bytes(proxy, channels, options.limits);
        const std::uint64_t temp = plane_bytes(image::Size{proxy.width, full.height}, channels, options.limits);
        one_peak = std::max(one_peak, full_bytes + temp + proxy_bytes);
        e.stage_one_bytes = proxy_bytes;
    }
    e.stage_one_peak = one_peak;
    const image::Size kept = proxy_size(full, request.proxy_long_edge);
    e.stage_two_bytes = plane_bytes(kept, 3, options.limits) + 3 * plane_bytes(kept, 1, options.limits);
    // The export path consumes stage one: RGB is reused in place, monochrome expands to RGB.
    const std::uint64_t in_place = (channels == 1 ? plane_bytes(kept, 3, options.limits) : 0) +
                                   3 * plane_bytes(kept, 1, options.limits);
    e.export_peak = std::max(e.stage_one_peak, e.stage_one_bytes + in_place);
    return e;
}

Expected<StageOneImage> render_stage_one(raw::DecodedRaw&& decoded, const StageOneRequest& request,
                                         const DevelopOptions& options, const core::CancellationToken& cancel) {
    return run_stage_one(std::move(decoded), request, options, cancel, std::nullopt, Clock::now());
}

Expected<StageOneImage> render_stage_one_file(const raw::DecoderRegistry& registry, const std::filesystem::path& path,
                                              std::optional<std::uint32_t> subimage, const StageOneRequest& request,
                                              const DevelopOptions& options, const core::CancellationToken& cancel) {
    const auto start = Clock::now();
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
    auto estimate = estimate_develop_memory(selection->probe.metadata, request, options);
    if (!estimate) {
        return Unexpected(std::move(estimate.error()));
    }
    if (auto budget = check_stage_one_budget(*estimate, request, options); !budget) {
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
        param("subimage", static_cast<std::int64_t>(decoded->metadata.subimage_index)),
    };
    s.seconds = seconds_since(t);
    return run_stage_one(std::move(*decoded), request, options, cancel, std::move(s), start);
}

Expected<StageOneImage> make_proxy(const StageOneImage& source, std::int32_t edge, const DevelopOptions& options,
                                   const core::CancellationToken& cancel) {
    const image::Size from = source.pixels.size();
    if (source.pixels.empty() || edge < 1 || edge >= long_edge(from)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("a proxy needs a long edge between 1 and {} (got {})", long_edge(from) - 1, edge),
                    kWhereDownscale);
    }
    const auto t = Clock::now();
    const image::Size to = proxy_size(from, edge);
    auto pixels =
        image::ImageBuffer<float>::create_uninitialized(to.width, to.height, source.pixels.channels(), options.limits);
    if (!pixels) {
        return Unexpected(std::move(pixels.error()));
    }
    if (auto status = box_downscale(source.pixels.const_view(), pixels->view(), cancel, options.parallel); !status) {
        return Unexpected(std::move(status.error()));
    }
    StageOneImage out = copy_records(source);
    out.pixels = std::move(*pixels);
    out.scale = static_cast<double>(long_edge(to)) / static_cast<double>(long_edge(source.full_size));
    out.downscale = DownscaleRecord{kBoxDownscaleVersion, from, to, seconds_since(t)};
    return out;
}

Expected<DevelopResult> render_stage_two(const StageOneImage& stage_one, const DevelopSettings& settings,
                                         const DevelopOptions& options, const core::CancellationToken& cancel) {
    return run_stage_two(stage_one, nullptr, settings, options, cancel);
}

Expected<DevelopResult> render_stage_two(StageOneImage&& stage_one, const DevelopSettings& settings,
                                         const DevelopOptions& options, const core::CancellationToken& cancel) {
    return run_stage_two(stage_one, &stage_one.pixels, settings, options, cancel);
}

Expected<DevelopResult> render_develop(raw::DecodedRaw&& decoded, const DevelopSettings& settings,
                                       const DevelopOptions& options, const core::CancellationToken& cancel) {
    if (auto valid = settings.validate(); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    StageOneRequest request;
    request.kind = StageOneKind::Export;
    request.white_balance = settings.white_balance;
    auto estimate = estimate_develop_memory(decoded.metadata, request, options);
    if (!estimate) {
        return Unexpected(std::move(estimate.error()));
    }
    if (auto budget = check_budget(estimate->export_peak, options.memory_budget_bytes, "the export render"); !budget) {
        return Unexpected(std::move(budget.error()));
    }
    auto one = render_stage_one(std::move(decoded), request, options, cancel);
    if (!one) {
        return Unexpected(std::move(one.error()));
    }
    return render_stage_two(std::move(*one), settings, options, cancel);
}

Expected<DevelopResult> render_develop_file(const raw::DecoderRegistry& registry, const std::filesystem::path& path,
                                            std::optional<std::uint32_t> subimage, const DevelopSettings& settings,
                                            const DevelopOptions& options, const core::CancellationToken& cancel) {
    if (auto valid = settings.validate(); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    StageOneRequest request;
    request.kind = StageOneKind::Export;
    request.white_balance = settings.white_balance;
    auto one = render_stage_one_file(registry, path, subimage, request, options, cancel);
    if (!one) {
        return Unexpected(std::move(one.error()));
    }
    return render_stage_two(std::move(*one), settings, options, cancel);
}

}  // namespace lucent::engine
