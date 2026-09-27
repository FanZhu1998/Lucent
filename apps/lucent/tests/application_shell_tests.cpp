// The shell end to end on the real application services (engine, LibRaw) with synthetic DNGs:
// import shows labelled camera previews, opening a photo renders Lucent's preview with its
// histogram and details and replaces the camera thumbnail, quick navigation ends on the photo
// shown last, and the Export dialog exports into the Exports view.

#include <QDir>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <memory>

#include <gtest/gtest.h>

#include "adapter_test_support.hpp"
#include "app_controller.hpp"
#include "developer_automation.hpp"
#include "shell.hpp"
#include "test_support.hpp"

namespace lucent::ui {
namespace {

using testing::kAdapterWaitMs;
using testing::write_dng;
namespace rts = lucent::raw::test_support;

constexpr int kImageStatusReady = 1;  // QQuickImageBase::Ready

class ApplicationShellTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto services = adapters::create_application_services(testing::test_backend_options());
        ASSERT_TRUE(services) << services.error().describe();
        controller = std::make_unique<AppController>(std::move(*services));
        controller->set_url_opener([](const QUrl&) { return true; });
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

    bool settle() {
        return QTest::qWaitFor(
            [this] { return !controller->busy() && !DeveloperAutomation::has_loading_images(*window); },
            kAdapterWaitMs);
    }

    // Visible items named `name` in the scene (delegates are found through the item tree: their
    // QObject parent is not the view).
    static int count_visible(const QQuickItem* item, const QString& name) {
        if (item == nullptr || !item->isVisible()) {
            return 0;
        }
        int count = item->objectName() == name ? 1 : 0;
        for (const QQuickItem* child : item->childItems()) {
            count += count_visible(child, name);
        }
        return count;
    }

    int visible_camera_labels() const {
        return count_visible(window->contentItem(), QStringLiteral("cameraPreviewLabel"));
    }

    // Warnings other than Qt Quick's report of a thumbnail that could not be loaded (a file without
    // a camera preview, which then shows Lucent's own render).
    QStringList unexpected_warnings() const {
        QStringList warnings = capture.warnings();
        warnings.removeIf(
            [](const QString& warning) { return warning.contains(QStringLiteral("no camera preview")); });
        return warnings;
    }

