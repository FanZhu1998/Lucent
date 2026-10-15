// The QML module end to end: Main.qml loads with its views and models and without a single QML or
// Qt warning, imports fill the contact sheet, keyboard navigation works, previews load, export
// jobs are listed, the screenshot automation writes a PNG, and the window icon and the About
// dialog's logo are Lucent's logo at every scale.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <set>

#include <QColor>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "app_controller.hpp"
#include "branding.hpp"
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
        Services services = fakes::create_fake_services(fakes::FakeTimings::immediate());
        updates = dynamic_cast<fakes::FakeUpdateService*>(services.updates.get());
        controller = std::make_unique<AppController>(std::move(services));
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
    fakes::FakeUpdateService* updates = nullptr;  // owned by the controller's services
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
// W10 (ADR 0018): Help > About shows Lucent's version, the camera support database and what the
// four support states mean; the Edit details say what the photo's state means.
TEST_F(QmlShellTest, AboutShowsTheCameraSupportDatabaseAndWhatTheStatesMean) {
    auto* dialog = child("aboutDialog");
    ASSERT_NE(dialog, nullptr);
    EXPECT_FALSE(dialog->property("opened").toBool());
    ASSERT_TRUE(QMetaObject::invokeMethod(child("aboutButton"), "clicked"));
    ASSERT_TRUE(QTest::qWaitFor([dialog] { return dialog->property("opened").toBool(); }, kWaitMs));
    const QString version = child("aboutVersion")->property("text").toString();
    EXPECT_TRUE(version.contains(controller->app_version())) << version.toStdString();
    const QString database = child("aboutCameraSupport")->property("text").toString();
    EXPECT_TRUE(database.contains(QStringLiteral("Version 1.0.0, qualified 2026-10-08")))
        << database.toStdString();
    EXPECT_EQ(child("aboutSupportStates")->property("count").toInt(), 4);
    // W13 (ADR 0023): the lens database the lens corrections use.
    const QString lenses = child("aboutLensDatabase")->property("text").toString();
    EXPECT_TRUE(lenses.contains(QStringLiteral("Version 1.0.0 of the Lensfun lens database")))
        << lenses.toStdString();
    ASSERT_TRUE(QMetaObject::invokeMethod(child("aboutCloseButton"), "clicked"));
    ASSERT_TRUE(QTest::qWaitFor([dialog] { return !dialog->property("visible").toBool(); }, kWaitMs));

    write_photos(3);
    import_folder();
    ASSERT_TRUE(controller->openItem(0));
    ASSERT_TRUE(settle());
    const QString meaning = child("supportMeaning")->property("text").toString();
    EXPECT_TRUE(meaning.startsWith(QStringLiteral("What this means: "))) << meaning.toStdString();
    EXPECT_TRUE(meaning.contains(controller->current()->support_meaning()));
    EXPECT_TRUE(capture.warnings().isEmpty()) << warnings_text(capture.warnings()).toStdString();
}

