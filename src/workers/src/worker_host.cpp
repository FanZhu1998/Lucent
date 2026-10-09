#include <lucent/workers/worker_host.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <windows.h>
#include <psapi.h>

#include <lucent/core/log.hpp>
#include <lucent/core/text.hpp>
#include <lucent/core/win32_error.hpp>
#include <lucent/workers/channel.hpp>
#include <lucent/workers/job_output.hpp>
#include <lucent/workers/worker_launch.hpp>

#include "win32_util.hpp"

namespace lucent::workers {

namespace {

using Clock = std::chrono::steady_clock;
using detail::UniqueHandle;

constexpr std::string_view kWhere = "workers.host";
constexpr std::string_view kWhereStart = "workers.host.start";
constexpr std::string_view kWhereHandshake = "workers.host.handshake";
constexpr std::chrono::milliseconds kPoll{20};
constexpr std::size_t kMaxQueuedEvents = 4096;
constexpr DWORD kPipeBufferBytes = 64 * 1024;
// Exit code given to a worker this host terminates (ERROR_PROCESS_ABORTED).
constexpr UINT kTerminatedExitCode = ERROR_PROCESS_ABORTED;
constexpr DWORD kTerminateWaitMs = 5000;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// Serialises the window in which the worker's handle ends are inheritable, so that two hosts
// starting workers at once never leak each other's pipe ends into the wrong child.
std::mutex& spawn_mutex() {
    static std::mutex mutex;
    return mutex;
}

struct ReaderEvent {
    std::optional<Message> message;
    std::optional<Error> failure;  // a read failure (Corrupt: the stream is not trustworthy)
    bool closed = false;           // the worker closed its end (it is ending)
};

// One worker process with its pipes, Job Object and reader thread.
struct Worker {
    UniqueHandle process;
    UniqueHandle job;
    UniqueHandle port;
    std::uint32_t pid = 0;
    std::unique_ptr<MessageChannel> channel;
    std::thread reader;

    std::mutex mutex;
    std::condition_variable changed;
    std::deque<ReaderEvent> events;
    bool reader_done = false;
    bool forward_heartbeats = false;
    Clock::time_point last_message = Clock::now();
    std::uint64_t peak_working_set = 0;
    std::atomic<bool> stopping{false};

    // Driving thread only.
    bool memory_limit_hit = false;
    bool grouped = false;  // a member of the host's WorkerGroup

    // The process's commit now (PrivateUsage); 0 when it cannot be read.
    [[nodiscard]] std::uint64_t commit() const noexcept {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (!::GetProcessMemoryInfo(process.get(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                    sizeof(counters))) {
            return 0;
        }
        return counters.PrivateUsage;
    }

    [[nodiscard]] bool exited() const noexcept {
        return ::WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0;
    }

    [[nodiscard]] std::optional<std::uint32_t> exit_code() const noexcept {
        DWORD code = 0;
        if (exited() && ::GetExitCodeProcess(process.get(), &code)) {
            return static_cast<std::uint32_t>(code);
        }
        return std::nullopt;
    }

    [[nodiscard]] bool alive() {
        std::lock_guard lock(mutex);
        return !reader_done && !exited();
    }

    void poll_port() noexcept {
        if (!port) {
            return;
        }
        DWORD message = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED overlapped = nullptr;
        while (::GetQueuedCompletionStatus(port.get(), &message, &key, &overlapped, 0)) {
            if (message == JOB_OBJECT_MSG_PROCESS_MEMORY_LIMIT || message == JOB_OBJECT_MSG_JOB_MEMORY_LIMIT) {
                memory_limit_hit = true;
            }
        }
    }

    [[nodiscard]] std::uint64_t peak_commit() const noexcept {
        if (!job) {
            return 0;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        if (!::QueryInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits),
                                         nullptr)) {
            return 0;
        }
        return limits.PeakProcessMemoryUsed;
    }

    void terminate() noexcept {
        if (exited()) {
            return;
        }
        if (job) {
            ::TerminateJobObject(job.get(), kTerminatedExitCode);
        } else {
            ::TerminateProcess(process.get(), kTerminatedExitCode);
        }
        ::WaitForSingleObject(process.get(), kTerminateWaitMs);
    }

    // Terminates the process if it still runs, then ends the reader thread. Afterwards no thread
    // references this object.
    void stop() noexcept {
        terminate();
        stopping = true;
        if (reader.joinable()) {
            // The pipe breaks when the process ends; a read that still blocks (its pipe end leaked
            // into another process) is cancelled.
            for (;;) {
                {
                    std::unique_lock lock(mutex);
                    if (changed.wait_for(lock, std::chrono::milliseconds(10), [&] { return reader_done; })) {
                        break;
                    }
                }
                ::CancelSynchronousIo(reader.native_handle());
            }
            reader.join();
        }
    }

