#include "app_controller.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QQmlEngine>
#include <algorithm>
#include <stdexcept>
#include <utility>

#include "imaging/image_provider.hpp"
#include "models/presentation.hpp"

namespace lucent::ui {
namespace {

QString tr_controller(const char* text) {
    return QCoreApplication::translate("lucent::ui::AppController", text);
}

// English singular/plural until translations exist.
QString count_phrase(int count, const char* singular, const char* plural) {
    return count == 1 ? tr_controller(singular) : tr_controller(plural).arg(count);
}

QString native(const QString& path) {
    return QDir::toNativeSeparators(path);
}

// One path as it is; several by the folder they share (files selected or dropped together), or
// by the number of distinct folders they come from. Text only: no file-system access.
QString describe_sources(const QStringList& paths) {
    if (paths.size() == 1) {
        return native(paths.front());
    }
    QStringList parents;
    QStringList keys;
    for (const QString& path : paths) {
        const QString parent = QFileInfo(QDir::cleanPath(path)).path();
        const QString key = parent.toCaseFolded();  // Windows paths compare case-insensitively
        if (!keys.contains(key)) {
            keys << key;
            parents << parent;
        }
    }
    if (parents.size() == 1) {
        return native(parents.front());
    }
    return tr_controller("%1 locations").arg(parents.size());
}

QString summary_message(const ImportSummary& summary) {
    const QString sources = describe_sources(summary.sources);
    QString message;
    if (summary.copied > 0 && !summary.copy_to.isEmpty()) {
        message =
            count_phrase(summary.copied, "Copied 1 photo from %2 into %3", "Copied %1 photos from %2 into %3")
                .arg(sources, summary.copy_to);
        if (summary.updated > 0) {
            message += QStringLiteral(" · ") +
                       count_phrase(summary.updated, "1 changed photo updated", "%1 changed photos updated");
        }
    } else if (summary.added > 0) {
        message = count_phrase(summary.added, "Added 1 photo from %2", "Added %1 photos from %2").arg(sources);
        if (summary.updated > 0) {
            message += QStringLiteral(" · ") + count_phrase(summary.updated, "1 changed photo updated",
                                                            "%1 changed photos updated");
        }
    } else if (summary.updated > 0) {
        message = count_phrase(summary.updated, "Updated 1 changed photo in %2", "Updated %1 changed photos in %2")
                      .arg(sources);
    } else if (summary.restored > 0) {
        message = count_phrase(summary.restored, "Found 1 missing photo again in %2",
                               "Found %1 missing photos again in %2")
                      .arg(sources);
    } else {
        message = tr_controller("No new photos in %1").arg(sources);
    }
    if (summary.restored > 0 && (summary.copied > 0 || summary.added > 0 || summary.updated > 0)) {
        message += QStringLiteral(" · ") + count_phrase(summary.restored, "1 missing photo found again",
                                                        "%1 missing photos found again");
    }
    if (summary.cancelled) {
        message += QStringLiteral(" · ") + tr_controller("import stopped early");
    }
    if (summary.unsupported > 0) {
        message += QStringLiteral(" · ") + count_phrase(summary.unsupported, "1 unsupported", "%1 unsupported");
    }
    if (summary.limited > 0) {
        message += QStringLiteral(" · ") + count_phrase(summary.limited, "1 limited", "%1 limited");
    }
    if (summary.removable > 0) {
        message += QStringLiteral(" · ") +
                   count_phrase(summary.removable, "1 on removable media", "%1 on removable media");
    }
    if (summary.already_in_library > 0) {
        message += QStringLiteral(" · ") + count_phrase(summary.already_in_library, "1 already in the library",
                                                        "%1 already in the library");
    }
    if (summary.ignored > 0) {
        message += QStringLiteral(" · ") +
                   count_phrase(summary.ignored, "1 other file ignored", "%1 other files ignored");
    }
    if (summary.failed > 0) {
        message += QStringLiteral(" · ") +
                   count_phrase(summary.failed, "1 could not be read", "%1 could not be read");
        if (!summary.errors.isEmpty()) {
            message += QStringLiteral(": ") + summary.errors.front();
        }
    }
    return message;
}

}  // namespace

AppController::AppController(Services services, QObject* parent)
    : QObject(parent), services_(std::move(services)), registry_(std::make_shared<GenerationRegistry>()) {
    if (!services_.complete()) {
        throw std::invalid_argument(
            "lucent::ui::AppController needs a library, image, export, edit and batch service");
    }
    library_model_ = std::make_unique<LibraryModel>(*services_.library, registry_);
    export_model_ = std::make_unique<ExportJobModel>(*services_.exports);
    current_ = std::make_unique<CurrentItem>(*library_model_);
    frame_store_ = std::make_shared<FrameStore>();
    edit_ = std::make_unique<EditSession>(*services_.edits, frame_store_);
    batch_list_ = std::make_unique<BatchListModel>(*services_.batches);
    batch_setup_ = std::make_unique<BatchSetupModel>(*services_.batches);
    batch_report_ = std::make_unique<BatchReportModel>(*services_.batches);
    // Owned here, never by the QML engine's garbage collector.
    QQmlEngine::setObjectOwnership(library_model_.get(), QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(export_model_.get(), QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(current_.get(), QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(edit_.get(), QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(edit_->history(), QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(batch_list_.get(), QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(batch_setup_.get(), QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(batch_report_.get(), QQmlEngine::CppOwnership);

    url_opener_ = [](const QUrl& url) { return QDesktopServices::openUrl(url); };

    LibraryService& library = *services_.library;
    connect(&library, &LibraryService::import_started, this, &AppController::on_import_started);
    connect(&library, &LibraryService::import_progress, this, &AppController::on_import_progress);
    connect(&library, &LibraryService::import_finished, this, &AppController::on_import_finished);
    connect(services_.exports.get(), &ExportService::job_changed, this, &AppController::on_job_changed);
    connect(&library, &LibraryService::busy_changed, this, &AppController::busyChanged);
    connect(&library, &LibraryService::relink_finished, this, &AppController::on_relink_finished);
    connect(&library, &LibraryService::backup_finished, this, &AppController::on_backup_finished);
    connect(&library, &LibraryService::verification_progress, this, &AppController::on_verification_progress);
    connect(services_.exports.get(), &ExportService::busy_changed, this, &AppController::busyChanged);
    connect(services_.edits.get(), &EditService::busy_changed, this, &AppController::busyChanged);
    // Auto runs in the background: the window is busy while one runs. The photo being edited reports
    // its outcome itself (EditSession::notice); a run of a photo the editor has left is reported here.
    connect(services_.edits.get(), &EditService::auto_progress, this, &AppController::busyChanged);
    connect(services_.edits.get(), &EditService::auto_finished, this, [this](const AutoOutcome& outcome) {
        if (outcome.item != edit_->item()) {
            QString name;
            if (const auto row = library_model_->row_of(outcome.item)) {
                name = QFileInfo(library_model_->item_at(*row)->file_path).fileName();
            }
            set_status_message(name.isEmpty() ? outcome.message
                                              : tr_controller("%1: %2").arg(name, outcome.message));
        }
        emit busyChanged();
    });
    connect(edit_.get(), &EditSession::notice, this, &AppController::set_status_message);
    // After the model has inserted the rows (it connected first).
    connect(&library, &LibraryService::items_added, this, &AppController::on_items_added);
    connect(current_.get(), &CurrentItem::changed, this, &AppController::on_current_changed);
    connect(this, &AppController::exportOptionsChanged, this, &AppController::update_suggested_export_name);

    BatchService& batches = *services_.batches;
    connect(&batches, &BatchService::busy_changed, this, &AppController::busyChanged);
    connect(&batches, &BatchService::submitted, this, &AppController::on_batch_submitted);
    connect(&batches, &BatchService::batch_changed, this, &AppController::on_batch_changed);
    connect(&batches, &BatchService::command_failed, this,
            [this](BatchId, const QString& message) { set_status_message(message); });
    connect(&batches, &BatchService::report_file_ready, this,
            [this](BatchId batch, const QString& path, const QString& error) {
                if (batch != report_requested_) {
                    return;
                }
                report_requested_ = 0;
                if (path.isEmpty()) {
                    set_status_message(tr_controller("The report could not be written: %1").arg(error));
                } else if (!url_opener_(QUrl::fromLocalFile(path))) {
                    set_status_message(tr_controller("Could not open %1").arg(native(path)));
                }
            });
    const auto update_active = [this] {
        const int active = active_export_count();
        const int attentions = export_attention_count();
        if (active != last_active_exports_ || attentions != last_export_attentions_) {
            last_active_exports_ = active;
            last_export_attentions_ = attentions;
            emit activeExportCountChanged();
        }
    };
    connect(batch_list_.get(), &BatchListModel::summaryChanged, this, update_active);
    connect(export_model_.get(), &ExportJobModel::countsChanged, this, update_active);
}

AppController::~AppController() {
    edit_->set_item(0);  // commits a pending gesture and closes the session
}

bool AppController::addFiles(const QList<QUrl>& urls) {
    return import_urls(urls, false);
}

bool AppController::addDropped(const QList<QUrl>& urls) {
    return import_urls(urls, include_subfolders_);
}

bool AppController::import_urls(const QList<QUrl>& urls, bool include_subfolders) {
    QStringList paths;
    for (const QUrl& url : urls) {
        if (url.isLocalFile()) {
            paths << url.toLocalFile();
        }
    }
    if (paths.isEmpty()) {
        if (!urls.isEmpty()) {
            set_status_message(tr_controller("Only files on this computer or the network can be added."));
        }
        return false;
    }
    return import_paths(paths, include_subfolders);
}

bool AppController::addFolder(const QUrl& folder) {
    if (!folder.isLocalFile()) {
        set_status_message(tr_controller("Only folders on this computer or the network can be added."));
        return false;
    }
    return import_paths({folder.toLocalFile()}, include_subfolders_);
}

bool AppController::import_paths(const QStringList& paths, bool include_subfolders) {
    return import_paths(paths, ImportOptions{include_subfolders, {}});
}

bool AppController::import_paths(const QStringList& paths, const ImportOptions& options) {
    QStringList absolute;
    for (const QString& path : paths) {
        if (!path.trimmed().isEmpty()) {
            absolute << QDir::cleanPath(QDir::current().absoluteFilePath(path));
        }
    }
    if (absolute.isEmpty()) {
        return false;
    }
    services_.library->start_import(absolute, options);
    return true;
}

void AppController::start() {
    services_.library->start();
}

void AppController::recheck_library() {
    services_.library->recheck_sources();
}

bool AppController::requestImport(const QList<QUrl>& urls) {
    QStringList paths;
    for (const QUrl& url : urls) {
        if (url.isLocalFile()) {
            paths << QDir::cleanPath(url.toLocalFile());
        }
    }
    if (paths.isEmpty()) {
        if (!urls.isEmpty()) {
            set_status_message(tr_controller("Only files on this computer or the network can be added."));
        }
        return false;
    }
    if (paths != pending_import_) {
        pending_import_ = paths;
        emit pendingImportChanged();
    }
    return true;
}

bool AppController::confirmImport() {
    if (pending_import_.isEmpty()) {
        return false;
    }
    ImportOptions options;
    options.include_subfolders = include_subfolders_;
    if (copy_into_library_) {
        if (!copy_destination_.isLocalFile()) {
            set_status_message(tr_controller("Choose a library folder for the copies."));
            return false;
        }
        options.copy_to = QDir::cleanPath(copy_destination_.toLocalFile());
    }
    const QStringList paths = std::exchange(pending_import_, {});
    emit pendingImportChanged();
    return import_paths(paths, options);
}

void AppController::cancelImport() {
    if (!pending_import_.isEmpty()) {
        pending_import_.clear();
        emit pendingImportChanged();
    }
}

bool AppController::relinkMissing(const QUrl& folder) {
    return relink(folder, {});
}

bool AppController::relinkCurrent(const QUrl& folder) {
    if (!current_->valid()) {
        return false;
    }
    return relink(folder, {current_->item_id()});
}

bool AppController::relink(const QUrl& folder, const QList<ItemId>& items) {
    if (!folder.isLocalFile()) {
        set_status_message(tr_controller("Choose a folder on this computer or the network."));
        return false;
    }
    const QString path = QDir::cleanPath(folder.toLocalFile());
    set_status_message(tr_controller("Looking for missing photos in %1…").arg(native(path)));
    services_.library->relink(path, items);
    return true;
}

void AppController::setCurrentPick(const QString& key) {
    if (!current_->valid()) {
        return;
    }
    PickState pick = PickState::None;
    if (key == u"picked") {
        pick = PickState::Picked;
    } else if (key == u"rejected") {
        pick = PickState::Rejected;
    }
    services_.library->set_pick(current_->item_id(), pick);
}

void AppController::backUpCatalog() {
    set_status_message(tr_controller("Backing up the catalog…"));
    services_.library->back_up_catalog();
}

bool AppController::openBackupFolder() {
    const QString folder = services_.library->backup_folder();
    if (folder.isEmpty()) {
        return false;
    }
    if (!url_opener_(QUrl::fromLocalFile(folder))) {
        set_status_message(tr_controller("Could not open %1").arg(native(folder)));
        return false;
    }
    return true;
}

void AppController::set_copy_into_library(bool copy) {
    if (copy != copy_into_library_) {
        copy_into_library_ = copy;
        emit importOptionsChanged();
    }
}

void AppController::set_copy_destination(const QUrl& folder) {
    if (folder != copy_destination_) {
        copy_destination_ = folder;
        emit importOptionsChanged();
    }
}

bool AppController::can_relink() const {
    return services_.library->can_relink();
}

bool AppController::can_back_up() const {
    return services_.library->can_back_up();
}

QString AppController::library_notice() const {
    return services_.library->library_notice();
}

QString AppController::app_version() const {
    const QString version = QCoreApplication::applicationVersion();
    return version.isEmpty() ? QStringLiteral("unknown") : version;
}

QVariantMap AppController::camera_support() const {
    return presentation::camera_support_map(services_.library->camera_support());
}

QVariantList AppController::support_states() const {
    return presentation::support_states();
}

void AppController::on_relink_finished(const RelinkSummary& summary) {
    if (!summary.error.isEmpty()) {
        set_status_message(tr_controller("Could not relink: %1").arg(summary.error));
        return;
    }
    if (summary.missing == 0) {
        set_status_message(tr_controller("No photos are missing."));
        return;
    }
    QString message =
        summary.relinked > 0
            ? count_phrase(summary.relinked, "Relinked 1 photo from %2", "Relinked %1 photos from %2")
                  .arg(summary.folder)
            : tr_controller("No missing photos found in %1").arg(summary.folder);
    if (summary.unmatched > 0) {
        message +=
            QStringLiteral(" · ") + count_phrase(summary.unmatched, "1 still missing", "%1 still missing");
    }
    if (summary.unverifiable > 0) {
        message += QStringLiteral(" (") +
                   count_phrase(summary.unverifiable, "1 was never verified, so it cannot be matched safely",
                                "%1 were never verified, so they cannot be matched safely") +
                   QStringLiteral(")");
    }
    if (summary.cancelled) {
        message += QStringLiteral(" · ") + tr_controller("stopped early");
    }
    set_status_message(message);
}

void AppController::on_backup_finished(const BackupResult& result) {
    set_status_message(result.ok ? tr_controller("Catalog backed up to %1").arg(result.path)
                                 : tr_controller("The catalog backup failed: %1").arg(result.message));
}

void AppController::on_verification_progress(const VerificationProgress& progress) {
    QString activity;
    if (progress.active) {
        activity = progress.total > 0
                       ? tr_controller("Verifying photos… %1 of %2").arg(progress.done).arg(progress.total)
                       : tr_controller("Checking the library…");
    }
    if (activity != background_activity_) {
        background_activity_ = activity;
        emit backgroundActivityChanged();
    }
}

int AppController::active_export_count() const {
    if (services_.batches->is_available()) {
        return batch_list_->remaining_photos();
    }
    return export_model_->active_count();
}

int AppController::export_attention_count() const {
    return services_.batches->is_available() ? batch_list_->attention_count() : 0;
}

bool AppController::openBatchExport() {
    if (library_model_->count() == 0) {
        set_status_message(tr_controller("Add photos to the library first."));
        return false;
    }
    if (!services_.batches->is_available()) {
        set_status_message(services_.batches->unavailable_reason());
        return false;
    }
    QList<ItemId> selection = library_model_->selected_items();
    if (selection.isEmpty() && current_->valid()) {
        selection << current_->item_id();
    }
    batch_setup_->prepare(selection, library_model_->all_items());
    emit batchExportRequested();
    return true;
}

bool AppController::openFolderBatchExport(const QUrl& folder) {
    if (!folder.isLocalFile()) {
        set_status_message(tr_controller("Choose a folder on this computer or the network."));
        return false;
    }
    if (!services_.batches->is_available()) {
        set_status_message(services_.batches->unavailable_reason());
        return false;
    }
    batch_setup_->prepare(library_model_->selected_items(), library_model_->all_items());
    batch_setup_->prepare_folder(folder, true);
    emit batchExportRequested();
    return true;
}

bool AppController::startBatch() {
    const QString label = batch_setup_->batch_label();
    const quint64 token = batch_setup_->start();
    if (token == 0) {
        const QString hint = batch_setup_->start_hint();
        set_status_message(hint.isEmpty() ? tr_controller("The batch cannot start.") : hint);
        return false;
    }
    last_batch_token_ = token;
    batch_list_->track_submission(token, label);
    batch_submissions_.insert(token, label);
    const QUrl output = batch_setup_->output_folder();
    if (output != last_output_folder_) {
        last_output_folder_ = output;
        emit lastOutputFolderChanged();
    }
    set_status_message(tr_controller("Preparing %1…").arg(label));
    set_current_view(Exports);
    return true;
}

bool AppController::openBatchFolder(int row) {
    const BatchSnapshot* batch = batch_list_->batch_at(row);
    if (batch == nullptr || batch->folder.isEmpty()) {
        return false;
    }
    if (!url_opener_(QUrl::fromLocalFile(batch->folder))) {
        set_status_message(tr_controller("Could not open %1").arg(native(batch->folder)));
        return false;
    }
    return true;
}

void AppController::openBatchReport(int row) {
    const BatchSnapshot* batch = batch_list_->batch_at(row);
    if (batch == nullptr) {
        return;
    }
    report_requested_ = batch->id;
    services_.batches->request_report_file(batch->id);
}

void AppController::showBatchReport(int row) {
    batch_report_->show(batch_list_->batchIdAt(row));
}

bool AppController::showFileInFolder(const QString& path) {
    if (path.isEmpty()) {
        return false;
    }
    const QString folder = QFileInfo(path).absolutePath();
    if (!url_opener_(QUrl::fromLocalFile(folder))) {
        set_status_message(tr_controller("Could not open %1").arg(native(folder)));
        return false;
    }
    return true;
}

void AppController::on_batch_submitted(quint64 token, BatchId batch, const QString& error) {
    const auto found = batch_submissions_.find(token);
    if (found == batch_submissions_.end()) {
        return;  // not started from the dialog
    }
    const QString label = found.value();
    batch_submissions_.erase(found);
    if (batch == 0) {
        set_status_message(tr_controller("Could not start %1: %2").arg(label, error));
        return;
    }
    watched_batches_.insert(batch, label);
    set_status_message(tr_controller("Started %1").arg(label));
    // A batch can be finished already (every photo exported before, so all skipped).
    if (const auto row = batch_list_->row_of(batch)) {
        on_batch_changed(*batch_list_->batch_at(*row));
    }
}

void AppController::on_batch_changed(const BatchSnapshot& batch) {
    // The service paused a batch by itself: say so wherever the photographer is (the Exports tab is
    // flagged too), once per new attention.
    const BatchAttention seen = attentions_.value(batch.id, BatchAttention::None);
    const BatchAttention now = batch.finished_state() ? BatchAttention::None : batch.attention;
    if (now != seen) {
        if (now == BatchAttention::None) {
            attentions_.remove(batch.id);
        } else {
            attentions_.insert(batch.id, now);
            if (!batch.attention_message.isEmpty()) {
                set_status_message(batch.attention_message);
            }
        }
    }
    const auto found = watched_batches_.find(batch.id);
    // Reported once its outcome is complete: the review count and the photos left out at the start
    // are read from its report right after it finished.
    if (found == watched_batches_.end() || !batch.finished_state() || batch.review < 0) {
        return;
    }
    const QString name = batch.name.isEmpty() ? found.value() : batch.name;
    watched_batches_.erase(found);
    // An analysing-only batch's photos were analysed (their automatic revisions are the result).
    const bool analysed = batch.automatic == BatchAutomatic::AnalyzeOnly;
    QString message = batch.state == BatchState::Cancelled
                          ? (analysed ? tr_controller("%1 was cancelled: %2 of %3 analysed")
                                      : tr_controller("%1 was cancelled: %2 of %3 exported"))
                          : (analysed ? tr_controller("%1 finished: %2 of %3 analysed")
                                      : tr_controller("%1 finished: %2 of %3 exported"));
    message = message.arg(name).arg(batch.completed).arg(batch.total);
    if (batch.skipped > 0) {
        message += QStringLiteral(" · ") + tr_controller("%1 skipped").arg(batch.skipped);
    }
    if (batch.excluded > 0) {
        message += QStringLiteral(" · ") + tr_controller("%1 left out").arg(batch.excluded);
    }
    if (batch.failed > 0) {
        message += QStringLiteral(" · ") + tr_controller("%1 failed").arg(batch.failed);
    }
    if (batch.review > 0) {
        message += QStringLiteral(" · ") + tr_controller("%1 for review").arg(batch.review);
    }
    set_status_message(message);
}

bool AppController::openItem(int row) {
    if (library_model_->item_at(row) == nullptr) {
        return false;
    }
    set_current_index(row);
    set_current_view(Edit);
    return true;
}

void AppController::showView(View view) {
    set_current_view(view);
}

void AppController::goBack() {
    switch (view_) {
    case Edit:
        set_current_view(Library);
        break;
    case Exports:
        set_current_view(previous_view_ == Exports ? Library : previous_view_);
        break;
    case Library:
        break;
    }
}

bool AppController::selectRelative(int delta) {
    const int count = library_model_->count();
    if (count == 0 || delta == 0) {
        return false;
    }
    const int from = std::max(current_index(), 0);
    const int to = std::clamp(from + delta, 0, count - 1);
    if (to == current_index()) {
        return false;
    }
    set_current_index(to);
    return true;
}

bool AppController::exportCurrent(const QUrl& folder) {
    if (!current_->valid()) {
        set_status_message(tr_controller("Open a photo to export it."));
        return false;
    }
    if (!folder.isLocalFile()) {
        set_status_message(tr_controller("Choose an output folder on this computer or the network."));
        return false;
    }
    return export_items({current_->item_id()}, folder.toLocalFile(), export_options_);
}

bool AppController::export_items(const QList<ItemId>& items, const QString& folder, const ExportOptions& options) {
    if (items.isEmpty() || folder.isEmpty()) {
        return false;
    }
    const QString absolute = QDir::cleanPath(QDir::current().absoluteFilePath(folder));
    services_.exports->start_export(ExportRequest{items, absolute, options});
    const QUrl url = QUrl::fromLocalFile(absolute);
    if (url != last_output_folder_) {
        last_output_folder_ = url;
        emit lastOutputFolderChanged();
    }
    return true;
}

void AppController::cancelExport(int row) {
    if (const ExportJob* job = export_model_->job_at(row); job != nullptr) {
        services_.exports->cancel_job(job->id);
    }
}

bool AppController::openOutputFolder(int row) {
    QString folder;
    if (row >= 0) {
        const ExportJob* job = export_model_->job_at(row);
        if (job == nullptr) {
            return false;
        }
        folder = job->output_folder;
    } else {
        folder = last_output_folder_.toLocalFile();
    }
    if (folder.isEmpty()) {
        return false;
    }
    if (!url_opener_(QUrl::fromLocalFile(folder))) {
        set_status_message(tr_controller("Could not open %1").arg(native(folder)));
        return false;
    }
    return true;
}

QString AppController::displayPath(const QUrl& url) const {
    return url.isLocalFile() ? native(url.toLocalFile()) : url.toString();
}

QString AppController::nativePath(const QString& path) const {
    return native(path);
}

void AppController::install_image_providers(QQmlEngine& engine) const {
    lucent::ui::install_image_providers(engine, services_.images, registry_);
    install_frame_providers(engine, frame_store_);
}

void AppController::set_url_opener(UrlOpener opener) {
    url_opener_ = std::move(opener);
}

void AppController::set_current_view(View view) {
    if (view != Library && view != Edit && view != Exports) {
        return;
    }
    if (view == view_) {
        return;
    }
    if (view == Edit && !current_->valid() && library_model_->count() > 0) {
        set_current_index(0);
    }
    previous_view_ = view_;
    view_ = view;
    update_edit_item();
    emit currentViewChanged();
    emit windowTitleChanged();
}

void AppController::update_edit_item() {
    edit_->set_item(view_ == Edit && current_->valid() ? current_->item_id() : 0);
}

void AppController::set_current_index(int row) {
    current_->set_row(row);  // emits CurrentItem::changed, handled by on_current_changed
}

bool AppController::busy() const {
    return importing_ || services_.library->is_busy() || services_.exports->is_busy() ||
           services_.edits->is_busy() || services_.edits->is_auto_running() || services_.batches->is_busy();
}

void AppController::set_include_subfolders(bool include) {
    if (include == include_subfolders_) {
        return;
    }
    include_subfolders_ = include;
    emit includeSubfoldersChanged();
}

QStringList AppController::photo_name_filters() const {
    QStringList patterns;
    for (const QString& suffix : services_.library->supported_suffixes()) {
        patterns << QStringLiteral("*.") + suffix;
    }
    QStringList filters;
    if (!patterns.isEmpty()) {
        filters << tr_controller("RAW photos (%1)").arg(patterns.join(u' '));
    }
    filters << tr_controller("All files (*)");
    return filters;
}

void AppController::set_clipping_overlay(bool enabled) {
    if (enabled == clipping_overlay_) {
        return;
    }
    clipping_overlay_ = enabled;
    edit_->set_clipping(enabled);
    emit clippingOverlayChanged();
}

QString AppController::window_title() const {
    const QString app = QStringLiteral("Lucent");
    switch (view_) {
    case Edit:
        return current_->valid() ? current_->file_name() + QStringLiteral(" — ") + app : app;
    case Exports:
        return tr_controller("Exports") + QStringLiteral(" — ") + app;
    case Library:
        break;
    }
    return app;
}

void AppController::on_import_started(const QStringList& paths) {
    set_importing(true);
    set_status_message(tr_controller("Importing from %1…").arg(describe_sources(paths)));
}

void AppController::on_import_progress(const ImportProgress& progress) {
    if (progress.candidates <= 0) {
        set_status_message(tr_controller("Looking for photos… %1 files found").arg(progress.files_seen));
        return;
    }
    if (progress.copying) {
        set_status_message(tr_controller("Copying and verifying photos… %1 of %2")
                               .arg(std::min(progress.checked + 1, progress.candidates))
                               .arg(progress.candidates));
        return;
    }
    set_status_message(
        tr_controller("Adding photos… %1 of %2 checked").arg(progress.checked).arg(progress.candidates));
}

void AppController::on_import_finished(const ImportSummary& summary) {
    set_importing(services_.library->is_importing());
    set_status_message(summary_message(summary));
    const QStringList problems = summary.errors + summary.notes;
    if (problems != import_problems_) {
        import_problems_ = problems;
        emit importProblemsChanged();
    }
}

void AppController::on_job_changed(const ExportJob& job) {
    switch (job.state) {
    case ExportJobState::Completed:
        set_status_message(tr_controller("Exported %1 to %2")
                               .arg(job.source_name, native(job.destination_path.isEmpty()
                                                                ? job.output_folder
                                                                : job.destination_path)));
        break;
    case ExportJobState::Failed:
        set_status_message(tr_controller("Could not export %1: %2").arg(job.source_name, job.message));
        break;
    case ExportJobState::Queued:
    case ExportJobState::Running:
    case ExportJobState::Cancelled:
        break;
    }
}

void AppController::set_export_options(const ExportOptions& options) {
    ExportOptions valid = options;
    if (valid.format != ExportFileFormat::Jpeg && valid.format != ExportFileFormat::Tiff16) {
        valid.format = ExportFileFormat::Jpeg;
    }
    if (valid.color_space != ExportColorSpace::Srgb && valid.color_space != ExportColorSpace::DisplayP3) {
        valid.color_space = ExportColorSpace::Srgb;
    }
    valid.jpeg_quality = std::clamp(valid.jpeg_quality, 1, 100);
    if (valid == export_options_) {
        return;
    }
    export_options_ = valid;
    emit exportOptionsChanged();
}

void AppController::set_export_format(FileFormat format) {
    ExportOptions options = export_options_;
    options.format = static_cast<ExportFileFormat>(format);
    set_export_options(options);
}

void AppController::set_export_color_space(ColorSpace color_space) {
    ExportOptions options = export_options_;
    options.color_space = static_cast<ExportColorSpace>(color_space);
    set_export_options(options);
}

void AppController::set_export_quality(int quality) {
    ExportOptions options = export_options_;
    options.jpeg_quality = quality;
    set_export_options(options);
}

void AppController::update_suggested_export_name() {
    const QString name =
        current_->valid() ? services_.exports->suggested_file_name(current_->item_id(), export_options_) : QString();
    if (name != suggested_export_name_) {
        suggested_export_name_ = name;
        emit suggestedExportNameChanged();
    }
}

void AppController::on_items_added() {
    if (!current_->valid() && library_model_->count() > 0) {
        set_current_index(0);
    }
}

void AppController::on_current_changed() {
    update_edit_item();
    if (current_->row() != last_row_) {
        last_row_ = current_->row();
        emit currentIndexChanged();
    }
    if (view_ == Edit) {
        emit windowTitleChanged();
    }
    update_suggested_export_name();
}

void AppController::set_status_message(const QString& message) {
    if (message == status_message_) {
        return;
    }
    status_message_ = message;
    emit statusMessageChanged();
}

void AppController::set_importing(bool importing) {
    if (importing == importing_) {
        return;
    }
    importing_ = importing;
    emit importingChanged();
    emit busyChanged();
}

}  // namespace lucent::ui
