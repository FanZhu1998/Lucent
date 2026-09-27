#pragma once

// Asynchronous QML image providers for thumbnails and previews (blueprint section 4: the UI never
// decodes on the UI thread; a late result from an earlier generation never replaces the current
// image).
//
// Register one provider per ImageKind with the QML engine under provider_id(kind)
// (install_image_providers() does both). QML then loads image_url(kind, item, generation) URLs
// (image_id.hpp). For each request the provider:
//   1. parses the id; a malformed id fails ("Invalid image address.");
//   2. rejects an unknown item or a generation that is not the item's current one in the
//      GenerationRegistry, without calling the source;
//   3. passes an ImageRequest (bounding box clamped to kMaxRequestedSide) to the ImageSource;
//   4. when the source completes, discards the result if the generation was superseded meanwhile,
//      and scales an image larger than the requested box down (aspect ratio preserved);
//   5. emits QQuickImageResponse::finished() exactly once, always from the response's own thread's
//      event loop (never from inside requestImageResponse()).
// Cancellation by Qt Quick is forwarded to the source through ImageReply::is_cancelled().
//
// Ownership: the QML engine owns the providers (QQmlEngine::addImageProvider). A provider holds
// shared ownership of the source and the registry, so both outlive every request in flight.
//
// Thread-safety: requestImageResponse() is called on Qt Quick's image-loader thread; the
// registry and the source are thread-safe.

#include <memory>

#include <QImage>
#include <QQuickAsyncImageProvider>
#include <QSize>

#include "imaging/generation_registry.hpp"
#include "services/image_source.hpp"

class QQmlEngine;

namespace lucent::ui {

class AsyncImageProvider final : public QQuickAsyncImageProvider {
public:
    AsyncImageProvider(ImageKind kind, std::shared_ptr<ImageSource> source,
                       std::shared_ptr<const GenerationRegistry> registry);

    QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requested_size) override;

    [[nodiscard]] ImageKind kind() const noexcept { return kind_; }

private:
    ImageKind kind_;
    std::shared_ptr<ImageSource> source_;
    std::shared_ptr<const GenerationRegistry> registry_;
};

// Registers a thumbnail and a preview provider with `engine` (which takes ownership).
void install_image_providers(QQmlEngine& engine, const std::shared_ptr<ImageSource>& source,
                             const std::shared_ptr<const GenerationRegistry>& registry);

// Clamps each side of a requested size to kMaxRequestedSide; a non-positive side stays as given
// (unconstrained).
[[nodiscard]] QSize clamp_bounding_size(QSize requested) noexcept;

// Scales `image` down (smooth, aspect ratio preserved) so it fits in `bounding`; a non-positive
// side of `bounding` is unconstrained. Images that already fit are returned unchanged; nothing is
// ever scaled up.
[[nodiscard]] QImage fit_within(QImage image, QSize bounding);

}  // namespace lucent::ui
