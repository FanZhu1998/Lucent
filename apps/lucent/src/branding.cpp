#include "branding.hpp"

#include <algorithm>

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QQmlEngine>

namespace lucent::ui {

namespace {

QString role_folder(const QString& role) {
    return QStringLiteral(":/lucent/branding/") + role;
}

}  // namespace

QList<BrandingImage> branding_images(const QString& role) {
    QList<BrandingImage> images;
    // A role is one folder name: anything else (an empty id, a path) names no images.
    if (role.isEmpty() || role.contains(u'/') || role.contains(u'\\') || role.startsWith(u'.')) {
        return images;
    }
    const QDir folder(role_folder(role));
    for (const QFileInfo& file : folder.entryInfoList({QStringLiteral("*.png")}, QDir::Files, QDir::Name)) {
        const QSize size = QImageReader(file.filePath()).size();
        if (size.isValid()) {
            images.append({file.filePath(), size});
        }
    }
    std::stable_sort(images.begin(), images.end(), [](const BrandingImage& a, const BrandingImage& b) {
        return a.size.width() * a.size.height() < b.size.width() * b.size.height();
    });
    return images;
}

QImage branding_image(const QString& role, QSize requested) {
    const QList<BrandingImage> images = branding_images(role);
    if (images.isEmpty()) {
        return {};
    }
    const int wanted = std::max(requested.width(), requested.height());
    const BrandingImage* chosen = &images.back();
    if (wanted > 0) {
        for (const BrandingImage& image : images) {
            if (std::min(image.size.width(), image.size.height()) >= wanted) {
                chosen = &image;
                break;
            }
        }
    }
    QImage image(chosen->path);
    if (image.isNull() || wanted <= 0 || std::max(image.width(), image.height()) <= wanted) {
        return image;
    }
    return image.scaled(QSize(wanted, wanted), Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QIcon branding_icon() {
    QIcon icon;
    for (const BrandingImage& image : branding_images(QString::fromLatin1(kBrandingAppIconRole))) {
        icon.addFile(image.path, image.size);
    }
    return icon;
}

BrandingImageProvider::BrandingImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage BrandingImageProvider::requestImage(const QString& id, QSize* size, const QSize& requested_size) {
    QImage image = branding_image(id, requested_size);
    if (size != nullptr) {
        *size = image.size();
    }
    return image;
}

void install_branding(QQmlEngine& engine) {
    QGuiApplication::setWindowIcon(branding_icon());
    engine.addImageProvider(QString::fromLatin1(kBrandingProviderId), new BrandingImageProvider);
}

}  // namespace lucent::ui
