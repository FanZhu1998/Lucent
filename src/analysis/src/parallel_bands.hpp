#pragma once

// Deterministic band-parallel loops for the analysis module. Rows are split into bands of a fixed
// number of rows (independent of the thread count); a band's body writes only its own outputs and
// reductions keep one partial result per band, merged in band order by the caller, so every result
// is independent of the schedule (conventions.md, "Determinism").

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <execution>
#include <new>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>

namespace lucent::analysis::detail {

inline constexpr std::int32_t kBandRows = 16;

[[nodiscard]] inline std::int32_t band_count(std::int32_t rows, std::int32_t band_rows = kBandRows) noexcept {
    return rows <= 0 ? 0 : (rows + band_rows - 1) / band_rows;
}

// Calls body(band, row_begin, row_end) for every band of `band_rows` rows of [0, rows), in parallel
// when `parallel` is set. `body` must not throw. Cancellation is checked before each band; once
// observed the remaining bands are skipped and Cancelled (at `where`) is returned. OutOfMemory when
// the schedule cannot be allocated.
template <class Body>
[[nodiscard]] Status for_each_band(std::int32_t rows, bool parallel, const core::CancellationToken& cancel,
                                   std::string_view where, const Body& body,
                                   std::int32_t band_rows = kBandRows) {
    const std::int32_t bands = band_count(rows, band_rows);
    if (bands == 0) {
        return ok();
    }
    std::atomic<bool> stopped{false};
    const auto run = [&](std::int32_t band) noexcept {
        if (stopped.load(std::memory_order_relaxed)) {
            return;
        }
        if (cancel.is_cancelled()) {
            stopped.store(true, std::memory_order_relaxed);
            return;
        }
        const std::int32_t begin = band * band_rows;
        const std::int32_t end = std::min(rows, begin + band_rows);
        body(band, begin, end);
    };
    try {
        if (parallel && bands > 1) {
            std::vector<std::int32_t> indices(static_cast<std::size_t>(bands));
            std::iota(indices.begin(), indices.end(), std::int32_t{0});
            std::for_each(std::execution::par, indices.begin(), indices.end(), run);
        } else {
            for (std::int32_t band = 0; band < bands; ++band) {
                run(band);
            }
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the parallel band schedule", std::string(where));
    }
    if (stopped.load()) {
        return cancel.check(where);
    }
    return ok();
}

}  // namespace lucent::analysis::detail
