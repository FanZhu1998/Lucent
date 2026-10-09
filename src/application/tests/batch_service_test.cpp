// BatchService with real lucent-worker processes over synthetic DNGs (W7, ADR 0012): end to end,
// naming and collisions, existing outputs, metadata, single exports through batches, failures,
// pause / cancel / resume and destination problems.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <format>
#include <mutex>
#include <optional>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include <lucent/application/batch_service.hpp>
#include <lucent/application/export_service.hpp>
#include <lucent/engine/develop_geometry.hpp>
#include <lucent/export/metadata.hpp>

#include "batch_test_support.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;
namespace rts = raw::test_support;
using namespace std::chrono_literals;

bool contains(const std::vector<std::wstring>& names, const std::wstring& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

std::size_t count_with_extension(const std::filesystem::path& folder, const std::wstring& extension) {
    std::size_t n = 0;
    for (const std::wstring& name : ts::folder_entries(folder)) {
        if (std::filesystem::path(name).extension() == extension) {
            ++n;
        }
    }
    return n;
}

class BatchServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        harness_ = std::make_unique<ts::BatchHarness>(dir_.path() / L"lucent");
        out_ = dir_.path() / L"out";
    }

    ts::BatchHarness& h() { return *harness_; }

    ts::ScratchDir dir_;
    std::unique_ptr<ts::BatchHarness> harness_;
    std::filesystem::path out_;
};

TEST_F(BatchServiceTest, ExportsABatchEndToEndAndWritesItsReport) {
    const auto files = ts::write_dngs(dir_, L"in", 3);
    std::vector<core::Digest256> before;
    for (const auto& file : files) {
        before.push_back(ts::file_sha256(file));
    }
    const std::vector<ItemId> ids = h().import(files);
    const catalog::BatchId id = h().run(h().request(ids, out_));
    ASSERT_TRUE(id.valid());

    const catalog::BatchRecord record = h().record(id);
    EXPECT_EQ(record.state, catalog::BatchState::Completed);
    EXPECT_EQ(record.counts.complete, 3u);
    const std::vector<std::wstring> names = ts::folder_entries(out_);
    for (std::size_t i = 0; i < files.size(); ++i) {
        EXPECT_TRUE(contains(names, L"photo" + std::to_wstring(i) + L"_Lucent_0.jpg"));
    }
    EXPECT_TRUE(contains(names, std::format(L"Lucent-batch-{}-report.json", id.value)));
    EXPECT_TRUE(contains(names, std::format(L"Lucent-batch-{}-report.html", id.value)));
    EXPECT_TRUE(ts::leftovers(out_).empty());

    // Every receipt matches its file; every job has exactly one receipt.
    const auto receipts = h().receipts(id);
    ASSERT_EQ(receipts.size(), 3u);
    for (const catalog::ReceiptRecord& receipt : receipts) {
        auto hashed = core::hash_file(receipt.final_path, {});
        ASSERT_TRUE(hashed);
        EXPECT_EQ(hashed->digest, receipt.output_sha256);
        EXPECT_EQ(hashed->identity.size_bytes, receipt.size_bytes);
        auto details = read_receipt_details(receipt.settings_json);
        ASSERT_TRUE(details) << details.error().describe();
        EXPECT_EQ(details->format, exporter::ExportFormat::Jpeg);
        EXPECT_GT(details->width, 0u);
    }
    // The journal of each job ends with ReceiptCommitted.
    for (const catalog::JobRecord& job : h().jobs(id)) {
        auto journal = h().catalog().query([&](catalog::ReadTransaction& tx) { return tx.publication_journal(job.id); });
        ASSERT_TRUE(journal);
        ASSERT_FALSE(journal->empty());
        EXPECT_EQ(journal->back().step, catalog::PublicationStep::ReceiptCommitted);
    }

    // The report: API and files agree.
    auto report = h().batches().report(id);
    ASSERT_TRUE(report) << report.error().describe();
    EXPECT_EQ(report->exported, 3u);
    EXPECT_EQ(report->failed, 0u);
    std::ifstream json_file(out_ / std::format(L"Lucent-batch-{}-report.json", id.value));
    const nlohmann::json json = nlohmann::json::parse(json_file);
    EXPECT_EQ(json["schema"], "lucent.batch-report");
    EXPECT_EQ(json["summary"]["exported"], 3);
    EXPECT_EQ(json["entries"].size(), 3u);

    // Originals are untouched (blueprint release gate "Original preservation").
    for (std::size_t i = 0; i < files.size(); ++i) {
        EXPECT_EQ(ts::file_sha256(files[i]), before[i]);
    }
    auto folder = h().batches().output_folder(id);
    ASSERT_TRUE(folder);
    EXPECT_TRUE(std::filesystem::equivalent(*folder, out_));
    // Events: created, started, finished, report.
    ASSERT_TRUE(h().log.wait_for_kind(id, BatchEventKind::ReportWritten));
    EXPECT_EQ(h().log.count(id, BatchEventKind::Created), 1u);
    EXPECT_EQ(h().log.count(id, BatchEventKind::JobStarted), 3u);
    EXPECT_EQ(h().log.count(id, BatchEventKind::Finished), 1u);
    EXPECT_EQ(h().log.count(id, BatchEventKind::ReportWritten), 1u);
}


TEST_F(BatchServiceTest, NamesCollideCaseInsensitivelyAndInsideTheBatch) {
    const auto first = ts::write_dngs(dir_, L"in\\a", 1, L"same");
    const auto second = ts::write_dngs(dir_, L"in\\b", 1, L"same", 1);
    // Rename the second so that both sources have the stem "same0".
    const auto second_path = dir_.path() / L"in\\b\\same0.dng";
    std::filesystem::rename(second[0], second_path);
    const auto third = ts::write_dngs(dir_, L"in", 1, L"photo");
    std::filesystem::create_directories(out_);
    dir_.write_text(L"out\\PHOTO0_LUCENT_0.JPG", "a file of the user");  // another case
    const std::vector<ItemId> ids = h().import({first[0], second_path, third[0]});
    BatchRequest request = h().request(ids, out_);

    auto plan = h().batches().plan(request);
    ASSERT_TRUE(plan) << plan.error().describe();
    EXPECT_EQ(plan->outputs, 3u);
    EXPECT_EQ(plan->renamed, 2u);
    ASSERT_EQ(plan->examples.size(), 3u);
    EXPECT_EQ(plan->examples[0].destination.filename(), L"same0_Lucent_0.jpg");
    EXPECT_EQ(plan->examples[1].destination.filename(), L"same0_Lucent_0_2.jpg");
    EXPECT_TRUE(plan->examples[1].renamed);
    EXPECT_EQ(plan->examples[2].destination.filename(), L"photo0_Lucent_0_2.jpg");

    const catalog::BatchId id = h().run(request);
    const std::vector<std::wstring> names = ts::folder_entries(out_);
    EXPECT_TRUE(contains(names, L"same0_Lucent_0.jpg"));
    EXPECT_TRUE(contains(names, L"same0_Lucent_0_2.jpg"));
    EXPECT_TRUE(contains(names, L"photo0_Lucent_0_2.jpg"));
    EXPECT_TRUE(contains(names, L"PHOTO0_LUCENT_0.JPG"));
    auto report = h().batches().report(id);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->exported, 3u);
    EXPECT_EQ(report->review_recommended, 2u) << "renamed outputs recommend a look";
}

TEST_F(BatchServiceTest, FolderBatchesMirrorSubfolders) {
    ts::write_dngs(dir_, L"in\\2026\\Iceland", 2, L"ice");
    ts::write_dngs(dir_, L"in\\2026\\Norway\\Day 1", 1, L"fjord", 2);
    BatchRequest request;
    request.folder = BatchFolder{dir_.path() / L"in\\2026", true};
    request.destination.folder = out_;
    request.destination.mirror_subfolders = true;
    request.output = exporter::ExportSettings::tiff16(exporter::OutputProfile::DisplayP3);
    auto plan = h().batches().plan(request);
    ASSERT_FALSE(plan) << "a plan never imports, and nothing in the folder is in the library yet";
    const catalog::BatchId id = h().run(request);
    ASSERT_TRUE(id.valid());
    EXPECT_EQ(h().record(id).counts.complete, 3u);
    EXPECT_TRUE(std::filesystem::exists(out_ / L"Iceland" / L"ice0_Lucent_0.tif"));
    EXPECT_TRUE(std::filesystem::exists(out_ / L"Iceland" / L"ice1_Lucent_0.tif"));
    EXPECT_TRUE(std::filesystem::exists(out_ / L"Norway" / L"Day 1" / L"fjord2_Lucent_0.tif"));
    for (const catalog::ReceiptRecord& receipt : h().receipts(id)) {
        auto details = read_receipt_details(receipt.settings_json);
        ASSERT_TRUE(details);
        EXPECT_EQ(details->bit_depth, exporter::BitDepth::Sixteen);
        EXPECT_EQ(details->profile, exporter::OutputProfile::DisplayP3);
    }
    // Planned again now that the folder is in the library: everything is already exported.
    auto again = h().batches().plan(request);
    ASSERT_TRUE(again) << again.error().describe();
    EXPECT_EQ(again->already_exported, 3u);
}

