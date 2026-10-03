// The adapters over the persistent library (W4, docs/adr/0008): a catalog-backed session that is
// closed and reopened (items, ids and picks survive; the contact sheet and the camera thumbnails
// come back before any source file is read; the background check then marks missing files),
// background hashing and content duplicates, relinking, copying into a library folder, catalog
// backups, and the fallback when the catalog is unavailable. Real services, synthetic DNGs.

#include <QCoreApplication>
#include <QFileInfo>
#include <QSignalSpy>
#include <QThread>
#include <filesystem>
#include <memory>

#include <gtest/gtest.h>

#include <lucent/catalog/catalog.hpp>
#include <lucent/core/hash.hpp>

#include "adapter_test_support.hpp"
#include "models/library_model.hpp"

namespace lucent::ui {
namespace {

using adapters::AppLibraryService;
using testing::kAdapterWaitMs;
using testing::request_image;
using testing::wait_completed;
using testing::write_dng;
namespace rts = lucent::raw::test_support;

class CatalogAdaptersTest : public ::testing::Test {
protected:
    void TearDown() override { close(); }

    // Creates the real services over the catalog and thumbnail cache in the scratch folder.
    void open() {
        adapters::BackendOptions options = testing::test_backend_options();
        options.catalog_path = catalog_path();
        options.thumbnail_directory = dir_.path() / L"lucent\\cache\\thumbnails";
        auto services = adapters::create_application_services(options);
        ASSERT_TRUE(services) << services.error().describe();
        services_ = std::move(*services);
        library_ = dynamic_cast<AppLibraryService*>(services_.library.get());
        ASSERT_NE(library_, nullptr);
        QObject::connect(library_, &LibraryService::items_added, library_,
                         [this](const QList<LibraryItem>& items) { added_ << items; });
        QObject::connect(library_, &LibraryService::import_finished, library_,
                         [this](const ImportSummary& summary) { summaries_ << summary; });
        QObject::connect(library_, &LibraryService::verification_progress, library_,
                         [this](const VerificationProgress& progress) { verification_ << progress; });
    }

    void close() {
        services_ = Services{};
        library_ = nullptr;
    }

    std::filesystem::path catalog_path() const { return dir_.path() / L"lucent\\data\\lucent.catalog"; }
    QString path_of(const std::wstring& relative) const {
        return QString::fromStdWString((dir_.path() / relative).wstring());
    }

    ImportSummary import(const QStringList& paths, const ImportOptions& options = {}) {
        const qsizetype before = summaries_.size();
        library_->start_import(paths, options);
        EXPECT_TRUE(QTest::qWaitFor([&] { return summaries_.size() > before; }, kAdapterWaitMs));
        return summaries_.isEmpty() ? ImportSummary{} : summaries_.back();
    }

    // Waits until the background verification (checks, hashing) has finished.
    bool wait_verified() {
        return QTest::qWaitFor([&] { return !library_->is_busy(); }, kAdapterWaitMs);
    }

