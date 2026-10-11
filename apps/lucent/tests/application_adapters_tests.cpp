// The adapters over the application services (adapters/): import into the shell's model with
// per-file reasons, camera-preview thumbnails, Lucent previews with generation and staleness
// handling across threads, cancellation, export, and shutdown with work outstanding.
//
// The services are real (session library, executor, preview and export services, LibRaw); the
// files are synthetic DNGs. Tests that control timing use GatedRenderer in place of the engine;
// export tests use the engine itself.

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "adapter_test_support.hpp"

namespace lucent::ui {
namespace {

using adapters::AppExportService;
using adapters::AppLibraryService;
using testing::GatedRenderer;
using testing::kAdapterWaitMs;
using testing::request_image;
using testing::wait_completed;
using testing::write_dng;
namespace rts = lucent::raw::test_support;

class ApplicationAdaptersTest : public ::testing::Test {
protected:
    void TearDown() override {
        if (gated_) {
            gated_->open_all();
        }
        services_ = Services{};
    }

    void create(std::shared_ptr<GatedRenderer> renderer) {
        gated_ = renderer;
        create(std::shared_ptr<const app::SourceRenderer>(std::move(renderer)));
    }

    // Creates the real services, with `renderer` in place of the engine when given.
    void create(std::shared_ptr<const app::SourceRenderer> renderer = {}) {
        auto services =
            adapters::create_application_services(testing::test_backend_options(std::move(renderer)));
        ASSERT_TRUE(services) << services.error().describe();
        services_ = std::move(*services);
        library_ = dynamic_cast<AppLibraryService*>(services_.library.get());
        exports_ = dynamic_cast<AppExportService*>(services_.exports.get());
        ASSERT_NE(library_, nullptr);
        ASSERT_NE(exports_, nullptr);
        QObject::connect(
            library_, &LibraryService::items_added, library_, [this](const QList<LibraryItem>& items) {
                off_ui_thread_ |= QThread::currentThread() != QCoreApplication::instance()->thread();
                added_ << items;
            });
        QObject::connect(library_, &LibraryService::item_changed, library_, [this](const LibraryItem& item) {
            off_ui_thread_ |= QThread::currentThread() != QCoreApplication::instance()->thread();
            changes_ << item;
        });
        QObject::connect(library_, &LibraryService::import_finished, library_,
                         [this](const ImportSummary& summary) { summaries_ << summary; });
        QObject::connect(exports_, &ExportService::job_added, exports_,
                         [this](const ExportJob& job) { job_events_ << job; });
        QObject::connect(exports_, &ExportService::job_changed, exports_, [this](const ExportJob& job) {
            off_ui_thread_ |= QThread::currentThread() != QCoreApplication::instance()->thread();
            job_events_ << job;
        });
    }

    ImportSummary import(const QStringList& paths, bool include_subfolders = false) {
        const qsizetype before = summaries_.size();
        library_->import_paths(paths, include_subfolders);
        EXPECT_TRUE(QTest::qWaitFor([&] { return summaries_.size() > before; }, kAdapterWaitMs));
        return summaries_.isEmpty() ? ImportSummary{} : summaries_.back();
    }

    ImportSummary import_dir() { return import({QString::fromStdWString(dir_.path().wstring())}); }

    [[nodiscard]] ItemId id_of(const QString& file_name) const {
        for (const LibraryItem& item : added_) {
            if (QFileInfo(item.file_path).fileName() == file_name) {
                return item.id;
            }
        }
        ADD_FAILURE() << "no item " << file_name.toStdString();
        return 0;
    }

    [[nodiscard]] LibraryItem current(ItemId id) const {
        const LibraryItem* item = library_->item(id);
        return item != nullptr ? *item : LibraryItem{};
    }

    // The last job snapshot of `id`.
    [[nodiscard]] ExportJob job(JobId id) const {
        for (auto it = job_events_.crbegin(); it != job_events_.crend(); ++it) {
            if (it->id == id) {
                return *it;
            }
        }
        return {};
    }

    [[nodiscard]] QString out_dir() const {
        return QString::fromStdWString((dir_.path() / L"out").wstring());
    }

