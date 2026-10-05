// DevelopPreviewService (develop_preview.hpp): latest-wins stage-two renders with stale-frame
// suppression, the refine rule, full resolution on demand, histogram and clipping masks, memory
// admission, lifetime; the W9 session caches (brush rasters, the denoised image) as retained
// memory, frames' geometry and mask overlays, coalesced mask strokes; with a scripted renderer
// (deterministic timing) and with the engine on synthetic DNGs. Also pins the application's export
// render to engine::render_develop_file.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <cmath>
#include <optional>
#include <thread>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/application/develop_preview.hpp>
#include <lucent/application/import.hpp>
#include <lucent/application/recipe.hpp>
#include <lucent/engine/develop.hpp>

#include "app_test_support.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;
namespace rts = raw::test_support;
using ts::settings_for;

DevelopView view_of(const engine::DevelopSettings& settings, bool settled,
                    PreviewQuality quality = PreviewQuality::fit(kDefaultProxyLongEdge)) {
    DevelopView view;
    view.settings = settings;
    view.settled = settled;
    view.quality = quality;
    return view;
}

// Exact equality of two float images (bit patterns).
bool same_pixels(const image::ImageBuffer<float>& a, const image::ImageBuffer<float>& b) {
    if (a.size() != b.size() || a.channels() != b.channels()) {
        return false;
    }
    const std::size_t row_bytes =
        static_cast<std::size_t>(a.width()) * static_cast<std::size_t>(a.channels()) * 4;
    for (std::int32_t y = 0; y < a.height(); ++y) {
        if (std::memcmp(a.row(y), b.row(y), row_bytes) != 0) {
            return false;
        }
    }
    return true;
}

// Waits until `predicate` holds (polling).
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

class DevelopPreviewTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto executor = TaskExecutor::create(ExecutorOptions{});
        ASSERT_TRUE(executor) << executor.error().describe();
        executor_ = std::move(*executor);
        auto added = library_.add({ts::fake_item(L"C:\\photos\\a.cr3")});
        ASSERT_TRUE(added);
        item_ = (*added)[0].id;
        DevelopPreviewOptions options;
        options.proxy_long_edge = 100;
        service_ = std::make_unique<DevelopPreviewService>(library_, *executor_, renderer_, options);
        ASSERT_TRUE(service_->open(item_, [this](const DevelopEvent& event) { events_.add(event); }));
    }

    void TearDown() override {
        service_.reset();
        executor_.reset();
    }

    std::vector<DevelopEvent> frames() const {
        std::vector<DevelopEvent> out;
        for (const DevelopEvent& e : events_.values()) {
            if (e.kind == DevelopEvent::Kind::Frame) {
                out.push_back(e);
            }
        }
        return out;
    }

    SessionLibrary library_;
    std::shared_ptr<ts::ScriptedRenderer> renderer_ =
        std::make_shared<ts::ScriptedRenderer>(image::Size{400, 300});
    std::unique_ptr<TaskExecutor> executor_;
    std::unique_ptr<DevelopPreviewService> service_;
    ItemId item_;
    ts::Collector<DevelopEvent> events_;
};

