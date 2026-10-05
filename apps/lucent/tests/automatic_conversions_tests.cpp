// W8: the conversions of automatic settings between the application services and the shell
// (edit_conversions.hpp, batch_conversions.hpp): an analysis record becomes the editor's summary
// (flags in the user's words, review reasons apart from notes, the locks report, the fallback, the
// group), an Auto run's result and errors become its outcome, automatic revisions are named in the
// history, and automatic batches map both ways (request, plan notes, snapshot counts, report rows,
// worker stages).

#include <format>

#include <gtest/gtest.h>

#include <lucent/analysis/auto_settings.hpp>
#include <lucent/application/auto_develop.hpp>
#include <lucent/application/batch_service.hpp>

#include "adapters/batch_conversions.hpp"
#include "adapters/edit_conversions.hpp"

namespace lucent::ui::adapters {
namespace {

using analysis::ReviewFlagCode;

std::uint16_t code(ReviewFlagCode flag) {
    return static_cast<std::uint16_t>(flag);
}

// A record as auto_analysis_record() writes it (the members the shell reads).
catalog::AutoAnalysisRecord record_of(catalog::RevisionId revision) {
    catalog::AutoAnalysisRecord record;
    record.revision = revision;
    record.item = catalog::ItemId{3};
    record.policy = "natural";
    record.attempt = 1;
    record.final_check = catalog::AutoCheckOutcome::Passed;
    record.confidence = 0.6;
    record.review = true;
    record.flags_json = std::format(
        R"([{{"code":{},"name":"large_edit","metric":null,"reason":"exposure +1.4 EV","review":true}},)"
        R"({{"code":{},"name":"white_balance_uncertain","metric":null,"reason":"estimators disagree","review":false}},)"
        R"({{"code":{},"name":"source_saturation","metric":"source_saturation","reason":"2 % clipped","review":false}},)"
        R"({{"code":{},"name":"final_check_fallback","metric":null,"reason":"attempt 0 failed","review":true}},)"
        R"({{"code":{},"name":"conservative_fallback","metric":null,"reason":"same words","review":true}},)"
        R"({{"code":{},"name":"locks_prevented_targets","metric":null,"reason":"Exposure is locked.","review":true}}])",
        code(ReviewFlagCode::LargeEdit), code(ReviewFlagCode::WhiteBalanceUncertain),
        code(ReviewFlagCode::SourceSaturation), app::kFlagFinalCheckFallback, code(ReviewFlagCode::ConservativeFallback),
        app::kFlagLocksPreventedTargets);
    record.record_json =
        R"({"schema":"lucent.auto-record","version":1,)"
        R"("attempt":{"index":1,"kind":"fallback","reason":"a conservative fallback after attempt 0 failed the )"
        R"(full-resolution check: new clipped fraction 0.031 > 0.02","check":{"outcome":"passed"}},)"
        R"("effective":true,)"
        R"("locks":{"locked":["exposure","white_balance"],"prevented_targets":true,)"
        R"("reasons":["Exposure is locked, so the photo stays about 1.2 EV darker than Lucent's usual target."]},)"
        R"("consistency":{"target":{"group_id":"time-1"}}})";
    return record;
}

// A fallback to the baseline is worded as such; an anchor kept by a batch is marked; "every control
// is locked" is information, not something the locks prevented.
TEST(AutomaticConversions, BaselineFallbacksAnchorsAndEverythingLockedAreWordedForWhatTheyAre) {
    app::HistoryEntry entry;
    entry.info.id = catalog::RevisionId{12};
    entry.info.revision_number = 4;
    entry.info.origin = catalog::RevisionOrigin::Auto;
    entry.current = true;
    catalog::AutoAnalysisRecord baseline = record_of(entry.info.id);
    baseline.attempt = 2;
    baseline.record_json =
        std::string(R"({"attempt":{"index":2,"kind":"baseline","reason":"the as-shot baseline after attempt 1 failed"},)") +
        R"("locks":{"locked":["exposure"],"prevented_targets":false,"reasons":[")" +
        std::string(app::kEveryControlLockedNote) + R"("]}})";
    const AutoSummary summary = to_auto_summary(baseline, entry);
    EXPECT_TRUE(summary.baseline);
    EXPECT_FALSE(summary.anchor);
    EXPECT_TRUE(summary.review_reasons.contains(
        QStringLiteral("Every automatic edit failed Lucent's full-size quality check, so Auto kept the camera's "
                       "rendering for the controls it sets.")))
        << summary.review_reasons.join(QStringLiteral(" | ")).toStdString();
    EXPECT_TRUE(summary.notes.contains(QString::fromUtf8(app::kEveryControlLockedNote.data(),
                                                         static_cast<qsizetype>(app::kEveryControlLockedNote.size()))));
    EXPECT_TRUE(summary.lock_notes.isEmpty()) << "nothing the locks prevented";

    catalog::AutoAnalysisRecord anchor = record_of(entry.info.id);
    anchor.job = catalog::JobId{9};
    anchor.attempt = 0;
    anchor.review = false;
    anchor.flags_json = "[]";
    anchor.record_json = R"({"attempt":{"index":0,"kind":"anchor","reason":"kept"},"consistency":{"target":{"group_id":"user:series"}}})";
    const AutoSummary kept = to_auto_summary(anchor, entry);
    EXPECT_TRUE(kept.anchor);
    EXPECT_EQ(kept.group, QStringLiteral("user:series"));
    EXPECT_FALSE(kept.review);
}

