#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include "fakes/fake_services.hpp"

namespace lucent::ui::fakes {
namespace {

constexpr double kProgressStep = 0.1;  // ten timer steps per photo

QString tr_fake(const char* text) {
    return QCoreApplication::translate("lucent::ui::fakes", text);
}

QString output_name(const QString& pattern, const QString& source, qsizetype ordinal, const ExportOptions& options) {
    QString name = pattern;
    name.replace(QStringLiteral("{original-stem}"), QFileInfo(source).completeBaseName());
    name.replace(QStringLiteral("{revision}"), QStringLiteral("0"));
    name.replace(QStringLiteral("{image}"), QStringLiteral("1"));
    name.replace(QStringLiteral("{sequence}"), QStringLiteral("%1").arg(ordinal + 1, 4, 10, QLatin1Char('0')));
    return name + (options.format == ExportFileFormat::Jpeg ? QStringLiteral(".jpg") : QStringLiteral(".tif"));
}

}  // namespace

FakeBatchService::FakeBatchService(std::shared_ptr<const FakeCatalog> catalog, FakeTimings timings, bool available,
                                   QObject* parent)
    : BatchService(parent), catalog_(std::move(catalog)), timings_(timings), available_(available) {
    timer_.setInterval(std::max(0, timings_.export_step_ms));
    connect(&timer_, &QTimer::timeout, this, &FakeBatchService::step);
}

FakeBatchService::~FakeBatchService() {
    timer_.stop();
}

QString FakeBatchService::unavailable_reason() const {
    return available_ ? QString() : tr_fake("Simulated: this session keeps no batches.");
}

void FakeBatchService::later(std::function<void()> work) {
    QMetaObject::invokeMethod(this, std::move(work), Qt::QueuedConnection);
}

quint64 FakeBatchService::request_plan(const BatchSetup& setup) {
    const quint64 token = ++next_token_;
    BatchPlanInfo plan;
    if (!setup.input_folder.isEmpty()) {
        plan.folder_not_listed = true;
    } else {
        plan.ok = true;
        plan.folder = QDir::toNativeSeparators(setup.output_folder);
        plan.folder_exists = QFileInfo(setup.output_folder).isDir();
        for (qsizetype i = 0; i < setup.items.size(); ++i) {
            const std::optional<FakeCatalog::Entry> entry = catalog_->find(setup.items[i]);
            if (!entry || entry->support == SupportStatus::Unsupported) {
                ++plan.excluded;
                plan.exclusions.push_back(
                    BatchPlanExclusion{entry ? entry->file_name : tr_fake("Photo %1").arg(setup.items[i]),
                                       entry ? entry->support_detail : tr_fake("Simulated: unknown photo.")});
                continue;
            }
            ++plan.outputs;
            plan.estimated_bytes += qint64{entry->width} * entry->height * 3;
            if (plan.examples.size() < 3) {
                plan.examples.push_back(BatchPlanExample{
                    entry->file_name,
                    QDir::toNativeSeparators(
                        QDir(setup.output_folder)
                            .filePath(output_name(setup.name_pattern, entry->file_name, i, setup.output))),
                    false, false});
            }
        }
        plan.largest_bytes = plan.outputs > 0 ? plan.estimated_bytes / plan.outputs : 0;
        if (setup.include_gps) {
            plan.warnings << tr_fake("Simulated: GPS positions would be written.");
        }
        if (setup.automatic != BatchAutomatic::Off) {
            plan.automatic_notes << tr_fake("Simulated: each photo is analysed before anything is written.");
        }
        if (setup.automatic == BatchAutomatic::AnalyzeOnly) {
            plan.automatic_notes << tr_fake("Simulated: nothing is exported.");
            plan.outputs = 0;
            plan.estimated_bytes = 0;
            plan.largest_bytes = 0;
            plan.examples.clear();
        }
        if (setup.automatic != BatchAutomatic::Off && setup.keep_consistent) {
            plan.automatic_notes << tr_fake("Simulated: related photos are analysed together first.");
        }
    }
    later([this, token, plan] { emit plan_ready(token, plan); });
    return token;
}

quint64 FakeBatchService::submit(const BatchSetup& setup) {
    const quint64 token = ++next_token_;
    if (!available_ || !setup.input_folder.isEmpty() || setup.items.isEmpty()) {
        later([this, token] { emit submitted(token, 0, tr_fake("Simulated: folder batches are not simulated.")); });
        return token;
    }
    Batch& batch = batches_[next_id_];
    batch.snapshot.id = next_id_++;
    batch.snapshot.name = setup.name.isEmpty() ? tr_fake("Export of %1 photos").arg(setup.items.size()) : setup.name;
    batch.snapshot.selection = tr_fake("%1 selected photos").arg(setup.items.size());
    batch.snapshot.folder = QDir::toNativeSeparators(setup.output_folder);
    batch.snapshot.state = BatchState::Running;
    batch.snapshot.created = QDateTime::currentDateTime();
    batch.snapshot.review = 0;
    batch.snapshot.excluded = 0;
    batch.snapshot.automatic = setup.automatic;
    batch.snapshot.keep_consistent = setup.automatic != BatchAutomatic::Off && setup.keep_consistent;
    for (qsizetype i = 0; i < setup.items.size(); ++i) {
        const std::optional<FakeCatalog::Entry> entry = catalog_->find(setup.items[i]);
        Job job;
        job.item = setup.items[i];
        job.source = entry ? entry->file_name : tr_fake("Photo %1").arg(setup.items[i]);
        job.output = QDir::toNativeSeparators(
            QDir(setup.output_folder).filePath(output_name(setup.name_pattern, job.source, i, setup.output)));
        batch.jobs.push_back(job);
    }
    const BatchId id = batch.snapshot.id;
    // As the real service: the submission is answered before the batch makes progress.
    later([this, token, id] {
        emit submitted(token, id, {});
        publish(batches_[id]);
        if (!timer_.isActive()) {
            timer_.start();
        }
        update_busy();
    });
    return token;
}

void FakeBatchService::step() {
    bool any = false;
    for (auto& [id, batch] : batches_) {
        if (batch.snapshot.state != BatchState::Running) {
            continue;
        }
        const auto next = std::find_if(batch.jobs.begin(), batch.jobs.end(),
                                       [](const Job& job) { return job.outcome == BatchOutcome::Pending; });
        if (next == batch.jobs.end()) {
            continue;
        }
        any = true;
        const std::optional<FakeCatalog::Entry> entry = catalog_->find(next->item);
        if (!entry || entry->support == SupportStatus::Unsupported) {
            next->outcome = BatchOutcome::Failed;
            next->reason = entry ? entry->support_detail : tr_fake("Simulated: this photo is not in the library.");
        } else {
            next->fraction = std::min(1.0, next->fraction + kProgressStep);
            if (next->fraction >= 1.0 - 1e-9) {
                if (batch.snapshot.automatic == BatchAutomatic::AnalyzeOnly) {
                    next->outcome = BatchOutcome::Analysed;
                } else {
                    next->outcome = BatchOutcome::Exported;
                    batch.snapshot.last_output = next->output;
                }
            }
        }
        publish(batch);
        break;  // one photo of one batch at a time
    }
    if (!any) {
        timer_.stop();
    }
    update_busy();
}

void FakeBatchService::publish(Batch& batch) {
    BatchSnapshot& s = batch.snapshot;
    s.total = static_cast<qint64>(batch.jobs.size());
    s.completed = s.failed = s.skipped = s.cancelled = s.queued = s.running = s.retryable = 0;
    s.running_jobs.clear();
    bool started = false;
    const bool automatic = s.automatic != BatchAutomatic::Off;
    qint64 review = 0;
    qint64 pending_analysis = 0;
    for (const Job& job : batch.jobs) {
        switch (job.outcome) {
        case BatchOutcome::Exported:
        case BatchOutcome::Analysed:
            ++s.completed;
            // Simulated: every third photo of an automatic batch recommends a review.
            review += automatic && (&job - batch.jobs.data()) % 3 == 2 ? 1 : 0;
            break;
        case BatchOutcome::Skipped:
            ++s.skipped;
            break;
        case BatchOutcome::Failed:
            ++s.failed;
            ++s.retryable;
            break;
        case BatchOutcome::Cancelled:
            ++s.cancelled;
            break;
        case BatchOutcome::Pending:
            if (!started && job.fraction > 0.0 && s.state == BatchState::Running) {
                started = true;
                ++s.running;
                s.running_jobs.push_back(BatchRunningJob{0, job.item, job.source,
                                                         automatic && job.fraction < 0.5 ? tr_fake("Analysing")
                                                                                         : tr_fake("Rendering"),
                                                         job.fraction});
            } else {
                ++s.queued;
            }
            ++pending_analysis;
            break;
        }
    }
    if (automatic) {
        s.review = review;
        s.analysis_pending = s.remaining() > 0 ? pending_analysis : -1;
        s.analysis_recorded = s.remaining() > 0 ? s.total - pending_analysis : -1;
    }
    if (s.state == BatchState::Running && s.remaining() == 0) {
        s.state = BatchState::Completed;
        s.finished = QDateTime::currentDateTime();
    }
    emit batch_changed(s);
}

void FakeBatchService::pause(BatchId id) {
    if (const auto found = batches_.find(id);
        found != batches_.end() && found->second.snapshot.state == BatchState::Running) {
        found->second.snapshot.state = BatchState::Paused;
        publish(found->second);
        update_busy();
    }
}

void FakeBatchService::resume(BatchId id) {
    if (const auto found = batches_.find(id);
        found != batches_.end() && found->second.snapshot.state == BatchState::Paused) {
        found->second.snapshot.state = BatchState::Running;
        found->second.snapshot.attention = BatchAttention::None;
        found->second.snapshot.attention_message.clear();
        publish(found->second);
        if (!timer_.isActive()) {
            timer_.start();
        }
        update_busy();
    }
}

void FakeBatchService::cancel(BatchId id) {
    const auto found = batches_.find(id);
    if (found == batches_.end()) {
        return;
    }
    for (Job& job : found->second.jobs) {
        if (job.outcome == BatchOutcome::Pending) {
            job.outcome = BatchOutcome::Cancelled;
            job.reason = tr_fake("Cancelled before it was exported.");
        }
    }
    found->second.snapshot.state = BatchState::Cancelled;
    found->second.snapshot.finished = QDateTime::currentDateTime();
    publish(found->second);
    update_busy();
}

void FakeBatchService::retry_failed(BatchId id) {
    const auto found = batches_.find(id);
    if (found == batches_.end()) {
        return;
    }
    for (Job& job : found->second.jobs) {
        if (job.outcome == BatchOutcome::Failed) {
            job.outcome = BatchOutcome::Pending;
            job.fraction = 0.0;
            job.reason.clear();
        }
    }
    found->second.snapshot.state = BatchState::Running;
    found->second.snapshot.finished = {};
    publish(found->second);
    if (!timer_.isActive()) {
        timer_.start();
    }
    update_busy();
}

void FakeBatchService::refresh() {
    QList<BatchSnapshot> list;
    for (auto it = batches_.rbegin(); it != batches_.rend(); ++it) {
        list.push_back(it->second.snapshot);
    }
    later([this, list] { emit batches_listed(list); });
}

void FakeBatchService::request_report(BatchId id) {
    BatchReportInfo report;
    report.id = id;
    if (const auto found = batches_.find(id); found != batches_.end()) {
        const BatchSnapshot& s = found->second.snapshot;
        report.ok = true;
        report.name = s.name;
        report.folder = s.folder;
        report.state = s.state;
        const bool analyse_only = s.automatic == BatchAutomatic::AnalyzeOnly;
        report.exported = analyse_only ? 0 : s.completed;
        report.analysed = analyse_only ? s.completed : 0;
        report.automatic = s.automatic;
        report.review = s.automatic != BatchAutomatic::Off ? s.review : 0;
        report.failed = s.failed;
        report.skipped = s.skipped;
        report.cancelled = s.cancelled;
        report.pending = s.remaining();
        report.format = analyse_only ? QString() : tr_fake("JPEG · sRGB · 8-bit");
        report.created = s.created;
        report.finished = s.finished;
        report.notes << tr_fake("Simulated batch: no file was written.");
        const auto& jobs = found->second.jobs;
        for (std::size_t i = 0; i < jobs.size(); ++i) {
            const Job& job = jobs[i];
            BatchReportEntry entry;
            entry.item = job.item;
            entry.source_name = job.source;
            entry.outcome = job.outcome;
            entry.output = job.outcome == BatchOutcome::Exported ? job.output : QString();
            entry.reason = job.reason;
            entry.attempts = job.outcome == BatchOutcome::Pending ? 0 : 1;
            const bool done = job.outcome == BatchOutcome::Exported || job.outcome == BatchOutcome::Analysed;
            if (s.automatic != BatchAutomatic::Off) {
                entry.automatic = true;
                if (done) {
                    entry.revision_number = 2;
                    entry.confidence = i % 3 == 2 ? 0.45 : 0.85;
                    entry.flags << tr_fake("Simulated: the colour of the light was uncertain.");
                    if (i % 3 == 2) {
                        entry.review << tr_fake("Simulated: Lucent was unsure, so it kept the change small.");
                        entry.flags << entry.review.front();
                    }
                }
            }
            report.entries.push_back(entry);
        }
    } else {
        report.error = tr_fake("Simulated: unknown batch.");
    }
    later([this, id, report] { emit report_ready(id, report); });
}

void FakeBatchService::request_report_file(BatchId id) {
    later([this, id] { emit report_file_ready(id, {}, tr_fake("Simulated batches have no report file.")); });
}

bool FakeBatchService::is_busy() const {
    return std::any_of(batches_.begin(), batches_.end(), [](const auto& entry) {
        return entry.second.snapshot.state == BatchState::Running && entry.second.snapshot.remaining() > 0;
    });
}

void FakeBatchService::update_busy() {
    const bool busy = is_busy();
    if (busy != busy_) {
        busy_ = busy;
        emit busy_changed(busy);
    }
}

}  // namespace lucent::ui::fakes