TEST_F(DevelopPreviewTest, FirstRequestBuildsTheProxyAndDeliversAFinalFrame) {
    auto generation = service_->request(item_, view_of(settings_for(1), true));
    ASSERT_TRUE(generation) << generation.error().describe();
    EXPECT_EQ(*generation, 1u);
    ASSERT_TRUE(events_.wait_for(1));
    const DevelopEvent event = events_.values()[0];
    ASSERT_EQ(event.kind, DevelopEvent::Kind::Frame) << (event.error ? event.error->describe() : "");
    const DevelopFrame& frame = *event.frame;
    EXPECT_EQ(frame.item, item_);
    EXPECT_EQ(frame.generation, 1u);
    EXPECT_EQ(frame.phase, DevelopPhase::Final);  // the proxy's white balance is the settings'
    EXPECT_FALSE(frame.full_resolution);
    EXPECT_EQ(frame.settings_sha256, write_recipe(settings_for(1))->sha256);
    EXPECT_EQ(frame.image.width, 100);
    EXPECT_EQ(frame.image.height, 75);
    EXPECT_EQ(frame.histogram.pixels, 7500u);
    ASSERT_EQ(frame.clipping.flags.size(), 7500u);
    EXPECT_EQ(frame.summary.rendered_size, (image::Size{400, 300}));
    EXPECT_GT(frame.latency_seconds, 0.0);
    ASSERT_TRUE(frame.geometry);  // the identity geometry of the proxy
    EXPECT_EQ(frame.geometry->upright_size(), (image::Size{100, 75}));
    EXPECT_EQ(frame.geometry->output_size(), (image::Size{100, 75}));
    EXPECT_TRUE(frame.masks.empty());

    auto stats = service_->stats(item_);
    ASSERT_TRUE(stats);
    EXPECT_EQ(stats->proxy_size, (image::Size{100, 75}));
    EXPECT_EQ(stats->stage_one_builds, 1u);
    EXPECT_GE(stats->proxy_bytes, 100u * 75u * 3u * sizeof(float));  // rows may be padded
    EXPECT_EQ(stats->retained_bytes, stats->proxy_bytes);            // the kept proxy is retained memory
    EXPECT_EQ(executor_->admission().stats().retained_bytes, stats->proxy_bytes);
    EXPECT_EQ(executor_->admission().stats().active, 0u);  // no render place is held

    // Errors.
    EXPECT_EQ(service_->request(ItemId{999}, view_of(settings_for(1), true)).error().code,
              ErrorCode::NotFound);
    EXPECT_EQ(service_->request(item_, view_of(settings_for(900), true)).error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(service_->open(item_, [](const DevelopEvent&) {}).error().code, ErrorCode::AlreadyExists);
    EXPECT_EQ(service_->open(ItemId{999}, [](const DevelopEvent&) {}).error().code, ErrorCode::NotFound);
}

TEST_F(DevelopPreviewTest, ClippingMaskMarksTheHistogramsEndBins) {
    ASSERT_TRUE(service_->request(item_, view_of(settings_for(1), true)));
    ASSERT_TRUE(events_.wait_for(1));
    const DevelopFrame& frame = *events_.values()[0].frame;
    std::uint64_t highlights = 0;
    std::uint64_t shadows = 0;
    for (std::size_t i = 0; i < frame.clipping.flags.size(); ++i) {
        const std::uint8_t* p = frame.image.rgb.data() + 3 * i;
        const bool high = p[0] == 255 || p[1] == 255 || p[2] == 255;
        const bool low = p[0] == 0 || p[1] == 0 || p[2] == 0;
        EXPECT_EQ((frame.clipping.flags[i] & ClippingMask::kHighlight) != 0, high);
        EXPECT_EQ((frame.clipping.flags[i] & ClippingMask::kShadow) != 0, low);
        highlights += high ? 1 : 0;
        shadows += low ? 1 : 0;
    }
    EXPECT_EQ(frame.clipping.highlight_pixels, highlights);
    EXPECT_EQ(frame.clipping.shadow_pixels, shadows);
    // The ramp runs from black to white: both ends are clipped, one column each.
    EXPECT_EQ(highlights, 75u);
    EXPECT_EQ(shadows, 75u);
    EXPECT_EQ(frame.histogram.red[255], highlights);
}

TEST_F(DevelopPreviewTest, RapidTransientEditsAreCoalescedAndStaleFramesNeverShow) {
    ASSERT_TRUE(service_->request(item_, view_of(settings_for(1), true)));
    ASSERT_TRUE(events_.wait_for(1));
    // The first slider position renders slowly; 27 more arrive meanwhile.
    auto gate = std::make_shared<ts::Gate>();
    renderer_->script(2, ts::Script{gate});
    const int started = renderer_->stage_two_started();
    ASSERT_TRUE(service_->request(item_, view_of(settings_for(2), false)));
    ASSERT_TRUE(eventually([&] { return renderer_->stage_two_started() > started; }));
    std::uint64_t last = 0;
    for (std::uint64_t revision = 3; revision <= 30; ++revision) {
        DevelopView view = view_of(settings_for(revision), false);
        view.transient_generation = revision;
        auto generation = service_->request(item_, view);
        ASSERT_TRUE(generation);
        last = *generation;
    }
    gate->open();
    ASSERT_TRUE(eventually([&] {
        const auto f = frames();
        return !f.empty() && f.back().generation == last;
    }));
    // Settle where the slider stopped.
    auto settled = service_->request(item_, view_of(settings_for(30), true));
    ASSERT_TRUE(settled);
    ASSERT_TRUE(eventually([&] { return frames().back().generation == *settled; }));

    const std::vector<DevelopEvent> delivered = frames();
    for (std::size_t i = 1; i < delivered.size(); ++i) {
        EXPECT_GT(delivered[i].generation, delivered[i - 1].generation) << "a stale frame was delivered";
    }
    // Rendered: the first position, the newest when the renderer was free, the settled request.
    EXPECT_EQ(renderer_->stage_two_revisions(), (std::vector<std::uint64_t>{1, 2, 30, 30}));
    ASSERT_EQ(delivered.size(), 4u);
    EXPECT_EQ(ts::revision_of(delivered[1].frame->report->settings), 2u);  // intermediate: newer than shown
    EXPECT_EQ(delivered[2].frame->transient_generation, 30u);
    EXPECT_EQ(delivered[2].frame->phase, DevelopPhase::Quick);  // unsettled
    EXPECT_EQ(delivered[3].frame->phase, DevelopPhase::Final);
    const auto stats = service_->stats(item_);
    ASSERT_TRUE(stats);
    EXPECT_EQ(stats->requests_coalesced, 27u);  // positions 3 .. 29 were never rendered
    EXPECT_EQ(stats->stage_one_builds, 1u);     // no rebuild for an unchanged white balance
}

TEST_F(DevelopPreviewTest, ALateFullResolutionFrameNeverReplacesANewerOne) {
    ASSERT_TRUE(service_->request(item_, view_of(settings_for(1), true)));
    ASSERT_TRUE(events_.wait_for(1));
    // A settled 100 % request: its quick frame, then the full-resolution build (held), then its
    // full-resolution render, which ignores cancellation in the renderer and finishes late. (Its
    // result is discarded as cancelled, or, had the cancellation come after its last check, dropped
    // as older than the newest delivered frame.)
    auto build_gate = std::make_shared<ts::Gate>();
    renderer_->stage_one_gate(build_gate);
    auto full = service_->request(item_, view_of(settings_for(5), true, PreviewQuality::full()));
    ASSERT_TRUE(full);
    ASSERT_TRUE(eventually([&] { return frames().back().generation == *full; }));
    EXPECT_EQ(frames().back().frame->phase, DevelopPhase::Quick);
    auto render_gate = std::make_shared<ts::Gate>();
    renderer_->script(5, ts::Script{render_gate, false});
    const int started = renderer_->stage_two_started();
    build_gate->open();
    ASSERT_TRUE(eventually([&] { return renderer_->stage_two_started() > started; }));
    // The user moves on while the full-resolution render is still running.
    auto moved = service_->request(item_, view_of(settings_for(6), false));
    ASSERT_TRUE(moved);
    ASSERT_TRUE(eventually([&] { return frames().back().generation == *moved; }));
    render_gate->open();
    ASSERT_TRUE(eventually([&] {
        const auto revisions = renderer_->stage_two_revisions();
        return std::count(revisions.begin(), revisions.end(), 5u) == 2;  // the quick and the full render
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(service_->stats(item_)->full_renders, 1u);
    for (const DevelopEvent& e : frames()) {
        EXPECT_FALSE(e.frame->full_resolution) << "the late full-resolution frame was shown";
    }
    EXPECT_EQ(frames().back().generation, *moved);
}

TEST_F(DevelopPreviewTest, RefineRebuildsStageOneOnlyWhenTheSettledWhiteBalanceMoved) {
    ASSERT_TRUE(service_->request(item_, view_of(settings_for(1), true)));
    ASSERT_TRUE(events_.wait_for(1));
    // Kelvin settings whose stage-two correction (scripted) is far from 1.
    engine::DevelopSettings kelvin = settings_for(7);
    kelvin.white_balance = engine::KelvinWhiteBalance{3200.0, 0.0};
    ts::Script correction;
    correction.correction = engine::RgbMultipliers{1.25, 1.0, 0.8};
    renderer_->script(7, correction);

    // While dragging: quick frames only, no rebuild.
    auto dragging = service_->request(item_, view_of(kelvin, false));
    ASSERT_TRUE(dragging);
    ASSERT_TRUE(eventually([&] { return frames().back().generation == *dragging; }));
    EXPECT_EQ(frames().back().frame->phase, DevelopPhase::Quick);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(service_->stats(item_)->stage_one_builds, 1u);

    // Settled: quick, then the proxy is rebuilt at the new white balance and rendered again.
    auto settled = service_->request(item_, view_of(kelvin, true));
    ASSERT_TRUE(settled);
    ASSERT_TRUE(eventually([&] {
        const auto f = frames();
        return f.back().generation == *settled && f.back().frame->phase == DevelopPhase::Final;
    }));
    std::vector<DevelopEvent> f = frames();
    ASSERT_GE(f.size(), 2u);
    EXPECT_EQ(f[f.size() - 2].generation, *settled);
    EXPECT_EQ(f[f.size() - 2].frame->phase, DevelopPhase::Quick);
    EXPECT_FALSE(f[f.size() - 2].frame->stage_one_rebuilt);
    EXPECT_TRUE(f.back().frame->stage_one_rebuilt);
    EXPECT_EQ(service_->stats(item_)->stage_one_builds, 2u);

    // Within the tolerance (the scripted correction of revision 8 is 1): one final frame, no rebuild.
    engine::DevelopSettings brighter = kelvin;
    brighter.exposure_ev = 0.08;
    auto next = service_->request(item_, view_of(brighter, true));
    ASSERT_TRUE(next);
    ASSERT_TRUE(eventually([&] { return frames().back().generation == *next; }));
    EXPECT_EQ(frames().back().frame->phase, DevelopPhase::Final);
    EXPECT_EQ(service_->stats(item_)->stage_one_builds, 2u);
}

TEST_F(DevelopPreviewTest, FailuresReachTheSinkForTheNewestRequest) {
    ts::Script failing;
    failing.error = Error{ErrorCode::Corrupt, "scripted failure", "test.render", 0};
    renderer_->script(9, failing);
    auto generation = service_->request(item_, view_of(settings_for(9), true));
    ASSERT_TRUE(generation);
    ASSERT_TRUE(eventually([&] { return events_.size() == 1; }));
    const DevelopEvent event = events_.values()[0];
    EXPECT_EQ(event.kind, DevelopEvent::Kind::Failed);
    EXPECT_EQ(event.generation, *generation);
    ASSERT_TRUE(event.error);
    EXPECT_EQ(event.error->code, ErrorCode::Corrupt);
    // The session goes on.
    ASSERT_TRUE(service_->request(item_, view_of(settings_for(1), true)));
    ASSERT_TRUE(events_.wait_for(2));
    EXPECT_EQ(events_.values()[1].kind, DevelopEvent::Kind::Frame);
}

TEST_F(DevelopPreviewTest, CloseCancelsWorkAndReleasesMemory) {
    ASSERT_TRUE(service_->request(item_, view_of(settings_for(1), true)));
    ASSERT_TRUE(events_.wait_for(1));
    auto gate = std::make_shared<ts::Gate>();
    renderer_->script(3, ts::Script{gate});
    const int started = renderer_->stage_two_started();
    ASSERT_TRUE(service_->request(item_, view_of(settings_for(3), true)));
    ASSERT_TRUE(eventually([&] { return renderer_->stage_two_started() > started; }));
    service_->close(item_);  // the gated render is cancelled; close waits for it
    EXPECT_EQ(events_.size(), 1u);
    EXPECT_FALSE(service_->stats(item_));
    EXPECT_EQ(executor_->admission().stats().retained_bytes, 0u);
    EXPECT_EQ(service_->request(item_, view_of(settings_for(1), true)).error().code, ErrorCode::NotFound);
    // It can be opened again.
    ASSERT_TRUE(service_->open(item_, [this](const DevelopEvent& e) { events_.add(e); }));
    ASSERT_TRUE(service_->request(item_, view_of(settings_for(1), true)));
    ASSERT_TRUE(events_.wait_for(2));
    EXPECT_EQ(service_->open_items(), std::vector<ItemId>{item_});
}

// ---- With the engine ---------------------------------------------------------------------------

class DevelopEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto executor = TaskExecutor::create(ExecutorOptions{});
        ASSERT_TRUE(executor);
        executor_ = std::move(*executor);
        registry_ = ts::default_registry();
        auto renderer = create_engine_renderer(registry_);
        ASSERT_TRUE(renderer);
        renderer_ = *renderer;
        rts::SyntheticDngSpec spec;
        spec.width = 256;
        spec.height = 192;
        path_ = ts::write_dng(dir_, L"large.dng", spec);
        auto report = import_sources(library_, *registry_, ImportRequest{.files = {path_}});
        ASSERT_TRUE(report);
        ASSERT_EQ(report->added.size(), 1u);
        item_ = report->added[0];
        DevelopPreviewOptions options;
        options.proxy_long_edge = 64;
        options.on_metadata = [this](ItemId, const raw::RawMetadata&) { ++metadata_calls_; };
        service_ = std::make_unique<DevelopPreviewService>(library_, *executor_, renderer_, options);
        ASSERT_TRUE(service_->open(item_, [this](const DevelopEvent& e) { events_.add(e); }));
    }

    void TearDown() override {
        service_.reset();
        executor_.reset();
    }

    // Waits for the Final frame of `generation`.
    std::shared_ptr<const DevelopFrame> final_frame(std::uint64_t generation) {
        std::shared_ptr<const DevelopFrame> found;
        EXPECT_TRUE(eventually([&] {
            for (const DevelopEvent& e : events_.values()) {
                if (e.kind == DevelopEvent::Kind::Failed && e.generation == generation) {
                    ADD_FAILURE() << e.error->describe();
                    return true;
                }
                if (e.kind == DevelopEvent::Kind::Frame && e.generation == generation &&
                    e.frame->phase == DevelopPhase::Final) {
                    found = e.frame;
                    return true;
                }
            }
            return false;
        }));
        return found;
    }

    std::size_t frames_of(std::uint64_t generation) const {
        std::size_t count = 0;
        for (const DevelopEvent& e : events_.values()) {
            count += e.kind == DevelopEvent::Kind::Frame && e.generation == generation ? 1 : 0;
        }
        return count;
    }

    ts::ScratchDir dir_;
    SessionLibrary library_;
    std::unique_ptr<TaskExecutor> executor_;
    std::shared_ptr<const raw::DecoderRegistry> registry_;
    std::shared_ptr<const SourceRenderer> renderer_;
    std::filesystem::path path_;
    ItemId item_;
    std::atomic<int> metadata_calls_{0};
    std::unique_ptr<DevelopPreviewService> service_;
    ts::Collector<DevelopEvent> events_;
};

TEST_F(DevelopEngineTest, AWhiteBalanceChangeIsQuickThenRefined) {
    auto first = service_->request(item_, view_of(engine::DevelopSettings{}, true));
    ASSERT_TRUE(first);
    auto shown = final_frame(*first);
    ASSERT_TRUE(shown);
    EXPECT_EQ(frames_of(*first), 1u);  // as shot on an as-shot proxy: final at once
    EXPECT_EQ(shown->image.width, 64);
    EXPECT_EQ(shown->image.height, 48);
    EXPECT_EQ(shown->summary.look, "neutral");
    EXPECT_GE(metadata_calls_.load(), 1);

    engine::DevelopSettings warm;
    warm.white_balance = engine::KelvinWhiteBalance{3000.0, 0.0};
    auto settled = service_->request(item_, view_of(warm, true));
    ASSERT_TRUE(settled);
    auto refined = final_frame(*settled);
    ASSERT_TRUE(refined);
    EXPECT_EQ(frames_of(*settled), 2u);  // quick, then refined
    EXPECT_TRUE(refined->stage_one_rebuilt);
    for (const double c : refined->report->correction) {
        EXPECT_NEAR(c, 1.0, 1e-12);  // the rebuilt proxy is balanced for the settings
    }
    EXPECT_EQ(service_->stats(item_)->stage_one_builds, 2u);

    // Same white balance, other exposure: final at once.
    warm.exposure_ev = 0.5;
    auto brighter = service_->request(item_, view_of(warm, true));
    ASSERT_TRUE(brighter);
    ASSERT_TRUE(final_frame(*brighter));
    EXPECT_EQ(frames_of(*brighter), 1u);
    EXPECT_EQ(service_->stats(item_)->stage_one_builds, 2u);
}

TEST_F(DevelopEngineTest, FullResolutionIsBuiltOnDemandAndMatchesTheExportRender) {
    engine::DevelopSettings settings;
    settings.white_balance = engine::KelvinWhiteBalance{4500.0, 4.0};
    settings.exposure_ev = 0.3;
    settings.contrast = 0.2;
    settings.shadows = 0.3;
    auto generation = service_->request(item_, view_of(settings, true, PreviewQuality::full()));
    ASSERT_TRUE(generation);
    auto full = final_frame(*generation);
    ASSERT_TRUE(full);
    EXPECT_TRUE(full->full_resolution);
    EXPECT_EQ(full->image.width, 256);
    EXPECT_EQ(full->image.height, 192);
    auto stats = service_->stats(item_);
    ASSERT_TRUE(stats);
    EXPECT_EQ(stats->full_size, (image::Size{256, 192}));
    EXPECT_GE(stats->full_bytes, 256u * 192u * 3u * sizeof(float));
    EXPECT_EQ(stats->retained_bytes, stats->proxy_bytes + stats->full_bytes);
    EXPECT_EQ(executor_->admission().stats().retained_bytes, stats->retained_bytes);

    // No sensor site of the synthetic file is clipped, so the interactive stage one at the settings'
    // white balance demosaics exactly what the export render does (develop.hpp, "Preview versus
    // export"): the 100 % frame is the export's pixels.
    const SourceItem item = *library_.find(item_);
    auto exported = renderer_->render(item, settings, {}, {});
    ASSERT_TRUE(exported) << exported.error().describe();
    auto encoded = encode_display_srgb8(exported->display);
    ASSERT_TRUE(encoded);
    ASSERT_EQ(encoded->rgb.size(), full->image.rgb.size());
    int worst = 0;
    for (std::size_t i = 0; i < encoded->rgb.size(); ++i) {
        worst = std::max(worst, std::abs(int{encoded->rgb[i]} - int{full->image.rgb[i]}));
    }
    EXPECT_EQ(worst, 0);

    service_->release_full_resolution(item_);
    stats = service_->stats(item_);
    EXPECT_EQ(stats->full_bytes, 0u);
    EXPECT_EQ(stats->retained_bytes, stats->proxy_bytes);
    EXPECT_EQ(executor_->admission().stats().retained_bytes, stats->proxy_bytes);
}

// Settings with chroma denoising and a brush mask (both fill a session cache) and a radial mask.
engine::DevelopSettings cached_settings() {
    engine::DevelopSettings s;
    s.chroma_denoise = engine::ChromaDenoiseSetting{0.6, 8.0};
    engine::LocalAdjustment brush;
    brush.id = 1;
    brush.shape = engine::BrushMask{{engine::BrushStroke{engine::BrushMode::Add, 0.08, 0.5, 1.0,
                                                         {{0.2, 0.3}, {0.5, 0.5}, {0.8, 0.4}}}}};
    brush.controls.exposure_ev = 0.7;
    engine::LocalAdjustment radial;
    radial.id = 2;
    radial.shape = engine::RadialGradientMask{{0.4, 0.6}, 0.2, 0.1, 30.0, 0.5};
    radial.controls.saturation = -0.4;
    s.local_adjustments = {brush, radial};
    return s;
}

TEST_F(DevelopEngineTest, SessionCachesAreReusedAndCountedAsRetainedMemory) {
    const engine::DevelopSettings plain;
    auto first_plain = service_->request(item_, view_of(plain, true));
    ASSERT_TRUE(first_plain);
    auto plain_frame = final_frame(*first_plain);
    ASSERT_TRUE(plain_frame);
    auto stats = service_->stats(item_);
    ASSERT_TRUE(stats);
    EXPECT_EQ(stats->cache_bytes, 0u);  // nothing to keep as shot
    EXPECT_EQ(stats->retained_bytes, stats->proxy_bytes);

    // Denoising and a brush mask: the denoised image and the brush raster are kept.
    engine::DevelopSettings settings = cached_settings();
    auto generation = service_->request(item_, view_of(settings, true));
    ASSERT_TRUE(generation);
    auto first = final_frame(*generation);
    ASSERT_TRUE(first);
    ASSERT_TRUE(first->report->chroma_denoise);
    ASSERT_TRUE(first->report->local_adjustments);
    EXPECT_EQ(first->report->local_adjustments->rasterized, 1u);
    // The settings-aware estimate claims the denoising's working memory.
    EXPECT_GT(first->summary.estimated_peak_bytes, plain_frame->summary.estimated_peak_bytes);
    stats = service_->stats(item_);
    ASSERT_TRUE(stats);
    // The 64 x 48 proxy's denoised image (3 floats per pixel) and the brush raster.
    EXPECT_GE(stats->cache_bytes, 64u * 48u * 3u * sizeof(float));
    EXPECT_EQ(stats->retained_bytes, stats->proxy_bytes + stats->cache_bytes);
    EXPECT_EQ(executor_->admission().stats().retained_bytes, stats->retained_bytes);
    EXPECT_EQ(executor_->admission().stats().active, 0u);

    // A control after the denoising moves: neither is made again, and the pixels are those of a
    // render without caches.
    settings.contrast = 0.3;
    settings.local_adjustments[1].controls.saturation = 0.2;
    auto moved = service_->request(item_, view_of(settings, true));
    ASSERT_TRUE(moved);
    auto reused = final_frame(*moved);
    ASSERT_TRUE(reused);
    EXPECT_EQ(reused->report->local_adjustments->rasterized, 0u);
    EXPECT_EQ(reused->report->local_adjustments->cache_hits, 1u);
    stats = service_->stats(item_);
    EXPECT_GE(stats->mask_cache_hits, 1u);
    EXPECT_GE(stats->stage_two_cache_hits, 1u);
    EXPECT_EQ(stats->retained_bytes, stats->proxy_bytes + stats->cache_bytes);
    {
        DevelopPreviewOptions uncached;
        uncached.proxy_long_edge = 64;
        uncached.mask_cache_bytes = 0;
        uncached.stage_two_cache_bytes = 0;
        DevelopPreviewService other(library_, *executor_, renderer_, uncached);
        ts::Collector<DevelopEvent> events;
        ASSERT_TRUE(other.open(item_, [&](const DevelopEvent& e) { events.add(e); }));
        // Built at the same (as-shot) white balance as the cached session's proxy.
        ASSERT_TRUE(other.request(item_, view_of(settings, true)));
        ASSERT_TRUE(eventually([&] {
            for (const DevelopEvent& e : events.values()) {
                if (e.kind == DevelopEvent::Kind::Frame && e.frame->phase == DevelopPhase::Final) {
                    return true;
                }
            }
            return false;
        }));
        std::shared_ptr<const DevelopFrame> fresh;
        for (const DevelopEvent& e : events.values()) {
            if (e.kind == DevelopEvent::Kind::Frame && e.frame->phase == DevelopPhase::Final) {
                fresh = e.frame;
            }
        }
        ASSERT_TRUE(fresh);
        EXPECT_EQ(fresh->report->local_adjustments->rasterized, 1u);
        EXPECT_EQ(fresh->image.rgb, reused->image.rgb);
        EXPECT_EQ(other.stats(item_)->cache_bytes, 0u);
        other.close(item_);
    }

    // Closing returns everything.
    service_->close(item_);
    EXPECT_EQ(executor_->admission().stats().retained_bytes, 0u);
}

TEST_F(DevelopEngineTest, FramesCarryTheirGeometryAndMaskOutlines) {
    engine::DevelopSettings settings = cached_settings();
    settings.geometry.crop = engine::CropRect{0.25, 0.0, 0.75, 1.0};
    settings.geometry.quarter_turns = 1;
    auto generation = service_->request(item_, view_of(settings, true));
    ASSERT_TRUE(generation);
    auto frame = final_frame(*generation);
    ASSERT_TRUE(frame);
    // The 64 x 48 proxy cropped to 32 x 48 and turned: 48 x 32.
    EXPECT_EQ(frame->image.width, 48);
    EXPECT_EQ(frame->image.height, 32);
    ASSERT_TRUE(frame->geometry);
    EXPECT_EQ(frame->geometry->upright_size(), (image::Size{64, 48}));
    EXPECT_EQ(frame->geometry->output_size(), (image::Size{48, 32}));
    EXPECT_EQ(frame->report->upright_size, (image::Size{64, 48}));
    EXPECT_EQ(frame->report->output_size, (image::Size{48, 32}));
    // The full-resolution size of the developed image: 256 x 192 cropped to 128 x 192, turned.
    EXPECT_EQ(frame->summary.rendered_size, (image::Size{192, 128}));
    // The outlines, in the frame's pixels, in the settings' order.
    ASSERT_EQ(frame->masks.size(), 2u);
    EXPECT_EQ(frame->masks[0].id, 1u);
    EXPECT_EQ(frame->masks[0].kind, engine::MaskKind::Brush);
    ASSERT_EQ(frame->masks[0].polylines.size(), 1u);  // the stroke's centre line
    ASSERT_EQ(frame->masks[0].polylines[0].size(), 3u);
    const engine::GeometryMapping& mapping = *frame->geometry;
    const image::PointD middle = mapping.output_from_normalized(engine::MaskPoint{0.5, 0.5});
    EXPECT_NEAR(frame->masks[0].polylines[0][1].x, middle.x, 1e-9);
    EXPECT_NEAR(frame->masks[0].polylines[0][1].y, middle.y, 1e-9);
    // A pointer position maps back to the brush point it shows.
    const engine::MaskPoint back = mapping.normalized_from_output(frame->masks[0].polylines[0][1]);
    EXPECT_NEAR(back.x, 0.5, 1e-9);
    EXPECT_NEAR(back.y, 0.5, 1e-9);
    EXPECT_EQ(frame->masks[1].kind, engine::MaskKind::RadialGradient);
    EXPECT_EQ(frame->masks[1].polylines.size(), 2u);  // the ellipse and its feather's inner ellipse
    for (const MaskOverlay& overlay : frame->masks) {
        for (const auto& line : overlay.polylines) {
            for (const image::PointD& p : line) {
                EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y));
            }
        }
    }
}

