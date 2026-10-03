#pragma once

// MaintenanceService: runs the source maintenance of source_maintenance.hpp in the background
// (W4; blueprint section 4: "Keep decoding, optimization, and encoding off the GUI thread";
// section 7: background work must not make editing unusable; ADR 0008).
//
// Work is done in steps, one at a time, each an executor task on the Background lane:
//   1. relink requests (user actions, first), in the order made;
//   2. checks of every file (check_all(): refresh_sources over the whole library);
//   3. re-reads of files found changed while hashing (refresh_sources over their items);
//   4. hashing of queued items (hash()), in batches of about hash_batch_bytes.
// Between steps the next one is submitted anew, so imports and exports submitted meanwhile run
// in between (the Background lane is first in, first out): a long hashing run never holds up an
// export by more than one batch.
//
// Chaining. A check queues the items of files that are present without a hash, and the items of
// changed files it re-read, for hashing; so does a relink for the files it relinked (one relinked
// by its identity may never have been hashed). Hashing that finds a file changed queues a re-read
// of it (step 3), whose result is hashed again. Missing files are only marked.
//
// Events. `observer` (MaintenanceServiceOptions) is called on the worker thread: Progress during
// every step (at most every progress_interval, and at the end of each file), and Idle when the
// last queued work has finished (with the totals of the hashing run since the previous Idle). It
// must be quick and must not throw; a UI posts the event to its thread.
//
// Callbacks of check_all() and relink() are called exactly once per accepted request: on the
// worker thread after the step, or with Cancelled by cancel_all() or the destructor (on the
// calling thread). They must not destroy the service.
//
// Lifetime: the library, the registry and the executor must outlive the service. The destructor
// cancels the running step cooperatively, fails queued requests with Cancelled and waits until
// the running step's callbacks have returned.
//
// Thread-safety: all members may be called concurrently.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <lucent/application/library.hpp>
#include <lucent/application/source_maintenance.hpp>
#include <lucent/application/task_executor.hpp>
#include <lucent/core/error.hpp>
#include <lucent/raw/decoder_registry.hpp>

namespace lucent::app {

struct MaintenanceEvent {
    enum class Kind : std::uint8_t {
        Progress = 1,
        Idle = 2,
    };
    Kind kind = Kind::Progress;
    MaintenanceProgress progress;  // Progress: the running step
    // Totals of the current hashing run: items (photos) queued for hashing since the service was
    // last idle, and how many of them have been dealt with.
    std::uint64_t hash_items_done = 0;
    std::uint64_t hash_items_total = 0;
    // Idle: what the run found.
    std::uint64_t hashed = 0;
    std::uint64_t changed = 0;
    std::uint64_t missing = 0;
    std::uint64_t failed = 0;
};

using MaintenanceObserver = std::function<void(const MaintenanceEvent&)>;

struct MaintenanceServiceOptions {
    HashOptions hashing;
    RefreshOptions refresh;
    // Bytes of files hashed per Background task before the next batch is submitted (at least one
    // file per batch).
    std::uint64_t hash_batch_bytes = std::uint64_t{512} << 20;
    std::chrono::milliseconds progress_interval{100};
    MaintenanceObserver observer;
};

class MaintenanceService {
public:
    using RefreshCallback = std::function<void(const Expected<RefreshReport>&)>;
    using RelinkCallback = std::function<void(const Expected<RelinkReport>&)>;

    MaintenanceService(Library& library, const raw::DecoderRegistry& registry, TaskExecutor& executor,
                       MaintenanceServiceOptions options = {});
    ~MaintenanceService();
    MaintenanceService(const MaintenanceService&) = delete;
    MaintenanceService& operator=(const MaintenanceService&) = delete;

    // Queues a check of every file (step 2), then hashing of what needs it. `done` may be empty.
    // Errors (no callback): Unsupported when the library has no source maintenance; Cancelled
    // after shutdown began.
    [[nodiscard]] Status check_all(RefreshCallback done = {});

    // Queues the files of `items` for hashing (step 4); items already queued, of files with a
    // hash, or no longer in the library are skipped when their turn comes. Errors as check_all().
    [[nodiscard]] Status hash(std::vector<ItemId> items);

    // Queues a relink (step 1). Errors as check_all(); InvalidArgument for a null callback.
    [[nodiscard]] Status relink(RelinkRequest request, RelinkCallback done);

    // Cancels the running step and drops queued work (queued callbacks report Cancelled).
    void cancel_all();

    // True while work is queued or running.
    [[nodiscard]] bool busy() const;

    struct State;  // implementation detail

private:
    std::shared_ptr<State> state_;
};

}  // namespace lucent::app
