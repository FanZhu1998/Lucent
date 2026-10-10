// Automatic settings in the application (auto_develop.hpp, auto_service.hpp; ADR 0013): the
// interactive "Auto" creates automatic revisions with their analysis records, respects and reports
// locks, keeps a failed final-check attempt with its reason (the fallback ladder, forced by the test
// hook), writes nothing when nothing passes or the run is cancelled, keeps its revisions as a branch
// when the photo changed meanwhile, adjusts global controls only (the photographer's geometry, masks
// and chroma denoising survive unchanged, and the decision is the one made without the geometry and
// masks), and its documents read back exactly. Items are synthetic DNGs:
// dark (a high white level) so that the optimiser brightens them, and large enough for every metric's
// evidence minimum.

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
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
#include <lucent/engine/develop_geometry.hpp>

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

// The photographer's geometry (a straightened crop inside the image, a quarter turn), masks and
// chroma denoising on top of `s` (DevelopSettings v2, ADR 0015).
engine::DevelopSettings with_parts(engine::DevelopSettings s) {
    s.chroma_denoise = engine::ChromaDenoiseSetting{0.3, 4.0};
    s.geometry.straighten_degrees = 1.5;
    s.geometry.crop = engine::largest_inside_crop(1.5, image::Size{256, 192});
    s.geometry.quarter_turns = 1;
    engine::LocalAdjustment radial;
    radial.id = 1;
    radial.shape = engine::RadialGradientMask{{0.5, 0.5}, 0.2, 0.15, 0.0, 0.5};
    radial.controls.exposure_ev = 0.5;
    engine::LocalAdjustment brush;
    brush.id = 2;
    brush.shape = engine::BrushMask{{engine::BrushStroke{engine::BrushMode::Add, 0.05, 0.5, 1.0,
                                                         {{0.2, 0.7}, {0.4, 0.75}}}}};
    brush.controls.shadows = 0.3;
    s.local_adjustments = {radial, brush};
    return s;
}

