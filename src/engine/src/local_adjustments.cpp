// Masks and local adjustments (local_adjustments.hpp).

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <numbers>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include <lucent/engine/guided_filter.hpp>
#include <lucent/engine/local_adjustments.hpp>
#include <lucent/engine/white_balance_model.hpp>
#include <lucent/image/color_math.hpp>

#include "octave_lut.hpp"
#include "parallel_rows.hpp"
#include "scratch_pool.hpp"

namespace lucent::engine {

namespace {

constexpr const char* kWhere = "engine.develop.masks";

constexpr double kLr = kRec2020Luminance[0];
constexpr double kLg = kRec2020Luminance[1];
constexpr double kLb = kRec2020Luminance[2];

// The per-pixel pass evaluates its exponentials, logarithms and tanh in float (the result is
// float; their relative error is about 1e-7).
inline float fexp2(double x) noexcept { return std::exp2(static_cast<float>(x)); }
inline float flog2(double x) noexcept { return std::log2(static_cast<float>(x)); }
inline float ftanh(double x) noexcept { return std::tanh(static_cast<float>(x)); }

// Rows per bin of the brush rasteriser's segment index.
constexpr std::int32_t kBinRows = 16;

double long_edge(image::Size size) noexcept { return static_cast<double>(std::max(size.width, size.height)); }

// 1 - smoothstep(lo, hi, x): 1 up to lo, 0 from hi.
double falloff(double lo, double hi, double x) noexcept {
    if (x <= lo) {
        return 1.0;
    }
    if (x >= hi) {
        return 0.0;
    }
    const double u = (x - lo) / (hi - lo);
    return 1.0 - u * u * (3.0 - 2.0 * u);
}

// A transition band (lo, hi) widened about its middle to at least `minimum`.
void widen(double& lo, double& hi, double minimum) noexcept {
    if (hi - lo < minimum) {
        const double mid = 0.5 * (lo + hi);
        lo = mid - 0.5 * minimum;
        hi = mid + 0.5 * minimum;
    }
}

image::PixelRect clip_to(image::Size size, double x0, double y0, double x1, double y1) noexcept {
    const auto clampi = [](double v, std::int32_t hi) {
        if (!(v > 0.0)) {
            return 0;
        }
        return v >= hi ? hi : static_cast<std::int32_t>(v);
    };
    const std::int32_t left = clampi(std::floor(x0), size.width);
    const std::int32_t top = clampi(std::floor(y0), size.height);
    const std::int32_t right = clampi(std::ceil(x1), size.width);
    const std::int32_t bottom = clampi(std::ceil(y1), size.height);
    if (right <= left || bottom <= top) {
        return image::PixelRect{};
    }
    return image::PixelRect{left, top, right - left, bottom - top};
}

image::PixelRect unite(const image::PixelRect& a, const image::PixelRect& b) noexcept {
    if (a.empty()) {
        return b;
    }
    if (b.empty()) {
        return a;
    }
    const std::int32_t left = std::min(a.x, b.x);
    const std::int32_t top = std::min(a.y, b.y);
    const auto right = static_cast<std::int32_t>(std::max(a.right(), b.right()));
    const auto bottom = static_cast<std::int32_t>(std::max(a.bottom(), b.bottom()));
    return image::PixelRect{left, top, right - left, bottom - top};
}

image::PixelRect expand(const image::PixelRect& r, std::int32_t by, image::Size size) noexcept {
    if (r.empty()) {
        return r;
    }
    const std::int32_t left = std::max(0, r.x - by);
    const std::int32_t top = std::max(0, r.y - by);
    const auto right = static_cast<std::int32_t>(std::min<std::int64_t>(size.width, r.right() + by));
    const auto bottom = static_cast<std::int32_t>(std::min<std::int64_t>(size.height, r.bottom() + by));
    return image::PixelRect{left, top, right - left, bottom - top};
}

// ---- Analytic shapes --------------------------------------------------------------------------------

struct LinearShape {
    double sx = 0.0;
    double sy = 0.0;
    double ux = 0.0;  // (e - s) / |e - s|^2
    double uy = 0.0;
    double t0 = 0.0;
    double t1 = 1.0;

    [[nodiscard]] double coverage(double px, double py) const noexcept {
        return falloff(t0, t1, (px - sx) * ux + (py - sy) * uy);
    }
};

struct RadialShape {
    double cx = 0.0;
    double cy = 0.0;
    double e1x = 1.0;  // the first semi-axis' direction / a
    double e1y = 0.0;
    double e2x = 0.0;  // the second's / b
    double e2y = 1.0;
    double r0 = 0.0;
    double r1 = 1.0;