    rts::ScratchDir dir_;
    std::shared_ptr<GatedRenderer> gated_;
    Services services_;
    AppLibraryService* library_ = nullptr;
    AppExportService* exports_ = nullptr;
    QList<LibraryItem> added_;
    QList<LibraryItem> changes_;
    QList<ImportSummary> summaries_;
    QList<ExportJob> job_events_;
    bool off_ui_thread_ = false;
};

TEST_F(ApplicationAdaptersTest, ImportListsPhotosOnTheUiThreadWithMetadataSupportAndReasons) {
    create();
    write_dng(dir_, L"b.dng", testing::dng_with_preview());
    write_dng(dir_, L"a.dng");
    rts::SyntheticDngSpec needs_opcode;
    needs_opcode.opcode_list3 = rts::opcode_list({{9, 0x01030000, 0, rts::filler(76)}});  // mandatory GainMap
    write_dng(dir_, L"c.dng", needs_opcode);
    dir_.write_text(L"broken.dng", "not a raw file");
    dir_.write_text(L"notes.txt", "ignored");
    write_dng(dir_, L"sub\\d.dng");

    QSignalSpy started(library_, &LibraryService::import_started);
    const ImportSummary summary = import_dir();
    EXPECT_EQ(started.count(), 1);
    EXPECT_FALSE(library_->is_importing());
    EXPECT_FALSE(off_ui_thread_);

    EXPECT_EQ(summary.added, 4);
    EXPECT_EQ(summary.unsupported, 2);
    EXPECT_EQ(summary.ignored, 1);  // notes.txt
    EXPECT_EQ(summary.failed, 0);
    EXPECT_TRUE(summary.errors.isEmpty());
    ASSERT_EQ(summary.notes.size(), 2) << summary.notes.join('\n').toStdString();
    EXPECT_TRUE(summary.notes[0].startsWith(QStringLiteral("broken.dng — Unsupported: ")));
    EXPECT_TRUE(summary.notes[1].startsWith(QStringLiteral("c.dng — Unsupported: ")));

    // Deterministic order (sorted by name), subfolders only on request.
    ASSERT_EQ(added_.size(), 4);
    QStringList names;
    for (const LibraryItem& item : added_) {
        names << QFileInfo(item.file_path).fileName();
    }
    EXPECT_EQ(names, (QStringList{"a.dng", "b.dng", "broken.dng", "c.dng"}));

    const LibraryItem a = added_[0];
    EXPECT_NE(a.id, 0u);
    EXPECT_EQ(a.camera_make, QStringLiteral("Lucent"));
    EXPECT_EQ(a.camera_model, QStringLiteral("Synthetic DNG"));
    EXPECT_EQ(a.pixel_width, 64);
    EXPECT_EQ(a.pixel_height, 48);
    EXPECT_GT(a.file_size_bytes, 0);
    // The synthetic camera is not in the bundled camera support database (W10, ADR 0018).
    EXPECT_EQ(a.support, SupportStatus::CompatibleUnverified);
    EXPECT_TRUE(a.support_detail.contains(QStringLiteral("not in Lucent's camera support database")))
        << a.support_detail.toStdString();
    EXPECT_EQ(a.thumbnail_generation, kCameraPreviewGeneration);
    EXPECT_TRUE(a.thumbnail_is_camera_preview);
    EXPECT_EQ(a.preview_generation, 1u);
    EXPECT_EQ(a.processing, ProcessingStatus::NotProcessed);

    const LibraryItem broken = added_[2];
    EXPECT_EQ(broken.support, SupportStatus::Unsupported);
    EXPECT_EQ(broken.support_detail, QStringLiteral("Lucent cannot decode this file."));
    EXPECT_EQ(broken.thumbnail_generation, kNoImage);  // nothing to extract from an unknown file

    const LibraryItem c = added_[3];
    EXPECT_EQ(c.support, SupportStatus::Unsupported);
    EXPECT_TRUE(c.support_detail.contains(QStringLiteral("processing step")))
        << c.support_detail.toStdString();
    EXPECT_TRUE(c.thumbnail_is_camera_preview);

    // Again, with subfolders: only the new file is added; the rest are already in the library.
    const ImportSummary again = import({QString::fromStdWString(dir_.path().wstring())}, true);
    EXPECT_EQ(again.added, 1);
    EXPECT_EQ(again.already_in_library, 4);
    EXPECT_EQ(added_.size(), 5);
    EXPECT_EQ(QFileInfo(added_.back().file_path).fileName(), QStringLiteral("d.dng"));

    // A missing path is a per-path failure with a reason, not an error of the whole import.
    const QString missing = QString::fromStdWString((dir_.path() / L"missing.dng").wstring());
    const ImportSummary failed = import({missing});
    EXPECT_EQ(failed.added, 0);
    EXPECT_EQ(failed.failed, 1);
    ASSERT_EQ(failed.errors.size(), 1);
    EXPECT_TRUE(failed.errors[0].contains(QStringLiteral("missing.dng was not found")))
        << failed.errors[0].toStdString();
}

// W10 (ADR 0018): the services find and verify the bundled camera support database next to the
// executable, and say why when there is none.
TEST_F(ApplicationAdaptersTest, TheBundledCameraSupportDatabaseIsActive) {
    create();
    const CameraSupportInfo info = library_->camera_support();
    EXPECT_TRUE(info.available) << info.problem.toStdString();
    EXPECT_FALSE(info.version.isEmpty());
    EXPECT_FALSE(info.qualified_on.isEmpty());
    EXPECT_GT(info.modes, 0);
    EXPECT_EQ(info.modes, info.verified + info.compatible_unverified + info.limited + info.unsupported);
    EXPECT_TRUE(info.bundled);
    EXPECT_TRUE(info.location.endsWith(QStringLiteral("assets\\camera-support")))
        << info.location.toStdString();
    EXPECT_TRUE(info.problem.isEmpty());
}

TEST(ApplicationAdaptersCameraSupport, AMissingDatabaseIsReportedNotReplaced) {
    QTemporaryDir empty;
    ASSERT_TRUE(empty.isValid());
    adapters::BackendOptions options = testing::test_backend_options();
    assets::AssetLocations locations;
    locations.bundled = std::filesystem::path(QDir::toNativeSeparators(empty.path()).toStdWString());
    options.asset_locations = locations;
    auto services = adapters::create_application_services(options);
    ASSERT_TRUE(services) << services.error().describe();
    const CameraSupportInfo info = services->library->camera_support();
    EXPECT_FALSE(info.available);
    EXPECT_TRUE(info.problem.contains(QStringLiteral("not installed"))) << info.problem.toStdString();
}

// W13 (ADR 0023): the services verify and load the bundled lens database next to the executable, and
// without it Lucent still starts: lens corrections are unavailable, with the reason (About and the
// Lens corrections panel), and nothing can be chosen.
TEST_F(ApplicationAdaptersTest, TheBundledLensDatabaseIsActive) {
    create();
    // W12 (APL-06): it loads off the UI thread.
    ASSERT_TRUE(QTest::qWaitFor([&] { return !library_->lens_database().loading; }, kAdapterWaitMs));
    const LensDatabaseInfo info = library_->lens_database();
    EXPECT_TRUE(info.available) << info.problem.toStdString();
    EXPECT_EQ(info.version, QStringLiteral("1.0.0"));
    EXPECT_GT(info.lenses, 1000);
    EXPECT_GT(info.cameras, 1000);
    EXPECT_TRUE(info.location.endsWith(QStringLiteral("assets\\lens-database"))) << info.location.toStdString();
    EXPECT_TRUE(info.problem.isEmpty());
}

TEST(ApplicationAdaptersLensDatabase, AMissingDatabaseLeavesCorrectionsUnavailable) {
    QTemporaryDir empty;
    ASSERT_TRUE(empty.isValid());
    adapters::BackendOptions options = testing::test_backend_options();
    assets::AssetLocations locations;
    locations.bundled = std::filesystem::path(QDir::toNativeSeparators(empty.path()).toStdWString());
    options.asset_locations = locations;
    auto services = adapters::create_application_services(options);
    ASSERT_TRUE(services) << services.error().describe();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !services->library->lens_database().loading; }, kAdapterWaitMs));
    const LensDatabaseInfo info = services->library->lens_database();
    EXPECT_FALSE(info.available);
    EXPECT_TRUE(info.problem.contains(QStringLiteral("not installed"))) << info.problem.toStdString();
    EXPECT_EQ(services->edits->lens_database().problem, info.problem);
    EXPECT_TRUE(services->edits->search_lens_profiles(QStringLiteral("nikkor"), 10).isEmpty());
}

