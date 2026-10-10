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

// A bound within this many pixels of a whole pixel is that pixel when a crop rounds inward.
constexpr double kInwardSlack = 1e-6;
// An output pixel whose coverage falls short of 1 by less than this is covered: crops placed on the
// image's edge (largest_inside_crop()) touch it up to floating-point rounding.
constexpr double kCoverageSlack = 1e-4;

// [round(lo n), round(hi n)) with at least one pixel, inside [0, n); `inward`: [ceil(lo n),
// floor(hi n)) instead (the nearest pixels when that is empty).
void crop_span(double lo, double hi, std::int32_t n, bool inward, std::int32_t& first, std::int32_t& count) noexcept {
    auto a = static_cast<std::int32_t>(std::lround(lo * n));
    auto b = static_cast<std::int32_t>(std::lround(hi * n));
    if (inward) {
        const auto ia = static_cast<std::int32_t>(std::ceil(lo * n - kInwardSlack));
        const auto ib = static_cast<std::int32_t>(std::floor(hi * n + kInwardSlack));
        if (ib > ia) {
            a = ia;
            b = ib;
        }
    }
    a = std::clamp(a, 0, n - 1);
    b = std::clamp(b, a + 1, n);
    first = a;
    count = b - a;
}

// True when the image of `size` straightened by the angle (cosine `cs`, sine `sn`) covers the
// rectangle [x0, x1] x [y0, y1] of the straightened frame (pixels): its corners lie inside the image
// (within 1e-9 of its long edge), and the image is convex.
bool covers(image::Size size, double cs, double sn, double x0, double y0, double x1, double y1) noexcept {
    const double w = size.width;
    const double h = size.height;
    const double tolerance = 1e-9 * std::max(w, h);
    for (const double u : {x0, x1}) {
        for (const double v : {y0, y1}) {
            const double dx = u - 0.5 * w;
            const double dy = v - 0.5 * h;
            const double x = 0.5 * w + cs * dx - sn * dy;
            const double y = 0.5 * h + sn * dx + cs * dy;
            if (x < -tolerance || y < -tolerance || x > w + tolerance || y > h + tolerance) {
                return false;
            }
        }
    }
    return true;
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
    const image::PointD o = m.corrected_from_output(image::PointD{0.0, 0.0});
    const image::PointD x = m.corrected_from_output(image::PointD{1.0, 0.0});
    const image::PointD y = m.corrected_from_output(image::PointD{0.0, 1.0});
    return Affine{o, image::PointD{x.x - o.x, x.y - o.y}, image::PointD{y.x - o.x, y.y - o.y}};
}

}  // namespace

// ---- GeometryMapping ------------------------------------------------------------------------------

Expected<GeometryMapping> GeometryMapping::create(const GeometrySetting& geometry, image::Size upright_size) {
    return create(geometry, upright_size, image::PixelRect::from_size(upright_size));
}

Expected<GeometryMapping> GeometryMapping::create(const GeometrySetting& geometry, image::Size upright_size,
                                                  const image::PixelRect& frame) {
    if (upright_size.empty()) {
        return fail(ErrorCode::InvalidArgument, "the geometry needs a non-empty upright image size", kWhere);
    }
    if (!image::PixelRect::from_size(upright_size).contains(frame)) {
        return fail(
            ErrorCode::InvalidArgument,
            std::format("the geometry's frame {}x{}+{}+{} is not inside the {} x {} upright image",
                        frame.width, frame.height, frame.x, frame.y, upright_size.width, upright_size.height),
            kWhere);
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
    m.frame_ = frame;
    const image::Size frame_size = frame.size();
    const double radians = geometry.straighten_degrees * std::numbers::pi / 180.0;
    m.cos_ = std::cos(radians);
    m.sin_ = std::sin(radians);
    std::int32_t x0 = 0;
    std::int32_t w = 0;
    std::int32_t y0 = 0;
    std::int32_t h = 0;
    crop_span(c.left, c.right, frame_size.width, false, x0, w);
    crop_span(c.top, c.bottom, frame_size.height, false, y0, h);
    // A straightened crop on the image's edge keeps every output pixel covered: when rounding to the
    // nearest pixels moves an edge beyond the image and rounding inward does not, the crop rounds
    // inward (also at a proxy, whose aspect ratio differs from the full image's by the rounding of
    // its sides, so a crop inside the full image may reach a fraction of a pixel beyond it).
    if (geometry.straighten_degrees != 0.0 && !covers(frame_size, m.cos_, m.sin_, x0, y0, x0 + w, y0 + h)) {
        std::int32_t ix = 0;
        std::int32_t iw = 0;
        std::int32_t iy = 0;
        std::int32_t ih = 0;
        crop_span(c.left, c.right, frame_size.width, true, ix, iw);
        crop_span(c.top, c.bottom, frame_size.height, true, iy, ih);
        if (covers(frame_size, m.cos_, m.sin_, ix, iy, ix + iw, iy + ih)) {
            x0 = ix;
            w = iw;
            y0 = iy;
            h = ih;
        }
    }
    m.crop_ = image::PixelRect{x0, y0, w, h};
    m.orientation_ = user_orientation(geometry);
    m.output_ = image::oriented_size(image::Size{w, h}, m.orientation_);
    std::uint64_t r = geometry_revision(geometry);
    const auto mix = [&r](std::int32_t v) {
        r ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(v)) + 0x9e3779b97f4a7c15ull + (r << 6) + (r >> 2);
    };
    mix(upright_size.width);
    mix(upright_size.height);
    if (frame != image::PixelRect::from_size(upright_size)) {
        for (const std::int32_t v : {frame.x, frame.y, frame.width, frame.height}) {
            mix(v);
        }
    }
    m.revision_ = r;
    return m;
}

