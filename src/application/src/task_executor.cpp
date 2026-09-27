#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <format>
#include <iterator>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <lucent/application/task_executor.hpp>
#include <lucent/core/log.hpp>

namespace lucent::app {

namespace {

constexpr std::string_view kWhereAdmission = "app.admission";
constexpr std::string_view kWhereExecutor = "app.executor";
// How often a waiting claim re-checks its cancellation token (tokens cannot notify).
constexpr auto kAdmissionPoll = std::chrono::milliseconds(10);
constexpr std::uint32_t kMaxWorkers = 64;
constexpr std::size_t kLaneCount = 3;

// Executor queues in priority order: interactive, quick, background.
std::size_t lane_index(Lane lane) noexcept {
    switch (lane) {
        case Lane::Interactive:
            return 0;
        case Lane::Quick:
            return 1;
        case Lane::Background:
            break;
    }
    return 2;
}

// Admission classes: interactive claims first, then every other lane's.
std::size_t admission_class(Lane lane) noexcept {
    return lane == Lane::Interactive ? 0 : 1;
}

bool is_known(Lane lane) noexcept {
    return lane == Lane::Interactive || lane == Lane::Quick || lane == Lane::Background;
}

}  // namespace

std::string_view to_string(Lane lane) noexcept {
    switch (lane) {
        case Lane::Interactive:
            return "interactive";
        case Lane::Background:
            return "background";
        case Lane::Quick:
            return "quick";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------------------------
// Memory admission
// ---------------------------------------------------------------------------------------------

struct AdmissionLease::State {
    std::mutex mutex;
    std::condition_variable changed;
    AdmissionOptions options;
    std::uint32_t active = 0;
    std::uint64_t active_bytes = 0;
    std::uint64_t next_ticket = 1;
    std::deque<std::uint64_t> waiting[2];  // tickets per admission class (interactive, others), FIFO
    bool shut_down = false;
    std::uint64_t granted = 0;

    void give_back(std::uint64_t bytes) noexcept {
        {
            std::lock_guard lock(mutex);
            --active;
            active_bytes -= bytes;
        }
        changed.notify_all();
    }
};

AdmissionLease::AdmissionLease(AdmissionLease&& other) noexcept
    : state_(std::move(other.state_)), bytes_(std::exchange(other.bytes_, 0)) {}

AdmissionLease& AdmissionLease::operator=(AdmissionLease&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        bytes_ = std::exchange(other.bytes_, 0);
    }
    return *this;
}

void AdmissionLease::release() noexcept {
    if (auto state = std::move(state_)) {
        state->give_back(bytes_);
        bytes_ = 0;
    }
}

Status AdmissionOptions::validate() const {
    if (max_concurrent == 0 || budget_bytes == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "admission max_concurrent and budget_bytes must be at least 1",
                    std::string(kWhereAdmission));
    }
    return ok();
}

MemoryAdmission::MemoryAdmission(const AdmissionOptions& options)
    : options_(options), state_(std::make_shared<AdmissionLease::State>()) {
    options_.max_concurrent = std::max<std::uint32_t>(1, options_.max_concurrent);
    options_.budget_bytes = std::max<std::uint64_t>(1, options_.budget_bytes);
    state_->options = options_;
}

MemoryAdmission::~MemoryAdmission() {
    shutdown();
}

Expected<AdmissionLease> MemoryAdmission::acquire(std::uint64_t bytes, Lane lane,
                                                  const core::CancellationToken& cancel) {
    if (bytes > options_.budget_bytes) {
        return fail(ErrorCode::LimitExceeded,
                    std::format("the task needs about {} bytes; the admission budget is {}", bytes,
                                options_.budget_bytes),
                    std::string(kWhereAdmission));
    }
    // Waiters hold their own reference to the shared state, so destroying the MemoryAdmission
    // (which shuts it down first) while a claim is waiting is safe: the waiter fails with Cancelled.
    const std::shared_ptr<AdmissionLease::State> state = state_;
    const AdmissionOptions options = options_;
    AdmissionLease::State& s = *state;
    std::unique_lock lock(s.mutex);
    if (s.shut_down) {
        return fail(ErrorCode::Cancelled, "memory admission has been shut down",
                    std::string(kWhereAdmission));
    }
    const std::size_t own = admission_class(lane);
    const std::uint64_t ticket = s.next_ticket++;
    try {
        s.waiting[own].push_back(ticket);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot queue the memory claim", std::string(kWhereAdmission));
    }
    for (;;) {
        if (s.shut_down || cancel.is_cancelled()) {
            const bool shut_down = s.shut_down;
            std::erase(s.waiting[own], ticket);
            lock.unlock();
            s.changed.notify_all();  // the next waiter may now be at the head
            return fail(ErrorCode::Cancelled,
                        shut_down ? "memory admission was shut down while waiting"
                                  : "waiting for memory was cancelled",
                        std::string(kWhereAdmission));
        }
        const bool at_head = s.waiting[own].front() == ticket;
        const bool lane_may_go = own == 0 || s.waiting[0].empty();
        const bool fits = s.active < options.max_concurrent && s.active_bytes + bytes <= options.budget_bytes;
        if (at_head && lane_may_go && fits) {
            s.waiting[own].pop_front();
            ++s.active;
            s.active_bytes += bytes;
            ++s.granted;
            lock.unlock();
            s.changed.notify_all();
            return AdmissionLease(state, bytes);
        }
        s.changed.wait_for(lock, kAdmissionPoll);
    }
}

void MemoryAdmission::shutdown() noexcept {
    {
        std::lock_guard lock(state_->mutex);
        state_->shut_down = true;
    }
    state_->changed.notify_all();
}

AdmissionStats MemoryAdmission::stats() const {
    std::lock_guard lock(state_->mutex);
    AdmissionStats stats;
    stats.active = state_->active;
    stats.active_bytes = state_->active_bytes;
    stats.waiting_interactive = static_cast<std::uint32_t>(state_->waiting[0].size());
    stats.waiting_background = static_cast<std::uint32_t>(state_->waiting[1].size());
    stats.granted = state_->granted;
    return stats;
}

// ---------------------------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------------------------

struct TaskHandle::State {
    std::uint64_t id = 0;
    Lane lane = Lane::Background;
    TaskFunction function;
    core::CancellationSource source;
    std::mutex mutex;
    std::condition_variable done;
    std::atomic<bool> finished{false};
};

std::uint64_t TaskHandle::id() const noexcept {
    return state_ ? state_->id : 0;
}

void TaskHandle::cancel() const noexcept {
    if (state_) {
        state_->source.cancel();
    }
}

bool TaskHandle::finished() const noexcept {
    return state_ && state_->finished.load(std::memory_order_acquire);
}

void TaskHandle::wait() const {
    if (!state_) {
        return;
    }
    std::unique_lock lock(state_->mutex);
    state_->done.wait(lock, [&] { return state_->finished.load(std::memory_order_acquire); });
}

// ---------------------------------------------------------------------------------------------
// Executor
// ---------------------------------------------------------------------------------------------

Status ExecutorOptions::validate() const {
    if (workers < 2 || workers > kMaxWorkers) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("workers must be 2..{}, not {}", kMaxWorkers, workers),
                    std::string(kWhereExecutor));
    }
    if (reserved_interactive < 1 || reserved_interactive >= workers) {
        return fail(
            ErrorCode::InvalidArgument,
            std::format("reserved_interactive must be 1..{}, not {}", workers - 1, reserved_interactive),
            std::string(kWhereExecutor));
    }
    if (reserved_quick > workers - 1 - reserved_interactive) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("reserved_quick must be 0..{} with {} workers and {} reserved for interactive "
                                "work, not {}",
                                workers - 1 - reserved_interactive, workers, reserved_interactive,
                                reserved_quick),
                    std::string(kWhereExecutor));
    }
    return admission.validate();
}

