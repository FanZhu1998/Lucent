// MaintenanceService: background checks, hashing and relinking on the executor's Background lane,
// chained follow-ups (a changed file found while hashing is re-read and hashed again), progress and
// Idle events, cancellation and shutdown with work queued.

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include <lucent/application/import.hpp>
#include <lucent/application/maintenance_service.hpp>

#include "app_test_support.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;
namespace rts = raw::test_support;

class MaintenanceServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry_ = ts::default_registry();
        fixture_ = ts::CatalogFixture::open(dir_.path() / L"data\\lucent.catalog");
        ASSERT_NE(fixture_.library, nullptr);
        auto executor = TaskExecutor::create(ExecutorOptions::for_this_machine());
        ASSERT_TRUE(executor);
        executor_ = std::move(*executor);
    }

    void TearDown() override {
        service_.reset();
        executor_.reset();
    }

    void start(MaintenanceServiceOptions options = {}) {
        options.observer = [this](const MaintenanceEvent& event) {
            std::lock_guard lock(mutex_);
            events_.push_back(event);
        };
        service_ = std::make_unique<MaintenanceService>(*fixture_.library, *registry_, *executor_, options);
    }

    void import(const std::filesystem::path& folder) {
        auto report =
            import_sources(*fixture_.library, *registry_, ImportRequest{.folders = {{folder, true}}});
        ASSERT_TRUE(report) << report.error().describe();
    }

    // Waits for the n-th Idle event and returns it.
    MaintenanceEvent wait_idle(std::size_t n = 1) {
        const auto deadline = std::chrono::steady_clock::now() + ts::kWaitTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard lock(mutex_);
                std::size_t seen = 0;
                for (const MaintenanceEvent& event : events_) {
                    if (event.kind == MaintenanceEvent::Kind::Idle && ++seen == n) {
                        return event;
                    }
                }
            }
            Sleep(2);
        }
        ADD_FAILURE() << "the maintenance service did not become idle";
        return {};
    }

    // Occupies both Background threads until the returned gate is opened.
    std::shared_ptr<ts::Gate> block_background() {
        auto gate = std::make_shared<ts::Gate>();
        auto started = std::make_shared<std::atomic<int>>(0);
        for (int i = 0; i < 2; ++i) {
            auto handle = executor_->submit(Lane::Background, [gate, started](TaskContext& context) {
                ++*started;
                gate->wait(context.cancel());
            });
            EXPECT_TRUE(handle);
        }
        while (started->load() < 2) {
            Sleep(1);
        }
        return gate;
    }

    std::size_t progress_events() {
        std::lock_guard lock(mutex_);
        return static_cast<std::size_t>(std::count_if(
            events_.begin(), events_.end(),
            [](const MaintenanceEvent& e) { return e.kind == MaintenanceEvent::Kind::Progress; }));
    }

    rts::ScratchDir dir_;
    std::shared_ptr<const raw::DecoderRegistry> registry_;
    ts::CatalogFixture fixture_;
    std::unique_ptr<TaskExecutor> executor_;
    std::unique_ptr<MaintenanceService> service_;
    std::mutex mutex_;
    std::vector<MaintenanceEvent> events_;
};

TEST_F(MaintenanceServiceTest, ACheckOfEveryFileIsFollowedByHashing) {
    ts::write_dng(dir_, L"in\\a.dng");
    ts::write_dng(dir_, L"in\\b.dng", rts::SyntheticDngSpec{.model = "Synthetic DNA"});
    ts::write_dng(dir_, L"gone\\c.dng", rts::SyntheticDngSpec{.model = "Synthetic DNC"});
    import(dir_.path() / L"in");
    import(dir_.path() / L"gone");
    std::filesystem::remove_all(dir_.path() / L"gone");
    start();

    ts::Collector<Expected<RefreshReport>> checked;
    ASSERT_TRUE(service_->check_all([&](const Expected<RefreshReport>& report) { checked.add(report); }));
    ASSERT_TRUE(checked.wait_for(1));
    const Expected<RefreshReport> report = checked.values().front();
    ASSERT_TRUE(report);
    EXPECT_EQ(report->missing, 1u);
    EXPECT_EQ(report->unhashed_items.size(), 2u);

    const MaintenanceEvent idle = wait_idle();
    EXPECT_EQ(idle.hashed, 2u);
    EXPECT_EQ(idle.missing, 1u);
    EXPECT_EQ(idle.hash_items_total, 2u);
    EXPECT_EQ(idle.hash_items_done, 2u);
    EXPECT_GT(progress_events(), 0u);
    EXPECT_FALSE(service_->busy());
    for (const SourceItem& item : fixture_.library->items()) {
        if (item.path.filename() == L"c.dng") {
            EXPECT_EQ(item.source_state, SourceState::Missing);
        } else {
            EXPECT_TRUE(item.sha256) << item.path.string();
        }
    }
}

