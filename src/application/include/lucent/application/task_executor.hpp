#pragma once

// Background work for the application services (blueprint section 4: "Keep decoding,
// optimization, and encoding off the GUI thread ... Give interactive jobs reserved capacity so a
// background batch cannot make detailed editing unusable"; section 7, "Memory and scheduling":
// "Use an aggregate memory budget, not one worker per CPU core ... Start with one
// full-resolution worker").
//
// TaskExecutor: a fixed pool of worker threads fed by three lanes, taken in priority order.
//   * Interactive tasks (previews the user is waiting for) are taken first and may run on every
//     worker.
//   * Quick tasks (short, latency-sensitive reads for browsing, such as camera previews) are
//     taken next. Quick and background tasks together occupy at most
//     workers - reserved_interactive threads.
//   * Background tasks (imports, exports: long work) are taken last and occupy at most
//     workers - reserved_interactive - reserved_quick threads.
//   So `reserved_interactive` threads are always free for interactive work, and `reserved_quick`
//   more are free for quick work, however much background work is queued, running or waiting
//   (for memory admission, on a slow disk): a batch cannot make editing or browsing unusable
//   (blueprint section 4). Within a lane, tasks start in submission order.
//   * Every accepted task runs exactly once. Each has its own CancellationSource; its token
//     (TaskContext::cancel) is cancelled by TaskHandle::cancel() and by shutdown(). A task that
//     is cancelled before it starts still runs, with its token already cancelled, so that it can
//     report the cancellation and release what it owns; tasks must check the token promptly.
//   * Tasks must not throw (an escaping exception is caught and logged as an error,
//     "app.executor") and must not block waiting for other tasks of the same executor.
//
// MemoryAdmission: the memory-admission hook. A task that is about to allocate a full-resolution
// render asks for its estimated bytes (for example engine::estimate_render_memory's peak) with
// TaskContext::admit() and holds the returned lease while the large buffers live. At most
// `max_concurrent` leases are active at once (default 1: one full-resolution render at a time on
// the 12 GB reference machine, blueprint section 7) and their bytes never exceed `budget_bytes`.
// Waiting interactive claims are granted before waiting claims of the other lanes (quick and
// background claims form one class); within a class, first come first served. The engine's estimate covers its own pixel buffers; callers add what they
// allocate on top (for example the quantised output image).
//
// Shutdown: shutdown() (also run by the destructor) stops accepting tasks, cancels every queued
// and running task's token and every admission wait, runs the remaining queued tasks (with
// cancelled tokens), and joins the workers. It must not be called from a task of the same
// executor.
//
// Thread-safety: all members are safe to call concurrently (except as stated for shutdown).

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/render.hpp>

namespace lucent::app {

enum class Lane : std::uint8_t {
    Interactive = 1,
    Background = 2,
    Quick = 3,
};

[[nodiscard]] std::string_view to_string(Lane lane) noexcept;  // "interactive", "background", "quick"

// ---------------------------------------------------------------------------------------------
// Memory admission
// ---------------------------------------------------------------------------------------------

struct AdmissionOptions {
    std::uint32_t max_concurrent = 1;                                 // leases active at once, >= 1
    std::uint64_t budget_bytes = engine::kDefaultRenderMemoryBudget;  // sum of active claims, >= 1

    // InvalidArgument ("app.admission") unless both are at least 1.
    [[nodiscard]] Status validate() const;
};

struct AdmissionStats {
    std::uint32_t active = 0;
    std::uint64_t active_bytes = 0;
    std::uint32_t waiting_interactive = 0;
    std::uint32_t waiting_background = 0;  // quick and background claims
    std::uint64_t granted = 0;  // leases granted since construction
};

// A granted claim. Move-only; release() or destruction returns it. Safe to release on any thread
// and after the MemoryAdmission object is gone.
class AdmissionLease {
public:
    AdmissionLease() noexcept = default;
    ~AdmissionLease() { release(); }
    AdmissionLease(AdmissionLease&& other) noexcept;
    AdmissionLease& operator=(AdmissionLease&& other) noexcept;
    AdmissionLease(const AdmissionLease&) = delete;
    AdmissionLease& operator=(const AdmissionLease&) = delete;

    [[nodiscard]] bool held() const noexcept { return state_ != nullptr; }
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
    void release() noexcept;

    struct State;  // implementation detail (shared with MemoryAdmission)

private:
    friend class MemoryAdmission;
    AdmissionLease(std::shared_ptr<State> state, std::uint64_t bytes) noexcept
        : state_(std::move(state)), bytes_(bytes) {}

    std::shared_ptr<State> state_;
    std::uint64_t bytes_ = 0;
};

class MemoryAdmission {
public:
    // Options must be valid (AdmissionOptions::validate); invalid values are raised to 1.
    explicit MemoryAdmission(const AdmissionOptions& options = {});
    // shutdown(): claims still waiting on other threads fail with Cancelled.
    ~MemoryAdmission();
    MemoryAdmission(const MemoryAdmission&) = delete;
    MemoryAdmission& operator=(const MemoryAdmission&) = delete;

