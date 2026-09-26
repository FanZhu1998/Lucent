// Baseline X-Trans reconstruction (demosaic.hpp): each missing colour is the mean of the
// same-colour samples in the 3 x 3 window. This is deliberately simple and marked
// RouteMaturity::Baseline; it reconstructs uniform colour fields exactly and never mixes
// colours, but it is not a quality demosaicer (no edge direction, no colour-difference
// interpolation).

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <vector>

#include <lucent/engine/demosaic.hpp>

#include "parallel_rows.hpp"

namespace lucent::engine {

namespace {

constexpr const char* kWhereInput = "engine.demosaic.input";
constexpr const char* kWhereRun = "engine.demosaic.run";

// Largest window radius: an 11 x 11 window clipped to an image of at least 6 x 6 pixels still
// spans a whole 6 x 6 period in each direction, so it holds every colour.
constexpr std::int32_t kMaxRadius = 5;
// Radius tried first; interior windows of an X-Trans tile always hold every colour at radius 1.
constexpr std::int32_t kInteriorRadius = 1;

// Output channel of a CFA colour: 0 red, 1 green (either), 2 blue.
std::int32_t output_channel(image::CfaColor color) noexcept {
    switch (color) {
        case image::CfaColor::Red:
            return 0;
        case image::CfaColor::Blue:
            return 2;
        default:
            return 1;
    }
}

struct Offset {
    std::int32_t dy = 0;
    std::int32_t dx = 0;
};

// For every phase of the period and every output channel: the offsets of the same-colour sites
// in the window of the smallest radius (up to kMaxRadius) that holds one, in row-major order.
// An empty list means the site itself has that colour.
struct PhaseTable {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t margin = 0;                   // largest radius used by any phase
    std::vector<std::array<std::vector<Offset>, 3>> entries;  // [py * width + px][channel]
    std::vector<std::int32_t> own;                            // output channel of each phase's site

    [[nodiscard]] std::size_t index(std::int32_t y, std::int32_t x) const noexcept {
        return static_cast<std::size_t>((y % height) * width + (x % width));
    }
};

PhaseTable make_phase_table(const image::CfaPattern& cfa) {
    PhaseTable table;
    table.width = cfa.width();
    table.height = cfa.height();
    table.entries.resize(static_cast<std::size_t>(table.width * table.height));
    table.own.resize(table.entries.size());
    for (std::int32_t py = 0; py < table.height; ++py) {
        for (std::int32_t px = 0; px < table.width; ++px) {
            const std::size_t i = table.index(py, px);
            table.own[i] = output_channel(cfa.color_at(py, px));
            for (std::int32_t channel = 0; channel < 3; ++channel) {
                if (channel == table.own[i]) {
                    continue;
                }
                for (std::int32_t radius = kInteriorRadius; radius <= kMaxRadius; ++radius) {
                    std::vector<Offset> found;
                    for (std::int32_t dy = -radius; dy <= radius; ++dy) {
                        for (std::int32_t dx = -radius; dx <= radius; ++dx) {
                            if (output_channel(cfa.color_at(py + dy, px + dx)) == channel) {
                                found.push_back(Offset{dy, dx});
                            }
                        }
                    }
                    if (!found.empty()) {
                        table.entries[i][static_cast<std::size_t>(channel)] = std::move(found);
                        table.margin = radius > table.margin ? radius : table.margin;
                        break;
                    }
                }
            }
        }
    }
    return table;
}

// Mean of the same-colour samples around a border pixel: the smallest window (clipped to the
// image) that holds one.
float border_mean(image::ConstImageView<float> mosaic, const image::CfaPattern& cfa, std::int32_t x, std::int32_t y,
                  std::int32_t channel) noexcept {
    for (std::int32_t radius = kInteriorRadius; radius <= kMaxRadius; ++radius) {
        float sum = 0.0f;
        std::int32_t count = 0;
        for (std::int32_t yy = y - radius; yy <= y + radius; ++yy) {
            if (yy < 0 || yy >= mosaic.height()) {
                continue;
            }
            for (std::int32_t xx = x - radius; xx <= x + radius; ++xx) {
                if (xx < 0 || xx >= mosaic.width() || output_channel(cfa.color_at(yy, xx)) != channel) {
                    continue;
                }
                sum += mosaic.row(yy)[xx];
                ++count;
            }
        }
        if (count > 0) {
            return sum / static_cast<float>(count);
        }
    }
    return 0.0f;  // unreachable for images of at least 6 x 6 pixels (checked by the caller)
}

}  // namespace

Status demosaic_xtrans_baseline(image::ConstImageView<float> mosaic, const image::CfaPattern& cfa,
                                image::ImageView<float> rgb, const core::CancellationToken& cancel, bool parallel) {
    if (cfa.empty() || !cfa.is_xtrans()) {
        return fail(ErrorCode::InvalidArgument,
                    "the X-Trans baseline needs an X-Trans pattern, not '" + cfa.to_string() + "'", kWhereInput);
    }
    if (mosaic.empty() || mosaic.channels() != 1) {
        return fail(ErrorCode::InvalidArgument, "the X-Trans baseline needs a single-channel mosaic", kWhereInput);
    }
    if (mosaic.width() < 6 || mosaic.height() < 6) {
        return fail(ErrorCode::InvalidArgument, "the X-Trans baseline needs a mosaic of at least 6 x 6 pixels",
                    kWhereInput);
    }
    if (rgb.empty() || rgb.channels() != 3 || rgb.width() != mosaic.width() || rgb.height() != mosaic.height()) {
        return fail(ErrorCode::InvalidArgument, "the X-Trans baseline needs a 3-channel output of the mosaic's size",
                    kWhereInput);
    }
    PhaseTable table;
    try {
        table = make_phase_table(cfa);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the X-Trans phase table", kWhereInput);
    }
    const std::int32_t width = mosaic.width();
    const std::int32_t height = mosaic.height();
    const std::int32_t margin = table.margin;

    const auto run_row = [&](std::int32_t y) noexcept {
        float* out = rgb.row(y);
        const float* centre_row = mosaic.row(y);
        const bool interior_row = y >= margin && y < height - margin;
        for (std::int32_t x = 0; x < width; ++x) {
            const std::size_t phase = table.index(y, x);
            const std::int32_t own = table.own[phase];
            float* pixel = out + static_cast<std::ptrdiff_t>(x) * 3;
            const bool interior = interior_row && x >= margin && x < width - margin;
            for (std::int32_t channel = 0; channel < 3; ++channel) {
                if (channel == own) {
                    pixel[channel] = centre_row[x];
                    continue;
                }
                if (!interior) {
                    pixel[channel] = border_mean(mosaic, cfa, x, y, channel);
                    continue;
                }
                const std::vector<Offset>& offsets = table.entries[phase][static_cast<std::size_t>(channel)];
                float sum = 0.0f;
                for (const Offset& o : offsets) {
                    sum += mosaic.row(y + o.dy)[x + o.dx];
                }
                pixel[channel] = sum / static_cast<float>(offsets.size());
            }
        }
    };
    return detail::for_each_row(height, parallel, cancel, kWhereRun, run_row);
}

}  // namespace lucent::engine
