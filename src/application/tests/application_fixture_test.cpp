// Application services on the downloaded camera fixtures (tools\fetch-fixtures.ps1); every test
// skips when its fixtures are absent. They import the fixture cache, measure preview latency on
// the 45 MP Canon EOS R5 file (the blueprint's "time to first RAW preview") and export it.
// Timings are printed and recorded as test properties; they are measurements, not gates.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

// clang-format off: psapi.h needs the declarations of windows.h.
#include <windows.h>
#include <psapi.h>
// clang-format on

#include <lucent/application/auto_service.hpp>
#include <lucent/application/develop_preview.hpp>
#include <lucent/application/display_image.hpp>
#include <lucent/application/export_service.hpp>
#include <lucent/application/import.hpp>
#include <lucent/application/preview_service.hpp>
#include <lucent/application/source_maintenance.hpp>
#include <lucent/application/thumbnail_cache.hpp>
#include <lucent/export/encode.hpp>

#include "app_test_support.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;
namespace rts = raw::test_support;

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

class ApplicationFixtures : public ::testing::Test {
protected:
    void SetUp() override {
        registry_ = ts::default_registry();
        auto renderer = create_engine_renderer(registry_);
        ASSERT_TRUE(renderer);
        renderer_ = *renderer;
        auto executor = TaskExecutor::create(ExecutorOptions::for_this_machine());
        ASSERT_TRUE(executor);
        executor_ = std::move(*executor);
    }

    SessionLibrary library_;
    std::shared_ptr<const raw::DecoderRegistry> registry_;
    std::shared_ptr<const SourceRenderer> renderer_;
    std::unique_ptr<TaskExecutor> executor_;
};

TEST_F(ApplicationFixtures, ImportsTheFixtureCache) {
    const std::filesystem::path cache = std::filesystem::path(LUCENT_FIXTURE_DIR) / "cache";
    if (!rts::fixture_file("canon-eos-r5-cr3-raw")) {
        GTEST_SKIP() << "fixtures are not downloaded; run tools\\fetch-fixtures.ps1";
    }
    const auto start = std::chrono::steady_clock::now();
    auto report = import_sources(library_, *registry_, ImportRequest{.folders = {{cache, true}}});
    const double seconds = seconds_since(start);
    ASSERT_TRUE(report) << report.error().describe();
    std::printf(
        "  imported %zu items (%llu supported, %llu limited, %llu unsupported) from %llu files in %.2f s\n",
        report->added.size(), static_cast<unsigned long long>(report->supported),
        static_cast<unsigned long long>(report->limited),
        static_cast<unsigned long long>(report->unsupported),
        static_cast<unsigned long long>(report->files_seen), seconds);
    RecordProperty("import_milliseconds", static_cast<int>(seconds * 1000));
    EXPECT_FALSE(report->added.empty());
    EXPECT_EQ(report->supported + report->limited + report->unsupported, report->added.size());
    for (const SourceItem& item : library_.items()) {
        std::printf("  %-8s %-40ls %s\n", std::string(to_string(item.support.status)).c_str(),
                    item.path.filename().c_str(),
                    item.support.reasons.empty() ? "" : item.support.reasons.front().message.c_str());
        if (item.probe) {
            EXPECT_FALSE(item.probe->upright_size.empty());
        }
    }
    // The smoke-tier Canon EOS R5 file is a plain, fully interpretable CR3.
    bool found = false;
    for (const SourceItem& item : library_.items()) {
        if (item.path.filename() == L"Canon_EOS_R5_RAW_ISO_100_nocrop_nodual.CR3") {
            found = true;
            EXPECT_EQ(item.support.status, SupportStatus::Supported);
            ASSERT_TRUE(item.probe);
            EXPECT_EQ(item.probe->camera_make, "Canon");
        }
    }
    EXPECT_TRUE(found);

    // A second import finds only duplicates.
    auto again = import_sources(library_, *registry_, ImportRequest{.folders = {{cache, true}}});
    ASSERT_TRUE(again);
    EXPECT_TRUE(again->added.empty());
}