    [[nodiscard]] double coverage(double px, double py) const noexcept {
        const double dx = px - cx;
        const double dy = py - cy;
        const double u = dx * e1x + dy * e1y;
        const double v = dx * e2x + dy * e2y;
        const double rho2 = u * u + v * v;
        if (rho2 >= r1 * r1) {
            return 0.0;
        }
        return falloff(r0, r1, std::sqrt(rho2));
    }
};

LinearShape prepare_linear(const LinearGradientMask& m, image::Size size) noexcept {
    LinearShape s;
    s.sx = m.start.x * size.width;
    s.sy = m.start.y * size.height;
    const double vx = m.end.x * size.width - s.sx;
    const double vy = m.end.y * size.height - s.sy;
    const double len2 = vx * vx + vy * vy;
    const double len = std::sqrt(len2);
    s.ux = vx / len2;
    s.uy = vy / len2;
    const double half = std::max(0.5 * m.feather, 0.5 / len);
    s.t0 = 0.5 - half;
    s.t1 = 0.5 + half;
    return s;
}

// The pixels a linear gradient covers: the image rectangle clipped to the half-plane t < t1
// (Sutherland-Hodgman with one edge), its bounding box widened by a pixel.
image::PixelRect linear_bounds(const LinearShape& s, image::Size size) noexcept {
    const double w = size.width;
    const double h = size.height;
    const std::array<image::PointD, 4> corners = {{{0.0, 0.0}, {w, 0.0}, {w, h}, {0.0, h}}};
    const auto beyond = [&](const image::PointD& p) { return (p.x - s.sx) * s.ux + (p.y - s.sy) * s.uy - s.t1; };
    double x0 = std::numeric_limits<double>::infinity();
    double y0 = x0;
    double x1 = -x0;
    double y1 = -x0;
    const auto add = [&](const image::PointD& p) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    };
    for (std::size_t i = 0; i < 4; ++i) {
        const image::PointD& p = corners[i];
        const image::PointD& q = corners[(i + 1) % 4];
        const double bp = beyond(p);
        const double bq = beyond(q);
        if (bp <= 0.0) {
            add(p);
        }
        if ((bp < 0.0 && bq > 0.0) || (bp > 0.0 && bq < 0.0)) {
            const double a = bp / (bp - bq);
            add(image::PointD{p.x + a * (q.x - p.x), p.y + a * (q.y - p.y)});
        }
    }
    if (!(x1 >= x0)) {
        return image::PixelRect{};
    }
    return clip_to(size, x0 - 1.0, y0 - 1.0, x1 + 1.0, y1 + 1.0);
}

struct RadialPrepared {
    RadialShape shape;
    image::PixelRect bounds;
};

RadialPrepared prepare_radial(const RadialGradientMask& m, image::Size size) noexcept {
    RadialPrepared p;
    RadialShape& s = p.shape;
    const double edge = long_edge(size);
    const double a = m.radius_x * edge;
    const double b = m.radius_y * edge;
    const double theta = m.angle_degrees * std::numbers::pi / 180.0;
    const double c = std::cos(theta);
    const double n = std::sin(theta);
    s.cx = m.center.x * size.width;
    s.cy = m.center.y * size.height;
    // Counter-clockwise as displayed (y grows downwards): the first axis points to (cos, -sin).
    s.e1x = c / a;
    s.e1y = -n / a;
    s.e2x = n / b;
    s.e2y = c / b;
    s.r0 = 1.0 - m.feather;
    s.r1 = 1.0;
    widen(s.r0, s.r1, 1.0 / std::min(a, b));
    const double ex = std::hypot(a * c, b * n) * s.r1 + 1.0;
    const double ey = std::hypot(a * n, b * c) * s.r1 + 1.0;
    p.bounds = clip_to(size, s.cx - ex - 0.5, s.cy - ey - 0.5, s.cx + ex + 0.5, s.cy + ey + 0.5);
    return p;
}

// ---- Brush --------------------------------------------------------------------------------------------

struct Segment {
    double x0, y0, x1, y1;
};

// Squared distance from (px, py) to the segment.
double distance2(const Segment& s, double px, double py) noexcept {
    const double vx = s.x1 - s.x0;
    const double vy = s.y1 - s.y0;
    const double wx = px - s.x0;
    const double wy = py - s.y0;
    const double vv = vx * vx + vy * vy;
    double t = vv > 0.0 ? (wx * vx + wy * vy) / vv : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const double dx = wx - t * vx;
    const double dy = wy - t * vy;
    return dx * dx + dy * dy;
}

struct StrokeGeometry {
    std::vector<Segment> segments;
    double inner = 0.0;  // full alpha up to this distance
    double outer = 1.0;  // no alpha from this distance
    image::PixelRect box;
};

Expected<StrokeGeometry> stroke_geometry(const BrushStroke& stroke, image::Size size) {
    try {
        StrokeGeometry g;
        const double radius = stroke.radius * long_edge(size);
        g.inner = stroke.hardness * radius;
        g.outer = radius;
        widen(g.inner, g.outer, 1.0);
        double x_min = std::numeric_limits<double>::infinity();
        double y_min = x_min;
        double x_max = -x_min;
        double y_max = -x_min;
        std::vector<image::PointD> points;
        points.reserve(stroke.points.size());
        for (const MaskPoint& p : stroke.points) {
            const image::PointD q{p.x * size.width, p.y * size.height};
            points.push_back(q);
            x_min = std::min(x_min, q.x);
            y_min = std::min(y_min, q.y);
            x_max = std::max(x_max, q.x);
            y_max = std::max(y_max, q.y);
        }
        if (points.size() == 1) {
            g.segments.push_back(Segment{points[0].x, points[0].y, points[0].x, points[0].y});
        }
        for (std::size_t i = 1; i < points.size(); ++i) {
            g.segments.push_back(Segment{points[i - 1].x, points[i - 1].y, points[i].x, points[i].y});
        }
        const double m = g.outer + 1.0;
        g.box = clip_to(size, x_min - m, y_min - m, x_max + m, y_max + m);
        return g;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the brush stroke's segments", kWhere);
    }
}

// Composes one stroke into `coverage` (the raster over `bounds`).
Status paint_stroke(const BrushStroke& stroke, const StrokeGeometry& g, const image::PixelRect& bounds,
                    image::ImageView<float> coverage, const core::CancellationToken& cancel, bool parallel) {
    const image::PixelRect box = g.box.intersect(bounds);
    if (box.empty() || g.segments.empty()) {
        return ok();
    }
    // Segment index per bin of kBinRows rows of the box: a segment is listed in every bin its
    // rows +- outer touch.
    const std::int32_t bins = (box.height + kBinRows - 1) / kBinRows;
    std::vector<std::vector<std::uint32_t>> index;
    try {
        index.resize(static_cast<std::size_t>(bins));
        for (std::size_t k = 0; k < g.segments.size(); ++k) {
            const Segment& s = g.segments[k];
            const double lo = std::min(s.y0, s.y1) - g.outer - 0.5 - box.y;
            const double hi = std::max(s.y0, s.y1) + g.outer - 0.5 - box.y;
            const auto first = static_cast<std::int32_t>(std::clamp(std::floor(lo / kBinRows), 0.0, bins - 1.0));
            const auto last = static_cast<std::int32_t>(std::clamp(std::floor(hi / kBinRows), 0.0, bins - 1.0));
            if (hi < 0.0 || lo > box.height) {
                continue;
            }
            for (std::int32_t b = first; b <= last; ++b) {
                index[static_cast<std::size_t>(b)].push_back(static_cast<std::uint32_t>(k));
            }
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the brush segment index", kWhere);
    }
    const double outer2 = g.outer * g.outer;
    const double flow = stroke.flow;
    const bool erase = stroke.mode == BrushMode::Erase;
    detail::ScratchPool pool(static_cast<std::size_t>(box.width));
    std::atomic<bool> no_memory{false};
    auto status = detail::for_each_row(box.height, parallel, cancel, kWhere, [&](std::int32_t row) {
        float* d2 = pool.acquire();
        if (d2 == nullptr) {
            no_memory.store(true, std::memory_order_relaxed);
            return;
        }
        std::fill(d2, d2 + box.width, std::numeric_limits<float>::infinity());
        const std::int32_t y = box.y + row;
        const double py = y + 0.5;
        bool touched = false;
        for (const std::uint32_t k : index[static_cast<std::size_t>(row / kBinRows)]) {
            const Segment& s = g.segments[k];
            if (py < std::min(s.y0, s.y1) - g.outer || py > std::max(s.y0, s.y1) + g.outer) {
                continue;
            }
            const double lo = std::min(s.x0, s.x1) - g.outer - 0.5;
            const double hi = std::max(s.x0, s.x1) + g.outer - 0.5;
            const auto x0 = std::max(box.x, static_cast<std::int32_t>(std::max(std::ceil(lo), -1.0)));
            const auto x1 = static_cast<std::int32_t>(std::min(std::floor(hi), static_cast<double>(box.right() - 1)));
            for (std::int32_t x = x0; x <= x1; ++x) {
                const auto dd = static_cast<float>(distance2(s, x + 0.5, py));
                float& slot = d2[x - box.x];
                if (dd < slot) {
                    slot = dd;
                    touched = true;
                }
            }
        }
        if (touched) {
            float* m = coverage.row(y - bounds.y) + (box.x - bounds.x);
            for (std::int32_t i = 0; i < box.width; ++i) {
                if (!(d2[i] < outer2)) {
                    continue;
                }
                const double alpha = flow * falloff(g.inner, g.outer, std::sqrt(static_cast<double>(d2[i])));
                const double v = m[i];
                m[i] = static_cast<float>(erase ? v * (1.0 - alpha) : v + alpha * (1.0 - v));
            }
        }
        pool.release(d2);
    });
    if (!status) {
        return status;
    }
    if (no_memory.load()) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the brush row scratch", kWhere);
    }
    return ok();
}

Expected<MaskRaster> rasterize_brush(const BrushMask& brush, image::Size size, const image::AllocationLimits& limits,
                                     const core::CancellationToken& cancel, bool parallel) {
    MaskRaster raster;
    raster.size = size;
    std::vector<StrokeGeometry> strokes;
    try {
        strokes.reserve(brush.strokes.size());
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the brush strokes", kWhere);
    }
    for (const BrushStroke& stroke : brush.strokes) {
        auto g = stroke_geometry(stroke, size);
        if (!g) {
            return Unexpected(std::move(g.error()));
        }
        if (stroke.mode == BrushMode::Add && stroke.flow > 0.0) {
            raster.bounds = unite(raster.bounds, g->box);
        }
        strokes.push_back(std::move(*g));
    }
    if (raster.bounds.empty()) {
        raster.bounds = image::PixelRect{};
        return raster;
    }
    auto plane = image::ImageBuffer<float>::create(raster.bounds.width, raster.bounds.height, 1, limits);
    if (!plane) {
        return fail(plane.error().code, "brush mask raster: " + plane.error().message, kWhere);
    }
    raster.coverage = std::move(*plane);
    for (std::size_t k = 0; k < brush.strokes.size(); ++k) {
        if (auto status =
                paint_stroke(brush.strokes[k], strokes[k], raster.bounds, raster.coverage.view(), cancel, parallel);
            !status) {
            return Unexpected(std::move(status.error()));
        }
    }
    return raster;
}

// ---- Evaluation -------------------------------------------------------------------------------------

// The coverage of one shape at render pixels: analytic (linear, radial) or a raster (brush).
struct Coverage {
    MaskKind kind = MaskKind::RadialGradient;
    LinearShape linear;
    RadialShape radial;
    std::shared_ptr<const MaskRaster> raster;
    image::PixelRect bounds;  // coverage is 0 outside

