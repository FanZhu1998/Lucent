#include "models/batch_report_model.hpp"

#include <QCoreApplication>
#include <QFileInfo>
#include <algorithm>

#include "models/batch_presentation.hpp"

namespace lucent::ui {
namespace {

bool same_progress(const BatchSnapshot& a, const BatchSnapshot& b) {
    return a.state == b.state && a.completed == b.completed && a.failed == b.failed && a.skipped == b.skipped &&
           a.cancelled == b.cancelled && a.queued == b.queued && a.review == b.review;
}

}  // namespace

BatchReportModel::BatchReportModel(BatchService& service, QObject* parent)
    : QAbstractListModel(parent), service_(service) {
    refresh_timer_.setSingleShot(true);
    connect(&refresh_timer_, &QTimer::timeout, this, &BatchReportModel::reload);
    connect(&service_, &BatchService::report_ready, this, &BatchReportModel::on_report);
    connect(&service_, &BatchService::batch_changed, this, &BatchReportModel::on_batch_changed);
}

void BatchReportModel::set_refresh_interval(std::chrono::milliseconds interval) {
    refresh_interval_ms_ = static_cast<int>(interval.count());
}

int BatchReportModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : count();
}

const BatchReportEntry* BatchReportModel::entry_at(int row) const noexcept {
    if (row < 0 || row >= count()) {
        return nullptr;
    }
    return &report_.entries[static_cast<qsizetype>(rows_[static_cast<std::size_t>(row)])];
}

QVariant BatchReportModel::data(const QModelIndex& index, int role) const {
    const BatchReportEntry* entry = index.isValid() ? entry_at(index.row()) : nullptr;
    if (entry == nullptr) {
        return {};
    }
    namespace p = presentation;
    switch (role) {
    case Qt::DisplayRole:
    case SourceNameRole:
        return entry->source_name;
    case SourcePathRole:
        return entry->source_path;
    case OutcomeKeyRole:
        return p::outcome_key(entry->outcome);
    case OutcomeLabelRole:
        return p::outcome_label(entry->outcome);
    case OutputRole:
        return entry->output;
    case OutputNameRole:
        return entry->output.isEmpty() ? QString() : QFileInfo(entry->output).fileName();
    case ReasonRole:
        return entry->reason;
    case ActionRole:
        return entry->action;
    case ReviewRole:
        return entry->review;
    case HasReviewRole:
        return !entry->review.isEmpty();
    case AttemptsRole:
        return entry->attempts;
    case AccessibleLabelRole: {
        QStringList parts{entry->source_name, p::outcome_label(entry->outcome)};
        if (!entry->output.isEmpty()) {
            parts << QFileInfo(entry->output).fileName();
        }
        if (!entry->reason.isEmpty()) {
            parts << entry->reason;
        }
        if (!entry->action.isEmpty()) {
            parts << entry->action;
        }
        parts << entry->review;
        return parts.join(QStringLiteral(", "));
    }
    default:
        return {};
    }
}

QHash<int, QByteArray> BatchReportModel::roleNames() const {
    return {
        {SourceNameRole, "sourceName"},     {SourcePathRole, "sourcePath"}, {OutcomeKeyRole, "outcomeKey"},
        {OutcomeLabelRole, "outcomeLabel"}, {OutputRole, "output"},         {OutputNameRole, "outputName"},
        {ReasonRole, "reason"},             {ActionRole, "action"},         {ReviewRole, "review"},
        {HasReviewRole, "hasReview"},       {AttemptsRole, "attempts"},     {AccessibleLabelRole, "accessibleLabel"},
    };
}

QString BatchReportModel::times_label() const {
    return presentation::batch_times_label(report_.created, report_.finished, locale_);
}

void BatchReportModel::show(qulonglong batch) {
    const BatchId id = static_cast<BatchId>(batch);
    refresh_timer_.stop();
    reload_pending_ = false;
    if (id != batch_) {
        batch_ = id;
        last_seen_ = BatchSnapshot{};
        beginResetModel();
        report_ = BatchReportInfo{};
        rows_.clear();
        endResetModel();
        emit countChanged();
    }
    loading_ = id != 0;
    emit reportChanged();
    if (id != 0) {
        last_read_.start();
        service_.request_report(id);
    }
}

void BatchReportModel::reload() {
    if (batch_ == 0) {
        return;
    }
    if (loading_) {
        reload_pending_ = true;  // read again once the current read has answered
        return;
    }
    loading_ = true;
    last_read_.start();
    emit reportChanged();
    service_.request_report(batch_);
}

void BatchReportModel::setFilter(const QString& filter) {
    const QString valid = filter == u"exported" || filter == u"skipped" || filter == u"failed" ||
                                  filter == u"review" || filter == u"cancelled"
                              ? filter
                              : QStringLiteral("all");
    if (valid == filter_) {
        return;
    }
    filter_ = valid;
    beginResetModel();
    rebuild_rows();
    endResetModel();
    emit filterChanged();
    emit countChanged();
}

bool BatchReportModel::matches(const BatchReportEntry& entry) const {
    if (filter_ == u"all") {
        return true;
    }
    if (filter_ == u"review") {
        return !entry.review.isEmpty();
    }
    return presentation::outcome_key(entry.outcome) == filter_;
}

void BatchReportModel::rebuild_rows() {
    rows_.clear();
    for (qsizetype i = 0; i < report_.entries.size(); ++i) {
        if (matches(report_.entries[i])) {
            rows_.push_back(static_cast<int>(i));
        }
    }
}

void BatchReportModel::on_report(BatchId batch, const BatchReportInfo& report) {
    if (batch != batch_) {
        return;  // a report of a batch no longer shown
    }
    loading_ = false;
    beginResetModel();
    report_ = report;
    rebuild_rows();
    endResetModel();
    emit countChanged();
    emit reportChanged();
    if (reload_pending_) {
        // Asked for while this read was running: the newer state is read now.
        reload_pending_ = false;
        refresh_timer_.stop();
        reload();
    }
}

void BatchReportModel::on_batch_changed(const BatchSnapshot& batch) {
    if (batch.id != batch_ || batch_ == 0) {
        return;
    }
    if (last_seen_.id == batch.id && same_progress(last_seen_, batch)) {
        return;  // only the stage or a running photo's progress moved
    }
    const bool just_finished = batch.finished_state() && !(last_seen_.id == batch.id && last_seen_.finished_state());
    last_seen_ = batch;
    if (just_finished) {
        // The final outcome is shown at once.
        refresh_timer_.stop();
        reload();
        return;
    }
    // Throttled: at most one read per refresh interval, whatever the number of photos finishing (the
    // first change after a quiet interval is read at once). A report reads every job, so a large batch
    // is read less often (2 ms per photo).
    if (refresh_timer_.isActive()) {
        return;
    }
    const qint64 interval = std::max<qint64>(refresh_interval_ms_, std::min<qint64>(2 * batch.total, 15000));
    const qint64 since = last_read_.isValid() ? last_read_.elapsed() : interval;
    if (since >= interval && !loading_) {
        reload();
    } else {
        refresh_timer_.start(static_cast<int>(std::max<qint64>(0, interval - since)));
    }
}

}  // namespace lucent::ui
