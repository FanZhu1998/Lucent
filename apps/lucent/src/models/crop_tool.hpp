#pragma once

// CropTool: the Edit view's crop and rotate tool (blueprint section 2, "crop, rotate, straighten";
// ADR 0015 frames). EditSession owns it (EditSession.crop); the tool is open while EditSession.tool
// is "crop" (R).
//
// Frames. The crop is normalised in the straightened frame S (develop_settings.hpp). While the tool
// is open the frames are rendered without the crop (EditService::set_crop_editing), so the view
// shows the whole straightened image and the crop as an overlay. The overlay and the pointer work
// in the displayed image's own coordinates: fractions (u, v) of its width and height. They are
// mapped to S through EditService::map_geometry() (the engine's GeometryMapping) of the settings'
// geometry without the crop at the photo's full-resolution upright size, so a handle drawn at (u,
// v) is exactly where the engine puts that crop corner, whatever the quarter turns and mirror. The
// crop math itself (handles, aspect ratio, minimum size) runs in those displayed pixels
// (crop_math below), where the aspect ratio is the one the photographer sees.
//
// Gestures. A handle drag is ONE revision (beginDrag ... dragTo ... endDrag: transient positions,
// then a commit). Levelling with a drawn horizon, a quarter turn, the mirror, an aspect ratio, the
// orientation swap and Reset are one revision each. The straighten slider is EditSession's
// "straighten" control (a drag is one revision); straightened() keeps the crop inside the image
// when "Constrain to image" is on: a crop that is the whole image, or the largest one inside the
// previous angle, follows the angle (engine largest_inside_crop() with the crop's aspect ratio);
// another crop shrinks about its centre until the image covers it (engine crop_inside_image()).
// Handle drags stop where the image ends when constrained. Enter closes the tool keeping the
// result; Esc (EditSession::cancelTool) restores the geometry the tool was opened with, as one
// revision when it changed.
//
// Aspect ratios: Original (the photo's own as displayed), Free, 1:1, 4:5, 3:2, 16:9 and Custom; each
// in landscape or portrait. The orientation is the current crop's as displayed (`portrait`: taller
// than wide), never a separate state, so it follows a quarter turn, Esc, undo and another photo;
// swapOrientation() refits the crop in the other one. Choosing a ratio fits the largest crop of it,
// centred, inside the current crop (inside the image when the crop is the whole image). The chosen
// ratio stays for the next photo.
//
// Thread-safety: UI thread only.

#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>
#include <optional>

#include "services/edit_service.hpp"

namespace lucent::ui {

class EditSession;

namespace crop_math {

enum class Handle : std::uint8_t {
    None,
    Move,
    Left,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

// "move", "left", "right", "top", "bottom", "topLeft", "topRight", "bottomLeft", "bottomRight".
[[nodiscard]] Handle parse_handle(const QString& name) noexcept;

// The largest rectangle of `aspect` (width / height, > 0) centred on `within`'s centre and inside it.
[[nodiscard]] QRectF fit_aspect(const QRectF& within, double aspect);

// `start` with `handle` dragged from `press` to `pointer` inside the bounds (0, 0) - `bounds`:
// edges follow the pointer (the opposite edge stays), Move translates without leaving the bounds.
// With `aspect` > 0 the ratio is kept: a corner keeps the opposite corner, an edge its opposite edge
// and the centre line across it. Every side stays at least `min_size`.
[[nodiscard]] QRectF drag(const QRectF& start, Handle handle, QPointF press, QPointF pointer, QSizeF bounds,
                          double aspect, QSizeF min_size);

// The straighten angle (degrees, within [-45, 45]) that makes the line from `a` to `b` (upright pixel
// coordinates, y down) horizontal or vertical, whichever is nearer; nullopt for points closer than
// two pixels.
[[nodiscard]] std::optional<double> level_angle(QPointF a, QPointF b);

}  // namespace crop_math

class CropTool : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("CropTool is provided by EditSession.crop.")
    // The tool can work: a frame of the photo with its geometry is shown and the photo can be edited.
    Q_PROPERTY(bool available READ available NOTIFY changed FINAL)
    // The overlay can be drawn: the tool is open and the newest frame is uncropped with the current
    // quarter turns and mirror (it follows the next frame otherwise).
    Q_PROPERTY(bool showing READ showing NOTIFY changed FINAL)
    // The crop in the displayed image (fractions of its width and height).
    Q_PROPERTY(QRectF rect READ rect NOTIFY changed FINAL)
    Q_PROPERTY(QString aspect READ aspect NOTIFY changed FINAL)
    Q_PROPERTY(bool portrait READ portrait NOTIFY changed FINAL)
    // The custom ratio; notified only when it changes, so a value being typed is not replaced
    // whenever a frame arrives.
    Q_PROPERTY(double customWidth READ custom_width NOTIFY customAspectChanged FINAL)
    Q_PROPERTY(double customHeight READ custom_height NOTIFY customAspectChanged FINAL)
    Q_PROPERTY(bool constrain READ constrain WRITE setConstrain NOTIFY changed FINAL)
    Q_PROPERTY(bool dragging READ dragging NOTIFY changed FINAL)
    // The geometry is not the identity (Reset has something to do).
    Q_PROPERTY(bool modified READ modified NOTIFY changed FINAL)
    // The cropped image's size at full resolution, as displayed ("5464 × 3643 px"); empty unknown.
    Q_PROPERTY(QString sizeText READ size_text NOTIFY changed FINAL)

public:
    explicit CropTool(EditSession& session, QObject* parent = nullptr);