// A grey card under a warm cast on the left and green foliage on the right, with a little texture
// (the synthetic camera's neutral is AsShotNeutral 0.5, 1, 0.667): the card is the neutral evidence.
rts::SyntheticDngSpec card_and_foliage_spec() {
    rts::SyntheticDngSpec spec = dark_spec();
    spec.scene = [](std::uint32_t x, std::uint32_t y, std::uint32_t color) -> std::uint16_t {
        const std::uint32_t hash = (x * 73856093u) ^ (y * 19349663u) ^ (color * 83492791u);
        const double texture = 1.0 + 0.01 * (static_cast<double>(hash % 1000u) / 500.0 - 1.0);
        const bool card = x < 100;
        // The card: a checkerboard of two greys (its edges are neutral too); the foliage: smooth.
        const double card_level = ((x / 8 + y / 8) % 2 == 0) ? 0.15 : 0.45;
        const std::array<double, 3> ratio =
            card ? std::array<double, 3>{0.5 * 1.25, 1.0, 0.667 * 0.85} : std::array<double, 3>{0.5 * 0.35, 1.0, 0.667 * 0.3};
        const double level = (card ? card_level : 0.2) * ratio[color] * texture;
        return static_cast<std::uint16_t>(256.0 + level * (65535.0 - 256.0));
    };
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
        return service_with(std::move(options));
    }

    AutoService& service_with(AutoServiceOptions options) {
        auto created = AutoService::create(*fixture_.library, *recipes_, *executor_, std::move(options));
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
        // W12 (ADR 0022): an update waits while a run is in progress; a finished run no longer counts.
        EXPECT_EQ(service.runs_in_progress(), 0u);
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
    EXPECT_EQ(auto_service.runs_in_progress(), 0u);
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

TEST_F(AutoServiceTest, AutoAdjustsGlobalControlsAndKeepsGeometryMasksAndDenoising) {
    const engine::DevelopSettings current = with_parts({});
    ASSERT_TRUE(recipes_->apply(item_, current));
    const AutoTaskResult result = run(service());
    ASSERT_FALSE(result.error) << result.error->describe();
    ASSERT_TRUE(result.run);
    const AutoRun& run = *result.run;
    EXPECT_EQ(run.start, current);
    EXPECT_GT(run.chosen.exposure_ev, 0.0) << "a dark photo is brightened";
    // The choice and every attempt of the full-resolution check carry them unchanged.
    EXPECT_EQ(run.chosen.geometry, current.geometry);
    EXPECT_EQ(run.chosen.local_adjustments, current.local_adjustments);
    EXPECT_EQ(run.chosen.chroma_denoise, current.chroma_denoise);
    for (const AutoAttempt& attempt : run.attempts) {
        EXPECT_EQ(attempt.settings.geometry, current.geometry);
        EXPECT_EQ(attempt.settings.local_adjustments, current.local_adjustments);
        EXPECT_EQ(attempt.settings.chroma_denoise, current.chroma_denoise);
    }
    // The check rendered the full settings (crop, rotation and masks) against a baseline with them,
    // measured in the cropped frame, and passed.
    ASSERT_TRUE(run.effective);
    EXPECT_EQ(*run.effective, 0u);
    EXPECT_EQ(run.attempts[0].check.outcome, catalog::AutoCheckOutcome::Passed)
        << (run.attempts[0].check.failures.empty() ? "" : run.attempts[0].check.failures.front());
    EXPECT_FALSE(run.attempts[0].check.values.empty());

    // The revision: the global controls changed, the parts did not.
    const RecipeState after = state();
    EXPECT_EQ(after.committed, run.effective_settings());
    EXPECT_EQ(after.committed.geometry, current.geometry);
    EXPECT_EQ(after.committed.local_adjustments, current.local_adjustments);
    EXPECT_EQ(after.committed.chroma_denoise, current.chroma_denoise);
    auto history = recipes_->history(item_);
    ASSERT_TRUE(history);
    EXPECT_EQ(history->entries.back().info.origin, catalog::RevisionOrigin::Auto);
    EXPECT_TRUE(history->entries.back().parts.empty());
    EXPECT_FALSE(history->entries.back().changed.empty());

    // The decision is the one Auto makes for the same photo without the geometry and masks.
    engine::DevelopSettings global = current;
    global.geometry = engine::GeometrySetting{};
    global.local_adjustments.clear();
    EXPECT_EQ(analysis::evaluation_settings(current), global);
    auto plain = run_automatic(*registry_, fixture_.library->find(item_)->path, 0, global, AutoRunRequest{});
    ASSERT_TRUE(plain) << plain.error().describe();
    EXPECT_EQ(analysis::evaluation_settings(run.chosen), plain->chosen);
    EXPECT_EQ(run.confidence, plain->confidence);
    ASSERT_TRUE(plain->effective);
    EXPECT_EQ(plain->attempts[*plain->effective].kind, run.attempts[*run.effective].kind);

    // A crop alone: the same decision again.
    engine::DevelopSettings cropped = global;
    cropped.geometry.crop = engine::CropRect{0.0, 0.0, 0.6, 0.6};
    auto with_crop = run_automatic(*registry_, fixture_.library->find(item_)->path, 0, cropped, AutoRunRequest{});
    ASSERT_TRUE(with_crop) << with_crop.error().describe();
    EXPECT_EQ(analysis::evaluation_settings(with_crop->chosen), plain->chosen);
    EXPECT_EQ(with_crop->chosen.geometry, cropped.geometry);
    ASSERT_TRUE(with_crop->effective);
}

// White balance is decided on the whole frame's neutral evidence (ADR 0015): a crop to the foliage,
// without the grey card that showed the cast, keeps the decision and its full-resolution check
// passes (it does not fail for lack of neutral surfaces inside the crop).
TEST_F(AutoServiceTest, ACropWithoutNeutralSurfacesKeepsTheWhiteBalanceDecision) {
    const auto path = ts::write_dng(dir_, L"card.dng", card_and_foliage_spec());
    const engine::DevelopSettings whole;
    auto uncropped = run_automatic(*registry_, path, 0, whole, AutoRunRequest{});
    ASSERT_TRUE(uncropped) << uncropped.error().describe();
    ASSERT_NE(uncropped->chosen.white_balance, whole.white_balance) << "the card's cast is corrected";
    ASSERT_TRUE(uncropped->effective);
    ASSERT_EQ(uncropped->attempts[*uncropped->effective].kind, AutoAttemptKind::Chosen);

    engine::DevelopSettings foliage;
    foliage.geometry.crop = engine::CropRect{0.5, 0.0, 1.0, 1.0};
    auto cropped = run_automatic(*registry_, path, 0, foliage, AutoRunRequest{});
    ASSERT_TRUE(cropped) << cropped.error().describe();
    EXPECT_EQ(analysis::evaluation_settings(cropped->chosen), uncropped->chosen);
    ASSERT_TRUE(cropped->effective);
    const AutoAttempt& effective = cropped->attempts[*cropped->effective];
    EXPECT_EQ(effective.kind, AutoAttemptKind::Chosen)
        << (cropped->attempts[0].check.failures.empty() ? "" : cropped->attempts[0].check.failures.front());
    EXPECT_EQ(effective.settings.white_balance, uncropped->chosen.white_balance);
}

// The camera's image area (geometry version 2, ADR 0017) is part of the geometry the evaluation sets
// aside: Auto decides the same for a recipe of geometry version 1 (the whole visible area) and one of
// version 2 (the DNG's DefaultCrop), keeps each recipe's version, and the full-resolution check
// measures the version-2 output (the area) without failing.
TEST_F(AutoServiceTest, TheCameraAreaDoesNotChangeTheAutoDecision) {
    rts::SyntheticDngSpec spec = dark_spec();
    spec.default_crop_origin = std::array<std::pair<std::uint32_t, std::uint32_t>, 2>{{{12, 1}, {8, 1}}};
    spec.default_crop_size = std::array<std::pair<std::uint32_t, std::uint32_t>, 2>{{{232, 1}, {176, 1}}};
    const auto path = ts::write_dng(dir_, L"area.dng", spec);
    engine::DevelopSettings whole;
    whole.operations.geometry = 1;
    const engine::DevelopSettings area;  // version 2
    auto a = run_automatic(*registry_, path, 0, whole, AutoRunRequest{});
    auto b = run_automatic(*registry_, path, 0, area, AutoRunRequest{});
    ASSERT_TRUE(a && b) << (a ? b.error().describe() : a.error().describe());
    EXPECT_EQ(a->chosen.operations.geometry, 1u);
    EXPECT_EQ(b->chosen.operations.geometry, 2u);
    engine::DevelopSettings b_as_whole = b->chosen;
    b_as_whole.operations.geometry = 1;
    EXPECT_EQ(b_as_whole, a->chosen);
    EXPECT_EQ(a->confidence, b->confidence);
    ASSERT_TRUE(a->effective && b->effective);
    EXPECT_EQ(a->attempts[*a->effective].kind, AutoAttemptKind::Chosen);
    EXPECT_EQ(b->attempts[*b->effective].kind, AutoAttemptKind::Chosen)
        << (b->attempts[0].check.failures.empty() ? "" : b->attempts[0].check.failures.front());
    ASSERT_EQ(a->attempts.size(), b->attempts.size());
    for (std::size_t i = 0; i < a->attempts.size(); ++i) {
        engine::DevelopSettings s = b->attempts[i].settings;
        s.operations.geometry = 1;
        EXPECT_EQ(s, a->attempts[i].settings) << i;
    }
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
    EXPECT_EQ(auto_service.runs_in_progress(), 1u) << "a queued run counts (an update waits for it)";
    auto_service.cancel(*ticket);
    gate.open();
    auto result = auto_service.wait(*ticket, 120s);
    ASSERT_TRUE(result);
    EXPECT_EQ(auto_service.runs_in_progress(), 0u);
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

// Auto holds its memory but not the executor's one render place (task_executor.hpp): the editor's
// stage-one builds, refines and 100 % views render beside it, within the budget that counts Auto's
// bytes.
TEST_F(AutoServiceTest, TheRunHoldsItsMemoryButNotTheRenderPlace) {
    AutoService& auto_service = service();
    std::mutex mutex;
    std::optional<AdmissionStats> during;
    std::uint64_t claim = 0;
    Subscription subscription = auto_service.subscribe([&](const AutoEvent& event) {
        if (event.kind == AutoEvent::Kind::Progress && event.status.stage == AutoTaskStage::Checking) {
            std::lock_guard lock(mutex);
            if (!during) {
                during = executor_->admission().stats();
                claim = event.status.memory_bytes;
            }
        }
    });
    const AutoTaskResult result = run(auto_service);
    ASSERT_FALSE(result.error) << result.error->describe();
    std::lock_guard lock(mutex);
    ASSERT_TRUE(during.has_value());
    EXPECT_GT(claim, 0u);
    EXPECT_EQ(during->active, 0u) << "no render place is held while Auto runs";
    EXPECT_EQ(during->retained, 1u);
    EXPECT_EQ(during->retained_bytes, claim) << "its memory still counts against the budget";
    EXPECT_EQ(executor_->admission().stats().active_bytes, 0u) << "everything is returned afterwards";
}

// A run whose claim does not fit says so (WaitingForMemory) and asks once for memory that is
// rebuilt on demand (the editor's cached 100 % stage one) to be released; then it runs.
TEST_F(AutoServiceTest, ARunWaitingForMemoryAsksForRoomOnce) {
    MemoryAdmission& admission = executor_->admission();
    auto held = admission.retain(admission.options().budget_bytes - 4096, {});
    ASSERT_TRUE(held);
    std::atomic<int> asked{0};
    AutoServiceOptions options;
    options.registry = registry_;
    options.make_room = [&](ItemId item) {
        EXPECT_EQ(item, item_);
        ++asked;
        held->release();  // the cached image is dropped (rebuilt when needed)
    };
    AutoService& auto_service = service_with(std::move(options));
    std::mutex mutex;
    std::vector<AutoTaskStage> stages;
    Subscription subscription = auto_service.subscribe([&](const AutoEvent& event) {
        std::lock_guard lock(mutex);
        if (stages.empty() || stages.back() != event.status.stage) {
            stages.push_back(event.status.stage);
        }
    });
    const AutoTaskResult result = run(auto_service);
    ASSERT_FALSE(result.error) << result.error->describe();
    EXPECT_EQ(asked.load(), 1);
    std::lock_guard lock(mutex);
    ASSERT_GE(stages.size(), 2u);
    EXPECT_EQ(stages.front(), AutoTaskStage::WaitingForMemory);
    EXPECT_EQ(to_string(AutoTaskStage::WaitingForMemory), "waiting_for_memory");
}

// The file is checked again after the run (blueprint: stable reads around decoding): a file that
// changed while it was analysed gets no revision describing it.
TEST_F(AutoServiceTest, AFileThatChangesDuringTheRunGetsNoRevision) {
    AutoService& auto_service = service();
    const std::filesystem::path path = fixture_.library->find(item_)->path;
    std::atomic<bool> touched{false};
    Subscription subscription = auto_service.subscribe([&](const AutoEvent& event) {
        if (event.kind == AutoEvent::Kind::Progress && event.status.stage == AutoTaskStage::Checking &&
            !touched.exchange(true)) {
            std::error_code ec;
            std::filesystem::last_write_time(path, std::filesystem::last_write_time(path, ec) + std::chrono::hours(1),
                                             ec);
            EXPECT_FALSE(ec) << ec.message();
        }
    });
    const AutoTaskResult result = run(auto_service);
    ASSERT_TRUE(touched.load());
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, ErrorCode::SourceChanged);
    EXPECT_TRUE(result.revisions.empty());
    EXPECT_FALSE(state().revision.has_value());
    EXPECT_TRUE(records().empty());
}

// A numerically invalid full-resolution baseline (nothing can be judged against it) is the final
// check's failure, "app.auto.check", not an internal error of the run.
TEST_F(AutoServiceTest, AnInvalidBaselineFailsTheCheckNotTheRun) {
    const auto file = fixture_.library->find(item_)->path;
    AutoRunOptions options;
    options.test_invalid_baseline = true;
    auto direct = run_automatic(*registry_, file, 0, engine::DevelopSettings{}, AutoRunRequest{}, options);
    ASSERT_TRUE(direct) << direct.error().describe();
    EXPECT_TRUE(direct->check_failed());
    ASSERT_EQ(direct->attempts.size(), 1u);
    EXPECT_EQ(direct->attempts[0].check.outcome, catalog::AutoCheckOutcome::Failed);
    ASSERT_FALSE(direct->attempts[0].check.failures.empty());
    EXPECT_NE(direct->attempts[0].check.failures[0].find("numerical_validity"), std::string::npos);
    EXPECT_TRUE(direct->review_recommended());

    AutoServiceOptions service_options;
    service_options.registry = registry_;
    service_options.run.test_invalid_baseline = true;
    const AutoTaskResult result = run(service_with(std::move(service_options)));
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->where, "app.auto.check");
    EXPECT_TRUE(result.revisions.empty());
    EXPECT_TRUE(records().empty());
}