TEST_F(MaintenanceServiceTest, AFileChangedBeforeItsHashIsReReadAndHashed) {
    const auto path = ts::write_dng(dir_, L"in\\a.dng");
    import(dir_.path() / L"in");
    const SourceItem before = fixture_.library->items().front();
    Sleep(20);
    ts::write_dng(dir_, L"in\\a.dng", rts::SyntheticDngSpec{.orientation = 3});
    start();
    ASSERT_TRUE(service_->hash({before.id}));
    const MaintenanceEvent idle = wait_idle();
    EXPECT_EQ(idle.changed, 1u);
    EXPECT_EQ(idle.hashed, 1u);  // after the re-read
    const SourceItem after = *fixture_.library->find(before.id);
    EXPECT_EQ(after.source_state, SourceState::Present);
    EXPECT_EQ(after.probe->orientation, image::Orientation::Rotate180);
    ASSERT_TRUE(after.sha256);
    EXPECT_EQ(*after.sha256, ts::file_sha256(path));
}

TEST_F(MaintenanceServiceTest, RelinkRunsInTheBackgroundAndReports) {
    ts::write_dng(dir_, L"old\\a.dng");
    import(dir_.path() / L"old");
    start();
    ASSERT_TRUE(service_->hash({fixture_.library->items().front().id}));
    wait_idle();
    std::filesystem::rename(dir_.path() / L"old", dir_.path() / L"new");
    ts::Collector<Expected<RefreshReport>> checked;
    ASSERT_TRUE(service_->check_all([&](const Expected<RefreshReport>& r) { checked.add(r); }));
    ASSERT_TRUE(checked.wait_for(1));

    ts::Collector<Expected<RelinkReport>> relinked;
    ASSERT_TRUE(service_->relink(RelinkRequest{.folder = dir_.path() / L"new"},
                                 [&](const Expected<RelinkReport>& r) { relinked.add(r); }));
    ASSERT_TRUE(relinked.wait_for(1));
    const Expected<RelinkReport> report = relinked.values().front();
    ASSERT_TRUE(report) << report.error().describe();
    EXPECT_EQ(report->relinked, 1u);
    EXPECT_EQ(fixture_.library->items().front().source_state, SourceState::Present);
}

TEST_F(MaintenanceServiceTest, ShutdownFailsQueuedRequestsWithCancelled) {
    ts::write_dng(dir_, L"in\\a.dng");
    import(dir_.path() / L"in");
    start();
    const std::shared_ptr<ts::Gate> gate = block_background();  // nothing of the service starts
    ASSERT_TRUE(service_->hash({fixture_.library->items().front().id}));
    std::atomic<int> cancelled{0};
    std::atomic<int> calls{0};
    ASSERT_TRUE(service_->relink(RelinkRequest{.folder = dir_.path()}, [&](const Expected<RelinkReport>& r) {
        ++calls;
        cancelled += (!r && r.error().code == ErrorCode::Cancelled) ? 1 : 0;
    }));
    ASSERT_TRUE(service_->check_all([&](const Expected<RefreshReport>& r) {
        ++calls;
        cancelled += (!r && r.error().code == ErrorCode::Cancelled) ? 1 : 0;
    }));
    EXPECT_TRUE(service_->busy());
    // The destructor waits for the step it submitted, which can only run once the lane is free.
    std::thread opener([gate] {
        Sleep(50);
        gate->open();
    });
    service_.reset();
    opener.join();
    EXPECT_EQ(calls.load(), 2);  // every accepted request is answered exactly once
    EXPECT_EQ(cancelled.load(), 2);
    EXPECT_FALSE(fixture_.library->items().front().sha256);
}

TEST_F(MaintenanceServiceTest, CancelAllDropsQueuedWork) {
    ts::write_dng(dir_, L"in\\a.dng");
    ts::write_dng(dir_, L"in\\b.dng", rts::SyntheticDngSpec{.model = "Synthetic DNA"});
    import(dir_.path() / L"in");
    start();
    const std::shared_ptr<ts::Gate> gate = block_background();
    std::vector<ItemId> all;
    for (const SourceItem& item : fixture_.library->items()) {
        all.push_back(item.id);
    }
    ASSERT_TRUE(service_->hash(all));
    service_->cancel_all();
    gate->open();
    wait_idle();
    for (const SourceItem& item : fixture_.library->items()) {
        EXPECT_FALSE(item.sha256);
    }
    EXPECT_FALSE(service_->busy());

    // The service accepts new work afterwards.
    ASSERT_TRUE(service_->hash(all));
    EXPECT_EQ(wait_idle(2).hashed, 2u);
}

TEST_F(MaintenanceServiceTest, ALibraryWithoutSourceFactsIsRefused) {
    SessionLibrary session;
    MaintenanceService service(session, *registry_, *executor_);
    EXPECT_EQ(service.check_all().error().code, ErrorCode::Unsupported);
    EXPECT_EQ(service.hash({ItemId{1}}).error().code, ErrorCode::Unsupported);
    EXPECT_EQ(service.relink(RelinkRequest{}, [](const Expected<RelinkReport>&) {}).error().code,
              ErrorCode::Unsupported);
    EXPECT_EQ(service.relink(RelinkRequest{}, {}).error().code, ErrorCode::InvalidArgument);
}

}  // namespace
}  // namespace lucent::app
