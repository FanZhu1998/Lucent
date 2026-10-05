#include "models/mask_editor.hpp"

#include <QCoreApplication>
#include <algorithm>
#include <cmath>

#include "models/edit_session.hpp"

namespace lucent::ui {
namespace {

constexpr double kPi = 3.14159265358979323846;

QString tr_text(const char* text) {
    return QCoreApplication::translate("lucent::ui::MaskEditor", text);
}

double round6(double value) {
    const double rounded = std::round(value * 1e6) / 1e6;
    return rounded == 0.0 ? 0.0 : rounded;
}

// A position within the engine's bounds (one image size around the image), six decimals.
QPointF tidy_point(QPointF point) {
    return QPointF(std::clamp(round6(point.x()), -kMaskPositionMargin, 1.0 + kMaskPositionMargin),
                   std::clamp(round6(point.y()), -kMaskPositionMargin, 1.0 + kMaskPositionMargin));
}

qsizetype brush_points(const QList<EditMask>& masks) {
    qsizetype points = 0;
    for (const EditMask& mask : masks) {
        for (const EditBrushStroke& stroke : mask.strokes) {
            points += stroke.points.size();
        }
    }
    return points;
}

std::optional<MaskKind> parse_kind(const QString& kind) {
    if (kind == u"brush") {
        return MaskKind::Brush;
    }
    if (kind == u"linear") {
        return MaskKind::Linear;
    }
    if (kind == u"radial") {
        return MaskKind::Radial;
    }
    return std::nullopt;
}

EditMask* find_mask(EditSettings& settings, quint32 id) {
    for (EditMask& mask : settings.masks) {
        if (mask.id == id) {
            return &mask;
        }
    }
    return nullptr;
}

qsizetype index_of(const QList<EditMask>& masks, quint32 id) {
    for (qsizetype i = 0; i < masks.size(); ++i) {
        if (masks[i].id == id) {
            return i;
        }
    }
    return -1;
}

}  // namespace

// ---- MaskListModel ---------------------------------------------------------------------------------------

MaskListModel::MaskListModel(QObject* parent) : QAbstractListModel(parent) {}

int MaskListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : count();
}

QString MaskListModel::kind_key(MaskKind kind) {
    switch (kind) {
        case MaskKind::Brush:
            return QStringLiteral("brush");
        case MaskKind::Linear:
            return QStringLiteral("linear");
        case MaskKind::Radial:
            break;
    }
    return QStringLiteral("radial");
}

QString MaskListModel::name_of(const QList<EditMask>& masks, qsizetype index) {
    if (index < 0 || index >= masks.size()) {
        return {};
    }
    const MaskKind kind = masks[index].kind;
    qsizetype number = 0;
    for (qsizetype i = 0; i <= index; ++i) {
        number += masks[i].kind == kind ? 1 : 0;
    }
    switch (kind) {
        case MaskKind::Brush:
            return tr_text("Brush %1").arg(number);
        case MaskKind::Linear:
            return tr_text("Linear %1").arg(number);
        case MaskKind::Radial:
            break;
    }
    return tr_text("Radial %1").arg(number);
}

QVariant MaskListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) {
        return {};
    }
    const EditMask& mask = masks_[index.row()];
    switch (role) {
        case MaskIdRole:
            return static_cast<int>(mask.id);
        case Qt::DisplayRole:
        case NameRole:
            return name_of(masks_, index.row());
        case KindRole:
            return kind_key(mask.kind);
        case KindLabelRole:
            return mask_kind_label(mask.kind);
        case EnabledRole:
            return mask.enabled;
        case InvertRole:
            return mask.invert;
        case OpacityRole:
            return std::round(mask.opacity * 100.0);
        case SelectedRole:
            return mask.id == selected_;
        case HasEffectRole:
            return mask.has_effect();
        default:
            break;
    }
    return {};
}

QHash<int, QByteArray> MaskListModel::roleNames() const {
    return {
        {MaskIdRole, "maskId"},       {NameRole, "name"},       {KindRole, "kind"},
        {KindLabelRole, "kindLabel"}, {EnabledRole, "maskEnabled"}, {InvertRole, "inverted"},
        {OpacityRole, "maskOpacity"}, {SelectedRole, "selected"}, {HasEffectRole, "hasEffect"},
    };
}