    void read_loop() {
        for (;;) {
            auto received = channel->receive();
            std::lock_guard lock(mutex);
            if (!received) {
                if (!(stopping && received.error().code == ErrorCode::Cancelled)) {
                    events.push_back(ReaderEvent{std::nullopt, std::move(received.error()), false});
                }
                break;
            }
            if (!*received) {
                events.push_back(ReaderEvent{std::nullopt, std::nullopt, true});
                break;
            }
            last_message = Clock::now();
            if (const auto* heartbeat = std::get_if<Heartbeat>(&**received)) {
                peak_working_set = std::max(peak_working_set, heartbeat->memory.peak_working_set);
                if (!forward_heartbeats) {
                    continue;
                }
            }
            if (events.size() >= kMaxQueuedEvents) {
                events.push_back(ReaderEvent{
                    std::nullopt, Error{ErrorCode::Corrupt, "the worker sends faster than the host reads",
                                        "workers.protocol.flood", 0},
                    false});
                break;
            }
            events.push_back(ReaderEvent{std::move(**received), std::nullopt, false});
            changed.notify_all();
        }
        std::lock_guard lock(mutex);
        reader_done = true;
        changed.notify_all();
    }

    // Waits up to `wait` for events and takes them all.
    std::deque<ReaderEvent> take_events(std::chrono::milliseconds wait) {
        std::unique_lock lock(mutex);
        changed.wait_for(lock, wait, [&] { return !events.empty(); });
        std::deque<ReaderEvent> taken;
        taken.swap(events);
        return taken;
    }

    [[nodiscard]] Clock::time_point last_message_time() {
        std::lock_guard lock(mutex);
        return last_message;
    }
};

Expected<std::unique_ptr<Worker>> spawn(const WorkerHostOptions& options, const TestHooks& hooks,
                                        const std::filesystem::path& executable, DWORD& creation_flags) {
    const auto win32_failure = [](std::string context) {
        return Unexpected(core::last_error(std::string(kWhereStart), std::move(context)));
    };
    HANDLE handles[4] = {};
    if (!::CreatePipe(&handles[0], &handles[1], nullptr, kPipeBufferBytes)) {
        return win32_failure("cannot create the request pipe");
    }
    UniqueHandle request_read(handles[0]);
    UniqueHandle request_write(handles[1]);
    if (!::CreatePipe(&handles[2], &handles[3], nullptr, kPipeBufferBytes)) {
        return win32_failure("cannot create the response pipe");
    }
    UniqueHandle response_read(handles[2]);
    UniqueHandle response_write(handles[3]);
    HANDLE self = nullptr;
    if (!::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentProcess(), ::GetCurrentProcess(), &self, SYNCHRONIZE,
                           FALSE, 0)) {
        return win32_failure("cannot duplicate the host process handle");
    }
    UniqueHandle parent(self);

    WorkerLaunch launch;
    launch.request_handle = reinterpret_cast<std::uintptr_t>(request_read.get());
    launch.response_handle = reinterpret_cast<std::uintptr_t>(response_write.get());
    launch.parent_handle = reinterpret_cast<std::uintptr_t>(parent.get());
    launch.hooks = hooks;
    auto arguments = worker_arguments(launch);
    if (!arguments) {
        return Unexpected(std::move(arguments.error()));
    }
    // Windows file names cannot contain '"', so quoting the executable is enough.
    std::wstring command_line = L"\"" + executable.native() + L"\" " + *arguments;

