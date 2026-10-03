#pragma once

// Guided image filter (blueprint section 5 "Noise, masks, and geometry" and Appendix A.2):
// K. He, J. Sun and X. Tang, "Guided Image Filtering", ECCV 2010 / IEEE TPAMI 35(6), 2013,
// Algorithm 1, with the conventions of OpenCV's cv::ximgproc::guidedFilter (opencv_contrib
// modules/ximgproc/src/guided_filter.cpp, BSD-3-Clause / Apache-2.0), the selected reference:
//
//     mean_I = f(I), mean_p = f(p), corr_I = f(I .* I), corr_Ip = f(I .* p)
//     var_I = corr_I - mean_I^2, cov_Ip = corr_Ip - mean_I .* mean_p
//     a = cov_Ip ./ (var_I + eps), b = mean_p - a .* mean_I
//     q = f(a) .* I + f(b)
//
// where f is the normalised box mean over the (2r + 1) x (2r + 1) window centred on each pixel
// and pixels outside the image are mirrored with the edge pixel repeated ("fedcba|abcdef",
// OpenCV BORDER_REFLECT, also for windows larger than the image). eps is in the squared units of
// the guide (OpenCV's `eps` argument, not its square): variations of the guide with a local
// variance well below eps are smoothed, edges with a variance well above it are kept.
//
// Units: the radius is in pixels of the image being filtered. Callers that define a radius in
// full-image pixels (blueprint: "Define radius in full-image pixels") scale it for proxies
// themselves (develop.hpp does). Composite operations built on the filter need a tiling halo of
// 2r pixels (two box passes) when processed in tiles; nothing in Lucent tiles yet.
//
// Numerics: samples are float32; window sums are accumulated in double; the guide is shifted by
// its mean before forming I .* I so that var_I does not lose precision to large offsets (the
// result is mathematically unchanged). Results match an exact (float64) evaluation of the
// formula to about 1e-5 for guides within +-16 (guided_filter_reference_test.cpp states the
// measured tolerance) and OpenCV's float32 implementation within its own rounding.
//
// Thread-safety: reentrant; rows and column strips run in parallel when requested, with
// schedule-independent results.

#include <cstdint>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/image/pixel_buffer.hpp>

namespace lucent::engine {

// Version of the guided-filter semantics.
inline constexpr std::uint32_t kGuidedFilterVersion = 1;

// Replaces every sample of `plane` (1..4 channels, each filtered independently) by the mean of
// its (2r + 1) x (2r + 1) window, mirrored at the borders as above, in place.
// Errors (engine.guided_filter): InvalidArgument for an empty view or radius < 1; OutOfMemory
// when the per-row scratch cannot be allocated; Cancelled.
[[nodiscard]] Status box_mean(image::ImageView<float> plane, std::int32_t radius,
                              const core::CancellationToken& cancel = {}, bool parallel = true);

// Guided filter of `input` (1..4 channels, filtered independently) with the single-channel
// `guide`, written to `output` (the input's shape). `output` may be the very same view as
// `input` and/or `guide` (in-place filtering); other overlaps are not allowed.
// Working memory: four float planes of the image size (one two-channel plane when input and
// guide are the same view), allocated under `limits`.
// Errors (engine.guided_filter): InvalidArgument for empty views, a guide with more than one
// channel, mismatched sizes or channel counts, radius < 1, or eps that is not finite and
// positive; LimitExceeded / OutOfMemory for the working planes; Cancelled.
[[nodiscard]] Status guided_filter(image::ConstImageView<float> guide, image::ConstImageView<float> input,
                                   image::ImageView<float> output, std::int32_t radius, double eps,
                                   const image::AllocationLimits& limits = {},
                                   const core::CancellationToken& cancel = {}, bool parallel = true);

}  // namespace lucent::engine
