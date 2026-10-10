#include <lucent/analysis/frame_measurement.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <new>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <lucent/engine/display_rendering.hpp>

#include "parallel_bands.hpp"
#include "render_kernel.hpp"
#include "stage_one_copy.hpp"

namespace lucent::analysis {

namespace {

constexpr const char* kWhere = "analysis.frame";
// SaturationClasses split at the measurements' threshold (source_diagnostics.cpp).
static_assert(kSaturatedCoverage == 0.5f);
static_assert(saturation_class_coverage(kPartlySaturated) > 0.0f &&
              saturation_class_coverage(kPartlySaturated) < kSaturatedCoverage &&
              saturation_class_coverage(kMostlySaturated) >= kSaturatedCoverage);
using Clock = std::chrono::steady_clock;
using detail::kMetricBandRows;

// Pixels of a default band at most (a band of 32 rows is the least, whatever the width).
constexpr std::int64_t kMaxBandPixels = std::int64_t{4} << 20;

// Rows per band: as requested (rounded down to whole partials of 32 rows), or by default one
// partial per hardware thread (at most 12) and at most kMaxBandPixels pixels of `width`.
std::int32_t band_rows_of(std::int32_t requested, std::int32_t width) noexcept {
    if (requested > 0) {
        return std::max(kMetricBandRows, requested / kMetricBandRows * kMetricBandRows);
    }
    const unsigned threads = std::thread::hardware_concurrency();
    const std::int32_t by_threads =
        kMetricBandRows * std::clamp<std::int32_t>(threads == 0 ? 4 : static_cast<std::int32_t>(threads), 1, 12);
    const auto by_pixels = static_cast<std::int32_t>(std::clamp<std::int64_t>(
        kMaxBandPixels / std::max<std::int64_t>(1, width) / kMetricBandRows * kMetricBandRows, kMetricBandRows,
        by_threads));
    return std::min(by_threads, by_pixels);
}

// Per-pixel inputs and baseline features of one band of output rows (row-major, the output width).
struct Band {
    std::vector<float> signal;
    std::vector<float> saturation;
    std::vector<std::uint8_t> neutral;
    std::vector<float> lightness;
    std::vector<float> a;
    std::vector<float> b;
    std::vector<float> chroma;
    std::vector<std::uint8_t> flags;
};

}  // namespace

bool streams_baseline(const engine::DevelopSettings& settings, bool monochrome) noexcept {
    if (settings.highlights != 0.0 || settings.shadows != 0.0) {
        return false;  // local tone: a guided filter over the whole image
    }
    if (!settings.chroma_denoise.is_identity() && !monochrome) {
        return false;  // a guided filter as well (monochrome data has no chroma to filter)
    }
    for (const engine::LocalAdjustment& adjustment : settings.local_adjustments) {
        if (adjustment.has_effect()) {
            return false;  // masks are placed relative to the image they are rendered on
        }
    }
    const engine::GeometrySetting& g = settings.geometry;
    return g.straighten_degrees == 0.0 && g.quarter_turns == 0 && !g.flip_horizontal;
}

std::uint64_t frame_measurement_band_bytes(image::Size output, std::int32_t upright_width, bool streamed,
                                           std::int32_t band_rows) noexcept {
    const std::int32_t band = band_rows_of(band_rows, std::max(output.width, upright_width));
    const auto rows = static_cast<std::uint64_t>(std::max<std::int32_t>(0, std::min(band, output.height)));
    const auto width = static_cast<std::uint64_t>(std::max<std::int32_t>(0, output.width));
    // Context (signal, saturation, neutral: 9 bytes) and baseline features (17 bytes) per pixel.
    std::uint64_t bytes = rows * width * (9 + 17);
    if (streamed) {
        // The band of stage one, rendered in place (RGB; monochrome data expands to a new RGB band).
        bytes += 2 * rows * static_cast<std::uint64_t>(std::max<std::int32_t>(0, upright_width)) * 12;
    }
    // Two sets of partials (the render's and the baseline's) per 32 rows of a band, and two totals.
    const auto bands = static_cast<std::uint64_t>(detail::band_count(band, kMetricBandRows));
    bytes += 2 * bands * (detail::kLightnessBins + detail::kNoiseBins) * sizeof(std::uint32_t) +
             2 * (detail::kLightnessBins + detail::kNoiseBins) * sizeof(std::uint64_t);
    return bytes;
}

struct FrameMeasurement::Impl {
    FrameMeasurementInputs in;
    engine::GeometryMapping mapping;
    image::Size out;
    image::Size upright;
    bool identity = true;
    bool streamed = false;
    std::int32_t ox = 0;  // a translation's offset of the output in the upright image
    std::int32_t oy = 0;
    std::int32_t band_rows = kMetricBandRows;
    std::int32_t green = 1;  // stage one's channel of the scene signal
    bool mono = false;
    engine::DevelopSettings band_settings;  // the baseline without its geometry (streamed)
    std::optional<engine::ColorImage> kept;
    detail::MetricMatrices matrices;

