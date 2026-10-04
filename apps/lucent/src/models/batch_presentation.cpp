#include "models/batch_presentation.hpp"

#include <algorithm>
#include <cmath>

#include <QCoreApplication>
#include <QStringList>

namespace lucent::ui::presentation {
namespace {

QString tr_text(const char* text) {
    return QCoreApplication::translate("lucent::ui::presentation", text);
}

qint64 done_of(const BatchSnapshot& batch) {
    return batch.completed + batch.failed + batch.skipped + batch.cancelled;
}

QString percent(double fraction, const QLocale& locale) {
    return tr_text("%1 %").arg(locale.toString(static_cast<int>(std::floor(std::clamp(fraction, 0.0, 1.0) * 100.0))));
}

}  // namespace

QString batch_state_key(const BatchSnapshot& batch) {
    switch (batch.state) {
    case BatchState::Draft:
        return QStringLiteral("draft");
    case BatchState::Running:
        if (batch.attention != BatchAttention::None) {
            return QStringLiteral("attention");
        }
        return batch.running == 0 && batch.running_jobs.isEmpty() && done_of(batch) == 0 ? QStringLiteral("waiting")
                                                                                        : QStringLiteral("running");
    case BatchState::Paused:
        return batch.attention != BatchAttention::None ? QStringLiteral("attention") : QStringLiteral("paused");
    case BatchState::Cancelled:
        return QStringLiteral("cancelled");
    case BatchState::Completed:
        if (batch.failed > 0) {
            return batch.completed == 0 && batch.skipped == 0 ? QStringLiteral("failed") : QStringLiteral("partial");
        }
        return QStringLiteral("completed");
    }
    return QStringLiteral("draft");
}

QString batch_state_label(const BatchSnapshot& batch) {
    const QString key = batch_state_key(batch);
    if (batch.single()) {
        if (key == u"completed") {
            return batch.skipped > 0 ? tr_text("Skipped") : tr_text("Exported");
        }
        if (key == u"cancelled" && !batch.outcome_reason.isEmpty()) {
            return tr_text("Not exported");  // stopped by the destination, not by the user
        }
        if (key == u"failed" || key == u"partial") {
            return tr_text("Failed");
        }
        if (key == u"waiting") {
            return tr_text("Waiting");
        }
        if (key == u"running") {
            return tr_text("Exporting");
        }
    }
    if (key == u"running") {
        return tr_text("Running");
    }
    if (key == u"waiting") {
        return tr_text("Starting");
    }
    if (key == u"paused") {
        return tr_text("Paused");
    }
    if (key == u"attention") {
        return tr_text("Needs attention");
    }
    if (key == u"completed") {
        return tr_text("Completed");
    }
    if (key == u"partial") {
        return tr_text("Completed with failures");
    }
    if (key == u"failed") {
        return tr_text("Failed");
    }
    if (key == u"cancelled") {
        return tr_text("Cancelled");
    }
    return tr_text("Not started");
}

double batch_progress(const BatchSnapshot& batch) {
    if (batch.total <= 0) {
        return 0.0;
    }
    double done = static_cast<double>(done_of(batch));
    for (const BatchRunningJob& job : batch.running_jobs) {
        done += std::clamp(job.fraction, 0.0, 1.0);
    }
    return std::clamp(done / static_cast<double>(batch.total), 0.0, 1.0);
}

QString batch_stage_label(const BatchSnapshot& batch, const QLocale& locale) {
    if (batch.finished_state()) {
        return {};
    }
    QStringList parts;
    for (const BatchRunningJob& job : batch.running_jobs) {
        const QString name = job.source_name.isEmpty() ? tr_text("a photo") : job.source_name;
        parts << tr_text("%1 %2 (%3)").arg(job.stage, name, percent(job.fraction, locale));
    }
    const QString running = parts.join(QStringLiteral(" · "));
    switch (batch.state) {
    case BatchState::Paused:
        if (batch.running > 0) {
            return batch.running == 1 ? tr_text("Finishing 1 photo before pausing")
                                      : tr_text("Finishing %1 photos before pausing").arg(batch.running);
        }
        return batch.attention != BatchAttention::None ? QString() : tr_text("Paused. Choose Resume to continue.");
    case BatchState::Cancelled:
        return batch.running > 0 ? tr_text("Stopping…") : QString();
    case BatchState::Draft:
        return tr_text("Not started");
    case BatchState::Running:
        if (!running.isEmpty()) {
            return running;
        }
        return batch.remaining() > 0 ? tr_text("Waiting to start") : QString();
    case BatchState::Completed:
        break;
    }
    return {};
}

QString eta_label(double seconds, const QLocale& locale) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return {};
    }
    const qint64 total = std::max<qint64>(1, std::llround(seconds));
    if (total < 60) {
        return tr_text("About %1 s left").arg(locale.toString(std::max<qint64>(5, (total + 4) / 5 * 5)));
    }
    const qint64 minutes = (total + 30) / 60;
    if (minutes < 60) {
        return tr_text("About %1 min left").arg(locale.toString(minutes));
    }
    return tr_text("About %1 h %2 min left").arg(locale.toString(minutes / 60), locale.toString(minutes % 60));
}

