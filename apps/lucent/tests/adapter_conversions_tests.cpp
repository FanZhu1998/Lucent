// Conversions between the application services' types and the shell's (adapters/conversions.hpp).

#include <QImage>
#include <algorithm>
#include <cmath>

#include <gtest/gtest.h>

#include <lucent/engine/develop_geometry.hpp>

#include "adapter_test_support.hpp"
#include "adapters/conversions.hpp"
#include "adapters/edit_conversions.hpp"

namespace lucent::ui::adapters {
namespace {

namespace rts = lucent::raw::test_support;

TEST(AdapterConversions, SupportStatusIsNeverVerified) {
    EXPECT_EQ(to_ui_support(app::SupportStatus::Supported), SupportStatus::CompatibleUnverified);
    EXPECT_EQ(to_ui_support(app::SupportStatus::Limited), SupportStatus::Limited);
    EXPECT_EQ(to_ui_support(app::SupportStatus::Unsupported), SupportStatus::Unsupported);
}

TEST(AdapterConversions, SupportDetailExplainsEveryDistinctReason) {
    app::SupportAssessment supported;
    supported.status = app::SupportStatus::Supported;
    EXPECT_TRUE(support_detail(supported).contains(QStringLiteral("not verified")));

    app::SupportAssessment limited;
    limited.status = app::SupportStatus::Limited;
    limited.interpretation = raw::InterpretationStatus::OptionalOperationsSkipped;
    app::SupportReason optional;
    optional.issue = raw::InterpretationIssueKind::OptionalOperationUnsupported;
    limited.reasons = {optional, optional};  // duplicates are shown once
    EXPECT_EQ(support_detail(limited),
              describe_issue(raw::InterpretationIssueKind::OptionalOperationUnsupported));

    app::SupportAssessment probe_failed =
        app::assess_probe_failure(Error{ErrorCode::Corrupt, "bad", "raw.probe", 0});
    EXPECT_EQ(support_detail(probe_failed), QStringLiteral("The file is damaged or incomplete."));

    app::SupportAssessment status_only;
    status_only.status = app::SupportStatus::Unsupported;
    status_only.interpretation = raw::InterpretationStatus::Ambiguous;
    app::SupportReason reason;
    reason.kind = app::SupportReasonKind::InterpretationStatus;
    status_only.reasons = {reason};
    EXPECT_EQ(support_detail(status_only),
              QStringLiteral("Lucent cannot determine how this file must be processed."));
}

TEST(AdapterConversions, EveryIssueKindHasItsOwnSentence) {
    QStringList sentences;
    for (int kind = 1; kind <= 13; ++kind) {
        const QString sentence = describe_issue(static_cast<raw::InterpretationIssueKind>(kind));
        EXPECT_FALSE(sentence.isEmpty());
        EXPECT_FALSE(sentences.contains(sentence)) << sentence.toStdString();
        sentences << sentence;
    }
}

TEST(AdapterConversions, ErrorsAreDescribedByCodeAndLocation) {
    EXPECT_EQ(describe_error(Error{ErrorCode::NotFound, "", "core.file.open", 0}),
              QStringLiteral("The file was not found. It may have been moved, renamed or deleted."));
    EXPECT_EQ(describe_error(Error{ErrorCode::NotFound, "", "app.export.destination", 0}),
              QStringLiteral("The output folder does not exist."));
    EXPECT_EQ(describe_error(Error{ErrorCode::NotFound, "", "app.preview", 0}),
              QStringLiteral("This photo is no longer in the library."));
    EXPECT_EQ(describe_error(Error{ErrorCode::Unsupported, "", "app.export.support", 0}),
              QStringLiteral("This photo is Unsupported, so Lucent does not export it."));
    EXPECT_EQ(describe_error(Error{ErrorCode::Unsupported, "", "raw.libraw.open", 0}),
              QStringLiteral("Lucent cannot decode this file."));
    EXPECT_EQ(describe_error(Error{ErrorCode::DiskFull, "", "app.export.space", 0}),
              QStringLiteral("There is not enough free space on the destination drive."));
    EXPECT_EQ(describe_error(Error{ErrorCode::PermissionDenied, "", "export.publish", 0}),
              QStringLiteral("Windows denied access to the output folder."));
    // Adding a changed file again does update it (app::Library::refresh), so the advice is real.
    EXPECT_EQ(describe_error(Error{ErrorCode::SourceChanged, "", "app.render.source", 0}),
              QStringLiteral("The file changed after it was added. Add it again to update it."));
    // A library saved by a newer Lucent is read-only: that is the library's limit, not the photo's.
    EXPECT_EQ(describe_error(Error{ErrorCode::VersionMismatch, "", "app.library.journal", 0}),
              QStringLiteral("This library was saved by a newer version of Lucent and is open read-only, so "
                             "photos cannot be added or changed."));
    EXPECT_EQ(describe_error(Error{ErrorCode::VersionMismatch, "", "catalog.write", 0}),
              describe_error(Error{ErrorCode::VersionMismatch, "", "app.library.add", 0}));
    EXPECT_EQ(describe_error(Error{ErrorCode::VersionMismatch, "", "raw.dng.version", 0}),
              QStringLiteral("The file needs a newer version of Lucent."));
    // A photo's edit that cannot be read is the edit's problem, never the file's or an internal one.
    EXPECT_EQ(describe_error(Error{ErrorCode::VersionMismatch, "", "app.recipe.unreadable", 0}),
              QStringLiteral("This photo's edit was made by a newer version of Lucent."));
    EXPECT_EQ(describe_error(Error{ErrorCode::VersionMismatch, "", "app.recipe", 0}),
              QStringLiteral("This photo's edit was made by a newer version of Lucent."));
    for (const ErrorCode code : {ErrorCode::InvalidArgument, ErrorCode::Corrupt, ErrorCode::Unsupported}) {
        EXPECT_EQ(describe_error(Error{code, "", "app.recipe.unreadable", 0}),
                  QStringLiteral("This photo's edit is damaged and cannot be read."));
    }
    // Developer-facing messages are never shown.
    EXPECT_EQ(describe_error(Error{ErrorCode::Internal, "secret detail", "engine", 0}),
              QStringLiteral("Lucent ran into an internal problem (internal)."));
    for (int code = 1; code <= 16; ++code) {
        EXPECT_FALSE(describe_error(Error{static_cast<ErrorCode>(code), "", "x", 0}).isEmpty());
    }
}

TEST(AdapterConversions, LibraryItemsCarryTheProbeFactsAndStartWithTheCameraPreview) {
    app::SourceItem source;
    source.id = app::ItemId{7};
    source.path = L"C:\\Photos\\IMG_0001.CR3";
    source.subimage = 1;
    source.subimage_count = 2;
    source.identity.size_bytes = 12345;
    app::ProbeSummary probe;
    probe.camera_make = "Canon";
    probe.camera_model = "EOS R5";
    probe.lens = "RF24-105mm F4 L IS USM";
    probe.capture_time_local = "2026-09-24T21:09:05";
    probe.iso = 400.0f;
    probe.exposure_time_s = 0.004f;
    probe.f_number = 5.6f;
    probe.focal_length_mm = 35.0f;
    probe.upright_size = image::Size{5464, 8192};
    source.probe = probe;
    source.support.status = app::SupportStatus::Supported;

    const LibraryItem item = to_library_item(source);
    EXPECT_EQ(item.id, 7u);
    EXPECT_EQ(item.file_path, QStringLiteral(R"(C:\Photos\IMG_0001.CR3)"));
    EXPECT_EQ(item.file_size_bytes, 12345);
    EXPECT_EQ(item.subimage, 1);
    EXPECT_EQ(item.subimage_count, 2);
    EXPECT_EQ(item.camera_model, QStringLiteral("EOS R5"));
    EXPECT_EQ(item.capture_time, QDateTime(QDate(2026, 9, 24), QTime(21, 9, 5)));
    EXPECT_EQ(item.pixel_width, 5464);
    EXPECT_EQ(item.pixel_height, 8192);
    EXPECT_DOUBLE_EQ(item.iso, 400.0);
    EXPECT_NEAR(item.exposure_time_s, 0.004, 1e-9);
    EXPECT_EQ(item.support, SupportStatus::CompatibleUnverified);
    EXPECT_EQ(item.thumbnail_generation, kCameraPreviewGeneration);
    EXPECT_TRUE(item.thumbnail_is_camera_preview);
    EXPECT_EQ(item.preview_generation, 1u);

    // Merging fresh facts keeps the shell's state and, once rendered, the render's size.
    LibraryItem shown = item;
    shown.thumbnail_generation = 3;
    shown.processing = ProcessingStatus::Processed;
    auto render = std::make_shared<RenderInfo>();
    shown.render = render;
    shown.pixel_width = 100;
    LibraryItem fresh = item;
    fresh.support = SupportStatus::Limited;
    merge_library_facts(shown, fresh);
    EXPECT_EQ(shown.support, SupportStatus::Limited);
    EXPECT_EQ(shown.thumbnail_generation, 3u);
    EXPECT_EQ(shown.processing, ProcessingStatus::Processed);
    EXPECT_EQ(shown.pixel_width, 100);

    // Without a probe there is nothing to show as a thumbnail.
    source.probe.reset();
    EXPECT_EQ(to_library_item(source).thumbnail_generation, kNoImage);
    EXPECT_EQ(to_library_item(source).file_size_bytes, 12345);
}

TEST(AdapterConversions, PreviewQualityFitsTheBoxOrAsksForFullResolution) {
    using Q = app::PreviewQuality;
    // A 3:2 landscape photo in a wider box: limited by the height.
    EXPECT_EQ(preview_quality_for({1280, 768}, 8192, 5464), Q::fit(1152));
    EXPECT_EQ(preview_quality_for({512, 512}, 8192, 5464), Q::fit(512));
    EXPECT_EQ(preview_quality_for({512, 0}, 8192, 5464), Q::fit(512));
    // The box of the 100 % view, or larger, or unconstrained: every pixel.
    EXPECT_EQ(preview_quality_for({8192, 5464}, 8192, 5464), Q::full());
    EXPECT_EQ(preview_quality_for({9000, 9000}, 8192, 5464), Q::full());
    EXPECT_EQ(preview_quality_for({-1, -1}, 8192, 5464), Q::full());
    EXPECT_EQ(preview_quality_for({0, 0}, 0, 0), Q::full());
    // Unknown size: the box's long side; clamped to the service's range.
    EXPECT_EQ(preview_quality_for({640, 480}, 0, 0), Q::fit(640));
    EXPECT_EQ(preview_quality_for({4, 4}, 8192, 5464), Q::fit(Q::kMinLongEdge));
    EXPECT_EQ(preview_quality_for({20000, 20000}, 0, 0), Q::fit(Q::kMaxLongEdge));
}

TEST(AdapterConversions, DisplayImagesBecomeOpaqueRgb32) {
    app::DisplayImage display;
    display.width = 2;
    display.height = 2;
    display.rgb = {255, 0, 0, 0, 255, 0, 0, 0, 255, 10, 20, 30};
    const QImage image = to_qimage(display);
    ASSERT_EQ(image.size(), QSize(2, 2));
    EXPECT_EQ(image.format(), QImage::Format_RGB32);
    EXPECT_EQ(image.pixel(0, 0), qRgb(255, 0, 0));
    EXPECT_EQ(image.pixel(1, 0), qRgb(0, 255, 0));
    EXPECT_EQ(image.pixel(0, 1), qRgb(0, 0, 255));
    EXPECT_EQ(image.pixel(1, 1), qRgb(10, 20, 30));

    display.rgb.pop_back();  // inconsistent size
    EXPECT_TRUE(to_qimage(display).isNull());
}

TEST(AdapterConversions, OrientationsFollowExifExactly) {
    // A 3 x 2 image whose pixels are numbered: (x, y) -> 10 * y + x.
    QImage stored(3, 2, QImage::Format_RGB32);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 3; ++x) {
            stored.setPixel(x, y, qRgb(10 * y + x, 0, 0));
        }
    }
    const auto at = [](const QImage& image, int x, int y) { return qRed(image.pixel(x, y)); };
    using O = image::Orientation;

