// DNG opcode processing (dng_opcode_processing.hpp, ADR 0014).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <lucent/engine/dng_opcode_processing.hpp>

#include "parallel_rows.hpp"

namespace lucent::engine {

namespace {

using Clock = std::chrono::steady_clock;
using raw::DngOpcode;
using raw::DngOpcodeId;
using raw::DngOpcodeListId;

constexpr const char* kWhere = "engine.dng_opcode";
constexpr double kStoredRange = 65535.0;  // logical range of 16-bit stored samples (List 1)
constexpr std::int32_t kMaxBadPixelRadius = 16;
// Bound of the unclipped float results of Lists 2 and 3 (see kDngOpcodeValueBound).
constexpr double kValueBound = kDngOpcodeValueBound;

double seconds_since(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }

bool is(const DngOpcode& opcode, DngOpcodeId id) noexcept { return opcode.id == static_cast<std::uint32_t>(id); }

int list_number(DngOpcodeListId list) noexcept { return static_cast<int>(list); }

// The stage image of one list run and where Lucent's buffer lies in it.
struct Frame {
    std::int64_t stage_width = 0;
    std::int64_t stage_height = 0;
    std::int64_t offset_x = 0;  // buffer pixel (0, 0) is stage pixel (offset_x, offset_y)
    std::int64_t offset_y = 0;
    std::int32_t width = 0;  // buffer size
    std::int32_t height = 0;
    std::int32_t channels = 1;
};

// One affected row (or column): its buffer index, stage coordinate and position among the
// area's affected rows ((stage - start) / pitch).
struct Line {
    std::int32_t buffer = 0;
    std::int64_t stage = 0;
    std::uint32_t step = 0;
};

// The rows (or columns) [lo, hi) with the given pitch that lie inside the stage image and the
// buffer.
std::vector<Line> affected_lines(std::uint32_t lo, std::uint32_t hi, std::uint32_t pitch, std::int64_t stage_extent,
                                 std::int64_t offset, std::int32_t buffer_extent) {
    std::vector<Line> lines;
    const std::int64_t end = std::min<std::int64_t>(hi, stage_extent);
    const std::int64_t first_visible = std::max<std::int64_t>(lo, offset);
    if (pitch == 0 || first_visible >= end) {
        return lines;
    }
    std::int64_t s = lo + ((first_visible - lo + pitch - 1) / pitch) * pitch;
    for (; s < end; s += pitch) {
        const std::int64_t b = s - offset;
        if (b >= buffer_extent) {
            break;
        }
        if (b >= 0) {
            lines.push_back(Line{static_cast<std::int32_t>(b), s, static_cast<std::uint32_t>((s - lo) / pitch)});
        }
    }
    return lines;
}

// The planes [first, last) of an area that the buffer has.
std::pair<std::int32_t, std::int32_t> area_planes(const raw::DngOpcodeArea& area, std::int32_t channels) {
    const std::int64_t first = area.plane;
    const std::int64_t last = std::min<std::int64_t>(std::int64_t{area.plane} + area.planes, channels);
    if (first >= last) {
        return {0, 0};
    }
    return {static_cast<std::int32_t>(first), static_cast<std::int32_t>(last)};
}

// Writes a computed value into a sample: 16-bit stored samples are rounded to nearest (halves
// away from zero) and limited to [0, 65535]; floats are stored as computed.
inline void store(std::uint16_t& sample, double value, std::uint64_t& limited) noexcept {
    double rounded = std::round(value);
    if (!(rounded >= 0.0)) {  // also NaN
        rounded = 0.0;
        ++limited;
    } else if (rounded > kStoredRange) {
        rounded = kStoredRange;
        ++limited;
    }
    sample = static_cast<std::uint16_t>(rounded);
}

// Floats are stored as computed within [-kValueBound, kValueBound]; a result beyond it (or NaN,
// which finite inputs and finite parameters cannot give) is limited and counted, so that a huge
// but finite coefficient can never make later stages overflow.
inline void store(float& sample, double value, std::uint64_t& limited) noexcept {
    if (!(value >= -kValueBound)) {  // also NaN
        value = std::isnan(value) ? 0.0 : -kValueBound;
        ++limited;
    } else if (value > kValueBound) {
        value = kValueBound;
        ++limited;
    }
    sample = static_cast<float>(value);
}

// Limits a value-map operand or result to [0, 1], counting changes.
inline double unit(double value, std::uint64_t& limited) noexcept {
    if (value < 0.0) {
        ++limited;
        return 0.0;
    }
    if (value > 1.0) {
        ++limited;
        return 1.0;
    }
    return value;
}

// The area as it applies to the stage image: an empty rectangle stands for the whole image (the
// DNG SDK's dng_area_spec::Overlap; the parser requires pitch 1 for it).
raw::DngOpcodeArea effective_area(const raw::DngOpcodeArea& area, const Frame& f) {
    if (!area.rect.empty()) {
        return area;
    }
    raw::DngOpcodeArea whole = area;
    whole.rect = raw::DngOpcodeRect{0, 0, static_cast<std::uint32_t>(std::min<std::int64_t>(f.stage_height, 0xFFFFFFFF)),
                                    static_cast<std::uint32_t>(std::min<std::int64_t>(f.stage_width, 0xFFFFFFFF))};
    return whole;
}

// Calls fn(value, row line, column line, plane, limited&) -> new value for every affected sample
// of `area` (rows in parallel); counts written and limited samples into `record`. `area` must be
// effective_area()'s (a non-empty rectangle).
template <class T, class Fn>
Status for_area(image::ImageView<T> view, const Frame& f, const raw::DngOpcodeArea& area, bool parallel,
                const core::CancellationToken& cancel, DngOpcodeRecord& record, const Fn& fn) {
    const std::vector<Line> rows = affected_lines(area.rect.top, area.rect.bottom, area.row_pitch, f.stage_height,
                                                  f.offset_y, f.height);
    const std::vector<Line> cols = affected_lines(area.rect.left, area.rect.right, area.col_pitch, f.stage_width,
                                                  f.offset_x, f.width);
    const auto planes = area_planes(area, f.channels);
    const std::int32_t first_plane = planes.first;
    const std::int32_t last_plane = planes.second;
    if (rows.empty() || cols.empty() || first_plane >= last_plane) {
        return ok();
    }
    std::vector<std::uint64_t> limited(rows.size(), 0);
    auto status = detail::for_each_row(static_cast<std::int32_t>(rows.size()), parallel, cancel, kWhere,
                                       [&](std::int32_t i) {
                                           const Line& row = rows[static_cast<std::size_t>(i)];
                                           T* line = view.row(row.buffer);
                                           std::uint64_t& count = limited[static_cast<std::size_t>(i)];
                                           for (const Line& col : cols) {
                                               T* pixel = line + static_cast<std::ptrdiff_t>(col.buffer) * f.channels;
                                               for (std::int32_t p = first_plane; p < last_plane; ++p) {
                                                   const double value = fn(static_cast<double>(pixel[p]), row, col, p,
                                                                           count);
                                                   store(pixel[p], value, count);
                                               }
                                           }
                                       });
    if (!status) {
        return status;
    }
    record.affected_samples += static_cast<std::uint64_t>(rows.size()) * cols.size() *
                               static_cast<std::uint64_t>(last_plane - first_plane);
    for (const std::uint64_t n : limited) {
        record.limited_samples += n;
    }
    return ok();
}

// The whole stage image, every plane.
raw::DngOpcodeArea whole_image(const Frame& f) {
    raw::DngOpcodeArea area;
    area.rect = raw::DngOpcodeRect{0, 0, static_cast<std::uint32_t>(std::min<std::int64_t>(f.stage_height, 0xFFFFFFFF)),
                                   static_cast<std::uint32_t>(std::min<std::int64_t>(f.stage_width, 0xFFFFFFFF))};
    area.plane = 0;
    area.planes = static_cast<std::uint32_t>(f.channels);
    return area;
}

std::string describe_area(const raw::DngOpcodeArea& a) {
    if (a.rect.empty()) {
        return std::format("empty area rectangle = the whole image, planes {}..{}", a.plane,
                           std::uint64_t{a.plane} + a.planes - 1);
    }
    return std::format("area rows {}..{} / columns {}..{} (pitch {} x {}), planes {}..{}", a.rect.top, a.rect.bottom,
                       a.rect.left, a.rect.right, a.row_pitch, a.col_pitch, a.plane,
                       std::uint64_t{a.plane} + a.planes - 1);
}

// ---- Geometry shared by the radial opcodes ---------------------------------------------------------

// Optical centre and m (distance to the farthest corner pixel) of a stage image, from the
// normalised centre (DNG: c = x0 + c^ (x1 - x0) with x0 = 0, x1 = W - 1).
struct Radial {
    double cx = 0.0;
    double cy = 0.0;
    double m = 0.0;
};

Radial radial_frame(const Frame& f, double center_x, double center_y) {
    Radial r;
    const double x1 = static_cast<double>(f.stage_width - 1);
    const double y1 = static_cast<double>(f.stage_height - 1);
    r.cx = center_x * x1;
    r.cy = center_y * y1;
    const double mx = std::max(std::abs(r.cx), std::abs(x1 - r.cx));
    const double my = std::max(std::abs(r.cy), std::abs(y1 - r.cy));
    r.m = std::sqrt(mx * mx + my * my);
    return r;
}

// Input position (x', y') of output stage pixel (x, y) for one coefficient set (DNG 1.7.1.0
// WarpRectilinear / WarpRectilinear2).
image::PointD warp_source(const raw::DngWarpPlane& k, bool extended, bool reciprocal, const Radial& c, double x,
                          double y) noexcept {
    if (!(c.m > 0.0)) {
        return {x, y};
    }
    const double dx = (x - c.cx) / c.m;
    const double dy = (y - c.cy) / c.m;
    const double r2 = dx * dx + dy * dy;
    double f = 0.0;
    if (extended) {
        const double r = std::clamp(std::sqrt(r2), k.min_valid_radius, k.max_valid_radius);
        for (std::size_t p = k.radial.size(); p-- > 0;) {
            f = f * r + k.radial[p];
        }
        if (reciprocal) {
            f = 1.0 / f;  // f == 0 gives an infinite position, reported by the caller
        }
    } else {
        f = k.radial[0] + r2 * (k.radial[2] + r2 * (k.radial[4] + r2 * k.radial[6]));
    }
    const double kt0 = k.tangential[0];
    const double kt1 = k.tangential[1];
    const double xt = kt0 * (2.0 * dx * dy) + kt1 * (r2 + 2.0 * dx * dx);
    const double yt = kt1 * (2.0 * dx * dy) + kt0 * (r2 + 2.0 * dy * dy);
    return {c.cx + c.m * (f * dx + xt), c.cy + c.m * (f * dy + yt)};
}

// Keys cubic convolution with a = -0.5 (Catmull-Rom): the weights of taps -1, 0, 1, 2 for a
// fractional offset t in [0, 1).
inline std::array<double, 4> catmull_rom(double t) noexcept {
    const double t2 = t * t;
    const double t3 = t2 * t;
    return {0.5 * (-t3 + 2.0 * t2 - t), 0.5 * (3.0 * t3 - 5.0 * t2 + 2.0), 0.5 * (-3.0 * t3 + 4.0 * t2 + t),
            0.5 * (t3 - t2)};
}

// Tap positions and weights for a buffer coordinate (taps clamped to [0, extent - 1]).
struct Taps {
    std::array<std::int32_t, 4> index{};
    std::array<double, 4> weight{};
};

inline Taps taps_for(double position, std::int32_t extent) noexcept {
    Taps taps;
    const double floor = std::floor(position);
    const auto base = static_cast<std::int64_t>(floor);
    taps.weight = catmull_rom(position - floor);
    for (std::int32_t k = 0; k < 4; ++k) {
        taps.index[static_cast<std::size_t>(k)] =
            static_cast<std::int32_t>(std::clamp<std::int64_t>(base - 1 + k, 0, extent - 1));
    }
    return taps;
}

std::string describe_warp(const raw::DngWarpRectilinear& w, const Radial& c) {
    std::string text = std::format("{} with {} coefficient set(s), optical centre ({}, {}) = stage pixel ({:.3f}, "
                                   "{:.3f}), m = {:.3f} px, Catmull-Rom resampling",
                                   w.extended ? "WarpRectilinear2" : "WarpRectilinear", w.planes.size(), w.center_x,
                                   w.center_y, c.cx, c.cy, c.m);
    for (std::size_t i = 0; i < w.planes.size(); ++i) {
        const raw::DngWarpPlane& k = w.planes[i];
        if (w.extended) {
            text += std::format("; set {}: radial [{:.6g}", i, k.radial[0]);
            for (std::size_t p = 1; p < k.radial.size(); ++p) {
                text += std::format(", {:.6g}", k.radial[p]);
            }
            text += std::format("], tangential [{:.6g}, {:.6g}], valid radius [{}, {}]{}", k.tangential[0],
                                k.tangential[1], k.min_valid_radius, k.max_valid_radius,
                                w.reciprocal_radial ? ", reciprocal" : "");
        } else {
            text += std::format("; set {}: kr [{:.6g}, {:.6g}, {:.6g}, {:.6g}], kt [{:.6g}, {:.6g}]", i, k.radial[0],
                                k.radial[2], k.radial[4], k.radial[6], k.tangential[0], k.tangential[1]);
        }
    }
    return text;
}

// ---- Bad pixels ------------------------------------------------------------------------------------

// Neighbour offsets of one Bayer lattice grouped into rings of equal distance, nearest first.
struct Rings {
    std::vector<std::array<std::int32_t, 2>> offsets;  // (dx, dy)
    std::vector<std::size_t> ring_end;                 // end index of each ring in `offsets`
};

Rings make_rings(bool green) {
    struct Offset {
        std::int32_t dx;
        std::int32_t dy;
        std::int32_t d2;
    };
    std::vector<Offset> all;
    for (std::int32_t dy = -kMaxBadPixelRadius; dy <= kMaxBadPixelRadius; ++dy) {
        for (std::int32_t dx = -kMaxBadPixelRadius; dx <= kMaxBadPixelRadius; ++dx) {
            if (dx == 0 && dy == 0) {
                continue;
            }
            const bool same = green ? ((dx + dy) % 2 == 0) : (dx % 2 == 0 && dy % 2 == 0);
            const std::int32_t d2 = dx * dx + dy * dy;
            if (same && d2 <= kMaxBadPixelRadius * kMaxBadPixelRadius) {
                all.push_back(Offset{dx, dy, d2});
            }
        }
    }
    std::stable_sort(all.begin(), all.end(), [](const Offset& a, const Offset& b) { return a.d2 < b.d2; });
    Rings rings;
    for (std::size_t i = 0; i < all.size(); ++i) {
        rings.offsets.push_back({all[i].dx, all[i].dy});
        if (i + 1 == all.size() || all[i + 1].d2 != all[i].d2) {
            rings.ring_end.push_back(rings.offsets.size());
        }
    }
    return rings;
}

// One bit per buffer pixel.
class BitMask {
public:
    BitMask(std::int32_t width, std::int32_t height)
        : width_(width), words_per_row_((static_cast<std::size_t>(width) + 63) / 64),
          bits_(words_per_row_ * static_cast<std::size_t>(height), 0) {}

