// The batch view models (W7, phase 3) over a recording BatchService: the Export batch dialog's
// setup (plan refresh debounce and superseded plans, presets, metadata, the explicit overwrite
// confirmation, local pattern checks), the Exports view's batch list (state transitions, commands,
// attention display, summaries, preparing submissions), the report model (filters, coalesced
// reloads), the Library's multi-selection and the controller's batch actions.

#include <QSignalSpy>
#include <QTest>
#include <memory>

#include <gtest/gtest.h>

#include "app_controller.hpp"
#include "models/batch_list_model.hpp"
#include "models/batch_presentation.hpp"
#include "models/batch_report_model.hpp"
#include "models/batch_setup.hpp"
#include "models/library_model.hpp"
#include "test_support.hpp"

namespace lucent::ui {
namespace {

using testing::RecordingBatchService;

constexpr int kWaitMs = 5000;

QUrl local(const QString& path) {
    return QUrl::fromLocalFile(path);
}

BatchSnapshot batch(BatchId id, BatchState state, qint64 total, qint64 completed, qint64 queued = 0,
                    qint64 running = 0, qint64 failed = 0) {
    BatchSnapshot s;
    s.id = id;
    s.name = QStringLiteral("Export of %1 photos").arg(total);
    s.folder = QStringLiteral("D:\\Out");
    s.state = state;
    s.total = total;
    s.completed = completed;
    s.queued = queued;
    s.running = running;
    s.failed = failed;
    return s;
}

QVariant role(const QAbstractItemModel& model, int row, int role) {
    return model.data(model.index(row, 0), role);
}

// --- BatchSetupModel -----------------------------------------------------------------------------

class BatchSetupTest : public ::testing::Test {
protected:
    void SetUp() override {
        setup.set_plan_delay(std::chrono::milliseconds(30));
        setup.prepare({1, 2}, {1, 2, 3});
        setup.setOutputFolder(local(QStringLiteral("D:/Out")));
    }

