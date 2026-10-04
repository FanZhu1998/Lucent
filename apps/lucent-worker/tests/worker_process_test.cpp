// The real lucent-worker.exe driven by WorkerHost (worker_host.hpp): start without a console,
// handshake and mismatches, jobs on synthetic DNGs, cancellation mid-render, a worker killed or
// crashing mid-job, a hung worker, a job timeout, the memory limit, restart, shutdown, and a worker
// whose host process dies.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <windows.h>

#include <lucent/application/auto_develop.hpp>
#include <lucent/workers/job_output.hpp>
#include <lucent/workers/worker_host.hpp>

#include "worker_test_support.hpp"

namespace lucent::worker_app {
namespace {

using namespace test_support;
using workers::FailureClass;
using workers::JobOutcome;
using workers::WorkerFault;
using workers::WorkerHost;

class WorkerProcess : public ::testing::Test {
protected:
    WorkerProcess() {
        source_ = write_dng(dir_, L"IMG_0001.dng");
        out_ = dir_.path() / L"out";
        rts::make_directories(out_);
    }

    std::filesystem::path temp(int n) const { return out_ / (L"~lucent-job-" + std::to_wstring(n) + L".partial"); }

    workers::JobRequest job(int n) const { return make_job(source_, temp(n), "job-" + std::to_string(n)); }

    static void expect_success(const JobOutcome& outcome) {
        ASSERT_TRUE(outcome.succeeded()) << outcome.failure->error.describe() << " fault "
                                         << workers::to_string(outcome.failure->fault);
    }

    // Opens the worker process (so its exit code stays readable) and kills it.
    static void kill_worker(std::uint32_t pid, UINT code) {
        HANDLE process = ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        ASSERT_NE(process, nullptr);
        ASSERT_TRUE(::TerminateProcess(process, code));
        ::WaitForSingleObject(process, 5000);
        ::CloseHandle(process);
    }