TEST_F(BatchServiceTest, OverwriteReplacesFilesButNeverAnOriginal) {
    const auto files = ts::write_dngs(dir_, L"in", 2);
    std::filesystem::create_directories(out_);
    const std::string earlier = "an earlier export";
    const auto existing = dir_.write_text(L"out\\photo0_Lucent_0.tif", earlier);
    // An original photograph (DNG content under a .tif name) where the second output would go.
    rts::SyntheticDngSpec spec;
    spec.width = 90;
    const auto original = ts::write_dng(dir_, L"out\\photo1_Lucent_0.tif", spec);
    const core::Digest256 original_sha = ts::file_sha256(original);
    std::vector<ItemId> ids = h().import(files);
    ASSERT_EQ(h().import({original}).size(), 1u);
    BatchRequest request = h().request(ids, out_);
    request.output = exporter::ExportSettings::tiff16(exporter::OutputProfile::Srgb);
    request.destination.collisions = CollisionPolicy::Overwrite;
    auto plan = h().batches().plan(request);
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan->replaced, 1u);
    EXPECT_EQ(plan->renamed, 1u);
    const catalog::BatchId id = h().run(request);
    EXPECT_EQ(h().record(id).counts.complete, 2u);
    EXPECT_NE(ts::file_sha256(existing), *core::sha256(std::as_bytes(std::span(earlier.data(), earlier.size()))));
    EXPECT_EQ(ts::file_sha256(original), original_sha) << "an original is never replaced";
    EXPECT_TRUE(std::filesystem::exists(out_ / L"photo1_Lucent_0_2.tif"));
    EXPECT_TRUE(ts::leftovers(out_).empty());
    bool replaced = false;
    bool protected_original = false;
    for (const catalog::ReceiptRecord& receipt : h().receipts(id)) {
        auto details = read_receipt_details(receipt.settings_json);
        ASSERT_TRUE(details);
        replaced = replaced || details->replaced_existing;
        for (const std::string& note : details->review) {
            protected_original = protected_original || note.find("original") != std::string::npos;
        }
    }
    EXPECT_TRUE(replaced);
    EXPECT_TRUE(protected_original);
}

TEST_F(BatchServiceTest, IdenticalOutputsAreSkippedOrExportedAgain) {
    const auto files = ts::write_dngs(dir_, L"in", 2);
    const std::vector<ItemId> ids = h().import(files);
    const catalog::BatchId first = h().run(h().request(ids, out_));
    EXPECT_EQ(h().record(first).counts.complete, 2u);

    auto plan = h().batches().plan(h().request(ids, out_));
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan->already_exported, 2u);
    EXPECT_EQ(plan->outputs, 0u);
    const catalog::BatchId second = h().run(h().request(ids, out_));
    EXPECT_EQ(h().record(second).counts.skipped, 2u);
    EXPECT_EQ(count_with_extension(out_, L".jpg"), 2u);
    auto report = h().batches().report(second);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->skipped, 2u);
    ASSERT_FALSE(report->entries.empty());
    EXPECT_TRUE(report->entries[0].output.has_value()) << "a skipped duplicate points at the existing output";

    BatchRequest again = h().request(ids, out_);
    again.existing_outputs = ExistingOutputs::ExportAgain;
    const catalog::BatchId third = h().run(again);
    EXPECT_EQ(h().record(third).counts.complete, 2u);
    EXPECT_EQ(count_with_extension(out_, L".jpg"), 4u);
    EXPECT_TRUE(std::filesystem::exists(out_ / L"photo0_Lucent_0_2.jpg"));
}

TEST_F(BatchServiceTest, WritesTheRightsTextAndFollowsTheMetadataPolicy) {
    const auto files = ts::write_dngs(dir_, L"in", 1);
    const std::vector<ItemId> ids = h().import(files);
    BatchRequest request = h().request(ids, out_);
    request.artist = "Ann Example";
    request.copyright = "\xC2\xA9 2026 Ann Example";
    h().run(request);
    auto metadata = exporter::read_export_metadata(out_ / L"photo0_Lucent_0.jpg");
    ASSERT_TRUE(metadata) << metadata.error().describe();
    EXPECT_EQ(metadata->artist, "Ann Example");
    EXPECT_EQ(metadata->copyright, "\xC2\xA9 2026 Ann Example");
    EXPECT_FALSE(metadata->gps.has_value());

    BatchRequest without = h().request(ids, dir_.path() / L"out2");
    without.artist = "Ann Example";
    without.output.metadata.include_copyright = false;
    without.output.metadata.include_capture = false;
    h().run(without);
    auto stripped = exporter::read_export_metadata(dir_.path() / L"out2" / L"photo0_Lucent_0.jpg");
    ASSERT_TRUE(stripped);
    EXPECT_TRUE(stripped->artist.empty());
    EXPECT_FALSE(stripped->has_capture());
}

TEST_F(BatchServiceTest, UnknownItemsAreExcludedWithReasonsAndBadRequestsRefused) {
    const auto files = ts::write_dngs(dir_, L"in", 1);
    const std::vector<ItemId> ids = h().import(files);
    BatchRequest request = h().request(ids, out_);
    request.items.push_back(BatchItem{ItemId{987654}, std::nullopt});
    auto submitted = h().batches().submit(request);
    ASSERT_TRUE(submitted) << submitted.error().describe();
    EXPECT_EQ(submitted->plan.excluded_count, 1u);
    ASSERT_TRUE(h().log.wait_for_kind(submitted->id, BatchEventKind::Finished));
    auto report = h().batches().report(submitted->id);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->exported, 1u);
    EXPECT_EQ(report->skipped, 1u);
    EXPECT_FALSE(report->entries.back().reason.empty());

    BatchRequest nothing;
    nothing.items.push_back(BatchItem{ItemId{987654}, std::nullopt});
    nothing.destination.folder = out_;
    auto refused = h().batches().submit(nothing);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().where, "app.batch.empty");

    BatchRequest bad_pattern = h().request(ids, out_);
    bad_pattern.destination.name_pattern = "{nonsense}";
    EXPECT_FALSE(h().batches().submit(bad_pattern));
    BatchRequest relative = h().request(ids, L"relative\\folder");
    EXPECT_FALSE(h().batches().submit(relative));
    BatchRequest unreproducible = h().request(ids, out_);
    engine::DevelopSettings edited;
    edited.exposure_ev = 1.0;
    unreproducible.items[0].recipe = FrozenRecipe{edited, std::nullopt, 0};
    auto refused_recipe = h().batches().submit(unreproducible);
    ASSERT_FALSE(refused_recipe);
    EXPECT_EQ(refused_recipe.error().code, ErrorCode::InvalidArgument);
}

TEST_F(BatchServiceTest, ASingleExportIsABatchOfOne) {
    const auto files = ts::write_dngs(dir_, L"in", 1);
    const std::vector<ItemId> ids = h().import(files, /*hash=*/false);  // hashed by the batch
    auto executor = TaskExecutor::create(ExecutorOptions::for_this_machine());
    ASSERT_TRUE(executor);
    auto renderer = create_engine_renderer(h().registry());
    ASSERT_TRUE(renderer);
    std::filesystem::create_directories(out_);
    {
        ExportService exports(h().library(), **executor, *renderer, &h().recipes(), &h().batches());
        std::vector<ExportRecord> records;
        for (int round = 0; round < 2; ++round) {
            std::mutex mutex;
            std::condition_variable changed;
            std::optional<Expected<ExportRecord>> result;
            std::vector<ExportProgress> progress;
            ExportRequest request;
            request.item = ids[0];
            request.output_directory = out_;
            auto submitted = exports.submit(
                request,
                [&](const ExportProgress& p) {
                    std::lock_guard lock(mutex);
                    progress.push_back(p);
                },
                [&](ItemId, const Expected<ExportRecord>& record) {
                    {
                        std::lock_guard lock(mutex);
                        result = record;
                    }
                    changed.notify_all();
                });
            ASSERT_TRUE(submitted) << submitted.error().describe();
            std::unique_lock lock(mutex);
            ASSERT_TRUE(changed.wait_for(lock, 120s, [&] { return result.has_value(); }));
            ASSERT_TRUE(*result) << result->error().describe();
            records.push_back(**result);
            ASSERT_FALSE(progress.empty());
            EXPECT_EQ(progress.back().stage, ExportStage::Finished);
        }
        EXPECT_EQ(records[0].receipt.path.filename(), L"photo0_Lucent.jpg");
        EXPECT_EQ(records[1].receipt.path.filename(), L"photo0_Lucent_2.jpg");
        EXPECT_EQ(records[1].name_index, 2u);
        for (const ExportRecord& record : records) {
            EXPECT_EQ(ts::file_sha256(record.receipt.path), record.receipt.sha256);
            EXPECT_GT(record.receipt.width, 0);
            EXPECT_TRUE(record.source_sha256.has_value());
            EXPECT_EQ(record.recipe.settings_sha256, write_recipe(engine::DevelopSettings{})->sha256);
        }
    }
    EXPECT_EQ(ts::folder_entries(out_).size(), 2u) << "single exports write no report files";
    auto batches = h().batches().list();
    ASSERT_TRUE(batches);
    EXPECT_EQ(batches->size(), 2u);
}

