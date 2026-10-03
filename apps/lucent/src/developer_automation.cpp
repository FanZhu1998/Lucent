#include "developer_automation.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QQuickItem>
#include <QQuickWindow>

namespace lucent::ui {
namespace {

// Settling: poll the services every kPollIntervalMs without rendering (a grab can take most of a
// second on a machine without a usable GPU, which would starve the services' own timers); once
// they are idle, render a frame at most every kGrabIntervalMs until kStableFramesRequired
// consecutive frames equal their predecessor with no visible image loading.
constexpr int kPollIntervalMs = 50;
constexpr int kGrabIntervalMs = 250;
constexpr int kStableFramesRequired = 2;

// --measure: the responsiveness probe's timer interval, and how often it looks for the first
// visible thumbnail.
constexpr int kProbeIntervalMs = 5;
constexpr qint64 kWalkIntervalMs = 10;

// Milliseconds since this process was created (GetProcessTimes), or -1.
qint64 milliseconds_since_process_start() {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        return -1;
    }
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const auto ticks = [](const FILETIME& time) {
        return (static_cast<qint64>(time.dwHighDateTime) << 32) | static_cast<qint64>(time.dwLowDateTime);
    };
    return (ticks(now) - ticks(creation)) / 10000;  // 100 ns ticks
}

// QQuickImageBase::Status (qquickimagebase_p.h): Null, Ready, Loading, Error.
constexpr int kImageStatusReady = 1;
constexpr int kImageStatusLoading = 2;
constexpr int kImageStatusError = 3;

struct ImageCounts {
    int ready = 0;
    int loading = 0;
    int failed = 0;
};

void count_images(const QQuickItem& item, ImageCounts& counts) {
    if (!item.isVisible()) {
        return;
    }
    if (item.inherits("QQuickImageBase")) {
        switch (item.property("status").toInt()) {
        case kImageStatusReady:
            ++counts.ready;
            break;
        case kImageStatusLoading:
            ++counts.loading;
            break;
        case kImageStatusError:
            ++counts.failed;
            break;
        default:
            break;
        }
    }
    const QList<QQuickItem*> children = item.childItems();
    for (const QQuickItem* child : children) {
        if (child != nullptr) {
            count_images(*child, counts);
        }
    }
}

std::optional<AppController::View> parse_view(const QString& text) {
    const QString view = text.trimmed().toLower();
    if (view == u"library") {
        return AppController::Library;
    }
    if (view == u"edit") {
        return AppController::Edit;
    }
    if (view == u"exports") {
        return AppController::Exports;
    }
    return std::nullopt;
}

}  // namespace

