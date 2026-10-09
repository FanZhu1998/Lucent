#pragma once

// BatchReportModel: the report of the batch selected in the Exports view (blueprint section 2
// step 5: "a report of exported, skipped, failed, and review-recommended items"): its counts and
// notes, and one row per photo with its outcome, output, reason, suggested action and review notes,
// filtered by outcome ("all", "exported", "skipped", "failed", "review", "cancelled", "analysed").
// W8: the rows of an automatic batch also say what Auto did (the automatic revision, its
// confidence, a conservative fallback, the related-shot group, the other findings and the locks).
//
// The report is read by the BatchService off the UI thread. While the shown batch changes (photos
// finish), the report is read again at most once per refresh interval (2 s, or 2 ms per photo for
// large batches, at most 15 s), so a large running batch does not re-read its report for every
// photo. A report read again for the same batch updates the rows in place (changed rows, rows
// entering or leaving the filter), so a list scrolled down keeps its place; another batch resets
// the rows and the filter to "all".
//
// QML-facing members use QML naming (lowerCamelCase); C++-only members use snake_case.
//
// Thread-safety: UI thread only.

#include <chrono>
#include <vector>

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QLocale>
#include <QStringList>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>

#include "services/batch_service.hpp"

namespace lucent::ui {

class BatchReportModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("BatchReportModel is provided by AppController.batchReport.")
    Q_PROPERTY(qulonglong batchId READ batch_id NOTIFY reportChanged FINAL)
    Q_PROPERTY(bool loading READ loading NOTIFY reportChanged FINAL)
    Q_PROPERTY(bool hasReport READ has_report NOTIFY reportChanged FINAL)
    Q_PROPERTY(QString error READ error NOTIFY reportChanged FINAL)
    Q_PROPERTY(QString name READ name NOTIFY reportChanged FINAL)
    Q_PROPERTY(QString folder READ folder NOTIFY reportChanged FINAL)
    Q_PROPERTY(QString format READ format NOTIFY reportChanged FINAL)
    Q_PROPERTY(QString timesLabel READ times_label NOTIFY reportChanged FINAL)
    Q_PROPERTY(bool finished READ finished NOTIFY reportChanged FINAL)
    Q_PROPERTY(int exported READ exported NOTIFY reportChanged FINAL)
    Q_PROPERTY(int skipped READ skipped NOTIFY reportChanged FINAL)
    Q_PROPERTY(int failed READ failed NOTIFY reportChanged FINAL)
    Q_PROPERTY(int cancelled READ cancelled NOTIFY reportChanged FINAL)
    Q_PROPERTY(int pending READ pending NOTIFY reportChanged FINAL)
    Q_PROPERTY(int review READ review NOTIFY reportChanged FINAL)
    Q_PROPERTY(int total READ total NOTIFY reportChanged FINAL)
    // W8: an automatic batch's report (its rows say what Auto did), and its analysed photos
    // (analysing only).
    Q_PROPERTY(bool automatic READ automatic NOTIFY reportChanged FINAL)
    Q_PROPERTY(int analysed READ analysed NOTIFY reportChanged FINAL)
    Q_PROPERTY(QStringList notes READ notes NOTIFY reportChanged FINAL)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged FINAL)
    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)

