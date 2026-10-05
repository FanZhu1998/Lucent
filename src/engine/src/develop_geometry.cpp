// The geometry operation and the mask/output point mapping (develop_geometry.hpp).

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <new>
#include <numbers>
#include <vector>

#include <lucent/engine/develop_geometry.hpp>

#include "parallel_rows.hpp"

namespace lucent::engine {

namespace {

constexpr const char* kWhere = "engine.develop.geometry";

std::int32_t long_edge(image::Size size) noexcept { return std::max(size.width, size.height); }

// [round(lo n), round(hi n)) with at least one pixel, inside [0, n).
void crop_span(double lo, double hi, std::int32_t n, std::int32_t& first, std::int32_t& count) noexcept {
    auto a = static_cast<std::int32_t>(std::lround(lo * n));
    auto b = static_cast<std::int32_t>(std::lround(hi * n));
    a = std::clamp(a, 0, n - 1);
    b = std::clamp(b, a + 1, n);
    first = a;
    count = b - a;
}

// Keys cubic convolution weights, a = -0.5 (Catmull-Rom), for taps at -1, 0, 1, 2 around a
// fraction t in [0, 1).
std::array<double, 4> catmull_rom(double t) noexcept {
    return {((-0.5 * t + 1.0) * t - 0.5) * t, (1.5 * t - 2.5) * t * t + 1.0, ((-1.5 * t + 2.0) * t + 0.5) * t,
            (0.5 * t - 0.5) * t * t};
}

// The affine map from output pixel coordinates to upright pixel coordinates:
// p = origin + x * dx + y * dy.
struct Affine {
    image::PointD origin;
    image::PointD dx;
    image::PointD dy;
};

Affine output_to_upright_affine(const GeometryMapping& m) noexcept {
    const image::PointD o = m.upright_from_output(image::PointD{0.0, 0.0});
    const image::PointD x = m.upright_from_output(image::PointD{1.0, 0.0});
    const image::PointD y = m.upright_from_output(image::PointD{0.0, 1.0});
    return Affine{o, image::PointD{x.x - o.x, x.y - o.y}, image::PointD{y.x - o.x, y.y - o.y}};
}

}  // namespace

// ---- GeometryMapping ------------------------------------------------------------------------------

Expected<GeometryMapping> GeometryMapping::create(const GeometrySetting& geometry, image::Size upright_size) {
    if (upright_size.empty()) {
        return fail(ErrorCode::InvalidArgument, "the geometry needs a non-empty upright image size", kWhere);
    }
    const CropRect& c = geometry.crop;
    if (!std::isfinite(geometry.straighten_degrees) || std::abs(geometry.straighten_degrees) > kMaxStraightenDegrees ||
        !std::isfinite(c.left) || !std::isfinite(c.top) || !std::isfinite(c.right) || !std::isfinite(c.bottom) ||
        c.left < 0.0 || c.top < 0.0 || c.right > 1.0 || c.bottom > 1.0 || !(c.right - c.left >= kMinCropExtent) ||
        !(c.bottom - c.top >= kMinCropExtent) || geometry.quarter_turns > 3) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("invalid geometry (straighten {} degrees, crop ({}, {}) - ({}, {}), {} quarter turns)",
                                geometry.straighten_degrees, c.left, c.top, c.right, c.bottom, geometry.quarter_turns),
                    kWhere);
    }
    GeometryMapping m;
    m.setting_ = geometry;
    m.upright_ = upright_size;
    std::int32_t x0 = 0;
    std::int32_t w = 0;
    std::int32_t y0 = 0;
    std::int32_t h = 0;
    crop_span(c.left, c.right, upright_size.width, x0, w);
    crop_span(c.top, c.bottom, upright_size.height, y0, h);
    m.crop_ = image::PixelRect{x0, y0, w, h};
    m.orientation_ = user_orientation(geometry);
    m.output_ = image::oriented_size(image::Size{w, h}, m.orientation_);
    const double radians = geometry.straighten_degrees * std::numbers::pi / 180.0;
    m.cos_ = std::cos(radians);
    m.sin_ = std::sin(radians);
    std::uint64_t r = geometry_revision(geometry);
    for (const std::int32_t v : {upright_size.width, upright_size.height}) {
        r ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(v)) + 0x9e3779b97f4a7c15ull + (r << 6) + (r >> 2);
    }
    m.revision_ = r;
    return m;
}

bool GeometryMapping::identity() const noexcept {
    return setting_.straighten_degrees == 0.0 && orientation_ == image::Orientation::Normal &&
           crop_ == image::PixelRect::from_size(upright_);
}

double GeometryMapping::pixels_per_unit_length() const noexcept { return static_cast<double>(long_edge(upright_)); }

