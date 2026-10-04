#pragma once

// The command line of lucent-worker.exe and its test hooks. The host builds it (worker_host.hpp)
// and the worker parses it (apps/lucent-worker); both use these functions so they never disagree.
//
//   lucent-worker.exe --lucent-worker --request-handle=<n> --response-handle=<n>
//                     [--parent-handle=<n>] [--test-hook=<spec>]...
//
// Handle values are decimal HANDLE values inherited by the worker (the host limits inheritance to
// exactly these handles with PROC_THREAD_ATTRIBUTE_HANDLE_LIST). Anything else on the command line
// is a usage error: a user who starts lucent-worker.exe by hand gets WorkerExitCode::Usage and
// nothing happens (the worker has no window or console to report it in).
//
// Test hooks exist so that tests can drive a real worker process into the failures the host must
// survive. They are inert unless named on the command line, which only WorkerHostOptions::
// test_hooks does; the worker lists the active ones in its worker_hello. Specs:
//   hang-job               the job thread blocks forever when a job starts, ignoring cancellation;
//                          heartbeats continue (job timeout and cancel-grace paths)
//   freeze                 when a job starts, heartbeats and pongs stop and the job blocks forever
//                          (an unresponsive process; heartbeat-timeout path)
//   crash-job              the job thread raises an access violation when a job starts
//   allocate:<bytes>       a job first allocates and touches <bytes> (memory-limit path), then
//                          frees them and runs normally
//   render-delay:<ms>      every render progress step waits <ms>, observing cancellation (makes a
//                          small render long enough to cancel mid-render)
//   fake-commit:<text>     the worker claims a build with git commit <text> (build mismatch)
//   protocol:<min>-<max>   the worker claims to speak protocol versions <min>..<max>
// <text> is 1..64 characters of [A-Za-z0-9._-]; numbers are decimal.
//
// Thread-safety: pure functions.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lucent/core/error.hpp>

namespace lucent::workers {

inline constexpr std::wstring_view kWorkerModeArgument = L"--lucent-worker";

struct TestHooks {
    bool hang_job = false;
    bool freeze = false;
    bool crash_job = false;
    std::uint64_t allocate_bytes = 0;
    std::uint32_t render_delay_ms = 0;
    std::optional<std::string> fake_commit;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> protocol_range;

    [[nodiscard]] bool any() const noexcept;
    // The canonical spec of every active hook, in the order listed above.
    [[nodiscard]] std::vector<std::string> specs() const;
};

// Adds the hook named by `spec` to `hooks`. InvalidArgument ("workers.launch.hook") for an unknown
// hook, a malformed value, an allocation above 1 TiB, a delay above 10 minutes, or a protocol range
// that starts at 0 or is empty.
[[nodiscard]] Status apply_test_hook(std::string_view spec, TestHooks& hooks);

struct WorkerLaunch {
    std::uintptr_t request_handle = 0;   // the pipe end the worker reads (host -> worker)
    std::uintptr_t response_handle = 0;  // the pipe end the worker writes (worker -> host)
    std::uintptr_t parent_handle = 0;    // a SYNCHRONIZE handle of the host process; 0: none
    TestHooks hooks;
};

// The arguments after the executable name for `launch` (handles must be non-zero, except the
// parent handle). InvalidArgument for zero handles.
[[nodiscard]] Expected<std::wstring> worker_arguments(const WorkerLaunch& launch);

// Parses the arguments after the executable name (argv[1..]). InvalidArgument ("workers.launch")
// for a missing --lucent-worker, a missing or zero request or response handle, a repeated or
// unknown argument, or an invalid test hook.
[[nodiscard]] Expected<WorkerLaunch> parse_worker_arguments(std::span<const std::wstring> arguments);

}  // namespace lucent::workers