    [[nodiscard]] double at(std::int32_t x, std::int32_t y) const noexcept {
        if (!bounds.contains(image::Point{x, y})) {
            return 0.0;
        }
        switch (kind) {
            case MaskKind::LinearGradient:
                return linear.coverage(x + 0.5, y + 0.5);
            case MaskKind::RadialGradient:
                return radial.coverage(x + 0.5, y + 0.5);
            case MaskKind::Brush:
                return raster->coverage.at(x - bounds.x, y - bounds.y, 0);
        }
        return 0.0;
    }
};

Status check_size(image::Size size) {
    if (size.empty()) {
        return fail(ErrorCode::InvalidArgument, "masks need a non-empty image size", kWhere);
    }
    return ok();
}

Status check_shape(const MaskShape& shape) {
    LocalAdjustment probe;
    probe.shape = shape;
    DevelopSettings settings;
    settings.local_adjustments.push_back(std::move(probe));
    if (auto status = settings.validate(); !status) {
        return fail(ErrorCode::InvalidArgument, "invalid mask: " + status.error().message, kWhere);
    }
    return ok();
}

struct CoverageSource {
    Coverage coverage;
    bool rasterized = false;
    bool cached = false;
};

Expected<CoverageSource> make_coverage(const MaskShape& shape, image::Size size, MaskRasterCache* cache,
                                       const image::AllocationLimits& limits, const core::CancellationToken& cancel,
                                       bool parallel) {
    CoverageSource out;
    Coverage& c = out.coverage;
    c.kind = kind_of(shape);
    if (const auto* linear = std::get_if<LinearGradientMask>(&shape)) {
        c.linear = prepare_linear(*linear, size);
        c.bounds = linear_bounds(c.linear, size);
        return out;
    }
    if (const auto* radial = std::get_if<RadialGradientMask>(&shape)) {
        const RadialPrepared p = prepare_radial(*radial, size);
        c.radial = p.shape;
        c.bounds = p.bounds;
        return out;
    }
    const auto& brush = std::get<BrushMask>(shape);
    if (cache != nullptr) {
        c.raster = cache->find(shape, size);
    }
    if (c.raster) {
        out.cached = true;
    } else {
        auto raster = rasterize_brush(brush, size, limits, cancel, parallel);
        if (!raster) {
            return Unexpected(std::move(raster.error()));
        }
        try {
            c.raster = std::make_shared<const MaskRaster>(std::move(*raster));
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "cannot keep the brush raster", kWhere);
        }
        out.rasterized = true;
        if (cache != nullptr) {
            cache->insert(shape, c.raster);
        }
    }
    c.bounds = c.raster->bounds;
    return out;
}

}  // namespace

