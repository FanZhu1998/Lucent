#include "imaging/image_provider.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <utility>

#include <QCoreApplication>
#include <QMetaObject>
#include <QQmlEngine>
#include <QQuickTextureFactory>

#include "imaging/image_id.hpp"

namespace lucent::ui {
namespace {

QString tr_provider(const char* text) {
    return QCoreApplication::translate("lucent::ui::AsyncImageProvider", text);
}

// One request. Lives on the thread that called requestImageResponse() (Qt Quick's image-loader
// thread), which runs an event loop; Qt Quick deletes it after finished().
//
// Completion path: the source completes the shared ImageReply on any thread; the reply's listener
// posts resolve() to this object's thread (a queued call is dropped if this object is deleted
// first); resolve() decides the outcome and emits finished(). The destructor detaches the
// listener, which waits for a listener already running on another thread, so no call can reach a
// deleted response.
class AsyncImageResponse final : public QQuickImageResponse {
public:
    AsyncImageResponse(ImageKind kind, std::shared_ptr<const GenerationRegistry> registry)
        : kind_(kind), registry_(std::move(registry)) {}

    ~AsyncImageResponse() override {
        if (reply_) {
            reply_->detach();
        }
    }

    AsyncImageResponse(const AsyncImageResponse&) = delete;
    AsyncImageResponse& operator=(const AsyncImageResponse&) = delete;

    // Hands the request to `source`. Called once, right after construction.
    void start(const ImageRequest& request, ImageSource& source) {
        request_ = request;
        reply_ = std::make_shared<ImageReply>([this] { post_resolve(); });
        try {
            source.request_image(request_, reply_);
        } catch (...) {
            // ImageSource implementations must not throw; contain it rather than lose the request.
            reply_->fail(tr_provider("The image could not be requested."));
        }
    }

    // Finishes without asking the source. Called at most once, instead of start().
    void fail_without_request(QString message) {
        {
            const std::lock_guard lock(mutex_);
            error_ = std::move(message);
        }
        post_resolve();
    }

    QQuickTextureFactory* textureFactory() const override {
        const std::lock_guard lock(mutex_);
        if (!error_.isEmpty() || image_.isNull()) {
            return nullptr;
        }
        return QQuickTextureFactory::textureFactoryForImage(image_);
    }

    QString errorString() const override {
        const std::lock_guard lock(mutex_);
        return error_;
    }

    void cancel() override {
        // Completes a pending reply as Cancelled, which still leads to finished() as Qt Quick
        // requires. A reply that already completed has its resolve() queued anyway.
        if (reply_) {
            reply_->cancel();
        }
    }

private:
    void post_resolve() {
        QMetaObject::invokeMethod(this, [this] { resolve(); }, Qt::QueuedConnection);
    }

    void resolve() {
        if (reply_) {
            resolve_from_reply();
        }
        emit finished();
    }

    void resolve_from_reply() {
        QImage image;
        QString error;
        switch (reply_->state()) {
        case ImageReply::State::Finished:
            if (registry_->is_current(request_.item, kind_, request_.generation)) {
                image = fit_within(reply_->image(), request_.bounding_size);
            } else {
                error = tr_provider("A newer version of this image replaced it.");
            }
            break;
        case ImageReply::State::Failed:
            error = reply_->error();
            if (error.isEmpty()) {
                error = tr_provider("The image could not be produced.");
            }
            break;
        case ImageReply::State::Cancelled:
            error = tr_provider("The image request was cancelled.");
            break;
        case ImageReply::State::Pending:
            error = tr_provider("The image request ended without a result.");
            break;
        }
        const std::lock_guard lock(mutex_);
        image_ = std::move(image);
        error_ = std::move(error);
    }

    const ImageKind kind_;
    const std::shared_ptr<const GenerationRegistry> registry_;
    ImageRequest request_;
    std::shared_ptr<ImageReply> reply_;

    mutable std::mutex mutex_;
    QImage image_;
    QString error_;
};

}  // namespace

AsyncImageProvider::AsyncImageProvider(ImageKind kind, std::shared_ptr<ImageSource> source,
                                       std::shared_ptr<const GenerationRegistry> registry)
    : kind_(kind), source_(std::move(source)), registry_(std::move(registry)) {}

QQuickImageResponse* AsyncImageProvider::requestImageResponse(const QString& id, const QSize& requested_size) {
    auto* response = new AsyncImageResponse(kind_, registry_);
    const std::optional<ImageKey> key = parse_image_id(id);
    if (!key) {
        response->fail_without_request(tr_provider("Invalid image address."));
        return response;
    }
    const std::optional<Generation> current = registry_->current(key->item, kind_);
    if (!current) {
        response->fail_without_request(tr_provider("This photo is not in the library."));
        return response;
    }
    if (*current != key->generation) {
        response->fail_without_request(tr_provider("A newer version of this image replaced it."));
        return response;
    }
    response->start(ImageRequest{kind_, key->item, key->generation, clamp_bounding_size(requested_size)},
                    *source_);
    return response;
}

void install_image_providers(QQmlEngine& engine, const std::shared_ptr<ImageSource>& source,
                             const std::shared_ptr<const GenerationRegistry>& registry) {
    engine.addImageProvider(provider_id(ImageKind::Thumbnail),
                            new AsyncImageProvider(ImageKind::Thumbnail, source, registry));
    engine.addImageProvider(provider_id(ImageKind::Preview),
                            new AsyncImageProvider(ImageKind::Preview, source, registry));
}

QSize clamp_bounding_size(QSize requested) noexcept {
    return QSize(std::min(requested.width(), kMaxRequestedSide), std::min(requested.height(), kMaxRequestedSide));
}

QImage fit_within(QImage image, QSize bounding) {
    if (image.isNull()) {
        return image;
    }
    constexpr double kUnconstrained = std::numeric_limits<double>::infinity();
    const double scale_x = bounding.width() > 0 ? double(bounding.width()) / image.width() : kUnconstrained;
    const double scale_y = bounding.height() > 0 ? double(bounding.height()) / image.height() : kUnconstrained;
    const double scale = std::min(scale_x, scale_y);
    if (!(scale < 1.0)) {
        return image;
    }
    const int width = std::max(1, static_cast<int>(std::lround(image.width() * scale)));
    const int height = std::max(1, static_cast<int>(std::lround(image.height() * scale)));
    return image.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

}  // namespace lucent::ui