    UniqueHandle job;
    UniqueHandle port;
    if (options.use_job_object) {
        job.reset(::CreateJobObjectW(nullptr, nullptr));
        if (!job) {
            return win32_failure("cannot create a Job Object");
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                                  JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
                                                  JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
        if (options.memory_limit_bytes != 0) {
            limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            limits.ProcessMemoryLimit = static_cast<SIZE_T>(options.memory_limit_bytes);
        }
        if (!::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            return win32_failure("cannot set the Job Object's limits");
        }
        port.reset(::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
        if (!port) {
            return win32_failure("cannot create a completion port");
        }
        JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{};
        association.CompletionKey = job.get();
        association.CompletionPort = port.get();
        if (!::SetInformationJobObject(job.get(), JobObjectAssociateCompletionPortInformation, &association,
                                       sizeof(association))) {
            return win32_failure("cannot associate the completion port");
        }
    }

    SIZE_T list_bytes = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &list_bytes);
    std::vector<std::byte> list_storage;
    try {
        list_storage.resize(list_bytes);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the attribute list", std::string(kWhereStart));
    }
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_storage.data());
    if (!::InitializeProcThreadAttributeList(list, 1, 0, &list_bytes)) {
        return win32_failure("cannot initialise the attribute list");
    }
    HANDLE inherited[3] = {request_read.get(), response_write.get(), parent.get()};
    if (!::UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
                                     nullptr, nullptr)) {
        auto failure = win32_failure("cannot limit the inherited handles");
        ::DeleteProcThreadAttributeList(list);
        return failure;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = list;
    // No standard handles: the worker talks through its pipes only, and nothing else of this process
    // (a console, redirected output) reaches it.
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    creation_flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
    if (options.below_normal_priority) {
        creation_flags |= BELOW_NORMAL_PRIORITY_CLASS;
    }
    const std::wstring directory = executable.parent_path().native();
    PROCESS_INFORMATION created{};
    BOOL started = FALSE;
    DWORD start_error = 0;
    {
        std::lock_guard lock(spawn_mutex());
        for (HANDLE handle : inherited) {
            ::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        }
        started = ::CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, TRUE, creation_flags,
                                   nullptr, directory.c_str(), &startup.StartupInfo, &created);
        start_error = started ? 0 : ::GetLastError();
        for (HANDLE handle : inherited) {
            ::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
        }
    }
    ::DeleteProcThreadAttributeList(list);
    if (!started) {
        return Unexpected(core::win32_error(
            start_error, std::string(kWhereStart),
            std::format("cannot start '{}'", core::path_to_utf8_lossy(executable))));
    }
    UniqueHandle process(created.hProcess);
    UniqueHandle thread(created.hThread);
    // The group's Job Object first, then the worker's own, which therefore nests inside it.
    const bool grouped = options.group && options.group->join(process.get(), created.dwProcessId);
    const auto abandon = [&] {
        ::TerminateProcess(process.get(), kTerminatedExitCode);
        if (grouped) {
            options.group->leave(created.dwProcessId);
        }
    };
    if (job && !::AssignProcessToJobObject(job.get(), process.get())) {
        auto failure = win32_failure("cannot assign the worker to its Job Object");
        abandon();
        return failure;
    }
    if (::ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        auto failure = win32_failure("cannot resume the worker");
        abandon();
        return failure;
    }
    // The worker holds its own copies of its ends now; closing ours lets each side see the other
    // end close.
    request_read.reset();
    response_write.reset();
    parent.reset();

    try {
        auto worker = std::make_unique<Worker>();
        worker->process = std::move(process);
        worker->job = std::move(job);
        worker->port = std::move(port);
        worker->pid = created.dwProcessId;
        worker->grouped = grouped;
        worker->channel = std::make_unique<MessageChannel>(response_read.get(), request_write.get(),
                                                           Direction::WorkerToHost);
        (void)response_read.release();
        (void)request_write.release();
        Worker* raw = worker.get();
        worker->reader = std::thread([raw] { raw->read_loop(); });
        return worker;
    } catch (const std::bad_alloc&) {
        if (grouped) {
            options.group->leave(created.dwProcessId);
        }
        return fail(ErrorCode::OutOfMemory, "cannot start the worker", std::string(kWhereStart));
    } catch (const std::system_error& e) {
        if (grouped) {
            options.group->leave(created.dwProcessId);
        }
        return fail(ErrorCode::Internal, std::format("cannot start the reader thread: {}", e.what()),
                    std::string(kWhereStart));
    }
}

std::string exit_text(std::optional<std::uint32_t> code) {
    return code ? std::format("exit code 0x{:08X}", *code) : std::string("exit code unknown");
}

FailureClass start_failure_class(const Error& error) noexcept {
    switch (error.code) {
        case ErrorCode::NotFound:
        case ErrorCode::InvalidArgument:
        case ErrorCode::VersionMismatch:
        case ErrorCode::PermissionDenied:
            return FailureClass::Deterministic;
        default:
            return FailureClass::Transient;
    }
}

}  // namespace

std::filesystem::path default_worker_executable() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return L"lucent-worker.exe";
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer).parent_path() / L"lucent-worker.exe";
}

std::string_view to_string(WorkerFault fault) noexcept {
    switch (fault) {
        case WorkerFault::None:
            return "none";
        case WorkerFault::StartFailed:
            return "start_failed";
        case WorkerFault::Crashed:
            return "crashed";
        case WorkerFault::Unresponsive:
            return "unresponsive";
        case WorkerFault::TimedOut:
            return "timed_out";
        case WorkerFault::CancelTimeout:
            return "cancel_timeout";
        case WorkerFault::MemoryLimit:
            return "memory_limit";
        case WorkerFault::ProtocolViolation:
            return "protocol_violation";
        case WorkerFault::GroupMemoryLimit:
            return "group_memory_limit";
    }
    return "unknown";
}

struct WorkerHost::State {
    WorkerHostOptions options;
    std::unique_ptr<Worker> worker;
    std::uint64_t next_nonce = 1;
    mutable std::mutex info_mutex;
    WorkerInfo info;

    void set_stopped() {
        std::lock_guard lock(info_mutex);
        info.running = false;
        info.grouped = false;
        info.idle_commit_bytes = 0;
    }