    RecordingBatchService service;
    BatchSetupModel setup{service};
};

TEST_F(BatchSetupTest, DefaultsAreTheSharingPresetUniqueNamesAndNoGps) {
    EXPECT_EQ(setup.scope(), BatchSetupModel::Selection);  // several photos selected
    const BatchSetup described = setup.setup();
    EXPECT_EQ(described.items, (QList<ItemId>{1, 2}));
    EXPECT_EQ(described.output.format, ExportFileFormat::Jpeg);
    EXPECT_EQ(described.output.color_space, ExportColorSpace::Srgb);
    EXPECT_EQ(described.output.jpeg_quality, 95);
    EXPECT_EQ(described.name_pattern, QStringLiteral("{original-stem}_Lucent_{revision}"));
    EXPECT_EQ(described.collisions, BatchCollisions::UniqueName);
    EXPECT_EQ(described.existing, BatchExisting::Skip);
    EXPECT_TRUE(described.include_capture);
    EXPECT_TRUE(described.include_rights);
    EXPECT_FALSE(described.include_gps);
    EXPECT_EQ(described.output_folder, QStringLiteral("D:/Out"));
    EXPECT_FALSE(setup.automatic_settings_available());
    EXPECT_EQ(setup.automatic_settings_note(), QStringLiteral("Available when Natural v1 is integrated"));

    // One photo selected (just the current one): every listed photo.
    setup.prepare({2}, {1, 2, 3});
    EXPECT_EQ(setup.scope(), BatchSetupModel::AllPhotos);
    EXPECT_EQ(setup.setup().items, (QList<ItemId>{1, 2, 3}));
}

TEST_F(BatchSetupTest, TheTiffPresetUsesTheChosenProfileAndTheJpegPresetIsAlwaysSrgbQuality95) {
    setup.setPreset(BatchSetupModel::Tiff16);
    setup.setDisplayP3(true);
    EXPECT_EQ(setup.setup().output.format, ExportFileFormat::Tiff16);
    EXPECT_EQ(setup.setup().output.color_space, ExportColorSpace::DisplayP3);
    setup.setPreset(BatchSetupModel::Jpeg);
    EXPECT_EQ(setup.setup().output.color_space, ExportColorSpace::Srgb);  // the sharing preset
    EXPECT_EQ(setup.setup().output.jpeg_quality, 95);
}

TEST_F(BatchSetupTest, PlansAreRequestedOnlyWhileActiveAndOnceAfterAQuietPeriod) {
    setup.setNamePattern(QStringLiteral("{original-stem}_a"));
    QTest::qWait(80);
    EXPECT_TRUE(service.plans.empty());  // the dialog is closed

    setup.setActive(true);
    EXPECT_TRUE(setup.planning());
    // Typing: every key restarts the quiet period, so one request follows.
    for (const char* pattern : {"{original-stem}_b", "{original-stem}_bc", "{original-stem}_bcd"}) {
        setup.setNamePattern(QString::fromLatin1(pattern));
        QTest::qWait(10);
    }
    ASSERT_TRUE(QTest::qWaitFor([&] { return !service.plans.empty(); }, kWaitMs));
    QTest::qWait(80);
    ASSERT_EQ(service.plans.size(), 1u);
    EXPECT_EQ(service.plans[0].name_pattern, QStringLiteral("{original-stem}_bcd"));
    EXPECT_TRUE(setup.planning());  // until the answer
    EXPECT_TRUE(setup.can_start());  // the service checks again when the batch is created

    BatchPlanInfo plan;
    plan.ok = true;
    plan.outputs = 2;
    plan.renamed = 1;
    plan.excluded = 1;
    plan.exclusions = {BatchPlanExclusion{QStringLiteral("C.dng"), QStringLiteral("Unsupported: ...")}};
    plan.examples = {BatchPlanExample{QStringLiteral("A.CR3"), QStringLiteral("D:\\Out\\A_bcd.jpg"), false, false},
                     BatchPlanExample{QStringLiteral("B.CR3"), QStringLiteral("D:\\Out\\B_bcd_2.jpg"), true, false}};
    plan.estimated_bytes = 2 * 1024 * 1024;
    plan.free_bytes = qint64{10} * 1024 * 1024 * 1024;
    QSignalSpy changed(&setup, &BatchSetupModel::planChanged);
    service.answer_plan(setup.plan_token(), plan);
    EXPECT_FALSE(setup.planning());
    EXPECT_TRUE(setup.plan_valid());
    EXPECT_TRUE(setup.can_start());
    ASSERT_EQ(setup.examples().size(), 2);
    EXPECT_EQ(setup.examples()[1].toMap().value(QStringLiteral("destination")).toString(),
              QStringLiteral("D:\\Out\\B_bcd_2.jpg"));
    EXPECT_FALSE(setup.examples()[1].toMap().value(QStringLiteral("note")).toString().isEmpty());
    EXPECT_EQ(setup.counts_text(),
              QStringLiteral("2 photos will be exported · 1 gets a number added to its name · 1 left out"));
    EXPECT_EQ(setup.exclusions().size(), 1);
    EXPECT_TRUE(setup.space_text().startsWith(QStringLiteral("Up to 2")));
    EXPECT_GE(changed.count(), 1);
}

TEST_F(BatchSetupTest, AnAnswerToASupersededRequestIsIgnored) {
    setup.setActive(true);
    ASSERT_TRUE(QTest::qWaitFor([&] { return service.plans.size() == 1; }, kWaitMs));
    const quint64 first = setup.plan_token();
    setup.setIncludeCapture(false);  // a change while the first plan is computed
    ASSERT_TRUE(QTest::qWaitFor([&] { return service.plans.size() == 2; }, kWaitMs));
    const quint64 second = setup.plan_token();
    ASSERT_NE(first, second);

    BatchPlanInfo stale;
    stale.ok = true;
    stale.outputs = 99;
    service.answer_plan(first, stale);
    EXPECT_EQ(setup.outputs(), 0);
    EXPECT_TRUE(setup.planning());

    BatchPlanInfo fresh;
    fresh.ok = true;
    fresh.outputs = 2;
    service.answer_plan(second, fresh);
    EXPECT_EQ(setup.outputs(), 2);
    EXPECT_FALSE(service.plans[1].include_capture);
}

TEST_F(BatchSetupTest, ReplacingFilesNeedsTheConfirmationAndIsResetForAnotherFolderOrBatch) {
    EXPECT_FALSE(setup.overwrite());
    setup.confirmOverwrite();
    EXPECT_TRUE(setup.overwrite());
    EXPECT_EQ(setup.setup().collisions, BatchCollisions::Overwrite);
    // The confirmation named the folder: another folder needs another confirmation.
    setup.setOutputFolder(local(QStringLiteral("E:/Elsewhere")));
    EXPECT_FALSE(setup.overwrite());
    setup.confirmOverwrite();
    setup.setIncludeGps(true);
    // The next batch starts with unique names and without GPS.
    setup.prepare({1, 2}, {1, 2, 3});
    EXPECT_FALSE(setup.overwrite());
    EXPECT_FALSE(setup.include_gps());
}

TEST_F(BatchSetupTest, PatternsAreCheckedAsTheUserTypes) {
    EXPECT_TRUE(setup.pattern_problem().isEmpty());
    setup.setNamePattern(QStringLiteral("export_{revision}"));
    EXPECT_TRUE(setup.pattern_problem().contains(QStringLiteral("{original-stem} or {sequence}")));
    EXPECT_FALSE(setup.can_start());
    EXPECT_EQ(setup.start(), 0u);
    setup.setNamePattern(QStringLiteral("{original-stem}:{revision}"));
    EXPECT_TRUE(setup.pattern_problem().contains(QStringLiteral("cannot contain")));
    setup.setNamePattern(QStringLiteral("{original-stem}_{date}"));
    EXPECT_TRUE(setup.pattern_problem().contains(QStringLiteral("{date} is not a token")));
    setup.setNamePattern(QStringLiteral("{sequence}"));
    EXPECT_TRUE(setup.pattern_problem().isEmpty());
    setup.resetNamePattern();
    EXPECT_EQ(setup.name_pattern(), QStringLiteral("{original-stem}_Lucent_{revision}"));
}

TEST_F(BatchSetupTest, ArtistCopyrightAndTheFolderAreRememberedButNotGps) {
    QSignalSpy remembered(&setup, &BatchSetupModel::rememberedChanged);
    setup.setArtist(QStringLiteral("Ada"));
    setup.setCopyright(QStringLiteral("(c) Ada"));
    EXPECT_EQ(remembered.count(), 2);
    setup.setIncludeGps(true);
    EXPECT_EQ(remembered.count(), 2);
    const BatchSetup described = setup.setup();
    EXPECT_EQ(described.artist, QStringLiteral("Ada"));
    EXPECT_EQ(described.copyright, QStringLiteral("(c) Ada"));
    EXPECT_TRUE(described.include_gps);

    BatchSetupModel restored(service);
    restored.restore(local(QStringLiteral("D:/Out")), QStringLiteral("{sequence}"), BatchSetupModel::Tiff16, true,
                     QStringLiteral("Ada"), QStringLiteral("(c) Ada"));
    EXPECT_EQ(restored.artist(), QStringLiteral("Ada"));
    EXPECT_EQ(restored.preset(), BatchSetupModel::Tiff16);
    EXPECT_TRUE(restored.display_p3());
    EXPECT_FALSE(restored.include_gps());
}

TEST_F(BatchSetupTest, AFolderBatchStartsBeforeItsPhotosAreListed) {
    setup.prepare_folder(local(QStringLiteral("D:/Shoot")), true);
    setup.setActive(true);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !service.plans.empty(); }, kWaitMs));
    EXPECT_EQ(service.plans.back().input_folder, QStringLiteral("D:/Shoot"));
    EXPECT_TRUE(service.plans.back().items.isEmpty());
    BatchPlanInfo plan;
    plan.folder_not_listed = true;  // none of its photos is in the library yet
    service.answer_plan(setup.plan_token(), plan);
    EXPECT_TRUE(setup.can_start());
    EXPECT_FALSE(setup.counts_text().isEmpty());
    EXPECT_EQ(setup.batch_label(), QStringLiteral("Export of Shoot"));
    EXPECT_NE(setup.start(), 0u);
    ASSERT_EQ(service.submissions.size(), 1u);
    EXPECT_EQ(service.submissions[0].name, QStringLiteral("Export of Shoot"));
    EXPECT_TRUE(service.submissions[0].include_subfolders);
}

