#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <format>
#include <list>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <lucent/application/display_image.hpp>
#include <lucent/application/preview_service.hpp>
#include <lucent/core/log.hpp>

namespace lucent::app {

namespace {

constexpr std::string_view kWhere = "app.preview";
constexpr std::string_view kWhereCamera = "app.preview.camera";

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

std::uint64_t pixel_bytes(image::Size size, std::uint64_t bytes_per_pixel) {
    return size.empty() ? 0 : static_cast<std::uint64_t>(size.area()) * bytes_per_pixel;
}

// Key of a camera preview: the item and the size it was requested at.
struct CameraKey {
    ItemId item;
    std::int32_t min_long_edge = 0;
    friend bool operator==(const CameraKey&, const CameraKey&) = default;
};

struct CameraKeyHash {
    std::size_t operator()(const CameraKey& key) const noexcept {
        return ItemIdHash{}(key.item) * 31u + std::hash<std::int32_t>{}(key.min_long_edge);
    }
};

// Bounded LRU of camera previews by key (the embedded JPEG bytes dominate the size).
class CameraCache {
public:
    explicit CameraCache(std::uint64_t capacity) : capacity_(capacity) {}

    std::shared_ptr<const CameraPreview> find(const CameraKey& key) {
        std::lock_guard lock(mutex_);
        const auto it = index_.find(key);
        if (it == index_.end()) {
            return nullptr;
        }
        lru_.splice(lru_.begin(), lru_, it->second);
        return it->second->preview;
    }

    void insert(const CameraKey& key, std::shared_ptr<const CameraPreview> preview) {
        std::uint64_t bytes = 1024;
        if (preview->embedded.preview) {
            bytes += preview->embedded.preview->bytes.size();
        }
        std::lock_guard lock(mutex_);
        if (bytes > capacity_) {
            return;
        }
        try {
            erase_locked(key);
            lru_.push_front(Entry{key, std::move(preview), bytes});
            try {
                index_.emplace(key, lru_.begin());
            } catch (...) {
                lru_.pop_front();
                throw;
            }
        } catch (const std::bad_alloc&) {
            return;  // disposable cache
        }
        bytes_ += bytes;
        while (bytes_ > capacity_ && !lru_.empty()) {
            bytes_ -= lru_.back().bytes;
            index_.erase(lru_.back().key);
            lru_.pop_back();
        }
    }