    void set(std::int32_t x, std::int32_t y) noexcept {
        bits_[static_cast<std::size_t>(y) * words_per_row_ + static_cast<std::size_t>(x) / 64] |=
            std::uint64_t{1} << (static_cast<unsigned>(x) % 64);
    }
    [[nodiscard]] bool test(std::int32_t x, std::int32_t y) const noexcept {
        return ((bits_[static_cast<std::size_t>(y) * words_per_row_ + static_cast<std::size_t>(x) / 64] >>
                 (static_cast<unsigned>(x) % 64)) &
                1u) != 0;
    }
    [[nodiscard]] bool row_empty(std::int32_t y) const noexcept {
        const auto begin = bits_.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * words_per_row_);
        return std::all_of(begin, begin + static_cast<std::ptrdiff_t>(words_per_row_),
                           [](std::uint64_t w) { return w == 0; });
    }
    [[nodiscard]] std::int32_t width() const noexcept { return width_; }

private:
    std::int32_t width_;
    std::size_t words_per_row_;
    std::vector<std::uint64_t> bits_;
};

// Called with the buffer position and new value of every repaired pixel, on the thread that
// repairs its row (it may write per-row state of that row only).
using RepairedFn = std::function<void(std::int32_t x, std::int32_t y, double value)>;

// Repairs the marked pixels of a one-channel Bayer buffer (see the header). `green_even`: green
// sites are those whose stage coordinates have an even sum.
template <class T>
Status repair_bad_pixels(image::ImageView<T> view, const Frame& f, const BitMask& bad, bool green_even, bool parallel,
                         const core::CancellationToken& cancel, DngOpcodeRecord& record, const RepairedFn& on_repaired) {
    static const Rings green_rings = make_rings(true);
    static const Rings other_rings = make_rings(false);
    std::vector<std::uint64_t> repaired(static_cast<std::size_t>(f.height), 0);
    std::vector<std::uint64_t> unrepaired(static_cast<std::size_t>(f.height), 0);
    auto status = detail::for_each_row(f.height, parallel, cancel, kWhere, [&](std::int32_t y) {
        if (bad.row_empty(y)) {
            return;
        }
        T* line = view.row(y);
        for (std::int32_t x = 0; x < f.width; ++x) {
            if (!bad.test(x, y)) {
                continue;
            }
            const std::int64_t sx = x + f.offset_x;
            const std::int64_t sy = y + f.offset_y;
            const bool green = ((sx + sy) % 2 == 0) == green_even;
            const Rings& rings = green ? green_rings : other_rings;
            double sum = 0.0;
            std::int64_t integer_sum = 0;
            std::int32_t count = 0;
            std::size_t begin = 0;
            for (const std::size_t end : rings.ring_end) {
                for (std::size_t k = begin; k < end; ++k) {
                    const std::int32_t nx = x + rings.offsets[k][0];
                    const std::int32_t ny = y + rings.offsets[k][1];
                    if (nx < 0 || ny < 0 || nx >= f.width || ny >= f.height || bad.test(nx, ny)) {
                        continue;
                    }
                    const T value = view.row(ny)[nx];
                    sum += static_cast<double>(value);
                    if constexpr (std::is_integral_v<T>) {
                        integer_sum += value;
                    }
                    ++count;
                }
                begin = end;
                if (count > 0) {
                    break;
                }
            }
            if (count == 0) {
                ++unrepaired[static_cast<std::size_t>(y)];
                continue;
            }
            if constexpr (std::is_integral_v<T>) {
                line[x] = static_cast<T>((integer_sum + count / 2) / count);
            } else {
                line[x] = static_cast<T>(sum / count);
            }
            if (on_repaired) {
                on_repaired(x, y, static_cast<double>(line[x]));
            }
            ++repaired[static_cast<std::size_t>(y)];
        }
    });
    if (!status) {
        return status;
    }
    for (std::size_t i = 0; i < repaired.size(); ++i) {
        record.affected_samples += repaired[i];
        record.unrepaired_pixels += unrepaired[i];
    }
    return ok();
}

// Marks the listed points and rectangles (stage coordinates) that lie in the buffer.
std::uint64_t mark_listed(BitMask& bad, const Frame& f, const raw::DngFixBadPixelsList& list) {
    std::uint64_t outside = 0;
    for (const raw::DngBadPoint& p : list.points) {
        const std::int64_t x = std::int64_t{p.column} - f.offset_x;
        const std::int64_t y = std::int64_t{p.row} - f.offset_y;
        if (p.column >= f.stage_width || p.row >= f.stage_height || x < 0 || y < 0 || x >= f.width || y >= f.height) {
            ++outside;
            continue;
        }
        bad.set(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y));
    }
    for (const raw::DngOpcodeRect& r : list.rects) {
        const std::int64_t x0 = std::max<std::int64_t>(std::int64_t{r.left} - f.offset_x, 0);
        const std::int64_t y0 = std::max<std::int64_t>(std::int64_t{r.top} - f.offset_y, 0);
        const std::int64_t x1 = std::min<std::int64_t>(std::min<std::int64_t>(r.right, f.stage_width) - f.offset_x,
                                                       f.width);
        const std::int64_t y1 = std::min<std::int64_t>(std::min<std::int64_t>(r.bottom, f.stage_height) - f.offset_y,
                                                       f.height);
        for (std::int64_t y = y0; y < y1; ++y) {
            for (std::int64_t x = x0; x < x1; ++x) {
                bad.set(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y));
            }
        }
    }
    return outside;
}

