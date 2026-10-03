#include "adapters/app_image_source.hpp"

#include <QBuffer>
#include <QCoreApplication>
#include <QImage>
#include <QImageReader>
#include <QTimer>
#include <algorithm>
#include <deque>
#include <functional>
#include <limits>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <lucent/application/preview_service.hpp>
#include <lucent/application/recipe_service.hpp>
#include <lucent/core/cancellation.hpp>

#include "adapters/conversions.hpp"
#include "adapters/ui_dispatcher.hpp"

namespace lucent::ui::adapters {
namespace {

// PreviewService channel of this source's Edit previews.
constexpr std::uint64_t kEditChannel = 1;

QString tr_text(const char* text) {
    return QCoreApplication::translate("lucent::ui::adapters", text);
}

QImage fit_long_edge(const QImage& image, int edge) {
    if (image.width() <= edge && image.height() <= edge) {
        return image;
    }
    return image.scaled(edge, edge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

// Decodes a Lucent-rendered thumbnail (an sRGB JPEG, upright) scaled down to fit `bounding`.
QImage decode_rendered_thumbnail(const app::Thumbnail& thumbnail, QSize bounding) {
    if (thumbnail.jpeg.empty() || thumbnail.jpeg.size() > std::size_t(std::numeric_limits<int>::max())) {
        return {};
    }
    const QByteArray bytes = QByteArray::fromRawData(reinterpret_cast<const char*>(thumbnail.jpeg.data()),
                                                     static_cast<qsizetype>(thumbnail.jpeg.size()));
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return {};
    }
    QImageReader reader(&buffer, "jpeg");
    reader.setAutoTransform(false);  // rendered upright
    const QSize stored(thumbnail.width, thumbnail.height);
    if (stored.isValid()) {
        QSize box = bounding;
        if (box.width() <= 0) {
            box.setWidth(stored.width());
        }
        if (box.height() <= 0) {
            box.setHeight(stored.height());
        }
        if (stored.width() > box.width() || stored.height() > box.height()) {
            reader.setScaledSize(stored.scaled(box, Qt::KeepAspectRatio));
        }
    }
    QImage image = reader.read();
    if (image.isNull()) {
        return {};
    }
    return image.format() == QImage::Format_RGB32 ? image : image.convertToFormat(QImage::Format_RGB32);
}

// Least-recently-used thumbnails by item, bounded by bytes. Each entry shows the settings whose
// canonical document has `settings` as SHA-256 (a changed recipe never finds an old thumbnail).
class ThumbnailCache {
public:
    explicit ThumbnailCache(std::uint64_t capacity) : capacity_(capacity) {}

    std::optional<QImage> find(ItemId item, const core::Digest256& settings) {
        const auto found = index_.find(item);
        if (found == index_.end() || found->second->settings != settings) {
            return std::nullopt;
        }
        entries_.splice(entries_.begin(), entries_, found->second);
        return found->second->image;
    }

    void insert(ItemId item, const core::Digest256& settings, QImage image) {
        const auto bytes = static_cast<std::uint64_t>(image.sizeInBytes());
        erase(item);
        if (image.isNull() || bytes > capacity_) {
            return;
        }
        entries_.push_front(Entry{item, settings, std::move(image), bytes});
        index_[item] = entries_.begin();
        bytes_ += bytes;
        while (bytes_ > capacity_ && !entries_.empty()) {
            bytes_ -= entries_.back().bytes;
            index_.erase(entries_.back().item);
            entries_.pop_back();
        }
    }

    void erase(ItemId item) {
        const auto found = index_.find(item);
        if (found != index_.end()) {
            bytes_ -= found->second->bytes;
            entries_.erase(found->second);
            index_.erase(found);
        }
    }

private:
    struct Entry {
        ItemId item = 0;
        core::Digest256 settings{};
        QImage image;
        std::uint64_t bytes = 0;
    };

    std::uint64_t capacity_;
    std::uint64_t bytes_ = 0;
    std::list<Entry> entries_;  // front = most recently used
    std::unordered_map<ItemId, std::list<Entry>::iterator> index_;
};

}  // namespace

struct AppImageSource::State : std::enable_shared_from_this<State> {
    enum class Route : std::uint8_t { Edit, Camera, LucentThumbnail };

