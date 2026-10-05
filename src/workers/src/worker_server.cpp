#include <lucent/workers/worker_server.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#include <windows.h>
#include <psapi.h>

#include <lucent/core/log.hpp>

namespace lucent::workers {

namespace {

constexpr std::string_view kWhere = "workers.worker";
using Clock = std::chrono::steady_clock;

struct ServerState {
    std::mutex mutex;
    std::condition_variable changed;
    // The next job: an export job or an analyze job.
    std::optional<std::variant<JobRequest, AnalysisRequest>> pending;
    std::optional<std::string> running_id;
    core::CancellationSource running_cancel;
    bool stop = false;
    WorkerExitCode exit_code = WorkerExitCode::Clean;
    Clock::time_point stop_requested_at{};
    bool finished = false;
    bool reader_done = false;
    bool forced = false;
    std::atomic<bool> frozen{false};
    std::atomic<bool> reader_stopping{false};

    // Caller holds `mutex`.
    void request_stop(WorkerExitCode code) {
        if (!stop) {
            stop = true;
            exit_code = code;
            stop_requested_at = Clock::now();
        }
        if (running_id) {
            running_cancel.cancel();
        }
        pending.reset();
        changed.notify_all();
    }
};

[[noreturn]] void block_forever() {
    for (;;) {
        ::Sleep(1000);
    }
}

void send_quietly(MessageChannel& channel, const Message& message) {
    if (auto sent = channel.send(message); !sent && !is_channel_closed(sent.error())) {
        core::log_warn(kWhere, "cannot send a {} message: {}", type_name(message), sent.error().describe());
    }
}

JobError fatal_error(Error error) {
    return JobError{std::nullopt, std::move(error), FailureClass::Deterministic};
}

const std::string& job_id_of(const std::variant<JobRequest, AnalysisRequest>& job) {
    return std::visit([](const auto& request) -> const std::string& { return request.job_id; }, job);
}

class Context final : public JobContext {
public:
    Context(MessageChannel& channel, std::string job_id, core::CancellationToken token, const TestHooks& hooks)
        : channel_(channel), job_id_(std::move(job_id)), token_(std::move(token)), hooks_(hooks) {}

    const core::CancellationToken& cancel() const noexcept override { return token_; }

    void progress(JobStage stage, double fraction) override {
        std::lock_guard lock(mutex_);
        if (!(fraction >= 0.0)) {
            fraction = 0.0;  // also NaN
        }
        highest_ = std::max(highest_, std::min(fraction, 1.0));
        const bool completes = highest_ >= 1.0 && last_sent_ < 1.0;
        if (sent_any_ && stage == last_stage_ && highest_ < last_sent_ + 0.01 && !completes) {
            return;
        }
        sent_any_ = true;
        last_stage_ = stage;
        last_sent_ = highest_;
        send_quietly(channel_, JobProgress{job_id_, stage, highest_});
    }

    const TestHooks& test_hooks() const noexcept override { return hooks_; }

private:
    MessageChannel& channel_;
    std::string job_id_;
    core::CancellationToken token_;
    const TestHooks& hooks_;
    std::mutex mutex_;
    bool sent_any_ = false;
    JobStage last_stage_ = JobStage::Preparing;
    double highest_ = 0.0;    // the largest fraction reported so far
    double last_sent_ = 0.0;  // the fraction of the last message sent
};

// The answer to one job: a result or an analysis.
using Answer = std::variant<JobResult, AnalysisResult>;

Expected<Answer> run_one(const std::variant<JobRequest, AnalysisRequest>& job, JobRunner& runner, Context& context,
                         const TestHooks& hooks, ServerState& state) {
    if (hooks.freeze) {
        state.frozen = true;
        block_forever();
    }
    if (hooks.hang_job) {
        block_forever();
    }
    if (hooks.crash_job) {
        ::RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    }
    try {
        if (hooks.allocate_bytes != 0) {
            const auto bytes = static_cast<std::size_t>(hooks.allocate_bytes);
            auto block = std::make_unique_for_overwrite<std::byte[]>(bytes);
            for (std::size_t offset = 0; offset < bytes; offset += 4096) {
                block[offset] = std::byte{1};  // commit and touch every page
            }
        }
        if (const auto* request = std::get_if<JobRequest>(&job)) {
            auto result = runner.run(*request, context);
            if (!result) {
                return Unexpected(std::move(result.error()));
            }
            result->job_id = request->job_id;
            return Answer(std::move(*result));
        }
        const auto& request = std::get<AnalysisRequest>(job);
        auto analysis = runner.analyze(request, context);
        if (!analysis) {
            return Unexpected(std::move(analysis.error()));
        }
        analysis->job_id = request.job_id;
        return Answer(std::move(*analysis));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "the job ran out of memory", std::string(kWhere));
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal, std::format("the job raised: {}", e.what()), std::string(kWhere));
    } catch (...) {
        return fail(ErrorCode::Internal, "the job raised an unknown exception", std::string(kWhere));
    }
}