TEST_F(DevelopEngineTest, AMaskStrokesPositionsAreCoalescedLikeASliderGesture) {
    ASSERT_TRUE(service_->request(item_, view_of(engine::DevelopSettings{}, true)));
    ASSERT_TRUE(eventually([&] { return frames_of(1) >= 1; }));
    // 40 pointer positions of one stroke arrive faster than the proxy renders them.
    engine::DevelopSettings stroke;
    engine::LocalAdjustment brush;
    brush.id = 1;
    brush.controls.exposure_ev = 1.0;
    brush.shape = engine::BrushMask{{engine::BrushStroke{}}};
    stroke.local_adjustments = {brush};
    auto& points = std::get<engine::BrushMask>(stroke.local_adjustments[0].shape).strokes[0].points;
    std::uint64_t last = 0;
    for (int step = 0; step < 40; ++step) {
        points.push_back(engine::MaskPoint{0.1 + 0.02 * step, 0.5});
        DevelopView view = view_of(stroke, false);
        view.transient_generation = static_cast<std::uint64_t>(step + 1);
        auto generation = service_->request(item_, view);
        ASSERT_TRUE(generation);
        last = *generation;
    }
    // The end of the stroke settles it.
    auto settled = service_->request(item_, view_of(stroke, true));
    ASSERT_TRUE(settled);
    auto frame = final_frame(*settled);
    ASSERT_TRUE(frame);
    EXPECT_GT(*settled, last);
    const auto stats = service_->stats(item_);
    ASSERT_TRUE(stats);
    EXPECT_EQ(stats->requests, 42u);
    // Every position was either rendered or replaced by a newer one before its render (latest
    // wins: how many depends on the machine's speed), and frames came in order.
    EXPECT_EQ(stats->proxy_renders + stats->requests_coalesced, stats->requests);
    std::uint64_t previous = 0;
    for (const DevelopEvent& e : events_.values()) {
        ASSERT_EQ(e.kind, DevelopEvent::Kind::Frame);
        EXPECT_GE(e.generation, previous);
        previous = e.generation;
    }
}

