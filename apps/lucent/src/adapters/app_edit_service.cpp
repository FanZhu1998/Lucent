#include "adapters/app_edit_service.hpp"

#include <QCoreApplication>
#include <QFileInfo>
#include <algorithm>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include <lucent/application/develop_preview.hpp>
#include <lucent/application/recipe_service.hpp>
#include <lucent/core/log.hpp>
#include <lucent/engine/develop_operations.hpp>

#include "adapters/conversions.hpp"
#include "adapters/edit_conversions.hpp"

namespace lucent::ui::adapters {
namespace {

constexpr std::string_view kLog = "ui.edit";

QString tr_text(const char* text) {
    return QCoreApplication::translate("lucent::ui::adapters", text);
}

app::ItemId app_id(ItemId item) {
    return app::ItemId{item};
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

// A user-facing sentence for a refused edit or command.
QString describe_edit_error(const Error& error) {
    if (starts_with(error.where, "app.recipe.kelvin")) {
        return tr_text("This camera's calibration does not define a colour temperature.");
    }
    switch (error.code) {
        case ErrorCode::VersionMismatch:
            return tr_text(
                "This photo's edit or its library was saved by a newer version of Lucent, so it cannot be "
                "changed here.");
        case ErrorCode::AlreadyExists:
            return tr_text("Another change was saved at the same time; the newest settings are shown.");
        case ErrorCode::InvalidArgument:
            return tr_text("Lucent did not accept this change (%1).")
                .arg(QString::fromStdString(std::string(to_string(error.code))));
        default:
            break;
    }
    return describe_error(error);
}

}  // namespace

struct AppEditService::State : std::enable_shared_from_this<State> {
    // The newest gesture position not sent yet (kept across close(): a commit queued before the
    // close still commits it).
    struct PendingTransient {
        quint64 token = 0;
        EditSettings settings;
        std::optional<engine::DevelopSettings> base;  // the session's committed settings, when open
        bool queued = false;  // a flush is queued on the command thread
    };

    // A develop request of a session.
    struct Request {
        quint64 token = 0;
        bool uncropped = false;  // rendered without the crop (the crop tool is open)
    };

    struct Session {
        quint64 id = 0;  // tells a reopened session of the same item from the closed one
        bool open = false;  // the develop session is open
        bool full = false;  // 100 %
        bool want_before = false;
        bool crop_editing = false;  // render without the crop (set_crop_editing)
        quint32 mask_overlay = 0;   // mask whose weights the frames show (0: none)
        engine::DevelopSettings committed;  // what to_develop_settings() builds on
        // Develop requests of the session.
        std::map<quint64, Request> tokens;  // generation -> request
        quint64 current_generation = 0;     // newest request of the photo's own settings
        bool current_settled = true;
        bool current_done = true;           // its last frame (Final, or Quick when unsettled) arrived
        quint64 before_generation = 0;      // outstanding as-shot request (0: none)
        bool before_valid = false;
        bool before_failed = false;
        // The geometry of the current request's pixels: the as-shot frame for before/after is
        // rendered with it (so the two line up), and again when it changes. Likewise the lens
        // correction (W13, ADR 0023: a distortion correction moves the content), as its operation
        // version and setting (std::nullopt: version 0, nothing corrected).
        engine::GeometrySetting shown_geometry;
        engine::GeometrySetting before_geometry;
        std::optional<engine::LensCorrectionSetting> shown_lens = engine::LensCorrectionSetting{};
        std::optional<engine::LensCorrectionSetting> before_lens = engine::LensCorrectionSetting{};
        bool reported_final = false;
        // The newest current frame, for the clipping and mask overlay toggles.
        EditFramePtr last_frame;
        std::shared_ptr<const app::ClippingMask> last_mask;
        std::shared_ptr<const engine::DevelopReport> last_report;
        // The current revision of the newest published state (0: as shot), once one was published.
        quint64 revision = 0;
        bool revision_known = false;
    };

    // An Auto run this adapter started (until it finished).
    struct AutoRun {
        std::uint64_t ticket = 0;  // 0 until AutoService::run() answered
        bool cancel = false;       // cancel_auto() before the ticket was known
    };

    State(Backend& backend, QThreadPool& command_pool, QThreadPool& conversion_pool,
          std::shared_ptr<UiDispatcher> ui, AppEditService* service, RenderEventSink sink)
        : recipes(&backend.recipes()),
          develop(&backend.develop()),
          automatic(backend.automatic()),
          library(&backend.library()),
          registry(&backend.registry()),
          activity(&backend.interactive()),
          commands(&command_pool),
          conversions(&conversion_pool),
          dispatcher(std::move(ui)),
          owner(service),
          events(std::move(sink)) {}

    // Valid while the owner lives (it waits for every task before releasing them).
    app::RecipeService* recipes;
    app::DevelopPreviewService* develop;
    app::AutoService* automatic;  // null: Auto is unavailable in this session
    app::Library* library;
    const raw::DecoderRegistry* registry;
    InteractiveActivity* activity;
    QThreadPool* commands;
    QThreadPool* conversions;
    const std::shared_ptr<UiDispatcher> dispatcher;
    AppEditService* const owner;  // used only by posted functions, on the UI thread
    const RenderEventSink events;

    std::mutex mutex;
    bool stopped = false;
    // Shutting down: queued commands still run (so edits and commits reach the recipes), but no
    // develop session is opened and nothing is rendered any more.
    bool draining = false;
    bool clipping = false;
    quint64 next_token = 0;
    quint64 next_session = 1;
    quint64 next_sequence = 0;
    int pending_commands = 0;
    bool posted_busy = false;
    bool render_busy = false;
    std::optional<app::RecipeClipboard> clipboard;
    std::unordered_map<ItemId, Session> sessions;
    std::unordered_map<ItemId, PendingTransient> transients;
    std::unordered_map<ItemId, AutoRun> auto_runs;

