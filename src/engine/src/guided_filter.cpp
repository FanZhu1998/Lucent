// Box mean and guided filter (guided_filter.hpp).

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <new>
#include <numeric>
#include <vector>

#include <lucent/engine/guided_filter.hpp>

#include "parallel_rows.hpp"
#include "scratch_pool.hpp"

namespace lucent::engine {

namespace {

constexpr const char* kWhere = "engine.guided_filter";
// Samples per column strip of the vertical pass (one cache line of doubles per 8 samples).
constexpr std::int32_t kStripSamples = 64;

// OpenCV borderInterpolate(p, n, BORDER_REFLECT): "fedcba|abcdefgh|hgfedcba", for any offset.
std::int32_t reflect(std::int64_t p, std::int32_t n) noexcept {
    if (n == 1) {
        return 0;
    }
    while (p < 0 || p >= n) {
        p = p < 0 ? -p - 1 : 2 * static_cast<std::int64_t>(n) - 1 - p;
    }
    return static_cast<std::int32_t>(p);
}

// idx[k] = reflect(k - r, n) for k = 0 .. n + 2r: the source index of every window position.
Expected<std::vector<std::int32_t>> reflection_table(std::int32_t n, std::int32_t r) {
    try {
        std::vector<std::int32_t> idx(static_cast<std::size_t>(n) + 2 * static_cast<std::size_t>(r) + 1);
        for (std::size_t k = 0; k < idx.size(); ++k) {
            idx[k] = reflect(static_cast<std::int64_t>(k) - r, n);
        }
        return idx;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the border table", kWhere);
    }
}

Status out_of_memory(const char* what) {
    return fail(ErrorCode::OutOfMemory, std::format("cannot allocate {}", what), kWhere);
}

// Box mean of a plane in place: a horizontal then a vertical running-sum pass, sums in double.
Status box_mean_impl(image::ImageView<float> plane, std::int32_t radius, const core::CancellationToken& cancel,
                     bool parallel) {
    const std::int32_t width = plane.width();
    const std::int32_t height = plane.height();
    const std::int32_t channels = plane.channels();
    const std::int32_t row_samples = width * channels;
    const double inv = 1.0 / static_cast<double>(2 * static_cast<std::int64_t>(radius) + 1);
    const std::int32_t span = 2 * radius + 1;

    auto horizontal = reflection_table(width, radius);
    if (!horizontal) {
        return Unexpected(std::move(horizontal.error()));
    }
    auto vertical = reflection_table(height, radius);
    if (!vertical) {
        return Unexpected(std::move(vertical.error()));
    }
    const std::vector<std::int32_t>& hx = *horizontal;
    const std::vector<std::int32_t>& vy = *vertical;

    // ---- Horizontal pass, per row ---------------------------------------------------------------
    {
        detail::ScratchPool pool(static_cast<std::size_t>(row_samples));
        std::atomic<bool> no_memory{false};
        auto status = detail::for_each_row(height, parallel, cancel, kWhere, [&](std::int32_t y) {
            float* scratch = pool.acquire();
            if (scratch == nullptr) {
                no_memory.store(true, std::memory_order_relaxed);
                return;
            }
            float* row = plane.row(y);
            std::memcpy(scratch, row, static_cast<std::size_t>(row_samples) * sizeof(float));
            for (std::int32_t c = 0; c < channels; ++c) {
                double sum = 0.0;
                for (std::int32_t k = 0; k < span; ++k) {
                    sum += scratch[static_cast<std::ptrdiff_t>(hx[static_cast<std::size_t>(k)]) * channels + c];
                }
                for (std::int32_t x = 0; x < width; ++x) {
                    row[static_cast<std::ptrdiff_t>(x) * channels + c] = static_cast<float>(sum * inv);
                    const std::int32_t add = hx[static_cast<std::size_t>(x) + static_cast<std::size_t>(span)];
                    const std::int32_t sub = hx[static_cast<std::size_t>(x)];
                    sum += static_cast<double>(scratch[static_cast<std::ptrdiff_t>(add) * channels + c]) -
                           static_cast<double>(scratch[static_cast<std::ptrdiff_t>(sub) * channels + c]);
                }
            }
            pool.release(scratch);
        });
        if (!status) {
            return status;
        }
        if (no_memory.load()) {
            return out_of_memory("the box filter's row scratch");
        }
    }

    // ---- Vertical pass, per strip of columns ----------------------------------------------------
    const std::int32_t strips = (row_samples + kStripSamples - 1) / kStripSamples;
    detail::ScratchPool pool(static_cast<std::size_t>(height) * kStripSamples);
    std::atomic<bool> no_memory{false};
    auto status = detail::for_each_row(strips, parallel, cancel, kWhere, [&](std::int32_t strip) {
        float* scratch = pool.acquire();
        if (scratch == nullptr) {
            no_memory.store(true, std::memory_order_relaxed);
            return;
        }
        const std::int32_t first = strip * kStripSamples;
        const std::int32_t count = std::min(kStripSamples, row_samples - first);
        for (std::int32_t y = 0; y < height; ++y) {
            std::memcpy(scratch + static_cast<std::ptrdiff_t>(y) * count, plane.row(y) + first,
                        static_cast<std::size_t>(count) * sizeof(float));
        }
        std::array<double, kStripSamples> acc{};
        for (std::int32_t k = 0; k < span; ++k) {
            const float* src = scratch + static_cast<std::ptrdiff_t>(vy[static_cast<std::size_t>(k)]) * count;
            for (std::int32_t j = 0; j < count; ++j) {
                acc[static_cast<std::size_t>(j)] += src[j];
            }
        }
        for (std::int32_t y = 0; y < height; ++y) {
            float* out = plane.row(y) + first;
            const float* add =
                scratch + static_cast<std::ptrdiff_t>(vy[static_cast<std::size_t>(y) + static_cast<std::size_t>(span)]) *
                              count;
            const float* sub = scratch + static_cast<std::ptrdiff_t>(vy[static_cast<std::size_t>(y)]) * count;
            for (std::int32_t j = 0; j < count; ++j) {
                double& a = acc[static_cast<std::size_t>(j)];
                out[j] = static_cast<float>(a * inv);
                a += static_cast<double>(add[j]) - static_cast<double>(sub[j]);
            }
        }
        pool.release(scratch);
    });
    if (!status) {
        return status;
    }
    if (no_memory.load()) {
        return out_of_memory("the box filter's column scratch");
    }
    return ok();
}

Status check_radius(std::int32_t radius) {
    if (radius < 1) {
        return fail(ErrorCode::InvalidArgument, std::format("the radius must be at least 1 (got {})", radius), kWhere);
    }
    return ok();
}

bool same_view(const image::ConstImageView<float>& a, const image::ConstImageView<float>& b) noexcept {
    return a.data() == b.data() && a.stride() == b.stride() && a.width() == b.width() && a.height() == b.height() &&
           a.channels() == b.channels();
}

Expected<image::ImageBuffer<float>> plane(std::int32_t width, std::int32_t height, std::int32_t channels,
                                          const image::AllocationLimits& limits) {
    auto buffer = image::ImageBuffer<float>::create_uninitialized(width, height, channels, limits);
    if (!buffer) {
        return fail(buffer.error().code, "guided-filter working plane: " + buffer.error().message, kWhere);
    }
    return buffer;
}

}  // namespace

Status box_mean(image::ImageView<float> plane, std::int32_t radius, const core::CancellationToken& cancel,
                bool parallel) {
    if (plane.empty()) {
        return fail(ErrorCode::InvalidArgument, "the plane is empty", kWhere);
    }
    if (auto valid = check_radius(radius); !valid) {
        return valid;
    }
    return box_mean_impl(plane, radius, cancel, parallel);
}

Status guided_filter(image::ConstImageView<float> guide, image::ConstImageView<float> input,
                     image::ImageView<float> output, std::int32_t radius, double eps,
                     const image::AllocationLimits& limits, const core::CancellationToken& cancel, bool parallel) {
    if (guide.empty() || input.empty() || output.empty()) {
        return fail(ErrorCode::InvalidArgument, "the guide, input and output must not be empty", kWhere);
    }
    if (guide.channels() != 1) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the guide must have one channel (got {})", guide.channels()), kWhere);
    }
    if (guide.size() != input.size() || output.size() != input.size() || output.channels() != input.channels()) {
        return fail(ErrorCode::InvalidArgument, "the guide, input and output must have the same size and the output "
                                                "the input's channel count",
                    kWhere);
    }
    if (auto valid = check_radius(radius); !valid) {
        return valid;
    }
    if (!std::isfinite(eps) || !(eps > 0.0)) {
        return fail(ErrorCode::InvalidArgument, std::format("eps must be finite and positive (got {})", eps), kWhere);
    }
    const std::int32_t width = guide.width();
    const std::int32_t height = guide.height();
    const std::int32_t channels = input.channels();

