#include "imaging/image_id.hpp"

#include <limits>

namespace lucent::ui {
namespace {

// Strict unsigned decimal: 1..20 digits, no leading zero, fits in 64 bits, non-zero.
std::optional<std::uint64_t> parse_positive(QStringView text) {
    if (text.isEmpty() || text.size() > 20 || text.front() == u'0') {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    for (const QChar c : text) {
        if (c < u'0' || c > u'9') {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(c.unicode() - u'0');
        if (value > (kMax - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return value;
}

}  // namespace

QString provider_id(ImageKind kind) {
    switch (kind) {
    case ImageKind::Thumbnail:
        return QStringLiteral("thumbnail");
    case ImageKind::Preview:
        return QStringLiteral("preview");
    }
    return {};
}

QString image_id(ItemId item, Generation generation) {
    if (item == 0 || generation == kNoImage) {
        return {};
    }
    return QString::number(item) + u'/' + QString::number(generation);
}

QString image_url(ImageKind kind, ItemId item, Generation generation) {
    const QString id = image_id(item, generation);
    if (id.isEmpty()) {
        return {};
    }
    return QStringLiteral("image://") + provider_id(kind) + u'/' + id;
}

std::optional<ImageKey> parse_image_id(QStringView id) {
    const qsizetype slash = id.indexOf(u'/');
    if (slash < 0) {
        return std::nullopt;
    }
    const auto item = parse_positive(id.first(slash));
    const auto generation = parse_positive(id.sliced(slash + 1));
    if (!item || !generation) {
        return std::nullopt;
    }
    return ImageKey{*item, *generation};
}

}  // namespace lucent::ui