    EXPECT_EQ(apply_orientation(stored, O::Normal), stored);
    const QImage mirrored = apply_orientation(stored, O::FlipHorizontal);
    EXPECT_EQ(at(mirrored, 0, 0), 2);
    const QImage turned = apply_orientation(stored, O::Rotate180);
    EXPECT_EQ(at(turned, 0, 0), 12);
    const QImage flipped = apply_orientation(stored, O::FlipVertical);
    EXPECT_EQ(at(flipped, 0, 0), 10);

    // 90-degree orientations: 2 x 3. The stored top-left pixel lands where EXIF says the
    // 0th row/column are.
    const QImage cw = apply_orientation(stored, O::Rotate90Cw);  // 0th row is the right side
    ASSERT_EQ(cw.size(), QSize(2, 3));
    EXPECT_EQ(at(cw, 1, 0), 0);
    EXPECT_EQ(at(cw, 0, 0), 10);
    EXPECT_EQ(at(cw, 1, 2), 2);
    const QImage ccw = apply_orientation(stored, O::Rotate90Ccw);  // 0th row is the left side
    EXPECT_EQ(at(ccw, 0, 2), 0);
    EXPECT_EQ(at(ccw, 0, 0), 2);
    const QImage transpose = apply_orientation(stored, O::Transpose);  // (x, y) -> (y, x)
    EXPECT_EQ(at(transpose, 0, 0), 0);
    EXPECT_EQ(at(transpose, 1, 0), 10);
    EXPECT_EQ(at(transpose, 0, 2), 2);
    const QImage transverse = apply_orientation(stored, O::Transverse);  // anti-diagonal
    EXPECT_EQ(at(transverse, 1, 2), 0);
    EXPECT_EQ(at(transverse, 0, 0), 12);
    for (const QImage& image : {cw, ccw, transpose, transverse}) {
        EXPECT_EQ(image.size(), QSize(2, 3));
    }
}

