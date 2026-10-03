#pragma once

// AppController: the root object the QML window binds to (Main.qml's required property
// `controller`). It owns the services, the view models and the generation registry, and turns user
// intents (add photos, open a photo, export, open the output folder) into service calls. W6: its
// EditSession (`edit`) follows the photo while the Edit view shows it, and the frame image
// providers (imaging/frame_store.hpp) serve that session's frames.
//
// Blueprint section 2 ("Library, Edit and Exports are three views of one application") and
// section 4 (the UI calls application services and never decodes on its thread).
//
// Naming: QML-facing members (properties, invokables, signals) use QML's lowerCamelCase; C++-only
// members use snake_case.
//
// Lifetime: create the controller before the QQmlEngine that loads Main.qml and destroy it after
// that engine (declare it first). The engine's image providers share ownership of the image source
// and the registry, so in-flight image requests stay valid regardless.
//
// Thread-safety: UI thread only.

#include <functional>
#include <memory>

#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QtQmlIntegration/qqmlintegration.h>

#include "imaging/frame_store.hpp"
#include "imaging/generation_registry.hpp"
#include "models/current_item.hpp"
#include "models/edit_session.hpp"
#include "models/export_job_model.hpp"
#include "models/library_model.hpp"
#include "services/services.hpp"

class QQmlEngine;

namespace lucent::ui {

class AppController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("AppController is created by the application.")
    Q_PROPERTY(View currentView READ current_view WRITE set_current_view NOTIFY currentViewChanged FINAL)
    Q_PROPERTY(int currentIndex READ current_index WRITE set_current_index NOTIFY currentIndexChanged FINAL)
    Q_PROPERTY(lucent::ui::LibraryModel* library READ library CONSTANT FINAL)
    Q_PROPERTY(lucent::ui::ExportJobModel* exports READ exports CONSTANT FINAL)
    Q_PROPERTY(lucent::ui::CurrentItem* current READ current CONSTANT FINAL)
    // W6. Editing of the photo shown in the Edit view (open while the Edit view shows it).
    Q_PROPERTY(lucent::ui::EditSession* edit READ edit CONSTANT FINAL)
    Q_PROPERTY(bool importing READ importing NOTIFY importingChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged FINAL)
    Q_PROPERTY(QString statusMessage READ status_message NOTIFY statusMessageChanged FINAL)
    Q_PROPERTY(bool includeSubfolders READ include_subfolders WRITE set_include_subfolders NOTIFY
                   includeSubfoldersChanged FINAL)
    Q_PROPERTY(QStringList photoNameFilters READ photo_name_filters CONSTANT FINAL)
    Q_PROPERTY(bool clippingOverlay READ clipping_overlay WRITE set_clipping_overlay NOTIFY
                   clippingOverlayChanged FINAL)
    Q_PROPERTY(QString simulationNotice READ simulation_notice CONSTANT FINAL)
    Q_PROPERTY(QUrl lastOutputFolder READ last_output_folder NOTIFY lastOutputFolderChanged FINAL)
    Q_PROPERTY(QString windowTitle READ window_title NOTIFY windowTitleChanged FINAL)
    // Per-file reasons of the most recent import (failures, then Limited/Unsupported items).
    Q_PROPERTY(QStringList importProblems READ import_problems NOTIFY importProblemsChanged FINAL)
    // Options of the next export (the Export dialog edits them; kept for the session).
    Q_PROPERTY(
        FileFormat exportFormat READ export_format WRITE set_export_format NOTIFY exportOptionsChanged FINAL)
    Q_PROPERTY(ColorSpace exportColorSpace READ export_color_space WRITE set_export_color_space NOTIFY
                   exportOptionsChanged FINAL)
    Q_PROPERTY(int exportQuality READ export_quality WRITE set_export_quality NOTIFY exportOptionsChanged FINAL)
    // File name the current photo would be exported as with the export options; empty when none.
    Q_PROPERTY(
        QString suggestedExportName READ suggested_export_name NOTIFY suggestedExportNameChanged FINAL)
    // W4. Options of the Import dialog ("Copy into library folder" and its destination, kept for the
    // session; Lucent.exe remembers the destination between sessions).
    Q_PROPERTY(bool copyIntoLibrary READ copy_into_library WRITE set_copy_into_library NOTIFY
                   importOptionsChanged FINAL)
    Q_PROPERTY(QUrl copyDestination READ copy_destination WRITE set_copy_destination NOTIFY
                   importOptionsChanged FINAL)
    // Files and folders chosen for the next import, waiting for the Import dialog; empty when none.
    Q_PROPERTY(QStringList pendingImport READ pending_import NOTIFY pendingImportChanged FINAL)
    Q_PROPERTY(bool canRelink READ can_relink CONSTANT FINAL)
    Q_PROPERTY(bool canBackUp READ can_back_up CONSTANT FINAL)
    // A notice about the library itself (catalog unavailable or read-only); empty when all is well.
    Q_PROPERTY(QString libraryNotice READ library_notice CONSTANT FINAL)
    // Background verification of the library's files ("Verifying photos… 12 of 340"); empty when idle.
    Q_PROPERTY(QString backgroundActivity READ background_activity NOTIFY backgroundActivityChanged FINAL)

public:
    enum View { Library = 0, Edit = 1, Exports = 2 };
    Q_ENUM(View)
    // Values of lucent::ui::ExportFileFormat and ExportColorSpace.
    enum FileFormat { Jpeg = 1, Tiff16 = 2 };
    Q_ENUM(FileFormat)
    enum ColorSpace { Srgb = 1, DisplayP3 = 2 };
    Q_ENUM(ColorSpace)