// When the choice is the baseline (here: every control the optimiser sets is locked) the check can
// only fail it on numerical validity: it renders it once and scans it, without measurements.
TEST_F(AutoServiceTest, AChoiceThatIsTheBaselineIsCheckedForValidityOnly) {
    const auto file = fixture_.library->find(item_)->path;
    engine::DevelopSettings locked;
    locked.exposure_ev = 0.3;
    locked.locks = {DevelopControl::WhiteBalance, DevelopControl::Exposure,   DevelopControl::Contrast,
                    DevelopControl::Highlights,   DevelopControl::Shadows,    DevelopControl::Saturation};
    auto run = run_automatic(*registry_, file, 0, locked, AutoRunRequest{});
    ASSERT_TRUE(run) << run.error().describe();
    EXPECT_TRUE(run->manual);
    EXPECT_EQ(run->chosen, locked) << "nothing to change";
    ASSERT_EQ(run->attempts.size(), 1u);
    const AutoAttempt& attempt = run->attempts[0];
    EXPECT_EQ(attempt.check.outcome, catalog::AutoCheckOutcome::Passed);
    EXPECT_TRUE(attempt.check.failures.empty());
    EXPECT_TRUE(attempt.check.values.empty()) << "validity only: no measurements";
    ASSERT_TRUE(run->effective);
    EXPECT_EQ(run->effective_settings(), locked);
    // Everything locked is information, not a missed target.
    ASSERT_FALSE(run->locks.reasons.empty());
    EXPECT_EQ(run->locks.reasons.back(), kEveryControlLockedNote);
    // The test hook still fails the only rung: nothing passes.
    AutoRunRequest forced;
    forced.fail_final_checks = 1;
    auto failed = run_automatic(*registry_, file, 0, locked, forced);
    ASSERT_TRUE(failed);
    EXPECT_TRUE(failed->check_failed());
}

