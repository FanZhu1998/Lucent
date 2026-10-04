#pragma once

// ToneCurvePlot: draws the Edit view's tone curve (blueprint section 2: "a monotone tone curve")
// over the histogram of the current frame: the curve as the renderer evaluates it (`samples`,
// EditService::sample_tone_curve), its control points (the selected one filled), the identity
// diagonal and quarter grid lines. Interaction is the QML editor's (ToneCurveEditor.qml); this item
// maps between item and curve coordinates for it.
//
// Coordinates: the curve's x and y are display lightness L*/100 (develop_settings.hpp); the plot is
// inset by `margin` so points on the border stay fully visible; y grows upwards. The histogram's
// code-value bins are placed at the lightness of their sRGB value, so they line up with the curve's
// axis.
//
// Thread-safety: a Qt Quick item; UI thread only.

#include <QList>
#include <QPointF>
#include <QQuickPaintedItem>
#include <QVariant>
#include <QtQmlIntegration/qqmlintegration.h>

#include "services/service_types.hpp"

namespace lucent::ui {

class ToneCurvePlot : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    // QPointF values in curve coordinates.
    Q_PROPERTY(QVariantList points READ points WRITE set_points NOTIFY pointsChanged FINAL)
    // The curve at evenly spaced x in [0, 1].
    Q_PROPERTY(QList<double> samples READ samples WRITE set_samples NOTIFY samplesChanged FINAL)
    // A RenderInfoPtr (EditSession.frameInfo); anything else draws no histogram.
    Q_PROPERTY(QVariant histogram READ histogram WRITE set_histogram NOTIFY histogramChanged FINAL)
    Q_PROPERTY(int selectedIndex READ selected_index WRITE set_selected_index NOTIFY selectedIndexChanged FINAL)
    Q_PROPERTY(double margin READ margin CONSTANT FINAL)

public:
    explicit ToneCurvePlot(QQuickItem* parent = nullptr);

    void paint(QPainter* painter) override;

    [[nodiscard]] QVariantList points() const { return points_; }
    void set_points(const QVariantList& points);
    [[nodiscard]] QList<double> samples() const { return samples_; }
    void set_samples(const QList<double>& samples);
    [[nodiscard]] QVariant histogram() const { return QVariant::fromValue(info_); }
    void set_histogram(const QVariant& value);
    [[nodiscard]] int selected_index() const noexcept { return selected_; }
    void set_selected_index(int index);
    [[nodiscard]] static constexpr double margin() noexcept { return 7.0; }

    // Item coordinates of the curve point (x, y), and back (clamped to [0, 1]).
    Q_INVOKABLE QPointF toItem(double x, double y) const;
    Q_INVOKABLE QPointF toCurve(double item_x, double item_y) const;
    // The index of the control point nearest to the item position within `radius` pixels; -1.
    Q_INVOKABLE int pointAt(double item_x, double item_y, double radius) const;

    // The lightness position (L*/100) of the sRGB code value `code` (0..255).
    [[nodiscard]] static double lightness_of_code(int code) noexcept;

signals:
    void pointsChanged();
    void samplesChanged();
    void histogramChanged();
    void selectedIndexChanged();

private:
    [[nodiscard]] QRectF plot_rect() const;

    QVariantList points_;
    QList<double> samples_;
    RenderInfoPtr info_;
    int selected_ = -1;
};

}  // namespace lucent::ui