bool green_even_for(std::uint32_t bayer_phase) noexcept { return bayer_phase == 1 || bayer_phase == 2; }

// ---- Gain maps ---------------------------------------------------------------------------------------

// Grid position along one axis for a pixel centre, map coordinate ((pixel + 0.5) / extent -
// origin) / spacing; indices are clamped to the grid ("outside the gain map bounds, values are
// replicated from the edges of the gain map", DNG 1.7.1.0 GainMap).
struct GridPosition {
    std::uint32_t i0 = 0;
    std::uint32_t i1 = 0;
    double fraction = 0.0;
};

GridPosition grid_position(std::int64_t pixel, std::int64_t extent, double origin, double spacing,
                           std::uint32_t points) noexcept {
    GridPosition g;
    if (points <= 1) {
        return g;
    }
    const double index = ((static_cast<double>(pixel) + 0.5) / static_cast<double>(extent) - origin) / spacing;
    const double last = static_cast<double>(points - 1);
    if (!(index > 0.0)) {
        return g;
    }
    if (index >= last) {
        g.i0 = points - 1;
        g.i1 = points - 1;
        return g;
    }
    g.i0 = static_cast<std::uint32_t>(index);
    g.i1 = g.i0 + 1;
    g.fraction = index - static_cast<double>(g.i0);
    return g;
}

// ---- Buffers -----------------------------------------------------------------------------------------

template <class T>
Expected<image::ImageBuffer<T>> crop_buffer(const image::ImageBuffer<T>& source, const image::PixelRect& rect,
                                            const image::AllocationLimits& limits) {
    auto out = image::ImageBuffer<T>::create_uninitialized(rect.width, rect.height, source.channels(), limits);
    if (!out) {
        return Unexpected(std::move(out.error()));
    }
    const std::size_t row_bytes = static_cast<std::size_t>(rect.width) * static_cast<std::size_t>(source.channels()) *
                                  sizeof(T);
    for (std::int32_t y = 0; y < rect.height; ++y) {
        std::memcpy(out->row(y), source.row(rect.y + y) + static_cast<std::ptrdiff_t>(rect.x) * source.channels(),
                    row_bytes);
    }
    return out;
}