    // Opens a folder or file URL in the desktop shell; returns false when that failed.
    using UrlOpener = std::function<bool(const QUrl&)>;

    // Takes ownership of `services`. Incomplete services are a programming error: the constructor
    // throws std::invalid_argument (caught nowhere; it cannot happen with service_factory.cpp).
    explicit AppController(Services services, QObject* parent = nullptr);
    ~AppController() override;

    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    // --- QML API ---------------------------------------------------------------------------------

    // Imports the local files among `urls` (other URLs are ignored). Returns false when none was
    // local.
    Q_INVOKABLE bool addFiles(const QList<QUrl>& urls);
    // Imports a local folder, recursively when includeSubfolders is set.
    Q_INVOKABLE bool addFolder(const QUrl& folder);
    // Imports what was dropped on the window: the local files and folders among `urls`, folders
    // recursively when includeSubfolders is set. Returns false when none was local.
    Q_INVOKABLE bool addDropped(const QList<QUrl>& urls);
    // Makes `row` current and shows the Edit view; false for an invalid row.
    Q_INVOKABLE bool openItem(int row);
    Q_INVOKABLE void showView(lucent::ui::AppController::View view);
    // Esc: Edit returns to Library; Exports returns to the view it was opened from.
    Q_INVOKABLE void goBack();
    // Moves the current photo by `delta` rows, stopping at either end. False when it did not move.
    Q_INVOKABLE bool selectRelative(int delta);
    // Exports the current photo to the local folder `folder` with the export options.
    Q_INVOKABLE bool exportCurrent(const QUrl& folder);
    Q_INVOKABLE void cancelExport(int row);
    // Opens the output folder of the job at `row`, or the last output folder when row < 0.
    Q_INVOKABLE bool openOutputFolder(int row);
    // The local path of `url` with native separators (the URL as text when it is not local).
    Q_INVOKABLE QString displayPath(const QUrl& url) const;
    // `path` with native separators.
    Q_INVOKABLE QString nativePath(const QString& path) const;

    // --- W4: import dialog, relinking, picks, catalog backup -----------------------------------

    // Remembers the local files and folders among `urls` as pendingImport, for the Import dialog
    // to confirm (with Include subfolders and Copy into library folder). False when none is local.
    Q_INVOKABLE bool requestImport(const QList<QUrl>& urls);
    // Imports pendingImport with the dialog's options and clears it. False (pendingImport kept)
    // when copying is chosen without a destination, or when nothing is pending.
    Q_INVOKABLE bool confirmImport();
    Q_INVOKABLE void cancelImport();
    // Looks for every missing photo in `folder` and its subfolders; files are relinked only when
    // their content matches. False for a folder that is not local.
    Q_INVOKABLE bool relinkMissing(const QUrl& folder);
    // The same for the current photo's file only.
    Q_INVOKABLE bool relinkCurrent(const QUrl& folder);
    // Sets the current photo's pick: "picked", "rejected" or "none".
    Q_INVOKABLE void setCurrentPick(const QString& key);
    // Writes a verified backup of the catalog; the outcome appears in the status line.
    Q_INVOKABLE void backUpCatalog();
    Q_INVOKABLE bool openBackupFolder();

    // --- C++ API ---------------------------------------------------------------------------------

    // Starts one import request for the non-empty paths among `paths` (made absolute against the
    // current directory). False, without a request, when there is none.
    bool import_paths(const QStringList& paths, bool include_subfolders);
    bool import_paths(const QStringList& paths, const ImportOptions& options);
    // Begins the library's background upkeep (LibraryService::start). Lucent.exe calls it once the
    // window is shown.
    void start();
    // Checks the library's files again (LibraryService::recheck_sources): Lucent.exe calls it when
    // a volume arrives or leaves (VolumeWatcher).
    void recheck_library();
    // Queues one export job per item, in order, into `folder` (absolute path). False when `items`
    // is empty or `folder` is empty.
    bool export_items(const QList<ItemId>& items, const QString& folder, const ExportOptions& options = {});
    // Registers the thumbnail and preview image providers with `engine`.
    void install_image_providers(QQmlEngine& engine) const;
    // Replaces the desktop opener (default: QDesktopServices::openUrl). Tests use this.
    void set_url_opener(UrlOpener opener);

    [[nodiscard]] View current_view() const noexcept { return view_; }
    void set_current_view(View view);
    [[nodiscard]] int current_index() const noexcept { return current_->row(); }
    void set_current_index(int row);

