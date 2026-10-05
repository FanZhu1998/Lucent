#pragma once

// OverlayPathItem: draws the Edit view's mask outlines (ADR 0015: engine::mask_outline() of the
// masks, DevelopFrame::masks) over the displayed image: radial ellipses, the three lines of a linear
// gradient and brush strokes' centre lines. Points are fractions of the displayed image's width
// and height (0..1 inside it), placed through `imageRect` (the image in the item's coordinates), so
// the item can cover only the visible view while the image is far larger at 100 %.
//
// Each path is drawn twice, a dark halo under a light line, so it reads on any photo; the selected
// mask's lines are brighter and wider, a switched-off mask's are dashed. Paths are clipped to the
// image (a gradient's lines are long enough to cross it).
//
// Thread-safety: a Qt Quick item; UI thread only.

#include <QList>
#include <QMetaType>
#include <QPolygonF>
#include <QQuickPaintedItem>
#include <QRectF>
#include <QVariant>

#include <QtQmlIntegration/qqmlintegration.h>

namespace lucent::ui {

struct OverlayPath {
    QPolygonF points;  // fractions of the displayed image's width and height
    bool emphasis = false;
    bool dashed = false;

    friend bool operator==(const OverlayPath&, const OverlayPath&) = default;
};

using OverlayPaths = QList<OverlayPath>;

class OverlayPathItem : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    // An OverlayPaths value (MaskEditor.outlines); anything else draws nothing.
    Q_PROPERTY(QVariant paths READ paths WRITE set_paths NOTIFY pathsChanged FINAL)
    Q_PROPERTY(QRectF imageRect READ image_rect WRITE set_image_rect NOTIFY imageRectChanged FINAL)
    Q_PROPERTY(int pathCount READ path_count NOTIFY pathsChanged FINAL)

public:
    explicit OverlayPathItem(QQuickItem* parent = nullptr);

    void paint(QPainter* painter) override;

    [[nodiscard]] QVariant paths() const { return QVariant::fromValue(paths_); }
    void set_paths(const QVariant& value);
    [[nodiscard]] QRectF image_rect() const noexcept { return image_rect_; }
    void set_image_rect(const QRectF& rect);
    [[nodiscard]] int path_count() const noexcept { return static_cast<int>(paths_.size()); }

signals:
    void pathsChanged();
    void imageRectChanged();

private:
    OverlayPaths paths_;
    QRectF image_rect_;
};

}  // namespace lucent::ui

Q_DECLARE_METATYPE(lucent::ui::OverlayPaths)
