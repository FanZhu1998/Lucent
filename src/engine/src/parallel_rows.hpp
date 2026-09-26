#pragma once

// Row-parallel loop shared by the engine's per-pixel operations.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <execution>
#include <new>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>

namespace lucent::engine::detail {

// Calls body(row) for every row in [0, rows), with std::execution::par when `parallel` is set
// and there is more than one row, otherwise in order on the calling thread. `body` must not
// throw and must only write data owned by its row, so the result does not depend on the
// schedule. Cancellation is checked before each row; once it is observed the remaining rows are
// skipped and Cancelled (at `where`) is returned. OutOfMemory when the parallel algorithm cannot
// allocate its schedule.
template <class Body>
[[nodiscard]] Status for_each_row(std::int32_t rows, bool parallel, const core::CancellationToken& cancel,
                                  std::string_view where, const Body& body) {
    if (rows <= 0) {
        return ok();
    }
    std::atomic<bool> stopped{false};
    const auto run = [&](std::int32_t row) noexcept {
        if (stopped.load(std::memory_order_relaxed)) {
            return;
        }
        if (cancel.is_cancelled()) {
            stopped.store(true, std::memory_order_relaxed);
            return;
        }
        body(row);
    };
    try {
        if (parallel && rows > 1) {
            std::vector<std::int32_t> indices(static_cast<std::size_t>(rows));
            std::iota(indices.begin(), indices.end(), std::int32_t{0});
            std::for_each(std::execution::par, indices.begin(), indices.end(), run);
        } else {
            for (std::int32_t row = 0; row < rows; ++row) {
                run(row);
            }
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the parallel row schedule", std::string(where));
    }
    if (stopped.load()) {
        return cancel.check(where);  // cancellation is sticky, so this reports Cancelled
    }
    return ok();
}

}  // namespace lucent::engine::detail
