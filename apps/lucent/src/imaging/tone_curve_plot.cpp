#include "imaging/tone_curve_plot.hpp"

#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

#include "imaging/histogram_plot.hpp"

namespace lucent::ui {

ToneCurvePlot::ToneCurvePlot(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    setOpaquePainting(false);
}

void ToneCurvePlot::set_points(const QVariantList& points) {
    if (points == points_) {
        return;
    }
    points_ = points;
    emit pointsChanged();
    update();
}

void ToneCurvePlot::set_samples(const QList<double>& samples) {
    if (samples == samples_) {
        return;
    }
    samples_ = samples;
    emit samplesChanged();
    update();
}

void ToneCurvePlot::set_histogram(const QVariant& value) {
    RenderInfoPtr info = value.canConvert<RenderInfoPtr>() ? value.value<RenderInfoPtr>() : RenderInfoPtr{};
    if (info == info_) {
        return;
    }
    info_ = std::move(info);
    emit histogramChanged();
    update();
}

void ToneCurvePlot::set_selected_index(int index) {
    if (index == selected_) {
        return;
    }
    selected_ = index;
    emit selectedIndexChanged();
    update();
}

QRectF ToneCurvePlot::plot_rect() const {
    const double m = margin();
    return QRectF(m, m, std::max(1.0, width() - 2 * m), std::max(1.0, height() - 2 * m));
}

QPointF ToneCurvePlot::toItem(double x, double y) const {
    const QRectF rect = plot_rect();
    return QPointF(rect.left() + x * rect.width(), rect.bottom() - y * rect.height());
}

QPointF ToneCurvePlot::toCurve(double item_x, double item_y) const {
    const QRectF rect = plot_rect();
    return QPointF(std::clamp((item_x - rect.left()) / rect.width(), 0.0, 1.0),
                   std::clamp((rect.bottom() - item_y) / rect.height(), 0.0, 1.0));
}

int ToneCurvePlot::pointAt(double item_x, double item_y, double radius) const {
    int best = -1;
    double best_distance = radius * radius;
    for (qsizetype i = 0; i < points_.size(); ++i) {
        const QPointF curve = points_[i].toPointF();
        const QPointF at = toItem(curve.x(), curve.y());
        const double dx = at.x() - item_x;
        const double dy = at.y() - item_y;
        const double distance = dx * dx + dy * dy;
        if (distance <= best_distance) {
            best_distance = distance;
            best = static_cast<int>(i);
        }
    }
    return best;
}

double ToneCurvePlot::lightness_of_code(int code) noexcept {
    const double v = std::clamp(code, 0, 255) / 255.0;
    const double linear = v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    const double lstar = linear > 216.0 / 24389.0 ? 116.0 * std::cbrt(linear) - 16.0 : (24389.0 / 27.0) * linear;
    return std::clamp(lstar / 100.0, 0.0, 1.0);
}

void ToneCurvePlot::paint(QPainter* painter) {
    if (width() < 4 || height() < 4) {
        return;
    }
    painter->setRenderHint(QPainter::Antialiasing, true);
    const QRectF rect = plot_rect();

    // Quarter grid.
    painter->setPen(QPen(QColor(0x2e, 0x2e, 0x2e), 1.0));
    for (int i = 1; i < 4; ++i) {
        const double x = rect.left() + rect.width() * i / 4.0;
        const double y = rect.top() + rect.height() * i / 4.0;
        painter->drawLine(QPointF(x, rect.top()), QPointF(x, rect.bottom()));
        painter->drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y));
    }

    // The histogram of the current frame (luma), bins at the lightness of their code value.
    if (info_ && info_->pixels > 0) {
        const double scale = HistogramPlot::scale_of(*info_);
        QPainterPath area;
        area.moveTo(rect.left(), rect.bottom());
        for (int i = 0; i < RenderInfo::kBins; ++i) {
            const double left = lightness_of_code(i == 0 ? 0 : i);
            const double right = i + 1 < RenderInfo::kBins ? lightness_of_code(i + 1) : 1.0;
            const double h = std::min(1.0, info_->luma[static_cast<std::size_t>(i)] / scale) * rect.height();
            area.lineTo(rect.left() + left * rect.width(), rect.bottom() - h);
            area.lineTo(rect.left() + right * rect.width(), rect.bottom() - h);
        }
        area.lineTo(rect.right(), rect.bottom());
        area.closeSubpath();
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(200, 200, 200, 46));
        painter->drawPath(area);
    }

    // Identity.
    QPen identity(QColor(0x66, 0x66, 0x66), 1.0, Qt::DashLine);
    painter->setPen(identity);
    painter->drawLine(QPointF(rect.left(), rect.bottom()), QPointF(rect.right(), rect.top()));

    // The curve.
    if (samples_.size() >= 2) {
        QPainterPath curve;
        for (qsizetype i = 0; i < samples_.size(); ++i) {
            const double x = double(i) / double(samples_.size() - 1);
            const QPointF at = toItem(x, std::clamp(samples_[i], 0.0, 1.0));
            if (i == 0) {
                curve.moveTo(at);
            } else {
                curve.lineTo(at);
            }
        }
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(isEnabled() ? QColor(0xe8, 0xe8, 0xe8) : QColor(0x93, 0x93, 0x93), 1.6));
        painter->drawPath(curve);
    }

    // Control points.
    for (qsizetype i = 0; i < points_.size(); ++i) {
        const QPointF curve = points_[i].toPointF();
        const QPointF at = toItem(curve.x(), curve.y());
        const bool selected = i == selected_;
        painter->setPen(QPen(selected ? QColor(0x8c, 0xc0, 0xf2) : QColor(0xe8, 0xe8, 0xe8), 1.5));
        painter->setBrush(selected ? QColor(0x4f, 0x8f, 0xd0) : QColor(0x23, 0x23, 0x23));
        painter->drawEllipse(at, selected ? 5.5 : 4.5, selected ? 5.5 : 4.5);
    }
}

}  // namespace lucent::ui
