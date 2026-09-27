#pragma once

// AppLibraryService: the shell's LibraryService (services/library_service.hpp) over the
// application services' import and session library (docs/adr/0007).
//
// Import. Each import_paths() request becomes one app::import_sources() call on the executor's
// Background lane; requests run one at a time, in the order made. Paths that are folders are
// scanned (recursively when asked), everything else is imported as a selected file; the probe
// decides what is a photo, and files it rejects stay listed as Unsupported with the reason
// (selected files with a non-RAW extension that are not RAW files are skipped and named in the
// summary's notes). Folders chosen for exports in this session are not scanned into when a scan
// reaches them below the folder being imported (Backend::output_folders(), import.hpp); the
// summary's notes name them. Progress is throttled to about ten updates a second.
//
// Changed files. Importing a file again after it changed on disk refreshes its items
// (app::Library::refresh): the library facts are replaced, and the shell's own state is reset:
// processing NotProcessed, render info cleared, preview generation + 1, and the thumbnail
// moves to a new generation of Lucent's render (generation 1, the camera preview, may be cached
// by Qt with the old file's pixels). The summary counts them as updated.
//
// Items. The adapter observes the application library: items committed by the import (on the
// import's worker thread) are converted to LibraryItem snapshots on that thread and posted to the
// UI thread, where items_added is emitted, before the request's import_finished. The adapter
// keeps the shell's own state of each item: thumbnail and preview generations, processing status
// and Lucent's latest render, which the image source reports through the RenderEvent sink.
//
// Render events (from AppImageSource, any thread; applied on the UI thread in posting order):
//   CameraPreviewUnavailable  the thumbnail moves from generation 1 (camera JPEG) to 2 (Lucent's
//                             render), no longer labelled as a camera preview;
//   PreviewStarted            processing = Processing (unless already Processed);
//   PreviewAbandoned          back to NotProcessed when it was Processing (superseded/cancelled);
//   PreviewFailed             processing = Failed, with the reason;
//   PreviewDelivered          processing = Processed, render info and full size updated, and a
//                             camera-preview thumbnail replaced by Lucent's render (generation + 1)
//                             when the event says a Lucent thumbnail is available.
//
// Lifetime and threading: lives on the UI thread; the destructor closes the dispatcher, ends the
// library subscription (waiting for an observer running on another thread) and cancels a
// running import, so nothing reaches the object afterwards. It emits no signals from its
// destructor.

#include <QHash>
#include <QList>
#include <QStringList>
#include <deque>
#include <functional>
#include <memory>

#include <lucent/application/import.hpp>
#include <lucent/application/library.hpp>
#include <lucent/application/task_executor.hpp>
#include <lucent/core/error.hpp>

#include "adapters/backend.hpp"
#include "adapters/ui_dispatcher.hpp"
#include "services/library_service.hpp"

namespace lucent::ui::adapters {

struct RenderEvent {
    enum class Kind : std::uint8_t {
        CameraPreviewUnavailable = 1,
        PreviewStarted = 2,
        PreviewAbandoned = 3,
        PreviewFailed = 4,
        PreviewDelivered = 5,
    };

    Kind kind = Kind::PreviewStarted;
    ItemId item = 0;
    QString message;       // PreviewFailed: the user-facing reason
    RenderInfoPtr render;  // PreviewDelivered
    // PreviewDelivered: a thumbnail made from this render is available (thumbnail generation 2+).
    bool lucent_thumbnail = false;
    // PreviewDelivered: replace existing render info (false keeps an earlier render's info, used
    // for small thumbnail renders).
    bool primary = true;
};

// Thread-safe; posting after the library service is gone does nothing.
using RenderEventSink = std::function<void(RenderEvent)>;

class AppLibraryService final : public LibraryService {
    Q_OBJECT

public:
    explicit AppLibraryService(std::shared_ptr<Backend> backend, QObject* parent = nullptr);
    ~AppLibraryService() override;

    [[nodiscard]] QStringList supported_suffixes() const override;
    void import_paths(const QStringList& paths, bool include_subfolders) override;
    [[nodiscard]] bool is_importing() const override { return active_imports_ > 0; }
    [[nodiscard]] bool is_busy() const override { return is_importing(); }

    // A sink for render events that is safe to call from any thread at any time.
    [[nodiscard]] RenderEventSink render_event_sink();

    // The current snapshot of `item`, or nullptr (for tests and the export adapter).
    [[nodiscard]] const LibraryItem* item(ItemId item) const;

private:
    struct PendingImport {
        QStringList paths;
        bool include_subfolders = false;
    };

    void start_next_import();
    void finish_import(const PendingImport& request, const Expected<app::ImportReport>& report);
    void add_items(const QList<LibraryItem>& items);
    void update_items(const QList<LibraryItem>& items);
    void refresh_items(const QList<LibraryItem>& items);
    void apply(const RenderEvent& event);
    void update_busy();

    std::shared_ptr<Backend> backend_;
    std::shared_ptr<UiDispatcher> dispatcher_;
    app::Subscription subscription_;  // after dispatcher_: reset first on destruction

    QHash<ItemId, LibraryItem> items_;
    std::deque<PendingImport> queue_;
    bool running_ = false;
    app::TaskHandle running_task_;
    int active_imports_ = 0;  // queued + running
    bool busy_ = false;
};

}  // namespace lucent::ui::adapters
