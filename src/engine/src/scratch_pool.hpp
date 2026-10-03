#pragma once

// A pool of equally sized float scratch buffers for the bodies of parallel row loops (private).
// The bodies of detail::for_each_row must not throw, so they acquire scratch here: acquire()
// returns nullptr instead of throwing when memory runs out, and the caller reports
// OutOfMemory after the loop. Buffers are created on demand (at most one per concurrently
// running body) and freed with the pool.

#include <cstddef>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace lucent::engine::detail {

class ScratchPool {
public:
    explicit ScratchPool(std::size_t floats_per_buffer) noexcept : size_(floats_per_buffer) {}
    ScratchPool(const ScratchPool&) = delete;
    ScratchPool& operator=(const ScratchPool&) = delete;

    // A buffer of floats_per_buffer floats (contents unspecified), or nullptr when it cannot be
    // allocated.
    [[nodiscard]] float* acquire() noexcept {
        const std::lock_guard lock(mutex_);
        if (!free_.empty()) {
            float* buffer = free_.back();
            free_.pop_back();
            return buffer;
        }
        try {
            all_.reserve(all_.size() + 1);
            free_.reserve(all_.size() + 1);
            all_.push_back(std::make_unique_for_overwrite<float[]>(size_ == 0 ? 1 : size_));
            return all_.back().get();
        } catch (const std::bad_alloc&) {
            return nullptr;
        }
    }

    // Returns a buffer obtained from acquire() (nullptr is ignored).
    void release(float* buffer) noexcept {
        if (buffer == nullptr) {
            return;
        }
        const std::lock_guard lock(mutex_);
        free_.push_back(buffer);  // capacity reserved by acquire(): never reallocates
    }

private:
    std::size_t size_;
    std::mutex mutex_;
    std::vector<std::unique_ptr<float[]>> all_;
    std::vector<float*> free_;
};

}  // namespace lucent::engine::detail
