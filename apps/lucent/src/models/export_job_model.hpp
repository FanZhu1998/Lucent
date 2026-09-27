#pragma once

// ExportJobModel: the Exports view's list of export jobs, mirroring the ExportService's job
// snapshots in queue order (the order the service added them, which is the order it runs them),
// so the jobs of a batch read top to bottom as they are processed.
//
// QML-facing members use QML naming (lowerCamelCase); C++-only members use snake_case.
//
// Thread-safety: UI thread only.

#include <optional>
#include <vector>

#include <QAbstractListModel>
#include <QHash>
#include <QLocale>
#include <QtQmlIntegration/qqmlintegration.h>

#include "services/export_service.hpp"

namespace lucent::ui {

class ExportJobModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("ExportJobModel is provided by AppController.exports.")
    Q_PROPERTY(int count READ count NOTIFY countsChanged FINAL)
    Q_PROPERTY(int activeCount READ active_count NOTIFY countsChanged FINAL)
    Q_PROPERTY(int completedCount READ completed_count NOTIFY countsChanged FINAL)
    Q_PROPERTY(int failedCount READ failed_count NOTIFY countsChanged FINAL)

public:
    enum Role {
        SourceNameRole = Qt::UserRole + 1,
        OutputFolderRole,     // native separators
        DestinationPathRole,  // native separators; may be empty
        FormatLabelRole,      // presentation::export_options_label
        StateKeyRole,         // presentation::job_state_key
        StateLabelRole,
        ProgressRole,         // 0..1
        ProgressLabelRole,    // "42 %" while running, "" otherwise
        MessageRole,
        IsActiveRole,
        CanCancelRole,        // active and cancellation not yet requested
        CanOpenFolderRole,
        AccessibleLabelRole,
    };
    Q_ENUM(Role)

    // `service` must outlive the model.
    explicit ExportJobModel(ExportService& service, QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] int count() const noexcept { return static_cast<int>(jobs_.size()); }
    [[nodiscard]] int active_count() const noexcept;
    [[nodiscard]] int completed_count() const noexcept;
    [[nodiscard]] int failed_count() const noexcept;

    // The job at `row`, or nullptr when out of range. Valid until the model next changes.
    [[nodiscard]] const ExportJob* job_at(int row) const noexcept;
    [[nodiscard]] std::optional<int> row_of(JobId id) const;

    void set_locale(const QLocale& locale);

signals:
    void countsChanged();

private:
    void add_job(const ExportJob& job);
    void update_job(const ExportJob& job);
    [[nodiscard]] int count_in(ExportJobState state) const noexcept;

    std::vector<ExportJob> jobs_;          // row order
    QHash<JobId, std::size_t> positions_;  // job id -> row
    QLocale locale_;
};

}  // namespace lucent::ui
