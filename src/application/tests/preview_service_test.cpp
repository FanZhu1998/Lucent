// PreviewService: delivery, caching, stale-generation suppression and cancellation with
// deterministic scheduling (a scripted renderer whose renders block until the test opens a gate),
// lane reservation, lifetime, and end-to-end renders and camera previews of synthetic DNGs.

#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/application/import.hpp>
#include <lucent/application/preview_service.hpp>

#include "app_test_support.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;
namespace rts = raw::test_support;

// Settings whose revision the scripted renderer can read back.
engine::RenderSettings settings_for(std::uint64_t revision) {
    engine::RenderSettings settings;
    settings.exposure_ev = static_cast<double>(revision);
    return settings;
}

PreviewRequest request_for(ItemId item, std::uint64_t revision, std::uint64_t generation,
                           PreviewQuality quality = PreviewQuality::fit(100), std::uint64_t channel = 1) {
    PreviewRequest r;
    r.item = item;
    r.settings_revision = revision;
    r.settings = settings_for(revision);
    r.quality = quality;
    r.channel = channel;
    r.generation = generation;
    return r;
}

class PreviewServiceTest : public ::testing::Test {
protected:
    void SetUp() override { start(ExecutorOptions{}); }

    void start(ExecutorOptions options) {
        service_.reset();
        executor_.reset();
        auto executor = TaskExecutor::create(options);
        ASSERT_TRUE(executor) << executor.error().describe();
        executor_ = std::move(*executor);
        service_ = std::make_unique<PreviewService>(library_, *executor_, renderer_);
    }

    ItemId add_item(const std::wstring& path = L"C:\\photos\\a.cr3") {
        auto outcomes = library_.add({ts::fake_item(path)});
        EXPECT_TRUE(outcomes);
        return (*outcomes)[0].id;
    }

    PreviewCallback collect() {
        return [this](const PreviewResult& r) { results_.add(r); };
    }

    // Declaration order matters: the service is destroyed first and its destructor still calls
    // back into results_.
    SessionLibrary library_;
    std::shared_ptr<ts::ScriptedRenderer> renderer_ =
        std::make_shared<ts::ScriptedRenderer>(image::Size{400, 300});
    ts::Collector<PreviewResult> results_;
    std::unique_ptr<TaskExecutor> executor_;
    std::unique_ptr<PreviewService> service_;
};

// The result for `generation` (callbacks of different requests may arrive in either order when
// their completion times are not forced by the test).
PreviewResult result_for(const std::vector<PreviewResult>& results, std::uint64_t generation) {
    for (const PreviewResult& r : results) {
        if (r.generation == generation) {
            return r;
        }
    }
    ADD_FAILURE() << "no result for generation " << generation;
    return {};
}

