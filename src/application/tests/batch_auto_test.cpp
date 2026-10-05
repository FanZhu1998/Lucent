// Automatic batches with real lucent-worker processes over synthetic DNGs (ADR 0013): Auto analyses
// each photo in its worker, freezes the effective automatic revision (export identity, output name
// and receipt bound to it) and exports it; AnalyzeOnly writes the revisions and a report only; a
// failed full-resolution check keeps the earlier attempt (forced by the worker's fail-final-check
// hook); locks are reported; and the related-shot pre-pass is durable: Lucent closing in its middle
// neither loses nor repeats an analysis, and no photo is frozen before every photo is analysed. An
// anchor keeps its own settings; a photo edited during the batch keeps the edit (the result is a
// branch); nothing is written after a cancellation; an analysis this build cannot read is left out
// of the related-shot plan.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <lucent/application/auto_develop.hpp>
#include <lucent/application/batch_service.hpp>
#include <lucent/application/recipe.hpp>
#include <lucent/application/recipe_store.hpp>
#include <lucent/catalog/catalog.hpp>

#include "batch_test_support.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;
namespace rts = raw::test_support;
using namespace std::chrono_literals;
using engine::DevelopControl;

// Dark synthetic photos with a noise model, large enough for every metric (auto_service_test.cpp);
// sizes differ so their contents differ.
std::vector<std::filesystem::path> dark_dngs(const ts::ScratchDir& dir, const std::filesystem::path& folder,
                                             std::size_t count) {
    std::vector<std::filesystem::path> files;
    for (std::size_t i = 0; i < count; ++i) {
        rts::SyntheticDngSpec spec;
        spec.width = 256 + 4 * static_cast<std::uint32_t>(i);
        spec.height = 192;
        spec.white_level = 65535;
        spec.raw_noise_profile = {2e-5, 2e-7, 2e-5, 2e-7, 2e-5, 2e-7};
        files.push_back(ts::write_dng(dir, folder / (L"dark" + std::to_wstring(i) + L".dng"), spec));
    }
    return files;
}

class BatchAutoTest : public ::testing::Test {
protected:
    void make(BatchServiceOptions options = ts::fast_batch_options()) {
        harness_ = std::make_unique<ts::BatchHarness>(dir_.path() / L"lucent", std::move(options));
        out_ = dir_.path() / L"out";
    }

    ts::BatchHarness& h() { return *harness_; }

    std::optional<catalog::RecipeRevision> revision(catalog::RevisionId id) {
        auto found = h().catalog().query([&](catalog::ReadTransaction& tx) { return tx.find_revision(id); });
        EXPECT_TRUE(found);
        return found ? *found : std::nullopt;
    }

    std::optional<catalog::AutoAnalysisRecord> record(catalog::RevisionId id) {
        auto found = h().catalog().query([&](catalog::ReadTransaction& tx) { return tx.find_auto_analysis(id); });
        EXPECT_TRUE(found);
        return found ? *found : std::nullopt;
    }

    ts::ScratchDir dir_;
    std::unique_ptr<ts::BatchHarness> harness_;
    std::filesystem::path out_;
};