// The part of the buffer inside a TrimBounds rectangle, in buffer coordinates.
image::PixelRect trim_rect(const Frame& f, const raw::DngOpcodeRect& b) {
    const std::int64_t x0 = std::max<std::int64_t>(std::int64_t{b.left} - f.offset_x, 0);
    const std::int64_t y0 = std::max<std::int64_t>(std::int64_t{b.top} - f.offset_y, 0);
    const std::int64_t x1 = std::min<std::int64_t>(std::int64_t{b.right} - f.offset_x, f.width);
    const std::int64_t y1 = std::min<std::int64_t>(std::int64_t{b.bottom} - f.offset_y, f.height);
    if (x1 <= x0 || y1 <= y0) {
        return {};
    }
    return image::PixelRect{static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0),
                            static_cast<std::int32_t>(x1 - x0), static_cast<std::int32_t>(y1 - y0)};
}

// ---- One opcode on one buffer ---------------------------------------------------------------------

// Everything an opcode run needs besides the opcode.
template <class T>
struct Target {
    image::ImageView<T> view;
    Frame frame;
    DngOpcodeListId list = DngOpcodeListId::List1;
    std::array<double, 4> multipliers{1.0, 1.0, 1.0, 1.0};  // List 3 white balance per plane
    bool parallel = true;
    RepairedFn on_repaired;  // bad-pixel opcodes: told about every repaired pixel
};

// The opcode is marked supported but its parameters break an invariant the parser guarantees (a
// hand-built or inconsistent DngOpcode): a contract violation, never an out-of-bounds read.
Status malformed_supported(const DngOpcode& opcode, std::string_view what) {
    return fail(ErrorCode::Internal,
                std::format("OpcodeList{}[{}] {} is marked supported but {}", list_number(opcode.list), opcode.index,
                            opcode.name(), what),
                kWhere);
}

template <class T>
Status apply_value_opcode(const DngOpcode& opcode, Target<T>& t, const core::CancellationToken& cancel,
                          DngOpcodeRecord& record) {
    const bool list1 = t.list == DngOpcodeListId::List1;
    const auto m = [&](std::int32_t p) { return t.list == DngOpcodeListId::List3 ? t.multipliers[p] : 1.0; };
    switch (static_cast<DngOpcodeId>(opcode.id)) {
        case DngOpcodeId::GainMap: {
            const auto& map = std::get<raw::DngGainMap>(opcode.parameters);
            if (!map.complete()) {
                return malformed_supported(opcode, "its gains do not fill its points_v x points_h x map_planes grid");
            }
            const raw::DngOpcodeArea area = effective_area(map.area, t.frame);
            const std::vector<Line> rows = affected_lines(area.rect.top, area.rect.bottom, area.row_pitch,
                                                          t.frame.stage_height, t.frame.offset_y, t.frame.height);
            const std::vector<Line> cols = affected_lines(area.rect.left, area.rect.right, area.col_pitch,
                                                          t.frame.stage_width, t.frame.offset_x, t.frame.width);
            std::vector<GridPosition> v(rows.size());
            std::vector<GridPosition> h(cols.size());
            for (std::size_t i = 0; i < rows.size(); ++i) {
                v[i] = grid_position(rows[i].stage, t.frame.stage_height, map.origin_v, map.spacing_v, map.points_v);
            }
            for (std::size_t j = 0; j < cols.size(); ++j) {
                h[j] = grid_position(cols[j].stage, t.frame.stage_width, map.origin_h, map.spacing_h, map.points_h);
            }
            // Index of a line in `rows` / `cols` from its step (lines are consecutive steps).
            const std::uint32_t row_step0 = rows.empty() ? 0 : rows.front().step;
            const std::uint32_t col_step0 = cols.empty() ? 0 : cols.front().step;
            auto [g_min, g_max] = std::minmax_element(map.gains.begin(), map.gains.end());
            record.detail = std::format("{} x {} points ({} map plane(s)), spacing {:.6g} x {:.6g}, origin ({:.6g}, "
                                        "{:.6g}), gains {:.4f}..{:.4f}; {}",
                                        map.points_v, map.points_h, map.map_planes, map.spacing_v, map.spacing_h,
                                        map.origin_v, map.origin_h, *g_min, *g_max, describe_area(map.area));
            return for_area(t.view, t.frame, area, t.parallel, cancel, record,
                            [&](double value, const Line& row, const Line& col, std::int32_t p, std::uint64_t&) {
                                const GridPosition& gv = v[row.step - row_step0];
                                const GridPosition& gh = h[col.step - col_step0];
                                // Image plane p uses map plane min(p, MapPlanes - 1): the absolute
                                // plane, as the DNG SDK (dng_opcode_GainMap::ProcessArea) indexes it.
                                const std::uint32_t mp =
                                    std::min<std::uint32_t>(static_cast<std::uint32_t>(p), map.map_planes - 1);
                                const double top = map.gain(gv.i0, gh.i0, mp) +
                                                   gh.fraction * (map.gain(gv.i0, gh.i1, mp) - map.gain(gv.i0, gh.i0, mp));
                                const double bottom =
                                    map.gain(gv.i1, gh.i0, mp) +
                                    gh.fraction * (map.gain(gv.i1, gh.i1, mp) - map.gain(gv.i1, gh.i0, mp));
                                return value * (top + gv.fraction * (bottom - top));
                            });
        }
        case DngOpcodeId::FixVignetteRadial: {
            const auto& v = std::get<raw::DngFixVignetteRadial>(opcode.parameters);
            const Radial c = radial_frame(t.frame, v.center_x, v.center_y);
            const double inv_m2 = c.m > 0.0 ? 1.0 / (c.m * c.m) : 0.0;
            const auto gain = [&](double r2) {
                return 1.0 + r2 * (v.k[0] + r2 * (v.k[1] + r2 * (v.k[2] + r2 * (v.k[3] + r2 * v.k[4]))));
            };
            record.detail = std::format("k [{:.6g}, {:.6g}, {:.6g}, {:.6g}, {:.6g}], optical centre ({}, {}) = stage "
                                        "pixel ({:.3f}, {:.3f}), m = {:.3f} px; gain at r = 1: {:.6g}",
                                        v.k[0], v.k[1], v.k[2], v.k[3], v.k[4], v.center_x, v.center_y, c.cx, c.cy,
                                        c.m, gain(1.0));
            return for_area(t.view, t.frame, whole_image(t.frame), t.parallel, cancel, record,
                            [&](double value, const Line& row, const Line& col, std::int32_t, std::uint64_t&) {
                                const double dx = static_cast<double>(col.stage) - c.cx;
                                const double dy = static_cast<double>(row.stage) - c.cy;
                                return value * gain((dx * dx + dy * dy) * inv_m2);
                            });
        }
        case DngOpcodeId::MapTable: {
            const auto& map = std::get<raw::DngMapTable>(opcode.parameters);
            if (map.table.empty()) {
                return malformed_supported(opcode, "its table is empty");
            }
            const std::size_t last = map.table.size() - 1;
            record.detail = std::format("{} entries ({}..{}); {}", map.table.size(), map.table.front(),
                                        map.table.back(), describe_area(map.area));
            return for_area(t.view, t.frame, effective_area(map.area, t.frame), t.parallel, cancel, record,
                            [&](double value, const Line&, const Line&, std::int32_t p, std::uint64_t& limited) {
                                if (list1) {
                                    const auto index = static_cast<std::size_t>(std::clamp(value, 0.0, kStoredRange));
                                    return static_cast<double>(map.table[std::min(index, last)]);
                                }
                                const double x = unit(value / m(p), limited);
                                const auto index = static_cast<std::size_t>(std::lround(x * kStoredRange));
                                return static_cast<double>(map.table[std::min(index, last)]) / kStoredRange * m(p);
                            });
        }
        case DngOpcodeId::MapPolynomial: {
            const auto& map = std::get<raw::DngMapPolynomial>(opcode.parameters);
            if (map.coefficients.empty()) {
                return malformed_supported(opcode, "it has no coefficients");
            }
            const auto poly = [&](double x) {
                double y = 0.0;
                for (std::size_t k = map.coefficients.size(); k-- > 0;) {
                    y = y * x + map.coefficients[k];
                }
                return y;
            };
            record.detail = std::format("degree {}, coefficients [", map.coefficients.size() - 1);
            for (std::size_t k = 0; k < map.coefficients.size(); ++k) {
                record.detail += std::format("{}{:.6g}", k == 0 ? "" : ", ", map.coefficients[k]);
            }
            record.detail += "]; " + describe_area(map.area);
            return for_area(t.view, t.frame, effective_area(map.area, t.frame), t.parallel, cancel, record,
                            [&](double value, const Line&, const Line&, std::int32_t p, std::uint64_t& limited) {
                                if (list1) {
                                    // On the stored values themselves: the DNG SDK rescales the
                                    // coefficients (c_k x 65535^(k - 1)) so that its [0, 1] float
                                    // stage-1 buffer gets y = sum c_k v^k in stored units.
                                    return poly(value);
                                }
                                return unit(poly(unit(value / m(p), limited)), limited) * m(p);
                            });
        }
        case DngOpcodeId::DeltaPerRow:
        case DngOpcodeId::DeltaPerColumn:
        case DngOpcodeId::ScalePerRow:
        case DngOpcodeId::ScalePerColumn: {
            const auto& values = std::get<raw::DngRowColumnValues>(opcode.parameters);
            const bool per_row = is(opcode, DngOpcodeId::DeltaPerRow) || is(opcode, DngOpcodeId::ScalePerRow);
            const bool delta = is(opcode, DngOpcodeId::DeltaPerRow) || is(opcode, DngOpcodeId::DeltaPerColumn);
            if (values.area.rect.empty() ||
                values.values.size() != (per_row ? values.area.affected_rows() : values.area.affected_columns())) {
                return malformed_supported(opcode, "it does not hold one value per affected row or column");
            }
            const auto [lo, hi] = std::minmax_element(values.values.begin(), values.values.end());
            record.detail = std::format("{} {} values {:.6g}..{:.6g}; {}", values.values.size(),
                                        delta ? "delta" : "scale", *lo, *hi, describe_area(values.area));
            return for_area(t.view, t.frame, values.area, t.parallel, cancel, record,
                            [&](double value, const Line& row, const Line& col, std::int32_t p, std::uint64_t&) {
                                const double v = values.values[per_row ? row.step : col.step];
                                if (!delta) {
                                    return value * v;
                                }
                                // List 1: in stored units (the DNG SDK scales the delta by 1 / 65535
                                // into its [0, 1] float stage-1 buffer). Lists 2 and 3: in the logical
                                // [0, 1] range, times the plane's white-balance multiplier in List 3.
                                return value + (list1 ? v : v * m(p));
                            });
        }
        default:
            break;
    }
    return fail(ErrorCode::Internal, std::format("{} is not a value opcode", opcode.name()), kWhere);
}

