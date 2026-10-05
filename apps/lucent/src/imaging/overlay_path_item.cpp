#include "imaging/overlay_path_item.hpp"

#include <algorithm>

#include <QPainter>
#include <QPainterPath>
#include <QPen>

namespace lucent::ui {

OverlayPathItem::OverlayPathItem(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
}

void OverlayPathItem::set_paths(const QVariant& value) {
    OverlayPaths paths = value.canConvert<OverlayPaths>() ? value.value<OverlayPaths>() : OverlayPaths{};
    if (paths == paths_) {
        return;
    }
    paths_ = std::move(paths);
    emit pathsChanged();
    update();
}

void OverlayPathItem::set_image_rect(const QRectF& rect) {
    if (rect == image_rect_) {
        return;
    }
    image_rect_ = rect;
    emit imageRectChanged();
    update();
}

void OverlayPathItem::paint(QPainter* painter) {
    if (paths_.isEmpty() || image_rect_.isEmpty()) {
        return;
    }
    painter->setRenderHint(QPainter::Antialiasing, true);
    // Masks act on the image only: a gradient's lines and an ellipse beyond it are not drawn.
    const QRectF clip = image_rect_.adjusted(-1.0, -1.0, 1.0, 1.0);
    painter->setClipRect(clip);
    // Segments are kept when they reach the part of the image in the item (with the pens' width).
    const QRectF visible = clip.intersected(QRectF(0.0, 0.0, width(), height())).adjusted(-4.0, -4.0, 4.0, 4.0);
    const auto place = [this](const QPointF& point) {
        return QPointF(image_rect_.x() + point.x() * image_rect_.width(),
                       image_rect_.y() + point.y() * image_rect_.height());
    };
    const auto reaches = [&visible](const QPointF& a, const QPointF& b) {
        return std::max(a.x(), b.x()) >= visible.left() && std::min(a.x(), b.x()) <= visible.right() &&
               std::max(a.y(), b.y()) >= visible.top() && std::min(a.y(), b.y()) <= visible.bottom();
    };
    constexpr double kMinStep2 = kMinStep * kMinStep;
    // Others first, the selected mask on top.
    for (const bool emphasis : {false, true}) {
        for (const OverlayPath& path : paths_) {
            if (path.emphasis != emphasis || path.points.size() < 2) {
                continue;
            }
            QPainterPath line;
            bool open = false;  // the current subpath ends at `last`
            QPointF last = place(path.points.front());
            const qsizetype n = path.points.size();
            for (qsizetype i = 1; i < n; ++i) {
                const QPointF p = place(path.points[i]);
                const QPointF step = p - last;
                if (i + 1 < n && step.x() * step.x() + step.y() * step.y() < kMinStep2) {
                    continue;  // closer than a pixel to the last point drawn
                }
                if (reaches(last, p)) {
                    if (!open) {
                        line.moveTo(last);
                        open = true;
                    }
                    line.lineTo(p);
                } else {
                    open = false;
                }
                last = p;
            }
            if (line.isEmpty()) {
                continue;
            }
            QPen halo(QColor(0, 0, 0, emphasis ? 150 : 110), emphasis ? 3.5 : 2.5);
            halo.setCapStyle(Qt::RoundCap);
            halo.setJoinStyle(Qt::RoundJoin);
            QPen pen(emphasis ? QColor(255, 255, 255, 235) : QColor(230, 230, 230, 170), emphasis ? 1.6 : 1.0);
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            if (path.dashed) {
                halo.setStyle(Qt::DashLine);
                pen.setStyle(Qt::DashLine);
            }
            painter->strokePath(line, halo);
            painter->strokePath(line, pen);
        }
    }
}

}  // namespace lucent::ui