    rts::ScratchDir dir_;
    std::filesystem::path source_;
    std::filesystem::path out_;
};

// An analyze job (protocol version 2) for `source` starting from as-shot settings.
workers::AnalysisRequest analyze_job(const std::filesystem::path& source, std::string job_id,
                                     const app::AutoRunRequest& auto_request = {}) {
    const workers::JobRequest job = make_job(source, source.parent_path() / L"unused.partial", job_id);
    workers::AnalysisRequest request;
    request.job_id = std::move(job_id);
    request.source = job.source;
    request.recipe = job.recipe;
    auto text = app::write_auto_request(auto_request);
    EXPECT_TRUE(text);
    request.request = text ? *text : std::string("{}");
    return request;
}

// A dark synthetic photo with a noise model that Natural brightens (auto_service_test.cpp).
rts::SyntheticDngSpec dark_spec() {
    rts::SyntheticDngSpec spec;
    spec.width = 256;
    spec.height = 192;
    spec.white_level = 65535;
    spec.raw_noise_profile = {2e-5, 2e-7, 2e-5, 2e-7, 2e-5, 2e-7};
    return spec;
}

TEST_F(WorkerProcess, AnalyzesAPhotoAndWalksTheFallbacksOfAFailedCheck) {
    const auto dark = write_dng(dir_, L"dark.dng", dark_spec());
    WorkerHost host(host_options({"fail-final-check:1"}));
    std::vector<workers::JobProgress> progress;
    workers::JobCallbacks callbacks;
    callbacks.progress = [&](const workers::JobProgress& p) { progress.push_back(p); };
    const workers::AnalysisRequest request = analyze_job(dark, "auto-1");
    const workers::AnalysisOutcome outcome = host.run_analysis(request, callbacks);
    ASSERT_TRUE(outcome.succeeded()) << outcome.failure->error.describe();
    EXPECT_EQ(outcome.result->recipe_sha256, request.recipe.sha256);
    EXPECT_EQ(outcome.result->source_identity, request.source.identity);
    auto run = app::read_auto_run(outcome.result->document);
    ASSERT_TRUE(run) << run.error().describe();
    EXPECT_GT(run->chosen.exposure_ev, 0.0);
    ASSERT_GE(run->attempts.size(), 2u);
    EXPECT_TRUE(run->attempts[0].check.forced);
    EXPECT_EQ(run->attempts[0].check.outcome, catalog::AutoCheckOutcome::Failed);
    ASSERT_TRUE(run->effective);
    EXPECT_EQ(*run->effective, 1u);
    bool analysing = false;
    bool checking = false;
    for (const workers::JobProgress& p : progress) {
        analysing = analysing || p.stage == workers::JobStage::Analysing;
        checking = checking || p.stage == workers::JobStage::Checking;
    }
    EXPECT_TRUE(analysing && checking);

    // The same worker exports afterwards: one process serves both kinds of job.
    expect_success(host.run_job(job(1)));
    EXPECT_EQ(host.info().starts, 1u);

    // Deterministic: a second analysis decides the same.
    const workers::AnalysisOutcome again = host.run_analysis(analyze_job(dark, "auto-2"));
    ASSERT_TRUE(again.succeeded());
    auto second = app::read_auto_run(again.result->document);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->effective_settings(), run->effective_settings());
    EXPECT_EQ(second->confidence, run->confidence);
}

TEST_F(WorkerProcess, AnAnalysisOfAChangedSourceOrUnreadableRequestFails) {
    const auto dark = write_dng(dir_, L"dark.dng", dark_spec());
    WorkerHost host(host_options());
    workers::AnalysisRequest request = analyze_job(dark, "auto-1");
    request.request = R"({"schema":"lucent.auto-request","version":99})";
    const workers::AnalysisOutcome unreadable = host.run_analysis(request);
    ASSERT_TRUE(unreadable.failure);
    EXPECT_EQ(unreadable.failure->error.where, "worker.analysis.request");
    EXPECT_EQ(unreadable.failure->failure_class, FailureClass::Deterministic);
    request = analyze_job(dark, "auto-2");
    request.source.identity.size_bytes += 1;
    const workers::AnalysisOutcome changed = host.run_analysis(request);
    ASSERT_TRUE(changed.failure);
    EXPECT_EQ(changed.failure->error.code, ErrorCode::SourceChanged);
    EXPECT_EQ(changed.failure->fault, WorkerFault::None) << "the worker stays usable";
}

TEST_F(WorkerProcess, AnAnalysisIsCancelledPromptly) {
    const auto dark = write_dng(dir_, L"dark.dng", dark_spec());
    WorkerHost host(host_options({"render-delay:5000"}));
    core::CancellationSource cancel;
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cancel.cancel();
    });
    const auto started = std::chrono::steady_clock::now();
    const workers::AnalysisOutcome outcome = host.run_analysis(analyze_job(dark, "auto-1"), {}, cancel.token());
    canceller.join();
    ASSERT_TRUE(outcome.failure);
    EXPECT_EQ(outcome.failure->failure_class, FailureClass::Cancelled);
    EXPECT_EQ(outcome.failure->fault, WorkerFault::None);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(4));
}