TEST_F(BatchAutoTest, AutoFreezesTheEffectiveRevisionAndExportsIt) {
    make();
    const std::vector<ItemId> ids = h().import(dark_dngs(dir_, dir_.path() / L"in", 3));
    ASSERT_EQ(ids.size(), 3u);
    // The third photo has its exposure locked well below Natural's target.
    engine::DevelopSettings locked;
    locked.exposure_ev = -1.0;
    locked.locks = {DevelopControl::Exposure};
    auto applied = h().recipes().apply(ids[2], locked);
    ASSERT_TRUE(applied);
    const catalog::RevisionId user_revision = *applied->revision;

    BatchRequest request = h().request(ids, out_);
    request.automatic = AutomaticSettings::Auto;
    auto plan = h().batches().plan(request);
    ASSERT_TRUE(plan) << plan.error().describe();
    EXPECT_NE(std::find(plan->notices.begin(), plan->notices.end(), PlanNotice::AutomaticSettings), plan->notices.end());
    const catalog::BatchId id = h().run(request);
    ASSERT_TRUE(id.valid());
    const catalog::BatchRecord batch = h().record(id);
    EXPECT_EQ(batch.state, catalog::BatchState::Completed);
    EXPECT_EQ(batch.automatic, catalog::AutoMode::Auto);
    EXPECT_EQ(batch.counts.complete, 3u);

    for (const catalog::JobRecord& job : h().jobs(id)) {
        EXPECT_EQ(job.stage, catalog::JobStage::Complete);
        EXPECT_EQ(job.auto_state, catalog::AutoJobState::Frozen);
        ASSERT_TRUE(job.revision);
        ASSERT_TRUE(job.analysis_json);
        const auto effective = revision(*job.revision);
        ASSERT_TRUE(effective);
        EXPECT_EQ(effective->info.origin, catalog::RevisionOrigin::Auto);
        EXPECT_EQ(effective->info.settings_sha256, job.settings_sha256) << "the identity binds the effective revision";
        const ItemId item{static_cast<std::uint64_t>(job.item->value)};
        // The effective revision is the item's current one (nobody edited during the batch).
        EXPECT_EQ(h().recipes().state(item)->revision, job.revision);
        // Its analysis record names the job.
        const auto analysis = record(*job.revision);
        ASSERT_TRUE(analysis);
        EXPECT_EQ(analysis->job, job.id);
        EXPECT_EQ(analysis->final_check, catalog::AutoCheckOutcome::Passed);
        // The receipt: the job's identity, the effective revision, the name's revision number.
        auto receipt = h().catalog().query([&](catalog::ReadTransaction& tx) { return tx.find_receipt(job.id); });
        ASSERT_TRUE(receipt && *receipt);
        EXPECT_EQ((*receipt)->identity_sha256, job.identity_sha256);
        auto details = read_receipt_details((*receipt)->settings_json);
        ASSERT_TRUE(details);
        EXPECT_EQ(details->revision, job.revision);
        EXPECT_EQ(details->revision_number, effective->info.revision_number);
        EXPECT_EQ(details->recipe_sha256, effective->info.settings_sha256);
        EXPECT_NE((*receipt)->final_path.filename().wstring().find(
                      L"_Lucent_" + std::to_wstring(effective->info.revision_number)),
                  std::wstring::npos)
            << (*receipt)->final_path.string();
        EXPECT_EQ(ts::file_sha256((*receipt)->final_path), (*receipt)->output_sha256);
        if (item == ids[2]) {
            EXPECT_EQ(effective->info.parent, user_revision) << "derived from the frozen recipe";
            auto settings = read_recipe(effective->settings_json);
            ASSERT_TRUE(settings);
            EXPECT_DOUBLE_EQ(settings->exposure_ev, -1.0) << "the locked exposure is kept";
            EXPECT_FALSE(details->review.empty()) << "the locks' effect is a review note";
        } else {
            auto settings = read_recipe(effective->settings_json);
            ASSERT_TRUE(settings);
            EXPECT_GT(settings->exposure_ev, 0.0);
        }
    }

    auto report = h().batches().report(id);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->exported, 3u);
    std::size_t prevented = 0;
    for (const ReportEntry& entry : report->entries) {
        ASSERT_TRUE(entry.automatic);
        EXPECT_EQ(entry.automatic->state, catalog::AutoJobState::Frozen);
        EXPECT_GT(entry.automatic->revision_number, 0u);
        EXPECT_EQ(entry.automatic->attempts, 1u);
        if (entry.automatic->locks_prevented_targets) {
            ++prevented;
            ASSERT_FALSE(entry.automatic->locks.empty());
            EXPECT_NE(entry.automatic->locks[0].find("Exposure is locked"), std::string::npos);
            EXPECT_FALSE(entry.review.empty());
        }
    }
    EXPECT_EQ(prevented, 1u);
    EXPECT_GE(report->review_recommended, 1u);
    auto json = report_json(*report);
    ASSERT_TRUE(json);
    const auto parsed = nlohmann::json::parse(*json);
    EXPECT_TRUE(parsed["entries"][0]["automatic"].is_object());
    EXPECT_TRUE(ts::leftovers(out_).empty());
}

