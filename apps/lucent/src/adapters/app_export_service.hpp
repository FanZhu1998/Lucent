#pragma once

// AppExportService: the shell's ExportService (services/export_service.hpp) over the application
// services' single-item export (app::ExportService; docs/adr/0007).
//
// start_export() adds one job per item (job_added, Queued). Jobs run one at a time, in the order
// they were added: a job is submitted to app::ExportService only when the previous one has
// finished (blueprint section 7, "start with one full-resolution worker"). So a queue of exports
// occupies one Background thread of the executor, never all of them: an export waiting for the
// memory admission cannot hold up an import, and camera previews run on their own lane anyway
// (task_executor.hpp). A submitted job runs on the Background lane under memory admission:
// render, conversion to the chosen output profile, verified no-clobber publication with Lucent's
// naming policy ("{stem}_Lucent[_{n}].jpg|tif", never overwriting). Progress arrives on a worker
// thread and is posted to the UI thread, throttled to about ten updates a second per job (stage
// changes always pass); the completion is posted after it, so job_changed reports Running updates
// and then the final state. A job cancelled while it waits its turn ends Cancelled at once.
// Unsupported items are refused by the application service with a reason; they are never
// exported silently (blueprint section 3).
//
// The output folder of every request is recorded in Backend::output_folders(): later imports do
// not scan into it when they reach it below the folder being imported (import.hpp).
//
// Lifetime and threading: lives on the UI thread; the destructor closes the dispatcher (later
// progress and completions are dropped) and requests cancellation of active jobs. The Backend's
// app::ExportService waits for their completions when it is destroyed. No signals are emitted
// from the destructor.

#include <deque>
#include <map>
#include <memory>
#include <optional>

#include <lucent/application/task_executor.hpp>

#include "adapters/backend.hpp"
#include "adapters/ui_dispatcher.hpp"
#include "services/export_service.hpp"

namespace lucent::ui::adapters {

class AppExportService final : public ExportService {
    Q_OBJECT

public:
    explicit AppExportService(std::shared_ptr<Backend> backend, QObject* parent = nullptr);
    ~AppExportService() override;

    void start_export(const ExportRequest& request) override;
    void cancel_job(JobId job) override;
    [[nodiscard]] bool is_busy() const override;
    [[nodiscard]] QString suggested_file_name(ItemId item, const ExportOptions& options) const override;

private:
    struct Job {
        ExportJob snapshot;
        app::ExportRequest request;  // what is submitted when the job's turn comes
        bool source_known = false;   // the item was in the library when the job was added
        app::TaskHandle handle;      // valid while submitted
    };

    // Submits waiting jobs until one is running (or none is left).
    void submit_next();
    void on_progress(JobId id, const app::ExportProgress& progress);
    void on_completed(JobId id, const Expected<app::ExportRecord>& record);
    void publish(Job& job);
    void update_busy();

    std::shared_ptr<Backend> backend_;
    std::shared_ptr<UiDispatcher> dispatcher_;
    std::map<JobId, Job> jobs_;
    std::deque<JobId> waiting_;       // Queued jobs not yet submitted, in order
    std::optional<JobId> submitted_;  // the job app::ExportService is running
    JobId next_id_ = 1;
    bool busy_ = false;
};

}  // namespace lucent::ui::adapters