TEST_F(WorkerProcess, StartsWithoutAConsoleAndReportsItsBuild) {
    WorkerHost host(host_options());
    ASSERT_TRUE(host.start());
    const workers::WorkerInfo info = host.info();
    EXPECT_TRUE(info.running);
    EXPECT_NE(info.pid, 0u);
    EXPECT_NE(info.pid, ::GetCurrentProcessId());
    EXPECT_EQ(info.protocol_version, workers::kMaxProtocolVersion);
    EXPECT_EQ(info.build, workers::current_build_identity());
    EXPECT_NE(info.creation_flags & CREATE_NO_WINDOW, 0u);
    EXPECT_NE(info.creation_flags & BELOW_NORMAL_PRIORITY_CLASS, 0u);
    EXPECT_FALSE(info.console_window);
    EXPECT_TRUE(info.test_hooks.empty());
    EXPECT_EQ(info.starts, 1u);
    EXPECT_GT(info.last_start_seconds, 0.0);
    EXPECT_TRUE(host.ping());

    // The executable is a GUI-subsystem program: Windows never gives it a console window.
    HANDLE file = ::CreateFileW(worker_exe().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(file, INVALID_HANDLE_VALUE);
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    DWORD got = 0;
    ASSERT_TRUE(::ReadFile(file, &dos, sizeof(dos), &got, nullptr));
    ASSERT_NE(::SetFilePointer(file, dos.e_lfanew, nullptr, FILE_BEGIN), INVALID_SET_FILE_POINTER);
    ASSERT_TRUE(::ReadFile(file, &nt, sizeof(nt), &got, nullptr));
    ::CloseHandle(file);
    EXPECT_EQ(nt.Signature, IMAGE_NT_SIGNATURE);
    EXPECT_EQ(nt.OptionalHeader.Subsystem, IMAGE_SUBSYSTEM_WINDOWS_GUI);
}

TEST_F(WorkerProcess, RunsJobsAndKeepsTheWorkerBetweenThem) {
    WorkerHost host(host_options());
    std::vector<workers::JobProgress> progress;
    workers::JobCallbacks callbacks;
    callbacks.progress = [&](const workers::JobProgress& p) { progress.push_back(p); };
    const JobOutcome first = host.run_job(job(1), callbacks);
    expect_success(first);
    EXPECT_EQ(first.result->job_id, "job-1");
    EXPECT_EQ(first.result->output.sha256, file_digest(temp(1)));
    EXPECT_GT(first.metrics.start_seconds, 0.0);
    EXPECT_GE(first.metrics.wall_seconds, first.metrics.start_seconds);
    EXPECT_GT(first.metrics.peak_commit_bytes, 0u);
    EXPECT_GT(first.metrics.peak_working_set_bytes, 0u);
    ASSERT_FALSE(progress.empty());
    EXPECT_EQ(progress.back().fraction, 1.0);

    const JobOutcome second = host.run_job(job(2));
    expect_success(second);
    EXPECT_EQ(second.metrics.start_seconds, 0.0);
    EXPECT_EQ(second.result->output.sha256, first.result->output.sha256);  // deterministic output
    EXPECT_EQ(host.info().starts, 1u);
    EXPECT_EQ(host.info().jobs, 2u);
}

TEST_F(WorkerProcess, AJobTheWorkerRefusesLeavesTheWorkerRunning) {
    WorkerHost host(host_options());
    auto missing = job(1);
    missing.source.path = dir_.path() / L"absent.dng";
    const JobOutcome outcome = host.run_job(missing);
    ASSERT_TRUE(outcome.failure);
    EXPECT_EQ(outcome.failure->error.code, ErrorCode::NotFound);
    EXPECT_EQ(outcome.failure->failure_class, FailureClass::Deterministic);
    EXPECT_EQ(outcome.failure->fault, WorkerFault::None);
    expect_success(host.run_job(job(2)));
    EXPECT_EQ(host.info().starts, 1u);
}

TEST_F(WorkerProcess, AnInvalidRequestNeverReachesTheWorker) {
    WorkerHost host(host_options());
    auto invalid = job(1);
    invalid.temp_path = L"relative.partial";
    const JobOutcome outcome = host.run_job(invalid);
    ASSERT_TRUE(outcome.failure);
    EXPECT_EQ(outcome.failure->error.code, ErrorCode::InvalidArgument);
    EXPECT_EQ(outcome.failure->fault, WorkerFault::None);
    EXPECT_EQ(host.info().starts, 0u);
}

TEST_F(WorkerProcess, CancelMidRenderStopsTheJobAndKeepsTheWorker) {
    WorkerHost host(host_options({"render-delay:3000"}));
    core::CancellationSource cancel;
    workers::JobCallbacks callbacks;
    callbacks.progress = [&](const workers::JobProgress& p) {
        if (p.stage == workers::JobStage::Rendering) {
            cancel.cancel();
        }
    };
    const auto start = std::chrono::steady_clock::now();
    const JobOutcome outcome = host.run_job(job(1), callbacks, cancel.token());
    ASSERT_TRUE(outcome.failure);
    EXPECT_EQ(outcome.failure->error.code, ErrorCode::Cancelled) << outcome.failure->error.describe();
    EXPECT_EQ(outcome.failure->failure_class, FailureClass::Cancelled);
    EXPECT_EQ(outcome.failure->fault, WorkerFault::None);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 2500ms);
    EXPECT_FALSE(path_exists(temp(1)));
    EXPECT_FALSE(path_exists(workers::job_scratch_directory(temp(1))));
    EXPECT_TRUE(host.ping());
    EXPECT_EQ(host.info().starts, 1u);
}