TEST_F(BatchAutoTest, AnalyzeOnlyWritesRevisionsAndAReportButNoOutput) {
    make();
    const std::vector<ItemId> ids = h().import(dark_dngs(dir_, dir_.path() / L"in", 2));
    BatchRequest request = h().request(ids, out_);
    request.automatic = AutomaticSettings::AnalyzeOnly;
    auto plan = h().batches().plan(request);
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan->outputs, 0u);
    EXPECT_NE(std::find(plan->notices.begin(), plan->notices.end(), PlanNotice::AnalyzeOnly), plan->notices.end());
    const catalog::BatchId id = h().run(request);
    ASSERT_TRUE(id.valid());
    EXPECT_EQ(h().record(id).counts.complete, 2u);
    EXPECT_TRUE(h().receipts(id).empty());
    for (const catalog::JobRecord& job : h().jobs(id)) {
        EXPECT_EQ(job.auto_state, catalog::AutoJobState::Frozen);
        ASSERT_TRUE(job.revision);
        EXPECT_TRUE(record(*job.revision).has_value());
        EXPECT_EQ(h().recipes().state(ItemId{static_cast<std::uint64_t>(job.item->value)})->revision, job.revision);
    }
    // Only the report is in the folder.
    for (const std::wstring& name : ts::folder_entries(out_)) {
        EXPECT_TRUE(name.starts_with(L"Lucent-batch-")) << std::filesystem::path(name).string();
    }
    auto report = h().batches().report(id);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->analysed, 2u);
    EXPECT_EQ(report->exported, 0u);
    for (const ReportEntry& entry : report->entries) {
        EXPECT_EQ(entry.outcome, ReportOutcome::Analysed);
        EXPECT_NE(entry.reason.find("revision"), std::string::npos) << entry.reason;
        EXPECT_FALSE(entry.output.has_value());
    }
    auto status = h().batches().status(id);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->automatic, catalog::AutoMode::AnalyzeOnly);
}

TEST_F(BatchAutoTest, AFailedFinalCheckFreezesTheFallbackAndKeepsTheAttempt) {
    BatchServiceOptions options = ts::fast_batch_options();
    options.max_workers = 1;
    options.test.worker_hooks_by_start = {{"fail-final-check:1"}};
    make(options);
    const std::vector<ItemId> ids = h().import(dark_dngs(dir_, dir_.path() / L"in", 1));
    BatchRequest request = h().request(ids, out_);
    request.automatic = AutomaticSettings::Auto;
    const catalog::BatchId id = h().run(request);
    ASSERT_TRUE(id.valid());
    const std::vector<catalog::JobRecord> jobs = h().jobs(id);
    ASSERT_EQ(jobs.size(), 1u);
    ASSERT_EQ(jobs[0].stage, catalog::JobStage::Complete) << (jobs[0].last_error ? jobs[0].last_error->message : "");
    auto records = h().catalog().query([&](catalog::ReadTransaction& tx) { return tx.list_auto_analyses(*jobs[0].item); });
    ASSERT_TRUE(records);
    ASSERT_EQ(records->size(), 2u);
    EXPECT_EQ((*records)[0].attempt, 0u);
    EXPECT_EQ((*records)[0].final_check, catalog::AutoCheckOutcome::Failed);
    EXPECT_EQ((*records)[1].attempt, 1u);
    EXPECT_EQ((*records)[1].final_check, catalog::AutoCheckOutcome::Passed);
    EXPECT_EQ((*records)[1].revision, jobs[0].revision) << "the export is the effective (fallback) revision";
    const auto failed = revision((*records)[0].revision);
    ASSERT_TRUE(failed);
    EXPECT_NE(failed->info.note.find("check=failed"), std::string::npos);
    auto report = h().batches().report(id);
    ASSERT_TRUE(report);
    ASSERT_EQ(report->entries.size(), 1u);
    const ReportEntry& entry = report->entries[0];
    ASSERT_TRUE(entry.automatic);
    EXPECT_EQ(entry.automatic->attempt, 1u);
    EXPECT_EQ(entry.automatic->attempts, 2u);
    EXPECT_TRUE(entry.automatic->review);
    const bool fallback_noted = std::any_of(entry.review.begin(), entry.review.end(), [](const std::string& note) {
        return note.find("more conservative") != std::string::npos;
    });
    EXPECT_TRUE(fallback_noted) << "the report recommends a review of the fallback";
}

