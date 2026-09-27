#pragma once

// ImageSource: produces thumbnails and previews for the shell's asynchronous image providers
// (imaging/image_provider.hpp). The UI never decodes or renders on its thread (blueprint
// section 4); the providers call this interface from Qt Quick's image-loader thread and the
// source does the work wherever it likes (a thread pool, a worker process).
//
// Staleness: every request names an item and an image generation (service_types.hpp). A source
// produces exactly that generation or fails; the provider additionally discards any result whose
// generation is no longer current, so a late image from an older generation is never shown.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

#include <QImage>
#include <QSize>
#include <QString>

#include "services/service_types.hpp"

namespace lucent::ui {

enum class ImageKind : std::uint8_t {
    Thumbnail = 1,  // contact sheet and filmstrip; may be the embedded camera JPEG
    Preview = 2,    // Edit view; Lucent's own render
};

// Largest width or height the providers pass on in ImageRequest::bounding_size.
inline constexpr int kMaxRequestedSide = 16384;

struct ImageRequest {
    ImageKind kind = ImageKind::Thumbnail;
    ItemId item = 0;
    Generation generation = kNoImage;
    // Box, in device pixels, the image should fit in (aspect ratio preserved). A non-positive
    // width or height leaves that side unconstrained; both non-positive asks for the source's
    // natural size. Each side is at most kMaxRequestedSide.
    QSize bounding_size;
};

// Completion slot of one asynchronous image request: a one-shot, thread-safe promise shared by
// the source (which completes it) and the provider (which consumes it).
//
// The first of finish(), fail() and cancel() decides the outcome; later calls change nothing and
// return false. When the outcome is decided the listener given to the constructor is called once,
// on the thread that decided it, unless detach() ran first.
//
// Thread-safety: every member function may be called from any thread.
class ImageReply {
public:
    enum class State : std::uint8_t { Pending, Finished, Failed, Cancelled };

    using Listener = std::function<void()>;

    // `listener` runs while an internal lock is held: it must return quickly and must not call
    // detach() on this reply. It may be empty.
    explicit ImageReply(Listener listener = {});

    ImageReply(const ImageReply&) = delete;
    ImageReply& operator=(const ImageReply&) = delete;

    // --- Source side --------------------------------------------------------------------------

    // Completes with `image`. A null image counts as a failure ("The image is empty.").
    bool finish(QImage image);
    // Completes with a failure; `message` is user-facing and should not be empty.
    bool fail(QString message);
    // True once the consumer no longer needs the image; a source should stop working on it.
    [[nodiscard]] bool is_cancelled() const noexcept { return cancelled_.load(std::memory_order_acquire); }
    // Installs `handler` (replacing an earlier one), which runs once when the consumer cancels
    // the reply: on the thread that calls cancel(), after the reply has completed and outside its
    // locks, so it may call back into the source. When the reply is already cancelled it runs
    // immediately on the calling thread. It must be quick and must not throw. An empty handler
    // removes the current one.
    void on_cancel(std::function<void()> handler);

    // --- Consumer side ------------------------------------------------------------------------

    // Completes the reply as cancelled if it is still pending (and marks it cancelled in any case).
    bool cancel();
    // Drops the listener. When the listener is running on another thread, waits for it to return.
    // Afterwards the listener is never called.
    void detach();

    [[nodiscard]] State state() const;
    // The image when state() == Finished; a null image otherwise.
    [[nodiscard]] QImage image() const;
    // The failure message when state() == Failed; empty otherwise.
    [[nodiscard]] QString error() const;

private:
    bool complete(State state, QImage image, QString error);

    mutable std::mutex state_mutex_;
    State state_ = State::Pending;
    QImage image_;
    QString error_;
    std::atomic<bool> cancelled_{false};

    std::mutex listener_mutex_;  // held while the listener runs and while detaching
    Listener listener_;

    std::mutex cancel_mutex_;  // guards cancel_handler_ (never held while it runs)
    std::function<void()> cancel_handler_;
};

class ImageSource {
public:
    ImageSource() = default;
    virtual ~ImageSource() = default;

    ImageSource(const ImageSource&) = delete;
    ImageSource& operator=(const ImageSource&) = delete;

    // Starts producing the image `request` describes and completes `reply` exactly once, from any
    // thread (possibly before returning).
    //
    // Called concurrently from Qt Quick's image-loader thread (never the UI thread); must be
    // thread-safe and return promptly: queue the work, never decode or render on the calling
    // thread.
    //
    // The image must show exactly request.generation of request.item; fail when that is not
    // possible (for example the generation was superseded, or the photo cannot be rendered, with
    // a user-facing reason). It should fit in request.bounding_size; the provider scales larger
    // images down. Work should stop early once reply->is_cancelled().
    //
    // A source completes (fails) every outstanding reply before its destructor returns.
    virtual void request_image(const ImageRequest& request, std::shared_ptr<ImageReply> reply) = 0;
};

}  // namespace lucent::ui