    // ---- Helpers --------------------------------------------------------------------------------

    Session* find_locked(ItemId item, quint64 id = 0) {
        const auto found = sessions.find(item);
        if (found == sessions.end() || (id != 0 && found->second.id != id)) {
            return nullptr;
        }
        return &found->second;
    }

    quint64 new_token() {
        const std::lock_guard lock(mutex);
        return ++next_token;
    }

    // Publishes the busy state (UI thread) and the render activity (thumbnails). Requires `mutex`;
    // posting under it keeps the posts in the order of the states.
    void update_busy_locked() {
        bool render = false;
        for (const auto& [item, s] : sessions) {
            if (s.open && (!s.current_done || s.before_generation != 0)) {
                render = true;
                break;
            }
        }
        const bool busy = render || pending_commands > 0;
        if (render != render_busy) {
            render_busy = render;
            activity->set_busy(render);
        }
        if (busy != posted_busy) {
            posted_busy = busy;
            dispatcher->post([owner = owner, busy] { owner->set_busy(busy); });
        }
    }

    void post_state(const EditState& state) {
        dispatcher->post([owner = owner, state] { emit owner->state_changed(state); });
    }

    void post_frame_failed(ItemId item, const QString& message) {
        dispatcher->post([owner = owner, item, message] { emit owner->frame_failed(item, message); });
    }

    void post_command_failed(ItemId item, const QString& message) {
        dispatcher->post([owner = owner, item, message] { emit owner->command_failed(item, message); });
    }

    void post_history(ItemId item) {
        auto history = recipes->history(app_id(item));
        if (!history) {
            core::log_warn(kLog, "the history of item {} cannot be listed: {}", item, history.error().describe());
            return;
        }
        QList<EditHistoryEntry> entries = to_edit_history(*history);
        AutoSummary summary = auto_summary(item, *history);
        dispatcher->post([owner = owner, item, entries, summary] {
            emit owner->history_changed(item, entries);
            emit owner->auto_summary_changed(item, summary);
        });
    }

    // What automatic settings did to the current revision (empty unless it is an automatic one).
    AutoSummary auto_summary(ItemId item, const app::RecipeHistory& history) {
        for (const app::HistoryEntry& entry : history.entries) {
            if (!entry.current || entry.info.origin != catalog::RevisionOrigin::Auto) {
                continue;
            }
            auto records = recipes->automatic_analyses(app_id(item));
            if (!records) {
                core::log_warn(kLog, "the analysis records of item {} cannot be read: {}", item,
                               records.error().describe());
                return {};
            }
            for (const catalog::AutoAnalysisRecord& record : *records) {
                if (record.revision == entry.info.id) {
                    return to_auto_summary(record, entry);
                }
            }
            return {};  // an automatic revision without a record (written by a newer Lucent)
        }
        return {};
    }

    // ---- Auto -------------------------------------------------------------------------------------

    void post_auto_finished(const AutoOutcome& outcome) {
        dispatcher->post([owner = owner, outcome] { owner->note_auto_finished(outcome); });
    }

    // Command thread: starts the run run_auto() registered.
    void start_auto(ItemId item) {
        bool cancelled = false;
        {
            const std::lock_guard lock(mutex);
            const auto found = auto_runs.find(item);
            if (found == auto_runs.end()) {
                return;
            }
            // Shutting down: a run queued behind the last commands is not started any more.
            cancelled = found->second.cancel || draining;
        }
        std::optional<Error> refused;
        std::uint64_t ticket = 0;
        if (automatic == nullptr) {
            refused = Error{ErrorCode::Unsupported, "automatic settings are unavailable", "ui.auto.unavailable", 0};
        } else if (cancelled) {
            refused = Error{ErrorCode::Cancelled, "cancelled before it started", "ui.auto", 0};
        } else if (auto started = automatic->run(app_id(item)); started) {
            ticket = *started;
        } else {
            refused = started.error();
        }
        if (refused) {
            if (refused->code != ErrorCode::Cancelled) {
                core::log_warn(kLog, "Auto of item {} did not start: {}", item, refused->describe());
            }
            {
                const std::lock_guard lock(mutex);
                auto_runs.erase(item);
            }
            AutoOutcome outcome;
            outcome.item = item;
            outcome.cancelled = refused->code == ErrorCode::Cancelled;
            outcome.message = describe_auto_error(*refused);
            post_auto_finished(outcome);
            return;
        }
        bool cancel_now = false;
        {
            const std::lock_guard lock(mutex);
            // Gone already when the run finished before this (its event names the item).
            if (const auto found = auto_runs.find(item); found != auto_runs.end() && found->second.ticket == 0) {
                found->second.ticket = ticket;
                cancel_now = found->second.cancel;
            }
        }
        if (cancel_now) {
            automatic->cancel(ticket);
        }
    }

    // An executor thread (AutoService observer): relays the progress and the end of a run.
    void on_auto_event(const app::AutoEvent& event) {
        const ItemId item = event.status.item.value;
        {
            const std::lock_guard lock(mutex);
            if (stopped) {
                return;
            }
            const auto found = auto_runs.find(item);
            if (found == auto_runs.end() ||
                (found->second.ticket != 0 && found->second.ticket != event.status.ticket)) {
                return;  // not a run of this adapter
            }
            if (event.kind == app::AutoEvent::Kind::Finished) {
                auto_runs.erase(found);
            }
        }
        if (event.kind == app::AutoEvent::Kind::Progress) {
            const AutoProgress progress{item, to_auto_stage(event.status.stage),
                                        std::clamp(event.status.fraction, 0.0, 1.0)};
            dispatcher->post([owner = owner, progress] { owner->note_auto_progress(progress); });
            return;
        }
        AutoOutcome outcome;
        if (event.result) {
            outcome = to_auto_outcome(item, *event.result);
        } else {
            outcome.item = item;
            outcome.message = describe_auto_error(Error{ErrorCode::Internal, "no result", "ui.auto", 0});
        }
        post_auto_finished(outcome);
    }