    // [{key, label}] of the aspect-ratio presets, in menu order.
    Q_INVOKABLE QVariantList aspectOptions() const;
    Q_INVOKABLE void setAspect(const QString& key);
    Q_INVOKABLE void swapOrientation();
    Q_INVOKABLE void setCustomAspect(double width, double height);
    void setConstrain(bool constrain);

    // A handle drag in displayed-image fractions (see the file comment): one revision.
    Q_INVOKABLE void beginDrag(const QString& handle, double u, double v);
    Q_INVOKABLE void dragTo(double u, double v);
    Q_INVOKABLE void endDrag();
    // Levels the photo along the line drawn from (u0, v0) to (u1, v1) on the displayed image (one
    // revision); false when the line is too short.
    Q_INVOKABLE bool level(double u0, double v0, double u1, double v1);
    Q_INVOKABLE void rotateLeft();
    Q_INVOKABLE void rotateRight();
    Q_INVOKABLE void flip();
    // Straighten 0, the whole image, no quarter turn and no mirror (one revision).
    Q_INVOKABLE void reset();
    // Sets the crop to fractions of the straightened image (developer automation; one revision).
    Q_INVOKABLE void setCrop(double left, double top, double width, double height);

    [[nodiscard]] bool available() const;
    [[nodiscard]] bool showing() const;
    [[nodiscard]] QRectF rect() const;
    [[nodiscard]] QString aspect() const { return aspect_; }
    // The current crop is taller than wide as displayed (a square counts as landscape).
    [[nodiscard]] bool portrait() const;
    [[nodiscard]] double custom_width() const noexcept { return custom_width_; }
    [[nodiscard]] double custom_height() const noexcept { return custom_height_; }
    [[nodiscard]] bool constrain() const noexcept { return constrain_; }
    [[nodiscard]] bool dragging() const noexcept { return drag_.has_value(); }
    [[nodiscard]] bool modified() const;
    [[nodiscard]] QString size_text() const;

    // The geometry after the straighten angle of `geometry` becomes `degrees` (clamped to +-45,
    // two decimals), the crop kept inside the image when constrained (see the file comment).
    [[nodiscard]] EditGeometry straightened(const EditGeometry& geometry, double degrees) const;
    // The displayed aspect ratio (width / height) of the current preset in the current crop's
    // orientation; 0 for Free.
    [[nodiscard]] double display_aspect() const;
    // The same in the orientation given.
    [[nodiscard]] double aspect_for(bool portrait) const;
    // The mapping of the displayed (uncropped) image for `geometry` at full resolution.
    [[nodiscard]] GeometryMap display_map(const EditGeometry& geometry) const;
    // The crop of `geometry` in displayed pixels of `map`, and back (clamped, six decimals).
    [[nodiscard]] static QRectF to_display(const GeometryMap& map, const EditCrop& crop);
    [[nodiscard]] static EditCrop from_display(const GeometryMap& map, const QRectF& rect);

signals:
    void changed();
    void customAspectChanged();

private:
    struct Drag {
        crop_math::Handle handle = crop_math::Handle::None;
        QRectF start;  // displayed pixels
        QPointF press;
        GeometryMap map;
        double aspect = 0.0;  // display_aspect() at the press
    };

    [[nodiscard]] bool inside(const EditGeometry& geometry) const;
    // `geometry` with its crop shrunk about its centre until the image covers it.
    [[nodiscard]] EditGeometry fit_inside(EditGeometry geometry) const;
    // The crop's aspect ratio in straightened pixels.
    [[nodiscard]] double straightened_aspect(const EditCrop& crop) const;
    [[nodiscard]] QSizeF min_size(const GeometryMap& map) const;
    // Applies `geometry` as one step (a pending gesture is committed first).
    void apply_geometry(const EditGeometry& geometry);
    // The crop of the current settings refitted to the current aspect preset in the orientation
    // `portrait` (one step): the largest such crop inside the current one, or with `keep_area` the
    // same area about the same centre (an orientation swap).
    void refit(bool portrait, bool keep_area);

    EditSession& session_;
    QString aspect_ = QStringLiteral("free");
    double custom_width_ = 5.0;
    double custom_height_ = 4.0;
    bool constrain_ = true;
    std::optional<Drag> drag_;
};

}  // namespace lucent::ui