// W12 release phase (ADR 0025): the window icon is Lucent's logo with every pre-rendered size, which
// covers what Windows asks for at the display scales it offers up to 300 % (the small and the large
// icon, 16 and 32 logical pixels), each image exactly as built; the "branding" provider serves the
// pre-rendered image of each scale's size and scales down only between them; Help > About shows the
// logo at 64 logical pixels, drawn 1:1 on whole device pixels, its plate lined up with the text column.
// CTest runs it at this window's ratio 1 and again with QT_SCALE_FACTOR 1.25 and 1.75
// (apps/lucent/tests/CMakeLists.txt; review finding BRD-08), where a whole logical position can fall
// between device pixels.
TEST_F(QmlShellTest, TheWindowIconAndTheAboutLogoAreLucentsLogoAtEveryScale) {
    const QString role = QString::fromLatin1(kBrandingAppIconRole);
    const QList<BrandingImage> images = branding_images(role);
    ASSERT_FALSE(images.isEmpty());
    std::set<int> sizes;
    for (const BrandingImage& image : images) {
        EXPECT_EQ(image.size.width(), image.size.height()) << image.path.toStdString();
        sizes.insert(image.size.width());
    }
    for (const int logical : {16, 32, 64}) {
        for (const int percent : {100, 125, 150, 175, 200, 225, 250, 300}) {
            const int pixels = logical * percent / 100;
            EXPECT_TRUE(sizes.contains(pixels))
                << "no " << pixels << " px image for " << logical << " logical px at " << percent << " %";
        }
    }

    // The window's icon: every image at its own size, each exactly as built (not rescaled). Both sides
    // premultiplied, as a QPixmap holds them.
    const QIcon icon = window->icon();
    std::set<int> icon_sizes;
    for (const QSize& size : icon.availableSizes()) {
        EXPECT_EQ(size.width(), size.height());
        icon_sizes.insert(size.width());
    }
    EXPECT_EQ(icon_sizes, sizes);
    for (const BrandingImage& image : images) {
        const QImage built = QImage(image.path).convertToFormat(QImage::Format_ARGB32_Premultiplied);
        const QImage shown =
            icon.pixmap(image.size, 1.0).toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        EXPECT_EQ(shown, built) << image.path.toStdString();
    }

    // The provider: the pre-rendered image for 64 logical pixels at each scale, a scaled-down larger
    // one between them, never an enlarged one, nothing for an unknown role.
    for (const int percent : {100, 125, 150, 175, 200, 225, 250, 300}) {
        const int pixels = 64 * percent / 100;
        const auto exact = std::find_if(images.begin(), images.end(), [pixels](const BrandingImage& image) {
            return image.size.width() == pixels;
        });
        ASSERT_NE(exact, images.end());
        EXPECT_EQ(branding_image(role, QSize(pixels, pixels)), QImage(exact->path)) << pixels;
    }
    EXPECT_EQ(branding_image(role, QSize(70, 70)).size(), QSize(70, 70));
    EXPECT_EQ(branding_image(role, QSize(4096, 4096)).size(), images.back().size);
    EXPECT_EQ(branding_image(role, QSize()).size(), images.back().size);
    EXPECT_TRUE(branding_image(QStringLiteral("no-such-role"), QSize(64, 64)).isNull());
    EXPECT_TRUE(branding_image(QStringLiteral("../app-icon"), QSize(64, 64)).isNull());

    // Help > About: the logo loads through the provider at 64 x 64 logical pixels, which is the image of
    // exactly 64 times the window's device pixel ratio.
    auto* dialog = child("aboutDialog");
    ASSERT_NE(dialog, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(child("aboutButton"), "clicked"));
    ASSERT_TRUE(QTest::qWaitFor([dialog] { return dialog->property("opened").toBool(); }, kWaitMs));
    auto* logo = child<QQuickItem>("aboutLogo");
    ASSERT_NE(logo, nullptr);
    ASSERT_TRUE(
        QTest::qWaitFor([logo] { return logo->property("status").toInt() == kImageStatusReady; }, kWaitMs));
    EXPECT_EQ(logo->property("source").toUrl(), QUrl(QStringLiteral("image://branding/app-icon")));
    EXPECT_EQ(logo->property("sourceSize").toSize(), QSize(64, 64));
    EXPECT_EQ(logo->width(), 64.0);
    EXPECT_EQ(logo->height(), 64.0);
    EXPECT_EQ(logo->property("implicitWidth").toReal(), 64.0);  // its image's pixels at the window's ratio
    const qreal ratio = window->devicePixelRatio();
    const int pixels = qRound(64 * ratio);
    ASSERT_NEAR(64 * ratio, pixels, 1e-9)
        << "64 logical pixels are no whole number of device pixels at " << ratio;
    const QImage expected =
        branding_image(role, QSize(pixels, pixels)).convertToFormat(QImage::Format_ARGB32);
    ASSERT_EQ(expected.size(), QSize(pixels, pixels)) << "no image of " << pixels << " pixels";

    // On whole device pixels: anywhere else Lucent.exe's renderer samples the image between its pixels, and
    // the logo blurs (the dialog is centred in device pixels; a whole logical position is half a device
    // pixel off at 125 and 175 %). Checked here directly: the software renderer of these tests draws an
    // image on the nearest whole pixel, so the grab below would not show it.
    const QPointF origin = logo->mapToScene(QPointF(0, 0)) * ratio;
    EXPECT_NEAR(origin.x(), std::round(origin.x()), 1e-6) << "at " << ratio;
    EXPECT_NEAR(origin.y(), std::round(origin.y()), 1e-6) << "at " << ratio;
    const int left = qRound(origin.x());
    const int top = qRound(origin.y());

    // The plate lines up with the text column below the logo (review finding BRD-11): the plate's edge is
    // where the artwork's content box starts (alpha 128 and more; ADR 0025, decisions 6 and 21, so a faint
    // rim in the margin does not count), and only the image's margin lies left of the column.
    const auto outside_content = [&expected](int x) {
        for (int y = 0; y < expected.height(); ++y) {
            if (qAlpha(expected.pixel(x, y)) >= 128) {
                return false;
            }
        }
        return true;
    };
    int margin = 0;
    while (margin < pixels && outside_content(margin)) {
        ++margin;
    }
    EXPECT_GT(margin, 0);
    auto* column = child<QQuickItem>("aboutCameraSupport");
    ASSERT_NE(column, nullptr);
    EXPECT_NEAR(left + margin, column->mapToScene(QPointF(0, 0)).x() * ratio, 1e-6) << "at " << ratio;

    // Drawn 1:1: where the logo is, the grab of the window (in device pixels) shows the image of this ratio
    // composited on the dialog's background, its anti-aliased edges included (another image scaled to
    // this size, as a logo loaded at another screen's ratio would be, differs at the edges).
    const QImage grab = window->grabWindow().convertToFormat(QImage::Format_ARGB32);
    ASSERT_GE(grab.width(), left + pixels);
    ASSERT_GE(grab.height(), top + pixels);
    auto* background = dialog->property("background").value<QQuickItem*>();
    ASSERT_NE(background, nullptr);
    const QColor surface = background->property("color").value<QColor>();
    int largest = 0;
    int different = 0;
    for (int y = 0; y < pixels; ++y) {
        for (int x = 0; x < pixels; ++x) {
            const QRgb image = expected.pixel(x, y);
            const int alpha = qAlpha(image);
            const auto over = [alpha](int colour, int under) {
                return (colour * alpha + under * (255 - alpha) + 127) / 255;
            };
            const QRgb shown = grab.pixel(left + x, top + y);
            const int difference = std::max({std::abs(qRed(shown) - over(qRed(image), surface.red())),
                                             std::abs(qGreen(shown) - over(qGreen(image), surface.green())),
                                             std::abs(qBlue(shown) - over(qBlue(image), surface.blue()))});
            largest = std::max(largest, difference);
            different += difference > 3 ? 1 : 0;
        }
    }
    EXPECT_EQ(different, 0) << "of " << pixels * pixels << " pixels at " << ratio << " (largest difference "
                            << largest << ")";

    ASSERT_TRUE(QMetaObject::invokeMethod(child("aboutCloseButton"), "clicked"));
    ASSERT_TRUE(QTest::qWaitFor([dialog] { return !dialog->property("visible").toBool(); }, kWaitMs));
    EXPECT_TRUE(capture.warnings().isEmpty()) << warnings_text(capture.warnings()).toStdString();
}

