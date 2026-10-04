// Automatic settings in the application (auto_develop.hpp, auto_service.hpp; ADR 0013): the
// interactive "Auto" creates automatic revisions with their analysis records, respects and reports
// locks, keeps a failed final-check attempt with its reason (the fallback ladder, forced by the test
// hook), writes nothing when nothing passes or the run is cancelled, keeps its revisions as a branch
// when the photo changed meanwhile, and its documents read back exactly. Items are synthetic DNGs:
// dark (a high white level) so that the optimiser brightens them, and large enough for every metric's
// evidence minimum.

#include <chrono>
#include <filesystem>
#include <format>
#include <span>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <lucent/analysis/auto_settings.hpp>
#include <lucent/application/auto_develop.hpp>
#include <lucent/application/auto_service.hpp>
#include <lucent/application/import.hpp>
#include <lucent/application/recipe.hpp>
#include <lucent/application/recipe_service.hpp>
#include <lucent/application/recipe_store.hpp>
#include <lucent/application/task_executor.hpp>

#include "app_test_support.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;
namespace rts = raw::test_support;
using namespace std::chrono_literals;
using engine::DevelopControl;

// A dark synthetic photo with a sensor noise model: the samples lie below 4000 of a 65535 white
// level, so Natural brightens it; 256 x 192 pixels meet every metric's minimum evidence.
rts::SyntheticDngSpec dark_spec(std::string model = "Synthetic DNG") {
    rts::SyntheticDngSpec spec;
    spec.width = 256;
    spec.height = 192;
    spec.white_level = 65535;
    spec.black_level = 256;
    spec.model = std::move(model);
    spec.raw_noise_profile = {2e-5, 2e-7, 2e-5, 2e-7, 2e-5, 2e-7};
    return spec;
}

class AutoServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry_ = ts::default_registry();
        fixture_ = ts::CatalogFixture::open(dir_.path() / L"data" / L"lucent.catalog");
        ASSERT_NE(fixture_.library, nullptr);
        auto store = create_catalog_recipe_store(fixture_.catalog);
        ASSERT_TRUE(store);
        store_ = *store;
        RecipeServiceOptions options;
        options.registry = registry_;
        recipes_ = std::make_unique<RecipeService>(*fixture_.library, store_, options);
        auto executor = TaskExecutor::create(ExecutorOptions{});
        ASSERT_TRUE(executor);
        executor_ = std::move(*executor);
        auto report = import_sources(*fixture_.library, *registry_,
                                     ImportRequest{.files = {ts::write_dng(dir_, L"dark.dng", dark_spec())}});
        ASSERT_TRUE(report) << report.error().describe();
        ASSERT_EQ(report->added.size(), 1u);
        item_ = report->added[0];
    }

    void TearDown() override {
        service_.reset();
        executor_.reset();
        recipes_.reset();
        store_.reset();
        fixture_.close();
    }

    AutoService& service(std::uint32_t fail_final_checks = 0) {
        AutoServiceOptions options;
        options.registry = registry_;
        options.fail_final_checks = fail_final_checks;
        auto created = AutoService::create(*fixture_.library, *recipes_, *executor_, options);
        EXPECT_TRUE(created);
        service_ = std::move(*created);
        return *service_;
    }

    AutoTaskResult run(AutoService& service) {
        auto ticket = service.run(item_);
        EXPECT_TRUE(ticket) << (ticket ? std::string() : ticket.error().describe());
        if (!ticket) {
            return {};
        }
        auto result = service.wait(*ticket, 120s);
        EXPECT_TRUE(result.has_value()) << "the automatic settings did not finish";
        return result ? *result : AutoTaskResult{};
    }

    RecipeState state() const {
        auto s = recipes_->state(item_);
        EXPECT_TRUE(s);
        return s ? *s : RecipeState{};
    }

    std::vector<catalog::AutoAnalysisRecord> records() const {
        auto listed = recipes_->automatic_analyses(item_);
        EXPECT_TRUE(listed) << (listed ? std::string() : listed.error().describe());
        return listed ? *listed : std::vector<catalog::AutoAnalysisRecord>{};
    }

    ts::ScratchDir dir_;
    std::shared_ptr<const raw::DecoderRegistry> registry_;
    ts::CatalogFixture fixture_;
    std::shared_ptr<RecipeStore> store_;
    std::unique_ptr<RecipeService> recipes_;
    std::unique_ptr<TaskExecutor> executor_;
    std::unique_ptr<AutoService> service_;
    ItemId item_;
};

