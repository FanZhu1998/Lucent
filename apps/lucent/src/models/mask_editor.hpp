#pragma once

// MaskEditor and MaskListModel: the Edit view's masks (blueprint section 2: "brush,
// linear-gradient, and radial masks with feathering for detailed local work"; ADR 0015). EditSession
// owns them (EditSession.masks, .masks.list); the tool is open while EditSession.tool is "masks"
// (B, G and Shift+R add a brush, linear or radial mask and open it).
//
// The list. Every mask of the settings, in their order (the order they are applied in), with a
// name, its kind, enabled, invert and opacity. The recipe stores no names, so a mask is named by its
// kind and its place among the masks of that kind ("Radial 2"). The selected mask (EditSession::
// selected_mask) is the one the panel's local controls (EditSession keys "maskExposure",
// "maskContrast", ..., "maskOpacity", "maskFeather") and the handles edit. Adding, deleting,
// duplicating, reordering, enabling and inverting are one revision each.
//
// Coordinates. Masks live in the normalised upright frame U; the pointer and the overlay work in
// fractions (u, v) of the displayed image. The mapping between them is the engine's GeometryMapping
// of the newest frame's geometry at full resolution (EditSession::frame_map(), EditService::
// map_geometry()), so a handle sits where the engine puts that mask point whatever the crop,
// straightening and quarter turns, and lengths (radii, the brush) are in the frame's isometric output
// pixels.
//
// Outlines (`outlines`, OverlayPaths) are the selected mask's: the frame's own (DevelopFrame::masks,
// what was rendered) while nothing moves the mask, and during a handle drag the engine's
// mask_outline() of the local settings, so the outline follows the pointer between frames. During a
// brush stroke `outlines` holds the mask's strokes before the one being painted (fixed for the
// gesture, so the view does not redraw them on every pointer move) and `strokeOutline` the stroke
// being painted (its centre line, as the engine's outline draws it), the only path that changes.
// Handles (`handles`) are always placed from the local settings: radial centre, the x semi-axis end
// (its direction sets the angle) and the y semi-axis end; linear start, end and middle (moves both).
//
// Gestures. A handle drag is ONE revision; so is a brush stroke (beginStroke ... extendStroke ...
// endStroke: transient positions, then a commit), also when it starts a new brush mask. Strokes are
// simplified as they are drawn: a point is kept only when it lies at least kStrokeSpacing brush radii
// from the previous one (and at least half a pixel), so a stroke stays far below the engine's 4096
// points; a stroke that reaches them continues as a new stroke of the same gesture, and painting
// stops at 65536 points of all brush masks. The brush (size as a percentage of the long edge,
// feather, flow) comes from EditSession's "brushSize", "brushFeather" and "brushFlow"; Alt or
// `erase` paints an erase stroke. The cursor's radius (`brushRadius`) is in fractions of the displayed
// image's width, so the view draws it at the brush's size on screen.
//
// Thread-safety: UI thread only.

#include <QAbstractListModel>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>
#include <memory>
#include <optional>

#include "imaging/overlay_path_item.hpp"
#include "services/edit_service.hpp"

namespace lucent::ui {

class EditSession;

class MaskListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("MaskListModel is provided by EditSession.masks.list.")
    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)

public:
    enum Role {
        MaskIdRole = Qt::UserRole + 1,
        NameRole,
        KindRole,       // "brush", "linear", "radial"
        KindLabelRole,  // "Brush", "Linear gradient", "Radial gradient"
        EnabledRole,
        InvertRole,
        OpacityRole,  // 0..100
        SelectedRole,
        HasEffectRole,
    };

    explicit MaskListModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] int count() const noexcept { return static_cast<int>(masks_.size()); }

    // Shows `masks` with `selected` selected: rows are updated in place while the ids and their
    // order stay, otherwise the model is reset.
    void set_masks(const QList<EditMask>& masks, quint32 selected);
    [[nodiscard]] const QList<EditMask>& masks() const noexcept { return masks_; }

    // The name of masks[index] ("Brush 1", "Linear 2", "Radial 1": the kind and the place among the
    // masks of that kind).
    [[nodiscard]] static QString name_of(const QList<EditMask>& masks, qsizetype index);
    [[nodiscard]] static QString kind_key(MaskKind kind);

signals:
    void countChanged();

private:
    QList<EditMask> masks_;
    quint32 selected_ = 0;
};

class MaskEditor : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("MaskEditor is provided by EditSession.masks.")
    Q_PROPERTY(lucent::ui::MaskListModel* list READ list CONSTANT FINAL)
    Q_PROPERTY(int count READ count NOTIFY changed FINAL)
    Q_PROPERTY(int selectedId READ selected_id WRITE select NOTIFY changed FINAL)
    // "brush", "linear", "radial"; empty without a selection.
    Q_PROPERTY(QString selectedKind READ selected_kind NOTIFY changed FINAL)
    Q_PROPERTY(QString selectedName READ selected_name NOTIFY changed FINAL)
    Q_PROPERTY(bool canAdd READ can_add NOTIFY changed FINAL)
    // The tool is open on a frame of the photo (handles, outlines and painting are shown).
    Q_PROPERTY(bool showing READ showing NOTIFY changed FINAL)
    // Painting is possible: showing, with a brush mask selected.
    Q_PROPERTY(bool brushActive READ brush_active NOTIFY changed FINAL)
    Q_PROPERTY(bool painting READ painting NOTIFY changed FINAL)
    Q_PROPERTY(bool erase READ erase WRITE set_erase NOTIFY changed FINAL)
    // The brush cursor's radius in fractions of the displayed image's width, and in full-resolution
    // pixels.
    Q_PROPERTY(double brushRadius READ brush_radius NOTIFY changed FINAL)
    Q_PROPERTY(int brushPixels READ brush_pixels NOTIFY changed FINAL)
    // OverlayPaths of the selected mask, and of the stroke being painted (see the file comment).
    Q_PROPERTY(QVariant outlines READ outlines NOTIFY overlayChanged FINAL)
    Q_PROPERTY(QVariant strokeOutline READ stroke_outline NOTIFY overlayChanged FINAL)
    // [{key, x, y}] of the selected linear or radial mask's handles (displayed-image fractions).
    Q_PROPERTY(QVariantList handles READ handles NOTIFY overlayChanged FINAL)
    // The weight overlay (O): EditSession.maskOverlay and its image.
    Q_PROPERTY(bool overlay READ overlay WRITE set_overlay NOTIFY changed FINAL)
    Q_PROPERTY(QString overlaySource READ overlay_source NOTIFY overlayChanged FINAL)