TEST_F(ApplicationAdaptersTest, SupportedSuffixesAreTheImportRawExtensions) {
    create();
    const QStringList suffixes = library_->supported_suffixes();
    EXPECT_TRUE(suffixes.contains(QStringLiteral("cr3")));
    EXPECT_TRUE(suffixes.contains(QStringLiteral("dng")));
    EXPECT_FALSE(suffixes.contains(QStringLiteral("jpg")));
}

TEST_F(ApplicationAdaptersTest, CameraPreviewThumbnailIsDecodedUprightIgnoringTheJpegOrientation) {
    create();
    write_dng(dir_, L"rotated.dng", testing::dng_with_preview(6));  // EXIF 6: rotate 90 degrees clockwise
    import_dir();
    const ItemId id = id_of(QStringLiteral("rotated.dng"));

    auto reply =
        request_image(*services_.images, ImageKind::Thumbnail, id, kCameraPreviewGeneration, {512, 512});
    ASSERT_TRUE(wait_completed(*reply));
    ASSERT_EQ(reply->state(), ImageReply::State::Finished) << reply->error().toStdString();
    const QImage image = reply->image();
    ASSERT_EQ(image.size(), QSize(64, 96));  // the 96 x 64 stored JPEG, upright
    // Stored: red rises left to right, green top to bottom. Rotated clockwise, red rises top to
    // bottom and green right to left.
    EXPECT_GT(qRed(image.pixel(32, 90)), qRed(image.pixel(32, 5)) + 100);
    EXPECT_GT(qGreen(image.pixel(3, 48)), qGreen(image.pixel(60, 48)) + 100);

    // A smaller box scales it down, keeping the aspect ratio.
    auto small =
        request_image(*services_.images, ImageKind::Thumbnail, id, kCameraPreviewGeneration, {48, 48});
    ASSERT_TRUE(wait_completed(*small));
    ASSERT_EQ(small->state(), ImageReply::State::Finished);
    EXPECT_EQ(small->image().size(), QSize(32, 48));
    EXPECT_TRUE(changes_.isEmpty());  // still a labelled camera preview
}

TEST_F(ApplicationAdaptersTest, FilesWithoutACameraPreviewMoveToLucentThumbnails) {
    auto renderer = std::make_shared<GatedRenderer>();
    create(renderer);
    write_dng(dir_, L"plain.dng");
    import_dir();
    const ItemId id = id_of(QStringLiteral("plain.dng"));

    auto camera =
        request_image(*services_.images, ImageKind::Thumbnail, id, kCameraPreviewGeneration, {512, 512});
    ASSERT_TRUE(wait_completed(*camera));
    EXPECT_EQ(camera->state(), ImageReply::State::Failed);
    ASSERT_TRUE(QTest::qWaitFor([&] { return current(id).thumbnail_generation == 2; }, kAdapterWaitMs));
    EXPECT_FALSE(current(id).thumbnail_is_camera_preview);

    auto lucent = request_image(*services_.images, ImageKind::Thumbnail, id, 2, {512, 512});
    ASSERT_TRUE(wait_completed(*lucent));
    ASSERT_EQ(lucent->state(), ImageReply::State::Finished) << lucent->error().toStdString();
    EXPECT_EQ(lucent->image().size(), QSize(120, 80));  // never enlarged
    EXPECT_EQ(renderer->finished(L"plain.dng"), 1);
    ASSERT_TRUE(QTest::qWaitFor([&] { return current(id).processing == ProcessingStatus::Processed; },
                                kAdapterWaitMs));
    // A thumbnail render (W6: PreviewService::request_rendered_thumbnail, the JPEG of the recipe)
    // has no histogram: render facts come from the Edit view's renders.
    EXPECT_EQ(current(id).render, nullptr);

    // Cached: the next request is served without rendering again.
    auto again = request_image(*services_.images, ImageKind::Thumbnail, id, 2, {512, 512});
    ASSERT_TRUE(wait_completed(*again));
    EXPECT_EQ(again->state(), ImageReply::State::Finished);
    EXPECT_EQ(renderer->started(L"plain.dng"), 1);
    EXPECT_FALSE(off_ui_thread_);
}

