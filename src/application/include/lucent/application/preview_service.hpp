#pragma once

// Previews of library items for the Edit and Library views (blueprint section 4: "Each preview
// request carries image ID, recipe revision, geometry revision, quality level, and generation
// token. A late result from an earlier slider position must never replace the current preview.
// ... Give interactive jobs reserved capacity").
//
// Lucent previews (request()): the item is rendered by the engine (SourceRenderer) on the
// executor's Interactive lane, under memory admission, and delivered as an upright 8-bit sRGB
// DisplayImage with its histogram and a summary of the render.
//
//   Quality. Full delivers every rendered pixel. Fit renders the full-resolution image, then
//   area-averages the display-referred LINEAR values down to the requested long edge
//   (downscale_area) before the sRGB encoding, so the fit view is the exact downscale of the
//   100% view (no gamma-space averaging, no second, lower-quality render path). The engine has
//   no reduced-resolution decode yet, so a Fit preview costs a full render. Measured on the
//   reference laptop for the 45 MP Canon EOS R5 fixture (lucent_application_fixture_tests,
//   three runs): Fit 2048 1.0-1.13 s from request to callback, of which the render is
//   0.9-1.03 s; Full 1.19-1.26 s; a cache hit under 1 ms; the embedded camera JPEG 3 ms. A
//   reduced-resolution (half-size) demosaic route is the obvious next speed-up for Fit.
//
//   Support status. Items of every status are rendered (the user may want to look at an
//   Unsupported file); the RenderSummary carries the interpretation after unpacking, so the view
//   can flag what was not applied.
//
//   Channels and generations. Requests are grouped into caller-chosen channels (for example one
//   per view). Within a channel, generations must strictly increase; the newest request is the
//   current one. A new request cancels every older request of its channel, queued or running
//   (cooperatively, through its token). A result is delivered as Delivered only if its
//   generation is still the newest of its channel at delivery time; otherwise its callback
//   reports Superseded (without an image). Callbacks of one channel never run concurrently, so a
//   delivered image is never followed by an older one: stale results never replace current
//   ones.
//
//   Cache. Finished previews are cached by (item, settings revision, quality) in a bounded LRU
//   (PreviewCache); a request that hits it is delivered from the cache (from_cache = true) without
//   rendering. Results that were superseded are still cached (the user may return to that
//   revision). invalidate() drops an item's entries; removing an item from the library, or
//   refreshing it after its file changed (LibraryChangeKind::Refreshed), does so automatically.
//
// Camera previews (request_camera_preview()): the file's embedded camera JPEG
// (raw::extract_embedded_preview) on the executor's Quick lane, for immediate browsing: long
// background work (imports, exports) cannot hold them up (task_executor.hpp). A request names the
// size it is wanted at, so a thumbnail reads a small preview when the file has one, and may carry
// a cancellation token so that requests for thumbnails scrolled out of view are dropped before
// they read anything. The result must be labelled as a camera preview and is never used as
// analysis input (embedded_preview.hpp).
//
// Disk thumbnail cache (W4, PreviewServiceOptions::thumbnails; blueprint section 10: "immediate
// cached thumbnails"). For an item whose content hash is known, a camera preview requested at up
// to kCachedCameraPreviewEdge pixels is first looked up in the on-disk ThumbnailCache under
// camera_thumbnail_key(hash, subimage) (source_maintenance.hpp): a hit is delivered without
// opening the source file (CameraPreview::from_disk_cache), so a library loaded at start-up shows
// its contact sheet before any source I/O, and a Missing file keeps its thumbnail. On a miss the
// file's preview is chosen for kCachedCameraPreviewEdge (the smallest covering it, so one entry
// serves every display scale up to 200 %), delivered, and stored in the disk cache. The
// orientation applied to a cached JPEG is the item's probe orientation, the same RAW orientation
// the file would give. Items without a hash use the W3 route only.
//
// Callbacks: every accepted request's callback is called exactly once, on an executor worker
// thread (never inside request()), after the service no longer needs the request. Callbacks
// should return quickly (post the result to the UI thread); they may call request(),
// cancel_channel() and invalidate(), but must not destroy the service. A request that is
// rejected synchronously (request() returns an error) never calls its callback.
//
// Lifetime: the library and the executor must outlive the service. The destructor cancels all
// outstanding requests and waits until their callbacks have returned.
//
// Thread-safety: all members are safe to call concurrently.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <lucent/application/library.hpp>
#include <lucent/application/preview.hpp>
#include <lucent/application/preview_cache.hpp>
#include <lucent/application/source_renderer.hpp>
#include <lucent/application/task_executor.hpp>
#include <lucent/application/thumbnail_cache.hpp>
#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/render.hpp>
#include <lucent/raw/embedded_preview.hpp>