TEST(EngineRenderer, TheExportRenderIsRenderDevelopFile) {
    ts::ScratchDir dir;
    rts::SyntheticDngSpec spec;
    spec.orientation = 6;
    const std::filesystem::path path = ts::write_dng(dir, L"parity.dng", spec);
    auto registry = ts::default_registry();
    SessionLibrary library;
    auto report = import_sources(library, *registry, ImportRequest{.files = {path}});
    ASSERT_TRUE(report);
    const SourceItem item = *library.find(report->added.at(0));
    auto renderer = create_engine_renderer(registry);
    ASSERT_TRUE(renderer);
    engine::DevelopSettings settings;
    settings.white_balance = engine::KelvinWhiteBalance{5200.0, -6.0};
    settings.exposure_ev = -0.4;
    settings.highlights = -0.5;
    settings.vibrance = 0.4;
    auto app_render = (*renderer)->render(item, settings, {}, {});
    ASSERT_TRUE(app_render) << app_render.error().describe();
    auto engine_render = engine::render_develop_file(*registry, path, 0u, settings);
    ASSERT_TRUE(engine_render) << engine_render.error().describe();
    EXPECT_TRUE(same_pixels(app_render->display.pixels, engine_render->display.pixels));
    EXPECT_EQ(app_render->report.operations.size(), engine_render->report.operations.size());

    // A file that changed since it was imported is refused.
    rts::SyntheticDngSpec other;
    other.width = 80;  // another size: another file version
    ts::write_dng(dir, L"parity.dng", other);
    EXPECT_EQ((*renderer)->render(item, settings, {}, {}).error().code, ErrorCode::SourceChanged);
}

}  // namespace
}  // namespace lucent::app
