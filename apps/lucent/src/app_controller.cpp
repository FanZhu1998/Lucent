#include "app_controller.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QQmlEngine>

#include "imaging/image_provider.hpp"

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
    if (summary.added > 0) {
        message = count_phrase(summary.added, "Added 1 photo from %2", "Added %1 photos from %2").arg(sources);
        if (summary.updated > 0) {
            message += QStringLiteral(" · ") + count_phrase(summary.updated, "1 changed photo updated",
                                                            "%1 changed photos updated");
        }
    } else if (summary.updated > 0) {
        message = count_phrase(summary.updated, "Updated 1 changed photo in %2", "Updated %1 changed photos in %2")
                      .arg(sources);
    } else {
        message = tr_controller("No new photos in %1").arg(sources);
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
        throw std::invalid_argument("lucent::ui::AppController needs a library, image and export service");
    }
    library_model_ = std::make_unique<LibraryModel>(*services_.library, registry_);
    export_model_ = std::make_unique<ExportJobModel>(*services_.exports);
    current_ = std::make_unique<CurrentItem>(*library_model_);
    // Owned here, never by the QML engine's garbage collector.
    QQmlEngine::setObjectOwnership(library_model_.get(), QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(export_model_.get(), QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(current_.get(), QQmlEngine::CppOwnership);

    url_opener_ = [](const QUrl& url) { return QDesktopServices::openUrl(url); };

    LibraryService& library = *services_.library;
    connect(&library, &LibraryService::import_started, this, &AppController::on_import_started);
    connect(&library, &LibraryService::import_progress, this, &AppController::on_import_progress);
    connect(&library, &LibraryService::import_finished, this, &AppController::on_import_finished);
    connect(services_.exports.get(), &ExportService::job_changed, this, &AppController::on_job_changed);
    connect(&library, &LibraryService::busy_changed, this, &AppController::busyChanged);
    connect(services_.exports.get(), &ExportService::busy_changed, this, &AppController::busyChanged);
    // After the model has inserted the rows (it connected first).
    connect(&library, &LibraryService::items_added, this, &AppController::on_items_added);
    connect(current_.get(), &CurrentItem::changed, this, &AppController::on_current_changed);
    connect(this, &AppController::exportOptionsChanged, this, &AppController::update_suggested_export_name);
}

AppController::~AppController() = default;

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
    QStringList absolute;
    for (const QString& path : paths) {
        if (!path.trimmed().isEmpty()) {
            absolute << QDir::cleanPath(QDir::current().absoluteFilePath(path));
        }
    }
    if (absolute.isEmpty()) {
        return false;
    }
    services_.library->import_paths(absolute, include_subfolders);
    return true;
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

void AppController::install_image_providers(QQmlEngine& engine) const {
    lucent::ui::install_image_providers(engine, services_.images, registry_);
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
    emit currentViewChanged();
    emit windowTitleChanged();
}

void AppController::set_current_index(int row) {
    current_->set_row(row);  // emits CurrentItem::changed, handled by on_current_changed
}

bool AppController::busy() const {
    return importing_ || services_.library->is_busy() || services_.exports->is_busy();
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