template <class T>
Status apply_bad_pixels(const DngOpcode& opcode, Target<T>& t, const core::CancellationToken& cancel,
                        DngOpcodeRecord& record) {
    if (t.frame.channels != 1) {
        return fail(ErrorCode::Internal, "bad-pixel repair needs a one-sample CFA buffer", kWhere);
    }
    try {
        BitMask bad(t.frame.width, t.frame.height);
        std::uint32_t phase = 0;
        if (is(opcode, DngOpcodeId::FixBadPixelsConstant)) {
            const auto& c = std::get<raw::DngFixBadPixelsConstant>(opcode.parameters);
            phase = c.bayer_phase;
            std::uint64_t marked = 0;
            if constexpr (std::is_integral_v<T>) {
                if (c.constant <= 0xFFFF) {
                    for (std::int32_t y = 0; y < t.frame.height; ++y) {
                        const T* line = t.view.row(y);
                        for (std::int32_t x = 0; x < t.frame.width; ++x) {
                            if (line[x] == c.constant) {
                                bad.set(x, y);
                                ++marked;
                            }
                        }
                    }
                }
            } else {
                return fail(ErrorCode::Internal, "FixBadPixelsConstant needs stored integer samples", kWhere);
            }
            record.detail = std::format("constant {}, BayerPhase {}: {} marked pixel(s)", c.constant, c.bayer_phase,
                                        marked);
        } else {
            const auto& list = std::get<raw::DngFixBadPixelsList>(opcode.parameters);
            phase = list.bayer_phase;
            const std::uint64_t outside = mark_listed(bad, t.frame, list);
            record.detail = std::format("BayerPhase {}: {} point(s), {} rectangle(s); {} point(s) outside the image",
                                        list.bayer_phase, list.points.size(), list.rects.size(), outside);
        }
        return repair_bad_pixels(t.view, t.frame, bad, green_even_for(phase), t.parallel, cancel, record,
                                 t.on_repaired);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the bad-pixel mask", kWhere);
    }
}

// Warps `pixels` (stage frame f) into a new buffer.
Status apply_warp(const DngOpcode& opcode, image::ImageBuffer<float>& pixels, const Frame& f,
                  const DngOpcodeOptions& options, const core::CancellationToken& cancel, DngOpcodeRecord& record,
                  DngOpcodeReport& report) {
    const auto& w = std::get<raw::DngWarpRectilinear>(opcode.parameters);
    const Radial c = radial_frame(f, w.center_x, w.center_y);
    record.detail = describe_warp(w, c);
    // "If kr0 = 1 and the remaining terms are zero, then the warp function is the identity":
    // nothing to resample (and no rounding of the mapping).
    const auto identity = [&](const raw::DngWarpPlane& k) {
        return k.radial[0] == 1.0 && std::all_of(k.radial.begin() + 1, k.radial.end(), [](double v) { return v == 0.0; }) &&
               k.tangential[0] == 0.0 && k.tangential[1] == 0.0;
    };
    if (std::all_of(w.planes.begin(), w.planes.end(), identity)) {
        record.detail += "; the identity (no resampling)";
        return ok();
    }
    auto out = image::ImageBuffer<float>::create_uninitialized(f.width, f.height, f.channels, options.limits);
    if (!out) {
        return Unexpected(std::move(out.error()));
    }
    const std::int32_t channels = f.channels;
    const bool shared = w.planes.size() == 1 ||
                        std::all_of(w.planes.begin(), w.planes.end(),
                                    [&](const raw::DngWarpPlane& k) { return k == w.planes.front(); });
    const image::ConstImageView<float> in = pixels.const_view();
    image::ImageView<float> dst = out->view();
    std::vector<std::uint64_t> outside(static_cast<std::size_t>(f.height), 0);
    std::vector<std::uint8_t> invalid(static_cast<std::size_t>(f.height), 0);
    auto status = detail::for_each_row(f.height, options.parallel, cancel, kWhere, [&](std::int32_t y) {
        float* row = dst.row(y);
        const double sy_stage = static_cast<double>(y + f.offset_y);
        for (std::int32_t x = 0; x < f.width; ++x) {
            const double sx_stage = static_cast<double>(x + f.offset_x);
            for (std::int32_t p = 0; p < channels; ++p) {
                if (shared && p > 0) {
                    break;
                }
                const raw::DngWarpPlane& k = w.planes[std::min<std::size_t>(static_cast<std::size_t>(p), w.planes.size() - 1)];
                const image::PointD s = warp_source(k, w.extended, w.reciprocal_radial, c, sx_stage, sy_stage);
                const double bx = s.x - static_cast<double>(f.offset_x);
                const double by = s.y - static_cast<double>(f.offset_y);
                if (!std::isfinite(bx) || !std::isfinite(by) || std::abs(bx) > 1e9 || std::abs(by) > 1e9) {
                    invalid[static_cast<std::size_t>(y)] = 1;
                    for (std::int32_t q = shared ? 0 : p; q < (shared ? channels : p + 1); ++q) {
                        row[static_cast<std::ptrdiff_t>(x) * channels + q] = 0.0f;
                    }
                    continue;
                }
                if (bx < -0.5 || by < -0.5 || bx > f.width - 0.5 || by > f.height - 0.5) {
                    outside[static_cast<std::size_t>(y)] += shared ? static_cast<std::uint64_t>(channels) : 1;
                }
                const Taps tx = taps_for(bx, f.width);
                const Taps ty = taps_for(by, f.height);
                const std::int32_t first = shared ? 0 : p;
                const std::int32_t last = shared ? channels : p + 1;
                for (std::int32_t q = first; q < last; ++q) {
                    double sum = 0.0;
                    for (std::size_t j = 0; j < 4; ++j) {
                        const float* src = in.row(ty.index[j]);
                        double line = 0.0;
                        for (std::size_t i = 0; i < 4; ++i) {
                            line += tx.weight[i] * static_cast<double>(
                                                       src[static_cast<std::ptrdiff_t>(tx.index[i]) * channels + q]);
                        }
                        sum += ty.weight[j] * line;
                    }
                    row[static_cast<std::ptrdiff_t>(x) * channels + q] = static_cast<float>(sum);
                }
            }
        }
    });
    if (!status) {
        return status;
    }
    if (std::any_of(invalid.begin(), invalid.end(), [](std::uint8_t v) { return v != 0; })) {
        return fail(ErrorCode::Corrupt,
                    std::format("OpcodeList{}[{}] {}: the warp maps some pixels to an undefined position (its radial "
                                "term divides by zero or overflows)",
                                list_number(opcode.list), opcode.index, opcode.name()),
                    kWhere);
    }
    for (const std::uint64_t n : outside) {
        record.outside_samples += n;
    }
    record.affected_samples = static_cast<std::uint64_t>(f.width) * static_cast<std::uint64_t>(f.height) *
                              static_cast<std::uint64_t>(channels);
    GeometryChange change;
    change.kind = GeometryChangeKind::Warp;
    change.list = opcode.list;
    change.index = opcode.index;
    change.input_size = pixels.size();
    change.output_size = out->size();
    change.warp = w;
    change.stage_size = image::Size{static_cast<std::int32_t>(f.stage_width), static_cast<std::int32_t>(f.stage_height)};
    change.offset_x = static_cast<std::int32_t>(f.offset_x);
    change.offset_y = static_cast<std::int32_t>(f.offset_y);
    change.center_x = c.cx;
    change.center_y = c.cy;
    change.max_distance = c.m;
    report.geometry.push_back(std::move(change));
    pixels = std::move(*out);
    return ok();
}

