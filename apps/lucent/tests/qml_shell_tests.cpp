// The QML module end to end: Main.qml loads with its views and models and without a single QML or
// Qt warning, imports fill the contact sheet, keyboard navigation works, previews load, export
// jobs are listed, and the screenshot automation writes a PNG.

#include <gtest/gtest.h>

#include <memory>

#include <QDir>
#include <QFile>
#include <QImage>
#include <QKeyEvent>
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

// A session without batches (the library is not saved) lists its in-process export jobs.
TEST_F(QmlShellTest, ExportsViewListsTheJobsWithoutBatches) {
    engine.reset();
    controller = std::make_unique<AppController>(
        fakes::create_fake_services(fakes::FakeTimings::immediate(), /*batches=*/false));
    controller->set_url_opener([this](const QUrl& url) {
        opened.push_back(url);
        return true;
    });
    engine = std::make_unique<QQmlApplicationEngine>();
    window = load_main_window(*engine, *controller);
    ASSERT_NE(window, nullptr);
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
    EXPECT_TRUE(child<QQuickItem>("exportJobs")->isVisible());
    EXPECT_FALSE(child<QQuickItem>("batchList")->isVisible());
    EXPECT_TRUE(child<QQuickItem>("batchesUnavailable")->isVisible());
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

// W6: the Edit view's adjustments edit the photo through its edit session: a key held on a slider
// and a mouse drag are one revision each, the history, undo and before/after work from the keyboard,
// copy and paste go through the category dialog and report the outcome, all without a warning.
TEST_F(QmlShellTest, TheEditPanelMakesOneRevisionPerGestureAndComparesBeforeAndAfter) {
    write_photos(1);
    import_folder();
    window->requestActivate();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window));
    ASSERT_TRUE(controller->openItem(0));
    auto* viewer = child<QQuickItem>("previewViewer");
    ASSERT_TRUE(QTest::qWaitFor([viewer] { return viewer->property("status").toInt() == kImageStatusReady; },
                                kWaitMs));
    ASSERT_TRUE(settle());
    auto* edits = dynamic_cast<fakes::FakeEditService*>(&controller->edit_service());
    ASSERT_NE(edits, nullptr);
    const ItemId item = controller->current()->item_id();
    EditSession& edit = *controller->edit();
    EXPECT_TRUE(edit.active());
    EXPECT_TRUE(edit.can_edit());
    EXPECT_FALSE(edit.frame_source().isEmpty());
    for (const char* name : {"adjustPanel", "toneCurvePlot", "historyList", "compareToggle", "splitToggle",
                             "undoToolButton", "redoToolButton"}) {
        EXPECT_NE(child(name), nullptr) << name;
    }
    for (const char* name : {"slider_exposure", "slider_contrast", "slider_highlights", "slider_shadows",
                             "slider_saturation", "slider_vibrance", "slider_temperature", "slider_tint",
                             "field_exposure", "lock_exposure", "autoButton"}) {
        EXPECT_FALSE(visual_items(name, true).isEmpty()) << name;
    }
    EXPECT_TRUE(visual_items("autoButton", true).front()->isEnabled());  // W8
    EXPECT_FALSE(child<QQuickItem>("autoSettingsPanel")->isVisible());   // nothing automatic yet
    EXPECT_FALSE(child<QQuickItem>("readOnlyBanner")->isVisible());
    QQuickItem* slider = visual_items("slider_exposure", true).front();

    // Arrow key held down on the exposure slider: one press, three repeats, one release.
    slider->forceActiveFocus(Qt::TabFocusReason);
    ASSERT_TRUE(slider->hasActiveFocus());
    const auto key = [this](QEvent::Type type, bool repeat) {
        QKeyEvent event(type, Qt::Key_Right, Qt::NoModifier, QString(), repeat);
        QCoreApplication::sendEvent(window, &event);
    };
    key(QEvent::KeyPress, false);
    for (int i = 0; i < 3; ++i) {
        key(QEvent::KeyPress, true);
    }
    key(QEvent::KeyRelease, false);
    ASSERT_TRUE(settle());
    EXPECT_EQ(edits->revision_count(item), 1);
    EXPECT_NEAR(edit.value(QStringLiteral("exposure")), 0.2, 1e-9);
    // The slider runs in display units: assistive technology reads the value the field shows.
    EXPECT_NEAR(slider->property("value").toDouble(), 0.2, 1e-9);
    EXPECT_DOUBLE_EQ(slider->property("to").toDouble(), 5.0);
    EXPECT_TRUE(edit.can_undo());
    EXPECT_EQ(edit.history()->count(), 2);  // the revision and "As shot"

    // A mouse drag on the contrast slider: one more revision.
    QQuickItem* contrast = visual_items("slider_contrast", true).front();
    auto* handle = contrast->property("handle").value<QQuickItem*>();
    ASSERT_NE(handle, nullptr);
    const QPoint from = handle->mapToScene(QPointF(handle->width() / 2, handle->height() / 2)).toPoint();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from);
    for (int step = 1; step <= 8; ++step) {
        QTest::mouseMove(window, from + QPoint(step * 6, 0));
    }
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, from + QPoint(48, 0));
    ASSERT_TRUE(settle());
    EXPECT_EQ(edits->revision_count(item), 2);
    EXPECT_GT(edit.value(QStringLiteral("contrast")), 0.0);

    // Before/after from the keyboard (\), then side by side.
    viewer->forceActiveFocus(Qt::OtherFocusReason);
    QTest::keyClick(window, Qt::Key_Backslash);
    EXPECT_TRUE(edit.compare());
    ASSERT_TRUE(QTest::qWaitFor([viewer] { return viewer->property("showingBefore").toBool(); }, kWaitMs));
    EXPECT_TRUE(child<QQuickItem>("beforeLabel")->isVisible());
    QTest::keyClick(window, Qt::Key_Backslash);
    EXPECT_FALSE(edit.compare());
    edit.set_split(true);
    ASSERT_TRUE(QTest::qWaitFor([this] { return child<QQuickItem>("splitDivider")->isVisible(); }, kWaitMs));
    // The divider works from the keyboard too.
    auto* divider = child<QQuickItem>("splitDivider");
    const double split_at = viewer->property("splitPosition").toDouble();
    divider->forceActiveFocus(Qt::TabFocusReason);
    ASSERT_TRUE(divider->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Left);
    EXPECT_NEAR(viewer->property("splitPosition").toDouble(), split_at - 0.02, 1e-9);
    QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    EXPECT_NEAR(viewer->property("splitPosition").toDouble(), split_at + 0.08, 1e-9);
    edit.set_split(false);

    // The clipping overlay (J) comes from the frames.
    QTest::keyClick(window, Qt::Key_J);
    EXPECT_TRUE(controller->clipping_overlay());
    ASSERT_TRUE(QTest::qWaitFor([&edit] { return !edit.clipping_source().isEmpty(); }, kWaitMs));
    QTest::keyClick(window, Qt::Key_J);

    // Undo (Ctrl+Z) and redo (Ctrl+Y).
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    ASSERT_TRUE(settle());
    EXPECT_DOUBLE_EQ(edit.value(QStringLiteral("contrast")), 0.0);
    EXPECT_TRUE(edit.can_redo());
    QTest::keyClick(window, Qt::Key_Y, Qt::ControlModifier);
    ASSERT_TRUE(settle());
    EXPECT_GT(edit.value(QStringLiteral("contrast")), 0.0);

    // Copy settings (Ctrl+Shift+C opens the category dialog), then paste (Ctrl+Shift+V).
    auto* dialog = child("copySettingsDialog");
    ASSERT_NE(dialog, nullptr);
    QTest::keyClick(window, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    ASSERT_TRUE(QTest::qWaitFor([dialog] { return dialog->property("opened").toBool(); }, kWaitMs));
    QMetaObject::invokeMethod(dialog, "accept");
    ASSERT_TRUE(QTest::qWaitFor([&edit] { return edit.has_clipboard(); }, kWaitMs));
    EXPECT_TRUE(edit.message().startsWith(QStringLiteral("Copied ")));
    viewer->forceActiveFocus(Qt::OtherFocusReason);
    QTest::keyClick(window, Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
    ASSERT_TRUE(QTest::qWaitFor([&edit] { return edit.message().startsWith(QStringLiteral("Pasted ")); }, kWaitMs));
    EXPECT_TRUE(child<QQuickItem>("editMessage")->isVisible());
    EXPECT_TRUE(controller->status_message().startsWith(QStringLiteral("Pasted ")));
    ASSERT_TRUE(settle());
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

namespace lucent::ui {
namespace {

// W7: Export batch from the Library (Ctrl+click selection, the dialog with its live example paths
// and the explicit confirmation to replace files), then the Exports view: the batch with its counts
// and the selected batch's report, without a warning.
TEST_F(QmlShellTest, TheBatchDialogShowsExamplesAndTheExportsViewListsTheBatchWithItsReport) {
    write_photos(3, 1);
    import_folder();
    auto* grid = child<QQuickItem>("contactSheet");
    ASSERT_NE(grid, nullptr);
    controller->library()->selectOnly(0);
    controller->library()->toggleSelected(2);
    EXPECT_EQ(controller->library()->selected_count(), 2);
    ASSERT_TRUE(settle());
    EXPECT_EQ(visual_items("selectionMark", true).size(), 2);

    auto* dialog = child("batchExportDialog");
    ASSERT_NE(dialog, nullptr);
    QTest::keyClick(window, Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier);
    ASSERT_TRUE(QTest::qWaitFor([dialog] { return dialog->property("opened").toBool(); }, kWaitMs));
    BatchSetupModel& setup = *controller->batch_setup();
    EXPECT_TRUE(setup.active());
    EXPECT_EQ(setup.scope(), BatchSetupModel::Selection);
    EXPECT_FALSE(child("batchStart")->property("enabled").toBool());  // no output folder yet
    setup.setOutputFolder(QUrl::fromLocalFile(folder.filePath(QStringLiteral("out"))));
    setup.setScope(BatchSetupModel::AllPhotos);
    ASSERT_TRUE(QTest::qWaitFor([&] { return setup.plan_valid() && !setup.planning(); }, kWaitMs));
    EXPECT_EQ(setup.examples().size(), 3);  // the first three
    EXPECT_EQ(setup.outputs(), 3);
    EXPECT_EQ(setup.excluded_count(), 1);  // the Unsupported photo, with its reason
    EXPECT_TRUE(child("batchStart")->property("enabled").toBool());
    // W8: automatic settings are offered, off by default; keeping shots consistent needs them.
    EXPECT_TRUE(child("batchAutomaticOff")->property("checked").toBool());
    EXPECT_FALSE(child("batchKeepConsistent")->property("enabled").toBool());
    EXPECT_FALSE(child("batchIncludeGps")->property("checked").toBool());

    // Replacing files asks first; keeping them changes nothing.
    auto* overwrite = child("batchOverwrite");
    auto* confirm = child("batchOverwriteConfirm");
    ASSERT_NE(overwrite, nullptr);
    ASSERT_NE(confirm, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(overwrite, "clicked"));
    ASSERT_TRUE(QTest::qWaitFor([confirm] { return confirm->property("opened").toBool(); }, kWaitMs));
    EXPECT_FALSE(setup.overwrite());
    EXPECT_FALSE(overwrite->property("checked").toBool());
    ASSERT_TRUE(QMetaObject::invokeMethod(child("batchOverwriteKeep"), "clicked"));
    ASSERT_TRUE(QTest::qWaitFor([confirm] { return !confirm->property("visible").toBool(); }, kWaitMs));
    EXPECT_FALSE(setup.overwrite());
    ASSERT_TRUE(QMetaObject::invokeMethod(overwrite, "clicked"));
    ASSERT_TRUE(QTest::qWaitFor([confirm] { return confirm->property("opened").toBool(); }, kWaitMs));
    ASSERT_TRUE(QMetaObject::invokeMethod(child("batchOverwriteReplace"), "clicked"));
    EXPECT_TRUE(setup.overwrite());
    EXPECT_TRUE(overwrite->property("checked").toBool());
    ASSERT_TRUE(QMetaObject::invokeMethod(child("batchUniqueNames"), "clicked"));
    EXPECT_FALSE(setup.overwrite());

    ASSERT_TRUE(QMetaObject::invokeMethod(child("batchStart"), "clicked"));
    ASSERT_TRUE(QTest::qWaitFor([dialog] { return !dialog->property("visible").toBool(); }, kWaitMs));
    EXPECT_EQ(controller->current_view(), AppController::Exports);
    ASSERT_TRUE(QTest::qWaitFor([this] { return controller->batches()->count() == 1; }, kWaitMs));
    ASSERT_TRUE(settle());
    const BatchSnapshot* batch = controller->batches()->batch_at(0);
    ASSERT_NE(batch, nullptr);
    EXPECT_EQ(batch->total, 4);  // three photos and the Unsupported one, which the simulation fails
    EXPECT_EQ(batch->completed, 3);
    EXPECT_EQ(batch->failed, 1);
    EXPECT_TRUE(child<QQuickItem>("batchList")->isVisible());
    EXPECT_EQ(child("batchList")->property("count").toInt(), 1);
    EXPECT_FALSE(child<QQuickItem>("exportJobs")->isVisible());
    ASSERT_TRUE(QTest::qWaitFor([this] { return controller->batch_report()->has_report(); }, kWaitMs));
    EXPECT_EQ(controller->batch_report()->count(), 4);
    EXPECT_EQ(child("reportEntries")->property("count").toInt(), 4);
    EXPECT_TRUE(controller->status_message().contains(QStringLiteral("finished")));
    EXPECT_FALSE(visual_items("batchRetry", true).isEmpty());  // a failed photo can be retried
    EXPECT_TRUE(capture.warnings().isEmpty()) << warnings_text(capture.warnings()).toStdString();
}

// W8: Auto from the Edit view (Ctrl+Shift+A): its run with Cancel in the Automatic settings panel,
// then the automatic revision with its summary (confidence, review, fallback), undo back to the
// recipe before it; the library's Review badge; the batch dialog's automatic settings and an
// analysing-only batch in the Exports view. Without a warning.
TEST_F(QmlShellTest, AutoShowsItsRunAndWhatItDidAndBatchesOfferAutomaticSettings) {
    write_photos(12);
    import_folder();
    // The Review badge marks the flagged photos (the simulation flags some).
    int flagged = -1;
    for (int row = 0; row < controller->library()->count(); ++row) {
        if (controller->library()->data(controller->library()->index(row), LibraryModel::ReviewRole).toBool()) {
            flagged = row;
            break;
        }
    }
    if (flagged >= 0) {
        controller->set_current_index(flagged);
        EXPECT_TRUE(QTest::qWaitFor([this] { return !visual_items("reviewBadge", true).isEmpty(); }, kWaitMs));
        // The badge says it; the card's flag line does not repeat it.
        for (QQuickItem* line : visual_items("flagLine", true)) {
            EXPECT_FALSE(line->property("text").toString().contains(QStringLiteral("Review recommended")));
        }
    }

    window->requestActivate();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window));
    ASSERT_TRUE(controller->openItem(0));
    auto* viewer = child<QQuickItem>("previewViewer");
    ASSERT_TRUE(QTest::qWaitFor([viewer] { return viewer->property("status").toInt() == kImageStatusReady; },
                                kWaitMs));
    ASSERT_TRUE(settle());
    auto* edits = dynamic_cast<fakes::FakeEditService*>(&controller->edit_service());
    ASSERT_NE(edits, nullptr);
    EditSession& edit = *controller->edit();
    AutoSettingsModel& automatic = *edit.automatic();
    auto* panel = child<QQuickItem>("autoSettingsPanel");
    ASSERT_NE(panel, nullptr);
    EXPECT_FALSE(panel->isVisible());

    // A run in progress: the panel shows it with Cancel; the button waits for it.
    edits->hold_auto(true);
    viewer->forceActiveFocus(Qt::OtherFocusReason);
    QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier | Qt::ShiftModifier);
    ASSERT_TRUE(QTest::qWaitFor([&] { return automatic.running() && automatic.progress() > 0.0; }, kWaitMs));
    EXPECT_TRUE(panel->isVisible());
    EXPECT_FALSE(visual_items("autoProgressRow", true).isEmpty());
    EXPECT_FALSE(visual_items("autoCancelButton", true).isEmpty());
    EXPECT_FALSE(visual_items("autoButton", true).front()->isEnabled());
    EXPECT_TRUE(controller->busy());

    // It ends: the automatic revision is shown with what Auto did.
    edits->hold_auto(false);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !automatic.running() && automatic.has_summary(); }, kWaitMs));
    ASSERT_TRUE(settle());
    EXPECT_NEAR(edit.value(QStringLiteral("exposure")), 0.5, 1e-9);
    EXPECT_EQ(edit.history()->count(), 2);  // the automatic revision and "As shot"
    EXPECT_TRUE(edit.history()->data(edit.history()->index(0), EditHistoryModel::TitleRole)
                    .toString()
                    .startsWith(QStringLiteral("Auto: ")));
    EXPECT_FALSE(visual_items("autoSummary", true).isEmpty());
    EXPECT_EQ(child("autoConfidence")->property("text").toString(), QStringLiteral("Confidence: high (82 %)"));
    EXPECT_TRUE(child("autoChanged")->property("text").toString().contains(QStringLiteral("Exposure +0.50 EV")));
    EXPECT_FALSE(child<QQuickItem>("autoReview")->isVisible());
    EXPECT_TRUE(controller->status_message().startsWith(QStringLiteral("Auto saved its settings")));
    EXPECT_TRUE(visual_items("autoButton", true).front()->isEnabled());

    // Undo returns to the recipe before Auto.
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    ASSERT_TRUE(settle());
    EXPECT_DOUBLE_EQ(edit.value(QStringLiteral("exposure")), 0.0);
    EXPECT_FALSE(automatic.has_summary());

    // A failed full-size check: the conservative fallback, recommended for review, with why.
    edits->fail_next_auto();
    QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier | Qt::ShiftModifier);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !automatic.running() && automatic.has_summary(); }, kWaitMs));
    ASSERT_TRUE(settle());
    EXPECT_TRUE(automatic.review());
    EXPECT_TRUE(child<QQuickItem>("autoReview")->isVisible());
    EXPECT_TRUE(child<QQuickItem>("autoFallback")->isVisible());
    EXPECT_EQ(edit.history()->count(), 4);  // both attempts are kept

    // The batch dialog: automatic settings with their note; keeping shots consistent needs them.
    auto* dialog = child("batchExportDialog");
    ASSERT_NE(dialog, nullptr);
    controller->showView(AppController::Library);
    ASSERT_TRUE(controller->openBatchExport());
    ASSERT_TRUE(QTest::qWaitFor([dialog] { return dialog->property("opened").toBool(); }, kWaitMs));
    BatchSetupModel& setup = *controller->batch_setup();
    setup.setOutputFolder(QUrl::fromLocalFile(folder.filePath(QStringLiteral("out"))));
    EXPECT_FALSE(child("batchKeepConsistent")->property("enabled").toBool());
    ASSERT_TRUE(QMetaObject::invokeMethod(child("batchAutomaticAnalyze"), "clicked"));
    EXPECT_EQ(setup.automatic(), BatchSetupModel::AutomaticAnalyzeOnly);
    EXPECT_TRUE(child("batchKeepConsistent")->property("enabled").toBool());
    EXPECT_EQ(child("batchStart")->property("text").toString(), QStringLiteral("Start analysis"));
    // Nothing is exported: no export settings are asked for.
    EXPECT_EQ(dialog->property("title").toString(), QStringLiteral("Analyse batch"));
    EXPECT_FALSE(child<QQuickItem>("batchPresetJpeg")->isVisible());
    EXPECT_FALSE(child<QQuickItem>("batchNamePattern")->isVisible());
    ASSERT_TRUE(QTest::qWaitFor([&] { return setup.plan_valid() && !setup.planning(); }, kWaitMs));
    EXPECT_FALSE(setup.analysis_notes().isEmpty());
    EXPECT_EQ(child("batchAnalysisNotes")->property("count").toInt(), setup.analysis_notes().size());
    ASSERT_TRUE(QMetaObject::invokeMethod(child("batchStart"), "clicked"));
    ASSERT_TRUE(QTest::qWaitFor([dialog] { return !dialog->property("visible").toBool(); }, kWaitMs));
    ASSERT_TRUE(QTest::qWaitFor([this] { return controller->batches()->count() == 1; }, kWaitMs));
    ASSERT_TRUE(settle());
    EXPECT_FALSE(visual_items("batchAutomaticTag", true).isEmpty());
    const BatchSnapshot* batch = controller->batches()->batch_at(0);
    ASSERT_NE(batch, nullptr);
    EXPECT_EQ(batch->automatic, BatchAutomatic::AnalyzeOnly);
    EXPECT_TRUE(controller->status_message().contains(QStringLiteral("analysed")))
        << controller->status_message().toStdString();
    // The finished batch's report: every photo analysed, with what Auto did.
    ASSERT_TRUE(QTest::qWaitFor(
        [this] { return controller->batch_report()->has_report() && controller->batch_report()->analysed() == 12; },
        kWaitMs));
    ASSERT_TRUE(settle());
    EXPECT_TRUE(QTest::qWaitFor([this] { return !visual_items("reportAutomatic", true).isEmpty(); }, kWaitMs));
    EXPECT_TRUE(capture.warnings().isEmpty()) << warnings_text(capture.warnings()).toStdString();
}

