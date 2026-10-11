// The lens database loaded off the UI thread (lens_database_source.hpp; W12, review finding APL-06 of
// W13; ADR 0023 amendment): the source's outcome, its listeners and its lifetime; a renderer created
// while the database loads waits for it and corrects exactly as one created with the database; the
// recipe service asks for its engine version once, outside its lock, and records the database in the
// first revision.

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/application/import.hpp>
#include <lucent/application/lens_database_source.hpp>
#include <lucent/application/recipe_service.hpp>
#include <lucent/application/recipe_store.hpp>
#include <lucent/engine/develop.hpp>

#include "app_test_support.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;
namespace rts = raw::test_support;
using namespace std::chrono_literals;

// A gate the test opens: the loader (or the engine version source) waits at it.
class Gate {
public:
    void open() {
        {
            const std::lock_guard lock(mutex_);
            open_ = true;
        }
        cv_.notify_all();
    }
    void pass() {
        {
            const std::lock_guard lock(mutex_);
            ++arrived_;
        }
        cv_.notify_all();
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return open_; });
    }
    // Waits until someone waits at the gate (false after `timeout`).
    bool wait_arrival(std::chrono::milliseconds timeout = 10s) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return arrived_ > 0; });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool open_ = false;
    int arrived_ = 0;
};

// The synthetic DNG's camera ("Lucent Synthetic DNG") with a fixed 28 mm lens that distorts: an exact
// match by the fixed-lens rule (develop_preview_test.cpp, DevelopSessionLens).
std::shared_ptr<const engine::LensDatabase> synthetic_database() {
    constexpr std::string_view kDatabase = R"(<lensdatabase version="1">
        <camera><maker>Lucent</maker><model>Synthetic DNG</model><mount>lucentSyntheticFixed</mount>
            <cropfactor>1</cropfactor></camera>
        <lens><maker>Lucent</maker><model>Synthetic fixed 28mm f/2.8</model><mount>lucentSyntheticFixed</mount>
            <cropfactor>1</cropfactor>
            <calibration><distortion model="poly3" focal="28" k1="-0.04"/></calibration></lens>
    </lensdatabase>)";
    const std::array<engine::LensDatabaseFile, 1> files = {engine::LensDatabaseFile{"test.xml", kDatabase}};
    auto database = engine::LensDatabase::load(
        files, engine::LensDatabaseIdentity{"lens-database-test", "0.0.2", "synthetic"});
    EXPECT_TRUE(database) << (database ? std::string() : database.error().describe());
    return database ? *database : nullptr;
}