public:
    // A stroke keeps a point when it lies this many brush radii from the previous one.
    static constexpr double kStrokeSpacing = 0.15;

    explicit MaskEditor(EditSession& session, QObject* parent = nullptr);
    ~MaskEditor() override;

    // Adds a mask of `kind` ("brush", "linear", "radial") with its default shape placed on the
    // displayed image, selects it and opens the masks tool (one revision). False when it cannot be
    // added (32 masks, read-only, no frame yet).
    Q_INVOKABLE bool add(const QString& kind);
    // B: opens the masks tool on the selected brush mask, or adds one.
    Q_INVOKABLE void startBrush();
    void select(int id);
    Q_INVOKABLE void remove(int id);
    Q_INVOKABLE void duplicate(int id);
    // Moves the mask `delta` places in the order (applied later when positive).
    Q_INVOKABLE void move(int id, int delta);
    Q_INVOKABLE void setEnabled(int id, bool enabled);
    Q_INVOKABLE void setInvert(int id, bool invert);

    // A handle drag of the selected mask, in displayed-image fractions: one revision.
    Q_INVOKABLE void beginHandle(const QString& key, double u, double v);
    Q_INVOKABLE void moveHandle(double u, double v);
    Q_INVOKABLE void endHandle();

    // A brush stroke into the selected brush mask (a new one when another kind is selected), in
    // displayed-image fractions: one revision.
    Q_INVOKABLE void beginStroke(double u, double v, bool erase);
    Q_INVOKABLE void extendStroke(double u, double v);
    Q_INVOKABLE void endStroke();

    // Normalised upright coordinates of a displayed-image point, and back (developer automation and
    // tests); nullopt without a frame.
    [[nodiscard]] std::optional<QPointF> normalized_at(double u, double v) const;
    [[nodiscard]] std::optional<QPointF> displayed_at(QPointF normalized) const;

    [[nodiscard]] MaskListModel* list() const noexcept { return list_.get(); }
    [[nodiscard]] int count() const;
    [[nodiscard]] int selected_id() const;
    [[nodiscard]] QString selected_kind() const;
    [[nodiscard]] QString selected_name() const;
    [[nodiscard]] bool can_add() const;
    [[nodiscard]] bool showing() const;
    [[nodiscard]] bool brush_active() const;
    [[nodiscard]] bool painting() const noexcept { return stroke_.has_value(); }
    [[nodiscard]] bool erase() const noexcept { return erase_; }
    void set_erase(bool erase);
    [[nodiscard]] double brush_radius() const;
    [[nodiscard]] int brush_pixels() const;
    [[nodiscard]] QVariant outlines() const;
    [[nodiscard]] QVariant stroke_outline() const;
    [[nodiscard]] QVariantList handles() const;
    [[nodiscard]] bool overlay() const;
    void set_overlay(bool overlay);
    [[nodiscard]] QString overlay_source() const;

signals:
    void changed();
    void overlayChanged();

private:
    struct HandleDrag {
        QString key;
        QPointF offset;     // handle minus pointer, normalised (the handle does not jump to the pointer)
        QPointF press;      // normalised
        EditMask start;     // the mask at the press
    };
    struct Stroke {
        quint32 mask = 0;
        QPointF last_pixel;  // upright pixels of the newest point kept
        QPointF pending;     // the newest pointer position not kept (normalised)
        bool has_pending = false;
        bool full = false;   // the point bounds were reached: nothing more is painted
        // The mask's strokes before those of this gesture still being painted (displayed), and how
        // many strokes of the mask they are: `outlines` while painting.
        QVariant settled;
        qsizetype settled_strokes = 0;
    };

    [[nodiscard]] const EditMask* selected_mask() const;
    [[nodiscard]] quint32 next_id() const;
    // A pointer position (displayed-image fractions) in normalised upright coordinates.
    [[nodiscard]] std::optional<QPointF> to_normalized(double u, double v) const;
    [[nodiscard]] QPointF to_upright_pixels(QPointF normalized) const;
    [[nodiscard]] QPointF to_displayed(QPointF normalized) const;
    // Applies `next` as one step (a pending gesture is committed first).
    void apply(const EditSettings& next);
    // Adds the brush point `normalized` to the stroke (see the file comment); false when nothing
    // changed.
    bool add_stroke_point(EditSettings& settings, QPointF normalized, bool force);
    // A brush stroke's centre line in displayed-image fractions.
    [[nodiscard]] OverlayPath stroke_path(const EditBrushStroke& stroke, bool enabled) const;
    void refresh();

    EditSession& session_;
    std::unique_ptr<MaskListModel> list_;
    bool erase_ = false;
    std::optional<HandleDrag> handle_;
    std::optional<Stroke> stroke_;
};

}  // namespace lucent::ui