TEST_F(BatchSetupTest, APlanErrorBlocksStart) {
    setup.setActive(true);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !service.plans.empty(); }, kWaitMs));
    BatchPlanInfo plan;
    plan.error = QStringLiteral("Nothing in the selection can be exported.");
    service.answer_plan(setup.plan_token(), plan);
    EXPECT_FALSE(setup.can_start());
    EXPECT_EQ(setup.start_hint(), plan.error);
    setup.setOutputFolder({});
    EXPECT_EQ(setup.start_hint(), QStringLiteral("Choose an output folder."));
}

// --- BatchListModel ------------------------------------------------------------------------------

class BatchListTest : public ::testing::Test {
protected:
    RecordingBatchService service;
    BatchListModel model{service};
};

TEST_F(BatchListTest, ListsNewestFirstAndInsertsNewBatchesAtTheTop) {
    service.list({batch(3, BatchState::Completed, 5, 5), batch(1, BatchState::Completed, 1, 1)});
    ASSERT_EQ(model.count(), 2);
    EXPECT_EQ(model.batchIdAt(0), 3u);
    service.change(batch(4, BatchState::Running, 10, 0, 10));
    ASSERT_EQ(model.count(), 3);
    EXPECT_EQ(model.batchIdAt(0), 4u);
    service.change(batch(2, BatchState::Completed, 2, 2));
    EXPECT_EQ(model.batchIdAt(2), 2u);
    EXPECT_EQ(model.rowOfBatch(1), 3);
}

