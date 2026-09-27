// The simulated services: real folder scanning, deterministic invented metadata, thumbnail
// replacement, painted images, and simulated export jobs.

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <optional>

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "fakes/fake_services.hpp"
#include "test_support.hpp"

namespace lucent::ui::fakes {
namespace {

constexpr int kWaitMs = 10000;

// Creates a small file at `relative` below `root` and returns its absolute path.
QString write_file(const QString& root, const QString& relative) {
    const QString path = QDir(root).filePath(relative);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("not really a raw file");
    return QFileInfo(path).absoluteFilePath();
}

// The first "IMG_nnnn.CR3" name whose simulated support status is (or is not) Unsupported.
QString sample_name(bool unsupported) {
    for (int i = 0; i < 1000; ++i) {
        const QString name = QStringLiteral("IMG_%1.CR3").arg(i, 4, 10, QLatin1Char('0'));
        const LibraryItem item = make_fake_item(1, QStringLiteral("C:/x/") + name, 0, {});
        if ((item.support == SupportStatus::Unsupported) == unsupported) {
            return name;
        }
    }
    ADD_FAILURE() << "no sample name found";
    return {};
}

TEST(FakeMetadata, StableHashIsFnv1a) {
    EXPECT_EQ(stable_hash(QString()), 2166136261u);
    EXPECT_EQ(stable_hash(QStringLiteral("a")), 0xe40c292cu);
}

TEST(FakeMetadata, IsDeterministicAndDerivedFromTheSuffix) {
    const QDateTime modified(QDate(2026, 9, 24), QTime(21, 9));
    const LibraryItem a = make_fake_item(3, QStringLiteral("C:/Photos/DSC06677.ARW"), 1234, modified);
    const LibraryItem b = make_fake_item(3, QStringLiteral("C:/Photos/DSC06677.ARW"), 1234, modified);
    EXPECT_EQ(a.camera_make, QStringLiteral("Sony"));
    EXPECT_EQ(a.camera_model, QStringLiteral("ILCE-7M4"));
    EXPECT_EQ(a.file_size_bytes, 1234);
    EXPECT_EQ(a.capture_time, modified);
    EXPECT_EQ(a.support, b.support);
    EXPECT_EQ(a.iso, b.iso);
    EXPECT_EQ(a.flags, b.flags);
    EXPECT_EQ(a.thumbnail_generation, Generation{1});
    EXPECT_TRUE(a.thumbnail_is_camera_preview);
    EXPECT_EQ(a.preview_generation, Generation{1});
    EXPECT_TRUE(a.pixel_width > 0 && a.pixel_height > 0);

    const LibraryItem dng = make_fake_item(4, QStringLiteral("C:/Photos/DJI_0109.DNG"), 1, modified);
    EXPECT_TRUE(dng.camera_make.isEmpty());
    EXPECT_TRUE(dng.camera_model.isEmpty());
}

class FakeLibraryServiceTest : public ::testing::Test {
protected:
    FakeLibraryServiceTest() {
        QObject::connect(&service, &LibraryService::import_finished, &service,
                         [this](const ImportSummary& summary) { last_summary = summary; });
    }

    ImportSummary import(const QStringList& paths, bool include_subfolders) {
        last_summary.reset();
        service.import_paths(paths, include_subfolders);
        EXPECT_TRUE(QTest::qWaitFor([this] { return last_summary.has_value(); }, kWaitMs));
        return last_summary.value_or(ImportSummary{});
    }

