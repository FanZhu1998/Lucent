#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <list>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <process.h>

#include <lucent/application/display_image.hpp>
#include <lucent/application/preview_service.hpp>
#include <lucent/application/recipe.hpp>
#include <lucent/application/recipe_service.hpp>
#include <lucent/application/source_maintenance.hpp>
#include <lucent/core/file.hpp>
#include <lucent/core/log.hpp>
#include <lucent/core/storage.hpp>
#include <lucent/engine/develop.hpp>
#include <lucent/export/encode.hpp>
#include <lucent/export/export.hpp>

namespace lucent::app {

namespace {

constexpr std::string_view kWhere = "app.preview";
constexpr std::string_view kWhereCamera = "app.preview.camera";
constexpr std::string_view kWhereRendered = "app.preview.rendered_thumbnail";

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

// Key of a Lucent-rendered thumbnail: the item and the settings.
struct RenderedKey {
    ItemId item;
    core::Digest256 settings{};
    friend bool operator==(const RenderedKey&, const RenderedKey&) = default;
};

struct RenderedKeyHash {
    std::size_t operator()(const RenderedKey& key) const noexcept {
        std::uint64_t digest = 0;
        std::memcpy(&digest, key.settings.data(), sizeof(digest));
        return ItemIdHash{}(key.item) * 31u + std::hash<std::uint64_t>{}(digest);
    }
};

std::uint64_t entry_bytes(const CameraPreview& preview) {
    return 1024 + (preview.embedded.preview ? preview.embedded.preview->bytes.size() : 0);
}

std::uint64_t entry_bytes(const RenderedThumbnail& thumbnail) {
    return 1024 + thumbnail.jpeg.jpeg.size();
}

// Bounded LRU of shared immutable values by key (the JPEG bytes dominate the size).
template <class Key, class Hash, class Value>
class SharedLru {
public:
    explicit SharedLru(std::uint64_t capacity) : capacity_(capacity) {}

    std::shared_ptr<const Value> find(const Key& key) {
        std::lock_guard lock(mutex_);
        const auto it = index_.find(key);
        if (it == index_.end()) {
            return nullptr;
        }
        lru_.splice(lru_.begin(), lru_, it->second);
        return it->second->value;
    }

    void insert(const Key& key, std::shared_ptr<const Value> value) {
        const std::uint64_t bytes = entry_bytes(*value);
        std::lock_guard lock(mutex_);
        if (bytes > capacity_) {
            return;
        }
        try {
            erase_locked(key);
            lru_.push_front(Entry{key, std::move(value), bytes});
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

    // Drops every entry of `item`.
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
        Key key;
        std::shared_ptr<const Value> value;
        std::uint64_t bytes = 0;
    };

    void erase_locked(const Key& key) {
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
    std::unordered_map<Key, typename std::list<Entry>::iterator, Hash> index_;
};

using CameraCache = SharedLru<CameraKey, CameraKeyHash, CameraPreview>;
using RenderedCache = SharedLru<RenderedKey, RenderedKeyHash, RenderedThumbnail>;

// Settings resolved for a request: what to render and its identity.
struct Resolved {
    engine::DevelopSettings settings;
    RecipeDocument document;
    std::optional<catalog::RevisionId> revision;
};

std::string decoder_text(const SourceItem& item) {
    return item.probe ? item.probe->decoder + " " + item.probe->decoder_version : std::string();
}

// The JPEG bytes of an 8-bit sRGB image, written by the exporter (write_verified: the same JPEG
// writer, sRGB profile and verification as exports) to a scratch file in `scratch` and read back.
Expected<Thumbnail> encode_thumbnail_jpeg(const DisplayImage& image, const std::filesystem::path& scratch,
                                          const core::CancellationToken& cancel) {
    static std::atomic<std::uint64_t> counter{0};
    exporter::OutputImage output;
    output.width = image.width;
    output.height = image.height;
    output.bit_depth = exporter::BitDepth::Eight;
    output.profile = exporter::OutputProfile::Srgb;
    output.samples8 = image.rgb;
    exporter::ExportSettings settings = exporter::ExportSettings::jpeg_srgb_q95();
    settings.jpeg.quality = kRenderedThumbnailQuality;
    if (auto created = core::create_directory_tree(scratch); !created) {
        return Unexpected(std::move(created.error()));
    }
    const std::filesystem::path path =
        scratch / std::format(L"rendered-{}-{}.jpg", _getpid(), counter.fetch_add(1) + 1);
    auto receipt = exporter::write_verified(output, path, settings, cancel);
    if (!receipt) {
        return Unexpected(std::move(receipt.error()));
    }
    auto bytes = core::read_file_bytes(path, std::uint64_t{64} << 20);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    if (!bytes) {
        return Unexpected(std::move(bytes.error()));
    }
    Thumbnail thumbnail;
    thumbnail.width = image.width;
    thumbnail.height = image.height;
    thumbnail.jpeg = std::move(*bytes);
    return thumbnail;
}

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

ThumbnailKey rendered_thumbnail_key(const core::Digest256& source_sha256, std::uint32_t subimage,
                                    const core::Digest256& settings_sha256,
                                    std::string_view decoder_version) {
    std::vector<std::byte> material;
    const auto append = [&](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::byte*>(data);
        material.insert(material.end(), bytes, bytes + size);
    };
    constexpr std::string_view kTag = "lucent.rendered-thumbnail";
    append(kTag.data(), kTag.size());
    append(settings_sha256.data(), settings_sha256.size());
    const std::uint32_t versions[2] = {engine::kDevelopPipelineVersion, engine::kBoxDownscaleVersion};
    for (const std::uint32_t v : versions) {
        const std::uint8_t le[4] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8),
                                    static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 24)};
        append(le, sizeof(le));
    }
    append(decoder_version.data(), decoder_version.size());
    ThumbnailKey key;
    key.source_sha256 = source_sha256;
    key.subimage = subimage;
    key.generator = ThumbnailGenerator::LucentRender;
    key.generator_version = kRenderedThumbnailGeneratorVersion;
    key.size_class = ThumbnailSizeClass::Large;
    auto variant = core::sha256(material);
    if (variant) {
        key.variant = *variant;
    }
    return key;
}

struct PreviewService::State : std::enable_shared_from_this<State> {
    State(Library& l, TaskExecutor& e, std::shared_ptr<const SourceRenderer> r,
          const PreviewServiceOptions& o)
        : library(l),
          executor(e),
          renderer(std::move(r)),
          options(o),
          cache(o.cache_bytes),
          camera_cache(o.camera_cache_bytes),
          rendered_cache(o.camera_cache_bytes) {}