TEST(AutomaticConversions, ARecordBecomesTheEditorsSummaryInTheUsersWords) {
    app::HistoryEntry entry;
    entry.info.id = catalog::RevisionId{12};
    entry.info.revision_number = 4;
    entry.info.origin = catalog::RevisionOrigin::Auto;
    entry.current = true;
    entry.changed = {engine::DevelopControl::Exposure, engine::DevelopControl::Shadows};
    const AutoSummary summary = to_auto_summary(record_of(entry.info.id), entry);

    EXPECT_EQ(summary.revision, 12u);
    EXPECT_EQ(summary.revision_number, 4u);
    EXPECT_FALSE(summary.from_batch);
    EXPECT_DOUBLE_EQ(summary.confidence, 0.6);
    EXPECT_TRUE(summary.review);
    EXPECT_EQ(summary.changed, (QList<EditControl>{EditControl::Exposure, EditControl::Shadows}));
    // The review reasons in the user's words: two codes sharing their wording once, and the locks
    // named in one sentence (their details are the locks report's).
    EXPECT_EQ(summary.review_reasons,
              (QStringList{QStringLiteral("Lucent made a large automatic change; check that it suits the photo."),
                           QStringLiteral("The first automatic edit failed a quality check, so a more conservative "
                                          "one was used."),
                           QStringLiteral("Locked controls kept Auto from its usual targets.")}));
    // Findings that do not recommend a review are notes.
    EXPECT_EQ(summary.notes,
              (QStringList{QStringLiteral("The colour of the light was uncertain, so the white balance was kept "
                                          "close to the camera's."),
                           QStringLiteral("Parts of the photo were overexposed in the camera.")}));
    EXPECT_EQ(summary.locked, (QList<EditControl>{EditControl::Exposure, EditControl::WhiteBalance}));
    EXPECT_TRUE(summary.locks_prevented_targets);
    ASSERT_EQ(summary.lock_notes.size(), 1);
    EXPECT_TRUE(summary.lock_notes[0].startsWith(QStringLiteral("Exposure is locked")));
    EXPECT_EQ(summary.attempt, 1u);
    EXPECT_FALSE(summary.baseline);
    EXPECT_TRUE(summary.fallback_detail.contains(QStringLiteral("new clipped fraction")));
    EXPECT_EQ(summary.group, QStringLiteral("time-1"));

    // A batch's record; a damaged document leaves its parts out rather than failing.
    catalog::AutoAnalysisRecord batch = record_of(entry.info.id);
    batch.job = catalog::JobId{7};
    batch.attempt = 0;
    batch.review = false;
    batch.flags_json = "[]";
    batch.record_json = "not json";
    const AutoSummary plain = to_auto_summary(batch, entry);
    EXPECT_TRUE(plain.from_batch);
    EXPECT_FALSE(plain.review);
    EXPECT_TRUE(plain.review_reasons.isEmpty());
    EXPECT_TRUE(plain.notes.isEmpty());
    EXPECT_TRUE(plain.locked.isEmpty());
    EXPECT_TRUE(plain.fallback_detail.isEmpty());
    EXPECT_TRUE(plain.group.isEmpty());
}