// ---- Rasterisation (public) ---------------------------------------------------------------------------

Expected<MaskRaster> rasterize_mask(const MaskShape& shape, image::Size size, const image::AllocationLimits& limits,
                                    const core::CancellationToken& cancel, bool parallel) {
    if (auto status = check_size(size); !status) {
        return Unexpected(std::move(status.error()));
    }
    if (auto status = check_shape(shape); !status) {
        return Unexpected(std::move(status.error()));
    }
    if (const auto* brush = std::get_if<BrushMask>(&shape)) {
        return rasterize_brush(*brush, size, limits, cancel, parallel);
    }
    auto source = make_coverage(shape, size, nullptr, limits, cancel, parallel);
    if (!source) {
        return Unexpected(std::move(source.error()));
    }
    const Coverage& c = source->coverage;
    MaskRaster raster;
    raster.size = size;
    raster.bounds = c.bounds;
    if (c.bounds.empty()) {
        return raster;
    }
    auto plane = image::ImageBuffer<float>::create_uninitialized(c.bounds.width, c.bounds.height, 1, limits);
    if (!plane) {
        return fail(plane.error().code, "mask raster: " + plane.error().message, kWhere);
    }
    const image::ImageView<float> view = plane->view();
    auto status = detail::for_each_row(c.bounds.height, parallel, cancel, kWhere, [&](std::int32_t row) {
        float* m = view.row(row);
        const std::int32_t y = c.bounds.y + row;
        for (std::int32_t i = 0; i < c.bounds.width; ++i) {
            m[i] = static_cast<float>(c.at(c.bounds.x + i, y));
        }
    });
    if (!status) {
        return Unexpected(std::move(status.error()));
    }
    raster.coverage = std::move(*plane);
    return raster;
}