    // Ends the current worker (if any) and forgets it.
    void discard_worker() {
        if (worker) {
            const std::uint64_t peak = worker->peak_commit();
            worker->stop();
            if (worker->grouped && options.group) {
                options.group->leave(worker->pid);
            }
            {
                std::lock_guard lock(info_mutex);
                info.peak_commit_bytes = std::max(info.peak_commit_bytes, peak);
            }
            worker.reset();
        }
        set_stopped();
    }

    // The group's aggregate limit stopped the current worker during the job that started with
    // `start` (see the file comment): the group's notification naming the worker or, with
    // `failed_for_memory` (the job ended with an OutOfMemory, or the worker ended), the group's peak
    // commit rising to within 5 % of the limit during the job. `shared`: another worker of the group
    // was alive at some time during the job. Consumes the notification: call once per job.
    struct GroupHit {
        bool shared = false;
    };
    [[nodiscard]] std::optional<GroupHit> group_limit(const std::optional<WorkerGroup::Snapshot>& start,
                                                      bool failed_for_memory) {
        if (!start || !options.group || !worker || !worker->grouped) {
            return std::nullopt;
        }
        const bool notified = options.group->take_limit_hit(worker->pid);
        const WorkerGroup::Snapshot end = options.group->snapshot(worker->pid);
        const std::uint64_t limit = options.group->memory_limit_bytes();
        const bool near_limit = end.peak_commit > start->peak_commit && end.peak_commit >= limit - limit / 20;
        if (!notified && !(failed_for_memory && near_limit)) {
            return std::nullopt;
        }
        GroupHit hit;
        hit.shared = start->other_members > 0 || end.other_members > 0 || end.joined > start->joined;
        return hit;
    }

    [[nodiscard]] JobFailure group_failure(const GroupHit& hit, const std::string& what) const {
        JobFailure failure;
        const std::uint64_t limit_mib = options.group->memory_limit_bytes() >> 20;
        if (hit.shared) {
            // Which worker an aggregate limit stops is whichever allocates at it: alone, this job
            // may well fit.
            failure.error = Error{ErrorCode::OutOfMemory,
                                  std::format("the workers together reached their aggregate memory limit of {} MiB "
                                              "while another worker held memory: {}",
                                              limit_mib, what),
                                  "workers.worker.group_memory_limit", 0};
            failure.failure_class = FailureClass::Transient;
            failure.fault = WorkerFault::GroupMemoryLimit;
        } else {
            failure.error = Error{ErrorCode::LimitExceeded,
                                  std::format("the worker alone exceeded the workers' aggregate memory limit of {} "
                                              "MiB: {}",
                                              limit_mib, what),
                                  "workers.worker.memory_limit", 0};
            failure.failure_class = FailureClass::Deterministic;
            failure.fault = WorkerFault::MemoryLimit;
        }
        return failure;
    }

    Status validate_options(TestHooks& hooks) const {
        const auto positive = [](std::chrono::milliseconds value) { return value.count() > 0; };
        if (!positive(options.handshake_timeout) || !positive(options.heartbeat_timeout) ||
            !positive(options.cancel_grace) || !positive(options.shutdown_grace) ||
            options.heartbeat_interval.count() < kMinHeartbeatMs ||
            options.heartbeat_interval.count() > kMaxHeartbeatMs ||
            options.heartbeat_timeout <= options.heartbeat_interval) {
            return fail(ErrorCode::InvalidArgument,
                        "the worker timeouts must be positive and the heartbeat timeout longer than its interval",
                        std::string(kWhereStart));
        }
        for (const std::string& spec : options.test_hooks) {
            if (auto applied = apply_test_hook(spec, hooks); !applied) {
                return applied;
            }
        }
        return ok();
    }

