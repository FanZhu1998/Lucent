#include "imaging/histogram_plot.hpp"

#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <array>

namespace lucent::ui {
namespace {

using Bins = std::array<std::uint32_t, RenderInfo::kBins>;

// The outline of `bins` as a closed area along the bottom edge of `rect`.
QPainterPath area_of(const Bins& bins, double scale, const QRectF& rect) {
    QPainterPath path;
    const double step = rect.width() / RenderInfo::kBins;
    path.moveTo(rect.left(), rect.bottom());
    for (int i = 0; i < RenderInfo::kBins; ++i) {
        const double height = std::min(1.0, bins[static_cast<std::size_t>(i)] / scale) * rect.height();
        const double top = rect.bottom() - height;
        path.lineTo(rect.left() + i * step, top);
        path.lineTo(rect.left() + (i + 1) * step, top);
    }
    path.lineTo(rect.right(), rect.bottom());
    path.closeSubpath();
    return path;
}

}  // namespace

HistogramPlot::HistogramPlot(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    setOpaquePainting(false);
}

void HistogramPlot::set_histogram(const QVariant& value) {
    RenderInfoPtr info = value.canConvert<RenderInfoPtr>() ? value.value<RenderInfoPtr>() : RenderInfoPtr{};
    if (info == info_) {
        return;
    }
    info_ = std::move(info);
    emit histogramChanged();
    update();
}

double HistogramPlot::shadows_clipped() const noexcept {
    return empty() ? 0.0 : double(info_->luma.front()) / double(info_->pixels);
}

double HistogramPlot::highlights_clipped() const noexcept {
    return empty() ? 0.0 : double(info_->luma.back()) / double(info_->pixels);
}

double HistogramPlot::scale_of(const RenderInfo& info) noexcept {
    std::uint32_t tallest = 1;
    for (std::size_t i = 1; i + 1 < RenderInfo::kBins; ++i) {
        tallest = std::max({tallest, info.red[i], info.green[i], info.blue[i], info.luma[i]});
    }
    return double(tallest);
}

void HistogramPlot::paint(QPainter* painter) {
    if (empty() || width() < 2 || height() < 2) {
        return;
    }
    const QRectF rect(0.5, 0.5, width() - 1.0, height() - 1.0);
    const double scale = scale_of(*info_);
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    // Additive blending of the three channels: where they overlap the colours add up to white.
    painter->setCompositionMode(QPainter::CompositionMode_Plus);
    painter->setBrush(QColor(200, 60, 60, 190));
    painter->drawPath(area_of(info_->red, scale, rect));
    painter->setBrush(QColor(60, 180, 70, 190));
    painter->drawPath(area_of(info_->green, scale, rect));
    painter->setBrush(QColor(70, 100, 220, 190));
    painter->drawPath(area_of(info_->blue, scale, rect));
    painter->setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(QColor(230, 230, 230, 170), 1.0));
    painter->drawPath(area_of(info_->luma, scale, rect));
}

}  // namespace lucent::ui