TEST_F(BatchAutoTest, TheRelatedShotPrePassSurvivesARestart) {
    struct Point {
        std::string name;
        std::int64_t job = 0;
    };
    auto points = std::make_shared<std::vector<Point>>();
    auto points_mutex = std::make_shared<std::mutex>();
    auto analysed = std::make_shared<std::atomic<int>>(0);
    BatchServiceOptions options = ts::fast_batch_options();
    options.max_workers = 1;
    // The first worker process delays each analysis, so Lucent closes in the middle of the pre-pass.
    options.test.worker_hooks_by_start = {{"render-delay:1500"}};
    options.test.on_point = [points, points_mutex, analysed](std::string_view name, catalog::JobId job) {
        if (name != "analysed" && name != "frozen") {
            return;
        }
        std::lock_guard lock(*points_mutex);
        points->push_back(Point{std::string(name), job.value});
        if (name == "analysed") {
            ++*analysed;
        }
    };
    make(options);
    const std::vector<ItemId> ids = h().import(dark_dngs(dir_, dir_.path() / L"in", 4));
    ASSERT_EQ(ids.size(), 4u);
    BatchRequest request = h().request(ids, out_);
    request.automatic = AutomaticSettings::Auto;
    request.keep_consistent = true;
    for (BatchItem& item : request.items) {
        item.group = "series";  // the user's group: related whatever their capture times
    }
    auto submitted = h().batches().submit(request);
    ASSERT_TRUE(submitted) << submitted.error().describe();
    const catalog::BatchId id = submitted->id;
    ASSERT_TRUE(ts::eventually([&] { return analysed->load() >= 1; }, 120s));
    h().close();  // Lucent stops in the middle of the pre-pass

    std::map<std::int64_t, std::string> before;
    std::size_t pending = 0;
    {
        auto catalog = ts::open_catalog(dir_.path() / L"lucent" / L"data" / L"lucent.catalog");
        ASSERT_NE(catalog, nullptr);
        catalog::JobQuery query;
        query.batch = id;
        auto jobs = catalog->query([&](catalog::ReadTransaction& tx) { return tx.list_jobs(query); });
        ASSERT_TRUE(jobs);
        for (const catalog::JobRecord& job : *jobs) {
            EXPECT_NE(job.auto_state, catalog::AutoJobState::Frozen) << "nothing is frozen during the pre-pass";
            if (job.analysis_json) {
                before.emplace(job.id.value, *job.analysis_json);
            } else {
                ++pending;
            }
        }
    }
    ASSERT_GE(before.size(), 1u);
    EXPECT_GE(pending, 1u) << "Lucent closed in the middle of the pre-pass";

    h().options().test.worker_hooks_by_start.clear();  // no delay after the restart
    h().open();
    ASSERT_TRUE(h().log.wait_for_kind(id, BatchEventKind::Finished, 300s));
    const catalog::BatchRecord batch = h().record(id);
    EXPECT_EQ(batch.state, catalog::BatchState::Completed);
    EXPECT_EQ(batch.counts.complete, 4u);
    for (const catalog::JobRecord& job : h().jobs(id)) {
        EXPECT_EQ(job.auto_state, catalog::AutoJobState::Frozen);
        ASSERT_TRUE(job.analysis_json);
        if (const auto kept = before.find(job.id.value); kept != before.end()) {
            EXPECT_EQ(*job.analysis_json, kept->second) << "an analysis recorded before the restart is never repeated";
        }
        ASSERT_TRUE(job.revision);
        const auto analysis = record(*job.revision);
        ASSERT_TRUE(analysis);
        EXPECT_EQ(analysis->consistency_version, analysis::kConsistencyVersion)
            << "every member was regularised towards its group";
        const auto doc = nlohmann::json::parse(analysis->record_json);
        EXPECT_EQ(doc["consistency"]["target"]["group_id"].get<std::string>(), "user:series");
    }
    // Every photo was analysed before any was frozen (across the restart: the first session's
    // points come first).
    {
        std::lock_guard lock(*points_mutex);
        std::size_t first_frozen = points->size();
        std::size_t last_analysed = 0;
        for (std::size_t i = 0; i < points->size(); ++i) {
            if ((*points)[i].name == "frozen") {
                first_frozen = std::min(first_frozen, i);
            } else {
                last_analysed = i;
            }
        }
        EXPECT_EQ(analysed->load(), 4) << "each photo's pre-pass analysis ran exactly once";
        EXPECT_LT(last_analysed, first_frozen);
    }
    auto report = h().batches().report(id);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->exported, 4u);
    for (const ReportEntry& entry : report->entries) {
        ASSERT_TRUE(entry.automatic);
        EXPECT_EQ(entry.automatic->group, std::optional<std::string>("user:series"));
    }
}

