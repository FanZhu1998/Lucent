#pragma once

// ExportService: the shell's view of export jobs (blueprint section 2 step 5 and section 7).
//
// Threading contract: as LibraryService (library_service.hpp). The object lives on the UI thread,
// member functions return promptly, rendering and encoding run elsewhere, and signals are emitted
// on the UI thread.
//
// No-clobber: a service never overwrites an existing file (blueprint section 7); a name collision
// is resolved by the service's naming policy or reported as a failed job.

#include <QObject>

#include "services/service_types.hpp"

namespace lucent::ui {

class ExportService : public QObject {
    Q_OBJECT

public:
    explicit ExportService(QObject* parent = nullptr) : QObject(parent) {}
    ~ExportService() override = default;

    ExportService(const ExportService&) = delete;
    ExportService& operator=(const ExportService&) = delete;

    // Queues one job per item in request.items (in that order) and emits job_added for each before
    // returning or shortly after. Items the service cannot export still get a job, which fails
    // with a reason. An empty request does nothing.
    virtual void start_export(const ExportRequest& request) = 0;

    // Requests cancellation of a queued or running job: it ends in Cancelled, or in its final
    // state if it finished first. Until then its snapshot has cancel_requested set. Unknown or
    // finished jobs are ignored.
    virtual void cancel_job(JobId job) = 0;

    // True while any job is queued or running.
    [[nodiscard]] virtual bool is_busy() const = 0;

    // The file name the first export of `item` with `options` would get in an empty folder (for
    // example "IMG_0001_Lucent.jpg"), so the shell can show it before the user starts; empty when
    // the item is unknown. Computed from names only: no file-system access.
    [[nodiscard]] virtual QString suggested_file_name(ItemId item, const ExportOptions& options) const = 0;

signals:
    void job_added(const lucent::ui::ExportJob& job);
    // A complete new snapshot of an existing job.
    void job_changed(const lucent::ui::ExportJob& job);
    void busy_changed(bool busy);
};

}  // namespace lucent::ui
