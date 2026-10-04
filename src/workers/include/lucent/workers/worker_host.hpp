#pragma once

// The host side of one isolated worker process (blueprint section 4: "Keep decoding, optimization,
// and encoding off the GUI thread. Use ... a worker process for crash containment and release of
// native allocations ... stop or restart a worker that exceeds its job timeout or becomes
// unresponsive"; section 7: cooperative cancellation, termination after a timeout with unpublished
// temporary output cleaned up). The application's batch service owns one WorkerHost per worker slot
// and decides admission, retries and publication; a WorkerHost runs one job at a time and reports
// precisely how it ended.
//
// Process separation is crash containment, not a security sandbox: the worker runs as the same
// user with the same rights, and reads and writes the paths it is given.
//
// Starting a worker (start(), or run_job() when none is running):
//  * two anonymous pipes, one per direction; the worker's ends and a SYNCHRONIZE handle of this
//    process are the only handles it inherits (PROC_THREAD_ATTRIBUTE_HANDLE_LIST; they are
//    inheritable only during CreateProcess, under a process-wide lock). CreatePipe opens both ends
//    of its single pipe instance at once, so no other process can connect to it, and its default
//    security descriptor admits only this user, SYSTEM and administrators: there is no pipe name
//    to squat on and no DACL to get wrong, which is why anonymous pipes were chosen over a named
//    pipe;
//  * CreateProcess with CREATE_NO_WINDOW and CREATE_SUSPENDED (lucent-worker.exe is a GUI-subsystem
//    program, so no console window ever flashes), below-normal priority by default, working
//    directory = the executable's folder;
//  * assigned to its own Job Object before it runs: JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE (the worker
//    dies with this process, whatever ends it), JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION (no
//    error dialog), JOB_OBJECT_LIMIT_ACTIVE_PROCESS = 1, and with memory_limit_bytes set,
//    JOB_OBJECT_LIMIT_PROCESS_MEMORY (a commit limit: allocations beyond it fail inside the worker);
//    an I/O completion port receives the job's memory-limit notifications;
//  * the handshake (protocol.hpp): versions negotiated, identical builds required. A mismatch is
//    VersionMismatch ("workers.handshake.*") naming both builds.
//
// Running a job (run_job): the request is sent and the call blocks until the job ends. Callbacks
// run on the calling thread. While waiting, the host
//  * forwards progress and heartbeats,
//  * declares the worker unresponsive when no message (heartbeats arrive every heartbeat_interval)
//    came for heartbeat_timeout, and terminates it,
//  * on cancellation (token) or after the job's timeout sends cancel and waits cancel_grace for the
//    worker's answer, then terminates it,
//  * notices the worker ending (process exit or broken pipe) as a crash,
//  * and maps everything to a JobOutcome (below). After any WorkerFault the worker process is gone
//    (terminated if needed), the job's temporary output is removed (remove_job_output), and the
//    next run_job() or start() starts a new process ("restart").
//
// Failures and classes (JobFailure):
//   | How the job ended                              | ErrorCode / where                  | Class         | Fault          |
//   |------------------------------------------------|------------------------------------|---------------|----------------|
//   | the worker reported an error                   | the worker's                       | the worker's  | None           |
//   | the worker process ended (crash, killed)       | Internal / workers.worker.crashed  | Transient     | Crashed        |
//   | no message for heartbeat_timeout               | Timeout / workers.worker.unresponsive | Transient  | Unresponsive   |
//   | the job's timeout; the worker then cancelled   | Timeout / workers.job.timeout      | Transient     | None           |
//   | the job's timeout; it did not stop in time     | Timeout / workers.job.timeout      | Transient     | TimedOut       |
//   | cancelled; the worker stopped in time          | Cancelled / the worker's           | Cancelled     | None           |
//   | cancelled; it did not stop within cancel_grace | Cancelled / workers.job.cancelled  | Cancelled     | CancelTimeout  |
//   | the worker hit memory_limit_bytes (it reported | LimitExceeded /                    | Deterministic | MemoryLimit    |
//   | OutOfMemory or ended)                          | workers.worker.memory_limit        |               |                |
//   | the worker broke the protocol, or answered     | Corrupt / workers.protocol.*       | Deterministic | ProtocolViolation |
//   | with another output, recipe or source          |                                    |               |                |
//   | the worker could not be started                | the start error (NotFound, VersionMismatch, Timeout, ...) | see start() | StartFailed |
// The memory limit is recognised from the Job Object's JOB_OBJECT_MSG_PROCESS_MEMORY_LIMIT
// notification or, because Windows does not guarantee delivery of those messages, from an
// OutOfMemory or crash while the worker's peak commit was within 5 % of the limit. An OutOfMemory
// without either is the worker's own error (Transient: memory pressure from elsewhere). After a
// memory-limit hit the worker is recycled even if its job succeeded.
//
// Thread-safety: start(), run_job(), ping() and shutdown() must not be called concurrently (one
// thread drives a host; cancellation arrives through the token). info() may be called from any
// thread at any time. The destructor shuts the worker down and must not race with other calls.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/workers/protocol.hpp>