    struct Pending {
        Route route = Route::Edit;
        ImageRequest request;
        std::shared_ptr<ImageReply> reply;
        std::uint64_t service_generation = 0;  // Edit previews
        // Camera previews and Lucent thumbnails: cancelled with the reply (skipped if not started).
        std::optional<core::CancellationSource> cancel;
    };

    // Call-outs (reply completions, render events) collected under the lock and run after it is
    // released, so no foreign code ever runs while `mutex` is held.
    using Actions = std::vector<std::function<void()>>;

    State(Backend& backend, QThreadPool& pool, RenderEventSink sink, const ImageSourceOptions& options,
          QObject& timer_context)
        : library(&backend.library()),
          previews(&backend.previews()),
          recipes(&backend.recipes()),
          activity(&backend.interactive()),
          pool(&pool),
          events(std::move(sink)),
          timers(std::make_shared<UiDispatcher>(&timer_context)),
          timer_context(&timer_context),
          quiet_ms(std::max(0, options.interactive_quiet_ms)),
          thumbnails(options.thumbnail_cache_bytes) {}

    // Valid while !stopped (the owner keeps the Backend and the pool alive until stop()).
    app::Library* library;
    app::PreviewService* previews;
    app::RecipeService* recipes;
    InteractiveActivity* activity;
    QThreadPool* pool;
    const RenderEventSink events;
    // Delayed thumbnail starts (the quiet period after interactive editing), on the UI thread.
    const std::shared_ptr<UiDispatcher> timers;
    QObject* const timer_context;  // used only by functions posted through `timers`
    const int quiet_ms;
    bool recheck_scheduled = false;

    std::mutex mutex;
    bool stopped = false;
    std::uint64_t next_token = 1;
    std::unordered_map<std::uint64_t, Pending> pending;
    std::uint64_t edit_generation = 0;
    int edit_outstanding = 0;  // Edit preview requests whose callback has not run yet
    std::deque<std::uint64_t> thumbnail_queue;
    std::optional<std::uint64_t> thumbnail_in_flight;
    ThumbnailCache thumbnails;
    std::unordered_map<ItemId, QSize> rendered_sizes;

    static void run(Actions& actions) {
        for (auto& action : actions) {
            action();
        }
        actions.clear();
    }

    void post_event(Actions& actions, RenderEvent event) const {
        if (events) {
            actions.push_back(
                [sink = events, event = std::move(event)]() mutable { sink(std::move(event)); });
        }
    }

    static void fail_later(Actions& actions, std::shared_ptr<ImageReply> reply, QString message) {
        actions.push_back([reply = std::move(reply), message = std::move(message)] { reply->fail(message); });
    }

    // Full-resolution upright size of `item`: the last render's, else the probe's (0 x 0 unknown).
    QSize full_size_locked(ItemId item) {
        if (const auto found = rendered_sizes.find(item); found != rendered_sizes.end()) {
            return found->second;
        }
        if (std::optional<app::SourceItem> source = library->find(app::ItemId{item});
            source && source->probe) {
            return QSize(source->probe->upright_size.width, source->probe->upright_size.height);
        }
        return {};
    }

    void watch_cancellation(const std::shared_ptr<ImageReply>& reply, std::uint64_t token) {
        reply->on_cancel([weak = weak_from_this(), token] {
            if (auto state = weak.lock()) {
                state->on_reply_cancelled(token);
            }
        });
    }

    // ---- Entry point --------------------------------------------------------------------------

    void request(const ImageRequest& request, std::shared_ptr<ImageReply> reply) {
        if (request.item == 0 || request.generation == kNoImage) {
            reply->fail(tr_text("Invalid image request."));
            return;
        }
        if (request.kind == ImageKind::Preview) {
            request_preview(request, std::move(reply));
        } else if (request.generation == kCameraPreviewGeneration) {
            request_camera(request, std::move(reply));
        } else {
            request_lucent_thumbnail(request, std::move(reply));
        }
    }

    // ---- Edit previews ------------------------------------------------------------------------

