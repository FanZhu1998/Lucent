// W9 (ADR 0015) view models against a recording EditService whose geometry and mask helpers are the
// engine's own (the adapters'): the crop math and the crop tool's handles mapped through
// GeometryMapping (quarter turns, aspect ratios, "Constrain to image", levelling, cancel), the mask
// list (names, add, duplicate, reorder, enable, invert, delete: one revision each), a brush stroke
// and a handle drag as ONE revision each with simplified points, the new control keys, and the
// copy categories (Detail and Geometry, Geometry not selected by default).

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

#include <QTest>

#include "models/crop_tool.hpp"
#include "models/edit_session.hpp"
#include "models/mask_editor.hpp"
#include "test_support.hpp"

namespace lucent::ui {
namespace {

using testing::RecordingEditService;

constexpr QSize kUpright{6000, 4000};

class CropMaskModelsTest : public ::testing::Test {
protected:
    CropMaskModelsTest() : store(std::make_shared<FrameStore>()), session(service, store) {}

    // Opens item 7 with `settings` and shows a frame of them (uncropped while the crop tool is open).
    void open(const EditSettings& settings = {}) {
        session.set_item(7);
        EditState state;
        state.item = 7;
        state.settings = settings;
        state.committed = settings;
        service.emit_state(state);
        show_frame();
    }

    // A frame of the session's current settings, as the service would render them.
    void show_frame() {
        auto frame = std::make_shared<EditFrame>();
        frame->item = 7;
        frame->sequence = ++sequence;
        frame->phase = FramePhase::Final;
        frame->geometry = session.settings().geometry;
        frame->uncropped = session.tool() == u"crop";
        if (frame->uncropped) {
            frame->geometry.crop = EditCrop{};
        }
        frame->full_upright_size = kUpright;
        const GeometryMap map = service.map_geometry(frame->geometry, kUpright);
        frame->full_output_size = map.output_size;
        frame->image = testing::solid_image(QSize(map.output_size.width() / 10, map.output_size.height() / 10));
        frame->info = std::make_shared<RenderInfo>();
        service.emit_frame(frame);
    }

    // Answers the newest call with the settings the session holds (as the service would).
    void answer() {
        EditState state;
        state.item = 7;
        state.token = service.calls.empty() ? 0 : service.calls.back().token;
        state.settings = session.settings();
        state.committed = session.settings();
        service.emit_state(state);
    }

    [[nodiscard]] EditSettings last_sent() const {
        for (auto it = service.calls.crbegin(); it != service.calls.crend(); ++it) {
            if (it->name == u"apply" || it->name == u"set_transient") {
                return it->settings;
            }
        }
        return {};
    }

