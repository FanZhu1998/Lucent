#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

#include <lucent/application/maintenance_service.hpp>
#include <lucent/core/log.hpp>

namespace lucent::app {

namespace {

constexpr std::string_view kWhere = "app.maintenance";

Error cancelled_error() {
    return Error{ErrorCode::Cancelled, "the maintenance request was cancelled", std::string(kWhere), 0};
}

template <class Callback, class Value>
void call_safely(const Callback& callback, const Value& value) {
    if (!callback) {
        return;
    }
    try {
        callback(value);
    } catch (const std::exception& e) {
        core::log_error(kWhere, "a maintenance callback threw: {}", e.what());
    } catch (...) {
        core::log_error(kWhere, "a maintenance callback threw an unknown exception");
    }
}

}  // namespace

struct MaintenanceService::State : std::enable_shared_from_this<State> {
    State(Library& l, const raw::DecoderRegistry& r, TaskExecutor& e, MaintenanceServiceOptions o)
        : library(l), registry(r), executor(e), options(std::move(o)) {}

    Library& library;
    const raw::DecoderRegistry& registry;
    TaskExecutor& executor;
    const MaintenanceServiceOptions options;

    mutable std::mutex mutex;
    std::condition_variable idle;
    bool shut_down = false;
    bool running = false;  // a step is submitted and has not finished
    TaskHandle running_task;

    std::deque<std::pair<RelinkRequest, RelinkCallback>> relinks;
    std::vector<RefreshCallback> checks;  // check_all() requests waiting for the next check step
    bool check_requested = false;
    std::vector<ItemId> rereads;
    std::deque<ItemId> hash_queue;
    std::unordered_set<std::uint64_t> hash_queued;

    // Totals of the hashing run since the last Idle.
    std::uint64_t run_items_total = 0;
    std::uint64_t run_items_done = 0;
    std::uint64_t run_hashed = 0;
    std::uint64_t run_changed = 0;
    std::uint64_t run_missing = 0;
    std::uint64_t run_failed = 0;

    std::chrono::steady_clock::time_point last_progress{};

    // ---- Queueing (require `mutex`) ---------------------------------------------------------

    bool has_work_locked() const {
        return !relinks.empty() || check_requested || !rereads.empty() || !hash_queue.empty();
    }

    void enqueue_hash_locked(const std::vector<ItemId>& items) {
        for (const ItemId id : items) {
            if (hash_queued.insert(id.value).second) {
                hash_queue.push_back(id);
                ++run_items_total;
            }
        }
    }

    // Submits the next step when none is running. Returns callbacks to fail when the executor
    // refused the step (it is shutting down).
    void schedule_locked(std::vector<RelinkCallback>& failed_relinks,
                         std::vector<RefreshCallback>& failed_checks) {
        if (running || shut_down || !has_work_locked()) {
            return;
        }
        auto submitted = executor.submit(
            Lane::Background, [self = shared_from_this()](TaskContext& context) { self->step(context); });
        if (!submitted) {
            drop_queued_locked(failed_relinks, failed_checks);
            return;
        }
        running = true;
        running_task = *submitted;
    }

    void drop_queued_locked(std::vector<RelinkCallback>& failed_relinks,
                            std::vector<RefreshCallback>& failed_checks) {
        for (auto& [request, callback] : relinks) {
            failed_relinks.push_back(std::move(callback));
        }
        relinks.clear();
        for (auto& callback : checks) {
            failed_checks.push_back(std::move(callback));
        }
        checks.clear();
        check_requested = false;
        rereads.clear();
        hash_queue.clear();
        hash_queued.clear();
        reset_run_locked();
    }

    void reset_run_locked() {
        run_items_total = 0;
        run_items_done = 0;
        run_hashed = 0;
        run_changed = 0;
        run_missing = 0;
        run_failed = 0;
    }

    static void fail_callbacks(const std::vector<RelinkCallback>& relink_callbacks,
                               const std::vector<RefreshCallback>& check_callbacks) {
        const Expected<RelinkReport> relink_result = Unexpected(cancelled_error());
        for (const RelinkCallback& callback : relink_callbacks) {
            call_safely(callback, relink_result);
        }
        const Expected<RefreshReport> check_result = Unexpected(cancelled_error());
        for (const RefreshCallback& callback : check_callbacks) {
            call_safely(callback, check_result);
        }
    }

    // ---- Events ---------------------------------------------------------------------------------

    void emit(const MaintenanceEvent& event) const { call_safely(options.observer, event); }