ExecutorOptions ExecutorOptions::for_this_machine() noexcept {
    return ExecutorOptions{};  // see the header: the defaults do not depend on the core count
}

struct TaskExecutor::Impl {
    explicit Impl(const ExecutorOptions& o) : options(o), admission(o.admission) {}

    ExecutorOptions options;
    MemoryAdmission admission;

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::shared_ptr<TaskHandle::State>> queues[kLaneCount];  // see lane_index
    std::vector<std::shared_ptr<TaskHandle::State>> running;
    std::uint32_t running_count[kLaneCount] = {0, 0, 0};
    std::uint64_t completed = 0;
    std::uint64_t next_id = 1;
    bool stopping = false;

    std::mutex join_mutex;
    bool joined = false;
    std::vector<std::thread> threads;

    // The queue a free worker takes its next task from, or kLaneCount when none may start (see
    // the header for the limits; while stopping, every queued task runs). Requires `mutex`.
    [[nodiscard]] std::size_t next_lane() const noexcept {
        if (!queues[0].empty()) {
            return 0;
        }
        const std::uint32_t others = running_count[1] + running_count[2];
        const bool below_shared_limit = others < options.workers - options.reserved_interactive;
        if (!queues[1].empty() && (stopping || below_shared_limit)) {
            return 1;
        }
        const std::uint32_t background_limit =
            options.workers - options.reserved_interactive - options.reserved_quick;
        if (!queues[2].empty() && (stopping || (below_shared_limit && running_count[2] < background_limit))) {
            return 2;
        }
        return kLaneCount;
    }

    [[nodiscard]] bool all_queues_empty() const noexcept {
        return std::all_of(std::begin(queues), std::end(queues), [](const auto& q) { return q.empty(); });
    }