// A crop and a quarter turn change the output size by design: the export has the size the recipe's
// geometry gives the photo and no review note about it.
// W10 (ADR 0018, blueprint section 3): an Unsupported photo is skipped with its reason, a Limited
// one is exported with a review note naming its limitation, and the report records each state.
TEST_F(BatchServiceTest, LimitedPhotosNameTheirLimitationAndUnsupportedOnesAreSkipped) {
    const auto files = ts::write_dngs(dir_, L"in", 3);
    const std::vector<ItemId> ids = h().import(files);
    const auto update = [&](ItemId item, SupportStatus status, SupportReason reason) {
        SupportUpdate changed{item, h().library().find(item)->support};
        changed.support.status = status;
        changed.support.reasons = {std::move(reason)};
        return changed;
    };
    SupportReason limitation;
    limitation.kind = SupportReasonKind::CameraLimitation;
    limitation.limitation = "xtrans_baseline_demosaic";
    limitation.message = "X-Trans demosaicing is baseline quality: fine detail can look softer.";
    SupportReason failed;
    failed.kind = SupportReasonKind::CameraUnsupported;
    failed.message = "Lucent's qualification found that this camera mode does not develop correctly.";
    const std::vector<SupportUpdate> updates{update(ids[1], SupportStatus::Limited, limitation),
                                             update(ids[2], SupportStatus::Unsupported, failed)};
    ASSERT_TRUE(h().library().set_support(updates));

    const catalog::BatchId id = h().run(h().request(ids, out_));
    ASSERT_TRUE(id.valid());
    auto report = h().batches().report(id);
    ASSERT_TRUE(report) << report.error().describe();
    EXPECT_EQ(report->exported, 2u);
    EXPECT_EQ(report->skipped, 1u);
    const auto entry_of = [&](ItemId item) -> const ReportEntry* {
        for (const ReportEntry& entry : report->entries) {
            if (entry.item == item) {
                return &entry;
            }
        }
        return nullptr;
    };
    const ReportEntry* clean = entry_of(ids[0]);
    ASSERT_NE(clean, nullptr);
    EXPECT_EQ(clean->outcome, ReportOutcome::Exported);
    EXPECT_EQ(clean->support, SupportStatus::CompatibleUnverified);
    const ReportEntry* limited = entry_of(ids[1]);
    ASSERT_NE(limited, nullptr);
    EXPECT_EQ(limited->outcome, ReportOutcome::Exported);
    EXPECT_EQ(limited->support, SupportStatus::Limited);
    EXPECT_TRUE(std::any_of(limited->review.begin(), limited->review.end(), [](const std::string& note) {
        return note ==
               "Limited support: X-Trans demosaicing is baseline quality: fine detail can look softer.";
    })) << "the review notes do not name the limitation";
    const ReportEntry* skipped = entry_of(ids[2]);
    ASSERT_NE(skipped, nullptr);
    EXPECT_EQ(skipped->outcome, ReportOutcome::Skipped);
    EXPECT_EQ(skipped->support, SupportStatus::Unsupported);
    ASSERT_TRUE(skipped->error.has_value());
    EXPECT_EQ(skipped->error->code, ErrorCode::Unsupported);
    EXPECT_NE(skipped->error->message.find("does not develop correctly"), std::string::npos);
    EXPECT_FALSE(skipped->reason.empty());

    auto json = report_json(*report);
    ASSERT_TRUE(json);
    const nlohmann::json parsed = nlohmann::json::parse(*json);
    std::vector<std::string> states;
    for (const nlohmann::json& entry : parsed["entries"]) {
        states.push_back(entry["support"].is_string() ? entry["support"].get<std::string>() : "-");
    }
    std::sort(states.begin(), states.end());
    EXPECT_EQ(states, (std::vector<std::string>{"compatible_unverified", "limited", "unsupported"}));
    EXPECT_NE(report_html(*report).find("Camera support: Limited"), std::string::npos);
}

// AST-07: a photo that becomes Unsupported after its batch was created (a database update, a
// re-probe) is skipped when its job runs, as at creation, not exported.
TEST_F(BatchServiceTest, APhotoThatBecameUnsupportedAfterCreationIsSkipped) {
    const auto files = ts::write_dngs(dir_, L"in", 2);
    const std::vector<ItemId> ids = h().import(files);
    BatchRequest request = h().request(ids, out_);
    request.start = false;  // created now, run later
    auto submitted = h().batches().submit(request);
    ASSERT_TRUE(submitted) << submitted.error().describe();
    SupportReason failed;
    failed.kind = SupportReasonKind::CameraUnsupported;
    failed.message = "Lucent's qualification found that this camera mode does not develop correctly.";
    SupportUpdate changed{ids[1], h().library().find(ids[1])->support};
    changed.support.status = SupportStatus::Unsupported;
    changed.support.reasons = {failed};
    const std::vector<SupportUpdate> updates{changed};
    ASSERT_TRUE(h().library().set_support(updates));

    ASSERT_TRUE(h().batches().start(submitted->id));
    ASSERT_TRUE(h().log.wait_for_kind(submitted->id, BatchEventKind::Finished));
    auto report = h().batches().report(submitted->id);
    ASSERT_TRUE(report) << report.error().describe();
    EXPECT_EQ(report->exported, 1u);
    EXPECT_EQ(report->skipped, 1u);
    EXPECT_EQ(report->failed, 0u);
    const auto entry = std::find_if(report->entries.begin(), report->entries.end(),
                                    [&](const ReportEntry& e) { return e.item == ids[1]; });
    ASSERT_NE(entry, report->entries.end());
    EXPECT_EQ(entry->outcome, ReportOutcome::Skipped);
    ASSERT_TRUE(entry->error.has_value());
    EXPECT_EQ(entry->error->code, ErrorCode::Unsupported);
    EXPECT_NE(entry->error->message.find("does not develop correctly"), std::string::npos) << entry->error->message;
    EXPECT_EQ(count_with_extension(out_, L".jpg"), 1u);
}

// QUA-04: a batch frozen by an earlier build with the as-shot settings of geometry version 1 (W9,
// identity 3104dbcc...) still runs after the update, rendering exactly that frozen recipe.
TEST_F(BatchServiceTest, AsShotJobsFrozenWithAnEarlierGeometryVersionStillRun) {
    const auto files = ts::write_dngs(dir_, L"in", 1);
    const std::vector<ItemId> ids = h().import(files);
    // A Draft batch gives the destination, export and policy documents this build writes.
    BatchRequest request = h().request(ids, out_);
    request.start = false;
    auto draft = h().batches().submit(request);
    ASSERT_TRUE(draft) << draft.error().describe();
    const catalog::BatchRecord record = h().record(draft->id);
    const std::vector<catalog::JobRecord> jobs = h().jobs(draft->id);
    ASSERT_EQ(jobs.size(), 1u);
    ASSERT_TRUE(jobs[0].item.has_value());

    // The same batch as the earlier build froze it: no revision, the geometry-1 as-shot identity.
    engine::DevelopSettings earlier;
    earlier.operations.geometry = 1;
    const auto earlier_document = write_recipe(earlier);
    ASSERT_TRUE(earlier_document);
    ASSERT_NE(earlier_document->sha256, write_recipe(engine::DevelopSettings{})->sha256);
    catalog::NewBatch batch;
    batch.name = "frozen before the update";
    batch.selection_description = "1 selected photo";
    batch.destination_json = record.destination_json;
    batch.export_settings_json = record.export_settings_json;
    batch.policy_json = record.policy_json;
    batch.renderer_version = record.renderer_version;
    batch.created_at = record.created_at;
    batch.start = true;
    catalog::NewJob job;
    job.item = jobs[0].item.value();
    job.settings_sha256 = earlier_document->sha256;
    auto created = h().catalog().transact(
        [&](catalog::WriteTransaction& tx) { return tx.create_batch(batch, std::span(&job, 1)); });
    ASSERT_TRUE(created) << created.error().describe();
    h().reopen();  // the updated build starts and resumes the unfinished batch
    ASSERT_TRUE(h().log.wait_for_kind(created->id, BatchEventKind::Finished));

    const catalog::BatchRecord resumed = h().record(created->id);
    EXPECT_EQ(resumed.counts.complete, 1u);
    EXPECT_EQ(resumed.counts.failed, 0u);
    const auto receipts = h().receipts(created->id);
    ASSERT_EQ(receipts.size(), 1u);
    auto details = read_receipt_details(receipts[0].settings_json);
    ASSERT_TRUE(details) << details.error().describe();
    EXPECT_EQ(details->recipe_sha256, earlier_document->sha256) << "the frozen recipe, not today's defaults";
}