void reader_loop(MessageChannel& channel, ServerState& state) {
    for (;;) {
        if (state.reader_stopping) {
            break;
        }
        auto received = channel.receive();
        std::unique_lock lock(state.mutex);
        if (!received) {
            if (received.error().code == ErrorCode::Corrupt) {
                lock.unlock();
                send_quietly(channel, fatal_error(received.error()));
                lock.lock();
                state.request_stop(WorkerExitCode::ProtocolViolation);
            } else if (!state.reader_stopping) {
                state.request_stop(WorkerExitCode::ParentGone);
            }
            break;
        }
        if (!*received) {
            state.request_stop(WorkerExitCode::ParentGone);
            break;
        }
        Message& message = **received;
        std::optional<std::variant<JobRequest, AnalysisRequest>> job;
        if (auto* export_job = std::get_if<JobRequest>(&message)) {
            job.emplace(std::move(*export_job));
        } else if (auto* analyze_job = std::get_if<AnalysisRequest>(&message)) {
            job.emplace(std::move(*analyze_job));
        }
        if (job) {
            if (state.stop) {
                continue;  // shutting down: the host gets no answer and does not wait for one
            }
            if (state.pending || state.running_id) {
                JobError busy{job_id_of(*job),
                              Error{ErrorCode::InvalidArgument, "a job is already running", "workers.worker.busy", 0},
                              FailureClass::Deterministic};
                lock.unlock();
                send_quietly(channel, busy);
                continue;
            }
            state.pending = std::move(*job);
            state.changed.notify_all();
        } else if (auto* cancel = std::get_if<CancelJob>(&message)) {
            if (state.running_id == cancel->job_id) {
                state.running_cancel.cancel();
            } else if (state.pending && job_id_of(*state.pending) == cancel->job_id) {
                state.pending.reset();
                JobError cancelled{cancel->job_id,
                                   Error{ErrorCode::Cancelled, "the job was cancelled before it started",
                                         std::string(kWhere), 0},
                                   FailureClass::Cancelled};
                lock.unlock();
                send_quietly(channel, cancelled);
            }
        } else if (auto* ping = std::get_if<Ping>(&message)) {
            if (!state.frozen) {
                const Pong pong{ping->nonce};
                lock.unlock();
                send_quietly(channel, pong);
            }
        } else if (std::holds_alternative<Shutdown>(message)) {
            state.request_stop(WorkerExitCode::Clean);
            break;
        } else {
            Error error{ErrorCode::Corrupt, std::format("unexpected '{}' message", type_name(message)),
                        "workers.protocol.sequence", 0};
            lock.unlock();
            send_quietly(channel, fatal_error(std::move(error)));
            lock.lock();
            state.request_stop(WorkerExitCode::ProtocolViolation);
            break;
        }
    }
    std::lock_guard lock(state.mutex);
    state.reader_done = true;
    state.changed.notify_all();
}

void heartbeat_loop(MessageChannel& channel, ServerState& state, const WorkerServerOptions& options,
                    std::chrono::milliseconds interval) {
    std::uint64_t sequence = 0;
    std::unique_lock lock(state.mutex);
    while (!state.finished) {
        state.changed.wait_for(lock, interval, [&] { return state.finished; });
        if (state.finished) {
            break;
        }
        if (options.parent_process != nullptr && ::WaitForSingleObject(options.parent_process, 0) == WAIT_OBJECT_0) {
            state.request_stop(WorkerExitCode::ParentGone);
        }
        if (state.stop && state.running_id && !state.forced &&
            Clock::now() - state.stop_requested_at > options.exit_grace) {
            // The job did not observe its cancellation in time: leave without it.
            state.forced = true;
            const WorkerExitCode code = state.exit_code;
            lock.unlock();
            if (options.force_exit) {
                options.force_exit(code);
            } else {
                ::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(code));
            }
            lock.lock();
            continue;
        }
        if (state.frozen) {
            continue;
        }
        Heartbeat heartbeat{++sequence, state.running_id, current_process_memory()};
        lock.unlock();
        send_quietly(channel, heartbeat);
        lock.lock();
    }
}

