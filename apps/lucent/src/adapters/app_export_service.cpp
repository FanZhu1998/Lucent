#include "adapters/app_export_service.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

#include <lucent/application/export_service.hpp>

#include "adapters/conversions.hpp"

namespace lucent::ui::adapters {
namespace {

constexpr auto kProgressInterval = std::chrono::milliseconds(100);
// W3 exports the default settings, revision 1 (W6 introduces recipe revisions).
constexpr std::uint64_t kDefaultSettingsRevision = 1;

QString tr_text(const char* text) {
    return QCoreApplication::translate("lucent::ui::adapters", text);
}

// Passes stage changes and at most one update per kProgressInterval. Called on one worker thread
// at a time (the export's), so it needs no locking.
class ProgressThrottle {
public:
    bool pass(const app::ExportProgress& progress) {
        const auto now = std::chrono::steady_clock::now();
        if (stage_ == progress.stage && now - last_ < kProgressInterval) {
            return false;
        }
        stage_ = progress.stage;
        last_ = now;
        return true;
    }

private:
    std::optional<app::ExportStage> stage_;
    std::chrono::steady_clock::time_point last_{};
};

QString source_name(const app::SourceItem& source) {
    const QString name = to_qstring(source.path.filename());
    if (source.subimage_count > 1) {
        return tr_text("%1 (image %2 of %3)")
            .arg(name, QString::number(source.subimage + 1), QString::number(source.subimage_count));
    }
    return name;
}

}  // namespace

AppExportService::AppExportService(std::shared_ptr<Backend> backend, QObject* parent)
    : ExportService(parent),
      backend_(std::move(backend)),
      dispatcher_(std::make_shared<UiDispatcher>(this)) {}

AppExportService::~AppExportService() {
    dispatcher_->close();
    for (auto& [id, job] : jobs_) {
        if (is_active(job.snapshot.state)) {
            job.handle.cancel();  // no-op for jobs that were never submitted
        }
    }
}

void AppExportService::start_export(const ExportRequest& request) {
    if (request.items.isEmpty()) {
        return;
    }
    const std::filesystem::path folder = to_path(request.output_folder);
    backend_->output_folders().add(folder);
    const exporter::ExportSettings settings = to_export_settings(request.options);

    for (const ItemId item : request.items) {
        const JobId id = next_id_++;
        Job& job = jobs_[id];
        job.snapshot.id = id;
        job.snapshot.item = item;
        job.snapshot.output_folder = QDir::toNativeSeparators(request.output_folder);
        job.snapshot.options = request.options;
        job.snapshot.state = ExportJobState::Queued;
        const std::optional<app::SourceItem> source = backend_->library().find(app::ItemId{item});
        job.source_known = source.has_value();
        job.snapshot.source_name = source ? source_name(*source) : tr_text("Photo %1").arg(item);
        job.request.item = app::ItemId{item};
        job.request.settings_revision = kDefaultSettingsRevision;
        job.request.output_directory = folder;
        job.request.output = settings;
        job.request.allow_unsupported = false;  // blueprint section 3: never exported silently
        waiting_.push_back(id);
        emit job_added(job.snapshot);
    }
    submit_next();
    update_busy();
}

void AppExportService::submit_next() {
    while (!submitted_ && !waiting_.empty()) {
        const JobId id = waiting_.front();
        waiting_.pop_front();
        const auto found = jobs_.find(id);
        if (found == jobs_.end() || found->second.snapshot.state != ExportJobState::Queued) {
            continue;
        }
        Job& job = found->second;
        auto throttle = std::make_shared<ProgressThrottle>();
        // Both callbacks run on an executor worker; `this` is only used by the posted functions.
        auto submitted = backend_->exports().submit(
            job.request,
            [this, dispatcher = dispatcher_, id, throttle](const app::ExportProgress& progress) {
                if (throttle->pass(progress)) {
                    dispatcher->post([this, id, progress] { on_progress(id, progress); });
                }
            },
            [this, dispatcher = dispatcher_, id](app::ItemId, const Expected<app::ExportRecord>& record) {
                auto shared = std::make_shared<const Expected<app::ExportRecord>>(record);
                dispatcher->post([this, id, shared] { on_completed(id, *shared); });
            });
        if (!submitted) {
            job.snapshot.state = ExportJobState::Failed;
            job.snapshot.message = job.source_known ? describe_error(submitted.error())
                                                    : tr_text("This photo is no longer in the library.");
            publish(job);
            continue;
        }
        job.handle = *submitted;
        submitted_ = id;
    }
}

void AppExportService::cancel_job(JobId id) {
    const auto found = jobs_.find(id);
    if (found == jobs_.end() || !is_active(found->second.snapshot.state) ||
        found->second.snapshot.cancel_requested) {
        return;
    }
    Job& job = found->second;
    if (const auto waiting = std::find(waiting_.begin(), waiting_.end(), id); waiting != waiting_.end()) {
        // Not started: it ends now, without reaching the application services.
        waiting_.erase(waiting);
        job.snapshot.state = ExportJobState::Cancelled;
        job.snapshot.message = tr_text("Cancelled.");
        publish(job);
        update_busy();
        return;
    }
    job.handle.cancel();
    job.snapshot.cancel_requested = true;
    job.snapshot.message = tr_text("Cancelling…");
    publish(job);
}

bool AppExportService::is_busy() const {
    for (const auto& [id, job] : jobs_) {
        if (is_active(job.snapshot.state)) {
            return true;
        }
    }
    return false;
}

QString AppExportService::suggested_file_name(ItemId item, const ExportOptions& options) const {
    const std::optional<app::SourceItem> source = backend_->library().find(app::ItemId{item});
    if (!source) {
        return {};
    }
    auto name = app::export_file_name(source->path, source->subimage, source->subimage_count,
                                      to_export_settings(options).format, 1);
    return name ? to_qstring(*name) : QString();
}

void AppExportService::on_progress(JobId id, const app::ExportProgress& progress) {
    const auto found = jobs_.find(id);
    if (found == jobs_.end() || !is_active(found->second.snapshot.state)) {
        return;
    }
    Job& job = found->second;
    job.snapshot.state = ExportJobState::Running;
    job.snapshot.progress = std::clamp(progress.fraction, 0.0, 1.0);
    if (!job.snapshot.cancel_requested) {
        job.snapshot.message = describe_export_stage(progress.stage);
    }
    publish(job);
}

void AppExportService::on_completed(JobId id, const Expected<app::ExportRecord>& record) {
    const auto found = jobs_.find(id);
    if (found == jobs_.end()) {
        return;
    }
    Job& job = found->second;
    job.snapshot.cancel_requested = false;
    job.handle = {};
    if (submitted_ == id) {
        submitted_.reset();
    }
    if (record) {
        const exporter::ExportReceipt& receipt = record->receipt;
        job.snapshot.state = ExportJobState::Completed;
        job.snapshot.progress = 1.0;
        job.snapshot.destination_path = QDir::toNativeSeparators(to_qstring(receipt.path));
        const QLocale locale;
        // Processing time: render, encode and write (waiting in the queue excluded).
        job.snapshot.message =
            tr_text("%1 × %2 · %3 · %4 s")
                .arg(QString::number(receipt.width), QString::number(receipt.height),
                     locale.formattedDataSize(static_cast<qint64>(receipt.byte_size), 1,
                                              QLocale::DataSizeTraditionalFormat),
                     locale.toString(record->render_seconds + record->encode_seconds + record->write_seconds,
                                     'f', 1));
    } else if (record.error().code == ErrorCode::Cancelled) {
        job.snapshot.state = ExportJobState::Cancelled;
        job.snapshot.message = tr_text("Cancelled.");
    } else {
        job.snapshot.state = ExportJobState::Failed;
        QString message = describe_error(record.error());
        if (record.error().code == ErrorCode::Unsupported && record.error().where == "app.export.support") {
            if (const auto source = backend_->library().find(app::ItemId{job.snapshot.item})) {
                message += u' ' + support_detail(source->support);
            }
        }
        job.snapshot.message = message;
    }
    publish(job);
    submit_next();
    update_busy();
}

void AppExportService::publish(Job& job) {
    emit job_changed(job.snapshot);
}

void AppExportService::update_busy() {
    const bool busy = is_busy();
    if (busy != busy_) {
        busy_ = busy;
        emit busy_changed(busy);
    }
}

}  // namespace lucent::ui::adapters