TEST_F(BatchServiceTest, AGeometryEditSetsTheOutputSizeWithoutAReviewNote) {
    const auto files = ts::write_dngs(dir_, L"in", 1);
    const std::vector<ItemId> ids = h().import(files);
    engine::DevelopSettings settings;
    settings.geometry.crop = engine::CropRect{0.1, 0.2, 0.7, 0.9};
    settings.geometry.quarter_turns = 1;
    settings.geometry.flip_horizontal = true;
    ASSERT_TRUE(h().recipes().apply(ids[0], settings));
    const catalog::BatchId id = h().run(h().request(ids, out_));
    ASSERT_TRUE(id.valid());
    const auto receipts = h().receipts(id);
    ASSERT_EQ(receipts.size(), 1u);
    auto details = read_receipt_details(receipts[0].settings_json);
    ASSERT_TRUE(details) << details.error().describe();
    const auto source = h().library().find(ids[0]);
    ASSERT_TRUE(source && source->probe);
    auto mapping = engine::GeometryMapping::create(settings.geometry, source->probe->upright_size);
    ASSERT_TRUE(mapping);
    EXPECT_NE(mapping->output_size(), source->probe->upright_size);
    EXPECT_EQ(static_cast<std::int32_t>(details->width), mapping->output_size().width);
    EXPECT_EQ(static_cast<std::int32_t>(details->height), mapping->output_size().height);
    for (const std::string& note : details->review) {
        EXPECT_EQ(note.find("catalogued"), std::string::npos) << note;
    }
}

TEST_F(BatchServiceTest, TwoHundredPhotosWithBoundedCoordinatorMemory) {
    const auto files = ts::write_dngs(dir_, L"in", 200);
    const std::vector<ItemId> ids = h().import(files);
    std::atomic<bool> sampling{true};
    std::atomic<std::uint64_t> peak{0};
    std::atomic<std::uint64_t> after_warmup{0};
    std::atomic<std::uint64_t> completed{0};
    Subscription counter = h().batches().subscribe([&](const BatchEvent& event) {
        if (event.kind == BatchEventKind::JobFinished && event.job_stage == catalog::JobStage::Complete) {
            completed.fetch_add(1);
        }
    });
    std::thread sampler([&] {
        while (sampling.load()) {
            const std::uint64_t bytes = ts::private_bytes();
            std::uint64_t seen = peak.load();
            while (bytes > seen && !peak.compare_exchange_weak(seen, bytes)) {
            }
            if (after_warmup.load() == 0 && completed.load() >= 25) {
                after_warmup.store(bytes);
            }
            std::this_thread::sleep_for(20ms);
        }
    });
    const auto start = std::chrono::steady_clock::now();
    const catalog::BatchId id = h().run(h().request(ids, out_), 600s);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const std::uint64_t at_end = ts::private_bytes();
    sampling = false;
    sampler.join();
    counter.reset();
    EXPECT_EQ(h().record(id).counts.complete, 200u);
    EXPECT_EQ(count_with_extension(out_, L".jpg"), 200u);
    const BatchSchedulerStats stats = h().batches().stats();
    std::printf("  200 photos in %.1f s; coordinator private bytes: after 25 jobs %.1f MB, peak %.1f MB, end %.1f MB; "
                "worker peak commit %.1f MB; at most %u workers; %llu worker starts\n",
                seconds, after_warmup.load() / 1048576.0, peak.load() / 1048576.0, at_end / 1048576.0,
                stats.peak_worker_commit_bytes / 1048576.0, stats.peak_active_workers,
                static_cast<unsigned long long>(stats.worker_starts));
    RecordProperty("coordinator_peak_private_mb", static_cast<int>(peak.load() >> 20));
    RecordProperty("worker_peak_commit_mb", static_cast<int>(stats.peak_worker_commit_bytes >> 20));
    // Bounded: the coordinator keeps no per-job state after a job ends.
    ASSERT_GT(after_warmup.load(), 0u);
    EXPECT_LT(peak.load(), after_warmup.load() + (std::uint64_t{48} << 20));
    EXPECT_TRUE(stats.calibrated);
    EXPECT_LE(stats.peak_active_workers, 2u);
}

TEST_F(BatchServiceTest, LongAndUnicodeDestinationsWork) {
    rts::SyntheticDngSpec spec;
    spec.width = 70;
    const auto source = ts::write_dng(dir_, L"in\\Fjörð \U0001F4F7 写真.dng", spec);
    // A destination far beyond MAX_PATH, with non-ASCII folder names.
    std::filesystem::path deep = dir_.path() / L"Ünïcödé 写真";
    while (deep.native().size() < 300) {
        deep /= L"a-rather-long-folder-name-for-the-test";
    }
    const std::vector<ItemId> ids = h().import({source});
    const catalog::BatchId id = h().run(h().request(ids, deep));
    ASSERT_TRUE(id.valid());
    EXPECT_EQ(h().record(id).counts.complete, 1u);
    const auto receipts = h().receipts(id);
    ASSERT_EQ(receipts.size(), 1u);
    EXPECT_GT(receipts[0].final_path.native().size(), 300u);
    EXPECT_EQ(receipts[0].final_path.filename(), L"Fjörð \U0001F4F7 写真_Lucent_0.jpg");
    EXPECT_NE(::GetFileAttributesW(rts::verbatim(receipts[0].final_path).c_str()), INVALID_FILE_ATTRIBUTES);
    EXPECT_EQ(ts::file_sha256(receipts[0].final_path), receipts[0].output_sha256);
}

// ---- Failures, retries and control ------------------------------------------------------------

class BatchFailureTest : public ::testing::Test {
protected:
    void open(BatchServiceOptions options) {
        harness_ = std::make_unique<ts::BatchHarness>(dir_.path() / L"lucent", std::move(options));
        out_ = dir_.path() / L"out";
    }
    ts::BatchHarness& h() { return *harness_; }

    // Waits for the first progress event of the batch in the worker's Rendering stage.
    bool wait_rendering(catalog::BatchId id) {
        return h().log.wait_for([&](const std::vector<BatchEvent>& events) {
            for (const BatchEvent& event : events) {
                if (event.batch == id && event.kind == BatchEventKind::JobProgress &&
                    event.worker_stage == workers::JobStage::Rendering) {
                    return true;
                }
            }
            return false;
        });
    }

    catalog::JobRecord job_at(catalog::BatchId id, std::uint32_t ordinal) {
        for (const catalog::JobRecord& job : h().jobs(id)) {
            if (job.ordinal == ordinal) {
                return job;
            }
        }
        ADD_FAILURE() << "no job " << ordinal;
        return {};
    }

    ts::ScratchDir dir_;
    std::unique_ptr<ts::BatchHarness> harness_;
    std::filesystem::path out_;
};