TEST_F(PreviewServiceTest, DeliversAFitPreviewWithHistogramAndCachesIt) {
    const ItemId item = add_item();
    ASSERT_TRUE(service_->request(request_for(item, 1, 1), collect()));
    ASSERT_TRUE(results_.wait_for(1));
    PreviewResult first = results_.values()[0];
    ASSERT_EQ(first.outcome, PreviewOutcome::Delivered) << (first.error ? first.error->describe() : "");
    ASSERT_TRUE(first.preview);
    EXPECT_FALSE(first.from_cache);
    EXPECT_EQ(first.preview->image.width, 100);
    EXPECT_EQ(first.preview->image.height, 75);
    EXPECT_EQ(first.preview->image.rgb.size(), 100u * 75u * 3u);
    EXPECT_EQ(first.preview->histogram.pixels, 7500u);
    EXPECT_EQ(first.preview->summary.demosaic, "scripted");
    EXPECT_EQ(first.preview->summary.rendered_size, (image::Size{400, 300}));
    EXPECT_TRUE(first.preview->report);
    EXPECT_EQ(first.preview->key, (PreviewKey{item, 1, PreviewQuality::fit(100)}));
    // The ramp's ends after 4:1 area averaging in linear light: the first output pixel is the mean
    // of source columns 0..3 (1.5 / 399), encoded with the sRGB curve; the last one is near white.
    const double linear = 1.5 / 399.0;
    const double encoded = 12.92 * linear;  // below the sRGB curve's linear-segment threshold
    EXPECT_NEAR(first.preview->image.rgb[0], std::floor(encoded * 255.0 + 0.5), 1.0);
    EXPECT_EQ(first.preview->image.rgb[99 * 3], 255);

    ASSERT_TRUE(service_->request(request_for(item, 1, 2), collect()));
    ASSERT_TRUE(results_.wait_for(2));
    const PreviewResult cached = results_.values()[1];
    EXPECT_EQ(cached.outcome, PreviewOutcome::Delivered);
    EXPECT_TRUE(cached.from_cache);
    EXPECT_EQ(cached.preview, first.preview);
    EXPECT_EQ(renderer_->renders(), 1);

    ASSERT_TRUE(service_->request(request_for(item, 1, 3, PreviewQuality::full()), collect()));
    ASSERT_TRUE(results_.wait_for(3));
    const PreviewResult full = results_.values()[2];
    ASSERT_EQ(full.outcome, PreviewOutcome::Delivered);
    EXPECT_EQ(full.preview->image.width, 400);
    EXPECT_EQ(full.preview->image.height, 300);
    EXPECT_EQ(service_->cache_stats().entries, 2u);
}

TEST_F(PreviewServiceTest, ANewerGenerationCancelsARunningRender) {
    const ItemId item = add_item();
    auto gate = std::make_shared<ts::Gate>();
    renderer_->script(1, ts::Script{gate, true, std::nullopt});
    ASSERT_TRUE(service_->request(request_for(item, 1, 1), collect()));
    ASSERT_TRUE(renderer_->wait_started(1));
    ASSERT_TRUE(service_->request(request_for(item, 2, 2), collect()));
    ASSERT_TRUE(results_.wait_for(2));
    const auto values = results_.values();
    ASSERT_EQ(values.size(), 2u);
    const PreviewResult stale = result_for(values, 1);
    EXPECT_EQ(stale.outcome, PreviewOutcome::Superseded);
    EXPECT_FALSE(stale.preview);
    ASSERT_TRUE(stale.error);
    EXPECT_EQ(stale.error->code, ErrorCode::Cancelled);
    EXPECT_EQ(result_for(values, 2).outcome, PreviewOutcome::Delivered);
    gate->open();
}

TEST_F(PreviewServiceTest, ALateResultNeverReplacesTheCurrentPreview) {
    // Two renders may run at once, so the newer one finishes first.
    ExecutorOptions options;
    options.admission.max_concurrent = 2;
    start(options);
    const ItemId item = add_item();
    auto gate = std::make_shared<ts::Gate>();
    renderer_->script(1, ts::Script{gate, false, std::nullopt});  // ignores cancellation
    ASSERT_TRUE(service_->request(request_for(item, 1, 1), collect()));
    ASSERT_TRUE(renderer_->wait_started(1));
    ASSERT_TRUE(service_->request(request_for(item, 2, 2), collect()));
    ASSERT_TRUE(results_.wait_for(1));
    EXPECT_EQ(results_.values()[0].generation, 2u);
    EXPECT_EQ(results_.values()[0].outcome, PreviewOutcome::Delivered);

    gate->open();  // the stale render now finishes (its later steps observe the cancellation)
    ASSERT_TRUE(results_.wait_for(2));
    const PreviewResult late = results_.values()[1];
    EXPECT_EQ(late.generation, 1u);
    EXPECT_EQ(late.outcome, PreviewOutcome::Superseded);
    EXPECT_FALSE(late.preview);
}

