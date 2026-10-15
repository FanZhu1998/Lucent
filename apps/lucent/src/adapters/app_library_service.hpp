#pragma once

// AppLibraryService: the shell's LibraryService (services/library_service.hpp) over the
// application services' import, library and maintenance (docs/adr/0007, 0008).
//
// Start-up (W4). The constructor converts every item the library already holds (the catalog,
// loaded by the Backend without reading any source file) into snapshots: current_items(). start()
// then queues the maintenance service's check of every file (Missing / Changed badges as they are
// found, changed files re-read) followed by hashing of files without a content hash; nothing
// happens before start(), so the window shows the library first. recheck_sources() queues the
// same check again (a card was inserted or removed); before start() it does nothing.
//
// Import. Each start_import() request becomes one app::import_sources() call on the executor's
// Background lane; requests run one at a time, in the order made. Paths that are folders are
// scanned (recursively when asked), everything else is imported as a selected file; the probe
// decides what is a photo, and files it rejects stay listed as Unsupported with the reason
// (selected files with a non-RAW extension that are not RAW files are skipped and named in the
// summary's notes). With ImportOptions::copy_to the photos are copied into that folder and the
// verified copies are added (app::ImportRequest::copy_to). Folders chosen for exports in this
// session, and Lucent's own folders, are not scanned into (Backend::output_folders(),
// Backend::lucent_directories(), import.hpp). Progress is throttled to about ten updates a
// second. When an import finishes, its new and refreshed items are queued for hashing.
//
// Changed files. Importing a file again after it changed on disk, or the background check
// finding it changed, refreshes its items (app::Library::refresh): the library facts are
// replaced, and the shell's own state is reset: processing NotProcessed, render info cleared,
// preview generation + 1, and the thumbnail moves to a new generation of Lucent's render
// (generation 1, the camera preview, may be cached by Qt with the old file's pixels). The summary
// counts them as updated.
//
// Items. The adapter observes the application library: items committed on worker threads (an
// import, the maintenance service, a pick) are converted to LibraryItem snapshots on that thread,
// with the paths of their content duplicates, and posted to the UI thread, where items_added or
// item_changed is emitted (an import's items_added before its import_finished). The adapter keeps
// the shell's own state of each item: thumbnail and preview generations, processing status and
// Lucent's latest render, which the image source reports through the RenderEvent sink.
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
// Edits (W6). The adapter observes the recipe service: when an item's committed recipe changes (a
// revision, an undo or redo, a paste) its `edited` flag follows the new settings and, when the
// settings' identity changed, its thumbnail moves to a new generation of Lucent's render of the
// recipe (the camera preview is no longer shown: it does not show the edit). An item whose recipe
// this Lucent cannot read keeps its thumbnail. start() also looks up the items of a persistent
// library that already have revisions (in the background), so edited photos show their edits
// from the start of a session. W8: an item whose current revision is an automatic one (Auto, or an
// automatic batch) whose analysis record recommends a review gets ItemFlag::ReviewRecommended (the
// library's "Review" badge); another current revision (the photographer's edit, an undo) clears it.
//
// W4 operations: relink() runs the maintenance service's relink; set_pick() writes the item's
// flags on the executor's Quick lane; back_up_catalog() writes a verified catalog backup on the
// Background lane. Their results are signals. Verification progress (presence checks and hashing)
// is reported by verification_progress, at most every 100 ms, and counts as busy.
//
// Lifetime and threading: lives on the UI thread; the destructor closes the dispatcher, detaches
// from the maintenance events, ends the library subscription (waiting for an observer running on
// another thread) and cancels a running import, so nothing reaches the object afterwards. It
// emits no signals from its destructor. A constructor that throws leaves nothing that could reach the
// object either: the library's and the recipes' observers and the lens database's listener are held
// by subscription members, and the maintenance sink, which has no subscription, is set last.

#include <QHash>
#include <QList>
#include <QStringList>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

#include <lucent/application/import.hpp>
#include <lucent/application/library.hpp>
#include <lucent/application/maintenance_service.hpp>
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
    [[nodiscard]] QList<LibraryItem> current_items() const override;
    void start() override;
    void recheck_sources() override;
    void start_import(const QStringList& paths, const ImportOptions& options) override;
    [[nodiscard]] bool is_importing() const override { return active_imports_ > 0; }
    [[nodiscard]] bool is_busy() const override {
        return is_importing() || verifying_ || pending_operations_ > 0;
    }

    [[nodiscard]] bool can_relink() const override;
    void relink(const QString& folder, const QList<ItemId>& items) override;
    void set_pick(ItemId item, PickState pick) override;
    [[nodiscard]] bool can_back_up() const override;
    void back_up_catalog() override;
    [[nodiscard]] QString backup_folder() const override;
    [[nodiscard]] QString library_notice() const override;
    [[nodiscard]] CameraSupportInfo camera_support() const override;
    [[nodiscard]] LensDatabaseInfo lens_database() const override;

    // A sink for render events that is safe to call from any thread at any time.
    [[nodiscard]] RenderEventSink render_event_sink();

    // The current snapshot of `item`, or nullptr (for tests and the export adapter).
    [[nodiscard]] const LibraryItem* item(ItemId item) const;

private:
    struct PendingImport {
        QStringList paths;
        ImportOptions options;
    };

    void start_next_import();
    void finish_import(const PendingImport& request, const Expected<app::ImportReport>& report);
    void add_items(const QList<LibraryItem>& items);
    void update_items(const QList<LibraryItem>& items);
    void refresh_items(const QList<LibraryItem>& items);
    void apply(const RenderEvent& event);
    // An item's committed recipe (W6): edited flag, settings identity, readable; W8: whether its
    // current (automatic) revision recommends a review.
    void apply_recipe(ItemId item, bool edited, const QByteArray& settings, bool readable, bool review);
    void scan_recipes();
    void on_maintenance(const app::MaintenanceEvent& event);
    void queue_hashing(const std::vector<app::ItemId>& items);
    void operation_finished();
    void update_busy();

    std::shared_ptr<Backend> backend_;
    std::shared_ptr<UiDispatcher> dispatcher_;
    app::Subscription subscription_;  // after dispatcher_: reset first on destruction
    app::Subscription recipe_subscription_;
    app::Subscription lens_database_subscription_;  // W12 (APL-06): lens_database_changed
    // Identity (settings SHA-256) of the recipe each item's thumbnail shows, once known.
    QHash<ItemId, QByteArray> recipe_identity_;

    QHash<ItemId, LibraryItem> items_;
    std::vector<ItemId> initial_order_;  // library order of the items loaded at construction
    std::deque<PendingImport> queue_;
    bool running_ = false;
    app::TaskHandle running_task_;
    int active_imports_ = 0;  // queued + running
    bool verifying_ = false;  // the maintenance service is checking or hashing
    int pending_operations_ = 0;  // relinks and backups not yet finished
    bool started_ = false;
    bool busy_ = false;
};

}  // namespace lucent::ui::adapters