CommandLine parse_command_line(const QStringList& arguments) {
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Lucent: offline RAW photo development.\n\n"
        "The options below other than --help and --version are for developers (automated\n"
        "verification) and are not part of the supported user interface."));
    const QCommandLineOption help = parser.addHelpOption();
    const QCommandLineOption version = parser.addVersionOption();
    const QCommandLineOption import_option(QStringLiteral("import"),
                                           QStringLiteral("Add a photo or folder (repeatable)."),
                                           QStringLiteral("path"));
    const QCommandLineOption subfolders_option(QStringLiteral("include-subfolders"),
                                               QStringLiteral("Scan --import folders recursively."));
    const QCommandLineOption open_first_option(QStringLiteral("open-first"),
                                               QStringLiteral("Open the first photo once imports finish."));
    const QCommandLineOption view_option(QStringLiteral("view"),
                                         QStringLiteral("Show a view once imports finish: library, edit "
                                                        "or exports."),
                                         QStringLiteral("view"));
    const QCommandLineOption export_option(QStringLiteral("export-all"),
                                           QStringLiteral("Export every photo into <folder> once imports "
                                                          "finish."),
                                           QStringLiteral("folder"));
    const QCommandLineOption screenshot_option(QStringLiteral("screenshot"),
                                               QStringLiteral("Save the settled window as a PNG."),
                                               QStringLiteral("file.png"));
    const QCommandLineOption quit_option(QStringLiteral("quit-after-screenshot"),
                                         QStringLiteral("Exit after --screenshot."));
    const QCommandLineOption timeout_option(QStringLiteral("settle-timeout"),
                                            QStringLiteral("Longest wait for the window to settle, in "
                                                           "milliseconds (default 60000)."),
                                            QStringLiteral("ms"));
    const QCommandLineOption format_option(QStringLiteral("export-format"),
                                           QStringLiteral("Export format: jpeg or tiff16."), QStringLiteral("format"));
    const QCommandLineOption color_option(QStringLiteral("export-color-space"),
                                          QStringLiteral("Export colour space: srgb or display-p3."),
                                          QStringLiteral("space"));
    const QCommandLineOption quality_option(QStringLiteral("export-quality"),
                                            QStringLiteral("JPEG quality, 1 to 100."), QStringLiteral("quality"));
    const QCommandLineOption zoom_option(QStringLiteral("zoom"),
                                         QStringLiteral("With --open-first, switch to this zoom once the fit "
                                                        "preview is shown: fit or 100."),
                                         QStringLiteral("zoom"));
    const QCommandLineOption dialog_option(QStringLiteral("show-export-dialog"),
                                           QStringLiteral("With --open-first, open the Export dialog before the "
                                                          "screenshot."));
    const QCommandLineOption data_root_option(
        QStringLiteral("data-root"),
        QStringLiteral("Keep the catalog and caches in <folder> instead of "
                       "the user's application data."),
        QStringLiteral("folder"));
    const QCommandLineOption measure_option(QStringLiteral("measure"),
                                            QStringLiteral("Report start-up, thumbnail, preview and "
                                                           "responsiveness timings on stderr."));
    parser.addOptions({import_option, subfolders_option, open_first_option, view_option, export_option,
                       screenshot_option, quit_option, timeout_option, format_option, color_option,
                       quality_option, zoom_option, measure_option, dialog_option, data_root_option});
    parser.addPositionalArgument(QStringLiteral("paths"), QStringLiteral("Photos or folders to add."),
                                 QStringLiteral("[paths...]"));

    CommandLine result;
    const auto fail = [&result](const QString& message) {
        result.action = CommandLine::Action::Error;
        result.message = message;
        return result;
    };

    if (!parser.parse(arguments)) {
        return fail(parser.errorText());
    }
    if (parser.isSet(help)) {
        result.action = CommandLine::Action::ShowHelp;
        result.message = parser.helpText();
        return result;
    }
    if (parser.isSet(version)) {
        result.action = CommandLine::Action::ShowVersion;
        result.message = QCoreApplication::applicationName() + u' ' + QCoreApplication::applicationVersion();
        return result;
    }

    DeveloperOptions& options = result.options;
    options.imports = parser.values(import_option) + parser.positionalArguments();
    options.include_subfolders = parser.isSet(subfolders_option);
    options.open_first = parser.isSet(open_first_option);
    if (parser.isSet(view_option)) {
        const QString text = parser.value(view_option);
        options.view = parse_view(text);
        if (!options.view) {
            return fail(QStringLiteral("Unknown view '%1': use library, edit or exports.").arg(text));
        }
    }
    if (parser.isSet(export_option)) {
        options.export_folder = parser.value(export_option);
        if (options.export_folder.trimmed().isEmpty()) {
            return fail(QStringLiteral("--export-all needs a folder."));
        }
    }
    if (parser.isSet(screenshot_option)) {
        options.screenshot_path = parser.value(screenshot_option);
        if (!options.screenshot_path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) {
            return fail(QStringLiteral("--screenshot needs a .png file name."));
        }
    }
    options.quit_after_screenshot = parser.isSet(quit_option);
    if (options.quit_after_screenshot && options.screenshot_path.isEmpty()) {
        return fail(QStringLiteral("--quit-after-screenshot needs --screenshot."));
    }
    if (parser.isSet(format_option)) {
        const QString format = parser.value(format_option).trimmed().toLower();
        if (format == u"jpeg" || format == u"jpg") {
            options.export_options.format = ExportFileFormat::Jpeg;
        } else if (format == u"tiff16" || format == u"tiff") {
            options.export_options.format = ExportFileFormat::Tiff16;
        } else {
            return fail(QStringLiteral("Unknown export format '%1': use jpeg or tiff16.").arg(format));
        }
    }
    if (parser.isSet(color_option)) {
        const QString space = parser.value(color_option).trimmed().toLower();
        if (space == u"srgb") {
            options.export_options.color_space = ExportColorSpace::Srgb;
        } else if (space == u"display-p3" || space == u"p3") {
            options.export_options.color_space = ExportColorSpace::DisplayP3;
        } else {
            return fail(QStringLiteral("Unknown export colour space '%1': use srgb or display-p3.").arg(space));
        }
    }
    if (parser.isSet(quality_option)) {
        bool ok = false;
        const int quality = parser.value(quality_option).toInt(&ok);
        if (!ok || quality < 1 || quality > 100) {
            return fail(QStringLiteral("--export-quality needs a number from 1 to 100."));
        }
        options.export_options.jpeg_quality = quality;
    }
    if (parser.isSet(zoom_option)) {
        const QString zoom = parser.value(zoom_option).trimmed().toLower();
        if (zoom == u"fit") {
            options.zoom_full = false;
        } else if (zoom == u"100" || zoom == u"100%") {
            options.zoom_full = true;
        } else {
            return fail(QStringLiteral("Unknown zoom '%1': use fit or 100.").arg(zoom));
        }
    }
    options.measure = parser.isSet(measure_option);
    options.show_export_dialog = parser.isSet(dialog_option);
    if (options.show_export_dialog && !options.open_first) {
        return fail(QStringLiteral("--show-export-dialog needs --open-first."));
    }
    if (parser.isSet(data_root_option)) {
        options.data_root = parser.value(data_root_option).trimmed();
        if (options.data_root.isEmpty()) {
            return fail(QStringLiteral("--data-root needs a folder."));
        }
    }
    if (parser.isSet(timeout_option)) {
        bool ok = false;
        const int timeout = parser.value(timeout_option).toInt(&ok);
        if (!ok || timeout <= 0) {
            return fail(QStringLiteral("--settle-timeout needs a positive number of milliseconds."));
        }
        options.settle_timeout_ms = timeout;
    }
    return result;
}