TEST_F(WorkerProcess, AWorkerKilledMidJobIsATransientCrashAndIsRestarted) {
    WorkerHost host(host_options({"render-delay:1000"}));
    ASSERT_TRUE(host.start());
    const std::uint32_t pid = host.info().pid;
    workers::JobCallbacks callbacks;
    std::atomic<bool> killed{false};
    callbacks.progress = [&](const workers::JobProgress& p) {
        if (p.stage == workers::JobStage::Rendering && !killed.exchange(true)) {
            kill_worker(pid, 0xDEAD);
        }
    };
    const JobOutcome outcome = host.run_job(job(1), callbacks);
    ASSERT_TRUE(killed.load());
    ASSERT_TRUE(outcome.failure);
    EXPECT_EQ(outcome.failure->error.code, ErrorCode::Internal);
    EXPECT_EQ(outcome.failure->error.where, "workers.worker.crashed");
    EXPECT_EQ(outcome.failure->failure_class, FailureClass::Transient);
    EXPECT_EQ(outcome.failure->fault, WorkerFault::Crashed);
    EXPECT_EQ(outcome.failure->exit_code, 0xDEADu);
    EXPECT_TRUE(outcome.failure->output_removed);
    EXPECT_FALSE(host.info().running);

    // The next job starts a new worker.
    expect_success(host.run_job(job(2)));
    EXPECT_EQ(host.info().starts, 2u);
    EXPECT_NE(host.info().pid, pid);
}

TEST_F(WorkerProcess, ACrashingWorkerEndsWithItsExceptionCodeAndNoDialog) {
    WorkerHost host(host_options({"crash-job"}));
    const auto start = std::chrono::steady_clock::now();
    const JobOutcome outcome = host.run_job(job(1));
    ASSERT_TRUE(outcome.failure);
    EXPECT_EQ(outcome.failure->fault, WorkerFault::Crashed);
    EXPECT_EQ(outcome.failure->exit_code, static_cast<std::uint32_t>(EXCEPTION_ACCESS_VIOLATION));
    EXPECT_EQ(outcome.failure->failure_class, FailureClass::Transient);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 10s);
}

TEST_F(WorkerProcess, AnUnresponsiveWorkerIsTerminatedAfterTheHeartbeatTimeout) {
    auto options = host_options({"freeze"});
    options.heartbeat_interval = 50ms;
    options.heartbeat_timeout = 1s;
    WorkerHost host(options);
    ASSERT_TRUE(host.start());
    const auto start = std::chrono::steady_clock::now();
    const JobOutcome outcome = host.run_job(job(1));
    const auto took = std::chrono::steady_clock::now() - start;
    ASSERT_TRUE(outcome.failure);
    EXPECT_EQ(outcome.failure->error.code, ErrorCode::Timeout);
    EXPECT_EQ(outcome.failure->error.where, "workers.worker.unresponsive");
    EXPECT_EQ(outcome.failure->failure_class, FailureClass::Transient);
    EXPECT_EQ(outcome.failure->fault, WorkerFault::Unresponsive);
    EXPECT_GE(took, 1s);
    EXPECT_LT(took, 8s);
    EXPECT_FALSE(host.info().running);
}

TEST_F(WorkerProcess, AJobIgnoringItsCancellationIsTerminatedAfterTheGrace) {
    auto options = host_options({"hang-job"});
    options.cancel_grace = 500ms;
    WorkerHost host(options);
    core::CancellationSource cancel;
    std::thread canceller([&] {
        std::this_thread::sleep_for(300ms);
        cancel.cancel();
    });
    const JobOutcome outcome = host.run_job(job(1), {}, cancel.token());
    canceller.join();
    ASSERT_TRUE(outcome.failure);
    EXPECT_EQ(outcome.failure->error.code, ErrorCode::Cancelled);
    EXPECT_EQ(outcome.failure->error.where, "workers.job.cancelled");
    EXPECT_EQ(outcome.failure->failure_class, FailureClass::Cancelled);
    EXPECT_EQ(outcome.failure->fault, WorkerFault::CancelTimeout);
    EXPECT_FALSE(host.info().running);
}