    // Drops every entry of `item`, whatever its size.
    void erase_item(ItemId item) {
        std::lock_guard lock(mutex_);
        for (auto it = lru_.begin(); it != lru_.end();) {
            if (it->key.item == item) {
                bytes_ -= it->bytes;
                index_.erase(it->key);
                it = lru_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    struct Entry {
        CameraKey key;
        std::shared_ptr<const CameraPreview> preview;
        std::uint64_t bytes = 0;
    };

    void erase_locked(const CameraKey& key) {
        const auto it = index_.find(key);
        if (it != index_.end()) {
            bytes_ -= it->second->bytes;
            lru_.erase(it->second);
            index_.erase(it);
        }
    }

    std::mutex mutex_;
    std::uint64_t capacity_;
    std::uint64_t bytes_ = 0;
    std::list<Entry> lru_;
    std::unordered_map<CameraKey, std::list<Entry>::iterator, CameraKeyHash> index_;
};

}  // namespace

std::string_view to_string(PreviewOutcome outcome) noexcept {
    switch (outcome) {
        case PreviewOutcome::Delivered:
            return "delivered";
        case PreviewOutcome::Superseded:
            return "superseded";
        case PreviewOutcome::Cancelled:
            return "cancelled";
        case PreviewOutcome::Failed:
            return "failed";
    }
    return "unknown";
}

struct PreviewService::State : std::enable_shared_from_this<State> {
    State(Library& l, TaskExecutor& e, std::shared_ptr<const SourceRenderer> r,
          const PreviewServiceOptions& o)
        : library(l),
          executor(e),
          renderer(std::move(r)),
          options(o),
          cache(o.cache_bytes),
          camera_cache(o.camera_cache_bytes) {}

    struct Channel {
        std::uint64_t latest = 0;             // newest generation requested
        std::uint64_t cancelled_through = 0;  // generations <= this were cancelled by cancel_channel
        std::shared_ptr<std::mutex> delivery = std::make_shared<std::mutex>();  // serialises callbacks
    };
    struct Active {
        std::uint64_t id = 0;
        std::uint64_t channel = 0;
        TaskHandle handle;
        bool camera = false;
    };

    Library& library;
    TaskExecutor& executor;
    std::shared_ptr<const SourceRenderer> renderer;
    PreviewServiceOptions options;
    PreviewCache cache;
    CameraCache camera_cache;

    std::mutex mutex;
    std::condition_variable idle;
    bool shut_down = false;
    std::uint64_t outstanding = 0;
    std::uint64_t next_id = 1;
    std::unordered_map<std::uint64_t, Channel> channels;
    std::vector<Active> active;

    void forget(std::uint64_t id) {  // requires mutex
        std::erase_if(active, [id](const Active& a) { return a.id == id; });
    }

    void finished_one() {
        {
            std::lock_guard lock(mutex);
            --outstanding;
        }
        idle.notify_all();
    }

    // ---- Lucent previews --------------------------------------------------------------------

    Expected<std::shared_ptr<const PreviewImage>> produce(const TaskContext& context,
                                                          const PreviewRequest& request,
                                                          const SourceItem& item,
                                                          std::chrono::steady_clock::time_point start) {
        const core::CancellationToken& cancel = context.cancel();
        auto estimate = renderer->estimate_memory(item, request.settings, cancel);
        if (!estimate) {
            return Unexpected(std::move(estimate.error()));
        }
        // The engine's estimate covers its pixel buffers; the preview adds the 8-bit output (and,
        // for Fit, the small downscaled float image).
        const image::Size upright = item.probe ? item.probe->upright_size : image::Size{};
        std::uint64_t extra = 0;
        if (request.quality.kind == PreviewQualityKind::Full) {
            extra = pixel_bytes(upright, 3);
        } else {
            extra = pixel_bytes(fit_within(upright, request.quality.long_edge), 15);
        }
        auto lease = context.admit(*estimate + extra);
        if (!lease) {
            return Unexpected(std::move(lease.error()));
        }

        engine::ColorImage display;
        std::shared_ptr<engine::RenderReport> report;
        {
            auto rendered = renderer->render(item, request.settings, {}, cancel);
            if (!rendered) {
                return Unexpected(std::move(rendered.error()));
            }
            display = std::move(rendered->display);
            report = std::make_shared<engine::RenderReport>(std::move(rendered->report));
        }  // the rest of the render result (metadata, scene-linear copy) is released here

        if (request.quality.kind == PreviewQualityKind::Fit) {
            const image::Size target = fit_within(display.pixels.size(), request.quality.long_edge);
            if (target != display.pixels.size()) {
                auto fitted = downscale_area(display, target, cancel, options.parallel);
                if (!fitted) {
                    return Unexpected(std::move(fitted.error()));
                }
                display = std::move(*fitted);  // releases the full-resolution image
            }
        }
        auto encoded = encode_display_srgb8(display, cancel, options.parallel);
        if (!encoded) {
            return Unexpected(std::move(encoded.error()));
        }
        display = {};
        lease->release();

        auto preview = std::make_shared<PreviewImage>();
        preview->key = PreviewKey{request.item, request.settings_revision, request.quality};
        preview->image = std::move(*encoded);
        preview->histogram = compute_histogram(preview->image);
        preview->summary = summarize_render(*report);
        preview->report = std::move(report);
        preview->seconds = seconds_since(start);
        return std::shared_ptr<const PreviewImage>(std::move(preview));
    }

    void run_preview(const TaskContext& context, std::uint64_t id, const PreviewRequest& request,
                     const SourceItem& item, const PreviewCallback& callback,
                     const std::shared_ptr<std::mutex>& delivery) {
        const auto start = std::chrono::steady_clock::now();
        const PreviewKey key{request.item, request.settings_revision, request.quality};
        Expected<std::shared_ptr<const PreviewImage>> result =
            fail(ErrorCode::Cancelled, "the preview was cancelled before it started", std::string(kWhere));
        bool from_cache = false;
        try {
            if (auto cached = cache.find(key)) {
                result = std::move(cached);
                from_cache = true;
            } else if (!context.cancel().is_cancelled()) {
                result = produce(context, request, item, start);
                if (result) {
                    cache.insert(key, *result);
                }
            }
        } catch (const std::bad_alloc&) {
            result = fail(ErrorCode::OutOfMemory, "the preview ran out of memory", std::string(kWhere));
        } catch (const std::exception& e) {
            result = fail(ErrorCode::Internal, std::format("the preview raised: {}", e.what()),
                          std::string(kWhere));
        }
        deliver(id, request, std::move(result), from_cache, callback, delivery);
    }

    void deliver(std::uint64_t id, const PreviewRequest& request,
                 Expected<std::shared_ptr<const PreviewImage>> result, bool from_cache,
                 const PreviewCallback& callback, const std::shared_ptr<std::mutex>& delivery) {
        PreviewResult out;
        out.item = request.item;
        out.channel = request.channel;
        out.generation = request.generation;
        out.settings_revision = request.settings_revision;
        out.quality = request.quality;
        {
            // Holding the channel's delivery lock across the decision and the callback keeps the
            // channel's callbacks serial, so no older image can follow a newer one.
            std::lock_guard delivering(*delivery);
            {
                std::lock_guard lock(mutex);
                forget(id);
                const Channel& channel = channels[request.channel];
                const bool newest = request.generation == channel.latest;
                const bool cancelled = shut_down || request.generation <= channel.cancelled_through;
                if (result) {
                    if (cancelled) {
                        out.outcome = PreviewOutcome::Cancelled;
                    } else if (!newest) {
                        out.outcome = PreviewOutcome::Superseded;
                    } else {
                        out.outcome = PreviewOutcome::Delivered;
                        out.preview = std::move(*result);
                        out.from_cache = from_cache;
                    }
                } else {
                    if (!newest) {
                        out.outcome = PreviewOutcome::Superseded;
                    } else if (cancelled || result.error().code == ErrorCode::Cancelled) {
                        out.outcome = PreviewOutcome::Cancelled;
                    } else {
                        out.outcome = PreviewOutcome::Failed;
                    }
                    out.error = std::move(result.error());
                }
            }
            try {
                callback(out);
            } catch (const std::exception& e) {
                core::log_error(kWhere, "a preview callback threw: {}", e.what());
            } catch (...) {
                core::log_error(kWhere, "a preview callback threw an unknown exception");
            }
        }
        finished_one();
    }

    // ---- Camera previews --------------------------------------------------------------------

    void run_camera(const TaskContext& context, std::uint64_t id, const SourceItem& item,
                    const CameraPreviewRequest& request, const CameraPreviewCallback& callback) {
        Expected<std::shared_ptr<const CameraPreview>> result =
            fail(ErrorCode::Cancelled, "the camera preview was cancelled", std::string(kWhereCamera));
        const CameraKey key{item.id, request.min_long_edge};
        try {
            if (context.cancel().is_cancelled() || request.cancel.is_cancelled()) {
                // Cancelled before it started (shutdown, or the caller no longer wants it).
            } else if (auto cached = camera_cache.find(key)) {
                result = std::move(cached);
            } else {
                raw::EmbeddedPreviewOptions extract = options.camera_preview;
                extract.min_long_edge = request.min_long_edge;
                auto embedded = raw::extract_embedded_preview(item.path, extract, context.cancel());
                if (!embedded) {
                    result = Unexpected(std::move(embedded.error()));
                } else if (embedded->source_identity != item.identity) {
                    result =
                        fail(ErrorCode::SourceChanged, "the file changed since it was imported; re-import it",
                             std::string(kWhereCamera));
                } else {
                    auto preview = std::make_shared<CameraPreview>();
                    preview->item = item.id;
                    preview->embedded = std::move(*embedded);
                    std::shared_ptr<const CameraPreview> shared = std::move(preview);
                    camera_cache.insert(key, shared);
                    result = std::move(shared);
                }
            }
        } catch (const std::bad_alloc&) {
            result = fail(ErrorCode::OutOfMemory, "the camera preview ran out of memory",
                          std::string(kWhereCamera));
        }
        {
            std::lock_guard lock(mutex);
            forget(id);
        }
        try {
            callback(item.id, result);
        } catch (const std::exception& e) {
            core::log_error(kWhereCamera, "a camera preview callback threw: {}", e.what());
        } catch (...) {
            core::log_error(kWhereCamera, "a camera preview callback threw an unknown exception");
        }
        finished_one();
    }
};

PreviewService::PreviewService(Library& library, TaskExecutor& executor,
                               std::shared_ptr<const SourceRenderer> renderer,
                               const PreviewServiceOptions& options)
    : state_(std::make_shared<State>(library, executor, std::move(renderer), options)) {
    subscription_ = library.subscribe([weak = std::weak_ptr<State>(state_)](const LibraryChange& change) {
        if (change.kind != LibraryChangeKind::Removed && change.kind != LibraryChangeKind::Refreshed) {
            return;
        }
        if (auto state = weak.lock()) {
            for (const ItemId id : change.items) {
                state->cache.erase_item(id);
                state->camera_cache.erase_item(id);
            }
        }
    });
}

PreviewService::~PreviewService() {
    subscription_.reset();
    std::unique_lock lock(state_->mutex);
    state_->shut_down = true;
    for (const State::Active& a : state_->active) {
        a.handle.cancel();
    }
    state_->idle.wait(lock, [&] { return state_->outstanding == 0; });
}

Status PreviewService::request(const PreviewRequest& request, PreviewCallback callback) {
    if (!callback) {
        return fail(ErrorCode::InvalidArgument, "the preview callback is empty", std::string(kWhere));
    }
    if (!state_->renderer) {
        return fail(ErrorCode::InvalidArgument, "the preview service has no renderer", std::string(kWhere));
    }
    if (auto valid = request.quality.validate(); !valid) {
        return valid;
    }
    if (auto valid = request.settings.validate(); !valid) {
        return valid;
    }
    if (request.generation == 0) {
        return fail(ErrorCode::InvalidArgument, "preview generations start at 1", std::string(kWhere));
    }
    std::optional<SourceItem> item = state_->library.find(request.item);
    if (!item) {
        return fail(ErrorCode::NotFound, std::format("no library item {}", request.item.value),
                    std::string(kWhere));
    }
    try {
        std::lock_guard lock(state_->mutex);
        if (state_->shut_down) {
            return fail(ErrorCode::Cancelled, "the preview service is shutting down", std::string(kWhere));
        }
        State::Channel& channel = state_->channels[request.channel];
        if (request.generation <= channel.latest) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("generation {} is not newer than {} in channel {}", request.generation,
                                    channel.latest, request.channel),
                        std::string(kWhere));
        }
        const std::uint64_t id = state_->next_id++;
        auto submitted =
            state_->executor.submit(Lane::Interactive, [state = state_, id, request, item = std::move(*item),
                                                        callback = std::move(callback),
                                                        delivery = channel.delivery](TaskContext& context) {
                state->run_preview(context, id, request, item, callback, delivery);
            });
        if (!submitted) {
            return Unexpected(std::move(submitted.error()));
        }
        channel.latest = request.generation;
        for (const State::Active& a : state_->active) {
            if (!a.camera && a.channel == request.channel) {
                a.handle.cancel();
            }
        }
        state_->active.push_back(State::Active{id, request.channel, *submitted, false});
        ++state_->outstanding;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot queue the preview", std::string(kWhere));
    }
    return ok();
}

void PreviewService::cancel_channel(std::uint64_t channel) {
    std::lock_guard lock(state_->mutex);
    const auto it = state_->channels.find(channel);
    if (it == state_->channels.end()) {
        return;
    }
    it->second.cancelled_through = it->second.latest;
    for (const State::Active& a : state_->active) {
        if (!a.camera && a.channel == channel) {
            a.handle.cancel();
        }
    }
}

void PreviewService::invalidate(ItemId item) {
    state_->cache.erase_item(item);
    state_->camera_cache.erase_item(item);
}

Status PreviewService::request_camera_preview(const CameraPreviewRequest& request,
                                              CameraPreviewCallback callback) {
    if (!callback) {
        return fail(ErrorCode::InvalidArgument, "the camera preview callback is empty",
                    std::string(kWhereCamera));
    }
    if (request.min_long_edge < 0) {
        return fail(ErrorCode::InvalidArgument, "the requested camera preview size is negative",
                    std::string(kWhereCamera));
    }
    std::optional<SourceItem> found = state_->library.find(request.item);
    if (!found) {
        return fail(ErrorCode::NotFound, std::format("no library item {}", request.item.value),
                    std::string(kWhereCamera));
    }
    try {
        std::lock_guard lock(state_->mutex);
        if (state_->shut_down) {
            return fail(ErrorCode::Cancelled, "the preview service is shutting down",
                        std::string(kWhereCamera));
        }
        const std::uint64_t id = state_->next_id++;
        auto submitted =
            state_->executor.submit(Lane::Quick, [state = state_, id, source = std::move(*found), request,
                                                  callback = std::move(callback)](TaskContext& context) {
                state->run_camera(context, id, source, request, callback);
            });
        if (!submitted) {
            return Unexpected(std::move(submitted.error()));
        }
        state_->active.push_back(State::Active{id, 0, *submitted, true});
        ++state_->outstanding;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot queue the camera preview", std::string(kWhereCamera));
    }
    return ok();
}

PreviewCacheStats PreviewService::cache_stats() const {
    return state_->cache.stats();
}

}  // namespace lucent::app
