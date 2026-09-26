// Sensor normalisation (sensor.hpp).

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <new>
#include <numeric>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <lucent/core/version.hpp>
#include <lucent/engine/sensor.hpp>

#include "parallel_rows.hpp"
#include "sensor_internal.hpp"

namespace lucent::engine {

namespace {

constexpr const char* kWhereContract = "engine.sensor.contract";
constexpr const char* kWhereLevels = "engine.sensor.levels";
constexpr const char* kWhereSamples = "engine.sensor.samples";
constexpr const char* kWhereLayout = "engine.sensor.layout";
constexpr const char* kWhereNormalize = "engine.sensor.normalize";
constexpr const char* kWhereMask = "engine.sensor.mask";

bool all_finite(std::span<const double> values) noexcept {
    return std::all_of(values.begin(), values.end(), [](double v) { return std::isfinite(v); });
}

double max_or_zero(const std::vector<double>& values) noexcept {
    return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
}

std::string color_list(std::span<const image::CfaColor> colors) {
    std::string text;
    for (const image::CfaColor color : colors) {
        text += text.empty() ? "" : ", ";
        text += image::to_string(color);
    }
    return text;
}

// Levels must describe a usable range before anything else is looked at: they come from the
// file (or the decoder's reading of it), so a failure is Corrupt data.
Status validate_levels(const raw::SensorLevels& levels) {
    if (!std::isfinite(levels.black) || !std::isfinite(levels.white) || !all_finite(levels.channel_black) ||
        !all_finite(levels.black_pattern) || !all_finite(levels.linear_max) ||
        !all_finite(levels.column_black_delta) || !all_finite(levels.row_black_delta)) {
        return fail(ErrorCode::Corrupt, "black or white levels are not finite", kWhereLevels);
    }
    const double max_black = levels.max_black_level();
    if (!(levels.white > max_black)) {
        return fail(ErrorCode::Corrupt,
                    std::format("the white level {} is not above the highest black level {}", levels.white,
                                max_black),
                    kWhereLevels);
    }
    return ok();
}

// Rejects layouts without a normalisation route (Unsupported).
Status check_layout(const raw::RawMetadata& m) {
    const raw::RepresentationInfo& rep = m.representation;
    if (m.interpretation.status == raw::InterpretationStatus::NotDecodable) {
        return fail(ErrorCode::Unsupported, "the decoder marked the data as not decodable", kWhereLayout);
    }
    switch (rep.kind) {
        case raw::RawRepresentation::Mosaic:
            for (std::int32_t r = 0; r < rep.cfa.height(); ++r) {
                for (std::int32_t c = 0; c < rep.cfa.width(); ++c) {
                    if (!detail::is_rgb_color(rep.cfa.color_at(r, c))) {
                        return fail(ErrorCode::Unsupported,
                                    std::format("a {} mosaic with filter colours ({}) other than red, green and "
                                                "blue has no processing route",
                                                rep.cfa.to_string(), color_list(rep.channel_colors)),
                                    kWhereLayout);
                    }
                }
            }
            return ok();
        case raw::RawRepresentation::LinearColor:
            if (rep.channel_colors !=
                std::vector<image::CfaColor>{image::CfaColor::Red, image::CfaColor::Green, image::CfaColor::Blue}) {
                return fail(ErrorCode::Unsupported,
                            std::format("full-colour data with channels ({}) other than red, green, blue has no "
                                        "processing route",
                                        color_list(rep.channel_colors)),
                            kWhereLayout);
            }
            return ok();
        case raw::RawRepresentation::Monochrome:
            return ok();
    }
    return fail(ErrorCode::Unsupported, "unknown representation", kWhereLayout);
}

// Everything the per-row loop needs, derived once.
struct Plan {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t channels = 1;  // samples per pixel
    image::PixelRect visible;
    detail::ChannelTable table;
    std::array<double, 4> base_black{};  // black + channel_black[c]
    std::array<double, 4> range{};       // white - black_max[c]
    std::array<double, 4> threshold{};   // RAW saturation threshold
    const std::vector<double>* pattern = nullptr;  // null without a spatial pattern
    std::int32_t pattern_rows = 0;
    std::int32_t pattern_cols = 0;
    const std::vector<double>* column_delta = nullptr;  // null when empty
    const std::vector<double>* row_delta = nullptr;     // null when empty
};

std::int64_t bounded_lcm(std::int64_t a, std::int64_t b, std::int64_t bound) noexcept {
    const std::int64_t value = std::lcm(a, b);
    return std::min(value, bound);
}

// Largest black level of any site of each channel (see sensor.hpp).
std::array<double, 4> black_max_per_channel(const raw::SensorLevels& lv, const Plan& plan,
                                            raw::RawRepresentation kind) {
    std::array<double, 4> pattern_max{};
    if (plan.pattern != nullptr) {
        const auto& pattern = *plan.pattern;
        if (kind == raw::RawRepresentation::Mosaic) {
            pattern_max.fill(-std::numeric_limits<double>::infinity());
            // The joint period of CFA and pattern, limited to the visible area.
            const std::int64_t period_x = bounded_lcm(plan.table.width(), plan.pattern_cols, plan.width);
            const std::int64_t period_y = bounded_lcm(plan.table.height(), plan.pattern_rows, plan.height);
            for (std::int64_t y = 0; y < period_y; ++y) {
                for (std::int64_t x = 0; x < period_x; ++x) {
                    const std::uint8_t c = plan.table.at(x, y);
                    const double value = pattern[static_cast<std::size_t>((y % plan.pattern_rows) * plan.pattern_cols +
                                                                          (x % plan.pattern_cols))];
                    pattern_max[c] = std::max(pattern_max[c], value);
                }
            }
            for (double& value : pattern_max) {
                if (!std::isfinite(value)) {
                    value = 0.0;  // channel without a site in the visible area
                }
            }
        } else {
            pattern_max.fill(*std::max_element(pattern.begin(), pattern.end()));
        }
    }
    const double deltas = max_or_zero(lv.column_black_delta) + max_or_zero(lv.row_black_delta);
    std::array<double, 4> result{};
    for (std::size_t c = 0; c < 4; ++c) {
        result[c] = lv.black + lv.channel_black[c] + pattern_max[c] + deltas;
    }
    return result;
}

template <class T>
Status normalize_rows(const image::ImageBuffer<T>& frame, const Plan& plan, image::ImageBuffer<float>& out,
                      SaturationMask& mask, std::vector<std::uint64_t>& saturated_per_row,
                      std::vector<std::uint8_t>& bad_rows, bool parallel, const core::CancellationToken& cancel) {
    const auto normalize_row = [&](std::int32_t y) noexcept {
        const T* src = frame.row(plan.visible.y + y) + static_cast<std::ptrdiff_t>(plan.visible.x) * plan.channels;
        float* dst = out.row(y);
        std::uint8_t* flags = mask.row(y);
        const double row_term = plan.row_delta != nullptr ? (*plan.row_delta)[static_cast<std::size_t>(y)] : 0.0;
        const double* pattern_row =
            plan.pattern != nullptr
                ? plan.pattern->data() + static_cast<std::size_t>(y % plan.pattern_rows) *
                                             static_cast<std::size_t>(plan.pattern_cols)
                : nullptr;
        const double* column_terms = plan.column_delta != nullptr ? plan.column_delta->data() : nullptr;
        const std::uint8_t* channel_row = plan.table.row(y % plan.table.height());
        const std::int32_t table_width = plan.table.width();
        std::int32_t table_x = 0;
        std::int32_t pattern_x = 0;
        std::uint64_t saturated = 0;
        bool finite = true;
        for (std::int32_t x = 0; x < plan.width; ++x) {
            // Black level terms common to every channel of this pixel, in the order of
            // raw::SensorLevels::black_level_at().
            const double pattern_term = pattern_row != nullptr ? pattern_row[pattern_x] : 0.0;
            const double column_term = column_terms != nullptr ? column_terms[x] : 0.0;
            std::uint8_t pixel_flags = 0;
            for (std::int32_t s = 0; s < plan.channels; ++s) {
                const std::size_t c = plan.channels == 1 ? channel_row[table_x] : static_cast<std::size_t>(s);
                double black = plan.base_black[c];
                black += pattern_term;
                black += column_term;
                black += row_term;
                const T sample = src[static_cast<std::ptrdiff_t>(x) * plan.channels + s];
                if constexpr (std::is_floating_point_v<T>) {
                    finite = finite && std::isfinite(sample);
                }
                const double value = static_cast<double>(sample);
                if (value >= plan.threshold[c]) {
                    pixel_flags = static_cast<std::uint8_t>(pixel_flags | (1u << c));
                    ++saturated;
                }
                dst[static_cast<std::ptrdiff_t>(x) * plan.channels + s] =
                    static_cast<float>((value - black) / plan.range[c]);
            }
            flags[x] = pixel_flags;
            if (++table_x == table_width) {
                table_x = 0;
            }
            if (pattern_row != nullptr && ++pattern_x == plan.pattern_cols) {
                pattern_x = 0;
            }
        }
        saturated_per_row[static_cast<std::size_t>(y)] = saturated;
        bad_rows[static_cast<std::size_t>(y)] = finite ? 0 : 1;
    };
    return detail::for_each_row(plan.height, parallel, cancel, kWhereNormalize, normalize_row);
}

}  // namespace

// ---- detail::ChannelTable -------------------------------------------------------------------

Expected<detail::ChannelTable> detail::ChannelTable::create(const image::CfaPattern& cfa,
                                                            std::span<const image::CfaColor> channel_colors,
                                                            const char* where) {
    ChannelTable table;
    if (cfa.empty()) {
        return table;
    }
    table.width_ = cfa.width();
    table.height_ = cfa.height();
    table.used_ = 0;
    for (std::int32_t r = 0; r < table.height_; ++r) {
        for (std::int32_t c = 0; c < table.width_; ++c) {
            const image::CfaColor color = cfa.color_at(r, c);
            const auto found = std::find(channel_colors.begin(), channel_colors.end(), color);
            if (found == channel_colors.end() || found - channel_colors.begin() >= 4) {
                return fail(ErrorCode::InvalidArgument,
                            std::format("CFA colour {} has no channel index", image::to_string(color)), where);
            }
            const auto index = static_cast<std::uint8_t>(found - channel_colors.begin());
            table.indices_[static_cast<std::size_t>(r * table.width_ + c)] = index;
            table.used_ |= 1u << index;
        }
    }
    return table;
}

// ---- SaturationMask ---------------------------------------------------------------------------

Expected<SaturationMask> SaturationMask::create(std::int32_t width, std::int32_t height,
                                                const image::AllocationLimits& limits) {
    if (width < 1 || height < 1) {
        return fail(ErrorCode::InvalidArgument, "a saturation mask needs width and height >= 1", kWhereMask);
    }
    if (width > limits.max_dimension || height > limits.max_dimension) {
        return fail(ErrorCode::LimitExceeded,
                    std::format("a {}x{} mask exceeds the dimension limit {}", width, height, limits.max_dimension),
                    kWhereMask);
    }
    const std::uint64_t bytes = std::uint64_t{static_cast<std::uint32_t>(width)} * static_cast<std::uint32_t>(height);
    if (bytes > limits.max_bytes) {
        return fail(ErrorCode::LimitExceeded,
                    std::format("a {}x{} mask needs {} bytes, above the limit {}", width, height, bytes,
                                limits.max_bytes),
                    kWhereMask);
    }
    SaturationMask mask;
    try {
        mask.bits_.assign(static_cast<std::size_t>(bytes), std::uint8_t{0});
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, std::format("cannot allocate a {}x{} saturation mask", width, height),
                    kWhereMask);
    }
    mask.width_ = width;
    mask.height_ = height;
    return mask;
}