void MaskListModel::set_masks(const QList<EditMask>& masks, quint32 selected) {
    bool same_rows = masks.size() == masks_.size();
    for (qsizetype i = 0; same_rows && i < masks.size(); ++i) {
        same_rows = masks[i].id == masks_[i].id && masks[i].kind == masks_[i].kind;
    }
    if (!same_rows) {
        const int before = count();
        beginResetModel();
        masks_ = masks;
        selected_ = selected;
        endResetModel();
        if (count() != before) {
            emit countChanged();
        }
        return;
    }
    const QList<EditMask> old = masks_;
    const quint32 old_selected = selected_;
    masks_ = masks;
    selected_ = selected;
    for (qsizetype i = 0; i < masks_.size(); ++i) {
        const bool selection_changed = (masks_[i].id == old_selected) != (masks_[i].id == selected_);
        if (selection_changed || !(old[i] == masks_[i])) {
            const QModelIndex at = index(static_cast<int>(i));
            emit dataChanged(at, at);
        }
    }
}

// ---- MaskEditor -------------------------------------------------------------------------------------------

MaskEditor::MaskEditor(EditSession& session, QObject* parent)
    : QObject(parent), session_(session), list_(std::make_unique<MaskListModel>()) {
    connect(&session_, &EditSession::settingsChanged, this, &MaskEditor::refresh);
    connect(&session_, &EditSession::maskSelectionChanged, this, &MaskEditor::refresh);
    connect(&session_, &EditSession::stateChanged, this, &MaskEditor::refresh);
    const auto overlay_and_state = [this] {
        emit changed();
        emit overlayChanged();
    };
    connect(&session_, &EditSession::frameChanged, this, overlay_and_state);
    connect(&session_, &EditSession::toolChanged, this, [this, overlay_and_state] {
        // A gesture never outlives the tool (its pointer area is gone).
        if (session_.tool() != u"masks") {
            endHandle();
            endStroke();
        }
        overlay_and_state();
    });
    connect(&session_, &EditSession::maskOverlayChanged, this, overlay_and_state);
}

MaskEditor::~MaskEditor() = default;

void MaskEditor::refresh() {
    list_->set_masks(session_.settings_known() ? session_.settings().masks : QList<EditMask>{},
                     session_.selected_mask());
    emit changed();
    emit overlayChanged();
}

const EditMask* MaskEditor::selected_mask() const {
    if (!session_.settings_known()) {
        return nullptr;
    }
    const quint32 id = session_.selected_mask();
    for (const EditMask& mask : session_.settings().masks) {
        if (mask.id == id) {
            return &mask;
        }
    }
    return nullptr;
}

int MaskEditor::count() const {
    return session_.settings_known() ? static_cast<int>(session_.settings().masks.size()) : 0;
}

int MaskEditor::selected_id() const {
    const EditMask* mask = selected_mask();
    return mask != nullptr ? static_cast<int>(mask->id) : 0;
}

QString MaskEditor::selected_kind() const {
    const EditMask* mask = selected_mask();
    return mask != nullptr ? MaskListModel::kind_key(mask->kind) : QString();
}

QString MaskEditor::selected_name() const {
    if (!session_.settings_known()) {
        return {};
    }
    const QList<EditMask>& masks = session_.settings().masks;
    return MaskListModel::name_of(masks, index_of(masks, session_.selected_mask()));
}

bool MaskEditor::can_add() const {
    return session_.can_edit() && session_.settings_known() && count() < kMaxMasks && session_.frame_map().valid;
}

bool MaskEditor::showing() const {
    return session_.tool() == u"masks" && session_.settings_known() && session_.has_frame() &&
           session_.frame_map().valid && !session_.frame_uncropped();
}

bool MaskEditor::brush_active() const {
    const EditMask* mask = selected_mask();
    return showing() && session_.can_edit() && (mask == nullptr || mask->kind == MaskKind::Brush);
}

