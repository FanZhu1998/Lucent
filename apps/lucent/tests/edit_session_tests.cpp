// EditSession (the Edit view's model, W6) against a recording EditService: a gesture is one revision,
// key repeats are one revision, undo/redo follow the service's state, a read-only photo refuses edits
// and says why, paste results list what was skipped, tone-curve points stay monotone, and frames
// are shown in order with their latency measured.

#include <gtest/gtest.h>

#include <memory>

#include <QSignalSpy>
#include <QTest>

#include "models/edit_history_model.hpp"
#include "models/edit_session.hpp"
#include "test_support.hpp"

namespace lucent::ui {
namespace {

using testing::RecordingEditService;

class EditSessionTest : public ::testing::Test {
protected:
    EditSessionTest() : store(std::make_shared<FrameStore>()), session(service, store) {}

    // Opens item 7 as shot and answers the open with a writable state.
    void open(ItemId item = 7) {
        session.set_item(item);
        EditState state;
        state.item = item;
        service.emit_state(state);
    }

    [[nodiscard]] static EditFramePtr frame(ItemId item, quint64 sequence, quint64 token, FramePhase phase,
                                            bool before = false) {
        auto out = std::make_shared<EditFrame>();
        out->item = item;
        out->sequence = sequence;
        out->token = token;
        out->phase = phase;
        out->before = before;
        out->image = testing::solid_image(QSize(32, 24));
        out->info = std::make_shared<RenderInfo>();
        return out;
    }