// The record skeleton of an opcode.
DngOpcodeRecord record_for(const DngOpcode& opcode) {
    DngOpcodeRecord r;
    r.list = opcode.list;
    r.index = opcode.index;
    r.id = opcode.id;
    r.name = std::string(opcode.name());
    r.mandatory = opcode.is_mandatory();
    return r;
}

const raw::RequiredOperation* operation_for(const raw::RawMetadata& m, const DngOpcode& opcode) {
    const raw::OperationStage stage = raw::operation_stage_of(opcode.list);
    for (const raw::RequiredOperation& op : m.required_operations) {
        if (op.kind == raw::RequiredOperationKind::DngOpcode && op.stage == stage && op.index == opcode.index &&
            op.opcode_id == opcode.id) {
            return &op;
        }
    }
    return nullptr;
}

// Decides the outcome of an opcode that is not applied; std::nullopt when it must be applied.
std::optional<std::pair<OpcodeOutcome, std::string>> skip_outcome(const raw::RawMetadata& m, const DngOpcode& opcode,
                                                                  bool enabled) {
    const raw::RequiredOperation* op = operation_for(m, opcode);
    if (op == nullptr) {
        return std::pair{opcode.is_optional() ? OpcodeOutcome::SkippedOptional : OpcodeOutcome::NotApplied,
                         std::string("the metadata lists no required operation for it")};
    }
    if (op->applied_by_decoder) {
        return std::pair{OpcodeOutcome::AppliedByDecoder, op->lucent_note};
    }
    if (op->superseded_by_warp_rectilinear2) {
        return std::pair{OpcodeOutcome::SkippedByWarpRule, op->lucent_note};
    }
    if (!op->supported_by_lucent) {
        return std::pair{op->mandatory ? OpcodeOutcome::NotApplied : OpcodeOutcome::SkippedOptional, op->lucent_note};
    }
    if (!enabled) {
        return std::pair{OpcodeOutcome::Disabled, std::string("opcode processing is switched off by the settings")};
    }
    return std::nullopt;
}

std::vector<const DngOpcode*> opcodes_of(const raw::RawMetadata& m, DngOpcodeListId list) {
    std::vector<const DngOpcode*> out;
    if (m.dng) {
        for (const DngOpcode& opcode : m.dng->raw_ifd.opcodes) {
            if (opcode.list == list) {
                out.push_back(&opcode);
            }
        }
        std::stable_sort(out.begin(), out.end(), [](const DngOpcode* a, const DngOpcode* b) { return a->index < b->index; });
    }
    return out;
}

}  // namespace

std::string_view to_string(OpcodeOutcome outcome) noexcept {
    switch (outcome) {
        case OpcodeOutcome::Applied:
            return "applied";
        case OpcodeOutcome::SkippedOptional:
            return "skipped_optional";
        case OpcodeOutcome::NotApplied:
            return "not_applied";
        case OpcodeOutcome::SkippedByWarpRule:
            return "skipped_by_warp_rule";
        case OpcodeOutcome::AppliedByDecoder:
            return "applied_by_decoder";
        case OpcodeOutcome::Disabled:
            return "disabled";
    }
    return "invalid";
}

image::PointD GeometryChange::source_of(image::PointD output, std::uint32_t plane) const noexcept {
    if (kind == GeometryChangeKind::Trim) {
        return {output.x + crop.x, output.y + crop.y};
    }
    if (warp.planes.empty()) {
        return output;
    }
    const raw::DngWarpPlane& k = warp.planes[std::min<std::size_t>(plane, warp.planes.size() - 1)];
    const Radial c{center_x, center_y, max_distance};
    const image::PointD s =
        warp_source(k, warp.extended, warp.reciprocal_radial, c, output.x - 0.5 + offset_x, output.y - 0.5 + offset_y);
    return {s.x - offset_x + 0.5, s.y - offset_y + 0.5};
}

bool DngOpcodeReport::any_applied() const noexcept {
    return std::any_of(opcodes.begin(), opcodes.end(),
                       [](const DngOpcodeRecord& r) { return r.outcome == OpcodeOutcome::Applied; });
}

CarriedLensCorrections carried_lens_corrections(const raw::RawMetadata& metadata) {
    CarriedLensCorrections out;
    if (!metadata.dng) {
        return out;
    }
    const raw::DngOpcodeFrame frame3 = raw::dng_opcode_frame(metadata, DngOpcodeListId::List3);
    for (const DngOpcode& opcode : metadata.dng->raw_ifd.opcodes) {
        const raw::RequiredOperation* op = operation_for(metadata, opcode);
        if (op == nullptr || !(op->supported_by_lucent || op->applied_by_decoder)) {
            continue;
        }
        const std::string source = std::format("OpcodeList{}[{}] {}", list_number(opcode.list), opcode.index,
                                               opcode.name());
        bool counted = false;
        if (const auto* w = std::get_if<raw::DngWarpRectilinear>(&opcode.parameters)) {
            out.distortion = true;
            counted = true;
            if (std::any_of(w->planes.begin(), w->planes.end(),
                            [&](const raw::DngWarpPlane& k) { return !(k == w->planes.front()); })) {
                out.lateral_chromatic_aberration = true;
            }
        } else if (is(opcode, DngOpcodeId::FixVignetteRadial)) {
            out.vignetting = true;
            counted = true;
        } else if (const auto* g = std::get_if<raw::DngGainMap>(&opcode.parameters);
                   g != nullptr && opcode.list != DngOpcodeListId::List1) {
            const raw::DngOpcodeRect& r = g->area.rect;
            if (r.empty() ||  // the whole image
                (r.top == 0 && r.left == 0 && r.bottom >= frame3.image.height && r.right >= frame3.image.width)) {
                out.vignetting = true;
                counted = true;
            }
        }
        if (counted) {
            out.sources.push_back(source);
        }
    }
    return out;
}