TEST(AdapterConversions, CameraPreviewsDecodeAtTheRequestedSizeAndReportDamage) {
    raw::EmbeddedPreview preview;
    preview.bytes = rts::jpeg_bytes(rts::kJpegRgb96x64);
    preview.width = 96;
    preview.height = 64;
    preview.components = 3;
    QString error;
    EXPECT_EQ(decode_camera_preview(preview, {512, 512}, &error).size(), QSize(96, 64));
    EXPECT_EQ(decode_camera_preview(preview, {48, 0}, &error).size(), QSize(48, 32));
    preview.orientation = image::Orientation::Rotate90Ccw;
    EXPECT_EQ(decode_camera_preview(preview, {32, 32}, &error).size(), QSize(21, 32));

    preview.bytes[0] = std::byte{0};  // no SOI: not a JPEG stream
    preview.bytes[1] = std::byte{0};
    const QImage damaged = decode_camera_preview(preview, {512, 512}, &error);
    EXPECT_TRUE(damaged.isNull());
    EXPECT_FALSE(error.isEmpty());
}

TEST(AdapterConversions, RenderInfoCarriesTheHistogramAndTheRenderFacts) {
    app::PreviewImage preview;
    preview.image.width = 4;
    preview.image.height = 2;
    preview.histogram.red[0] = 3;
    preview.histogram.luminance[255] = 5;
    preview.histogram.pixels = 8;
    preview.summary.rendered_size = image::Size{8192, 5464};
    preview.summary.look = "neutral";
    preview.summary.look_version = 1;
    preview.summary.provisional = true;
    preview.summary.render_seconds = 0.9;
    preview.summary.interpretation = raw::InterpretationStatus::OptionalOperationsSkipped;
    preview.summary.issues = {{raw::InterpretationIssueKind::OptionalOperationUnsupported, "dev text"}};
    preview.seconds = 1.1;
    const RenderInfoPtr info = to_render_info(preview);
    EXPECT_EQ(info->red[0], 3u);
    EXPECT_EQ(info->luma[255], 5u);
    EXPECT_EQ(info->pixels, 8u);
    EXPECT_EQ(info->preview_width, 4);
    EXPECT_EQ(info->rendered_width, 8192);
    EXPECT_EQ(info->rendering, QStringLiteral("Neutral v1"));
    EXPECT_TRUE(info->provisional);
    EXPECT_DOUBLE_EQ(info->preview_seconds, 1.1);
    EXPECT_EQ(info->notes,
              QStringList{describe_issue(raw::InterpretationIssueKind::OptionalOperationUnsupported)});
    EXPECT_FALSE(info->incomplete);  // Limited: a conservative path, not an incomplete render

    // A mandatory step missing (Unsupported): the render is marked incomplete.
    preview.summary.interpretation = raw::InterpretationStatus::RequiresUnsupportedOperation;
    preview.summary.issues = {{raw::InterpretationIssueKind::MandatoryOperationUnsupported, "dev text"}};
    EXPECT_TRUE(to_render_info(preview)->incomplete);
    preview.summary.interpretation = raw::InterpretationStatus::Complete;
    preview.summary.issues.clear();
    EXPECT_FALSE(to_render_info(preview)->incomplete);
}