QString photo_count_label(qint64 count) {
    return count == 1 ? tr_text("1 photo") : tr_text("%1 photos").arg(count);
}

QString batch_accessible_label(const BatchSnapshot& batch, const QLocale& locale) {
    QStringList parts{batch.name, batch_state_label(batch)};
    if (!batch.single()) {
        parts << tr_text("%1 of %2 exported").arg(batch.completed).arg(batch.total);
        if (batch.remaining() > 0) {
            parts << tr_text("%1 remaining").arg(batch.remaining());
        }
        if (batch.failed > 0) {
            parts << tr_text("%1 failed").arg(batch.failed);
        }
        if (batch.skipped > 0) {
            parts << tr_text("%1 skipped").arg(batch.skipped);
        }
        if (batch.excluded > 0) {
            parts << tr_text("%1 left out").arg(batch.excluded);
        }
        if (batch.review > 0) {
            parts << tr_text("%1 recommended for review").arg(batch.review);
        }
    } else {
        if (batch.review > 0) {
            parts << tr_text("recommended for review");
        }
        if (!batch.outcome_reason.isEmpty()) {
            parts << batch.outcome_reason;
        }
    }
    if (batch.state == BatchState::Running && batch.attention == BatchAttention::None) {
        if (const QString eta = eta_label(batch.eta_seconds, locale); !eta.isEmpty()) {
            parts << eta;
        }
    }
    if (!batch.attention_message.isEmpty()) {
        parts << batch.attention_message;
    }
    return parts.join(QStringLiteral(", "));
}

QString outcome_key(BatchOutcome outcome) {
    switch (outcome) {
    case BatchOutcome::Exported:
        return QStringLiteral("exported");
    case BatchOutcome::Skipped:
        return QStringLiteral("skipped");
    case BatchOutcome::Failed:
        return QStringLiteral("failed");
    case BatchOutcome::Cancelled:
        return QStringLiteral("cancelled");
    case BatchOutcome::Pending:
        break;
    }
    return QStringLiteral("pending");
}

QString outcome_label(BatchOutcome outcome) {
    switch (outcome) {
    case BatchOutcome::Exported:
        return tr_text("Exported");
    case BatchOutcome::Skipped:
        return tr_text("Skipped");
    case BatchOutcome::Failed:
        return tr_text("Failed");
    case BatchOutcome::Cancelled:
        return tr_text("Cancelled");
    case BatchOutcome::Pending:
        break;
    }
    return tr_text("Waiting");
}

QString batch_times_label(const QDateTime& created, const QDateTime& finished, const QLocale& locale) {
    if (!created.isValid()) {
        return {};
    }
    QString text = tr_text("Created %1").arg(locale.toString(created, QLocale::ShortFormat));
    if (finished.isValid()) {
        const bool same_day = finished.date() == created.date();
        text += QStringLiteral(" · ") +
                tr_text("finished %1")
                    .arg(same_day ? locale.toString(finished.time(), QLocale::ShortFormat)
                                  : locale.toString(finished, QLocale::ShortFormat));
    }
    return text;
}

}  // namespace lucent::ui::presentation
