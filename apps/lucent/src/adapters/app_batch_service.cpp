#include "adapters/app_batch_service.hpp"

#include <QCoreApplication>
#include <QDir>
#include <algorithm>
#include <filesystem>
#include <utility>

#include <lucent/core/log.hpp>

#include "adapters/batch_conversions.hpp"
#include "adapters/conversions.hpp"

namespace lucent::ui::adapters {
namespace {

constexpr std::string_view kLog = "ui.batches";
constexpr auto kSubmitProgressInterval = std::chrono::milliseconds(100);

QString tr_text(const char* text) {
    return QCoreApplication::translate("lucent::ui::adapters", text);
}

catalog::BatchId to_catalog(BatchId id) {
    return catalog::BatchId{static_cast<std::int64_t>(id)};
}

bool is_terminal(catalog::BatchState state) {
    return state == catalog::BatchState::Completed || state == catalog::BatchState::Cancelled;
}

}  // namespace

// The observer's pending events (see the file comment of the header).
struct AppBatchService::Shared {
    struct Job {
        ItemId item = 0;
        workers::JobStage stage = workers::JobStage::Preparing;
        double fraction = 0.0;
        bool finished = false;  // the attempt ended: remove the running job
    };
    struct Batch {
        bool has_counts = false;
        catalog::BatchState state = catalog::BatchState::Running;
        catalog::BatchCounts counts;
        bool has_attention = false;
        app::BatchAttention attention = app::BatchAttention::None;
        std::optional<std::uint64_t> attention_free_bytes;
        std::uint64_t attention_needed_bytes = 0;
        std::map<std::int64_t, Job> jobs;
        std::optional<std::filesystem::path> last_output;
        std::optional<std::filesystem::path> report;
        bool created = false;
        bool finished = false;
    };