TEST(AdapterConversions, ExportOptionsMapToTheExporterSettings) {
    const exporter::ExportSettings jpeg = to_export_settings(ExportOptions{});
    EXPECT_EQ(jpeg.format, exporter::ExportFormat::Jpeg);
    EXPECT_EQ(jpeg.profile, exporter::OutputProfile::Srgb);
    EXPECT_EQ(jpeg.jpeg.quality, 95);
    const exporter::ExportSettings p3 =
        to_export_settings(ExportOptions{ExportFileFormat::Jpeg, ExportColorSpace::DisplayP3, 150});
    EXPECT_EQ(p3.profile, exporter::OutputProfile::DisplayP3);
    EXPECT_EQ(p3.jpeg.quality, 100);  // clamped
    const exporter::ExportSettings tiff =
        to_export_settings(ExportOptions{ExportFileFormat::Tiff16, ExportColorSpace::DisplayP3});
    EXPECT_EQ(tiff.format, exporter::ExportFormat::Tiff);
    EXPECT_EQ(tiff.profile, exporter::OutputProfile::DisplayP3);
    EXPECT_TRUE(tiff.validate());
}

TEST(AdapterConversions, ImportSummariesCountAndExplainSkippedPaths) {
    app::ImportReport report;
    report.added = {app::ItemId{1}, app::ItemId{2}};
    report.unsupported = 1;
    report.ignored_non_raw = 3;
    report.cancelled = true;
    report.refreshed = {app::ItemId{7}};
    report.restored = {app::ItemId{11}, app::ItemId{12}};
    report.skipped = {
        {L"C:\\a.dng", app::SkipReason::Duplicate, app::ItemId{9}, std::nullopt},
        {L"C:\\out", app::SkipReason::InOutputDirectory, std::nullopt, std::nullopt},
        {L"C:\\a.jpg", app::SkipReason::NotRawPhoto, std::nullopt, std::nullopt},
        {L"C:\\loop", app::SkipReason::DirectoryCycle, std::nullopt, std::nullopt},
        {L"C:\\gone.cr3", app::SkipReason::NotFound, std::nullopt, Error{ErrorCode::NotFound, "", "", 0}},
        {L"C:\\locked.cr3", app::SkipReason::Inaccessible, std::nullopt,
         Error{ErrorCode::PermissionDenied, "", "core.file.open", 0}},
        {L"C:\\bad.dng", app::SkipReason::InvalidName, std::nullopt, Error{ErrorCode::InvalidArgument, "", "", 0}},
    };
    const ImportSummary summary = summarize_import({QStringLiteral("C:/")}, report);
    EXPECT_EQ(summary.added, 2);
    EXPECT_EQ(summary.unsupported, 1);
    EXPECT_EQ(summary.already_in_library, 1);
    EXPECT_EQ(summary.updated, 1);
    EXPECT_EQ(summary.restored, 2);
    EXPECT_EQ(summary.ignored, 4);  // three by extension, a.jpg; the output folder is a note, not a file
    EXPECT_EQ(summary.failed, 3);
    EXPECT_TRUE(summary.cancelled);
    EXPECT_EQ(summary.notes,
              (QStringList{QStringLiteral(R"(C:\out was not searched: Lucent exported photos into it in this session.)"),
                           QStringLiteral(R"(C:\a.jpg is not a RAW photo, so it was not added.)")}));
    EXPECT_EQ(summary.errors,
              (QStringList{QStringLiteral(R"(C:\gone.cr3 was not found.)"),
                           QStringLiteral(R"(C:\locked.cr3 could not be read: Windows denied access )"
                                          R"(to the file.)"),
                           QStringLiteral(R"(C:\bad.dng was not added: its name contains characters Lucent )"
                                          R"(cannot store. Rename it and add it again.)")}));
}