// The service keeps the results of its last kKeptAutoResults finished runs, not every run's.
TEST_F(AutoServiceTest, OnlyTheLastResultsAreKept) {
    AutoService& auto_service = service();
    std::vector<std::uint64_t> tickets;
    for (std::size_t i = 0; i < kKeptAutoResults + 2; ++i) {
        auto ticket = auto_service.run(item_);
        ASSERT_TRUE(ticket) << ticket.error().describe();
        ASSERT_TRUE(auto_service.wait(*ticket, 120s).has_value());
        tickets.push_back(*ticket);
    }
    EXPECT_FALSE(auto_service.status(tickets[0]).has_value());
    EXPECT_FALSE(auto_service.status(tickets[1]).has_value());
    for (std::size_t i = 2; i < tickets.size(); ++i) {
        ASSERT_TRUE(auto_service.status(tickets[i]).has_value()) << i;
        EXPECT_EQ(auto_service.status(tickets[i])->stage, AutoTaskStage::Finished);
    }
}

// A gesture in progress when automatic revisions arrive keeps its own base: the revisions are a
// branch, and committing the gesture appends the photographer's edit (nothing is reverted or lost).
TEST_F(AutoServiceTest, AGestureInProgressKeepsItsBaseWhenAutomaticRevisionsArrive) {
    auto run = run_automatic(*registry_, fixture_.library->find(item_)->path, 0, engine::DevelopSettings{},
                             AutoRunRequest{});
    ASSERT_TRUE(run) << run.error().describe();
    auto revisions = automatic_revisions(*run, std::nullopt, recipes_->now(), recipes_->engine_version());
    ASSERT_TRUE(revisions);
    engine::DevelopSettings gesture;
    gesture.contrast = 0.25;
    ASSERT_TRUE(recipes_->set_transient(item_, gesture));
    bool make_current_offered = true;
    auto applied = recipes_->write_automatic(item_, std::nullopt, [&](bool make_current) {
        make_current_offered = make_current;
        return store_->append_automatic(item_, std::nullopt, *revisions, make_current);
    });
    ASSERT_TRUE(applied) << applied.error().describe();
    EXPECT_FALSE(make_current_offered) << "a pending gesture keeps the automatic revisions off the current pointer";
    EXPECT_FALSE(applied->made_current);
    EXPECT_TRUE(applied->state.transient) << "the gesture is still in progress";
    EXPECT_TRUE(is_automatic_branch_note(applied->revisions[0].note)) << applied->revisions[0].note;
    auto committed = recipes_->commit(item_);
    ASSERT_TRUE(committed) << committed.error().describe();
    EXPECT_EQ(committed->committed, gesture) << "the photographer's edit is applied, not reverted";
    auto history = recipes_->history(item_);
    ASSERT_TRUE(history);
    ASSERT_EQ(history->entries.size(), 2u);
    EXPECT_FALSE(history->entries[1].info.parent.has_value()) << "derived from the recipe the gesture started on";
}