void report(const QString& message) {
    const QByteArray line = QStringLiteral("lucent: %1\n").arg(message).toUtf8();
    std::fputs(line.constData(), stderr);
    std::fflush(stderr);
}

DeveloperAutomation::DeveloperAutomation(DeveloperOptions options, AppController& controller,
                                         QQuickWindow& window, QObject* parent)
    : QObject(parent), options_(std::move(options)), controller_(controller), window_(window) {
    poll_timer_.setInterval(kPollIntervalMs);
    connect(&poll_timer_, &QTimer::timeout, this, &DeveloperAutomation::poll);
    probe_timer_.setInterval(kProbeIntervalMs);
    probe_timer_.setTimerType(Qt::PreciseTimer);
    connect(&probe_timer_, &QTimer::timeout, this, &DeveloperAutomation::probe_tick);
    connect(&controller_.library_service(), &LibraryService::import_finished, this,
            [this](const ImportSummary& summary) {
                report(QStringLiteral("import finished: %1 added (%2 limited, %3 unsupported), %4 already in "
                                      "library, %5 found again, %6 ignored, %7 failed")
                           .arg(summary.added)
                           .arg(summary.limited)
                           .arg(summary.unsupported)
                           .arg(summary.already_in_library)
                           .arg(summary.restored)
                           .arg(summary.ignored)
                           .arg(summary.failed));
                for (const QString& error : summary.errors) {
                    report(QStringLiteral("import error: %1").arg(error));
                }
                for (const QString& note : summary.notes) {
                    report(QStringLiteral("import note: %1").arg(note));
                }
                if (imports_pending_ > 0) {
                    --imports_pending_;
                }
            });
    connect(&controller_.export_service(), &ExportService::job_changed, this, [](const ExportJob& job) {
        if (job.state == ExportJobState::Completed) {
            report(QStringLiteral("export completed: %1 -> %2 (%3)")
                       .arg(job.source_name, job.destination_path, job.message));
        } else if (job.state == ExportJobState::Failed || job.state == ExportJobState::Cancelled) {
            report(QStringLiteral("export %1: %2: %3")
                       .arg(job.state == ExportJobState::Failed ? QStringLiteral("failed")
                                                                : QStringLiteral("cancelled"),
                            job.source_name, job.message));
        }
    });
}

