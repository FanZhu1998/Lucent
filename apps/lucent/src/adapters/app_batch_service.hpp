#pragma once

// AppBatchService: the shell's BatchService (services/batch_service.hpp) over the durable batch
// service of the application (app::BatchService, ADR 0012), which the Backend opens over a writable
// persistent library.
//
// Nothing here blocks the UI thread. app::BatchService reads files and the catalog in plan(),
// submit(), status(), list(), report() and its commands, so they run on three single-thread pools of
// this adapter:
//   * planning: the newest request_plan() wins; a newer one cancels the running plan cooperatively
//     (its CancellationToken) and a superseded result is never emitted;
//   * submitting: submissions in order (a folder batch imports the folder and fingerprints its
//     originals first, which can take minutes), each cancellable until its batch is written;
//   * commands: pause, resume, cancel, retry, the list, status refreshes and reports, in call order,
//     never behind a long submission or plan.
//
// Coalescing. The service's observer runs on its coordinator and slot threads, about a hundred
// times per photo (worker progress every 1 %). It only merges the event into a per-batch pending
// record under a mutex (state and counts, attention, the running jobs' stages, the newest output,
// the report file) and posts one wake-up to the UI thread while none is outstanding. The UI thread
// applies everything that accumulated at most once per `coalesce` interval, emitting one
// batch_changed() per changed batch. So a batch of thousands of photos produces a few UI updates a
// second, whatever its size. Events carry no ETA and no review count: while a batch has work, its
// status (running jobs, ETA once the service has measured three photos and only while it runs,
// attention, review and retry counts, left-out photos; a batch of one: its output or why there is
// none) is read every `status_interval`, and once more when it finishes. No report is read for the
// list. A status read that started before newer events were applied does not overwrite their state
// or counts. A status read can find a batch finished while its last attempt still returns and the
// service writes its report: such a batch lists no running job, and its report path arrives with
// the report's own event.
//
// The list: refresh() lists the newest `list_limit` batches and every batch with work left
// whatever its age (a paused batch is never lost behind newer single exports); load_older() pages
// further back.
//
// The output folder of every submission is recorded in Backend::output_folders() (imports do not
// scan into it when they reach it below the folder being imported).
//
// Lifetime: lives on the UI thread. The destructor closes the dispatcher, ends the subscription
// (waiting for an observer call in progress), cancels the running plan and submissions and waits
// for the three pools; no signal is emitted from it.

#include <QHash>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

#include <lucent/application/batch_service.hpp>
#include <lucent/core/cancellation.hpp>

#include "adapters/backend.hpp"
#include "adapters/ui_dispatcher.hpp"
#include "services/batch_service.hpp"

namespace lucent::ui::adapters {

struct BatchAdapterOptions {
    std::chrono::milliseconds coalesce{200};
    std::chrono::milliseconds status_interval{1000};
    std::uint32_t list_limit = 100;  // newest batches listed at once (and per load_older page)
};

class AppBatchService final : public BatchService {
    Q_OBJECT

public:
    explicit AppBatchService(std::shared_ptr<Backend> backend, BatchAdapterOptions options = {},
                             QObject* parent = nullptr);
    ~AppBatchService() override;

    [[nodiscard]] bool is_available() const override;
    [[nodiscard]] QString unavailable_reason() const override;
    quint64 request_plan(const BatchSetup& setup) override;
    quint64 submit(const BatchSetup& setup) override;
    void cancel_submit(quint64 token) override;
    void pause(BatchId batch) override;
    void resume(BatchId batch) override;
    void use_replaced_folder(BatchId batch) override;
    void cancel(BatchId batch) override;
    void retry_failed(BatchId batch) override;
    void refresh() override;
    void load_older() override;
    void request_report(BatchId batch) override;
    void request_report_file(BatchId batch) override;
    [[nodiscard]] bool is_busy() const override;

    // The current snapshot of `batch` (tests); nullptr when unknown.
    [[nodiscard]] const BatchSnapshot* snapshot(BatchId batch) const;
    // How many times the UI thread applied coalesced events (tests).
    [[nodiscard]] int flushes() const noexcept { return flushes_; }

    struct Shared;  // implementation detail: the observer's pending events

private:
    struct StatusResult {
        BatchId id = 0;
        std::optional<BatchSnapshot> snapshot;  // std::nullopt: the status could not be read
        quint64 sequence = 0;  // sequence_ when the read was dispatched
    };

    void schedule_flush();
    void flush();
    void on_status_tick();
    // Reads the status of `batch`; false when a read of it is already outstanding.
    bool request_status(BatchId batch);
    void apply_status(const std::vector<StatusResult>& results);
    // `oldest`: the oldest batch of the page read (0: none); `more`: the page was full.
    void apply_listed(const std::vector<BatchSnapshot>& listed, BatchId oldest, bool more);
    void store(const BatchSnapshot& snapshot);
    void rebuild_running(BatchSnapshot& snapshot);
    void update_timers();
    void update_busy();
    void run_command(BatchId batch, const char* what);
    [[nodiscard]] QString source_name(ItemId item);

    std::shared_ptr<Backend> backend_;
    BatchAdapterOptions options_;
    std::shared_ptr<UiDispatcher> dispatcher_;
    std::shared_ptr<Shared> shared_;
    app::Subscription subscription_;
    std::unique_ptr<QThreadPool> planner_;
    std::unique_ptr<QThreadPool> submitter_;
    std::unique_ptr<QThreadPool> commands_;

    QTimer flush_timer_;
    QTimer status_timer_;
    std::map<BatchId, BatchSnapshot> snapshots_;
    // Running jobs per batch by job id (events add, update and remove them).
    std::map<BatchId, std::map<std::int64_t, BatchRunningJob>> running_;
    std::map<BatchId, quint64> sequence_;  // incremented whenever events changed a batch's counts
    std::set<BatchId> final_refresh_;       // finished batches to read once more
    BatchId oldest_listed_ = 0;             // the oldest batch of the pages read (load_older)
    bool older_available_ = false;
    bool listing_ = false;                  // a page is being read
    std::set<BatchId> status_requested_;    // unknown batches whose status is being read
    bool status_in_flight_ = false;
    QHash<ItemId, QString> names_;

    quint64 next_token_ = 0;
    std::atomic<quint64> latest_plan_{0};
    std::shared_ptr<core::CancellationSource> plan_cancel_;
    std::map<quint64, std::shared_ptr<core::CancellationSource>> submissions_;
    bool busy_ = false;
    int flushes_ = 0;
};

}  // namespace lucent::ui::adapters