    std::mutex mutex;
    std::map<std::int64_t, Batch> batches;
    bool posted = false;
};

AppBatchService::AppBatchService(std::shared_ptr<Backend> backend, BatchAdapterOptions options, QObject* parent)
    : BatchService(parent),
      backend_(std::move(backend)),
      options_(options),
      dispatcher_(std::make_shared<UiDispatcher>(this)),
      shared_(std::make_shared<Shared>()),
      planner_(std::make_unique<QThreadPool>()),
      submitter_(std::make_unique<QThreadPool>()),
      commands_(std::make_unique<QThreadPool>()) {
    for (auto* pool : {planner_.get(), submitter_.get(), commands_.get()}) {
        pool->setMaxThreadCount(1);
        pool->setExpiryTimeout(-1);
    }
    planner_->setObjectName(QStringLiteral("lucent-batch-plans"));
    submitter_->setObjectName(QStringLiteral("lucent-batch-submissions"));
    commands_->setObjectName(QStringLiteral("lucent-batch-commands"));

    flush_timer_.setSingleShot(true);
    flush_timer_.setInterval(static_cast<int>(options_.coalesce.count()));
    connect(&flush_timer_, &QTimer::timeout, this, &AppBatchService::flush);
    status_timer_.setInterval(static_cast<int>(options_.status_interval.count()));
    connect(&status_timer_, &QTimer::timeout, this, &AppBatchService::on_status_tick);

    app::BatchService* batches = backend_->batches();
    if (batches == nullptr) {
        return;
    }
    subscription_ =
        batches->subscribe([shared = shared_, dispatcher = dispatcher_, this](const app::BatchEvent& event) {
            // Service thread: merge and wake the UI thread once (it applies everything accumulated).
            bool post = false;
            {
                const std::lock_guard lock(shared->mutex);
                Shared::Batch& batch = shared->batches[event.batch.value];
                switch (event.kind) {
                    case app::BatchEventKind::JobStarted:
                    case app::BatchEventKind::JobProgress:
                        // These events carry no counts (and always the Running state).
                        if (event.job) {
                            Shared::Job& job = batch.jobs[event.job->value];
                            job.item = event.item ? event.item->value : 0;
                            job.stage = event.worker_stage;
                            job.fraction = event.fraction;
                            job.finished = false;
                        }
                        break;
                    case app::BatchEventKind::JobFinished:
                        if (event.job) {
                            batch.jobs[event.job->value].finished = true;
                        }
                        if (event.job_stage == catalog::JobStage::Complete && event.output) {
                            batch.last_output = event.output;
                        }
                        [[fallthrough]];
                    case app::BatchEventKind::Created:
                    case app::BatchEventKind::StateChanged:
                    case app::BatchEventKind::Attention:
                    case app::BatchEventKind::Finished:
                        batch.has_counts = true;
                        batch.state = event.state;
                        batch.counts = event.counts;
                        batch.has_attention = true;
                        if (event.kind == app::BatchEventKind::Attention || event.attention != batch.attention) {
                            batch.attention_free_bytes = event.attention_free_bytes;
                            batch.attention_needed_bytes = event.attention_needed_bytes;
                        }
                        batch.attention = event.attention;
                        batch.created = batch.created || event.kind == app::BatchEventKind::Created;
                        batch.finished = batch.finished || event.kind == app::BatchEventKind::Finished;
                        break;
                    case app::BatchEventKind::ReportWritten:
                        batch.report = event.output;
                        break;
                }
                if (!shared->posted) {
                    shared->posted = true;
                    post = true;
                }
            }
            if (post) {
                dispatcher->post([this] { schedule_flush(); });
            }
        });
    refresh();
}

AppBatchService::~AppBatchService() {
    dispatcher_->close();
    subscription_.reset();  // waits for an observer call in progress
    if (plan_cancel_) {
        plan_cancel_->cancel();
    }
    for (auto& [token, source] : submissions_) {
        source->cancel();
    }
    planner_->waitForDone();
    submitter_->waitForDone();
    commands_->waitForDone();
}

bool AppBatchService::is_available() const {
    return backend_->batches() != nullptr;
}

QString AppBatchService::unavailable_reason() const {
    if (is_available()) {
        return {};
    }
    if (backend_->library_problem()) {
        return tr_text("Exports cannot be kept as batches because the library is not saved in this session.");
    }
    return tr_text("Batch export is unavailable in this session.");
}

quint64 AppBatchService::request_plan(const BatchSetup& setup) {
    const quint64 token = ++next_token_;
    latest_plan_.store(token);
    if (plan_cancel_) {
        plan_cancel_->cancel();  // the previous plan is superseded
    }
    app::BatchService* batches = backend_->batches();
    if (batches == nullptr) {
        BatchPlanInfo plan;
        plan.error = unavailable_reason();
        dispatcher_->post([this, token, plan] { emit plan_ready(token, plan); });
        return token;
    }
    auto source = std::make_shared<core::CancellationSource>();
    plan_cancel_ = source;
    const bool folder_batch = !setup.input_folder.isEmpty();
    const QString pattern = setup.name_pattern;
    app::BatchRequest request = to_batch_request(setup);
    planner_->start([backend = backend_, dispatcher = dispatcher_, this, token, source, folder_batch, pattern,
                     request = std::move(request)] {
        if (source->is_cancelled() || latest_plan_.load() != token) {
            return;  // superseded before it started
        }
        auto plan = backend->batches()->plan(request, source->token());
        if (!plan && plan.error().code == ErrorCode::Cancelled) {
            return;
        }
        BatchPlanInfo info = plan ? to_plan_info(*plan) : to_plan_failure(plan.error(), folder_batch, pattern);
        dispatcher->post([this, token, info = std::move(info)] {
            if (latest_plan_.load() == token) {
                emit plan_ready(token, info);
            }
        });
    });
    return token;
}

quint64 AppBatchService::submit(const BatchSetup& setup) {
    const quint64 token = ++next_token_;
    app::BatchService* batches = backend_->batches();
    if (batches == nullptr) {
        const QString reason = unavailable_reason();
        dispatcher_->post([this, token, reason] { emit submitted(token, 0, reason); });
        return token;
    }
    backend_->output_folders().add(to_path(QDir::cleanPath(setup.output_folder)));
    auto source = std::make_shared<core::CancellationSource>();
    submissions_[token] = source;
    app::BatchRequest request = to_batch_request(setup);
    submitter_->start([backend = backend_, dispatcher = dispatcher_, this, token, source,
                       request = std::move(request)] {
        if (source->is_cancelled()) {
            dispatcher->post([this, token] {
                submissions_.erase(token);
                emit submitted(token, 0, tr_text("Cancelled."));
                update_busy();
            });
            return;
        }
        std::optional<app::BatchSubmitProgress::Phase> phase;
        auto last = std::chrono::steady_clock::time_point{};
        auto submitted_batch = backend->batches()->submit(
            request,
            [&](const app::BatchSubmitProgress& progress) {
                const auto now = std::chrono::steady_clock::now();
                if (phase == progress.phase && now - last < kSubmitProgressInterval) {
                    return;
                }
                phase = progress.phase;
                last = now;
                const QString text = describe_submit_progress(progress);
                const double fraction = submit_fraction(progress);
                dispatcher->post([this, token, text, fraction] { emit submit_progress(token, text, fraction); });
            },
            source->token());
        BatchId id = 0;
        QString error;
        if (submitted_batch) {
            id = static_cast<BatchId>(submitted_batch->id.value);
        } else {
            error = describe_batch_error(submitted_batch.error());
            core::log_warn(kLog, "the batch was not created: {}", submitted_batch.error().describe());
        }
        dispatcher->post([this, token, id, error] {
            submissions_.erase(token);
            if (id != 0 && !snapshots_.contains(id)) {
                request_status(id);
            }
            emit submitted(token, id, error);
            update_busy();
        });
    });
    update_busy();
    return token;
}

void AppBatchService::cancel_submit(quint64 token) {
    if (const auto found = submissions_.find(token); found != submissions_.end()) {
        found->second->cancel();
    }
}

void AppBatchService::run_command(BatchId batch, const char* what) {
    app::BatchService* batches = backend_->batches();
    if (batches == nullptr) {
        const QString reason = unavailable_reason();
        dispatcher_->post([this, batch, reason] { emit command_failed(batch, reason); });
        return;
    }
    const std::string command = what;
    commands_->start([backend = backend_, dispatcher = dispatcher_, this, batch, command] {
        app::BatchService& service = *backend->batches();
        const catalog::BatchId id = to_catalog(batch);
        Status result = ok();
        if (command == "pause") {
            result = service.pause(id);
        } else if (command == "resume") {
            result = service.resume(id);
        } else if (command == "use_folder") {
            result = service.use_replaced_folder(id);
        } else if (command == "cancel") {
            result = service.cancel(id);
        } else if (command == "retry") {
            auto retried = service.retry_failed(id);
            if (!retried) {
                result = Unexpected(std::move(retried.error()));
            }
        }
        if (!result) {
            core::log_warn(kLog, "batch {}: {} failed: {}", batch, command, result.error().describe());
            const QString message = result.error().where == "catalog.batch.invalid_transition"
                                        ? tr_text("The batch has changed meanwhile; try again.")
                                        : describe_batch_error(result.error());
            dispatcher->post([this, batch, message] { emit command_failed(batch, message); });
        }
    });
}

void AppBatchService::pause(BatchId batch) {
    run_command(batch, "pause");
}

void AppBatchService::resume(BatchId batch) {
    run_command(batch, "resume");
}

void AppBatchService::use_replaced_folder(BatchId batch) {
    run_command(batch, "use_folder");
}

void AppBatchService::cancel(BatchId batch) {
    run_command(batch, "cancel");
}

void AppBatchService::retry_failed(BatchId batch) {
    run_command(batch, "retry");
}

void AppBatchService::refresh() {
    if (backend_->batches() == nullptr) {
        dispatcher_->post([this] { emit batches_listed({}); });
        return;
    }
    const std::uint32_t limit = options_.list_limit;
    listing_ = true;
    commands_->start([backend = backend_, dispatcher = dispatcher_, this, limit] {
        // The newest batches, and every batch with work left whatever its age.
        auto listed = backend->batches()->list(limit);
        if (!listed) {
            core::log_warn(kLog, "cannot list the batches: {}", listed.error().describe());
            dispatcher->post([this] { listing_ = false; });
            return;
        }
        auto snapshots = std::make_shared<std::vector<BatchSnapshot>>();
        snapshots->reserve(listed->size());
        std::set<BatchId> seen;
        BatchId oldest = 0;
        for (const app::BatchStatus& status : *listed) {
            snapshots->push_back(to_snapshot(status));
            seen.insert(snapshots->back().id);
            oldest = snapshots->back().id;
        }
        const bool more = listed->size() >= limit;
        if (auto unfinished = backend->batches()->list_unfinished()) {
            for (const app::BatchStatus& status : *unfinished) {
                if (!seen.contains(static_cast<BatchId>(status.record.id.value))) {
                    snapshots->push_back(to_snapshot(status));
                }
            }
        } else {
            core::log_warn(kLog, "cannot list the unfinished batches: {}", unfinished.error().describe());
        }
        dispatcher->post([this, snapshots, oldest, more] { apply_listed(*snapshots, oldest, more); });
    });
}

void AppBatchService::load_older() {
    if (backend_->batches() == nullptr || listing_ || !older_available_ || oldest_listed_ == 0) {
        return;
    }
    const std::uint32_t limit = options_.list_limit;
    const BatchId before = oldest_listed_;
    listing_ = true;
    commands_->start([backend = backend_, dispatcher = dispatcher_, this, limit, before] {
        auto listed = backend->batches()->list(limit, to_catalog(before));
        if (!listed) {
            core::log_warn(kLog, "cannot list older batches: {}", listed.error().describe());
            dispatcher->post([this] { listing_ = false; });
            return;
        }
        auto snapshots = std::make_shared<std::vector<BatchSnapshot>>();
        BatchId oldest = 0;
        for (const app::BatchStatus& status : *listed) {
            snapshots->push_back(to_snapshot(status));
            oldest = snapshots->back().id;
        }
        const bool more = listed->size() >= limit;
        dispatcher->post([this, snapshots, oldest, more] { apply_listed(*snapshots, oldest, more); });
    });
}

void AppBatchService::request_report(BatchId batch) {
    if (backend_->batches() == nullptr) {
        BatchReportInfo report;
        report.id = batch;
        report.error = unavailable_reason();
        dispatcher_->post([this, batch, report] { emit report_ready(batch, report); });
        return;
    }
    commands_->start([backend = backend_, dispatcher = dispatcher_, this, batch] {
        auto report = backend->batches()->report(to_catalog(batch));
        BatchReportInfo info;
        if (report) {
            info = to_report_info(*report);
        } else {
            info.id = batch;
            info.error = describe_batch_error(report.error());
        }
        dispatcher->post([this, batch, info = std::move(info)] { emit report_ready(batch, info); });
    });
}

void AppBatchService::request_report_file(BatchId batch) {
    if (backend_->batches() == nullptr) {
        const QString reason = unavailable_reason();
        dispatcher_->post([this, batch, reason] { emit report_file_ready(batch, {}, reason); });
        return;
    }
    commands_->start([backend = backend_, dispatcher = dispatcher_, this, batch] {
        app::BatchService& service = *backend->batches();
        QString path;
        QString error;
        auto folder = service.output_folder(to_catalog(batch));
        if (folder) {
            const std::filesystem::path html = report_html_path(*folder, to_catalog(batch));
            std::error_code ec;
            if (std::filesystem::is_regular_file(html, ec)) {
                path = QDir::toNativeSeparators(to_qstring(html));
            }
        }
        if (path.isEmpty()) {
            auto written = service.write_report(to_catalog(batch));
            if (written) {
                path = QDir::toNativeSeparators(to_qstring(written->html));
            } else {
                error = describe_batch_error(written.error());
            }
        }
        dispatcher->post([this, batch, path, error] {
            if (!path.isEmpty()) {
                if (const auto found = snapshots_.find(batch); found != snapshots_.end() &&
                                                               found->second.report_path != path) {
                    found->second.report_path = path;
                    emit batch_changed(found->second);
                }
            }
            emit report_file_ready(batch, path, error);
        });
    });
}

bool AppBatchService::is_busy() const {
    // Submissions, and the final reads of batches that just finished (their final counts).
    if (!submissions_.empty() || !status_requested_.empty() || !final_refresh_.empty()) {
        return true;
    }
    return std::any_of(snapshots_.begin(), snapshots_.end(), [](const auto& entry) {
        return entry.second.state == BatchState::Running && entry.second.remaining() > 0;
    });
}

const BatchSnapshot* AppBatchService::snapshot(BatchId batch) const {
    const auto found = snapshots_.find(batch);
    return found == snapshots_.end() ? nullptr : &found->second;
}

QString AppBatchService::source_name(ItemId item) {
    if (item == 0) {
        return {};
    }
    if (const auto found = names_.constFind(item); found != names_.cend()) {
        return *found;
    }
    QString name;
    if (const std::optional<app::SourceItem> source = backend_->library().find(app::ItemId{item})) {
        name = to_qstring(source->path.filename());
    }
    names_.insert(item, name);
    return name;
}

void AppBatchService::schedule_flush() {
    if (!flush_timer_.isActive()) {
        flush_timer_.start();
    }
}

void AppBatchService::rebuild_running(BatchSnapshot& snapshot) {
    snapshot.running_jobs.clear();
    if (const auto found = running_.find(snapshot.id); found != running_.end()) {
        for (const auto& [job, running] : found->second) {
            snapshot.running_jobs.push_back(running);
        }
    }
}

void AppBatchService::flush() {
    std::map<std::int64_t, Shared::Batch> pending;
    {
        const std::lock_guard lock(shared_->mutex);
        pending.swap(shared_->batches);
        shared_->posted = false;
    }
    if (pending.empty()) {
        return;
    }
    ++flushes_;
    for (auto& [raw_id, batch] : pending) {
        const BatchId id = static_cast<BatchId>(raw_id);
        // Running jobs first: they are kept even for a batch whose snapshot is not known yet.
        auto& jobs = running_[id];
        for (const auto& [job, change] : batch.jobs) {
            if (change.finished) {
                jobs.erase(job);
                continue;
            }
            BatchRunningJob& running = jobs[job];
            running.job = job;
            running.item = change.item;
            running.source_name = source_name(change.item);
            running.stage = describe_worker_stage(change.stage);
            running.fraction = std::clamp(change.fraction, 0.0, 1.0);
        }
        if (batch.has_counts && is_terminal(batch.state) && batch.counts.leased() == 0) {
            jobs.clear();
        }
        const auto found = snapshots_.find(id);
        if (found == snapshots_.end()) {
            // A new batch (or one not listed): read it completely.
            if (!request_status(id) && batch.finished) {
                final_refresh_.insert(id);
            }
            continue;
        }
        BatchSnapshot& snapshot = found->second;
        if (batch.has_counts) {
            apply_counts(snapshot, batch.state, batch.counts);
            ++sequence_[id];
        }
        // Finished: its time, review and retry counts and output are read at once (not at the next
        // tick; while it runs, the tick reads them every status_interval).
        if (batch.finished && !request_status(id)) {
            final_refresh_.insert(id);
        }
        if (batch.has_attention) {
            snapshot.attention = to_ui_attention(batch.attention);
            snapshot.attention_message = describe_attention(snapshot.attention, snapshot.folder,
                                                            batch.attention_free_bytes, batch.attention_needed_bytes);
        }
        if (batch.last_output) {
            snapshot.last_output = QDir::toNativeSeparators(to_qstring(*batch.last_output));
        }
        if (batch.report) {
            snapshot.report_path = QDir::toNativeSeparators(to_qstring(*batch.report));
        }
        if (snapshot.state != BatchState::Running || snapshot.attention != BatchAttention::None) {
            snapshot.eta_seconds = -1.0;  // stalled or done: no time left to tell
        }
        rebuild_running(snapshot);
        emit batch_changed(snapshot);
    }
    update_timers();
    update_busy();
}

bool AppBatchService::request_status(BatchId batch) {
    if (backend_->batches() == nullptr || !status_requested_.insert(batch).second) {
        return false;
    }
    const quint64 sequence = sequence_[batch];
    commands_->start([backend = backend_, dispatcher = dispatcher_, this, batch, sequence] {
        StatusResult result;
        result.id = batch;
        result.sequence = sequence;
        if (auto status = backend->batches()->status(to_catalog(batch))) {
            result.snapshot = to_snapshot(*status);
        }
        dispatcher->post([this, result = std::move(result)] {
            status_requested_.erase(result.id);
            apply_status({result});
        });
    });
    return true;
}

void AppBatchService::on_status_tick() {
    if (status_in_flight_ || backend_->batches() == nullptr) {
        return;
    }
    std::vector<BatchId> wanted;
    for (const auto& [id, snapshot] : snapshots_) {
        const bool active = (snapshot.state == BatchState::Running && snapshot.remaining() > 0) ||
                            (snapshot.state == BatchState::Paused && snapshot.running > 0) ||
                            (snapshot.state == BatchState::Cancelled && snapshot.running > 0);
        if (active || final_refresh_.contains(id)) {
            wanted.push_back(id);
        }
    }
    final_refresh_.clear();
    if (wanted.empty()) {
        update_timers();
        return;
    }
    status_in_flight_ = true;
    std::vector<quint64> sequences;
    for (const BatchId id : wanted) {
        sequences.push_back(sequence_[id]);
    }
    commands_->start([backend = backend_, dispatcher = dispatcher_, this, wanted, sequences] {
        auto results = std::make_shared<std::vector<StatusResult>>();
        for (std::size_t i = 0; i < wanted.size(); ++i) {
            StatusResult result;
            result.id = wanted[i];
            result.sequence = sequences[i];
            if (auto status = backend->batches()->status(to_catalog(wanted[i]))) {
                result.snapshot = to_snapshot(*status);
            }
            results->push_back(std::move(result));
        }
        dispatcher->post([this, results] {
            status_in_flight_ = false;
            apply_status(*results);
        });
    });
}

void AppBatchService::apply_status(const std::vector<StatusResult>& results) {
    for (const StatusResult& result : results) {
        if (!result.snapshot) {
            continue;
        }
        BatchSnapshot fresh = *result.snapshot;
        const auto found = snapshots_.find(result.id);
        if (found == snapshots_.end()) {
            // A new batch: the status is its first snapshot; its running jobs come from the events
            // that arrived meanwhile when there are any.
            if (const auto jobs = running_.find(result.id); jobs != running_.end() && !jobs->second.empty()) {
                rebuild_running(fresh);
            } else {
                for (const BatchRunningJob& job : fresh.running_jobs) {
                    running_[result.id][job.job] = job;
                }
            }
            store(fresh);
            emit batch_changed(snapshots_[result.id]);
            continue;
        }
        BatchSnapshot& snapshot = found->second;
        const bool current = sequence_[result.id] == result.sequence;
        if (current) {
            // Nothing newer arrived while the status was read: it is authoritative.
            snapshot.state = fresh.state;
            snapshot.total = fresh.total;
            snapshot.completed = fresh.completed;
            snapshot.queued = fresh.queued;
            snapshot.running = fresh.running;
            snapshot.failed = fresh.failed;
            snapshot.skipped = fresh.skipped;
            snapshot.cancelled = fresh.cancelled;
            snapshot.attention = fresh.attention;
            snapshot.attention_message = fresh.attention_message;
            snapshot.retryable = fresh.retryable;
            auto& jobs = running_[result.id];
            jobs.clear();
            for (const BatchRunningJob& job : fresh.running_jobs) {
                jobs[job.job] = job;
            }
            rebuild_running(snapshot);
        }
        snapshot.name = fresh.name;
        snapshot.selection = fresh.selection;
        snapshot.folder = fresh.folder;
        snapshot.created = fresh.created;
        snapshot.finished = fresh.finished;
        snapshot.single_export = fresh.single_export;
        snapshot.review = fresh.review;
        snapshot.excluded = fresh.excluded;
        snapshot.outcome_reason = fresh.outcome_reason;
        if (!fresh.last_output.isEmpty()) {
            snapshot.last_output = fresh.last_output;
        }
        snapshot.eta_seconds = snapshot.state == BatchState::Running && snapshot.attention == BatchAttention::None
                                   ? fresh.eta_seconds
                                   : -1.0;
        emit batch_changed(snapshot);
    }
    update_timers();
    update_busy();
}

void AppBatchService::store(const BatchSnapshot& snapshot) {
    BatchSnapshot& stored = snapshots_[snapshot.id];
    const QString last_output = stored.last_output;
    const QString report = stored.report_path;
    const qint64 review = stored.review;
    const qint64 excluded = stored.excluded;
    stored = snapshot;
    if (stored.last_output.isEmpty()) {
        stored.last_output = last_output;
    }
    if (stored.report_path.isEmpty()) {
        stored.report_path = report;
    }
    if (stored.review < 0) {
        stored.review = review;
    }
    if (stored.excluded < 0) {
        stored.excluded = excluded;
    }
}

void AppBatchService::apply_listed(const std::vector<BatchSnapshot>& listed, BatchId oldest, bool more) {
    listing_ = false;
    QList<BatchSnapshot> list;
    for (const BatchSnapshot& snapshot : listed) {
        store(snapshot);
        list.push_back(snapshots_[snapshot.id]);
    }
    if (oldest != 0 && (oldest_listed_ == 0 || oldest < oldest_listed_)) {
        oldest_listed_ = oldest;
    }
    const bool older = more && oldest_listed_ != 0;
    emit batches_listed(list);
    if (older != older_available_) {
        older_available_ = older;
        emit older_batches(older);
    }
    update_timers();
    update_busy();
}

void AppBatchService::update_timers() {
    const bool active =
        !final_refresh_.empty() || std::any_of(snapshots_.begin(), snapshots_.end(), [](const auto& e) {
            const BatchSnapshot& s = e.second;
            return (s.state == BatchState::Running && s.remaining() > 0) || s.running > 0;
        });
    if (active && !status_timer_.isActive()) {
        status_timer_.start();
    } else if (!active && status_timer_.isActive()) {
        status_timer_.stop();
    }
}

void AppBatchService::update_busy() {
    const bool busy = is_busy();
    if (busy != busy_) {
        busy_ = busy;
        emit busy_changed(busy);
    }
}

}  // namespace lucent::ui::adapters
