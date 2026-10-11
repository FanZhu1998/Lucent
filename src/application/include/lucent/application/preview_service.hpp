#pragma once

// Previews of library items for the Edit and Library views (blueprint section 4: "Each preview
// request carries image ID, recipe revision, geometry revision, quality level, and generation
// token. A late result from an earlier slider position must never replace the current preview.
// ... Give interactive jobs reserved capacity").
//
// Lucent previews (request()): the item is rendered by the engine's develop pipeline
// (SourceRenderer::render, the export render: stage one at the settings' white balance, then
// stage two, ADR 0010) on the executor's Interactive lane, under memory admission, and delivered
// as an upright 8-bit sRGB DisplayImage with its histogram and a summary of the render. This is the
// route for photos that are not open for editing (Library browsing, Lucent thumbnails, the first
// Edit view of a photo); interactive editing uses DevelopPreviewService (develop_preview.hpp).
//
//   Settings. A request names the settings to render, or none: the item's current recipe
//   (PreviewServiceOptions::recipes, recipe_service.hpp; "as shot" without a recipe service). They
//   are resolved when request() is called, so a request renders the recipe of that moment; an item
//   whose recipe this build cannot read is refused with the recipe's error (never rendered with
//   other settings). Results carry the recipe identity (settings_sha256 and revision).
//
//   Quality. Full delivers every rendered pixel. Fit renders the full-resolution image, then
//   area-averages the display-referred LINEAR values down to the requested long edge
//   (downscale_area) before the sRGB encoding, so the fit view is the exact downscale of the
//   100% view (no gamma-space averaging, no second, lower-quality render path). The engine has
//   no reduced-resolution decode yet, so a Fit preview costs a full render. Measured on the
//   reference laptop for the 45 MP Canon EOS R5 fixture with the W2 path (W3; the develop path's
//   timings are in ADR 0010): Fit 2048 1.0-1.13 s from request to callback, Full 1.19-1.26 s; a
//   cache hit under 1 ms; the embedded camera JPEG 3 ms.
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
//   Cache. Finished previews are cached by (item, settings SHA-256, quality) in a bounded LRU
//   (PreviewCache); a request that hits it is delivered from the cache (from_cache = true) without
//   rendering. Results that were superseded are still cached (the user may return to those
//   settings). invalidate() drops an item's entries; removing an item from the library, or
//   refreshing it after its file changed (LibraryChangeKind::Refreshed), does so automatically.
//
// Lucent-rendered thumbnails (request_rendered_thumbnail(); blueprint section 2: the camera JPEG
// is "labeled until replaced by Lucent's own render"). An edited photo's thumbnail must show its
// recipe, so it is a Fit render of kRenderedThumbnailEdge pixels with the item's current settings,
// encoded as an sRGB JPEG (kRenderedThumbnailQuality) and cached in memory and, for an item whose
// content hash is known, in the disk ThumbnailCache under rendered_thumbnail_key(): generator
// LucentRender, keyed by the source hash, subimage and a variant digest of the settings SHA-256,
// the develop pipeline version, the decoder build and the lens database the renderer corrects
// lenses with (SourceRenderer::lens_database(), ADR 0023), so a changed recipe, engine or lens
// database never shows an old thumbnail and the next session shows it without rendering. Interactive lane (a
// cache hit costs no render); callers queue them one at a time behind the photo being edited, as the W3
// thumbnails are.
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
#include <string>
#include <string_view>
#include <vector>

#include <lucent/application/library.hpp>
#include <lucent/application/preview.hpp>
#include <lucent/application/preview_cache.hpp>
#include <lucent/application/source_renderer.hpp>
#include <lucent/application/task_executor.hpp>
#include <lucent/application/thumbnail_cache.hpp>
#include <lucent/catalog/types.hpp>
#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/engine/develop_settings.hpp>
#include <lucent/raw/embedded_preview.hpp>