    MaintenanceProgressCallback progress_callback(std::uint64_t items_done_before) {
        if (!options.observer) {
            return {};
        }
        return [this, items_done_before](const MaintenanceProgress& progress) {
            const auto now = std::chrono::steady_clock::now();
            const bool file_finished =
                progress.current.empty() || progress.files_done == progress.files_total;
            if (!file_finished && now - last_progress < options.progress_interval) {
                return;
            }
            last_progress = now;
            MaintenanceEvent event;
            event.kind = MaintenanceEvent::Kind::Progress;
            event.progress = progress;
            {
                std::lock_guard lock(mutex);
                event.hash_items_total = run_items_total;
                event.hash_items_done = progress.phase == MaintenancePhase::Hashing
                                            ? items_done_before + progress.items_done
                                            : run_items_done;
            }
            emit(event);
        };
    }

    // ---- One step -----------------------------------------------------------------------------

    void step(TaskContext& context) {
        const core::CancellationToken& cancel = context.cancel();
        std::optional<std::pair<RelinkRequest, RelinkCallback>> relink;
        std::vector<RefreshCallback> check_callbacks;
        bool check = false;
        std::vector<ItemId> reread;
        std::vector<ItemId> batch;
        std::uint64_t items_done_before = 0;
        {
            std::lock_guard lock(mutex);
            if (!relinks.empty()) {
                relink = std::move(relinks.front());
                relinks.pop_front();
            } else if (check_requested) {
                check = true;
                check_requested = false;
                check_callbacks = std::move(checks);
                checks.clear();
            } else if (!rereads.empty()) {
                reread = std::move(rereads);
                rereads.clear();
            } else {
                batch = take_hash_batch_locked();
                items_done_before = run_items_done;
            }
        }

        std::vector<ItemId> to_hash;
        std::vector<ItemId> to_reread;
        bool delivered = false;  // the step's callbacks have been called
        try {
            if (relink) {
                auto report = relink_sources(library, relink->first, progress_callback(0), cancel);
                if (report) {
                    to_hash = report->relinked_items;  // a file relinked by its identity may be unhashed
                }
                delivered = true;
                call_safely(relink->second, report);
            } else if (check) {
                auto report =
                    refresh_sources(library, registry, {}, options.refresh, progress_callback(0), cancel);
                if (report) {
                    to_hash = report->unhashed_items;
                    std::lock_guard lock(mutex);
                    run_missing += report->missing;
                    run_changed += report->changed;
                }
                delivered = true;
                for (const RefreshCallback& callback : check_callbacks) {
                    call_safely(callback, report);
                }
            } else if (!reread.empty()) {
                auto report =
                    refresh_sources(library, registry, reread, options.refresh, progress_callback(0), cancel);
                if (report) {
                    to_hash = report->unhashed_items;
                } else {
                    core::log_warn(kWhere, "re-reading changed files failed: {}", report.error().describe());
                }
            } else if (!batch.empty()) {
                auto report = hash_sources(library, batch, options.hashing,
                                           progress_callback(items_done_before), cancel);
                std::lock_guard lock(mutex);
                run_items_done += batch.size();
                if (report) {
                    run_hashed += report->hashed;
                    run_changed += report->changed;
                    run_missing += report->missing;
                    run_failed += report->failed;
                    to_reread = report->changed_items;
                } else {
                    run_failed += batch.size();
                    core::log_warn(kWhere, "hashing failed: {}", report.error().describe());
                }
            }
        } catch (const std::bad_alloc&) {
            core::log_error(kWhere, "a maintenance step ran out of memory");
            if (!delivered) {
                const Error error{ErrorCode::OutOfMemory, "a maintenance step ran out of memory",
                                  std::string(kWhere), 0};
                if (relink) {
                    call_safely(relink->second, Expected<RelinkReport>(Unexpected(error)));
                }
                for (const RefreshCallback& callback : check_callbacks) {
                    call_safely(callback, Expected<RefreshReport>(Unexpected(error)));
                }
            }
        }

        std::vector<RelinkCallback> failed_relinks;
        std::vector<RefreshCallback> failed_checks;
        std::optional<MaintenanceEvent> idle_event;
        {
            std::lock_guard lock(mutex);
            running = false;
            running_task = {};
            if (!shut_down && !cancel.is_cancelled()) {
                try {
                    enqueue_hash_locked(to_hash);
                    rereads.insert(rereads.end(), to_reread.begin(), to_reread.end());
                } catch (const std::bad_alloc&) {
                    core::log_error(kWhere, "queueing follow-up work ran out of memory");
                }
            }
            if (!shut_down && has_work_locked()) {
                schedule_locked(failed_relinks, failed_checks);
            }
            if (!running) {
                MaintenanceEvent event;
                event.kind = MaintenanceEvent::Kind::Idle;
                event.hash_items_done = run_items_done;
                event.hash_items_total = run_items_total;
                event.hashed = run_hashed;
                event.changed = run_changed;
                event.missing = run_missing;
                event.failed = run_failed;
                idle_event = event;
                reset_run_locked();
                hash_queued.clear();
            }
        }
        fail_callbacks(failed_relinks, failed_checks);
        if (idle_event) {
            emit(*idle_event);
        }
        idle.notify_all();
    }

