#include "models/batch_list_model.hpp"

#include <algorithm>
#include <climits>

#include <QCoreApplication>
#include <QStringList>

#include "models/batch_presentation.hpp"

namespace lucent::ui {
namespace {

QString tr_text(const char* text) {
    return QCoreApplication::translate("lucent::ui::BatchListModel", text);
}

bool batch_active(const BatchSnapshot& batch) {
    if (batch.state == BatchState::Running || batch.state == BatchState::Paused) {
        return batch.remaining() > 0 || batch.attention != BatchAttention::None;
    }
    return batch.state == BatchState::Cancelled && batch.running > 0;
}

bool can_pause(const BatchSnapshot& batch) {
    return batch.state == BatchState::Running && batch.remaining() > 0;
}

bool can_resume(const BatchSnapshot& batch) {
    return batch.state == BatchState::Paused && (batch.queued > 0 || batch.attention != BatchAttention::None);
}

bool can_cancel(const BatchSnapshot& batch) {
    return (batch.state == BatchState::Running || batch.state == BatchState::Paused ||
            batch.state == BatchState::Draft) &&
           batch.remaining() > 0;
}

bool can_retry(const BatchSnapshot& batch) {
    return batch.failed > 0 && batch.state != BatchState::Cancelled && batch.state != BatchState::Draft;
}

}  // namespace

BatchListModel::BatchListModel(BatchService& service, QObject* parent)
    : QAbstractListModel(parent), service_(service) {
    connect(&service_, &BatchService::batches_listed, this, &BatchListModel::on_listed);
    connect(&service_, &BatchService::batch_changed, this, &BatchListModel::on_changed);
    connect(&service_, &BatchService::submit_progress, this, &BatchListModel::on_submit_progress);
    connect(&service_, &BatchService::submitted, this, &BatchListModel::on_submitted);
}

int BatchListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : count();
}

const BatchSnapshot* BatchListModel::batch_at(int row) const noexcept {
    if (row < 0 || row >= count()) {
        return nullptr;
    }
    return &batches_[static_cast<std::size_t>(row)];
}

std::optional<int> BatchListModel::row_of(BatchId id) const {
    const auto found = rows_.constFind(id);
    if (found == rows_.cend()) {
        return std::nullopt;
    }
    return *found;
}

qulonglong BatchListModel::batchIdAt(int row) const {
    const BatchSnapshot* batch = batch_at(row);
    return batch == nullptr ? 0 : static_cast<qulonglong>(batch->id);
}

int BatchListModel::rowOfBatch(qulonglong batch) const {
    return row_of(static_cast<BatchId>(batch)).value_or(-1);
}

QVariant BatchListModel::data(const QModelIndex& index, int role) const {
    const BatchSnapshot* batch = index.isValid() ? batch_at(index.row()) : nullptr;
    if (batch == nullptr) {
        return {};
    }
    namespace p = presentation;
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return batch->name;
    case BatchIdRole:
        return static_cast<qulonglong>(batch->id);
    case SingleRole:
        return batch->single();
    case SelectionRole:
        return batch->selection;
    case FolderRole:
        return batch->folder;
    case StateKeyRole:
        return p::batch_state_key(*batch);
    case StateLabelRole:
        return p::batch_state_label(*batch);
    case TotalRole:
        return batch->total;
    case CompletedRole:
        return batch->completed;
    case RemainingRole:
        return batch->remaining();
    case FailedRole:
        return batch->failed;
    case SkippedRole:
        // As the report counts them: skipped photos and those left out when the batch was created.
        return batch->skipped + std::max<qint64>(0, batch->excluded);
    case ReviewRole:
        return batch->review;
    case ProgressRole:
        return p::batch_progress(*batch);
    case ProgressLabelRole:
        return tr_text("%1 of %2").arg(locale_.toString(batch->completed + batch->failed + batch->skipped +
                                                        batch->cancelled),
                                       locale_.toString(batch->total));
    case StageRole:
        return p::batch_stage_label(*batch, locale_);
    case EtaRole:
        return batch->finished_state() ? QString() : p::eta_label(batch->eta_seconds, locale_);
    case HasAttentionRole:
        return batch->attention != BatchAttention::None && !batch->finished_state();
    case AttentionRole:
        return batch->finished_state() ? QString() : batch->attention_message;
    case CanPauseRole:
        return can_pause(*batch);
    case CanResumeRole:
        return can_resume(*batch);
    case CanCancelRole:
        return can_cancel(*batch);
    case CanRetryRole:
        return can_retry(*batch);
    case CanOpenFolderRole:
        // No file-system check on the UI thread; opening reports a folder that no longer exists.
        return !batch->folder.isEmpty();
    case CanOpenReportRole:
        return !batch->single() && batch->finished_state() && !batch->folder.isEmpty();
    case OutputPathRole:
        return batch->last_output;
    case TimesLabelRole:
        return p::batch_times_label(batch->created, batch->finished, locale_);
    case IsActiveRole:
        return batch_active(*batch);
    case AccessibleLabelRole:
        return p::batch_accessible_label(*batch, locale_);
    default:
        return {};
    }
}