Expected<SaturationMask> SaturationMask::clone() const {
    SaturationMask copy;
    try {
        copy.bits_ = bits_;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot copy the saturation mask", kWhereMask);
    }
    copy.width_ = width_;
    copy.height_ = height_;
    return copy;
}

std::uint8_t SaturationMask::at(std::int32_t x, std::int32_t y) const noexcept {
    assert(x >= 0 && x < width_ && y >= 0 && y < height_);
    return bits_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)];
}

std::uint8_t* SaturationMask::row(std::int32_t y) noexcept {
    assert(y >= 0 && y < height_);
    return bits_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width_);
}

const std::uint8_t* SaturationMask::row(std::int32_t y) const noexcept {
    assert(y >= 0 && y < height_);
    return bits_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width_);
}

std::uint64_t SaturationMask::count_flagged() const noexcept {
    return static_cast<std::uint64_t>(
        std::count_if(bits_.begin(), bits_.end(), [](std::uint8_t flags) { return flags != 0; }));
}

// ---- SensorImage ------------------------------------------------------------------------------

std::int32_t SensorImage::channel_of_site(std::int32_t x, std::int32_t y) const noexcept {
    if (representation != raw::RawRepresentation::Mosaic || cfa.empty()) {
        return -1;
    }
    const image::CfaColor color = cfa.color_at(y, x);
    const auto found = std::find(channel_colors.begin(), channel_colors.end(), color);
    return found == channel_colors.end() ? -1 : static_cast<std::int32_t>(found - channel_colors.begin());
}

