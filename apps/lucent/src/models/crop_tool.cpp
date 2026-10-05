#include "models/crop_tool.hpp"

#include <QCoreApplication>
#include <QLocale>
#include <algorithm>
#include <array>
#include <cmath>

#include "models/edit_session.hpp"

namespace lucent::ui {
namespace {

QString tr_text(const char* text) {
    return QCoreApplication::translate("lucent::ui::CropTool", text);
}

struct AspectPreset {
    const char* key;
    const char* label;
    double long_side;  // 0: not a fixed ratio (original, free, custom)
    double short_side;
};

constexpr std::array kAspects{
    AspectPreset{"original", QT_TRANSLATE_NOOP("lucent::ui::CropTool", "Original"), 0.0, 0.0},
    AspectPreset{"free", QT_TRANSLATE_NOOP("lucent::ui::CropTool", "Free"), 0.0, 0.0},
    AspectPreset{"1:1", QT_TRANSLATE_NOOP("lucent::ui::CropTool", "1 : 1 (square)"), 1.0, 1.0},
    AspectPreset{"4:5", QT_TRANSLATE_NOOP("lucent::ui::CropTool", "4 : 5"), 5.0, 4.0},
    AspectPreset{"3:2", QT_TRANSLATE_NOOP("lucent::ui::CropTool", "3 : 2"), 3.0, 2.0},
    AspectPreset{"16:9", QT_TRANSLATE_NOOP("lucent::ui::CropTool", "16 : 9"), 16.0, 9.0},
    AspectPreset{"custom", QT_TRANSLATE_NOOP("lucent::ui::CropTool", "Custom"), 0.0, 0.0},
};

const AspectPreset* preset_of(const QString& key) {
    for (const AspectPreset& preset : kAspects) {
        if (key == QLatin1StringView(preset.key)) {
            return &preset;
        }
    }
    return nullptr;
}

double round6(double value) {
    const double rounded = std::round(value * 1e6) / 1e6;
    return rounded == 0.0 ? 0.0 : rounded;
}

// A crop within [0, 1] keeping at least the minimum extent, rounded to six decimals (0.006 pixels of
// a 6000-pixel side: recipes stay tidy).
EditCrop tidy(double left, double top, double right, double bottom) {
    const auto axis = [](double low, double high) {
        low = std::clamp(round6(low), 0.0, 1.0);
        high = std::clamp(round6(high), 0.0, 1.0);
        if (high - low < kMinCropExtent) {
            const double middle = std::clamp((low + high) / 2.0, kMinCropExtent / 2.0, 1.0 - kMinCropExtent / 2.0);
            low = round6(middle - kMinCropExtent / 2.0);
            high = round6(middle + kMinCropExtent / 2.0);
            low = std::max(0.0, std::min(low, 1.0 - kMinCropExtent));
            high = std::min(1.0, std::max(high, low + kMinCropExtent));
        }
        return std::pair{low, high};
    };
    const auto [l, r] = axis(left, right);
    const auto [t, b] = axis(top, bottom);
    return EditCrop{l, t, r, b};
}

// `rect` moved (and scaled down about its centre when larger) into (0, 0) - `bounds`.
QRectF keep_inside(QRectF rect, QSizeF bounds) {
    const double scale = std::min({1.0, bounds.width() / std::max(rect.width(), 1e-9),
                                   bounds.height() / std::max(rect.height(), 1e-9)});
    if (scale < 1.0) {
        const QPointF centre = rect.center();
        rect.setSize(rect.size() * scale);
        rect.moveCenter(centre);
    }
    rect.moveLeft(std::clamp(rect.left(), 0.0, std::max(0.0, bounds.width() - rect.width())));
    rect.moveTop(std::clamp(rect.top(), 0.0, std::max(0.0, bounds.height() - rect.height())));
    return rect;
}

QRectF lerp(const QRectF& a, const QRectF& b, double t) {
    return QRectF(QPointF(a.left() + (b.left() - a.left()) * t, a.top() + (b.top() - a.top()) * t),
                  QPointF(a.right() + (b.right() - a.right()) * t, a.bottom() + (b.bottom() - a.bottom()) * t));
}

}  // namespace

// ---- Crop math -----------------------------------------------------------------------------------------

namespace crop_math {

Handle parse_handle(const QString& name) noexcept {
    if (name == u"move") {
        return Handle::Move;
    }
    if (name == u"left") {
        return Handle::Left;
    }
    if (name == u"right") {
        return Handle::Right;
    }
    if (name == u"top") {
        return Handle::Top;
    }
    if (name == u"bottom") {
        return Handle::Bottom;
    }
    if (name == u"topLeft") {
        return Handle::TopLeft;
    }
    if (name == u"topRight") {
        return Handle::TopRight;
    }
    if (name == u"bottomLeft") {
        return Handle::BottomLeft;
    }
    if (name == u"bottomRight") {
        return Handle::BottomRight;
    }
    return Handle::None;
}

QRectF fit_aspect(const QRectF& within, double aspect) {
    if (!(aspect > 0.0) || within.isEmpty()) {
        return within;
    }
    double width = within.width();
    double height = width / aspect;
    if (height > within.height()) {
        height = within.height();
        width = height * aspect;
    }
    QRectF out(0.0, 0.0, width, height);
    out.moveCenter(within.center());
    return out;
}

QRectF drag(const QRectF& start, Handle handle, QPointF press, QPointF pointer, QSizeF bounds, double aspect,
            QSizeF min_size) {
    const double bw = bounds.width();
    const double bh = bounds.height();
    if (handle == Handle::None || !(bw > 0.0) || !(bh > 0.0)) {
        return start;
    }
    if (handle == Handle::Move) {
        const double dx = std::clamp(pointer.x() - press.x(), -start.left(), bw - start.right());
        const double dy = std::clamp(pointer.y() - press.y(), -start.top(), bh - start.bottom());
        return start.translated(dx, dy);
    }
    const QPointF p(std::clamp(pointer.x(), 0.0, bw), std::clamp(pointer.y(), 0.0, bh));
    const double min_w = std::min(min_size.width(), bw);
    const double min_h = std::min(min_size.height(), bh);
    const bool left = handle == Handle::Left || handle == Handle::TopLeft || handle == Handle::BottomLeft;
    const bool right = handle == Handle::Right || handle == Handle::TopRight || handle == Handle::BottomRight;
    const bool top = handle == Handle::Top || handle == Handle::TopLeft || handle == Handle::TopRight;
    const bool bottom = handle == Handle::Bottom || handle == Handle::BottomLeft || handle == Handle::BottomRight;
    double l = start.left();
    double r = start.right();
    double t = start.top();
    double b = start.bottom();

    if (!(aspect > 0.0)) {
        if (left) {
            l = std::min(p.x(), r - min_w);
        }
        if (right) {
            r = std::max(p.x(), l + min_w);
        }
        if (top) {
            t = std::min(p.y(), b - min_h);
        }
        if (bottom) {
            b = std::max(p.y(), t + min_h);
        }
        return keep_inside(QRectF(QPointF(l, t), QPointF(r, b)), bounds);
    }

    const double min_width = std::max(min_w, min_h * aspect);
    if ((left || right) && (top || bottom)) {
        // A corner: the opposite corner stays; the pointer's larger proportional extent wins.
        const double ax = left ? r : l;
        const double ay = top ? b : t;
        double width = std::max(std::abs(p.x() - ax), std::abs(p.y() - ay) * aspect);
        const double room_w = left ? ax : bw - ax;
        const double room_h = top ? ay : bh - ay;
        width = std::min({width, room_w, room_h * aspect});
        width = std::max(width, min_width);
        const double height = width / aspect;
        const double x = left ? ax - width : ax;
        const double y = top ? ay - height : ay;
        return keep_inside(QRectF(x, y, width, height), bounds);
    }
    if (left || right) {
        // A side edge: the opposite edge and the horizontal centre line stay.
        const double ax = left ? r : l;
        const double cy = (t + b) / 2.0;
        double width = left ? ax - p.x() : p.x() - ax;
        const double room_w = left ? ax : bw - ax;
        width = std::min({width, room_w, 2.0 * std::min(cy, bh - cy) * aspect});
        width = std::max(width, min_width);
        const double height = width / aspect;
        return keep_inside(QRectF(left ? ax - width : ax, cy - height / 2.0, width, height), bounds);
    }
    // A top or bottom edge: the opposite edge and the vertical centre line stay.
    const double ay = top ? b : t;
    const double cx = (l + r) / 2.0;
    double height = top ? ay - p.y() : p.y() - ay;
    const double room_h = top ? ay : bh - ay;
    height = std::min({height, room_h, 2.0 * std::min(cx, bw - cx) / aspect});
    height = std::max(height, min_width / aspect);
    const double width = height * aspect;
    return keep_inside(QRectF(cx - width / 2.0, top ? ay - height : ay, width, height), bounds);
}

std::optional<double> level_angle(QPointF a, QPointF b) {
    const double dx = b.x() - a.x();
    const double dy = b.y() - a.y();
    if (std::hypot(dx, dy) < 2.0) {
        return std::nullopt;
    }
    // The line's angle as displayed (counter-clockwise positive, y grows downwards).
    const double theta = std::atan2(-dy, dx) * 180.0 / 3.14159265358979323846;
    const double target = std::round(theta / 90.0) * 90.0;
    double angle = std::clamp(target - theta, -kMaxStraighten, kMaxStraighten);
    angle = std::round(angle * 100.0) / 100.0;
    return angle == 0.0 ? 0.0 : angle;
}

}  // namespace crop_math

// ---- CropTool ------------------------------------------------------------------------------------------

CropTool::CropTool(EditSession& session, QObject* parent) : QObject(parent), session_(session) {
    connect(&session_, &EditSession::settingsChanged, this, &CropTool::changed);
    connect(&session_, &EditSession::frameChanged, this, &CropTool::changed);
    connect(&session_, &EditSession::stateChanged, this, &CropTool::changed);
    connect(&session_, &EditSession::toolChanged, this, [this] {
        if (session_.tool() != u"crop") {
            endDrag();  // a drag never outlives the tool (its handles are gone)
        }
        emit changed();
    });
}

QVariantList CropTool::aspectOptions() const {
    QVariantList list;
    for (const AspectPreset& preset : kAspects) {
        list << QVariantMap{{QStringLiteral("key"), QString::fromLatin1(preset.key)},
                            {QStringLiteral("label"), tr_text(preset.label)}};
    }
    return list;
}

bool CropTool::available() const {
    return session_.can_edit() && session_.settings_known() && session_.has_frame() &&
           session_.frame_full_upright_size().isValid() && !session_.frame_full_upright_size().isEmpty();
}

bool CropTool::showing() const {
    if (session_.tool() != u"crop" || !available() || !session_.frame_uncropped()) {
        return false;
    }
    const EditGeometry& shown = session_.frame_geometry();
    const EditGeometry& wanted = session_.settings().geometry;
    return shown.quarter_turns == wanted.quarter_turns && shown.flip_horizontal == wanted.flip_horizontal;
}

GeometryMap CropTool::display_map(const EditGeometry& geometry) const {
    EditGeometry shown = geometry;
    shown.crop = EditCrop{};
    return session_.service().map_geometry(shown, session_.frame_full_upright_size());
}

QRectF CropTool::to_display(const GeometryMap& map, const EditCrop& crop) {
    if (!map.valid) {
        return {};
    }
    return map.straightened_to_output.mapRect(
        QRectF(QPointF(crop.left, crop.top), QPointF(crop.right, crop.bottom)));
}

EditCrop CropTool::from_display(const GeometryMap& map, const QRectF& rect) {
    bool invertible = false;
    const QTransform inverse = map.straightened_to_output.inverted(&invertible);
    if (!map.valid || !invertible) {
        return EditCrop{};
    }
    const QRectF crop = inverse.mapRect(rect);
    return tidy(crop.left(), crop.top(), crop.right(), crop.bottom());
}

QRectF CropTool::rect() const {
    const GeometryMap map = display_map(session_.settings().geometry);
    if (!map.valid || map.output_size.isEmpty()) {
        return {};
    }
    const QRectF shown = to_display(map, session_.settings().geometry.crop);
    const double w = map.output_size.width();
    const double h = map.output_size.height();
    return QRectF(shown.x() / w, shown.y() / h, shown.width() / w, shown.height() / h);
}

bool CropTool::modified() const {
    return !session_.settings().geometry.is_identity();
}

QString CropTool::size_text() const {
    if (!available()) {
        return {};
    }
    const GeometryMap map =
        session_.service().map_geometry(session_.settings().geometry, session_.frame_full_upright_size());
    if (!map.valid) {
        return {};
    }
    return tr_text("%1 × %2 px").arg(map.output_size.width()).arg(map.output_size.height());
}

double CropTool::display_aspect() const {
    const AspectPreset* preset = preset_of(aspect_);
    double long_side = 0.0;
    double short_side = 0.0;
    if (preset == nullptr || aspect_ == u"free") {
        return 0.0;
    }
    if (aspect_ == u"original") {
        const GeometryMap map = display_map(session_.settings().geometry);
        if (!map.valid || map.output_size.isEmpty()) {
            return 0.0;
        }
        long_side = std::max(map.output_size.width(), map.output_size.height());
        short_side = std::min(map.output_size.width(), map.output_size.height());
    } else if (aspect_ == u"custom") {
        long_side = std::max(custom_width_, custom_height_);
        short_side = std::min(custom_width_, custom_height_);
    } else {
        long_side = preset->long_side;
        short_side = preset->short_side;
    }
    if (!(long_side > 0.0) || !(short_side > 0.0)) {
        return 0.0;
    }
    return portrait_ ? short_side / long_side : long_side / short_side;
}

QSizeF CropTool::min_size(const GeometryMap& map) const {
    const double w = map.output_size.width();
    const double h = map.output_size.height();
    return QSizeF(std::max(kMinCropExtent * w, std::min(32.0, w)), std::max(kMinCropExtent * h, std::min(32.0, h)));
}

bool CropTool::inside(const EditGeometry& geometry) const {
    return session_.service().crop_inside_image(geometry, session_.frame_full_upright_size());
}

double CropTool::straightened_aspect(const EditCrop& crop) const {
    const QSize upright = session_.frame_full_upright_size();
    if (upright.isEmpty() || !(crop.height() > 0.0)) {
        return 0.0;
    }
    return (crop.width() * upright.width()) / (crop.height() * upright.height());
}

EditGeometry CropTool::fit_inside(EditGeometry geometry) const {
    if (inside(geometry)) {
        return geometry;
    }
    const EditCrop crop = geometry.crop;
    const double cx = (crop.left + crop.right) / 2.0;
    const double cy = (crop.top + crop.bottom) / 2.0;
    const double hw = crop.width() / 2.0;
    const double hh = crop.height() / 2.0;
    const auto scaled = [&](double s) { return tidy(cx - hw * s, cy - hh * s, cx + hw * s, cy + hh * s); };
    double low = 0.0;
    double high = 1.0;
    for (int i = 0; i < 30; ++i) {
        const double middle = (low + high) / 2.0;
        EditGeometry candidate = geometry;
        candidate.crop = scaled(middle);
        (inside(candidate) ? low : high) = middle;
    }
    if (low < 0.01) {
        // The crop's centre is not on the image: the largest centred crop of the same shape.
        geometry.crop = session_.service().largest_inside_crop(geometry.straighten_degrees,
                                                               session_.frame_full_upright_size(),
                                                               straightened_aspect(crop));
        const EditCrop c = geometry.crop;
        geometry.crop = tidy(c.left, c.top, c.right, c.bottom);
        return geometry;
    }
    geometry.crop = scaled(low);
    return geometry;
}

EditGeometry CropTool::straightened(const EditGeometry& geometry, double degrees) const {
    EditGeometry next = geometry;
    const double angle = std::clamp(std::round(degrees * 100.0) / 100.0, -kMaxStraighten, kMaxStraighten);
    next.straighten_degrees = angle == 0.0 ? 0.0 : angle;
    const QSize upright = session_.frame_full_upright_size();
    if (!constrain_ || upright.isEmpty() || next.straighten_degrees == geometry.straighten_degrees) {
        return next;
    }
    const double aspect = straightened_aspect(geometry.crop);
    const EditCrop previous_largest =
        session_.service().largest_inside_crop(geometry.straighten_degrees, upright, aspect);
    const auto near = [](const EditCrop& a, const EditCrop& b) {
        return std::abs(a.left - b.left) < 1e-4 && std::abs(a.top - b.top) < 1e-4 &&
               std::abs(a.right - b.right) < 1e-4 && std::abs(a.bottom - b.bottom) < 1e-4;
    };
    if (geometry.crop.is_full() || near(geometry.crop, previous_largest)) {
        // A crop that follows the angle: the largest of its shape inside the image.
        const EditCrop c = session_.service().largest_inside_crop(next.straighten_degrees, upright, aspect);
        next.crop = tidy(c.left, c.top, c.right, c.bottom);
        return next;
    }
    return fit_inside(next);
}

void CropTool::apply_geometry(const EditGeometry& geometry) {
    session_.finish_pending_gesture();
    EditSettings next = session_.settings();
    if (next.geometry == geometry) {
        return;
    }
    next.geometry = geometry;
    session_.edit_settings(next);
}

void CropTool::setConstrain(bool constrain) {
    if (constrain == constrain_) {
        return;
    }
    constrain_ = constrain;
    if (constrain_ && available() && !drag_) {
        const EditGeometry& geometry = session_.settings().geometry;
        if (!inside(geometry)) {
            apply_geometry(fit_inside(geometry));
        }
    }
    emit changed();
}

void CropTool::setAspect(const QString& key) {
    if (preset_of(key) == nullptr) {
        return;
    }
    aspect_ = key;
    if (available()) {
        const GeometryMap map = display_map(session_.settings().geometry);
        const QRectF shown = to_display(map, session_.settings().geometry.crop);
        portrait_ = shown.height() > shown.width() + 1.0;  // a square counts as landscape
    }
    refit(false);
    emit changed();
}

void CropTool::swapOrientation() {
    portrait_ = !portrait_;
    refit(true);
    emit changed();
}

void CropTool::setCustomAspect(double width, double height) {
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0 || height <= 0.0) {
        return;
    }
    custom_width_ = std::clamp(width, 0.01, 1000.0);
    custom_height_ = std::clamp(height, 0.01, 1000.0);
    portrait_ = custom_height_ > custom_width_;
    aspect_ = QStringLiteral("custom");
    refit(false);
    emit changed();
}

