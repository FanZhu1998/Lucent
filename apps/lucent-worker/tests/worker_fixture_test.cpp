// lucent-worker.exe on the camera fixtures (skipped when fixtures/cache is absent): the 45 MP Canon
// EOS R5 smoke fixture rendered in a worker and in-process (spawn and handshake time, per-job
// overhead, peak worker memory), cancellation mid-render, and a memory limit below the render's
// needs. Run with: ctest -L fixtures -R Worker

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include <lucent/workers/job_output.hpp>
#include <lucent/workers/worker_host.hpp>

#include "worker_test_support.hpp"

namespace lucent::worker_app {
namespace {

using namespace test_support;
using workers::JobOutcome;
using workers::WorkerFault;
using workers::WorkerHost;

double seconds(std::chrono::steady_clock::duration d) { return std::chrono::duration<double>(d).count(); }
double mib(std::uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

void print_timings(const char* what, const workers::JobTimings& t) {
    std::printf("  %-28s prepare %.3f / render %.3f / encode %.3f / write %.3f / verify %.3f = %.3f s\n", what,
                t.prepare_seconds, t.render_seconds, t.encode_seconds, t.write_seconds, t.verify_seconds,
                t.total_seconds);
}

// Timings depend on the machine's load: the numbers are printed for the record, and only the
// functional results are asserted.
TEST(WorkerFixture, RendersTheCanonR5InAWorkerAndMeasuresTheOverhead) {
    LUCENT_REQUIRE_FIXTURE(source, "canon-eos-r5-cr3-raw");
    rts::ScratchDir dir;

    // In-process reference (also warms the file cache for the worker runs).
    DevelopJobRunner runner(default_registry());
    LocalContext context;
    auto local = runner.run(make_job(source, dir.path() / L"local.partial", "local"), context);
    ASSERT_TRUE(local) << local.error().describe();

    // Normal priority, so that the comparison with the in-process run measures the process boundary
    // rather than the scheduler (batch workers run below normal by default).
    auto options = host_options();
    options.memory_limit_bytes = 3ull << 30;
    options.below_normal_priority = false;
    WorkerHost host(options);
    const auto spawn_start = std::chrono::steady_clock::now();
    ASSERT_TRUE(host.start());
    const double spawn_seconds = seconds(std::chrono::steady_clock::now() - spawn_start);

    const JobOutcome first = host.run_job(make_job(source, dir.path() / L"worker.partial", "worker"));
    ASSERT_TRUE(first.succeeded()) << first.failure->error.describe();
    const workers::JobResult& result = *first.result;
    EXPECT_EQ(result.output.width, local->output.width);
    EXPECT_EQ(result.output.height, local->output.height);
    EXPECT_GT(result.output.width, 8000u);
    EXPECT_EQ(result.output.sha256, file_digest(dir.path() / L"worker.partial"));
    EXPECT_EQ(result.output.sha256, local->output.sha256);  // the same bytes as in-process
    EXPECT_LT(first.metrics.peak_commit_bytes, options.memory_limit_bytes);

    const JobOutcome second = host.run_job(make_job(source, dir.path() / L"worker2.partial", "worker2"));
    ASSERT_TRUE(second.succeeded()) << second.failure->error.describe();
    LocalContext context2;
    auto local2 = runner.run(make_job(source, dir.path() / L"local2.partial", "local2"), context2);
    ASSERT_TRUE(local2) << local2.error().describe();

    // The process boundary's own cost: the host's wall time minus the worker's job time.
    const double boundary1 = first.metrics.wall_seconds - result.timings.total_seconds;
    const double boundary2 = second.metrics.wall_seconds - second.result->timings.total_seconds;
    std::printf("  spawn+handshake %.3f s\n", spawn_seconds);
    print_timings("in-process 1", local->timings);
    print_timings("worker job 1 (new process)", result.timings);
    print_timings("worker job 2 (warm process)", second.result->timings);
    print_timings("in-process 2", local2->timings);
    std::printf("  host wall: job 1 %.3f s (boundary %.3f s), job 2 %.3f s (boundary %.3f s)\n",
                first.metrics.wall_seconds, boundary1, second.metrics.wall_seconds, boundary2);
    std::printf("  worker peak commit %.0f MiB, peak working set %.0f MiB (engine estimate %.0f MiB)\n",
                mib(first.metrics.peak_commit_bytes), mib(first.metrics.peak_working_set_bytes),
                mib(result.render.estimated_peak_bytes));
    EXPECT_LT(boundary2, 0.5);
    RecordProperty("spawn_ms", static_cast<int>(spawn_seconds * 1000));
    RecordProperty("boundary_ms", static_cast<int>(boundary2 * 1000));
    RecordProperty("peak_commit_mib", static_cast<int>(mib(first.metrics.peak_commit_bytes)));
}

TEST(WorkerFixture, CancelsTheCanonR5MidRender) {
    LUCENT_REQUIRE_FIXTURE(source, "canon-eos-r5-cr3-raw");
    rts::ScratchDir dir;
    WorkerHost host(host_options());
    ASSERT_TRUE(host.start());
    core::CancellationSource cancel;
    std::chrono::steady_clock::time_point cancelled_at{};
    workers::JobCallbacks callbacks;
    callbacks.progress = [&](const workers::JobProgress& p) {
        if (p.stage == workers::JobStage::Rendering && p.fraction > 0.1 && !cancel.is_cancelled()) {
            cancelled_at = std::chrono::steady_clock::now();
            cancel.cancel();
        }
    };
    const auto temp = dir.path() / L"cancelled.partial";
    const JobOutcome outcome = host.run_job(make_job(source, temp, "r5-cancel"), callbacks, cancel.token());
    ASSERT_TRUE(outcome.failure) << "the job finished before it could be cancelled";
    EXPECT_EQ(outcome.failure->error.code, ErrorCode::Cancelled);
    EXPECT_EQ(outcome.failure->fault, WorkerFault::None);
    const double latency = seconds(std::chrono::steady_clock::now() - cancelled_at);
    std::printf("  cancel to answer %.3f s\n", latency);
    EXPECT_LT(latency, 5.0);
    EXPECT_FALSE(path_exists(temp));
    EXPECT_FALSE(path_exists(workers::job_scratch_directory(temp)));
    EXPECT_TRUE(host.ping());
}

TEST(WorkerFixture, ABudgetBelowTheRendersNeedsFailsCleanly) {
    LUCENT_REQUIRE_FIXTURE(source, "canon-eos-r5-cr3-raw");
    rts::ScratchDir dir;
    auto options = host_options();
    options.memory_limit_bytes = 384ull << 20;
    WorkerHost host(options);
    const auto temp = dir.path() / L"limited.partial";
    const JobOutcome outcome = host.run_job(make_job(source, temp, "r5-limited"));
    ASSERT_TRUE(outcome.failure);
    std::printf("  %s (fault %s)\n", outcome.failure->error.describe().c_str(),
                std::string(workers::to_string(outcome.failure->fault)).c_str());
    EXPECT_EQ(outcome.failure->failure_class, workers::FailureClass::Deterministic);
    EXPECT_TRUE(outcome.failure->error.code == ErrorCode::LimitExceeded) << outcome.failure->error.describe();
    EXPECT_FALSE(path_exists(temp));
    EXPECT_FALSE(path_exists(workers::job_scratch_directory(temp)));

    // The next job gets a fresh worker within a budget that fits.
    WorkerHost roomy(host_options());
    const JobOutcome fits = roomy.run_job(make_job(source, dir.path() / L"fits.partial", "r5-fits"));
    EXPECT_TRUE(fits.succeeded()) << fits.failure->error.describe();
}

}  // namespace
}  // namespace lucent::worker_app
