// The aggregate memory limit of a WorkerGroup (worker_group.hpp) over real lucent-worker.exe
// processes: two workers whose allocations together exceed the group's limit (the one that allocates
// at the limit fails cleanly and transiently, the other is untouched), a single worker within the
// limit, and a single worker alone above it (deterministic, as its own limit). The jobs name a
// source that does not exist: the allocation test hook runs first (worker_launch.hpp), so a job that
// gets past it fails with the runner's own error, never a memory one.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include <lucent/workers/job_output.hpp>
#include <lucent/workers/worker_group.hpp>
#include <lucent/workers/worker_host.hpp>

#include "raw_test_support.hpp"
#include "workers_test_support.hpp"

namespace lucent::workers {
namespace {

namespace rts = raw::test_support;
using namespace std::chrono_literals;

constexpr std::uint64_t kMiB = std::uint64_t{1} << 20;

std::filesystem::path worker_exe() { return std::filesystem::path(LUCENT_WORKER_EXE).make_preferred(); }

WorkerHostOptions host_options(std::shared_ptr<WorkerGroup> group, std::vector<std::string> hooks,
                               std::uint64_t memory_limit) {
    WorkerHostOptions options;
    options.executable = worker_exe();
    options.memory_limit_bytes = memory_limit;
    options.group = std::move(group);
    options.test_hooks = std::move(hooks);
    options.heartbeat_interval = 100ms;
    options.heartbeat_timeout = 15s;
    options.cancel_grace = 5s;
    options.shutdown_grace = 5s;
    return options;
}

// A job of a source that does not exist: past the allocation hook it fails with the runner's error.
JobRequest job(const rts::ScratchDir& dir, const std::string& id) {
    JobRequest request = test_support::sample_job(id);
    request.source.path = dir.path() / L"missing.dng";
    request.temp_path = dir.path() / (L"~lucent-" + std::wstring(id.begin(), id.end()) + L".partial");
    request.memory_budget_bytes = 0;
    return request;
}

bool is_memory_fault(const JobOutcome& outcome) {
    return outcome.failure &&
           (outcome.failure->fault == WorkerFault::MemoryLimit || outcome.failure->fault == WorkerFault::GroupMemoryLimit);
}

std::string describe(const JobOutcome& outcome) {
    if (!outcome.failure) {
        return "succeeded";
    }
    return outcome.failure->error.describe() + " (fault " + std::string(to_string(outcome.failure->fault)) + ")";
}

TEST(WorkerGroup, RefusesAZeroLimit) {
    auto group = WorkerGroup::create(0);
    ASSERT_FALSE(group);
    EXPECT_EQ(group.error().code, ErrorCode::InvalidArgument);
}

TEST(WorkerGroup, TwoWorkersAboveTheAggregateLimitStopOneOfThemTransiently) {
    rts::ScratchDir dir;
    auto group = WorkerGroup::create(1024 * kMiB);
    ASSERT_TRUE(group) << group.error().describe();
    // Each worker's own limit (1 GiB) admits its 600 MiB; together they need 1.2 GiB.
    WorkerHost holder(host_options(*group, {"allocate:629145600", "hold-allocation:6000"}, 1024 * kMiB));
    WorkerHost second(host_options(*group, {"allocate:629145600"}, 1024 * kMiB));
    ASSERT_TRUE(holder.start());
    ASSERT_TRUE(second.start());
    EXPECT_TRUE(holder.info().grouped);
    EXPECT_TRUE(second.info().grouped);
    EXPECT_EQ((*group)->stats().members, 2u);

    auto first = std::async(std::launch::async, [&] { return holder.run_job(job(dir, "holder")); });
    // Wait until the holder has committed its allocation.
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while ((*group)->stats().peak_commit_bytes < 600 * kMiB && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
    }
    ASSERT_GE((*group)->stats().peak_commit_bytes, 600 * kMiB);

    const JobRequest stopped = job(dir, "second");
    const JobOutcome outcome = second.run_job(stopped);
    ASSERT_TRUE(outcome.failure) << "the second worker's allocation should have failed";
    EXPECT_EQ(outcome.failure->fault, WorkerFault::GroupMemoryLimit) << describe(outcome);
    EXPECT_EQ(outcome.failure->failure_class, FailureClass::Transient);
    EXPECT_EQ(outcome.failure->error.code, ErrorCode::OutOfMemory);
    EXPECT_EQ(outcome.failure->error.where, "workers.worker.group_memory_limit");
    EXPECT_NE(outcome.failure->error.message.find("aggregate memory limit of 1024 MiB"), std::string::npos)
        << outcome.failure->error.message;
    EXPECT_FALSE(second.info().running);  // recycled
    EXPECT_TRUE(outcome.failure->output_removed);
    EXPECT_FALSE(std::filesystem::exists(stopped.temp_path));

    // The holder is untouched: past its allocation it fails with the runner's own error.
    const JobOutcome held = first.get();
    EXPECT_FALSE(is_memory_fault(held)) << describe(held);
    EXPECT_TRUE(holder.info().running);

    const WorkerGroupStats stats = (*group)->stats();
    std::printf("  stopped: %s; group peak %llu MiB, notifications %llu, holder peak commit %llu MiB, second %llu MiB\n",
                describe(outcome).c_str(), static_cast<unsigned long long>(stats.peak_commit_bytes / kMiB),
                static_cast<unsigned long long>(stats.limit_notifications),
                static_cast<unsigned long long>(held.metrics.peak_commit_bytes / kMiB),
                static_cast<unsigned long long>(outcome.metrics.peak_commit_bytes / kMiB));
    EXPECT_GE(stats.limit_notifications, 1u);
    // Job Object peaks count the charge of the allocation the limit refused (the group's peak is
    // about both allocations, the second worker's about its own refused one); what the holder
    // committed is its 600 MiB and its baseline.
    EXPECT_GE(held.metrics.peak_commit_bytes, 600 * kMiB);
    EXPECT_LT(held.metrics.peak_commit_bytes, 700 * kMiB);
    EXPECT_GE(stats.joined, 2u);
    EXPECT_EQ(stats.refused, 0u);

    // Alone, the stopped job's allocation fits: the second host's new worker gets past it.
    const JobOutcome retried = second.run_job(job(dir, "second-again"));
    EXPECT_FALSE(is_memory_fault(retried)) << describe(retried);
}

// A holder keeps `held_bytes` in `group` while a second worker runs a job with `second_hooks`; both
// with their own limit `own_limit`. Returns the second job's outcome (and checks the holder's).
JobOutcome stopped_beside_a_holder(const rts::ScratchDir& dir, const std::shared_ptr<WorkerGroup>& group,
                                   std::uint64_t held_bytes, std::vector<std::string> second_hooks,
                                   std::uint64_t own_limit) {
    WorkerHost holder(host_options(group, {std::format("allocate:{}", held_bytes), "hold-allocation:6000"}, own_limit));
    WorkerHost second(host_options(group, std::move(second_hooks), own_limit));
    EXPECT_TRUE(holder.start());
    EXPECT_TRUE(second.start());
    const std::uint64_t before = group->stats().peak_commit_bytes;
    auto first = std::async(std::launch::async, [&] { return holder.run_job(job(dir, "holder")); });
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (group->stats().peak_commit_bytes < before + held_bytes && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_GE(group->stats().peak_commit_bytes, before + held_bytes);
    JobOutcome outcome = second.run_job(job(dir, "second"));
    const JobOutcome held = first.get();
    EXPECT_FALSE(is_memory_fault(held)) << describe(held);
    std::printf("  beside a holder of %llu MiB: %s; second's peak commit %llu MiB, group peak %llu MiB, "
                "notifications %llu\n",
                static_cast<unsigned long long>(held_bytes / kMiB), describe(outcome).c_str(),
                static_cast<unsigned long long>(outcome.metrics.peak_commit_bytes / kMiB),
                static_cast<unsigned long long>(group->stats().peak_commit_bytes / kMiB),
                static_cast<unsigned long long>(group->stats().limit_notifications));
    return outcome;
}

TEST(WorkerGroup, AStopBesideAnotherWorkerStaysTransientNearTheWorkersOwnLimit) {
    // COR-1 (W12 review): the second worker's own commit plus its refused 980 MiB is within 5 % of
    // its own limit (equal to the group's, as in production), which the own limit's old 5 % fallback
    // took for its own failure (Deterministic: the photo failed for good although it fits alone).
    rts::ScratchDir dir;
    auto group = WorkerGroup::create(1024 * kMiB);
    ASSERT_TRUE(group) << group.error().describe();
    const JobOutcome outcome =
        stopped_beside_a_holder(dir, *group, 100 * kMiB, {"allocate:1027604480"}, 1024 * kMiB);
    ASSERT_TRUE(outcome.failure) << "the second worker's allocation should have failed";
    EXPECT_EQ(outcome.failure->fault, WorkerFault::GroupMemoryLimit) << describe(outcome);
    EXPECT_EQ(outcome.failure->failure_class, FailureClass::Transient);
    EXPECT_EQ(outcome.failure->error.where, "workers.worker.group_memory_limit");
    EXPECT_GE(outcome.metrics.peak_commit_bytes, 1024 * kMiB - 1024 * kMiB / 20)
        << "the refused allocation is charged to the worker's peak";
    EXPECT_LE(outcome.metrics.peak_commit_bytes, 1024 * kMiB);
    // Alone, the same allocation fits.
    WorkerHost alone(host_options(*group, {"allocate:1027604480"}, 1024 * kMiB));
    const JobOutcome retried = alone.run_job(job(dir, "second-alone"));
    EXPECT_FALSE(is_memory_fault(retried)) << describe(retried);
}

TEST(WorkerGroup, AnAllocationRefusedUnderAnotherErrorCodeIsStillTheGroupsStop) {
    // COR-3 (W12 review): the group's notification names the worker whose job failed with Internal
    // (as std::thread's std::system_error or a library's NULL surfaces): the stop is the group's,
    // transient, not the job's own deterministic error.
    rts::ScratchDir dir;
    auto group = WorkerGroup::create(1024 * kMiB);
    ASSERT_TRUE(group) << group.error().describe();
    const JobOutcome outcome = stopped_beside_a_holder(
        dir, *group, 600 * kMiB, {"allocate:629145600", "allocation-error-internal"}, 1024 * kMiB);
    ASSERT_TRUE(outcome.failure);
    EXPECT_EQ(outcome.failure->fault, WorkerFault::GroupMemoryLimit) << describe(outcome);
    EXPECT_EQ(outcome.failure->failure_class, FailureClass::Transient);
    EXPECT_NE(outcome.failure->error.message.find("the test allocation was refused"), std::string::npos)
        << outcome.failure->error.message;
}

TEST(WorkerGroup, ASingleWorkerWithinTheAggregateLimitIsNotStopped) {
    rts::ScratchDir dir;
    auto group = WorkerGroup::create(1024 * kMiB);
    ASSERT_TRUE(group);
    WorkerHost host(host_options(*group, {"allocate:629145600"}, 1024 * kMiB));
    const JobOutcome outcome = host.run_job(job(dir, "alone"));
    EXPECT_FALSE(is_memory_fault(outcome)) << describe(outcome);
    EXPECT_TRUE(host.info().grouped);
    EXPECT_TRUE(host.info().running);
    EXPECT_GT(host.info().idle_commit_bytes, 0u);
    EXPECT_LT(host.info().idle_commit_bytes, 600 * kMiB);  // the allocation was freed
    const WorkerGroupStats stats = (*group)->stats();
    EXPECT_GE(stats.peak_commit_bytes, 600 * kMiB);
    EXPECT_LE(stats.peak_commit_bytes, 1024 * kMiB);
    EXPECT_EQ(stats.limit_notifications, 0u);
    std::printf("  one worker within a 1024 MiB group: peak commit %llu MiB, idle %llu MiB\n",
                static_cast<unsigned long long>(stats.peak_commit_bytes / kMiB),
                static_cast<unsigned long long>(host.info().idle_commit_bytes / kMiB));
    host.shutdown();
    EXPECT_EQ((*group)->stats().members, 0u);
}

TEST(WorkerGroup, AWorkerAloneAboveTheAggregateLimitFailsDeterministically) {
    rts::ScratchDir dir;
    auto group = WorkerGroup::create(512 * kMiB);
    ASSERT_TRUE(group);
    // Its own limit (2 GiB) would admit 700 MiB; the group's does not, and nobody else holds memory.
    WorkerHost host(host_options(*group, {"allocate:734003200"}, 2048 * kMiB));
    const JobRequest request = job(dir, "too-large");
    const JobOutcome outcome = host.run_job(request);
    ASSERT_TRUE(outcome.failure);
    EXPECT_EQ(outcome.failure->fault, WorkerFault::MemoryLimit) << describe(outcome);
    EXPECT_EQ(outcome.failure->failure_class, FailureClass::Deterministic);
    EXPECT_EQ(outcome.failure->error.code, ErrorCode::LimitExceeded);
    EXPECT_EQ(outcome.failure->error.where, "workers.worker.memory_limit");
    EXPECT_NE(outcome.failure->error.message.find("alone exceeded the workers' aggregate memory limit of 512 MiB"),
              std::string::npos)
        << outcome.failure->error.message;
    EXPECT_FALSE(host.info().running);
    EXPECT_FALSE(std::filesystem::exists(request.temp_path));
    std::printf("  alone above: group peak %llu MiB, worker peak %llu MiB\n",
                static_cast<unsigned long long>((*group)->stats().peak_commit_bytes / kMiB),
                static_cast<unsigned long long>(outcome.metrics.peak_commit_bytes / kMiB));
}

TEST(WorkerGroup, WorkersWithoutAGroupKeepTheirOwnLimitOnly) {
    rts::ScratchDir dir;
    WorkerHost host(host_options(nullptr, {"allocate:134217728"}, 1024 * kMiB));
    const JobOutcome outcome = host.run_job(job(dir, "ungrouped"));
    EXPECT_FALSE(is_memory_fault(outcome)) << describe(outcome);
    EXPECT_FALSE(host.info().grouped);
}

}  // namespace
}  // namespace lucent::workers
