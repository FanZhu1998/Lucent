#pragma once

// A hard limit on the memory of several worker processes together (blueprint section 7, "Memory and
// scheduling": "Use an aggregate memory budget, not one worker per CPU core"; ADR 0012, amendment
// "Aggregate worker memory limit").
//
// The batch scheduler admits a second worker only when the running jobs' estimates fit its worker
// budget, but an estimate is a calibration, not a bound: a photo whose real commit exceeds it could
// take two workers, each within its own per-process limit, up to twice the budget. A WorkerGroup
// makes the budget a limit the operating system enforces. It owns a Job Object with
// JOB_OBJECT_LIMIT_JOB_MEMORY = memory_limit_bytes (and JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE); a
// WorkerHost given the group (WorkerHostOptions::group) assigns every worker it starts to the
// group's Job Object first and then to the worker's own Job Object, which therefore nests inside it
// (nested Job Objects, Windows 8 and later). Each worker keeps its per-process limit
// (WorkerHostOptions::memory_limit_bytes); every allocation that would take the group's total commit
// beyond its limit fails inside the worker that makes it.
//
// Which worker the limit stops is whichever allocates while the group is at its limit, not
// necessarily the one that overran its estimate. WorkerHost therefore classifies a group-limit hit
// by whether the worker had company (worker_host.hpp, "Failures and classes"):
//   * another worker of the group was alive during the job: Transient, WorkerFault::GroupMemoryLimit,
//     OutOfMemory at "workers.worker.group_memory_limit" - the job may well succeed alone, and the
//     batch scheduler retries it with exclusive admission (batch_service.hpp);
//   * the worker was alone in the group: the photo needs more than the whole limit, the outcome of
//     its own per-process limit (Deterministic, WorkerFault::MemoryLimit, LimitExceeded at
//     "workers.worker.memory_limit").
// The hit is recognised from the group's JOB_OBJECT_MSG_JOB_MEMORY_LIMIT notification naming the
// worker's process or, because Windows does not guarantee delivery of those messages, from an
// OutOfMemory or crash while the group's peak commit rose to within 5 % of its limit during the job.
//
// A worker that cannot join the group (an environment whose own Job Object refuses nesting) runs
// with its per-process limit only; WorkerInfo::grouped says so and the first refusal is logged.
//
// Thread-safety: a group is shared by the hosts of one scheduler (one per slot thread); every member
// function is thread-safe. The group must outlive the processes it holds only in the sense that
// closing it ends them (kill on close), so it is held by shared_ptr from every host.

#include <cstdint>
#include <memory>

#include <lucent/core/error.hpp>

namespace lucent::workers {

struct WorkerGroupStats {
    std::uint64_t memory_limit_bytes = 0;
    // The largest total commit of the group's processes so far (Job Object accounting,
    // PeakJobMemoryUsed); 0 when it cannot be read. Windows charges an allocation before it refuses
    // it at the limit, so after a refusal this peak includes the refused allocation and can exceed
    // the limit by it although the processes never held it.
    std::uint64_t peak_commit_bytes = 0;
    std::uint64_t limit_notifications = 0;  // JOB_OBJECT_MSG_JOB_MEMORY_LIMIT messages received
    std::uint64_t joined = 0;               // processes that joined the group, in total
    std::uint64_t refused = 0;              // processes that could not join (run ungrouped)
    std::uint32_t members = 0;              // processes of the group still alive
};

class WorkerGroup {
public:
    // A group whose processes together may commit at most `memory_limit_bytes`. Errors
    // ("workers.group"): InvalidArgument for a limit of 0; IoError (a Win32 failure creating the
    // Job Object or its completion port); OutOfMemory.
    [[nodiscard]] static Expected<std::shared_ptr<WorkerGroup>> create(std::uint64_t memory_limit_bytes);

    ~WorkerGroup();
    WorkerGroup(const WorkerGroup&) = delete;
    WorkerGroup& operator=(const WorkerGroup&) = delete;

    [[nodiscard]] std::uint64_t memory_limit_bytes() const noexcept;
    [[nodiscard]] WorkerGroupStats stats() const;

    // ---- For WorkerHost (worker_host.cpp) -----------------------------------------------------

    // Assigns the (suspended, not yet in its own Job Object) process `process` with id `pid` to the
    // group. False when Windows refuses (logged once per group); the process is then not a member.
    [[nodiscard]] bool join(void* process, std::uint32_t pid);
    // What a job of member `pid` saw of the group when it started (or ended).
    struct Snapshot {
        std::uint64_t joined = 0;          // processes that joined so far
        std::uint32_t other_members = 0;   // other members alive now
        std::uint64_t peak_commit = 0;     // the group's peak commit so far
    };
    [[nodiscard]] Snapshot snapshot(std::uint32_t pid) const;
    // True when a JOB_OBJECT_MSG_JOB_MEMORY_LIMIT naming `pid` arrived since the process joined or
    // the last call for it; the notification is consumed.
    [[nodiscard]] bool take_limit_hit(std::uint32_t pid);
    // Forgets member `pid` (its process ended or is being discarded) and its pending notifications.
    void leave(std::uint32_t pid) noexcept;

    struct State;  // implementation detail

private:
    explicit WorkerGroup(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

}  // namespace lucent::workers