    // Takes items for one hashing step: whole files, about hash_batch_bytes. Items no longer in the
    // library, of files that are not present or already hashed, are dropped (counted as done).
    std::vector<ItemId> take_hash_batch_locked() {
        std::vector<ItemId> batch;
        std::unordered_set<std::wstring> files;
        std::uint64_t bytes = 0;
        while (!hash_queue.empty()) {
            const ItemId id = hash_queue.front();
            std::optional<SourceItem> item = library.find(id);
            if (!item || item->sha256 || item->source_state != SourceState::Present) {
                hash_queue.pop_front();
                hash_queued.erase(id.value);
                ++run_items_done;
                continue;
            }
            const bool new_file = files.insert(item->path.native()).second;
            if (new_file && !batch.empty() && bytes + item->identity.size_bytes > options.hash_batch_bytes) {
                break;
            }
            if (new_file) {
                bytes += item->identity.size_bytes;
            }
            hash_queue.pop_front();
            hash_queued.erase(id.value);
            batch.push_back(id);
        }
        return batch;
    }
};

MaintenanceService::MaintenanceService(Library& library, const raw::DecoderRegistry& registry,
                                       TaskExecutor& executor, MaintenanceServiceOptions options)
    : state_(std::make_shared<State>(library, registry, executor, std::move(options))) {}

MaintenanceService::~MaintenanceService() {
    std::vector<RelinkCallback> failed_relinks;
    std::vector<RefreshCallback> failed_checks;
    {
        std::unique_lock lock(state_->mutex);
        state_->shut_down = true;
        state_->running_task.cancel();
        state_->drop_queued_locked(failed_relinks, failed_checks);
    }
    State::fail_callbacks(failed_relinks, failed_checks);
    std::unique_lock lock(state_->mutex);
    state_->idle.wait(lock, [&] { return !state_->running; });
}

Status MaintenanceService::check_all(RefreshCallback done) {
    if (!state_->library.capabilities().source_maintenance) {
        return fail(ErrorCode::Unsupported, "this library does not keep source facts", std::string(kWhere));
    }
    std::vector<RelinkCallback> failed_relinks;
    std::vector<RefreshCallback> failed_checks;
    try {
        std::lock_guard lock(state_->mutex);
        if (state_->shut_down) {
            return fail(ErrorCode::Cancelled, "the maintenance service is shutting down",
                        std::string(kWhere));
        }
        state_->check_requested = true;
        if (done) {
            state_->checks.push_back(std::move(done));
        }
        state_->schedule_locked(failed_relinks, failed_checks);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot queue the check", std::string(kWhere));
    }
    State::fail_callbacks(failed_relinks, failed_checks);
    return ok();
}

Status MaintenanceService::hash(std::vector<ItemId> items) {
    if (!state_->library.capabilities().source_maintenance) {
        return fail(ErrorCode::Unsupported, "this library does not keep source facts", std::string(kWhere));
    }
    std::vector<RelinkCallback> failed_relinks;
    std::vector<RefreshCallback> failed_checks;
    try {
        std::lock_guard lock(state_->mutex);
        if (state_->shut_down) {
            return fail(ErrorCode::Cancelled, "the maintenance service is shutting down",
                        std::string(kWhere));
        }
        state_->enqueue_hash_locked(items);
        state_->schedule_locked(failed_relinks, failed_checks);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot queue hashing", std::string(kWhere));
    }
    State::fail_callbacks(failed_relinks, failed_checks);
    return ok();
}

Status MaintenanceService::relink(RelinkRequest request, RelinkCallback done) {
    if (!done) {
        return fail(ErrorCode::InvalidArgument, "the relink callback is empty", std::string(kWhere));
    }
    if (!state_->library.capabilities().source_maintenance) {
        return fail(ErrorCode::Unsupported, "this library does not keep source facts", std::string(kWhere));
    }
    std::vector<RelinkCallback> failed_relinks;
    std::vector<RefreshCallback> failed_checks;
    try {
        std::lock_guard lock(state_->mutex);
        if (state_->shut_down) {
            return fail(ErrorCode::Cancelled, "the maintenance service is shutting down",
                        std::string(kWhere));
        }
        state_->relinks.emplace_back(std::move(request), std::move(done));
        state_->schedule_locked(failed_relinks, failed_checks);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot queue the relink", std::string(kWhere));
    }
    State::fail_callbacks(failed_relinks, failed_checks);
    return ok();
}

void MaintenanceService::cancel_all() {
    std::vector<RelinkCallback> failed_relinks;
    std::vector<RefreshCallback> failed_checks;
    {
        std::lock_guard lock(state_->mutex);
        state_->running_task.cancel();
        state_->drop_queued_locked(failed_relinks, failed_checks);
    }
    State::fail_callbacks(failed_relinks, failed_checks);
}

bool MaintenanceService::busy() const {
    std::lock_guard lock(state_->mutex);
    return state_->running || state_->has_work_locked();
}

}  // namespace lucent::app