Expected<image::ImageBuffer<float>> mask_weights(const LocalAdjustment& adjustment, image::Size size,
                                                 const image::AllocationLimits& limits,
                                                 const core::CancellationToken& cancel, bool parallel) {
    if (auto status = check_size(size); !status) {
        return Unexpected(std::move(status.error()));
    }
    if (auto status = check_shape(adjustment.shape); !status) {
        return Unexpected(std::move(status.error()));
    }
    if (!std::isfinite(adjustment.opacity) || adjustment.opacity < 0.0 || adjustment.opacity > 1.0) {
        return fail(ErrorCode::InvalidArgument, "the mask opacity must lie within [0, 1]", kWhere);
    }
    auto plane = image::ImageBuffer<float>::create(size.width, size.height, 1, limits);
    if (!plane) {
        return fail(plane.error().code, "mask weights: " + plane.error().message, kWhere);
    }
    if (!adjustment.enabled) {
        return plane;
    }
    auto source = make_coverage(adjustment.shape, size, nullptr, limits, cancel, parallel);
    if (!source) {
        return Unexpected(std::move(source.error()));
    }
    const Coverage& c = source->coverage;
    const double opacity = adjustment.opacity;
    const bool invert = adjustment.invert;
    const image::ImageView<float> view = plane->view();
    auto status = detail::for_each_row(size.height, parallel, cancel, kWhere, [&](std::int32_t y) {
        float* w = view.row(y);
        for (std::int32_t x = 0; x < size.width; ++x) {
            const double m = c.at(x, y);
            w[x] = static_cast<float>(opacity * (invert ? 1.0 - m : m));
        }
    });
    if (!status) {
        return Unexpected(std::move(status.error()));
    }
    return plane;
}

MaskOutline mask_outline(const MaskShape& shape, image::Size size, std::int32_t segments) {
    MaskOutline outline;
    if (size.empty() || shape.valueless_by_exception()) {
        return outline;
    }
    const double w = size.width;
    const double h = size.height;
    const auto normalised = [&](double x, double y) { return MaskPoint{x / w, y / h}; };
    segments = std::max(segments, 4);
    if (const auto* radial = std::get_if<RadialGradientMask>(&shape)) {
        const double edge = long_edge(size);
        const double theta = radial->angle_degrees * std::numbers::pi / 180.0;
        const double c = std::cos(theta);
        const double n = std::sin(theta);
        const double cx = radial->center.x * w;
        const double cy = radial->center.y * h;
        std::vector<double> scales{1.0};
        if (radial->feather > 0.0 && radial->feather < 1.0) {
            scales.push_back(1.0 - radial->feather);
        }
        for (const double rho : scales) {
            std::vector<MaskPoint> ring;
            ring.reserve(static_cast<std::size_t>(segments) + 1);
            for (std::int32_t k = 0; k <= segments; ++k) {
                const double phi = 2.0 * std::numbers::pi * (k % segments) / segments;
                const double u = rho * radial->radius_x * edge * std::cos(phi);
                const double v = rho * radial->radius_y * edge * std::sin(phi);
                ring.push_back(normalised(cx + u * c + v * n, cy - u * n + v * c));
            }
            outline.polylines.push_back(std::move(ring));
        }
    } else if (const auto* linear = std::get_if<LinearGradientMask>(&shape)) {
        const double sx = linear->start.x * w;
        const double sy = linear->start.y * h;
        const double ex = linear->end.x * w;
        const double ey = linear->end.y * h;
        const double len = std::hypot(ex - sx, ey - sy);
        const double reach = 2.0 * std::hypot(w, h) + len;
        const double nx = len > 0.0 ? -(ey - sy) / len : 0.0;
        const double ny = len > 0.0 ? (ex - sx) / len : 1.0;
        for (const double t : {0.0, 0.5, 1.0}) {
            const double px = sx + t * (ex - sx);
            const double py = sy + t * (ey - sy);
            outline.polylines.push_back(
                {normalised(px - reach * nx, py - reach * ny), normalised(px + reach * nx, py + reach * ny)});
        }
    } else {
        for (const BrushStroke& stroke : std::get<BrushMask>(shape).strokes) {
            outline.polylines.push_back(stroke.points);
        }
    }
    return outline;
}

// ---- Cache ------------------------------------------------------------------------------------------

std::shared_ptr<const MaskRaster> MaskRasterCache::find(const MaskShape& shape, image::Size size) {
    const std::uint64_t revision = mask_shape_revision(shape);
    const std::lock_guard lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->revision == revision && it->raster->size == size && it->shape == shape) {
            entries_.splice(entries_.begin(), entries_, it);
            ++hits_;
            return entries_.front().raster;
        }
    }
    ++misses_;
    return nullptr;
}