    QTemporaryDir dir;
    std::shared_ptr<FakeCatalog> catalog = std::make_shared<FakeCatalog>();
    FakeLibraryService service{catalog, FakeTimings::immediate()};
    std::optional<ImportSummary> last_summary;
};

TEST_F(FakeLibraryServiceTest, ScansSubfoldersOnlyWhenAsked) {
    ASSERT_TRUE(dir.isValid());
    write_file(dir.path(), QStringLiteral("b.arw"));
    write_file(dir.path(), QStringLiteral("A.CR3"));
    write_file(dir.path(), QStringLiteral("notes.txt"));
    write_file(dir.path(), QStringLiteral("sub/c.NEF"));

    QSignalSpy added(&service, &LibraryService::items_added);
    QSignalSpy started(&service, &LibraryService::import_started);
    service.import_paths({dir.path()}, false);
    EXPECT_TRUE(service.is_importing());
    EXPECT_TRUE(service.is_busy());
    EXPECT_EQ(started.count(), 1);
    ASSERT_TRUE(QTest::qWaitFor([this] { return last_summary.has_value(); }, kWaitMs));
    EXPECT_FALSE(service.is_importing());
    EXPECT_EQ(last_summary->added, 2);
    EXPECT_EQ(last_summary->ignored, 1);
    EXPECT_EQ(last_summary->failed, 0);
    EXPECT_EQ(added.count(), 1);
    ASSERT_EQ(service.items().size(), 2u);
    EXPECT_TRUE(service.items()[0].file_path.endsWith(QStringLiteral("A.CR3")));  // sorted, case-insensitive
    EXPECT_TRUE(service.items()[1].file_path.endsWith(QStringLiteral("b.arw")));
    EXPECT_EQ(service.items()[0].id, ItemId{1});

    const ImportSummary recursive = import({dir.path()}, true);
    EXPECT_EQ(recursive.added, 1);
    EXPECT_EQ(recursive.already_in_library, 2);
    EXPECT_EQ(recursive.ignored, 1);
    ASSERT_EQ(service.items().size(), 3u);
    EXPECT_TRUE(service.items()[2].file_path.endsWith(QStringLiteral("sub/c.NEF")));
}

TEST_F(FakeLibraryServiceTest, ImportsSingleFilesAndReportsMissingPaths) {
    const QString photo = write_file(dir.path(), QStringLiteral("A.CR3"));
    const QString text = write_file(dir.path(), QStringLiteral("readme.txt"));
    const QString missing = QDir(dir.path()).filePath(QStringLiteral("missing.CR3"));

    const ImportSummary summary = import({photo, text, missing, photo}, false);
    EXPECT_EQ(summary.added, 1);
    EXPECT_EQ(summary.ignored, 1);
    EXPECT_EQ(summary.failed, 1);
    ASSERT_EQ(summary.errors.size(), 1);
    EXPECT_TRUE(summary.errors[0].contains(QStringLiteral("missing.CR3")));
    EXPECT_EQ(summary.sources.size(), 4);
}

TEST_F(FakeLibraryServiceTest, ReplacesCameraPreviewsExceptForUnsupportedPhotos) {
    const QString supported = sample_name(false);
    const QString unsupported = sample_name(true);
    write_file(dir.path(), supported);
    write_file(dir.path(), unsupported);

    QSignalSpy changed(&service, &LibraryService::item_changed);
    QSignalSpy busy(&service, &LibraryService::busy_changed);
    import({dir.path()}, false);
    ASSERT_TRUE(QTest::qWaitFor([this] { return !service.is_busy(); }, kWaitMs));
    EXPECT_EQ(changed.count(), 1);
    ASSERT_GE(busy.count(), 2);
    EXPECT_FALSE(busy.last().at(0).toBool());

    for (const LibraryItem& item : service.items()) {
        if (item.file_path.endsWith(supported)) {
            EXPECT_EQ(item.thumbnail_generation, Generation{2});
            EXPECT_FALSE(item.thumbnail_is_camera_preview);
            EXPECT_EQ(item.processing, ProcessingStatus::Processed);
        } else {
            EXPECT_EQ(item.support, SupportStatus::Unsupported);
            EXPECT_EQ(item.thumbnail_generation, Generation{1});
            EXPECT_TRUE(item.thumbnail_is_camera_preview);
        }
    }
    const auto entry = catalog->find(1);
    ASSERT_TRUE(entry.has_value());
    EXPECT_FALSE(entry->file_name.isEmpty());
}

class FakeImageSourceTest : public ::testing::Test {
protected:
    FakeImageSourceTest() {
        catalog->add(1, FakeCatalog::Entry{QStringLiteral("A.CR3"), 600, 400, 7, SupportStatus::Verified, {}});
        catalog->add(2, FakeCatalog::Entry{QStringLiteral("B.CR3"), 600, 400, 9, SupportStatus::Unsupported,
                                           QStringLiteral("Simulated: cannot decode.")});
    }

    std::shared_ptr<ImageReply> request(ImageKind kind, ItemId item, QSize bounding) {
        auto reply = std::make_shared<ImageReply>();
        source->request_image(ImageRequest{kind, item, 1, bounding}, reply);
        EXPECT_TRUE(QTest::qWaitFor([&reply] { return reply->state() != ImageReply::State::Pending; }, kWaitMs));
        return reply;
    }

