#pragma once

// The worker's side of the protocol (protocol.hpp), run by lucent-worker.exe: the handshake, the
// request loop, heartbeats, cancellation, and leaving when the host goes away. The work itself is a
// JobRunner (apps/lucent-worker implements the develop render and export), so this library does
// not depend on the application services and tests can run a server in-process.
//
// Threads. run_worker_server() runs jobs on the calling thread, one at a time. A reader thread
// receives requests (a job is queued, cancel cancels the running job's token, ping is answered at
// once, shutdown ends the loop); a heartbeat thread sends a heartbeat every heartbeat_ms with the
// process's memory, independent of the job, and watches the parent process.
//
// Leaving. The server returns when the host asks for shutdown (WorkerExitCode::Clean), when the
// host's end of the pipe closes or the parent process ends (ParentGone), after a refused handshake
// (Refused), or after the host violated the protocol (ProtocolViolation). A running job is
// cancelled first; if it has not returned within WorkerServerOptions::exit_grace the heartbeat
// thread calls force_exit, so a worker never outlives its host for long, whatever its job does.
//
// Test hooks handled here (worker_launch.hpp): hang-job, freeze, crash-job and allocate act when a
// job (export or analyze) starts, before the runner is called; render-delay and fail-final-check
// are the runner's (JobContext::test_hooks()).

#include <chrono>
#include <functional>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/workers/channel.hpp>
#include <lucent/workers/protocol.hpp>
#include <lucent/workers/worker_launch.hpp>

namespace lucent::workers {

// What a runner sees of the server while it runs one job.
class JobContext {
public:
    virtual ~JobContext() = default;

    // Cancelled when the host cancels the job or goes away, or the worker shuts down.
    [[nodiscard]] virtual const core::CancellationToken& cancel() const noexcept = 0;

    // Reports progress to the host. The fraction is clamped to [0, 1] and never decreases within a
    // job (a lower value reports the highest so far); a message is sent only when the stage changes,
    // the fraction grew by at least 0.01 since the last message, or it reached 1. Never fails (a
    // closed pipe shows up as cancellation). Thread-safe.
    virtual void progress(JobStage stage, double fraction) = 0;

    [[nodiscard]] virtual const TestHooks& test_hooks() const noexcept = 0;
};

class JobRunner {
public:
    virtual ~JobRunner() = default;

    // Runs one job on the server's job thread. The request passed decode_message (absolute paths,
    // a recipe whose SHA-256 matches its document, valid export settings). Must return promptly
    // with Cancelled once context.cancel() is cancelled. Exceptions are caught by the server:
    // std::bad_alloc becomes OutOfMemory, any other exception Internal. The server answers with a
    // result (its job_id is set by the server) or an error whose class is default_failure_class().
    [[nodiscard]] virtual Expected<JobResult> run(const JobRequest& request, JobContext& context) = 0;

    // Runs one analyze job (protocol version 2), with run()'s contract; the server answers with an
    // analysis (its job_id set by the server) or an error. The default refuses with Unsupported
    // ("workers.worker.analyze"): a runner that only exports.
    [[nodiscard]] virtual Expected<AnalysisResult> analyze(const AnalysisRequest& request, JobContext& context);

protected:
    JobRunner() = default;
};

struct WorkerServerOptions {
    TestHooks hooks;
    // A handle signalled when the host process ends (WorkerLaunch::parent_handle); not owned; null
    // to rely on the pipe alone.
    void* parent_process = nullptr;
    // How long a running job may take to return after the host went away or asked for shutdown
    // before force_exit is called.
    std::chrono::milliseconds exit_grace{2000};
    // Ends the process with the given code without unwinding. Default: TerminateProcess on the
    // current process. Tests replace it.
    std::function<void(WorkerExitCode)> force_exit;
};

// Runs the worker's side of the protocol on `channel` (the worker's ends; incoming direction
// HostToWorker) until it ends (see the file comment) and returns the exit code for the process.
// The runner must stay valid until it returns.
[[nodiscard]] WorkerExitCode run_worker_server(MessageChannel& channel, JobRunner& runner,
                                               const WorkerServerOptions& options);

// Memory counters of the current process (GetProcessMemoryInfo); zeros if they cannot be read.
[[nodiscard]] MemorySnapshot current_process_memory() noexcept;

}  // namespace lucent::workers