TEST_F(ApplicationAdaptersTest, PreviewQualityFollowsTheRequestedBoxAndTheRenderedSize) {
    auto renderer = std::make_shared<GatedRenderer>(image::Size{120, 80});
    create(renderer);
    write_dng(dir_, L"a.dng", testing::dng_with_preview());
    import_dir();
    const ItemId id = id_of(QStringLiteral("a.dng"));

    // The probe says 64 x 48; a 60 x 60 box asks for Fit 60 of the (120 x 80) render.
    auto fit = request_image(*services_.images, ImageKind::Preview, id, 1, {60, 60});
    ASSERT_TRUE(wait_completed(*fit));
    ASSERT_EQ(fit->state(), ImageReply::State::Finished) << fit->error().toStdString();
    EXPECT_EQ(fit->image().size(), QSize(60, 40));
    ASSERT_TRUE(QTest::qWaitFor([&] { return current(id).processing == ProcessingStatus::Processed; },
                                kAdapterWaitMs));
    const LibraryItem rendered = current(id);
    EXPECT_EQ(rendered.pixel_width, 120);  // the render's size is authoritative
    EXPECT_EQ(rendered.pixel_height, 80);
    ASSERT_NE(rendered.render, nullptr);
    EXPECT_EQ(rendered.render->preview_width, 60);
    EXPECT_EQ(rendered.render->pixels, 60u * 40u);
    // The camera preview thumbnail was replaced by Lucent's render.
    EXPECT_EQ(rendered.thumbnail_generation, 2u);
    EXPECT_FALSE(rendered.thumbnail_is_camera_preview);
    auto thumbnail = request_image(*services_.images, ImageKind::Thumbnail, id, 2, {512, 512});
    ASSERT_TRUE(wait_completed(*thumbnail));
    EXPECT_EQ(thumbnail->state(), ImageReply::State::Finished);
    EXPECT_EQ(renderer->started(L"a.dng"), 1);  // from the Edit preview, not a new render

    // The full size (as the 100 % view asks for it) is a Full preview.
    auto full = request_image(*services_.images, ImageKind::Preview, id, 1, {120, 80});
    ASSERT_TRUE(wait_completed(*full));
    ASSERT_EQ(full->state(), ImageReply::State::Finished);
    EXPECT_EQ(full->image().size(), QSize(120, 80));
    EXPECT_EQ(full->image().format(), QImage::Format_RGB32);
}

TEST_F(ApplicationAdaptersTest, ANewerPreviewSupersedesAnOlderOneAcrossThreads) {
    auto renderer = std::make_shared<GatedRenderer>();
    create(renderer);
    write_dng(dir_, L"a.dng");
    write_dng(dir_, L"b.dng");
    import_dir();
    const ItemId a = id_of(QStringLiteral("a.dng"));
    const ItemId b = id_of(QStringLiteral("b.dng"));
    renderer->close_gate(L"a.dng");
    renderer->close_gate(L"b.dng");

    // Moving from A to B (the filmstrip): B's request cancels A's render.
    auto reply_a = request_image(*services_.images, ImageKind::Preview, a, 1, {256, 256});
    ASSERT_TRUE(renderer->wait_started(L"a.dng", 1));
    ASSERT_TRUE(QTest::qWaitFor([&] { return current(a).processing == ProcessingStatus::Processing; },
                                kAdapterWaitMs));
    auto reply_b = request_image(*services_.images, ImageKind::Preview, b, 1, {256, 256});
    ASSERT_TRUE(wait_completed(*reply_a));
    EXPECT_EQ(reply_a->state(), ImageReply::State::Failed);
    EXPECT_EQ(renderer->cancelled(L"a.dng"), 1);  // cooperative cancellation reached the render

    renderer->open_gate(L"b.dng");
    ASSERT_TRUE(wait_completed(*reply_b));
    ASSERT_EQ(reply_b->state(), ImageReply::State::Finished) << reply_b->error().toStdString();
    ASSERT_TRUE(QTest::qWaitFor([&] { return current(b).processing == ProcessingStatus::Processed; },
                                kAdapterWaitMs));
    EXPECT_EQ(current(a).processing, ProcessingStatus::NotProcessed);
    EXPECT_EQ(current(a).render, nullptr);
    EXPECT_FALSE(off_ui_thread_);
}

TEST_F(ApplicationAdaptersTest, ALateResultIgnoringCancellationNeverReplacesTheNewerPreview) {
    auto renderer = std::make_shared<GatedRenderer>();
    create(renderer);
    write_dng(dir_, L"a.dng");
    write_dng(dir_, L"b.dng");
    import_dir();
    const ItemId a = id_of(QStringLiteral("a.dng"));
    const ItemId b = id_of(QStringLiteral("b.dng"));
    renderer->close_gate(L"a.dng", /*ignore_cancellation=*/true);

    // B is requested while A renders. A ignores its cancellation and keeps the memory admission
    // (one full-resolution render at a time), so B waits; A then completes with a result that is
    // superseded, never delivered, and B follows.
    auto reply_a = request_image(*services_.images, ImageKind::Preview, a, 1, {256, 256});
    ASSERT_TRUE(renderer->wait_started(L"a.dng", 1));
    auto reply_b = request_image(*services_.images, ImageKind::Preview, b, 1, {256, 256});
    QTest::qWait(100);
    EXPECT_EQ(renderer->started(L"b.dng"), 0);
    EXPECT_EQ(reply_a->state(), ImageReply::State::Pending);
    renderer->open_gate(L"a.dng");
    ASSERT_TRUE(wait_completed(*reply_a));
    EXPECT_EQ(reply_a->state(), ImageReply::State::Failed);
    EXPECT_EQ(renderer->finished(L"a.dng"), 1);  // the late result existed
    ASSERT_TRUE(wait_completed(*reply_b));
    ASSERT_EQ(reply_b->state(), ImageReply::State::Finished) << reply_b->error().toStdString();
    ASSERT_TRUE(QTest::qWaitFor([&] { return current(a).processing == ProcessingStatus::NotProcessed; },
                                kAdapterWaitMs));
    EXPECT_EQ(current(b).processing, ProcessingStatus::Processed);
    for (const LibraryItem& change : changes_) {
        EXPECT_FALSE(change.id == a && change.processing == ProcessingStatus::Processed);
    }
}

