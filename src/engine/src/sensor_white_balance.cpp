// Pre-demosaic white balance and highlight policy (sensor.hpp).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <new>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <lucent/engine/sensor.hpp>

#include "parallel_rows.hpp"
#include "sensor_internal.hpp"

namespace lucent::engine {

namespace {

constexpr const char* kWhereResolve = "engine.white_balance.resolve";
constexpr const char* kWhereApply = "engine.white_balance.apply";

bool usable(double value) noexcept { return std::isfinite(value) && value > 0.0; }

// Channel indices of the greens (-1 when absent).
struct Greens {
    std::int32_t green = -1;
    std::int32_t green2 = -1;

    [[nodiscard]] std::int32_t reference() const noexcept { return green >= 0 ? green : green2; }
};

Greens find_greens(const std::vector<image::CfaColor>& colors) noexcept {
    Greens g;
    for (std::size_t i = 0; i < colors.size() && i < 4; ++i) {
        if (colors[i] == image::CfaColor::Green && g.green < 0) {
            g.green = static_cast<std::int32_t>(i);
        } else if (colors[i] == image::CfaColor::Green2 && g.green2 < 0) {
            g.green2 = static_cast<std::int32_t>(i);
        }
    }
    return g;
}

// Multipliers for the first `count` channel indices, completed (a missing Green2 takes Green and
// vice versa, as LibRaw does for its fourth multiplier) and normalised to green = 1. std::nullopt
// with `reason` set when a used channel has no finite positive multiplier.
std::optional<std::array<double, 4>> normalized(std::array<double, 4> values, std::size_t count,
                                                const Greens& greens, std::string& reason) {
    if (greens.green >= 0 && greens.green2 >= 0) {
        const auto g = static_cast<std::size_t>(greens.green);
        const auto g2 = static_cast<std::size_t>(greens.green2);
        if (!usable(values[g2]) && usable(values[g])) {
            values[g2] = values[g];
        } else if (!usable(values[g]) && usable(values[g2])) {
            values[g] = values[g2];
        }
    }
    for (std::size_t c = 0; c < count; ++c) {
        if (!usable(values[c])) {
            reason = std::format("channel {} has no finite positive multiplier ({})", c, values[c]);
            return std::nullopt;
        }
    }
    const double green = values[static_cast<std::size_t>(greens.reference())];
    std::array<double, 4> result{};
    for (std::size_t c = 0; c < count; ++c) {
        result[c] = values[c] / green;
    }
    return result;
}

std::array<double, 4> widen(const std::array<float, 4>& values) noexcept {
    return {values[0], values[1], values[2], values[3]};
}

std::string list(const std::array<double, 4>& values, std::size_t count) {
    std::string text;
    for (std::size_t c = 0; c < count; ++c) {
        text += std::format("{}{}", c == 0 ? "" : ", ", values[c]);
    }
    return text;
}

std::array<double, 4> unity(std::size_t count) noexcept {
    std::array<double, 4> result{};
    for (std::size_t c = 0; c < count; ++c) {
        result[c] = 1.0;
    }
    return result;
}

// Channel indices present in the image (bit c for index c).
std::uint32_t used_channels(const SensorImage& image, const detail::ChannelTable& table) noexcept {
    switch (image.representation) {
        case raw::RawRepresentation::Mosaic:
            return table.used_channels();
        case raw::RawRepresentation::LinearColor:
            return 0b111u;
        case raw::RawRepresentation::Monochrome:
            return 0b1u;
    }
    return 0;
}

}  // namespace

std::string_view to_string(WhiteBalanceSource value) noexcept {
    switch (value) {
        case WhiteBalanceSource::AsShot:
            return "as_shot";
        case WhiteBalanceSource::Daylight:
            return "daylight";
        case WhiteBalanceSource::Custom:
            return "custom";
        case WhiteBalanceSource::AlreadyApplied:
            return "already_applied";
        case WhiteBalanceSource::NotApplicable:
            return "not_applicable";
        case WhiteBalanceSource::Unavailable:
            return "unavailable";
    }
    return "invalid";
}

std::string_view to_string(HighlightPolicy value) noexcept {
    switch (value) {
        case HighlightPolicy::ClipToLowestSaturation:
            return "clip_to_lowest_saturation";
        case HighlightPolicy::None:
            return "none";
    }
    return "invalid";
}

Expected<WhiteBalance> resolve_white_balance(const raw::RawMetadata& metadata, const WhiteBalanceOptions& options) {
    const raw::RepresentationInfo& rep = metadata.representation;
    WhiteBalance wb;
    if (rep.kind == raw::RawRepresentation::Monochrome) {
        wb.source = WhiteBalanceSource::NotApplicable;
        wb.multipliers = unity(1);
        wb.basis = "monochrome data has no colour channels to balance";
        return wb;
    }
    const std::size_t count = std::min<std::size_t>(rep.channel_colors.size(), 4);
    for (std::size_t c = 0; c < count; ++c) {
        if (!detail::is_rgb_color(rep.channel_colors[c])) {
            return fail(ErrorCode::Unsupported,
                        std::format("channel {} is {}; white balance is defined for red, green and blue channels",
                                    c, image::to_string(rep.channel_colors[c])),
                        kWhereResolve);
        }
    }
    const Greens greens = find_greens(rep.channel_colors);
    if (greens.reference() < 0 || count == 0) {
        return fail(ErrorCode::InvalidArgument, "colour data without a green channel cannot be balanced",
                    kWhereResolve);
    }
    const bool already_applied = metadata.has_prior_processing(raw::PriorProcessingKind::WhiteBalanceApplied);

    std::string reason;
    if (options.request == WhiteBalanceRequest::Custom) {
        const auto values = normalized(options.custom_multipliers, count, greens, reason);
        if (!values) {
            return fail(ErrorCode::InvalidArgument, "custom white balance: " + reason, kWhereResolve);
        }
        wb.source = WhiteBalanceSource::Custom;
        wb.multipliers = *values;
        wb.basis = std::format("custom multipliers {}", list(*values, count));
        if (already_applied) {
            wb.basis += "; the data already carries the as-shot white balance, so they act relative to it";
        }
        return wb;
    }
    if (already_applied) {
        wb.source = WhiteBalanceSource::AlreadyApplied;
        wb.multipliers = unity(count);
        wb.basis = "the decoder reports that the as-shot white balance is already applied to the samples; "
                   "unity multipliers so that it is not applied twice";
        return wb;
    }
    const raw::CameraColorData& color = metadata.color;
    const std::string provenance(raw::to_string(color.provenance));
    std::string unusable;
    if (options.request == WhiteBalanceRequest::AsShot) {
        if (const auto values = normalized(widen(color.as_shot_multipliers), count, greens, reason)) {
            wb.source = WhiteBalanceSource::AsShot;
            wb.multipliers = *values;
            wb.basis = std::format("as-shot multipliers {} (provenance {}), normalised to green = 1",
                                   list(widen(color.as_shot_multipliers), count), provenance);
            return wb;
        }
        unusable = std::format("as-shot multipliers unusable: {}; ", reason);
    }
    if (const auto values = normalized(widen(color.daylight_multipliers), count, greens, reason)) {
        wb.source = WhiteBalanceSource::Daylight;
        wb.multipliers = *values;
        wb.basis = std::format("{}daylight multipliers {} (provenance {}), normalised to green = 1", unusable,
                               list(widen(color.daylight_multipliers), count), provenance);
        return wb;
    }
    wb.source = WhiteBalanceSource::Unavailable;
    wb.multipliers = unity(count);
    wb.basis = std::format("{}daylight multipliers unusable: {}; unity multipliers, the image is not white balanced",
                           unusable, reason);
    return wb;
}

Status apply_white_balance(SensorImage& image, const WhiteBalance& white_balance, HighlightPolicy highlights,
                           const core::CancellationToken& cancel, bool parallel) {
    if (image.pixels.empty()) {
        return fail(ErrorCode::InvalidArgument, "the sensor image is empty", kWhereApply);
    }
    if (image.white_balance) {
        return fail(ErrorCode::InvalidArgument, "the sensor image is already white balanced", kWhereApply);
    }
    if (highlights != HighlightPolicy::ClipToLowestSaturation && highlights != HighlightPolicy::None) {
        return fail(ErrorCode::InvalidArgument, "unknown highlight policy", kWhereApply);
    }
    auto table = detail::ChannelTable::create(image.cfa, image.channel_colors, kWhereApply);
    if (!table) {
        return Unexpected(std::move(table.error()));
    }
    const std::uint32_t used = used_channels(image, *table);
    std::array<double, 4> multipliers{};
    double clip_level = std::numeric_limits<double>::infinity();
    for (std::size_t c = 0; c < 4; ++c) {
        if ((used & (1u << c)) == 0) {
            continue;
        }
        multipliers[c] = white_balance.multipliers[c];
        if (!usable(multipliers[c])) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("white-balance multiplier {} of channel {} is not finite and positive",
                                    multipliers[c], c),
                        kWhereApply);
        }
        clip_level = std::min(clip_level, static_cast<double>(image.normalization.saturation_level[c]) * multipliers[c]);
    }
    const float limit = highlights == HighlightPolicy::ClipToLowestSaturation
                            ? static_cast<float>(clip_level)
                            : std::numeric_limits<float>::infinity();

    const std::int32_t channels = image.pixels.channels();
    const std::int32_t width = image.pixels.width();
    std::vector<std::uint64_t> clipped_per_row;
    try {
        clipped_per_row.assign(static_cast<std::size_t>(image.pixels.height()), 0);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate per-row counters", kWhereApply);
    }
    const detail::ChannelTable& channel_table = *table;
    const auto balance_row = [&](std::int32_t y) noexcept {
        float* row = image.pixels.row(y);
        const std::uint8_t* channel_row = channel_table.row(y % channel_table.height());
        const std::int32_t table_width = channel_table.width();
        std::int32_t table_x = 0;
        std::uint64_t clipped = 0;
        for (std::int32_t x = 0; x < width; ++x) {
            for (std::int32_t s = 0; s < channels; ++s) {
                const std::size_t c = channels == 1 ? channel_row[table_x] : static_cast<std::size_t>(s);
                float& sample = row[static_cast<std::ptrdiff_t>(x) * channels + s];
                float value = static_cast<float>(static_cast<double>(sample) * multipliers[c]);
                if (value > limit) {
                    value = limit;
                    ++clipped;
                }
                sample = value;
            }
            if (++table_x == table_width) {
                table_x = 0;
            }
        }
        clipped_per_row[static_cast<std::size_t>(y)] = clipped;
    };
    if (auto status = detail::for_each_row(image.pixels.height(), parallel, cancel, kWhereApply, balance_row);
        !status) {
        return status;
    }
    WhiteBalanceRecord record;
    record.applied = white_balance;
    record.applied.multipliers = multipliers;
    record.highlights = highlights;
    record.clip_level = limit;
    record.clipped_samples = std::accumulate(clipped_per_row.begin(), clipped_per_row.end(), std::uint64_t{0});
    image.white_balance = std::move(record);
    return ok();
}

Status white_balance_sensor(SensorImage& image, const raw::RawMetadata& metadata, const WhiteBalanceOptions& options,
                            const core::CancellationToken& cancel, bool parallel) {
    auto resolved = resolve_white_balance(metadata, options);
    if (!resolved) {
        return Unexpected(std::move(resolved.error()));
    }
    return apply_white_balance(image, *resolved, options.highlights, cancel, parallel);
}

}  // namespace lucent::engine