TEST_F(BatchFailureTest, AWorkerCrashIsRetried) {
    BatchServiceOptions options = ts::fast_batch_options();
    options.test.worker_hooks_by_start = {{"crash-job"}};
    open(options);
    const std::vector<ItemId> ids = h().import(ts::write_dngs(dir_, L"in", 1));
    const catalog::BatchId id = h().run(h().request(ids, out_));
    const catalog::JobRecord job = job_at(id, 0);
    EXPECT_EQ(job.stage, catalog::JobStage::Complete);
    EXPECT_EQ(job.attempts, 2u);
    EXPECT_EQ(job.transient_failures, 1u);
    EXPECT_GE(h().batches().stats().worker_starts, 2u);
    EXPECT_TRUE(ts::leftovers(out_).empty());
    // The failed attempt's publication was closed.
    auto journal = h().catalog().query([&](catalog::ReadTransaction& tx) { return tx.publication_journal(job.id); });
    ASSERT_TRUE(journal);
    bool abandoned = false;
    for (const catalog::PublicationRecord& entry : *journal) {
        abandoned = abandoned || (entry.attempt == 1 && entry.step == catalog::PublicationStep::Abandoned);
    }
    EXPECT_TRUE(abandoned);
}

TEST_F(BatchFailureTest, AWorkerKilledMidJobIsRetried) {
    BatchServiceOptions options = ts::fast_batch_options();
    options.test.worker_hooks_by_start = {{"render-delay:300"}};
    open(options);
    const std::vector<ItemId> ids = h().import(ts::write_dngs(dir_, L"in", 2));
    auto submitted = h().batches().submit(h().request(ids, out_));
    ASSERT_TRUE(submitted);
    ASSERT_TRUE(wait_rendering(submitted->id));
    std::vector<std::uint32_t> pids;
    for (int i = 0; i < 100 && pids.empty(); ++i) {
        pids = h().batches().stats().worker_pids;
        std::this_thread::sleep_for(20ms);
    }
    ASSERT_FALSE(pids.empty());
    HANDLE process = ::OpenProcess(PROCESS_TERMINATE, FALSE, pids.front());
    ASSERT_NE(process, nullptr);
    EXPECT_TRUE(::TerminateProcess(process, 9));
    ::CloseHandle(process);
    ASSERT_TRUE(h().log.wait_for_kind(submitted->id, BatchEventKind::Finished));
    const catalog::BatchRecord record = h().record(submitted->id);
    EXPECT_EQ(record.counts.complete, 2u);
    EXPECT_EQ(job_at(submitted->id, 0).attempts, 2u);
    EXPECT_EQ(count_with_extension(out_, L".jpg"), 2u);
    EXPECT_TRUE(ts::leftovers(out_).empty());
}

TEST_F(BatchFailureTest, TransientFailuresAreBounded) {
    BatchServiceOptions options = ts::fast_batch_options();
    options.retry.max_transient_failures = 2;
    options.test.worker_hooks_by_start = {{"crash-job"}, {"crash-job"}, {"crash-job"}};
    open(options);
    const std::vector<ItemId> ids = h().import(ts::write_dngs(dir_, L"in", 1));
    const catalog::BatchId id = h().run(h().request(ids, out_));
    const catalog::JobRecord job = job_at(id, 0);
    EXPECT_EQ(job.stage, catalog::JobStage::Failed);
    EXPECT_EQ(job.attempts, 3u);  // the first try and two retries
    ASSERT_TRUE(job.last_error);
    EXPECT_EQ(job.last_error->failure_class, catalog::FailureClass::Transient);
    auto report = h().batches().report(id);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->failed, 1u);
    EXPECT_FALSE(report->entries[0].action.empty());
    // Retry failed runs it again (the hooks are used up).
    auto retried = h().batches().retry_failed(id);
    ASSERT_TRUE(retried);
    EXPECT_EQ(*retried, 1u);
    ASSERT_TRUE(h().log.wait_for([&](const std::vector<BatchEvent>& events) {
        return std::count_if(events.begin(), events.end(), [&](const BatchEvent& e) {
                   return e.batch == id && e.kind == BatchEventKind::Finished;
               }) >= 2;
    }));
    EXPECT_EQ(job_at(id, 0).stage, catalog::JobStage::Complete);
}

TEST_F(BatchFailureTest, ASourceChangedMidBatchFailsOnlyThatPhoto) {
    BatchServiceOptions options = ts::fast_batch_options();
    options.max_workers = 1;
    options.test.worker_hooks_by_start = {{"render-delay:150"}};
    open(options);
    const auto files = ts::write_dngs(dir_, L"in", 3);
    const core::Digest256 untouched0 = ts::file_sha256(files[0]);
    const core::Digest256 untouched1 = ts::file_sha256(files[1]);
    const std::vector<ItemId> ids = h().import(files);
    auto submitted = h().batches().submit(h().request(ids, out_));
    ASSERT_TRUE(submitted);
    ASSERT_TRUE(wait_rendering(submitted->id));
    {
        std::ofstream append(files[2], std::ios::binary | std::ios::app);
        append << "edited by another program";
    }
    ASSERT_TRUE(h().log.wait_for_kind(submitted->id, BatchEventKind::Finished));
    const catalog::JobRecord changed = job_at(submitted->id, 2);
    EXPECT_EQ(changed.stage, catalog::JobStage::Failed);
    EXPECT_EQ(changed.attempts, 1u) << "deterministic failures fail fast";
    ASSERT_TRUE(changed.last_error);
    EXPECT_EQ(changed.last_error->code, ErrorCode::SourceChanged);
    EXPECT_EQ(job_at(submitted->id, 0).stage, catalog::JobStage::Complete);
    EXPECT_EQ(job_at(submitted->id, 1).stage, catalog::JobStage::Complete);
    EXPECT_EQ(ts::file_sha256(files[0]), untouched0);
    EXPECT_EQ(ts::file_sha256(files[1]), untouched1);
    auto report = h().batches().report(submitted->id);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->failed, 1u);
    EXPECT_NE(report->entries[2].action.find("again"), std::string::npos);
}

TEST_F(BatchFailureTest, AMissingSourceFailsWithAnActionAndRetriesAfterRelink) {
    BatchServiceOptions options = ts::fast_batch_options();
    options.max_workers = 1;
    options.test.worker_hooks_by_start = {{"render-delay:150"}};
    open(options);
    const auto files = ts::write_dngs(dir_, L"in", 2);
    const std::vector<ItemId> ids = h().import(files);
    auto submitted = h().batches().submit(h().request(ids, out_));
    ASSERT_TRUE(submitted);
    ASSERT_TRUE(wait_rendering(submitted->id));
    const auto moved = files[1].parent_path() / L"moved.bin";
    std::filesystem::rename(files[1], moved);
    ASSERT_TRUE(h().log.wait_for_kind(submitted->id, BatchEventKind::Finished));
    const catalog::JobRecord missing = job_at(submitted->id, 1);
    EXPECT_EQ(missing.stage, catalog::JobStage::Failed);
    ASSERT_TRUE(missing.last_error);
    EXPECT_EQ(missing.last_error->code, ErrorCode::NotFound);
    auto report = h().batches().report(submitted->id);
    ASSERT_TRUE(report);
    EXPECT_NE(report->entries[1].action.find("Reconnect"), std::string::npos);
    // The file comes back: Retry failed exports it.
    std::filesystem::rename(moved, files[1]);
    ASSERT_TRUE(h().batches().retry_failed(submitted->id));
    ASSERT_TRUE(h().log.wait_for([&](const std::vector<BatchEvent>& events) {
        return std::count_if(events.begin(), events.end(), [&](const BatchEvent& e) {
                   return e.batch == submitted->id && e.kind == BatchEventKind::Finished;
               }) >= 2;
    }));
    EXPECT_EQ(job_at(submitted->id, 1).stage, catalog::JobStage::Complete);
}

TEST_F(BatchFailureTest, AFullDiskPausesTheBatchUntilResumed) {
    BatchServiceOptions options = ts::fast_batch_options();
    auto free = std::make_shared<std::atomic<std::uint64_t>>(std::uint64_t{1} << 40);
    options.test.free_space = [free](const std::filesystem::path&) -> std::optional<std::uint64_t> {
        return free->load();
    };
    open(options);
    const std::vector<ItemId> ids = h().import(ts::write_dngs(dir_, L"in", 3));
    BatchRequest request = h().request(ids, out_);
    request.start = false;
    auto submitted = h().batches().submit(request);
    ASSERT_TRUE(submitted) << submitted.error().describe();
    free->store(1 << 20);  // 1 MiB left
    ASSERT_TRUE(h().batches().start(submitted->id));
    ASSERT_TRUE(h().log.wait_for_kind(submitted->id, BatchEventKind::Attention, 30s));
    auto status = h().batches().status(submitted->id);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->attention, BatchAttention::DestinationFull);
    EXPECT_EQ(status->record.state, catalog::BatchState::Paused);
    EXPECT_EQ(status->record.counts.queued, 3u);
    for (const catalog::JobRecord& job : h().jobs(submitted->id)) {
        EXPECT_EQ(job.transient_failures + job.deterministic_failures, 0u) << "a full disk is not the photo's fault";
    }
    free->store(std::uint64_t{1} << 40);
    ASSERT_TRUE(h().batches().resume(submitted->id));
    ASSERT_TRUE(h().log.wait_for_kind(submitted->id, BatchEventKind::Finished));
    EXPECT_EQ(h().record(submitted->id).counts.complete, 3u);
    EXPECT_EQ(h().batches().status(submitted->id)->attention, BatchAttention::None);
}

