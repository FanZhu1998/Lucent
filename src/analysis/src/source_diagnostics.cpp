#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <new>
#include <type_traits>
#include <variant>

#include <lucent/analysis/source_diagnostics.hpp>

#include "parallel_bands.hpp"

namespace lucent::analysis {

namespace {

constexpr const char* kWhere = "analysis.source";

using Clock = std::chrono::steady_clock;

// Channel index of every site of a CFA tile (raw-frame relative), -1 for colours without one.
struct ChannelTable {
    std::int32_t width = 1;
    std::int32_t height = 1;
    std::vector<std::int8_t> index{0};

    [[nodiscard]] std::int32_t at(std::int64_t x, std::int64_t y) const noexcept {
        const auto tx = static_cast<std::size_t>(x % width);
        const auto ty = static_cast<std::size_t>(y % height);
        return index[ty * static_cast<std::size_t>(width) + tx];
    }
};

Expected<ChannelTable> channel_table(const raw::RawMetadata& metadata) {
    ChannelTable table;
    const raw::RepresentationInfo& rep = metadata.representation;
    if (rep.kind != raw::RawRepresentation::Mosaic) {
        return table;
    }
    const image::CfaPattern& cfa = rep.cfa;
    if (cfa.empty()) {
        return fail(ErrorCode::Internal, "a mosaic without a CFA", kWhere);
    }
    table.width = cfa.width();
    table.height = cfa.height();
    table.index.assign(static_cast<std::size_t>(table.width) * static_cast<std::size_t>(table.height), -1);
    for (std::int32_t y = 0; y < table.height; ++y) {
        for (std::int32_t x = 0; x < table.width; ++x) {
            const image::CfaColor color = cfa.color_at(y, x);
            for (std::size_t c = 0; c < rep.channel_colors.size() && c < 4; ++c) {
                if (rep.channel_colors[c] == color) {
                    table.index[static_cast<std::size_t>(y) * static_cast<std::size_t>(table.width) +
                                static_cast<std::size_t>(x)] = static_cast<std::int8_t>(c);
                    break;
                }
            }
            if (table.index[static_cast<std::size_t>(y) * static_cast<std::size_t>(table.width) +
                            static_cast<std::size_t>(x)] < 0) {
                return fail(ErrorCode::Unsupported,
                            std::format("CFA colour {} has no channel index", static_cast<int>(color)),
                            kWhere);
            }
        }
    }
    return table;
}

// Index of the green channel (Green, else Green2, else 0).
std::size_t green_channel(const raw::RawMetadata& metadata) {
    const auto& colors = metadata.representation.channel_colors;
    for (std::size_t c = 0; c < colors.size(); ++c) {
        if (colors[c] == image::CfaColor::Green) {
            return c;
        }
    }
    for (std::size_t c = 0; c < colors.size(); ++c) {
        if (colors[c] == image::CfaColor::Green2) {
            return c;
        }
    }
    return 0;
}

template <class T>
[[nodiscard]] bool is_finite_sample(T v) noexcept {
    if constexpr (std::is_floating_point_v<T>) {
        return std::isfinite(v);
    } else {
        return true;
    }
}

struct ChannelCounts {
    std::array<std::uint64_t, 4> sites{};
    std::array<std::uint64_t, 4> saturated{};
    std::uint64_t nonfinite = 0;
};

// Per-channel saturation over the visible area.
template <class T>
Expected<ChannelCounts> count_channels(const image::ImageBuffer<T>& frame, const raw::RawMetadata& metadata,
                                       const ChannelTable& table, bool parallel,
                                       const core::CancellationToken& cancel) {
    const image::PixelRect visible = metadata.geometry.visible_area;
    const double white = metadata.levels.white;
    const raw::RawRepresentation kind = metadata.representation.kind;
    const std::int32_t samples = kind == raw::RawRepresentation::LinearColor ? 3 : 1;
    std::vector<ChannelCounts> bands(static_cast<std::size_t>(detail::band_count(visible.height)));
    auto status = detail::for_each_band(visible.height, parallel, cancel, kWhere,
                                        [&](std::int32_t band, std::int32_t begin, std::int32_t end) {
                                            ChannelCounts& counts = bands[static_cast<std::size_t>(band)];
                                            for (std::int32_t vy = begin; vy < end; ++vy) {
                                                const std::int64_t ry = std::int64_t{visible.y} + vy;
                                                const T* row = frame.row(static_cast<std::int32_t>(ry));
                                                for (std::int32_t vx = 0; vx < visible.width; ++vx) {
                                                    const std::int64_t rx = std::int64_t{visible.x} + vx;
                                                    for (std::int32_t s = 0; s < samples; ++s) {
                                                        const T v = row[rx * samples + s];
                                                        const std::size_t c =
                                                            kind == raw::RawRepresentation::Mosaic
                                                                ? static_cast<std::size_t>(table.at(rx, ry))
                                                                : static_cast<std::size_t>(s);
                                                        if (!is_finite_sample(v)) {
                                                            ++counts.nonfinite;
                                                            continue;
                                                        }
                                                        ++counts.sites[c];
                                                        if (static_cast<double>(v) >= white) {
                                                            ++counts.saturated[c];
                                                        }
                                                    }
                                                }
                                            }
                                        });
    if (!status) {
        return Unexpected(std::move(status.error()));
    }
    ChannelCounts total;
    for (const ChannelCounts& b : bands) {
        for (std::size_t c = 0; c < 4; ++c) {
            total.sites[c] += b.sites[c];
            total.saturated[c] += b.saturated[c];
        }
        total.nonfinite += b.nonfinite;
    }
    return total;
}

// Coverage of one grid (upright size `upright_grid`).
template <class T>
Expected<CoverageMap> coverage_for(const image::ImageBuffer<T>& frame, const raw::RawMetadata& metadata,
                                   image::Size upright_grid, bool parallel,
                                   const core::CancellationToken& cancel) {
    const image::PixelRect visible = metadata.geometry.visible_area;
    const image::Orientation orientation = metadata.geometry.orientation;
    const double white = metadata.levels.white;
    const std::int32_t samples = metadata.representation.kind == raw::RawRepresentation::LinearColor ? 3 : 1;
    const image::Size stored_grid =
        image::swaps_axes(orientation) ? image::Size{upright_grid.height, upright_grid.width} : upright_grid;
    const std::int64_t W = visible.width;
    const std::int64_t H = visible.height;
    const std::int64_t gw = stored_grid.width;
    const std::int64_t gh = stored_grid.height;
    std::vector<std::int32_t> column_cell(static_cast<std::size_t>(W));
    for (std::int64_t vx = 0; vx < W; ++vx) {
        column_cell[static_cast<std::size_t>(vx)] = static_cast<std::int32_t>(vx * gw / W);
    }
    std::vector<float> stored(static_cast<std::size_t>(gw * gh), 0.0f);
    auto status = detail::for_each_band(
        static_cast<std::int32_t>(gh), parallel, cancel, kWhere,
        [&](std::int32_t, std::int32_t begin, std::int32_t end) {
            std::vector<std::uint32_t> sat(static_cast<std::size_t>(gw));
            std::vector<std::uint32_t> all(static_cast<std::size_t>(gw));
            for (std::int32_t cy = begin; cy < end; ++cy) {
                std::fill(sat.begin(), sat.end(), 0u);
                std::fill(all.begin(), all.end(), 0u);
                const std::int64_t y0 = (std::int64_t{cy} * H + gh - 1) / gh;
                const std::int64_t y1 = std::min(H, ((std::int64_t{cy} + 1) * H + gh - 1) / gh);
                for (std::int64_t vy = y0; vy < y1; ++vy) {
                    const T* row = frame.row(static_cast<std::int32_t>(visible.y + vy));
                    const T* p = row + (std::int64_t{visible.x}) * samples;
                    for (std::int64_t vx = 0; vx < W; ++vx) {
                        const auto cell = static_cast<std::size_t>(column_cell[static_cast<std::size_t>(vx)]);
                        for (std::int32_t s = 0; s < samples; ++s, ++p) {
                            ++all[cell];
                            if (is_finite_sample(*p) && static_cast<double>(*p) >= white) {
                                ++sat[cell];
                            }
                        }
                    }
                }
                float* out = stored.data() + static_cast<std::size_t>(cy) * static_cast<std::size_t>(gw);
                for (std::size_t x = 0; x < static_cast<std::size_t>(gw); ++x) {
                    out[x] =
                        all[x] == 0
                            ? 0.0f
                            : static_cast<float>(static_cast<double>(sat[x]) / static_cast<double>(all[x]));
                }
            }
        },
        8);
    if (!status) {
        return Unexpected(std::move(status.error()));
    }
    CoverageMap map;
    map.size = upright_grid;
    map.fraction.assign(static_cast<std::size_t>(upright_grid.area()), 0.0f);
    for (std::int32_t cy = 0; cy < stored_grid.height; ++cy) {
        for (std::int32_t cx = 0; cx < stored_grid.width; ++cx) {
            const image::Point u = image::map_to_oriented(image::Point{cx, cy}, stored_grid, orientation);
            map.fraction[static_cast<std::size_t>(u.y) * static_cast<std::size_t>(upright_grid.width) +
                         static_cast<std::size_t>(u.x)] =
                stored[static_cast<std::size_t>(cy) * static_cast<std::size_t>(gw) +
                       static_cast<std::size_t>(cx)];
        }
    }
    return map;
}

// The classes of the upright full-resolution grid: coverage_for() of that grid, where every cell is
// exactly one site (its `samples` samples), reduced to SaturationClasses without the float map.
// The class boundary is the measurements' kSaturatedCoverage (render_metrics.hpp, 0.5).
template <class T>
Expected<SaturationClasses> saturation_classes_for(const image::ImageBuffer<T>& frame,
                                                   const raw::RawMetadata& metadata, bool parallel,
                                                   const core::CancellationToken& cancel) {
    const image::PixelRect visible = metadata.geometry.visible_area;
    const image::Orientation orientation = metadata.geometry.orientation;
    const double white = metadata.levels.white;
    const std::int32_t samples = metadata.representation.kind == raw::RawRepresentation::LinearColor ? 3 : 1;
    const image::Size stored = visible.size();
    SaturationClasses out;
    out.size = image::oriented_size(stored, orientation);
    try {
        out.classes.assign(static_cast<std::size_t>(out.size.area()), kNotSaturated);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the full-resolution saturation classes", kWhere);
    }
    const auto upright_width = static_cast<std::size_t>(out.size.width);
    auto status = detail::for_each_band(
        stored.height, parallel, cancel, kWhere,
        [&](std::int32_t, std::int32_t begin, std::int32_t end) {
            for (std::int32_t cy = begin; cy < end; ++cy) {
                const T* p = frame.row(visible.y + cy) + std::int64_t{visible.x} * samples;
                for (std::int32_t cx = 0; cx < stored.width; ++cx) {
                    std::uint32_t saturated = 0;
                    for (std::int32_t s = 0; s < samples; ++s, ++p) {
                        if (is_finite_sample(*p) && static_cast<double>(*p) >= white) {
                            ++saturated;
                        }
                    }
                    // As coverage_for() computes the cell's fraction.
                    const auto fraction = static_cast<float>(static_cast<double>(saturated) / static_cast<double>(samples));
                    const std::uint8_t c = fraction <= 0.0f   ? kNotSaturated
                                           : fraction >= 0.5f ? kMostlySaturated
                                                              : kPartlySaturated;
                    const image::Point u = image::map_to_oriented(image::Point{cx, cy}, stored, orientation);
                    out.classes[static_cast<std::size_t>(u.y) * upright_width + static_cast<std::size_t>(u.x)] = c;
                }
            }
        },
        8);
    if (!status) {
        return Unexpected(std::move(status.error()));
    }
    return out;
}

// Robust read-noise estimate (normalised units) of the green channel on the optical-black margins.
struct MaskedEstimate {
    std::optional<double> sigma;
    std::uint64_t samples = 0;
};

template <class T>
MaskedEstimate masked_read_noise(const image::ImageBuffer<T>& frame, const raw::RawMetadata& metadata,
                                 const ChannelTable& table) {
    MaskedEstimate out;
    const raw::RawRepresentation kind = metadata.representation.kind;
    if (kind == raw::RawRepresentation::LinearColor) {
        return out;  // linear (demosaiced) data carries no optical black
    }
    const image::PixelRect visible = metadata.geometry.visible_area;
    const std::size_t green = green_channel(metadata);
    const std::int32_t margin = 4;
    std::vector<image::PixelRect> regions;
    if (visible.x >= 16) {
        regions.push_back(image::PixelRect{margin, visible.y, visible.x - 2 * margin, visible.height});
    }
    if (visible.y >= 16) {
        regions.push_back(image::PixelRect{visible.x, margin, visible.width, visible.y - 2 * margin});
    }
    std::uint64_t available = 0;
    for (const image::PixelRect& r : regions) {
        available += static_cast<std::uint64_t>(r.width) * static_cast<std::uint64_t>(r.height);
    }
    if (available < kMinMaskedSamples) {
        return out;
    }
    const std::uint64_t stride = std::max<std::uint64_t>(1, available / 400000);
    std::vector<double> values;
    std::uint64_t k = 0;
    for (const image::PixelRect& r : regions) {
        for (std::int32_t y = r.y; y < r.y + r.height; ++y) {
            const T* row = frame.row(y);
            for (std::int32_t x = r.x; x < r.x + r.width; ++x, ++k) {
                if (k % stride != 0) {
                    continue;
                }
                if (kind == raw::RawRepresentation::Mosaic &&
                    static_cast<std::size_t>(table.at(x, y)) != green) {
                    continue;
                }
                const T v = row[x];
                if (is_finite_sample(v)) {
                    values.push_back(static_cast<double>(v));
                }
            }
        }
    }
    if (values.size() < kMinMaskedSamples / 2) {
        return out;
    }
    const raw::SensorLevels& levels = metadata.levels;
    const double black = levels.black + levels.channel_black[green < 4 ? green : 0];
    const double range = levels.white - levels.max_black_level();
    if (!(range > 0.0)) {
        return out;
    }
    const auto mid = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), mid, values.end());
    const double median = *mid;
    if (std::abs(median - black) > kMaskedBlackTolerance * (levels.white - black)) {
        return out;  // not optical black (zeroed margins, image data, a different black model)
    }
    for (double& v : values) {
        v = std::abs(v - median);
    }
    std::nth_element(values.begin(), mid, values.end());
    const double mad = *mid;
    if (!(mad > 0.0)) {
        return out;  // quantised to a single code: no measurable noise
    }
    out.sigma = 1.4826 * mad / range;
    out.samples = values.size();
    return out;
}

}  // namespace