TEST_F(ApplicationFixtures, CanonR5PreviewLatency) {
    LUCENT_REQUIRE_FIXTURE(path, "canon-eos-r5-cr3-raw");
    auto report = import_sources(library_, *registry_, ImportRequest{.files = {path}});
    ASSERT_TRUE(report);
    ASSERT_EQ(report->added.size(), 1u);
    const ItemId item = report->added[0];
    PreviewService service(library_, *executor_, renderer_);

    const auto run = [&](PreviewQuality quality, std::uint64_t generation) {
        ts::Collector<PreviewResult> results;
        PreviewRequest request;
        request.item = item;
        request.quality = quality;
        request.generation = generation;
        const auto start = std::chrono::steady_clock::now();
        EXPECT_TRUE(service.request(request, [&](const PreviewResult& r) { results.add(r); }));
        EXPECT_TRUE(results.wait_for(1));
        const double seconds = seconds_since(start);
        const PreviewResult r = results.values().at(0);
        EXPECT_EQ(r.outcome, PreviewOutcome::Delivered) << (r.error ? r.error->describe() : "");
        return std::make_pair(r, seconds);
    };

    // Camera JPEG first: what the Library view can show immediately.
    ts::Collector<Expected<std::shared_ptr<const CameraPreview>>> camera;
    const auto camera_start = std::chrono::steady_clock::now();
    ASSERT_TRUE(service.request_camera_preview(
        {item}, [&](ItemId, const Expected<std::shared_ptr<const CameraPreview>>& r) { camera.add(r); }));
    ASSERT_TRUE(camera.wait_for(1));
    const double camera_seconds = seconds_since(camera_start);
    ASSERT_TRUE(camera.values()[0]);

    const auto [fit, fit_seconds] = run(PreviewQuality::fit(2048), 1);
    const auto [cached, cached_seconds] = run(PreviewQuality::fit(2048), 2);
    const auto [full, full_seconds] = run(PreviewQuality::full(), 3);
    ASSERT_TRUE(fit.preview && full.preview && cached.preview);
    EXPECT_TRUE(cached.from_cache);
    const image::Size upright = library_.find(item)->probe->upright_size;  // LibRaw's visible area
    EXPECT_EQ(full.preview->image.width, upright.width);
    EXPECT_EQ(full.preview->image.height, upright.height);
    EXPECT_EQ((image::Size{fit.preview->image.width, fit.preview->image.height}), fit_within(upright, 2048));
    EXPECT_EQ(fit.preview->image.width, 2048);

    // The direct display conversion against Little CMS on a real 45 MP render.
    {
        auto render = renderer_->render(*library_.find(item), engine::DevelopSettings{}, {}, {});
        ASSERT_TRUE(render) << render.error().describe();
        const auto direct_start = std::chrono::steady_clock::now();
        auto direct = encode_display_srgb8(render->display);
        const double direct_seconds = seconds_since(direct_start);
        const auto lcms_start = std::chrono::steady_clock::now();
        auto lcms = exporter::encode_for_output(render->display.pixels.const_view(), render->display.encoding,
                                                exporter::OutputProfile::Srgb, exporter::BitDepth::Eight);
        const double lcms_seconds = seconds_since(lcms_start);
        ASSERT_TRUE(direct && lcms);
        std::size_t differing = 0;
        int worst = 0;
        for (std::size_t i = 0; i < direct->rgb.size(); ++i) {
            const int d = std::abs(int{direct->rgb[i]} - int{lcms->samples8[i]});
            worst = std::max(worst, d);
            differing += d != 0 ? 1 : 0;
        }
        std::printf(
            "  display encoding: direct %.3f s, Little CMS %.3f s; %zu of %zu samples differ, worst %d\n",
            direct_seconds, lcms_seconds, differing, direct->rgb.size(), worst);
        EXPECT_LE(worst, 1);
        EXPECT_LT(static_cast<double>(differing), 0.02 * static_cast<double>(direct->rgb.size()));
    }
    std::printf(
        "  camera JPEG %.3f s | fit-2048 %.3f s (render %.3f s) | cached %.4f s | full %.3f s (render %.3f "
        "s)\n",
        camera_seconds, fit_seconds, fit.preview->summary.render_seconds, cached_seconds, full_seconds,
        full.preview->summary.render_seconds);
    RecordProperty("camera_preview_ms", static_cast<int>(camera_seconds * 1000));
    RecordProperty("fit_2048_ms", static_cast<int>(fit_seconds * 1000));
    RecordProperty("cached_ms", static_cast<int>(cached_seconds * 1000));
    RecordProperty("full_ms", static_cast<int>(full_seconds * 1000));
}