    RecordingEditService service;
    std::shared_ptr<FrameStore> store;
    EditSession session;
};

TEST_F(EditSessionTest, OpeningAPhotoOpensItsSessionAndClosingCommitsAPendingGesture) {
    open(7);
    ASSERT_EQ(service.count("open"), 1);
    EXPECT_EQ(service.calls_named("open").front().item, 7u);
    EXPECT_TRUE(session.active());
    EXPECT_TRUE(session.can_edit());

    session.beginGesture();
    session.setValue(QStringLiteral("exposure"), 0.5);
    session.set_item(8);  // the filmstrip moved on mid-gesture
    EXPECT_EQ(service.count("commit"), 1);
    ASSERT_EQ(service.count("close"), 1);
    EXPECT_EQ(service.calls_named("close").front().item, 7u);
    EXPECT_EQ(service.calls_named("open").back().item, 8u);
    EXPECT_FALSE(session.ready());  // waiting for item 8's state
    EXPECT_FALSE(session.gesture_active());
}

TEST_F(EditSessionTest, ADragIsOneRevision) {
    open();
    session.beginGesture();
    for (int i = 1; i <= 5; ++i) {
        session.setPosition(QStringLiteral("exposure"), 0.5 + 0.02 * i);
    }
    EXPECT_EQ(service.count("set_transient"), 5);
    EXPECT_EQ(service.count("commit"), 0);
    EXPECT_EQ(service.count("apply"), 0);
    session.endGesture();
    EXPECT_EQ(service.count("commit"), 1);
    EXPECT_EQ(service.count("apply"), 0);
    // Exposure spans -5..+5 EV: position 0.6 is +1 EV.
    EXPECT_DOUBLE_EQ(session.value(QStringLiteral("exposure")), 1.0);
    EXPECT_DOUBLE_EQ(service.calls_named("set_transient").back().settings.exposure_ev, 1.0);

    // A press and release without a move writes nothing.
    session.beginGesture();
    session.endGesture();
    EXPECT_EQ(service.count("commit"), 1);
}

TEST_F(EditSessionTest, HeldArrowKeysAreOneRevisionAndShiftTakesCoarseSteps) {
    open();
    for (int i = 0; i < 4; ++i) {
        session.nudge(QStringLiteral("exposure"), 1, false);
    }
    EXPECT_EQ(service.count("set_transient"), 4);
    EXPECT_NEAR(session.value(QStringLiteral("exposure")), 0.2, 1e-12);
    session.endNudge();  // the key was released
    EXPECT_EQ(service.count("commit"), 1);

    session.nudge(QStringLiteral("contrast"), -1, true);  // Shift: 10 of 100
    EXPECT_DOUBLE_EQ(session.value(QStringLiteral("contrast")), -10.0);
    EXPECT_DOUBLE_EQ(service.calls_named("set_transient").back().settings.contrast, -0.1);
    // Without a release (a lost key-up), a pause ends the gesture.
    ASSERT_TRUE(QTest::qWaitFor([this] { return service.count("commit") == 2; }, EditSession::kNudgeCommitMs * 4));
    EXPECT_EQ(service.count("apply"), 0);
}

TEST_F(EditSessionTest, TypedValuesAndResetsAreOneStepEachAndValuesAreClamped) {
    open();
    EXPECT_TRUE(session.setValueText(QStringLiteral("exposure"), QStringLiteral("+1.25 EV")));
    ASSERT_EQ(service.count("apply"), 1);
    EXPECT_DOUBLE_EQ(service.calls_named("apply").back().settings.exposure_ev, 1.25);
    EXPECT_EQ(session.valueText(QStringLiteral("exposure")), QStringLiteral("+1.25"));

    EXPECT_FALSE(session.setValueText(QStringLiteral("contrast"), QStringLiteral("lots")));
    EXPECT_EQ(service.count("apply"), 1);

    EXPECT_TRUE(session.setValueText(QStringLiteral("saturation"), QStringLiteral("250")));
    EXPECT_DOUBLE_EQ(service.calls_named("apply").back().settings.saturation, 1.0);  // clamped to +100

    session.resetValue(QStringLiteral("exposure"));
    EXPECT_EQ(service.count("apply"), 3);
    EXPECT_DOUBLE_EQ(service.calls_named("apply").back().settings.exposure_ev, 0.0);
    EXPECT_EQ(service.count("set_transient"), 0);
    EXPECT_EQ(service.count("commit"), 0);
}

TEST_F(EditSessionTest, UndoAndRedoFollowTheStateAndNewerLocalValuesAreKept) {
    open();
    EXPECT_FALSE(session.can_undo());
    EXPECT_FALSE(session.can_redo());

    session.setValue(QStringLiteral("exposure"), 1.0);  // token 1
    session.setValue(QStringLiteral("exposure"), 2.0);  // token 2
    const quint64 first = service.calls_named("apply").front().token;
    const quint64 second = service.calls_named("apply").back().token;
    EditSettings one;
    one.exposure_ev = 1.0;
    service.answer(7, first, one, /*can_undo=*/true);
    // The answer to the first call does not move the slider back while the second is outstanding.
    EXPECT_DOUBLE_EQ(session.value(QStringLiteral("exposure")), 2.0);
    EXPECT_TRUE(session.can_undo());
    EditSettings two;
    two.exposure_ev = 2.0;
    service.answer(7, second, two, true);
    EXPECT_DOUBLE_EQ(session.value(QStringLiteral("exposure")), 2.0);

    QSignalSpy state_changes(&session, &EditSession::stateChanged);
    session.undo();
    ASSERT_EQ(service.count("undo"), 1);
    service.answer(7, service.calls_named("undo").back().token, one, true, /*can_redo=*/true);
    EXPECT_DOUBLE_EQ(session.value(QStringLiteral("exposure")), 1.0);
    EXPECT_TRUE(session.can_redo());
    EXPECT_GE(state_changes.count(), 1);
    session.redo();
    EXPECT_EQ(service.count("redo"), 1);

    session.resetToAsShot();
    EXPECT_EQ(service.count("reset_to_as_shot"), 1);
}

TEST_F(EditSessionTest, ACommandDuringADragNeverTurnsTheRestOfItIntoARevisionPerMove) {
    open();
    // Home on the pressed slider (or a double-click reset): what the drag changed so far is
    // committed, the reset and the rest of the drag are one more gesture.
    session.beginGesture();
    session.setPosition(QStringLiteral("exposure"), 0.55);
    session.setPosition(QStringLiteral("exposure"), 0.6);
    session.resetValue(QStringLiteral("exposure"));
    EXPECT_EQ(service.count("commit"), 1);
    EXPECT_TRUE(session.gesture_active());
    for (int i = 1; i <= 10; ++i) {
        session.setPosition(QStringLiteral("exposure"), 0.5 + 0.01 * i);
    }
    EXPECT_EQ(service.count("apply"), 0);
    session.endGesture();
    EXPECT_EQ(service.count("commit"), 2);
    EXPECT_EQ(service.count("apply"), 0);
    EXPECT_FALSE(session.gesture_active());
    const quint64 last_commit = service.calls_named("commit").back().token;
    EditSettings current;
    current.exposure_ev = 1.0;
    service.answer(7, last_commit, current, /*can_undo=*/true);

    // Ctrl+Z mid-drag: the drag so far is one revision, then the undo; moves made before the undo
    // is answered would build on the undone values and are not sent; the rest of the drag after the
    // answer is one more revision, committed on release.
    session.beginGesture();
    session.setPosition(QStringLiteral("exposure"), 0.7);
    session.setPosition(QStringLiteral("exposure"), 0.72);
    session.undo();
    EXPECT_EQ(service.count("commit"), 3);
    ASSERT_EQ(service.count("undo"), 1);
    const int transients = service.count("set_transient");
    session.setPosition(QStringLiteral("exposure"), 0.75);
    session.setPosition(QStringLiteral("exposure"), 0.8);
    EXPECT_EQ(service.count("set_transient"), transients);
    EditSettings undone;
    undone.exposure_ev = 1.0;
    undone.contrast = 0.0;
    service.answer(7, service.calls_named("undo").back().token, undone, true, true);
    EXPECT_DOUBLE_EQ(session.value(QStringLiteral("exposure")), 1.0);  // the gesture took the new values
    for (int i = 1; i <= 5; ++i) {
        session.setPosition(QStringLiteral("exposure"), 0.6 + 0.01 * i);
    }
    EXPECT_EQ(service.count("set_transient"), transients + 5);
    session.endGesture();
    EXPECT_EQ(service.count("commit"), 4);
    EXPECT_EQ(service.count("apply"), 0);
}

TEST_F(EditSessionTest, EditsWaitForAHistoryCommandToBeAnswered) {
    open();
    session.setValue(QStringLiteral("exposure"), 1.0);
    EditSettings one;
    one.exposure_ev = 1.0;
    service.answer(7, service.calls_named("apply").back().token, one, true);
    session.setValue(QStringLiteral("contrast"), 20.0);
    EditSettings two = one;
    two.contrast = 0.2;
    service.answer(7, service.calls_named("apply").back().token, two, true);

    session.undo();
    // An arrow key and a typed value before the undo's state arrives: built on the undone contrast,
    // they would bring it back; they are not sent.
    session.nudge(QStringLiteral("exposure"), 1, false);
    session.endNudge();
    static_cast<void>(session.setValueText(QStringLiteral("saturation"), QStringLiteral("10")));
    EXPECT_EQ(service.count("set_transient"), 0);
    EXPECT_EQ(service.count("apply"), 2);
    EXPECT_EQ(service.count("commit"), 0);
    EXPECT_DOUBLE_EQ(session.value(QStringLiteral("contrast")), 20.0);  // until the answer

    service.answer(7, service.calls_named("undo").back().token, one, true, true);
    EXPECT_DOUBLE_EQ(session.value(QStringLiteral("contrast")), 0.0);
    session.nudge(QStringLiteral("exposure"), 1, false);
    session.endNudge();
    ASSERT_EQ(service.count("set_transient"), 1);
    EXPECT_DOUBLE_EQ(service.calls_named("set_transient").back().settings.contrast, 0.0);
    EXPECT_NEAR(service.calls_named("set_transient").back().settings.exposure_ev, 1.05, 1e-12);
    EXPECT_EQ(service.count("commit"), 1);

    // A paste is applied by the service too: edits wait for its report.
    service.emit_copied(7, 0xFF);
    session.pasteSettings();
    ASSERT_EQ(service.count("paste_settings"), 1);
    session.setValue(QStringLiteral("vibrance"), 30.0);
    EXPECT_EQ(service.count("apply"), 2);
    service.emit_pasted(PasteReport{});
    session.setValue(QStringLiteral("vibrance"), 30.0);
    EXPECT_EQ(service.count("apply"), 3);
}

TEST_F(EditSessionTest, AReadOnlyRecipeRefusesEditsAndSaysWhy) {
    session.set_item(7);
    EditState state;
    state.item = 7;
    state.revision = 3;
    state.revision_number = 3;
    state.can_undo = true;
    state.read_only = true;
    state.read_only_reason = QStringLiteral("This photo's edit was made by a newer version of Lucent.");
    state.history_available = true;  // only the recipe is unreadable: other revisions can be shown
    service.emit_state(state);

    EXPECT_TRUE(session.read_only());
    EXPECT_FALSE(session.can_edit());
    EXPECT_EQ(session.read_only_reason(), state.read_only_reason);
    session.setValue(QStringLiteral("exposure"), 1.0);
    session.beginGesture();
    session.setPosition(QStringLiteral("contrast"), 0.9);
    session.endGesture();
    session.setLocked(QStringLiteral("exposure"), true);
    EXPECT_EQ(session.addCurvePoint(0.5), -1);
    EXPECT_EQ(service.count("apply") + service.count("set_transient") + service.count("commit") +
                  service.count("set_lock"),
              0);
    // Moving away from the unreadable revision stays possible.
    EXPECT_TRUE(session.can_undo());
    session.undo();
    EXPECT_EQ(service.count("undo"), 1);

    // A library saved by a newer Lucent refuses every change, history included.
    state.history_available = false;
    service.emit_state(state);
    EXPECT_FALSE(session.can_undo());
    session.undo();
    session.resetToAsShot();
    EXPECT_EQ(service.count("undo"), 1);
    EXPECT_EQ(service.count("reset_to_as_shot"), 0);
}

TEST_F(EditSessionTest, LocksAreRevisionsOfTheirControl) {
    open();
    EXPECT_FALSE(session.isLocked(QStringLiteral("exposure")));
    session.setLocked(QStringLiteral("exposure"), true);
    ASSERT_EQ(service.count("set_lock"), 1);
    EXPECT_EQ(service.calls_named("set_lock").front().revision, static_cast<quint64>(EditControl::Exposure));
    EXPECT_TRUE(service.calls_named("set_lock").front().flag);
    EXPECT_TRUE(session.isLocked(QStringLiteral("exposure")));
    EXPECT_EQ(session.control(QStringLiteral("temperature")).value(QStringLiteral("lockKey")).toString(),
              QStringLiteral("whiteBalance"));
}

TEST_F(EditSessionTest, PasteResultsListWhatWasSkippedAndWhy) {
    open();
    EXPECT_FALSE(session.has_clipboard());
    session.copySettings({QStringLiteral("whiteBalance"), QStringLiteral("exposure"), QStringLiteral("tone")});
    ASSERT_EQ(service.count("copy_settings"), 1);
    EXPECT_EQ(service.calls_named("copy_settings").front().categories,
              category_bit(SettingsCategory::WhiteBalance) | category_bit(SettingsCategory::Exposure) |
                  category_bit(SettingsCategory::Tone));
    service.emit_copied(7, service.calls_named("copy_settings").front().categories);
    EXPECT_TRUE(session.has_clipboard());
    EXPECT_EQ(session.message(), QStringLiteral("Copied White balance, Exposure, Tone."));

    QSignalSpy notices(&session, &EditSession::notice);
    session.pasteSettings();
    ASSERT_EQ(service.count("paste_settings"), 1);
    PasteReport report;
    report.targets = 1;
    report.changed = 1;
    report.applied = {SettingsCategory::Exposure, SettingsCategory::Tone};
    report.skipped = {QStringLiteral("White balance: the copied balance is specific to the Canon EOS R5's sensor "
                                     "and this photo is from Sony ILCE-7M4.")};
    service.emit_pasted(report);
    EXPECT_TRUE(session.message().startsWith(QStringLiteral("Pasted Exposure, Tone.")));
    EXPECT_TRUE(session.message().contains(QStringLiteral("Not pasted — White balance: the copied balance")));
    ASSERT_EQ(notices.count(), 1);
    EXPECT_EQ(notices.front().front().toString(), session.message());

    PasteReport failed;
    failed.targets = 1;
    failed.errors = {QStringLiteral("Nothing has been copied yet.")};
    service.emit_pasted(failed);
    EXPECT_EQ(session.message(), QStringLiteral("Could not paste: Nothing has been copied yet."));
}

TEST_F(EditSessionTest, TemperatureUsesReciprocalKelvinAndStartsFromTheAsShotWhite) {
    open();
    WhiteBalanceInfo info;
    info.known = true;
    info.kelvin_available = true;
    info.as_shot_temperature_k = 5503.0;
    info.as_shot_tint = 7.4;
    service.emit_white_balance(7, info);
    EXPECT_EQ(session.white_balance_controls(), QStringLiteral("kelvin"));
    EXPECT_EQ(session.white_balance_mode(), QStringLiteral("asShot"));
    EXPECT_DOUBLE_EQ(session.value(QStringLiteral("temperature")), 5503.0);
    EXPECT_DOUBLE_EQ(session.value(QStringLiteral("tint")), 7.0);  // shown in whole units

    // Reciprocal scale: 1/K is linear in the slider position (2000 K at 0, 50000 K at 1).
    session.setPosition(QStringLiteral("temperature"), 0.5);
    const double expected = 1.0 / (0.5 / 2000.0 + 0.5 / 50000.0);
    EXPECT_NEAR(session.value(QStringLiteral("temperature")), std::round(expected), 1e-9);
    EXPECT_NEAR(session.position(QStringLiteral("temperature")), 0.5, 1e-3);
    ASSERT_EQ(service.count("apply"), 1);
    const EditSettings applied = service.calls_named("apply").back().settings;
    EXPECT_EQ(applied.white_balance, WhiteBalanceMode::Kelvin);
    EXPECT_DOUBLE_EQ(applied.tint, 7.0);  // the as-shot tint carried over

    session.resetWhiteBalance();
    EXPECT_EQ(service.calls_named("apply").back().settings.white_balance, WhiteBalanceMode::AsShot);
}

TEST_F(EditSessionTest, WithoutKelvinTheCameraChannelsAreAdjustedRelativeToAsShot) {
    open();
    WhiteBalanceInfo info;
    info.known = true;
    info.kelvin_available = false;
    info.as_shot_neutral = {2.0, 1.0, 1.5};
    info.explanation = QStringLiteral("No colour temperature for this camera.");
    service.emit_white_balance(7, info);
    EXPECT_EQ(session.white_balance_controls(), QStringLiteral("neutral"));
    EXPECT_EQ(session.white_balance_note(), info.explanation);
    EXPECT_DOUBLE_EQ(session.value(QStringLiteral("redBlue")), 0.0);

    session.setValue(QStringLiteral("redBlue"), 50.0);  // +1 EV red against blue
    const EditSettings warmer = service.calls_named("apply").back().settings;
    EXPECT_EQ(warmer.white_balance, WhiteBalanceMode::CameraNeutral);
    EXPECT_NEAR(warmer.neutral[0], 2.0 * std::sqrt(2.0), 1e-12);
    EXPECT_DOUBLE_EQ(warmer.neutral[1], 1.0);
    EXPECT_NEAR(warmer.neutral[2], 1.5 / std::sqrt(2.0), 1e-12);
    EXPECT_NEAR(session.value(QStringLiteral("redBlue")), 50.0, 1e-9);
    EXPECT_NEAR(session.value(QStringLiteral("greenMagenta")), 0.0, 1e-9);

    session.setValue(QStringLiteral("greenMagenta"), -20.0);
    EXPECT_NEAR(session.value(QStringLiteral("redBlue")), 50.0, 1e-9);
    EXPECT_NEAR(session.value(QStringLiteral("greenMagenta")), -20.0, 1e-9);
}

TEST_F(EditSessionTest, CurvePointsStayMonotoneAndTheEndPointsStay) {
    open();
    const QList<QPointF> points{{0.0, 0.0}, {0.3, 0.25}, {0.6, 0.7}, {1.0, 1.0}};
    // x stays strictly between the neighbours (a gap apart), y between their y.
    EXPECT_EQ(EditSession::constrain_curve_point(points, 1, QPointF(0.9, 0.95)), QPointF(0.59, 0.7));
    EXPECT_EQ(EditSession::constrain_curve_point(points, 1, QPointF(-1.0, -1.0)), QPointF(0.01, 0.0));
    EXPECT_EQ(EditSession::constrain_curve_point(points, 0, QPointF(0.5, 0.5)), QPointF(0.29, 0.25));
    EXPECT_EQ(EditSession::constrain_curve_point(points, 3, QPointF(0.1, 2.0)), QPointF(0.61, 1.0));

    session.beginGesture();
    const int index = session.addCurvePoint(0.5);
    ASSERT_EQ(index, 1);
    EXPECT_NEAR(session.settings().tone_curve[1].y(), 0.5, 1e-9);  // on the (identity) curve
    const QPointF moved = session.moveCurvePoint(1, 0.5, 0.65);
    EXPECT_EQ(moved, QPointF(0.5, 0.65));
    session.moveCurvePoint(1, 1.5, 1.5);  // clamped: before the end point, not above it
    EXPECT_EQ(session.settings().tone_curve[1], QPointF(0.99, 1.0));
    session.endGesture();
    EXPECT_EQ(service.count("commit"), 1);  // add + moves: one revision

    EXPECT_EQ(session.addCurvePoint(0.995), -1);  // too close to a point
    EXPECT_FALSE(session.removeCurvePoint(0));     // end points are moved, not removed
    EXPECT_TRUE(session.removeCurvePoint(1));
    EXPECT_EQ(session.settings().tone_curve.size(), 2);
    EXPECT_EQ(service.count("apply"), 1);
    session.resetCurve();
    EXPECT_TRUE(session.curve_is_identity());
}

TEST_F(EditSessionTest, FramesAreShownInOrderAndTheirLatencyIsMeasured) {
    open();
    session.beginGesture();
    session.setValue(QStringLiteral("exposure"), 0.5);
    const quint64 token = service.calls_named("set_transient").back().token;
    QTest::qWait(20);
    service.emit_frame(frame(7, 5, token, FramePhase::Quick));
    const QString shown = session.frame_source();
    EXPECT_EQ(shown, QStringLiteral("image://frame/7/5"));
    EXPECT_EQ(session.frame_phase(), QStringLiteral("quick"));
    EXPECT_FALSE(store->find(FrameStore::Kind::Frame, 7, 5).isNull());
    ASSERT_EQ(session.published_latencies().size(), 1u);
    EXPECT_GE(session.published_latencies().front(), 15.0);

    service.emit_frame(frame(7, 4, token, FramePhase::Final));  // older: ignored
    service.emit_frame(frame(8, 9, token, FramePhase::Final));  // another photo: ignored
    EXPECT_EQ(session.frame_source(), shown);

    session.noteFrameShown(shown);
    ASSERT_EQ(session.shown_latencies().size(), 1u);
    EXPECT_GE(session.last_latency_ms(), session.published_latencies().front());

    service.emit_frame(frame(7, 6, 0, FramePhase::Quick, /*before=*/true));
    EXPECT_EQ(session.before_source(), QStringLiteral("image://before/7/6"));
    EXPECT_EQ(session.frame_source(), shown);  // the as-shot frame does not replace the current one
    session.endGesture();

    session.set_compare(true);
    EXPECT_TRUE(service.calls_named("set_comparison").back().flag);
    session.set_compare(false);
    EXPECT_FALSE(service.calls_named("set_comparison").back().flag);
    session.set_clipping(true);
    EXPECT_TRUE(service.calls_named("set_clipping").back().flag);
}

TEST(EditHistoryModel, ListsRevisionsNewestFirstWithAsShotLast) {
    EditHistoryModel model;
    EditHistoryEntry first;
    first.revision = 11;
    first.number = 1;
    first.origin = QStringLiteral("user");
    first.changed = {EditControl::Exposure, EditControl::Contrast};
    first.created = QDateTime::currentDateTimeUtc();
    first.on_current_path = true;
    EditHistoryEntry reset;
    reset.revision = 12;
    reset.number = 2;
    reset.parent_number = 1;
    reset.origin = QStringLiteral("reset");
    reset.created = first.created;
    reset.current = true;
    reset.on_current_path = true;
    EditHistoryEntry pasted;
    pasted.revision = 13;
    pasted.number = 3;
    pasted.parent_number = 1;
    pasted.origin = QStringLiteral("paste");
    pasted.changed = {EditControl::ToneCurve};
    pasted.created = first.created;

    model.set_entries({first, reset, pasted}, false);
    ASSERT_EQ(model.count(), 4);
    const auto title = [&model](int row) { return model.data(model.index(row), EditHistoryModel::TitleRole).toString(); };
    EXPECT_EQ(title(0), QStringLiteral("Pasted: Tone curve"));
    EXPECT_EQ(title(1), QStringLiteral("Reset to as shot"));
    EXPECT_EQ(title(2), QStringLiteral("Exposure, Contrast"));
    EXPECT_EQ(title(3), QStringLiteral("As shot"));
    EXPECT_EQ(model.current_row(), 1);
    EXPECT_FALSE(model.data(model.index(0), EditHistoryModel::OnCurrentPathRole).toBool());  // an undone branch
    EXPECT_EQ(model.revision_at(3), 0u);
    EXPECT_EQ(model.revision_at(2), 11u);
    EXPECT_TRUE(model.data(model.index(0), EditHistoryModel::AccessibleLabelRole).toString().endsWith(
        QStringLiteral(", undone")));
    EXPECT_TRUE(model.data(model.index(2), EditHistoryModel::DetailRole).toString().startsWith(QStringLiteral("#1 · ")));
}

// ---- Auto (W8) ----------------------------------------------------------------------------------

TEST_F(EditSessionTest, AutoCommitsAPendingGestureFirstAndShowsItsRunUntilItEnds) {
    open(7);
    AutoSettingsModel& automatic = *session.automatic();
    EXPECT_TRUE(session.can_run_auto());
    EXPECT_FALSE(automatic.running());
    EXPECT_FALSE(automatic.has_summary());

    // A drag in progress: its change is committed before Auto reads the photo's settings.
    session.beginGesture();
    session.setValue(QStringLiteral("contrast"), 20.0);
    QSignalSpy notices(&session, &EditSession::notice);
    session.runAuto();
    ASSERT_EQ(service.count("run_auto"), 1);
    ASSERT_EQ(service.count("commit"), 1);
    std::size_t commit_at = 0;
    std::size_t auto_at = 0;
    for (std::size_t i = 0; i < service.calls.size(); ++i) {
        commit_at = service.calls[i].name == u"commit" ? i : commit_at;
        auto_at = service.calls[i].name == u"run_auto" ? i : auto_at;
    }
    EXPECT_LT(commit_at, auto_at);
    EXPECT_TRUE(automatic.running());
    EXPECT_FALSE(session.can_run_auto());  // one run per photo
    session.runAuto();
    EXPECT_EQ(service.count("run_auto"), 1);
    EXPECT_EQ(automatic.stage_text(), QStringLiteral("Waiting to start…"));

    // Waiting for memory says so (other photos are being rendered).
    service.emit_auto_progress(AutoProgress{7, AutoStage::WaitingForMemory, 0.0});
    EXPECT_TRUE(automatic.stage_text().startsWith(QStringLiteral("Waiting for memory"))) << automatic.stage_text().toStdString();

    // Progress of this photo is shown; another photo's is not.
    service.emit_auto_progress(AutoProgress{7, AutoStage::Checking, 0.6});
    service.emit_auto_progress(AutoProgress{8, AutoStage::Analysing, 0.9});
    EXPECT_EQ(automatic.stage_text(), QStringLiteral("Checking the result at full size…"));
    EXPECT_DOUBLE_EQ(automatic.progress(), 0.6);

    // Cancelling is asked of the service; the run says so until it ends.
    automatic.cancel();
    EXPECT_EQ(service.count("cancel_auto"), 1);
    EXPECT_TRUE(automatic.cancelling());
    EXPECT_EQ(automatic.stage_text(), QStringLiteral("Cancelling…"));

    AutoOutcome outcome;
    outcome.item = 7;
    outcome.cancelled = true;
    outcome.message = QStringLiteral("Auto was cancelled; nothing was changed.");
    service.emit_auto_finished(outcome);
    EXPECT_FALSE(automatic.running());
    EXPECT_FALSE(automatic.cancelling());
    EXPECT_EQ(automatic.last_result(), outcome.message);
    EXPECT_FALSE(automatic.last_result_failed());  // a cancellation is not a failure
    EXPECT_TRUE(session.can_run_auto());
    ASSERT_EQ(notices.count(), 1);
    EXPECT_EQ(notices.at(0).at(0).toString(), outcome.message);

    // A run that saved nothing for a reason is shown as such.
    session.runAuto();
    AutoOutcome failed;
    failed.item = 7;
    failed.message =
        QStringLiteral("No automatic edit passed Lucent's full-size quality check, so nothing was changed.");
    service.emit_auto_finished(failed);
    EXPECT_TRUE(automatic.last_result_failed());
    EXPECT_TRUE(automatic.last_result_worth_showing());

    // A result saved and made current is the summary itself; one kept as a branch is news.
    session.runAuto();
    AutoOutcome saved;
    saved.item = 7;
    saved.saved = true;
    saved.made_current = true;
    saved.revision_number = 2;
    saved.message = QStringLiteral("Auto saved its settings as revision 2.");
    service.emit_auto_finished(saved);
    EXPECT_FALSE(automatic.last_result_failed());
    EXPECT_FALSE(automatic.last_result_worth_showing());
    session.runAuto();
    saved.made_current = false;
    service.emit_auto_finished(saved);
    EXPECT_TRUE(automatic.last_result_worth_showing());
}

TEST_F(EditSessionTest, TheAutoSummaryDescribesTheCurrentAutomaticRevisionWithItsValues) {
    open(7);
    AutoSettingsModel& automatic = *session.automatic();
    // The automatic revision is current: its settings are the committed ones.
    EditState state;
    state.item = 7;
    state.revision = 41;
    state.revision_number = 3;
    state.settings.exposure_ev = 0.75;
    state.settings.contrast = 0.12;
    state.settings.white_balance = WhiteBalanceMode::Kelvin;
    state.settings.temperature_k = 5450.0;
    state.settings.tint = 3.0;
    state.settings.set_locked(EditControl::Highlights, true);
    state.committed = state.settings;
    state.can_undo = true;
    service.emit_state(state);

    AutoSummary summary;
    summary.revision = 41;
    summary.revision_number = 3;
    summary.confidence = 0.82;
    summary.review = true;
    summary.review_reasons = {QStringLiteral("Lucent made a large automatic change; check that it suits the photo.")};
    summary.notes = {QStringLiteral("Parts of the photo were overexposed in the camera.")};
    summary.changed = {EditControl::WhiteBalance, EditControl::Exposure, EditControl::Contrast};
    summary.locked = {EditControl::Highlights};
    summary.locks_prevented_targets = true;
    summary.lock_notes = {QStringLiteral("Highlights are locked, so bright areas stay brighter.")};
    summary.attempt = 1;
    summary.fallback_detail =
        QStringLiteral("a conservative fallback after attempt 0 failed the full-resolution check");
    summary.group = QStringLiteral("time-1");
    QSignalSpy changed(&automatic, &AutoSettingsModel::summaryChanged);
    service.emit_auto_summary(8, summary);  // another photo's: ignored
    EXPECT_FALSE(automatic.has_summary());
    service.emit_auto_summary(7, summary);
    ASSERT_TRUE(automatic.has_summary());
    EXPECT_EQ(changed.count(), 1);
    EXPECT_EQ(automatic.origin(), QStringLiteral("Revision 3 · by Auto"));
    EXPECT_EQ(automatic.confidence_percent(), 82);
    EXPECT_EQ(automatic.confidence_level(), QStringLiteral("high"));
    EXPECT_EQ(automatic.confidence_text(), QStringLiteral("Confidence: high (82 %)"));
    EXPECT_TRUE(automatic.review());
    EXPECT_EQ(automatic.review_reasons(), summary.review_reasons);
    EXPECT_EQ(automatic.notes(), summary.notes);
    EXPECT_EQ(automatic.changed_text(),
              QStringLiteral("White balance 5450 K, tint +3 · Exposure +0.75 EV · Contrast +12"));
    EXPECT_EQ(automatic.locked_text(), QStringLiteral("Locked, kept as they were: Highlights"));
    EXPECT_TRUE(automatic.locks_prevented_targets());
    // The review reasons say a more conservative edit is used; the fallback line says why.
    EXPECT_EQ(automatic.fallback_text(), QStringLiteral("Why: ") + summary.fallback_detail);
    EXPECT_EQ(automatic.fallback_detail(), summary.fallback_detail);
    EXPECT_EQ(automatic.group_text(), QStringLiteral("Matched to its related shots (group time-1)."));

    // A revision that changed nothing shows no confidence (there was no change to be sure of).
    AutoSummary unchanged = summary;
    unchanged.changed.clear();
    service.emit_auto_summary(7, unchanged);
    EXPECT_TRUE(automatic.confidence_text().isEmpty());
    // A batch's anchor: its own settings were kept and set its group's look.
    AutoSummary anchor = summary;
    anchor.anchor = true;
    anchor.from_batch = true;
    anchor.attempt = 0;
    service.emit_auto_summary(7, anchor);
    EXPECT_TRUE(automatic.confidence_text().isEmpty());
    EXPECT_EQ(automatic.group_text(),
              QStringLiteral("Kept as the anchor of its related shots (group time-1): your settings set their look."));
    service.emit_auto_summary(7, summary);

    // A batch's revision says so; a low confidence is called low.
    summary.from_batch = true;
    summary.confidence = 0.3;
    summary.attempt = 0;
    service.emit_auto_summary(7, summary);
    EXPECT_EQ(automatic.origin(), QStringLiteral("Revision 3 · by an automatic batch"));
    EXPECT_EQ(automatic.confidence_level(), QStringLiteral("low"));
    EXPECT_TRUE(automatic.fallback_text().isEmpty());

    // Undo to a revision that is not automatic: no summary. Another photo starts without one.
    service.emit_auto_summary(7, AutoSummary{});
    EXPECT_FALSE(automatic.has_summary());
    service.emit_auto_summary(7, summary);
    session.set_item(8);
    EXPECT_FALSE(automatic.has_summary());
    EXPECT_FALSE(automatic.running());
}

TEST_F(EditSessionTest, AutoIsOfferedOnlyForAnEditablePhotoAndARunInProgressShowsAgain) {
    session.set_item(7);
    EXPECT_FALSE(session.can_run_auto());  // no state yet
    EditState read_only;
    read_only.item = 7;
    read_only.read_only = true;
    read_only.read_only_reason = QStringLiteral("This library was saved by a newer version of Lucent.");
    service.emit_state(read_only);
    EXPECT_FALSE(session.can_run_auto());
    session.runAuto();
    EXPECT_EQ(service.count("run_auto"), 0);

    // A run of photo 9 started earlier continues while the editor shows another photo.
    open(9);
    session.runAuto();
    open(10);
    EXPECT_FALSE(session.automatic()->running());
    open(9);
    EXPECT_TRUE(session.automatic()->running());
    EXPECT_FALSE(session.can_run_auto());
}

TEST(EditHistoryModelAuto, AutomaticAttemptsAreNamedByWhatHappenedToThem) {
    EditHistoryEntry chosen;
    chosen.revision = 5;
    chosen.number = 2;
    chosen.origin = QStringLiteral("auto");
    chosen.auto_kind = QStringLiteral("chosen");
    chosen.changed = {EditControl::Exposure};
    EXPECT_EQ(EditHistoryModel::title_of(chosen), QStringLiteral("Auto: Exposure"));
    chosen.auto_failed = true;
    EXPECT_EQ(EditHistoryModel::title_of(chosen), QStringLiteral("Auto, failed the quality check"));
    EditHistoryEntry fallback = chosen;
    fallback.auto_failed = false;
    fallback.auto_kind = QStringLiteral("fallback");
    EXPECT_EQ(EditHistoryModel::title_of(fallback), QStringLiteral("Auto (conservative): Exposure"));
}

}  // namespace
}  // namespace lucent::ui