TEST_F(ApplicationAdaptersTest, CancellingTheNewestPreviewStopsItsRender) {
    auto renderer = std::make_shared<GatedRenderer>();
    create(renderer);
    write_dng(dir_, L"a.dng");
    import_dir();
    const ItemId a = id_of(QStringLiteral("a.dng"));
    renderer->close_gate(L"a.dng");

    auto reply = request_image(*services_.images, ImageKind::Preview, a, 1, {256, 256});
    ASSERT_TRUE(renderer->wait_started(L"a.dng", 1));
    reply->cancel();  // what Qt Quick does when the Edit view closes
    ASSERT_TRUE(QTest::qWaitFor([&] { return renderer->cancelled(L"a.dng") == 1; }, kAdapterWaitMs));
    ASSERT_TRUE(QTest::qWaitFor([&] { return current(a).processing == ProcessingStatus::NotProcessed; },
                                kAdapterWaitMs));
    EXPECT_EQ(renderer->finished(L"a.dng"), 0);
}

TEST_F(ApplicationAdaptersTest, ThumbnailRendersWaitWhileAnEditPreviewIsOutstanding) {
    auto renderer = std::make_shared<GatedRenderer>();
    create(renderer);
    write_dng(dir_, L"a.dng");
    write_dng(dir_, L"b.dng");
    import_dir();
    const ItemId a = id_of(QStringLiteral("a.dng"));
    const ItemId b = id_of(QStringLiteral("b.dng"));
    renderer->close_gate(L"a.dng");

    auto preview = request_image(*services_.images, ImageKind::Preview, a, 1, {256, 256});
    ASSERT_TRUE(renderer->wait_started(L"a.dng", 1));
    auto thumbnail = request_image(*services_.images, ImageKind::Thumbnail, b, 2, {512, 512});
    QTest::qWait(200);
    EXPECT_EQ(renderer->started(L"b.dng"), 0);  // the photo being looked at goes first
    EXPECT_EQ(thumbnail->state(), ImageReply::State::Pending);

    renderer->open_gate(L"a.dng");
    ASSERT_TRUE(wait_completed(*preview));
    ASSERT_TRUE(wait_completed(*thumbnail));
    EXPECT_EQ(thumbnail->state(), ImageReply::State::Finished) << thumbnail->error().toStdString();
    EXPECT_EQ(renderer->started(L"b.dng"), 1);
}

TEST_F(ApplicationAdaptersTest, InvalidRequestsFailWithoutReachingTheServices) {
    auto renderer = std::make_shared<GatedRenderer>();
    create(renderer);
    auto no_item = request_image(*services_.images, ImageKind::Preview, 0, 1, {64, 64});
    EXPECT_EQ(no_item->state(), ImageReply::State::Failed);
    auto unknown = request_image(*services_.images, ImageKind::Preview, 999, 1, {64, 64});
    ASSERT_TRUE(wait_completed(*unknown));
    EXPECT_EQ(unknown->state(), ImageReply::State::Failed);
    EXPECT_EQ(unknown->error(), QStringLiteral("This photo is no longer in the library."));
}

TEST_F(ApplicationAdaptersTest, DestroyingTheServicesCompletesEveryOutstandingReply) {
    auto renderer = std::make_shared<GatedRenderer>();
    create(renderer);
    write_dng(dir_, L"a.dng", testing::dng_with_preview());
    write_dng(dir_, L"b.dng");
    import_dir();
    const ItemId a = id_of(QStringLiteral("a.dng"));
    const ItemId b = id_of(QStringLiteral("b.dng"));
    renderer->close_gate(L"a.dng");  // never opened
    renderer->close_gate(L"b.dng");

    auto preview = request_image(*services_.images, ImageKind::Preview, a, 1, {256, 256});
    ASSERT_TRUE(renderer->wait_started(L"a.dng", 1));
    auto queued = request_image(*services_.images, ImageKind::Thumbnail, b, 2, {512, 512});
    std::filesystem::create_directories(dir_.path() / L"out");
    services_.exports->start_export(ExportRequest{{b}, out_dir(), {}});

    QElapsedTimer clock;
    clock.start();
    services_ = Services{};  // what closing the window does
    EXPECT_LT(clock.elapsed(), 10000);
    EXPECT_NE(preview->state(), ImageReply::State::Pending);
    EXPECT_NE(queued->state(), ImageReply::State::Pending);
    // Nothing is posted to the destroyed adapters.
    QTest::qWait(100);
    library_ = nullptr;
    exports_ = nullptr;
}