// The editor's "Auto" in process on the 45 MP Canon EOS R5 file (ADR 0013): analysis, the
// full-resolution final check and the revision, timed; the process's peak working set bounds what
// the final check holds at full resolution.
TEST_F(ApplicationFixtures, CanonR5InteractiveAuto) {
    LUCENT_REQUIRE_FIXTURE(path, "canon-eos-r5-cr3-raw");
    auto report = import_sources(library_, *registry_, ImportRequest{.files = {path}});
    ASSERT_TRUE(report);
    ASSERT_EQ(report->added.size(), 1u);
    const ItemId item = report->added[0];
    RecipeServiceOptions recipe_options;
    recipe_options.registry = registry_;
    RecipeService recipes(library_, create_memory_recipe_store(), recipe_options);
    AutoServiceOptions options;
    options.registry = registry_;
    auto service = AutoService::create(library_, recipes, *executor_, options);
    ASSERT_TRUE(service);
    const auto start = std::chrono::steady_clock::now();
    auto ticket = (*service)->run(item);
    ASSERT_TRUE(ticket);
    auto result = (*service)->wait(*ticket, std::chrono::minutes(10));
    const double seconds = seconds_since(start);
    ASSERT_TRUE(result);
    ASSERT_FALSE(result->error) << result->error->describe();
    const AutoRun& run = *result->run;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    ::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                           sizeof(counters));
    std::printf("  Canon EOS R5 Auto: %.2f s (analysis %.2f s, final check %.2f s, %zu attempt(s)); exposure %+.2f EV, "
                "confidence %.2f, %zu flag(s); estimate %.0f MiB, process peak working set %.0f MiB\n",
                seconds, run.analysis_seconds, run.check_seconds, run.attempts.size(), run.chosen.exposure_ev,
                run.confidence, run.flags.size(), static_cast<double>(run.estimated_peak_bytes) / 1048576.0,
                static_cast<double>(counters.PeakWorkingSetSize) / 1048576.0);
    RecordProperty("auto_milliseconds", static_cast<int>(seconds * 1000));
    RecordProperty("auto_check_milliseconds", static_cast<int>(run.check_seconds * 1000));
    EXPECT_EQ(run.width, library_.find(item)->probe->upright_size.width);
    EXPECT_TRUE(result->made_current);
    EXPECT_EQ(recipes.state(item)->revision, result->effective);
    ASSERT_TRUE(run.effective);
    EXPECT_EQ(run.attempts[*run.effective].check.outcome, catalog::AutoCheckOutcome::Passed);
}

TEST_F(ApplicationFixtures, ExportsTheCanonR5AsJpeg) {
    LUCENT_REQUIRE_FIXTURE(path, "canon-eos-r5-cr3-raw");
    auto report = import_sources(library_, *registry_, ImportRequest{.files = {path}});
    ASSERT_TRUE(report);
    const SourceItem item = *library_.find(report->added.at(0));
    ts::ScratchDir out;
    ExportRequest request;
    request.item = item.id;
    request.output_directory = out.path();
    request.settings = engine::DevelopSettings{};
    auto record = export_item(item, request, *renderer_, &executor_->admission(), Lane::Background, {}, {});
    ASSERT_TRUE(record) << record.error().describe();
    EXPECT_EQ(record->receipt.path.filename(),
              std::filesystem::path(L"Canon_EOS_R5_RAW_ISO_100_nocrop_nodual_Lucent.jpg"));
    EXPECT_EQ(record->receipt.width, item.probe->upright_size.width);
    EXPECT_EQ(record->receipt.height, item.probe->upright_size.height);
    std::printf("  export: render %.2f s, encode %.2f s, write+verify %.2f s, total %.2f s, %llu bytes\n",
                record->render_seconds, record->encode_seconds, record->write_seconds, record->total_seconds,
                static_cast<unsigned long long>(record->receipt.byte_size));
    RecordProperty("export_total_ms", static_cast<int>(record->total_seconds * 1000));
}

