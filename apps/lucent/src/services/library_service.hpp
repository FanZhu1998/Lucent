#pragma once

// LibraryService: the shell's view of import and the library (blueprint section 2, "Import and
// organize").
//
// Threading contract: a LibraryService object lives on the UI thread. Its member functions are
// called on the UI thread and must return promptly: scanning folders, probing or decoding files
// and hashing happen elsewhere (blueprint section 4). Results are delivered by the signals, which
// are emitted on the UI thread.
//
// Ownership: the shell owns the service (Services::library) and destroys it after the QML engine.
// A service must not emit signals from its destructor.

#include <QList>
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

    // Starts importing `paths`: absolute paths of files and/or folders. Folders are scanned for
    // supported files, recursively when `include_subfolders` is true (junctions and symbolic links
    // never cause loops or double scans). Requests are processed in the order made. For each
    // request the service emits import_started, then items_added and import_progress zero or more
    // times, then import_finished exactly once (also when every path failed).
    virtual void import_paths(const QStringList& paths, bool include_subfolders) = 0;

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
};

}  // namespace lucent::ui