TEST_F(ApplicationAdaptersTest, ExportWritesJpegAndTiffAndReportsProgress) {
    create();  // the engine
    write_dng(dir_, L"a.dng");
    std::filesystem::create_directories(dir_.path() / L"out");
    import_dir();
    const ItemId a = id_of(QStringLiteral("a.dng"));
    EXPECT_EQ(exports_->suggested_file_name(a, {}), QStringLiteral("a_Lucent.jpg"));
    EXPECT_EQ(exports_->suggested_file_name(
                  a, ExportOptions{ExportFileFormat::Tiff16, ExportColorSpace::DisplayP3}),
              QStringLiteral("a_Lucent.tif"));
    EXPECT_TRUE(exports_->suggested_file_name(12345, {}).isEmpty());

    QSignalSpy busy(exports_, &ExportService::busy_changed);
    exports_->start_export(
        ExportRequest{{a}, out_dir(), ExportOptions{ExportFileFormat::Jpeg, ExportColorSpace::Srgb, 90}});
    exports_->start_export(
        ExportRequest{{a}, out_dir(), ExportOptions{ExportFileFormat::Tiff16, ExportColorSpace::DisplayP3}});
    EXPECT_TRUE(exports_->is_busy());
    ASSERT_TRUE(QTest::qWaitFor([&] { return !exports_->is_busy(); }, kAdapterWaitMs));
    EXPECT_GE(busy.count(), 2);  // busy, then idle

    const ExportJob jpeg = job(1);
    ASSERT_EQ(jpeg.state, ExportJobState::Completed) << jpeg.message.toStdString();
    EXPECT_EQ(QFileInfo(jpeg.destination_path).fileName(), QStringLiteral("a_Lucent.jpg"));
    EXPECT_TRUE(QFileInfo::exists(jpeg.destination_path));
    EXPECT_EQ(jpeg.options.jpeg_quality, 90);
    EXPECT_DOUBLE_EQ(jpeg.progress, 1.0);
    const ExportJob tiff = job(2);
    ASSERT_EQ(tiff.state, ExportJobState::Completed) << tiff.message.toStdString();
    EXPECT_EQ(QFileInfo(tiff.destination_path).fileName(), QStringLiteral("a_Lucent.tif"));
    EXPECT_TRUE(QFileInfo::exists(tiff.destination_path));

    // Snapshots arrived in order: added (Queued), Running with progress, then the final state.
    bool saw_running = false;
    for (const ExportJob& event : job_events_) {
        if (event.id == 1 && event.state == ExportJobState::Running) {
            saw_running = true;
            EXPECT_FALSE(event.message.isEmpty());  // the current stage
        }
    }
    EXPECT_TRUE(saw_running);
    EXPECT_EQ(job_events_.front().state, ExportJobState::Queued);
    EXPECT_FALSE(off_ui_thread_);

    // Again: a new name, never overwriting.
    exports_->start_export(ExportRequest{{a}, out_dir(), {}});
    ASSERT_TRUE(QTest::qWaitFor([&] { return !exports_->is_busy(); }, kAdapterWaitMs));
    EXPECT_EQ(QFileInfo(job(3).destination_path).fileName(), QStringLiteral("a_Lucent_2.jpg"));
}

TEST_F(ApplicationAdaptersTest, AQueuedExportKeepsTheRecipeItHadWhenItWasQueued) {
    auto renderer = std::make_shared<GatedRenderer>();
    create(renderer);
    write_dng(dir_, L"a.dng");
    write_dng(dir_, L"b.dng");
    std::filesystem::create_directories(dir_.path() / L"out");
    import_dir();
    const ItemId a = id_of(QStringLiteral("a.dng"));
    const ItemId b = id_of(QStringLiteral("b.dng"));
    QList<quint64> answered;
    QObject::connect(services_.edits.get(), &EditService::state_changed, services_.edits.get(),
                     [&answered](const EditState& state) { answered << state.token; });
    const auto apply = [&](ItemId item, double exposure) {
        EditSettings settings;
        settings.exposure_ev = exposure;
        const quint64 token = services_.edits->apply(item, settings);
        ASSERT_TRUE(QTest::qWaitFor([&] { return answered.contains(token); }, kAdapterWaitMs));
    };
    apply(b, 0.5);  // the recipe when the export is queued
    renderer->close_gate(L"a.dng");

    exports_->start_export(ExportRequest{{a, b}, out_dir(), {}});
    ASSERT_TRUE(renderer->wait_started(L"a.dng", 1));
    EXPECT_EQ(job(2).state, ExportJobState::Queued);
    // Editing b while its export waits its turn does not change that export.
    apply(b, 1.0);
    renderer->open_gate(L"a.dng");
    ASSERT_TRUE(QTest::qWaitFor([&] { return !exports_->is_busy(); }, kAdapterWaitMs));
    ASSERT_EQ(job(2).state, ExportJobState::Completed) << job(2).message.toStdString();
    const std::vector<engine::DevelopSettings> rendered = renderer->rendered_settings(L"b.dng");
    ASSERT_EQ(rendered.size(), 1u);
    EXPECT_DOUBLE_EQ(rendered[0].exposure_ev, 0.5);
    ASSERT_EQ(renderer->rendered_settings(L"a.dng").size(), 1u);
    EXPECT_DOUBLE_EQ(renderer->rendered_settings(L"a.dng")[0].exposure_ev, 0.0);

    // A queued export of a photo that left the library fails at once with the reason.
    exports_->start_export(ExportRequest{{777777}, out_dir(), {}});
    ASSERT_TRUE(QTest::qWaitFor([&] { return !exports_->is_busy(); }, kAdapterWaitMs));
    EXPECT_EQ(job(3).state, ExportJobState::Failed);
    EXPECT_EQ(job(3).message, QStringLiteral("This photo is no longer in the library."));
}

TEST_F(ApplicationAdaptersTest, ExportRefusesUnsupportedPhotosAndReportsAMissingFolder) {
    create();
    rts::SyntheticDngSpec needs_opcode;
    needs_opcode.opcode_list3 = rts::opcode_list({{9, 0x01030000, 0, rts::filler(76)}});
    write_dng(dir_, L"c.dng", needs_opcode);
    write_dng(dir_, L"a.dng");
    std::filesystem::create_directories(dir_.path() / L"out");
    import_dir();
    const ItemId a = id_of(QStringLiteral("a.dng"));
    const ItemId c = id_of(QStringLiteral("c.dng"));

    exports_->start_export(ExportRequest{{c}, out_dir(), {}});
    exports_->start_export(ExportRequest{{a}, out_dir() + QStringLiteral("\\missing"), {}});
    exports_->start_export(ExportRequest{{424242}, out_dir(), {}});
    ASSERT_TRUE(QTest::qWaitFor([&] { return !exports_->is_busy(); }, kAdapterWaitMs));

    const ExportJob unsupported = job(1);
    EXPECT_EQ(unsupported.state, ExportJobState::Failed);
    EXPECT_TRUE(unsupported.message.startsWith(QStringLiteral("This photo is Unsupported")))
        << unsupported.message.toStdString();
    EXPECT_TRUE(unsupported.message.contains(QStringLiteral("processing step")));
    const ExportJob missing = job(2);
    EXPECT_EQ(missing.state, ExportJobState::Failed);
    EXPECT_EQ(missing.message, QStringLiteral("The output folder does not exist."));
    const ExportJob unknown = job(3);
    EXPECT_EQ(unknown.state, ExportJobState::Failed);
    EXPECT_EQ(unknown.source_name, QStringLiteral("Photo 424242"));
    EXPECT_TRUE(std::filesystem::is_empty(dir_.path() / L"out"));
}