void CropTool::refit(bool swap) {
    const double aspect = display_aspect();
    if (!available() || !(aspect > 0.0) || drag_) {
        return;
    }
    const EditGeometry& geometry = session_.settings().geometry;
    const GeometryMap map = display_map(geometry);
    if (!map.valid) {
        return;
    }
    const QSizeF bounds(map.output_size.width(), map.output_size.height());
    const QRectF shown = to_display(map, geometry.crop);
    QRectF target;
    if (swap) {
        // The same area in the other orientation, about the same centre.
        const double area = shown.width() * shown.height();
        const double width = std::sqrt(area * aspect);
        target = QRectF(0.0, 0.0, width, width / aspect);
        target.moveCenter(shown.center());
        target = keep_inside(target, bounds);
    } else {
        target = crop_math::fit_aspect(geometry.crop.is_full() ? QRectF(QPointF(0.0, 0.0), bounds) : shown, aspect);
    }
    EditGeometry next = geometry;
    next.crop = from_display(map, target);
    if (constrain_ && !inside(next)) {
        next = fit_inside(next);
    }
    apply_geometry(next);
}

void CropTool::beginDrag(const QString& handle, double u, double v) {
    const crop_math::Handle parsed = crop_math::parse_handle(handle);
    if (drag_ || parsed == crop_math::Handle::None || !showing() || !std::isfinite(u) || !std::isfinite(v)) {
        return;
    }
    const GeometryMap map = display_map(session_.settings().geometry);
    if (!map.valid) {
        return;
    }
    session_.begin_pointer_gesture();
    drag_ = Drag{parsed, to_display(map, session_.settings().geometry.crop),
                 QPointF(u * map.output_size.width(), v * map.output_size.height()), map};
    emit changed();
}