    void request_preview(const ImageRequest& request, std::shared_ptr<ImageReply> reply) {
        Actions actions;
        std::uint64_t token = 0;
        {
            const std::lock_guard lock(mutex);
            if (stopped) {
                fail_later(actions, reply, tr_text("Lucent is closing."));
            } else {
                const QSize full = full_size_locked(request.item);
                token = next_token++;
                const std::uint64_t generation = ++edit_generation;
                app::PreviewRequest preview;
                preview.item = app::ItemId{request.item};
                preview.settings_revision = request.generation;
                preview.quality = preview_quality_for(request.bounding_size, full.width(), full.height());
                preview.channel = kEditChannel;
                preview.generation = generation;
                const Status status = previews->request(
                    preview, [weak = weak_from_this(), token](const app::PreviewResult& result) {
                        if (auto state = weak.lock()) {
                            state->on_preview(token, result);
                        }
                    });
                if (!status) {
                    fail_later(actions, reply, describe_error(status.error()));
                    token = 0;
                } else {
                    pending.emplace(token, Pending{Route::Edit, request, reply, generation});
                    ++edit_outstanding;
                    post_event(actions, RenderEvent{RenderEvent::Kind::PreviewStarted, request.item});
                }
            }
        }
        run(actions);
        if (token != 0) {
            watch_cancellation(reply, token);
        }
    }

    void on_preview(std::uint64_t token, const app::PreviewResult& result) {
        Actions actions;
        {
            const std::lock_guard lock(mutex);
            if (stopped) {
                return;
            }
            --edit_outstanding;
            if (const auto found = pending.find(token); found != pending.end()) {
                handle_render_result(found, result, actions);
            }
            dispatch_thumbnail_locked(actions);
        }
        run(actions);
    }

    // Shared by Edit previews and thumbnail renders. Requires `mutex`; `found` stays pending when a
    // conversion is started (the conversion completes it).
    void handle_render_result(std::unordered_map<std::uint64_t, Pending>::iterator found,
                              const app::PreviewResult& result, Actions& actions) {
        const Pending entry = found->second;
        switch (result.outcome) {
            case app::PreviewOutcome::Delivered:
                if (result.preview) {
                    start_conversion(found->first, entry, result.preview, result.settings_sha256);
                    return;
                }
                pending.erase(found);
                fail_later(actions, entry.reply, tr_text("The preview is empty."));
                post_event(actions, RenderEvent{RenderEvent::Kind::PreviewAbandoned, entry.request.item});
                return;
            case app::PreviewOutcome::Superseded:
                pending.erase(found);
                fail_later(actions, entry.reply, tr_text("A newer preview replaced this one."));
                post_event(actions, RenderEvent{RenderEvent::Kind::PreviewAbandoned, entry.request.item});
                return;
            case app::PreviewOutcome::Cancelled:
                pending.erase(found);
                fail_later(actions, entry.reply, tr_text("The preview was cancelled."));
                post_event(actions, RenderEvent{RenderEvent::Kind::PreviewAbandoned, entry.request.item});
                return;
            case app::PreviewOutcome::Failed: {
                pending.erase(found);
                const QString message = result.error ? describe_error(*result.error)
                                                     : tr_text("Lucent could not render this photo.");
                fail_later(actions, entry.reply, message);
                RenderEvent event{RenderEvent::Kind::PreviewFailed, entry.request.item, message};
                post_event(actions, std::move(event));
                return;
            }
        }
    }

    // Converts a delivered Edit preview on the conversion pool. Requires `mutex` and !stopped.
    void start_conversion(std::uint64_t token, const Pending& entry,
                          std::shared_ptr<const app::PreviewImage> preview, const core::Digest256& settings) {
        pool->start([weak = weak_from_this(), token, item = entry.request.item, settings,
                     preview = std::move(preview)] {
            auto state = weak.lock();
            if (!state) {
                return;
            }
            QImage image = to_qimage(preview->image);
            RenderInfoPtr info = to_render_info(*preview);
            // An Edit preview also gives the item a Lucent thumbnail of the same settings.
            const QImage thumbnail = fit_long_edge(image, ImageSourceOptions::kThumbnailEdge);
            state->finish_conversion(token, item, std::move(image), thumbnail, settings, std::move(info));
        });
    }