// Handshake; std::nullopt when it succeeded, otherwise the exit code.
std::optional<WorkerExitCode> handshake(MessageChannel& channel, const WorkerServerOptions& options,
                                        std::chrono::milliseconds& interval) {
    auto first = channel.receive();
    if (!first) {
        if (first.error().code == ErrorCode::Corrupt) {
            send_quietly(channel, fatal_error(first.error()));
            return WorkerExitCode::ProtocolViolation;
        }
        return WorkerExitCode::ParentGone;
    }
    if (!*first) {
        return WorkerExitCode::ParentGone;
    }
    const auto* hello = std::get_if<HostHello>(&**first);
    if (hello == nullptr) {
        send_quietly(channel, fatal_error(Error{ErrorCode::Corrupt, "the first message must be host_hello",
                                                "workers.protocol.sequence", 0}));
        return WorkerExitCode::ProtocolViolation;
    }
    const auto [low, high] = options.hooks.protocol_range.value_or(std::pair(kMinProtocolVersion, kMaxProtocolVersion));
    auto version = negotiate_version(hello->min_version, hello->max_version, low, high);
    if (!version) {
        send_quietly(channel, fatal_error(std::move(version.error())));
        return WorkerExitCode::Refused;
    }
    BuildIdentity build = current_build_identity();
    if (options.hooks.fake_commit) {
        build.git_commit = *options.hooks.fake_commit;
    }
    if (auto same = check_same_build(build, hello->build, "host"); !same) {
        send_quietly(channel, fatal_error(std::move(same.error())));
        return WorkerExitCode::Refused;
    }
    interval = std::chrono::milliseconds(hello->heartbeat_ms);
    WorkerHello answer;
    answer.version = *version;
    answer.build = std::move(build);
    answer.pid = static_cast<std::uint32_t>(::GetCurrentProcessId());
    answer.console_window = ::GetConsoleWindow() != nullptr;
    answer.test_hooks = options.hooks.specs();
    if (auto sent = channel.send(answer); !sent) {
        return WorkerExitCode::ParentGone;
    }
    return std::nullopt;
}

}  // namespace

Expected<AnalysisResult> JobRunner::analyze(const AnalysisRequest& request, JobContext&) {
    return fail(ErrorCode::Unsupported, std::format("this worker does not analyse photos (job '{}')", request.job_id),
                "workers.worker.analyze");
}

MemorySnapshot current_process_memory() noexcept {
    MemorySnapshot snapshot;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                               sizeof(counters))) {
        // The counters are not read atomically: a page faulted in by another thread can make the
        // current value exceed the peak sampled with it. A peak is never below the current value.
        snapshot.working_set = counters.WorkingSetSize;
        snapshot.peak_working_set = std::max<std::uint64_t>(counters.PeakWorkingSetSize, counters.WorkingSetSize);
        snapshot.commit = counters.PrivateUsage;
        snapshot.peak_commit = std::max<std::uint64_t>(counters.PeakPagefileUsage, counters.PrivateUsage);
    }
    return snapshot;
}

WorkerExitCode run_worker_server(MessageChannel& channel, JobRunner& runner, const WorkerServerOptions& options) {
    std::chrono::milliseconds interval{500};
    if (auto refused = handshake(channel, options, interval)) {
        return *refused;
    }

    ServerState state;
    std::thread reader;
    std::thread heartbeat;
    try {
        reader = std::thread([&] { reader_loop(channel, state); });
        heartbeat = std::thread([&] { heartbeat_loop(channel, state, options, interval); });
    } catch (const std::system_error&) {
        {
            std::lock_guard lock(state.mutex);
            state.finished = true;
            state.changed.notify_all();
        }
        if (heartbeat.joinable()) {
            heartbeat.join();
        }
        if (reader.joinable()) {
            state.reader_stopping = true;
            reader.detach();  // the process is about to end
        }
        return WorkerExitCode::Internal;
    }

    for (;;) {
        std::unique_lock lock(state.mutex);
        state.changed.wait(lock, [&] { return state.stop || state.pending.has_value(); });
        if (state.stop) {
            break;
        }
        std::variant<JobRequest, AnalysisRequest> job = std::move(*state.pending);
        state.pending.reset();
        const std::string job_id = job_id_of(job);
        state.running_id = job_id;
        state.running_cancel = core::CancellationSource();
        Context context(channel, job_id, state.running_cancel.token(), options.hooks);
        lock.unlock();

        Expected<Answer> result = run_one(job, runner, context, options.hooks, state);

        lock.lock();
        state.running_id.reset();
        lock.unlock();
        if (result) {
            std::visit([&](auto& answer) { send_quietly(channel, Message(std::move(answer))); }, *result);
        } else {
            const FailureClass failure_class = default_failure_class(result.error());
            send_quietly(channel, JobError{job_id, std::move(result.error()), failure_class});
        }
    }

    WorkerExitCode code;
    {
        std::lock_guard lock(state.mutex);
        state.finished = true;
        code = state.exit_code;
        state.changed.notify_all();
    }
    heartbeat.join();
    // The reader ends by itself after shutdown or a closed pipe; otherwise (the parent process ended
    // but its pipe end is still open somewhere) its blocking read is cancelled.
    state.reader_stopping = true;
    for (int attempt = 0; attempt < 200; ++attempt) {
        {
            std::unique_lock lock(state.mutex);
            if (state.changed.wait_for(lock, std::chrono::milliseconds(10), [&] { return state.reader_done; })) {
                break;
            }
        }
        ::CancelSynchronousIo(reader.native_handle());
    }
    bool reader_done = false;
    {
        std::lock_guard lock(state.mutex);
        reader_done = state.reader_done;
    }
    if (reader_done) {
        reader.join();
        return code;
    }
    // The reader still references this frame: never return under it.
    if (options.force_exit) {
        options.force_exit(code);
    } else {
        ::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(code));
    }
    reader.detach();  // reached only when a test's force_exit returns
    return code;
}

}  // namespace lucent::workers