    testing::MessageCapture capture;  // first: outlives the engine
    rts::ScratchDir dir;
    std::unique_ptr<AppController> controller;
    std::unique_ptr<QQmlApplicationEngine> engine;
    QQuickWindow* window = nullptr;
};

TEST_F(ApplicationShellTest, ImportOpenNavigateAndExportOnTheRealServices) {
    write_dng(dir, L"a.dng", testing::dng_with_preview());
    write_dng(dir, L"b.dng", testing::dng_with_preview(6));
    write_dng(dir, L"c.dng");  // no camera preview
    std::filesystem::create_directories(dir.path() / L"out");

    ASSERT_TRUE(controller->addFolder(QUrl::fromLocalFile(QString::fromStdWString(dir.path().wstring()))));
    ASSERT_TRUE(QTest::qWaitFor([this] { return !controller->importing(); }, kAdapterWaitMs));
    ASSERT_TRUE(settle());
    EXPECT_EQ(child("contactSheet")->property("count").toInt(), 3);
    EXPECT_TRUE(
        child("statusMessage")->property("text").toString().startsWith(QStringLiteral("Added 3 photos")));
    // a and b show their embedded JPEGs, labelled; c shows Lucent's own render instead.
    EXPECT_EQ(visible_camera_labels(), 2);
    const LibraryItem* c = controller->library()->item_at(2);
    ASSERT_NE(c, nullptr);
    EXPECT_FALSE(c->thumbnail_is_camera_preview);
    EXPECT_EQ(c->processing, ProcessingStatus::Processed);

    // Opening a photo renders Lucent's preview, fills the histogram and replaces the thumbnail.
    ASSERT_TRUE(controller->openItem(0));
    auto* viewer = child<QQuickItem>("previewViewer");
    ASSERT_TRUE(QTest::qWaitFor([viewer] { return viewer->property("status").toInt() == kImageStatusReady; },
                                kAdapterWaitMs));
    ASSERT_TRUE(settle());
    CurrentItem& current = *controller->current();
    EXPECT_TRUE(current.has_render());
    EXPECT_TRUE(current.render_label().startsWith(QStringLiteral("Neutral v0")));
    EXPECT_EQ(current.processing_label(), QStringLiteral("Processed"));
    EXPECT_FALSE(current.thumbnail_is_camera_preview());
    EXPECT_FALSE(child("histogramPlot")->property("empty").toBool());
    EXPECT_TRUE(child<QQuickItem>("clippingSummary")->isVisible());
    EXPECT_FALSE(child<QQuickItem>("renderingLabel")->isVisible());

    // Quick navigation through the filmstrip ends on the last photo, rendered.
    controller->selectRelative(1);
    controller->selectRelative(1);
    ASSERT_TRUE(QTest::qWaitFor([viewer] { return viewer->property("status").toInt() == kImageStatusReady; },
                                kAdapterWaitMs));
    ASSERT_TRUE(settle());
    EXPECT_EQ(controller->current_index(), 2);
    EXPECT_EQ(viewer->property("source").toString(), current.preview_source());
    EXPECT_TRUE(current.has_render());

    // The Export dialog shows the name and exports with the chosen options.
    controller->set_export_format(AppController::Tiff16);
    controller->set_export_color_space(AppController::DisplayP3);
    QObject* dialog = child("exportDialog");
    ASSERT_NE(dialog, nullptr);
    QMetaObject::invokeMethod(dialog, "open");
    ASSERT_TRUE(QTest::qWaitFor([dialog] { return dialog->property("opened").toBool(); }, kAdapterWaitMs));
    EXPECT_TRUE(
        child("exportNamePreview")->property("text").toString().contains(QStringLiteral("c_Lucent.tif")));
    const QUrl out = QUrl::fromLocalFile(QString::fromStdWString((dir.path() / L"out").wstring()));
    dialog->setProperty("folder", out);
    QMetaObject::invokeMethod(dialog, "accept");
    ASSERT_TRUE(
        QTest::qWaitFor([this] { return controller->exports()->completed_count() == 1; }, kAdapterWaitMs));
    const ExportJob* job = controller->exports()->job_at(0);
    ASSERT_NE(job, nullptr);
    EXPECT_EQ(QFileInfo(job->destination_path).fileName(), QStringLiteral("c_Lucent.tif"));
    EXPECT_TRUE(QFileInfo::exists(job->destination_path));
    EXPECT_EQ(controller->last_output_folder(), out);

    controller->showView(AppController::Exports);
    ASSERT_TRUE(settle());
    EXPECT_EQ(child("exportSummary")->property("text").toString(), QStringLiteral("1 exported"));
    EXPECT_TRUE(unexpected_warnings().isEmpty()) << unexpected_warnings().join('\n').toStdString();
}

TEST_F(ApplicationShellTest, AnUnreadableFileIsListedWithItsReason) {
    dir.write_text(L"broken.dng", "not a raw file");
    ASSERT_TRUE(controller->addFiles(
        {QUrl::fromLocalFile(QString::fromStdWString((dir.path() / L"broken.dng").wstring()))}));
    ASSERT_TRUE(QTest::qWaitFor([this] { return !controller->importing(); }, kAdapterWaitMs));
    ASSERT_TRUE(settle());
    EXPECT_TRUE(
        child("statusMessage")->property("text").toString().contains(QStringLiteral("1 unsupported")));
    ASSERT_EQ(controller->import_problems().size(), 1);
    EXPECT_TRUE(controller->import_problems()[0].startsWith(QStringLiteral("broken.dng — Unsupported: ")));
    EXPECT_TRUE(child<QQuickItem>("importProblemsButton")->isVisible());

    // Opening it shows why no preview can be made.
    ASSERT_TRUE(controller->openItem(0));
    ASSERT_TRUE(QTest::qWaitFor(
        [this] { return controller->current()->processing_label() == QStringLiteral("Processing failed"); },
        kAdapterWaitMs));
    EXPECT_EQ(controller->current()->processing_detail(), QStringLiteral("Lucent cannot decode this file."));
}

}  // namespace
}  // namespace lucent::ui