    Status handshake(Worker& w) {
        HostHello hello;
        hello.build = current_build_identity();
        hello.heartbeat_ms = static_cast<std::uint32_t>(options.heartbeat_interval.count());
        hello.host_pid = static_cast<std::uint32_t>(::GetCurrentProcessId());
        if (auto sent = w.channel->send(hello); !sent) {
            return fail(ErrorCode::IoError,
                        std::format("the worker ended before the handshake ({})", exit_text(w.exit_code())),
                        std::string(kWhereHandshake));
        }
        const auto deadline = Clock::now() + options.handshake_timeout;
        while (Clock::now() < deadline) {
            for (ReaderEvent& event : w.take_events(kPoll)) {
                if (event.closed || event.failure) {
                    if (event.failure && event.failure->code == ErrorCode::Corrupt) {
                        return Unexpected(std::move(*event.failure));
                    }
                    ::WaitForSingleObject(w.process.get(), kTerminateWaitMs);
                    return fail(ErrorCode::IoError,
                                std::format("the worker ended during the handshake ({})",
                                            exit_text(w.exit_code())),
                                std::string(kWhereHandshake));
                }
                if (auto* refusal = std::get_if<JobError>(&*event.message); refusal && !refusal->job_id) {
                    return Unexpected(std::move(refusal->error));
                }
                auto* answer = std::get_if<WorkerHello>(&*event.message);
                if (answer == nullptr) {
                    return fail(ErrorCode::Corrupt,
                                std::format("the worker answered the handshake with '{}'", type_name(*event.message)),
                                "workers.protocol.sequence");
                }
                if (answer->version < kMinProtocolVersion || answer->version > kMaxProtocolVersion) {
                    return fail(ErrorCode::VersionMismatch,
                                std::format("the worker chose protocol version {}, this host speaks {}..{}",
                                            answer->version, kMinProtocolVersion, kMaxProtocolVersion),
                                "workers.handshake.version");
                }
                if (auto same = check_same_build(hello.build, answer->build, "worker"); !same) {
                    return same;
                }
                std::lock_guard lock(info_mutex);
                info.pid = w.pid;
                info.protocol_version = answer->version;
                info.build = answer->build;
                info.console_window = answer->console_window;
                info.test_hooks = answer->test_hooks;
                return ok();
            }
            if (w.exited()) {
                // Messages written before the exit are still in the pipe; the reader delivers them
                // before the close, so keep reading until the closed event arrives.
                continue;
            }
        }
        return fail(ErrorCode::Timeout,
                    std::format("the worker did not answer the handshake within {} ms",
                                options.handshake_timeout.count()),
                    std::string(kWhereHandshake));
    }

    Status start() {
        if (worker && worker->alive()) {
            return ok();
        }
        discard_worker();
        TestHooks hooks;
        if (auto valid = validate_options(hooks); !valid) {
            return valid;
        }
        const std::filesystem::path executable =
            options.executable.empty() ? default_worker_executable() : options.executable;
        if (::GetFileAttributesW(detail::verbatim(executable).c_str()) == INVALID_FILE_ATTRIBUTES) {
            return fail(ErrorCode::NotFound,
                        std::format("the worker '{}' does not exist", core::path_to_utf8_lossy(executable)),
                        std::string(kWhereStart));
        }
        const auto started_at = Clock::now();
        DWORD flags = 0;
        auto spawned = spawn(options, hooks, executable, flags);
        if (!spawned) {
            return Unexpected(std::move(spawned.error()));
        }
        worker = std::move(*spawned);
        {
            std::lock_guard lock(info_mutex);
            info = WorkerInfo{.starts = info.starts + 1, .jobs = info.jobs};
            info.creation_flags = flags;
            info.pid = worker->pid;
            info.grouped = worker->grouped;
        }
        if (auto shaken = handshake(*worker); !shaken) {
            discard_worker();
            return shaken;
        }
        const std::uint64_t idle = worker->commit();
        std::lock_guard lock(info_mutex);
        info.running = true;
        info.last_start_seconds = seconds_since(started_at);
        info.idle_commit_bytes = idle;
        return ok();
    }

    // Classifies the end of a worker that went away during a job (`group`: the group-limit evidence,
    // asked for once the process ended).
    JobFailure lost(std::string_view job_id, std::optional<Error> protocol_error,
                    const std::function<std::optional<GroupHit>()>& group) {
        JobFailure failure;
        if (protocol_error) {
            failure.error = std::move(*protocol_error);
            failure.failure_class = FailureClass::Deterministic;
            failure.fault = WorkerFault::ProtocolViolation;
            return failure;
        }
        ::WaitForSingleObject(worker->process.get(), kTerminateWaitMs);
        worker->poll_port();
        failure.exit_code = worker->exit_code();
        if (memory_limit_reached()) {
            failure.error = memory_limit_error(
                std::format("the worker ended ({}) running job '{}'", exit_text(failure.exit_code), job_id));
            failure.failure_class = FailureClass::Deterministic;
            failure.fault = WorkerFault::MemoryLimit;
            return failure;
        }
        if (const std::optional<GroupHit> hit = group()) {
            JobFailure grouped = group_failure(
                *hit, std::format("the worker ended ({}) running job '{}'", exit_text(failure.exit_code), job_id));
            grouped.exit_code = failure.exit_code;
            return grouped;
        }
        if (failure.exit_code == static_cast<std::uint32_t>(WorkerExitCode::ProtocolViolation)) {
            failure.error = Error{ErrorCode::Corrupt,
                                  std::format("the worker refused a message of job '{}' as a protocol violation",
                                              job_id),
                                  "workers.protocol.refused", 0};
            failure.failure_class = FailureClass::Deterministic;
            failure.fault = WorkerFault::ProtocolViolation;
            return failure;
        }
        failure.error = Error{ErrorCode::Internal,
                              std::format("the worker process ended unexpectedly ({}) running job '{}'",
                                          exit_text(failure.exit_code), job_id),
                              "workers.worker.crashed",
                              failure.exit_code ? static_cast<std::int64_t>(*failure.exit_code) : 0};
        failure.failure_class = FailureClass::Transient;
        failure.fault = WorkerFault::Crashed;
        return failure;
    }

