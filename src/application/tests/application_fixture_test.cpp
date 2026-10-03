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
#include <string>
#include <vector>

#include <gtest/gtest.h>

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
        auto render = renderer_->render(*library_.find(item), engine::RenderSettings{}, {}, {});
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

TEST_F(ApplicationFixtures, ExportsTheCanonR5AsJpeg) {
    LUCENT_REQUIRE_FIXTURE(path, "canon-eos-r5-cr3-raw");
    auto report = import_sources(library_, *registry_, ImportRequest{.files = {path}});
    ASSERT_TRUE(report);
    const SourceItem item = *library_.find(report->added.at(0));
    ts::ScratchDir out;
    ExportRequest request;
    request.item = item.id;
    request.output_directory = out.path();
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

}  // namespace
}  // namespace lucent::app