TEST_F(BatchFailureTest, ARemovedDestinationPausesTheBatchUntilItReturns) {
    BatchServiceOptions options = ts::fast_batch_options();
    options.max_workers = 1;
    options.test.worker_hooks_by_start = {{"render-delay:200"}};
    open(options);
    const std::vector<ItemId> ids = h().import(ts::write_dngs(dir_, L"in", 3));
    auto submitted = h().batches().submit(h().request(ids, out_));
    ASSERT_TRUE(submitted);
    ASSERT_TRUE(wait_rendering(submitted->id));
    // The drive goes away while the first photo renders (the folder disappears from its path).
    const auto away = dir_.path() / L"away";
    std::filesystem::rename(out_, away);
    ASSERT_TRUE(h().log.wait_for_kind(submitted->id, BatchEventKind::Attention, 60s));
    auto status = h().batches().status(submitted->id);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->attention, BatchAttention::DestinationUnavailable);
    EXPECT_EQ(status->record.state, catalog::BatchState::Paused);
    EXPECT_EQ(status->record.counts.failed, 0u);
    // It comes back: resume continues and nothing is exported twice.
    std::filesystem::rename(away, out_);
    ASSERT_TRUE(h().batches().resume(submitted->id));
    ASSERT_TRUE(h().log.wait_for_kind(submitted->id, BatchEventKind::Finished));
    EXPECT_EQ(h().record(submitted->id).counts.complete, 3u);
    EXPECT_EQ(count_with_extension(out_, L".jpg"), 3u);
    EXPECT_TRUE(ts::leftovers(out_).empty());
}

TEST_F(BatchFailureTest, PauseLetsTheRunningJobFinishAndResumeContinues) {
    BatchServiceOptions options = ts::fast_batch_options();
    options.max_workers = 1;
    options.test.worker_hooks_by_start = {{"render-delay:100"}};
    open(options);
    const std::vector<ItemId> ids = h().import(ts::write_dngs(dir_, L"in", 3));
    auto submitted = h().batches().submit(h().request(ids, out_));
    ASSERT_TRUE(submitted);
    ASSERT_TRUE(wait_rendering(submitted->id));
    ASSERT_TRUE(h().batches().pause(submitted->id));
    ASSERT_TRUE(h().log.wait_for([&](const std::vector<BatchEvent>& events) {
        return std::any_of(events.begin(), events.end(), [&](const BatchEvent& e) {
            return e.batch == submitted->id && e.kind == BatchEventKind::JobFinished;
        });
    }));
    std::this_thread::sleep_for(500ms);  // nothing new may start meanwhile
    catalog::BatchRecord record = h().record(submitted->id);
    EXPECT_EQ(record.state, catalog::BatchState::Paused);
    EXPECT_EQ(record.counts.complete, 1u) << "the running job finished its attempt";
    EXPECT_EQ(record.counts.queued, 2u);
    ASSERT_TRUE(h().batches().resume(submitted->id));
    ASSERT_TRUE(h().log.wait_for_kind(submitted->id, BatchEventKind::Finished));
    EXPECT_EQ(h().record(submitted->id).counts.complete, 3u);
}

TEST_F(BatchFailureTest, CancelStopsQueuedAndRunningJobs) {
    BatchServiceOptions options = ts::fast_batch_options();
    options.max_workers = 1;
    options.test.worker_hooks_by_start = {{"render-delay:400"}};
    open(options);
    const std::vector<ItemId> ids = h().import(ts::write_dngs(dir_, L"in", 3));
    auto submitted = h().batches().submit(h().request(ids, out_));
    ASSERT_TRUE(submitted);
    ASSERT_TRUE(wait_rendering(submitted->id));
    ASSERT_TRUE(h().batches().cancel(submitted->id));
    ASSERT_TRUE(h().log.wait_for_kind(submitted->id, BatchEventKind::Finished));
    const catalog::BatchRecord record = h().record(submitted->id);
    EXPECT_EQ(record.state, catalog::BatchState::Cancelled);
    EXPECT_EQ(record.counts.cancelled, 3u);
    EXPECT_EQ(count_with_extension(out_, L".jpg"), 0u);
    EXPECT_TRUE(ts::leftovers(out_).empty());
    auto report = h().batches().report(submitted->id);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->cancelled, 3u);
    EXPECT_TRUE(std::filesystem::exists(out_ / std::format(L"Lucent-batch-{}-report.html", submitted->id.value)));
}

TEST_F(BatchFailureTest, InteractiveWorkHoldsBackAdmission) {
    BatchServiceOptions options = ts::fast_batch_options();
    auto busy = std::make_shared<std::atomic<bool>>(true);
    options.interactive_busy = [busy] { return busy->load(); };
    open(options);
    const std::vector<ItemId> ids = h().import(ts::write_dngs(dir_, L"in", 2));
    auto submitted = h().batches().submit(h().request(ids, out_));
    ASSERT_TRUE(submitted);
    std::this_thread::sleep_for(700ms);
    EXPECT_EQ(h().log.count(submitted->id, BatchEventKind::JobStarted), 0u);
    EXPECT_GT(h().batches().stats().deferred_for_interactive, 0u);
    busy->store(false);
    h().batches().wake();
    ASSERT_TRUE(h().log.wait_for_kind(submitted->id, BatchEventKind::Finished));
    EXPECT_EQ(h().record(submitted->id).counts.complete, 2u);
}

TEST_F(BatchFailureTest, ClosingTheServiceGivesJobsBackAndTheyContinue) {
    BatchServiceOptions options = ts::fast_batch_options();
    options.max_workers = 1;
    options.test.worker_hooks_by_start = {{"render-delay:300"}};
    open(options);
    const std::vector<ItemId> ids = h().import(ts::write_dngs(dir_, L"in", 2));
    auto submitted = h().batches().submit(h().request(ids, out_));
    ASSERT_TRUE(submitted);
    ASSERT_TRUE(wait_rendering(submitted->id));
    h().reopen();  // Lucent closes mid-job and starts again
    ASSERT_TRUE(h().log.wait_for_kind(submitted->id, BatchEventKind::Finished));
    EXPECT_EQ(h().record(submitted->id).counts.complete, 2u);
    EXPECT_EQ(count_with_extension(out_, L".jpg"), 2u);
    EXPECT_TRUE(ts::leftovers(out_).empty());
    EXPECT_EQ(h().batches().last_recovery().reclaimed, 0u) << "a clean close leaves no lease behind";
}

TEST_F(BatchFailureTest, AFileCreatedAtTheOutputNameMeanwhileIsNeverReplaced) {
    BatchServiceOptions options = ts::fast_batch_options();
    options.max_workers = 1;
    options.test.worker_hooks_by_start = {{"render-delay:300"}};
    open(options);
    const std::vector<ItemId> ids = h().import(ts::write_dngs(dir_, L"in", 1));
    auto submitted = h().batches().submit(h().request(ids, out_));
    ASSERT_TRUE(submitted) << submitted.error().describe();
    ASSERT_TRUE(wait_rendering(submitted->id));
    // Another program takes the reserved name while the photo renders.
    const std::string theirs = "another program's file";
    const auto taken = dir_.write_text(L"out\\photo0_Lucent_0.jpg", theirs);
    ASSERT_TRUE(h().log.wait_for_kind(submitted->id, BatchEventKind::Finished));
    EXPECT_EQ(h().record(submitted->id).counts.complete, 1u);
    EXPECT_EQ(ts::file_sha256(taken), *core::sha256(std::as_bytes(std::span(theirs.data(), theirs.size()))))
        << "a file created after the name was checked is never replaced";
    const auto receipts = h().receipts(submitted->id);
    ASSERT_EQ(receipts.size(), 1u);
    EXPECT_EQ(receipts[0].final_path.filename(), L"photo0_Lucent_0_2.jpg");
    EXPECT_EQ(ts::file_sha256(receipts[0].final_path), receipts[0].output_sha256);
    auto details = read_receipt_details(receipts[0].settings_json);
    ASSERT_TRUE(details);
    EXPECT_EQ(details->name_index, 2u);
    EXPECT_FALSE(details->review.empty()) << "a renamed output recommends a look";
    EXPECT_TRUE(ts::leftovers(out_).empty());
}