public:
    enum Role {
        SourceNameRole = Qt::UserRole + 1,
        SourcePathRole,
        OutcomeKeyRole,  // presentation::outcome_key
        OutcomeLabelRole,
        OutputRole,      // full path; may be empty
        OutputNameRole,  // file name of OutputRole
        ReasonRole,
        ActionRole,
        ReviewRole,  // QStringList
        HasReviewRole,
        AttemptsRole,
        AccessibleLabelRole,
        // W8: automatic batches.
        AutomaticRole,         // bool: the row says what Auto did
        AutomaticSummaryRole,  // "Automatic revision 3 · confidence 82 % · conservative fallback"
        AutoNotesRole,         // QStringList: Auto's other findings (those not in ReviewRole)
        LockNotesRole,         // QStringList: what locked controls did
        LocksPreventedRole,    // bool: locks kept Auto from its usual targets
        // W10: the photo's camera support state as the batch recorded it.
        SupportKeyRole,      // presentation::support_key ("unknown" when not recorded)
        SupportLabelRole,    // presentation::support_label; empty when not recorded
        SupportMeaningRole,  // presentation::support_meaning; empty when not recorded
    };
    Q_ENUM(Role)

    // `service` must outlive the model.
    explicit BatchReportModel(BatchService& service, QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    // Shows the report of `batch` (0 clears). Showing the batch already shown reads it again.
    Q_INVOKABLE void show(qulonglong batch);
    Q_INVOKABLE void clear() { show(0); }
    Q_INVOKABLE void reload();

    [[nodiscard]] qulonglong batch_id() const noexcept { return static_cast<qulonglong>(batch_); }
    [[nodiscard]] bool loading() const noexcept { return loading_; }
    [[nodiscard]] bool has_report() const noexcept { return report_.ok && report_.id == batch_ && batch_ != 0; }
    [[nodiscard]] QString error() const { return report_.error; }
    [[nodiscard]] QString name() const { return report_.name; }
    [[nodiscard]] QString folder() const { return report_.folder; }
    [[nodiscard]] QString format() const { return report_.format; }
    [[nodiscard]] QString times_label() const;
    [[nodiscard]] bool finished() const noexcept {
        return report_.state == BatchState::Completed || report_.state == BatchState::Cancelled;
    }
    [[nodiscard]] int exported() const noexcept { return static_cast<int>(report_.exported); }
    [[nodiscard]] int skipped() const noexcept { return static_cast<int>(report_.skipped); }
    [[nodiscard]] int failed() const noexcept { return static_cast<int>(report_.failed); }
    [[nodiscard]] int cancelled() const noexcept { return static_cast<int>(report_.cancelled); }
    [[nodiscard]] int pending() const noexcept { return static_cast<int>(report_.pending); }
    [[nodiscard]] int review() const noexcept { return static_cast<int>(report_.review); }
    [[nodiscard]] int total() const noexcept { return static_cast<int>(report_.entries.size()); }
    [[nodiscard]] bool automatic() const noexcept { return report_.automatic != BatchAutomatic::Off; }
    [[nodiscard]] int analysed() const noexcept { return static_cast<int>(report_.analysed); }
    [[nodiscard]] QStringList notes() const { return report_.notes; }
    [[nodiscard]] QString filter() const { return filter_; }
    void setFilter(const QString& filter);
    [[nodiscard]] int count() const noexcept { return static_cast<int>(rows_.size()); }

    [[nodiscard]] const BatchReportEntry* entry_at(int row) const noexcept;
    void set_refresh_interval(std::chrono::milliseconds interval);
    void set_locale(const QLocale& locale) { locale_ = locale; }

signals:
    void reportChanged();
    void filterChanged();
    void countChanged();

private:
    void on_report(BatchId batch, const BatchReportInfo& report);
    void on_batch_changed(const BatchSnapshot& batch);
    void rebuild_rows();
    // Replaces the report of the shown batch with `report` (the same entries in the same order),
    // updating rows in place.
    void update_rows(const BatchReportInfo& report);
    [[nodiscard]] bool matches(const BatchReportEntry& entry) const;
    [[nodiscard]] QString automatic_summary(const BatchReportEntry& entry) const;

    BatchService& service_;
    BatchId batch_ = 0;
    BatchReportInfo report_;
    std::vector<int> rows_;  // indices into report_.entries that pass the filter
    QString filter_ = QStringLiteral("all");
    bool loading_ = false;
    bool reload_pending_ = false;  // a change arrived while a read was in flight
    QTimer refresh_timer_;
    QElapsedTimer last_read_;  // since the last report was requested
    int refresh_interval_ms_ = 2000;  // set_refresh_interval
    QLocale locale_;
    BatchSnapshot last_seen_;
};

}  // namespace lucent::ui