    rts::ScratchDir dir_;
    Services services_;
    AppLibraryService* library_ = nullptr;
    QList<LibraryItem> added_;  // items_added since open()
    QList<ImportSummary> summaries_;
    QList<VerificationProgress> verification_;
};

// Looks an item up by file name among the items the service knows (the initial ones and those
// added since).
LibraryItem find_named(AppLibraryService& library, const QList<LibraryItem>& candidates,
                       const QString& name) {
    for (const LibraryItem& candidate : candidates) {
        if (const LibraryItem* item = library.item(candidate.id);
            item && QFileInfo(item->file_path).fileName() == name) {
            return *item;
        }
    }
    return {};
}

TEST_F(CatalogAdaptersTest, TheLibraryAndItsThumbnailsComeBackAfterARestartBeforeAnyFileIsRead) {
    open();
    write_dng(dir_, L"card\\a.dng", testing::dng_with_preview());
    write_dng(dir_, L"card\\b.dng", rts::SyntheticDngSpec{.model = "Synthetic DNA"});
    const ImportSummary summary = import({path_of(L"card")});
    ASSERT_EQ(summary.added, 2);
    const LibraryItem a = find_named(*library_, added_, QStringLiteral("a.dng"));
    ASSERT_NE(a.id, 0u);
    library_->set_pick(a.id, PickState::Picked);
    ASSERT_TRUE(
        QTest::qWaitFor([&] { return library_->item(a.id)->pick == PickState::Picked; }, kAdapterWaitMs));
    // The import queued hashing (and the camera-thumbnail warm-up) in the background.
    ASSERT_TRUE(wait_verified());
    EXPECT_TRUE(library_->item(a.id)->content_verified);
    ASSERT_FALSE(verification_.isEmpty());
    EXPECT_FALSE(verification_.back().active);
    close();

    // The card is removed while Lucent is closed.
    std::filesystem::rename(dir_.path() / L"card", dir_.path() / L"card-out");
    open();
    const QList<LibraryItem> loaded = library_->current_items();
    ASSERT_EQ(loaded.size(), 2);
    const LibraryItem reloaded = find_named(*library_, loaded, QStringLiteral("a.dng"));
    EXPECT_EQ(reloaded.id, a.id);
    EXPECT_EQ(reloaded.pick, PickState::Picked);
    EXPECT_EQ(reloaded.camera_model, QStringLiteral("Synthetic DNG"));
    EXPECT_TRUE(reloaded.content_verified);
    EXPECT_FALSE(has_flag(reloaded.flags, ItemFlag::MissingSource));  // not checked yet
    EXPECT_EQ(reloaded.thumbnail_generation, kCameraPreviewGeneration);

    // A model created now starts with the library.
    LibraryModel model(*library_, std::make_shared<GenerationRegistry>());
    EXPECT_EQ(model.count(), 2);
    EXPECT_EQ(model.missing_count(), 0);

    // The camera thumbnail comes from the disk cache: the file is not there to read.
    auto reply = request_image(*services_.images, ImageKind::Thumbnail, a.id, kCameraPreviewGeneration,
                               QSize(512, 512));
    ASSERT_TRUE(wait_completed(*reply));
    ASSERT_EQ(reply->state(), ImageReply::State::Finished) << reply->error().toStdString();
    EXPECT_EQ(reply->image().size(), QSize(96, 64));

    // The background check marks both files missing; they stay listed.
    library_->start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return model.missing_count() == 2; }, kAdapterWaitMs));
    EXPECT_TRUE(has_flag(library_->item(a.id)->flags, ItemFlag::MissingSource));
    EXPECT_EQ(model.count(), 2);
    ASSERT_TRUE(wait_verified());

    // Relink finds them in their new folder (their own file versions, moved: no hash needed).
    QSignalSpy relinked(library_, &LibraryService::relink_finished);
    EXPECT_TRUE(library_->can_relink());
    library_->relink(path_of(L"card-out"), {});
    ASSERT_TRUE(QTest::qWaitFor([&] { return relinked.count() == 1; }, kAdapterWaitMs));
    const RelinkSummary relink = relinked.front().front().value<RelinkSummary>();
    EXPECT_TRUE(relink.error.isEmpty()) << relink.error.toStdString();
    EXPECT_EQ(relink.missing, 2);
    EXPECT_EQ(relink.relinked, 2);  // both were hashed after the import
    EXPECT_EQ(relink.unmatched, 0);
    ASSERT_TRUE(QTest::qWaitFor([&] { return model.missing_count() == 0; }, kAdapterWaitMs));
    EXPECT_TRUE(library_->item(a.id)->file_path.contains(QStringLiteral("card-out")));
    EXPECT_EQ(library_->item(a.id)->pick, PickState::Picked);
}

// A card removed and inserted again while Lucent runs: a re-check (Lucent.exe asks for one when a
// volume arrives or leaves) marks its photos Missing and Present again; importing the card again
// once it is back also finds them again instead of calling them "already in the library".
TEST_F(CatalogAdaptersTest, ACardRemovedDuringTheSessionIsNoticedAndFoundAgain) {
    open();
    LibraryModel model(*library_, std::make_shared<GenerationRegistry>());  // sees the import's items
    write_dng(dir_, L"card\\a.dng", testing::dng_with_preview());
    ASSERT_EQ(import({path_of(L"card")}).added, 1);
    ASSERT_EQ(model.count(), 1);
    library_->start();
    ASSERT_TRUE(wait_verified());
    EXPECT_EQ(model.missing_count(), 0);

    std::filesystem::rename(dir_.path() / L"card", dir_.path() / L"card-out");  // removed
    library_->recheck_sources();
    ASSERT_TRUE(QTest::qWaitFor([&] { return model.missing_count() == 1; }, kAdapterWaitMs));
    ASSERT_TRUE(wait_verified());
    std::filesystem::rename(dir_.path() / L"card-out", dir_.path() / L"card");  // back
    library_->recheck_sources();
    ASSERT_TRUE(QTest::qWaitFor([&] { return model.missing_count() == 0; }, kAdapterWaitMs));
    ASSERT_TRUE(wait_verified());

    std::filesystem::rename(dir_.path() / L"card", dir_.path() / L"card-out");
    library_->recheck_sources();
    ASSERT_TRUE(QTest::qWaitFor([&] { return model.missing_count() == 1; }, kAdapterWaitMs));
    ASSERT_TRUE(wait_verified());
    std::filesystem::rename(dir_.path() / L"card-out", dir_.path() / L"card");
    const ImportSummary again = import({path_of(L"card")});
    EXPECT_EQ(again.restored, 1);
    EXPECT_EQ(again.already_in_library, 0);
    EXPECT_EQ(again.added, 0);
    ASSERT_TRUE(QTest::qWaitFor([&] { return model.missing_count() == 0; }, kAdapterWaitMs));
}