void MaskRasterCache::insert(const MaskShape& shape, std::shared_ptr<const MaskRaster> raster) noexcept {
    if (!raster) {
        return;
    }
    const std::uint64_t bytes = raster->byte_size();
    if (bytes > capacity_) {
        return;
    }
    const std::uint64_t revision = mask_shape_revision(shape);
    try {
        Entry entry{revision, shape, raster};
        const std::lock_guard lock(mutex_);
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->revision == revision && it->raster->size == raster->size && it->shape == shape) {
                bytes_ -= it->raster->byte_size();
                entries_.erase(it);
                break;
            }
        }
        while (!entries_.empty() && bytes_ + bytes > capacity_) {
            bytes_ -= entries_.back().raster->byte_size();
            entries_.pop_back();
        }
        entries_.push_front(std::move(entry));
        bytes_ += bytes;
    } catch (const std::bad_alloc&) {
        // Not kept: the cache is an optimisation.
    }
}

void MaskRasterCache::clear() noexcept {
    const std::lock_guard lock(mutex_);
    entries_.clear();
    bytes_ = 0;
}

MaskRasterCache::Stats MaskRasterCache::stats() const {
    const std::lock_guard lock(mutex_);
    return Stats{entries_.size(), bytes_, capacity_, hits_, misses_};
}

// ---- Local controls -----------------------------------------------------------------------------------

Expected<LocalWhiteAxes> local_white_axes() {
    const auto fail_internal = [](const Error& e) {
        return Unexpected(Error{ErrorCode::Internal, "local white axes: " + e.message, kWhere, e.native_code});
    };
    auto reference = temperature_tint_from_xy(image::kD65White);
    if (!reference) {
        return fail_internal(reference.error());
    }
    auto to_rgb = image::xyz_to_rgb(image::kRec2020Primaries, image::kD65White);
    if (!to_rgb) {
        return fail_internal(to_rgb.error());
    }
    // Rec.2020 RGB of a white of luminance 1.
    const auto rgb_of = [&](const TemperatureTint& white) -> Expected<image::Vec3d> {
        auto xy = xy_from_temperature_tint(white);
        if (!xy) {
            return Unexpected(std::move(xy.error()));
        }
        auto xyz = image::xy_to_XYZ(*xy, 1.0);
        if (!xyz) {
            return Unexpected(std::move(xyz.error()));
        }
        return *to_rgb * *xyz;
    };
    auto d65 = image::xy_to_XYZ(image::kD65White, 1.0);
    if (!d65) {
        return fail_internal(d65.error());
    }
    const image::Vec3d target = *to_rgb * *d65;
    const double mired = 1e6 / reference->temperature_k;
    TemperatureTint bluer = *reference;
    bluer.temperature_k = 1e6 / (mired - kLocalTemperatureMired);
    TemperatureTint greener = *reference;
    greener.tint = reference->tint + kLocalTintUnits;
    auto blue = rgb_of(bluer);
    if (!blue) {
        return fail_internal(blue.error());
    }
    auto green = rgb_of(greener);
    if (!green) {
        return fail_internal(green.error());
    }
    LocalWhiteAxes axes;
    for (std::size_t c = 0; c < 3; ++c) {
        if (!((*blue)[c] > 0.0) || !((*green)[c] > 0.0) || !(target[c] > 0.0)) {
            return fail(ErrorCode::Internal, "local white axes: a white outside Rec.2020", kWhere);
        }
        axes.temperature[c] = std::log2(target[c] / (*blue)[c]);
        axes.tint[c] = std::log2(target[c] / (*green)[c]);
    }
    return axes;
}

std::uint64_t local_adjustments_working_bytes(std::span<const LocalAdjustment> adjustments,
                                              image::Size size) noexcept {
    if (size.empty()) {
        return 0;
    }
    const std::uint64_t plane = static_cast<std::uint64_t>(size.area()) * sizeof(float);
    std::uint64_t bytes = 0;
    bool tone = false;
    for (const LocalAdjustment& a : adjustments) {
        if (!a.has_effect()) {
            continue;
        }
        if (a.kind() == MaskKind::Brush) {
            bytes += plane;
        }
        tone = tone || a.controls.highlights != 0.0 || a.controls.shadows != 0.0;
    }
    // The base layer: the log-luminance plane and the guided filter's two-channel working plane.
    return bytes + (tone ? 3 * plane : 0);
}