// ---- W9: version-2 settings, geometry maps and mask overlays (edit_conversions.hpp) ---------------------

TEST(AdapterConversions, VersionTwoSettingsRoundTripThroughTheEditor) {
    engine::DevelopSettings settings;
    settings.exposure_ev = 0.5;
    settings.chroma_denoise = engine::ChromaDenoiseSetting{0.4, 6.0};
    settings.geometry.straighten_degrees = 2.5;
    settings.geometry.crop = engine::CropRect{0.1, 0.2, 0.9, 0.8};
    settings.geometry.quarter_turns = 3;
    settings.geometry.flip_horizontal = true;
    engine::LocalAdjustment brush;
    brush.id = 2;
    brush.invert = true;
    brush.opacity = 0.8;
    engine::BrushStroke stroke;
    stroke.mode = engine::BrushMode::Erase;
    stroke.radius = 0.03;
    stroke.hardness = 0.2;
    stroke.flow = 0.7;
    stroke.points = {{0.1, 0.2}, {0.3, 0.4}};
    brush.shape = engine::BrushMask{{stroke}};
    brush.controls.exposure_ev = -0.5;
    engine::LocalAdjustment linear;
    linear.id = 3;
    linear.enabled = false;
    linear.shape = engine::LinearGradientMask{{0.2, 0.1}, {0.4, 0.9}, 0.3};
    linear.controls.tint = 0.25;
    engine::LocalAdjustment radial;
    radial.id = 4;
    radial.shape = engine::RadialGradientMask{{0.6, 0.4}, 0.2, 0.1, -30.0, 0.7};
    radial.controls.temperature = -0.4;
    settings.local_adjustments = {brush, linear, radial};
    ASSERT_TRUE(settings.validate());

    const EditSettings edit = to_edit_settings(settings);
    ASSERT_EQ(edit.masks.size(), 3);
    EXPECT_EQ(edit.masks[0].kind, MaskKind::Brush);
    EXPECT_EQ(edit.masks[0].strokes.front().mode, BrushMode::Erase);
    EXPECT_EQ(edit.masks[1].kind, MaskKind::Linear);
    EXPECT_EQ(edit.masks[2].kind, MaskKind::Radial);
    EXPECT_DOUBLE_EQ(edit.masks[2].angle_degrees, -30.0);
    EXPECT_EQ(edit.geometry.quarter_turns, 3);
    EXPECT_FALSE(edit.renders_as_shot());
    EXPECT_EQ(to_develop_settings(edit, engine::DevelopSettings{}), settings);

    // A version-1 base (an item last edited before version 2) is upgraded under the editor's values.
    engine::DevelopSettings old;
    old.version = 1;
    const engine::DevelopSettings upgraded = to_develop_settings(EditSettings{}, old);
    EXPECT_EQ(upgraded.version, engine::kDevelopSettingsVersion);
    EXPECT_TRUE(upgraded.validate());
}

