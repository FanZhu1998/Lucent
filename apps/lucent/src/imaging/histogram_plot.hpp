#pragma once

// HistogramPlot: draws the Edit view's histogram (blueprint section 2, "Detailed individual
// editing") from a RenderInfo: the red, green and blue code-value histograms of the displayed
// preview as additive translucent areas (overlaps read as yellow, cyan, magenta and white), with
// the luma histogram as an outline.
//
// Scaling: bin heights are linear, normalised to the tallest bin among 1..254, so that a spike of
// clipped pixels in bin 0 or 255 cannot flatten the rest of the plot; extreme bins that exceed
// the scale are drawn to full height. `shadowsClipped` and `highlightsClipped` report the share
// of clipped pixels for a textual indication beside the plot: those the clipping overlay marks
// (some channel at 0 or 255) when the render counted them (edit frames), otherwise the pixels whose
// luma is 0 or 255.
//
// Thread-safety: a Qt Quick item; UI thread only (paint() runs while the UI thread is blocked in
// the scene graph's synchronisation, so it may read the members).

#include <QQuickPaintedItem>
#include <QVariant>

#include <QtQmlIntegration/qqmlintegration.h>

#include "services/service_types.hpp"

namespace lucent::ui {

class HistogramPlot : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    // A RenderInfoPtr (CurrentItem.renderInfo); anything else shows an empty plot.
    Q_PROPERTY(QVariant histogram READ histogram WRITE set_histogram NOTIFY histogramChanged FINAL)
    Q_PROPERTY(bool empty READ empty NOTIFY histogramChanged FINAL)
    Q_PROPERTY(double shadowsClipped READ shadows_clipped NOTIFY histogramChanged FINAL)
    Q_PROPERTY(double highlightsClipped READ highlights_clipped NOTIFY histogramChanged FINAL)

public:
    explicit HistogramPlot(QQuickItem* parent = nullptr);

    void paint(QPainter* painter) override;

    [[nodiscard]] QVariant histogram() const { return QVariant::fromValue(info_); }
    void set_histogram(const QVariant& value);
    [[nodiscard]] bool empty() const noexcept { return !info_ || info_->pixels == 0; }
    // Fractions 0..1 of the pixels with luma 0 / 255; 0 when empty.
    [[nodiscard]] double shadows_clipped() const noexcept;
    [[nodiscard]] double highlights_clipped() const noexcept;

    // The value every bin is divided by to get a height in 0..1 (at least 1; see the file comment).
    [[nodiscard]] static double scale_of(const RenderInfo& info) noexcept;

signals:
    void histogramChanged();

private:
    RenderInfoPtr info_;
};

}  // namespace lucent::ui