TEST_F(ApplicationAdaptersTest, ExportsRunOneAtATimeAndNeverHoldUpImportsOrCameraPreviews) {
    auto renderer = std::make_shared<GatedRenderer>();
    create(renderer);
    write_dng(dir_, L"a.dng");
    write_dng(dir_, L"b.dng");
    write_dng(dir_, L"c.dng");
    std::filesystem::create_directories(dir_.path() / L"out");
    import_dir();
    renderer->close_gate(L"a.dng");  // the first export renders until the test opens it
    renderer->close_gate(L"b.dng");

    const ItemId a = id_of(QStringLiteral("a.dng"));
    const ItemId b = id_of(QStringLiteral("b.dng"));
    const ItemId c = id_of(QStringLiteral("c.dng"));
    exports_->start_export(ExportRequest{{a, b, c}, out_dir(), {}});
    ASSERT_TRUE(renderer->wait_started(L"a.dng", 1));
    QTest::qWait(100);
    // Only the first job reached the application services; the others wait their turn in the
    // adapter (a submitted job would report its Preparing stage at once: a background thread is
    // free, so it would be Running while it waits for the memory admission).
    EXPECT_EQ(job(1).state, ExportJobState::Running);
    EXPECT_EQ(job(2).state, ExportJobState::Queued);
    EXPECT_EQ(job(3).state, ExportJobState::Queued);
    EXPECT_EQ(renderer->started(L"b.dng"), 0);

    // An import and a camera-preview thumbnail finish while the export is still rendering.
    write_dng(dir_, L"later\\d.dng", testing::dng_with_preview());
    const ImportSummary later = import({QString::fromStdWString((dir_.path() / L"later").wstring())});
    EXPECT_EQ(later.added, 1);
    auto thumbnail = request_image(*services_.images, ImageKind::Thumbnail, id_of(QStringLiteral("d.dng")),
                                   kCameraPreviewGeneration, {512, 512});
    ASSERT_TRUE(wait_completed(*thumbnail));
    EXPECT_EQ(thumbnail->state(), ImageReply::State::Finished) << thumbnail->error().toStdString();
    EXPECT_EQ(renderer->finished(L"a.dng"), 0);  // still rendering

    // A queued job cancelled before its turn ends at once and never renders.
    exports_->cancel_job(2);
    EXPECT_EQ(job(2).state, ExportJobState::Cancelled);
    renderer->open_gate(L"a.dng");
    ASSERT_TRUE(QTest::qWaitFor([&] { return !exports_->is_busy(); }, kAdapterWaitMs));
    EXPECT_EQ(job(1).state, ExportJobState::Completed) << job(1).message.toStdString();
    EXPECT_EQ(job(2).state, ExportJobState::Cancelled);
    EXPECT_EQ(job(3).state, ExportJobState::Completed) << job(3).message.toStdString();
    EXPECT_EQ(renderer->started(L"b.dng"), 0);
    // Job 3 started only after job 1 had finished.
    EXPECT_EQ(renderer->started(L"c.dng"), 1);
}

TEST_F(ApplicationAdaptersTest, ExportingIntoThePhotosFolderNeverHidesItFromLaterImports) {
    create();  // the engine
    write_dng(dir_, L"a.dng");
    import_dir();
    const ItemId a = id_of(QStringLiteral("a.dng"));

    // Export next to the original, the common choice.
    const QString photos = QString::fromStdWString(dir_.path().wstring());
    exports_->start_export(ExportRequest{{a}, photos, {}});
    ASSERT_TRUE(QTest::qWaitFor([&] { return !exports_->is_busy(); }, kAdapterWaitMs));
    ASSERT_EQ(job(1).state, ExportJobState::Completed) << job(1).message.toStdString();

    // New photos in that folder (and in a subfolder) are still found; the export is ignored as a JPEG.
    write_dng(dir_, L"b.dng");
    write_dng(dir_, L"sub\\c.dng");
    const ImportSummary again = import({photos}, true);
    EXPECT_EQ(again.added, 2);
    EXPECT_EQ(again.already_in_library, 1);
    EXPECT_EQ(again.ignored, 1);  // a_Lucent.jpg
    EXPECT_TRUE(again.notes.isEmpty()) << again.notes.join('\n').toStdString();

    // An output folder below the imported folder is not searched, and the Details say why.
    std::filesystem::create_directories(dir_.path() / L"out");
    exports_->start_export(ExportRequest{{a}, out_dir(), {}});
    ASSERT_TRUE(QTest::qWaitFor([&] { return !exports_->is_busy(); }, kAdapterWaitMs));
    write_dng(dir_, L"out\\copied_in_later.dng");
    const ImportSummary nested = import({photos}, true);
    EXPECT_EQ(nested.added, 0);
    ASSERT_EQ(nested.notes.size(), 1);
    EXPECT_TRUE(nested.notes[0].contains(QStringLiteral("was not searched"))) << nested.notes[0].toStdString();
}