TEST(AutomaticConversions, RunsBecomeOutcomesAndErrorsAreExplained) {
    app::AutoTaskResult result;
    result.run = app::AutoRun{};
    catalog::RevisionInfo failed;
    failed.revision_number = 2;
    catalog::RevisionInfo effective;
    effective.revision_number = 3;
    result.revisions = {failed, effective};
    result.made_current = true;
    AutoOutcome outcome = to_auto_outcome(9, result);
    EXPECT_EQ(outcome.item, 9u);
    EXPECT_TRUE(outcome.saved);
    EXPECT_TRUE(outcome.made_current);
    EXPECT_EQ(outcome.revision_number, 3u);
    EXPECT_EQ(outcome.revisions, 2);
    EXPECT_TRUE(outcome.message.startsWith(QStringLiteral("Auto saved its settings as revision 3.")));
    EXPECT_TRUE(outcome.message.contains(QStringLiteral("more conservative")));

    result.made_current = false;  // the photo was edited meanwhile
    outcome = to_auto_outcome(9, result);
    EXPECT_FALSE(outcome.made_current);
    EXPECT_TRUE(outcome.message.contains(QStringLiteral("kept in the history as revision 3")));

    app::AutoTaskResult cancelled;
    cancelled.error = Error{ErrorCode::Cancelled, "cancelled", "app.auto", 0};
    outcome = to_auto_outcome(9, cancelled);
    EXPECT_FALSE(outcome.saved);
    EXPECT_TRUE(outcome.cancelled);
    EXPECT_EQ(outcome.message, QStringLiteral("Auto was cancelled; nothing was changed."));

    EXPECT_TRUE(describe_auto_error(Error{ErrorCode::Internal, "", "app.auto.check", 0})
                    .startsWith(QStringLiteral("No automatic edit passed")));
    EXPECT_TRUE(describe_auto_error(Error{ErrorCode::Unsupported, "", "app.auto.support", 0})
                    .contains(QStringLiteral("cannot render this camera file faithfully")));
    EXPECT_EQ(describe_auto_error(Error{ErrorCode::AlreadyExists, "", "app.auto.busy", 0}),
              QStringLiteral("Auto is already running for this photo."));
    EXPECT_TRUE(describe_auto_error(Error{ErrorCode::LimitExceeded, "", "app.auto", 0})
                    .contains(QStringLiteral("more memory")));
    EXPECT_EQ(to_auto_stage(app::AutoTaskStage::Checking), AutoStage::Checking);
    EXPECT_EQ(to_auto_stage(app::AutoTaskStage::Finished), AutoStage::Saving);
}

TEST(AutomaticConversions, TheHistoryNamesAutomaticAttemptsFromTheirNotes) {
    app::RecipeHistory history;
    app::HistoryEntry failed;
    failed.info.id = catalog::RevisionId{1};
    failed.info.revision_number = 1;
    failed.info.origin = catalog::RevisionOrigin::Auto;
    failed.info.note = "auto=chosen;attempt=0;check=failed;policy=natural/1;changed=exposure;reason=x";
    app::HistoryEntry used = failed;
    used.info.id = catalog::RevisionId{2};
    used.info.revision_number = 2;
    used.info.note = "auto=fallback;attempt=1;check=passed;policy=natural/1;changed=exposure;reason=a=b";
    used.current = true;
    app::HistoryEntry user;
    user.info.id = catalog::RevisionId{3};
    user.info.origin = catalog::RevisionOrigin::User;
    user.info.note = "auto=chosen;check=failed";  // not an automatic revision: ignored
    history.entries = {failed, used, user};
    const QList<EditHistoryEntry> entries = to_edit_history(history);
    ASSERT_EQ(entries.size(), 3);
    EXPECT_EQ(entries[0].auto_kind, QStringLiteral("chosen"));
    EXPECT_TRUE(entries[0].auto_failed);
    EXPECT_EQ(entries[1].auto_kind, QStringLiteral("fallback"));
    EXPECT_FALSE(entries[1].auto_failed);
    EXPECT_TRUE(entries[2].auto_kind.isEmpty());
    EXPECT_FALSE(entries[2].auto_failed);
}

