#pragma once

// DevelopPreviewService: interactive previews of the photos open for editing (blueprint section 2,
// "Detailed individual editing"; section 4: "Each preview request carries image ID, recipe
// revision, ... and generation token. A late result from an earlier slider position must never
// replace the current preview. Debounce adjustments, display a quick preview, then refine";
// section 7: memory admission; ADR 0010).
//
// Sessions. open() starts an edit session for one item; the session caches the item's stage one
// (engine develop pipeline, develop.hpp: decode, normalise, white balance, demosaic, orientation)
// so that every settings change only runs stage two:
//   * the proxy: an Interactive stage one downscaled to DevelopPreviewOptions::proxy_long_edge
//     (2048) pixels, built when the session opens, balanced with the white balance of the request
//     that triggered the build (highlights not clipped, so stage two can apply any other balance);
//   * the full-resolution stage one, built on demand for a settled 100 % view (PreviewQuality
//     Full) and kept until release_full_resolution(), close(), or a settled request whose white
//     balance it does not match.
//
// Requests and frames. request() takes the settings to show (DevelopView) and returns the request's
// generation (1, 2, 3, ... per session). Frames go to the session's sink (DevelopEvent):
//   * Quick: stage two of the request's settings on the cached proxy. While a stage-two render is
//     running, newer requests replace the waiting one (latest wins): when the render finishes the
//     newest request is rendered next, and the requests in between are never rendered.
//   * Final, after the request has settled (DevelopView::settled; the user interface marks a
//     transient gesture position unsettled and the committed revision settled):
//       - for a Fit request, if the proxy's white balance differs from the settings' by more than the refine
//         tolerance (any camera channel's stage-two correction factor further than
//         DevelopPreviewOptions::refine_tolerance from 1), the proxy is rebuilt at the settings'
//         white balance (refine) and the request is rendered again on it;
//       - for a Full request, the full-resolution stage one at the settings' white balance is
//         built (or reused; its build also replaces the proxy with one at that white balance,
//         instead of a separate refine) and stage two runs at full resolution;
//       - otherwise the Quick frame already is the Final one (no second render).
//   Which differences remain between a Final frame and the export render is stated in develop.hpp
//   ("Preview versus export") and measured in ADR 0010.
// A new request cancels a running full-resolution render and any stage-one rebuild that it makes
// useless (another white balance, or no longer a settled request for it); the proxy renders are
// short and run to completion.
//
// Staleness. Every frame carries the item, its request's generation, settings revision and
// transient generation, and settings SHA-256. Frames of a session are delivered in strictly
// increasing (generation, phase) order, never concurrently: a frame older than one already
// delivered is dropped, so a late result from an earlier slider position never replaces a newer
// one. A frame of an older request that arrives before any frame of the newest one is delivered (it
// is newer than what is shown: the preview keeps following the slider). Failed events are sent for
// the newest generation only.
//
// Overlays (ADR 0015). Every frame carries the geometry of its render (DevelopFrame::geometry: the
// engine::GeometryMapping stage two rendered with, engine::geometry_mapping() of its report: the
// settings' geometry for the upright size of the stage one it was rendered from, i.e. at the frame's
// scale, and the lens correction's point mapping when one was applied, ADR 0023), which maps
// normalised mask points and upright pixels to the frame's pixels and back (pointer positions to
// brush points), and the outlines of its masks (engine::mask_outline()) already mapped to the
// frame's pixels: through the correction, so they stay on the masks' content.
//
// Lens corrections (ADR 0023). The renderer's lens database (SourceRenderer::lens_database())
// corrects every render the settings ask it to; the plan of each frame (the match, the profile and
// every correction's outcome) is in its report (DevelopReport::lens_correction).
//
// Caches (ADR 0015 decisions 5 and 7). Each session owns an engine::MaskRasterCache (brush rasters
// at the proxy's size; a brush raster is made again only when its shape changes) and an
// engine::StageTwoCache (the proxy's scene-linear image after chroma denoising, reused while the
// controls after the denoising move), passed to its proxy renders (Quick and the Fit Final);
// full-resolution renders use neither. The proxy's rebuild drops the denoised image (another stage
// one); a changed file or close() drops both.
//
// Memory (MemoryAdmission, task_executor.hpp). Building a stage one is a full-resolution render
// (TaskContext::admit, Interactive lane, the engine's stage_one_peak). The kept stage one becomes a
// retained claim of its own bytes (the proxy: 2048 x 1366 x 3 floats, about 34 MB; the 45 MP full
// resolution: about 540 MB), so exports see it but it holds no render place. A proxy stage-two
// render claims its working memory as retained memory too: it never waits for a running export.
// Its claim is the renderer's settings-aware stage-two estimate (SourceRenderer::
// estimate_stage_two(): engine::estimate_develop_memory with the settings and the renderer's lens
// database: denoising radius, brush rasters, the masks' base layer over their supports mapped
// through the lens correction, as the render's own check counts them) plus the 8-bit image and the
// clipping mask, and, when the settings use a cache (denoising applied or a brush mask with an
// effect), room for the caches to fill up to their capacities; after the render the caches' bytes
// stay claimed as retained memory of the session (DevelopSessionStats::cache_bytes) and the rest is
// returned. A full-resolution stage-two render is a render claim of the same estimate at full
// resolution. stats() reports each session's memory.
//
// Unedited photos keep the W3 route (PreviewService: camera preview first, then a Lucent render);
// a session is for the photo in the Edit view.
//
// Sinks are called on executor worker threads, one at a time per session. A sink may call
// request() and release_full_resolution() but must not call close() or destroy the service.
//
// Lifetime: the library and the executor must outlive the service; close() and the destructor
// cancel the session's work and return when no sink call is running or will follow.
//
// Thread-safety: all members are safe to call concurrently.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <lucent/application/display_image.hpp>
#include <lucent/application/library.hpp>
#include <lucent/application/preview.hpp>
#include <lucent/application/source_renderer.hpp>
#include <lucent/application/task_executor.hpp>
#include <lucent/catalog/types.hpp>
#include <lucent/core/error.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/engine/develop.hpp>
#include <lucent/engine/develop_settings.hpp>
#include <lucent/engine/local_adjustments.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/raw/decoded_raw.hpp>

