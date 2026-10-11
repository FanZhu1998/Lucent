// The per-row application of a lens-correction plan (lens_sampler.hpp).

#include "lens_sampler.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "parallel_rows.hpp"

namespace lucent::engine::detail {

namespace {

// Keys cubic convolution with a = -0.5 (Catmull-Rom): weights of the taps at -1, 0, 1, 2 for a
// fractional offset t in [0, 1) (the DNG warps' kernel, dng_opcode_processing.cpp).
inline std::array<double, 4> catmull_rom(double t) noexcept {
    const double t2 = t * t;
    const double t3 = t2 * t;
    return {0.5 * (-t3 + 2.0 * t2 - t), 0.5 * (3.0 * t3 - 5.0 * t2 + 2.0), 0.5 * (-3.0 * t3 + 4.0 * t2 + t),
            0.5 * (t3 - t2)};
}

struct Taps {
    std::array<std::int32_t, 4> index{};
    std::array<double, 4> weight{};
};

inline Taps taps_for(double position, std::int32_t extent) noexcept {
    Taps taps;
    const double whole = std::floor(position);
    const auto base = static_cast<std::int64_t>(whole);
    taps.weight = catmull_rom(position - whole);
    for (std::int32_t k = 0; k < 4; ++k) {
        taps.index[static_cast<std::size_t>(k)] =
            static_cast<std::int32_t>(std::clamp<std::int64_t>(base - 1 + k, 0, extent - 1));
    }
    return taps;
}

inline double sample(const image::ConstImageView<float>& view, const Taps& tx, const Taps& ty,
                     std::int32_t channels, std::int32_t channel) noexcept {
    double sum = 0.0;
    for (std::size_t j = 0; j < 4; ++j) {
        const float* row = view.row(ty.index[j]);
        double line = 0.0;
        for (std::size_t i = 0; i < 4; ++i) {
            line += tx.weight[i] *
                    static_cast<double>(row[static_cast<std::ptrdiff_t>(tx.index[i]) * channels + channel]);
        }
        sum += ty.weight[j] * line;
    }
    return sum;
}

}  // namespace

LensRowSampler::LensRowSampler(const LensCorrectionPlan& plan, image::ConstImageView<float> pixels,
                               image::Size full) noexcept
    : pixels_(pixels), width_(pixels.width()), height_(pixels.height()), channels_(pixels.channels()) {
    if (plan.geometry) {
        geometry_ = &*plan.geometry;
    }
    if (plan.vignetting) {
        vignetting_ = &*plan.vignetting;
        const double a = vignetting_->frame.aspect_ratio_correction;
        inverse_aspect2_ = 1.0 / (a * a);
        vignetting_terms_ = vignetting_->calibration.terms;
    }
    if (!active() || width_ <= 0 || height_ <= 0 || full.empty()) {
        geometry_ = nullptr;
        vignetting_ = nullptr;
        return;
    }
    const LensfunFrame& frame = geometry_ != nullptr ? geometry_->frame : vignetting_->frame;
    const image::Orientation orientation =
        geometry_ != nullptr ? geometry_->orientation : vignetting_->orientation;
    const double fx = static_cast<double>(full.width) / static_cast<double>(width_);
    const double fy = static_cast<double>(full.height) / static_cast<double>(height_);
    // Both compositions are affine (a per-axis scale, an orientation, Lensfun's normalisation), so
    // three points determine them exactly.
    const auto forward = [&](double x, double y) {
        const image::PointD upright{x * fx, y * fy};
        return frame.normalized(image::map_from_oriented(upright, frame.size, orientation));
    };
    const auto backward = [&](double nx, double ny) {
        const image::PointD upright =
            image::map_to_oriented(frame.stored(image::PointD{nx, ny}), frame.size, orientation);
        return image::PointD{upright.x / fx - 0.5, upright.y / fy - 0.5};
    };
    const auto fit = [](const auto& fn) {
        const image::PointD p0 = fn(0.0, 0.0);
        const image::PointD px = fn(1.0, 0.0);
        const image::PointD py = fn(0.0, 1.0);
        Affine m;
        m.a = px.x - p0.x;
        m.b = py.x - p0.x;
        m.c = p0.x;
        m.d = px.y - p0.y;
        m.e = py.y - p0.y;
        m.f = p0.y;
        return m;
    };
    to_normalized_ = fit(forward);
    to_buffer_ = fit(backward);
}

std::uint64_t LensRowSampler::row(std::int32_t y, float* out) const noexcept {
    std::uint64_t outside = 0;
    const double cy = static_cast<double>(y) + 0.5;
    const auto gain = [&](double nx, double ny) {
        if (vignetting_ == nullptr) {
            return 1.0;
        }
        const double r2 = (nx * nx + ny * ny) * inverse_aspect2_;
        const auto& k = vignetting_terms_;
        return 1.0 / (1.0 + k[0] * r2 + k[1] * r2 * r2 + k[2] * r2 * r2 * r2);
    };
    if (geometry_ == nullptr) {
        // Vignetting alone: every pixel keeps its place.
        const float* src = pixels_.row(y);
        for (std::int32_t x = 0; x < width_; ++x) {
            const image::PointD n = to_normalized_.apply(static_cast<double>(x) + 0.5, cy);
            const double g = gain(n.x, n.y);
            for (std::int32_t c = 0; c < channels_; ++c) {
                const std::ptrdiff_t i = static_cast<std::ptrdiff_t>(x) * channels_ + c;
                out[i] = static_cast<float>(static_cast<double>(src[i]) * g);
            }
        }
        return 0;
    }
    const LensGeometryMap& map = *geometry_;
    const bool per_channel = map.tca.has_value() && channels_ == 3;
    const double max_x = static_cast<double>(width_) - 0.5;
    const double max_y = static_cast<double>(height_) - 0.5;
    for (std::int32_t x = 0; x < width_; ++x) {
        const image::PointD n = to_normalized_.apply(static_cast<double>(x) + 0.5, cy);
        if (!per_channel) {
            // One source position for every channel: green's (distortion alone, or monochrome).
            const image::PointD s = map.normalized_source(n, 1);
            const image::PointD b = to_buffer_.apply(s.x, s.y);
            if (!(b.x >= -0.5 && b.x <= max_x && b.y >= -0.5 && b.y <= max_y)) {
                outside += static_cast<std::uint64_t>(channels_);
            }
            const double bx = std::isfinite(b.x) ? std::clamp(b.x, -2.0, max_x + 2.0) : 0.0;
            const double by = std::isfinite(b.y) ? std::clamp(b.y, -2.0, max_y + 2.0) : 0.0;
            const Taps tx = taps_for(bx, width_);
            const Taps ty = taps_for(by, height_);
            const double g = gain(s.x, s.y);
            for (std::int32_t c = 0; c < channels_; ++c) {
                out[static_cast<std::ptrdiff_t>(x) * channels_ + c] =
                    static_cast<float>(sample(pixels_, tx, ty, channels_, c) * g);
            }
            continue;
        }
        // The distortion once for the three channels (normalized_sources(): the same arithmetic as
        // normalized_source() per channel).
        const std::array<image::PointD, 3> sources = map.normalized_sources(n);
        for (std::int32_t c = 0; c < 3; ++c) {
            const image::PointD s = sources[static_cast<std::size_t>(c)];
            const image::PointD b = to_buffer_.apply(s.x, s.y);
            if (!(b.x >= -0.5 && b.x <= max_x && b.y >= -0.5 && b.y <= max_y)) {
                ++outside;
            }
            const double bx = std::isfinite(b.x) ? std::clamp(b.x, -2.0, max_x + 2.0) : 0.0;
            const double by = std::isfinite(b.y) ? std::clamp(b.y, -2.0, max_y + 2.0) : 0.0;
            const Taps tx = taps_for(bx, width_);
            const Taps ty = taps_for(by, height_);
            out[static_cast<std::ptrdiff_t>(x) * 3 + c] =
                static_cast<float>(sample(pixels_, tx, ty, 3, c) * gain(s.x, s.y));
        }
    }
    return outside;
}

Expected<CorrectedImage> correct_whole_image(const LensRowSampler& sampler, image::Size size, std::int32_t channels,
                                             const image::AllocationLimits& limits,
                                             const core::CancellationToken& cancel, bool parallel,
                                             std::string_view where) {
    CorrectedImage result;
    auto out = image::ImageBuffer<float>::create_uninitialized(size.width, size.height, channels, limits);
    if (!out) {
        return Unexpected(std::move(out.error()));
    }
    std::vector<std::uint64_t> outside;
    try {
        outside.assign(static_cast<std::size_t>(size.height), 0);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate per-row counters", std::string(where));
    }
    const image::ImageView<float> dst = out->view();
    auto status = for_each_row(size.height, parallel, cancel, where, [&](std::int32_t y) {
        outside[static_cast<std::size_t>(y)] = sampler.row(y, dst.row(y));
    });
    if (!status) {
        return Unexpected(std::move(status.error()));
    }
    for (const std::uint64_t n : outside) {
        result.outside_samples += n;
    }
    result.pixels = std::move(*out);
    return result;
}

}  // namespace lucent::engine::detail