// ---- normalize_sensor -------------------------------------------------------------------------

Expected<SensorImage> normalize_sensor(const raw::DecodedRaw& decoded, const NormalizeOptions& options,
                                       const core::CancellationToken& cancel) {
    if (auto readable = core::check_readable(decoded.contract, raw::kDecodedRawContractVersion,
                                             "decoded-RAW contract");
        !readable) {
        Error error = std::move(readable.error());
        error.where = kWhereContract;
        return Unexpected(std::move(error));
    }
    const raw::RawMetadata& m = decoded.metadata;
    const raw::SensorLevels& lv = m.levels;
    if (auto levels = validate_levels(lv); !levels) {
        return Unexpected(std::move(levels.error()));
    }
    if (auto consistent = raw::check_consistency(decoded); !consistent) {
        return Unexpected(std::move(consistent.error()));
    }
    if (auto layout = check_layout(m); !layout) {
        return Unexpected(std::move(layout.error()));
    }
    if (auto cancelled = cancel.check(kWhereNormalize); !cancelled) {
        return Unexpected(std::move(cancelled.error()));
    }

    const raw::RepresentationInfo& rep = m.representation;
    SensorImage image;
    image.representation = rep.kind;
    image.channel_colors = rep.channel_colors;
    switch (rep.kind) {
        case raw::RawRepresentation::Mosaic:
            image.encoding = image::ColorEncoding::camera_native_mosaic();
            image.cfa = raw::cfa_at_visible_origin(m);
            break;
        case raw::RawRepresentation::LinearColor:
            image.encoding = image::ColorEncoding::camera_native_linear();
            break;
        case raw::RawRepresentation::Monochrome:
            image.encoding = raw::camera_native_monochrome();
            break;
    }

    Plan plan;
    plan.visible = m.geometry.visible_area;
    plan.width = plan.visible.width;
    plan.height = plan.visible.height;
    plan.channels = rep.kind == raw::RawRepresentation::LinearColor ? 3 : 1;
    auto table = detail::ChannelTable::create(image.cfa, image.channel_colors, kWhereLayout);
    if (!table) {
        return Unexpected(std::move(table.error()));
    }
    plan.table = *table;
    if (lv.pattern_rows != 0 && lv.pattern_cols != 0) {
        plan.pattern = &lv.black_pattern;
        plan.pattern_rows = static_cast<std::int32_t>(lv.pattern_rows);
        plan.pattern_cols = static_cast<std::int32_t>(lv.pattern_cols);
    }
    plan.column_delta = lv.column_black_delta.empty() ? nullptr : &lv.column_black_delta;
    plan.row_delta = lv.row_black_delta.empty() ? nullptr : &lv.row_black_delta;

    NormalizationRecord& record = image.normalization;
    record.source_area = plan.visible;
    record.levels_provenance = lv.provenance;
    record.white = lv.white;
    record.black_pattern_applied = plan.pattern != nullptr;
    record.column_deltas_applied = plan.column_delta != nullptr;
    record.row_deltas_applied = plan.row_delta != nullptr;
    const std::array<double, 4> black_max = black_max_per_channel(lv, plan, rep.kind);
    const std::size_t channel_count = std::min<std::size_t>(image.channel_colors.size(), 4);
    for (std::size_t c = 0; c < channel_count; ++c) {
        const double range = lv.white - black_max[c];
        if (!(range > 0.0)) {
            return fail(ErrorCode::Corrupt,
                        std::format("the white level {} is not above the highest black level {} of channel {}",
                                    lv.white, black_max[c], c),
                        kWhereLevels);
        }
        // The white level, never linear_max (see sensor.hpp, "Saturation").
        const double threshold = lv.white;
        plan.base_black[c] = lv.black + lv.channel_black[c];
        plan.range[c] = range;
        plan.threshold[c] = threshold;
        record.black_max[c] = black_max[c];
        record.saturation_threshold[c] = threshold;
        record.saturation_level[c] = static_cast<float>((threshold - black_max[c]) / range);
    }

    auto pixels = image::ImageBuffer<float>::create_uninitialized(plan.width, plan.height, plan.channels,
                                                                  options.limits);
    if (!pixels) {
        return Unexpected(std::move(pixels.error()));
    }
    auto mask = SaturationMask::create(plan.width, plan.height, options.limits);
    if (!mask) {
        return Unexpected(std::move(mask.error()));
    }
    image.pixels = std::move(*pixels);
    image.saturation = std::move(*mask);

    std::vector<std::uint64_t> saturated_per_row;
    std::vector<std::uint8_t> bad_rows;
    try {
        saturated_per_row.assign(static_cast<std::size_t>(plan.height), 0);
        bad_rows.assign(static_cast<std::size_t>(plan.height), 0);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate per-row counters", kWhereNormalize);
    }
    const Status status = std::visit(
        [&](const auto& frame) {
            return normalize_rows(frame, plan, image.pixels, image.saturation, saturated_per_row, bad_rows,
                                  options.parallel, cancel);
        },
        raw::pixels_of(decoded.image));
    if (!status) {
        return Unexpected(status.error());
    }
    const auto bad = std::find(bad_rows.begin(), bad_rows.end(), std::uint8_t{1});
    if (bad != bad_rows.end()) {
        return fail(ErrorCode::Corrupt,
                    std::format("visible row {} holds a sample that is not a finite number", bad - bad_rows.begin()),
                    kWhereSamples);
    }
    record.saturated_samples = std::accumulate(saturated_per_row.begin(), saturated_per_row.end(), std::uint64_t{0});
    return image;
}

}  // namespace lucent::engine