std::string_view to_string(NoiseModelSource source) noexcept {
    switch (source) {
        case NoiseModelSource::DngNoiseProfile:
            return "dng_noise_profile";
        case NoiseModelSource::MaskedAreaAndIso:
            return "masked_area_and_iso";
        case NoiseModelSource::IsoAssumption:
            return "iso_assumption";
        case NoiseModelSource::MaskedAreaOnly:
            return "masked_area_only";
        case NoiseModelSource::Unavailable:
            return "unavailable";
    }
    return "invalid";
}

double SensorNoiseModel::sigma(double signal) const noexcept {
    const double variance = read_variance + shot_scale * std::max(0.0, signal);
    return std::sqrt(std::max(0.0, variance));
}

SensorNoiseModel make_noise_model(const raw::RawMetadata& metadata, std::optional<double> masked_read_sigma,
                                  std::uint64_t masked_samples) {
    SensorNoiseModel model;
    model.masked_read_sigma = masked_read_sigma;
    model.masked_samples = masked_read_sigma ? masked_samples : 0;
    if (metadata.capture.iso && std::isfinite(*metadata.capture.iso) && *metadata.capture.iso > 0.0f) {
        model.iso = static_cast<double>(*metadata.capture.iso);
    }
    if (metadata.noise && !metadata.noise->planes.empty()) {
        const auto& planes = metadata.noise->planes;
        const raw::NoiseModel& plane = planes.size() >= 3 ? planes[1] : planes[0];
        if (std::isfinite(plane.scale) && std::isfinite(plane.offset) && plane.scale >= 0.0 &&
            plane.offset >= 0.0 && (plane.scale > 0.0 || plane.offset > 0.0)) {
            model.source = NoiseModelSource::DngNoiseProfile;
            model.shot_scale = plane.scale;
            model.read_variance = plane.offset;
            model.confidence = 1.0;
            model.basis = std::format("DNG NoiseProfile ({} plane{}, {}): variance = {:.6g} x + {:.6g}",
                                      planes.size(), planes.size() == 1 ? "" : "s",
                                      metadata.noise->source_ifd, plane.scale, plane.offset);
            return model;
        }
    }
    const double full_well_100 = kAssumedFullWellAtIso100;
    if (masked_read_sigma && model.iso) {
        model.source = NoiseModelSource::MaskedAreaAndIso;
        model.read_variance = *masked_read_sigma * *masked_read_sigma;
        model.shot_scale = *model.iso / (100.0 * full_well_100);
        model.confidence = 0.9;
        model.basis = std::format(
            "read noise {:.4g} of white measured on {} optical-black samples; shot noise of an assumed full "
            "well of "
            "{:.0f} e- at ISO 100, scaled to ISO {:.0f}",
            *masked_read_sigma, masked_samples, full_well_100, *model.iso);
        return model;
    }
    if (model.iso) {
        model.source = NoiseModelSource::IsoAssumption;
        const double read = kAssumedReadNoiseAtIso100 * std::sqrt(std::max(1.0, *model.iso / 100.0));
        model.read_variance = read * read;
        model.shot_scale = *model.iso / (100.0 * full_well_100);
        model.confidence = 0.7;
        model.basis = std::format(
            "assumed read noise {:.4g} of white and full well {:.0f} e- at ISO 100, scaled to ISO {:.0f} "
            "(no noise profile, no usable optical black)",
            read, full_well_100, *model.iso);
        return model;
    }
    if (masked_read_sigma) {
        model.source = NoiseModelSource::MaskedAreaOnly;
        model.read_variance = *masked_read_sigma * *masked_read_sigma;
        model.shot_scale = 1.0 / full_well_100;
        model.confidence = 0.6;
        model.basis = std::format(
            "read noise {:.4g} of white measured on {} optical-black samples; no ISO: shot noise assumed at "
            "ISO 100",
            *masked_read_sigma, masked_samples);
        return model;
    }
    model.source = NoiseModelSource::Unavailable;
    model.confidence = 0.0;
    model.basis = "no NoiseProfile, no ISO and no usable optical black: no noise model";
    return model;
}