    // Blocks until the claim fits (see the file comment) and returns its lease. A claim of 0
    // bytes still takes one of the max_concurrent places. Errors ("app.admission"): LimitExceeded
    // when `bytes` alone exceeds the budget (it could never be admitted: an insufficient-memory
    // result, blueprint section 7); Cancelled when `cancel` is cancelled (observed within about
    // 10 ms) or shutdown() is called while waiting or before.
    [[nodiscard]] Expected<AdmissionLease> acquire(std::uint64_t bytes, Lane lane,
                                                   const core::CancellationToken& cancel);

    // Fails every current and future acquire() with Cancelled. Held leases stay valid.
    void shutdown() noexcept;

    [[nodiscard]] AdmissionStats stats() const;
    [[nodiscard]] const AdmissionOptions& options() const noexcept { return options_; }

private:
    AdmissionOptions options_;
    std::shared_ptr<AdmissionLease::State> state_;
};

// ---------------------------------------------------------------------------------------------
// Executor
// ---------------------------------------------------------------------------------------------

struct ExecutorOptions {
    std::uint32_t workers = 4;               // threads, 2..64
    std::uint32_t reserved_interactive = 1;  // threads only interactive work may occupy, >= 1
    std::uint32_t reserved_quick = 1;        // further threads background work never occupies, >= 0
    AdmissionOptions admission;

    // InvalidArgument ("app.executor") for values outside the documented ranges or when the
    // reservations leave background work no thread (reserved_interactive + reserved_quick must
    // be at most workers - 1).
    [[nodiscard]] Status validate() const;

    // Defaults for this machine (the member defaults): four workers, one reserved for interactive
    // work and one more for quick work, so the Background lane has two threads (an import can run
    // beside an export) and browsing always has one; one full-resolution render at a time within
    // engine::kDefaultRenderMemoryBudget. The worker count does not follow the core count: most
    // tasks wait (on the disk, on admission), CPU-heavy work is bounded by memory admission, and
    // renders parallelise internally.
    [[nodiscard]] static ExecutorOptions for_this_machine() noexcept;
};

class TaskContext {
public:
    TaskContext(Lane lane, core::CancellationToken cancel, MemoryAdmission& admission) noexcept
        : lane_(lane), cancel_(std::move(cancel)), admission_(&admission) {}

    [[nodiscard]] Lane lane() const noexcept { return lane_; }
    [[nodiscard]] const core::CancellationToken& cancel() const noexcept { return cancel_; }
    // MemoryAdmission::acquire(bytes, lane(), cancel()).
    [[nodiscard]] Expected<AdmissionLease> admit(std::uint64_t bytes) const {
        return admission_->acquire(bytes, lane_, cancel_);
    }
    [[nodiscard]] MemoryAdmission& admission() const noexcept { return *admission_; }

private:
    Lane lane_;
    core::CancellationToken cancel_;
    MemoryAdmission* admission_;
};

using TaskFunction = std::function<void(TaskContext&)>;

// Observer of one submitted task. Copyable; safe to use after the executor is gone.
class TaskHandle {
public:
    TaskHandle() noexcept = default;

    [[nodiscard]] std::uint64_t id() const noexcept;  // 1, 2, 3, ... per executor; 0 when empty
    [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }
    // Requests cancellation (idempotent): the task's token becomes cancelled.
    void cancel() const noexcept;
    // True once the task function has returned.
    [[nodiscard]] bool finished() const noexcept;
    // Blocks until the task function has returned. Must not be called from a task of the same
    // executor (it could wait for a task queued behind the caller).
    void wait() const;

    struct State;  // implementation detail

private:
    friend class TaskExecutor;
    explicit TaskHandle(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}
    std::shared_ptr<State> state_;
};

struct ExecutorStats {
    std::uint32_t workers = 0;
    std::uint32_t queued_interactive = 0;
    std::uint32_t queued_quick = 0;
    std::uint32_t queued_background = 0;
    std::uint32_t running_interactive = 0;
    std::uint32_t running_quick = 0;
    std::uint32_t running_background = 0;
    std::uint64_t completed = 0;
    AdmissionStats admission;
};

class TaskExecutor {
public:
    // Starts the workers. InvalidArgument for invalid options; OutOfMemory / Internal when a
    // thread cannot be started.
    [[nodiscard]] static Expected<std::unique_ptr<TaskExecutor>> create(const ExecutorOptions& options);

    ~TaskExecutor();
    TaskExecutor(const TaskExecutor&) = delete;
    TaskExecutor& operator=(const TaskExecutor&) = delete;

    // Queues `task` on `lane`. Errors ("app.executor"): InvalidArgument for an empty function;
    // Cancelled after shutdown() has begun.
    [[nodiscard]] Expected<TaskHandle> submit(Lane lane, TaskFunction task);

    // See the file comment. Idempotent; concurrent calls all return after the workers are joined.
    void shutdown() noexcept;

    [[nodiscard]] ExecutorStats stats() const;
    [[nodiscard]] MemoryAdmission& admission() noexcept;
    [[nodiscard]] const ExecutorOptions& options() const noexcept;

    struct Impl;  // implementation detail

private:
    explicit TaskExecutor(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lucent::app