TEST_F(CatalogAdaptersTest, ContentDuplicatesAreFlaggedWithTheOtherPaths) {
    open();
    write_dng(dir_, L"in\\a.dng");
    write_dng(dir_, L"in\\copy.dng");  // the same bytes
    ASSERT_EQ(import({path_of(L"in")}).added, 2);
    ASSERT_TRUE(wait_verified());
    const LibraryItem a = find_named(*library_, added_, QStringLiteral("a.dng"));
    ASSERT_NE(a.id, 0u);
    EXPECT_TRUE(has_flag(a.flags, ItemFlag::DuplicateContent));
    ASSERT_EQ(a.duplicate_paths.size(), 1);
    EXPECT_TRUE(a.duplicate_paths.front().endsWith(QStringLiteral("copy.dng")));
    const LibraryItem copy = find_named(*library_, added_, QStringLiteral("copy.dng"));
    EXPECT_TRUE(has_flag(copy.flags, ItemFlag::DuplicateContent));
    // The choice is kept per item: rejecting the copy leaves the original as it is.
    library_->set_pick(copy.id, PickState::Rejected);
    ASSERT_TRUE(QTest::qWaitFor([&] { return library_->item(copy.id)->pick == PickState::Rejected; },
                                kAdapterWaitMs));
    EXPECT_EQ(library_->item(a.id)->pick, PickState::None);
}

TEST_F(CatalogAdaptersTest, CopyIntoLibraryFolderAddsVerifiedCopies) {
    open();
    write_dng(dir_, L"card\\DCIM\\a.dng", testing::dng_with_preview());
    std::filesystem::create_directories(dir_.path() / L"Pictures");
    const auto original = dir_.path() / L"card\\DCIM\\a.dng";
    auto before = core::hash_file(original, {});
    ASSERT_TRUE(before);
    const ImportSummary summary = import({path_of(L"card")}, ImportOptions{true, path_of(L"Pictures")});
    EXPECT_EQ(summary.added, 1);
    EXPECT_EQ(summary.copied, 1);
    EXPECT_EQ(summary.copy_to, path_of(L"Pictures"));
    ASSERT_EQ(added_.size(), 1);
    EXPECT_TRUE(added_.front().copied);
    EXPECT_TRUE(added_.front().origin_path.endsWith(QStringLiteral("a.dng")));
    EXPECT_TRUE(added_.front().file_path.contains(QStringLiteral("Pictures")));
    EXPECT_TRUE(added_.front().content_verified);  // verified while copying
    auto after = core::hash_file(original, {});
    ASSERT_TRUE(after);
    EXPECT_EQ(after->digest, before->digest);
    EXPECT_EQ(after->identity, before->identity);
}

TEST_F(CatalogAdaptersTest, TheCatalogIsBackedUpInTheBackground) {
    open();
    EXPECT_TRUE(library_->can_back_up());
    QSignalSpy finished(library_, &LibraryService::backup_finished);
    library_->back_up_catalog();
    EXPECT_TRUE(library_->is_busy());
    ASSERT_TRUE(QTest::qWaitFor([&] { return finished.count() == 1; }, kAdapterWaitMs));
    const BackupResult result = finished.front().front().value<BackupResult>();
    ASSERT_TRUE(result.ok) << result.message.toStdString();
    EXPECT_TRUE(QFileInfo::exists(result.path));
    EXPECT_TRUE(result.path.startsWith(library_->backup_folder()));
    EXPECT_FALSE(library_->is_busy());
}

TEST_F(CatalogAdaptersTest, ACatalogOpenElsewhereFallsBackToAnUnsavedLibraryWithANotice) {
    // Another Lucent holds the catalog.
    catalog::OpenOptions other;
    other.path = catalog_path();
    auto held = catalog::Catalog::open(other);
    ASSERT_TRUE(held) << held.error().describe();
    open();
    EXPECT_FALSE(library_->library_notice().isEmpty());
    EXPECT_TRUE(library_->library_notice().contains(QStringLiteral("another Lucent window")))
        << library_->library_notice().toStdString();
    EXPECT_FALSE(library_->can_back_up());
    EXPECT_FALSE(library_->can_relink());
    // Imports still work, in memory.
    write_dng(dir_, L"in\\a.dng");
    EXPECT_EQ(import({path_of(L"in")}).added, 1);
    QSignalSpy relinked(library_, &LibraryService::relink_finished);
    library_->relink(path_of(L"in"), {});
    ASSERT_TRUE(QTest::qWaitFor([&] { return relinked.count() == 1; }, kAdapterWaitMs));
    EXPECT_FALSE(relinked.front().front().value<RelinkSummary>().error.isEmpty());
}

}  // namespace
}  // namespace lucent::ui