void MaskEditor::set_erase(bool erase) {
    if (erase != erase_) {
        erase_ = erase;
        emit changed();
    }
}

double MaskEditor::brush_radius() const {
    const GeometryMap& map = session_.frame_map();
    if (!map.valid || map.output_size.isEmpty()) {
        return 0.0;
    }
    return session_.brush_size() / 100.0 * map.pixels_per_unit_length / map.output_size.width();
}

int MaskEditor::brush_pixels() const {
    const GeometryMap& map = session_.frame_map();
    return map.valid ? static_cast<int>(std::lround(session_.brush_size() / 100.0 * map.pixels_per_unit_length)) : 0;
}

bool MaskEditor::overlay() const {
    return session_.mask_overlay();
}

void MaskEditor::set_overlay(bool overlay) {
    session_.set_mask_overlay(overlay);
}

QString MaskEditor::overlay_source() const {
    return session_.mask_overlay_source();
}

std::optional<QPointF> MaskEditor::to_normalized(double u, double v) const {
    const GeometryMap& map = session_.frame_map();
    if (!map.valid || !std::isfinite(u) || !std::isfinite(v)) {
        return std::nullopt;
    }
    bool invertible = false;
    const QTransform inverse = map.normalized_to_output.inverted(&invertible);
    if (!invertible) {
        return std::nullopt;
    }
    return inverse.map(QPointF(u * map.output_size.width(), v * map.output_size.height()));
}

std::optional<QPointF> MaskEditor::normalized_at(double u, double v) const {
    return to_normalized(u, v);
}

std::optional<QPointF> MaskEditor::displayed_at(QPointF normalized) const {
    if (!session_.frame_map().valid) {
        return std::nullopt;
    }
    return to_displayed(normalized);
}

QPointF MaskEditor::to_upright_pixels(QPointF normalized) const {
    const QSize size = session_.frame_map().upright_size;
    return QPointF(normalized.x() * size.width(), normalized.y() * size.height());
}

QPointF MaskEditor::to_displayed(QPointF normalized) const {
    const GeometryMap& map = session_.frame_map();
    const QPointF output = map.normalized_to_output.map(normalized);
    return QPointF(output.x() / std::max(1, map.output_size.width()), output.y() / std::max(1, map.output_size.height()));
}

quint32 MaskEditor::next_id() const {
    quint32 id = 0;
    for (const EditMask& mask : session_.settings().masks) {
        id = std::max(id, mask.id);
    }
    return id + 1;
}

void MaskEditor::apply(const EditSettings& next) {
    session_.finish_pending_gesture();
    session_.edit_settings(next);
}

bool MaskEditor::add(const QString& kind) {
    const std::optional<MaskKind> parsed = parse_kind(kind);
    if (!parsed || !can_add() || handle_ || stroke_) {
        return false;
    }
    session_.setTool(QStringLiteral("masks"));
    // Placed on the photo as it is about to be shown: the settings' geometry, not the newest frame's
    // (still the crop tool's uncropped frame when the mask is added from it).
    GeometryMap map = session_.service().map_geometry(session_.settings().geometry, session_.frame_full_upright_size());
    bool invertible = false;
    QTransform inverse = map.normalized_to_output.inverted(&invertible);
    if (!map.valid || !invertible || map.output_size.isEmpty()) {
        map = session_.frame_map();
        inverse = map.normalized_to_output.inverted(&invertible);
    }
    const auto at = [&](double u, double v) {
        const QPointF normalized =
            invertible ? inverse.map(QPointF(u * map.output_size.width(), v * map.output_size.height())) : QPointF(u, v);
        return tidy_point(normalized);
    };
    EditMask mask;
    mask.id = next_id();
    mask.kind = *parsed;
    switch (*parsed) {
        case MaskKind::Linear:
            // From the top of the displayed photo (fully covered) to its middle: a sky gradient.
            mask.start = at(0.5, 0.0);
            mask.end = at(0.5, 0.5);
            mask.feather = 1.0;
            break;
        case MaskKind::Radial: {
            mask.center = at(0.5, 0.5);
            const double radius = 0.3 * std::min(map.output_size.width(), map.output_size.height()) /
                                  std::max(1.0, map.pixels_per_unit_length);
            mask.radius_x = std::clamp(round6(radius), kMinRadialRadius, kMaxRadialRadius);
            mask.radius_y = mask.radius_x;
            mask.feather = 0.5;
            break;
        }
        case MaskKind::Brush:
            break;
    }
    EditSettings next = session_.settings();
    next.masks << mask;
    apply(next);
    session_.set_selected_mask(mask.id);
    return index_of(session_.settings().masks, mask.id) >= 0;
}

