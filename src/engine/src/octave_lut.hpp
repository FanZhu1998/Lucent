#pragma once

// Tabulated scalar functions for the per-pixel develop passes (private).
//
// OctaveLut tabulates f over positive floats [2^e_min, 2^(e_min + octaves)) with
// kOctaveLutCells equally spaced nodes per octave (the node spacing is relative: 1/512 of the
// octave's start). A lookup reads the float's exponent and leading mantissa bits directly (no
// logarithm) and interpolates linearly between the two neighbouring nodes, so a non-decreasing f
// stays non-decreasing. For a function with bounded second derivative the relative error is about
// (1/512)^2 / 8 = 4.8e-7 times |x^2 f''(x) / f(x)|.
//
// UniformLut tabulates f on [lo, hi] with equally spaced nodes and clamps outside.
//
// Both are immutable after construction and safe to read from many threads.

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <new>
#include <string_view>
#include <vector>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>

#include "parallel_rows.hpp"

namespace lucent::engine::detail {

inline constexpr std::int32_t kOctaveLutCellBits = 9;
inline constexpr std::int32_t kOctaveLutCells = 1 << kOctaveLutCellBits;

class OctaveLut {
public:
    // Tabulates `f` (must not throw) over the octaves [e_min, e_min + octaves). The nodes are
    // computed in parallel when requested (each node independently: schedule-independent).
    // OutOfMemory when the table cannot be allocated; Cancelled.
    [[nodiscard]] static Expected<OctaveLut> create(std::int32_t e_min, std::int32_t octaves,
                                                    const std::function<double(double)>& f, bool parallel,
                                                    const core::CancellationToken& cancel, std::string_view where) {
        OctaveLut lut;
        lut.e_min_ = e_min;
        lut.octaves_ = std::max(1, octaves);
        try {
            lut.nodes_.resize(static_cast<std::size_t>(lut.octaves_) * kOctaveLutCells + 1);
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "cannot allocate a lookup table", std::string(where));
        }
        auto status = for_each_row(lut.octaves_ + 1, parallel, cancel, where, [&](std::int32_t o) {
            const std::int32_t cells = o == lut.octaves_ ? 1 : kOctaveLutCells;
            for (std::int32_t j = 0; j < cells; ++j) {
                const double x = std::ldexp(1.0 + static_cast<double>(j) / kOctaveLutCells, e_min + o);
                lut.nodes_[static_cast<std::size_t>(o) * kOctaveLutCells + static_cast<std::size_t>(j)] = f(x);
            }
        });
        if (!status) {
            return Unexpected(std::move(status.error()));
        }
        return lut;
    }

    // Smallest and largest arguments covered: [lower(), upper()).
    [[nodiscard]] double lower() const noexcept { return std::ldexp(1.0, e_min_); }
    [[nodiscard]] double upper() const noexcept { return std::ldexp(1.0, e_min_ + octaves_); }

    // f(x) interpolated; x must be a normal float within [lower(), upper()).
    [[nodiscard]] double operator()(float x) const noexcept {
        const auto bits = std::bit_cast<std::uint32_t>(x);
        const auto exponent = static_cast<std::int32_t>((bits >> 23) & 0xFFu) - 127;
        const std::uint32_t mantissa = bits & 0x7FFFFFu;
        constexpr std::uint32_t kFractionBits = 23 - kOctaveLutCellBits;
        const auto cell = static_cast<std::size_t>(exponent - e_min_) * kOctaveLutCells +
                          static_cast<std::size_t>(mantissa >> kFractionBits);
        const double t = static_cast<double>(mantissa & ((1u << kFractionBits) - 1u)) /
                         static_cast<double>(1u << kFractionBits);
        return nodes_[cell] + t * (nodes_[cell + 1] - nodes_[cell]);
    }

private:
    std::int32_t e_min_ = 0;
    std::int32_t octaves_ = 1;
    std::vector<double> nodes_;
};

class UniformLut {
public:
    // Tabulates `f` (must not throw) at `count` >= 2 nodes over [lo, hi]. OutOfMemory.
    [[nodiscard]] static Expected<UniformLut> create(double lo, double hi, std::int32_t count,
                                                     const std::function<double(double)>& f, std::string_view where) {
        UniformLut lut;
        lut.lo_ = lo;
        lut.step_ = (hi - lo) / static_cast<double>(count - 1);
        lut.inv_step_ = 1.0 / lut.step_;
        try {
            lut.nodes_.resize(static_cast<std::size_t>(count));
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "cannot allocate a lookup table", std::string(where));
        }
        for (std::int32_t i = 0; i < count; ++i) {
            lut.nodes_[static_cast<std::size_t>(i)] = f(lo + lut.step_ * static_cast<double>(i));
        }
        return lut;
    }

    // f(x) interpolated; clamped to the end nodes outside [lo, hi].
    [[nodiscard]] double operator()(double x) const noexcept {
        const double u = (x - lo_) * inv_step_;
        if (!(u > 0.0)) {
            return nodes_.front();
        }
        const auto last = static_cast<double>(nodes_.size() - 1);
        if (!(u < last)) {
            return nodes_.back();
        }
        const auto i = static_cast<std::size_t>(u);
        const double t = u - static_cast<double>(i);
        return nodes_[i] + t * (nodes_[i + 1] - nodes_[i]);
    }

private:
    double lo_ = 0.0;
    double step_ = 1.0;
    double inv_step_ = 1.0;
    std::vector<double> nodes_;
};

}  // namespace lucent::engine::detail