    void worker() {
        for (;;) {
            std::shared_ptr<TaskHandle::State> task;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return next_lane() < kLaneCount || (stopping && all_queues_empty()); });
                const std::size_t lane = next_lane();
                if (lane == kLaneCount) {
                    return;  // stopping and nothing left
                }
                task = std::move(queues[lane].front());
                queues[lane].pop_front();
                ++running_count[lane];
                running.push_back(task);
            }
            {
                TaskContext context(task->lane, task->source.token(), admission);
                try {
                    task->function(context);
                } catch (const std::exception& e) {
                    core::log_error(kWhereExecutor, "task {} threw: {}", task->id, e.what());
                } catch (...) {
                    core::log_error(kWhereExecutor, "task {} threw an unknown exception", task->id);
                }
                task->function = nullptr;  // release the task's captures before reporting completion
            }
            {
                std::lock_guard lock(mutex);
                --running_count[lane_index(task->lane)];
                ++completed;
                std::erase(running, task);
            }
            {
                std::lock_guard lock(task->mutex);
                task->finished.store(true, std::memory_order_release);
            }
            task->done.notify_all();
            wake.notify_all();  // a quick or background slot may have been freed
        }
    }
};

TaskExecutor::TaskExecutor(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Expected<std::unique_ptr<TaskExecutor>> TaskExecutor::create(const ExecutorOptions& options) {
    if (auto valid = options.validate(); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    std::unique_ptr<TaskExecutor> executor;
    try {
        executor.reset(new TaskExecutor(std::make_unique<Impl>(options)));
        Impl& impl = *executor->impl_;
        impl.threads.reserve(options.workers);
        for (std::uint32_t i = 0; i < options.workers; ++i) {
            impl.threads.emplace_back([&impl] { impl.worker(); });
        }
    } catch (const std::bad_alloc&) {
        if (executor) {
            executor->shutdown();
        }
        return fail(ErrorCode::OutOfMemory, "cannot start the executor", std::string(kWhereExecutor));
    } catch (const std::system_error& e) {
        if (executor) {
            executor->shutdown();
        }
        return fail(ErrorCode::Internal, std::format("cannot start a worker thread: {}", e.what()),
                    std::string(kWhereExecutor));
    }
    return executor;
}

TaskExecutor::~TaskExecutor() {
    shutdown();
}

Expected<TaskHandle> TaskExecutor::submit(Lane lane, TaskFunction task) {
    if (!task) {
        return fail(ErrorCode::InvalidArgument, "the task function is empty", std::string(kWhereExecutor));
    }
    if (!is_known(lane)) {
        return fail(ErrorCode::InvalidArgument, "unknown lane", std::string(kWhereExecutor));
    }
    try {
        auto state = std::make_shared<TaskHandle::State>();
        state->lane = lane;
        state->function = std::move(task);
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->stopping) {
                return fail(ErrorCode::Cancelled, "the executor has been shut down",
                            std::string(kWhereExecutor));
            }
            state->id = impl_->next_id++;
            impl_->queues[lane_index(lane)].push_back(state);
        }
        impl_->wake.notify_all();
        return TaskHandle(std::move(state));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot queue the task", std::string(kWhereExecutor));
    }
}

void TaskExecutor::shutdown() noexcept {
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->stopping) {
            impl_->stopping = true;
            for (const auto& queue : impl_->queues) {
                for (const auto& task : queue) {
                    task->source.cancel();
                }
            }
            for (const auto& task : impl_->running) {
                task->source.cancel();
            }
        }
    }
    impl_->admission.shutdown();
    impl_->wake.notify_all();

    std::lock_guard join_lock(impl_->join_mutex);
    if (impl_->joined) {
        return;
    }
    for (std::thread& thread : impl_->threads) {
        if (!thread.joinable()) {
            continue;
        }
        if (thread.get_id() == std::this_thread::get_id()) {
            core::log_error(kWhereExecutor, "shutdown() was called from a task; that worker is detached");
            thread.detach();
            continue;
        }
        thread.join();
    }
    impl_->joined = true;
}

ExecutorStats TaskExecutor::stats() const {
    ExecutorStats stats;
    {
        std::lock_guard lock(impl_->mutex);
        stats.workers = impl_->options.workers;
        stats.queued_interactive = static_cast<std::uint32_t>(impl_->queues[0].size());
        stats.queued_quick = static_cast<std::uint32_t>(impl_->queues[1].size());
        stats.queued_background = static_cast<std::uint32_t>(impl_->queues[2].size());
        stats.running_interactive = impl_->running_count[0];
        stats.running_quick = impl_->running_count[1];
        stats.running_background = impl_->running_count[2];
        stats.completed = impl_->completed;
    }
    stats.admission = impl_->admission.stats();
    return stats;
}

MemoryAdmission& TaskExecutor::admission() noexcept {
    return impl_->admission;
}

const ExecutorOptions& TaskExecutor::options() const noexcept {
    return impl_->options;
}

}  // namespace lucent::app
