#pragma once

// Applies a lens-correction plan to a stage-one image, one output row at a time (private; used by
// the fused first pass of stage two, develop.cpp; lens_correction.hpp section 4).
//
// For every output pixel of the render (full resolution or a proxy, positions scaled per axis to
// the full-resolution model) and every camera channel: the source position of that channel
// (LensGeometryMap's model), the channel sampled there with the Keys cubic convolution kernel
// a = -0.5 (Catmull-Rom, 4 x 4 taps, sums in double, taps beyond the image clamped to its edge
// pixels, as the DNG warps, ADR 0014) and divided by the vignetting at that source position. Without
// a geometric correction the pixel itself is read (no resampling) and only the vignetting applies.
//
// Thread-safety: row() is const and may run concurrently for different rows.

#include <array>
#include <cstdint>

#include <lucent/engine/lens_correction.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/image/pixel_buffer.hpp>

namespace lucent::engine::detail {

class LensRowSampler {
public:
    // `pixels`: stage one's image (3 channels camera R, G, B or 1 channel), upright, at the render
    // scale; `full`: its full-resolution upright size (the plan's).
    LensRowSampler(const LensCorrectionPlan& plan, image::ConstImageView<float> pixels,
                   image::Size full) noexcept;

    [[nodiscard]] bool active() const noexcept { return geometry_ != nullptr || vignetting_ != nullptr; }
    [[nodiscard]] bool resamples() const noexcept { return geometry_ != nullptr; }

    // Writes the corrected values of output row `y` (width x channels floats) to `out` and returns
    // the number of source positions that lay outside the image (their taps were clamped).
    std::uint64_t row(std::int32_t y, float* out) const noexcept;

private:
    struct Affine {
        double a = 1.0, b = 0.0, c = 0.0, d = 0.0, e = 1.0, f = 0.0;
        [[nodiscard]] image::PointD apply(double x, double y) const noexcept {
            return image::PointD{a * x + b * y + c, d * x + e * y + f};
        }
    };

    const LensGeometryMap* geometry_ = nullptr;
    const LensVignettingCorrection* vignetting_ = nullptr;
    image::ConstImageView<float> pixels_;
    std::int32_t width_ = 0;
    std::int32_t height_ = 0;
    std::int32_t channels_ = 1;
    Affine to_normalized_;  // render pixel continuous point -> normalised (corrected) position
    Affine to_buffer_;      // normalised source position -> render buffer coordinate (centres at integers)
    double inverse_aspect2_ = 1.0;  // 1 / aspect_ratio_correction^2 (vignetting)
    std::array<double, 3> vignetting_terms_{};
};

}  // namespace lucent::engine::detail