TEST(AutomaticConversions, AutomaticBatchesMapBothWays) {
    BatchSetup setup;
    setup.items = {1, 2};
    setup.output_folder = QStringLiteral("D:/Out");
    setup.keep_consistent = true;  // without automatic settings it means nothing
    app::BatchRequest request = to_batch_request(setup);
    EXPECT_EQ(request.automatic, app::AutomaticSettings::Off);
    EXPECT_FALSE(request.keep_consistent);
    setup.automatic = BatchAutomatic::AnalyzeOnly;
    request = to_batch_request(setup);
    EXPECT_EQ(request.automatic, app::AutomaticSettings::AnalyzeOnly);
    EXPECT_TRUE(request.keep_consistent);
    setup.automatic = BatchAutomatic::Auto;
    setup.keep_consistent = false;
    request = to_batch_request(setup);
    EXPECT_EQ(request.automatic, app::AutomaticSettings::Auto);
    EXPECT_FALSE(request.keep_consistent);

    // The plan: what the analysis adds, in the user's words.
    app::BatchPlan plan;
    plan.outputs = 2;
    plan.examples.push_back(app::PlannedOutput{});
    plan.notices = {app::PlanNotice::AutomaticSettings, app::PlanNotice::KeepConsistent};
    BatchPlanInfo info = to_plan_info(plan);
    ASSERT_EQ(info.automatic_notes.size(), 3);
    EXPECT_TRUE(info.automatic_notes[0].contains(QStringLiteral("a few seconds per photo")));
    EXPECT_TRUE(info.automatic_notes[1].contains(QStringLiteral("analysed together first")));
    EXPECT_TRUE(info.automatic_notes[2].contains(QStringLiteral("automatic revision")));
    EXPECT_TRUE(info.warnings.isEmpty());  // never the service's developer wording
    plan.examples.clear();
    plan.outputs = 0;
    plan.notices = {app::PlanNotice::AutomaticSettings, app::PlanNotice::AnalyzeOnly};
    info = to_plan_info(plan);
    ASSERT_EQ(info.automatic_notes.size(), 2);
    EXPECT_TRUE(info.automatic_notes[1].startsWith(QStringLiteral("Nothing is exported")));

    // The status: the mode and the related-shot pre-pass's counts while work is left.
    app::BatchStatus status;
    status.record.id = catalog::BatchId{5};
    status.automatic = catalog::AutoMode::AnalyzeOnly;
    status.keep_consistent = true;
    status.automatic_counts = catalog::AutoJobCounts{3, 1, 0, 2};
    BatchSnapshot snapshot = to_snapshot(status);
    EXPECT_EQ(snapshot.automatic, BatchAutomatic::AnalyzeOnly);
    EXPECT_TRUE(snapshot.keep_consistent);
    EXPECT_EQ(snapshot.analysis_pending, 3);
    EXPECT_EQ(snapshot.analysis_recorded, 2);
    status.automatic_counts.reset();
    status.automatic = catalog::AutoMode::None;
    snapshot = to_snapshot(status);
    EXPECT_EQ(snapshot.automatic, BatchAutomatic::Off);
    EXPECT_EQ(snapshot.analysis_pending, -1);

    EXPECT_EQ(describe_worker_stage(workers::JobStage::Analysing), QStringLiteral("Analysing"));
    EXPECT_EQ(describe_worker_stage(workers::JobStage::Checking), QStringLiteral("Checking at full size"));

    // The report: analysed photos and what Auto did to each.
    app::BatchReport report;
    report.batch.id = catalog::BatchId{5};
    report.batch.automatic = catalog::AutoMode::AnalyzeOnly;
    report.analysed = 1;
    report.review_recommended = 1;
    app::ReportEntry analysed;
    analysed.outcome = app::ReportOutcome::Analysed;
    analysed.review = {"Lucent was unsure about the automatic edit, so it kept the change small."};
    app::AutomaticReport automatic;
    automatic.state = catalog::AutoJobState::Frozen;
    automatic.revision = catalog::RevisionId{40};
    automatic.revision_number = 2;
    automatic.confidence = 0.42;
    automatic.attempt = 1;
    automatic.attempts = 2;
    automatic.review = true;
    automatic.flags = {analysed.review[0], "Parts of the photo were overexposed in the camera.", analysed.review[0]};
    automatic.group = "time-1";
    automatic.locks_prevented_targets = true;
    automatic.locks = {"Exposure is locked."};
    analysed.automatic = automatic;
    app::ReportEntry pending;
    pending.outcome = app::ReportOutcome::Pending;
    pending.automatic = app::AutomaticReport{};  // not analysed yet
    report.entries = {analysed, pending};
    const BatchReportInfo converted = to_report_info(report);
    EXPECT_EQ(converted.automatic, BatchAutomatic::AnalyzeOnly);
    EXPECT_EQ(converted.analysed, 1);
    EXPECT_EQ(converted.review, 1);
    ASSERT_EQ(converted.entries.size(), 2);
    const BatchReportEntry& row = converted.entries[0];
    EXPECT_EQ(row.outcome, BatchOutcome::Analysed);
    EXPECT_TRUE(row.automatic);
    EXPECT_EQ(row.revision_number, 2u);
    EXPECT_DOUBLE_EQ(row.confidence, 0.42);
    EXPECT_EQ(row.attempt, 1);
    EXPECT_EQ(row.flags.size(), 2);  // once each
    EXPECT_TRUE(row.locks_prevented);
    EXPECT_EQ(row.locks, QStringList{QStringLiteral("Exposure is locked.")});
    EXPECT_EQ(row.group, QStringLiteral("time-1"));
    EXPECT_TRUE(converted.entries[1].automatic);
    EXPECT_EQ(converted.entries[1].revision_number, 0u);
    EXPECT_LT(converted.entries[1].confidence, 0.0);  // unknown until it is analysed
}

}  // namespace
}  // namespace lucent::ui::adapters