QHash<int, QByteArray> BatchListModel::roleNames() const {
    return {
        {BatchIdRole, "batchId"},
        {NameRole, "name"},
        {SingleRole, "single"},
        {SelectionRole, "selection"},
        {FolderRole, "folder"},
        {StateKeyRole, "stateKey"},
        {StateLabelRole, "stateLabel"},
        {TotalRole, "total"},
        {CompletedRole, "completed"},
        {RemainingRole, "remaining"},
        {FailedRole, "failed"},
        {SkippedRole, "skipped"},
        {ReviewRole, "review"},
        {ProgressRole, "progress"},
        {ProgressLabelRole, "progressLabel"},
        {StageRole, "stage"},
        {EtaRole, "eta"},
        {HasAttentionRole, "hasAttention"},
        {AttentionRole, "attention"},
        {CanPauseRole, "canPause"},
        {CanResumeRole, "canResume"},
        {CanCancelRole, "canCancel"},
        {CanRetryRole, "canRetry"},
        {CanOpenFolderRole, "canOpenFolder"},
        {CanOpenReportRole, "canOpenReport"},
        {OutputPathRole, "outputPath"},
        {TimesLabelRole, "timesLabel"},
        {IsActiveRole, "isActive"},
        {AccessibleLabelRole, "accessibleLabel"},
    };
}

int BatchListModel::active_count() const noexcept {
    return static_cast<int>(std::count_if(batches_.begin(), batches_.end(),
                                          [](const BatchSnapshot& b) { return batch_active(b); }));
}

int BatchListModel::attention_count() const noexcept {
    return static_cast<int>(std::count_if(batches_.begin(), batches_.end(), [](const BatchSnapshot& batch) {
        return batch.attention != BatchAttention::None && !batch.finished_state();
    }));
}

int BatchListModel::remaining_photos() const noexcept {
    qint64 remaining = 0;
    for (const BatchSnapshot& batch : batches_) {
        if (batch.state == BatchState::Running || batch.state == BatchState::Paused) {
            remaining += batch.remaining();
        }
    }
    return static_cast<int>(std::min<qint64>(remaining, INT_MAX));
}

QString BatchListModel::summary() const {
    int running = 0;
    int paused = 0;
    for (const BatchSnapshot& batch : batches_) {
        if (!batch_active(batch) || batch.attention != BatchAttention::None) {
            continue;
        }
        (batch.state == BatchState::Paused ? paused : running) += 1;
    }
    QStringList parts;
    if (running > 0) {
        parts << tr_text("%1 running").arg(running);
    }
    if (paused > 0) {
        parts << tr_text("%1 paused").arg(paused);
    }
    if (const int attention = attention_count(); attention > 0) {
        parts << (attention == 1 ? tr_text("1 needs attention") : tr_text("%1 need attention").arg(attention));
    }
    return parts.join(QStringLiteral(" · "));
}

QString BatchListModel::preparing_name() const {
    return submissions_.empty() ? QString() : submissions_.front().name;
}

QString BatchListModel::preparing_text() const {
    return submissions_.empty() ? QString() : submissions_.front().text;
}