TEST(AdapterConversions, GeometryMapsAreTheEnginesMappingAsExactTransforms) {
    const EditGeometry geometry{3.0, EditCrop{0.1, 0.1, 0.9, 0.95}, 1, true};
    const QSize upright(600, 400);
    const GeometryMap map = geometry_map(geometry, upright);
    auto mapping = engine::GeometryMapping::create(to_geometry_setting(geometry), image::Size{600, 400});
    ASSERT_TRUE(mapping);
    ASSERT_TRUE(map.valid);
    EXPECT_EQ(map.output_size, QSize(mapping->output_size().width, mapping->output_size().height));
    EXPECT_DOUBLE_EQ(map.pixels_per_unit_length, 600.0);
    for (const QPointF point : {QPointF(0.0, 0.0), QPointF(0.5, 0.5), QPointF(0.83, 0.12), QPointF(-0.4, 1.7)}) {
        const QPointF shown = map.normalized_to_output.map(point);
        const image::PointD expected = mapping->output_from_normalized(engine::MaskPoint{point.x(), point.y()});
        EXPECT_NEAR(shown.x(), expected.x, 1e-9);
        EXPECT_NEAR(shown.y(), expected.y, 1e-9);
    }
    // The crop frame: the straightened canvas's crop corner is the output's corner (it is turned and
    // mirrored, so the top-left crop corner lands on another corner of the output).
    const QRectF crop = map.straightened_to_output.mapRect(QRectF(QPointF(0.1, 0.1), QPointF(0.9, 0.95)));
    EXPECT_NEAR(crop.left(), 0.0, 1e-9);
    EXPECT_NEAR(crop.top(), 0.0, 1e-9);
    EXPECT_NEAR(crop.width(), map.output_size.width(), 1e-9);
    EXPECT_NEAR(crop.height(), map.output_size.height(), 1e-9);
    // Invalid geometry (a reversed crop) or no size: no map.
    EXPECT_FALSE(geometry_map(EditGeometry{0.0, EditCrop{0.6, 0.0, 0.4, 1.0}, 0, false}, upright).valid);
    EXPECT_FALSE(geometry_map(EditGeometry{}, QSize()).valid);
    // The turn helpers are the engine's.
    EXPECT_EQ(geometry_turned(EditGeometry{}, GeometryTurn::Clockwise).quarter_turns, 1);
    EXPECT_EQ(geometry_turned(EditGeometry{}, GeometryTurn::CounterClockwise).quarter_turns, 3);
    EXPECT_TRUE(geometry_turned(EditGeometry{}, GeometryTurn::FlipHorizontal).flip_horizontal);
}