void DeveloperAutomation::start() {
    clock_.start();
    if (options_.measure) {
        report(QStringLiteral("measure: window loaded %1 ms after the process started")
                   .arg(milliseconds_since_process_start()));
        probe_timer_.start();
    }
    if (options_.include_subfolders) {
        controller_.set_include_subfolders(true);  // show the option the imports used
    }
    controller_.set_export_options(options_.export_options);
    // One request for all paths; its import_finished ends the wait.
    if (controller_.import_paths(options_.imports, options_.include_subfolders)) {
        imports_pending_ = 1;
    }
    poll_timer_.start();
}

bool DeveloperAutomation::has_loading_images(const QQuickWindow& window) {
    ImageCounts counts;
    if (const QQuickItem* root = window.contentItem(); root != nullptr) {
        count_images(*root, counts);
    }
    return counts.loading > 0;
}

int DeveloperAutomation::viewer_status() const {
    const QQuickItem* viewer = window_.findChild<QQuickItem*>(QStringLiteral("previewViewer"));
    return viewer != nullptr ? viewer->property("status").toInt() : 0;
}

QImage DeveloperAutomation::grab() {
    QImage frame = window_.grabWindow();
    last_tick_ms_ = -1;  // the grab blocked this thread on purpose: not a responsiveness gap
    return frame;
}

void DeveloperAutomation::probe_tick() {
    const qint64 now = clock_.elapsed();
    if (last_tick_ms_ >= 0) {
        gaps_ms_.push_back(now - last_tick_ms_);
    }
    last_tick_ms_ = now;
    if (first_thumbnail_ms_ < 0 && (last_walk_ms_ < 0 || now - last_walk_ms_ >= kWalkIntervalMs)) {
        last_walk_ms_ = now;
        ImageCounts counts;
        for (const char* name : {"contactSheet", "filmstrip"}) {
            if (const QQuickItem* view = window_.findChild<QQuickItem*>(QString::fromLatin1(name)); view != nullptr) {
                count_images(*view, counts);
            }
        }
        if (counts.ready > 0) {
            first_thumbnail_ms_ = now;
            report(QStringLiteral("measure: first thumbnail visible %1 ms after the import started").arg(now));
        }
    }
}

void DeveloperAutomation::report_preview(const char* what, qint64 visible_ms) {
    QString services;
    const QVariant info = controller_.current()->render_info();
    if (const RenderInfoPtr render = info.value<RenderInfoPtr>(); render) {
        services = QStringLiteral("; latest render %1 x %2 px: engine %3 s, preview %4 s (request to pixels)")
                       .arg(render->rendered_width)
                       .arg(render->rendered_height)
                       .arg(render->render_seconds, 0, 'f', 2)
                       .arg(render->preview_seconds, 0, 'f', 2);
    }
    report(QStringLiteral("measure: %1 preview visible %2 ms after it was requested%3")
               .arg(QString::fromLatin1(what))
               .arg(visible_ms)
               .arg(services));
}

