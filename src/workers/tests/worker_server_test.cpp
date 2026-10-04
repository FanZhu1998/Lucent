// The worker's side of the protocol run in-process over real pipes with scripted runners
// (worker_server.hpp): handshake refusals, jobs, cancellation, heartbeats, progress rate limits,
// exceptions, shutdown, and leaving when the host goes away.

#include <atomic>
#include <chrono>
#include <cmath>
#include <new>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>
#include <windows.h>

#include <lucent/workers/worker_server.hpp>

#include "workers_test_support.hpp"

namespace lucent::workers {
namespace {

using namespace std::chrono_literals;
using test_support::sample_job;
using test_support::ServerHarness;

WorkerHello expect_hello(const std::optional<Message>& message) {
    EXPECT_TRUE(message.has_value());
    if (!message) {
        return {};
    }
    const auto* hello = std::get_if<WorkerHello>(&*message);
    EXPECT_NE(hello, nullptr) << type_name(*message);
    return hello != nullptr ? *hello : WorkerHello{};
}

JobError expect_fatal(const std::optional<Message>& message) {
    EXPECT_TRUE(message.has_value());
    if (!message) {
        return {};
    }
    const auto* error = std::get_if<JobError>(&*message);
    EXPECT_NE(error, nullptr) << type_name(*message);
    return error != nullptr ? *error : JobError{};
}

TEST(WorkerServer, HandshakeThenJobResultWithProgress) {
    ServerHarness server(test_support::succeed);
    const WorkerHello hello = expect_hello(server.handshake(server.hello()));
    EXPECT_EQ(hello.version, kMaxProtocolVersion);
    EXPECT_EQ(hello.build, current_build_identity());
    EXPECT_EQ(hello.pid, ::GetCurrentProcessId());
    EXPECT_TRUE(hello.test_hooks.empty());

    ASSERT_TRUE(server.host().channel().send(sample_job("job-7")));
    std::vector<JobProgress> progress;
    std::optional<JobResult> result;
    while (!result) {
        auto message = server.host().next();
        ASSERT_TRUE(message);
        if (auto* p = std::get_if<JobProgress>(&*message)) {
            progress.push_back(*p);
        } else if (auto* r = std::get_if<JobResult>(&*message)) {
            result = *r;
        } else {
            FAIL() << "unexpected " << type_name(*message);
        }
    }
    EXPECT_EQ(result->job_id, "job-7");
    ASSERT_EQ(progress.size(), 3u);
    EXPECT_EQ(progress.front().stage, JobStage::Preparing);
    EXPECT_EQ(progress.back().fraction, 1.0);
    EXPECT_EQ(server.runner().calls.load(), 1);

    ASSERT_TRUE(server.host().channel().send(Shutdown{}));
    EXPECT_EQ(server.wait_done(), WorkerExitCode::Clean);
}

TEST(WorkerServer, RefusesAnotherProtocolVersion) {
    ServerHarness server(test_support::succeed);
    HostHello hello = server.hello();
    hello.min_version = kMaxProtocolVersion + 1;
    hello.max_version = kMaxProtocolVersion + 3;
    const JobError refusal = expect_fatal(server.handshake(hello));
    EXPECT_FALSE(refusal.job_id.has_value());
    EXPECT_EQ(refusal.error.code, ErrorCode::VersionMismatch);
    EXPECT_EQ(refusal.error.where, "workers.handshake.version");
    EXPECT_EQ(server.wait_done(), WorkerExitCode::Refused);
}

TEST(WorkerServer, RefusesAnotherBuild) {
    ServerHarness server(test_support::succeed);
    HostHello hello = server.hello();
    hello.build.git_commit = "0badc0de";
    const JobError refusal = expect_fatal(server.handshake(hello));
    EXPECT_EQ(refusal.error.code, ErrorCode::VersionMismatch);
    EXPECT_EQ(refusal.error.where, "workers.handshake.build");
    EXPECT_NE(refusal.error.message.find("0badc0de"), std::string::npos);
    EXPECT_EQ(server.wait_done(), WorkerExitCode::Refused);
}

TEST(WorkerServer, TheFirstMessageMustBeTheHostHello) {
    ServerHarness server(test_support::succeed);
    ASSERT_TRUE(server.host().channel().send(Ping{1}));
    const JobError refusal = expect_fatal(server.host().next());
    EXPECT_EQ(refusal.error.code, ErrorCode::Corrupt);
    EXPECT_EQ(server.wait_done(), WorkerExitCode::ProtocolViolation);
}

TEST(WorkerServer, ARepeatedHelloIsAProtocolViolation) {
    ServerHarness server(test_support::succeed);
    expect_hello(server.handshake(server.hello()));
    ASSERT_TRUE(server.host().channel().send(server.hello()));
    const JobError violation = expect_fatal(server.host().next());
    EXPECT_EQ(violation.error.code, ErrorCode::Corrupt);
    EXPECT_EQ(server.wait_done(), WorkerExitCode::ProtocolViolation);
}

TEST(WorkerServer, CancelStopsTheRunningJob) {
    ServerHarness server(test_support::wait_for_cancel);
    expect_hello(server.handshake(server.hello()));
    ASSERT_TRUE(server.host().channel().send(sample_job("job-c")));
    auto started = server.host().next_of<JobProgress>();
    ASSERT_TRUE(started);
    ASSERT_TRUE(server.host().channel().send(CancelJob{"other-job"}));  // not running: ignored
    ASSERT_TRUE(server.host().channel().send(CancelJob{"job-c"}));
    auto error = server.host().next_of<JobError>();
    ASSERT_TRUE(error);
    EXPECT_EQ(error->job_id, "job-c");
    EXPECT_EQ(error->error.code, ErrorCode::Cancelled);
    EXPECT_EQ(error->failure_class, FailureClass::Cancelled);

    // The worker is still usable.
    ASSERT_TRUE(server.host().channel().send(Ping{99}));
    auto pong = server.host().next_of<Pong>();
    ASSERT_TRUE(pong);
    EXPECT_EQ(pong->nonce, 99u);
}

TEST(WorkerServer, ASecondJobWhileBusyIsRefused) {
    ServerHarness server(test_support::wait_for_cancel);
    expect_hello(server.handshake(server.hello()));
    ASSERT_TRUE(server.host().channel().send(sample_job("first")));
    ASSERT_TRUE(server.host().next_of<JobProgress>());
    ASSERT_TRUE(server.host().channel().send(sample_job("second")));
    auto busy = server.host().next_of<JobError>();
    ASSERT_TRUE(busy);
    EXPECT_EQ(busy->job_id, "second");
    EXPECT_EQ(busy->error.where, "workers.worker.busy");
    ASSERT_TRUE(server.host().channel().send(CancelJob{"first"}));
    auto cancelled = server.host().next_of<JobError>();
    ASSERT_TRUE(cancelled);
    EXPECT_EQ(cancelled->job_id, "first");
    EXPECT_EQ(server.runner().calls.load(), 1);
}

TEST(WorkerServer, AnalyzeJobsAreAnsweredWithAnAnalysis) {
    ServerHarness server(test_support::wait_for_cancel);
    expect_hello(server.handshake(server.hello()));
    AnalysisRequest analyze;
    analyze.job_id = "auto-1";
    analyze.source = sample_job().source;
    analyze.recipe = sample_job().recipe;
    analyze.request = "{}";
    // A runner that only exports refuses.
    ASSERT_TRUE(server.host().channel().send(analyze));
    auto refused = server.host().next_of<JobError>();
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->job_id, "auto-1");
    EXPECT_EQ(refused->error.code, ErrorCode::Unsupported);
    EXPECT_EQ(refused->error.where, "workers.worker.analyze");

