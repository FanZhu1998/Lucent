// Export naming and collisions, export_item end to end on synthetic DNGs (JPEG and 16-bit TIFF),
// refusal paths, cancellation without leftovers, and the asynchronous ExportService.

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/application/export_service.hpp>
#include <lucent/application/import.hpp>
#include <lucent/application/recipe.hpp>
#include <lucent/application/recipe_service.hpp>
#include <lucent/application/recipe_store.hpp>
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
        r.settings = engine::DevelopSettings{};  // export_item() renders resolved settings
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
    EXPECT_EQ(first->support, SupportStatus::CompatibleUnverified);
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

// W6: exports render the item's current recipe, frozen at submission, and record its identity.
TEST_F(ExportTest, RecordsCarryTheRecipeIdentityAndTheOriginalIsPreserved) {
    const SourceItem item = import(ts::write_dng(dir_, L"in\\edited.dng"));
    const auto source_before = core::hash_file(item.path, {});
    ASSERT_TRUE(source_before);
    RecipeServiceOptions recipe_options;
    recipe_options.registry = registry_;
    RecipeService recipes(library_, create_memory_recipe_store(), recipe_options);
    engine::DevelopSettings settings;
    settings.white_balance = engine::KelvinWhiteBalance{4800.0, 2.0};
    settings.exposure_ev = 0.6;
    settings.shadows = 0.25;
    settings.locks = {engine::DevelopControl::Exposure};
    auto edited = recipes.apply(item.id, settings);
    ASSERT_TRUE(edited) << edited.error().describe();

    auto executor = TaskExecutor::create(ExecutorOptions{});
    ASSERT_TRUE(executor);
    ts::Collector<Expected<ExportRecord>> done;
    {
        ExportService service(library_, **executor, renderer_, &recipes);
        ExportRequest request = request_for(item);
        request.settings.reset();  // the current recipe
        ASSERT_TRUE(
            service.submit(request, {}, [&](ItemId, const Expected<ExportRecord>& r) { done.add(r); }));
        ASSERT_TRUE(done.wait_for(1));
    }
    const Expected<ExportRecord> record = done.values()[0];
    ASSERT_TRUE(record) << record.error().describe();
    EXPECT_EQ(record->settings, settings);
    const RecipeIdentity& recipe = record->recipe;
    EXPECT_EQ(recipe.revision, edited->revision);
    EXPECT_EQ(recipe.revision_number, 1u);
    EXPECT_EQ(recipe.schema, "lucent.develop");
    EXPECT_EQ(recipe.schema_version, kRecipeSchemaVersion);  // the canonical document's version
    EXPECT_EQ(recipe.settings_sha256, edited->committed_sha256);
    EXPECT_EQ(recipe.settings_json, write_recipe(settings)->json);
    EXPECT_EQ(recipe.operations, engine::DevelopOperationVersions{});
    EXPECT_EQ(recipe.base_look, "neutral");
    EXPECT_EQ(recipe.base_look_version, 1u);
    EXPECT_EQ(recipe.pipeline_version, engine::kDevelopPipelineVersion);
    EXPECT_EQ(record->render.look, "neutral");

    // "Original preservation": the source is byte-identical after editing and exporting.
    const auto source_after = core::hash_file(item.path, {});
    ASSERT_TRUE(source_after);
    EXPECT_EQ(source_after->digest, source_before->digest);
    EXPECT_EQ(source_after->identity, source_before->identity);
}

// W9 (ADR 0015): an export of a recipe with masks, geometry and chroma denoising records the
// identity of the whole recipe and renders the cropped and turned image.
TEST_F(ExportTest, ReceiptsCarryTheIdentityOfRecipesWithMasksAndGeometry) {
    const SourceItem item = import(ts::write_dng(dir_, L"in\\masked.dng"));  // 64 x 48
    RecipeServiceOptions recipe_options;
    recipe_options.registry = registry_;
    RecipeService recipes(library_, create_memory_recipe_store(), recipe_options);
    engine::DevelopSettings settings;
    settings.exposure_ev = 0.3;
    engine::LocalAdjustment radial;
    radial.id = 1;
    radial.shape = engine::RadialGradientMask{{0.5, 0.5}, 0.3, 0.2, 0.0, 0.5};
    radial.controls.exposure_ev = 1.0;
    engine::LocalAdjustment brush;
    brush.id = 2;
    brush.shape = engine::BrushMask{{engine::BrushStroke{engine::BrushMode::Add, 0.05, 0.5, 1.0,
                                                         {{0.3, 0.3}, {0.6, 0.4}}}}};
    brush.controls.saturation = -0.5;
    settings.local_adjustments = {radial, brush};
    settings.chroma_denoise = engine::ChromaDenoiseSetting{0.5, 4.0};
    settings.geometry.crop = engine::CropRect{0.25, 0.0, 0.75, 1.0};  // 32 x 48 pixels
    settings.geometry.quarter_turns = 1;                               // 48 x 32
    auto edited = recipes.apply(item.id, settings);
    ASSERT_TRUE(edited) << edited.error().describe();

    auto executor = TaskExecutor::create(ExecutorOptions{});
    ASSERT_TRUE(executor);
    ts::Collector<Expected<ExportRecord>> done;
    {
        ExportService service(library_, **executor, renderer_, &recipes);
        ExportRequest request = request_for(item);
        request.settings.reset();  // the current recipe
        ASSERT_TRUE(
            service.submit(request, {}, [&](ItemId, const Expected<ExportRecord>& r) { done.add(r); }));
        ASSERT_TRUE(done.wait_for(1));
    }
    const Expected<ExportRecord> record = done.values()[0];
    ASSERT_TRUE(record) << record.error().describe();
    EXPECT_EQ(record->settings, settings);
    const RecipeIdentity& recipe = record->recipe;
    EXPECT_EQ(recipe.revision, edited->revision);
    EXPECT_EQ(recipe.schema_version, kRecipeSchemaVersion);
    EXPECT_EQ(recipe.settings_sha256, edited->committed_sha256);
    EXPECT_EQ(recipe.settings_json, write_recipe(settings)->json);
    auto recorded = read_recipe(recipe.settings_json);  // the identity names exactly what was rendered
    ASSERT_TRUE(recorded) << recorded.error().describe();
    EXPECT_EQ(*recorded, settings);
    EXPECT_EQ(recipe.operations, engine::DevelopOperationVersions{});
    // Another mask position is another identity.
    engine::DevelopSettings moved = settings;
    moved.local_adjustments[0].shape = engine::RadialGradientMask{{0.4, 0.5}, 0.3, 0.2, 0.0, 0.5};
    EXPECT_NE(write_recipe(moved)->sha256, recipe.settings_sha256);
    // The export is the cropped, turned image; the summary says so.
    EXPECT_EQ(record->receipt.width, 48);
    EXPECT_EQ(record->receipt.height, 32);
    EXPECT_EQ(record->render.rendered_size, (image::Size{48, 32}));
}