TEST_F(AutoServiceTest, AutoCreatesAnEditableRevisionWithItsAnalysisRecord) {
    AutoService& auto_service = service();
    std::vector<AutoEvent> events;
    std::mutex mutex;
    Subscription subscription = auto_service.subscribe([&](const AutoEvent& event) {
        std::lock_guard lock(mutex);
        events.push_back(event);
    });
    const AutoTaskResult result = run(auto_service);
    ASSERT_FALSE(result.error) << result.error->describe();
    ASSERT_TRUE(result.run);
    const AutoRun& run = *result.run;
    std::printf("  chosen exposure %+.2f EV, shadows %.3f, confidence %.2f, %zu attempt(s), %zu flag(s)\n",
                run.chosen.exposure_ev, run.chosen.shadows, run.confidence, run.attempts.size(), run.flags.size());
    EXPECT_GT(run.chosen.exposure_ev, 0.0) << "a dark photo is brightened";
    ASSERT_EQ(result.revisions.size(), 1u);
    ASSERT_TRUE(run.effective);
    EXPECT_EQ(*run.effective, 0u);
    EXPECT_EQ(run.attempts[0].check.outcome, catalog::AutoCheckOutcome::Passed);
    EXPECT_FALSE(result.base.has_value());

    // An editable revision of origin Auto, now current, derived from "as shot".
    const catalog::RevisionInfo& info = result.revisions[0];
    EXPECT_EQ(info.origin, catalog::RevisionOrigin::Auto);
    EXPECT_FALSE(info.parent.has_value());
    EXPECT_TRUE(result.made_current);
    const RecipeState after = state();
    EXPECT_EQ(after.revision, info.id);
    EXPECT_EQ(after.committed, run.effective_settings());
    EXPECT_NE(info.note.find("auto=chosen;attempt=0;check=passed;policy=natural/"), std::string::npos) << info.note;
    auto history = recipes_->history(item_);
    ASSERT_TRUE(history);
    ASSERT_EQ(history->entries.size(), 1u);
    EXPECT_FALSE(history->entries[0].changed.empty());

    // Its analysis record: versioned, with the confidence and flags.
    const std::vector<catalog::AutoAnalysisRecord> stored = records();
    ASSERT_EQ(stored.size(), 1u);
    const catalog::AutoAnalysisRecord& record = stored[0];
    EXPECT_EQ(record.revision, info.id);
    EXPECT_FALSE(record.job.has_value());
    EXPECT_EQ(record.policy, analysis::kNaturalPolicyId);
    EXPECT_EQ(record.policy_version, analysis::kNaturalPolicyVersion);
    EXPECT_EQ(record.search_version, analysis::kSearchVersion);
    EXPECT_EQ(record.metric_contract_version, analysis::kMetricContractVersion);
    EXPECT_EQ(record.develop_settings_version, engine::kDevelopSettingsVersion);
    EXPECT_FALSE(record.consistency_version.has_value());
    EXPECT_EQ(record.attempt, 0u);
    EXPECT_EQ(record.final_check, catalog::AutoCheckOutcome::Passed);
    EXPECT_DOUBLE_EQ(record.confidence, run.confidence);
    EXPECT_EQ(record.review, run.review_recommended());
    const auto doc = nlohmann::json::parse(record.record_json);
    EXPECT_EQ(doc.at("schema").get<std::string>(), kAutoRecordSchema);
    EXPECT_EQ(doc.at("attempt").at("check").at("outcome").get<std::string>(), "passed");
    EXPECT_TRUE(doc.at("effective").get<bool>());
    const auto flags = nlohmann::json::parse(record.flags_json);
    ASSERT_TRUE(flags.is_array());
    EXPECT_EQ(flags.size(), run.flags.size());

    // Progress was observed, then completion.
    {
        std::lock_guard lock(mutex);
        ASSERT_FALSE(events.empty());
        EXPECT_EQ(events.back().kind, AutoEvent::Kind::Finished);
        bool checking = false;
        for (const AutoEvent& event : events) {
            checking = checking || event.status.stage == AutoTaskStage::Checking;
        }
        EXPECT_TRUE(checking);
    }

    // Undo returns to the recipe before Auto.
    auto undone = recipes_->undo(item_);
    ASSERT_TRUE(undone);
    EXPECT_FALSE(undone->revision.has_value());
}