void MaskEditor::startBrush() {
    session_.setTool(QStringLiteral("masks"));
    const EditMask* mask = selected_mask();
    if (mask == nullptr || mask->kind != MaskKind::Brush) {
        static_cast<void>(add(QStringLiteral("brush")));
    }
}

void MaskEditor::select(int id) {
    session_.set_selected_mask(id > 0 ? static_cast<quint32>(id) : 0u);
}

void MaskEditor::remove(int id) {
    if (!session_.can_edit() || handle_ || stroke_) {
        return;
    }
    EditSettings next = session_.settings();
    const qsizetype at = index_of(next.masks, static_cast<quint32>(id));
    if (at < 0) {
        return;
    }
    next.masks.removeAt(at);
    const bool was_selected = session_.selected_mask() == static_cast<quint32>(id);
    apply(next);
    if (was_selected) {
        const QList<EditMask>& masks = session_.settings().masks;
        session_.set_selected_mask(masks.isEmpty() ? 0u : masks[std::min(at, masks.size() - 1)].id);
    }
}

void MaskEditor::duplicate(int id) {
    if (!can_add() || handle_ || stroke_) {
        return;
    }
    EditSettings next = session_.settings();
    const qsizetype at = index_of(next.masks, static_cast<quint32>(id));
    if (at < 0) {
        return;
    }
    EditMask copy = next.masks[at];
    copy.id = next_id();
    next.masks.insert(at + 1, copy);
    if (brush_points(next.masks) > kMaxBrushPoints) {
        session_.show_message(tr_text("This brush mask cannot be duplicated: the photo's brush masks would hold "
                                      "more points than Lucent allows."));
        return;
    }
    apply(next);
    session_.set_selected_mask(copy.id);
}

void MaskEditor::move(int id, int delta) {
    if (!session_.can_edit() || handle_ || stroke_ || delta == 0) {
        return;
    }
    EditSettings next = session_.settings();
    const qsizetype at = index_of(next.masks, static_cast<quint32>(id));
    if (at < 0) {
        return;
    }
    const qsizetype to = std::clamp<qsizetype>(at + delta, 0, next.masks.size() - 1);
    if (to == at) {
        return;
    }
    next.masks.move(at, to);
    apply(next);
}

void MaskEditor::setEnabled(int id, bool enabled) {
    EditSettings next = session_.settings();
    EditMask* mask = find_mask(next, static_cast<quint32>(id));
    if (mask == nullptr || mask->enabled == enabled || handle_ || stroke_) {
        return;
    }
    mask->enabled = enabled;
    apply(next);
}

void MaskEditor::setInvert(int id, bool invert) {
    EditSettings next = session_.settings();
    EditMask* mask = find_mask(next, static_cast<quint32>(id));
    if (mask == nullptr || mask->invert == invert || handle_ || stroke_) {
        return;
    }
    mask->invert = invert;
    apply(next);
}

// ---- Handles ------------------------------------------------------------------------------------------------