TEST_F(PreviewServiceTest, ASuccessfulButStaleResultIsSupersededNotDelivered) {
    ExecutorOptions options;
    options.workers = 2;
    options.reserved_interactive = 1;
    options.reserved_quick = 0;
    start(options);
    const ItemId item = add_item();
    // Cache revision 1, so that a later request for it succeeds even after being cancelled.
    ASSERT_TRUE(service_->request(request_for(item, 1, 1), collect()));
    ASSERT_TRUE(results_.wait_for(1));
    ASSERT_EQ(results_.values()[0].outcome, PreviewOutcome::Delivered);

    // Occupy both workers so the next requests stay queued.
    auto hold = std::make_shared<ts::Gate>();
    auto a = executor_->submit(Lane::Interactive, [hold](TaskContext&) { hold->wait({}); });
    auto b = executor_->submit(Lane::Interactive, [hold](TaskContext&) { hold->wait({}); });
    ASSERT_TRUE(a && b);
    ASSERT_TRUE(service_->request(request_for(item, 1, 2), collect()));  // cached, soon stale
    ASSERT_TRUE(service_->request(request_for(item, 2, 3), collect()));  // the current one
    hold->open();
    ASSERT_TRUE(results_.wait_for(3));
    const auto values = results_.values();
    const PreviewResult stale = result_for(values, 2);
    EXPECT_EQ(stale.outcome, PreviewOutcome::Superseded);
    EXPECT_FALSE(stale.preview);
    EXPECT_FALSE(stale.error);  // it succeeded (from the cache) but was no longer the newest
    EXPECT_EQ(result_for(values, 3).outcome, PreviewOutcome::Delivered);
}

TEST_F(PreviewServiceTest, AStaleResultThatFinishesFirstIsStillSuperseded) {
    // One render at a time: the stale render holds the admission until it finishes.
    const ItemId item = add_item();
    auto gate = std::make_shared<ts::Gate>();
    renderer_->script(1, ts::Script{gate, false, std::nullopt});
    ASSERT_TRUE(service_->request(request_for(item, 1, 1), collect()));
    ASSERT_TRUE(renderer_->wait_started(1));
    ASSERT_TRUE(service_->request(request_for(item, 2, 2), collect()));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(results_.size(), 0u);
    gate->open();
    ASSERT_TRUE(results_.wait_for(2));
    const auto values = results_.values();
    EXPECT_EQ(result_for(values, 1).outcome, PreviewOutcome::Superseded);
    EXPECT_FALSE(result_for(values, 1).preview);
    EXPECT_EQ(result_for(values, 2).outcome, PreviewOutcome::Delivered);
}

TEST_F(PreviewServiceTest, ChannelsAreIndependent) {
    const ItemId a = add_item(L"C:\\a.cr3");
    const ItemId b = add_item(L"C:\\b.cr3");
    ASSERT_TRUE(service_->request(request_for(a, 1, 5, PreviewQuality::fit(64), 1), collect()));
    ASSERT_TRUE(service_->request(request_for(b, 1, 1, PreviewQuality::fit(64), 2), collect()));
    ASSERT_TRUE(results_.wait_for(2));
    for (const PreviewResult& r : results_.values()) {
        EXPECT_EQ(r.outcome, PreviewOutcome::Delivered);
    }
}

