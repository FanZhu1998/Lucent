// The QML module end to end: Main.qml loads with its views and models and without a single QML or
// Qt warning, imports fill the contact sheet, keyboard navigation works, previews load, export
// jobs are listed, and the screenshot automation writes a PNG.

#include <gtest/gtest.h>

#include <memory>

#include <QDir>
#include <QFile>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "app_controller.hpp"
#include "developer_automation.hpp"
#include "fakes/fake_services.hpp"
#include "shell.hpp"
#include "test_support.hpp"

namespace lucent::ui {
namespace {

constexpr int kWaitMs = 20000;
constexpr int kImageStatusReady = 1;  // QQuickImageBase::Ready

QString warnings_text(const QStringList& warnings) {
    return warnings.join(QStringLiteral("\n"));
}

class QmlShellTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(folder.isValid());
        controller = std::make_unique<AppController>(fakes::create_fake_services(fakes::FakeTimings::immediate()));
        controller->set_url_opener([this](const QUrl& url) {
            opened.push_back(url);
            return true;
        });
        engine = std::make_unique<QQmlApplicationEngine>();
        window = load_main_window(*engine, *controller);
        ASSERT_NE(window, nullptr);
        ASSERT_TRUE(QTest::qWaitForWindowExposed(window));
    }

    void TearDown() override {
        engine.reset();
        controller.reset();
    }

    template <typename T = QObject>
    T* child(const char* name) const {
        return window->findChild<T*>(QString::fromLatin1(name));
    }

    // Items named `name` in the visual item tree, which (unlike the QObject tree) also holds the
    // delegates of views; only visible ones when `visible_only`.
    QList<QQuickItem*> visual_items(const char* name, bool visible_only) const {
        QList<QQuickItem*> found;
        QList<QQuickItem*> pending{window->contentItem()};
        while (!pending.isEmpty()) {
            QQuickItem* item = pending.takeLast();
            if (item->objectName() == QLatin1String(name) && (!visible_only || item->isVisible())) {
                found << item;
            }
            pending << item->childItems();
        }
        return found;
    }

    // Writes `count` files whose simulated support status is not Unsupported (plus `unsupported`
    // files that are), so previews load without expected failures.
    void write_photos(int count, int unsupported = 0) {
        int written = 0;
        int written_unsupported = 0;
        for (int i = 0; i < 2000 && (written < count || written_unsupported < unsupported); ++i) {
            const QString name = QStringLiteral("IMG_%1.CR3").arg(i, 4, 10, QLatin1Char('0'));
            const bool is_unsupported =
                fakes::make_fake_item(1, folder.filePath(name), 0, {}).support == SupportStatus::Unsupported;
            if (is_unsupported ? written_unsupported >= unsupported : written >= count) {
                continue;
            }
            QFile file(folder.filePath(name));
            ASSERT_TRUE(file.open(QIODevice::WriteOnly));
            file.write("raw");
            (is_unsupported ? written_unsupported : written) += 1;
        }
    }

    // True once no service work is pending and no visible image is loading.
    bool settle() {
        return QTest::qWaitFor(
            [this] { return !controller->busy() && !DeveloperAutomation::has_loading_images(*window); }, kWaitMs);
    }

    void import_folder() {
        ASSERT_TRUE(controller->addFolder(QUrl::fromLocalFile(folder.path())));
        ASSERT_TRUE(QTest::qWaitFor([this] { return !controller->importing(); }, kWaitMs));
        ASSERT_TRUE(settle());
    }

    testing::MessageCapture capture;  // first: outlives the engine
    QTemporaryDir folder;
    std::unique_ptr<AppController> controller;
    std::unique_ptr<QQmlApplicationEngine> engine;
    QQuickWindow* window = nullptr;
    std::vector<QUrl> opened;
};