namespace lucent::engine {

Expected<LensCorrectionRecord> apply_lens_correction(const LensCorrectionPlan& plan,
                                                     image::ImageBuffer<float>& pixels, image::Size full_size,
                                                     const image::AllocationLimits& limits,
                                                     const core::CancellationToken& cancel, bool parallel) {
    constexpr const char* kWhere = "engine.lens.apply";
    const auto start = std::chrono::steady_clock::now();
    if (pixels.empty() || (pixels.channels() != 1 && pixels.channels() != 3) || full_size.empty()) {
        return fail(
            ErrorCode::InvalidArgument,
            std::format("a lens correction needs a non-empty 1- or 3-channel image and its full size (got {} "
                        "channels)",
                        pixels.channels()),
            kWhere);
    }
    LensCorrectionRecord record;
    record.plan = plan;
    record.render_size = pixels.size();
    const detail::LensRowSampler sampler(plan, pixels.const_view(), full_size);
    if (!sampler.active()) {
        return record;
    }
    auto corrected =
        detail::correct_whole_image(sampler, pixels.size(), pixels.channels(), limits, cancel, parallel, kWhere);
    if (!corrected) {
        return Unexpected(std::move(corrected.error()));
    }
    record.outside_samples = corrected->outside_samples;
    record.resampled = sampler.resamples();
    pixels = std::move(corrected->pixels);
    record.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return record;
}

}  // namespace lucent::engine
