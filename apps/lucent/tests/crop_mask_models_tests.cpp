// W9 (ADR 0015) view models against a recording EditService whose geometry and mask helpers are the
// engine's own (the adapters'): the crop math and the crop tool's handles mapped through
// GeometryMapping (quarter turns, aspect ratios, "Constrain to image", levelling, cancel), the mask
// list (names, add, duplicate, reorder, enable, invert, delete: one revision each), a brush stroke
// and a handle drag as ONE revision each with simplified points, the new control keys, and the
// copy categories (Detail and Geometry, Geometry not selected by default).

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

#include <QImage>
#include <QPainter>
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
        frame->full_frame = frame_area;  // the camera's image area of a geometry-2 recipe (QUA-05)
        const GeometryMap map = service.map_geometry(frame->geometry, kUpright, frame_area);
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
    QRect frame_area;  // the frames' geometry frame F (empty: the whole upright image)
};

// QUA-05: a geometry-version-2 recipe of a file with a camera image area works in that area (the
// frame F, ADR 0017): the tools map pointer positions, handles and the straightened crop through it,
// not through the whole upright image (whose border lies outside the output).
TEST_F(CropMaskModelsTest, TheToolsWorkInTheCameraAreaOfTheFrame) {
    frame_area = QRect(44, 30, 5900, 3936);  // a border of 44 x 30 pixels, as the A7 IV APS-C crop
    open();
    ASSERT_EQ(session.frame_full_frame(), frame_area);
    ASSERT_EQ(session.frame_output_width(), 5900);
    ASSERT_EQ(session.frame_output_height(), 3936);

    // Masks: the displayed image's corners are F's corners in the masks' frame U.
    session.setTool(QStringLiteral("masks"));
    show_frame();
    const auto top_left = session.masks()->normalized_at(0.0, 0.0);
    const auto bottom_right = session.masks()->normalized_at(1.0, 1.0);
    ASSERT_TRUE(top_left && bottom_right);
    EXPECT_NEAR(top_left->x(), 44.0 / 6000.0, 1e-9);
    EXPECT_NEAR(top_left->y(), 30.0 / 4000.0, 1e-9);
    EXPECT_NEAR(bottom_right->x(), (44.0 + 5900.0) / 6000.0, 1e-9);
    EXPECT_NEAR(bottom_right->y(), (30.0 + 3936.0) / 4000.0, 1e-9);
    const auto shown = session.masks()->displayed_at(QPointF(0.5, 0.5));
    ASSERT_TRUE(shown);
    EXPECT_NEAR(shown->x(), (3000.0 - 44.0) / 5900.0, 1e-9);
    EXPECT_NEAR(shown->y(), (2000.0 - 30.0) / 3936.0, 1e-9);

    // Crop: the identity crop is the whole displayed area, and a straightened crop that follows the
    // angle keeps F's own aspect ratio and stays inside F.
    session.setTool(QStringLiteral("crop"));
    show_frame();
    EXPECT_EQ(session.crop()->rect(), QRectF(0.0, 0.0, 1.0, 1.0));
    EXPECT_EQ(session.crop()->size_text(), QStringLiteral("5900 × 3936 px"));
    const EditGeometry level = session.crop()->straightened(session.settings().geometry, 5.0);
    const EditCrop c = level.crop;
    const EditCrop in_frame = service.largest_inside_crop(5.0, QSize(5900, 3936), 5900.0 / 3936.0);
    EXPECT_NEAR(c.left, in_frame.left, 1e-6);
    EXPECT_NEAR(c.top, in_frame.top, 1e-6);
    EXPECT_NEAR(c.right, in_frame.right, 1e-6);
    EXPECT_NEAR(c.bottom, in_frame.bottom, 1e-6);
    EXPECT_NEAR((c.width() * 5900.0) / (c.height() * 3936.0), 5900.0 / 3936.0, 1e-5);
    // The whole upright image's answer is another crop.
    const EditCrop in_upright = service.largest_inside_crop(5.0, kUpright, 1.5);
    EXPECT_GT(std::abs(in_upright.top - in_frame.top) + std::abs(in_upright.left - in_frame.left), 1e-6);
}

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