// An attention is shown on the batch with the service's text and a Resume button.
TEST_F(QmlShellTest, AnAttentionBannerOffersResume) {
    controller->showView(AppController::Exports);
    BatchSnapshot batch;
    batch.id = 3;
    batch.name = QStringLiteral("Export of 4 photos");
    batch.folder = QStringLiteral("E:\\Out");
    batch.state = BatchState::Paused;
    batch.total = 4;
    batch.completed = 1;
    batch.queued = 3;
    batch.attention = BatchAttention::DestinationUnavailable;
    batch.attention_message = QStringLiteral("E:\\Out is not available: its drive may have been disconnected, so "
                                             "the batch is paused. Reconnect it, then choose Resume.");
    emit controller->batch_service().batch_changed(batch);
    ASSERT_TRUE(settle());
    ASSERT_TRUE(QTest::qWaitFor([this] { return visual_items("batchAttention", true).size() == 1; }, kWaitMs));
    EXPECT_EQ(visual_items("batchAttentionResume", true).size(), 1);
    EXPECT_EQ(child("exportSummary")->property("text").toString(), QStringLiteral("1 needs attention"));
    EXPECT_TRUE(capture.warnings().isEmpty()) << warnings_text(capture.warnings()).toStdString();
}

}  // namespace
}  // namespace lucent::ui