    // Command thread: an automatic revision was written for `item` (the editor's Auto, or an automatic
    // batch): its open session shows the item's state, history and summary, and renders the new
    // settings when the current revision changed.
    void refresh_after_auto(ItemId item) {
        auto state = recipes->state(app_id(item));
        bool request = true;
        {
            const std::lock_guard lock(mutex);
            const Session* s = find_locked(item);
            if (s == nullptr) {
                return;
            }
            if (state && !state->transient && s->revision_known) {
                const quint64 revision = state->revision ? static_cast<quint64>(state->revision->value) : 0;
                request = revision != s->revision;  // a branch only: nothing new to show
            }
        }
        finish(item, 0, state, request, true);
    }

    // Runs `body` on the command thread, in call order.
    void queue(std::function<void(State&)> body) {
        {
            const std::lock_guard lock(mutex);
            if (stopped) {
                return;
            }
            ++pending_commands;
            update_busy_locked();
        }
        commands->start([weak = weak_from_this(), body = std::move(body)] {
            const std::shared_ptr<State> self = weak.lock();
            if (!self) {
                return;
            }
            bool run = true;
            {
                const std::lock_guard lock(self->mutex);
                run = !self->stopped;
            }
            if (run) {
                try {
                    body(*self);
                } catch (const std::exception& e) {
                    core::log_error(kLog, "an edit command failed: {}", e.what());
                }
            }
            const std::lock_guard lock(self->mutex);
            --self->pending_commands;
            if (!self->stopped) {
                self->update_busy_locked();
            }
        });
    }

    // The settings the editor's values are laid over (base look, operation versions).
    engine::DevelopSettings base_settings(ItemId item) {
        {
            const std::lock_guard lock(mutex);
            if (const Session* s = find_locked(item)) {
                return s->committed;
            }
        }
        if (auto state = recipes->state(app_id(item)); state && !state->recipe_unreadable) {
            return state->committed;
        }
        return engine::DevelopSettings{};
    }

    // ---- Requests (require `mutex`) -----------------------------------------------------------------

    void request_locked(ItemId item, Session& s, const app::RecipeState& state, quint64 token) {
        if (!s.open || draining) {
            return;
        }
        if (state.recipe_unreadable) {
            s.current_done = true;
            return;
        }
        app::DevelopView view;
        view.settings = state.settings;
        if (s.crop_editing) {
            // The crop tool shows the whole straightened image under its overlay.
            view.settings.geometry.crop = engine::CropRect{};
        }
        view.revision = state.transient ? std::nullopt : state.revision;
        view.transient_generation = state.transient ? state.transient_generation : 0;
        view.settled = !state.transient;
        view.quality = s.full ? app::PreviewQuality::full() : app::PreviewQuality::fit(app::kDefaultProxyLongEdge);
        auto generation = develop->request(app_id(item), view);
        if (!generation) {
            s.current_done = true;
            post_frame_failed(item, describe_error(generation.error()));
            update_busy_locked();
            return;
        }
        s.tokens[*generation] = Request{token, s.crop_editing};
        s.current_generation = *generation;
        s.current_settled = view.settled;
        s.current_done = false;
        s.shown_geometry = view.settings.geometry;
        s.shown_lens = engine::effective_lens_correction(view.settings);
        if (s.before_valid && (!(s.before_geometry == s.shown_geometry) || s.before_lens != s.shown_lens)) {
            s.before_valid = false;  // rendered again once the current settings are
        }
        update_busy_locked();
    }

    // The as-shot frame for before/after, once the current settings are rendered.
    void maybe_request_before_locked(ItemId item, Session& s) {
        if (!s.open || draining || !s.want_before || s.before_valid || s.before_failed ||
            s.before_generation != 0 || !s.current_done) {
            return;
        }
        // As shot, with the geometry and the lens correction shown (so before and after line up).
        app::DevelopView view;
        view.settings.geometry = s.shown_geometry;
        if (s.shown_lens) {
            view.settings.lens_correction = *s.shown_lens;
        } else {
            view.settings.operations.lens_correction = engine::kOldestLensCorrectionOperationVersion;
        }
        view.settled = false;  // the proxy corrected in stage two: no refine
        view.quality = app::PreviewQuality::fit(app::kDefaultProxyLongEdge);
        auto generation = develop->request(app_id(item), view);
        if (!generation) {
            s.before_failed = true;
            return;
        }
        s.before_generation = *generation;
        s.before_geometry = s.shown_geometry;
        s.before_lens = s.shown_lens;
        s.tokens[*generation] = Request{0, false};
        update_busy_locked();
    }

    // Publishes the result of a recipe command: the state, the new view's request, the history.
    void finish(ItemId item, quint64 token, const Expected<app::RecipeState>& result, bool request, bool history) {
        if (!result) {
            core::log_warn(kLog, "an edit of item {} was refused: {}", item, result.error().describe());
            post_command_failed(item, describe_edit_error(result.error()));
            // The view goes back to what the service holds.
            auto state = recipes->state(app_id(item));
            if (state) {
                EditState ui = to_edit_state(*state, recipes->read_only_reason().has_value());
                ui.token = token;
                post_state(ui);
            } else if (request) {
                // Nothing can be rendered for the photo: end the view's wait with the reason instead
                // of leaving it on its placeholder.
                const std::lock_guard lock(mutex);
                if (Session* s = find_locked(item); s != nullptr && s->open) {
                    s->current_done = true;
                    post_frame_failed(item, describe_edit_error(state.error()));
                    update_busy_locked();
                }
            }
            return;
        }
        EditState ui = to_edit_state(*result, recipes->read_only_reason().has_value());
        ui.token = token;
        {
            const std::lock_guard lock(mutex);
            if (Session* s = find_locked(item)) {
                if (!result->recipe_unreadable) {
                    s->committed = result->committed;
                }
                s->revision = result->revision ? static_cast<quint64>(result->revision->value) : 0;
                s->revision_known = true;
                if (request) {
                    if (result->recipe_unreadable && s->open) {
                        s->current_done = true;
                        post_frame_failed(item, ui.read_only_reason);
                        update_busy_locked();
                    } else {
                        request_locked(item, *s, *result, token);
                    }
                }
            }
        }
        post_state(ui);
        if (history) {
            post_history(item);
        }
    }