TEST_F(PreviewServiceTest, RejectsInvalidRequestsSynchronously) {
    const ItemId item = add_item();
    int calls = 0;
    const PreviewCallback counting = [&](const PreviewResult&) { ++calls; };
    EXPECT_EQ(service_->request(request_for(item, 1, 0), counting).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(service_->request(request_for(ItemId{99}, 1, 1), counting).error().code, ErrorCode::NotFound);
    EXPECT_EQ(service_->request(request_for(item, 1, 1), PreviewCallback{}).error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(service_->request(request_for(item, 1, 1, PreviewQuality::fit(4)), counting).error().code,
              ErrorCode::InvalidArgument);
    PreviewRequest bad_settings = request_for(item, 1, 1);
    bad_settings.settings.exposure_ev = 100.0;
    EXPECT_FALSE(service_->request(bad_settings, counting));

    ASSERT_TRUE(service_->request(request_for(item, 1, 3), collect()));
    EXPECT_EQ(service_->request(request_for(item, 1, 3), counting).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(service_->request(request_for(item, 1, 2), counting).error().code, ErrorCode::InvalidArgument);
    ASSERT_TRUE(results_.wait_for(1));
    EXPECT_EQ(calls, 0);
}

TEST_F(PreviewServiceTest, CancelChannelCancelsOutstandingRequests) {
    const ItemId item = add_item();
    auto gate = std::make_shared<ts::Gate>();
    renderer_->script(1, ts::Script{gate, true, std::nullopt});
    ASSERT_TRUE(service_->request(request_for(item, 1, 1), collect()));
    ASSERT_TRUE(renderer_->wait_started(1));
    service_->cancel_channel(1);
    ASSERT_TRUE(results_.wait_for(1));
    EXPECT_EQ(results_.values()[0].outcome, PreviewOutcome::Cancelled);

    ASSERT_TRUE(service_->request(request_for(item, 2, 2), collect()));
    ASSERT_TRUE(results_.wait_for(2));
    EXPECT_EQ(results_.values()[1].outcome, PreviewOutcome::Delivered);
    service_->cancel_channel(12345);  // unknown channel: nothing to do
}

TEST_F(PreviewServiceTest, RenderFailuresAreReported) {
    const ItemId item = add_item();
    renderer_->script(3, ts::Script{nullptr, true, Error{ErrorCode::SourceChanged, "modified", "test", 0}});
    ASSERT_TRUE(service_->request(request_for(item, 3, 1), collect()));
    ASSERT_TRUE(results_.wait_for(1));
    const PreviewResult r = results_.values()[0];
    EXPECT_EQ(r.outcome, PreviewOutcome::Failed);
    ASSERT_TRUE(r.error);
    EXPECT_EQ(r.error->code, ErrorCode::SourceChanged);
    EXPECT_EQ(service_->cache_stats().entries, 0u);
}

TEST_F(PreviewServiceTest, DestructionCancelsAndWaitsForEveryCallback) {
    const ItemId item = add_item();
    auto gate = std::make_shared<ts::Gate>();  // never opened
    renderer_->script(1, ts::Script{gate, true, std::nullopt});
    ASSERT_TRUE(service_->request(request_for(item, 1, 1, PreviewQuality::fit(100), 1), collect()));
    ASSERT_TRUE(service_->request(request_for(item, 1, 1, PreviewQuality::fit(100), 2), collect()));
    ASSERT_TRUE(renderer_->wait_started(1));
    service_.reset();
    EXPECT_EQ(results_.size(), 2u);  // both callbacks returned before the destructor did
    for (const PreviewResult& r : results_.values()) {
        EXPECT_EQ(r.outcome, PreviewOutcome::Cancelled);
    }
}

TEST_F(PreviewServiceTest, RemovingAnItemDropsItsCachedPreviews) {
    const ItemId item = add_item();
    ASSERT_TRUE(service_->request(request_for(item, 1, 1), collect()));
    ASSERT_TRUE(results_.wait_for(1));
    EXPECT_EQ(service_->cache_stats().entries, 1u);
    const std::vector<ItemId> ids = {item};
    ASSERT_TRUE(library_.remove(ids));
    EXPECT_EQ(service_->cache_stats().entries, 0u);

    const ItemId other = add_item(L"C:\\other.cr3");
    ASSERT_TRUE(service_->request(request_for(other, 1, 2), collect()));
    ASSERT_TRUE(results_.wait_for(2));
    service_->invalidate(other);
    EXPECT_EQ(service_->cache_stats().entries, 0u);
}

TEST_F(PreviewServiceTest, RefreshingAChangedItemDropsItsCachedPreviews) {
    const ItemId item = add_item();
    ASSERT_TRUE(service_->request(request_for(item, 1, 1), collect()));
    ASSERT_TRUE(results_.wait_for(1));
    EXPECT_EQ(service_->cache_stats().entries, 1u);

    SourceItem fresh = *library_.find(item);
    fresh.identity.size_bytes += 1;  // the file changed on disk and was imported again
    ASSERT_TRUE(library_.refresh(fresh));
    EXPECT_EQ(service_->cache_stats().entries, 0u);
    ASSERT_TRUE(service_->request(request_for(item, 1, 2), collect()));
    ASSERT_TRUE(results_.wait_for(2));
    EXPECT_FALSE(results_.values()[1].from_cache);
    EXPECT_EQ(renderer_->renders(), 2);
}

TEST_F(PreviewServiceTest, InteractiveCapacityIsReservedFromBackgroundWork) {
    ExecutorOptions options;
    options.workers = 2;
    options.reserved_interactive = 1;
    options.reserved_quick = 0;
    start(options);
    auto hold = std::make_shared<ts::Gate>();
    std::vector<TaskHandle> background;
    for (int i = 0; i < 3; ++i) {
        auto h = executor_->submit(Lane::Background, [hold](TaskContext& c) { hold->wait(c.cancel()); });
        ASSERT_TRUE(h);
        background.push_back(*h);
    }
    const ItemId item = add_item();
    ASSERT_TRUE(service_->request(request_for(item, 1, 1), collect()));
    ASSERT_TRUE(results_.wait_for(1));
    EXPECT_EQ(results_.values()[0].outcome, PreviewOutcome::Delivered);
    hold->open();
    for (const TaskHandle& h : background) {
        h.wait();
    }
}

// ---- End to end with the engine -------------------------------------------------------------

class PreviewEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry_ = ts::default_registry();
        auto renderer = create_engine_renderer(registry_);
        ASSERT_TRUE(renderer);
        auto executor = TaskExecutor::create(ExecutorOptions{});
        ASSERT_TRUE(executor);
        executor_ = std::move(*executor);
        service_ = std::make_unique<PreviewService>(library_, *executor_, *renderer);
    }

    ItemId import(const std::filesystem::path& path) {
        auto report = import_sources(library_, *registry_, ImportRequest{.files = {path}});
        EXPECT_TRUE(report);
        EXPECT_EQ(report->added.size(), 1u);
        return report->added.empty() ? ItemId{} : report->added[0];
    }

    ts::ScratchDir dir_;
    SessionLibrary library_;
    std::shared_ptr<const raw::DecoderRegistry> registry_;
    ts::Collector<PreviewResult> results_;
    std::unique_ptr<TaskExecutor> executor_;
    std::unique_ptr<PreviewService> service_;
};

TEST_F(PreviewEngineTest, RendersASyntheticDngUprightAtFitAndFullQuality) {
    rts::SyntheticDngSpec spec;
    spec.orientation = 6;
    const ItemId item = import(ts::write_dng(dir_, "rotated.dng", spec));
    PreviewRequest fit;
    fit.item = item;
    fit.quality = PreviewQuality::fit(32);
    fit.generation = 1;
    ASSERT_TRUE(service_->request(fit, [&](const PreviewResult& r) { results_.add(r); }));
    PreviewRequest full = fit;
    full.quality = PreviewQuality::full();
    full.channel = 2;
    ASSERT_TRUE(service_->request(full, [&](const PreviewResult& r) { results_.add(r); }));
    ASSERT_TRUE(results_.wait_for(2));
    for (const PreviewResult& r : results_.values()) {
        ASSERT_EQ(r.outcome, PreviewOutcome::Delivered) << (r.error ? r.error->describe() : "");
        const PreviewImage& p = *r.preview;
        EXPECT_EQ(p.summary.orientation, image::Orientation::Rotate90Cw);
        EXPECT_EQ(p.summary.rendered_size, (image::Size{48, 64}));  // 64 x 48 stored, rotated
        EXPECT_EQ(p.summary.interpretation, raw::InterpretationStatus::Complete);
        EXPECT_TRUE(p.summary.provisional);  // Neutral v0
        EXPECT_FALSE(p.summary.demosaic.empty());
        ASSERT_TRUE(p.report);
        EXPECT_EQ(p.report->stages.size(), 8u);
        if (r.quality.kind == PreviewQualityKind::Fit) {
            EXPECT_EQ(p.image.width, 24);
            EXPECT_EQ(p.image.height, 32);
        } else {
            EXPECT_EQ(p.image.width, 48);
            EXPECT_EQ(p.image.height, 64);
        }
        EXPECT_EQ(p.histogram.pixels, static_cast<std::uint64_t>(p.image.width) * p.image.height);
    }
}

TEST_F(PreviewEngineTest, AModifiedSourceFailsWithSourceChanged) {
    const auto path = ts::write_dng(dir_, "changing.dng");
    const ItemId item = import(path);
    rts::SyntheticDngSpec other;
    other.make = "Changed";
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // a distinct last-write time
    dir_.write("changing.dng", rts::build_synthetic_dng(other));
    PreviewRequest request;
    request.item = item;
    request.generation = 1;
    ASSERT_TRUE(service_->request(request, [&](const PreviewResult& r) { results_.add(r); }));
    ASSERT_TRUE(results_.wait_for(1));
    const PreviewResult r = results_.values()[0];
    EXPECT_EQ(r.outcome, PreviewOutcome::Failed);
    ASSERT_TRUE(r.error);
    EXPECT_EQ(r.error->code, ErrorCode::SourceChanged) << r.error->describe();
}

TEST_F(PreviewEngineTest, CameraPreviewsAreExtractedAndCached) {
    rts::SyntheticDngSpec spec;
    spec.jpeg_previews = {rts::SyntheticJpegPreview{rts::jpeg_bytes(rts::kJpegRgb96x64), 96, 64, 3}};
    const ItemId with_preview = import(ts::write_dng(dir_, "with.dng", spec));
    const ItemId without = import(ts::write_dng(dir_, "without.dng"));

    ts::Collector<std::pair<ItemId, Expected<std::shared_ptr<const CameraPreview>>>> camera;
    const CameraPreviewCallback callback =
        [&](ItemId id, const Expected<std::shared_ptr<const CameraPreview>>& r) { camera.add({id, r}); };
    ASSERT_TRUE(service_->request_camera_preview({with_preview}, callback));
    ASSERT_TRUE(camera.wait_for(1));
    ASSERT_TRUE(service_->request_camera_preview({with_preview}, callback));
    ASSERT_TRUE(service_->request_camera_preview({without}, callback));
    ASSERT_TRUE(camera.wait_for(3));
    const auto values = camera.values();
    ASSERT_TRUE(values[0].second) << values[0].second.error().describe();
    const CameraPreview& found = **values[0].second;
    EXPECT_EQ(found.item, with_preview);
    EXPECT_EQ(found.embedded.status, raw::EmbeddedPreviewStatus::Found);
    ASSERT_TRUE(found.embedded.preview);
    EXPECT_EQ(found.embedded.preview->width, 96);
    for (std::size_t i = 1; i < 3; ++i) {
        ASSERT_TRUE(values[i].second);
        if (values[i].first == with_preview) {
            EXPECT_EQ(*values[i].second, *values[0].second);  // the cached object
        } else {
            EXPECT_EQ((*values[i].second)->embedded.status, raw::EmbeddedPreviewStatus::NoneInFile);
        }
    }
    EXPECT_EQ(service_->request_camera_preview({ItemId{77}}, callback).error().code, ErrorCode::NotFound);
    EXPECT_EQ(service_->request_camera_preview({with_preview}, {}).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(service_->request_camera_preview({with_preview, -1}, callback).error().code,
              ErrorCode::InvalidArgument);
}

TEST_F(PreviewEngineTest, CameraPreviewsAreChosenForTheRequestedSizeAndCachedPerSize) {
    rts::SyntheticDngSpec spec;
    spec.jpeg_previews = {rts::SyntheticJpegPreview{rts::jpeg_bytes(rts::kJpegRgb96x64), 96, 64, 3},
                          rts::SyntheticJpegPreview{rts::jpeg_bytes(rts::kJpegRgb48x32), 48, 32, 3}};
    const ItemId item = import(ts::write_dng(dir_, "two.dng", spec));
    ts::Collector<Expected<std::shared_ptr<const CameraPreview>>> camera;
    const CameraPreviewCallback callback = [&](ItemId, const Expected<std::shared_ptr<const CameraPreview>>& r) {
        camera.add(r);
    };
    ASSERT_TRUE(service_->request_camera_preview({item, 40}, callback));  // a small thumbnail
    ASSERT_TRUE(camera.wait_for(1));
    ASSERT_TRUE(service_->request_camera_preview({item, 0}, callback));  // the largest
    ASSERT_TRUE(camera.wait_for(2));
    ASSERT_TRUE(service_->request_camera_preview({item, 40}, callback));
    ASSERT_TRUE(camera.wait_for(3));
    const auto values = camera.values();
    for (const auto& value : values) {
        ASSERT_TRUE(value) << value.error().describe();
        ASSERT_TRUE((*value)->embedded.preview);
    }
    EXPECT_EQ((*values[0])->embedded.preview->width, 48);
    EXPECT_EQ((*values[1])->embedded.preview->width, 96);
    EXPECT_EQ(*values[2], *values[0]);  // cached for that size
}

TEST_F(PreviewEngineTest, ACameraPreviewCancelledBeforeItStartsReadsNothing) {
    rts::SyntheticDngSpec spec;
    spec.jpeg_previews = {rts::SyntheticJpegPreview{rts::jpeg_bytes(rts::kJpegRgb96x64), 96, 64, 3}};
    const ItemId item = import(ts::write_dng(dir_, "with.dng", spec));
    ts::Collector<Expected<std::shared_ptr<const CameraPreview>>> camera;
    const CameraPreviewCallback callback = [&](ItemId, const Expected<std::shared_ptr<const CameraPreview>>& r) {
        camera.add(r);
    };
    core::CancellationSource scrolled_away;
    scrolled_away.cancel();
    ASSERT_TRUE(service_->request_camera_preview({item, 0, scrolled_away.token()}, callback));
    ASSERT_TRUE(camera.wait_for(1));
    ASSERT_FALSE(camera.values()[0]);
    EXPECT_EQ(camera.values()[0].error().code, ErrorCode::Cancelled);
}

TEST_F(PreviewEngineTest, CameraPreviewsAreNotHeldUpByBackgroundWork) {
    // Imports and exports run on the Background lane; exports can wait there for the memory
    // admission. Browsing must not wait for them (blueprint section 4): the Quick lane keeps a
    // thread of its own.
    rts::SyntheticDngSpec spec;
    spec.jpeg_previews = {rts::SyntheticJpegPreview{rts::jpeg_bytes(rts::kJpegRgb96x64), 96, 64, 3}};
    const ItemId item = import(ts::write_dng(dir_, "with.dng", spec));
    const ExecutorOptions& options = executor_->options();
    auto hold = std::make_shared<ts::Gate>();
    std::vector<TaskHandle> background;
    for (std::uint32_t i = 0; i < options.workers + 2; ++i) {
        auto h = executor_->submit(Lane::Background, [hold](TaskContext& c) { hold->wait(c.cancel()); });
        ASSERT_TRUE(h);
        background.push_back(*h);
    }
    const std::uint32_t background_threads = options.workers - options.reserved_interactive - options.reserved_quick;
    const auto deadline = std::chrono::steady_clock::now() + ts::kWaitTimeout;
    while (executor_->stats().running_background < background_threads && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(executor_->stats().running_background, background_threads);

    ts::Collector<Expected<std::shared_ptr<const CameraPreview>>> camera;
    ASSERT_TRUE(service_->request_camera_preview(
        {item, 0}, [&](ItemId, const Expected<std::shared_ptr<const CameraPreview>>& r) { camera.add(r); }));
    ASSERT_TRUE(camera.wait_for(1));
    ASSERT_TRUE(camera.values()[0]) << camera.values()[0].error().describe();
    EXPECT_EQ(executor_->stats().queued_background, options.workers + 2 - background_threads);  // still waiting
    hold->open();
    for (const TaskHandle& h : background) {
        h.wait();
    }
}

}  // namespace
}  // namespace lucent::app
