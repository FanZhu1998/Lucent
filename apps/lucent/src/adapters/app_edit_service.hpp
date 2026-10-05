#pragma once

// AppEditService: the shell's EditService (services/edit_service.hpp) over the application services'
// RecipeService (settings, revisions, copy and paste) and DevelopPreviewService (edit sessions:
// quick, then refined frames); docs/adr/0010.
//
// Threads. Recipe commands (set_transient, commit, apply, undo, redo, select, reset, locks, copy,
// paste), session opening and closing and the camera probe run in order on this adapter's command
// thread: a catalog write, a file probe or a session close (which waits for a running render) never
// blocks the UI thread. Gesture positions are coalesced: while one set_transient() waits for the
// command thread, a newer position replaces it. Frames are converted (8-bit pixels to QImage, the
// clipping overlay, the histogram) on this adapter's conversion thread, in delivery order, and
// posted to the UI thread. Nothing here renders: the develop service does, on the executor's
// Interactive lane.
//
// Requests. Every change of a session's settings becomes one DevelopView request: gesture positions
// unsettled (Quick frames only), everything else settled (a Final frame follows: the quick frame,
// a refine of the white balance, or the full resolution at 100 %), with the revision and transient
// generation of the RecipeState it came from (ADR 0010, "Staleness"). The request's generation is
// mapped to the token of the call, which the frame carries.
//
// Before/after. While set_comparison() wants it, an as-shot request (unsettled: the proxy corrected
// in stage two, no refine) is made once the current settings are rendered, and its frame is marked
// `before`; it stays valid for the session while the geometry shown stays the same (it is rendered
// with the current geometry, so before and after line up). A current request that supersedes it
// before it was rendered makes it come again later.
//
// Crop tool and mask overlay (ADR 0015). While set_crop_editing() holds, every request of the
// session renders the settings without their crop (EditFrame::uncropped). set_mask_overlay() adds
// the chosen mask's weight tint to each frame on the conversion thread (engine::mask_weights() at
// most 1024 pixels on the long edge, mapped through the frame's geometry), and re-sends the newest
// frame with it. Frames carry their render's geometry and mask outlines (DevelopFrame::geometry,
// ::masks) for the overlays; the geometry helpers are the engine's.
//
// Busy. is_busy() holds while commands are queued, or a session's newest settings (or its wanted
// as-shot frame) have not been rendered. The render part is also published to the Backend's
// InteractiveActivity, so Lucent thumbnail renders wait for the photo being edited.
//
// The library. Opening a session reports PreviewStarted, the first Final frame PreviewDelivered (the
// render facts for the details panel; the thumbnail is not replaced: thumbnails are rendered from
// the recipe, app_image_source.hpp), a failure before any frame PreviewFailed (RenderEventSink).
//
// 100 %. set_full_resolution(true) asks for full-resolution Final frames; 30 seconds after the view
// left 100 % the session's full-resolution stage one is released (ADR 0010, "Consequences").
//
// Auto (W8, ADR 0013). run_auto() starts the Backend's AutoService for the photo on the command
// thread (after the commands queued before it, so a gesture committed just before is the run's
// starting point); the service's progress and completion are relayed to the UI thread. The automatic
// revisions are published by the recipe service (a change of origin Auto): for a photo with an open
// session the adapter then publishes its state, history and Auto summary and renders the new
// settings, exactly as it does when an automatic batch writes a photo's revisions. The summary of the
// current revision (its analysis record) accompanies every published history.
//
// Lifetime: lives on the UI thread. The destructor closes the dispatcher, lets the commands already
// queued finish (so a commit of a pending gesture, an apply or an undo still reaches the recipes;
// nothing is rendered or opened any more meanwhile), stops the command thread, closes every session
// (waiting for running sink calls), and waits for the conversion thread; it must run on the UI
// thread (it may release the last reference to the Backend).

#include <QHash>
#include <QThreadPool>
#include <QTimer>
#include <memory>

#include "adapters/app_library_service.hpp"
#include "adapters/backend.hpp"
#include "adapters/ui_dispatcher.hpp"
#include "services/edit_service.hpp"

namespace lucent::ui::adapters {

struct EditServiceOptions {
    int release_full_resolution_ms = 30000;
};

class AppEditService final : public EditService {
    Q_OBJECT

public:
    // `events` may be empty.
    AppEditService(std::shared_ptr<Backend> backend, RenderEventSink events, EditServiceOptions options = {},
                   QObject* parent = nullptr);
    ~AppEditService() override;

    void open(ItemId item) override;
    void close(ItemId item) override;
    void set_full_resolution(ItemId item, bool full) override;
    void set_comparison(ItemId item, bool wanted) override;
    void set_clipping(bool wanted) override;
    void set_crop_editing(ItemId item, bool editing) override;
    void set_mask_overlay(ItemId item, quint32 mask_id) override;

    quint64 set_transient(ItemId item, const EditSettings& settings) override;
    quint64 commit(ItemId item) override;
    quint64 apply(ItemId item, const EditSettings& settings) override;
    quint64 set_lock(ItemId item, EditControl control, bool locked) override;
    quint64 undo(ItemId item) override;
    quint64 redo(ItemId item) override;
    quint64 select_revision(ItemId item, quint64 revision) override;
    quint64 reset_to_as_shot(ItemId item) override;

    void copy_settings(ItemId item, quint32 categories) override;
    [[nodiscard]] bool has_clipboard() const override { return has_clipboard_; }
    void paste_settings(const QList<ItemId>& targets) override;

    void run_auto(ItemId item) override;
    void cancel_auto(ItemId item) override;
    [[nodiscard]] bool is_auto_running(ItemId item = 0) const override;

    [[nodiscard]] GeometryMap map_geometry(const EditGeometry& geometry, QSize upright_size) const override;
    [[nodiscard]] bool crop_inside_image(const EditGeometry& geometry, QSize upright_size) const override;
    [[nodiscard]] EditCrop largest_inside_crop(double straighten_degrees, QSize upright_size,
                                               double aspect) const override;
    [[nodiscard]] EditGeometry turned(const EditGeometry& geometry, GeometryTurn turn) const override;
    [[nodiscard]] QList<QPolygonF> mask_outline(const EditMask& mask, QSize upright_size) const override;
    [[nodiscard]] QList<double> sample_tone_curve(const QList<QPointF>& points, int count) const override;
    [[nodiscard]] bool is_busy() const override { return busy_; }

    struct State;  // implementation detail, shared with the command and conversion threads

private:
    void set_busy(bool busy);
    void release_idle_full_resolution();
    // UI thread: the relayed Auto events.
    void note_auto_progress(const AutoProgress& progress);
    void note_auto_finished(const AutoOutcome& outcome);

    std::shared_ptr<Backend> backend_;
    std::shared_ptr<UiDispatcher> dispatcher_;
    std::unique_ptr<QThreadPool> commands_;
    std::unique_ptr<QThreadPool> conversions_;
    std::shared_ptr<State> state_;
    app::Subscription auto_subscription_;
    app::Subscription recipe_subscription_;
    QTimer release_timer_;
    QHash<ItemId, AutoProgress> auto_runs_;  // runs in progress, as last reported
    bool has_clipboard_ = false;
    bool busy_ = false;
};

}  // namespace lucent::ui::adapters
