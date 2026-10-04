// TaskExecutor lanes, cancellation and shutdown; MemoryAdmission ordering and failure paths.
// Scheduling is made deterministic with gates: tasks block until the test opens them.

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/application/task_executor.hpp>

#include "app_test_support.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;

std::unique_ptr<TaskExecutor> make_executor(std::uint32_t workers, std::uint32_t reserved,
                                            AdmissionOptions admission = {}, std::uint32_t reserved_quick = 0) {
    ExecutorOptions options;
    options.workers = workers;
    options.reserved_interactive = reserved;
    options.reserved_quick = reserved_quick;
    options.admission = admission;
    auto executor = TaskExecutor::create(options);
    EXPECT_TRUE(executor) << executor.error().describe();
    return std::move(*executor);
}

template <class Predicate>
bool eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + ts::kWaitTimeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

TEST(TaskExecutor, ValidatesOptions) {
    ExecutorOptions options;
    options.workers = 1;
    EXPECT_EQ(TaskExecutor::create(options).error().code, ErrorCode::InvalidArgument);
    options.workers = 3;
    options.reserved_interactive = 3;
    EXPECT_EQ(TaskExecutor::create(options).error().code, ErrorCode::InvalidArgument);
    options.reserved_interactive = 0;
    EXPECT_FALSE(options.validate());
    options.reserved_interactive = 1;
    options.reserved_quick = 2;  // would leave background work no thread
    EXPECT_EQ(options.validate().error().code, ErrorCode::InvalidArgument);
    options.reserved_quick = 1;
    EXPECT_TRUE(options.validate());
    options.admission.max_concurrent = 0;
    EXPECT_FALSE(options.validate());
    const ExecutorOptions defaults = ExecutorOptions::for_this_machine();
    EXPECT_TRUE(defaults.validate());
    // Two background threads (an import beside an export), one kept for quick work and one for
    // interactive work, whatever the core count.
    EXPECT_EQ(defaults.workers, 4u);
    EXPECT_EQ(defaults.reserved_interactive, 1u);
    EXPECT_EQ(defaults.reserved_quick, 1u);
    EXPECT_EQ(defaults.admission.max_concurrent, 1u);
}

TEST(TaskExecutor, RunsTasksAndReportsCompletion) {
    auto executor = make_executor(2, 1);
    std::atomic<int> runs{0};
    auto handle = executor->submit(Lane::Background, [&](TaskContext& context) {
        EXPECT_EQ(context.lane(), Lane::Background);
        EXPECT_FALSE(context.cancel().is_cancelled());
        ++runs;
    });
    ASSERT_TRUE(handle);
    handle->wait();
    EXPECT_TRUE(handle->finished());
    EXPECT_EQ(runs.load(), 1);
    EXPECT_GT(handle->id(), 0u);
    EXPECT_EQ(executor->submit(Lane::Interactive, TaskFunction{}).error().code, ErrorCode::InvalidArgument);
}