    // ---- Commands (command thread) ----------------------------------------------------------------

    void open_session(ItemId item, quint64 id) {
        {
            const std::lock_guard lock(mutex);
            if (draining) {
                return;  // shutting down: nothing to show any more
            }
        }
        auto sink = [weak = weak_from_this(), item, id](const app::DevelopEvent& event) {
            if (auto self = weak.lock()) {
                self->on_event(item, id, event);
            }
        };
        Status opened = develop->open(app_id(item), sink);
        if (!opened && opened.error().code == ErrorCode::AlreadyExists) {
            develop->close(app_id(item));  // a session left from before: start afresh
            opened = develop->open(app_id(item), sink);
        }
        if (!opened) {
            const QString message = describe_error(opened.error());
            post_frame_failed(item, message);
            if (events) {
                events(RenderEvent{RenderEvent::Kind::PreviewFailed, item, message});
            }
        } else {
            const std::lock_guard lock(mutex);
            if (Session* s = find_locked(item, id)) {
                s->open = true;
            }
            if (events) {
                events(RenderEvent{RenderEvent::Kind::PreviewStarted, item});
            }
        }
        finish(item, 0, recipes->state(app_id(item)), true, true);

        WhiteBalanceInfo info = unknown_white_balance_info();
        if (std::optional<app::SourceItem> source = library->find(app_id(item)); source) {
            raw::ProbeOptions probe;
            probe.subimage = source->subimage;
            probe.describe_subimages = false;
            auto selection = registry->select(source->path, probe, {});
            if (selection && selection->probe.metadata.source.identity == source->identity) {
                info = white_balance_info(selection->probe.metadata);
            }
        }
        dispatcher->post([owner = owner, item, info] { emit owner->white_balance_changed(item, info); });
    }

    void flush_transient(ItemId item) {
        PendingTransient pending;
        {
            const std::lock_guard lock(mutex);
            const auto found = transients.find(item);
            if (found == transients.end()) {
                return;
            }
            pending = std::move(found->second);
            transients.erase(found);
        }
        const engine::DevelopSettings base = pending.base ? *pending.base : base_settings(item);
        finish(item, pending.token,
               recipes->set_transient(app_id(item), to_develop_settings(pending.settings, base)), true, false);
    }

    void paste(const QList<ItemId>& targets) {
        std::optional<app::RecipeClipboard> copied;
        {
            const std::lock_guard lock(mutex);
            copied = clipboard;
        }
        PasteReport report;
        report.targets = static_cast<int>(targets.size());
        const auto publish = [this](const PasteReport& done) {
            dispatcher->post([owner = owner, done] { emit owner->pasted(done); });
        };
        if (!copied) {
            report.errors << tr_text("Nothing has been copied yet.");
            publish(report);
            return;
        }
        std::vector<app::ItemId> ids;
        for (const ItemId target : targets) {
            ids.push_back(app_id(target));
        }
        auto outcomes = recipes->paste(*copied, ids);
        if (!outcomes) {
            report.errors << describe_edit_error(outcomes.error());
            publish(report);
            return;
        }
        std::set<SettingsCategory> applied;
        for (const app::PasteOutcome& outcome : *outcomes) {
            QString name;
            if (std::optional<app::SourceItem> source = library->find(outcome.item); source) {
                name = QFileInfo(to_qstring(source->path)).fileName();
            }
            if (outcome.error) {
                report.errors << (name.isEmpty() ? describe_edit_error(*outcome.error)
                                                 : tr_text("%1: %2").arg(name, describe_edit_error(*outcome.error)));
                continue;
            }
            for (const app::RecipeCategory category : outcome.applied) {
                applied.insert(to_settings_category(category));
            }
            if (!outcome.skipped.empty()) {
                std::optional<app::CameraCapabilities> camera;
                if (auto found = recipes->camera(outcome.item); found) {
                    camera = std::move(*found);
                }
                for (const app::PasteSkip& skip : outcome.skipped) {
                    QString text = describe_paste_skip(skip, *copied, camera);
                    if (targets.size() > 1 && !name.isEmpty()) {
                        text = tr_text("%1 — %2").arg(name, text);
                    }
                    if (!report.skipped.contains(text)) {
                        report.skipped << text;
                    }
                }
            }
            if (outcome.revision) {
                ++report.changed;
                bool open = false;
                {
                    const std::lock_guard lock(mutex);
                    open = find_locked(outcome.item.value) != nullptr;
                }
                if (open) {
                    finish(outcome.item.value, 0, recipes->state(outcome.item), true, true);
                }
            }
        }
        report.applied.assign(applied.begin(), applied.end());
        publish(report);
    }

    // ---- Frames -----------------------------------------------------------------------------------

