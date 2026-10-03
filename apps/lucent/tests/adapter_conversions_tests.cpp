// Conversions between the application services' types and the shell's (adapters/conversions.hpp).

#include <QImage>

#include <gtest/gtest.h>

#include "adapter_test_support.hpp"
#include "adapters/conversions.hpp"

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

}  // namespace
}  // namespace lucent::ui::adapters