namespace lucent::app {

inline constexpr std::int32_t kDefaultProxyLongEdge = 2048;
// Default refine tolerance (see the file comment). A 1500 K white-balance change applied on a stage
// one balanced for another white (stage-two correction factors of about 0.7 to 1.4) differs from
// the refined render by CIEDE2000 p99 < 2.5 on the 2048-pixel proxy (develop_fixture_test.cpp);
// the difference scales with the correction's distance from 1, so 0.5 % keeps it far below a just
// noticeable difference (about 0.03 by linear scaling).
inline constexpr double kDefaultRefineTolerance = 0.005;

enum class DevelopPhase : std::uint8_t {
    Quick = 1,  // stage two on the cached proxy; a Final frame of the same request may follow
    Final = 2,  // the settled result at the requested quality (no further frame for the request)
};

struct DevelopView {
    engine::DevelopSettings settings;
    // Identity of the settings for staleness checks and the user interface (RecipeState): the
    // committed revision (none: as shot or transient) and the transient generation (0: not transient).
    std::optional<catalog::RevisionId> revision;
    std::uint64_t transient_generation = 0;
    // False while a gesture is in progress: no refine.
    bool settled = true;
    // Fit: a frame of the proxy's size (the long edge is not used beyond that); Full: 100 %.
    PreviewQuality quality = PreviewQuality::fit(kDefaultProxyLongEdge);
};

// A mask of the frame's settings for an overlay.
struct MaskOverlay {
    std::uint32_t id = 0;  // engine::LocalAdjustment::id
    engine::MaskKind kind = engine::MaskKind::RadialGradient;
    bool enabled = true;
    bool invert = false;
    // engine::mask_outline() of the shape mapped to the frame: continuous pixel coordinates of
    // DevelopFrame::image (pixel (i, j) covers [i, i + 1) x [j, j + 1)).
    std::vector<std::vector<image::PointD>> polylines;
};

struct DevelopFrame {
    ItemId item;
    std::uint64_t generation = 0;
    std::optional<catalog::RevisionId> revision;
    std::uint64_t transient_generation = 0;
    core::Digest256 settings_sha256{};
    DevelopPhase phase = DevelopPhase::Quick;
    bool full_resolution = false;    // rendered from the full-resolution stage one
    bool stage_one_rebuilt = false;  // a refine rebuilt the stage one this frame was rendered from
    DisplayImage image;              // upright 8-bit sRGB
    Histogram histogram;             // of `image`
    ClippingMask clipping;           // of `image`
    RenderSummary summary;
    std::shared_ptr<const engine::DevelopReport> report;
    // The geometry of this render: engine::geometry_mapping() of its report (the settings' geometry
    // for the upright size of the stage one it was rendered from, and the lens correction's point
    // mapping); its output_size() is the image's size. std::nullopt only when the mapping cannot be
    // made (never for a successful engine render).
    std::optional<engine::GeometryMapping> geometry;
    std::vector<MaskOverlay> masks;  // in the settings' order (disabled ones included)
    double latency_seconds = 0.0;    // request() to frame ready (before the sink is called)
    double stage_two_seconds = 0.0;  // the engine's stage two
    double display_seconds = 0.0;    // 8-bit encoding, histogram and clipping mask
};

struct DevelopEvent {
    enum class Kind : std::uint8_t {
        Frame = 1,
        Failed = 2,
    };
    Kind kind = Kind::Frame;
    ItemId item;
    std::uint64_t generation = 0;
    std::shared_ptr<const DevelopFrame> frame;  // Frame
    std::optional<Error> error;                 // Failed
};

using DevelopSink = std::function<void(const DevelopEvent&)>;

struct DevelopSessionStats {
    ItemId item;
    image::Size proxy_size;            // 0 x 0 while not built
    image::Size full_size;             // of the cached full-resolution stage one (0 x 0: none)
    std::uint64_t proxy_bytes = 0;     // the cached proxy stage one
    std::uint64_t full_bytes = 0;      // the cached full-resolution stage one
    std::uint64_t cache_bytes = 0;     // the proxy caches: brush rasters and the denoised image
    // Claimed by the session's retained leases: proxy + full + the caches' claim (which equals
    // cache_bytes once no render is running).
    std::uint64_t retained_bytes = 0;
    std::uint64_t mask_cache_hits = 0;       // brush rasters reused
    std::uint64_t stage_two_cache_hits = 0;  // denoised images reused
    std::uint64_t requests = 0;
    std::uint64_t stage_one_builds = 0;
    std::uint64_t proxy_renders = 0;
    std::uint64_t full_renders = 0;
    std::uint64_t frames_delivered = 0;
    std::uint64_t frames_dropped = 0;      // finished after a newer frame had been delivered
    std::uint64_t requests_coalesced = 0;  // replaced before they were rendered
};

inline constexpr std::uint64_t kDefaultSessionMaskCacheBytes = std::uint64_t{64} << 20;
inline constexpr std::uint64_t kDefaultSessionStageTwoCacheBytes = std::uint64_t{64} << 20;

struct DevelopPreviewOptions {
    std::int32_t proxy_long_edge = kDefaultProxyLongEdge;  // 64 .. 16384
    double refine_tolerance = kDefaultRefineTolerance;     // >= 0
    // Capacities of a session's proxy caches (see the file comment); 0 disables a cache. The
    // denoised image of a 2048-pixel proxy is 32 MiB, one brush raster at most 11 MiB.
    std::uint64_t mask_cache_bytes = kDefaultSessionMaskCacheBytes;
    std::uint64_t stage_two_cache_bytes = kDefaultSessionStageTwoCacheBytes;
    bool parallel = true;                                  // display encoding (results do not depend on it)
    // Called (on a worker thread) with the metadata of every stage one built, for example to tell
    // RecipeService::note_camera() what the file's calibration supports. Must not throw.
    std::function<void(ItemId, const raw::RawMetadata&)> on_metadata;
};

class DevelopPreviewService {
public:
    // With a null `renderer` every open() fails with InvalidArgument.
    DevelopPreviewService(Library& library, TaskExecutor& executor,
                          std::shared_ptr<const SourceRenderer> renderer, DevelopPreviewOptions options = {});
    ~DevelopPreviewService();
    DevelopPreviewService(const DevelopPreviewService&) = delete;
    DevelopPreviewService& operator=(const DevelopPreviewService&) = delete;

    // Opens a session for `item` delivering to `sink`; nothing is built until the first request().
    // Errors ("app.develop"): InvalidArgument (null sink or renderer, invalid options); NotFound (no
    // such item); AlreadyExists (a session for the item is open); Cancelled (shutting down).
    [[nodiscard]] Status open(ItemId item, DevelopSink sink);

    // Queues `view` (see the file comment) and returns its generation. Errors ("app.develop"):
    // NotFound (no session); InvalidArgument (invalid settings or quality); Cancelled (closing).
    [[nodiscard]] Expected<std::uint64_t> request(ItemId item, const DevelopView& view);

    // Drops the session's full-resolution stage one and its retained memory (the view left 100 %).
    void release_full_resolution(ItemId item);

    // Closes the session: cancels its work, releases its memory, and returns when no sink call of
    // it is running or will follow. Nothing happens for an item without a session.
    void close(ItemId item);

    [[nodiscard]] std::optional<DevelopSessionStats> stats(ItemId item) const;
    [[nodiscard]] std::vector<ItemId> open_items() const;

    struct State;  // implementation detail

private:
    std::shared_ptr<State> state_;
    Subscription subscription_;
};

}  // namespace lucent::app