    void finish_conversion(std::uint64_t token, ItemId item, QImage image, const QImage& thumbnail,
                           const core::Digest256& settings, RenderInfoPtr info) {
        Actions actions;
        {
            const std::lock_guard lock(mutex);
            if (stopped) {
                return;
            }
            const auto found = pending.find(token);
            std::shared_ptr<ImageReply> reply = found != pending.end() ? found->second.reply : nullptr;
            if (found != pending.end()) {
                pending.erase(found);
            }
            if (image.isNull()) {
                if (reply) {
                    fail_later(actions, reply, describe_error(Error{ErrorCode::OutOfMemory, {}, {}, 0}));
                }
                post_event(actions, RenderEvent{RenderEvent::Kind::PreviewAbandoned, item});
            } else {
                if (info->rendered_width > 0 && info->rendered_height > 0) {
                    rendered_sizes[item] = QSize(info->rendered_width, info->rendered_height);
                }
                // Served only for Lucent-render generations (2+), which an item whose render is
                // incomplete reaches only when it has no camera preview to keep.
                thumbnails.insert(item, settings, thumbnail);
                if (reply) {
                    actions.push_back(
                        [reply, image = std::move(image)]() mutable { reply->finish(std::move(image)); });
                }
                RenderEvent event{RenderEvent::Kind::PreviewDelivered, item};
                // An incomplete render (a mandatory step missing) never replaces the camera's
                // own rendering in the contact sheet.
                event.lucent_thumbnail = !info->incomplete;
                event.primary = true;
                event.render = std::move(info);
                post_event(actions, std::move(event));
            }
        }
        run(actions);
    }

    // ---- Camera previews ----------------------------------------------------------------------

    void request_camera(const ImageRequest& request, std::shared_ptr<ImageReply> reply) {
        Actions actions;
        std::uint64_t token = 0;
        {
            const std::lock_guard lock(mutex);
            if (stopped) {
                fail_later(actions, reply, tr_text("Lucent is closing."));
            } else {
                token = next_token++;
                core::CancellationSource cancel;
                app::CameraPreviewRequest camera;
                camera.item = app::ItemId{request.item};
                // The smallest preview of the file that covers the box (0: unconstrained).
                camera.min_long_edge =
                    std::max(0, std::max(request.bounding_size.width(), request.bounding_size.height()));
                camera.cancel = cancel.token();
                const Status status = previews->request_camera_preview(
                    camera, [weak = weak_from_this(), token](
                                app::ItemId, const Expected<std::shared_ptr<const app::CameraPreview>>& result) {
                        if (auto state = weak.lock()) {
                            state->on_camera(token, result);
                        }
                    });
                if (!status) {
                    fail_later(actions, reply, describe_error(status.error()));
                    token = 0;
                } else {
                    pending.emplace(token, Pending{Route::Camera, request, reply, 0, std::move(cancel)});
                }
            }
        }
        run(actions);
        if (token != 0) {
            watch_cancellation(reply, token);
        }
    }

    void on_camera(std::uint64_t token, const Expected<std::shared_ptr<const app::CameraPreview>>& result) {
        Actions actions;
        {
            const std::lock_guard lock(mutex);
            if (stopped) {
                return;
            }
            const auto found = pending.find(token);
            if (found == pending.end()) {
                return;
            }
            const Pending entry = found->second;
            if (result && *result && (*result)->embedded.preview) {
                pool->start([weak = weak_from_this(), token, camera = *result,
                             bounding = entry.request.bounding_size] {
                    if (auto state = weak.lock()) {
                        QString error;
                        QImage image = decode_camera_preview(*camera->embedded.preview, bounding, &error);
                        state->finish_camera(token, std::move(image), error);
                    }
                });
                return;
            }
            pending.erase(found);
            if (!result) {
                fail_later(actions, entry.reply, describe_error(result.error()));
                if (result.error().code != ErrorCode::Cancelled) {
                    post_event(actions,
                               RenderEvent{RenderEvent::Kind::CameraPreviewUnavailable, entry.request.item});
                }
            } else {
                fail_later(actions, entry.reply, tr_text("This file has no camera preview."));
                post_event(actions,
                           RenderEvent{RenderEvent::Kind::CameraPreviewUnavailable, entry.request.item});
            }
        }
        run(actions);
    }