// W13 (ADR 0023): the Lens corrections panel shows the (simulated) exact match and its profile; a
// switch is one revision; corrections the file makes itself are shown off and disabled, with a note;
// the profile choice opens and closes without changing anything.
TEST_F(QmlShellTest, TheLensCorrectionsPanelShowsTheMatchAndEachSwitchIsOneRevision) {
    write_photos(1);
    import_folder();
    ASSERT_TRUE(controller->openItem(0));
    ASSERT_TRUE(settle());
    auto* edits = dynamic_cast<fakes::FakeEditService*>(&controller->edit_service());
    ASSERT_NE(edits, nullptr);
    const ItemId item = controller->current()->item_id();
    EditSession& edit = *controller->edit();
    ASSERT_TRUE(QTest::qWaitFor([&] { return edit.lens()->known(); }, kWaitMs));
    auto* panel = child<QQuickItem>("lensPanel");
    ASSERT_NE(panel, nullptr);
    EXPECT_TRUE(panel->isVisible());
    EXPECT_EQ(child("lensMatchBadge")->property("text").toString(), QStringLiteral("Exact match"));
    EXPECT_EQ(child("lensProfile")->property("text").toString(),
              QStringLiteral("Simulated Optics Zoom 24-70mm f/4"));
    EXPECT_EQ(child("lensCamera")->property("text").toString(), QStringLiteral("Simulated Camera"));
    EXPECT_FALSE(child<QQuickItem>("lensUnavailable")->isVisible());
    EXPECT_FALSE(child<QQuickItem>("lensTurnOn")->isVisible());
    const QList<QQuickItem*> distortion = visual_items("lensCorrection_distortion", false);
    ASSERT_EQ(distortion.size(), 1);
    EXPECT_TRUE(distortion.front()->property("checked").toBool());
    EXPECT_TRUE(distortion.front()->isEnabled());
    EXPECT_EQ(visual_items("lensStatus_distortion", false).front()->property("text").toString(),
              QStringLiteral("Applied"));

    // A click on a switch: one revision, rendered without that correction.
    const int before = edits->revision_count(item);
    ASSERT_TRUE(QMetaObject::invokeMethod(distortion.front(), "click"));
    ASSERT_TRUE(settle());
    EXPECT_EQ(edits->revision_count(item), before + 1);
    EXPECT_FALSE(edit.settings().lens.distortion);
    EXPECT_FALSE(distortion.front()->property("checked").toBool());
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            return visual_items("lensStatus_distortion", false).front()->property("text").toString() ==
                   u"Off";
        },
        kWaitMs));
    // The profile choice opens without a revision, with its search field focused, and closes again:
    // with Cancel, and with Esc from any of its controls, never leaving the Edit view (the window's
    // Esc is off while the choice is open; W13 closure, APL-07).
    window->requestActivate();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window));
    ASSERT_TRUE(QMetaObject::invokeMethod(child("lensChooseProfile"), "click"));
    EXPECT_TRUE(edit.lens()->picking());
    EXPECT_TRUE(child<QQuickItem>("lensPicker")->isVisible());
    EXPECT_EQ(edits->revision_count(item), before + 1);
    auto* search = child<QQuickItem>("lensSearch");
    ASSERT_TRUE(QTest::qWaitFor([search] { return search->hasActiveFocus(); }, kWaitMs));
    ASSERT_TRUE(QMetaObject::invokeMethod(child("lensPickerCancel"), "click"));
    EXPECT_FALSE(edit.lens()->picking());
    for (const char* focused : {"lensSearch", "lensPickerCancel"}) {
        SCOPED_TRACE(focused);
        ASSERT_TRUE(QMetaObject::invokeMethod(child("lensChooseProfile"), "click"));
        ASSERT_TRUE(edit.lens()->picking());
        auto* control = child<QQuickItem>(focused);
        control->forceActiveFocus();
        ASSERT_TRUE(QTest::qWaitFor([control] { return control->hasActiveFocus(); }, kWaitMs));
        QTest::keyClick(window, Qt::Key_Escape);
        ASSERT_TRUE(QTest::qWaitFor([&] { return !edit.lens()->picking(); }, kWaitMs));
        EXPECT_EQ(controller->current_view(), AppController::Edit);
    }
    EXPECT_EQ(edits->revision_count(item), before + 1);

    // A file that corrects distortion and vignetting itself: those switches are off and disabled.
    fakes::FakeEditService::FakeLens carried;
    carried.carried_distortion = true;
    carried.carried_vignetting = true;
    edits->set_lens(carried);
    edit.setValue(QStringLiteral("exposure"), 0.3);  // a new frame
    ASSERT_TRUE(settle());
    ASSERT_TRUE(QTest::qWaitFor([&] { return child<QQuickItem>("lensCarriedNote")->isVisible(); }, kWaitMs));
    EXPECT_FALSE(visual_items("lensCorrection_vignetting", false).front()->isEnabled());
    EXPECT_FALSE(visual_items("lensCorrection_vignetting", false).front()->property("checked").toBool());
    EXPECT_TRUE(visual_items("lensCorrection_chromaticAberration", false).front()->isEnabled());
    EXPECT_TRUE(
        child("lensCarriedNote")->property("text").toString().contains(QStringLiteral("DNG opcodes")));
    EXPECT_TRUE(capture.warnings().isEmpty()) << warnings_text(capture.warnings()).toStdString();
}