// A caller-marked anchor keeps its own (hand-tuned) settings: it is exported with its submission
// recipe, recorded as the anchor of its group, and its look is the target the other members are
// regularised towards.
TEST_F(BatchAutoTest, AnAnchorKeepsItsSettingsAndSetsItsGroupsLook) {
    BatchServiceOptions options = ts::fast_batch_options();
    options.max_workers = 1;
    make(options);
    const std::vector<ItemId> ids = h().import(dark_dngs(dir_, dir_.path() / L"in", 3));
    ASSERT_EQ(ids.size(), 3u);
    engine::DevelopSettings look;
    look.exposure_ev = 0.6;
    look.contrast = 0.1;
    auto tuned = h().recipes().apply(ids[1], look);
    ASSERT_TRUE(tuned);
    const catalog::RevisionId tuned_revision = *tuned->revision;
    const core::Digest256 tuned_sha = tuned->committed_sha256;

    BatchRequest request = h().request(ids, out_);
    request.automatic = AutomaticSettings::Auto;
    request.keep_consistent = true;
    for (BatchItem& item : request.items) {
        item.group = "series";
        item.anchor = item.item == ids[1];
    }
    const catalog::BatchId id = h().run(request);
    ASSERT_TRUE(id.valid());
    EXPECT_EQ(h().record(id).counts.complete, 3u);
    std::size_t anchored_members = 0;
    for (const catalog::JobRecord& job : h().jobs(id)) {
        ASSERT_EQ(job.auto_state, catalog::AutoJobState::Frozen);
        ASSERT_TRUE(job.revision);
        const auto analysis = record(*job.revision);
        ASSERT_TRUE(analysis);
        const auto doc = nlohmann::json::parse(analysis->record_json);
        const ItemId item{static_cast<std::uint64_t>(job.item->value)};
        if (item == ids[1]) {
            EXPECT_EQ(job.settings_sha256, tuned_sha) << "the anchor is exported with its own recipe";
            EXPECT_EQ(doc["attempt"]["kind"].get<std::string>(), "anchor");
            EXPECT_EQ(analysis->final_check, catalog::AutoCheckOutcome::NotRun);
            EXPECT_FALSE(analysis->review);
            const auto effective = revision(*job.revision);
            ASSERT_TRUE(effective);
            EXPECT_EQ(effective->info.parent, tuned_revision);
            EXPECT_EQ(read_recipe(effective->settings_json).value_or(engine::DevelopSettings{}), look);
            EXPECT_EQ(doc["consistency"]["target"]["group_id"].get<std::string>(), "user:series");
            auto receipt = h().catalog().query([&](catalog::ReadTransaction& tx) { return tx.find_receipt(job.id); });
            ASSERT_TRUE(receipt && *receipt);
            auto details = read_receipt_details((*receipt)->settings_json);
            ASSERT_TRUE(details);
            EXPECT_EQ(details->recipe_sha256, tuned_sha);
            auto all = h().catalog().query([&](catalog::ReadTransaction& tx) { return tx.list_auto_analyses(*job.item); });
            ASSERT_TRUE(all);
            EXPECT_EQ(all->size(), 1u) << "one record: the anchor is neither optimised nor checked";
        } else {
            EXPECT_EQ(doc["consistency"]["target"]["group_id"].get<std::string>(), "user:series");
            if (doc["consistency"]["target"]["anchored"].get<bool>()) {
                ++anchored_members;
            }
        }
    }
    EXPECT_EQ(anchored_members, 2u) << "the members are regularised towards the anchor's look";
    auto report = h().batches().report(id);
    ASSERT_TRUE(report);
    for (const ReportEntry& entry : report->entries) {
        ASSERT_TRUE(entry.automatic);
        EXPECT_EQ(entry.automatic->anchor, entry.item == std::optional<ItemId>(ids[1]));
        EXPECT_EQ(entry.automatic->group, std::optional<std::string>("user:series"));
    }
}

