#include "models/export_job_model.hpp"

#include <algorithm>
#include <cmath>

#include <QCoreApplication>
#include <QDir>

#include "models/presentation.hpp"

namespace lucent::ui {
namespace {

double clamped_progress(const ExportJob& job) {
    if (job.state == ExportJobState::Completed) {
        return 1.0;
    }
    if (!std::isfinite(job.progress)) {
        return 0.0;
    }
    return std::clamp(job.progress, 0.0, 1.0);
}

}  // namespace

ExportJobModel::ExportJobModel(ExportService& service, QObject* parent) : QAbstractListModel(parent) {
    connect(&service, &ExportService::job_added, this, &ExportJobModel::add_job);
    connect(&service, &ExportService::job_changed, this, &ExportJobModel::update_job);
}

int ExportJobModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : count();
}

const ExportJob* ExportJobModel::job_at(int row) const noexcept {
    if (row < 0 || row >= count()) {
        return nullptr;
    }
    return &jobs_[static_cast<std::size_t>(row)];
}

std::optional<int> ExportJobModel::row_of(JobId id) const {
    const auto found = positions_.constFind(id);
    if (found == positions_.cend()) {
        return std::nullopt;
    }
    return static_cast<int>(*found);
}

QVariant ExportJobModel::data(const QModelIndex& index, int role) const {
    const ExportJob* job = index.isValid() ? job_at(index.row()) : nullptr;
    if (job == nullptr) {
        return {};
    }
    namespace p = presentation;
    const double progress = clamped_progress(*job);
    switch (role) {
    case Qt::DisplayRole:
    case SourceNameRole:
        return job->source_name;
    case OutputFolderRole:
        return QDir::toNativeSeparators(job->output_folder);
    case DestinationPathRole:
        return QDir::toNativeSeparators(job->destination_path);
    case FormatLabelRole:
        return p::export_options_label(job->options);
    case StateKeyRole:
        return p::job_state_key(job->state);
    case StateLabelRole:
        return p::job_state_label(job->state);
    case ProgressRole:
        return progress;
    case ProgressLabelRole:
        if (job->state != ExportJobState::Running) {
            return QString();
        }
        return QCoreApplication::translate("lucent::ui::ExportJobModel", "%1 %")
            .arg(locale_.toString(static_cast<int>(std::floor(progress * 100.0))));
    case MessageRole:
        return job->message;
    case IsActiveRole:
        return is_active(job->state);
    case CanCancelRole:
        return is_active(job->state) && !job->cancel_requested;
    case CanOpenFolderRole:
        // No file-system check here: data() runs on the UI thread and a folder on an unreachable
        // network share could block it. Opening reports a folder that no longer exists.
        return !job->output_folder.isEmpty();
    case AccessibleLabelRole: {
        QStringList parts{job->source_name, p::job_state_label(job->state)};
        if (job->state == ExportJobState::Running) {
            parts << data(index, ProgressLabelRole).toString();
        }
        if (!job->message.isEmpty()) {
            parts << job->message;
        }
        return parts.join(QStringLiteral(", "));
    }
    default:
        return {};
    }
}

QHash<int, QByteArray> ExportJobModel::roleNames() const {
    return {
        {SourceNameRole, "sourceName"},
        {OutputFolderRole, "outputFolder"},
        {DestinationPathRole, "destinationPath"},
        {FormatLabelRole, "formatLabel"},
        {StateKeyRole, "stateKey"},
        {StateLabelRole, "stateLabel"},
        {ProgressRole, "progress"},
        {ProgressLabelRole, "progressLabel"},
        {MessageRole, "message"},
        {IsActiveRole, "isActive"},
        {CanCancelRole, "canCancel"},
        {CanOpenFolderRole, "canOpenFolder"},
        {AccessibleLabelRole, "accessibleLabel"},
    };
}

int ExportJobModel::count_in(ExportJobState state) const noexcept {
    return static_cast<int>(
        std::count_if(jobs_.begin(), jobs_.end(), [state](const ExportJob& job) { return job.state == state; }));
}

int ExportJobModel::active_count() const noexcept {
    return count_in(ExportJobState::Queued) + count_in(ExportJobState::Running);
}

int ExportJobModel::completed_count() const noexcept {
    return count_in(ExportJobState::Completed);
}

int ExportJobModel::failed_count() const noexcept {
    return count_in(ExportJobState::Failed);
}

void ExportJobModel::set_locale(const QLocale& locale) {
    if (locale == locale_) {
        return;
    }
    locale_ = locale;
    if (!jobs_.empty()) {
        emit dataChanged(index(0), index(count() - 1));
    }
}

void ExportJobModel::add_job(const ExportJob& job) {
    if (job.id == 0) {
        return;
    }
    if (positions_.contains(job.id)) {
        update_job(job);
        return;
    }
    const int row = count();
    beginInsertRows({}, row, row);
    positions_.insert(job.id, jobs_.size());
    jobs_.push_back(job);
    endInsertRows();
    emit countsChanged();
}

void ExportJobModel::update_job(const ExportJob& job) {
    const auto found = positions_.constFind(job.id);
    if (found == positions_.cend()) {
        return;
    }
    ExportJob& stored = jobs_[*found];
    const bool state_changed = stored.state != job.state;
    stored = job;
    const QModelIndex changed = index(static_cast<int>(*found));
    emit dataChanged(changed, changed);
    if (state_changed) {
        emit countsChanged();
    }
}

}  // namespace lucent::ui
