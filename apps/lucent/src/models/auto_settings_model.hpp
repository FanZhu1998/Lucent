#pragma once

// AutoSettingsModel: the Edit view's "Automatic settings" panel (blueprint section 2, "Detailed
// individual editing": "Auto creates a new editable revision. The photographer can lock controls
// before rerunning it. The optimizer may change only unlocked controls and must report when the locks
// prevent its usual technical targets"; ADR 0013). EditSession (`edit.automatic`) owns it and keeps it
// on the photo the Edit view shows.
//
// Running: while Auto runs for the photo, `running` with the stage ("Analysing the photo…") and the
// fraction of the run; cancel() stops it (nothing is written). A run continues when the photo changes
// and shows again when the Edit view returns to it. When it ends, `lastResult` says what happened (the
// new revision, a branch kept because the photo changed meanwhile, a cancellation, or why nothing was
// saved) and finished() carries the same text for the status line.
//
// Summary: when the photo's current revision is an automatic one (made by Auto or by an automatic
// batch), what Auto did, from its analysis record: the confidence, whether a review is recommended
// and why, the other findings, what changed (with the values shown), the locked controls and what
// they prevented, the conservative fallback taken after a failed full-size check (with the failed
// checks as diagnostics), and the related-shot group. `hasSummary` is false otherwise.
//
// QML-facing members use QML naming (lowerCamelCase); C++-only members use snake_case.
//
// Thread-safety: UI thread only.

#include <functional>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QtQmlIntegration/qqmlintegration.h>

#include "services/edit_service.hpp"

namespace lucent::ui {

class AutoSettingsModel : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("AutoSettingsModel is provided by EditSession.automatic.")
    Q_PROPERTY(bool running READ running NOTIFY runningChanged FINAL)
    Q_PROPERTY(bool cancelling READ cancelling NOTIFY runningChanged FINAL)
    // "Waiting to start…", "Analysing the photo…", "Checking at full size…", "Saving…".
    Q_PROPERTY(QString stageText READ stage_text NOTIFY progressChanged FINAL)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged FINAL)
    // The outcome of the photo's last run in this session; empty when none (or after the next starts).
    Q_PROPERTY(QString lastResult READ last_result NOTIFY lastResultChanged FINAL)
    Q_PROPERTY(bool lastResultFailed READ last_result_failed NOTIFY lastResultChanged FINAL)
    // The last result says more than the summary shows: nothing was saved, or the result was kept as
    // a branch of the history (a saved, current result is the summary itself).
    Q_PROPERTY(bool lastResultWorthShowing READ last_result_worth_showing NOTIFY lastResultChanged FINAL)

    Q_PROPERTY(bool hasSummary READ has_summary NOTIFY summaryChanged FINAL)
    // "Revision 3 · by Auto" or "Revision 3 · by an automatic batch".
    Q_PROPERTY(QString origin READ origin NOTIFY summaryChanged FINAL)
    Q_PROPERTY(int confidencePercent READ confidence_percent NOTIFY summaryChanged FINAL)
    // "high", "medium" or "low" (the panel's colour).
    Q_PROPERTY(QString confidenceLevel READ confidence_level NOTIFY summaryChanged FINAL)
    // "Confidence: high (90 %)".
    Q_PROPERTY(QString confidenceText READ confidence_text NOTIFY summaryChanged FINAL)
    Q_PROPERTY(bool review READ review NOTIFY summaryChanged FINAL)
    Q_PROPERTY(QStringList reviewReasons READ review_reasons NOTIFY summaryChanged FINAL)
    Q_PROPERTY(QStringList notes READ notes NOTIFY summaryChanged FINAL)
    // "Exposure +0.75 EV · Shadows +12" or "Nothing: the photo already met Lucent's targets".
    Q_PROPERTY(QString changedText READ changed_text NOTIFY summaryChanged FINAL)
    // "Locked, kept as they were: Exposure, White balance"; empty without locks.
    Q_PROPERTY(QString lockedText READ locked_text NOTIFY summaryChanged FINAL)
    Q_PROPERTY(bool locksPreventedTargets READ locks_prevented_targets NOTIFY summaryChanged FINAL)
    Q_PROPERTY(QStringList lockNotes READ lock_notes NOTIFY summaryChanged FINAL)
    // The fallback taken after a failed full-size check; empty when Auto's first choice passed.
    Q_PROPERTY(QString fallbackText READ fallback_text NOTIFY summaryChanged FINAL)
    // The checks the earlier attempt failed (diagnostics, developer wording); empty when none.
    Q_PROPERTY(QString fallbackDetail READ fallback_detail NOTIFY summaryChanged FINAL)
    // "Matched to its related shots (group time-1)"; empty when not regularised.
    Q_PROPERTY(QString groupText READ group_text NOTIFY summaryChanged FINAL)