    explicit Impl(engine::GeometryMapping m) : mapping(std::move(m)) {}

    [[nodiscard]] const engine::StageOneImage& one() const noexcept { return *in.stage_one; }

    // The upright pixel of output pixel (x, y): the nearest one through the geometry, as
    // map_image_context() picks it; false outside the upright image (uncovered canvas).
    [[nodiscard]] bool upright_of(std::int32_t x, std::int32_t y, std::int32_t& ux, std::int32_t& uy) const noexcept {
        if (identity) {
            ux = x;
            uy = y;
            return true;
        }
        const image::PointD p = mapping.upright_from_output(image::PointD{x + 0.5, y + 0.5});
        const double fx = std::floor(p.x);
        const double fy = std::floor(p.y);
        if (!(fx >= 0.0 && fy >= 0.0 && fx < upright.width && fy < upright.height)) {
            return false;
        }
        ux = static_cast<std::int32_t>(fx);
        uy = static_cast<std::int32_t>(fy);
        return true;
    }

    // The context of output rows [y0, y0 + rows) into `band` (rows of the output width).
    Status fill_context(std::int32_t y0, std::int32_t rows, Band& band, const core::CancellationToken& cancel) const {
        const engine::StageOneImage& s = one();
        const std::int32_t channels = s.pixels.channels();
        const bool classes = in.saturation != nullptr && !in.saturation->empty();
        const std::int32_t width = out.width;
        return detail::for_each_band(
            rows, in.develop.parallel, cancel, kWhere,
            [&](std::int32_t, std::int32_t begin, std::int32_t end) {
                for (std::int32_t r = begin; r < end; ++r) {
                    const std::size_t i = static_cast<std::size_t>(r) * static_cast<std::size_t>(width);
                    float* signal = band.signal.data() + i;
                    float* saturation = classes ? band.saturation.data() + i : nullptr;
                    std::uint8_t* neutral = in.neutral ? band.neutral.data() + i : nullptr;
                    for (std::int32_t x = 0; x < width; ++x) {
                        std::int32_t ux = 0;
                        std::int32_t uy = 0;
                        if (!upright_of(x, y0 + r, ux, uy)) {
                            signal[x] = 0.0f;
                            if (saturation != nullptr) {
                                saturation[x] = 0.0f;
                            }
                            if (neutral != nullptr) {
                                neutral[x] = 0;
                            }
                            continue;
                        }
                        const float* px = s.pixels.row(uy) + static_cast<std::ptrdiff_t>(ux) * channels;
                        const float v = px[green];
                        signal[x] = std::isfinite(v) ? v : 0.0f;
                        std::uint8_t saturation_class = kNotSaturated;
                        if (classes) {
                            saturation_class =
                                in.saturation->classes[static_cast<std::size_t>(uy) *
                                                           static_cast<std::size_t>(upright.width) +
                                                       static_cast<std::size_t>(ux)];
                            saturation[x] = saturation_class_coverage(saturation_class);
                        }
                        if (neutral != nullptr) {
                            neutral[x] = in.neutral->neutral(px[0], px[1], px[2], saturation_class != kNotSaturated,
                                                             in.noise)
                                             ? 1
                                             : 0;
                        }
                    }
                }
            },
            kMetricBandRows);
    }

    // The baseline's render of output rows [y0, y0 + rows): full upright rows re-rendered from the
    // stage one (only per-pixel operations: the same pixels as the whole render; the geometry, a
    // translation, is the caller's offset).
    Expected<engine::ColorImage> render_band(std::int32_t y0, std::int32_t rows,
                                             const core::CancellationToken& cancel) const {
        const image::PixelRect rect{0, y0 + oy, upright.width, rows};
        auto band = detail::crop_stage_one(one(), rect, in.develop.limits);
        if (!band) {
            return Unexpected(std::move(band.error()));
        }
        auto rendered = engine::render_stage_two(std::move(*band), band_settings, in.develop, cancel);
        if (!rendered) {
            return Unexpected(std::move(rendered.error()));
        }
        if (rendered->display.pixels.size() != image::Size{upright.width, rows} ||
            rendered->display.encoding != engine::display_linear_rec2020()) {
            return fail(ErrorCode::Internal, "the baseline's band render has another size or encoding", kWhere);
        }
        return std::move(rendered->display);
    }