TEST_F(AutoServiceTest, LockedControlsAreKeptAndTheirEffectReported) {
    engine::DevelopSettings locked;
    locked.exposure_ev = -1.0;
    locked.locks = {DevelopControl::Exposure};
    auto applied = recipes_->apply(item_, locked);
    ASSERT_TRUE(applied);
    const catalog::RevisionId base = *applied->revision;

    const AutoTaskResult result = run(service());
    ASSERT_FALSE(result.error) << result.error->describe();
    const AutoRun& run = *result.run;
    EXPECT_EQ(result.base, base);
    EXPECT_DOUBLE_EQ(run.effective_settings().exposure_ev, -1.0);
    EXPECT_TRUE(run.effective_settings().locks.contains(DevelopControl::Exposure));
    ASSERT_EQ(run.locks.locked.size(), 1u);
    EXPECT_EQ(run.locks.locked[0], DevelopControl::Exposure);
    // The photo stays far darker than Natural's target: the lock is reported explicitly.
    EXPECT_TRUE(run.locks.prevented_targets);
    ASSERT_FALSE(run.locks.reasons.empty());
    EXPECT_NE(run.locks.reasons[0].find("Exposure is locked"), std::string::npos) << run.locks.reasons[0];
    bool flagged = false;
    for (const AutoFlag& flag : run.flags) {
        if (flag.code == kFlagLocksPreventedTargets) {
            flagged = true;
            EXPECT_TRUE(flag.review);
            EXPECT_EQ(flag.name, "locks_prevented_targets");
        }
    }
    EXPECT_TRUE(flagged);
    EXPECT_TRUE(run.review_recommended());
    // The revision is derived from the locked one and keeps the lock.
    ASSERT_EQ(result.revisions.size(), 1u);
    EXPECT_EQ(result.revisions[0].parent, base);
    EXPECT_TRUE(state().committed.locks.contains(DevelopControl::Exposure));
}

TEST_F(AutoServiceTest, AFailedFinalCheckKeepsTheAttemptAndItsReason) {
    const AutoTaskResult result = run(service(/*fail_final_checks=*/1));
    ASSERT_FALSE(result.error) << result.error->describe();
    const AutoRun& run = *result.run;
    ASSERT_GE(run.attempts.size(), 2u);
    EXPECT_EQ(run.attempts[0].kind, AutoAttemptKind::Chosen);
    EXPECT_EQ(run.attempts[0].check.outcome, catalog::AutoCheckOutcome::Failed);
    EXPECT_TRUE(run.attempts[0].check.forced);
    ASSERT_TRUE(run.effective);
    EXPECT_EQ(*run.effective, 1u);
    const AutoAttempt& effective = run.attempts[1];
    EXPECT_NE(effective.kind, AutoAttemptKind::Chosen);
    EXPECT_EQ(effective.check.outcome, catalog::AutoCheckOutcome::Passed);
    EXPECT_NE(effective.reason.find("attempt 0 failed the full-resolution check"), std::string::npos)
        << effective.reason;
    // Fallbacks are more conservative: never a larger exposure change.
    EXPECT_LE(std::abs(effective.settings.exposure_ev), std::abs(run.chosen.exposure_ev));
    bool flagged = false;
    for (const AutoFlag& flag : run.flags) {
        flagged = flagged || flag.code == kFlagFinalCheckFallback;
    }
    EXPECT_TRUE(flagged);

    // Both attempts are revisions derived from the starting recipe; the effective one is current
    // and the failed one stays in the history with its reason.
    ASSERT_EQ(result.revisions.size(), 2u);
    EXPECT_EQ(result.revisions[0].parent, result.base);
    EXPECT_EQ(result.revisions[1].parent, result.base);
    EXPECT_NE(result.revisions[0].note.find("check=failed"), std::string::npos) << result.revisions[0].note;
    EXPECT_NE(result.revisions[1].note.find("check=passed"), std::string::npos) << result.revisions[1].note;
    EXPECT_EQ(state().revision, result.revisions[1].id);
    EXPECT_EQ(result.effective, result.revisions[1].id);
    const std::vector<catalog::AutoAnalysisRecord> stored = records();
    ASSERT_EQ(stored.size(), 2u);
    EXPECT_EQ(stored[0].attempt, 0u);
    EXPECT_EQ(stored[0].final_check, catalog::AutoCheckOutcome::Failed);
    EXPECT_TRUE(stored[0].review);
    EXPECT_EQ(stored[1].attempt, 1u);
    EXPECT_EQ(stored[1].final_check, catalog::AutoCheckOutcome::Passed);
    EXPECT_LE(stored[1].confidence, analysis::kFallbackConfidence);
    // Undo returns to the recipe before Auto, not to the failed attempt.
    auto undone = recipes_->undo(item_);
    ASSERT_TRUE(undone);
    EXPECT_EQ(undone->revision, result.base);
}