TEST_F(QmlShellTest, LoadsTheWindowViewsAndModelsWithoutWarnings) {
    EXPECT_EQ(window->objectName(), QStringLiteral("mainWindow"));
    EXPECT_EQ(window->title(), QStringLiteral("Lucent"));
    EXPECT_GE(window->minimumWidth(), 960);
    EXPECT_GE(window->minimumHeight(), 600);
    EXPECT_GE(window->width(), window->minimumWidth());

    for (const char* name : {"libraryView", "editView", "exportsView", "contactSheet", "emptyState", "filmstrip",
                             "previewViewer", "metadataPanel", "histogram", "exportJobs", "exportSummary",
                             "statusMessage", "libraryTab", "editTab", "exportsTab", "addPhotosButton",
                             "addFolderButton", "includeSubfoldersOption", "exportButton", "fitButton",
                             "actualSizeButton", "clippingToggle", "openLastOutputFolder"}) {
        EXPECT_NE(child(name), nullptr) << name;
    }
    EXPECT_EQ(child("contactSheet")->property("model").value<QObject*>(), controller->library());
    EXPECT_EQ(child("filmstrip")->property("model").value<QObject*>(), controller->library());
    EXPECT_EQ(child("exportJobs")->property("model").value<QObject*>(), controller->exports());

    EXPECT_TRUE(child<QQuickItem>("libraryView")->isVisible());
    EXPECT_FALSE(child<QQuickItem>("editView")->isVisible());
    EXPECT_TRUE(child<QQuickItem>("emptyState")->isVisible());
    EXPECT_FALSE(child<QQuickItem>("contactSheet")->isVisible());
    EXPECT_FALSE(child("editTab")->property("enabled").toBool());  // nothing to edit yet

    QTest::qWait(100);
    EXPECT_TRUE(capture.warnings().isEmpty()) << warnings_text(capture.warnings()).toStdString();
}

TEST_F(QmlShellTest, ImportFillsTheContactSheet) {
    write_photos(3, 1);
    QDir().mkpath(folder.filePath(QStringLiteral("nested")));
    QFile nested(folder.filePath(QStringLiteral("nested/deep.ARW")));
    ASSERT_TRUE(nested.open(QIODevice::WriteOnly));
    nested.close();

    controller->set_include_subfolders(true);
    EXPECT_TRUE(child("includeSubfoldersOption")->property("checked").toBool());
    import_folder();

    auto* grid = child<QQuickItem>("contactSheet");
    EXPECT_EQ(grid->property("count").toInt(), 5);
    EXPECT_TRUE(grid->isVisible());
    EXPECT_FALSE(child<QQuickItem>("emptyState")->isVisible());
    EXPECT_TRUE(child("editTab")->property("enabled").toBool());
    EXPECT_TRUE(child("statusMessage")->property("text").toString().startsWith(QStringLiteral("Added 5 photos")));
    // The keyboard focus moved from the empty state to the contact sheet.
    EXPECT_TRUE(grid->hasActiveFocus());
    EXPECT_TRUE(capture.warnings().isEmpty()) << warnings_text(capture.warnings()).toStdString();
}

TEST_F(QmlShellTest, KeyboardMovesThroughLibraryEditAndExports) {
    write_photos(3);
    import_folder();
    window->requestActivate();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window));
    auto* grid = child<QQuickItem>("contactSheet");
    ASSERT_TRUE(QTest::qWaitFor([grid] { return grid->hasActiveFocus(); }, kWaitMs));
    EXPECT_EQ(controller->current_index(), 0);

    QTest::keyClick(window, Qt::Key_Right);
    EXPECT_EQ(controller->current_index(), 1);
    QTest::keyClick(window, Qt::Key_Return);
    EXPECT_EQ(controller->current_view(), AppController::Edit);
    auto* viewer = child<QQuickItem>("previewViewer");
    ASSERT_TRUE(QTest::qWaitFor([viewer] { return viewer->hasActiveFocus(); }, kWaitMs));
    EXPECT_TRUE(child<QQuickItem>("editView")->isVisible());

    QTest::keyClick(window, Qt::Key_Right);
    EXPECT_EQ(controller->current_index(), 2);
    QTest::keyClick(window, Qt::Key_Right);  // already the last photo
    EXPECT_EQ(controller->current_index(), 2);
    QTest::keyClick(window, Qt::Key_Home);
    EXPECT_EQ(controller->current_index(), 0);

    QTest::keyClick(window, Qt::Key_Escape);
    EXPECT_EQ(controller->current_view(), AppController::Library);
    ASSERT_TRUE(QTest::qWaitFor([grid] { return grid->hasActiveFocus(); }, kWaitMs));
    EXPECT_EQ(grid->property("currentIndex").toInt(), 0);

    QTest::keyClick(window, Qt::Key_3, Qt::ControlModifier | Qt::AltModifier);
    EXPECT_EQ(controller->current_view(), AppController::Exports);
    QTest::keyClick(window, Qt::Key_Escape);
    EXPECT_EQ(controller->current_view(), AppController::Library);

    // Tab reaches the contact sheet again from the toolbar (Add folder, Add photos, then the grid).
    child<QQuickItem>("toolbarAddFolder")->forceActiveFocus(Qt::TabFocusReason);
    for (int i = 0; i < 3 && !grid->hasActiveFocus(); ++i) {
        QTest::keyClick(window, Qt::Key_Tab);
    }
    EXPECT_TRUE(grid->hasActiveFocus());
    EXPECT_TRUE(settle());
    EXPECT_TRUE(capture.warnings().isEmpty()) << warnings_text(capture.warnings()).toStdString();
}