    // One pass over the bands: `render` against the baseline (when not null) and the baseline's own
    // statistics (when `baseline_out` is not null).
    Expected<RenderStats> run(const engine::ColorImage* render, RenderStats* baseline_out,
                              const core::CancellationToken& cancel, const std::function<void(double)>& progress) {
        const auto start = Clock::now();
        const std::int32_t width = out.width;
        const std::int32_t height = out.height;
        // The partials of one band of rows at a time, merged in order into the totals after it (the
        // same additions in the same order as measure_render()'s merge of all of them).
        const auto sub_bands = static_cast<std::size_t>(detail::band_count(band_rows, kMetricBandRows));
        std::vector<detail::MetricPartial> render_partials;
        std::vector<detail::MetricPartial> baseline_partials;
        std::optional<detail::MetricTotal> render_total;
        std::optional<detail::MetricTotal> baseline_total;
        try {
            if (render != nullptr) {
                render_total.emplace();
            }
            if (baseline_out != nullptr) {
                baseline_total.emplace();
            }
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "cannot allocate the measurement totals", kWhere);
        }
        Band band;
        try {
            const std::size_t n = static_cast<std::size_t>(std::min(band_rows, height)) * static_cast<std::size_t>(width);
            band.signal.resize(n);
            if (in.saturation != nullptr && !in.saturation->empty()) {
                band.saturation.resize(n);
            }
            if (in.neutral) {
                band.neutral.resize(n);
            }
            band.lightness.resize(n);
            band.a.resize(n);
            band.b.resize(n);
            band.chroma.resize(n);
            band.flags.resize(n);
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "cannot allocate a measurement band", kWhere);
        }
        const detail::MetricKernel kernel(matrices, in.noise);
        for (std::int32_t y0 = 0; y0 < height; y0 += band_rows) {
            if (Status c = cancel.check(kWhere); !c) {
                return Unexpected(std::move(c.error()));
            }
            const std::int32_t rows = std::min(band_rows, height - y0);
            if (render != nullptr) {
                if (Status made = detail::make_partials(render_partials, sub_bands); !made) {
                    return Unexpected(std::move(made.error()));
                }
            }
            if (baseline_out != nullptr) {
                if (Status made = detail::make_partials(baseline_partials, sub_bands); !made) {
                    return Unexpected(std::move(made.error()));
                }
            }
            if (Status filled = fill_context(y0, rows, band, cancel); !filled) {
                return Unexpected(std::move(filled.error()));
            }
            std::optional<engine::ColorImage> rendered;
            if (streamed) {
                auto r = render_band(y0, rows, cancel);
                if (!r) {
                    return Unexpected(std::move(r.error()));
                }
                rendered = std::move(*r);
            }
            // The baseline's pixels of output row y.
            const auto baseline_row = [&](std::int32_t y) -> const float* {
                if (rendered) {
                    return rendered->pixels.row(y - y0) + static_cast<std::ptrdiff_t>(ox) * 3;
                }
                return kept->pixels.row(y);
            };
            auto status = detail::for_each_band(
                rows, in.develop.parallel, cancel, kWhere,
                [&](std::int32_t sub, std::int32_t begin, std::int32_t end) {
                    const auto g = static_cast<std::size_t>(sub);
                    for (std::int32_t r = begin; r < end; ++r) {
                        const std::int32_t y = y0 + r;
                        const std::size_t i = static_cast<std::size_t>(r) * static_cast<std::size_t>(width);
                        detail::ContextRow context;
                        context.signal = band.signal.data() + i;
                        context.saturation = band.saturation.empty() ? nullptr : band.saturation.data() + i;
                        context.neutral = band.neutral.empty() ? nullptr : band.neutral.data() + i;
                        const detail::FeatureRow features{band.lightness.data() + i, band.a.data() + i,
                                                          band.b.data() + i, band.chroma.data() + i,
                                                          band.flags.data() + i};
                        if (baseline_out != nullptr) {
                            kernel.measure_row(baseline_partials[g], baseline_row(y), width, context, nullptr,
                                               &features, nullptr);
                        } else {
                            kernel.features_row(baseline_row(y), width, features);
                        }
                        if (render != nullptr) {
                            const detail::BaselineRow base{features.lightness, features.a, features.b, features.chroma,
                                                           features.flags};
                            kernel.measure_row(render_partials[g], render->pixels.row(y), width, context, &base,
                                               nullptr, nullptr);
                        }
                    }
                },
                kMetricBandRows);
            if (!status) {
                return Unexpected(std::move(status.error()));
            }
            const auto used = static_cast<std::size_t>(detail::band_count(rows, kMetricBandRows));
            for (std::size_t k = 0; k < used; ++k) {
                if (render_total) {
                    render_total->merge(render_partials[k]);
                }
                if (baseline_total) {
                    baseline_total->merge(baseline_partials[k]);
                }
            }
            if (progress) {
                progress(static_cast<double>(y0 + rows) / static_cast<double>(height));
            }
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        if (baseline_total) {
            *baseline_out = detail::finish_stats(*baseline_total, out, in.noise.available());
            baseline_out->seconds = seconds;
        }
        if (!render_total) {
            return baseline_out != nullptr ? *baseline_out : RenderStats{};
        }
        RenderStats stats = detail::finish_stats(*render_total, out, in.noise.available());
        stats.seconds = seconds;
        return stats;
    }
};