TEST(AdapterConversions, AMaskOverlayIsTheMasksWeightsThroughTheGeometry) {
    // A horizontal stroke of radius 20 px (0.1 of 200) through the middle of a 200 x 100 image.
    EditMask mask;
    mask.id = 5;
    mask.kind = MaskKind::Brush;
    EditBrushStroke stroke;
    stroke.radius = 0.1;
    stroke.hardness = 0.5;
    stroke.points = {QPointF(0.25, 0.5), QPointF(0.75, 0.5)};
    mask.strokes = {stroke};
    mask.enabled = false;  // the overlay shows where a switched-off mask is too
    const QImage overlay = mask_overlay_image(to_local_adjustment(mask), engine::GeometrySetting{},
                                              image::Size{200, 100});
    ASSERT_EQ(overlay.size(), QSize(200, 100));
    // Full weight within half the radius, the smoothstep beyond, nothing past the radius.
    EXPECT_EQ(qAlpha(overlay.pixel(100, 50)), 153);
    EXPECT_EQ(qAlpha(overlay.pixel(100, 55)), 153);                      // d = 5.5
    const double u = (12.5 - 10.0) / 10.0;                                // d = 12.5
    EXPECT_NEAR(qAlpha(overlay.pixel(100, 62)), (1.0 - u * u * (3.0 - 2.0 * u)) * 153.0, 1.0);
    EXPECT_EQ(qAlpha(overlay.pixel(100, 75)), 0);                          // d = 25.5
    EXPECT_EQ(qAlpha(overlay.pixel(10, 50)), 0);                           // beyond the stroke's end
    // A quarter turn turns the overlay with the image; a large image is reduced to the long edge given.
    engine::GeometrySetting turned;
    turned.quarter_turns = 1;
    const QImage small = mask_overlay_image(to_local_adjustment(mask), turned, image::Size{2000, 1000}, 400);
    ASSERT_EQ(small.size(), QSize(200, 400));
    EXPECT_EQ(qAlpha(small.pixel(100, 200)), 153);  // the stroke runs down the middle
    EXPECT_EQ(qAlpha(small.pixel(20, 200)), 0);
}

