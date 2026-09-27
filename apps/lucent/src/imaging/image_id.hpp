#pragma once

// Image URLs of the shell's asynchronous image providers (image_provider.hpp):
//
//   image://thumbnail/<item>/<generation>
//   image://preview/<item>/<generation>
//
// <item> and <generation> are non-zero unsigned 64-bit decimal numbers without sign, spaces or
// leading zeros. Because the generation is part of the URL, a new generation is a new URL: QML
// reloads it, Qt's pixmap cache never returns an older generation for it, and the provider can
// reject a request whose generation is no longer current.

#include <optional>

#include <QString>
#include <QStringView>

#include "services/image_source.hpp"
#include "services/service_types.hpp"

namespace lucent::ui {

struct ImageKey {
    ItemId item = 0;
    Generation generation = kNoImage;

    friend bool operator==(const ImageKey&, const ImageKey&) = default;
};

// Provider ids registered with the QML engine ("thumbnail", "preview").
[[nodiscard]] QString provider_id(ImageKind kind);

// "<item>/<generation>". Both must be non-zero (the result is empty otherwise).
[[nodiscard]] QString image_id(ItemId item, Generation generation);

// "image://<provider>/<item>/<generation>", or an empty string when item is 0 or generation is
// kNoImage (QML then shows no image).
[[nodiscard]] QString image_url(ImageKind kind, ItemId item, Generation generation);

// Parses an image id (the part of the URL after the provider id). std::nullopt for anything that
// image_id() cannot produce.
[[nodiscard]] std::optional<ImageKey> parse_image_id(QStringView id);

}  // namespace lucent::ui