QVariantList MaskEditor::handles() const {
    QVariantList list;
    const EditMask* mask = selected_mask();
    if (!showing() || mask == nullptr || mask->kind == MaskKind::Brush) {
        return list;
    }
    const auto add = [&](const char* key, QPointF normalized) {
        const QPointF shown = to_displayed(normalized);
        list << QVariantMap{{QStringLiteral("key"), QString::fromLatin1(key)},
                            {QStringLiteral("x"), shown.x()},
                            {QStringLiteral("y"), shown.y()}};
    };
    if (mask->kind == MaskKind::Linear) {
        add("start", mask->start);
        add("end", mask->end);
        add("middle", (mask->start + mask->end) / 2.0);
        return list;
    }
    const QSize size = session_.frame_map().upright_size;
    const double long_edge = std::max(size.width(), size.height());
    const double angle = mask->angle_degrees * kPi / 180.0;
    const QPointF centre = to_upright_pixels(mask->center);
    const auto normalized = [&](QPointF pixel) {
        return QPointF(pixel.x() / std::max(1, size.width()), pixel.y() / std::max(1, size.height()));
    };
    // Counter-clockwise as displayed with y growing downwards.
    const QPointF x_axis(std::cos(angle), -std::sin(angle));
    const QPointF y_axis(std::sin(angle), std::cos(angle));
    add("center", mask->center);
    add("radiusX", normalized(centre + x_axis * (mask->radius_x * long_edge)));
    add("radiusY", normalized(centre - y_axis * (mask->radius_y * long_edge)));
    return list;
}

void MaskEditor::beginHandle(const QString& key, double u, double v) {
    const EditMask* mask = selected_mask();
    if (handle_ || stroke_ || !showing() || !session_.can_edit() || mask == nullptr ||
        mask->kind == MaskKind::Brush) {
        return;
    }
    const std::optional<QPointF> pointer = to_normalized(u, v);
    if (!pointer) {
        return;
    }
    // Where the handle is, so that it keeps its distance from the pointer.
    std::optional<QPointF> position;
    for (const QVariant& handle : handles()) {
        const QVariantMap entry = handle.toMap();
        if (entry.value(QStringLiteral("key")).toString() == key) {
            position = to_normalized(entry.value(QStringLiteral("x")).toDouble(), entry.value(QStringLiteral("y")).toDouble());
        }
    }
    if (!position) {
        return;
    }
    session_.begin_pointer_gesture();
    handle_ = HandleDrag{key, *position - *pointer, *pointer, *mask};
    emit changed();
    emit overlayChanged();
}

void MaskEditor::moveHandle(double u, double v) {
    if (!handle_) {
        return;
    }
    const std::optional<QPointF> pointer = to_normalized(u, v);
    if (!pointer) {
        return;
    }
    EditSettings next = session_.settings();
    EditMask* mask = find_mask(next, handle_->start.id);
    if (mask == nullptr) {
        return;
    }
    const QPointF target = *pointer + handle_->offset;
    const QSize size = session_.frame_map().upright_size;
    const double long_edge = std::max(1, std::max(size.width(), size.height()));
    const QString& key = handle_->key;
    if (key == u"center") {
        mask->center = tidy_point(target);
    } else if (key == u"radiusX" || key == u"radiusY") {
        const QPointF d = to_upright_pixels(target) - to_upright_pixels(mask->center);
        const double radius = std::clamp(round6(std::hypot(d.x(), d.y()) / long_edge), kMinRadialRadius,
                                         kMaxRadialRadius);
        if (key == u"radiusX") {
            mask->radius_x = radius;
            if (std::hypot(d.x(), d.y()) > 1.0) {
                const double angle = std::round(std::atan2(-d.y(), d.x()) * 180.0 / kPi * 100.0) / 100.0;
                mask->angle_degrees = std::clamp(angle == 0.0 ? 0.0 : angle, -180.0, 180.0);
            }
        } else {
            mask->radius_y = radius;
        }
    } else if (key == u"start" || key == u"end") {
        const QPointF point = tidy_point(target);
        const QPointF other = key == u"start" ? mask->end : mask->start;
        if (std::abs(point.x() - other.x()) < kMinGradientLength && std::abs(point.y() - other.y()) < kMinGradientLength) {
            return;
        }
        (key == u"start" ? mask->start : mask->end) = point;
    } else if (key == u"middle") {
        const QPointF delta = *pointer - handle_->press;
        mask->start = tidy_point(handle_->start.start + delta);
        mask->end = tidy_point(handle_->start.end + delta);
        if (std::abs(mask->start.x() - mask->end.x()) < kMinGradientLength &&
            std::abs(mask->start.y() - mask->end.y()) < kMinGradientLength) {
            return;  // both ends pressed against the same bound
        }
    } else {
        return;
    }
    session_.edit_settings(next);
}