    RecordingEditService service;
    std::shared_ptr<FrameStore> store;
    EditSession session;
    quint64 sequence = 0;
};

// ---- Crop math ------------------------------------------------------------------------------------------

TEST(CropMath, HandlesKeepTheAspectRatioTheBoundsAndTheMinimumSize) {
    using crop_math::Handle;
    const QSizeF bounds(6000, 4000);
    const QSizeF min(32, 32);
    const QRectF start(1000, 1000, 3000, 2000);  // 3:2

    // Free: the edge follows the pointer, the opposite edge stays.
    const QRectF left = crop_math::drag(start, Handle::Left, {1000, 1500}, {500, 1500}, bounds, 0.0, min);
    EXPECT_EQ(left, QRectF(QPointF(500, 1000), QPointF(4000, 3000)));
    // Beyond the bounds the edge stops at them.
    const QRectF far = crop_math::drag(start, Handle::Top, {2500, 1000}, {2500, -800}, bounds, 0.0, min);
    EXPECT_DOUBLE_EQ(far.top(), 0.0);
    EXPECT_DOUBLE_EQ(far.bottom(), 3000.0);
    // Past the opposite edge: the minimum size.
    const QRectF tiny = crop_math::drag(start, Handle::Right, {4000, 1500}, {200, 1500}, bounds, 0.0, min);
    EXPECT_DOUBLE_EQ(tiny.width(), 32.0);

    // 3:2 kept by a corner (the opposite corner stays) and by an edge (its centre line stays).
    const QRectF corner = crop_math::drag(start, Handle::BottomRight, {4000, 3000}, {5000, 3100}, bounds, 1.5, min);
    EXPECT_NEAR(corner.width() / corner.height(), 1.5, 1e-9);
    EXPECT_EQ(corner.topLeft(), QPointF(1000, 1000));
    EXPECT_NEAR(corner.width(), 4000.0, 1e-9);
    const QRectF edge = crop_math::drag(start, Handle::Right, {4000, 2000}, {4600, 2000}, bounds, 1.5, min);
    EXPECT_NEAR(edge.width() / edge.height(), 1.5, 1e-9);
    EXPECT_NEAR(edge.center().y(), 2000.0, 1e-9);
    EXPECT_NEAR(edge.left(), 1000.0, 1e-9);
    // A corner pulled beyond the image stops where the ratio still fits.
    const QRectF stopped = crop_math::drag(start, Handle::BottomRight, {4000, 3000}, {9000, 9000}, bounds, 1.5, min);
    EXPECT_NEAR(stopped.width() / stopped.height(), 1.5, 1e-9);
    EXPECT_LE(stopped.right(), 6000.0 + 1e-9);
    EXPECT_LE(stopped.bottom(), 4000.0 + 1e-9);

    // Moving never leaves the bounds.
    const QRectF moved = crop_math::drag(start, Handle::Move, {2000, 2000}, {9000, -9000}, bounds, 1.5, min);
    EXPECT_EQ(moved, QRectF(3000, 0, 3000, 2000));

    EXPECT_EQ(crop_math::fit_aspect(QRectF(0, 0, 6000, 4000), 1.0), QRectF(1000, 0, 4000, 4000));
}

TEST(CropMath, LevellingTurnsTheDrawnLineHorizontalOrVertical) {
    // A horizon rising by 2 degrees to the right: straighten by -2 (clockwise).
    const double rise = std::tan(2.0 * 3.14159265358979323846 / 180.0) * 1000.0;
    ASSERT_TRUE(crop_math::level_angle({0, 500}, {1000, 500 - rise}).has_value());
    EXPECT_NEAR(*crop_math::level_angle({0, 500}, {1000, 500 - rise}), -2.0, 0.01);
    // Drawn right to left, the same line.
    EXPECT_NEAR(*crop_math::level_angle({1000, 500 - rise}, {0, 500}), -2.0, 0.01);
    // A nearly vertical edge leaning 3 degrees right is made upright.
    const double lean = std::tan(3.0 * 3.14159265358979323846 / 180.0) * 1000.0;
    EXPECT_NEAR(*crop_math::level_angle({500, 1000}, {500 + lean, 0}), 3.0, 0.01);
    EXPECT_FALSE(crop_math::level_angle({10, 10}, {11, 11}).has_value());
}

// ---- Crop tool ------------------------------------------------------------------------------------------

TEST_F(CropMaskModelsTest, TheCropToolShowsTheUncroppedImageAndItsHandlesMapThroughTheGeometry) {
    EditSettings settings;
    settings.geometry.quarter_turns = 1;  // displayed 4000 x 6000
    settings.geometry.crop = EditCrop{0.1, 0.2, 0.6, 0.9};
    open(settings);
    CropTool& crop = *session.crop();
    EXPECT_TRUE(crop.available());
    EXPECT_FALSE(crop.showing());

    session.setTool(QStringLiteral("crop"));
    ASSERT_EQ(service.count("set_crop_editing"), 1);
    EXPECT_TRUE(service.calls_named("set_crop_editing").back().flag);
    EXPECT_FALSE(crop.showing());  // the cropped frame is still on screen
    show_frame();
    ASSERT_TRUE(crop.showing());
    EXPECT_EQ(session.frame_output_width(), 4000);
    EXPECT_EQ(session.frame_output_height(), 6000);

    // A clockwise quarter turn maps straightened (x, y) to displayed (H - y, x): the crop's corners
    // (600, 800) and (3600, 3600) become x 400..3200, y 600..3600 of 4000 x 6000.
    const QRectF rect = crop.rect();
    EXPECT_NEAR(rect.left(), 0.1, 1e-9);
    EXPECT_NEAR(rect.top(), 0.1, 1e-9);
    EXPECT_NEAR(rect.width(), 0.7, 1e-9);
    EXPECT_NEAR(rect.height(), 0.5, 1e-9);
    EXPECT_EQ(crop.size_text(), QStringLiteral("2800 × 3000 px"));

    // The displayed left edge is the straightened bottom: dragging it to u = 0.05 (200 px) moves the
    // crop's bottom to 1 - 200 / 4000.
    crop.setConstrain(false);
    crop.beginDrag(QStringLiteral("left"), rect.left(), rect.center().y());
    for (const double u : {0.09, 0.07, 0.05}) {
        crop.dragTo(u, rect.center().y());
    }
    crop.endDrag();
    EXPECT_EQ(service.count("set_transient"), 3);
    EXPECT_EQ(service.count("commit"), 1);  // one revision for the drag
    EXPECT_EQ(service.count("apply"), 0);
    const EditCrop dragged = session.settings().geometry.crop;
    EXPECT_NEAR(dragged.bottom, 0.95, 1e-9);
    EXPECT_NEAR(dragged.left, 0.1, 1e-9);
    EXPECT_NEAR(dragged.top, 0.2, 1e-9);
    EXPECT_NEAR(dragged.right, 0.6, 1e-9);

    // Esc restores the geometry the tool was opened with (one revision) and closes it.
    session.cancelTool();
    EXPECT_EQ(service.count("apply"), 1);
    EXPECT_EQ(last_sent().geometry, settings.geometry);
    EXPECT_TRUE(session.tool().isEmpty());
    EXPECT_FALSE(service.calls_named("set_crop_editing").back().flag);
}

TEST_F(CropMaskModelsTest, ConstrainedStraighteningKeepsTheCropInsideTheImage) {
    open();
    CropTool& crop = *session.crop();
    session.setTool(QStringLiteral("crop"));
    show_frame();
    ASSERT_TRUE(crop.showing());
    ASSERT_TRUE(crop.constrain());

    // The whole image follows the angle: the largest crop of its shape inside the turned image.
    session.setValue(QStringLiteral("straighten"), 10.0);
    const EditGeometry straightened = session.settings().geometry;
    EXPECT_DOUBLE_EQ(straightened.straighten_degrees, 10.0);
    const EditCrop largest = service.largest_inside_crop(10.0, kUpright, 1.5);
    EXPECT_NEAR(straightened.crop.left, largest.left, 1e-6);
    EXPECT_NEAR(straightened.crop.bottom, largest.bottom, 1e-6);
    EXPECT_TRUE(service.crop_inside_image(straightened, kUpright));
    EXPECT_NEAR(straightened.crop.width() * 6000.0 / (straightened.crop.height() * 4000.0), 1.5, 1e-4);
    // ... and back to the whole image at 0 degrees.
    session.setValue(QStringLiteral("straighten"), 0.0);
    EXPECT_TRUE(session.settings().geometry.crop.is_full());
    session.setValue(QStringLiteral("straighten"), 10.0);
    show_frame();

    // A corner pulled far out stops where the image ends.
    const QRectF rect = crop.rect();
    crop.beginDrag(QStringLiteral("bottomRight"), rect.right(), rect.bottom());
    crop.dragTo(1.2, 1.2);
    crop.endDrag();
    EXPECT_TRUE(service.crop_inside_image(session.settings().geometry, kUpright));

    // Without the constraint the crop goes beyond the turned image (black corners).
    crop.setConstrain(false);
    const QRectF again = crop.rect();
    crop.beginDrag(QStringLiteral("bottomRight"), again.right(), again.bottom());
    crop.dragTo(1.0, 1.0);
    crop.endDrag();
    EXPECT_FALSE(service.crop_inside_image(session.settings().geometry, kUpright));
    // Turning the constraint on fits it inside again (one revision).
    const int applies = service.count("apply");
    crop.setConstrain(true);
    EXPECT_EQ(service.count("apply"), applies + 1);
    EXPECT_TRUE(service.crop_inside_image(session.settings().geometry, kUpright));
}

TEST_F(CropMaskModelsTest, AspectPresetsFitTheCropAndSwapOrientation) {
    open();
    CropTool& crop = *session.crop();
    session.setTool(QStringLiteral("crop"));
    show_frame();
    crop.setAspect(QStringLiteral("1:1"));
    EXPECT_EQ(service.count("apply"), 1);
    const EditCrop square = session.settings().geometry.crop;
    EXPECT_NEAR(square.width() * 6000.0, square.height() * 4000.0, 0.01);  // square in pixels
    EXPECT_NEAR(square.height(), 1.0, 1e-9);
    EXPECT_NEAR((square.left + square.right) / 2.0, 0.5, 1e-9);

    crop.setAspect(QStringLiteral("16:9"));
    EXPECT_FALSE(crop.portrait());
    crop.swapOrientation();  // 9:16, same area about the same centre
    EXPECT_TRUE(crop.portrait());
    const EditCrop portrait = session.settings().geometry.crop;
    EXPECT_NEAR((portrait.height() * 4000.0) / (portrait.width() * 6000.0), 16.0 / 9.0, 1e-3);

    crop.setAspect(QStringLiteral("free"));
    EXPECT_DOUBLE_EQ(crop.display_aspect(), 0.0);
    crop.setCustomAspect(7.0, 5.0);
    EXPECT_EQ(crop.aspect(), QStringLiteral("custom"));
    EXPECT_NEAR(crop.display_aspect(), 7.0 / 5.0, 1e-12);

    // Quarter turns and the mirror: one revision each, through the engine's rules.
    const int applies = service.count("apply");
    crop.rotateRight();
    crop.flip();
    EXPECT_EQ(service.count("apply"), applies + 2);
    // Mirrored as displayed after a quarter turn: three turns of the mirrored image (the engine's
    // flipped_horizontally()).
    EXPECT_EQ(session.settings().geometry.quarter_turns, 3);
    EXPECT_TRUE(session.settings().geometry.flip_horizontal);
    crop.reset();
    EXPECT_TRUE(session.settings().geometry.is_identity());
}

TEST_F(CropMaskModelsTest, LevellingAlongADrawnHorizonIsOneRevision) {
    open();
    session.setTool(QStringLiteral("crop"));
    show_frame();
    // A line rising by 80 of 4800 pixels to the right on the 6000 x 4000 display: about 0.95 degrees.
    ASSERT_TRUE(session.crop()->level(0.1, 0.5, 0.9, 0.48));
    EXPECT_EQ(service.count("apply"), 1);
    const double expected = -std::atan2(80.0, 4800.0) * 180.0 / 3.14159265358979323846;
    EXPECT_NEAR(last_sent().geometry.straighten_degrees, std::round(expected * 100.0) / 100.0, 1e-9);
    EXPECT_FALSE(session.crop()->level(0.5, 0.5, 0.5001, 0.5));  // too short
}

// ---- Masks ------------------------------------------------------------------------------------------------

TEST_F(CropMaskModelsTest, TheMaskListNamesMasksAndEveryChangeIsOneRevision) {
    open();
    MaskEditor& masks = *session.masks();
    MaskListModel& list = *masks.list();
    ASSERT_TRUE(masks.can_add());

    ASSERT_TRUE(masks.add(QStringLiteral("radial")));
    EXPECT_EQ(session.tool(), QStringLiteral("masks"));
    ASSERT_TRUE(masks.add(QStringLiteral("linear")));
    ASSERT_TRUE(masks.add(QStringLiteral("brush")));
    EXPECT_EQ(service.count("apply"), 3);
    ASSERT_EQ(list.count(), 3);
    EXPECT_EQ(list.data(list.index(0), MaskListModel::NameRole).toString(), QStringLiteral("Radial 1"));
    EXPECT_EQ(list.data(list.index(1), MaskListModel::NameRole).toString(), QStringLiteral("Linear 1"));
    EXPECT_EQ(list.data(list.index(2), MaskListModel::NameRole).toString(), QStringLiteral("Brush 1"));
    EXPECT_TRUE(list.data(list.index(2), MaskListModel::SelectedRole).toBool());
    // The radial mask sits in the middle of the displayed image, a linear one runs from its top to
    // its middle.
    const EditMask radial = session.settings().masks[0];
    EXPECT_NEAR(radial.center.x(), 0.5, 1e-6);
    EXPECT_NEAR(radial.center.y(), 0.5, 1e-6);
    EXPECT_NEAR(radial.radius_x, 0.3 * 4000.0 / 6000.0, 1e-6);
    const EditMask linear = session.settings().masks[1];
    EXPECT_NEAR(linear.start.y(), 0.0, 1e-6);
    EXPECT_NEAR(linear.end.y(), 0.5, 1e-6);
    EXPECT_DOUBLE_EQ(linear.feather, 1.0);

    masks.duplicate(static_cast<int>(radial.id));
    ASSERT_EQ(list.count(), 4);
    EXPECT_EQ(list.data(list.index(1), MaskListModel::NameRole).toString(), QStringLiteral("Radial 2"));
    EXPECT_EQ(masks.selected_id(), static_cast<int>(session.settings().masks[1].id));
    masks.move(static_cast<int>(radial.id), 2);
    EXPECT_EQ(session.settings().masks[2].id, radial.id);
    masks.setEnabled(static_cast<int>(radial.id), false);
    masks.setInvert(static_cast<int>(radial.id), true);
    EXPECT_FALSE(session.settings().masks[2].enabled);
    EXPECT_TRUE(session.settings().masks[2].invert);
    EXPECT_EQ(service.count("apply"), 7);
    EXPECT_EQ(service.count("set_transient"), 0);

    // The selected mask's local controls are control keys like the photo's.
    masks.select(static_cast<int>(radial.id));
    session.setValue(QStringLiteral("maskExposure"), 0.7);
    session.setValue(QStringLiteral("maskTemperature"), 40.0);
    EXPECT_DOUBLE_EQ(session.settings().masks[2].controls.exposure_ev, 0.7);
    EXPECT_DOUBLE_EQ(session.settings().masks[2].controls.temperature, 0.4);
    EXPECT_DOUBLE_EQ(session.value(QStringLiteral("maskExposure")), 0.7);
    session.resetValue(QStringLiteral("maskOpacity"));
    EXPECT_DOUBLE_EQ(session.value(QStringLiteral("maskOpacity")), 100.0);

    // Deleting the selected mask selects its neighbour.
    masks.remove(static_cast<int>(radial.id));
    ASSERT_EQ(list.count(), 3);
    EXPECT_EQ(masks.selected_id(), static_cast<int>(session.settings().masks[2].id));
    // The overlay (O) follows the selection.
    session.set_mask_overlay(true);
    ASSERT_FALSE(service.calls_named("set_mask_overlay").empty());
    EXPECT_EQ(service.calls_named("set_mask_overlay").back().revision, static_cast<quint64>(masks.selected_id()));
}

TEST_F(CropMaskModelsTest, ABrushStrokeIsOneRevisionWithSimplifiedPoints) {
    open();
    MaskEditor& masks = *session.masks();
    session.setTool(QStringLiteral("masks"));
    ASSERT_TRUE(masks.brush_active());  // nothing selected yet: painting starts a brush mask
    session.setValue(QStringLiteral("brushSize"), 2.0);  // 120 px of 6000
    EXPECT_TRUE(service.calls.empty() || service.calls.back().name != u"apply");  // a tool setting, not an edit
    EXPECT_NEAR(masks.brush_radius(), 0.02, 1e-12);
    EXPECT_EQ(masks.brush_pixels(), 120);

    // A pointer move every 4 output pixels across a third of the image.
    masks.beginStroke(0.2, 0.5, false);
    for (int i = 1; i <= 500; ++i) {
        masks.extendStroke(0.2 + i * 4.0 / 6000.0, 0.5);
    }
    masks.endStroke();
    EXPECT_EQ(service.count("commit"), 1);
    EXPECT_EQ(service.count("apply"), 0);
    EXPECT_GE(service.count("set_transient"), 2);
    ASSERT_EQ(session.settings().masks.size(), 1);
    const EditMask& brush = session.settings().masks.front();
    EXPECT_EQ(brush.kind, MaskKind::Brush);
    ASSERT_EQ(brush.strokes.size(), 1);
    const EditBrushStroke& stroke = brush.strokes.front();
    // Points at least 0.15 radius (18 px) apart: every fifth of the 501 positions (20 px).
    EXPECT_GE(stroke.points.size(), 98);
    EXPECT_LE(stroke.points.size(), 104);
    for (qsizetype i = 1; i < stroke.points.size(); ++i) {
        EXPECT_GE((stroke.points[i].x() - stroke.points[i - 1].x()) * 6000.0, 18.0 - 1e-3) << i;
    }
    EXPECT_NEAR(stroke.points.back().x(), 0.2 + 2000.0 / 6000.0, 1e-6);  // ends where released
    EXPECT_DOUBLE_EQ(stroke.radius, 0.02);
    EXPECT_DOUBLE_EQ(stroke.hardness, 0.5);
    EXPECT_EQ(stroke.mode, BrushMode::Add);
    EXPECT_EQ(masks.selected_kind(), QStringLiteral("brush"));

    // Alt: an erase stroke into the same mask, one more revision.
    masks.beginStroke(0.3, 0.5, true);
    masks.extendStroke(0.31, 0.5);
    masks.endStroke();
    EXPECT_EQ(service.count("commit"), 2);
    ASSERT_EQ(session.settings().masks.front().strokes.size(), 2);
    EXPECT_EQ(session.settings().masks.front().strokes.back().mode, BrushMode::Erase);
}

TEST_F(CropMaskModelsTest, AHandleDragIsOneRevisionAndTheOutlineFollowsIt) {
    open();
    MaskEditor& masks = *session.masks();
    ASSERT_TRUE(masks.add(QStringLiteral("radial")));
    answer();
    show_frame();
    const QVariantList handles = masks.handles();
    ASSERT_EQ(handles.size(), 3);
    const QVariantMap centre = handles[0].toMap();
    EXPECT_EQ(centre.value(QStringLiteral("key")).toString(), QStringLiteral("center"));
    EXPECT_NEAR(centre.value(QStringLiteral("x")).toDouble(), 0.5, 1e-6);

    masks.beginHandle(QStringLiteral("center"), 0.5, 0.5);
    masks.moveHandle(0.55, 0.5);
    masks.moveHandle(0.6, 0.45);
    // The outline is the engine's of the local settings while the handle moves.
    const OverlayPaths moving = masks.outlines().value<OverlayPaths>();
    ASSERT_FALSE(moving.isEmpty());
    masks.endHandle();
    EXPECT_EQ(service.count("set_transient"), 2);
    EXPECT_EQ(service.count("commit"), 1);
    EXPECT_NEAR(session.settings().masks.front().center.x(), 0.6, 1e-6);
    EXPECT_NEAR(session.settings().masks.front().center.y(), 0.45, 1e-6);

    // The x semi-axis handle sets the size and the angle.
    const QVariantMap x_axis = masks.handles()[1].toMap();
    masks.beginHandle(QStringLiteral("radiusX"), x_axis.value(QStringLiteral("x")).toDouble(),
                      x_axis.value(QStringLiteral("y")).toDouble());
    masks.moveHandle(0.6, 0.45 - 600.0 / 4000.0);  // 600 px straight above the centre
    masks.endHandle();
    EXPECT_EQ(service.count("commit"), 2);
    EXPECT_NEAR(session.settings().masks.front().angle_degrees, 90.0, 1e-6);
    EXPECT_NEAR(session.settings().masks.front().radius_x, 600.0 / 6000.0, 1e-6);
}

// ---- Detail and copy categories ---------------------------------------------------------------------------

TEST_F(CropMaskModelsTest, NoiseReductionKeysAndTheCopyCategories) {
    open();
    session.setValue(QStringLiteral("noiseReduction"), 60.0);
    EXPECT_DOUBLE_EQ(last_sent().chroma_denoise.strength, 0.6);
    session.setValue(QStringLiteral("noiseRadius"), 12.0);
    EXPECT_DOUBLE_EQ(last_sent().chroma_denoise.radius, 12.0);
    session.resetValue(QStringLiteral("noiseRadius"));
    EXPECT_DOUBLE_EQ(last_sent().chroma_denoise.radius, kDefaultDenoiseRadius);
    // The radius slider is logarithmic.
    session.setPosition(QStringLiteral("noiseRadius"), 0.5);
    EXPECT_DOUBLE_EQ(session.value(QStringLiteral("noiseRadius")), 8.0);

    const QVariantList categories = session.categories();
    ASSERT_EQ(categories.size(), 7);
    QStringList keys;
    for (const QVariant& category : categories) {
        const QVariantMap entry = category.toMap();
        keys << entry.value(QStringLiteral("key")).toString();
        EXPECT_EQ(entry.value(QStringLiteral("checked")).toBool(), entry.value(QStringLiteral("key")).toString() != u"geometry");
    }
    EXPECT_TRUE(keys.contains(QStringLiteral("detail")));
    EXPECT_TRUE(keys.contains(QStringLiteral("geometry")));
    session.copySettings({QStringLiteral("detail"), QStringLiteral("geometry")});
    ASSERT_EQ(service.count("copy_settings"), 1);
    EXPECT_EQ(service.calls_named("copy_settings").back().categories,
              category_bit(SettingsCategory::Detail) | category_bit(SettingsCategory::Geometry));
    EXPECT_EQ(category_label(SettingsCategory::Local), QStringLiteral("Highlights, shadows and masks"));
}

}  // namespace
}  // namespace lucent::ui