public:
    // The display value of a control as the panel lists it ("Exposure +0.75 EV"); EditSession's.
    using ValueText = std::function<QString(EditControl)>;

    // `service` must outlive the model.
    AutoSettingsModel(EditService& service, ValueText value_text, QObject* parent = nullptr);

    // Follows `item` (0: none): its summary arrives with its history; a run of it in progress shows.
    void set_item(ItemId item);
    // A run of the current photo was asked for (EditService::run_auto follows): shown at once.
    void start();
    // The values shown changed (the summary's "changed" line reads them).
    void values_changed();

    Q_INVOKABLE void cancel();
    Q_INVOKABLE void clearLastResult();

    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] bool cancelling() const noexcept { return running_ && cancelling_; }
    [[nodiscard]] QString stage_text() const;
    [[nodiscard]] double progress() const noexcept { return progress_.fraction; }
    [[nodiscard]] QString last_result() const { return last_result_; }
    [[nodiscard]] bool last_result_failed() const noexcept { return last_result_failed_; }
    [[nodiscard]] bool last_result_worth_showing() const noexcept {
        return !last_result_.isEmpty() && !last_result_current_;
    }
    [[nodiscard]] bool has_summary() const noexcept { return summary_.revision != 0; }
    [[nodiscard]] QString origin() const;
    [[nodiscard]] int confidence_percent() const noexcept;
    [[nodiscard]] QString confidence_level() const;
    [[nodiscard]] QString confidence_text() const;
    [[nodiscard]] bool review() const noexcept { return summary_.review; }
    [[nodiscard]] QStringList review_reasons() const { return summary_.review_reasons; }
    [[nodiscard]] QStringList notes() const { return summary_.notes; }
    [[nodiscard]] QString changed_text() const;
    [[nodiscard]] QString locked_text() const;
    [[nodiscard]] bool locks_prevented_targets() const noexcept { return summary_.locks_prevented_targets; }
    [[nodiscard]] QStringList lock_notes() const { return summary_.lock_notes; }
    [[nodiscard]] QString fallback_text() const;
    [[nodiscard]] QString fallback_detail() const { return summary_.fallback_detail; }
    [[nodiscard]] QString group_text() const;
    [[nodiscard]] const AutoSummary& summary() const noexcept { return summary_; }

    // Confidence levels: high from 0.75, medium from 0.5.
    static constexpr double kHighConfidence = 0.75;
    static constexpr double kMediumConfidence = 0.5;

signals:
    void runningChanged();
    void progressChanged();
    void lastResultChanged();
    void summaryChanged();
    // A run of the current photo ended (`message`: AutoOutcome::message).
    void finished(const QString& message);

private:
    void on_progress(const AutoProgress& progress);
    void on_finished(const AutoOutcome& outcome);
    void on_summary(ItemId item, const AutoSummary& summary);
    void set_running(bool running);
    void set_last_result(const QString& text, bool failed, bool current = false);

    EditService& service_;
    ValueText value_text_;
    ItemId item_ = 0;
    bool running_ = false;
    bool cancelling_ = false;
    AutoProgress progress_;
    QString last_result_;
    bool last_result_failed_ = false;
    bool last_result_current_ = false;  // saved and made current
    AutoSummary summary_;
};

}  // namespace lucent::ui