void CropTool::dragTo(double u, double v) {
    if (!drag_ || !std::isfinite(u) || !std::isfinite(v)) {
        return;
    }
    const GeometryMap& map = drag_->map;
    const QSizeF bounds(map.output_size.width(), map.output_size.height());
    const QPointF pointer(u * bounds.width(), v * bounds.height());
    QRectF next = crop_math::drag(drag_->start, drag_->handle, drag_->press, pointer, bounds, display_aspect(),
                                  min_size(map));
    EditGeometry geometry = session_.settings().geometry;
    geometry.crop = from_display(map, next);
    if (constrain_ && !inside(geometry)) {
        EditGeometry from = geometry;
        from.crop = from_display(map, drag_->start);
        if (inside(from)) {
            // As far as the image reaches along the way from the press.
            double low = 0.0;
            double high = 1.0;
            for (int i = 0; i < 24; ++i) {
                const double middle = (low + high) / 2.0;
                EditGeometry candidate = geometry;
                candidate.crop = from_display(map, lerp(drag_->start, next, middle));
                (inside(candidate) ? low : high) = middle;
            }
            geometry.crop = from_display(map, lerp(drag_->start, next, low));
        }
    }
    EditSettings settings = session_.settings();
    if (settings.geometry == geometry) {
        return;
    }
    settings.geometry = geometry;
    session_.edit_settings(settings);
}

