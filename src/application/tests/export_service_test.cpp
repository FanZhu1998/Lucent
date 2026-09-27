// Export naming and collisions, export_item end to end on synthetic DNGs (JPEG and 16-bit TIFF),
// refusal paths, cancellation without leftovers, and the asynchronous ExportService.

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/application/export_service.hpp>
#include <lucent/application/import.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/core/text.hpp>

#include "app_test_support.hpp"
#include "win32_paths.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;
namespace rts = raw::test_support;
using exporter::ExportFormat;

std::wstring name_of(const Expected<std::filesystem::path>& name) {
    EXPECT_TRUE(name) << (name ? "" : name.error().describe());
    return name ? name->wstring() : std::wstring{};
}

TEST(ExportNames, FollowThePattern) {
    EXPECT_EQ(name_of(export_file_name(L"C:\\in\\IMG_0001.CR3", 0, 1, ExportFormat::Jpeg, 1)),
              L"IMG_0001_Lucent.jpg");
    EXPECT_EQ(name_of(export_file_name(L"C:\\in\\IMG_0001.CR3", 0, 1, ExportFormat::Jpeg, 2)),
              L"IMG_0001_Lucent_2.jpg");
    EXPECT_EQ(name_of(export_file_name(L"C:\\in\\IMG_0001.CR3", 0, 1, ExportFormat::Tiff, 17)),
              L"IMG_0001_Lucent_17.tif");
    EXPECT_EQ(name_of(export_file_name(L"C:\\in\\burst.CR3", 2, 5, ExportFormat::Jpeg, 1)),
              L"burst_img3_Lucent.jpg");
    EXPECT_EQ(name_of(export_file_name(L"C:\\in\\photo.v2.dng", 0, 1, ExportFormat::Jpeg, 1)),
              L"photo.v2_Lucent.jpg");
    EXPECT_EQ(name_of(export_file_name(L"C:\\in\\\u5199\u771f.NEF", 0, 1, ExportFormat::Jpeg, 1)),
              L"\u5199\u771f_Lucent.jpg");
    // Reserved device names get a prefix so the result is an ordinary file.
    EXPECT_EQ(name_of(export_file_name(L"\\\\?\\C:\\in\\CON.x.dng", 0, 1, ExportFormat::Jpeg, 1)),
              L"_CON.x_Lucent.jpg");
    EXPECT_EQ(name_of(export_file_name(L"C:\\in\\lpt1.dng", 0, 1, ExportFormat::Jpeg, 1)),
              L"_lpt1_Lucent.jpg");
    EXPECT_EQ(name_of(export_file_name(L"C:\\in\\COM10.dng", 0, 1, ExportFormat::Jpeg, 1)),
              L"COM10_Lucent.jpg");
}

TEST(ExportNames, LongStemsAreTruncatedWithoutSplittingSurrogatePairs) {
    std::wstring stem(260, L'a');
    auto name = export_file_name(L"C:\\in\\" + stem + L".dng", 0, 1, ExportFormat::Jpeg, 123);
    ASSERT_TRUE(name);
    EXPECT_EQ(name->native().size(), 255u);
    EXPECT_TRUE(name->native().ends_with(L"_Lucent_123.jpg"));

    // U+1F4F7 (camera) is a surrogate pair; place one across the cut.
    const std::wstring suffix = L"_Lucent.jpg";
    std::wstring tricky(255 - suffix.size() - 1, L'b');
    tricky += L"\U0001F4F7";
    tricky += L"tail";
    auto cut = export_file_name(L"C:\\in\\" + tricky + L".dng", 0, 1, ExportFormat::Jpeg, 1);
    ASSERT_TRUE(cut);
    EXPECT_LE(cut->native().size(), 255u);
    const std::wstring& text = cut->native();
    const std::size_t before_suffix = text.size() - suffix.size() - 1;
    EXPECT_FALSE(text[before_suffix] >= 0xD800 && text[before_suffix] <= 0xDBFF)
        << "a dangling high surrogate";
    EXPECT_TRUE(core::wide_to_utf8(text));  // well-formed UTF-16
}