FrameMeasurement::FrameMeasurement(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
FrameMeasurement::FrameMeasurement(FrameMeasurement&&) noexcept = default;
FrameMeasurement& FrameMeasurement::operator=(FrameMeasurement&&) noexcept = default;
FrameMeasurement::~FrameMeasurement() = default;

Expected<FrameMeasurement> FrameMeasurement::create(FrameMeasurementInputs inputs) {
    const engine::StageOneImage* one = inputs.stage_one;
    if (one == nullptr || one->pixels.empty() || (one->pixels.channels() != 3 && one->pixels.channels() != 1)) {
        return fail(ErrorCode::InvalidArgument, "the measurement needs a stage-one image of 1 or 3 channels", kWhere);
    }
    if (one->scale != 1.0 || one->pixels.size() != one->full_size) {
        return fail(ErrorCode::InvalidArgument, "the measurement needs the full-resolution stage one", kWhere);
    }
    if (inputs.saturation != nullptr && !inputs.saturation->empty() &&
        (inputs.saturation->size != one->pixels.size() ||
         inputs.saturation->classes.size() != static_cast<std::size_t>(one->pixels.size().area()))) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("saturation classes of {}x{} do not match the stage one's {}x{}",
                                inputs.saturation->size.width, inputs.saturation->size.height,
                                one->pixels.width(), one->pixels.height()),
                    kWhere);
    }
    if (inputs.neutral && one->pixels.channels() != 3) {
        return fail(ErrorCode::InvalidArgument, "neutral evidence needs a 3-channel stage one", kWhere);
    }
    auto mapping = engine::geometry_mapping(inputs.baseline, *one);
    if (!mapping) {
        return Unexpected(std::move(mapping.error()));
    }
    auto matrices = detail::metric_matrices();
    if (!matrices) {
        return Unexpected(std::move(matrices.error()));
    }
    try {
        auto impl = std::make_unique<Impl>(std::move(*mapping));
        impl->in = std::move(inputs);
        impl->matrices = *matrices;
        impl->upright = one->pixels.size();
        impl->out = impl->mapping.output_size();
        impl->identity = impl->mapping.identity();
        impl->mono = one->pixels.channels() == 1;
        impl->green = impl->mono ? 0 : 1;
        impl->band_rows = band_rows_of(impl->in.band_rows, std::max(impl->out.width, impl->upright.width));
        bool translation = impl->identity;
        if (!impl->identity && !impl->mapping.resamples() &&
            impl->mapping.orientation() == image::Orientation::Normal) {
            // A crop (and the camera's image area) moves pixels without resampling: check that the
            // output's corners map onto one offset inside the upright image.
            const image::PointD first = impl->mapping.upright_from_output(image::PointD{0.5, 0.5});
            const image::PointD last = impl->mapping.upright_from_output(
                image::PointD{impl->out.width - 0.5, impl->out.height - 0.5});
            const double ox = std::floor(first.x);
            const double oy = std::floor(first.y);
            translation = ox >= 0.0 && oy >= 0.0 && std::floor(last.x) == ox + impl->out.width - 1 &&
                          std::floor(last.y) == oy + impl->out.height - 1 &&
                          ox + impl->out.width <= impl->upright.width && oy + impl->out.height <= impl->upright.height;
            if (translation) {
                impl->ox = static_cast<std::int32_t>(ox);
                impl->oy = static_cast<std::int32_t>(oy);
            }
        }
        impl->streamed = translation && streams_baseline(impl->in.baseline, impl->mono);
        impl->band_settings = impl->in.baseline;
        impl->band_settings.geometry = engine::GeometrySetting{};
        return FrameMeasurement(std::move(impl));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot prepare the measurement", kWhere);
    }
}

