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
// status (running jobs, ETA once the service has measured three photos, attention) is read every
// `status_interval` and its review count and left-out photos (from its report) at most every
// `review_interval` (longer for large batches: a report reads every job), and once more when it
// finishes. A status read that started before newer events were applied does not overwrite their
// state or counts.
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
    std::chrono::milliseconds review_interval{4000};
    std::uint32_t list_limit = 100;
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
    void cancel(BatchId batch) override;
    void retry_failed(BatchId batch) override;
    void refresh() override;
    void request_report(BatchId batch) override;
    void request_report_file(BatchId batch) override;
    [[nodiscard]] bool is_busy() const override;

    // The current snapshot of `batch` (tests); nullptr when unknown.
    [[nodiscard]] const BatchSnapshot* snapshot(BatchId batch) const;
    // How many times the UI thread applied coalesced events (tests).
    [[nodiscard]] int flushes() const noexcept { return flushes_; }

    struct Shared;  // implementation detail: the observer's pending events

    // What a batch's snapshot takes from its report (read off the UI thread): the review count, the
    // photos left out at submission (the report lists them as skipped) and, for a batch of one, its
    // output.
    struct ReportFacts {
        qint64 review = 0;
        qint64 excluded = 0;
        QString single_output;
    };

private:
    struct StatusResult {
        BatchId id = 0;
        std::optional<BatchSnapshot> snapshot;  // std::nullopt: the status could not be read
        std::optional<ReportFacts> facts;
        quint64 sequence = 0;  // sequence_ when the read was dispatched
    };

    // Applies `facts`; true when the snapshot changed.
    static bool apply_facts(BatchSnapshot& snapshot, const ReportFacts& facts);

    void schedule_flush();
    void flush();
    void on_status_tick();
    // Reads the status (and with `with_review` the report facts) of `batch`; false when a read of
    // it is already outstanding.
    bool request_status(BatchId batch, bool with_review);
    void apply_status(const std::vector<StatusResult>& results);
    void apply_listed(const std::vector<BatchSnapshot>& listed);
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
    std::set<BatchId> review_dirty_;        // photos finished since the review count was read
    std::map<BatchId, std::chrono::steady_clock::time_point> review_read_;
    std::set<BatchId> final_refresh_;       // finished batches to read once more
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