// A photo edited while its job runs (a slider still held when the job freezes) keeps the edit in
// progress: the automatic revisions become a branch, the gesture commits on its own base, and the
// outcome is reported (also by Analyse only, from the revision's durable note).
TEST_F(BatchAutoTest, APhotoEditedDuringTheBatchKeepsItsEditAndTheResultIsABranch) {
    make();
    const std::vector<ItemId> ids = h().import(dark_dngs(dir_, dir_.path() / L"in", 2));
    engine::DevelopSettings gesture;
    gesture.contrast = 0.3;
    ASSERT_TRUE(h().recipes().set_transient(ids[0], gesture));
    BatchRequest request = h().request(ids, out_);
    request.automatic = AutomaticSettings::Auto;
    const catalog::BatchId id = h().run(request);
    ASSERT_TRUE(id.valid());
    EXPECT_EQ(h().record(id).counts.complete, 2u);
    for (const catalog::JobRecord& job : h().jobs(id)) {
        const ItemId item{static_cast<std::uint64_t>(job.item->value)};
        const auto effective = revision(*job.revision);
        ASSERT_TRUE(effective);
        auto receipt = h().catalog().query([&](catalog::ReadTransaction& tx) { return tx.find_receipt(job.id); });
        ASSERT_TRUE(receipt && *receipt);
        auto details = read_receipt_details((*receipt)->settings_json);
        ASSERT_TRUE(details);
        const bool noted = std::any_of(details->review.begin(), details->review.end(), [](const std::string& note) {
            return note.find("edited while the batch ran") != std::string::npos;
        });
        if (item == ids[0]) {
            EXPECT_TRUE(is_automatic_branch_note(effective->info.note)) << effective->info.note;
            EXPECT_TRUE(noted);
            const RecipeState state = *h().recipes().state(item);
            EXPECT_FALSE(state.revision.has_value()) << "the photo still shows its own recipe";
            EXPECT_TRUE(state.transient) << "the gesture in progress is kept";
        } else {
            EXPECT_FALSE(is_automatic_branch_note(effective->info.note));
            EXPECT_FALSE(noted);
            EXPECT_EQ(h().recipes().state(item)->revision, job.revision);
        }
    }
    auto committed = h().recipes().commit(ids[0]);
    ASSERT_TRUE(committed) << committed.error().describe();
    EXPECT_EQ(committed->committed, gesture) << "releasing the slider applies the edit; nothing is reverted";

    // Analyse only reports the branch from the revision's note.
    engine::DevelopSettings second = gesture;
    second.saturation = 0.2;
    ASSERT_TRUE(h().recipes().set_transient(ids[0], second));
    BatchRequest analyse = h().request({ids[0]}, out_);
    analyse.automatic = AutomaticSettings::AnalyzeOnly;
    const catalog::BatchId analysed = h().run(analyse);
    ASSERT_TRUE(analysed.valid());
    auto report = h().batches().report(analysed);
    ASSERT_TRUE(report);
    ASSERT_EQ(report->entries.size(), 1u);
    const ReportEntry& entry = report->entries[0];
    ASSERT_TRUE(entry.automatic);
    EXPECT_TRUE(entry.automatic->branch);
    EXPECT_NE(entry.reason.find("kept in the history"), std::string::npos) << entry.reason;
    EXPECT_FALSE(entry.review.empty());
    ASSERT_TRUE(h().recipes().discard_transient(ids[0]));
}