void DeveloperAutomation::report_measurements() {
    if (!options_.measure) {
        return;
    }
    if (first_thumbnail_ms_ < 0) {
        report(QStringLiteral("measure: no thumbnail became visible"));
    }
    if (gaps_ms_.empty()) {
        return;
    }
    std::vector<qint64> sorted = gaps_ms_;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&sorted](double p) {
        const auto index = static_cast<std::size_t>(p * double(sorted.size() - 1) + 0.5);
        return sorted[std::min(index, sorted.size() - 1)];
    };
    const auto over = [&sorted](qint64 limit) {
        return static_cast<qint64>(
            std::count_if(sorted.begin(), sorted.end(), [limit](qint64 gap) { return gap > limit; }));
    };
    report(QStringLiteral("measure: UI thread over %1 s: %2 ticks of a %3 ms timer; gap median %4 ms, 99th "
                          "percentile %5 ms, max %6 ms; %7 gaps over 50 ms, %8 over 100 ms (window grabs "
                          "excluded)")
               .arg(double(clock_.elapsed()) / 1000.0, 0, 'f', 1)
               .arg(static_cast<qint64>(sorted.size()))
               .arg(kProbeIntervalMs)
               .arg(percentile(0.5))
               .arg(percentile(0.99))
               .arg(sorted.back())
               .arg(over(50))
               .arg(over(100)));
}

void DeveloperAutomation::poll() {
    switch (stage_) {
    case Stage::WaitingForImports:
        if (imports_pending_ == 0) {
            after_imports();
        }
        return;
    case Stage::WaitingForFitPreview: {
        const int status = viewer_status();
        if (settle_clock_.hasExpired(options_.settle_timeout_ms)) {
            report(QStringLiteral("the preview did not load within %1 ms").arg(options_.settle_timeout_ms));
            start_settling();
            return;
        }
        if (status == 0 && (!grab_clock_.isValid() || grab_clock_.hasExpired(kGrabIntervalMs))) {
            // Nothing requested yet: the view is laid out, and so requests its preview, when a
            // frame is rendered. A session that presents no frames (headless, locked) needs one
            // rendered here. The latency clock starts with that frame (its rendering overlaps the
            // preview render, which starts during it).
            open_ms_ = clock_.elapsed();
            static_cast<void>(grab());
            grab_clock_.start();
            return;
        }
        if (status != kImageStatusReady && status != kImageStatusError) {
            return;
        }
        if (status == kImageStatusError) {
            report(QStringLiteral("the fit preview failed: %1").arg(controller_.current()->processing_detail()));
        } else if (options_.measure) {
            report_preview("fit", clock_.elapsed() - open_ms_);
        }
        QQuickItem* viewer = window_.findChild<QQuickItem*>(QStringLiteral("previewViewer"));
        if (status == kImageStatusReady && options_.zoom_full.value_or(false) && viewer != nullptr) {
            zoom_ms_ = clock_.elapsed();
            QMetaObject::invokeMethod(viewer, "setFit", Q_ARG(QVariant, false), Q_ARG(QVariant, QVariant()),
                                      Q_ARG(QVariant, QVariant()));
            grab_clock_.start();
            stage_ = Stage::WaitingForZoomedPreview;
            return;
        }
        start_settling();
        return;
    }
    case Stage::WaitingForZoomedPreview: {
        const int status = viewer_status();
        if (settle_clock_.hasExpired(options_.settle_timeout_ms)) {
            report(QStringLiteral("the 100 % preview did not load within %1 ms").arg(options_.settle_timeout_ms));
            start_settling();
            return;
        }
        if (status == kImageStatusLoading) {
            zoom_loading_seen_ = true;
        } else if (!zoom_loading_seen_ && (!grab_clock_.isValid() || grab_clock_.hasExpired(kGrabIntervalMs))) {
            static_cast<void>(grab());  // lay the view out if the zoom has not requested yet
            grab_clock_.start();
        } else if (status == kImageStatusError) {
            report(QStringLiteral("the 100 % preview failed: %1").arg(controller_.current()->processing_detail()));
            start_settling();
        } else if (status == kImageStatusReady && zoom_loading_seen_) {
            if (options_.measure) {
                report_preview("100 %", clock_.elapsed() - zoom_ms_);
            }
            start_settling();
        }
        return;
    }
    case Stage::Settling: {
        if (settle_clock_.hasExpired(options_.settle_timeout_ms)) {
            report(QStringLiteral("the window did not settle within %1 ms").arg(options_.settle_timeout_ms));
            take_screenshot(false);
            return;
        }
        if (controller_.busy()) {
            stable_frames_ = 0;
            return;
        }
        if (grab_clock_.isValid() && !grab_clock_.hasExpired(kGrabIntervalMs)) {
            return;
        }
        // Rendering a frame also polishes the scene (layouts, then the image requests they cause).
        // Qt Quick presents frames only while the window is really on screen, so an occluded window
        // or a locked session would otherwise keep a stale layout until the final grab.
        QImage frame = grab();
        grab_clock_.start();
        const bool settled = !frame.isNull() && frame == last_frame_ && !has_loading_images(window_);
        last_frame_ = std::move(frame);
        stable_frames_ = settled ? stable_frames_ + 1 : 0;
        if (stable_frames_ >= kStableFramesRequired) {
            take_screenshot(true);
        }
        return;
    }
    case Stage::Done:
        if (options_.measure && options_.screenshot_path.isEmpty()) {
            // Without a screenshot, --measure reports once the services are idle.
            if (!controller_.busy() && !has_loading_images(window_)) {
                poll_timer_.stop();
                probe_timer_.stop();
                report_measurements();
            }
            return;
        }
        poll_timer_.stop();
        return;
    }
}

