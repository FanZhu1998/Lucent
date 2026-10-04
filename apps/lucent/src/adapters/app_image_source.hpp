#pragma once

// AppImageSource: the shell's ImageSource (services/image_source.hpp) over the application
// services' PreviewService (docs/adr/0007).
//
// Routes (by ImageRequest kind and generation):
//   Thumbnail, generation 1   The file's embedded camera JPEG (PreviewService::
//                             request_camera_preview, Quick lane): the smallest preview in the
//                             file that covers the requested box, decoded on this source's
//                             conversion threads at the requested size, with the RAW's
//                             orientation applied and the JPEG's own EXIF orientation ignored.
//                             A file without a usable camera preview fails the reply and reports
//                             CameraPreviewUnavailable, which moves the item to generation 2.
//   Thumbnail, generation 2+  Lucent's own render of the item's current recipe (W6: edited photos
//                             show their edits): from this source's thumbnail cache when it holds
//                             the recipe's settings (filled by Edit previews and thumbnail renders,
//                             keyed by the settings' SHA-256), otherwise PreviewService::
//                             request_rendered_thumbnail (a 1024-pixel Fit render, cached in memory
//                             and on disk by settings, ADR 0010 decision 9), decoded at the
//                             requested size on the conversion threads. These renders run one at a
//                             time and only while no Edit preview is outstanding and interactive
//                             editing has not waited for a render for interactive_quiet_ms
//                             (Backend::interactive()): they never delay the photo the user is looking
//                             at by more than one render, and a thumbnail render (a full-resolution
//                             render) does not compete with the next slider drag for the processor.
//   Preview                   Lucent's render for the Edit view (PreviewService::request,
//                             Interactive lane), Fit to the requested box or Full when the box is
//                             the photo's full size (conversions.hpp, preview_quality_for).
//
// Generations and staleness. All Edit previews share one PreviewService channel; each request
// gets the next channel generation, so a newer request (another photo, another zoom) cancels the
// older ones and the service reports them Superseded: a late result can never replace a newer
// one (blueprint section 4). The shell's own generations (in the image URL) select what is shown;
// the preview generation is used as the settings revision tag; every render uses the item's
// current recipe (PreviewService resolves it when the request is made).
//
// Cancellation. When the shell cancels the reply of the newest Edit preview (the view closed or
// moved on), the channel is cancelled, which stops the render cooperatively. Cancelled replies of
// queued thumbnail renders are dropped before they start, and camera previews whose reply was
// cancelled (a card scrolled out of view) read nothing if they have not started.
//
// Incomplete renders. A render that lacks a mandatory interpretation step (RenderInfo::
// incomplete: the file is Unsupported) is delivered to the Edit view, which marks it, but never
// replaces a camera-preview thumbnail: the camera's own rendering stays in the contact sheet and
// filmstrip, labelled.
//
// Changed files. When the library refreshes an item after its file changed on disk, this
// source drops its thumbnail and rendered size for the item (it subscribes to the library).
//
// Pixels. Previews are converted to QImage (Format_RGB32) and camera JPEGs decoded on a private
// pool of conversion threads: never on the UI thread and never on the image-loader thread.
//
// Render events go to the RenderEventSink (AppLibraryService): PreviewStarted when an Edit
// preview or thumbnail render is requested, then exactly one of PreviewDelivered,
// PreviewAbandoned or PreviewFailed; CameraPreviewUnavailable as above.
//
// Lifetime. The destructor fails every outstanding reply ("Lucent is closing."), cancels its
// channels, and waits for its conversion threads. Service callbacks that arrive later find
// nothing to do. It must run on a thread that is not an executor worker (the UI thread), since it
// may release the last reference to the Backend.
//
// Thread-safety: request_image() may be called concurrently from any thread.

#include <QObject>
#include <QThreadPool>
#include <cstdint>
#include <memory>

#include <lucent/application/library.hpp>

#include "adapters/app_library_service.hpp"
#include "adapters/backend.hpp"
#include "services/image_source.hpp"

namespace lucent::ui::adapters {

struct ImageSourceOptions {
    int conversion_threads = 2;
    // Thumbnails made from Lucent's renders (Format_RGB32, long edge kThumbnailEdge).
    std::uint64_t thumbnail_cache_bytes = std::uint64_t{48} << 20;
    // Lucent thumbnail renders start once interactive editing has been idle this long.
    int interactive_quiet_ms = 1500;

    static constexpr int kThumbnailEdge = 512;
};

class AppImageSource final : public ImageSource {
public:
    // `events` may be empty.
    AppImageSource(std::shared_ptr<Backend> backend, RenderEventSink events,
                   const ImageSourceOptions& options = {});
    ~AppImageSource() override;

    void request_image(const ImageRequest& request, std::shared_ptr<ImageReply> reply) override;

    struct State;  // implementation detail, shared with in-flight callbacks

private:
    std::shared_ptr<Backend> backend_;
    std::unique_ptr<QObject> timers_;  // lives on the creating (UI) thread: delayed thumbnail starts
    std::unique_ptr<QThreadPool> pool_;
    std::shared_ptr<State> state_;
    app::Subscription subscription_;  // reset first on destruction
};

}  // namespace lucent::ui::adapters
