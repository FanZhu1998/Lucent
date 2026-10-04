#pragma once

// BatchListModel: the Exports view's list of batches (blueprint section 2 step 5), newest first,
// mirroring the BatchService's snapshots: live counts (exported, remaining, failed, skipped, for
// review), the current stage, an ETA only when the service has one, the attention with its
// actionable text, and the commands (pause, resume, cancel, retry failed). Single exports are
// batches of one and read as one photo. Submissions that have not created their batch yet
// (importing a folder, fingerprinting originals) are shown as "preparing".
//
// The service already coalesces its updates (a few per second per batch); this model only updates
// the row that changed and emits summaryChanged when the summary moved.
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

#include "services/batch_service.hpp"

namespace lucent::ui {

class BatchListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("BatchListModel is provided by AppController.batches.")
    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)
    Q_PROPERTY(bool available READ available CONSTANT FINAL)
    Q_PROPERTY(QString unavailableReason READ unavailable_reason CONSTANT FINAL)
    // Batches with photos still to export (running, paused or waiting for attention).
    Q_PROPERTY(int activeCount READ active_count NOTIFY summaryChanged FINAL)
    Q_PROPERTY(int attentionCount READ attention_count NOTIFY summaryChanged FINAL)
    // Photos still to export in running or paused batches.
    Q_PROPERTY(int remainingPhotos READ remaining_photos NOTIFY summaryChanged FINAL)
    // "2 running · 1 needs attention"; empty when nothing is happening.
    Q_PROPERTY(QString summary READ summary NOTIFY summaryChanged FINAL)
    // A submission is preparing its batch: what it is doing ("Adding the folder's photos… 3 of 9").
    Q_PROPERTY(bool preparing READ preparing NOTIFY preparingChanged FINAL)
    Q_PROPERTY(QString preparingName READ preparing_name NOTIFY preparingChanged FINAL)
    Q_PROPERTY(QString preparingText READ preparing_text NOTIFY preparingChanged FINAL)
    Q_PROPERTY(double preparingProgress READ preparing_progress NOTIFY preparingChanged FINAL)

public:
    enum Role {
        BatchIdRole = Qt::UserRole + 1,
        NameRole,
        SingleRole,          // a batch of one photo (single exports)
        SelectionRole,       // "5 selected photos", "the folder D:\Shoot"
        FolderRole,          // the output folder
        StateKeyRole,        // presentation::batch_state_key
        StateLabelRole,
        TotalRole,
        CompletedRole,
        RemainingRole,
        FailedRole,
        SkippedRole,
        ReviewRole,          // -1 until counted
        ProgressRole,        // 0..1
        ProgressLabelRole,   // "3 of 5"
        StageRole,           // the running photos and their stages; empty when nothing runs
        EtaRole,             // empty unless the service measured one
        HasAttentionRole,
        AttentionRole,       // what happened and what to do
        CanPauseRole,
        CanResumeRole,
        CanCancelRole,
        CanRetryRole,
        CanOpenFolderRole,
        CanOpenReportRole,   // a finished batch of more than one photo
        OutputPathRole,      // the newest exported file (a batch of one: its output); may be empty
        TimesLabelRole,
        IsActiveRole,
        AccessibleLabelRole,
    };
    Q_ENUM(Role)

    // `service` must outlive the model.
    explicit BatchListModel(BatchService& service, QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] int count() const noexcept { return static_cast<int>(batches_.size()); }
    [[nodiscard]] bool available() const { return service_.is_available(); }
    [[nodiscard]] QString unavailable_reason() const { return service_.unavailable_reason(); }
    [[nodiscard]] int active_count() const noexcept;
    [[nodiscard]] int attention_count() const noexcept;
    [[nodiscard]] int remaining_photos() const noexcept;
    [[nodiscard]] QString summary() const;
    [[nodiscard]] bool preparing() const noexcept { return !submissions_.empty(); }
    [[nodiscard]] QString preparing_name() const;
    [[nodiscard]] QString preparing_text() const;
    [[nodiscard]] double preparing_progress() const;

    // Commands for the batch at `row` (ignored for an invalid row or one that cannot do it).
    Q_INVOKABLE void pause(int row);
    Q_INVOKABLE void resume(int row);
    Q_INVOKABLE void cancel(int row);
    Q_INVOKABLE void retryFailed(int row);
    // Stops the oldest preparing submission.
    Q_INVOKABLE void cancelPreparing();
    // The id of the batch at `row` (0 for an invalid row).
    Q_INVOKABLE qulonglong batchIdAt(int row) const;
    Q_INVOKABLE int rowOfBatch(qulonglong batch) const;

    // A submission whose batch is being prepared; shown until submitted() arrives for `token`.
    void track_submission(quint64 token, const QString& name);

    [[nodiscard]] const BatchSnapshot* batch_at(int row) const noexcept;
    [[nodiscard]] std::optional<int> row_of(BatchId id) const;
    void set_locale(const QLocale& locale);

signals:
    void countChanged();
    void summaryChanged();
    void preparingChanged();

private:
    struct Submission {
        quint64 token = 0;
        QString name;
        QString text;
        double fraction = -1.0;
    };

    void on_listed(const QList<BatchSnapshot>& batches);
    void on_changed(const BatchSnapshot& batch);
    void on_submit_progress(quint64 token, const QString& text, double fraction);
    void on_submitted(quint64 token, BatchId batch, const QString& error);
    void reindex();
    void emit_summary_if_changed();

    BatchService& service_;
    std::vector<BatchSnapshot> batches_;  // newest (highest id) first
    QHash<BatchId, int> rows_;
    std::vector<Submission> submissions_;
    QLocale locale_;
    QString last_summary_;
    int last_active_ = -1;
    int last_attention_ = -1;
    int last_remaining_ = -1;
};

}  // namespace lucent::ui
