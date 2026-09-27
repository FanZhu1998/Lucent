#include <algorithm>
#include <cmath>
#include <utility>

#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLinearGradient>
#include <QPainter>
#include <QPolygonF>
#include <QThread>

#include "fakes/fake_services.hpp"

namespace lucent::ui::fakes {
namespace {

// A camera's embedded JPEG is typically about this size; thumbnails are painted no larger.
constexpr QSize kEmbeddedPreviewBox{1620, 1080};

QString tr_fake(const char* text) {
    return QCoreApplication::translate("lucent::ui::fakes", text);
}

QSize fit(QSize natural, QSize bounding) {
    const double scale_x = bounding.width() > 0 ? double(bounding.width()) / natural.width() : 1.0;
    const double scale_y = bounding.height() > 0 ? double(bounding.height()) / natural.height() : 1.0;
    const double scale = std::min({scale_x, scale_y, 1.0});
    return QSize(std::max(1, static_cast<int>(std::lround(natural.width() * scale))),
                 std::max(1, static_cast<int>(std::lround(natural.height() * scale))));
}

// Waits up to `milliseconds`, returning early when the reply is cancelled or the source stops.
void simulate_latency(int milliseconds, const ImageReply& reply, const std::atomic<bool>& stopping) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < milliseconds && !reply.is_cancelled() && !stopping.load()) {
        QThread::msleep(5);
    }
}

// Unit-interval value from 8 bits of the seed.
double unit(std::uint32_t seed, int shift) {
    return double((seed >> shift) & 0xffu) / 255.0;
}

}  // namespace

FakeImageSource::FakeImageSource(std::shared_ptr<const FakeCatalog> catalog, FakeTimings timings)
    : catalog_(std::move(catalog)), timings_(timings) {
    pool_.setMaxThreadCount(2);
}

FakeImageSource::~FakeImageSource() {
    // Queued tasks still run, see stopping_ and fail their replies, so every reply completes.
    stopping_.store(true);
    pool_.waitForDone();
}

void FakeImageSource::request_image(const ImageRequest& request, std::shared_ptr<ImageReply> reply) {
    pool_.start([this, request, reply = std::move(reply)] {
        const int delay =
            request.kind == ImageKind::Thumbnail ? timings_.thumbnail_delay_ms : timings_.preview_delay_ms;
        simulate_latency(delay, *reply, stopping_);
        if (stopping_.load()) {
            reply->fail(tr_fake("The simulated image source is shutting down."));
            return;
        }
        if (reply->is_cancelled()) {
            return;  // already completed as cancelled by the consumer
        }
        const std::optional<FakeCatalog::Entry> entry = catalog_->find(request.item);
        if (!entry) {
            reply->fail(tr_fake("Simulated: this photo is not in the library."));
            return;
        }
        if (request.kind == ImageKind::Preview && entry->support == SupportStatus::Unsupported) {
            reply->fail(entry->support_detail);
            return;
        }
        reply->finish(paint(*entry, request.kind, request.generation, request.bounding_size));
    });
}

QImage FakeImageSource::paint(const FakeCatalog::Entry& entry, ImageKind kind, Generation generation,
                              QSize bounding) {
    const QSize full(std::max(1, entry.width), std::max(1, entry.height));
    const QSize natural = kind == ImageKind::Thumbnail ? fit(full, kEmbeddedPreviewBox) : full;
    const QSize size = fit(natural, bounding);
    const double w = size.width();
    const double h = size.height();
    const std::uint32_t seed = entry.seed;
    const int hue = static_cast<int>(seed % 360u);

    QImage image(size, QImage::Format_RGB32);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);

    QLinearGradient sky(0.0, 0.0, 0.0, h * 0.7);
    sky.setColorAt(0.0, QColor::fromHsv(hue, 110, 140));
    sky.setColorAt(1.0, QColor::fromHsv((hue + 30) % 360, 55, 230));
    painter.fillRect(QRectF(0.0, 0.0, w, h), sky);

    const double radius = std::min(w, h) * 0.07;
    painter.setBrush(QColor::fromHsv((hue + 45) % 360, 60, 250));
    painter.drawEllipse(QPointF(w * (0.2 + 0.6 * unit(seed, 8)), h * (0.18 + 0.15 * unit(seed, 16))), radius,
                        radius);

    const auto ridge = [&](double base, double amplitude, int shift, const QColor& color) {
        QPolygonF polygon;
        polygon << QPointF(0.0, h);
        constexpr int kSegments = 12;
        for (int i = 0; i <= kSegments; ++i) {
            const double x = w * i / kSegments;
            const double wave = std::sin(i * (0.9 + unit(seed, shift)) + unit(seed, shift + 4) * 6.0);
            polygon << QPointF(x, h * (base - amplitude * wave));
        }
        polygon << QPointF(w, h);
        painter.setBrush(color);
        painter.drawPolygon(polygon);
    };
    ridge(0.55, 0.08, 3, QColor::fromHsv((hue + 200) % 360, 45, 120));
    ridge(0.68, 0.05, 11, QColor::fromHsv((hue + 150) % 360, 70, 80));
    ridge(0.84, 0.03, 19, QColor::fromHsv((hue + 110) % 360, 85, 55));

    if (kind == ImageKind::Thumbnail && generation <= 1) {
        // The "camera JPEG" is graded warmer so its replacement by Lucent's render is visible.
        painter.fillRect(QRectF(0.0, 0.0, w, h), QColor(255, 170, 80, 40));
    }
    painter.end();
    return image;
}

}  // namespace lucent::ui::fakes