TEST_F(WorkerProcess, AJobTimeoutCancelsCooperativelyOrTerminates) {
    {
        WorkerHost host(host_options({"render-delay:3000"}));
        const JobOutcome outcome = host.run_job(job(1), {}, {}, 300ms);
        ASSERT_TRUE(outcome.failure);
        EXPECT_EQ(outcome.failure->error.code, ErrorCode::Timeout);
        EXPECT_EQ(outcome.failure->error.where, "workers.job.timeout");
        EXPECT_EQ(outcome.failure->failure_class, FailureClass::Transient);
        EXPECT_EQ(outcome.failure->fault, WorkerFault::None);  // the worker stopped by itself
        EXPECT_TRUE(host.info().running);
    }
    {
        auto options = host_options({"hang-job"});
        options.cancel_grace = 300ms;
        WorkerHost host(options);
        const JobOutcome outcome = host.run_job(job(2), {}, {}, 300ms);
        ASSERT_TRUE(outcome.failure);
        EXPECT_EQ(outcome.failure->error.code, ErrorCode::Timeout);
        EXPECT_EQ(outcome.failure->fault, WorkerFault::TimedOut);
        EXPECT_FALSE(host.info().running);
    }
}

TEST_F(WorkerProcess, ExceedingTheMemoryLimitFailsCleanly) {
    auto options = host_options({"allocate:536870912"});  // 512 MiB
    options.memory_limit_bytes = 128ull << 20;
    WorkerHost host(options);
    const JobOutcome outcome = host.run_job(job(1));
    ASSERT_TRUE(outcome.failure);
    EXPECT_EQ(outcome.failure->error.code, ErrorCode::LimitExceeded) << outcome.failure->error.describe();
    EXPECT_EQ(outcome.failure->error.where, "workers.worker.memory_limit");
    EXPECT_EQ(outcome.failure->failure_class, FailureClass::Deterministic);
    EXPECT_EQ(outcome.failure->fault, WorkerFault::MemoryLimit);
    EXPECT_FALSE(host.info().running);  // recycled
    EXPECT_FALSE(path_exists(temp(1)));
}

TEST_F(WorkerProcess, ASmallRenderFitsUnderAModestLimit) {
    auto options = host_options();
    options.memory_limit_bytes = 512ull << 20;
    WorkerHost host(options);
    const JobOutcome outcome = host.run_job(job(1));
    expect_success(outcome);
    EXPECT_LT(outcome.metrics.peak_commit_bytes, options.memory_limit_bytes);
}

TEST_F(WorkerProcess, MismatchedComponentsFailClearly) {
    {
        WorkerHost host(host_options({"protocol:3-4"}));
        auto started = host.start();
        ASSERT_FALSE(started);
        EXPECT_EQ(started.error().code, ErrorCode::VersionMismatch);
        EXPECT_EQ(started.error().where, "workers.handshake.version");
        EXPECT_FALSE(host.info().running);
    }
    {
        WorkerHost host(host_options({"fake-commit:0badbee"}));
        const JobOutcome outcome = host.run_job(job(1));
        ASSERT_TRUE(outcome.failure);
        EXPECT_EQ(outcome.failure->error.code, ErrorCode::VersionMismatch);
        EXPECT_EQ(outcome.failure->error.where, "workers.handshake.build");
        EXPECT_NE(outcome.failure->error.message.find("0badbee"), std::string::npos);
        EXPECT_EQ(outcome.failure->failure_class, FailureClass::Deterministic);
        EXPECT_EQ(outcome.failure->fault, WorkerFault::StartFailed);
    }
}

TEST_F(WorkerProcess, AMissingExecutableIsNotFound) {
    auto options = host_options();
    options.executable = dir_.path() / L"no-such-worker.exe";
    WorkerHost host(options);
    auto started = host.start();
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error().code, ErrorCode::NotFound);
    const JobOutcome outcome = host.run_job(job(1));
    ASSERT_TRUE(outcome.failure);
    EXPECT_EQ(outcome.failure->fault, WorkerFault::StartFailed);
    EXPECT_EQ(outcome.failure->failure_class, FailureClass::Deterministic);
}

TEST_F(WorkerProcess, ShutdownEndsTheWorkerCleanly) {
    WorkerHost host(host_options());
    ASSERT_TRUE(host.start());
    HANDLE process = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, host.info().pid);
    ASSERT_NE(process, nullptr);
    host.shutdown();
    EXPECT_EQ(wait_for_exit(process, 5s), static_cast<DWORD>(workers::WorkerExitCode::Clean));
    ::CloseHandle(process);
    EXPECT_FALSE(host.info().running);
    host.shutdown();  // no effect
}