TEST_F(QmlShellTest, OpeningAPhotoLoadsItsPreviewAtFitAndFullSize) {
    write_photos(2);
    import_folder();
    ASSERT_TRUE(controller->openItem(1));
    auto* viewer = child<QQuickItem>("previewViewer");
    ASSERT_TRUE(QTest::qWaitFor([viewer] { return viewer->property("status").toInt() == kImageStatusReady; },
                                kWaitMs));
    EXPECT_EQ(child("editTitle")->property("text").toString(), controller->current()->file_name());
    EXPECT_TRUE(viewer->property("fitMode").toBool());
    const QSize fit_request = viewer->property("requestSize").toSize();
    EXPECT_LT(fit_request.width(), controller->current()->pixel_width());

    QMetaObject::invokeMethod(viewer, "setFit", Q_ARG(QVariant, false), Q_ARG(QVariant, QVariant()),
                              Q_ARG(QVariant, QVariant()));
    EXPECT_FALSE(viewer->property("fitMode").toBool());
    EXPECT_EQ(viewer->property("requestSize").toSize(),
              QSize(controller->current()->pixel_width(), controller->current()->pixel_height()));
    ASSERT_TRUE(settle());
    EXPECT_EQ(viewer->property("status").toInt(), kImageStatusReady);
    EXPECT_TRUE(capture.warnings().isEmpty()) << warnings_text(capture.warnings()).toStdString();
}

TEST_F(QmlShellTest, CameraPreviewsAreLabelledEverywhereAndCardsShowProcessing) {
    // The simulated services render every photo except the Unsupported one in the background:
    // two Lucent thumbnails (processed) and one camera preview.
    write_photos(2, 1);
    import_folder();
    const auto visible_count = [this](const char* name) { return visual_items(name, true).size(); };
    EXPECT_EQ(visible_count("cameraPreviewLabel"), 1);
    EXPECT_EQ(visible_count("processingBadge"), 2);  // blueprint section 2: processing status

    ASSERT_TRUE(controller->openItem(0));
    ASSERT_TRUE(settle());
    // The filmstrip labels the camera preview too (blueprint section 2, ADR 0007).
    EXPECT_EQ(visual_items("filmstripCameraTag", false).size(), 3);
    EXPECT_EQ(visible_count("filmstripCameraTag"), 1);
    EXPECT_FALSE(child<QQuickItem>("incompleteRenderBanner")->isVisible());
    EXPECT_TRUE(capture.warnings().isEmpty()) << warnings_text(capture.warnings()).toStdString();
}

TEST_F(QmlShellTest, AnIncompleteRenderIsMarkedOnTheImage) {
    write_photos(1);
    import_folder();
    LibraryItem item = *controller->library()->item_at(0);
    auto info = std::make_shared<RenderInfo>(item.render ? *item.render : RenderInfo{});
    info->incomplete = true;
    item.render = info;
    item.processing = ProcessingStatus::Processed;
    emit controller->library_service().item_changed(item);
    ASSERT_TRUE(controller->openItem(0));
    auto* viewer = child<QQuickItem>("previewViewer");
    ASSERT_TRUE(QTest::qWaitFor([viewer] { return viewer->property("status").toInt() == kImageStatusReady; },
                                kWaitMs));
    EXPECT_TRUE(child<QQuickItem>("incompleteRenderBanner")->isVisible());
    EXPECT_EQ(controller->current()->processing_label(), QStringLiteral("Incomplete render"));
    EXPECT_TRUE(capture.warnings().isEmpty()) << warnings_text(capture.warnings()).toStdString();
}