TEST_F(BatchListTest, CountsStageAndCommandsFollowTheStateTransitions) {
    BatchSnapshot s = batch(7, BatchState::Running, 10, 3, 5, 2, 0);
    s.skipped = 0;
    s.excluded = 1;  // left out when it was created
    s.review = 1;
    s.running_jobs = {BatchRunningJob{1, 11, QStringLiteral("A.CR3"), QStringLiteral("Rendering"), 0.42}};
    service.change(s);
    EXPECT_EQ(role(model, 0, BatchListModel::StateKeyRole).toString(), QStringLiteral("running"));
    EXPECT_EQ(role(model, 0, BatchListModel::CompletedRole).toLongLong(), 3);
    EXPECT_EQ(role(model, 0, BatchListModel::RemainingRole).toLongLong(), 7);
    EXPECT_EQ(role(model, 0, BatchListModel::SkippedRole).toLongLong(), 1);
    EXPECT_EQ(role(model, 0, BatchListModel::ReviewRole).toLongLong(), 1);
    EXPECT_EQ(role(model, 0, BatchListModel::StageRole).toString(), QStringLiteral("Rendering A.CR3 (42 %)"));
    EXPECT_TRUE(role(model, 0, BatchListModel::EtaRole).toString().isEmpty());  // no ETA yet
    EXPECT_NEAR(role(model, 0, BatchListModel::ProgressRole).toDouble(), 0.342, 1e-9);
    EXPECT_TRUE(role(model, 0, BatchListModel::CanPauseRole).toBool());
    EXPECT_FALSE(role(model, 0, BatchListModel::CanResumeRole).toBool());
    EXPECT_TRUE(role(model, 0, BatchListModel::CanCancelRole).toBool());
    EXPECT_FALSE(role(model, 0, BatchListModel::CanRetryRole).toBool());
    EXPECT_FALSE(role(model, 0, BatchListModel::CanOpenReportRole).toBool());
    EXPECT_EQ(model.summary(), QStringLiteral("1 running"));
    EXPECT_EQ(model.remaining_photos(), 7);

    s.eta_seconds = 130;
    service.change(s);
    EXPECT_EQ(role(model, 0, BatchListModel::EtaRole).toString(), QStringLiteral("About 2 min left"));

    model.pause(0);
    ASSERT_EQ(service.commands.size(), 1u);
    EXPECT_EQ(service.commands[0], (std::pair<QString, BatchId>{QStringLiteral("pause"), 7}));
    s.state = BatchState::Paused;
    s.running_jobs.clear();
    s.queued = 7;
    s.running = 0;
    s.eta_seconds = -1;
    service.change(s);
    EXPECT_EQ(role(model, 0, BatchListModel::StateKeyRole).toString(), QStringLiteral("paused"));
    EXPECT_FALSE(role(model, 0, BatchListModel::CanPauseRole).toBool());
    EXPECT_TRUE(role(model, 0, BatchListModel::CanResumeRole).toBool());
    EXPECT_EQ(model.summary(), QStringLiteral("1 paused"));
    model.pause(0);  // not possible: nothing sent
    model.resume(0);
    EXPECT_EQ(service.commands.back().first, QStringLiteral("resume"));

    s.state = BatchState::Completed;
    s.completed = 8;
    s.failed = 2;
    s.queued = 0;
    s.finished = QDateTime::currentDateTime();
    service.change(s);
    EXPECT_EQ(role(model, 0, BatchListModel::StateKeyRole).toString(), QStringLiteral("partial"));
    EXPECT_EQ(role(model, 0, BatchListModel::StateLabelRole).toString(), QStringLiteral("Completed with failures"));
    EXPECT_TRUE(role(model, 0, BatchListModel::CanRetryRole).toBool());
    EXPECT_FALSE(role(model, 0, BatchListModel::CanCancelRole).toBool());
    EXPECT_TRUE(role(model, 0, BatchListModel::CanOpenReportRole).toBool());
    EXPECT_TRUE(role(model, 0, BatchListModel::StageRole).toString().isEmpty());
    EXPECT_FALSE(role(model, 0, BatchListModel::IsActiveRole).toBool());
    EXPECT_TRUE(model.summary().isEmpty());
    model.retryFailed(0);
    EXPECT_EQ(service.commands.back(), (std::pair<QString, BatchId>{QStringLiteral("retry"), 7}));
}