    // Mean of the guide (per-row sums added in row order: deterministic).
    std::vector<double> row_sums;
    try {
        row_sums.assign(static_cast<std::size_t>(height), 0.0);
    } catch (const std::bad_alloc&) {
        return out_of_memory("per-row sums");
    }
    if (auto status = detail::for_each_row(height, parallel, cancel, kWhere,
                                           [&](std::int32_t y) {
                                               const float* g = guide.row(y);
                                               double s = 0.0;
                                               for (std::int32_t x = 0; x < width; ++x) {
                                                   s += g[x];
                                               }
                                               row_sums[static_cast<std::size_t>(y)] = s;
                                           });
        !status) {
        return status;
    }
    const double shift = std::accumulate(row_sums.begin(), row_sums.end(), 0.0) /
                         (static_cast<double>(width) * static_cast<double>(height));
    const float fshift = static_cast<float>(shift);

    if (same_view(guide, input)) {
        // ---- Self-guided: I = p --------------------------------------------------------------------
        // One two-channel plane: (I, I^2) -> box -> (mean_I, corr_I) -> (a, b) -> box -> (mean_a,
        // mean_b). Each channel is filtered exactly as a separate plane would be.
        auto work = plane(width, height, 2, limits);
        if (!work) {
            return Unexpected(std::move(work.error()));
        }
        const image::ImageView<float> ab = work->view();
        if (auto status = detail::for_each_row(height, parallel, cancel, kWhere,
                                               [&](std::int32_t y) {
                                                   const float* g = guide.row(y);
                                                   float* p = ab.row(y);
                                                   for (std::int32_t x = 0; x < width; ++x, p += 2) {
                                                       const float v = g[x] - fshift;
                                                       p[0] = v;
                                                       p[1] = v * v;
                                                   }
                                               });
            !status) {
            return status;
        }
        if (auto status = box_mean_impl(ab, radius, cancel, parallel); !status) {
            return status;
        }
        if (auto status = detail::for_each_row(height, parallel, cancel, kWhere,
                                               [&](std::int32_t y) {
                                                   float* p = ab.row(y);
                                                   for (std::int32_t x = 0; x < width; ++x, p += 2) {
                                                       const double mean_i = p[0];
                                                       const double var = static_cast<double>(p[1]) - mean_i * mean_i;
                                                       const double a = var / (var + eps);
                                                       p[0] = static_cast<float>(a);
                                                       p[1] = static_cast<float>(mean_i - a * mean_i);
                                                   }
                                               });
            !status) {
            return status;
        }
        if (auto status = box_mean_impl(ab, radius, cancel, parallel); !status) {
            return status;
        }
        return detail::for_each_row(height, parallel, cancel, kWhere, [&](std::int32_t y) {
            const float* g = guide.row(y);
            const float* p = ab.row(y);
            float* q = output.row(y);
            for (std::int32_t x = 0; x < width; ++x, p += 2) {
                const double v = static_cast<double>(g[x]) - shift;
                q[x] = static_cast<float>(static_cast<double>(p[0]) * v + static_cast<double>(p[1]) + shift);
            }
        });
    }