TEST_F(BatchFailureTest, AnOlderBatchRunsHoweverManyBatchesFollowIt) {
    // Every single export is a batch: an unfinished batch must be found among any number of newer
    // ones, also after a restart.
    BatchServiceOptions options = ts::fast_batch_options();
    auto busy = std::make_shared<std::atomic<bool>>(false);
    options.interactive_busy = [busy] { return busy->load(); };
    open(options);
    const std::vector<ItemId> ids = h().import(ts::write_dngs(dir_, L"in", 1));
    const catalog::BatchId first = h().run(h().request(ids, out_));
    ASSERT_EQ(h().record(first).counts.complete, 1u);
    busy->store(true);  // nothing is admitted from here on
    auto older = h().batches().submit(h().request(ids, dir_.path() / L"out2"));
    ASSERT_TRUE(older) << older.error().describe();
    for (int i = 0; i < 70; ++i) {
        // Newer batches that finish at once: their output already exists in `out_`.
        BatchRequest again = h().request(ids, out_);
        again.write_report_files = false;
        auto skipped = h().batches().submit(again);
        ASSERT_TRUE(skipped) << skipped.error().describe();
        ASSERT_EQ(skipped->skipped_existing, 1u);
    }
    h().reopen();
    busy->store(false);
    h().batches().wake();
    ASSERT_TRUE(h().log.wait_for_kind(older->id, BatchEventKind::Finished, 60s));
    EXPECT_EQ(h().record(older->id).counts.complete, 1u);
    EXPECT_TRUE(std::filesystem::exists(dir_.path() / L"out2" / L"photo0_Lucent_0.jpg"));
}


// Runs icacls with `arguments` (true when it succeeded).
bool icacls(const std::wstring& arguments) {
    std::wstring command = L"icacls.exe " + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                          &startup, &process)) {
        return false;
    }
    ::WaitForSingleObject(process.hProcess, 30'000);
    DWORD code = 1;
    ::GetExitCodeProcess(process.hProcess, &code);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return code == 0;
}

std::wstring user_name() {
    wchar_t name[256]{};
    DWORD size = 256;
    ::GetUserNameW(name, &size);
    return name;
}

TEST_F(BatchFailureTest, LostPermissionPausesTheBatchUntilRestored) {
    BatchServiceOptions options = ts::fast_batch_options();
    options.max_workers = 1;
    open(options);
    const std::vector<ItemId> ids = h().import(ts::write_dngs(dir_, L"in", 2));
    BatchRequest request = h().request(ids, out_);
    request.start = false;
    auto submitted = h().batches().submit(request);
    ASSERT_TRUE(submitted) << submitted.error().describe();
    const std::wstring folder = L"\"" + out_.wstring() + L"\"";
    const std::wstring user = L"\"" + user_name() + L":(W)\"";
    ASSERT_TRUE(icacls(folder + L" /deny " + user)) << "cannot deny writing";
    struct Restore {
        std::wstring folder;
        ~Restore() { icacls(folder + L" /remove:d \"" + user_name() + L"\""); }
    } restore{folder};
    ASSERT_TRUE(h().batches().start(submitted->id));
    ASSERT_TRUE(h().log.wait_for_kind(submitted->id, BatchEventKind::Attention, 60s));
    auto status = h().batches().status(submitted->id);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->attention, BatchAttention::DestinationPermission);
    EXPECT_EQ(status->record.state, catalog::BatchState::Paused);
    EXPECT_EQ(status->record.counts.failed, 0u);
    // Permission comes back: resume finishes the batch.
    ASSERT_TRUE(icacls(folder + L" /remove:d \"" + user_name() + L"\""));
    ASSERT_TRUE(h().batches().resume(submitted->id));
    ASSERT_TRUE(h().log.wait_for_kind(submitted->id, BatchEventKind::Finished));
    EXPECT_EQ(h().record(submitted->id).counts.complete, 2u);
    EXPECT_TRUE(ts::leftovers(out_).empty());
}

// ---- Closing review of W7: interruptions, folders created again, plans, status facts ---------

TEST_F(BatchFailureTest, InterruptionsAreBoundedApartFromTransientFailures) {
    BatchServiceOptions options = ts::fast_batch_options();
    options.lease_duration = 2s;
    options.lease_renewal = 1s;
    options.retry.max_interruptions = 1;
    // A step that outlasts the lease (like a system sleep): every attempt loses it.
    options.test.on_point = [](std::string_view point, catalog::JobId) {
        if (point == "temp_written") {
            std::this_thread::sleep_for(3s);
        }
    };
    open(options);
    const std::vector<ItemId> ids = h().import(ts::write_dngs(dir_, L"in", 1));
    const catalog::BatchId id = h().run(h().request(ids, out_), 120s);
    const catalog::JobRecord job = job_at(id, 0);
    EXPECT_EQ(job.stage, catalog::JobStage::Failed) << "a job interrupted at every attempt does not loop forever";
    EXPECT_EQ(job.attempts, 2u);
    EXPECT_EQ(job.interruptions, 2u);
    EXPECT_EQ(job.transient_failures, 0u) << "interruptions keep the budget of genuine worker failures";
    ASSERT_TRUE(job.last_error);
    EXPECT_EQ(job.last_error->where, "app.batch.interrupted.limit");
    auto report = h().batches().report(id);
    ASSERT_TRUE(report);
    ASSERT_FALSE(report->entries.empty());
    EXPECT_NE(report->entries[0].reason.find("interrupted repeatedly"), std::string::npos) << report->entries[0].reason;
    EXPECT_TRUE(ts::leftovers(out_).empty());
    auto status = h().batches().status(id);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->retryable, 1u);
}

TEST_F(BatchFailureTest, AFolderCreatedAgainIsUsedOnlyOnceTheUserAcceptsIt) {
    open(ts::fast_batch_options());
    const std::vector<ItemId> ids = h().import(ts::write_dngs(dir_, L"in", 2));
    BatchRequest request = h().request(ids, out_);
    request.start = false;
    auto submitted = h().batches().submit(request);
    ASSERT_TRUE(submitted) << submitted.error().describe();
    const catalog::BatchId id = submitted->id;
    // The user deletes the output folder: the batch never creates it again by itself.
    std::filesystem::remove_all(out_);
    ASSERT_TRUE(h().batches().start(id));
    ASSERT_TRUE(h().log.wait_for_kind(id, BatchEventKind::Attention, 30s));
    auto status = h().batches().status(id);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->attention, BatchAttention::DestinationUnavailable);
    std::this_thread::sleep_for(300ms);
    EXPECT_FALSE(std::filesystem::exists(out_));
    // The user creates it again: another folder object. Resuming says so instead of using it.
    std::filesystem::create_directories(out_);
    ASSERT_TRUE(ts::eventually([&] { return h().record(id).state == catalog::BatchState::Paused; }));
    ASSERT_TRUE(h().batches().resume(id));
    ASSERT_TRUE(ts::eventually([&] { return h().log.count(id, BatchEventKind::Attention) >= 2; }));
    status = h().batches().status(id);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->attention, BatchAttention::DestinationReplaced);
    EXPECT_EQ(status->record.counts.queued, 2u);
    // Accepting the new folder continues there, and the choice is recorded in the batch.
    ASSERT_TRUE(ts::eventually([&] { return h().record(id).state == catalog::BatchState::Paused; }));
    ASSERT_TRUE(h().batches().use_replaced_folder(id));
    ASSERT_TRUE(h().log.wait_for_kind(id, BatchEventKind::Finished));
    EXPECT_EQ(h().record(id).counts.complete, 2u);
    EXPECT_EQ(count_with_extension(out_, L".jpg"), 2u);
    EXPECT_EQ(h().batches().status(id)->attention, BatchAttention::None);
    EXPECT_FALSE(h().batches().use_replaced_folder(id)) << "a finished batch keeps its folder";
}

TEST_F(BatchFailureTest, ASingleExportRunsBeforeTheRestOfAnOlderBatch) {
    BatchServiceOptions options = ts::fast_batch_options();
    options.max_workers = 1;
    options.test.worker_hooks_by_start = {{"render-delay:150"}};
    open(options);
    const std::vector<ItemId> ids = h().import(ts::write_dngs(dir_, L"in", 13));
    const std::vector<ItemId> many(ids.begin(), ids.begin() + 12);
    auto big = h().batches().submit(h().request(many, out_));
    ASSERT_TRUE(big);
    ASSERT_TRUE(wait_rendering(big->id));
    BatchRequest single = h().request({ids[12]}, dir_.path() / L"single");
    single.single_export = true;
    auto one = h().batches().submit(single);
    ASSERT_TRUE(one) << one.error().describe();
    ASSERT_TRUE(h().log.wait_for_kind(one->id, BatchEventKind::Finished, 60s));
    EXPECT_GT(h().record(big->id).counts.pending(), 0u) << "the single export did not wait for the whole batch";
    auto status = h().batches().status(one->id);
    ASSERT_TRUE(status);
    EXPECT_TRUE(status->single_export);
    ASSERT_TRUE(status->output);
    EXPECT_EQ(status->output->filename(), L"photo12_Lucent_0.jpg");
    ASSERT_TRUE(h().log.wait_for_kind(big->id, BatchEventKind::Finished));
}

