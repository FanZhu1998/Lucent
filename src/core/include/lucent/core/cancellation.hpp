#pragma once

// Cooperative cancellation (blueprint section 3, adapter boundary; section 7, job lifecycle).
//
// A CancellationSource owns a cancellation flag; CancellationTokens observe it. Long-running
// operations (hashing, decoding, rendering) take a token by value and call check() at safe
// boundaries such as between chunks, rows or tiles; they return ErrorCode::Cancelled promptly
// once cancellation is requested and never leave partially published results behind.
//
// Cancellation is one-way and sticky: once cancel() has been called the flag stays set for
// every token of that source.
//
// Memory ordering: cancel() stores the flag with release semantics and is_cancelled() loads it
// with acquire semantics. Everything the cancelling thread wrote before cancel() (for example a
// reason recorded by the application) is therefore visible to a worker that observes
// is_cancelled() == true. Observing false guarantees nothing about ordering, and a request that
// races with a check may be seen at the next check.
//
// Thread-safety: all members of both classes are safe to call concurrently from any number of
// threads. The flag is a lock-free std::atomic<bool>; copies of sources and tokens share it
// through a reference-counted pointer, so a token stays valid after its source is destroyed.

#include <atomic>
#include <memory>
#include <string_view>
#include <utility>

#include "lucent/core/error.hpp"

namespace lucent::core {

class CancellationSource;

// Cheap, copyable observer of a CancellationSource. A default-constructed token is never
// cancelled; pass CancellationToken{} for operations that cannot be cancelled.
class CancellationToken {
public:
    CancellationToken() noexcept = default;

    // True once the associated source has been cancelled. Always false for a default token.
    [[nodiscard]] bool is_cancelled() const noexcept;

    // True when this token is connected to a source, i.e. cancellation is possible at all.
    [[nodiscard]] bool can_be_cancelled() const noexcept { return state_ != nullptr; }

    // ok() when not cancelled; otherwise ErrorCode::Cancelled with `where` as the location.
    [[nodiscard]] Status check(std::string_view where) const;

private:
    friend class CancellationSource;
    explicit CancellationToken(std::shared_ptr<const std::atomic<bool>> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<const std::atomic<bool>> state_;
};

// Owner of a cancellation flag. Copies share the same flag (like std::stop_source).
class CancellationSource {
public:
    // Allocates the shared flag; throws std::bad_alloc only if that small allocation fails.
    CancellationSource();

    // Copies share the flag. No move operations are declared, so moving copies too: a source
    // is never left without a flag.
    CancellationSource(const CancellationSource&) = default;
    CancellationSource& operator=(const CancellationSource&) = default;
    ~CancellationSource() = default;

    // Requests cancellation. Idempotent; never blocks.
    void cancel() noexcept;

    [[nodiscard]] bool is_cancelled() const noexcept;

    // A token observing this source.
    [[nodiscard]] CancellationToken token() const noexcept { return CancellationToken(state_); }

private:
    static_assert(std::atomic<bool>::is_always_lock_free);
    std::shared_ptr<std::atomic<bool>> state_;
};

}  // namespace lucent::core