image::PointD GeometryMapping::output_from_upright(image::PointD p) const noexcept {
    const double cx = 0.5 * upright_.width;
    const double cy = 0.5 * upright_.height;
    const double dx = p.x - cx;
    const double dy = p.y - cy;
    const image::PointD s{cx + cos_ * dx + sin_ * dy, cy - sin_ * dx + cos_ * dy};
    const image::PointD q{s.x - crop_.x, s.y - crop_.y};
    return image::map_to_oriented(q, crop_.size(), orientation_);
}

image::PointD GeometryMapping::upright_from_output(image::PointD o) const noexcept {
    const image::PointD q = image::map_from_oriented(o, crop_.size(), orientation_);
    const double cx = 0.5 * upright_.width;
    const double cy = 0.5 * upright_.height;
    const double dx = q.x + crop_.x - cx;
    const double dy = q.y + crop_.y - cy;
    return image::PointD{cx + cos_ * dx - sin_ * dy, cy + sin_ * dx + cos_ * dy};
}

image::PointD GeometryMapping::output_from_normalized(MaskPoint p) const noexcept {
    return output_from_upright(image::PointD{p.x * upright_.width, p.y * upright_.height});
}

MaskPoint GeometryMapping::normalized_from_output(image::PointD o) const noexcept {
    const image::PointD p = upright_from_output(o);
    return MaskPoint{p.x / upright_.width, p.y / upright_.height};
}

bool GeometryMapping::inside_output(image::PointD o) const noexcept {
    return o.x >= 0.0 && o.y >= 0.0 && o.x <= output_.width && o.y <= output_.height;
}

// ---- Crop helpers ---------------------------------------------------------------------------------

bool crop_inside_image(const GeometrySetting& geometry, image::Size size) noexcept {
    if (size.empty()) {
        return false;
    }
    const double w = size.width;
    const double h = size.height;
    const double radians = geometry.straighten_degrees * std::numbers::pi / 180.0;
    const double cs = std::cos(radians);
    const double sn = std::sin(radians);
    const double tolerance = 1e-9 * std::max(w, h);
    const CropRect& c = geometry.crop;
    for (const double u : {c.left, c.right}) {
        for (const double v : {c.top, c.bottom}) {
            const double dx = u * w - 0.5 * w;
            const double dy = v * h - 0.5 * h;
            const double x = 0.5 * w + cs * dx - sn * dy;
            const double y = 0.5 * h + sn * dx + cs * dy;
            if (x < -tolerance || y < -tolerance || x > w + tolerance || y > h + tolerance) {
                return false;
            }
        }
    }
    return true;
}

CropRect largest_inside_crop(double straighten_degrees, image::Size size, double aspect) noexcept {
    if (size.empty() || !std::isfinite(straighten_degrees)) {
        return CropRect{};
    }
    const double w = size.width;
    const double h = size.height;
    const double a = std::isfinite(aspect) && aspect > 0.0 ? aspect : w / h;
    const double radians = straighten_degrees * std::numbers::pi / 180.0;
    const double cs = std::abs(std::cos(radians));
    const double sn = std::abs(std::sin(radians));
    // A centred rectangle of height t and width a t fits when its rotated corners stay inside:
    // t (a cos + sin) <= W and t (a sin + cos) <= H (half extents on both sides).
    const double t = std::min(w / (a * cs + sn), h / (a * sn + cs));
    const double half_x = std::min(0.5, 0.5 * a * t / w);
    const double half_y = std::min(0.5, 0.5 * t / h);
    return CropRect{0.5 - half_x, 0.5 - half_y, 0.5 + half_x, 0.5 + half_y};
}

// ---- Rendering ------------------------------------------------------------------------------------

