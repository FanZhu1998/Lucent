#include <lucent/workers/worker_group.hpp>

#include <format>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <utility>

#include <windows.h>

#include <lucent/core/log.hpp>
#include <lucent/core/win32_error.hpp>

#include "win32_util.hpp"

namespace lucent::workers {

namespace {

constexpr std::string_view kWhere = "workers.group";
using detail::UniqueHandle;

}  // namespace

struct WorkerGroup::State {
    std::uint64_t limit = 0;
    UniqueHandle job;
    UniqueHandle port;

    mutable std::mutex mutex;
    // (mutex) Live members by process id, with a SYNCHRONIZE handle to see them end.
    mutable std::map<std::uint32_t, UniqueHandle> members;
    std::set<std::uint32_t> hits;  // (mutex) members named by a job-memory-limit notification
    std::uint64_t notifications = 0;
    std::uint64_t joined = 0;
    std::uint64_t refused = 0;
    bool refusal_logged = false;

    // (mutex held) Takes every queued notification; a job-memory-limit message is kept for the
    // process it names until its host takes it or the process leaves (only workers are in the
    // group, and each host forgets its own process, so the set stays small).
    void drain() {
        DWORD message = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED overlapped = nullptr;
        while (::GetQueuedCompletionStatus(port.get(), &message, &key, &overlapped, 0)) {
            if (message == JOB_OBJECT_MSG_JOB_MEMORY_LIMIT) {
                ++notifications;
                try {
                    hits.insert(static_cast<std::uint32_t>(reinterpret_cast<ULONG_PTR>(overlapped)));
                } catch (const std::bad_alloc&) {
                    // Lost: the host falls back on the group's peak commit.
                }
            }
        }
    }

    // (mutex held) Drops members whose process has ended.
    void prune() const {
        for (auto it = members.begin(); it != members.end();) {
            if (::WaitForSingleObject(it->second.get(), 0) == WAIT_OBJECT_0) {
                it = members.erase(it);
            } else {
                ++it;
            }
        }
    }

    [[nodiscard]] std::uint64_t peak_commit() const noexcept {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        if (!::QueryInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits),
                                         nullptr)) {
            return 0;
        }
        return limits.PeakJobMemoryUsed;
    }
};

WorkerGroup::WorkerGroup(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

WorkerGroup::~WorkerGroup() = default;

Expected<std::shared_ptr<WorkerGroup>> WorkerGroup::create(std::uint64_t memory_limit_bytes) {
    if (memory_limit_bytes == 0) {
        return fail(ErrorCode::InvalidArgument, "a worker group needs a memory limit", std::string(kWhere));
    }
    try {
        auto state = std::make_unique<State>();
        state->limit = memory_limit_bytes;
        const auto win32_failure = [](std::string context) {
            const DWORD code = ::GetLastError();
            return Unexpected(core::win32_error(code, std::string(kWhere), std::move(context)));
        };
        state->job.reset(::CreateJobObjectW(nullptr, nullptr));
        if (!state->job) {
            return win32_failure("cannot create the group's Job Object");
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_JOB_MEMORY;
        limits.JobMemoryLimit = static_cast<SIZE_T>(memory_limit_bytes);
        if (!::SetInformationJobObject(state->job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            return win32_failure("cannot set the group's memory limit");
        }
        state->port.reset(::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
        if (!state->port) {
            return win32_failure("cannot create the group's completion port");
        }
        JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{};
        association.CompletionKey = state->job.get();
        association.CompletionPort = state->port.get();
        if (!::SetInformationJobObject(state->job.get(), JobObjectAssociateCompletionPortInformation, &association,
                                       sizeof(association))) {
            return win32_failure("cannot associate the group's completion port");
        }
        return std::shared_ptr<WorkerGroup>(new WorkerGroup(std::move(state)));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot create a worker group", std::string(kWhere));
    }
}

std::uint64_t WorkerGroup::memory_limit_bytes() const noexcept {
    return state_->limit;
}

WorkerGroupStats WorkerGroup::stats() const {
    State& s = *state_;
    std::lock_guard lock(s.mutex);
    s.drain();
    s.prune();
    WorkerGroupStats out;
    out.memory_limit_bytes = s.limit;
    out.peak_commit_bytes = s.peak_commit();
    out.limit_notifications = s.notifications;
    out.joined = s.joined;
    out.refused = s.refused;
    out.members = static_cast<std::uint32_t>(s.members.size());
    return out;
}

bool WorkerGroup::join(void* process, std::uint32_t pid) {
    State& s = *state_;
    std::lock_guard lock(s.mutex);
    s.drain();
    s.prune();
    if (!::AssignProcessToJobObject(s.job.get(), static_cast<HANDLE>(process))) {
        const DWORD code = ::GetLastError();
        ++s.refused;
        if (!s.refusal_logged) {
            s.refusal_logged = true;
            core::log_warn(kWhere,
                           "a worker could not join the group's Job Object (Win32 error {}); it runs with its own "
                           "memory limit only, without the aggregate limit of {} MiB",
                           code, s.limit >> 20);
        }
        return false;
    }
    HANDLE watch = nullptr;
    if (!::DuplicateHandle(::GetCurrentProcess(), static_cast<HANDLE>(process), ::GetCurrentProcess(), &watch,
                           SYNCHRONIZE, FALSE, 0)) {
        watch = nullptr;  // still a member of the Job Object; only its liveness cannot be watched
    }
    try {
        s.members[pid] = UniqueHandle(watch);
    } catch (const std::bad_alloc&) {
        // The process is in the Job Object (limited); only the company bookkeeping is missing.
    }
    s.hits.erase(pid);
    ++s.joined;
    return true;
}

WorkerGroup::Snapshot WorkerGroup::snapshot(std::uint32_t pid) const {
    State& s = *state_;
    std::lock_guard lock(s.mutex);
    s.drain();
    s.prune();
    Snapshot out;
    out.joined = s.joined;
    for (const auto& [member, handle] : s.members) {
        out.other_members += member != pid ? 1u : 0u;
    }
    out.peak_commit = s.peak_commit();
    return out;
}

bool WorkerGroup::take_limit_hit(std::uint32_t pid) {
    State& s = *state_;
    std::lock_guard lock(s.mutex);
    s.drain();
    return s.hits.erase(pid) > 0;
}

void WorkerGroup::leave(std::uint32_t pid) noexcept {
    State& s = *state_;
    std::lock_guard lock(s.mutex);
    s.members.erase(pid);
    s.hits.erase(pid);
}

}  // namespace lucent::workers