namespace lucent::app {

class RecipeService;

struct PreviewRequest {
    ItemId item;
    // Caller's tag for the settings (for example the shell's image generation), returned in the
    // result; not part of the cache key.
    std::uint64_t settings_revision = 0;
    // The settings to render; std::nullopt: the item's current recipe (see the file comment).
    std::optional<engine::DevelopSettings> settings;
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
    std::uint64_t settings_revision = 0;  // the request's tag
    core::Digest256 settings_sha256{};    // identity of the settings rendered
    std::optional<catalog::RevisionId>
        recipe_revision;  // their revision (none: explicit settings or as shot)
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

// ---- Lucent-rendered thumbnails ----------------------------------------------------------------

// Long edge of Lucent-rendered thumbnails (the size camera thumbnails are cached at,
// source_maintenance.hpp) and their JPEG quality.
inline constexpr std::int32_t kRenderedThumbnailEdge = 1024;
inline constexpr std::int32_t kRenderedThumbnailQuality = 85;
// Version of the bytes ThumbnailGenerator::LucentRender entries hold: 1, a Fit render of
// kRenderedThumbnailEdge (develop pipeline, downscale_area, encode_display_srgb8) written by the
// exporter's JPEG writer at kRenderedThumbnailQuality with the sRGB profile. Bump when that
// changes.
inline constexpr std::uint32_t kRenderedThumbnailGeneratorVersion = 1;

// The disk-cache key of the Lucent-rendered thumbnail of image `subimage` of the file with content
// `source_sha256`, rendered with the settings whose canonical document has `settings_sha256`, by
// the decoder build `decoder_version` (ProbeSummary::decoder + " " + decoder_version) and the lens
// database `lens_database` (lens_database_key()). The variant is the SHA-256 of
// "lucent.rendered-thumbnail", the settings SHA-256 (which covers the recipe's operation and
// base-look versions), every engine::kDevelopEngineVersions entry (the engine's other algorithm
// versions, so an engine change never shows an old thumbnail), the decoder text and the lens
// database text (the database is data that changes pixels, ADR 0023).
[[nodiscard]] ThumbnailKey rendered_thumbnail_key(const core::Digest256& source_sha256,
                                                  std::uint32_t subimage,
                                                  const core::Digest256& settings_sha256,
                                                  std::string_view decoder_version,
                                                  std::string_view lens_database);

// The lens database's part of rendered_thumbnail_key(): "lens-database:<asset id> <version>
// <manifest SHA-256>", or "lens-database:none" without a database.
[[nodiscard]] std::string lens_database_key(const std::optional<engine::LensDatabaseIdentity>& identity);

struct RenderedThumbnailRequest {
    ItemId item;
    std::optional<engine::DevelopSettings> settings;  // std::nullopt: the item's current recipe
    // Checked when the task starts: a request cancelled by then renders nothing (Cancelled).
    core::CancellationToken cancel;
};

struct RenderedThumbnail {
    ItemId item;
    core::Digest256 settings_sha256{};
    std::optional<catalog::RevisionId> recipe_revision;
    Thumbnail jpeg;                // upright sRGB JPEG, long edge at most kRenderedThumbnailEdge
    bool from_disk_cache = false;  // read from the disk ThumbnailCache (no render)
};

using RenderedThumbnailCallback =
    std::function<void(ItemId, const Expected<std::shared_ptr<const RenderedThumbnail>>&)>;

struct PreviewServiceOptions {
    std::uint64_t cache_bytes = std::uint64_t{384} << 20;  // Lucent previews
    // Embedded camera JPEGs and Lucent-rendered thumbnails (each kind its own LRU of this size).
    std::uint64_t camera_cache_bytes = std::uint64_t{64} << 20;
    raw::EmbeddedPreviewOptions camera_preview;
    bool parallel = true;  // parallel downscaling and display encoding (results do not depend on it)
    // On-disk cache of camera and Lucent-rendered thumbnails (see the file comment); null: not used.
    std::shared_ptr<ThumbnailCache> thumbnails;
    // The items' recipes, for requests without settings; null: such requests render as shot. Must
    // outlive the service.
    RecipeService* recipes = nullptr;
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
    // the recipe's error when the item's current recipe cannot be read; Cancelled when the service
    // or the executor is shutting down.
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

    // Queues the item's Lucent-rendered thumbnail (see the file comment). Errors as for request()
    // (InvalidArgument for a null callback or invalid settings, NotFound, the recipe's error,
    // Cancelled). The result fails with the render's error, or Cancelled.
    [[nodiscard]] Status request_rendered_thumbnail(const RenderedThumbnailRequest& request,
                                                    RenderedThumbnailCallback callback);

    [[nodiscard]] PreviewCacheStats cache_stats() const;

    struct State;  // implementation detail

private:
    std::shared_ptr<State> state_;
    Subscription subscription_;
};

}  // namespace lucent::app