Expected<GeometryRecord> apply_geometry(ColorImage& image, const GeometryMapping& mapping,
                                        const image::AllocationLimits& limits, const core::CancellationToken& cancel,
                                        bool parallel) {
    if (image.pixels.empty() || image.pixels.channels() < 1 || image.pixels.channels() > 4) {
        return fail(ErrorCode::InvalidArgument, "the geometry needs a non-empty image of 1 to 4 channels", kWhere);
    }
    if (image.pixels.size() != mapping.upright_size()) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the image is {}x{}, the geometry was made for {}x{}", image.pixels.width(),
                                image.pixels.height(), mapping.upright_size().width, mapping.upright_size().height),
                    kWhere);
    }
    GeometryRecord record;
    record.straighten_degrees = mapping.setting().straighten_degrees;
    record.crop = mapping.crop_pixels();
    record.orientation = mapping.orientation();
    record.input_size = image.pixels.size();
    record.output_size = mapping.output_size();
    record.resampled = mapping.resamples();
    if (mapping.identity()) {
        return record;
    }
    const image::Size out_size = mapping.output_size();
    const std::int32_t channels = image.pixels.channels();
    auto out = image::ImageBuffer<float>::create_uninitialized(out_size.width, out_size.height, channels, limits);
    if (!out) {
        return fail(out.error().code, "geometry output: " + out.error().message, kWhere);
    }
    const image::ConstImageView<float> src = image.pixels.const_view();
    const image::ImageView<float> dst = out->view();
    const Affine a = output_to_upright_affine(mapping);
    const std::int32_t width = src.width();
    const std::int32_t height = src.height();
    std::vector<std::uint64_t> uncovered;
    try {
        uncovered.assign(static_cast<std::size_t>(out_size.height), 0);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate per-row counters", kWhere);
    }

    Status status = ok();
    if (!mapping.resamples()) {
        // Whole-pixel crop and an orientation: output pixel centres map to input pixel centres
        // exactly (the affine coefficients are 0 or +-1 and the offsets whole or half pixels).
        status = detail::for_each_row(out_size.height, parallel, cancel, kWhere, [&](std::int32_t y) {
            float* o = dst.row(y);
            for (std::int32_t x = 0; x < out_size.width; ++x, o += channels) {
                const double ox = x + 0.5;
                const double oy = y + 0.5;
                const double px = a.origin.x + ox * a.dx.x + oy * a.dy.x;
                const double py = a.origin.y + ox * a.dx.y + oy * a.dy.y;
                const auto sx = std::clamp(static_cast<std::int32_t>(std::floor(px)), 0, width - 1);
                const auto sy = std::clamp(static_cast<std::int32_t>(std::floor(py)), 0, height - 1);
                const float* s = src.row(sy) + static_cast<std::ptrdiff_t>(sx) * channels;
                for (std::int32_t c = 0; c < channels; ++c) {
                    o[c] = s[c];
                }
            }
        });
    } else {
        status = detail::for_each_row(out_size.height, parallel, cancel, kWhere, [&](std::int32_t y) {
            float* o = dst.row(y);
            std::uint64_t partial = 0;
            for (std::int32_t x = 0; x < out_size.width; ++x, o += channels) {
                const double ox = x + 0.5;
                const double oy = y + 0.5;
                const double px = a.origin.x + ox * a.dx.x + oy * a.dy.x;
                const double py = a.origin.y + ox * a.dx.y + oy * a.dy.y;
                // Coverage: the signed distance of the sample point to the image's edge.
                const double d = std::min(std::min(px, width - px), std::min(py, height - py));
                const double coverage = std::clamp(0.5 + d, 0.0, 1.0);
                if (coverage < 1.0) {
                    ++partial;
                }
                if (coverage <= 0.0) {
                    for (std::int32_t c = 0; c < channels; ++c) {
                        o[c] = 0.0f;
                    }
                    continue;
                }
                const double sx = px - 0.5;
                const double sy = py - 0.5;
                const double fx = std::floor(sx);
                const double fy = std::floor(sy);
                const std::array<double, 4> wx = catmull_rom(sx - fx);
                const std::array<double, 4> wy = catmull_rom(sy - fy);
                const auto ix = static_cast<std::int32_t>(fx);
                const auto iy = static_cast<std::int32_t>(fy);
                std::array<std::int32_t, 4> cols{};
                std::array<const float*, 4> rows{};
                for (std::int32_t k = 0; k < 4; ++k) {
                    cols[static_cast<std::size_t>(k)] = std::clamp(ix - 1 + k, 0, width - 1) * channels;
                    rows[static_cast<std::size_t>(k)] = src.row(std::clamp(iy - 1 + k, 0, height - 1));
                }
                for (std::int32_t c = 0; c < channels; ++c) {
                    double sum = 0.0;
                    for (std::size_t j = 0; j < 4; ++j) {
                        const float* r = rows[j] + c;
                        const double h = wx[0] * r[cols[0]] + wx[1] * r[cols[1]] + wx[2] * r[cols[2]] +
                                         wx[3] * r[cols[3]];
                        sum += wy[j] * h;
                    }
                    // No ringing: the range of the four nearest pixels.
                    const float a00 = rows[1][cols[1] + c];
                    const float a01 = rows[1][cols[2] + c];
                    const float a10 = rows[2][cols[1] + c];
                    const float a11 = rows[2][cols[2] + c];
                    const double lo = std::min(std::min(a00, a01), std::min(a10, a11));
                    const double hi = std::max(std::max(a00, a01), std::max(a10, a11));
                    o[c] = static_cast<float>(coverage * std::clamp(sum, lo, hi));
                }
            }
            uncovered[static_cast<std::size_t>(y)] = partial;
        });
    }
    if (!status) {
        return Unexpected(std::move(status.error()));
    }
    for (const std::uint64_t n : uncovered) {
        record.uncovered_pixels += n;
    }
    image.pixels = std::move(*out);
    return record;
}

}  // namespace lucent::engine