TEST_F(ApplicationAdaptersTest, AddingAChangedFileAgainRefreshesItsItem) {
    create();  // the engine: SourceChanged comes from the real identity check
    const auto path = write_dng(dir_, L"a.dng", testing::dng_with_preview());
    import_dir();
    const ItemId a = id_of(QStringLiteral("a.dng"));
    auto first = request_image(*services_.images, ImageKind::Preview, a, 1, {32, 32});
    ASSERT_TRUE(wait_completed(*first));
    ASSERT_EQ(first->state(), ImageReply::State::Finished) << first->error().toStdString();
    ASSERT_TRUE(QTest::qWaitFor([&] { return current(a).processing == ProcessingStatus::Processed; },
                                kAdapterWaitMs));
    const LibraryItem before = current(a);

    // The file is rewritten on disk: renders refuse it, with advice that now works.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    rts::SyntheticDngSpec rewritten = testing::dng_with_preview();
    rewritten.model = "Rewritten";
    dir_.write(L"a.dng", rts::build_synthetic_dng(rewritten));
    auto stale = request_image(*services_.images, ImageKind::Preview, a, 1, {24, 24});
    ASSERT_TRUE(wait_completed(*stale));
    ASSERT_EQ(stale->state(), ImageReply::State::Failed);
    EXPECT_EQ(stale->error(), QStringLiteral("The file changed after it was added. Add it again to update it."));

    const ImportSummary again = import({QString::fromStdWString(path.wstring())});
    EXPECT_EQ(again.added, 0);
    EXPECT_EQ(again.updated, 1);
    EXPECT_EQ(again.already_in_library, 0);
    ASSERT_TRUE(QTest::qWaitFor([&] { return current(a).camera_model == QStringLiteral("Rewritten"); },
                                kAdapterWaitMs));
    const LibraryItem after = current(a);
    EXPECT_EQ(after.processing, ProcessingStatus::NotProcessed);
    EXPECT_EQ(after.render, nullptr);
    EXPECT_GT(after.preview_generation, before.preview_generation);
    EXPECT_GT(after.thumbnail_generation, before.thumbnail_generation);
    EXPECT_FALSE(after.thumbnail_is_camera_preview);

    // The new generations render the new file.
    auto preview = request_image(*services_.images, ImageKind::Preview, a, after.preview_generation, {32, 32});
    ASSERT_TRUE(wait_completed(*preview));
    EXPECT_EQ(preview->state(), ImageReply::State::Finished) << preview->error().toStdString();
    auto thumbnail =
        request_image(*services_.images, ImageKind::Thumbnail, a, after.thumbnail_generation, {512, 512});
    ASSERT_TRUE(wait_completed(*thumbnail));
    EXPECT_EQ(thumbnail->state(), ImageReply::State::Finished) << thumbnail->error().toStdString();
    EXPECT_FALSE(off_ui_thread_);
}

TEST_F(ApplicationAdaptersTest, DroppedCompanionFilesAreSkippedWithAReason) {
    create();
    const auto raw = write_dng(dir_, L"IMG_0001.dng");
    const auto jpeg = dir_.write_text(L"IMG_0001.JPG", "the camera's JPEG");
    const auto xmp = dir_.write_text(L"IMG_0001.xmp", "<x:xmpmeta/>");
    const ImportSummary summary = import({QString::fromStdWString(raw.wstring()),
                                          QString::fromStdWString(jpeg.wstring()),
                                          QString::fromStdWString(xmp.wstring())});
    EXPECT_EQ(summary.added, 1);
    EXPECT_EQ(summary.unsupported, 0);
    EXPECT_EQ(summary.ignored, 2);
    ASSERT_EQ(summary.notes.size(), 2) << summary.notes.join('\n').toStdString();
    EXPECT_TRUE(summary.notes[0].contains(QStringLiteral("IMG_0001.JPG is not a RAW photo")))
        << summary.notes[0].toStdString();
    EXPECT_EQ(added_.size(), 1);
}

TEST_F(ApplicationAdaptersTest, AnIncompleteRenderIsMarkedAndKeepsTheCameraThumbnail) {
    create();  // the engine
    rts::SyntheticDngSpec spec = testing::dng_with_preview();
    spec.opcode_list3 = rts::opcode_list({{9, 0x01030000, 0, rts::filler(76)}});  // mandatory GainMap
    write_dng(dir_, L"needs_opcode.dng", spec);
    import_dir();
    const ItemId id = id_of(QStringLiteral("needs_opcode.dng"));
    ASSERT_EQ(current(id).support, SupportStatus::Unsupported);

    auto preview = request_image(*services_.images, ImageKind::Preview, id, 1, {32, 32});
    ASSERT_TRUE(wait_completed(*preview));
    ASSERT_EQ(preview->state(), ImageReply::State::Finished) << preview->error().toStdString();
    ASSERT_TRUE(QTest::qWaitFor([&] { return current(id).processing == ProcessingStatus::Processed; },
                                kAdapterWaitMs));
    const LibraryItem item = current(id);
    ASSERT_NE(item.render, nullptr);
    EXPECT_TRUE(item.render->incomplete);
    // The camera's own rendering stays the thumbnail, labelled.
    EXPECT_EQ(item.thumbnail_generation, kCameraPreviewGeneration);
    EXPECT_TRUE(item.thumbnail_is_camera_preview);
}

TEST_F(ApplicationAdaptersTest, CancellingAnExportEndsItCancelled) {
    auto renderer = std::make_shared<GatedRenderer>();
    create(renderer);
    write_dng(dir_, L"a.dng");
    std::filesystem::create_directories(dir_.path() / L"out");
    import_dir();
    renderer->close_gate(L"a.dng");

    exports_->start_export(ExportRequest{{id_of(QStringLiteral("a.dng"))}, out_dir(), {}});
    ASSERT_TRUE(renderer->wait_started(L"a.dng", 1));
    exports_->cancel_job(1);
    EXPECT_TRUE(job(1).cancel_requested);
    EXPECT_EQ(job(1).message, QStringLiteral("Cancelling…"));
    exports_->cancel_job(1);  // idempotent
    ASSERT_TRUE(QTest::qWaitFor([&] { return job(1).state == ExportJobState::Cancelled; }, kAdapterWaitMs));
    EXPECT_FALSE(job(1).cancel_requested);
    EXPECT_FALSE(exports_->is_busy());
    EXPECT_TRUE(std::filesystem::is_empty(dir_.path() / L"out"));
}

}  // namespace
}  // namespace lucent::ui