TEST_F(BatchListTest, AnAttentionIsShownWithItsTextAndResumeUntilTheBatchMovesOn) {
    BatchSnapshot s = batch(9, BatchState::Paused, 4, 1, 3);
    s.attention = BatchAttention::DestinationFull;
    s.attention_message = QStringLiteral("The drive of D:\\Out is full, so the batch is paused. Free some space on "
                                         "it, then choose Resume.");
    QSignalSpy summary(&model, &BatchListModel::summaryChanged);
    service.change(s);
    EXPECT_TRUE(role(model, 0, BatchListModel::HasAttentionRole).toBool());
    EXPECT_EQ(role(model, 0, BatchListModel::AttentionRole).toString(), s.attention_message);
    EXPECT_EQ(role(model, 0, BatchListModel::StateKeyRole).toString(), QStringLiteral("attention"));
    EXPECT_EQ(role(model, 0, BatchListModel::StateLabelRole).toString(), QStringLiteral("Needs attention"));
    EXPECT_TRUE(role(model, 0, BatchListModel::CanResumeRole).toBool());
    EXPECT_EQ(model.attention_count(), 1);
    EXPECT_EQ(model.summary(), QStringLiteral("1 needs attention"));
    EXPECT_GE(summary.count(), 1);
    EXPECT_TRUE(role(model, 0, BatchListModel::AccessibleLabelRole).toString().contains(s.attention_message));

    model.resume(0);
    EXPECT_EQ(service.commands.back(), (std::pair<QString, BatchId>{QStringLiteral("resume"), 9}));
    s.state = BatchState::Running;
    s.attention = BatchAttention::None;
    s.attention_message.clear();
    service.change(s);
    EXPECT_FALSE(role(model, 0, BatchListModel::HasAttentionRole).toBool());
    EXPECT_EQ(model.attention_count(), 0);
}

TEST_F(BatchListTest, ASingleExportReadsAsOnePhoto) {
    BatchSnapshot s = batch(5, BatchState::Running, 1, 0, 0, 1);
    s.running_jobs = {BatchRunningJob{1, 3, QStringLiteral("A.NEF"), QStringLiteral("Writing"), 0.9}};
    service.change(s);
    EXPECT_TRUE(role(model, 0, BatchListModel::SingleRole).toBool());
    EXPECT_EQ(role(model, 0, BatchListModel::StateLabelRole).toString(), QStringLiteral("Exporting"));
    s.state = BatchState::Completed;
    s.running = 0;
    s.completed = 1;
    s.running_jobs.clear();
    s.last_output = QStringLiteral("D:\\Out\\A_Lucent.jpg");
    service.change(s);
    EXPECT_EQ(role(model, 0, BatchListModel::StateLabelRole).toString(), QStringLiteral("Exported"));
    EXPECT_EQ(role(model, 0, BatchListModel::OutputPathRole).toString(), s.last_output);
    EXPECT_FALSE(role(model, 0, BatchListModel::CanOpenReportRole).toBool());  // no clutter
}