// Polls `predicate` until it holds (false after `timeout`).
template <class Predicate>
bool eventually(Predicate predicate, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

// Exact equality of two float images (bit patterns).
bool same_pixels(const image::ImageBuffer<float>& a, const image::ImageBuffer<float>& b) {
    if (a.size() != b.size() || a.channels() != b.channels()) {
        return false;
    }
    const std::size_t row_bytes = static_cast<std::size_t>(a.width()) * static_cast<std::size_t>(a.channels()) * 4;
    for (std::int32_t y = 0; y < a.height(); ++y) {
        if (std::memcmp(a.row(y), b.row(y), row_bytes) != 0) {
            return false;
        }
    }
    return true;
}

assets::LensDatabaseLookup lookup_of(std::shared_ptr<const engine::LensDatabase> database) {
    assets::LensDatabaseLookup lookup;
    lookup.database = std::move(database);
    return lookup;
}

TEST(LensDatabaseSource, NothingIsKnownUntilTheLoaderFinishesAndWaitBlocksUntilThen) {
    Gate gate;
    const auto database = synthetic_database();
    ASSERT_TRUE(database);
    auto source = LensDatabaseSource::start([&] {
        gate.pass();
        return lookup_of(database);
    });
    ASSERT_TRUE(source) << source.error().describe();
    ASSERT_TRUE(gate.wait_arrival());
    EXPECT_EQ((*source)->try_get(), nullptr);
    EXPECT_FALSE((*source)->loaded());
    EXPECT_EQ((*source)->finished_at(), std::chrono::steady_clock::time_point{});

    std::atomic<bool> waited{false};
    std::thread waiter([&] {
        EXPECT_EQ((*source)->database(), database);
        waited = true;
    });
    std::this_thread::sleep_for(200ms);
    EXPECT_FALSE(waited.load()) << "wait() returned before the database had loaded";
    gate.open();
    waiter.join();
    EXPECT_TRUE(waited.load());
    ASSERT_NE((*source)->try_get(), nullptr);
    EXPECT_EQ((*source)->try_get()->database, database);
    EXPECT_TRUE((*source)->loaded());
    EXPECT_FALSE((*source)->wait().problem);
    EXPECT_GT((*source)->finished_at(), std::chrono::steady_clock::time_point{});
    EXPECT_LE((*source)->finished_at(), std::chrono::steady_clock::now());
}

TEST(LensDatabaseSource, ListenersRunOnceInOrderAndALateOneAtOnce) {
    Gate gate;
    auto source = LensDatabaseSource::start([&] {
        gate.pass();
        return lookup_of(nullptr);
    });
    ASSERT_TRUE(source);
    ASSERT_TRUE(gate.wait_arrival());
    std::mutex mutex;
    std::vector<int> calls;
    std::atomic<bool> outcome_known_in_listener{false};
    (*source)->on_ready([&] {
        const std::lock_guard lock(mutex);
        calls.push_back(1);
    });
    (*source)->on_ready([&, s = source->get()] {
        outcome_known_in_listener = s->try_get() != nullptr;  // the outcome is published first
        const std::lock_guard lock(mutex);
        calls.push_back(2);
    });
    (*source)->on_ready([] { throw std::runtime_error("a listener that throws is logged and dropped"); });
    {
        const std::lock_guard lock(mutex);
        EXPECT_TRUE(calls.empty());
    }
    gate.open();
    static_cast<void>((*source)->wait());
    ASSERT_TRUE(eventually([&] {
        const std::lock_guard lock(mutex);
        return calls.size() == 2;
    }));
    EXPECT_TRUE(outcome_known_in_listener.load());
    // A listener added once the outcome is known runs at once, on this thread.
    const std::thread::id here = std::this_thread::get_id();
    std::thread::id ran_on;
    (*source)->on_ready([&] { ran_on = std::this_thread::get_id(); });
    EXPECT_EQ(ran_on, here);
    std::this_thread::sleep_for(50ms);
    const std::lock_guard lock(mutex);
    EXPECT_EQ(calls, (std::vector<int>{1, 2})) << "each listener once, in the order added";
}

TEST(LensDatabaseSource, ALoaderThatFailsGivesAnOutcomeWithItsProblem) {
    auto throwing = LensDatabaseSource::start([]() -> assets::LensDatabaseLookup {
        throw std::runtime_error("the disk went away");
    });
    ASSERT_TRUE(throwing);
    const assets::LensDatabaseLookup& thrown = (*throwing)->wait();
    EXPECT_FALSE(thrown.database);
    ASSERT_TRUE(thrown.problem);
    EXPECT_EQ(thrown.problem->code, ErrorCode::Internal);
    EXPECT_NE(thrown.problem->message.find("the disk went away"), std::string::npos) << thrown.problem->message;

    auto exhausted = LensDatabaseSource::start([]() -> assets::LensDatabaseLookup { throw std::bad_alloc(); });
    ASSERT_TRUE(exhausted);
    ASSERT_TRUE((*exhausted)->wait().problem);
    EXPECT_EQ((*exhausted)->wait().problem->code, ErrorCode::OutOfMemory);

    auto empty = LensDatabaseSource::start({});
    ASSERT_TRUE(empty);
    ASSERT_TRUE((*empty)->wait().problem);
    EXPECT_FALSE((*empty)->database());

    // The loader's own outcome without a database (a missing asset) is passed on as it is.
    auto missing = LensDatabaseSource::start([] {
        assets::LensDatabaseLookup lookup;
        lookup.problem = Error{ErrorCode::NotFound, "no lens database", "assets.lens", 0};
        return lookup;
    });
    ASSERT_TRUE(missing);
    ASSERT_TRUE((*missing)->wait().problem);
    EXPECT_EQ((*missing)->wait().problem->code, ErrorCode::NotFound);
}

TEST(LensDatabaseSource, TheDestructorWaitsForTheLoadingThread) {
    std::atomic<bool> finished{false};
    {
        auto source = LensDatabaseSource::start([&] {
            std::this_thread::sleep_for(150ms);
            finished = true;
            return lookup_of(nullptr);
        });
        ASSERT_TRUE(source);
    }
    EXPECT_TRUE(finished.load()) << "the source was destroyed while its loader still ran";
}

TEST(LensDatabaseSource, AReadySourceIsLoadedAtOnce) {
    const auto database = synthetic_database();
    auto source = LensDatabaseSource::ready(lookup_of(database));
    ASSERT_NE(source->try_get(), nullptr);
    EXPECT_EQ(source->database(), database);
    bool told = false;
    source->on_ready([&] { told = true; });
    EXPECT_TRUE(told);
}

// A renderer created while the lens database loads (Lucent.exe's, W12) renders nothing until it has
// loaded, then corrects exactly as a renderer created with the database: pending, never wrong.
TEST(EngineRendererLensSource, ARenderWaitsForTheLoadingDatabaseAndCorrectsWithIt) {
    const auto database = synthetic_database();
    ASSERT_TRUE(database);
    auto registry = ts::default_registry();
    ts::ScratchDir dir;
    rts::SyntheticDngSpec spec;
    spec.width = 320;
    spec.height = 240;
    const std::filesystem::path path = ts::write_dng(dir, L"lens.dng", spec);
    SessionLibrary library;
    auto report = import_sources(library, *registry, ImportRequest{.files = {path}});
    ASSERT_TRUE(report);
    const SourceItem item = *library.find(report->added.at(0));
    const engine::DevelopSettings settings;  // automatic lens corrections (all on)

    EngineRendererOptions direct_options;
    direct_options.lens_database = database;
    auto direct = create_engine_renderer(registry, direct_options);
    ASSERT_TRUE(direct);
    auto expected = (*direct)->render(item, settings, {}, {});
    ASSERT_TRUE(expected) << expected.error().describe();
    ASSERT_TRUE(expected->report.lens_correction.plan.geometry.has_value())
        << "the synthetic camera's fixed lens must be corrected";

    Gate gate;
    auto source = LensDatabaseSource::start([&] {
        gate.pass();
        return lookup_of(database);
    });
    ASSERT_TRUE(source);
    EngineRendererOptions loading_options;
    loading_options.lens_database_source = *source;
    auto loading = create_engine_renderer(registry, loading_options);
    ASSERT_TRUE(loading);
    ASSERT_TRUE(gate.wait_arrival());

    std::atomic<bool> done{false};
    Expected<DevelopRender> rendered = fail(ErrorCode::Internal, "not rendered");
    std::optional<engine::LensDatabaseIdentity> identity;
    std::thread render([&] {
        rendered = (*loading)->render(item, settings, {}, {});
        identity = (*loading)->lens_database();
        done = true;
    });
    std::this_thread::sleep_for(300ms);
    EXPECT_FALSE(done.load()) << "the render did not wait for the lens database";
    gate.open();
    render.join();
    ASSERT_TRUE(rendered) << rendered.error().describe();
    EXPECT_TRUE(rendered->report.lens_correction.plan.geometry.has_value());
    EXPECT_TRUE(same_pixels(rendered->display.pixels, expected->display.pixels))
        << "not the same pixels as with the database given";
    ASSERT_TRUE(identity);
    EXPECT_EQ(identity->asset_version, "0.0.2");

    // A source that loaded no database: renders run without corrections, as with no database at all.
    EngineRendererOptions none_options;
    none_options.lens_database_source = LensDatabaseSource::ready(lookup_of(nullptr));
    auto none = create_engine_renderer(registry, none_options);
    ASSERT_TRUE(none);
    auto plain = (*none)->render(item, settings, {}, {});
    ASSERT_TRUE(plain) << plain.error().describe();
    EXPECT_FALSE(plain->report.lens_correction.plan.geometry.has_value());
    EXPECT_FALSE((*none)->lens_database());
}

// The recipe service asks for its engine version once, by the first revision, without holding its lock
// while it waits (readers are served meanwhile), and records it in every revision (W12, APL-06).
TEST(RecipeServiceEngineVersion, TheSourceIsAskedOnceOutsideTheLockAndRecordedInEveryRevision) {
    auto registry = ts::default_registry();
    ts::ScratchDir dir;
    SessionLibrary library;
    auto report = import_sources(library, *registry, ImportRequest{.files = {ts::write_dng(dir, L"a.dng")}});
    ASSERT_TRUE(report);
    const ItemId item = report->added.at(0);
    auto store = create_memory_recipe_store();
    Gate gate;
    std::atomic<int> asked{0};
    RecipeServiceOptions options;
    options.registry = registry;
    options.engine_version = "ignored while the source gives a version";
    options.engine_version_source = [&] {
        ++asked;
        gate.pass();
        return std::string("9.9.9; lens database lens-database 0.0.2 (manifest test)");
    };
    RecipeService service(library, store, options);
    EXPECT_EQ(asked.load(), 0) << "nothing is asked before a revision is written";

    engine::DevelopSettings brighter;
    brighter.exposure_ev = 0.5;
    std::atomic<bool> applied{false};
    std::thread writer([&] {
        EXPECT_TRUE(service.apply(item, brighter));
        applied = true;
    });
    ASSERT_TRUE(gate.wait_arrival());
    // The writer waits for the version without the service's lock: a reader is served meanwhile.
    const auto read_started = std::chrono::steady_clock::now();
    auto state = service.state(item);
    EXPECT_TRUE(state);
    EXPECT_LT(std::chrono::steady_clock::now() - read_started, 2s);
    EXPECT_FALSE(applied.load());
    gate.open();
    writer.join();
    ASSERT_TRUE(applied.load());

    engine::DevelopSettings darker;
    darker.exposure_ev = -0.5;
    ASSERT_TRUE(service.apply(item, darker));
    EXPECT_EQ(service.engine_version(), "9.9.9; lens database lens-database 0.0.2 (manifest test)");
    EXPECT_EQ(asked.load(), 1) << "asked once, then kept";
    auto history = store->history(item);
    ASSERT_TRUE(history) << history.error().describe();
    ASSERT_EQ(history->revisions.size(), 2u);
    for (const catalog::RevisionInfo& revision : history->revisions) {
        EXPECT_EQ(revision.engine_version, "9.9.9; lens database lens-database 0.0.2 (manifest test)");
    }
}

}  // namespace
}  // namespace lucent::app