    // Develop sink: an executor worker, one call at a time per session.
    void on_event(ItemId item, quint64 id, const app::DevelopEvent& event) {
        const std::lock_guard lock(mutex);
        if (stopped) {
            return;
        }
        Session* s = find_locked(item, id);
        if (s == nullptr) {
            return;
        }
        const quint64 generation = event.generation;
        Request request;
        if (const auto found = s->tokens.find(generation); found != s->tokens.end()) {
            request = found->second;
        }
        const quint64 token = request.token;
        s->tokens.erase(s->tokens.begin(), s->tokens.lower_bound(generation));
        const bool before = s->before_generation != 0 && generation == s->before_generation;

        if (event.kind == app::DevelopEvent::Kind::Failed) {
            const QString message =
                event.error ? describe_error(*event.error) : tr_text("Lucent could not render this photo.");
            if (before) {
                s->before_generation = 0;
                s->before_failed = true;
            } else {
                if (generation >= s->current_generation) {
                    s->current_done = true;
                }
                post_frame_failed(item, message);
                if (!s->reported_final && events) {
                    events(RenderEvent{RenderEvent::Kind::PreviewFailed, item, message});
                }
            }
            update_busy_locked();
            return;
        }
        std::shared_ptr<const app::DevelopFrame> frame = event.frame;
        if (!frame) {
            return;
        }
        if (before) {
            s->before_generation = 0;
            s->before_valid = true;
        } else {
            if (s->before_generation != 0 && generation > s->before_generation) {
                s->before_generation = 0;  // replaced before it was rendered: asked for again later
            }
            if (generation >= s->current_generation &&
                (frame->phase == app::DevelopPhase::Final || !s->current_settled)) {
                s->current_done = true;
            }
        }
        const bool report_final = !before && frame->phase == app::DevelopPhase::Final && !s->reported_final;
        if (report_final) {
            s->reported_final = true;
        }
        const bool overlay = clipping && !before;
        const quint32 mask_overlay = before ? 0 : s->mask_overlay;
        const bool uncropped = request.uncropped;
        maybe_request_before_locked(item, *s);
        update_busy_locked();
        conversions->start([weak = weak_from_this(), item, id, frame = std::move(frame), token, before, overlay,
                            mask_overlay, uncropped, report_final] {
            if (auto self = weak.lock()) {
                self->convert(item, id, *frame, token, before, overlay, mask_overlay, uncropped, report_final);
            }
        });
    }

    // The weight overlay of mask `mask_id` for a frame rendered as `report` says, through the mapping
    // stage two rendered it with (engine::geometry_mapping() of the report: the frame F is the
    // camera's area for a geometry-version-2 recipe, QUA-05, and the lens correction's point mapping
    // keeps the weights on their content, ADR 0023); null when the settings have no such mask.
    static QImage mask_overlay_for(const engine::DevelopReport& report, quint32 mask_id) {
        for (const engine::LocalAdjustment& adjustment : report.settings.local_adjustments) {
            if (adjustment.id != mask_id) {
                continue;
            }
            auto mapping = engine::geometry_mapping(report);
            return mapping ? mask_overlay_image(adjustment, *mapping) : QImage();
        }
        return {};
    }

    // Conversion thread, in delivery order.
    void convert(ItemId item, quint64 id, const app::DevelopFrame& frame, quint64 token, bool before, bool overlay,
                 quint32 mask_overlay, bool uncropped, bool report_final) {
        std::shared_ptr<EditFrame> out = to_edit_frame(frame, overlay);
        if (out) {
            out->uncropped = uncropped;
            if (mask_overlay != 0 && frame.report) {
                out->mask_overlay = mask_overlay_for(*frame.report, mask_overlay);
                out->mask_overlay_id = out->mask_overlay.isNull() ? 0 : mask_overlay;
            }
        }
        std::shared_ptr<const app::ClippingMask> mask;
        if (out && !before) {
            try {
                mask = std::make_shared<const app::ClippingMask>(frame.clipping);
            } catch (const std::bad_alloc&) {
                // Only the later overlay toggle needs it.
            }
        }
        const std::lock_guard lock(mutex);
        if (stopped) {
            return;
        }
        Session* s = find_locked(item, id);
        if (s == nullptr) {
            return;
        }
        if (!out) {
            post_frame_failed(item, describe_error(Error{ErrorCode::OutOfMemory, {}, {}, 0}));
            return;
        }
        out->token = token;
        out->before = before;
        out->sequence = ++next_sequence;
        if (!before) {
            s->last_frame = out;
            s->last_mask = std::move(mask);
            s->last_report = frame.report;
        }
        EditFramePtr published = out;
        dispatcher->post([owner = owner, published] { emit owner->frame_ready(published); });
        if (report_final && events) {
            RenderEvent delivered{RenderEvent::Kind::PreviewDelivered, item};
            delivered.render = out->info;
            delivered.primary = true;
            delivered.lucent_thumbnail = false;  // thumbnails come from the recipe's own render
            events(std::move(delivered));
        }
    }

    // Conversion thread: the newest frame of `item` again, with its clipping overlay.
    void add_overlay(ItemId item, quint64 id) {
        EditFramePtr frame;
        std::shared_ptr<const app::ClippingMask> mask;
        {
            const std::lock_guard lock(mutex);
            const Session* s = find_locked(item, id);
            if (s == nullptr || !clipping || !s->last_frame || !s->last_frame->clipping.isNull() || !s->last_mask) {
                return;
            }
            frame = s->last_frame;
            mask = s->last_mask;
        }
        auto out = std::make_shared<EditFrame>(*frame);
        out->clipping = clipping_overlay(*mask);
        if (out->clipping.isNull()) {
            return;
        }
        const std::lock_guard lock(mutex);
        Session* s = find_locked(item, id);
        if (stopped || s == nullptr || s->last_frame != frame) {
            return;  // a newer frame was published meanwhile (with its own overlay)
        }
        out->sequence = ++next_sequence;
        s->last_frame = out;
        EditFramePtr published = out;
        dispatcher->post([owner = owner, published] { emit owner->frame_ready(published); });
    }