    void finish_camera(std::uint64_t token, QImage image, const QString& error) {
        Actions actions;
        {
            const std::lock_guard lock(mutex);
            if (stopped) {
                return;
            }
            const auto found = pending.find(token);
            if (found == pending.end()) {
                return;
            }
            const Pending entry = found->second;
            pending.erase(found);
            if (image.isNull()) {
                fail_later(actions, entry.reply, error);
                post_event(actions,
                           RenderEvent{RenderEvent::Kind::CameraPreviewUnavailable, entry.request.item});
            } else {
                actions.push_back([reply = entry.reply, image = std::move(image)]() mutable {
                    reply->finish(std::move(image));
                });
            }
        }
        run(actions);
    }

    // ---- Lucent thumbnails --------------------------------------------------------------------

    // The identity of the settings an item's thumbnail must show now (its committed recipe);
    // std::nullopt when they cannot be read.
    std::optional<core::Digest256> thumbnail_settings(ItemId item) const {
        auto state = recipes->state(app::ItemId{item});
        if (!state || state->recipe_unreadable) {
            return std::nullopt;
        }
        return state->committed_sha256;
    }

    void request_lucent_thumbnail(const ImageRequest& request, std::shared_ptr<ImageReply> reply) {
        // Read before locking: the recipe service may read its store. The Backend keeps the recipe
        // service alive while this source exists.
        const std::optional<core::Digest256> settings = thumbnail_settings(request.item);
        Actions actions;
        std::uint64_t token = 0;
        {
            const std::lock_guard lock(mutex);
            std::optional<QImage> cached;
            if (!stopped && settings) {
                cached = thumbnails.find(request.item, *settings);
            }
            if (stopped) {
                fail_later(actions, reply, tr_text("Lucent is closing."));
            } else if (cached) {
                actions.push_back([reply, image = *cached]() mutable { reply->finish(std::move(image)); });
            } else {
                token = next_token++;
                Pending entry{Route::LucentThumbnail, request, reply, 0, core::CancellationSource{}};
                pending.emplace(token, std::move(entry));
                thumbnail_queue.push_back(token);
                dispatch_thumbnail_locked(actions);
            }
        }
        run(actions);
        if (token != 0) {
            watch_cancellation(reply, token);
        }
    }

    // Starts the next queued thumbnail render when none is running, no Edit preview is outstanding
    // and interactive editing is not waiting for a render (InteractiveActivity). Requires `mutex`.
    void dispatch_thumbnail_locked(Actions& actions) {
        if (stopped || thumbnail_in_flight || edit_outstanding > 0 || activity->busy()) {
            return;  // a callback or the idle listener comes back
        }
        if (const auto idle = activity->idle_for().count(); !thumbnail_queue.empty() && idle < quiet_ms) {
            schedule_recheck_locked(static_cast<int>(quiet_ms - idle));
            return;
        }
        while (!thumbnail_queue.empty()) {
            const std::uint64_t token = thumbnail_queue.front();
            thumbnail_queue.pop_front();
            const auto found = pending.find(token);
            if (found == pending.end()) {
                continue;
            }
            Pending& entry = found->second;
            if (entry.reply->is_cancelled()) {
                pending.erase(found);
                continue;
            }
            // The item's current recipe, rendered and cached by its settings (memory and disk).
            app::RenderedThumbnailRequest request;
            request.item = app::ItemId{entry.request.item};
            request.cancel = entry.cancel ? entry.cancel->token() : core::CancellationToken{};
            const Status status = previews->request_rendered_thumbnail(
                request, [weak = weak_from_this(), token](
                             app::ItemId, const Expected<std::shared_ptr<const app::RenderedThumbnail>>& result) {
                    if (auto state = weak.lock()) {
                        state->on_rendered_thumbnail(token, result);
                    }
                });
            if (!status) {
                const QString message = describe_error(status.error());
                fail_later(actions, entry.reply, message);
                post_event(actions, RenderEvent{RenderEvent::Kind::PreviewFailed, entry.request.item, message});
                pending.erase(found);
                continue;
            }
            thumbnail_in_flight = token;
            post_event(actions, RenderEvent{RenderEvent::Kind::PreviewStarted, entry.request.item});
            return;
        }
    }