// The overlay lies exactly on a frame with a small crop: its pixels are the frame's output pixels
// mapped back through the frame's geometry (a crop rounded at the overlay's own scale was off by up
// to an overlay pixel, 8 frame pixels here), and it keeps its resolution across the crop.
TEST(AdapterConversions, AMaskOverlayLiesOnASmallCrop) {
    const image::Size upright{8192, 5464};
    engine::GeometrySetting geometry;
    geometry.crop = engine::CropRect{0.40, 0.40, 0.45, 0.45};
    auto mapping = engine::GeometryMapping::create(geometry, upright);
    ASSERT_TRUE(mapping);
    ASSERT_EQ(mapping->output_size(), (image::Size{409, 273}));
    // A horizontal linear gradient whose transition band (0.1 of 0.05 of the width: 41 pixels) lies
    // inside the crop.
    engine::LocalAdjustment linear;
    linear.id = 1;
    linear.shape = engine::LinearGradientMask{{0.40, 0.42}, {0.45, 0.42}, 0.1};
    linear.controls.exposure_ev = 1.0;
    const QImage overlay = mask_overlay_image(linear, geometry, upright);
    ASSERT_EQ(overlay.size(), QSize(409, 273));  // one overlay pixel per frame pixel
    const double start = 0.40 * upright.width;
    const double length = 0.05 * upright.width;
    double worst = 0.0;
    for (int x = 0; x < overlay.width(); ++x) {
        // The frame's column x shows upright column 3277 + x (the crop's first pixel, rounded).
        const double t = (3277.0 + x + 0.5 - start) / length;
        const double u = std::clamp((t - 0.45) / 0.1, 0.0, 1.0);
        const double expected = (1.0 - u * u * (3.0 - 2.0 * u)) * 153.0;
        worst = std::max(worst, std::abs(qAlpha(overlay.pixel(x, 100)) - expected));
    }
    EXPECT_LT(worst, 4.0);
}

}  // namespace
}  // namespace lucent::ui::adapters