// Cancelling while an analysis is being answered: nothing of it is written (no recorded analysis,
// no revision, no freeze).
TEST_F(BatchAutoTest, NothingIsWrittenAfterACancellation) {
    BatchServiceOptions options = ts::fast_batch_options();
    options.max_workers = 1;
    auto batch = std::make_shared<std::atomic<std::int64_t>>(0);
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    options.test.on_point = [this, batch, cancelled](std::string_view name, catalog::JobId) {
        if (name == "answered" && batch->load() != 0 && !cancelled->exchange(true)) {
            EXPECT_TRUE(h().batches().cancel(catalog::BatchId{batch->load()}));
        }
    };
    make(options);
    const std::vector<ItemId> ids = h().import(dark_dngs(dir_, dir_.path() / L"in", 2));
    BatchRequest request = h().request(ids, out_);
    request.automatic = AutomaticSettings::Auto;
    auto submitted = h().batches().submit(request);
    ASSERT_TRUE(submitted) << submitted.error().describe();
    batch->store(submitted->id.value);
    ASSERT_TRUE(h().log.wait_for_kind(submitted->id, BatchEventKind::Finished, 300s));
    ASSERT_TRUE(cancelled->load()) << "the hook cancelled the batch while an answer was in";
    EXPECT_EQ(h().record(submitted->id).state, catalog::BatchState::Cancelled);
    EXPECT_TRUE(h().receipts(submitted->id).empty());
    for (const catalog::JobRecord& job : h().jobs(submitted->id)) {
        EXPECT_EQ(job.auto_state, catalog::AutoJobState::Pending);
        EXPECT_FALSE(job.analysis_json.has_value());
        auto all = h().catalog().query([&](catalog::ReadTransaction& tx) { return tx.list_auto_analyses(*job.item); });
        ASSERT_TRUE(all);
        EXPECT_TRUE(all->empty());
        EXPECT_FALSE(h().recipes().state(ItemId{static_cast<std::uint64_t>(job.item->value)})->revision.has_value());
    }
}