// W12 (ADR 0022): Help > About's "Check for updates": nothing is checked until the photographer asks;
// the release is offered with its notes (plain text), downloaded and verified, and installed only
// between jobs, after which the application is asked to quit.
TEST_F(QmlShellTest, AboutChecksForUpdatesOnlyWhenAskedAndInstallsBetweenJobs) {
    ASSERT_NE(updates, nullptr);
    auto* dialog = child("aboutDialog");
    ASSERT_NE(dialog, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(child("aboutButton"), "clicked"));
    ASSERT_TRUE(QTest::qWaitFor([dialog] { return dialog->property("opened").toBool(); }, kWaitMs));
    const QString channel = child("updateChannel")->property("text").toString();
    EXPECT_TRUE(channel.contains(QStringLiteral("when you ask, and only then"))) << channel.toStdString();
    EXPECT_EQ(updates->checks(), 0);
    auto* check = child("updateCheckButton");
    ASSERT_NE(check, nullptr);
    EXPECT_TRUE(check->property("visible").toBool());
    EXPECT_TRUE(check->property("enabled").toBool());

    ASSERT_TRUE(QMetaObject::invokeMethod(check, "clicked"));
    UpdateModel* model = controller->updates();
    ASSERT_TRUE(QTest::qWaitFor([model] { return model->state() == UpdateModel::Available; }, kWaitMs));
    EXPECT_EQ(updates->checks(), 1);
    auto* download = child("updateDownloadButton");
    ASSERT_NE(download, nullptr);
    EXPECT_TRUE(download->property("visible").toBool());
    EXPECT_TRUE(download->property("text").toString().contains(QStringLiteral("9.9.0")));
    EXPECT_TRUE(child("updateNotes")->property("visible").toBool());
    EXPECT_TRUE(child("updateStatus")->property("text").toString().contains(QStringLiteral("9.9.0")));

    ASSERT_TRUE(QMetaObject::invokeMethod(download, "clicked"));
    ASSERT_TRUE(QTest::qWaitFor([model] { return model->state() == UpdateModel::Ready; }, kWaitMs));
    auto* install = child("updateInstallButton");
    ASSERT_NE(install, nullptr);
    EXPECT_TRUE(install->property("visible").toBool());
    EXPECT_TRUE(install->property("enabled").toBool());

    // An export is running: Install waits and the panel says why.
    updates->script().blocker = QStringLiteral("Lucent installs updates between jobs.");
    model->refresh();
    EXPECT_FALSE(install->property("enabled").toBool());
    EXPECT_TRUE(child("updateStatus")->property("text").toString().contains(updates->script().blocker));

    updates->script().blocker.clear();
    model->refresh();
    ASSERT_TRUE(install->property("enabled").toBool());
    QSignalSpy quit(controller.get(), &AppController::quitRequested);
    ASSERT_TRUE(QMetaObject::invokeMethod(install, "clicked"));
    ASSERT_TRUE(QTest::qWaitFor([&quit] { return quit.count() == 1; }, kWaitMs));
    EXPECT_EQ(updates->installs(), 1);
    EXPECT_TRUE(capture.warnings().isEmpty()) << warnings_text(capture.warnings()).toStdString();
}

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