double BatchListModel::preparing_progress() const {
    return submissions_.empty() ? -1.0 : submissions_.front().fraction;
}

void BatchListModel::pause(int row) {
    if (const BatchSnapshot* batch = batch_at(row); batch != nullptr && can_pause(*batch)) {
        service_.pause(batch->id);
    }
}

void BatchListModel::resume(int row) {
    if (const BatchSnapshot* batch = batch_at(row); batch != nullptr && can_resume(*batch)) {
        service_.resume(batch->id);
    }
}

void BatchListModel::cancel(int row) {
    if (const BatchSnapshot* batch = batch_at(row); batch != nullptr && can_cancel(*batch)) {
        service_.cancel(batch->id);
    }
}

void BatchListModel::retryFailed(int row) {
    if (const BatchSnapshot* batch = batch_at(row); batch != nullptr && can_retry(*batch)) {
        service_.retry_failed(batch->id);
    }
}

void BatchListModel::cancelPreparing() {
    if (!submissions_.empty()) {
        service_.cancel_submit(submissions_.front().token);
        submissions_.front().text = tr_text("Cancelling…");
        emit preparingChanged();
    }
}

void BatchListModel::track_submission(quint64 token, const QString& name) {
    submissions_.push_back(Submission{token, name, tr_text("Preparing…"), -1.0});
    emit preparingChanged();
}

void BatchListModel::set_locale(const QLocale& locale) {
    if (locale == locale_) {
        return;
    }
    locale_ = locale;
    if (!batches_.empty()) {
        emit dataChanged(index(0), index(count() - 1));
    }
}

void BatchListModel::reindex() {
    rows_.clear();
    for (std::size_t i = 0; i < batches_.size(); ++i) {
        rows_.insert(batches_[i].id, static_cast<int>(i));
    }
}

void BatchListModel::on_listed(const QList<BatchSnapshot>& batches) {
    beginResetModel();
    batches_.assign(batches.begin(), batches.end());
    std::stable_sort(batches_.begin(), batches_.end(),
                     [](const BatchSnapshot& a, const BatchSnapshot& b) { return a.id > b.id; });
    reindex();
    endResetModel();
    emit countChanged();
    emit_summary_if_changed();
}

void BatchListModel::on_changed(const BatchSnapshot& batch) {
    if (batch.id == 0) {
        return;
    }
    if (const auto row = row_of(batch.id)) {
        batches_[static_cast<std::size_t>(*row)] = batch;
        const QModelIndex changed = index(*row);
        emit dataChanged(changed, changed);
        emit_summary_if_changed();
        return;
    }
    // A new batch: its place by id (newest first).
    const auto position = std::find_if(batches_.begin(), batches_.end(),
                                       [&](const BatchSnapshot& other) { return other.id < batch.id; });
    const int row = static_cast<int>(position - batches_.begin());
    beginInsertRows({}, row, row);
    batches_.insert(position, batch);
    reindex();
    endInsertRows();
    emit countChanged();
    emit_summary_if_changed();
}

void BatchListModel::on_submit_progress(quint64 token, const QString& text, double fraction) {
    for (Submission& submission : submissions_) {
        if (submission.token == token) {
            submission.text = text;
            submission.fraction = fraction;
            if (&submission == &submissions_.front()) {
                emit preparingChanged();
            }
            return;
        }
    }
}

void BatchListModel::on_submitted(quint64 token, BatchId, const QString&) {
    const auto found = std::find_if(submissions_.begin(), submissions_.end(),
                                    [token](const Submission& submission) { return submission.token == token; });
    if (found != submissions_.end()) {
        submissions_.erase(found);
        emit preparingChanged();
    }
}

void BatchListModel::emit_summary_if_changed() {
    const QString text = summary();
    const int active = active_count();
    const int attention = attention_count();
    const int remaining = remaining_photos();
    if (text != last_summary_ || active != last_active_ || attention != last_attention_ ||
        remaining != last_remaining_) {
        last_summary_ = text;
        last_active_ = active;
        last_attention_ = attention;
        last_remaining_ = remaining;
        emit summaryChanged();
    }
}

}  // namespace lucent::ui