TEST(TaskExecutor, BackgroundWorkNeverTakesTheReservedWorkers) {
    auto executor = make_executor(3, 1);
    auto gate = std::make_shared<ts::Gate>();
    std::vector<TaskHandle> background;
    for (int i = 0; i < 4; ++i) {
        auto h = executor->submit(Lane::Background, [gate](TaskContext& c) { gate->wait(c.cancel()); });
        ASSERT_TRUE(h);
        background.push_back(*h);
    }
    ASSERT_TRUE(eventually([&] { return executor->stats().running_background == 2; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ExecutorStats stats = executor->stats();
    EXPECT_EQ(stats.running_background, 2u);  // 3 workers - 1 reserved
    EXPECT_EQ(stats.queued_background, 2u);

    // Interactive work still runs at once on the reserved worker.
    std::atomic<bool> ran{false};
    auto interactive = executor->submit(Lane::Interactive, [&](TaskContext&) { ran = true; });
    ASSERT_TRUE(interactive);
    interactive->wait();
    EXPECT_TRUE(ran.load());
    EXPECT_FALSE(background[0].finished());

    gate->open();
    for (const TaskHandle& h : background) {
        h.wait();
    }
    EXPECT_EQ(executor->stats().completed, 5u);
}

TEST(TaskExecutor, QuickWorkKeepsItsReservedThreadWhateverTheBackgroundLoad) {
    // The defaults: 4 workers, 1 reserved for interactive and 1 for quick work.
    auto executor = make_executor(4, 1, {}, 1);
    auto gate = std::make_shared<ts::Gate>();
    std::vector<TaskHandle> background;
    for (int i = 0; i < 4; ++i) {  // exports waiting for admission, a long import, ...
        auto h = executor->submit(Lane::Background, [gate](TaskContext& c) { gate->wait(c.cancel()); });
        ASSERT_TRUE(h);
        background.push_back(*h);
    }
    ASSERT_TRUE(eventually([&] { return executor->stats().running_background == 2; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(executor->stats().running_background, 2u);
    EXPECT_EQ(executor->stats().queued_background, 2u);

    // Quick work (a camera preview) still starts at once, ahead of the queued background tasks.
    std::atomic<int> quick_runs{0};
    for (int i = 0; i < 3; ++i) {
        auto quick = executor->submit(Lane::Quick, [&](TaskContext& c) {
            EXPECT_EQ(c.lane(), Lane::Quick);
            ++quick_runs;
        });
        ASSERT_TRUE(quick);
        quick->wait();
    }
    EXPECT_EQ(quick_runs.load(), 3);
    EXPECT_FALSE(background[0].finished());
    EXPECT_EQ(executor->stats().queued_background, 2u);

    gate->open();
    for (const TaskHandle& h : background) {
        h.wait();
    }
}

TEST(TaskExecutor, QuickWorkNeverTakesTheInteractiveReservation) {
    auto executor = make_executor(3, 1, {}, 1);
    auto gate = std::make_shared<ts::Gate>();
    std::vector<TaskHandle> quick;
    for (int i = 0; i < 3; ++i) {
        auto h = executor->submit(Lane::Quick, [gate](TaskContext& c) { gate->wait(c.cancel()); });
        ASSERT_TRUE(h);
        quick.push_back(*h);
    }
    ASSERT_TRUE(eventually([&] { return executor->stats().running_quick == 2; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ExecutorStats stats = executor->stats();
    EXPECT_EQ(stats.running_quick, 2u);  // 3 workers - 1 reserved for interactive work
    EXPECT_EQ(stats.queued_quick, 1u);
    // Background work cannot start either: quick and background share the non-reserved threads.
    std::atomic<bool> background_ran{false};
    auto background = executor->submit(Lane::Background, [&](TaskContext&) { background_ran = true; });
    ASSERT_TRUE(background);
    std::atomic<bool> interactive_ran{false};
    auto interactive = executor->submit(Lane::Interactive, [&](TaskContext&) { interactive_ran = true; });
    ASSERT_TRUE(interactive);
    interactive->wait();
    EXPECT_TRUE(interactive_ran.load());
    EXPECT_FALSE(background_ran.load());

    gate->open();
    background->wait();
    for (const TaskHandle& h : quick) {
        h.wait();
    }
    EXPECT_TRUE(background_ran.load());
}

TEST(TaskExecutor, QuickTasksAreTakenBeforeEarlierBackgroundTasks) {
    auto executor = make_executor(3, 1);  // no quick reservation: quick work waits for a free thread
    auto hold_a = std::make_shared<ts::Gate>();
    auto hold_b = std::make_shared<ts::Gate>();
    auto a = executor->submit(Lane::Background, [hold_a](TaskContext& c) { hold_a->wait(c.cancel()); });
    auto b = executor->submit(Lane::Background, [hold_b](TaskContext& c) { hold_b->wait(c.cancel()); });
    ASSERT_TRUE(a && b);
    ASSERT_TRUE(eventually([&] { return executor->stats().running_background == 2; }));
    std::mutex mutex;
    std::vector<std::string> order;
    auto later_background = executor->submit(Lane::Background, [&](TaskContext&) {
        std::lock_guard lock(mutex);
        order.push_back("background");
    });
    auto quick = executor->submit(Lane::Quick, [&](TaskContext&) {
        std::lock_guard lock(mutex);
        order.push_back("quick");
    });
    ASSERT_TRUE(later_background && quick);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(executor->stats().queued_quick, 1u);
    hold_a->open();  // one thread frees up: the quick task goes first
    quick->wait();
    later_background->wait();
    hold_b->open();
    b->wait();
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], "quick");
}

TEST(TaskExecutor, InteractiveTasksAreTakenBeforeEarlierBackgroundTasks) {
    auto executor = make_executor(2, 1);
    auto hold_a = std::make_shared<ts::Gate>();
    auto hold_b = std::make_shared<ts::Gate>();
    // Occupy both workers, then free exactly one: it must take the interactive task first.
    auto a = executor->submit(Lane::Interactive, [hold_a](TaskContext& c) { hold_a->wait(c.cancel()); });
    auto b = executor->submit(Lane::Interactive, [hold_b](TaskContext& c) { hold_b->wait(c.cancel()); });
    ASSERT_TRUE(a && b);
    ASSERT_TRUE(eventually([&] { return executor->stats().running_interactive == 2; }));

    std::mutex mutex;
    std::vector<std::string> order;
    auto record = [&](std::string name) {
        return [&, name](TaskContext&) {
            std::lock_guard lock(mutex);
            order.push_back(name);
        };
    };
    auto background = executor->submit(Lane::Background, record("background"));
    auto interactive = executor->submit(Lane::Interactive, record("interactive"));
    ASSERT_TRUE(background && interactive);
    hold_a->open();
    background->wait();
    interactive->wait();
    hold_b->open();
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], "interactive");
    EXPECT_EQ(order[1], "background");
}

TEST(TaskExecutor, CancelledTasksStillRunWithACancelledToken) {
    auto executor = make_executor(2, 1);
    auto hold = std::make_shared<ts::Gate>();
    auto busy = executor->submit(Lane::Background, [hold](TaskContext& c) { hold->wait(c.cancel()); });
    ASSERT_TRUE(busy);
    ASSERT_TRUE(eventually([&] { return executor->stats().running_background == 1; }));
    std::atomic<int> saw_cancelled{-1};
    auto queued = executor->submit(
        Lane::Background, [&](TaskContext& c) { saw_cancelled = c.cancel().is_cancelled() ? 1 : 0; });
    ASSERT_TRUE(queued);
    queued->cancel();
    hold->open();
    queued->wait();
    EXPECT_EQ(saw_cancelled.load(), 1);
}

TEST(TaskExecutor, ShutdownCancelsRunningAndQueuedTasksAndRunsThemAll) {
    auto executor = make_executor(2, 1);
    auto never = std::make_shared<ts::Gate>();
    std::atomic<int> cancelled_runs{0};
    std::vector<TaskHandle> handles;
    for (int i = 0; i < 5; ++i) {
        auto h = executor->submit(Lane::Background, [&, never](TaskContext& c) {
            never->wait(c.cancel());  // returns once cancelled
            if (c.cancel().is_cancelled()) {
                ++cancelled_runs;
            }
        });
        ASSERT_TRUE(h);
        handles.push_back(*h);
    }
    ASSERT_TRUE(eventually([&] { return executor->stats().running_background == 1; }));
    executor->shutdown();
    for (const TaskHandle& h : handles) {
        EXPECT_TRUE(h.finished());
    }
    EXPECT_EQ(cancelled_runs.load(), 5);
    EXPECT_EQ(executor->submit(Lane::Interactive, [](TaskContext&) {}).error().code, ErrorCode::Cancelled);
    executor->shutdown();  // idempotent
}

TEST(TaskExecutor, AThrowingTaskDoesNotStopTheWorker) {
    auto executor = make_executor(2, 1);
    auto bad =
        executor->submit(Lane::Interactive, [](TaskContext&) { throw std::runtime_error("task failure"); });
    ASSERT_TRUE(bad);
    bad->wait();
    std::atomic<bool> ran{false};
    auto good = executor->submit(Lane::Interactive, [&](TaskContext&) { ran = true; });
    ASSERT_TRUE(good);
    good->wait();
    EXPECT_TRUE(ran.load());
}

// ---- Memory admission ---------------------------------------------------------------------

TEST(MemoryAdmission, LimitsConcurrentLeases) {
    MemoryAdmission admission(AdmissionOptions{1, 1000});
    auto first = admission.acquire(600, Lane::Background, {});
    ASSERT_TRUE(first);
    std::atomic<bool> second_granted{false};
    std::thread waiter([&] {
        auto second = admission.acquire(100, Lane::Background, {});
        ASSERT_TRUE(second);
        second_granted = true;
    });
    ASSERT_TRUE(eventually([&] { return admission.stats().waiting_background == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_FALSE(second_granted.load());
    EXPECT_EQ(admission.stats().active, 1u);
    EXPECT_EQ(admission.stats().active_bytes, 600u);
    first->release();
    waiter.join();
    EXPECT_TRUE(second_granted.load());
    EXPECT_EQ(admission.stats().active, 0u);
    EXPECT_EQ(admission.stats().granted, 2u);
}

TEST(MemoryAdmission, RespectsTheByteBudget) {
    MemoryAdmission admission(AdmissionOptions{4, 1000});
    auto a = admission.acquire(700, Lane::Interactive, {});
    ASSERT_TRUE(a);
    auto b = admission.acquire(300, Lane::Interactive, {});  // exactly fits
    ASSERT_TRUE(b);
    core::CancellationSource cancel;
    std::thread waiter([&] {
        auto c = admission.acquire(1, Lane::Interactive, cancel.token());
        ASSERT_FALSE(c);
        EXPECT_EQ(c.error().code, ErrorCode::Cancelled);
    });
    ASSERT_TRUE(eventually([&] { return admission.stats().waiting_interactive == 1; }));
    cancel.cancel();
    waiter.join();
    EXPECT_EQ(admission.stats().waiting_interactive, 0u);

    auto too_big = admission.acquire(1001, Lane::Interactive, {});
    ASSERT_FALSE(too_big);
    EXPECT_EQ(too_big.error().code, ErrorCode::LimitExceeded);
}

TEST(MemoryAdmission, RetainedMemoryCountsBytesButHoldsNoPlace) {
    MemoryAdmission admission(AdmissionOptions{1, 1000});
    auto render = admission.acquire(600, Lane::Background, {});
    ASSERT_TRUE(render);
    // The only render place is taken, but a cached image or a small render is still admitted.
    auto cached = admission.retain(300, {});
    ASSERT_TRUE(cached) << cached.error().describe();
    EXPECT_TRUE(cached->retained());
    AdmissionStats stats = admission.stats();
    EXPECT_EQ(stats.active, 1u);
    EXPECT_EQ(stats.retained, 1u);
    EXPECT_EQ(stats.active_bytes, 900u);
    EXPECT_EQ(stats.retained_bytes, 300u);
    // Retained claims still respect the budget: this one waits, and gives up when cancelled.
    core::CancellationSource cancel;
    std::thread waiter([&] {
        auto over = admission.retain(200, cancel.token());
        ASSERT_FALSE(over);
        EXPECT_EQ(over.error().code, ErrorCode::Cancelled);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    cancel.cancel();
    waiter.join();
    EXPECT_EQ(admission.retain(1001, {}).error().code, ErrorCode::LimitExceeded);

    // A finished render keeps only its result: its place and the rest of its bytes return at once.
    render->retain_only(100);
    EXPECT_TRUE(render->retained());
    EXPECT_EQ(render->bytes(), 100u);
    stats = admission.stats();
    EXPECT_EQ(stats.active, 0u);
    EXPECT_EQ(stats.retained, 2u);
    EXPECT_EQ(stats.active_bytes, 400u);
    // So the next render is admitted beside the retained memory, and must fit beside it.
    auto next = admission.acquire(600, Lane::Interactive, {});
    ASSERT_TRUE(next);
    render->retain_only(50);  // shrinking a retained lease
    EXPECT_EQ(admission.stats().active_bytes, 950u);
    render->release();
    cached->release();
    next->release();
    stats = admission.stats();
    EXPECT_EQ(stats.active_bytes, 0u);
    EXPECT_EQ(stats.retained, 0u);
    EXPECT_EQ(stats.active, 0u);
}

TEST(MemoryAdmission, InteractiveClaimsGoFirst) {
    MemoryAdmission admission(AdmissionOptions{1, 1000});
    auto held = admission.acquire(10, Lane::Background, {});
    ASSERT_TRUE(held);
    std::mutex mutex;
    std::vector<std::string> order;
    std::thread background([&] {
        auto lease = admission.acquire(10, Lane::Background, {});
        ASSERT_TRUE(lease);
        std::lock_guard lock(mutex);
        order.push_back("background");
    });
    ASSERT_TRUE(eventually([&] { return admission.stats().waiting_background == 1; }));
    std::thread interactive([&] {
        auto lease = admission.acquire(10, Lane::Interactive, {});
        ASSERT_TRUE(lease);
        std::lock_guard lock(mutex);
        order.push_back("interactive");
    });
    ASSERT_TRUE(eventually([&] { return admission.stats().waiting_interactive == 1; }));
    held->release();
    background.join();
    interactive.join();
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], "interactive");
}

TEST(MemoryAdmission, QuickClaimsQueueWithBackgroundClaims) {
    MemoryAdmission admission(AdmissionOptions{1, 1000});
    auto held = admission.acquire(10, Lane::Quick, {});
    ASSERT_TRUE(held);
    std::thread quick([&] { ASSERT_TRUE(admission.acquire(10, Lane::Quick, {})); });
    ASSERT_TRUE(eventually([&] { return admission.stats().waiting_background == 1; }));
    EXPECT_EQ(admission.stats().waiting_interactive, 0u);
    held->release();
    quick.join();
    EXPECT_EQ(admission.stats().granted, 2u);
}

TEST(MemoryAdmission, ShutdownFailsWaitersAndLaterClaims) {
    auto admission = std::make_unique<MemoryAdmission>(AdmissionOptions{1, 100});
    auto held = admission->acquire(10, Lane::Background, {});
    ASSERT_TRUE(held);
    std::thread waiter([&] {
        auto lease = admission->acquire(10, Lane::Background, {});
        ASSERT_FALSE(lease);
        EXPECT_EQ(lease.error().code, ErrorCode::Cancelled);
    });
    ASSERT_TRUE(eventually([&] { return admission->stats().waiting_background == 1; }));
    admission->shutdown();
    waiter.join();
    EXPECT_EQ(admission->acquire(1, Lane::Interactive, {}).error().code, ErrorCode::Cancelled);
    admission.reset();
    held->release();  // a lease outliving its admission is safe to release
}

TEST(MemoryAdmission, TasksAdmitThroughTheirContext) {
    auto executor = make_executor(2, 1, AdmissionOptions{1, 1000});
    auto hold = std::make_shared<ts::Gate>();
    std::atomic<int> stage{0};
    auto first = executor->submit(Lane::Background, [&, hold](TaskContext& c) {
        auto lease = c.admit(500);
        ASSERT_TRUE(lease);
        stage = 1;
        hold->wait(c.cancel());
    });
    ASSERT_TRUE(first);
    ASSERT_TRUE(eventually([&] { return stage.load() == 1; }));
    std::atomic<bool> admitted{false};
    auto second = executor->submit(Lane::Interactive, [&](TaskContext& c) {
        auto lease = c.admit(500);
        admitted = static_cast<bool>(lease);
    });
    ASSERT_TRUE(second);
    ASSERT_TRUE(eventually([&] { return executor->stats().admission.waiting_interactive == 1; }));
    EXPECT_FALSE(admitted.load());
    hold->open();
    second->wait();
    EXPECT_TRUE(admitted.load());
}

TEST(TaskExecutor, LaneNames) {
    EXPECT_EQ(to_string(Lane::Interactive), "interactive");
    EXPECT_EQ(to_string(Lane::Background), "background");
    EXPECT_EQ(to_string(Lane::Quick), "quick");
}

}  // namespace
}  // namespace lucent::app