// ---- Outline painting -------------------------------------------------------------------------------------

// What OverlayPathItem paints for `paths` on a 400 x 300 item whose image covers `image_rect`.
QImage painted(const OverlayPaths& paths, const QRectF& image_rect) {
    OverlayPathItem item;
    item.setSize(QSizeF(400, 300));
    item.set_paths(QVariant::fromValue(paths));
    item.set_image_rect(image_rect);
    QImage image(400, 300, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    item.paint(&painter);
    painter.end();
    return image;
}

int painted_pixels(const QImage& image) {
    int n = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            n += qAlpha(image.pixel(x, y)) > 0 ? 1 : 0;
        }
    }
    return n;
}

// A paint costs what is seen: a stroke of 65536 points closer than a pixel apart paints the line its
// end points give (points under 0.75 pixels from the last one drawn are skipped), and segments
// outside the visible image are left out.
TEST(OverlayPainting, DensePathsPaintWhatIsSeen) {
    const QRectF image(20, 20, 360, 260);
    QPolygonF dense;
    for (int i = 0; i < 65536; ++i) {
        dense << QPointF(0.1 + 0.8 * i / 65535.0, 0.5);
    }
    const QImage many = painted({OverlayPath{dense, true, false}}, image);
    const QImage two = painted({OverlayPath{QPolygonF{QPointF(0.1, 0.5), QPointF(0.9, 0.5)}, true, false}}, image);
    EXPECT_GT(painted_pixels(two), 252 * 3);
    int different = 0;
    for (int y = 0; y < 300; ++y) {
        for (int x = 0; x < 400; ++x) {
            different += std::abs(qAlpha(many.pixel(x, y)) - qAlpha(two.pixel(x, y))) > 64 ? 1 : 0;
        }
    }
    EXPECT_LT(different, 8);
    // A path whose every segment lies outside the image (here beyond the item) paints nothing; a path
    // crossing the visible part paints only there.
    const QRectF large(-4000, -3000, 8000, 6000);  // the image at a high zoom, the view on its centre
    EXPECT_EQ(painted_pixels(painted({OverlayPath{QPolygonF{QPointF(0.01, 0.01), QPointF(0.02, 0.02)}, true, false}},
                                     large)),
              0);
    const QImage crossing =
        painted({OverlayPath{QPolygonF{QPointF(0.0, 0.525), QPointF(0.25, 0.525), QPointF(0.75, 0.525), QPointF(1.0, 0.525)},
                             true, false}},
                large);
    EXPECT_GT(painted_pixels(crossing), 400 * 3);
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

// The aspect ratio's orientation is the crop's own as displayed: after a quarter turn that Esc
// takes back, a corner drag keeps a landscape 3:2 crop landscape (a separate orientation flag stayed
// "portrait" and turned the whole crop into a 2:3 strip).
TEST_F(CropMaskModelsTest, TheAspectOrientationFollowsTheCropThroughATurnAndEsc) {
    open();
    CropTool& crop = *session.crop();
    session.setTool(QStringLiteral("crop"));
    show_frame();
    crop.setAspect(QStringLiteral("3:2"));
    EXPECT_FALSE(crop.portrait());
    EXPECT_NEAR(crop.display_aspect(), 1.5, 1e-12);
    crop.rotateRight();
    show_frame();
    EXPECT_TRUE(crop.portrait());  // the crop stands upright now
    EXPECT_NEAR(crop.display_aspect(), 2.0 / 3.0, 1e-12);
    session.cancelTool();  // Esc: the geometry the tool was opened with
    EXPECT_EQ(session.settings().geometry.quarter_turns, 0);
    EXPECT_FALSE(crop.portrait());
    session.setTool(QStringLiteral("crop"));
    show_frame();
    const QRectF before = crop.rect();
    crop.beginDrag(QStringLiteral("bottomRight"), before.right(), before.bottom());
    crop.dragTo(before.right() - 0.05, before.bottom() - 0.05);
    crop.endDrag();
    const EditCrop after = session.settings().geometry.crop;
    EXPECT_NEAR((after.width() * 6000.0) / (after.height() * 4000.0), 1.5, 1e-3);
    EXPECT_GT(after.width(), 0.8);  // shrunk by the drag, not cut to a portrait strip
    EXPECT_EQ(crop.aspect(), QStringLiteral("3:2"));  // the ratio chosen stays
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

// While a stroke is painted the mask's earlier strokes stay one unchanged outline (drawn once) and
// only the stroke being painted changes (strokeOutline), also across the split at 4096 points.
TEST_F(CropMaskModelsTest, TheStrokeBeingPaintedIsTheOnlyOutlineThatChanges) {
    open();
    MaskEditor& masks = *session.masks();
    session.setTool(QStringLiteral("masks"));
    session.setValue(QStringLiteral("brushSize"), 0.1);  // 6 px of 6000: a point every 0.9 px
    masks.beginStroke(0.1, 0.2, false);
    for (int i = 1; i <= 50; ++i) {
        masks.extendStroke(0.1 + i * 2.0 / 6000.0, 0.2);
    }
    masks.endStroke();
    ASSERT_EQ(masks.outlines().value<OverlayPaths>().size(), 1);
    EXPECT_TRUE(masks.stroke_outline().value<OverlayPaths>().isEmpty());

    masks.beginStroke(0.1, 0.6, false);
    const OverlayPaths first = masks.outlines().value<OverlayPaths>();
    ASSERT_EQ(first.size(), 1);  // the earlier stroke only
    for (int i = 1; i <= 4200; ++i) {
        masks.extendStroke(0.1 + i * 1.2 / 6000.0, 0.6);
        if (i == 100) {
            const OverlayPaths during = masks.outlines().value<OverlayPaths>();
            EXPECT_EQ(during.constData(), first.constData()) << "the same paths: nothing to redraw";
            const OverlayPaths active = masks.stroke_outline().value<OverlayPaths>();
            ASSERT_EQ(active.size(), 1);
            EXPECT_EQ(active.front().points.size(), 101);
            EXPECT_NEAR(active.front().points.back().x(), 0.1 + 120.0 / 6000.0, 1e-6);
            EXPECT_NEAR(active.front().points.back().y(), 0.6, 1e-6);
        }
    }
    // Past 4096 points the gesture went on as another stroke: the finished part joined the
    // unchanged layer once.
    const EditMask& painted = session.settings().masks.front();
    ASSERT_EQ(painted.strokes.size(), 3);
    EXPECT_EQ(painted.strokes[1].points.size(), kMaxStrokePoints);
    EXPECT_EQ(masks.outlines().value<OverlayPaths>().size(), 2);
    ASSERT_EQ(masks.stroke_outline().value<OverlayPaths>().size(), 1);
    EXPECT_EQ(masks.stroke_outline().value<OverlayPaths>().front().points.size(), painted.strokes[2].points.size());
    masks.endStroke();
    EXPECT_EQ(service.count("commit"), 2);
    EXPECT_EQ(masks.outlines().value<OverlayPaths>().size(), 3);
    EXPECT_TRUE(masks.stroke_outline().value<OverlayPaths>().isEmpty());
}

// The photo-wide bound of brush points: painting stops with a message, the refusal names the limit
// that was reached, and continuing a full stroke (two points) never passes the bound.
TEST_F(CropMaskModelsTest, BrushPointLimitsStopPaintingWithAMessage) {
    EditSettings settings;
    EditMask brush;
    brush.id = 1;
    brush.kind = MaskKind::Brush;
    EditBrushStroke full;
    full.radius = 0.001;
    for (int i = 0; i < kMaxStrokePoints; ++i) {
        full.points << QPointF(0.1 + 0.8 * i / kMaxStrokePoints, 0.1);
    }
    for (int i = 0; i < kMaxBrushPoints / kMaxStrokePoints - 1; ++i) {
        brush.strokes << full;
    }
    brush.strokes.back().points.removeLast();  // 61439 points
    settings.masks << brush;
    open(settings);
    MaskEditor& masks = *session.masks();
    session.setTool(QStringLiteral("masks"));
    masks.select(1);
    session.setValue(QStringLiteral("brushSize"), 0.1);
    const auto points = [this] {
        qsizetype n = 0;
        for (const EditMask& mask : session.settings().masks) {
            for (const EditBrushStroke& stroke : mask.strokes) {
                n += stroke.points.size();
            }
        }
        return n;
    };
    // A stroke of 4096 points brings the photo to 65535; its continuation would need two more.
    masks.beginStroke(0.1, 0.9, false);
    for (int i = 1; i < 4300; ++i) {
        masks.extendStroke(0.1 + i * 1.2 / 6000.0, 0.9);
        ASSERT_LE(points(), kMaxBrushPoints) << i;
    }
    EXPECT_EQ(points(), kMaxBrushPoints - 1);
    EXPECT_TRUE(session.message().contains(QStringLiteral("Painting stopped"))) << session.message().toStdString();
    EXPECT_TRUE(session.message().contains(QStringLiteral("brush points")));
    masks.endStroke();
    // One more point fits: the next stroke takes it, then the refusal names the limit reached.
    session.clearMessage();
    masks.beginStroke(0.5, 0.5, false);
    masks.endStroke();
    EXPECT_EQ(points(), kMaxBrushPoints);
    masks.beginStroke(0.6, 0.6, false);
    EXPECT_FALSE(masks.painting());
    EXPECT_TRUE(session.message().contains(QStringLiteral("delete a brush mask"))) << session.message().toStdString();
}

// A gradient added while the crop tool is open is placed on the photo as it is about to be shown
// (cropped), not on the uncropped frame the crop tool shows.
TEST_F(CropMaskModelsTest, AMaskAddedFromTheCropToolIsPlacedOnTheCroppedPhoto) {
    EditSettings settings;
    settings.geometry.crop = EditCrop{0.5, 0.0, 1.0, 0.5};  // the top right quarter
    open(settings);
    session.setTool(QStringLiteral("crop"));
    show_frame();
    ASSERT_TRUE(session.frame_uncropped());
    MaskEditor& masks = *session.masks();
    ASSERT_TRUE(masks.add(QStringLiteral("radial")));
    ASSERT_TRUE(masks.add(QStringLiteral("linear")));
    const EditMask radial = session.settings().masks[0];
    EXPECT_NEAR(radial.center.x(), 0.75, 1e-6);  // the middle of the crop
    EXPECT_NEAR(radial.center.y(), 0.25, 1e-6);
    EXPECT_NEAR(radial.radius_x, 0.3 * 2000.0 / 6000.0, 1e-6);  // 0.3 of the crop's short side
    const EditMask linear = session.settings().masks[1];
    EXPECT_NEAR(linear.start.y(), 0.0, 1e-6);  // from the crop's top to its middle
    EXPECT_NEAR(linear.end.y(), 0.25, 1e-6);
    EXPECT_NEAR(linear.start.x(), 0.75, 1e-6);
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
    ASSERT_EQ(categories.size(), 9);
    QStringList keys;
    for (const QVariant& category : categories) {
        const QVariantMap entry = category.toMap();
        const QString key = entry.value(QStringLiteral("key")).toString();
        keys << key;
        // Masks and the crop are placed on this photo's content, the lens corrections belong to its camera
        // and lens: not selected by default.
        EXPECT_EQ(entry.value(QStringLiteral("checked")).toBool(),
                  key != u"geometry" && key != u"masks" && key != u"lensCorrections")
            << key.toStdString();
    }
    EXPECT_TRUE(keys.contains(QStringLiteral("masks")));
    EXPECT_TRUE(keys.contains(QStringLiteral("detail")));
    EXPECT_TRUE(keys.contains(QStringLiteral("geometry")));
    session.copySettings({QStringLiteral("detail"), QStringLiteral("geometry")});
    ASSERT_EQ(service.count("copy_settings"), 1);
    EXPECT_EQ(service.calls_named("copy_settings").back().categories,
              category_bit(SettingsCategory::Detail) | category_bit(SettingsCategory::Geometry));
    EXPECT_EQ(category_label(SettingsCategory::Local), QStringLiteral("Highlights and shadows"));
    EXPECT_EQ(category_label(SettingsCategory::Masks), QStringLiteral("Masks"));
}

}  // namespace
}  // namespace lucent::ui