DngOpcodeProcessor::DngOpcodeProcessor(const raw::RawMetadata& metadata, DngOpcodeOptions options)
    : metadata_(&metadata), options_(std::move(options)), list3_frame_(raw::dng_opcode_frame(metadata, DngOpcodeListId::List3)) {}

bool DngOpcodeProcessor::has_list(DngOpcodeListId list) const noexcept {
    if (!metadata_->dng) {
        return false;
    }
    return std::any_of(metadata_->dng->raw_ifd.opcodes.begin(), metadata_->dng->raw_ifd.opcodes.end(),
                       [&](const DngOpcode& opcode) { return opcode.list == list; });
}

std::vector<DngOpcodeRecord> DngOpcodeProcessor::records_of(DngOpcodeListId list) const {
    std::vector<DngOpcodeRecord> out;
    for (const DngOpcodeRecord& r : report_.opcodes) {
        if (r.list == list) {
            out.push_back(r);
        }
    }
    return out;
}

Status DngOpcodeProcessor::run_list1(raw::RawImage& image, const core::CancellationToken& cancel) {
    const std::vector<const DngOpcode*> opcodes = opcodes_of(*metadata_, DngOpcodeListId::List1);
    if (opcodes.empty()) {
        return ok();
    }
    const raw::DngOpcodeFrame stage = raw::dng_opcode_frame(*metadata_, DngOpcodeListId::List1);
    raw::RawPixels& pixels = std::visit([](auto& frame) -> raw::RawPixels& { return frame.pixels; }, image);
    auto* buffer = std::get_if<image::ImageBuffer<std::uint16_t>>(&pixels);
    for (const DngOpcode* opcode : opcodes) {
        DngOpcodeRecord record = record_for(*opcode);
        const auto start = Clock::now();
        if (auto skip = skip_outcome(*metadata_, *opcode, options_.enabled)) {
            record.outcome = skip->first;
            record.detail = std::move(skip->second);
            report_.opcodes.push_back(std::move(record));
            continue;
        }
        if (buffer == nullptr || buffer->empty() || buffer->width() != static_cast<std::int32_t>(stage.image.width) ||
            buffer->height() != static_cast<std::int32_t>(stage.image.height) ||
            buffer->channels() != static_cast<std::int32_t>(stage.image.planes)) {
            return fail(ErrorCode::Internal,
                        std::format("OpcodeList1[{}] {} is marked supported but the decoded frame is not the {} x {} "
                                    "16-bit raw image it processes",
                                    opcode->index, opcode->name(), stage.image.width, stage.image.height),
                        kWhere);
        }
        Target<std::uint16_t> t;
        t.view = buffer->view();
        t.frame = Frame{stage.image.width, stage.image.height, 0, 0, buffer->width(), buffer->height(), buffer->channels()};
        t.list = DngOpcodeListId::List1;
        t.parallel = options_.parallel;
        Status status = ok();
        if (is(*opcode, DngOpcodeId::FixBadPixelsConstant) || is(*opcode, DngOpcodeId::FixBadPixelsList)) {
            status = apply_bad_pixels(*opcode, t, cancel, record);
        } else {
            status = apply_value_opcode(*opcode, t, cancel, record);
        }
        if (!status) {
            return status;
        }
        record.outcome = OpcodeOutcome::Applied;
        record.seconds = seconds_since(start);
        report_.opcodes.push_back(std::move(record));
    }
    return ok();
}

Status DngOpcodeProcessor::run_list2(SensorImage& sensor, const core::CancellationToken& cancel) {
    const std::vector<const DngOpcode*> opcodes = opcodes_of(*metadata_, DngOpcodeListId::List2);
    if (opcodes.empty()) {
        return ok();
    }
    const raw::DngOpcodeFrame stage = raw::dng_opcode_frame(*metadata_, DngOpcodeListId::List2);
    Frame f{stage.image.width, stage.image.height, stage.offset_x, stage.offset_y, sensor.pixels.width(),
            sensor.pixels.height(), sensor.pixels.channels()};
    bool applied = false;
    for (const DngOpcode* opcode : opcodes) {
        DngOpcodeRecord record = record_for(*opcode);
        const auto start = Clock::now();
        if (auto skip = skip_outcome(*metadata_, *opcode, options_.enabled)) {
            record.outcome = skip->first;
            record.detail = std::move(skip->second);
            report_.opcodes.push_back(std::move(record));
            continue;
        }
        if (sensor.pixels.empty() || sensor.white_balance ||
            (!applied && sensor.pixels.size() != metadata_->geometry.visible_area.size())) {
            return fail(ErrorCode::Internal,
                        std::format("OpcodeList2[{}] {} needs the normalised, not yet white-balanced visible area",
                                    opcode->index, opcode->name()),
                        kWhere);
        }
        Status status = ok();
        if (is(*opcode, DngOpcodeId::TrimBounds)) {
            const auto& trim = std::get<raw::DngTrimBounds>(opcode->parameters);
            if (trim.bounds.bottom > f.stage_height || trim.bounds.right > f.stage_width) {
                return fail(ErrorCode::Corrupt,
                            std::format("OpcodeList2[{}] TrimBounds exceeds the {} x {} image", opcode->index,
                                        f.stage_width, f.stage_height),
                            kWhere);
            }
            const image::PixelRect keep = trim_rect(f, trim.bounds);
            if (keep.empty()) {
                return fail(ErrorCode::Corrupt,
                            std::format("OpcodeList2[{}] TrimBounds keeps no pixel of the visible area", opcode->index),
                            kWhere);
            }
            auto pixels = crop_buffer(sensor.pixels, keep, options_.limits);
            if (!pixels) {
                return Unexpected(std::move(pixels.error()));
            }
            auto mask = SaturationMask::create(keep.width, keep.height, options_.limits);
            if (!mask) {
                return Unexpected(std::move(mask.error()));
            }
            for (std::int32_t y = 0; y < keep.height; ++y) {
                std::memcpy(mask->row(y), sensor.saturation.row(keep.y + y) + keep.x, static_cast<std::size_t>(keep.width));
            }
            GeometryChange change;
            change.kind = GeometryChangeKind::Trim;
            change.list = opcode->list;
            change.index = opcode->index;
            change.input_size = sensor.pixels.size();
            change.output_size = keep.size();
            change.crop = keep;
            report_.geometry.push_back(change);
            record.detail = std::format("bounds rows {}..{}, columns {}..{}: the buffer becomes {} x {} at ({}, {})",
                                        trim.bounds.top, trim.bounds.bottom, trim.bounds.left, trim.bounds.right,
                                        keep.width, keep.height, keep.x, keep.y);
            record.affected_samples = static_cast<std::uint64_t>(keep.size().area()) *
                                      static_cast<std::uint64_t>(sensor.pixels.channels());
            sensor.pixels = std::move(*pixels);
            sensor.saturation = std::move(*mask);
            if (!sensor.cfa.empty()) {
                sensor.cfa = sensor.cfa.shifted(keep.x, keep.y);
            }
            sensor.normalization.source_area = image::PixelRect{sensor.normalization.source_area.x + keep.x,
                                                                sensor.normalization.source_area.y + keep.y,
                                                                keep.width, keep.height};
            f.offset_x = f.offset_x + keep.x - trim.bounds.left;
            f.offset_y = f.offset_y + keep.y - trim.bounds.top;
            f.stage_width = trim.bounds.right - trim.bounds.left;
            f.stage_height = trim.bounds.bottom - trim.bounds.top;
            f.width = keep.width;
            f.height = keep.height;
        } else {
            Target<float> t;
            t.view = sensor.pixels.view();
            t.frame = f;
            t.list = DngOpcodeListId::List2;
            t.parallel = options_.parallel;
            if (is(*opcode, DngOpcodeId::FixBadPixelsList)) {
                // A repair discards the stored value, and with it the saturation normalisation
                // flagged from it: a repaired pixel's flag is derived again from its new value,
                // so the resaturation below never undoes a repair.
                if (!sensor.saturation.empty()) {
                    t.on_repaired = [&sensor](std::int32_t x, std::int32_t y, double value) {
                        const std::int32_t c = sensor.channel_of_site(x, y);
                        const bool saturated =
                            c >= 0 && c <= 3 &&
                            value >= static_cast<double>(
                                         sensor.normalization.saturation_level[static_cast<std::size_t>(c)]);
                        sensor.saturation.row(y)[x] = saturated ? static_cast<std::uint8_t>(1u << c) : std::uint8_t{0};
                    };
                }
                status = apply_bad_pixels(*opcode, t, cancel, record);
            } else {
                status = apply_value_opcode(*opcode, t, cancel, record);
            }
        }
        if (!status) {
            return status;
        }
        applied = true;
        record.outcome = OpcodeOutcome::Applied;
        record.seconds = seconds_since(start);
        report_.opcodes.push_back(std::move(record));
    }
    // List 3 sees the (possibly trimmed) image.
    list3_frame_.image.width = static_cast<std::uint32_t>(f.stage_width);
    list3_frame_.image.height = static_cast<std::uint32_t>(f.stage_height);
    list3_frame_.offset_x = static_cast<std::int32_t>(f.offset_x);
    list3_frame_.offset_y = static_cast<std::int32_t>(f.offset_y);

    // Saturated samples stay at or above their saturation level.
    if (applied && !sensor.saturation.empty()) {
        const std::int32_t channels = sensor.pixels.channels();
        std::vector<std::uint64_t> raised(static_cast<std::size_t>(sensor.pixels.height()), 0);
        auto status = detail::for_each_row(sensor.pixels.height(), options_.parallel, cancel, kWhere, [&](std::int32_t y) {
            float* line = sensor.pixels.row(y);
            const std::uint8_t* flags = sensor.saturation.row(y);
            for (std::int32_t x = 0; x < sensor.pixels.width(); ++x) {
                if (flags[x] == 0) {
                    continue;
                }
                for (std::int32_t s = 0; s < channels; ++s) {
                    const std::int32_t c = channels == 1 ? (sensor.representation == raw::RawRepresentation::Mosaic
                                                                ? sensor.channel_of_site(x, y)
                                                                : 0)
                                                         : s;
                    if (c < 0 || c > 3 || (flags[x] & (1u << c)) == 0) {
                        continue;
                    }
                    const float level = sensor.normalization.saturation_level[static_cast<std::size_t>(c)];
                    float& v = line[static_cast<std::ptrdiff_t>(x) * channels + s];
                    if (v < level) {
                        v = level;
                        ++raised[static_cast<std::size_t>(y)];
                    }
                }
            }
        });
        if (!status) {
            return status;
        }
        for (const std::uint64_t n : raised) {
            report_.list2_resaturated_samples += n;
        }
    }
    return ok();
}