TEST_F(AutoServiceTest, NothingIsWrittenWhenNoAttemptPasses) {
    const AutoTaskResult result = run(service(/*fail_final_checks=*/16));
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->where, "app.auto.check");
    ASSERT_TRUE(result.run);
    EXPECT_TRUE(result.run->check_failed());
    EXPECT_TRUE(result.revisions.empty());
    EXPECT_FALSE(state().revision.has_value());
    EXPECT_TRUE(records().empty());
}

TEST_F(AutoServiceTest, ACancelledRunWritesNothing) {
    // Occupy the only background thread so the run is cancelled before it starts.
    ts::Gate gate;
    auto blocker = executor_->submit(Lane::Background, [&](TaskContext& c) { (void)gate.wait(c.cancel()); });
    ASSERT_TRUE(blocker);
    auto blocker2 = executor_->submit(Lane::Background, [&](TaskContext& c) { (void)gate.wait(c.cancel()); });
    ASSERT_TRUE(blocker2);
    AutoService& auto_service = service();
    auto ticket = auto_service.run(item_);
    ASSERT_TRUE(ticket);
    // A second run of the same item is refused while the first is pending.
    auto busy = auto_service.run(item_);
    ASSERT_FALSE(busy);
    EXPECT_EQ(busy.error().where, "app.auto.busy");
    auto_service.cancel(*ticket);
    gate.open();
    auto result = auto_service.wait(*ticket, 120s);
    ASSERT_TRUE(result);
    ASSERT_TRUE(result->error);
    EXPECT_EQ(result->error->code, ErrorCode::Cancelled);
    EXPECT_FALSE(state().revision.has_value());
    EXPECT_TRUE(records().empty());
}

TEST_F(AutoServiceTest, RevisionsOfAPhotoEditedMeanwhileStayABranch) {
    // The run's revisions as the service would write them, applied after the user edited.
    AutoRunRequest request;
    auto run = run_automatic(*registry_, fixture_.library->find(item_)->path, 0, engine::DevelopSettings{}, request);
    ASSERT_TRUE(run) << run.error().describe();
    auto revisions = automatic_revisions(*run, std::nullopt, recipes_->now(), recipes_->engine_version());
    ASSERT_TRUE(revisions);
    engine::DevelopSettings edit;
    edit.contrast = 0.2;
    auto edited = recipes_->apply(item_, edit);
    ASSERT_TRUE(edited);
    auto applied = recipes_->apply_automatic(item_, std::nullopt, *revisions);
    ASSERT_TRUE(applied) << applied.error().describe();
    EXPECT_FALSE(applied->made_current);
    EXPECT_EQ(applied->state.revision, edited->revision) << "the user's edit stays current";
    ASSERT_EQ(applied->revisions.size(), 1u);
    EXPECT_FALSE(applied->revisions[0].parent.has_value());
    EXPECT_EQ(records().size(), 1u);
    // Selecting the automatic branch shows its settings.
    auto selected = recipes_->select_revision(item_, applied->revisions[0].id);
    ASSERT_TRUE(selected);
    EXPECT_EQ(selected->committed, run->effective_settings());
}