void MaskEditor::endHandle() {
    if (!handle_) {
        return;
    }
    handle_.reset();
    session_.end_pointer_gesture();
    emit changed();
    emit overlayChanged();
}

// ---- Brush ----------------------------------------------------------------------------------------------------

void MaskEditor::beginStroke(double u, double v, bool erase) {
    if (stroke_ || handle_ || !showing() || !session_.can_edit()) {
        return;
    }
    const std::optional<QPointF> pointer = to_normalized(u, v);
    if (!pointer) {
        return;
    }
    EditSettings next = session_.settings();
    EditMask* mask = find_mask(next, session_.selected_mask());
    if (mask == nullptr || mask->kind != MaskKind::Brush) {
        if (next.masks.size() >= kMaxMasks) {
            session_.show_message(tr_text("This photo has the most masks Lucent allows; delete one to add a brush."));
            return;
        }
        EditMask brush;
        brush.id = next_id();
        brush.kind = MaskKind::Brush;
        next.masks << brush;
        mask = &next.masks.back();
    }
    if (mask->strokes.size() >= kMaxBrushStrokes) {
        session_.show_message(
            tr_text("This brush mask holds the most strokes Lucent allows; add another brush mask to paint more."));
        return;
    }
    if (brush_points(next.masks) >= kMaxBrushPoints) {
        session_.show_message(tr_text("The photo's brush masks hold the most brush points Lucent allows; delete a "
                                      "brush mask to paint more."));
        return;
    }
    // The strokes painted before: drawn once for the whole gesture.
    OverlayPaths settled;
    for (const EditBrushStroke& before : mask->strokes) {
        settled << stroke_path(before, mask->enabled);
    }
    const qsizetype settled_strokes = mask->strokes.size();
    EditBrushStroke stroke;
    stroke.mode = erase || erase_ ? BrushMode::Erase : BrushMode::Add;
    stroke.radius = std::clamp(round6(session_.brush_size() / 100.0), kMinBrushRadius, kMaxBrushRadius);
    stroke.hardness = std::clamp(round6(1.0 - session_.brush_feather() / 100.0), 0.0, 1.0);
    stroke.flow = std::clamp(round6(session_.brush_flow() / 100.0), 0.0, 1.0);
    stroke.points << tidy_point(*pointer);
    mask->strokes << stroke;
    const quint32 id = mask->id;
    session_.begin_pointer_gesture();
    stroke_ = Stroke{id, to_upright_pixels(*pointer)};
    stroke_->settled = QVariant::fromValue(settled);
    stroke_->settled_strokes = settled_strokes;
    session_.edit_settings(next);
    session_.set_selected_mask(id);
    emit changed();
    emit overlayChanged();
}

bool MaskEditor::add_stroke_point(EditSettings& settings, QPointF normalized, bool force) {
    EditMask* mask = find_mask(settings, stroke_->mask);
    if (mask == nullptr || mask->strokes.isEmpty() || stroke_->full) {
        return false;
    }
    const QPointF pixel = to_upright_pixels(normalized);
    const QSize size = session_.frame_map().upright_size;
    const double radius_pixels = mask->strokes.back().radius * std::max(size.width(), size.height());
    const double spacing = std::max(kStrokeSpacing * radius_pixels, 0.5);
    const double distance = std::hypot(pixel.x() - stroke_->last_pixel.x(), pixel.y() - stroke_->last_pixel.y());
    if (distance < (force ? 0.5 : spacing)) {
        if (!force) {
            stroke_->pending = normalized;
            stroke_->has_pending = true;
        }
        return false;
    }
    // The engine's bound for one stroke: the gesture paints on as a new stroke from here, which
    // starts at the last point (two points).
    const bool split = mask->strokes.back().points.size() >= kMaxStrokePoints;
    if (brush_points(settings.masks) + (split ? 2 : 1) > kMaxBrushPoints) {
        stroke_->full = true;
        session_.show_message(tr_text("Painting stopped: the photo's brush masks hold the most brush points Lucent "
                                      "allows. Delete a brush mask to paint more."));
        return false;
    }
    if (split) {
        if (mask->strokes.size() >= kMaxBrushStrokes) {
            stroke_->full = true;
            session_.show_message(tr_text("Painting stopped: this brush mask holds the most strokes Lucent allows. "
                                          "Add another brush mask to paint more."));
            return false;
        }
        EditBrushStroke next = mask->strokes.back();
        next.points = {next.points.back()};
        // The finished part joins the strokes drawn once.
        OverlayPaths settled = stroke_->settled.value<OverlayPaths>();
        while (stroke_->settled_strokes < mask->strokes.size()) {
            settled << stroke_path(mask->strokes[stroke_->settled_strokes++], mask->enabled);
        }
        stroke_->settled = QVariant::fromValue(settled);
        mask->strokes << next;
    }
    mask->strokes.back().points << tidy_point(normalized);
    stroke_->last_pixel = pixel;
    stroke_->has_pending = false;
    return true;
}