Expected<GeometryMapping> GeometryMapping::create(const GeometrySetting& geometry, image::Size upright_size,
                                                  const image::PixelRect& frame, const LensGeometryMap& lens,
                                                  image::Size full_size) {
    if (full_size.empty()) {
        return fail(ErrorCode::InvalidArgument, "a lens correction's mapping needs a non-empty full size", kWhere);
    }
    auto m = create(geometry, upright_size, frame);
    if (!m) {
        return m;
    }
    m->lens_ = lens;
    m->full_ = full_size;
    m->revision_ ^= lens.revision() + 0x9e3779b97f4a7c15ull + (m->revision_ << 6) + (m->revision_ >> 2);
    return m;
}

bool GeometryMapping::affine_identity() const noexcept {
    return setting_.straighten_degrees == 0.0 && orientation_ == image::Orientation::Normal &&
           frame_ == image::PixelRect::from_size(upright_) && crop_ == image::PixelRect::from_size(upright_);
}

bool GeometryMapping::identity() const noexcept { return affine_identity() && !lens_.has_value(); }

double GeometryMapping::pixels_per_unit_length() const noexcept { return static_cast<double>(long_edge(upright_)); }

image::PointD GeometryMapping::corrected_from_upright(image::PointD p) const noexcept {
    if (!lens_) {
        return p;
    }
    const double fx = static_cast<double>(full_.width) / upright_.width;
    const double fy = static_cast<double>(full_.height) / upright_.height;
    const auto c = lens_->corrected_of(image::PointD{p.x * fx, p.y * fy});
    if (!c) {
        return p;  // beyond the model's monotone range: as without the correction
    }
    return image::PointD{c->x / fx, c->y / fy};
}

image::PointD GeometryMapping::upright_from_corrected(image::PointD p) const noexcept {
    if (!lens_) {
        return p;
    }
    const double fx = static_cast<double>(full_.width) / upright_.width;
    const double fy = static_cast<double>(full_.height) / upright_.height;
    const image::PointD s = lens_->source_of(image::PointD{p.x * fx, p.y * fy}, 1);
    return image::PointD{s.x / fx, s.y / fy};
}

image::PointD GeometryMapping::output_from_upright(image::PointD p) const noexcept {
    return output_from_corrected(corrected_from_upright(p));
}

image::PointD GeometryMapping::upright_from_output(image::PointD o) const noexcept {
    return upright_from_corrected(corrected_from_output(o));
}

// With F = U' (frame_ at the origin) every expression below is exactly version 1's: adding and
// subtracting the frame's zero origin changes no bit.
image::PointD GeometryMapping::output_from_corrected(image::PointD p) const noexcept {
    const double hx = 0.5 * frame_.width;  // F's centre in S
    const double hy = 0.5 * frame_.height;
    const double dx = p.x - (frame_.x + hx);
    const double dy = p.y - (frame_.y + hy);
    const image::PointD s{hx + cos_ * dx + sin_ * dy, hy - sin_ * dx + cos_ * dy};
    const image::PointD q{s.x - crop_.x, s.y - crop_.y};
    return image::map_to_oriented(q, crop_.size(), orientation_);
}

image::PointD GeometryMapping::corrected_from_output(image::PointD o) const noexcept {
    const image::PointD q = image::map_from_oriented(o, crop_.size(), orientation_);
    const double hx = 0.5 * frame_.width;
    const double hy = 0.5 * frame_.height;
    const double dx = q.x + crop_.x - hx;
    const double dy = q.y + crop_.y - hy;
    return image::PointD{(frame_.x + hx) + cos_ * dx - sin_ * dy, (frame_.y + hy) + sin_ * dx + cos_ * dy};
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
    const CropRect& c = geometry.crop;
    return covers(size, std::cos(radians), std::sin(radians), c.left * w, c.top * h, c.right * w, c.bottom * h);
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
    record.frame = mapping.frame();
    record.crop = mapping.crop_pixels();
    record.orientation = mapping.orientation();
    record.input_size = image.pixels.size();
    record.output_size = mapping.output_size();
    record.resampled = mapping.resamples();
    if (mapping.affine_identity()) {
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
    // F's edges (U's for version 1): coverage is measured against them.
    const double fx0 = mapping.frame().x;
    const double fy0 = mapping.frame().y;
    const double fx1 = fx0 + mapping.frame().width;
    const double fy1 = fy0 + mapping.frame().height;
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
                // Coverage: the signed distance of the sample point to the frame's edge.
                const double d = std::min(std::min(px - fx0, fx1 - px), std::min(py - fy0, fy1 - py));
                const double coverage = d >= 0.5 - kCoverageSlack ? 1.0 : std::clamp(0.5 + d, 0.0, 1.0);
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