Expected<LocalAdjustmentsRecord> apply_local_adjustments(ColorImage& image,
                                                         std::span<const LocalAdjustment> adjustments,
                                                         const LocalAdjustmentsParameters& parameters,
                                                         MaskRasterCache* cache, const image::AllocationLimits& limits,
                                                         const core::CancellationToken& cancel, bool parallel) {
    if (image.pixels.empty() || image.pixels.channels() != 3) {
        return fail(ErrorCode::InvalidArgument, "local adjustments need a non-empty 3-channel image", kWhere);
    }
    if (image.encoding != image::ColorEncoding::scene_linear_rec2020_d65()) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("local adjustments work on scene-linear Rec.2020 / D65 (got {})",
                                image.encoding.to_string()),
                    kWhere);
    }
    const LocalAdjustmentsParameters& q = parameters;
    if (!std::isfinite(q.white) || !(q.white > 0.0) || q.tone_radius < 1 || !std::isfinite(q.tone_epsilon) ||
        !(q.tone_epsilon > 0.0)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("invalid local-adjustment parameters (white {}, radius {}, eps {})", q.white,
                                q.tone_radius, q.tone_epsilon),
                    kWhere);
    }
    {
        DevelopSettings check;
        try {
            check.local_adjustments.assign(adjustments.begin(), adjustments.end());
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "cannot copy the local adjustments", kWhere);
        }
        if (auto status = check.validate(); !status) {
            return fail(ErrorCode::InvalidArgument, "invalid local adjustments: " + status.error().message, kWhere);
        }
    }
    LocalAdjustmentsRecord record;
    const image::Size size = image.pixels.size();

    // The adjustments with an effect, their coverage and their controls.
    struct Active {
        Coverage coverage;
        double opacity = 1.0;
        bool invert = false;
        LocalControls controls;
    };
    std::vector<Active> active;
    bool tone = false;
    image::PixelRect tone_support;
    for (const LocalAdjustment& a : adjustments) {
        if (!a.has_effect()) {
            continue;
        }
        auto source = make_coverage(a.shape, size, cache, limits, cancel, parallel);
        if (!source) {
            return Unexpected(std::move(source.error()));
        }
        record.rasterized += source->rasterized ? 1u : 0u;
        record.cache_hits += source->cached ? 1u : 0u;
        if (source->coverage.raster) {
            record.raster_bytes += source->coverage.raster->byte_size();
        }
        if (a.controls.highlights != 0.0 || a.controls.shadows != 0.0) {
            tone = true;
            tone_support =
                unite(tone_support, a.invert ? image::PixelRect::from_size(size) : source->coverage.bounds);
        }
        try {
            active.push_back(Active{std::move(source->coverage), a.opacity, a.invert, a.controls});
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "cannot allocate the local adjustments", kWhere);
        }
    }
    record.masks = static_cast<std::uint32_t>(active.size());
    if (active.empty()) {
        return record;
    }
    if (auto cancelled = cancel.check(kWhere); !cancelled) {
        return Unexpected(std::move(cancelled.error()));
    }

    auto look = NeutralV1Curve::create(q.white);
    if (!look) {
        return Unexpected(std::move(look.error()));
    }
    auto axes = local_white_axes();
    if (!axes) {
        return Unexpected(std::move(axes.error()));
    }
    const double white_ev = std::log2(q.white);
    const double floor_value = q.white * std::exp2(kLocalToneFloorEv);
    const double log2_pivot = std::log2(look->inverse(kLocalContrastPivotDisplay));
    const image::ImageView<float> rgb = image.pixels.view();

    // ---- The highlights/shadows base layer over the tone masks' support plus its halo ----------------
    image::ImageBuffer<float> base_plane;
    image::PixelRect region;
    std::optional<detail::UniformLut> highlight_weight;
    std::optional<detail::UniformLut> shadow_weight;
    if (tone && !tone_support.empty()) {
        region = expand(tone_support, 2 * q.tone_radius, size);
        auto plane = image::ImageBuffer<float>::create_uninitialized(region.width, region.height, 1, limits);
        if (!plane) {
            return fail(plane.error().code, "local tone base layer: " + plane.error().message, kWhere);
        }
        base_plane = std::move(*plane);
        const image::ImageView<float> base = base_plane.view();
        const auto lfloor = static_cast<float>(floor_value);
        auto filled = detail::for_each_row(region.height, parallel, cancel, kWhere, [&](std::int32_t row) {
            const float* p = rgb.row(region.y + row) + static_cast<std::ptrdiff_t>(region.x) * 3;
            float* l = base.row(row);
            for (std::int32_t x = 0; x < region.width; ++x, p += 3) {
                const auto lum = static_cast<float>(kLr * p[0] + kLg * p[1] + kLb * p[2]);
                l[x] = std::log2(std::max(lum, lfloor));
            }
        });
        if (!filled) {
            return Unexpected(std::move(filled.error()));
        }
        if (auto status = guided_filter(base, base, base, q.tone_radius, q.tone_epsilon, limits, cancel, parallel);
            !status) {
            return Unexpected(Error{status.error().code, "local adjustments: " + status.error().message,
                                    status.error().where, status.error().native_code});
        }
        // The weights as functions of the (exposure-shifted) base value, 256 nodes per EV over the span
        // where they vary (constant beyond it).
        const double lo = white_ev + kLocalToneFloorEv - 1.0 - kMaxLocalExposureEv;
        const double hi = white_ev + 1.0 + kMaxLocalExposureEv;
        const auto count = static_cast<std::int32_t>(256.0 * (hi - lo)) + 1;
        auto hw = detail::UniformLut::create(
            lo, hi, count, [&](double b) { return local_tone_gain_ev(b, *look, 1.0, 0.0) / kHighlightsMaxEv; },
            kWhere);
        if (!hw) {
            return Unexpected(std::move(hw.error()));
        }
        auto sw = detail::UniformLut::create(
            lo, hi, count, [&](double b) { return local_tone_gain_ev(b, *look, 0.0, 1.0) / kShadowsMaxEv; }, kWhere);
        if (!sw) {
            return Unexpected(std::move(sw.error()));
        }
        highlight_weight = std::move(*hw);
        shadow_weight = std::move(*sw);
        record.tone_radius = q.tone_radius;
        record.tone_region = region;
    }

    // ---- The per-pixel pass ------------------------------------------------------------------------------
    const LocalWhiteAxes& wa = *axes;
    std::vector<std::uint64_t> changed;
    try {
        changed.assign(static_cast<std::size_t>(size.height), 0);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate per-row counters", kWhere);
    }
    std::atomic<std::uint64_t> non_finite{0};
    const image::ConstImageView<float> base_view = base_plane.const_view();
    auto status = detail::for_each_row(size.height, parallel, cancel, kWhere, [&](std::int32_t y) {
        float* p = rgb.row(y);
        std::uint64_t touched = 0;
        std::uint64_t bad = 0;
        for (std::int32_t x = 0; x < size.width; ++x, p += 3) {
            double e = 0.0, con = 0.0, hl = 0.0, sh = 0.0, sat = 0.0, temp = 0.0, tint = 0.0;
            bool any = false;
            for (const Active& a : active) {
                const double m = a.coverage.at(x, y);
                const double w = a.opacity * (a.invert ? 1.0 - m : m);
                if (!(w > 0.0)) {
                    continue;
                }
                any = true;
                e += w * a.controls.exposure_ev;
                con += w * a.controls.contrast;
                hl += w * a.controls.highlights;
                sh += w * a.controls.shadows;
                sat += w * a.controls.saturation;
                temp += w * a.controls.temperature;
                tint += w * a.controls.tint;
            }
            if (!any) {
                continue;
            }
            ++touched;
            e = std::clamp(e, -kMaxLocalExposureEv, kMaxLocalExposureEv);
            con = std::clamp(con, -kMaxAdjustment, kMaxAdjustment);
            hl = std::clamp(hl, -kMaxAdjustment, kMaxAdjustment);
            sh = std::clamp(sh, -kMaxAdjustment, kMaxAdjustment);
            sat = std::clamp(sat, -kMaxAdjustment, kMaxAdjustment);
            temp = std::clamp(temp, -kMaxAdjustment, kMaxAdjustment);
            tint = std::clamp(tint, -kMaxAdjustment, kMaxAdjustment);
            double r = p[0];
            double g = p[1];
            double b = p[2];
            // 1. White shift, normalised to keep the luminance of a neutral.
            if (temp != 0.0 || tint != 0.0) {
                const double gr = fexp2(temp * wa.temperature[0] + tint * wa.tint[0]);
                const double gg = fexp2(temp * wa.temperature[1] + tint * wa.tint[1]);
                const double gb = fexp2(temp * wa.temperature[2] + tint * wa.tint[2]);
                const double n = kLr * gr + kLg * gg + kLb * gb;
                r *= gr / n;
                g *= gg / n;
                b *= gb / n;
            }
            double lum = kLr * r + kLg * g + kLb * b;
            // 2. One luminance gain: exposure, highlights/shadows and contrast.
            double ev = e;
            if ((hl != 0.0 || sh != 0.0) && highlight_weight) {
                double level = 0.0;
                if (region.contains(image::Point{x, y})) {
                    level = base_view.row(y - region.y)[x - region.x];
                } else {
                    level = flog2(std::max(lum, floor_value));
                }
                level += e;
                ev += kHighlightsMaxEv * hl * (*highlight_weight)(level) + kShadowsMaxEv * sh * (*shadow_weight)(level);
            }
            if (con != 0.0) {
                const double xv = flog2(std::max(lum, floor_value)) + ev - log2_pivot;
                ev += con * kLocalContrastStrength * kLocalContrastRangeEv * ftanh(xv / kLocalContrastRangeEv);
            }
            if (ev != 0.0) {
                const double k = fexp2(ev);
                r *= k;
                g *= k;
                b *= k;
                lum *= k;
            }
            // 3. Saturation around the luminance.
            if (sat != 0.0) {
                const double f = 1.0 + sat;
                r = lum + f * (r - lum);
                g = lum + f * (g - lum);
                b = lum + f * (b - lum);
            }
            p[0] = static_cast<float>(r);
            p[1] = static_cast<float>(g);
            p[2] = static_cast<float>(b);
            bad += static_cast<std::uint64_t>(!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]));
        }
        changed[static_cast<std::size_t>(y)] = touched;
        if (bad != 0) {
            non_finite.fetch_add(bad, std::memory_order_relaxed);
        }
    });
    if (!status) {
        return Unexpected(std::move(status.error()));
    }
    if (non_finite.load() != 0) {
        return fail(ErrorCode::Internal,
                    std::format("local adjustments produced {} non-finite pixels", non_finite.load()), kWhere);
    }
    for (const std::uint64_t n : changed) {
        record.changed_pixels += n;
    }
    return record;
}

}  // namespace lucent::engine