// Blueprint release gate "Original preservation" (section 10: source hashes unchanged after
// import), for the W4 import paths: the smoke fixtures are imported in place into a catalog,
// hashed (with the camera-thumbnail warm-up), checked, then copied into a library folder (the
// referenced items move to their verified copies) and verified. The originals' SHA-256 and
// identities (size, modification time, file id) must be unchanged, and every copy must hash like
// its original.
TEST(ApplicationFixturesW4, OriginalsAreUnchangedByImportHashingAndCopying) {
    std::vector<std::filesystem::path> originals;
    for (const char* id : {"canon-eos-r5-cr3-raw", "sony-ilce-7m4-arw-compressed", "dji-fc4382-dng",
                           "fujifilm-x-t4-raf-lossless", "nikon-z-6-nef-lossless"}) {
        if (auto file = rts::fixture_file(id)) {
            originals.push_back(*file);
        }
    }
    if (originals.empty()) {
        GTEST_SKIP() << "fixtures are not downloaded; run tools\\fetch-fixtures.ps1";
    }
    struct Snapshot {
        core::Digest256 sha256{};
        core::FileIdentity identity;
    };
    std::vector<Snapshot> before;
    for (const std::filesystem::path& file : originals) {
        before.push_back(Snapshot{ts::file_sha256(file), *core::stat_file(file)});
    }

    ts::ScratchDir dir;
    auto registry = ts::default_registry();
    ts::CatalogFixture fixture = ts::CatalogFixture::open(dir.path() / L"data\\lucent.catalog");
    ASSERT_NE(fixture.library, nullptr);
    const auto start = std::chrono::steady_clock::now();
    auto in_place = import_sources(*fixture.library, *registry, ImportRequest{.files = originals});
    ASSERT_TRUE(in_place) << in_place.error().describe();
    ASSERT_EQ(in_place->added.size(), originals.size());

    auto opened = ThumbnailCache::open(ThumbnailCacheOptions{dir.path() / L"cache\\thumbnails"});
    ASSERT_TRUE(opened);
    HashOptions hashing;
    hashing.thumbnails = std::shared_ptr<ThumbnailCache>(std::move(*opened));
    auto hashed = hash_sources(*fixture.library, {}, hashing);
    ASSERT_TRUE(hashed) << hashed.error().describe();
    EXPECT_EQ(hashed->hashed, originals.size());
    const double hash_seconds = seconds_since(start);
    auto checked = refresh_sources(*fixture.library, *registry, {});
    ASSERT_TRUE(checked);
    EXPECT_EQ(checked->present, originals.size());

    std::filesystem::create_directories(dir.path() / L"library");
    ImportRequest copy;
    copy.files = originals;
    copy.copy_to = dir.path() / L"library";
    auto copied = import_sources(*fixture.library, *registry, copy);
    ASSERT_TRUE(copied) << copied.error().describe();
    EXPECT_EQ(copied->copied.size(), originals.size());
    HashOptions verify;
    verify.verify_hashed = true;
    auto verified = hash_sources(*fixture.library, {}, verify);
    ASSERT_TRUE(verified);
    EXPECT_EQ(verified->verified, originals.size());
    EXPECT_EQ(verified->changed, 0u);

    for (std::size_t i = 0; i < originals.size(); ++i) {
        SCOPED_TRACE(originals[i].string());
        EXPECT_EQ(ts::file_sha256(originals[i]), before[i].sha256);
        EXPECT_EQ(*core::stat_file(originals[i]), before[i].identity);
    }
    for (const SourceItem& item : fixture.library->items()) {
        SCOPED_TRACE(item.path.string());
        EXPECT_EQ(item.import_mode, ImportMode::Copied);
        ASSERT_TRUE(item.sha256);
        EXPECT_EQ(*item.sha256, ts::file_sha256(item.path));
    }
    std::printf("  %zu originals: import + hash %.2f s, %llu thumbnails cached, %llu bytes copied\n",
                originals.size(), hash_seconds, static_cast<unsigned long long>(hashed->thumbnails_cached),
                static_cast<unsigned long long>(copied->bytes_copied));
}

// ---- W6: interactive editing on the 45 MP Canon EOS R5 ----------------------------------------

std::uint64_t working_set_bytes() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    return ::K32GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters))
               ? static_cast<std::uint64_t>(counters.WorkingSetSize)
               : 0;
}

struct Spread {
    double min = 0.0;
    double median = 0.0;
    double max = 0.0;
};

Spread spread(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values.empty() ? Spread{} : Spread{values.front(), values[values.size() / 2], values.back()};
}