    // Looks at the queue again after `delay_ms` (a timer on the UI thread). Requires `mutex`.
    void schedule_recheck_locked(int delay_ms) {
        if (recheck_scheduled) {
            return;
        }
        recheck_scheduled = true;
        timers->post([weak = weak_from_this(), context = timer_context, delay_ms] {
            QTimer::singleShot(delay_ms, context, [weak] {
                if (auto state = weak.lock()) {
                    state->on_recheck();
                }
            });
        });
    }

    void on_recheck() {
        Actions actions;
        {
            const std::lock_guard lock(mutex);
            recheck_scheduled = false;
            dispatch_thumbnail_locked(actions);
        }
        run(actions);
    }

    // The idle listener of the InteractiveActivity: queued thumbnails may start.
    void on_interactive_idle() {
        Actions actions;
        {
            const std::lock_guard lock(mutex);
            dispatch_thumbnail_locked(actions);
        }
        run(actions);
    }

    void on_rendered_thumbnail(std::uint64_t token,
                               const Expected<std::shared_ptr<const app::RenderedThumbnail>>& result) {
        Actions actions;
        {
            const std::lock_guard lock(mutex);
            if (stopped) {
                return;
            }
            if (thumbnail_in_flight == token) {
                thumbnail_in_flight.reset();
            }
            const auto found = pending.find(token);
            if (found != pending.end()) {
                const Pending entry = found->second;
                if (result && *result) {
                    pool->start([weak = weak_from_this(), token, thumbnail = *result,
                                 bounding = entry.request.bounding_size] {
                        if (auto state = weak.lock()) {
                            QImage image = decode_rendered_thumbnail(thumbnail->jpeg, bounding);
                            state->finish_thumbnail(token, std::move(image), thumbnail->settings_sha256);
                        }
                    });
                } else {
                    pending.erase(found);
                    if (!result && result.error().code == ErrorCode::Cancelled) {
                        fail_later(actions, entry.reply, tr_text("The preview was cancelled."));
                        post_event(actions, RenderEvent{RenderEvent::Kind::PreviewAbandoned, entry.request.item});
                    } else {
                        const QString message = !result ? describe_error(result.error())
                                                        : tr_text("Lucent could not render this photo.");
                        fail_later(actions, entry.reply, message);
                        post_event(actions,
                                   RenderEvent{RenderEvent::Kind::PreviewFailed, entry.request.item, message});
                    }
                }
            }
            dispatch_thumbnail_locked(actions);
        }
        run(actions);
    }

    void finish_thumbnail(std::uint64_t token, QImage image, const core::Digest256& settings) {
        Actions actions;
        {
            const std::lock_guard lock(mutex);
            if (stopped) {
                return;
            }
            const auto found = pending.find(token);
            if (found == pending.end()) {
                return;
            }
            const Pending entry = found->second;
            pending.erase(found);
            if (image.isNull()) {
                const QString message = tr_text("Lucent's thumbnail of this photo could not be read.");
                fail_later(actions, entry.reply, message);
                post_event(actions, RenderEvent{RenderEvent::Kind::PreviewFailed, entry.request.item, message});
            } else {
                thumbnails.insert(entry.request.item, settings, image);
                actions.push_back(
                    [reply = entry.reply, image = std::move(image)]() mutable { reply->finish(std::move(image)); });
                // Processed, without render facts (a thumbnail render has no histogram): an Edit
                // render provides those.
                RenderEvent event{RenderEvent::Kind::PreviewDelivered, entry.request.item};
                event.primary = false;
                post_event(actions, std::move(event));
            }
        }
        run(actions);
    }

    // ---- Library changes ----------------------------------------------------------------------