double SourceDiagnostics::max_channel_fraction() const noexcept {
    double m = 0.0;
    for (const ChannelSaturation& c : channels) {
        m = std::max(m, c.fraction());
    }
    return m;
}

Expected<SourceAnalysis> analyze_source(const raw::DecodedRaw& decoded,
                                        std::span<const image::Size> coverage_grids, bool parallel,
                                        const core::CancellationToken& cancel, bool full_resolution_classes) {
    const auto start = Clock::now();
    if (auto consistent = raw::check_consistency(decoded); !consistent) {
        return Unexpected(std::move(consistent.error()));
    }
    const raw::RawMetadata& metadata = decoded.metadata;
    SourceAnalysis out;
    SourceDiagnostics& d = out.diagnostics;
    d.representation = metadata.representation.kind;
    d.stored_size = metadata.geometry.visible_area.size();
    d.orientation = metadata.geometry.orientation;
    d.upright_size = image::oriented_size(d.stored_size, d.orientation);
    for (const image::Size& grid : coverage_grids) {
        if (grid.width < 1 || grid.height < 1 || grid.width > d.upright_size.width ||
            grid.height > d.upright_size.height) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("a coverage grid must be non-empty and at most {}x{} (got {}x{})",
                                    d.upright_size.width, d.upright_size.height, grid.width, grid.height),
                        kWhere);
        }
    }
    auto table = channel_table(metadata);
    if (!table) {
        return Unexpected(std::move(table.error()));
    }
    const std::size_t channel_count = std::min<std::size_t>(4, metadata.representation.channel_colors.size());

    Status status = ok();
    std::visit(
        [&](const auto& frame) {
            auto counts = count_channels(frame, metadata, *table, parallel, cancel);
            if (!counts) {
                status = Unexpected(std::move(counts.error()));
                return;
            }
            for (std::size_t c = 0; c < channel_count; ++c) {
                ChannelSaturation s;
                s.color = metadata.representation.channel_colors[c];
                s.sites = counts->sites[c];
                s.saturated = counts->saturated[c];
                s.threshold = metadata.levels.white;
                d.sites += s.sites;
                d.saturated_sites += s.saturated;
                d.channels.push_back(s);
            }
            d.nonfinite_samples = counts->nonfinite;
            for (const image::Size& grid : coverage_grids) {
                auto map = coverage_for(frame, metadata, grid, parallel, cancel);
                if (!map) {
                    status = Unexpected(std::move(map.error()));
                    return;
                }
                out.coverage.push_back(std::move(*map));
            }
            if (full_resolution_classes) {
                auto classes = saturation_classes_for(frame, metadata, parallel, cancel);
                if (!classes) {
                    status = Unexpected(std::move(classes.error()));
                    return;
                }
                out.full_resolution = std::move(*classes);
            }
            const MaskedEstimate masked = masked_read_noise(frame, metadata, *table);
            d.noise = make_noise_model(metadata, masked.sigma, masked.samples);
        },
        raw::pixels_of(decoded.image));
    if (!status) {
        return Unexpected(std::move(status.error()));
    }
    d.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return out;
}

}  // namespace lucent::analysis