namespace lucent::workers {

struct WorkerHostOptions {
    // lucent-worker.exe to start; empty: default_worker_executable().
    std::filesystem::path executable;
    // Per-process commit limit of the worker (Job Object); 0: none. Measured (W7): a JPEG export of
    // the 45 MP Canon EOS R5 fixture peaks at 731 MiB of commit, below the engine's estimate of
    // 1 GiB (JobResult::render.estimated_peak_bytes). The batch service derives the limit from its
    // memory budget; setting JobRequest::memory_budget_bytes to at most this limit makes the
    // engine refuse an oversized render up front (LimitExceeded) instead of failing an allocation.
    std::uint64_t memory_limit_bytes = 0;
    std::chrono::milliseconds handshake_timeout{15'000};
    std::chrono::milliseconds heartbeat_interval{500};  // sent to the worker in host_hello
    std::chrono::milliseconds heartbeat_timeout{15'000};
    std::chrono::milliseconds cancel_grace{5'000};
    std::chrono::milliseconds shutdown_grace{5'000};
    // BELOW_NORMAL_PRIORITY_CLASS, so batch work yields to the interface and interactive previews.
    bool below_normal_priority = true;

    // ---- Test only ------------------------------------------------------------------------------
    // Test hook specs passed as --test-hook (worker_launch.hpp). Never set in production.
    std::vector<std::string> test_hooks;
    // false: no Job Object (no memory limit, no kill-on-close), so a test can observe the worker's
    // own reaction to its host ending (the pipe and the parent handle).
    bool use_job_object = true;
};

// "lucent-worker.exe" in the folder of the running executable.
[[nodiscard]] std::filesystem::path default_worker_executable();

// Why the worker process is gone after a job (persisted in job records; append only).
enum class WorkerFault : std::uint8_t {
    None = 0,               // the worker answered and stays usable
    StartFailed = 1,        // it could not be started, or the handshake failed
    Crashed = 2,            // the process ended during the job
    Unresponsive = 3,       // no heartbeat within heartbeat_timeout; terminated
    TimedOut = 4,           // the job's timeout passed and it did not stop after cancel; terminated
    CancelTimeout = 5,      // cancelled and did not stop within cancel_grace; terminated
    MemoryLimit = 6,        // it reached memory_limit_bytes; recycled
    ProtocolViolation = 7,  // it sent something the protocol forbids; terminated
};

[[nodiscard]] std::string_view to_string(WorkerFault fault) noexcept;  // "none", "crashed", ...

struct JobFailure {
    Error error;
    FailureClass failure_class = FailureClass::Transient;
    WorkerFault fault = WorkerFault::None;
    std::optional<std::uint32_t> exit_code;  // when the worker process ended
    // The job's temporary output was removed after the worker was lost (remove_job_output); false
    // when removal failed (the error is logged) or the worker stayed (it removes its own leftovers).
    bool output_removed = false;
};

struct JobMetrics {
    double wall_seconds = 0.0;   // the whole run_job() call, including any start
    double start_seconds = 0.0;  // spawn and handshake when run_job() had to start the worker
    // The largest peak working set the worker reported up to the end of the job (heartbeats,
    // result), and the worker process's peak commit from Job Object accounting (0 without one).
    // Both are peaks over the process's life, so they describe this job when the process was new.
    std::uint64_t peak_working_set_bytes = 0;
    std::uint64_t peak_commit_bytes = 0;
};

// Exactly one of result and failure is set.
struct JobOutcome {
    std::optional<JobResult> result;
    std::optional<JobFailure> failure;
    JobMetrics metrics;

    [[nodiscard]] bool succeeded() const noexcept { return result.has_value(); }
};

struct JobCallbacks {
    std::function<void(const JobProgress&)> progress;  // on the run_job() thread
    std::function<void(const Heartbeat&)> heartbeat;   // on the run_job() thread, during the job
};

struct WorkerInfo {
    bool running = false;
    std::uint32_t pid = 0;
    std::uint32_t protocol_version = 0;
    BuildIdentity build;                     // the worker's, from its hello
    std::uint32_t creation_flags = 0;        // passed to CreateProcess (CREATE_NO_WINDOW, ...)
    bool console_window = false;             // as the worker reported it
    std::vector<std::string> test_hooks;     // as the worker reported them
    std::uint64_t starts = 0;                // processes this host started (restarts = starts - 1)
    double last_start_seconds = 0.0;         // spawn + handshake of the newest process
    std::uint64_t jobs = 0;                  // jobs dispatched
    std::uint64_t peak_commit_bytes = 0;     // Job Object accounting of the newest process
};

class WorkerHost {
public:
    // Does not start a process.
    explicit WorkerHost(WorkerHostOptions options = {});
    // shutdown().
    ~WorkerHost();
    WorkerHost(const WorkerHost&) = delete;
    WorkerHost& operator=(const WorkerHost&) = delete;

    // Starts a worker and completes the handshake; ok() at once when one is running. Errors
    // ("workers.host.*" unless stated): InvalidArgument (options: a test hook spec, timeouts that
    // are not positive); NotFound (no executable); VersionMismatch ("workers.handshake.*": another
    // build or protocol); Timeout (no hello within handshake_timeout); IoError (the worker ended
    // during the handshake, or a Win32 failure); OutOfMemory.
    [[nodiscard]] Status start();

    // Runs `request` on the worker (starting one first when none is running) and blocks until the
    // job ends (see the file comment). `timeout` (0: none) bounds the job from dispatch. A request
    // that encode_message refuses fails with InvalidArgument, Deterministic, WorkerFault::None,
    // without reaching the worker.
    [[nodiscard]] JobOutcome run_job(const JobRequest& request, const JobCallbacks& callbacks = {},
                                     const core::CancellationToken& cancel = {},
                                     std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());

    // Round trip to an idle worker. Errors: Internal when no worker is running; Timeout (no pong
    // within `timeout`: the worker is then terminated); IoError when the worker is gone.
    [[nodiscard]] Status ping(std::chrono::milliseconds timeout = std::chrono::seconds(5));

    // Asks the worker to exit (shutdown message, then the pipe is closed), waits up to
    // shutdown_grace, then terminates it. No effect when none is running.
    void shutdown();

    [[nodiscard]] WorkerInfo info() const;

    struct State;  // implementation detail

private:
    std::unique_ptr<State> state_;
};

}  // namespace lucent::workers