void MaskEditor::extendStroke(double u, double v) {
    if (!stroke_) {
        return;
    }
    const std::optional<QPointF> pointer = to_normalized(u, v);
    if (!pointer) {
        return;
    }
    EditSettings next = session_.settings();
    if (add_stroke_point(next, *pointer, false)) {
        session_.edit_settings(next);
    }
}

void MaskEditor::endStroke() {
    if (!stroke_) {
        return;
    }
    if (stroke_->has_pending) {
        // The stroke ends where the pointer was released.
        EditSettings next = session_.settings();
        if (add_stroke_point(next, stroke_->pending, true)) {
            session_.edit_settings(next);
        }
    }
    stroke_.reset();
    session_.end_pointer_gesture();
    emit changed();
    emit overlayChanged();
}

// ---- Outlines -----------------------------------------------------------------------------------------------

OverlayPath MaskEditor::stroke_path(const EditBrushStroke& stroke, bool enabled) const {
    QPolygonF shown;
    shown.reserve(stroke.points.size());
    for (const QPointF& point : stroke.points) {
        shown << to_displayed(point);
    }
    return OverlayPath{shown, true, !enabled};
}

QVariant MaskEditor::stroke_outline() const {
    OverlayPaths paths;
    const EditMask* mask = selected_mask();
    if (!stroke_ || !showing() || mask == nullptr || mask->id != stroke_->mask) {
        return QVariant::fromValue(paths);
    }
    for (qsizetype i = stroke_->settled_strokes; i < mask->strokes.size(); ++i) {
        paths << stroke_path(mask->strokes[i], mask->enabled);
    }
    return QVariant::fromValue(paths);
}

QVariant MaskEditor::outlines() const {
    OverlayPaths paths;
    const EditMask* mask = selected_mask();
    if (!showing() || mask == nullptr) {
        return QVariant::fromValue(paths);
    }
    if (stroke_ && stroke_->mask == mask->id) {
        // Painting: the strokes before this gesture's, unchanged until it ends (strokeOutline draws it).
        return stroke_->settled;
    }
    const bool moving = handle_ && handle_->start.id == mask->id;
    const FrameMaskOutline* framed = nullptr;
    for (const FrameMaskOutline& outline : session_.frame_masks()) {
        if (outline.id == mask->id) {
            framed = &outline;
        }
    }
    if (!moving && framed != nullptr) {
        // What was rendered (DevelopFrame::masks).
        for (const QPolygonF& line : framed->polylines) {
            paths << OverlayPath{line, true, !mask->enabled};
        }
        return QVariant::fromValue(paths);
    }
    // The local settings through the engine's outline and the frame's mapping.
    const QList<QPolygonF> lines = session_.service().mask_outline(*mask, session_.frame_map().upright_size);
    for (const QPolygonF& line : lines) {
        QPolygonF shown;
        shown.reserve(line.size());
        for (const QPointF& point : line) {
            shown << to_displayed(point);
        }
        paths << OverlayPath{shown, true, !mask->enabled};
    }
    return QVariant::fromValue(paths);
}

}  // namespace lucent::ui