    [[nodiscard]] LibraryModel* library() const noexcept { return library_model_.get(); }
    [[nodiscard]] ExportJobModel* exports() const noexcept { return export_model_.get(); }
    [[nodiscard]] CurrentItem* current() const noexcept { return current_.get(); }
    [[nodiscard]] EditSession* edit() const noexcept { return edit_.get(); }
    [[nodiscard]] EditService& edit_service() const noexcept { return *services_.edits; }
    [[nodiscard]] LibraryService& library_service() const noexcept { return *services_.library; }
    [[nodiscard]] ExportService& export_service() const noexcept { return *services_.exports; }

    [[nodiscard]] bool importing() const noexcept { return importing_; }
    [[nodiscard]] bool busy() const;
    [[nodiscard]] QString status_message() const { return status_message_; }
    [[nodiscard]] bool include_subfolders() const noexcept { return include_subfolders_; }
    void set_include_subfolders(bool include);
    [[nodiscard]] QStringList photo_name_filters() const;
    [[nodiscard]] bool clipping_overlay() const noexcept { return clipping_overlay_; }
    void set_clipping_overlay(bool enabled);
    [[nodiscard]] QString simulation_notice() const { return services_.simulation_notice; }
    [[nodiscard]] QUrl last_output_folder() const { return last_output_folder_; }
    [[nodiscard]] QString window_title() const;
    [[nodiscard]] QStringList import_problems() const { return import_problems_; }
    [[nodiscard]] const ExportOptions& export_options() const noexcept { return export_options_; }
    void set_export_options(const ExportOptions& options);
    [[nodiscard]] FileFormat export_format() const noexcept {
        return static_cast<FileFormat>(export_options_.format);
    }
    void set_export_format(FileFormat format);
    [[nodiscard]] ColorSpace export_color_space() const noexcept {
        return static_cast<ColorSpace>(export_options_.color_space);
    }
    void set_export_color_space(ColorSpace color_space);
    [[nodiscard]] int export_quality() const noexcept { return export_options_.jpeg_quality; }
    void set_export_quality(int quality);
    [[nodiscard]] QString suggested_export_name() const { return suggested_export_name_; }
    [[nodiscard]] bool copy_into_library() const noexcept { return copy_into_library_; }
    void set_copy_into_library(bool copy);
    [[nodiscard]] QUrl copy_destination() const { return copy_destination_; }
    void set_copy_destination(const QUrl& folder);
    [[nodiscard]] QStringList pending_import() const { return pending_import_; }
    [[nodiscard]] bool can_relink() const;
    [[nodiscard]] bool can_back_up() const;
    [[nodiscard]] QString library_notice() const;
    [[nodiscard]] QString background_activity() const { return background_activity_; }

signals:
    void currentViewChanged();
    void currentIndexChanged();
    void importingChanged();
    void busyChanged();
    void statusMessageChanged();
    void includeSubfoldersChanged();
    void clippingOverlayChanged();
    void lastOutputFolderChanged();
    void windowTitleChanged();
    void importProblemsChanged();
    void exportOptionsChanged();
    void suggestedExportNameChanged();
    void importOptionsChanged();
    void pendingImportChanged();
    void backgroundActivityChanged();

private:
    bool import_urls(const QList<QUrl>& urls, bool include_subfolders);
    void on_import_started(const QStringList& paths);
    void on_import_progress(const ImportProgress& progress);
    void on_import_finished(const ImportSummary& summary);
    void on_job_changed(const ExportJob& job);
    void update_suggested_export_name();
    void on_items_added();
    void on_current_changed();
    void set_status_message(const QString& message);
    void set_importing(bool importing);
    void on_relink_finished(const RelinkSummary& summary);
    void on_backup_finished(const BackupResult& result);
    void on_verification_progress(const VerificationProgress& progress);
    bool relink(const QUrl& folder, const QList<ItemId>& items);
    // The edit session follows the photo while the Edit view shows it.
    void update_edit_item();

    Services services_;  // declared first: destroyed after the models that observe it
    std::shared_ptr<GenerationRegistry> registry_;
    std::unique_ptr<LibraryModel> library_model_;
    std::unique_ptr<ExportJobModel> export_model_;
    std::unique_ptr<CurrentItem> current_;
    std::shared_ptr<FrameStore> frame_store_;  // shared with the frame image providers
    std::unique_ptr<EditSession> edit_;

    View view_ = Library;
    View previous_view_ = Library;
    int last_row_ = -1;
    bool importing_ = false;
    bool include_subfolders_ = false;
    bool clipping_overlay_ = false;
    QString status_message_;
    QUrl last_output_folder_;
    UrlOpener url_opener_;
    QStringList import_problems_;
    ExportOptions export_options_;
    QString suggested_export_name_;
    bool copy_into_library_ = false;
    QUrl copy_destination_;
    QStringList pending_import_;
    QString background_activity_;
};

}  // namespace lucent::ui