// W9: the crop tool (R) and the masks (Shift+R, B, O) from the keyboard and the mouse: the crop
// tool shows the uncropped image with handles, a handle drag and a brush stroke are one revision
// each, Enter and Esc close the tools before Esc leaves Edit, all without a warning.
TEST_F(QmlShellTest, TheCropAndMaskToolsWorkFromTheKeyboardAndTheMouse) {
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
    ASSERT_GT(edit.frame_output_width(), 0);
    const auto centre_of = [](QQuickItem* item) {
        return item->mapToScene(QPointF(item->width() / 2.0, item->height() / 2.0)).toPoint();
    };
    const auto drag = [this](QPoint from, QPoint to, int steps) {
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from);
        for (int step = 1; step <= steps; ++step) {
            QTest::mouseMove(window, from + (to - from) * step / steps);
        }
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, to);
    };

    // R: the crop tool on the uncropped image, eight handles.
    viewer->forceActiveFocus(Qt::OtherFocusReason);
    QTest::keyClick(window, Qt::Key_R);
    EXPECT_EQ(edit.tool(), QStringLiteral("crop"));
    ASSERT_TRUE(QTest::qWaitFor([this] { return child<QQuickItem>("cropOverlay")->isVisible(); }, kWaitMs));
    EXPECT_TRUE(edit.frame_uncropped());
    ASSERT_EQ(visual_items("cropHandle_right", true).size(), 1);
    EXPECT_EQ(visual_items("cropHandle_topLeft", true).size(), 1);
    QQuickItem* right = visual_items("cropHandle_right", true).front();
    const QPoint from = centre_of(right);
    drag(from, from - QPoint(80, 0), 8);
    ASSERT_TRUE(settle());
    EXPECT_EQ(edits->revision_count(item), 1);  // one revision for the drag
    EXPECT_LT(edit.settings().geometry.crop.right, 0.95);
    EXPECT_DOUBLE_EQ(edit.settings().geometry.crop.left, 0.0);
    // Enter applies the crop and closes the tool.
    QTest::keyClick(window, Qt::Key_Return);
    EXPECT_TRUE(edit.tool().isEmpty());
    ASSERT_TRUE(settle());
    EXPECT_FALSE(child<QQuickItem>("cropOverlay")->isVisible());
    EXPECT_FALSE(edit.frame_uncropped());

    // Shift+R: a radial mask with its handles; dragging the centre is one revision.
    QTest::keyClick(window, Qt::Key_R, Qt::ShiftModifier);
    EXPECT_EQ(edit.tool(), QStringLiteral("masks"));
    ASSERT_TRUE(settle());
    EXPECT_EQ(edits->revision_count(item), 2);
    ASSERT_TRUE(QTest::qWaitFor([this] { return !visual_items("maskHandle_center", true).isEmpty(); }, kWaitMs));
    EXPECT_GT(child<QQuickItem>("maskOutlines")->property("pathCount").toInt(), 0);
    const QPointF centre_before = edit.settings().masks.front().center;
    const QPoint handle = centre_of(visual_items("maskHandle_center", true).front());
    drag(handle, handle + QPoint(40, 20), 6);
    ASSERT_TRUE(settle());
    EXPECT_EQ(edits->revision_count(item), 3);
    EXPECT_GT(edit.settings().masks.front().center.x(), centre_before.x());
    // O: the selected mask's weights over the photo.
    QTest::keyClick(window, Qt::Key_O);
    EXPECT_TRUE(edit.mask_overlay());
    ASSERT_TRUE(QTest::qWaitFor([&edit] { return !edit.mask_overlay_source().isEmpty(); }, kWaitMs));
    ASSERT_TRUE(QTest::qWaitFor([this] { return child<QQuickItem>("maskOverlayImage")->isVisible(); }, kWaitMs));
    QTest::keyClick(window, Qt::Key_O);
    EXPECT_FALSE(edit.mask_overlay());

    // B: a brush mask; a mouse stroke across the photo is one revision.
    QTest::keyClick(window, Qt::Key_B);
    ASSERT_TRUE(settle());
    EXPECT_EQ(edit.masks()->selected_kind(), QStringLiteral("brush"));
    EXPECT_EQ(edits->revision_count(item), 4);
    ASSERT_TRUE(QTest::qWaitFor([this] { return child<QQuickItem>("maskPaintArea")->isEnabled(); }, kWaitMs));
    const QPoint middle = centre_of(viewer);
    drag(middle - QPoint(100, 0), middle + QPoint(100, 30), 20);
    ASSERT_TRUE(settle());
    EXPECT_EQ(edits->revision_count(item), 5);
    const EditMask& brush = edit.settings().masks.back();
    ASSERT_EQ(brush.kind, MaskKind::Brush);
    ASSERT_EQ(brush.strokes.size(), 1);
    EXPECT_GE(brush.strokes.front().points.size(), 2);
    ASSERT_GE(edit.history()->count(), 2);

    // Esc closes the tool, the next Esc leaves Edit.
    QTest::keyClick(window, Qt::Key_Escape);
    EXPECT_TRUE(edit.tool().isEmpty());
    EXPECT_EQ(controller->current_view(), AppController::Edit);
    QTest::keyClick(window, Qt::Key_Escape);
    EXPECT_EQ(controller->current_view(), AppController::Library);
    ASSERT_TRUE(settle());
    EXPECT_TRUE(capture.warnings().isEmpty()) << warnings_text(capture.warnings()).toStdString();
}

