#pragma once

// LibraryService: the shell's view of import and the library (blueprint section 2, "Import and
// organize").
//
// Threading contract: a LibraryService object lives on the UI thread. Its member functions are
// called on the UI thread and must return promptly: scanning folders, probing or decoding files,
// copying, hashing and catalog writes happen elsewhere (blueprint section 4). Results are
// delivered by the signals, which are emitted on the UI thread, never from inside the call that
// requested them.
//
// Persistence (W4). A persistent library (the catalog) already holds items when the service is
// created: current_items() lists them, so the contact sheet shows the library before any file is
// read. start() then begins the background upkeep (presence checks, content hashing), whose
// results arrive as item_changed. Relinking, picks and catalog backups are W4 operations a
// service may not support (can_relink(), can_back_up()); the defaults do nothing.
//
// Ownership: the shell owns the service (Services::library) and destroys it after the QML engine.
// A service must not emit signals from its destructor.

#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QStringList>

#include "services/service_types.hpp"

namespace lucent::ui {

class LibraryService : public QObject {
    Q_OBJECT

public:
    explicit LibraryService(QObject* parent = nullptr) : QObject(parent) {}
    ~LibraryService() override = default;

    LibraryService(const LibraryService&) = delete;
    LibraryService& operator=(const LibraryService&) = delete;

    // Lower-case file suffixes without the dot (e.g. "cr3", "arw", "dng") that import accepts,
    // sorted. Used for the file picker's filter; the service still validates every file.
    [[nodiscard]] virtual QStringList supported_suffixes() const = 0;

    // Items in the library when the service was created (a persistent library loads them at
    // start-up), in library order. Models created afterwards start from this list. Default: none.
    [[nodiscard]] virtual QList<LibraryItem> current_items() const { return {}; }

    // Starts the background upkeep of a persistent library once the window is up: every file is
    // checked (Missing / Changed badges) and files without a content hash are hashed. Default:
    // nothing to do.
    virtual void start() {}

    // Checks every file of a persistent library again after start(), for example when a drive or
    // memory card was connected or removed: files whose volume left become Missing, files that
    // are back become Present. Requests made while a check is pending are merged. Default:
    // nothing to do.
    virtual void recheck_sources() {}

    // Starts importing `paths`: absolute paths of files and/or folders. Folders are scanned for
    // supported files, recursively when options.include_subfolders is set (junctions and symbolic
    // links never cause loops or double scans). With options.copy_to the photos are copied into
    // that folder, each copy verified, and the copies are added (the originals are only read).
    // Requests are processed in the order made. For each request the service emits
    // import_started, then items_added, item_changed and import_progress zero or more times, then
    // import_finished exactly once (also when every path failed).
    virtual void start_import(const QStringList& paths, const ImportOptions& options) = 0;

    // start_import() without copying.
    void import_paths(const QStringList& paths, bool include_subfolders) {
        start_import(paths, ImportOptions{include_subfolders, {}});
    }

    // Relinking missing files (relink_finished reports the outcome). `items` selects the files to
    // relink (any item of a file); empty relinks every missing file. Default: unsupported, and
    // relink_finished reports that.
    [[nodiscard]] virtual bool can_relink() const { return false; }
    virtual void relink(const QString& folder, const QList<ItemId>& items);

    // Records the photographer's pick of `item` (item_changed follows). Default: ignored.
    virtual void set_pick(ItemId item, PickState pick) {
        Q_UNUSED(item);
        Q_UNUSED(pick);
    }

    // Writes a verified backup of the catalog into its backup folder (backup_finished reports
    // it). Default: unsupported, and backup_finished reports that.
    [[nodiscard]] virtual bool can_back_up() const { return false; }
    virtual void back_up_catalog();
    // The folder catalog backups are written to; empty when there is none.
    [[nodiscard]] virtual QString backup_folder() const { return {}; }

    // A short user-facing notice about the library itself (for example that the catalog could not
    // be opened and changes are not saved); empty when all is well.
    [[nodiscard]] virtual QString library_notice() const { return {}; }

    // W10 (ADR 0018). The camera support database the items' support states come from (Help >
    // About). Default: none (available false, no problem).
    [[nodiscard]] virtual CameraSupportInfo camera_support() const { return {}; }
    // W13 (ADR 0023). The lens database the lens-profile corrections use (Help > About). Default: none
    // (available false, no problem).
    [[nodiscard]] virtual LensDatabaseInfo lens_database() const { return {}; }

    // True from the first import_started until the last import_finished.
    [[nodiscard]] virtual bool is_importing() const = 0;

    // True while an import runs or other work that will change items is pending (for example
    // thumbnails being rendered). Changes are announced by busy_changed.
    [[nodiscard]] virtual bool is_busy() const = 0;

signals:
    void import_started(const QStringList& paths);
    // Progress of the running request (throttled; at most a few times per second).
    void import_progress(const lucent::ui::ImportProgress& progress);
    // New items, in a deterministic order (sorted by path within one request). Ids are new.
    void items_added(const QList<lucent::ui::LibraryItem>& items);
    // A complete new snapshot of an existing item.
    void item_changed(const lucent::ui::LibraryItem& item);
    void import_finished(const lucent::ui::ImportSummary& summary);
    void busy_changed(bool busy);
    void relink_finished(const lucent::ui::RelinkSummary& summary);
    void backup_finished(const lucent::ui::BackupResult& result);
    void verification_progress(const lucent::ui::VerificationProgress& progress);
};

}  // namespace lucent::ui