    [[nodiscard]] bool memory_limit_reached() const {
        if (options.memory_limit_bytes == 0 || !worker) {
            return false;
        }
        if (worker->memory_limit_hit) {
            return true;
        }
        // Delivery of Job Object messages is not guaranteed: a peak within 5 % of the limit counts.
        const std::uint64_t peak = worker->peak_commit();
        return peak >= options.memory_limit_bytes - options.memory_limit_bytes / 20;
    }

    [[nodiscard]] Error memory_limit_error(std::string what) const {
        return Error{ErrorCode::LimitExceeded,
                     std::format("the worker exceeded its memory limit of {} MiB: {}",
                                 options.memory_limit_bytes >> 20, what),
                     "workers.worker.memory_limit", 0};
    }

    // An export job (JobRequest -> JobOutcome) or an analyze job (AnalysisRequest ->
    // AnalysisOutcome): the same supervision.
    template <class Outcome, class Request>
    Outcome run_request(const Request& request, const JobCallbacks& callbacks, const core::CancellationToken& cancel,
                        std::chrono::milliseconds timeout);
};

namespace {

// The protocol violation when a job's answer names anything but the job's own output, recipe and
// source (the caller publishes or records what the answer describes); std::nullopt when it matches.
std::optional<Error> mismatch(const JobRequest& request, const JobResult& result) {
    if (result.output.temp_path != request.temp_path || result.recipe_sha256 != request.recipe.sha256 ||
        result.source_identity != request.source.identity) {
        return Error{ErrorCode::Corrupt,
                     std::format("the result of job '{}' names another output, recipe or source than the job",
                                 request.job_id),
                     "workers.protocol.result", 0};
    }
    return std::nullopt;
}

std::optional<Error> mismatch(const AnalysisRequest& request, const AnalysisResult& result) {
    if (result.recipe_sha256 != request.recipe.sha256 || result.source_identity != request.source.identity) {
        return Error{ErrorCode::Corrupt,
                     std::format("the analysis of job '{}' names another recipe or source than the job", request.job_id),
                     "workers.protocol.result", 0};
    }
    return std::nullopt;
}

}  // namespace