    struct Channel {
        std::uint64_t latest = 0;             // newest generation requested
        std::uint64_t cancelled_through = 0;  // generations <= this were cancelled by cancel_channel
        std::shared_ptr<std::mutex> delivery = std::make_shared<std::mutex>();  // serialises callbacks
    };
    struct Active {
        std::uint64_t id = 0;
        std::uint64_t channel = 0;
        TaskHandle handle;
        bool channelled = false;  // a request() of `channel` (camera and thumbnail tasks are not)
    };

    Library& library;
    TaskExecutor& executor;
    std::shared_ptr<const SourceRenderer> renderer;
    PreviewServiceOptions options;
    PreviewCache cache;
    CameraCache camera_cache;
    RenderedCache rendered_cache;

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

    // The settings of a request (see the header): explicit ones, else the item's recipe.
    Expected<Resolved> resolve(ItemId item, const std::optional<engine::DevelopSettings>& settings) const {
        try {
            if (settings) {
                auto document = write_recipe(*settings);  // validates
                if (!document) {
                    return Unexpected(std::move(document.error()));
                }
                return Resolved{*settings, std::move(*document), std::nullopt};
            }
            if (options.recipes != nullptr) {
                auto current = options.recipes->current_settings(item);
                if (!current) {
                    return Unexpected(std::move(current.error()));
                }
                return Resolved{std::move(current->settings), std::move(current->document),
                                current->revision};
            }
            auto document = write_recipe(engine::DevelopSettings{});
            if (!document) {
                return Unexpected(std::move(document.error()));
            }
            return Resolved{engine::DevelopSettings{}, std::move(*document), std::nullopt};
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "cannot resolve the settings", std::string(kWhere));
        }
    }

    // ---- Lucent previews --------------------------------------------------------------------

