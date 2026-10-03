// AppController: user intents become the right service calls; navigation and status follow.

#include <gtest/gtest.h>

#include <stdexcept>

#include <QDir>
#include <QSignalSpy>

#include "app_controller.hpp"
#include "test_support.hpp"

namespace lucent::ui {
namespace {

using testing::make_item;
using testing::make_scripted_services;
using testing::ScriptedServices;

class AppControllerTest : public ::testing::Test {
protected:
    AppControllerTest() : scripted(make_scripted_services()), controller(std::move(scripted.services)) {
        controller.set_url_opener([this](const QUrl& url) {
            opened.push_back(url);
            return opener_result;
        });
    }

    void add_two_photos() {
        scripted.library->add({make_item(1, QStringLiteral("A.CR3")), make_item(2, QStringLiteral("B.ARW"))});
    }

    ScriptedServices scripted;
    AppController controller;
    std::vector<QUrl> opened;
    bool opener_result = true;
};

TEST(AppControllerConstruction, RejectsIncompleteServices) {
    EXPECT_THROW(AppController(Services{}), std::invalid_argument);
}

TEST_F(AppControllerTest, AddFilesImportsOnlyLocalFiles) {
    EXPECT_TRUE(controller.addFiles({QUrl::fromLocalFile(QStringLiteral("C:/Photos/a.cr3")),
                                     QUrl(QStringLiteral("https://example.com/b.cr3"))}));
    ASSERT_EQ(scripted.library->requests.size(), 1u);
    EXPECT_EQ(scripted.library->requests[0].paths, QStringList{QStringLiteral("C:/Photos/a.cr3")});
    EXPECT_FALSE(scripted.library->requests[0].include_subfolders);

    EXPECT_FALSE(controller.addFiles({QUrl(QStringLiteral("https://example.com/b.cr3"))}));
    EXPECT_EQ(scripted.library->requests.size(), 1u);
    EXPECT_FALSE(controller.status_message().isEmpty());
    EXPECT_FALSE(controller.addFiles({}));
}

TEST_F(AppControllerTest, AddFolderHonoursIncludeSubfolders) {
    QSignalSpy spy(&controller, &AppController::includeSubfoldersChanged);
    controller.set_include_subfolders(true);
    controller.set_include_subfolders(true);
    EXPECT_EQ(spy.count(), 1);
    EXPECT_TRUE(controller.addFolder(QUrl::fromLocalFile(QStringLiteral("D:/Card/DCIM"))));
    ASSERT_EQ(scripted.library->requests.size(), 1u);
    EXPECT_EQ(scripted.library->requests[0].paths, QStringList{QStringLiteral("D:/Card/DCIM")});
    EXPECT_TRUE(scripted.library->requests[0].include_subfolders);
    EXPECT_FALSE(controller.addFolder(QUrl(QStringLiteral("ftp://host/photos"))));
}

TEST_F(AppControllerTest, DroppedFilesAndFoldersUseTheSubfolderOption) {
    controller.set_include_subfolders(true);
    EXPECT_TRUE(controller.addDropped({QUrl::fromLocalFile(QStringLiteral("D:/Shoot")),
                                       QUrl::fromLocalFile(QStringLiteral("D:/Other/a.cr3")),
                                       QUrl(QStringLiteral("https://example.com/b.cr3"))}));
    ASSERT_EQ(scripted.library->requests.size(), 1u);
    EXPECT_EQ(scripted.library->requests[0].paths,
              (QStringList{QStringLiteral("D:/Shoot"), QStringLiteral("D:/Other/a.cr3")}));
    EXPECT_TRUE(scripted.library->requests[0].include_subfolders);
    EXPECT_FALSE(controller.addDropped({QUrl(QStringLiteral("https://example.com/b.cr3"))}));
    EXPECT_EQ(scripted.library->requests.size(), 1u);
}

TEST_F(AppControllerTest, ImportPathsMakesPathsAbsoluteAndSkipsEmptyOnes) {
    EXPECT_FALSE(controller.import_paths({QString(), QStringLiteral("  ")}, false));
    EXPECT_TRUE(scripted.library->requests.empty());
    EXPECT_TRUE(controller.import_paths({QStringLiteral("relative/photo.cr3")}, true));
    ASSERT_EQ(scripted.library->requests.size(), 1u);
    EXPECT_EQ(scripted.library->requests[0].paths.front(),
              QDir::cleanPath(QDir::current().absoluteFilePath(QStringLiteral("relative/photo.cr3"))));
}

TEST_F(AppControllerTest, ReportsImportProgressAndOutcome) {
    QSignalSpy importing(&controller, &AppController::importingChanged);
    scripted.library->begin({QStringLiteral("D:/Photos")});
    EXPECT_TRUE(controller.importing());
    EXPECT_TRUE(controller.busy());
    EXPECT_EQ(controller.status_message(), QStringLiteral("Importing from D:\\Photos…"));

    add_two_photos();
    ImportSummary summary;
    summary.sources = {QStringLiteral("D:/Photos")};
    summary.added = 2;
    summary.already_in_library = 1;
    summary.ignored = 3;
    summary.failed = 1;
    summary.errors = {QStringLiteral("X.CR3 could not be read.")};
    scripted.library->finish(summary);
    EXPECT_FALSE(controller.importing());
    EXPECT_EQ(importing.count(), 2);
    EXPECT_EQ(controller.status_message(),
              QStringLiteral("Added 2 photos from D:\\Photos · 1 already in the library · 3 other files ignored · 1 "
                             "could not be read: X.CR3 could not be read."));

    summary.added = 0;
    summary.already_in_library = 0;
    summary.ignored = 0;
    summary.failed = 0;
    summary.sources = {QStringLiteral("D:/A"), QStringLiteral("E:/B"), QStringLiteral("e:/c.cr3")};
    scripted.library->finish(summary);
    EXPECT_EQ(controller.status_message(), QStringLiteral("No new photos in 2 locations"));

    // Missing photos found again (a card that came back) are named, alone or after the new ones.
    summary.restored = 3;
    scripted.library->finish(summary);
    EXPECT_EQ(controller.status_message(), QStringLiteral("Found 3 missing photos again in 2 locations"));
    summary.added = 1;
    summary.sources = {QStringLiteral("D:/Photos")};
    scripted.library->finish(summary);
    EXPECT_EQ(controller.status_message(),
              QStringLiteral("Added 1 photo from D:\\Photos · 3 missing photos found again"));
    summary.added = 0;
    summary.restored = 0;
    summary.sources = {QStringLiteral("D:/A"), QStringLiteral("E:/B"), QStringLiteral("e:/c.cr3")};

    // Files selected or dropped together are described by their folder, not counted as locations.
    summary.sources = {QStringLiteral("D:/Card/DCIM/IMG_1.CR3"), QStringLiteral("D:/Card/DCIM/IMG_2.CR3"),
                       QStringLiteral("D:/Card/DCIM/IMG_2.JPG")};
    summary.added = 2;
    summary.ignored = 1;
    scripted.library->finish(summary);
    EXPECT_EQ(controller.status_message(),
              QStringLiteral("Added 2 photos from D:\\Card\\DCIM · 1 other file ignored"));

    // Files that changed on disk and were read again.
    summary.added = 0;
    summary.ignored = 0;
    summary.updated = 1;
    summary.sources = {QStringLiteral("D:/Photos")};
    scripted.library->finish(summary);
    EXPECT_EQ(controller.status_message(), QStringLiteral("Updated 1 changed photo in D:\\Photos"));
    summary.added = 3;
    summary.updated = 2;
    scripted.library->finish(summary);
    EXPECT_EQ(controller.status_message(), QStringLiteral("Added 3 photos from D:\\Photos · 2 changed photos updated"));
}

TEST_F(AppControllerTest, TheFirstImportedPhotoBecomesCurrent) {
    QSignalSpy index(&controller, &AppController::currentIndexChanged);
    EXPECT_EQ(controller.current_index(), -1);
    add_two_photos();
    EXPECT_EQ(controller.current_index(), 0);
    EXPECT_EQ(index.count(), 1);
    scripted.library->add({make_item(3, QStringLiteral("C.CR3"))});
    EXPECT_EQ(controller.current_index(), 0);  // an existing selection is kept
}

TEST_F(AppControllerTest, OpeningAPhotoShowsEditAndEscReturns) {
    add_two_photos();
    QSignalSpy view(&controller, &AppController::currentViewChanged);
    EXPECT_TRUE(controller.openItem(1));
    EXPECT_EQ(controller.current_view(), AppController::Edit);
    EXPECT_EQ(controller.current_index(), 1);
    EXPECT_EQ(controller.current()->file_name(), QStringLiteral("B.ARW"));
    EXPECT_EQ(controller.window_title(), QStringLiteral("B.ARW — Lucent"));
    EXPECT_EQ(view.count(), 1);

    controller.goBack();
    EXPECT_EQ(controller.current_view(), AppController::Library);
    EXPECT_EQ(controller.window_title(), QStringLiteral("Lucent"));
    controller.goBack();  // Library has nowhere to go back to
    EXPECT_EQ(controller.current_view(), AppController::Library);

    EXPECT_FALSE(controller.openItem(9));
    EXPECT_FALSE(controller.openItem(-1));
    EXPECT_EQ(controller.current_view(), AppController::Library);
}

TEST_F(AppControllerTest, ExportsReturnsToTheViewItWasOpenedFrom) {
    add_two_photos();
    controller.showView(AppController::Edit);
    controller.showView(AppController::Exports);
    EXPECT_EQ(controller.window_title(), QStringLiteral("Exports — Lucent"));
    controller.goBack();
    EXPECT_EQ(controller.current_view(), AppController::Edit);

    controller.showView(AppController::Library);
    controller.showView(AppController::Exports);
    controller.goBack();
    EXPECT_EQ(controller.current_view(), AppController::Library);

    controller.showView(static_cast<AppController::View>(7));  // from QML, an invalid value
    EXPECT_EQ(controller.current_view(), AppController::Library);
}

TEST_F(AppControllerTest, EditWithoutASelectionSelectsTheFirstPhoto) {
    add_two_photos();
    controller.set_current_index(-1);
    controller.showView(AppController::Edit);
    EXPECT_EQ(controller.current_index(), 0);
}

TEST_F(AppControllerTest, SelectRelativeStopsAtTheEnds) {
    EXPECT_FALSE(controller.selectRelative(1));  // empty library
    add_two_photos();
    EXPECT_TRUE(controller.selectRelative(1));
    EXPECT_EQ(controller.current_index(), 1);
    EXPECT_FALSE(controller.selectRelative(1));
    EXPECT_TRUE(controller.selectRelative(-5));
    EXPECT_EQ(controller.current_index(), 0);
    EXPECT_FALSE(controller.selectRelative(0));
}

TEST_F(AppControllerTest, ExportCurrentQueuesTheCurrentPhotoAndRemembersTheFolder) {
    EXPECT_FALSE(controller.exportCurrent(QUrl::fromLocalFile(QStringLiteral("C:/Out"))));  // nothing open
    EXPECT_TRUE(scripted.exports->requests.empty());

    add_two_photos();
    controller.openItem(1);
    QSignalSpy folder(&controller, &AppController::lastOutputFolderChanged);
    EXPECT_TRUE(controller.exportCurrent(QUrl::fromLocalFile(QStringLiteral("C:/Out"))));
    ASSERT_EQ(scripted.exports->requests.size(), 1u);
    const ExportRequest& request = scripted.exports->requests[0];
    EXPECT_EQ(request.items, QList<ItemId>{2});
    EXPECT_EQ(request.output_folder, QStringLiteral("C:/Out"));
    EXPECT_EQ(request.options, ExportOptions{});
    EXPECT_EQ(controller.last_output_folder(), QUrl::fromLocalFile(QStringLiteral("C:/Out")));
    EXPECT_EQ(folder.count(), 1);

    EXPECT_FALSE(controller.exportCurrent(QUrl(QStringLiteral("https://example.com/out"))));
    EXPECT_FALSE(controller.export_items({}, QStringLiteral("C:/Out")));
    EXPECT_FALSE(controller.export_items({1}, QString()));
    EXPECT_EQ(scripted.exports->requests.size(), 1u);
}

TEST_F(AppControllerTest, OpenOutputFolderUsesTheDesktopOpener) {
    EXPECT_FALSE(controller.openOutputFolder(-1));  // no export yet
    EXPECT_FALSE(controller.openOutputFolder(0));

    ExportJob job;
    job.id = 5;
    job.item = 1;
    job.source_name = QStringLiteral("A.CR3");
    job.output_folder = QStringLiteral("C:/Out/Job");
    scripted.exports->add(job);
    EXPECT_TRUE(controller.openOutputFolder(0));
    ASSERT_EQ(opened.size(), 1u);
    EXPECT_EQ(opened[0], QUrl::fromLocalFile(QStringLiteral("C:/Out/Job")));

    add_two_photos();
    controller.openItem(0);
    controller.exportCurrent(QUrl::fromLocalFile(QStringLiteral("C:/Out/Last")));
    EXPECT_TRUE(controller.openOutputFolder(-1));
    EXPECT_EQ(opened.back(), QUrl::fromLocalFile(QStringLiteral("C:/Out/Last")));

    opener_result = false;
    EXPECT_FALSE(controller.openOutputFolder(0));
    EXPECT_EQ(controller.status_message(), QStringLiteral("Could not open C:\\Out\\Job"));
}

TEST_F(AppControllerTest, CancelExportForwardsTheJobOfTheRow) {
    ExportJob job;
    job.id = 11;
    job.output_folder = QStringLiteral("C:/Out");
    scripted.exports->add(job);
    controller.cancelExport(0);
    controller.cancelExport(3);  // no such row
    EXPECT_EQ(scripted.exports->cancelled, std::vector<JobId>{11});
}

TEST_F(AppControllerTest, PhotoNameFiltersListTheSupportedSuffixes) {
    EXPECT_EQ(controller.photo_name_filters(),
              (QStringList{QStringLiteral("RAW photos (*.arw *.cr3)"), QStringLiteral("All files (*)")}));
}

TEST_F(AppControllerTest, BusyFollowsTheServices) {
    QSignalSpy busy(&controller, &AppController::busyChanged);
    EXPECT_FALSE(controller.busy());
    scripted.library->set_busy(true);
    EXPECT_TRUE(controller.busy());
    EXPECT_EQ(busy.count(), 1);
    scripted.library->set_busy(false);
    scripted.exports->busy = true;
    EXPECT_TRUE(controller.busy());
}

TEST_F(AppControllerTest, ClippingOverlayIsAToggle) {
    QSignalSpy spy(&controller, &AppController::clippingOverlayChanged);
    controller.set_clipping_overlay(true);
    controller.set_clipping_overlay(true);
    EXPECT_TRUE(controller.clipping_overlay());
    EXPECT_EQ(spy.count(), 1);
}

// ---- W4: the Import dialog, copies, relinking, picks, backups, background activity -------------

TEST_F(AppControllerTest, RequestedImportsWaitForTheDialogAndCarryItsOptions) {
    QSignalSpy pending(&controller, &AppController::pendingImportChanged);
    EXPECT_FALSE(controller.requestImport({QUrl(QStringLiteral("https://example.com/a.cr3"))}));
    EXPECT_TRUE(controller.pending_import().isEmpty());
    ASSERT_TRUE(controller.requestImport({QUrl::fromLocalFile(QStringLiteral("D:/Card/DCIM"))}));
    EXPECT_EQ(controller.pending_import(), QStringList{QStringLiteral("D:/Card/DCIM")});
    EXPECT_EQ(pending.count(), 1);
    EXPECT_TRUE(scripted.library->requests.empty());  // nothing happens before the confirmation

    // Copying needs a destination; the pending import is kept until there is one.
    controller.set_include_subfolders(true);
    controller.set_copy_into_library(true);
    EXPECT_FALSE(controller.confirmImport());
    EXPECT_EQ(controller.pending_import().size(), 1);
    EXPECT_FALSE(controller.status_message().isEmpty());
    controller.set_copy_destination(QUrl::fromLocalFile(QStringLiteral("E:/Pictures")));
    ASSERT_TRUE(controller.confirmImport());
    EXPECT_TRUE(controller.pending_import().isEmpty());
    ASSERT_EQ(scripted.library->requests.size(), 1u);
    EXPECT_TRUE(scripted.library->requests[0].include_subfolders);
    EXPECT_EQ(scripted.library->requests[0].copy_to, QStringLiteral("E:/Pictures"));
    EXPECT_FALSE(controller.confirmImport());  // nothing pending any more

    // Cancelling drops the selection.
    ASSERT_TRUE(controller.requestImport({QUrl::fromLocalFile(QStringLiteral("D:/Other"))}));
    controller.cancelImport();
    EXPECT_TRUE(controller.pending_import().isEmpty());
    EXPECT_EQ(scripted.library->requests.size(), 1u);
}

TEST_F(AppControllerTest, StartBeginsTheLibrarysBackgroundUpkeep) {
    EXPECT_EQ(scripted.library->starts, 0);
    controller.start();
    EXPECT_EQ(scripted.library->starts, 1);
}

TEST_F(AppControllerTest, RelinkingAsksTheServiceAndReportsTheOutcome) {
    add_two_photos();
    EXPECT_TRUE(controller.can_relink());
    EXPECT_FALSE(controller.relinkMissing(QUrl(QStringLiteral("https://example.com/"))));
    ASSERT_TRUE(controller.relinkMissing(QUrl::fromLocalFile(QStringLiteral("F:/Moved"))));
    ASSERT_EQ(scripted.library->relinks.size(), 1u);
    EXPECT_EQ(scripted.library->relinks[0].first, QStringLiteral("F:/Moved"));
    EXPECT_TRUE(scripted.library->relinks[0].second.isEmpty());  // every missing photo

    controller.set_current_index(1);
    ASSERT_TRUE(controller.relinkCurrent(QUrl::fromLocalFile(QStringLiteral("F:/Moved"))));
    EXPECT_EQ(scripted.library->relinks[1].second, QList<ItemId>{2});

    RelinkSummary summary;
    summary.folder = QStringLiteral("F:\\Moved");
    summary.missing = 3;
    summary.relinked = 2;
    summary.unmatched = 1;
    summary.unverifiable = 1;
    scripted.library->finish_relink(summary);
    EXPECT_TRUE(controller.status_message().startsWith(QStringLiteral("Relinked 2 photos from F:\\Moved")))
        << controller.status_message().toStdString();
    EXPECT_TRUE(controller.status_message().contains(QStringLiteral("1 still missing")));
    EXPECT_TRUE(controller.status_message().contains(QStringLiteral("never verified")));

    summary = RelinkSummary{};
    summary.error = QStringLiteral("The folder was not found.");
    scripted.library->finish_relink(summary);
    EXPECT_EQ(controller.status_message(), QStringLiteral("Could not relink: The folder was not found."));
}

TEST_F(AppControllerTest, PicksAreSetOnTheCurrentPhoto) {
    controller.setCurrentPick(QStringLiteral("picked"));  // no current photo: nothing
    EXPECT_TRUE(scripted.library->picks.empty());
    add_two_photos();
    controller.set_current_index(0);
    controller.setCurrentPick(QStringLiteral("rejected"));
    controller.setCurrentPick(QStringLiteral("none"));
    ASSERT_EQ(scripted.library->picks.size(), 2u);
    EXPECT_EQ(scripted.library->picks[0], (std::pair<ItemId, PickState>{1, PickState::Rejected}));
    EXPECT_EQ(scripted.library->picks[1], (std::pair<ItemId, PickState>{1, PickState::None}));
}

TEST_F(AppControllerTest, CatalogBackupsAreRequestedAndReported) {
    scripted.library->backup_location = QStringLiteral("C:\\Lucent\\backups");
    EXPECT_TRUE(controller.can_back_up());
    controller.backUpCatalog();
    EXPECT_EQ(scripted.library->backups, 1);
    scripted.library->finish_backup(BackupResult{true, QStringLiteral("C:\\Lucent\\backups\\x.bak"), {}});
    EXPECT_EQ(controller.status_message(), QStringLiteral("Catalog backed up to C:\\Lucent\\backups\\x.bak"));
    scripted.library->finish_backup(
        BackupResult{false, {}, QStringLiteral("There is not enough free space.")});
    EXPECT_TRUE(controller.status_message().contains(QStringLiteral("not enough free space")));
    EXPECT_TRUE(controller.openBackupFolder());
    ASSERT_EQ(opened.size(), 1u);
    EXPECT_EQ(opened[0], QUrl::fromLocalFile(QStringLiteral("C:\\Lucent\\backups")));
}

TEST_F(AppControllerTest, BackgroundVerificationIsShownAsActivity) {
    QSignalSpy activity(&controller, &AppController::backgroundActivityChanged);
    scripted.library->verify(VerificationProgress{true, 0, 0});
    EXPECT_EQ(controller.background_activity(), QStringLiteral("Checking the library…"));
    scripted.library->verify(VerificationProgress{true, 12, 340});
    EXPECT_EQ(controller.background_activity(), QStringLiteral("Verifying photos… 12 of 340"));
    scripted.library->verify(VerificationProgress{false, 340, 340});
    EXPECT_TRUE(controller.background_activity().isEmpty());
    EXPECT_EQ(activity.count(), 3);
}

TEST_F(AppControllerTest, ImportSummariesNameCopiesAndRemovableMedia) {
    ImportSummary summary;
    summary.sources = {QStringLiteral("E:/DCIM")};
    summary.added = 3;
    summary.copied = 3;
    summary.copy_to = QStringLiteral("D:\\Pictures");
    scripted.library->finish(summary);
    EXPECT_TRUE(controller.status_message().startsWith(
        QStringLiteral("Copied 3 photos from E:\\DCIM into D:\\Pictures")))
        << controller.status_message().toStdString();

    summary = ImportSummary{};
    summary.sources = {QStringLiteral("E:/DCIM")};
    summary.added = 2;
    summary.removable = 2;
    summary.notes << QStringLiteral("2 photos are on removable media (E:\\).");
    scripted.library->finish(summary);
    EXPECT_TRUE(controller.status_message().contains(QStringLiteral("2 on removable media")));
    EXPECT_TRUE(
        controller.import_problems().contains(QStringLiteral("2 photos are on removable media (E:\\).")));
}

}  // namespace
}  // namespace lucent::ui
