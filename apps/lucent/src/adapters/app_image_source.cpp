#include "adapters/app_image_source.hpp"

#include <QCoreApplication>
#include <QImage>
#include <algorithm>
#include <deque>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <lucent/application/preview_service.hpp>
#include <lucent/core/cancellation.hpp>

#include "adapters/conversions.hpp"

namespace lucent::ui::adapters {
namespace {

// PreviewService channels of this source.
constexpr std::uint64_t kEditChannel = 1;
constexpr std::uint64_t kThumbnailChannel = 2;

// W3 renders the default settings; their revision is 1 (W6 introduces recipe revisions).
constexpr std::uint64_t kDefaultSettingsRevision = 1;

QString tr_text(const char* text) {
    return QCoreApplication::translate("lucent::ui::adapters", text);
}

QImage fit_long_edge(const QImage& image, int edge) {
    if (image.width() <= edge && image.height() <= edge) {
        return image;
    }
    return image.scaled(edge, edge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

// Least-recently-used thumbnails by item, bounded by bytes.
class ThumbnailCache {
public:
    explicit ThumbnailCache(std::uint64_t capacity) : capacity_(capacity) {}

    std::optional<QImage> find(ItemId item) {
        const auto found = index_.find(item);
        if (found == index_.end()) {
            return std::nullopt;
        }
        entries_.splice(entries_.begin(), entries_, found->second);
        return found->second->image;
    }

    void insert(ItemId item, QImage image) {
        const auto bytes = static_cast<std::uint64_t>(image.sizeInBytes());
        erase(item);
        if (image.isNull() || bytes > capacity_) {
            return;
        }
        entries_.push_front(Entry{item, std::move(image), bytes});
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
        std::uint64_t service_generation = 0;  // Edit and LucentThumbnail renders
        std::optional<core::CancellationSource> cancel;  // Camera: cancelled with the reply
    };

    // Call-outs (reply completions, render events) collected under the lock and run after it is
    // released, so no foreign code ever runs while `mutex` is held.
    using Actions = std::vector<std::function<void()>>;

    State(Backend& backend, QThreadPool& pool, RenderEventSink sink, const ImageSourceOptions& options)
        : library(&backend.library()),
          previews(&backend.previews()),
          pool(&pool),
          events(std::move(sink)),
          thumbnails(options.thumbnail_cache_bytes) {}

    // Valid while !stopped (the owner keeps the Backend and the pool alive until stop()).
    app::Library* library;
    app::PreviewService* previews;
    QThreadPool* pool;
    const RenderEventSink events;

    std::mutex mutex;
    bool stopped = false;
    std::uint64_t next_token = 1;
    std::unordered_map<std::uint64_t, Pending> pending;
    std::uint64_t edit_generation = 0;
    std::uint64_t thumbnail_generation = 0;
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
                    start_conversion(found->first, entry, result.preview);
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

    // Converts a delivered render on the conversion pool. Requires `mutex` and !stopped.
    void start_conversion(std::uint64_t token, const Pending& entry,
                          std::shared_ptr<const app::PreviewImage> preview) {
        pool->start([weak = weak_from_this(), token, route = entry.route, item = entry.request.item,
                     preview = std::move(preview)] {
            auto state = weak.lock();
            if (!state) {
                return;
            }
            QImage image = to_qimage(preview->image);
            RenderInfoPtr info = to_render_info(*preview);
            // Edit previews also give the item a Lucent thumbnail; thumbnail renders are one.
            const QImage thumbnail =
                route == Route::Edit ? fit_long_edge(image, ImageSourceOptions::kThumbnailEdge) : image;
            state->finish_conversion(token, route, item, std::move(image), thumbnail, std::move(info));
        });
    }

    void finish_conversion(std::uint64_t token, Route route, ItemId item, QImage image,
                           const QImage& thumbnail, RenderInfoPtr info) {
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
                thumbnails.insert(item, thumbnail);
                if (reply) {
                    actions.push_back(
                        [reply, image = std::move(image)]() mutable { reply->finish(std::move(image)); });
                }
                RenderEvent event{RenderEvent::Kind::PreviewDelivered, item};
                // An incomplete render (a mandatory step missing) never replaces the camera's
                // own rendering in the contact sheet.
                event.lucent_thumbnail = route == Route::Edit && !info->incomplete;
                event.primary = route == Route::Edit;
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

    void request_lucent_thumbnail(const ImageRequest& request, std::shared_ptr<ImageReply> reply) {
        Actions actions;
        std::uint64_t token = 0;
        {
            const std::lock_guard lock(mutex);
            if (stopped) {
                fail_later(actions, reply, tr_text("Lucent is closing."));
            } else if (std::optional<QImage> cached = thumbnails.find(request.item)) {
                actions.push_back([reply, image = *cached]() mutable { reply->finish(std::move(image)); });
            } else {
                token = next_token++;
                pending.emplace(token, Pending{Route::LucentThumbnail, request, reply, 0});
                thumbnail_queue.push_back(token);
                dispatch_thumbnail_locked(actions);
            }
        }
        run(actions);
        if (token != 0) {
            watch_cancellation(reply, token);
        }
    }

    // Starts the next queued thumbnail render when none is running and no Edit preview is
    // outstanding. Requires `mutex`.
    void dispatch_thumbnail_locked(Actions& actions) {
        if (stopped || thumbnail_in_flight || edit_outstanding > 0) {
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
            if (std::optional<QImage> cached = thumbnails.find(entry.request.item)) {
                actions.push_back(
                    [reply = entry.reply, image = *cached]() mutable { reply->finish(std::move(image)); });
                pending.erase(found);
                continue;
            }
            const QSize full = full_size_locked(entry.request.item);
            const std::uint64_t generation = ++thumbnail_generation;
            app::PreviewRequest preview;
            preview.item = app::ItemId{entry.request.item};
            preview.settings_revision = kDefaultSettingsRevision;
            preview.quality = preview_quality_for(entry.request.bounding_size, full.width(), full.height());
            preview.channel = kThumbnailChannel;
            preview.generation = generation;
            const Status status = previews->request(
                preview, [weak = weak_from_this(), token](const app::PreviewResult& result) {
                    if (auto state = weak.lock()) {
                        state->on_thumbnail(token, result);
                    }
                });
            if (!status) {
                fail_later(actions, entry.reply, describe_error(status.error()));
                pending.erase(found);
                continue;
            }
            entry.service_generation = generation;
            thumbnail_in_flight = token;
            post_event(actions, RenderEvent{RenderEvent::Kind::PreviewStarted, entry.request.item});
            return;
        }
    }

    void on_thumbnail(std::uint64_t token, const app::PreviewResult& result) {
        Actions actions;
        {
            const std::lock_guard lock(mutex);
            if (stopped) {
                return;
            }
            if (thumbnail_in_flight == token) {
                thumbnail_in_flight.reset();
            }
            if (const auto found = pending.find(token); found != pending.end()) {
                handle_render_result(found, result, actions);
            }
            dispatch_thumbnail_locked(actions);
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
                    previews->cancel_channel(kThumbnailChannel);
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
            }
            pending.clear();
            thumbnail_queue.clear();
            thumbnail_in_flight.reset();
            previews->cancel_channel(kEditChannel);
            previews->cancel_channel(kThumbnailChannel);
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
    : backend_(std::move(backend)), pool_(std::make_unique<QThreadPool>()) {
    pool_->setObjectName(QStringLiteral("lucent-image-conversion"));
    pool_->setMaxThreadCount(std::max(1, options.conversion_threads));
    state_ = std::make_shared<State>(*backend_, *pool_, std::move(events), options);
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
}

AppImageSource::~AppImageSource() {
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