template <class Outcome, class Request>
Outcome WorkerHost::State::run_request(const Request& request, const JobCallbacks& callbacks,
                                       const core::CancellationToken& cancel, std::chrono::milliseconds timeout) {
    using Result = typename decltype(Outcome::result)::value_type;
    const auto entered = Clock::now();
    Outcome outcome;
    const auto finish = [&](Outcome& done) -> Outcome {
        done.metrics.wall_seconds = seconds_since(entered);
        return std::move(done);
    };

    if (auto encoded = encode_message(Message(request)); !encoded) {
        outcome.failure = JobFailure{std::move(encoded.error()), FailureClass::Deterministic, WorkerFault::None};
        return finish(outcome);
    }
    if (!worker || !worker->alive()) {
        const auto starting = Clock::now();
        if (auto started = start(); !started) {
            const FailureClass failure_class = start_failure_class(started.error());
            outcome.failure = JobFailure{std::move(started.error()), failure_class, WorkerFault::StartFailed};
            return finish(outcome);
        }
        outcome.metrics.start_seconds = seconds_since(starting);
    }
    Worker& w = *worker;
    {
        std::lock_guard lock(w.mutex);
        // Late messages of an earlier job are of no interest; a close or failure is kept.
        std::erase_if(w.events, [](const ReaderEvent& e) { return e.message.has_value(); });
        w.forward_heartbeats = true;
    }
    w.poll_port();
    w.memory_limit_hit = false;
    // The group as the job starts (its company and peak commit); a notification left from before
    // belongs to no job of this worker.
    std::optional<WorkerGroup::Snapshot> group_start;
    if (options.group && w.grouped) {
        (void)options.group->take_limit_hit(w.pid);
        group_start = options.group->snapshot(w.pid);
    }
    // The group-limit evidence, evaluated once (it consumes the group's notification).
    std::optional<GroupHit> group_hit;
    bool group_checked = false;
    const auto check_group = [&](bool failed_for_memory) {
        if (!group_checked) {
            group_checked = true;
            group_hit = group_limit(group_start, failed_for_memory);
        }
        return group_hit;
    };
    {
        std::lock_guard lock(info_mutex);
        ++info.jobs;
    }

    enum class CancelReason { None, Requested, Timeout };
    CancelReason cancel_reason = CancelReason::None;
    Clock::time_point cancel_deadline{};
    std::optional<Error> protocol_error;
    bool gone = false;
    bool answered = false;
    const auto dispatched = Clock::now();

    if (auto sent = w.channel->send(Message(request)); !sent) {
        if (!is_channel_closed(sent.error())) {
            core::log_warn(kWhere, "cannot send job '{}': {}", request.job_id, sent.error().describe());
        }
        gone = true;
    }

    const auto send_cancel = [&](CancelReason reason) {
        cancel_reason = reason;
        cancel_deadline = Clock::now() + options.cancel_grace;
        if (auto sent = w.channel->send(CancelJob{request.job_id}); !sent && !is_channel_closed(sent.error())) {
            core::log_warn(kWhere, "cannot cancel job '{}': {}", request.job_id, sent.error().describe());
        }
    };

    while (!gone && !answered) {
        for (ReaderEvent& event : w.take_events(kPoll)) {
            if (answered || gone) {
                break;
            }
            if (event.closed || event.failure) {
                if (event.failure && event.failure->code == ErrorCode::Corrupt) {
                    protocol_error = std::move(event.failure);
                }
                gone = true;
                break;
            }
            Message& message = *event.message;
            if (auto* progress = std::get_if<JobProgress>(&message)) {
                if (progress->job_id == request.job_id && callbacks.progress) {
                    callbacks.progress(*progress);
                }
            } else if (auto* heartbeat = std::get_if<Heartbeat>(&message)) {
                outcome.metrics.peak_working_set_bytes =
                    std::max(outcome.metrics.peak_working_set_bytes, heartbeat->memory.peak_working_set);
                if (callbacks.heartbeat) {
                    callbacks.heartbeat(*heartbeat);
                }
            } else if (auto* result = std::get_if<Result>(&message)) {
                if (result->job_id != request.job_id) {
                    continue;  // a late answer to an earlier job
                }
                // The caller publishes or records what the answer describes: an answer about anything
                // else is never trusted.
                if (auto wrong = mismatch(request, *result)) {
                    protocol_error = std::move(*wrong);
                    gone = true;
                    break;
                }
                outcome.metrics.peak_working_set_bytes =
                    std::max(outcome.metrics.peak_working_set_bytes, result->memory.peak_working_set);
                outcome.result = std::move(*result);
                answered = true;
            } else if (auto* error = std::get_if<JobError>(&message)) {
                if (!error->job_id) {
                    protocol_error = std::move(error->error);  // fatal: the worker is leaving
                    gone = true;
                } else if (*error->job_id == request.job_id) {
                    JobFailure failure{std::move(error->error), error->failure_class, WorkerFault::None};
                    if (cancel_reason == CancelReason::Timeout && failure.error.code == ErrorCode::Cancelled) {
                        failure.error = Error{ErrorCode::Timeout,
                                              std::format("job '{}' exceeded its {} ms timeout and was cancelled",
                                                          request.job_id, timeout.count()),
                                              "workers.job.timeout", 0};
                        failure.failure_class = FailureClass::Transient;
                    }
                    outcome.failure = std::move(failure);
                    answered = true;
                }
            } else if (!std::holds_alternative<Pong>(message)) {
                protocol_error = Error{ErrorCode::Corrupt,
                                       std::format("unexpected '{}' message during a job", type_name(message)),
                                       "workers.protocol.sequence", 0};
                gone = true;
            }
        }
        if (answered || gone) {
            break;
        }
        w.poll_port();
        if (w.exited()) {
            // Anything the worker wrote before it ended is still delivered; the closed event follows.
            continue;
        }
        const auto now = Clock::now();
        if (now - w.last_message_time() > options.heartbeat_timeout) {
            w.terminate();
            outcome.failure = JobFailure{
                Error{ErrorCode::Timeout,
                      std::format("the worker sent nothing for {} ms during job '{}' and was terminated",
                                  options.heartbeat_timeout.count(), request.job_id),
                      "workers.worker.unresponsive", 0},
                FailureClass::Transient, WorkerFault::Unresponsive};
            break;
        }
        if (cancel_reason == CancelReason::None) {
            if (cancel.is_cancelled()) {
                send_cancel(CancelReason::Requested);
            } else if (timeout.count() > 0 && now - dispatched > timeout) {
                send_cancel(CancelReason::Timeout);
            }
        } else if (now > cancel_deadline) {
            w.terminate();
            if (cancel_reason == CancelReason::Requested) {
                outcome.failure = JobFailure{
                    Error{ErrorCode::Cancelled,
                          std::format("job '{}' did not stop within {} ms of its cancellation; the worker was "
                                      "terminated",
                                      request.job_id, options.cancel_grace.count()),
                          "workers.job.cancelled", 0},
                    FailureClass::Cancelled, WorkerFault::CancelTimeout};
            } else {
                outcome.failure = JobFailure{
                    Error{ErrorCode::Timeout,
                          std::format("job '{}' exceeded its {} ms timeout and did not stop; the worker was terminated",
                                      request.job_id, timeout.count()),
                          "workers.job.timeout", 0},
                    FailureClass::Transient, WorkerFault::TimedOut};
            }
            break;
        }
    }

    if (gone && !outcome.failure) {
        if (protocol_error) {
            w.terminate();
        }
        outcome.failure = lost(request.job_id, std::move(protocol_error), [&] { return check_group(true); });
    }
    w.poll_port();
    {
        std::lock_guard lock(w.mutex);
        w.forward_heartbeats = false;
        outcome.metrics.peak_working_set_bytes = std::max(outcome.metrics.peak_working_set_bytes, w.peak_working_set);
    }
    outcome.metrics.peak_commit_bytes = w.peak_commit();
    {
        std::lock_guard lock(info_mutex);
        info.peak_commit_bytes = std::max(info.peak_commit_bytes, outcome.metrics.peak_commit_bytes);
    }

    // A worker that reported OutOfMemory at its limit is recycled like one that ended at it; at its
    // group's limit, classified by its company.
    if (outcome.failure && outcome.failure->fault == WorkerFault::None &&
        outcome.failure->error.code == ErrorCode::OutOfMemory) {
        if (memory_limit_reached()) {
            outcome.failure->error = memory_limit_error(outcome.failure->error.describe());
            outcome.failure->failure_class = FailureClass::Deterministic;
            outcome.failure->fault = WorkerFault::MemoryLimit;
        } else if (const std::optional<GroupHit> hit = check_group(true)) {
            outcome.failure = group_failure(*hit, outcome.failure->error.describe());
        }
    }
    // A group-limit notification about a job that ended otherwise still recycles the worker.
    (void)check_group(false);
    const bool recycle = memory_limit_reached() || group_hit.has_value() ||
                         (outcome.failure && outcome.failure->fault != WorkerFault::None);
    if (recycle) {
        if (outcome.failure && !outcome.failure->exit_code && worker->exited()) {
            outcome.failure->exit_code = worker->exit_code();
        }
        discard_worker();
        if constexpr (std::is_same_v<Request, JobRequest>) {
            if (outcome.failure) {
                if (auto removed = remove_job_output(request.temp_path); removed) {
                    outcome.failure->output_removed = true;
                } else {
                    core::log_warn(kWhere, "cannot remove the output of job '{}': {}", request.job_id,
                                   removed.error().describe());
                }
            }
        }
    } else if (worker && worker->alive()) {
        // What the worker keeps while idle counts against its group's limit (the scheduler's
        // admission looks at it).
        const std::uint64_t idle = worker->commit();
        std::lock_guard lock(info_mutex);
        info.idle_commit_bytes = idle;
    }
    return finish(outcome);
}

