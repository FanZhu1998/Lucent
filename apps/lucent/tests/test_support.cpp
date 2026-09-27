#include "test_support.hpp"

#include <QColor>
#include <QImage>

namespace lucent::ui::testing {
namespace {

std::mutex g_capture_mutex;
QStringList g_warnings;
QtMessageHandler g_previous = nullptr;

}  // namespace

MessageCapture::MessageCapture() {
    const std::lock_guard lock(g_capture_mutex);
    g_warnings.clear();
    previous_ = qInstallMessageHandler(&MessageCapture::handle);
    g_previous = previous_;
}

MessageCapture::~MessageCapture() {
    qInstallMessageHandler(previous_);
    const std::lock_guard lock(g_capture_mutex);
    g_previous = nullptr;
}

QStringList MessageCapture::warnings() const {
    const std::lock_guard lock(g_capture_mutex);
    return g_warnings;
}

void MessageCapture::clear() {
    const std::lock_guard lock(g_capture_mutex);
    g_warnings.clear();
}

void MessageCapture::handle(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    QtMessageHandler previous = nullptr;
    {
        const std::lock_guard lock(g_capture_mutex);
        if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
            g_warnings << message;
        }
        previous = g_previous;
    }
    if (previous != nullptr) {
        previous(type, context, message);
    }
}

void SynchronousImageSource::request_image(const ImageRequest&, std::shared_ptr<ImageReply> reply) {
    reply->finish(solid_image(size_));
}

ScriptedServices make_scripted_services() {
    ScriptedServices scripted;
    auto library = std::make_unique<ScriptedLibraryService>();
    auto exports = std::make_unique<RecordingExportService>();
    scripted.library = library.get();
    scripted.exports = exports.get();
    scripted.images = std::make_shared<ManualImageSource>();
    scripted.services.library = std::move(library);
    scripted.services.exports = std::move(exports);
    scripted.services.images = scripted.images;
    return scripted;
}

LibraryItem make_item(ItemId id, const QString& file_name, Generation thumbnail, Generation preview) {
    LibraryItem item;
    item.id = id;
    item.file_path = QStringLiteral("C:/Photos/") + file_name;
    item.file_size_bytes = 1500000;
    item.camera_make = QStringLiteral("Canon");
    item.camera_model = QStringLiteral("EOS R5");
    item.capture_time = QDateTime(QDate(2026, 9, 24), QTime(21, 9));
    item.pixel_width = 6000;
    item.pixel_height = 4000;
    item.iso = 400;
    item.exposure_time_s = 1.0 / 250;
    item.f_number = 5.6;
    item.focal_length_mm = 35;
    item.support = SupportStatus::Verified;
    item.thumbnail_generation = thumbnail;
    item.thumbnail_is_camera_preview = true;
    item.preview_generation = preview;
    return item;
}

QImage solid_image(QSize size) {
    QImage image(size, QImage::Format_RGB32);
    image.fill(QColor(90, 120, 150));
    return image;
}

}  // namespace lucent::ui::testing