TEST_F(BatchListTest, PreparingSubmissionsAreShownUntilTheyAnswer) {
    model.track_submission(1, QStringLiteral("Export of Shoot"));
    EXPECT_TRUE(model.preparing());
    EXPECT_EQ(model.preparing_name(), QStringLiteral("Export of Shoot"));
    service.progress(1, QStringLiteral("Adding the folder's photos… 3 of 9"), 0.33);
    EXPECT_EQ(model.preparing_text(), QStringLiteral("Adding the folder's photos… 3 of 9"));
    EXPECT_NEAR(model.preparing_progress(), 0.33, 1e-9);
    model.cancelPreparing();
    ASSERT_EQ(service.cancelled_submissions.size(), 1u);
    EXPECT_EQ(service.cancelled_submissions[0], 1u);
    service.finish_submit(1, 0, QStringLiteral("Cancelled."));
    EXPECT_FALSE(model.preparing());
}

TEST(BatchPresentationTest, EtaLabelsRoundToReadableUnits) {
    const QLocale locale(QLocale::English, QLocale::UnitedStates);
    EXPECT_TRUE(presentation::eta_label(-1, locale).isEmpty());
    EXPECT_EQ(presentation::eta_label(12, locale), QStringLiteral("About 15 s left"));
    EXPECT_EQ(presentation::eta_label(95, locale), QStringLiteral("About 2 min left"));
    EXPECT_EQ(presentation::eta_label(3900, locale), QStringLiteral("About 1 h 5 min left"));
}

// --- BatchReportModel ----------------------------------------------------------------------------

BatchReportInfo report_of(BatchId id) {
    BatchReportInfo report;
    report.id = id;
    report.ok = true;
    report.name = QStringLiteral("Export of 3 photos");
    report.folder = QStringLiteral("D:\\Out");
    report.state = BatchState::Completed;
    report.exported = 1;
    report.skipped = 1;
    report.failed = 1;
    report.review = 1;
    BatchReportEntry exported;
    exported.source_name = QStringLiteral("A.CR3");
    exported.outcome = BatchOutcome::Exported;
    exported.output = QStringLiteral("D:\\Out\\A_Lucent_0.jpg");
    exported.reason = QStringLiteral("Exported.");
    exported.review = {QStringLiteral("Saved as A_Lucent_0_2.jpg because A_Lucent_0.jpg already existed.")};
    BatchReportEntry skipped;
    skipped.source_name = QStringLiteral("B.dng");
    skipped.outcome = BatchOutcome::Skipped;
    skipped.reason = QStringLiteral("Lucent cannot render this camera file faithfully yet, so it was not exported.");
    BatchReportEntry failed;
    failed.source_name = QStringLiteral("C.NEF");
    failed.outcome = BatchOutcome::Failed;
    failed.reason = QStringLiteral("The original file could not be found.");
    failed.action = QStringLiteral("Reconnect its drive or relink the photo, then choose Retry failed.");
    report.entries = {exported, skipped, failed};
    return report;
}

TEST(BatchReportModelTest, ShowsTheSelectedBatchsReportWithFilters) {
    RecordingBatchService service;
    BatchReportModel model(service);
    model.show(4);
    ASSERT_EQ(service.reports.size(), 1u);
    EXPECT_TRUE(model.loading());
    service.answer_report(3, report_of(3));  // another batch: ignored
    EXPECT_FALSE(model.has_report());
    service.answer_report(4, report_of(4));
    EXPECT_FALSE(model.loading());
    ASSERT_TRUE(model.has_report());
    EXPECT_EQ(model.count(), 3);
    EXPECT_EQ(model.exported(), 1);
    EXPECT_EQ(model.failed(), 1);
    EXPECT_EQ(role(model, 0, BatchReportModel::OutputNameRole).toString(), QStringLiteral("A_Lucent_0.jpg"));
    EXPECT_TRUE(role(model, 0, BatchReportModel::HasReviewRole).toBool());

    model.setFilter(QStringLiteral("failed"));
    ASSERT_EQ(model.count(), 1);
    EXPECT_EQ(role(model, 0, BatchReportModel::SourceNameRole).toString(), QStringLiteral("C.NEF"));
    EXPECT_EQ(role(model, 0, BatchReportModel::ActionRole).toString(),
              QStringLiteral("Reconnect its drive or relink the photo, then choose Retry failed."));
    model.setFilter(QStringLiteral("review"));
    ASSERT_EQ(model.count(), 1);
    EXPECT_EQ(role(model, 0, BatchReportModel::OutcomeKeyRole).toString(), QStringLiteral("exported"));
    model.setFilter(QStringLiteral("nonsense"));
    EXPECT_EQ(model.filter(), QStringLiteral("all"));
    EXPECT_EQ(model.count(), 3);

    model.clear();
    EXPECT_EQ(model.count(), 0);
    EXPECT_FALSE(model.has_report());
}