// The memory store (session libraries) keeps automatic revisions and records like the catalog.
TEST(AutoRevisionStores, TheMemoryStoreAppendsSiblingsWithTheirRecords) {
    std::shared_ptr<RecipeStore> store = create_memory_recipe_store();
    const ItemId item{7};
    catalog::NewRevision user;
    user.schema_name = "lucent.develop";
    user.schema_version = 1;
    user.settings_json = R"({"user":1})";
    auto base = store->append(item, user, std::nullopt);
    ASSERT_TRUE(base);
    std::vector<AutomaticRevision> revisions(2);
    for (std::size_t i = 0; i < revisions.size(); ++i) {
        revisions[i].revision = user;
        revisions[i].revision.origin = catalog::RevisionOrigin::Auto;
        revisions[i].revision.settings_json = std::format(R"({{"auto":{}}})", i);
        catalog::NewAutoAnalysis record;
        record.policy = "natural";
        record.policy_version = record.search_version = record.metric_contract_version = 2;
        record.develop_pipeline_version = record.develop_settings_version = 1;
        record.attempt = static_cast<std::uint32_t>(i);
        record.final_check = i == 0 ? catalog::AutoCheckOutcome::Failed : catalog::AutoCheckOutcome::Passed;
        revisions[i].analysis = record;
    }
    auto appended = store->append_automatic(item, base->id, revisions, true);
    ASSERT_TRUE(appended) << appended.error().describe();
    EXPECT_TRUE(appended->made_current);
    ASSERT_EQ(appended->revisions.size(), 2u);
    EXPECT_EQ(appended->revisions[0].parent, base->id);
    EXPECT_EQ(appended->revisions[1].parent, base->id);
    auto current = store->current(item);
    ASSERT_TRUE(current && *current);
    EXPECT_EQ((*current)->info.id, appended->revisions[1].id);
    auto records = store->auto_analyses(item);
    ASSERT_TRUE(records);
    ASSERT_EQ(records->size(), 2u);
    EXPECT_EQ((*records)[0].revision, appended->revisions[0].id);
    EXPECT_EQ((*records)[1].final_check, catalog::AutoCheckOutcome::Passed);
    // From a revision the item no longer shows: a branch, the current pointer stays.
    auto branch = store->append_automatic(item, base->id, std::span(revisions).first(1), true);
    ASSERT_TRUE(branch);
    EXPECT_FALSE(branch->made_current);
    EXPECT_EQ((*store->current(item))->info.id, appended->revisions[1].id);
    // Only automatic revisions.
    std::vector<AutomaticRevision> wrong(1);
    wrong[0].revision = user;
    auto refused = store->append_automatic(item, base->id, wrong, true);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::InvalidArgument);
}

TEST(AutoDocuments, ReadBackExactlyAndStrictly) {
    ts::ScratchDir dir;
    auto registry = ts::default_registry();
    const auto file = ts::write_dng(dir, L"dark.dng", dark_spec());
    AutoRunRequest request;
    request.anchor = true;
    auto run = run_automatic(*registry, file, 0, engine::DevelopSettings{}, request);
    ASSERT_TRUE(run) << run.error().describe();
    ASSERT_TRUE(run->anchor_appearance);
    auto text = write_auto_run(*run);
    ASSERT_TRUE(text) << text.error().describe();
    auto back = read_auto_run(*text);
    ASSERT_TRUE(back) << back.error().describe();
    auto again = write_auto_run(*back);
    ASSERT_TRUE(again);
    EXPECT_EQ(*again, *text) << "a document read back writes the same bytes";
    EXPECT_EQ(back->effective_settings(), run->effective_settings());
    EXPECT_EQ(back->shot.structure, run->shot.structure);
    EXPECT_EQ(back->appearance, run->appearance);
    EXPECT_EQ(back->confidence, run->confidence);

    // Strict: an unknown member, a newer version.
    auto json = nlohmann::json::parse(*text);
    json["surprise"] = 1;
    auto unknown = read_auto_run(json.dump());
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, ErrorCode::Corrupt);
    json.erase("surprise");
    json["version"] = kAutoDocumentVersion + 1;
    auto newer = read_auto_run(json.dump());
    ASSERT_FALSE(newer);
    EXPECT_EQ(newer.error().code, ErrorCode::VersionMismatch);

    // The request document, with a consistency target.
    AutoRunRequest targeted;
    analysis::ConsistencyTarget target;
    target.group_id = "time-1";
    target.lightness = 0.43;
    target.colour = true;
    target.u = 0.2;
    target.v = 0.47;
    target.contributors = 3;
    targeted.target = target;
    targeted.final_check = false;
    auto request_text = write_auto_request(targeted);
    ASSERT_TRUE(request_text);
    auto request_back = read_auto_request(*request_text);
    ASSERT_TRUE(request_back) << request_back.error().describe();
    ASSERT_TRUE(request_back->target);
    EXPECT_EQ(request_back->target->group_id, "time-1");
    EXPECT_EQ(request_back->target->lightness, 0.43);
    EXPECT_FALSE(request_back->final_check);
}

TEST(AutoDocuments, UnsupportedPhotosAreNotAnalysed) {
    ts::ScratchDir dir;
    auto registry = ts::default_registry();
    // A mandatory opcode the decoder cannot apply makes the interpretation unsupported.
    rts::SyntheticDngSpec spec = dark_spec();
    spec.backward_version = std::array<std::uint8_t, 4>{9, 9, 0, 0};
    const auto file = ts::write_dng(dir, L"future.dng", spec);
    auto run = run_automatic(*registry, file, 0, engine::DevelopSettings{}, AutoRunRequest{});
    ASSERT_FALSE(run);
    EXPECT_EQ(run.error().code, ErrorCode::Unsupported);
    EXPECT_EQ(run.error().where, "app.auto.support");
}

}  // namespace
}  // namespace lucent::app
