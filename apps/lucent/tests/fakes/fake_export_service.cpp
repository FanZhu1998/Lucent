#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include "fakes/fake_services.hpp"

namespace lucent::ui::fakes {
namespace {

// Ten timer steps per job.
constexpr double kProgressStep = 0.1;

QString tr_fake(const char* text) {
    return QCoreApplication::translate("lucent::ui::fakes", text);
}

QString output_suffix(const ExportOptions& options) {
    return options.format == ExportFileFormat::Jpeg ? QStringLiteral(".jpg") : QStringLiteral(".tif");
}

}  // namespace

FakeExportService::FakeExportService(std::shared_ptr<const FakeCatalog> catalog, FakeTimings timings,
                                     QObject* parent)
    : ExportService(parent), catalog_(std::move(catalog)), timings_(timings) {
    timer_.setInterval(std::max(0, timings_.export_step_ms));
    connect(&timer_, &QTimer::timeout, this, &FakeExportService::step);
}

FakeExportService::~FakeExportService() {
    timer_.stop();
}

void FakeExportService::start_export(const ExportRequest& request) {
    for (const ItemId item : request.items) {
        ExportJob job;
        job.id = next_id_++;
        job.item = item;
        const std::optional<FakeCatalog::Entry> entry = catalog_->find(item);
        job.source_name = entry ? entry->file_name : tr_fake("Photo %1").arg(item);
        job.output_folder = request.output_folder;
        job.options = request.options;
        job.state = ExportJobState::Queued;
        jobs_[job.id] = job;
        queue_.push_back(job.id);
        emit job_added(job);
    }
    update_busy();
    start_next();
}

void FakeExportService::cancel_job(JobId id) {
    const auto found = jobs_.find(id);
    if (found == jobs_.end() || !is_active(found->second.state)) {
        return;
    }
    queue_.erase(std::remove(queue_.begin(), queue_.end(), id), queue_.end());
    if (running_ == id) {
        running_ = 0;
    }
    ExportJob job = found->second;
    job.state = ExportJobState::Cancelled;
    job.message = tr_fake("Cancelled.");
    publish(job);
    start_next();
}

void FakeExportService::start_next() {
    while (running_ == 0 && !queue_.empty()) {
        const JobId id = queue_.front();
        queue_.pop_front();
        ExportJob job = jobs_[id];
        const std::optional<FakeCatalog::Entry> entry = catalog_->find(job.item);
        if (!entry || entry->support == SupportStatus::Unsupported) {
            job.state = ExportJobState::Failed;
            job.message = entry ? entry->support_detail : tr_fake("Simulated: this photo is not in the library.");
            publish(job);
            continue;
        }
        job.state = ExportJobState::Running;
        job.progress = 0.0;
        job.destination_path =
            QDir(job.output_folder).filePath(QFileInfo(entry->file_name).completeBaseName() + output_suffix(job.options));
        publish(job);
        running_ = id;
    }
    if (running_ != 0) {
        if (!timer_.isActive()) {
            timer_.start();
        }
    } else {
        timer_.stop();
    }
    update_busy();
}

void FakeExportService::step() {
    if (running_ == 0) {
        timer_.stop();
        return;
    }
    ExportJob job = jobs_[running_];
    job.progress = std::min(1.0, job.progress + kProgressStep);
    if (job.progress >= 1.0 - 1e-9) {
        job.progress = 1.0;
        job.state = ExportJobState::Completed;
        job.message = tr_fake("Simulated export: no file was written.");
        publish(job);
        running_ = 0;
        start_next();
        return;
    }
    publish(job);
}

QString FakeExportService::suggested_file_name(ItemId item, const ExportOptions& options) const {
    const std::optional<FakeCatalog::Entry> entry = catalog_->find(item);
    return entry ? QFileInfo(entry->file_name).completeBaseName() + QStringLiteral("_Lucent") + output_suffix(options)
                 : QString();
}

void FakeExportService::publish(const ExportJob& job) {
    jobs_[job.id] = job;
    emit job_changed(job);
}

void FakeExportService::update_busy() {
    const bool busy = is_busy();
    if (busy != busy_) {
        busy_ = busy;
        emit busy_changed(busy);
    }
}

}  // namespace lucent::ui::fakes