TEST(BatchReportModelTest, ARunningBatchsReportIsReadAgainAtMostOncePerInterval) {
    RecordingBatchService service;
    BatchReportModel model(service);
    model.set_refresh_interval(std::chrono::milliseconds(100));
    model.show(4);
    service.answer_report(4, report_of(4));
    ASSERT_EQ(service.reports.size(), 1u);
    // Many photos finish in quick succession.
    for (int done = 1; done <= 20; ++done) {
        BatchSnapshot s = batch(4, BatchState::Running, 40, done, 40 - done);
        service.change(s);
    }
    // One read for all of them.
    ASSERT_TRUE(QTest::qWaitFor([&] { return service.reports.size() == 2; }, kWaitMs));
    QTest::qWait(250);
    EXPECT_EQ(service.reports.size(), 2u);
}

// --- Library selection ---------------------------------------------------------------------------

TEST(LibrarySelectionTest, CtrlShiftAndSelectAllBuildTheSelectionInRowOrder) {
    testing::ScriptedLibraryService library;
    LibraryModel model(library, std::make_shared<GenerationRegistry>());
    library.add({testing::make_item(10, QStringLiteral("A.CR3")), testing::make_item(11, QStringLiteral("B.CR3")),
                 testing::make_item(12, QStringLiteral("C.CR3")), testing::make_item(13, QStringLiteral("D.CR3"))});
    QSignalSpy changed(&model, &LibraryModel::selectionChanged);
    model.selectOnly(1);
    EXPECT_EQ(model.selected_items(), (QList<ItemId>{11}));
    model.toggleSelected(3);
    EXPECT_EQ(model.selected_items(), (QList<ItemId>{11, 13}));
    EXPECT_TRUE(role(model, 3, LibraryModel::InSelectionRole).toBool());
    model.selectRange(0);  // from the anchor (row 3) to row 0
    EXPECT_EQ(model.selected_items(), (QList<ItemId>{10, 11, 12, 13}));
    model.toggleSelected(2);
    EXPECT_EQ(model.selected_count(), 3);
    model.clearSelection();
    EXPECT_EQ(model.selected_count(), 0);
    model.selectAll();
    EXPECT_EQ(model.selected_items(), model.all_items());
    EXPECT_GE(changed.count(), 5);
}

// --- AppController -------------------------------------------------------------------------------

class BatchControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        scripted = testing::make_scripted_services();
        batches = scripted.batches;
        library = scripted.library;
        controller = std::make_unique<AppController>(std::move(scripted.services));
        controller->set_url_opener([this](const QUrl& url) {
            opened.push_back(url);
            return true;
        });
        library->add({testing::make_item(1, QStringLiteral("A.CR3")), testing::make_item(2, QStringLiteral("B.CR3")),
                      testing::make_item(3, QStringLiteral("C.CR3"))});
    }

    testing::ScriptedServices scripted;
    RecordingBatchService* batches = nullptr;
    testing::ScriptedLibraryService* library = nullptr;
    std::unique_ptr<AppController> controller;
    std::vector<QUrl> opened;
};