// W9 closure: while a tool is open its Enter and Esc shortcuts leave the editors alone (a slider's
// value field, the custom aspect ratio, the masks list, the history list), the open tool's panel is
// scrolled into view, and the masks list works from the keyboard.
TEST_F(QmlShellTest, EditorsKeepEnterAndEscWhileAToolIsOpen) {
    write_photos(1);
    import_folder();
    window->requestActivate();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window));
    ASSERT_TRUE(controller->openItem(0));
    auto* viewer = child<QQuickItem>("previewViewer");
    ASSERT_TRUE(QTest::qWaitFor([viewer] { return viewer->property("status").toInt() == kImageStatusReady; },
                                kWaitMs));
    ASSERT_TRUE(settle());
    EditSession& edit = *controller->edit();
    auto* panel = child<QQuickItem>("editPanel");
    ASSERT_NE(panel, nullptr);
    // The section's title lies inside the side panel's visible part.
    const auto in_view = [this, panel](const char* name) {
        auto* section = child<QQuickItem>(name);
        const double y = section->mapToItem(panel, QPointF(0.0, 0.0)).y();
        return y >= 0.0 && y < panel->height() - 40.0;
    };
    const auto type_into = [this](QQuickItem* field, const QString& text) {
        field->forceActiveFocus(Qt::TabFocusReason);
        QMetaObject::invokeMethod(field, "selectAll");
        for (const QChar c : text) {
            QTest::keyClick(window, c.toLatin1());
        }
    };

    // R opens the crop tool and brings its panel into view.
    viewer->forceActiveFocus(Qt::OtherFocusReason);
    QTest::keyClick(window, Qt::Key_R);
    ASSERT_EQ(edit.tool(), QStringLiteral("crop"));
    EXPECT_TRUE(QTest::qWaitFor([&] { return in_view("cropPanel"); }, kWaitMs));
    ASSERT_TRUE(settle());

    // Enter in the straighten field applies the typed value; the tool stays open.
    QQuickItem* straighten = visual_items("field_straighten", true).front();
    type_into(straighten, QStringLiteral("5"));
    QTest::keyClick(window, Qt::Key_Return);
    EXPECT_EQ(edit.tool(), QStringLiteral("crop"));
    EXPECT_DOUBLE_EQ(edit.settings().geometry.straighten_degrees, 5.0);
    // Esc abandons a typed value; the tool stays open and keeps its geometry.
    type_into(straighten, QStringLiteral("9"));
    QTest::keyClick(window, Qt::Key_Escape);
    EXPECT_EQ(edit.tool(), QStringLiteral("crop"));
    EXPECT_DOUBLE_EQ(edit.settings().geometry.straighten_degrees, 5.0);
    EXPECT_FALSE(straighten->property("text").toString().startsWith(u'9'));
    ASSERT_TRUE(settle());

    // A custom ratio typed and confirmed with Enter is applied; the tool stays open.
    edit.crop()->setAspect(QStringLiteral("custom"));
    ASSERT_TRUE(QTest::qWaitFor([this] { return !visual_items("cropCustomWidth", true).isEmpty(); }, kWaitMs));
    // Typed while frames arrive: the value being typed stays until Enter.
    type_into(visual_items("cropCustomWidth", true).front(), QStringLiteral("7"));
    ASSERT_TRUE(settle());
    QTest::keyClick(window, Qt::Key_Return);
    EXPECT_EQ(edit.tool(), QStringLiteral("crop"));
    EXPECT_DOUBLE_EQ(edit.crop()->custom_width(), 7.0);
    ASSERT_TRUE(settle());

    // From the photo, Enter still applies the crop and closes the tool.
    viewer->forceActiveFocus(Qt::OtherFocusReason);
    QTest::keyClick(window, Qt::Key_Return);
    EXPECT_TRUE(edit.tool().isEmpty());
    ASSERT_TRUE(settle());

    // Two masks (Shift+R, G): the masks panel comes into view; Space, Down and Enter on its rows
    // select masks from the keyboard while the tool stays open.
    QTest::keyClick(window, Qt::Key_R, Qt::ShiftModifier);
    ASSERT_TRUE(settle());
    QTest::keyClick(window, Qt::Key_G);
    ASSERT_TRUE(settle());
    ASSERT_EQ(edit.tool(), QStringLiteral("masks"));
    ASSERT_EQ(edit.settings().masks.size(), 2);
    EXPECT_TRUE(QTest::qWaitFor([&] { return in_view("masksPanel"); }, kWaitMs));
    const int first = static_cast<int>(edit.settings().masks[0].id);
    const int second = static_cast<int>(edit.settings().masks[1].id);
    EXPECT_EQ(edit.masks()->selected_id(), second);
    ASSERT_TRUE(QTest::qWaitFor([this] { return !visual_items("maskRow_0", true).isEmpty(); }, kWaitMs));
    QQuickItem* row = visual_items("maskRow_0", true).front();
    row->forceActiveFocus(Qt::TabFocusReason);
    ASSERT_TRUE(row->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Space);
    EXPECT_EQ(edit.masks()->selected_id(), first);
    QTest::keyClick(window, Qt::Key_Down);
    EXPECT_EQ(edit.masks()->selected_id(), second);
    EXPECT_TRUE(visual_items("maskRow_1", true).front()->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Up);
    QTest::keyClick(window, Qt::Key_Return);
    EXPECT_EQ(edit.masks()->selected_id(), first);
    EXPECT_EQ(edit.tool(), QStringLiteral("masks"));

    // Enter in the history list shows the revision chosen; the tool stays open.
    auto* history = child<QQuickItem>("historyList");
    history->forceActiveFocus(Qt::TabFocusReason);
    const int revisions = edit.history()->count();
    QTest::keyClick(window, Qt::Key_Down);
    QTest::keyClick(window, Qt::Key_Return);
    EXPECT_EQ(edit.tool(), QStringLiteral("masks"));
    ASSERT_TRUE(settle());
    EXPECT_EQ(edit.history()->count(), revisions);
    EXPECT_LT(edit.settings().masks.size(), 2u) << "an earlier revision is shown";
    EXPECT_TRUE(capture.warnings().isEmpty()) << warnings_text(capture.warnings()).toStdString();
}

}  // namespace
}  // namespace lucent::ui