    // Conversion thread: the newest frame of `item` again, with the weight overlay of the session's
    // chosen mask.
    void add_mask_overlay(ItemId item, quint64 id) {
        EditFramePtr frame;
        std::shared_ptr<const engine::DevelopReport> report;
        quint32 mask = 0;
        {
            const std::lock_guard lock(mutex);
            const Session* s = find_locked(item, id);
            if (s == nullptr || s->mask_overlay == 0 || !s->last_frame || !s->last_report ||
                s->last_frame->mask_overlay_id == s->mask_overlay) {
                return;
            }
            frame = s->last_frame;
            report = s->last_report;
            mask = s->mask_overlay;
        }
        auto out = std::make_shared<EditFrame>(*frame);
        out->mask_overlay = mask_overlay_for(*report, mask);
        out->mask_overlay_id = out->mask_overlay.isNull() ? 0 : mask;
        if (out->mask_overlay.isNull()) {
            return;
        }
        const std::lock_guard lock(mutex);
        Session* s = find_locked(item, id);
        if (stopped || s == nullptr || s->last_frame != frame) {
            return;  // a newer frame was published meanwhile (with its own overlay)
        }
        out->sequence = ++next_sequence;
        s->last_frame = out;
        EditFramePtr published = out;
        dispatcher->post([owner = owner, published] { emit owner->frame_ready(published); });
    }
};

AppEditService::AppEditService(std::shared_ptr<Backend> backend, RenderEventSink events, EditServiceOptions options,
                               QObject* parent)
    : EditService(parent),
      backend_(std::move(backend)),
      dispatcher_(std::make_shared<UiDispatcher>(this)),
      commands_(std::make_unique<QThreadPool>()),
      conversions_(std::make_unique<QThreadPool>()) {
    commands_->setObjectName(QStringLiteral("lucent-edit-commands"));
    commands_->setMaxThreadCount(1);  // in call order
    commands_->setExpiryTimeout(-1);
    conversions_->setObjectName(QStringLiteral("lucent-edit-frames"));
    conversions_->setMaxThreadCount(1);  // in delivery order
    conversions_->setExpiryTimeout(-1);
    state_ = std::make_shared<State>(*backend_, *commands_, *conversions_, dispatcher_, this, std::move(events));
    release_timer_.setSingleShot(true);
    release_timer_.setInterval(std::max(0, options.release_full_resolution_ms));
    connect(&release_timer_, &QTimer::timeout, this, &AppEditService::release_idle_full_resolution);
    // W12 (APL-06): the lens database loads off the UI thread; the Lens corrections panel is told when
    // it has (`this` is used only by the posted function, on the UI thread while it lives). The listener
    // ends with its subscription member, also when a statement below throws.
    lens_database_subscription_ = backend_->lens_database_source()->on_ready(
        [this, dispatcher = dispatcher_] { dispatcher->post([this] { emit lens_database_changed(); }); });

    // Auto's progress and completion (an executor thread; relayed to the UI thread).
    if (app::AutoService* automatic = backend_->automatic(); automatic != nullptr) {
        auto_subscription_ = automatic->subscribe([weak = std::weak_ptr<State>(state_)](const app::AutoEvent& event) {
            if (auto self = weak.lock()) {
                self->on_auto_event(event);
            }
        });
    }
    // Automatic revisions written for a photo being edited (by Auto or an automatic batch): runs on
    // the thread that wrote them; the session's update is queued on the command thread.
    recipe_subscription_ =
        backend_->recipes().subscribe([weak = std::weak_ptr<State>(state_)](const app::RecipeChange& change) {
            if (change.kind != app::RecipeChangeKind::Committed || change.origin != catalog::RevisionOrigin::Auto) {
                return;
            }
            auto self = weak.lock();
            if (!self) {
                return;
            }
            const ItemId item = change.state.item.value;
            {
                const std::lock_guard lock(self->mutex);
                if (self->stopped || self->draining || self->find_locked(item) == nullptr) {
                    return;
                }
            }
            self->queue([item](State& s) { s.refresh_after_auto(item); });
        });
}

AppEditService::~AppEditService() {
    // No relayed event or queued refresh after this point (each reset waits for a call in progress).
    lens_database_subscription_.reset();
    recipe_subscription_.reset();
    auto_subscription_.reset();
    std::vector<std::uint64_t> tickets;
    {
        const std::lock_guard lock(state_->mutex);
        for (const auto& [item, run] : state_->auto_runs) {
            if (run.ticket != 0) {
                tickets.push_back(run.ticket);
            }
        }
    }
    if (app::AutoService* automatic = backend_->automatic(); automatic != nullptr) {
        for (const std::uint64_t ticket : tickets) {
            automatic->cancel(ticket);  // the Backend waits for them when it closes
        }
    }
    dispatcher_->close();
    release_timer_.stop();
    // Queued commands still run first: a commit of a pending gesture (EditSession::set_item(0) from
    // the controller's destructor), an apply or an undo waiting behind a slower command must reach
    // the recipes. While draining nothing new is rendered and nothing is posted (the dispatcher is
    // closed).
    {
        const std::lock_guard lock(state_->mutex);
        state_->draining = true;
    }
    commands_->waitForDone();
    std::vector<ItemId> items;
    {
        const std::lock_guard lock(state_->mutex);
        state_->stopped = true;
        for (const auto& [item, session] : state_->sessions) {
            items.push_back(item);
        }
        state_->sessions.clear();
    }
    commands_->waitForDone();  // commands queued meanwhile see `stopped` and do nothing
    for (const ItemId item : items) {
        backend_->develop().close(app_id(item));  // waits for a running sink call
    }
    conversions_->waitForDone();
    backend_->interactive().set_busy(false);
    state_.reset();
    conversions_.reset();
    commands_.reset();
    backend_.reset();  // possibly the last reference: joins the executor (see backend.hpp)
}

void AppEditService::set_busy(bool busy) {
    if (busy != busy_) {
        busy_ = busy;
        emit busy_changed(busy);
    }
}

void AppEditService::open(ItemId item) {
    if (item == 0) {
        return;
    }
    quint64 id = 0;
    {
        const std::lock_guard lock(state_->mutex);
        if (state_->stopped || state_->sessions.contains(item)) {
            return;
        }
        State::Session& session = state_->sessions[item];
        session.id = state_->next_session++;
        id = session.id;
    }
    state_->queue([item, id](State& self) { self.open_session(item, id); });
}

void AppEditService::close(ItemId item) {
    {
        const std::lock_guard lock(state_->mutex);
        const auto found = state_->sessions.find(item);
        if (found == state_->sessions.end()) {
            return;
        }
        state_->sessions.erase(found);
        state_->update_busy_locked();
    }
    state_->queue([item](State& self) {
        self.develop->close(app_id(item));  // waits for a running render's sink call
        // A gesture never committed is not kept for the next session.
        static_cast<void>(self.recipes->discard_transient(app_id(item)));
    });
}

void AppEditService::set_full_resolution(ItemId item, bool full) {
    {
        const std::lock_guard lock(state_->mutex);
        State::Session* s = state_->find_locked(item);
        if (s == nullptr || s->full == full) {
            return;
        }
        s->full = full;
    }
    if (full) {
        state_->queue([item](State& self) { self.finish(item, 0, self.recipes->state(app_id(item)), true, false); });
    } else {
        release_timer_.start();
    }
}

void AppEditService::release_idle_full_resolution() {
    std::vector<ItemId> items;
    {
        const std::lock_guard lock(state_->mutex);
        for (const auto& [item, s] : state_->sessions) {
            if (s.open && !s.full) {
                items.push_back(item);
            }
        }
    }
    for (const ItemId item : items) {
        state_->queue([item](State& self) {
            bool full = true;
            {
                const std::lock_guard lock(self.mutex);
                const State::Session* s = self.find_locked(item);
                full = s == nullptr || s->full;
            }
            if (!full) {
                self.develop->release_full_resolution(app_id(item));
            }
        });
    }
}

void AppEditService::set_comparison(ItemId item, bool wanted) {
    const std::lock_guard lock(state_->mutex);
    State::Session* s = state_->find_locked(item);
    if (s == nullptr) {
        return;
    }
    s->want_before = wanted;
    if (wanted) {
        s->before_failed = false;
        state_->maybe_request_before_locked(item, *s);
    }
    state_->update_busy_locked();
}

void AppEditService::set_clipping(bool wanted) {
    std::vector<std::pair<ItemId, quint64>> sessions;
    {
        const std::lock_guard lock(state_->mutex);
        state_->clipping = wanted;
        if (!wanted) {
            return;
        }
        for (const auto& [item, s] : state_->sessions) {
            if (s.last_frame && s.last_frame->clipping.isNull()) {
                sessions.emplace_back(item, s.id);
            }
        }
    }
    for (const auto& [item, id] : sessions) {
        conversions_->start([weak = std::weak_ptr<State>(state_), item, id] {
            if (auto self = weak.lock()) {
                self->add_overlay(item, id);
            }
        });
    }
}

void AppEditService::set_crop_editing(ItemId item, bool editing) {
    {
        const std::lock_guard lock(state_->mutex);
        State::Session* s = state_->find_locked(item);
        if (s == nullptr || s->crop_editing == editing) {
            return;
        }
        s->crop_editing = editing;
    }
    // The settings shown again, with or without their crop.
    state_->queue([item](State& self) { self.finish(item, 0, self.recipes->state(app_id(item)), true, false); });
}

void AppEditService::set_mask_overlay(ItemId item, quint32 mask_id) {
    quint64 id = 0;
    {
        const std::lock_guard lock(state_->mutex);
        State::Session* s = state_->find_locked(item);
        if (s == nullptr || s->mask_overlay == mask_id) {
            return;
        }
        s->mask_overlay = mask_id;
        if (mask_id == 0) {
            return;
        }
        id = s->id;
    }
    conversions_->start([weak = std::weak_ptr<State>(state_), item, id] {
        if (auto self = weak.lock()) {
            self->add_mask_overlay(item, id);
        }
    });
}

quint64 AppEditService::set_transient(ItemId item, const EditSettings& settings) {
    const quint64 token = state_->new_token();
    bool queue = false;
    {
        const std::lock_guard lock(state_->mutex);
        State::PendingTransient& pending = state_->transients[item];
        pending.token = token;
        pending.settings = settings;
        const State::Session* s = state_->find_locked(item);
        pending.base = s != nullptr ? std::optional<engine::DevelopSettings>(s->committed) : std::nullopt;
        if (!pending.queued) {
            pending.queued = true;
            queue = true;
        }
    }
    if (queue) {
        state_->queue([item](State& self) { self.flush_transient(item); });
    }
    return token;
}

quint64 AppEditService::commit(ItemId item) {
    const quint64 token = state_->new_token();
    state_->queue([item, token](State& self) {
        self.flush_transient(item);
        self.finish(item, token, self.recipes->commit(app_id(item)), true, true);
    });
    return token;
}

quint64 AppEditService::apply(ItemId item, const EditSettings& settings) {
    const quint64 token = state_->new_token();
    state_->queue([item, token, settings](State& self) {
        self.flush_transient(item);
        const engine::DevelopSettings base = self.base_settings(item);
        self.finish(item, token, self.recipes->apply(app_id(item), to_develop_settings(settings, base)), true, true);
    });
    return token;
}

quint64 AppEditService::set_lock(ItemId item, EditControl control, bool locked) {
    const quint64 token = state_->new_token();
    state_->queue([item, token, control, locked](State& self) {
        // Locks do not change pixels: no new frame.
        self.finish(item, token, self.recipes->set_lock(app_id(item), to_develop_control(control), locked), false,
                    true);
    });
    return token;
}

quint64 AppEditService::undo(ItemId item) {
    const quint64 token = state_->new_token();
    state_->queue([item, token](State& self) { self.finish(item, token, self.recipes->undo(app_id(item)), true, true); });
    return token;
}

quint64 AppEditService::redo(ItemId item) {
    const quint64 token = state_->new_token();
    state_->queue([item, token](State& self) { self.finish(item, token, self.recipes->redo(app_id(item)), true, true); });
    return token;
}

quint64 AppEditService::select_revision(ItemId item, quint64 revision) {
    const quint64 token = state_->new_token();
    state_->queue([item, token, revision](State& self) {
        const std::optional<catalog::RevisionId> target =
            revision == 0 ? std::nullopt
                          : std::optional<catalog::RevisionId>(catalog::RevisionId{static_cast<std::int64_t>(revision)});
        self.finish(item, token, self.recipes->select_revision(app_id(item), target), true, true);
    });
    return token;
}

quint64 AppEditService::reset_to_as_shot(ItemId item) {
    const quint64 token = state_->new_token();
    state_->queue(
        [item, token](State& self) { self.finish(item, token, self.recipes->reset_to_as_shot(app_id(item)), true, true); });
    return token;
}

void AppEditService::copy_settings(ItemId item, quint32 categories) {
    state_->queue([item, categories](State& self) {
        auto copied = self.recipes->copy(app_id(item), to_recipe_categories(categories));
        if (!copied) {
            self.post_command_failed(item, describe_edit_error(copied.error()));
            return;
        }
        {
            const std::lock_guard lock(self.mutex);
            self.clipboard = std::move(*copied);
        }
        self.dispatcher->post([owner = self.owner, item, categories] {
            owner->has_clipboard_ = true;
            emit owner->copied(item, categories);
        });
    });
}

void AppEditService::paste_settings(const QList<ItemId>& targets) {
    state_->queue([targets](State& self) { self.paste(targets); });
}

void AppEditService::run_auto(ItemId item) {
    if (item == 0) {
        return;
    }
    {
        const std::lock_guard lock(state_->mutex);
        if (state_->stopped || state_->auto_runs.contains(item)) {
            return;  // one run per photo at a time
        }
        state_->auto_runs[item] = State::AutoRun{};
    }
    const AutoProgress queued{item, AutoStage::Queued, 0.0};
    auto_runs_.insert(item, queued);
    // Signals never come from inside the call that caused them (edit_service.hpp).
    dispatcher_->post([this, queued] {
        if (auto_runs_.contains(queued.item)) {
            emit auto_progress(queued);
        }
    });
    // After the commands queued before it: a gesture committed just before is the run's start.
    state_->queue([item](State& self) { self.start_auto(item); });
}

void AppEditService::cancel_auto(ItemId item) {
    std::uint64_t ticket = 0;
    {
        const std::lock_guard lock(state_->mutex);
        const auto found = state_->auto_runs.find(item);
        if (found == state_->auto_runs.end()) {
            return;
        }
        found->second.cancel = true;
        ticket = found->second.ticket;
    }
    if (app::AutoService* automatic = backend_->automatic(); automatic != nullptr && ticket != 0) {
        automatic->cancel(ticket);
    }
}

bool AppEditService::is_auto_running(ItemId item) const {
    return item == 0 ? !auto_runs_.isEmpty() : auto_runs_.contains(item);
}

void AppEditService::note_auto_progress(const AutoProgress& progress) {
    const auto found = auto_runs_.find(progress.item);
    if (found == auto_runs_.end() || progress.fraction < found->fraction) {
        return;  // finished already, or never backwards
    }
    *found = progress;
    emit auto_progress(progress);
}

void AppEditService::note_auto_finished(const AutoOutcome& outcome) {
    auto_runs_.remove(outcome.item);
    emit auto_finished(outcome);
}

GeometryMap AppEditService::map_geometry(const EditGeometry& geometry, QSize upright_size, QRect frame) const {
    return geometry_map(geometry, upright_size, frame);
}

bool AppEditService::crop_inside_image(const EditGeometry& geometry, QSize frame_size) const {
    return geometry_crop_inside(geometry, frame_size);
}

EditCrop AppEditService::largest_inside_crop(double straighten_degrees, QSize frame_size, double aspect) const {
    return geometry_largest_inside_crop(straighten_degrees, frame_size, aspect);
}

EditGeometry AppEditService::turned(const EditGeometry& geometry, GeometryTurn turn) const {
    return geometry_turned(geometry, turn);
}

QList<QPolygonF> AppEditService::mask_outline(const EditMask& mask, QSize upright_size) const {
    return mask_outline_of(mask, upright_size);
}

LensDatabaseInfo AppEditService::lens_database() const {
    return to_lens_database_info(backend_->lens_database_source()->try_get());  // never waits
}

QList<LensProfileEntry> AppEditService::search_lens_profiles(const QString& text, int limit) const {
    // Nothing while the database loads (the panel says so, and is told when it has loaded).
    const assets::LensDatabaseLookup* load = backend_->lens_database_source()->try_get();
    if (load == nullptr || !load->database) {
        return {};
    }
    return lens_profile_search(load->database->profiles(), text, limit);
}

QList<double> AppEditService::sample_tone_curve(const QList<QPointF>& points, int count) const {
    QList<double> samples;
    if (count < 2) {
        return samples;
    }
    engine::ToneCurveSetting setting;
    setting.points.clear();
    for (const QPointF& point : points) {
        setting.points.push_back(engine::ToneCurvePoint{point.x(), point.y()});
    }
    auto curve = engine::ToneCurve::create(setting);
    if (!curve) {
        return samples;
    }
    samples.reserve(count);
    for (int i = 0; i < count; ++i) {
        samples << curve->evaluate(double(i) / double(count - 1));
    }
    return samples;
}

}  // namespace lucent::ui::adapters