const engine::GeometryMapping& FrameMeasurement::mapping() const noexcept {
    return impl_->mapping;
}

bool FrameMeasurement::baseline_streamed() const noexcept {
    return impl_->streamed;
}

Expected<std::uint64_t> FrameMeasurement::upright_neutral_pixels(const core::CancellationToken& cancel) const {
    const Impl& m = *impl_;
    if (!m.in.neutral) {
        return std::uint64_t{0};
    }
    const engine::StageOneImage& s = m.one();
    const bool classes = m.in.saturation != nullptr && !m.in.saturation->empty();
    const std::int32_t width = m.upright.width;
    std::vector<std::uint64_t> counts;
    try {
        counts.assign(static_cast<std::size_t>(detail::band_count(m.upright.height, kMetricBandRows)), 0);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot count the neutral evidence", kWhere);
    }
    auto status = detail::for_each_band(
        m.upright.height, m.in.develop.parallel, cancel, kWhere,
        [&](std::int32_t band, std::int32_t begin, std::int32_t end) {
            std::uint64_t n = 0;
            for (std::int32_t y = begin; y < end; ++y) {
                const float* row = s.pixels.row(y);
                const std::uint8_t* sat =
                    classes ? m.in.saturation->classes.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
                            : nullptr;
                for (std::int32_t x = 0; x < width; ++x, row += 3) {
                    const bool saturated = sat != nullptr && sat[x] != kNotSaturated;
                    n += m.in.neutral->neutral(row[0], row[1], row[2], saturated, m.in.noise) ? 1 : 0;
                }
            }
            counts[static_cast<std::size_t>(band)] = n;
        },
        kMetricBandRows);
    if (!status) {
        return Unexpected(std::move(status.error()));
    }
    std::uint64_t total = 0;
    for (const std::uint64_t n : counts) {
        total += n;
    }
    return total;
}

Status FrameMeasurement::keep_baseline(engine::ColorImage baseline) {
    Impl& m = *impl_;
    if (m.streamed) {
        return fail(ErrorCode::InvalidArgument, "the baseline is rendered band by band, not kept", kWhere);
    }
    if (baseline.pixels.channels() != 3 || baseline.encoding != engine::display_linear_rec2020() ||
        baseline.pixels.size() != m.out) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the baseline must be a display-linear Rec.2020 render of {}x{}", m.out.width,
                                m.out.height),
                    kWhere);
    }
    m.kept = std::move(baseline);
    return ok();
}

Expected<RenderStats> FrameMeasurement::measure_baseline(const core::CancellationToken& cancel) {
    Impl& m = *impl_;
    if (!m.streamed && !m.kept) {
        return fail(ErrorCode::InvalidArgument, "the baseline render was not kept", kWhere);
    }
    RenderStats baseline;
    auto measured = m.run(nullptr, &baseline, cancel, {});
    if (!measured) {
        return Unexpected(std::move(measured.error()));
    }
    return baseline;
}

Expected<RenderStats> FrameMeasurement::measure(const engine::ColorImage& render, RenderStats* baseline_stats,
                                                const core::CancellationToken& cancel,
                                                const std::function<void(double)>& progress) {
    Impl& m = *impl_;
    const auto& px = render.pixels;
    if (px.empty() || px.channels() != 3 || render.encoding != engine::display_linear_rec2020() || px.size() != m.out) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the measurement needs a display-linear Rec.2020 render of {}x{} (got {}x{}, {} "
                                "channels, {})",
                                m.out.width, m.out.height, px.width(), px.height(), px.channels(),
                                render.encoding.to_string()),
                    kWhere);
    }
    if (!m.streamed && !m.kept) {
        return fail(ErrorCode::InvalidArgument, "the baseline render was not kept", kWhere);
    }
    return m.run(&render, baseline_stats, cancel, progress);
}

std::uint64_t FrameMeasurement::kept_bytes() const noexcept {
    return impl_->kept ? impl_->kept->pixels.byte_size() : 0;
}

}  // namespace lucent::analysis