TEST_F(QmlShellTest, ExportsViewListsTheJobs) {
    write_photos(2, 1);
    import_folder();
    QList<ItemId> items;
    for (int row = 0; row < controller->library()->count(); ++row) {
        items << controller->library()->item_at(row)->id;
    }
    ASSERT_TRUE(controller->export_items(items, folder.path()));
    controller->showView(AppController::Exports);
    ASSERT_TRUE(settle());
    EXPECT_EQ(child("exportJobs")->property("count").toInt(), 3);
    EXPECT_EQ(child("exportSummary")->property("text").toString(), QStringLiteral("2 exported · 1 failed"));
    EXPECT_TRUE(child("openLastOutputFolder")->property("enabled").toBool());
    EXPECT_TRUE(controller->openOutputFolder(-1));
    ASSERT_EQ(opened.size(), 1u);
    EXPECT_EQ(opened[0], QUrl::fromLocalFile(QDir::cleanPath(folder.path())));
    EXPECT_TRUE(capture.warnings().isEmpty()) << warnings_text(capture.warnings()).toStdString();
}

TEST_F(QmlShellTest, ScreenshotAutomationWritesThePngAndRequestsQuit) {
    write_photos(2);
    DeveloperOptions options;
    options.imports = {folder.path()};
    options.open_first = true;
    options.screenshot_path = folder.filePath(QStringLiteral("shots/edit.png"));
    options.quit_after_screenshot = true;
    DeveloperAutomation automation(options, *controller, *window);
    QSignalSpy quit(&automation, &DeveloperAutomation::quitRequested);
    automation.start();
    ASSERT_TRUE(quit.wait(kWaitMs));
    EXPECT_EQ(quit.at(0).at(0).toInt(), 0);
    EXPECT_EQ(controller->current_view(), AppController::Edit);

    const QImage shot(options.screenshot_path);
    ASSERT_FALSE(shot.isNull());
    EXPECT_EQ(shot.size(), window->size() * window->devicePixelRatio());
    EXPECT_TRUE(capture.warnings().isEmpty()) << warnings_text(capture.warnings()).toStdString();
}

// W4: chosen photos wait in the Import dialog (Include subfolders, Copy into library folder), and
// the contact sheet shows the file badges of the model's items.
TEST_F(QmlShellTest, TheImportDialogConfirmsAnImportAndCardsShowSourceBadges) {
    write_photos(12);
    auto* dialog = child("importDialog");
    ASSERT_NE(dialog, nullptr);
    EXPECT_FALSE(dialog->property("opened").toBool());
    ASSERT_TRUE(controller->requestImport({QUrl::fromLocalFile(folder.path())}));
    ASSERT_TRUE(QTest::qWaitFor([dialog] { return dialog->property("opened").toBool(); }, kWaitMs));
    EXPECT_FALSE(controller->importing());  // nothing starts before the confirmation

    auto* confirm = child("importConfirm");
    ASSERT_NE(confirm, nullptr);
    controller->set_copy_into_library(true);  // without a destination the import cannot start
    EXPECT_FALSE(confirm->property("enabled").toBool());
    controller->set_copy_into_library(false);
    EXPECT_TRUE(confirm->property("enabled").toBool());
    ASSERT_TRUE(QMetaObject::invokeMethod(confirm, "clicked"));
    ASSERT_TRUE(QTest::qWaitFor([dialog] { return !dialog->property("visible").toBool(); }, kWaitMs));
    EXPECT_TRUE(controller->pending_import().isEmpty());
    ASSERT_TRUE(QTest::qWaitFor([this] { return controller->library()->count() == 12; }, kWaitMs));
    ASSERT_TRUE(settle());

    // A simulated content duplicate shows its badge on its card.
    int duplicate_row = -1;
    for (int row = 0; row < controller->library()->count(); ++row) {
        if (has_flag(controller->library()->item_at(row)->flags, ItemFlag::DuplicateContent)) {
            duplicate_row = row;
            break;
        }
    }
    ASSERT_GE(duplicate_row, 0) << "the simulated library has no content duplicate";
    {
        controller->set_current_index(duplicate_row);  // the contact sheet scrolls to it
        EXPECT_TRUE(
            QTest::qWaitFor([this] { return !visual_items("badge_duplicate", true).isEmpty(); }, kWaitMs));
    }
    EXPECT_TRUE(capture.warnings().isEmpty()) << warnings_text(capture.warnings()).toStdString();
}

}  // namespace
}  // namespace lucent::ui