// ---- The host process dies ------------------------------------------------------------------

struct HelperParent {
    HANDLE process = nullptr;
    HANDLE output = nullptr;

    HelperParent(const std::wstring& mode, const std::wstring& activity) {
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE write = nullptr;
        EXPECT_TRUE(::CreatePipe(&output, &write, &inherit, 0));
        ::SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = write;
        startup.hStdError = write;
        std::wstring command = L"\"" + std::filesystem::path(LUCENT_WORKER_TEST_PARENT_EXE).make_preferred().native() +
                               L"\" \"" + worker_exe().native() + L"\" " + mode + L" " + activity;
        PROCESS_INFORMATION info{};
        EXPECT_TRUE(::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                     nullptr, &startup, &info));
        ::CloseHandle(write);
        ::CloseHandle(info.hThread);
        process = info.hProcess;
    }
    ~HelperParent() {
        if (process != nullptr) {
            ::TerminateProcess(process, 1);
            ::CloseHandle(process);
        }
        ::CloseHandle(output);
    }

    // The next line the helper prints; empty when it ended.
    std::string line() {
        std::string text;
        char c = 0;
        DWORD got = 0;
        while (::ReadFile(output, &c, 1, &got, nullptr) && got == 1) {
            if (c == '\n') {
                return text;
            }
            if (c != '\r') {  // the helper's standard output is in text mode
                text.push_back(c);
            }
        }
        return text;
    }
};

// Starts the helper, waits for its worker (and job), opens the worker, kills the helper, and
// returns the worker's exit code if it ended within `timeout`.
std::optional<DWORD> worker_exit_after_parent_death(const std::wstring& mode, const std::wstring& activity,
                                                    std::chrono::milliseconds timeout,
                                                    std::chrono::milliseconds* took = nullptr) {
    HelperParent parent(mode, activity);
    const std::string pid_line = parent.line();
    EXPECT_TRUE(pid_line.starts_with("pid=")) << pid_line;
    if (!pid_line.starts_with("pid=")) {
        return std::nullopt;
    }
    const DWORD pid = static_cast<DWORD>(std::stoul(pid_line.substr(4)));
    HANDLE worker = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pid);
    EXPECT_NE(worker, nullptr);
    if (worker == nullptr) {
        return std::nullopt;
    }
    if (activity == L"busy") {
        EXPECT_EQ(parent.line(), "busy");
    }
    const auto killed_at = std::chrono::steady_clock::now();
    ::TerminateProcess(parent.process, 9);
    ::WaitForSingleObject(parent.process, 5000);
    auto code = wait_for_exit(worker, timeout);
    if (took != nullptr) {
        *took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - killed_at);
    }
    if (!code) {
        ::TerminateProcess(worker, 1);  // never leave an orphan behind a failed test
    }
    ::CloseHandle(worker);
    return code;
}

TEST(WorkerParentDeath, AnIdleWorkerLeavesWhenItsHostDies) {
    std::chrono::milliseconds took{};
    auto code = worker_exit_after_parent_death(L"nojob", L"idle", 10s, &took);
    ASSERT_TRUE(code.has_value()) << "the worker outlived its host";
    EXPECT_EQ(*code, static_cast<DWORD>(workers::WorkerExitCode::ParentGone));
    EXPECT_LT(took, 3s);
}

TEST(WorkerParentDeath, ABusyWorkerWhoseJobIgnoresCancellationStillLeaves) {
    std::chrono::milliseconds took{};
    auto code = worker_exit_after_parent_death(L"nojob", L"busy", 10s, &took);
    ASSERT_TRUE(code.has_value()) << "the worker outlived its host";
    EXPECT_EQ(*code, static_cast<DWORD>(workers::WorkerExitCode::ParentGone));
    EXPECT_GE(took, 1s);  // the exit grace (2 s) passed first
    EXPECT_LT(took, 6s);
}

TEST(WorkerParentDeath, TheJobObjectEndsTheWorkerWithItsHost) {
    auto code = worker_exit_after_parent_death(L"job", L"busy", 10s);
    EXPECT_TRUE(code.has_value()) << "the worker outlived its host";
}

}  // namespace
}  // namespace lucent::worker_app