    // The file of `items` changed (or they were removed): what was derived from it is stale.
    void forget_items(const std::vector<app::ItemId>& items) {
        const std::lock_guard lock(mutex);
        for (const app::ItemId id : items) {
            thumbnails.erase(id.value);
            rendered_sizes.erase(id.value);
        }
    }

    // ---- Cancellation and shutdown ------------------------------------------------------------

    void on_reply_cancelled(std::uint64_t token) {
        const std::lock_guard lock(mutex);
        if (stopped) {
            return;
        }
        const auto found = pending.find(token);
        if (found == pending.end()) {
            return;
        }
        const Pending& entry = found->second;
        switch (entry.route) {
            case Route::Edit:
                // Only the newest request of the channel: cancelling the channel stops it without
                // touching newer requests (older ones are superseded anyway).
                if (entry.service_generation == edit_generation) {
                    previews->cancel_channel(kEditChannel);
                }
                break;
            case Route::LucentThumbnail:
                if (thumbnail_in_flight == token) {
                    // Skipped when it has not started (a running render is short and finishes).
                    if (found->second.cancel) {
                        found->second.cancel->cancel();
                    }
                } else {
                    std::erase(thumbnail_queue, token);
                    pending.erase(found);
                }
                break;
            case Route::Camera:
                // Not read yet: the service skips it (its callback still comes, Cancelled).
                if (found->second.cancel) {
                    found->second.cancel->cancel();
                }
                break;
        }
    }

    // Fails every outstanding reply and detaches from the services. Idempotent.
    void stop() {
        std::vector<std::shared_ptr<ImageReply>> replies;
        {
            const std::lock_guard lock(mutex);
            if (stopped) {
                return;
            }
            stopped = true;
            for (auto& [token, entry] : pending) {
                replies.push_back(entry.reply);
                if (entry.cancel) {
                    entry.cancel->cancel();
                }
            }
            pending.clear();
            thumbnail_queue.clear();
            thumbnail_in_flight.reset();
            previews->cancel_channel(kEditChannel);
            previews = nullptr;
            library = nullptr;
            pool = nullptr;
        }
        for (const auto& reply : replies) {
            reply->fail(tr_text("Lucent is closing."));
        }
    }
};

AppImageSource::AppImageSource(std::shared_ptr<Backend> backend, RenderEventSink events,
                               const ImageSourceOptions& options)
    : backend_(std::move(backend)), timers_(std::make_unique<QObject>()), pool_(std::make_unique<QThreadPool>()) {
    pool_->setObjectName(QStringLiteral("lucent-image-conversion"));
    pool_->setMaxThreadCount(std::max(1, options.conversion_threads));
    state_ = std::make_shared<State>(*backend_, *pool_, std::move(events), options, *timers_);
    // Runs on the thread that changed the library (an import's worker).
    subscription_ = backend_->library().subscribe(
        [weak = std::weak_ptr<State>(state_)](const app::LibraryChange& change) {
            if (change.kind != app::LibraryChangeKind::Refreshed && change.kind != app::LibraryChangeKind::Removed) {
                return;
            }
            if (auto state = weak.lock()) {
                state->forget_items(change.items);
            }
        });
    // Thumbnail renders wait while the photo being edited waits for a render.
    backend_->interactive().set_idle_listener([weak = std::weak_ptr<State>(state_)] {
        if (auto state = weak.lock()) {
            state->on_interactive_idle();
        }
    });
}

AppImageSource::~AppImageSource() {
    backend_->interactive().set_idle_listener({});  // waits for a listener running on another thread
    state_->timers->close();
    timers_.reset();  // pending timers go with it
    subscription_.reset();  // waits for an observer running on another thread
    state_->stop();
    pool_->waitForDone();  // conversions still running see `stopped` and drop their results
    pool_.reset();
    state_.reset();
    backend_.reset();  // possibly the last reference: joins the executor (see backend.hpp)
}

void AppImageSource::request_image(const ImageRequest& request, std::shared_ptr<ImageReply> reply) {
    if (!reply) {
        return;
    }
    state_->request(request, std::move(reply));
}

}  // namespace lucent::ui::adapters