TEST_F(BatchControllerTest, OpenBatchExportPreparesTheSelectionAndAsksForTheDialog) {
    QSignalSpy requested(controller.get(), &AppController::batchExportRequested);
    controller->library()->selectOnly(0);
    controller->library()->toggleSelected(2);
    ASSERT_TRUE(controller->openBatchExport());
    EXPECT_EQ(requested.count(), 1);
    EXPECT_EQ(controller->batch_setup()->selection_count(), 2);
    EXPECT_EQ(controller->batch_setup()->all_count(), 3);
    EXPECT_EQ(controller->batch_setup()->scope(), BatchSetupModel::Selection);

    batches->available = false;
    EXPECT_FALSE(controller->openBatchExport());
    EXPECT_EQ(controller->status_message(), QStringLiteral("Not available"));
}

TEST_F(BatchControllerTest, StartBatchShowsItPreparingThenReportsItsOutcome) {
    ASSERT_TRUE(controller->openBatchExport());
    controller->batch_setup()->setOutputFolder(QUrl::fromLocalFile(QStringLiteral("D:/Out")));
    ASSERT_TRUE(controller->startBatch());  // the dialog is not open: no plan needed
    ASSERT_EQ(batches->submissions.size(), 1u);
    EXPECT_EQ(batches->submissions[0].items, (QList<ItemId>{1, 2, 3}));
    EXPECT_EQ(controller->current_view(), AppController::Exports);
    EXPECT_TRUE(controller->batches()->preparing());
    EXPECT_EQ(controller->last_output_folder(), QUrl::fromLocalFile(QStringLiteral("D:/Out")));

    const quint64 token = controller->last_batch_token();
    batches->finish_submit(token, 12);
    EXPECT_FALSE(controller->batches()->preparing());
    EXPECT_EQ(controller->status_message(), QStringLiteral("Started Export of 3 photos"));

    BatchSnapshot s = batch(12, BatchState::Running, 3, 1, 2);
    batches->change(s);
    EXPECT_EQ(controller->active_export_count(), 2);
    s.state = BatchState::Completed;
    s.completed = 2;
    s.failed = 1;
    s.queued = 0;
    batches->change(s);
    EXPECT_EQ(controller->status_message(), QStringLiteral("Started Export of 3 photos"));  // facts not read yet
    s.review = 1;
    s.excluded = 1;
    batches->change(s);
    EXPECT_EQ(controller->status_message(),
              QStringLiteral("Export of 3 photos finished: 2 of 3 exported · 1 skipped · 1 failed · 1 for review"));
    EXPECT_EQ(controller->active_export_count(), 0);

    // Opening its folder and its report.
    ASSERT_TRUE(controller->openBatchFolder(0));
    EXPECT_EQ(opened.back(), QUrl::fromLocalFile(QStringLiteral("D:\\Out")));
    controller->openBatchReport(0);
    ASSERT_EQ(batches->report_files.size(), 1u);
    batches->answer_report_file(12, QStringLiteral("D:\\Out\\Lucent-batch-12-report.html"));
    EXPECT_EQ(opened.back(), QUrl::fromLocalFile(QStringLiteral("D:\\Out\\Lucent-batch-12-report.html")));
}

TEST_F(BatchControllerTest, AFailedSubmissionAndAFailedCommandAreReported) {
    ASSERT_TRUE(controller->openBatchExport());
    controller->batch_setup()->setOutputFolder(QUrl::fromLocalFile(QStringLiteral("D:/Out")));
    ASSERT_TRUE(controller->startBatch());
    batches->finish_submit(controller->last_batch_token(), 0,
                           QStringLiteral("Lucent is not allowed to create files in the output folder."));
    EXPECT_EQ(controller->status_message(),
              QStringLiteral(
                  "Could not start Export of 3 photos: Lucent is not allowed to create files in the output "
                  "folder."));
    batches->fail_command(3, QStringLiteral("The batch has changed meanwhile; try again."));
    EXPECT_EQ(controller->status_message(), QStringLiteral("The batch has changed meanwhile; try again."));
}

TEST_F(BatchControllerTest, BusyFollowsTheBatchService) {
    EXPECT_FALSE(controller->busy());
    QSignalSpy busy(controller.get(), &AppController::busyChanged);
    batches->busy = true;
    emit batches->busy_changed(true);
    EXPECT_TRUE(controller->busy());
    EXPECT_EQ(busy.count(), 1);
}

}  // namespace
}  // namespace lucent::ui