TEST(ExportNames, RejectsInvalidArguments) {
    EXPECT_EQ(export_file_name(L"C:\\a.dng", 0, 1, ExportFormat::Jpeg, 0).error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(export_file_name(L"C:\\a.dng", 0, 1, ExportFormat::Jpeg, kMaxExportNameIndex + 1).error().code,
              ErrorCode::LimitExceeded);
    EXPECT_EQ(export_file_name(L"C:\\a.dng", 1, 1, ExportFormat::Jpeg, 1).error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(export_file_name(L"C:\\a.dng", 0, 1, static_cast<ExportFormat>(9), 1).error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(export_file_name(L"C:\\", 0, 1, ExportFormat::Jpeg, 1).error().code,
              ErrorCode::InvalidArgument);
}

TEST(ExportNames, CollisionsAreCaseInsensitive) {
    ts::ScratchDir dir;
    dir.write_text(L"img_0001_lucent.JPG", "taken");
    dir.write_text(L"IMG_0001_LUCENT_2.jpg", "taken");
    dir.write_text(L"IMG_0001_Lucent.tif", "other format: no collision for JPEG");
    auto chosen = choose_export_name(dir.path(), L"C:\\in\\IMG_0001.CR3", 0, 1, ExportFormat::Jpeg);
    ASSERT_TRUE(chosen) << chosen.error().describe();
    EXPECT_EQ(chosen->index, 3u);
    EXPECT_EQ(chosen->path.filename(), std::filesystem::path(L"IMG_0001_Lucent_3.jpg"));
    EXPECT_EQ(chosen->path.parent_path(), dir.path());

    auto first_free = choose_export_name(dir.path(), L"C:\\in\\other.CR3", 0, 1, ExportFormat::Jpeg);
    ASSERT_TRUE(first_free);
    EXPECT_EQ(first_free->index, 1u);
    auto from_five = choose_export_name(dir.path(), L"C:\\in\\other.CR3", 0, 1, ExportFormat::Jpeg, 5);
    ASSERT_TRUE(from_five);
    EXPECT_EQ(from_five->index, 5u);

    EXPECT_EQ(
        choose_export_name(dir.path() / L"missing", L"C:\\a.dng", 0, 1, ExportFormat::Jpeg).error().code,
        ErrorCode::NotFound);
}

class ExportTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry_ = ts::default_registry();
        auto renderer = create_engine_renderer(registry_);
        ASSERT_TRUE(renderer);
        renderer_ = *renderer;
        std::error_code ec;
        std::filesystem::create_directories(out(), ec);
    }

    std::filesystem::path out() const { return dir_.path() / L"out"; }

    SourceItem import(const std::filesystem::path& path) {
        auto report = import_sources(library_, *registry_, ImportRequest{.files = {path}});
        EXPECT_TRUE(report);
        EXPECT_EQ(report->added.size(), 1u);
        return *library_.find(report->added.at(0));
    }

    std::vector<std::wstring> output_names() const {
        std::vector<std::wstring> names;
        for (const auto& entry : std::filesystem::directory_iterator(out())) {
            names.push_back(entry.path().filename().wstring());
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    ExportRequest request_for(const SourceItem& item, ExportFormat format = ExportFormat::Jpeg) const {
        ExportRequest r;
        r.item = item.id;
        r.output_directory = out();
        r.output = format == ExportFormat::Jpeg
                       ? exporter::ExportSettings::jpeg_srgb_q95()
                       : exporter::ExportSettings::tiff16(exporter::OutputProfile::Srgb);
        return r;
    }

    ts::ScratchDir dir_;
    SessionLibrary library_;
    std::shared_ptr<const raw::DecoderRegistry> registry_;
    std::shared_ptr<const SourceRenderer> renderer_;
};

TEST_F(ExportTest, ExportsJpegAndTiffWithUniqueNames) {
    rts::SyntheticDngSpec spec;
    spec.orientation = 8;
    const SourceItem item = import(ts::write_dng(dir_, L"in\\IMG_0042.DNG", spec));
    const auto source_before = core::hash_file(item.path, {});
    ASSERT_TRUE(source_before);

    std::vector<ExportProgress> progress;
    auto first = export_item(item, request_for(item), *renderer_, nullptr, Lane::Background,
                             [&](const ExportProgress& p) { progress.push_back(p); }, {});
    ASSERT_TRUE(first) << first.error().describe();
    EXPECT_EQ(first->receipt.path.filename(), std::filesystem::path(L"IMG_0042_Lucent.jpg"));
    EXPECT_EQ(first->name_index, 1u);
    EXPECT_EQ(first->receipt.width, 48);  // 64 x 48 stored, rotated 90 degrees
    EXPECT_EQ(first->receipt.height, 64);
    EXPECT_EQ(first->receipt.format, ExportFormat::Jpeg);
    EXPECT_EQ(first->item, item.id);
    EXPECT_EQ(first->source_identity, item.identity);
    EXPECT_EQ(first->support, SupportStatus::Supported);
    EXPECT_EQ(first->render.orientation, image::Orientation::Rotate90Ccw);
    EXPECT_GT(first->total_seconds, 0.0);
    ASSERT_FALSE(progress.empty());
    EXPECT_EQ(progress.front().stage, ExportStage::Preparing);
    EXPECT_EQ(progress.back().stage, ExportStage::Finished);
    EXPECT_DOUBLE_EQ(progress.back().fraction, 1.0);
    for (std::size_t i = 1; i < progress.size(); ++i) {
        EXPECT_GE(progress[i].fraction, progress[i - 1].fraction);
        EXPECT_GE(static_cast<int>(progress[i].stage), static_cast<int>(progress[i - 1].stage));
    }
    // The published file is what the receipt describes.
    auto hashed = core::hash_file(first->receipt.path, {});
    ASSERT_TRUE(hashed);
    EXPECT_EQ(hashed->digest, first->receipt.sha256);

    auto second = export_item(item, request_for(item), *renderer_, nullptr, Lane::Background, {}, {});
    ASSERT_TRUE(second) << second.error().describe();
    EXPECT_EQ(second->receipt.path.filename(), std::filesystem::path(L"IMG_0042_Lucent_2.jpg"));
    EXPECT_EQ(second->name_index, 2u);

    auto tiff = export_item(item, request_for(item, ExportFormat::Tiff), *renderer_, nullptr,
                            Lane::Background, {}, {});
    ASSERT_TRUE(tiff) << tiff.error().describe();
    EXPECT_EQ(tiff->receipt.path.filename(), std::filesystem::path(L"IMG_0042_Lucent.tif"));
    EXPECT_EQ(tiff->receipt.bit_depth, exporter::BitDepth::Sixteen);

    EXPECT_EQ(output_names(), (std::vector<std::wstring>{L"IMG_0042_Lucent.jpg", L"IMG_0042_Lucent.tif",
                                                         L"IMG_0042_Lucent_2.jpg"}));
    const auto source_after = core::hash_file(item.path, {});
    ASSERT_TRUE(source_after);
    EXPECT_EQ(source_before->digest, source_after->digest);  // the original is never touched
}

TEST_F(ExportTest, ExportingIntoTheSourceFolderNeverTouchesTheOriginal) {
    const SourceItem item = import(ts::write_dng(dir_, L"out\\IMG_7.dng"));
    auto record = export_item(item, request_for(item), *renderer_, nullptr, Lane::Background, {}, {});
    ASSERT_TRUE(record) << record.error().describe();
    EXPECT_EQ(output_names(), (std::vector<std::wstring>{L"IMG_7.dng", L"IMG_7_Lucent.jpg"}));
}

TEST_F(ExportTest, RefusesUnsupportedItemsUnlessAllowed) {
    rts::SyntheticDngSpec spec;
    spec.opcode_list3 = rts::opcode_list({{9, 0x01030000, 0, rts::filler(76)}});  // mandatory GainMap
    const SourceItem item = import(ts::write_dng(dir_, L"in\\needs_opcode.dng", spec));
    ASSERT_EQ(item.support.status, SupportStatus::Unsupported);

    auto refused = export_item(item, request_for(item), *renderer_, nullptr, Lane::Background, {}, {});
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::Unsupported);
    EXPECT_EQ(refused.error().where, "app.export.support");
    EXPECT_TRUE(output_names().empty());

    ExportRequest allowed = request_for(item);
    allowed.allow_unsupported = true;
    auto exported = export_item(item, allowed, *renderer_, nullptr, Lane::Background, {}, {});
    ASSERT_TRUE(exported) << exported.error().describe();
    EXPECT_EQ(exported->support, SupportStatus::Unsupported);
    EXPECT_EQ(exported->render.interpretation, raw::InterpretationStatus::RequiresUnsupportedOperation);
}

TEST_F(ExportTest, FailurePathsLeaveNothingBehind) {
    const SourceItem item = import(ts::write_dng(dir_, L"in\\a.dng"));

    ExportRequest missing_dir = request_for(item);
    missing_dir.output_directory = dir_.path() / L"no-such-dir";
    EXPECT_EQ(export_item(item, missing_dir, *renderer_, nullptr, Lane::Background, {}, {}).error().code,
              ErrorCode::NotFound);
    ExportRequest no_dir = request_for(item);
    no_dir.output_directory.clear();
    EXPECT_EQ(export_item(item, no_dir, *renderer_, nullptr, Lane::Background, {}, {}).error().code,
              ErrorCode::InvalidArgument);
    ExportRequest bad_quality = request_for(item);
    bad_quality.output.jpeg.quality = 0;
    EXPECT_EQ(export_item(item, bad_quality, *renderer_, nullptr, Lane::Background, {}, {}).error().code,
              ErrorCode::InvalidArgument);

    // Cancelled while rendering: no output and no publication temporary.
    core::CancellationSource cancel;
    auto cancelled = export_item(
        item, request_for(item), *renderer_, nullptr, Lane::Background,
        [&](const ExportProgress& p) {
            if (p.stage == ExportStage::Rendering) {
                cancel.cancel();
            }
        },
        cancel.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::Cancelled);
    EXPECT_TRUE(output_names().empty());

    // A source modified since import is refused.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    rts::SyntheticDngSpec other;
    other.model = "Modified";
    dir_.write(L"in\\a.dng", rts::build_synthetic_dng(other));
    auto changed = export_item(item, request_for(item), *renderer_, nullptr, Lane::Background, {}, {});
    ASSERT_FALSE(changed);
    EXPECT_EQ(changed.error().code, ErrorCode::SourceChanged);
    EXPECT_TRUE(output_names().empty());
}

TEST_F(ExportTest, AdmissionIsHeldOnlyWhileRendering) {
    const SourceItem item = import(ts::write_dng(dir_, L"in\\a.dng"));
    MemoryAdmission admission(AdmissionOptions{1, std::uint64_t{1} << 30});
    std::vector<std::uint32_t> active_during_writing;
    auto record = export_item(item, request_for(item), *renderer_, &admission, Lane::Background,
                              [&](const ExportProgress& p) {
                                  if (p.stage == ExportStage::Rendering || p.stage == ExportStage::Writing) {
                                      active_during_writing.push_back(admission.stats().active);
                                  }
                              },
                              {});
    ASSERT_TRUE(record) << record.error().describe();
    ASSERT_GE(active_during_writing.size(), 2u);
    EXPECT_EQ(active_during_writing.front(), 1u);  // rendering: admitted
    EXPECT_EQ(active_during_writing.back(), 0u);   // writing: released
    EXPECT_EQ(admission.stats().granted, 1u);

    MemoryAdmission tiny(AdmissionOptions{1, 16});
    EXPECT_EQ(export_item(item, request_for(item), *renderer_, &tiny, Lane::Background, {}, {}).error().code,
              ErrorCode::LimitExceeded);
}

TEST_F(ExportTest, TheServiceRunsExportsInTheBackground) {
    const SourceItem item = import(ts::write_dng(dir_, L"in\\async.dng"));
    auto executor = TaskExecutor::create(ExecutorOptions{});
    ASSERT_TRUE(executor);
    ts::Collector<Expected<ExportRecord>> done;
    ts::Collector<ExportProgress> progress;
    {
        ExportService service(library_, **executor, renderer_);
        auto handle = service.submit(
            request_for(item), [&](const ExportProgress& p) { progress.add(p); },
            [&](ItemId id, const Expected<ExportRecord>& r) {
                EXPECT_EQ(id, item.id);
                done.add(r);
            });
        ASSERT_TRUE(handle) << handle.error().describe();
        ASSERT_TRUE(done.wait_for(1));
        ASSERT_TRUE(done.values()[0]) << done.values()[0].error().describe();
        EXPECT_EQ(done.values()[0]->receipt.path.filename(), std::filesystem::path(L"async_Lucent.jpg"));
        EXPECT_EQ(progress.values().back().stage, ExportStage::Finished);

        EXPECT_EQ(
            service
                .submit(ExportRequest{.item = ItemId{999}}, {}, [](ItemId, const Expected<ExportRecord>&) {})
                .error()
                .code,
            ErrorCode::NotFound);
        EXPECT_EQ(service.submit(request_for(item), {}, ExportCompletion{}).error().code,
                  ErrorCode::InvalidArgument);

        // Cancelled through its handle before it can start: the completion still runs, with Cancelled.
        auto hold = std::make_shared<ts::Gate>();
        std::vector<TaskHandle> blockers;
        for (std::uint32_t i = 0;
             i < (*executor)->options().workers - (*executor)->options().reserved_interactive; ++i) {
            auto b =
                (*executor)->submit(Lane::Background, [hold](TaskContext& c) { hold->wait(c.cancel()); });
            ASSERT_TRUE(b);
            blockers.push_back(*b);
        }
        auto cancelled = service.submit(request_for(item), {},
                                        [&](ItemId, const Expected<ExportRecord>& r) { done.add(r); });
        ASSERT_TRUE(cancelled);
        cancelled->cancel();
        hold->open();
        ASSERT_TRUE(done.wait_for(2));
        ASSERT_FALSE(done.values()[1]);
        EXPECT_EQ(done.values()[1].error().code, ErrorCode::Cancelled);
    }  // the destructor waits for outstanding completions
    EXPECT_EQ(output_names(), (std::vector<std::wstring>{L"async_Lucent.jpg"}));
}

TEST(ExportStageNames, AreStable) {
    EXPECT_EQ(to_string(ExportStage::Preparing), "preparing");
    EXPECT_EQ(to_string(ExportStage::Finished), "finished");
    EXPECT_EQ(export_extension(ExportFormat::Jpeg), L".jpg");
    EXPECT_EQ(export_extension(ExportFormat::Tiff), L".tif");
}

}  // namespace
}  // namespace lucent::app