namespace lucent::app {

struct PreviewRequest {
    ItemId item;
    std::uint64_t settings_revision = 0;  // identifies `settings` for this item (cache key)
    engine::RenderSettings settings;
    PreviewQuality quality;
    std::uint64_t channel = 0;     // supersession scope, chosen by the caller
    std::uint64_t generation = 0;  // strictly increasing per channel, >= 1
};

enum class PreviewOutcome : std::uint8_t {
    Delivered = 1,   // `preview` is the current preview of its channel
    Superseded = 2,  // a newer request of the channel replaced this one; no image
    Cancelled = 3,   // cancelled by cancel_channel() or service shutdown; no image
    Failed = 4,      // `error` says why; no image
};

[[nodiscard]] std::string_view to_string(PreviewOutcome outcome) noexcept;

struct PreviewResult {
    PreviewOutcome outcome = PreviewOutcome::Failed;
    ItemId item;
    std::uint64_t channel = 0;
    std::uint64_t generation = 0;
    std::uint64_t settings_revision = 0;
    PreviewQuality quality;
    std::shared_ptr<const PreviewImage> preview;  // Delivered only
    bool from_cache = false;                      // Delivered from the cache without rendering
    std::optional<Error> error;                   // Failed (and the cause of Superseded/Cancelled when known)
};

using PreviewCallback = std::function<void(const PreviewResult&)>;

struct CameraPreview {
    ItemId item;
    raw::EmbeddedPreviewResult embedded;  // status, the JPEG (when Found) and every candidate
    // Read from the disk thumbnail cache, not from the source file (no candidates are listed).
    bool from_disk_cache = false;
};

using CameraPreviewCallback =
    std::function<void(ItemId, const Expected<std::shared_ptr<const CameraPreview>>&)>;

struct CameraPreviewRequest {
    ItemId item;
    // Long edge, in pixels, the preview is wanted at (raw::EmbeddedPreviewOptions::min_long_edge):
    // the smallest preview of the file that covers it; 0 for the largest. Part of the cache key.
    std::int32_t min_long_edge = 0;
    // The caller's cancellation. Checked when the task starts: a request cancelled by then reads
    // nothing and reports Cancelled. An extraction already running is short and finishes.
    core::CancellationToken cancel;
};

struct PreviewServiceOptions {
    std::uint64_t cache_bytes = std::uint64_t{384} << 20;        // Lucent previews
    std::uint64_t camera_cache_bytes = std::uint64_t{64} << 20;  // embedded camera JPEGs
    raw::EmbeddedPreviewOptions camera_preview;
    bool parallel = true;  // parallel downscaling and display encoding (results do not depend on it)
    // On-disk cache of camera thumbnails (see the file comment); null: not used.
    std::shared_ptr<ThumbnailCache> thumbnails;
};

class PreviewService {
public:
    // With a null `renderer` every request() fails with InvalidArgument.
    PreviewService(Library& library, TaskExecutor& executor, std::shared_ptr<const SourceRenderer> renderer,
                   const PreviewServiceOptions& options = {});
    ~PreviewService();
    PreviewService(const PreviewService&) = delete;
    PreviewService& operator=(const PreviewService&) = delete;

    // Queues a preview (see the file comment). Errors, with the callback never called
    // ("app.preview"): InvalidArgument for a null callback, an invalid quality or settings, or a
    // generation that is 0 or not newer than the channel's newest; NotFound for an unknown item;
    // Cancelled when the service or the executor is shutting down.
    [[nodiscard]] Status request(const PreviewRequest& request, PreviewCallback callback);

    // Cancels every outstanding request of `channel` (their callbacks report Cancelled). Later
    // requests of the channel still need newer generations.
    void cancel_channel(std::uint64_t channel);

    // Drops cached previews (Lucent and camera) of `item`.
    void invalidate(ItemId item);

    // Queues extraction of the item's embedded camera JPEG on the Quick lane (cached by item and
    // requested size). The result fails with SourceChanged when the file no longer has the item's
    // identity, and with Cancelled when request.cancel was cancelled before the task started.
    // Errors as for request() (InvalidArgument for a null callback or a negative size, NotFound,
    // Cancelled).
    [[nodiscard]] Status request_camera_preview(const CameraPreviewRequest& request,
                                                CameraPreviewCallback callback);

    [[nodiscard]] PreviewCacheStats cache_stats() const;

    struct State;  // implementation detail

private:
    std::shared_ptr<State> state_;
    Subscription subscription_;
};

}  // namespace lucent::app