void DeveloperAutomation::after_imports() {
    settle_clock_.start();
    if (options_.open_first) {
        if (controller_.openItem(0)) {
            open_ms_ = clock_.elapsed();
        } else {
            report(QStringLiteral("--open-first: the library is empty"));
        }
    }
    if (options_.view) {
        controller_.showView(*options_.view);
    }
    if (!options_.export_folder.isEmpty()) {
        QList<ItemId> items;
        const LibraryModel& library = *controller_.library();
        for (int row = 0; row < library.count(); ++row) {
            items << library.item_at(row)->id;
        }
        if (!controller_.export_items(items, options_.export_folder, options_.export_options)) {
            report(QStringLiteral("--export-all: nothing to export"));
        }
    }
    const bool follow_preview = open_ms_ >= 0 && controller_.current_view() == AppController::Edit &&
                                (options_.measure || options_.zoom_full.has_value());
    if (follow_preview) {
        stage_ = Stage::WaitingForFitPreview;
        return;
    }
    start_settling();
}

void DeveloperAutomation::start_settling() {
    if (options_.screenshot_path.isEmpty()) {
        stage_ = Stage::Done;
        if (!options_.measure) {
            poll_timer_.stop();
        }
        return;
    }
    if (options_.show_export_dialog) {
        if (QObject* dialog = window_.findChild<QObject*>(QStringLiteral("exportDialog")); dialog != nullptr) {
            QMetaObject::invokeMethod(dialog, "open");
        }
    }
    stage_ = Stage::Settling;
}

void DeveloperAutomation::take_screenshot(bool settled) {
    stage_ = Stage::Done;
    poll_timer_.stop();
    probe_timer_.stop();
    report_measurements();

    ImageCounts counts;
    if (const QQuickItem* root = window_.contentItem(); root != nullptr) {
        count_images(*root, counts);
    }
    report(QStringLiteral("visible images: %1 ready, %2 loading, %3 failed")
               .arg(counts.ready)
               .arg(counts.loading)
               .arg(counts.failed));

    const QString path = QFileInfo(options_.screenshot_path).absoluteFilePath();
    const QImage image = last_frame_.isNull() ? grab() : last_frame_;
    bool saved = false;
    if (image.isNull()) {
        report(QStringLiteral("could not grab the window"));
    } else if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        report(QStringLiteral("could not create the folder of %1").arg(QDir::toNativeSeparators(path)));
    } else if (!image.save(path, "PNG")) {
        report(QStringLiteral("could not save %1").arg(QDir::toNativeSeparators(path)));
    } else {
        saved = true;
        report(QStringLiteral("screenshot saved: %1 (%2 x %3 px)")
                   .arg(QDir::toNativeSeparators(path))
                   .arg(image.width())
                   .arg(image.height()));
    }
    last_frame_ = QImage();
    emit screenshotTaken(saved, path);
    if (options_.quit_after_screenshot) {
        emit quitRequested(!saved ? 1 : (settled ? 0 : 3));
    }
}

}  // namespace lucent::ui
