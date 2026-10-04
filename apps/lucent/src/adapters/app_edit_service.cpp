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

    struct Session {
        quint64 id = 0;  // tells a reopened session of the same item from the closed one
        bool open = false;  // the develop session is open
        bool full = false;  // 100 %
        bool want_before = false;
        engine::DevelopSettings committed;  // what to_develop_settings() builds on
        // Develop requests of the session.
        std::map<quint64, quint64> tokens;  // generation -> token
        quint64 current_generation = 0;     // newest request of the photo's own settings
        bool current_settled = true;
        bool current_done = true;           // its last frame (Final, or Quick when unsettled) arrived
        quint64 before_generation = 0;      // outstanding as-shot request (0: none)
        bool before_valid = false;
        bool before_failed = false;
        bool reported_final = false;
        // The newest current frame, for the clipping overlay toggle.
        EditFramePtr last_frame;
        std::shared_ptr<const app::ClippingMask> last_mask;
    };

    State(Backend& backend, QThreadPool& command_pool, QThreadPool& conversion_pool,
          std::shared_ptr<UiDispatcher> ui, AppEditService* service, RenderEventSink sink)
        : recipes(&backend.recipes()),
          develop(&backend.develop()),
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
        dispatcher->post([owner = owner, item, entries] { emit owner->history_changed(item, entries); });
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
        s.tokens[*generation] = token;
        s.current_generation = *generation;
        s.current_settled = view.settled;
        s.current_done = false;
        update_busy_locked();
    }

    // The as-shot frame for before/after, once the current settings are rendered.
    void maybe_request_before_locked(ItemId item, Session& s) {
        if (!s.open || draining || !s.want_before || s.before_valid || s.before_failed ||
            s.before_generation != 0 || !s.current_done) {
            return;
        }
        app::DevelopView view;  // default settings: as shot
        view.settled = false;   // the proxy corrected in stage two: no refine
        view.quality = app::PreviewQuality::fit(app::kDefaultProxyLongEdge);
        auto generation = develop->request(app_id(item), view);
        if (!generation) {
            s.before_failed = true;
            return;
        }
        s.before_generation = *generation;
        s.tokens[*generation] = 0;
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
        quint64 token = 0;
        if (const auto found = s->tokens.find(generation); found != s->tokens.end()) {
            token = found->second;
        }
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
        maybe_request_before_locked(item, *s);
        update_busy_locked();
        conversions->start([weak = weak_from_this(), item, id, frame = std::move(frame), token, before, overlay,
                            report_final] {
            if (auto self = weak.lock()) {
                self->convert(item, id, *frame, token, before, overlay, report_final);
            }
        });
    }

    // Conversion thread, in delivery order.
    void convert(ItemId item, quint64 id, const app::DevelopFrame& frame, quint64 token, bool before, bool overlay,
                 bool report_final) {
        std::shared_ptr<EditFrame> out = to_edit_frame(frame, overlay);
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
}

AppEditService::~AppEditService() {
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
