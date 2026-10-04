#include "models/auto_settings_model.hpp"

#include <QCoreApplication>
#include <algorithm>
#include <cmath>
#include <utility>

namespace lucent::ui {
namespace {

QString tr_text(const char* text) {
    return QCoreApplication::translate("lucent::ui::AutoSettingsModel", text);
}

}  // namespace

AutoSettingsModel::AutoSettingsModel(EditService& service, ValueText value_text, QObject* parent)
    : QObject(parent), service_(service), value_text_(std::move(value_text)) {
    connect(&service_, &EditService::auto_progress, this, &AutoSettingsModel::on_progress);
    connect(&service_, &EditService::auto_finished, this, &AutoSettingsModel::on_finished);
    connect(&service_, &EditService::auto_summary_changed, this, &AutoSettingsModel::on_summary);
}

void AutoSettingsModel::set_item(ItemId item) {
    if (item == item_) {
        return;
    }
    item_ = item;
    if (summary_ != AutoSummary{}) {
        summary_ = AutoSummary{};
        emit summaryChanged();
    }
    set_last_result({}, false);
    cancelling_ = false;
    progress_ = AutoProgress{item, AutoStage::Queued, 0.0};
    // A run of this photo started earlier continues: its next progress event fills in the stage.
    set_running(item != 0 && service_.is_auto_running(item));
    emit runningChanged();
    emit progressChanged();
}

void AutoSettingsModel::start() {
    if (item_ == 0) {
        return;
    }
    cancelling_ = false;
    progress_ = AutoProgress{item_, AutoStage::Queued, 0.0};
    set_last_result({}, false);
    set_running(true);
    emit progressChanged();
}

void AutoSettingsModel::values_changed() {
    if (has_summary()) {
        emit summaryChanged();  // changedText reads the values shown
    }
}

void AutoSettingsModel::cancel() {
    if (!running_ || item_ == 0 || cancelling_) {
        return;
    }
    cancelling_ = true;
    emit runningChanged();
    emit progressChanged();
    service_.cancel_auto(item_);
}

void AutoSettingsModel::clearLastResult() {
    set_last_result({}, false);
}

void AutoSettingsModel::set_running(bool running) {
    if (running == running_) {
        return;
    }
    running_ = running;
    if (!running_) {
        cancelling_ = false;
    }
    emit runningChanged();
}

void AutoSettingsModel::set_last_result(const QString& text, bool failed, bool current) {
    if (text == last_result_ && failed == last_result_failed_ && current == last_result_current_) {
        return;
    }
    last_result_ = text;
    last_result_failed_ = failed;
    last_result_current_ = current;
    emit lastResultChanged();
}

void AutoSettingsModel::on_progress(const AutoProgress& progress) {
    if (progress.item != item_ || item_ == 0) {
        return;
    }
    progress_ = progress;
    set_running(true);
    emit progressChanged();
}

void AutoSettingsModel::on_finished(const AutoOutcome& outcome) {
    if (outcome.item != item_ || item_ == 0) {
        return;
    }
    set_running(false);
    set_last_result(outcome.message, !outcome.saved && !outcome.cancelled, outcome.saved && outcome.made_current);
    emit finished(outcome.message);
}

void AutoSettingsModel::on_summary(ItemId item, const AutoSummary& summary) {
    if (item != item_ || item_ == 0 || summary == summary_) {
        return;
    }
    summary_ = summary;
    emit summaryChanged();
}

QString AutoSettingsModel::stage_text() const {
    if (!running_) {
        return {};
    }
    if (cancelling_) {
        return tr_text("Cancelling…");
    }
    switch (progress_.stage) {
    case AutoStage::Queued:
        return tr_text("Waiting to start…");
    case AutoStage::Analysing:
        return tr_text("Analysing the photo…");
    case AutoStage::Checking:
        return tr_text("Checking the result at full size…");
    case AutoStage::Saving:
        return tr_text("Saving the automatic revision…");
    }
    return {};
}

QString AutoSettingsModel::origin() const {
    if (!has_summary()) {
        return {};
    }
    return summary_.from_batch ? tr_text("Revision %1 · by an automatic batch").arg(summary_.revision_number)
                               : tr_text("Revision %1 · by Auto").arg(summary_.revision_number);
}

int AutoSettingsModel::confidence_percent() const noexcept {
    return static_cast<int>(std::lround(std::clamp(summary_.confidence, 0.0, 1.0) * 100.0));
}

QString AutoSettingsModel::confidence_level() const {
    if (!has_summary()) {
        return {};
    }
    if (summary_.confidence >= kHighConfidence) {
        return QStringLiteral("high");
    }
    return summary_.confidence >= kMediumConfidence ? QStringLiteral("medium") : QStringLiteral("low");
}

QString AutoSettingsModel::confidence_text() const {
    if (!has_summary()) {
        return {};
    }
    const QString level = confidence_level();
    const QString word = level == u"high" ? tr_text("high") : level == u"medium" ? tr_text("medium") : tr_text("low");
    return tr_text("Confidence: %1 (%2 %)").arg(word).arg(confidence_percent());
}

QString AutoSettingsModel::changed_text() const {
    if (!has_summary()) {
        return {};
    }
    if (summary_.changed.isEmpty()) {
        return tr_text("No control was changed.");
    }
    QStringList parts;
    for (const EditControl control : summary_.changed) {
        parts << (value_text_ ? value_text_(control) : control_label(control));
    }
    return parts.join(QStringLiteral(" · "));
}

QString AutoSettingsModel::locked_text() const {
    if (!has_summary() || summary_.locked.isEmpty()) {
        return {};
    }
    QStringList labels;
    for (const EditControl control : summary_.locked) {
        labels << control_label(control);
    }
    return tr_text("Locked, kept as they were: %1").arg(labels.join(QStringLiteral(", ")));
}

QString AutoSettingsModel::fallback_text() const {
    if (!has_summary() || summary_.attempt == 0) {
        return {};
    }
    if (summary_.baseline) {
        return tr_text("Every automatic choice failed Lucent's full-size quality check, so Auto kept the camera's "
                       "rendering for the controls it sets.");
    }
    return tr_text("Auto's first choice failed Lucent's full-size quality check, so a more conservative version is "
                   "used.");
}

QString AutoSettingsModel::group_text() const {
    if (!has_summary() || summary_.group.isEmpty()) {
        return {};
    }
    return tr_text("Matched to its related shots (group %1).").arg(summary_.group);
}

}  // namespace lucent::ui