WorkerHost::WorkerHost(WorkerHostOptions options) : state_(std::make_unique<State>()) {
    state_->options = std::move(options);
}

WorkerHost::~WorkerHost() { shutdown(); }

Status WorkerHost::start() { return state_->start(); }

JobOutcome WorkerHost::run_job(const JobRequest& request, const JobCallbacks& callbacks,
                               const core::CancellationToken& cancel, std::chrono::milliseconds timeout) {
    return state_->run_request<JobOutcome>(request, callbacks, cancel, timeout);
}

AnalysisOutcome WorkerHost::run_analysis(const AnalysisRequest& request, const JobCallbacks& callbacks,
                                         const core::CancellationToken& cancel, std::chrono::milliseconds timeout) {
    return state_->run_request<AnalysisOutcome>(request, callbacks, cancel, timeout);
}

Status WorkerHost::ping(std::chrono::milliseconds timeout) {
    State& s = *state_;
    if (!s.worker || !s.worker->alive()) {
        return fail(ErrorCode::Internal, "no worker is running", std::string(kWhere));
    }
    Worker& w = *s.worker;
    const std::uint64_t nonce = s.next_nonce++;
    if (auto sent = w.channel->send(Ping{nonce}); !sent) {
        s.discard_worker();
        return fail(ErrorCode::IoError, "the worker is gone", std::string(kWhere));
    }
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        for (ReaderEvent& event : w.take_events(kPoll)) {
            if (event.closed || event.failure) {
                s.discard_worker();
                return fail(ErrorCode::IoError, "the worker ended", std::string(kWhere));
            }
            if (const auto* pong = std::get_if<Pong>(&*event.message); pong && pong->nonce == nonce) {
                return ok();
            }
        }
    }
    s.discard_worker();
    return fail(ErrorCode::Timeout, std::format("the worker did not answer a ping within {} ms", timeout.count()),
                std::string(kWhere));
}

void WorkerHost::shutdown() {
    State& s = *state_;
    if (!s.worker) {
        return;
    }
    Worker& w = *s.worker;
    if (!w.exited()) {
        (void)w.channel->send(Shutdown{});
        w.channel->close_write();
        const auto grace = static_cast<DWORD>(std::clamp<long long>(s.options.shutdown_grace.count(), 0, 600'000));
        ::WaitForSingleObject(w.process.get(), grace);
    }
    s.discard_worker();
}

WorkerInfo WorkerHost::info() const {
    std::lock_guard lock(state_->info_mutex);
    return state_->info;
}

}  // namespace lucent::workers