    std::shared_ptr<FakeCatalog> catalog = std::make_shared<FakeCatalog>();
    std::unique_ptr<FakeImageSource> source = std::make_unique<FakeImageSource>(catalog, FakeTimings::immediate());
};

TEST_F(FakeImageSourceTest, PaintsWithinTheBoundingBox) {
    const auto thumbnail = request(ImageKind::Thumbnail, 1, QSize(300, 300));
    EXPECT_EQ(thumbnail->image().size(), QSize(300, 200));
    const auto preview = request(ImageKind::Preview, 1, QSize());
    EXPECT_EQ(preview->image().size(), QSize(600, 400));
    // The camera preview (generation 1) and Lucent's render (generation 2) differ visibly.
    const QImage camera = FakeImageSource::paint(*catalog->find(1), ImageKind::Thumbnail, 1, QSize(60, 40));
    const QImage render = FakeImageSource::paint(*catalog->find(1), ImageKind::Thumbnail, 2, QSize(60, 40));
    EXPECT_NE(camera, render);
    EXPECT_EQ(render, FakeImageSource::paint(*catalog->find(1), ImageKind::Thumbnail, 2, QSize(60, 40)));
}

TEST_F(FakeImageSourceTest, FailsPreviewsOfUnsupportedAndUnknownPhotos) {
    const auto unsupported = request(ImageKind::Preview, 2, QSize(100, 100));
    EXPECT_EQ(unsupported->state(), ImageReply::State::Failed);
    EXPECT_EQ(unsupported->error(), QStringLiteral("Simulated: cannot decode."));
    // The embedded camera preview of an unsupported file is still shown.
    EXPECT_EQ(request(ImageKind::Thumbnail, 2, QSize(100, 100))->state(), ImageReply::State::Finished);
    EXPECT_EQ(request(ImageKind::Preview, 99, QSize(100, 100))->state(), ImageReply::State::Failed);
}

TEST_F(FakeImageSourceTest, DestructionCompletesOutstandingReplies) {
    FakeTimings slow = FakeTimings::immediate();
    slow.preview_delay_ms = 60000;
    source = std::make_unique<FakeImageSource>(catalog, slow);
    auto reply = std::make_shared<ImageReply>();
    source->request_image(ImageRequest{ImageKind::Preview, 1, 1, QSize(10, 10)}, reply);
    source.reset();
    EXPECT_EQ(reply->state(), ImageReply::State::Failed);
}

class FakeExportServiceTest : public ::testing::Test {
protected:
    FakeExportServiceTest() {
        catalog->add(1, FakeCatalog::Entry{QStringLiteral("A.CR3"), 600, 400, 1, SupportStatus::Verified, {}});
        catalog->add(2, FakeCatalog::Entry{QStringLiteral("B.CR3"), 600, 400, 2, SupportStatus::Unsupported,
                                           QStringLiteral("Simulated: cannot decode.")});
        catalog->add(3, FakeCatalog::Entry{QStringLiteral("C.ARW"), 600, 400, 3, SupportStatus::Limited, {}});
    }

    void watch(FakeExportService& service) {
        QObject::connect(&service, &ExportService::job_added, &service,
                         [this](const ExportJob& job) { jobs[job.id] = job; });
        QObject::connect(&service, &ExportService::job_changed, &service,
                         [this](const ExportJob& job) { jobs[job.id] = job; });
    }

    std::shared_ptr<FakeCatalog> catalog = std::make_shared<FakeCatalog>();
    std::map<JobId, ExportJob> jobs;
};

TEST_F(FakeExportServiceTest, RunsJobsInOrderAndFailsUnsupportedPhotos) {
    FakeExportService service(catalog, FakeTimings::immediate());
    watch(service);
    service.start_export(ExportRequest{{1, 2, 3}, QStringLiteral("C:/Out"), ExportOptions{}});
    EXPECT_TRUE(service.is_busy());
    ASSERT_TRUE(QTest::qWaitFor([&service] { return !service.is_busy(); }, kWaitMs));
    ASSERT_EQ(jobs.size(), 3u);
    EXPECT_EQ(jobs[1].state, ExportJobState::Completed);
    EXPECT_EQ(jobs[1].destination_path, QStringLiteral("C:/Out/A.jpg"));
    EXPECT_DOUBLE_EQ(jobs[1].progress, 1.0);
    EXPECT_EQ(jobs[2].state, ExportJobState::Failed);
    EXPECT_EQ(jobs[2].message, QStringLiteral("Simulated: cannot decode."));
    EXPECT_EQ(jobs[3].state, ExportJobState::Completed);
    service.start_export(ExportRequest{});  // nothing to do
    EXPECT_FALSE(service.is_busy());
}

TEST_F(FakeExportServiceTest, CancelsQueuedAndRunningJobs) {
    FakeTimings slow = FakeTimings::immediate();
    slow.export_step_ms = 60000;
    FakeExportService service(catalog, slow);
    watch(service);
    service.start_export(
        ExportRequest{{1, 3}, QStringLiteral("C:/Out"), ExportOptions{ExportFileFormat::Tiff16, ExportColorSpace::Srgb}});
    EXPECT_EQ(jobs[1].state, ExportJobState::Running);
    EXPECT_EQ(jobs[1].destination_path, QStringLiteral("C:/Out/A.tif"));
    EXPECT_EQ(jobs[2].state, ExportJobState::Queued);

    service.cancel_job(2);  // queued
    EXPECT_EQ(jobs[2].state, ExportJobState::Cancelled);
    EXPECT_EQ(jobs[1].state, ExportJobState::Running);
    service.cancel_job(1);  // running
    EXPECT_EQ(jobs[1].state, ExportJobState::Cancelled);
    EXPECT_FALSE(service.is_busy());
    service.cancel_job(1);  // finished jobs are ignored
    service.cancel_job(77);
    EXPECT_EQ(jobs[1].state, ExportJobState::Cancelled);
}

}  // namespace
}  // namespace lucent::ui::fakes