    // ---- General: guide I (one channel), input p (channels) ---------------------------------------
    auto mean_i = plane(width, height, 1, limits);
    if (!mean_i) {
        return Unexpected(std::move(mean_i.error()));
    }
    auto corr_i = plane(width, height, 1, limits);
    if (!corr_i) {
        return Unexpected(std::move(corr_i.error()));
    }
    auto mean_p = plane(width, height, channels, limits);
    if (!mean_p) {
        return Unexpected(std::move(mean_p.error()));
    }
    auto corr_ip = plane(width, height, channels, limits);
    if (!corr_ip) {
        return Unexpected(std::move(corr_ip.error()));
    }
    {
        const image::ImageView<float> mi = mean_i->view();
        const image::ImageView<float> ci = corr_i->view();
        const image::ImageView<float> mp = mean_p->view();
        const image::ImageView<float> cip = corr_ip->view();
        if (auto status = detail::for_each_row(height, parallel, cancel, kWhere,
                                               [&](std::int32_t y) {
                                                   const float* g = guide.row(y);
                                                   const float* p = input.row(y);
                                                   float* rmi = mi.row(y);
                                                   float* rci = ci.row(y);
                                                   float* rmp = mp.row(y);
                                                   float* rcip = cip.row(y);
                                                   for (std::int32_t x = 0; x < width; ++x) {
                                                       const float v = g[x] - fshift;
                                                       rmi[x] = v;
                                                       rci[x] = v * v;
                                                       for (std::int32_t c = 0; c < channels; ++c) {
                                                           const std::ptrdiff_t k =
                                                               static_cast<std::ptrdiff_t>(x) * channels + c;
                                                           rmp[k] = p[k];
                                                           rcip[k] = v * p[k];
                                                       }
                                                   }
                                               });
            !status) {
            return status;
        }
        for (image::ImageView<float> p : {mi, ci, mp, cip}) {
            if (auto status = box_mean_impl(p, radius, cancel, parallel); !status) {
                return status;
            }
        }
        // a -> corr_ip, b -> mean_p.
        if (auto status = detail::for_each_row(height, parallel, cancel, kWhere,
                                               [&](std::int32_t y) {
                                                   const float* rmi = mi.row(y);
                                                   const float* rci = ci.row(y);
                                                   float* rmp = mp.row(y);
                                                   float* rcip = cip.row(y);
                                                   for (std::int32_t x = 0; x < width; ++x) {
                                                       const double m = rmi[x];
                                                       const double var = static_cast<double>(rci[x]) - m * m;
                                                       for (std::int32_t c = 0; c < channels; ++c) {
                                                           const std::ptrdiff_t k =
                                                               static_cast<std::ptrdiff_t>(x) * channels + c;
                                                           const double cov = static_cast<double>(rcip[k]) -
                                                                              m * static_cast<double>(rmp[k]);
                                                           const double a = cov / (var + eps);
                                                           rcip[k] = static_cast<float>(a);
                                                           rmp[k] = static_cast<float>(static_cast<double>(rmp[k]) - a * m);
                                                       }
                                                   }
                                               });
            !status) {
            return status;
        }
    }
    *mean_i = {};
    *corr_i = {};
    const image::ImageView<float> a_plane = corr_ip->view();
    const image::ImageView<float> b_plane = mean_p->view();
    for (image::ImageView<float> p : {a_plane, b_plane}) {
        if (auto status = box_mean_impl(p, radius, cancel, parallel); !status) {
            return status;
        }
    }
    return detail::for_each_row(height, parallel, cancel, kWhere, [&](std::int32_t y) {
        const float* g = guide.row(y);
        const float* a = a_plane.row(y);
        const float* b = b_plane.row(y);
        float* q = output.row(y);
        for (std::int32_t x = 0; x < width; ++x) {
            const double v = static_cast<double>(g[x]) - shift;
            for (std::int32_t c = 0; c < channels; ++c) {
                const std::ptrdiff_t k = static_cast<std::ptrdiff_t>(x) * channels + c;
                q[k] = static_cast<float>(static_cast<double>(a[k]) * v + static_cast<double>(b[k]));
            }
        }
    });
}

}  // namespace lucent::engine