void CropTool::endDrag() {
    if (!drag_) {
        return;
    }
    drag_.reset();
    session_.end_pointer_gesture();
    emit changed();
}

bool CropTool::level(double u0, double v0, double u1, double v1) {
    if (!available() || drag_ || !std::isfinite(u0) || !std::isfinite(v0) || !std::isfinite(u1) ||
        !std::isfinite(v1)) {
        return false;
    }
    // Through the geometry of the pixels on screen (the newest frame's), to the upright image.
    const GeometryMap map =
        session_.service().map_geometry(session_.frame_geometry(), session_.frame_full_upright_size());
    bool invertible = false;
    const QTransform to_normalized = map.normalized_to_output.inverted(&invertible);
    if (!map.valid || !invertible) {
        return false;
    }
    const auto upright = [&](double u, double v) {
        const QPointF n = to_normalized.map(QPointF(u * map.output_size.width(), v * map.output_size.height()));
        return QPointF(n.x() * map.upright_size.width(), n.y() * map.upright_size.height());
    };
    const std::optional<double> angle = crop_math::level_angle(upright(u0, v0), upright(u1, v1));
    if (!angle) {
        return false;
    }
    apply_geometry(straightened(session_.settings().geometry, *angle));
    return true;
}

void CropTool::rotateLeft() {
    if (!available()) {
        return;
    }
    apply_geometry(session_.service().turned(session_.settings().geometry, GeometryTurn::CounterClockwise));
    portrait_ = !portrait_;
    emit changed();
}

void CropTool::rotateRight() {
    if (!available()) {
        return;
    }
    apply_geometry(session_.service().turned(session_.settings().geometry, GeometryTurn::Clockwise));
    portrait_ = !portrait_;
    emit changed();
}

void CropTool::flip() {
    if (!available()) {
        return;
    }
    apply_geometry(session_.service().turned(session_.settings().geometry, GeometryTurn::FlipHorizontal));
}

void CropTool::reset() {
    if (!available()) {
        return;
    }
    apply_geometry(EditGeometry{});
}

void CropTool::setCrop(double left, double top, double width, double height) {
    if (!available() || !std::isfinite(left) || !std::isfinite(top) || !std::isfinite(width) ||
        !std::isfinite(height)) {
        return;
    }
    EditGeometry geometry = session_.settings().geometry;
    geometry.crop = tidy(left, top, left + width, top + height);
    apply_geometry(geometry);
}

}  // namespace lucent::ui