    Expected<std::shared_ptr<const PreviewImage>> produce(const TaskContext& context, const SourceItem& item,
                                                          const Resolved& resolved, PreviewQuality quality,
                                                          std::chrono::steady_clock::time_point start) {
        const core::CancellationToken& cancel = context.cancel();
        engine::StageOneRequest export_request;
        export_request.kind = engine::StageOneKind::Export;
        export_request.white_balance = resolved.settings.white_balance;
        auto estimate = renderer->estimate(item, export_request, cancel);
        if (!estimate) {
            return Unexpected(std::move(estimate.error()));
        }
        // The engine's estimate covers its pixel buffers; the preview adds the 8-bit output (and,
        // for Fit, the small downscaled float image).
        const image::Size upright = item.probe ? item.probe->upright_size : image::Size{};
        std::uint64_t extra = 0;
        if (quality.kind == PreviewQualityKind::Full) {
            extra = pixel_bytes(upright, 3);
        } else {
            extra = pixel_bytes(fit_within(upright, quality.long_edge), 15);
        }
        const std::uint64_t admitted = estimate->export_peak + extra;
        auto lease = context.admit(admitted);
        if (!lease) {
            return Unexpected(std::move(lease.error()));
        }

        engine::ColorImage display;
        std::shared_ptr<engine::DevelopReport> report;
        raw::Interpretation interpretation;
        {
            auto rendered = renderer->render(item, resolved.settings, {}, cancel);
            if (!rendered) {
                return Unexpected(std::move(rendered.error()));
            }
            display = std::move(rendered->display);
            interpretation = std::move(rendered->interpretation);
            report = std::make_shared<engine::DevelopReport>(std::move(rendered->report));
        }

        if (quality.kind == PreviewQualityKind::Fit) {
            const image::Size target = fit_within(display.pixels.size(), quality.long_edge);
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
        preview->key = PreviewKey{item.id, resolved.document.sha256, quality};
        preview->image = std::move(*encoded);
        preview->histogram = compute_histogram(preview->image);
        preview->summary = summarize_render(*report, interpretation);
        preview->summary.estimated_peak_bytes = admitted;
        preview->report = std::move(report);
        preview->seconds = seconds_since(start);
        return std::shared_ptr<const PreviewImage>(std::move(preview));
    }

    void run_preview(const TaskContext& context, std::uint64_t id, const PreviewRequest& request,
                     const SourceItem& item, const Resolved& resolved, const PreviewCallback& callback,
                     const std::shared_ptr<std::mutex>& delivery) {
        const auto start = std::chrono::steady_clock::now();
        const PreviewKey key{request.item, resolved.document.sha256, request.quality};
        Expected<std::shared_ptr<const PreviewImage>> result =
            fail(ErrorCode::Cancelled, "the preview was cancelled before it started", std::string(kWhere));
        bool from_cache = false;
        try {
            if (auto cached = cache.find(key)) {
                result = std::move(cached);
                from_cache = true;
            } else if (!context.cancel().is_cancelled()) {
                result = produce(context, item, resolved, request.quality, start);
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
        deliver(id, request, resolved, std::move(result), from_cache, callback, delivery);
    }

    void deliver(std::uint64_t id, const PreviewRequest& request, const Resolved& resolved,
                 Expected<std::shared_ptr<const PreviewImage>> result, bool from_cache,
                 const PreviewCallback& callback, const std::shared_ptr<std::mutex>& delivery) {
        PreviewResult out;
        out.item = request.item;
        out.channel = request.channel;
        out.generation = request.generation;
        out.settings_revision = request.settings_revision;
        out.settings_sha256 = resolved.document.sha256;
        out.recipe_revision = resolved.revision;
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

    // ---- Lucent-rendered thumbnails ------------------------------------------------------------

    std::optional<ThumbnailKey> disk_key(const SourceItem& item, const Resolved& resolved) const {
        if (!options.thumbnails || !item.sha256 || !item.probe) {
            return std::nullopt;
        }
        return rendered_thumbnail_key(*item.sha256, item.subimage, resolved.document.sha256,
                                      decoder_text(item));
    }

    std::filesystem::path scratch_directory() const {
        if (options.thumbnails) {
            return options.thumbnails->directory() / L"render-scratch";
        }
        std::error_code ec;
        std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
        return (ec ? std::filesystem::path(L".") : temp) / L"Lucent" / L"render-scratch";
    }

    Expected<std::shared_ptr<const RenderedThumbnail>> produce_thumbnail(
        const TaskContext& context, const SourceItem& item, const Resolved& resolved,
        const core::CancellationToken& caller) {
        const RenderedKey key{item.id, resolved.document.sha256};
        if (auto cached = rendered_cache.find(key)) {
            return cached;
        }
        const std::optional<ThumbnailKey> on_disk = disk_key(item, resolved);
        if (on_disk) {
            if (std::optional<Thumbnail> stored = options.thumbnails->find(*on_disk)) {
                auto thumbnail = std::make_shared<RenderedThumbnail>();
                thumbnail->item = item.id;
                thumbnail->settings_sha256 = resolved.document.sha256;
                thumbnail->recipe_revision = resolved.revision;
                thumbnail->jpeg = std::move(*stored);
                thumbnail->from_disk_cache = true;
                std::shared_ptr<const RenderedThumbnail> shared = std::move(thumbnail);
                rendered_cache.insert(key, shared);
                return shared;
            }
        }
        if (caller.is_cancelled()) {
            return fail(ErrorCode::Cancelled, "the thumbnail is no longer wanted",
                        std::string(kWhereRendered));
        }
        auto preview = produce(context, item, resolved, PreviewQuality::fit(kRenderedThumbnailEdge),
                               std::chrono::steady_clock::now());
        if (!preview) {
            return Unexpected(std::move(preview.error()));
        }
        // The Fit preview of these settings serves the Library and Edit views too.
        cache.insert(
            PreviewKey{item.id, resolved.document.sha256, PreviewQuality::fit(kRenderedThumbnailEdge)},
            *preview);
        auto jpeg = encode_thumbnail_jpeg((*preview)->image, scratch_directory(), context.cancel());
        if (!jpeg) {
            return Unexpected(std::move(jpeg.error()));
        }
        if (on_disk) {
            // A failure only costs a render in a later session.
            (void)options.thumbnails->insert(*on_disk, *jpeg);
        }
        auto thumbnail = std::make_shared<RenderedThumbnail>();
        thumbnail->item = item.id;
        thumbnail->settings_sha256 = resolved.document.sha256;
        thumbnail->recipe_revision = resolved.revision;
        thumbnail->jpeg = std::move(*jpeg);
        std::shared_ptr<const RenderedThumbnail> shared = std::move(thumbnail);
        rendered_cache.insert(key, shared);
        return shared;
    }

    void run_rendered(const TaskContext& context, std::uint64_t id, const SourceItem& item,
                      const Resolved& resolved, const core::CancellationToken& caller,
                      const RenderedThumbnailCallback& callback) {
        Expected<std::shared_ptr<const RenderedThumbnail>> result =
            fail(ErrorCode::Cancelled, "the thumbnail was cancelled", std::string(kWhereRendered));
        try {
            if (!context.cancel().is_cancelled() && !caller.is_cancelled()) {
                result = produce_thumbnail(context, item, resolved, caller);
            }
        } catch (const std::bad_alloc&) {
            result =
                fail(ErrorCode::OutOfMemory, "the thumbnail ran out of memory", std::string(kWhereRendered));
        } catch (const std::exception& e) {
            result = fail(ErrorCode::Internal, std::format("the thumbnail raised: {}", e.what()),
                          std::string(kWhereRendered));
        }
        {
            std::lock_guard lock(mutex);
            forget(id);
        }
        try {
            callback(item.id, result);
        } catch (const std::exception& e) {
            core::log_error(kWhereRendered, "a thumbnail callback threw: {}", e.what());
        } catch (...) {
            core::log_error(kWhereRendered, "a thumbnail callback threw an unknown exception");
        }
        finished_one();
    }

    // ---- Camera previews --------------------------------------------------------------------

    // A request the disk thumbnail cache serves: the item's content is known and the wanted size
    // is at most what cached entries are chosen for.
    bool disk_cacheable(const SourceItem& item, const CameraPreviewRequest& request) const {
        return options.thumbnails && item.sha256 && item.probe && request.min_long_edge > 0 &&
               request.min_long_edge <= kCachedCameraPreviewEdge;
    }

    // The item's camera preview from the disk thumbnail cache, or null (see the header).
    std::shared_ptr<const CameraPreview> from_disk_cache(const SourceItem& item,
                                                         const CameraPreviewRequest& request) const {
        if (!disk_cacheable(item, request)) {
            return nullptr;
        }
        std::optional<Thumbnail> stored =
            options.thumbnails->find(camera_thumbnail_key(*item.sha256, item.subimage));
        if (!stored) {
            return nullptr;
        }
        auto preview = std::make_shared<CameraPreview>();
        preview->item = item.id;
        preview->from_disk_cache = true;
        preview->embedded.status = raw::EmbeddedPreviewStatus::Found;
        preview->embedded.source_identity = item.identity;
        raw::EmbeddedPreview& embedded = preview->embedded.preview.emplace();
        embedded.format = raw::EmbeddedPreviewFormat::Jpeg;
        embedded.bytes = std::move(stored->jpeg);
        embedded.width = stored->width;
        embedded.height = stored->height;
        embedded.components = 3;
        embedded.orientation = item.probe->orientation;
        return preview;
    }

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
            } else if (auto stored = from_disk_cache(item, request)) {
                camera_cache.insert(key, stored);
                result = std::move(stored);
            } else {
                const bool cacheable = disk_cacheable(item, request);
                raw::EmbeddedPreviewOptions extract = options.camera_preview;
                extract.min_long_edge = cacheable ? kCachedCameraPreviewEdge : request.min_long_edge;
                auto embedded = raw::extract_embedded_preview(item.path, extract, context.cancel());
                if (!embedded) {
                    result = Unexpected(std::move(embedded.error()));
                } else if (embedded->source_identity != item.identity) {
                    result =
                        fail(ErrorCode::SourceChanged, "the file changed since it was imported; re-import it",
                             std::string(kWhereCamera));
                } else {
                    if (cacheable && embedded->preview) {
                        // Reduced for the cache (camera_thumbnail_entry); this request gets the
                        // preview as extracted. A failure only costs a later re-read of the file.
                        if (const std::optional<Thumbnail> entry =
                                camera_thumbnail_entry(*embedded->preview, context.cancel())) {
                            (void)options.thumbnails->insert(
                                camera_thumbnail_key(*item.sha256, item.subimage), *entry);
                        }
                    }
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
                state->rendered_cache.erase_item(id);
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
    if (request.generation == 0) {
        return fail(ErrorCode::InvalidArgument, "preview generations start at 1", std::string(kWhere));
    }
    std::optional<SourceItem> item = state_->library.find(request.item);
    if (!item) {
        return fail(ErrorCode::NotFound, std::format("no library item {}", request.item.value),
                    std::string(kWhere));
    }
    auto resolved = state_->resolve(request.item, request.settings);
    if (!resolved) {
        return Unexpected(std::move(resolved.error()));
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
        auto submitted = state_->executor.submit(
            Lane::Interactive,
            [state = state_, id, request, item = std::move(*item), resolved = std::move(*resolved),
             callback = std::move(callback), delivery = channel.delivery](TaskContext& context) {
                state->run_preview(context, id, request, item, resolved, callback, delivery);
            });
        if (!submitted) {
            return Unexpected(std::move(submitted.error()));
        }
        channel.latest = request.generation;
        for (const State::Active& a : state_->active) {
            if (a.channelled && a.channel == request.channel) {
                a.handle.cancel();
            }
        }
        state_->active.push_back(State::Active{id, request.channel, *submitted, true});
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
        if (a.channelled && a.channel == channel) {
            a.handle.cancel();
        }
    }
}

void PreviewService::invalidate(ItemId item) {
    state_->cache.erase_item(item);
    state_->camera_cache.erase_item(item);
    state_->rendered_cache.erase_item(item);
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
        state_->active.push_back(State::Active{id, 0, *submitted, false});
        ++state_->outstanding;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot queue the camera preview", std::string(kWhereCamera));
    }
    return ok();
}

Status PreviewService::request_rendered_thumbnail(const RenderedThumbnailRequest& request,
                                                  RenderedThumbnailCallback callback) {
    if (!callback) {
        return fail(ErrorCode::InvalidArgument, "the thumbnail callback is empty",
                    std::string(kWhereRendered));
    }
    if (!state_->renderer) {
        return fail(ErrorCode::InvalidArgument, "the preview service has no renderer",
                    std::string(kWhereRendered));
    }
    std::optional<SourceItem> found = state_->library.find(request.item);
    if (!found) {
        return fail(ErrorCode::NotFound, std::format("no library item {}", request.item.value),
                    std::string(kWhereRendered));
    }
    auto resolved = state_->resolve(request.item, request.settings);
    if (!resolved) {
        return Unexpected(std::move(resolved.error()));
    }
    try {
        std::lock_guard lock(state_->mutex);
        if (state_->shut_down) {
            return fail(ErrorCode::Cancelled, "the preview service is shutting down",
                        std::string(kWhereRendered));
        }
        const std::uint64_t id = state_->next_id++;
        auto submitted = state_->executor.submit(
            Lane::Interactive,
            [state = state_, id, source = std::move(*found), resolved = std::move(*resolved),
             caller = request.cancel, callback = std::move(callback)](TaskContext& context) {
                state->run_rendered(context, id, source, resolved, caller, callback);
            });
        if (!submitted) {
            return Unexpected(std::move(submitted.error()));
        }
        state_->active.push_back(State::Active{id, 0, *submitted, false});
        ++state_->outstanding;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot queue the thumbnail", std::string(kWhereRendered));
    }
    return ok();
}

PreviewCacheStats PreviewService::cache_stats() const {
    return state_->cache.stats();
}

}  // namespace lucent::app