// An analysis recorded in a form this build cannot read (written by another version, before an
// update) leaves its photo out of the related-shot plan; the batch still completes.
TEST_F(BatchAutoTest, AnAnalysisThisBuildCannotReadIsLeftOutOfThePlan) {
    BatchServiceOptions options = ts::fast_batch_options();
    options.max_workers = 1;
    options.test.worker_hooks_by_start = {{"render-delay:2000"}};
    make(options);
    const std::vector<std::filesystem::path> files = dark_dngs(dir_, dir_.path() / L"in", 3);
    const std::vector<ItemId> ids = h().import(files);
    ASSERT_EQ(ids.size(), 3u);
    BatchRequest request = h().request(ids, out_);
    request.automatic = AutomaticSettings::Auto;
    request.keep_consistent = true;
    for (BatchItem& item : request.items) {
        item.group = "series";
    }
    auto submitted = h().batches().submit(request);
    ASSERT_TRUE(submitted) << submitted.error().describe();
    const catalog::BatchId id = submitted->id;
    h().close();  // before any analysis is recorded

    // Another version of Lucent recorded one photo's pre-pass analysis.
    std::int64_t doctored = 0;
    std::string document;
    {
        auto catalog = ts::open_catalog(dir_.path() / L"lucent" / L"data" / L"lucent.catalog");
        ASSERT_NE(catalog, nullptr);
        auto claimed = catalog->transact([&](catalog::WriteTransaction& tx) {
            catalog::ClaimRequest claim;
            claim.owner = "another-lucent";
            claim.now = catalog::current_time();
            claim.batch = id;
            claim.auto_states = {catalog::AutoJobState::Pending};
            return tx.claim_next_job(claim);
        });
        ASSERT_TRUE(claimed && *claimed) << "a pending job remains";
        const catalog::ClaimedJob& job = **claimed;
        doctored = job.job.id.value;
        std::filesystem::path file;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (static_cast<std::int64_t>(ids[i].value) == job.job.item->value) {
                file = files[i];
            }
        }
        AutoRunRequest pre_pass;
        pre_pass.final_check = false;
        auto run = run_automatic(*ts::default_registry(), file, 0, engine::DevelopSettings{}, pre_pass);
        ASSERT_TRUE(run) << run.error().describe();
        auto json = nlohmann::json::parse(*write_auto_run(*run));
        json["version"] = kAutoDocumentVersion + 1;
        document = json.dump();
        ASSERT_TRUE(catalog->write([&](catalog::WriteTransaction& tx) {
            return tx.record_job_analysis(job.lease, document, catalog::current_time());
        }));
        ASSERT_TRUE(catalog->transact(
            [&](catalog::WriteTransaction& tx) { return tx.release_job(job.lease, catalog::current_time()); }));
    }

    h().options().test.worker_hooks_by_start.clear();
    h().open();
    ASSERT_TRUE(h().log.wait_for_kind(id, BatchEventKind::Finished, 300s));
    EXPECT_EQ(h().record(id).state, catalog::BatchState::Completed);
    EXPECT_EQ(h().record(id).counts.complete, 3u);
    for (const catalog::JobRecord& job : h().jobs(id)) {
        ASSERT_EQ(job.auto_state, catalog::AutoJobState::Frozen);
        const auto analysis = record(*job.revision);
        ASSERT_TRUE(analysis);
        if (job.id.value == doctored) {
            EXPECT_EQ(job.analysis_json, std::optional<std::string>(document)) << "kept as recorded";
            EXPECT_FALSE(analysis->consistency_version.has_value()) << "developed on its own";
        } else {
            EXPECT_EQ(analysis->consistency_version, analysis::kConsistencyVersion);
        }
    }
}

TEST_F(BatchAutoTest, KeepConsistentNeedsAutomaticSettings) {
    make();
    const std::vector<ItemId> ids = h().import(dark_dngs(dir_, dir_.path() / L"in", 1));
    BatchRequest request = h().request(ids, out_);
    request.keep_consistent = true;
    auto refused = h().batches().plan(request);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::InvalidArgument);
}

}  // namespace
}  // namespace lucent::app