// Measurements (printed and recorded as test properties, not gates): opening an edit session
// (decode, demosaic, 2048-pixel proxy, first frame), transient-edit latency from request() to the
// delivered frame, a 60 Hz slider drag, the refine after a white-balance change, the 100 % view,
// and the memory an open item holds. Also checks the contract on the real file: quick frames for
// transient edits, a rebuilt stage one after a settled white-balance change, and that the source
// is unchanged.
TEST_F(ApplicationFixtures, CanonR5InteractiveEditing) {
    LUCENT_REQUIRE_FIXTURE(path, "canon-eos-r5-cr3-raw");
    const core::Digest256 source_before = ts::file_sha256(path);
    auto report = import_sources(library_, *registry_, ImportRequest{.files = {path}});
    ASSERT_TRUE(report);
    const ItemId item = report->added.at(0);
    ts::Collector<DevelopEvent> events;
    DevelopPreviewService service(library_, *executor_, renderer_);
    ASSERT_TRUE(service.open(item, [&](const DevelopEvent& e) { events.add(e); }));
    const std::uint64_t working_set_before = working_set_bytes();

    // Waits for the frame of `generation` in `phase` (or any phase) and returns it.
    const auto frame_of = [&](std::uint64_t generation, std::optional<DevelopPhase> phase) {
        std::shared_ptr<const DevelopFrame> found;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!found && std::chrono::steady_clock::now() < deadline) {
            for (const DevelopEvent& e : events.values()) {
                if (e.kind == DevelopEvent::Kind::Failed && e.generation == generation) {
                    ADD_FAILURE() << e.error->describe();
                    return found;
                }
                if (e.kind == DevelopEvent::Kind::Frame && e.generation == generation &&
                    (!phase || e.frame->phase == *phase)) {
                    found = e.frame;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_TRUE(found) << "no frame for generation " << generation;
        return found;
    };
    const auto view = [](const engine::DevelopSettings& settings, bool settled,
                         PreviewQuality quality = PreviewQuality::fit(kDefaultProxyLongEdge)) {
        DevelopView v;
        v.settings = settings;
        v.settled = settled;
        v.quality = quality;
        return v;
    };

    // Open: stage one (decode .. proxy) and the first frame.
    const auto open_start = std::chrono::steady_clock::now();
    auto first = service.request(item, view(engine::DevelopSettings{}, true));
    ASSERT_TRUE(first);
    auto opened = frame_of(*first, DevelopPhase::Final);
    ASSERT_TRUE(opened);
    const double open_seconds = seconds_since(open_start);
    EXPECT_EQ(opened->image.width, 2048);
    const DevelopSessionStats after_open = *service.stats(item);
    const std::uint64_t working_set_open = working_set_bytes();

    // Transient edits, one at a time (each waits for its frame): request -> pixels.
    std::vector<double> latency;
    std::vector<double> stage_two;
    std::vector<double> display;
    engine::DevelopSettings edit;
    for (int step = 1; step <= 15; ++step) {
        edit.exposure_ev = 0.05 * step;
        edit.shadows = 0.02 * step;
        edit.contrast = 0.01 * step;
        auto generation = service.request(item, view(edit, false));
        ASSERT_TRUE(generation);
        auto frame = frame_of(*generation, std::nullopt);
        ASSERT_TRUE(frame);
        EXPECT_EQ(frame->phase, DevelopPhase::Quick);
        latency.push_back(frame->latency_seconds);
        stage_two.push_back(frame->stage_two_seconds);
        display.push_back(frame->display_seconds);
    }
    const Spread transient = spread(latency);
    const Spread stage_two_spread = spread(stage_two);
    const Spread display_spread = spread(display);

    // A 60 Hz slider drag: 60 positions 16 ms apart; frames follow the newest position.
    const std::size_t frames_before_drag = events.size();
    const auto drag_start = std::chrono::steady_clock::now();
    std::uint64_t last = 0;
    for (int step = 0; step < 60; ++step) {
        edit.exposure_ev = 0.75 + 0.01 * step;
        auto generation = service.request(item, view(edit, false));
        ASSERT_TRUE(generation);
        last = *generation;
        std::this_thread::sleep_until(drag_start + std::chrono::milliseconds(16 * (step + 1)));
    }
    auto last_frame = frame_of(last, std::nullopt);
    ASSERT_TRUE(last_frame);
    const double drag_seconds = seconds_since(drag_start);
    const std::size_t drag_frames = events.size() - frames_before_drag;
    const double final_after_release = last_frame->latency_seconds;

    // Refine: a settled white balance far from as shot.
    engine::DevelopSettings warm = edit;
    warm.white_balance = engine::KelvinWhiteBalance{3200.0, 6.0};
    const auto refine_start = std::chrono::steady_clock::now();
    auto settled = service.request(item, view(warm, true));
    ASSERT_TRUE(settled);
    auto quick = frame_of(*settled, DevelopPhase::Quick);
    const double refine_quick_seconds = seconds_since(refine_start);
    auto refined = frame_of(*settled, DevelopPhase::Final);
    const double refine_seconds = seconds_since(refine_start);
    ASSERT_TRUE(quick && refined);
    EXPECT_TRUE(refined->stage_one_rebuilt);
    EXPECT_EQ(service.stats(item)->stage_one_builds, 2u);

    // 100 %: the full-resolution stage one at the settled white balance, then stage two.
    const auto full_start = std::chrono::steady_clock::now();
    auto full_generation = service.request(item, view(warm, true, PreviewQuality::full()));
    ASSERT_TRUE(full_generation);
    auto full = frame_of(*full_generation, DevelopPhase::Final);
    const double full_seconds = seconds_since(full_start);
    ASSERT_TRUE(full);
    EXPECT_TRUE(full->full_resolution);
    const DevelopSessionStats at_full = *service.stats(item);
    const std::uint64_t working_set_full = working_set_bytes();
    // An exposure change at 100 %: quick on the proxy, then full-resolution stage two only.
    warm.exposure_ev += 0.2;
    const auto full_edit_start = std::chrono::steady_clock::now();
    auto full_edit = service.request(item, view(warm, true, PreviewQuality::full()));
    ASSERT_TRUE(full_edit);
    auto full_quick = frame_of(*full_edit, DevelopPhase::Quick);
    const double full_quick_seconds = seconds_since(full_edit_start);
    auto full_final = frame_of(*full_edit, DevelopPhase::Final);
    const double full_edit_seconds = seconds_since(full_edit_start);
    ASSERT_TRUE(full_quick && full_final);
    EXPECT_EQ(service.stats(item)->stage_one_builds, at_full.stage_one_builds);  // no new build

    service.release_full_resolution(item);
    const DevelopSessionStats released = *service.stats(item);
    service.close(item);
    EXPECT_EQ(executor_->admission().stats().retained_bytes, 0u);
    EXPECT_EQ(ts::file_sha256(path), source_before);  // original preservation

    const auto mb = [](std::uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); };
    std::printf(
        "  open (stage one + proxy + first frame) %.3f s\n"
        "  transient edit, request -> pixels: min %.1f / median %.1f / max %.1f ms (stage two %.1f / %.1f / "
        "%.1f; display %.1f "
        "ms)\n"
        "  60 Hz drag over %.2f s: %zu frames; last position shown %.1f ms after its request\n"
        "  refine (3200 K): quick frame %.3f s, refined %.3f s\n"
        "  100 %%: %.3f s; exposure change at 100 %%: quick %.3f s, full %.3f s\n"
        "  memory: proxy %.1f MiB (retained %.1f MiB), full resolution %.1f MiB (retained %.1f MiB), after "
        "release %.1f MiB; working set +%.0f MiB open, +%.0f MiB at 100 %%\n",
        open_seconds, transient.min * 1000, transient.median * 1000, transient.max * 1000,
        stage_two_spread.min * 1000, stage_two_spread.median * 1000, stage_two_spread.max * 1000,
        display_spread.median * 1000, drag_seconds, drag_frames, final_after_release * 1000,
        refine_quick_seconds, refine_seconds, full_seconds, full_quick_seconds, full_edit_seconds,
        mb(after_open.proxy_bytes), mb(after_open.retained_bytes), mb(at_full.full_bytes),
        mb(at_full.retained_bytes), mb(released.retained_bytes),
        mb(working_set_open > working_set_before ? working_set_open - working_set_before : 0),
        mb(working_set_full > working_set_before ? working_set_full - working_set_before : 0));
    RecordProperty("open_ms", static_cast<int>(open_seconds * 1000));
    RecordProperty("transient_median_ms", static_cast<int>(transient.median * 1000));
    RecordProperty("transient_max_ms", static_cast<int>(transient.max * 1000));
    RecordProperty("refine_ms", static_cast<int>(refine_seconds * 1000));
    RecordProperty("full_ms", static_cast<int>(full_seconds * 1000));
    RecordProperty("proxy_mib", static_cast<int>(mb(after_open.proxy_bytes)));
    RecordProperty("full_mib", static_cast<int>(mb(at_full.full_bytes)));
}

}  // namespace
}  // namespace lucent::app