Status DngOpcodeProcessor::run_list3(image::ImageBuffer<float>& pixels, const std::array<double, 3>& plane_multipliers,
                                     const core::CancellationToken& cancel) {
    const std::vector<const DngOpcode*> opcodes = opcodes_of(*metadata_, DngOpcodeListId::List3);
    if (opcodes.empty()) {
        return ok();
    }
    Frame f{list3_frame_.image.width, list3_frame_.image.height, list3_frame_.offset_x, list3_frame_.offset_y,
            pixels.width(), pixels.height(), pixels.channels()};
    for (const DngOpcode* opcode : opcodes) {
        DngOpcodeRecord record = record_for(*opcode);
        const auto start = Clock::now();
        if (auto skip = skip_outcome(*metadata_, *opcode, options_.enabled)) {
            record.outcome = skip->first;
            record.detail = std::move(skip->second);
            report_.opcodes.push_back(std::move(record));
            continue;
        }
        if (pixels.empty() || static_cast<std::uint32_t>(f.channels) != list3_frame_.image.planes ||
            f.offset_x < 0 || f.offset_y < 0 || f.offset_x + f.width > f.stage_width ||
            f.offset_y + f.height > f.stage_height) {
            return fail(ErrorCode::Internal,
                        std::format("OpcodeList3[{}] {} needs the demosaiced {}-plane image of the visible area "
                                    "(got {} x {} x {})",
                                    opcode->index, opcode->name(), list3_frame_.image.planes, f.width, f.height,
                                    f.channels),
                        kWhere);
        }
        Status status = ok();
        if (is(*opcode, DngOpcodeId::WarpRectilinear) || is(*opcode, DngOpcodeId::WarpRectilinear2)) {
            status = apply_warp(*opcode, pixels, f, options_, cancel, record, report_);
        } else if (is(*opcode, DngOpcodeId::TrimBounds)) {
            const auto& trim = std::get<raw::DngTrimBounds>(opcode->parameters);
            if (trim.bounds.bottom > f.stage_height || trim.bounds.right > f.stage_width) {
                return fail(ErrorCode::Corrupt,
                            std::format("OpcodeList3[{}] TrimBounds exceeds the {} x {} image", opcode->index,
                                        f.stage_width, f.stage_height),
                            kWhere);
            }
            const image::PixelRect keep = trim_rect(f, trim.bounds);
            if (keep.empty()) {
                return fail(ErrorCode::Corrupt,
                            std::format("OpcodeList3[{}] TrimBounds keeps no pixel of the image", opcode->index), kWhere);
            }
            auto cropped = crop_buffer(pixels, keep, options_.limits);
            if (!cropped) {
                return Unexpected(std::move(cropped.error()));
            }
            GeometryChange change;
            change.kind = GeometryChangeKind::Trim;
            change.list = opcode->list;
            change.index = opcode->index;
            change.input_size = pixels.size();
            change.output_size = keep.size();
            change.crop = keep;
            report_.geometry.push_back(change);
            record.detail = std::format("bounds rows {}..{}, columns {}..{}: the image becomes {} x {} at ({}, {})",
                                        trim.bounds.top, trim.bounds.bottom, trim.bounds.left, trim.bounds.right,
                                        keep.width, keep.height, keep.x, keep.y);
            record.affected_samples = static_cast<std::uint64_t>(keep.size().area()) *
                                      static_cast<std::uint64_t>(pixels.channels());
            pixels = std::move(*cropped);
            f.offset_x = f.offset_x + keep.x - trim.bounds.left;
            f.offset_y = f.offset_y + keep.y - trim.bounds.top;
            f.stage_width = trim.bounds.right - trim.bounds.left;
            f.stage_height = trim.bounds.bottom - trim.bounds.top;
            f.width = keep.width;
            f.height = keep.height;
        } else {
            Target<float> t;
            t.view = pixels.view();
            t.frame = f;
            t.list = DngOpcodeListId::List3;
            t.parallel = options_.parallel;
            for (std::size_t p = 0; p < 3; ++p) {
                t.multipliers[p] = plane_multipliers[p] > 0.0 ? plane_multipliers[p] : 1.0;
            }
            status = apply_value_opcode(*opcode, t, cancel, record);
        }
        if (!status) {
            return status;
        }
        record.outcome = OpcodeOutcome::Applied;
        record.seconds = seconds_since(start);
        report_.opcodes.push_back(std::move(record));
    }
    return ok();
}

}  // namespace lucent::engine
