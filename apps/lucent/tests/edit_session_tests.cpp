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

}  // namespace
}  // namespace lucent::ui