    server.runner().analysis = [](const AnalysisRequest& request, JobContext& context) -> Expected<AnalysisResult> {
        context.progress(JobStage::Analysing, 0.4);
        context.progress(JobStage::Checking, 1.0);
        AnalysisResult result;
        result.source_identity = request.source.identity;
        result.recipe_sha256 = request.recipe.sha256;
        result.document = R"({"analysed":true})";
        return result;
    };
    analyze.job_id = "auto-2";
    ASSERT_TRUE(server.host().channel().send(analyze));
    auto progress = server.host().next_of<JobProgress>();
    ASSERT_TRUE(progress);
    EXPECT_EQ(progress->stage, JobStage::Analysing);
    auto analysis = server.host().next_of<AnalysisResult>();
    ASSERT_TRUE(analysis);
    EXPECT_EQ(analysis->job_id, "auto-2");
    EXPECT_EQ(analysis->document, R"({"analysed":true})");
    EXPECT_EQ(server.runner().analyses.load(), 2);
    EXPECT_EQ(server.runner().calls.load(), 0);

    // An analyze job while an export runs is refused like a second job.
    ASSERT_TRUE(server.host().channel().send(sample_job("export")));
    ASSERT_TRUE(server.host().next_of<JobProgress>());
    analyze.job_id = "auto-3";
    ASSERT_TRUE(server.host().channel().send(analyze));
    auto busy = server.host().next_of<JobError>();
    ASSERT_TRUE(busy);
    EXPECT_EQ(busy->job_id, "auto-3");
    EXPECT_EQ(busy->error.where, "workers.worker.busy");
    ASSERT_TRUE(server.host().channel().send(CancelJob{"export"}));
    ASSERT_TRUE(server.host().next_of<JobError>());
}

TEST(WorkerServer, RunnerExceptionsBecomeErrors) {
    std::atomic<int> call{0};
    ServerHarness server([&](const JobRequest&, JobContext&) -> Expected<JobResult> {
        if (call++ == 0) {
            throw std::bad_alloc();
        }
        throw std::runtime_error("boom");
    });
    expect_hello(server.handshake(server.hello()));
    ASSERT_TRUE(server.host().channel().send(sample_job("a")));
    auto oom = server.host().next_of<JobError>();
    ASSERT_TRUE(oom);
    EXPECT_EQ(oom->error.code, ErrorCode::OutOfMemory);
    EXPECT_EQ(oom->failure_class, FailureClass::Transient);
    ASSERT_TRUE(server.host().channel().send(sample_job("b")));
    auto internal = server.host().next_of<JobError>();
    ASSERT_TRUE(internal);
    EXPECT_EQ(internal->error.code, ErrorCode::Internal);
    EXPECT_EQ(internal->failure_class, FailureClass::Deterministic);
    EXPECT_NE(internal->error.message.find("boom"), std::string::npos);
}

TEST(WorkerServer, HeartbeatsArriveAtTheRequestedIntervalAndNameTheJob) {
    ServerHarness server(test_support::wait_for_cancel);
    expect_hello(server.handshake(server.hello(20)));
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t last = 0;
    for (int i = 0; i < 5; ++i) {
        auto heartbeat = server.host().next_of<Heartbeat>(2s);
        ASSERT_TRUE(heartbeat);
        EXPECT_GT(heartbeat->sequence, last);
        last = heartbeat->sequence;
        EXPECT_FALSE(heartbeat->job_id.has_value());
        EXPECT_GT(heartbeat->memory.working_set, 0u);
        EXPECT_GE(heartbeat->memory.peak_commit, heartbeat->memory.commit);
    }
    EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
    ASSERT_TRUE(server.host().channel().send(sample_job("hb")));
    bool named = false;
    for (int i = 0; i < 50 && !named; ++i) {
        auto heartbeat = server.host().next_of<Heartbeat>(2s);
        ASSERT_TRUE(heartbeat);
        named = heartbeat->job_id == "hb";
    }
    EXPECT_TRUE(named);
    ASSERT_TRUE(server.host().channel().send(CancelJob{"hb"}));
}

TEST(WorkerServer, ProgressIsRateLimitedAndNeverDecreases) {
    ServerHarness server([](const JobRequest& request, JobContext& context) -> Expected<JobResult> {
        for (int i = 0; i <= 10'000; ++i) {
            context.progress(JobStage::Rendering, i / 10'000.0);
        }
        context.progress(JobStage::Rendering, 0.2);  // lower: clamped, not sent
        context.progress(JobStage::Encoding, std::nan(""));
        return test_support::sample_result(request.job_id);
    });
    expect_hello(server.handshake(server.hello()));
    ASSERT_TRUE(server.host().channel().send(sample_job()));
    std::vector<JobProgress> progress;
    for (;;) {
        auto message = server.host().next();
        ASSERT_TRUE(message);
        if (auto* p = std::get_if<JobProgress>(&*message)) {
            progress.push_back(*p);
        } else {
            ASSERT_TRUE(std::holds_alternative<JobResult>(*message));
            break;
        }
    }
    EXPECT_LE(progress.size(), 103u);
    EXPECT_GE(progress.size(), 90u);
    for (std::size_t i = 1; i < progress.size(); ++i) {
        EXPECT_GE(progress[i].fraction, progress[i - 1].fraction);
    }
    EXPECT_EQ(progress.back().stage, JobStage::Encoding);
    EXPECT_EQ(progress.back().fraction, 1.0);
}

TEST(WorkerServer, ShutdownCancelsTheRunningJobAndEndsCleanly) {
    ServerHarness server(test_support::wait_for_cancel);
    expect_hello(server.handshake(server.hello()));
    ASSERT_TRUE(server.host().channel().send(sample_job("long")));
    ASSERT_TRUE(server.host().next_of<JobProgress>());
    ASSERT_TRUE(server.host().channel().send(Shutdown{}));
    auto cancelled = server.host().next_of<JobError>();
    ASSERT_TRUE(cancelled);
    EXPECT_EQ(cancelled->error.code, ErrorCode::Cancelled);
    EXPECT_EQ(server.wait_done(), WorkerExitCode::Clean);
}

TEST(WorkerServer, LeavesWhenTheHostClosesThePipe) {
    ServerHarness server(test_support::wait_for_cancel);
    expect_hello(server.handshake(server.hello()));
    ASSERT_TRUE(server.host().channel().send(sample_job("orphan")));
    ASSERT_TRUE(server.host().next_of<JobProgress>());
    server.host().channel().close_write();
    EXPECT_EQ(server.wait_done(5s), WorkerExitCode::ParentGone);
}

TEST(WorkerServer, LeavesBeforeTheHandshakeWhenTheHostIsGone) {
    ServerHarness server(test_support::succeed);
    server.host().channel().close_write();
    EXPECT_EQ(server.wait_done(5s), WorkerExitCode::ParentGone);
}

TEST(WorkerServer, LeavesWhenTheParentProcessEndsEvenIfThePipeStaysOpen) {
    // The parent handle is an event here; signalling it is what the host process ending looks like.
    HANDLE parent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(parent, nullptr);
    WorkerServerOptions options;
    options.parent_process = parent;
    {
        ServerHarness server(test_support::succeed, options);
        expect_hello(server.handshake(server.hello(20)));
        ::SetEvent(parent);
        // The reader is blocked in ReadFile on a pipe that stays open: the server cancels it.
        EXPECT_EQ(server.wait_done(5s), WorkerExitCode::ParentGone);
    }
    ::CloseHandle(parent);
}

TEST(WorkerServer, AJobThatIgnoresCancellationIsCutOffAfterTheExitGrace) {
    std::atomic<bool> release{false};
    std::atomic<int> forced{0};
    std::atomic<WorkerExitCode> forced_code{WorkerExitCode::Clean};
    WorkerServerOptions options;
    options.exit_grace = 100ms;
    options.force_exit = [&](WorkerExitCode code) {
        forced_code = code;
        ++forced;
        release = true;  // a real worker ends here; the test lets the job return instead
    };
    ServerHarness server(
        [&](const JobRequest&, JobContext& context) -> Expected<JobResult> {
            context.progress(JobStage::Rendering, 0.1);
            while (!release) {
                std::this_thread::sleep_for(2ms);
            }
            return fail(ErrorCode::Cancelled, "late", "test");
        },
        options);
    expect_hello(server.handshake(server.hello(20)));
    ASSERT_TRUE(server.host().channel().send(sample_job("stuck")));
    ASSERT_TRUE(server.host().next_of<JobProgress>());
    const auto closed_at = std::chrono::steady_clock::now();
    server.host().channel().close_write();
    EXPECT_EQ(server.wait_done(5s), WorkerExitCode::ParentGone);
    EXPECT_EQ(forced.load(), 1);
    EXPECT_EQ(forced_code.load(), WorkerExitCode::ParentGone);
    EXPECT_GE(std::chrono::steady_clock::now() - closed_at, 100ms);
}

TEST(WorkerServer, MemoryCountersDescribeThisProcess) {
    const MemorySnapshot memory = current_process_memory();
    EXPECT_GT(memory.working_set, 0u);
    EXPECT_GE(memory.peak_working_set, memory.working_set);
    EXPECT_GT(memory.commit, 0u);
    EXPECT_GE(memory.peak_commit, memory.commit);
}

}  // namespace
}  // namespace lucent::workers