TEST_F(BatchFailureTest, APausedBatchHasNoTimeLeftToTell) {
    BatchServiceOptions options = ts::fast_batch_options();
    options.max_workers = 1;
    options.test.worker_hooks_by_start = {{"render-delay:100"}};
    open(options);
    const std::vector<ItemId> ids = h().import(ts::write_dngs(dir_, L"in", 8));
    auto submitted = h().batches().submit(h().request(ids, out_));
    ASSERT_TRUE(submitted);
    ASSERT_TRUE(h().log.wait_for([&](const std::vector<BatchEvent>& events) {
        return std::count_if(events.begin(), events.end(), [&](const BatchEvent& e) {
                   return e.batch == submitted->id && e.kind == BatchEventKind::JobFinished;
               }) >= 3;
    }));
    ASSERT_TRUE(h().batches().pause(submitted->id));
    ASSERT_TRUE(ts::eventually([&] { return h().record(submitted->id).counts.leased() == 0; }));
    auto status = h().batches().status(submitted->id);
    ASSERT_TRUE(status);
    if (status->record.counts.pending() > 0) {
        EXPECT_EQ(status->record.state, catalog::BatchState::Paused);
        EXPECT_FALSE(status->eta_seconds.has_value()) << "a paused batch is not about to finish";
    }
    ASSERT_TRUE(h().batches().resume(submitted->id));
    ASSERT_TRUE(h().log.wait_for_kind(submitted->id, BatchEventKind::Finished));
    EXPECT_FALSE(h().batches().status(submitted->id)->eta_seconds.has_value());
}

TEST_F(BatchServiceTest, PlansShowOutputsToWriteFirstAndTrustSizesUntilSubmission) {
    const std::vector<ItemId> ids = h().import(ts::write_dngs(dir_, L"in", 3));
    h().run(h().request({ids[0], ids[1]}, out_));
    // Another program changes one earlier output without changing its size.
    const std::filesystem::path changed = out_ / L"photo1_Lucent_0.jpg";
    {
        std::fstream file(changed, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(file.is_open());
        file.seekp(200);
        file.put('\x5A');
    }
    auto plan = h().batches().plan(h().request(ids, out_));
    ASSERT_TRUE(plan) << plan.error().describe();
    EXPECT_EQ(plan->outputs, 1u);
    EXPECT_EQ(plan->already_exported, 2u) << "a plan does not hash earlier outputs";
    ASSERT_EQ(plan->examples.size(), 3u);
    EXPECT_FALSE(plan->examples[0].already_exported) << "outputs that will be written come first";
    EXPECT_EQ(plan->examples[0].destination.filename(), L"photo2_Lucent_0.jpg");
    EXPECT_TRUE(plan->examples[1].already_exported);
    EXPECT_TRUE(plan->examples[2].already_exported);
    auto submitted = h().batches().submit(h().request(ids, out_));
    ASSERT_TRUE(submitted) << submitted.error().describe();
    EXPECT_EQ(submitted->skipped_existing, 1u) << "submission verifies byte for byte";
    ASSERT_TRUE(h().log.wait_for_kind(submitted->id, BatchEventKind::Finished));
    EXPECT_EQ(h().record(submitted->id).counts.complete, 2u);
}

TEST_F(BatchServiceTest, StatusCarriesWhatTheInterfaceShows) {
    const std::vector<ItemId> ids = h().import(ts::write_dngs(dir_, L"in", 2));
    BatchRequest request = h().request({ids[0], ItemId{999'999}}, out_);
    const catalog::BatchId first = h().run(request);
    auto status = h().batches().status(first);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->excluded, 1u);
    EXPECT_FALSE(status->single_export);
    EXPECT_EQ(status->review_recommended, 0u);
    EXPECT_EQ(status->retryable, 0u);
    ASSERT_TRUE(status->output);
    EXPECT_EQ(*status->output, out_ / L"photo0_Lucent_0.jpg");
    EXPECT_FALSE(status->outcome_error);
    EXPECT_FALSE(status->eta_seconds);
    // The same photo again under a new name: its receipt recommends a review.
    BatchRequest again = h().request({ids[0]}, out_);
    again.existing_outputs = ExistingOutputs::ExportAgain;
    again.single_export = true;
    const catalog::BatchId second = h().run(again);
    status = h().batches().status(second);
    ASSERT_TRUE(status);
    EXPECT_TRUE(status->single_export);
    EXPECT_EQ(status->review_recommended, 1u);
    ASSERT_TRUE(status->output);
    EXPECT_EQ(status->output->filename(), L"photo0_Lucent_0_2.jpg");
    // Skipped as already exported: the batch of one names the existing file.
    const catalog::BatchId third = h().run(h().request({ids[0]}, out_));
    status = h().batches().status(third);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->record.counts.skipped, 1u);
    ASSERT_TRUE(status->output);
    EXPECT_EQ(status->output->filename(), L"photo0_Lucent_0.jpg");
    ASSERT_TRUE(status->outcome_error);
    EXPECT_EQ(status->outcome_error->where, "app.batch.existing");
    // Paging and the unfinished batches.
    auto newest = h().batches().list(1);
    ASSERT_TRUE(newest && newest->size() == 1u);
    EXPECT_EQ(newest->front().record.id, third);
    auto older = h().batches().list(10, third);
    ASSERT_TRUE(older);
    ASSERT_EQ(older->size(), 2u);
    EXPECT_EQ(older->front().record.id, second);
    BatchRequest draft = h().request({ids[1]}, out_);
    draft.start = false;
    auto drafted = h().batches().submit(draft);
    ASSERT_TRUE(drafted);
    auto unfinished = h().batches().list_unfinished();
    ASSERT_TRUE(unfinished);
    ASSERT_EQ(unfinished->size(), 1u);
    EXPECT_EQ(unfinished->front().record.id, drafted->id);
}

TEST_F(BatchServiceTest, ASkippedPhotoNamesTheOutputInItsOwnFolder) {
    const std::vector<ItemId> ids = h().import(ts::write_dngs(dir_, L"in", 1));
    const std::filesystem::path a = out_ / L"A";
    const std::filesystem::path b = out_ / L"B";
    h().run(h().request(ids, a));
    h().run(h().request(ids, b));
    const catalog::BatchId id = h().run(h().request(ids, a));
    auto report = h().batches().report(id);
    ASSERT_TRUE(report);
    ASSERT_EQ(report->entries.size(), 1u);
    EXPECT_EQ(report->entries[0].outcome, ReportOutcome::Skipped);
    ASSERT_TRUE(report->entries[0].output);
    EXPECT_EQ(*report->entries[0].output, a / L"photo0_Lucent_0.jpg");
    ASSERT_TRUE(report->entries[0].sha256);
    EXPECT_EQ(*report->entries[0].sha256, ts::file_sha256(a / L"photo0_Lucent_0.jpg"));
}

TEST_F(BatchServiceTest, ACancellationKeepsItsReason) {
    const std::vector<ItemId> ids = h().import(ts::write_dngs(dir_, L"in", 1));
    BatchRequest request = h().request(ids, out_);
    request.start = false;
    auto submitted = h().batches().submit(request);
    ASSERT_TRUE(submitted);
    const Error full{ErrorCode::DiskFull, "the destination is full", "app.batch.destination", 112};
    ASSERT_TRUE(h().batches().cancel(submitted->id, full));
    auto status = h().batches().status(submitted->id);
    ASSERT_TRUE(status);
    ASSERT_TRUE(status->outcome_error);
    EXPECT_EQ(status->outcome_error->code, ErrorCode::DiskFull);
    auto report = h().batches().report(submitted->id);
    ASSERT_TRUE(report);
    ASSERT_EQ(report->entries.size(), 1u);
    EXPECT_EQ(report->entries[0].outcome, ReportOutcome::Cancelled);
    EXPECT_EQ(report->entries[0].reason, "The destination ran out of space.");
}

}  // namespace
}  // namespace lucent::app