TEST(ExportRecipes, AQueuedExportKeepsItsFrozenRevision) {
    SessionLibrary library;
    auto added = library.add({ts::fake_item(L"C:\\photos\\frozen.cr3")});
    ASSERT_TRUE(added);
    const ItemId item = (*added)[0].id;
    RecipeService recipes(library, create_memory_recipe_store());
    auto first = recipes.apply(item, ts::settings_for(3));
    ASSERT_TRUE(first) << first.error().describe();
    auto renderer = std::make_shared<ts::ScriptedRenderer>(image::Size{40, 30});
    auto gate = std::make_shared<ts::Gate>();
    renderer->script(3, ts::Script{gate});
    auto executor = TaskExecutor::create(ExecutorOptions{});
    ASSERT_TRUE(executor);
    ts::ScratchDir out;
    ts::Collector<Expected<ExportRecord>> done;
    {
        ExportService service(library, **executor, renderer, &recipes);
        ExportRequest request;
        request.item = item;
        request.output_directory = out.path();
        ASSERT_TRUE(
            service.submit(request, {}, [&](ItemId, const Expected<ExportRecord>& r) { done.add(r); }));
        ASSERT_TRUE(renderer->wait_started(1));
        // The photographer edits the photo while its export is running.
        ASSERT_TRUE(recipes.apply(item, ts::settings_for(4)));
        gate->open();
        ASSERT_TRUE(done.wait_for(1));
    }
    const Expected<ExportRecord> record = done.values()[0];
    ASSERT_TRUE(record) << record.error().describe();
    EXPECT_EQ(record->settings, ts::settings_for(3));
    EXPECT_EQ(record->recipe.revision, first->revision);
    EXPECT_EQ(record->recipe.settings_sha256, first->committed_sha256);
}

TEST(ExportRecipes, ARecipeThisBuildCannotReadIsNeverExported) {
    SessionLibrary library;
    auto added = library.add({ts::fake_item(L"C:\\photos\\newer.cr3")});
    ASSERT_TRUE(added);
    const ItemId item = (*added)[0].id;
    auto store = create_memory_recipe_store();
    catalog::NewRevision newer;
    newer.schema_name = "lucent.develop";
    newer.schema_version = kRecipeSchemaVersion + 1;  // written by a newer Lucent
    newer.settings_json = std::format(R"({{"schema":"lucent.develop","version":{}}})", kRecipeSchemaVersion + 1);
    newer.created_at = catalog::current_time();
    ASSERT_TRUE(store->append(item, newer, std::nullopt));
    RecipeService recipes(library, store);
    auto executor = TaskExecutor::create(ExecutorOptions{});
    ASSERT_TRUE(executor);
    ts::ScratchDir out;
    const auto renderer = std::make_shared<ts::ScriptedRenderer>();
    ExportService service(library, **executor, renderer, &recipes);
    ExportRequest request;
    request.item = item;
    request.output_directory = out.path();
    auto refused = service.submit(request, {}, [](ItemId, const Expected<ExportRecord>&) {});
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::VersionMismatch);
    // export_item() itself refuses unresolved settings.
    EXPECT_EQ(
        export_item(*library.find(item), request, *renderer, nullptr, Lane::Background, {}, {}).error().code,
        ErrorCode::InvalidArgument);
    EXPECT_EQ(renderer->started(), 0);
}

TEST(ExportStageNames, AreStable) {
    EXPECT_EQ(to_string(ExportStage::Preparing), "preparing");
    EXPECT_EQ(to_string(ExportStage::Finished), "finished");
    EXPECT_EQ(export_extension(ExportFormat::Jpeg), L".jpg");
    EXPECT_EQ(export_extension(ExportFormat::Tiff), L".tif");
}

}  // namespace
}  // namespace lucent::app