// Redo follows the path the photographer came from: after an automatic result kept as a branch,
// undoing the photographer's edit and redoing returns to that edit (also after a restart, when the
// branch's note says it is one), never silently to the branch.
TEST_F(AutoServiceTest, RedoReturnsToThePhotographersEditNotToAnAutomaticBranch) {
    auto run = run_automatic(*registry_, fixture_.library->find(item_)->path, 0, engine::DevelopSettings{},
                             AutoRunRequest{});
    ASSERT_TRUE(run) << run.error().describe();
    auto revisions = automatic_revisions(*run, std::nullopt, recipes_->now(), recipes_->engine_version());
    ASSERT_TRUE(revisions);
    engine::DevelopSettings edit;
    edit.exposure_ev = -0.5;
    auto edited = recipes_->apply(item_, edit);  // made while Auto ran
    ASSERT_TRUE(edited);
    auto applied = recipes_->apply_automatic(item_, std::nullopt, *revisions);
    ASSERT_TRUE(applied);
    ASSERT_FALSE(applied->made_current);
    ASSERT_TRUE(recipes_->undo(item_));
    auto redone = recipes_->redo(item_);
    ASSERT_TRUE(redone);
    EXPECT_EQ(redone->revision, edited->revision);
    EXPECT_EQ(redone->committed, edit);

    // After a restart nothing is remembered: the branch's note keeps redo on the edit.
    ASSERT_TRUE(recipes_->undo(item_));
    RecipeServiceOptions options;
    options.registry = registry_;
    RecipeService restarted(*fixture_.library, store_, options);
    auto again = restarted.redo(item_);
    ASSERT_TRUE(again);
    EXPECT_EQ(again->revision, edited->revision);

    // A branch the photographer chose stays the way back: select it, undo, redo returns to it.
    ASSERT_TRUE(restarted.select_revision(item_, applied->revisions[0].id));
    ASSERT_TRUE(restarted.undo(item_));
    auto back = restarted.redo(item_);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->revision, applied->revisions[0].id);
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
    EXPECT_TRUE(is_automatic_branch_note(branch->revisions[0].note)) << "a branch says so in its note";
    EXPECT_FALSE(is_automatic_branch_note(appended->revisions[1].note));
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
