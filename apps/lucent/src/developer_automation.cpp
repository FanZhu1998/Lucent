#include "developer_automation.hpp"

// clang-format off: psapi.h needs the declarations of windows.h.
#include <windows.h>
#include <psapi.h>
// clang-format on

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QQuickItem>
#include <QQuickWindow>

#include "models/batch_presentation.hpp"

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

// W12 measurement hooks: the largest photo count of --cancel-batch-after / --pause-batch-after and
// the longest delay of --resume-after / --cancel-auto-after.
constexpr int kMaxBatchPhotos = 1'000'000;
constexpr int kMaxHookDelayMs = 600'000;

constexpr double kMebibyte = 1024.0 * 1024.0;

double filetime_seconds(const FILETIME& time) {
    const auto ticks = (static_cast<quint64>(time.dwHighDateTime) << 32) | static_cast<quint64>(time.dwLowDateTime);
    return static_cast<double>(ticks) / 1e7;  // 100 ns ticks
}

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

// "x:y,x:y,..." with values in [0, 1].
std::optional<QList<QPointF>> parse_curve(const QString& text) {
    QList<QPointF> points;
    for (const QString& pair : text.split(u',', Qt::SkipEmptyParts)) {
        const QStringList xy = pair.split(u':');
        bool x_ok = false;
        bool y_ok = false;
        const double x = xy.size() == 2 ? xy[0].trimmed().toDouble(&x_ok) : 0.0;
        const double y = xy.size() == 2 ? xy[1].trimmed().toDouble(&y_ok) : 0.0;
        if (!x_ok || !y_ok || x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0) {
            return std::nullopt;
        }
        points << QPointF(x, y);
    }
    if (points.size() < kMinCurvePoints || points.size() > kMaxCurvePoints) {
        return std::nullopt;
    }
    return points;
}

// The visible item named `name` in the visual item tree (which, unlike the QObject tree, also holds
// the delegates of repeaters and views); nullptr when there is none.
QQuickItem* find_visible_item(QQuickItem* root, const QString& name) {
    if (root == nullptr || !root->isVisible()) {
        return nullptr;
    }
    if (root->objectName() == name) {
        return root;
    }
    const QList<QQuickItem*> children = root->childItems();
    for (QQuickItem* child : children) {
        if (QQuickItem* found = find_visible_item(child, name)) {
            return found;
        }
    }
    return nullptr;
}

// "a,b,c,..." as doubles; nullopt unless there are `minimum` to `maximum` finite numbers.
std::optional<QList<double>> parse_numbers(const QString& text, qsizetype minimum, qsizetype maximum) {
    QList<double> values;
    for (const QString& part : text.split(u',')) {
        bool ok = false;
        const double value = part.trimmed().toDouble(&ok);
        if (!ok || !std::isfinite(value)) {
            return std::nullopt;
        }
        values << value;
    }
    if (values.size() < minimum || values.size() > maximum) {
        return std::nullopt;
    }
    return values;
}

// "x1,y1;x2,y2;..." (normalised upright positions within [-1, 2]).
std::optional<QList<QPointF>> parse_stroke(const QString& text) {
    QList<QPointF> points;
    for (const QString& pair : text.split(u';', Qt::SkipEmptyParts)) {
        const std::optional<QList<double>> xy = parse_numbers(pair, 2, 2);
        if (!xy || (*xy)[0] < -1.0 || (*xy)[0] > 2.0 || (*xy)[1] < -1.0 || (*xy)[1] > 2.0) {
            return std::nullopt;
        }
        points << QPointF((*xy)[0], (*xy)[1]);
    }
    if (points.isEmpty()) {
        return std::nullopt;
    }
    return points;
}

// An --add-mask spec: "brush", "linear[:x1,y1,x2,y2[,feather]]" or "radial[:cx,cy,rx,ry[,angle[,feather]]]".
bool valid_mask_spec(const QString& spec) {
    const QString kind = spec.section(u':', 0, 0).trimmed();
    const QString values = spec.section(u':', 1);
    if (kind == u"brush") {
        return values.isEmpty();
    }
    if (kind == u"linear") {
        return values.isEmpty() || parse_numbers(values, 4, 5).has_value();
    }
    if (kind == u"radial") {
        return values.isEmpty() || parse_numbers(values, 4, 6).has_value();
    }
    return false;
}

bool is_lock_key(const QString& key) {
    static const QStringList kLocks{QStringLiteral("whiteBalance"), QStringLiteral("exposure"),
                                    QStringLiteral("contrast"),     QStringLiteral("highlights"),
                                    QStringLiteral("shadows"),      QStringLiteral("toneCurve"),
                                    QStringLiteral("saturation"),   QStringLiteral("vibrance")};
    return kLocks.contains(key);
}

template <typename T>
T percentile_of(std::vector<T> values, double p) {
    if (values.empty()) {
        return T{};
    }
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(p * double(values.size() - 1) + 0.5);
    return values[std::min(index, values.size() - 1)];
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
                                           QStringLiteral("Export format: jpeg or tiff16."),
                                           QStringLiteral("format"));
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
    const QCommandLineOption about_option(
        QStringLiteral("show-about"),
        QStringLiteral("Open the About dialog (camera support database, support "
                       "states) before the screenshot."));
    const QCommandLineOption data_root_option(
        QStringLiteral("data-root"),
        QStringLiteral("Keep the catalog and caches in <folder> instead of "
                       "the user's application data."),
        QStringLiteral("folder"));
    const QCommandLineOption set_option(QStringLiteral("set"),
                                        QStringLiteral("With --open-first, make an edit once the photo is "
                                                       "shown: control=value (repeatable)."),
                                        QStringLiteral("control=value"));
    const QCommandLineOption drag_option(QStringLiteral("drag"),
                                         QStringLiteral("With --open-first, drag this slider with synthetic "
                                                        "mouse events."),
                                         QStringLiteral("control"));
    const QCommandLineOption drag_steps_option(QStringLiteral("drag-steps"),
                                               QStringLiteral("Mouse moves of --drag (default 60)."),
                                               QStringLiteral("n"));
    const QCommandLineOption compare_option(QStringLiteral("compare"),
                                            QStringLiteral("With --open-first, show before (as shot) or "
                                                           "split before the screenshot."),
                                            QStringLiteral("mode"));
    const QCommandLineOption clipping_option(QStringLiteral("clipping"),
                                             QStringLiteral("With --open-first, turn the clipping overlay "
                                                            "on before the screenshot."));
    const QCommandLineOption scroll_option(QStringLiteral("scroll-panel"),
                                           QStringLiteral("With --open-first, scroll the Edit view's side panel to "
                                                          "this fraction (0 top, 1 bottom) before the screenshot."),
                                           QStringLiteral("fraction"));
    const QCommandLineOption batch_option(QStringLiteral("export-batch"),
                                          QStringLiteral("Export every photo of <folder> as one batch once imports "
                                                         "finish (needs --to)."),
                                          QStringLiteral("folder"));
    const QCommandLineOption to_option(QStringLiteral("to"), QStringLiteral("The output folder of --export-batch."),
                                       QStringLiteral("folder"));
    const QCommandLineOption preset_option(QStringLiteral("preset"),
                                           QStringLiteral("The preset of --export-batch: jpeg (default) or tiff."),
                                           QStringLiteral("preset"));
    const QCommandLineOption batch_dialog_option(QStringLiteral("show-batch-dialog"),
                                                 QStringLiteral("With --export-batch, open the Export batch dialog "
                                                                "instead of starting."));
    const QCommandLineOption running_option(QStringLiteral("screenshot-running"),
                                            QStringLiteral("With --export-batch, also save the window while the "
                                                           "batch runs."),
                                            QStringLiteral("file.png"));
    const QCommandLineOption done_option(QStringLiteral("quit-when-done"),
                                         QStringLiteral("With --export-batch, exit when the batch has finished."));
    const QCommandLineOption auto_option(QStringLiteral("auto"),
                                         QStringLiteral("With --open-first, run Auto on the photo once it is shown "
                                                        "and report what it did."));
    const QCommandLineOption automatic_option(QStringLiteral("automatic"),
                                              QStringLiteral("The automatic settings of --export-batch: off "
                                                             "(default), auto or analyze-only."),
                                              QStringLiteral("mode"));
    const QCommandLineOption consistent_option(QStringLiteral("keep-consistent"),
                                               QStringLiteral("With --automatic, keep related shots consistent."));
    const QCommandLineOption turn_option(QStringLiteral("turn"),
                                         QStringLiteral("With --open-first, turn the photo: cw, ccw or flip (repeatable)."),
                                         QStringLiteral("turn"));
    const QCommandLineOption straighten_option(QStringLiteral("straighten"),
                                               QStringLiteral("With --open-first, straighten by <degrees> (-45..45)."),
                                               QStringLiteral("degrees"));
    const QCommandLineOption crop_option(QStringLiteral("crop"),
                                         QStringLiteral("With --open-first, crop to l,t,w,h (fractions of the "
                                                        "straightened image)."),
                                         QStringLiteral("l,t,w,h"));
    const QCommandLineOption add_mask_option(QStringLiteral("add-mask"),
                                             QStringLiteral("With --open-first, add a mask: brush, "
                                                            "linear[:x1,y1,x2,y2[,feather]] or "
                                                            "radial[:cx,cy,rx,ry[,angle[,feather]]] (repeatable)."),
                                             QStringLiteral("spec"));
    const QCommandLineOption mask_set_option(QStringLiteral("mask-set"),
                                             QStringLiteral("A local control of the masks the automation adds: "
                                                            "control=value (repeatable)."),
                                             QStringLiteral("control=value"));
    const QCommandLineOption mask_exposure_option(QStringLiteral("mask-exposure"),
                                                  QStringLiteral("The exposure of the masks the automation adds, in EV."),
                                                  QStringLiteral("ev"));
    const QCommandLineOption brush_option(QStringLiteral("brush"),
                                          QStringLiteral("With --open-first, paint a brush stroke through "
                                                         "x1,y1;x2,y2;... (normalised; repeatable)."),
                                          QStringLiteral("points"));
    const QCommandLineOption brush_size_option(QStringLiteral("brush-size"),
                                               QStringLiteral("The --brush size, percent of the long edge "
                                                              "(default 3)."),
                                               QStringLiteral("percent"));
    const QCommandLineOption mask_overlay_option(QStringLiteral("mask-overlay"),
                                                 QStringLiteral("With --open-first, show the selected mask's "
                                                                "overlay before the screenshot."));
    const QCommandLineOption tool_option(QStringLiteral("tool"),
                                         QStringLiteral("With --open-first, open a tool before the screenshot: crop "
                                                        "or masks."),
                                         QStringLiteral("tool"));
    const QCommandLineOption measure_option(QStringLiteral("measure"),
                                            QStringLiteral("Report start-up, thumbnail, preview and "
                                                           "responsiveness timings on stderr."));
    // W12 measurement hooks.
    const QCommandLineOption resume_batch_option(
        QStringLiteral("resume-batch"),
        QStringLiteral("Follow the library's newest batch with work left until it has finished (instead of "
                       "--export-batch)."));
    const QCommandLineOption cancel_batch_option(
        QStringLiteral("cancel-batch-after"),
        QStringLiteral("With --export-batch or --resume-batch, cancel the batch once <n> photos are done."),
        QStringLiteral("n"));
    const QCommandLineOption pause_batch_option(
        QStringLiteral("pause-batch-after"),
        QStringLiteral("With --export-batch or --resume-batch, pause the batch once <n> photos are done, then "
                       "resume it."),
        QStringLiteral("n"));
    const QCommandLineOption resume_after_option(
        QStringLiteral("resume-after"),
        QStringLiteral("With --pause-batch-after, resume this many milliseconds after the pause took effect "
                       "(default 5000)."),
        QStringLiteral("ms"));
    const QCommandLineOption cancel_auto_option(
        QStringLiteral("cancel-auto-after"),
        QStringLiteral("With --auto, cancel the run this many milliseconds after it started."), QStringLiteral("ms"));
    parser.addOptions(
        {import_option,        subfolders_option,   open_first_option,   view_option,         export_option,
         screenshot_option,    quit_option,         timeout_option,      format_option,       color_option,
         quality_option,       zoom_option,         measure_option,      dialog_option,       data_root_option,
         set_option,           drag_option,         drag_steps_option,   compare_option,      clipping_option,
         scroll_option,        batch_option,        to_option,           preset_option,       batch_dialog_option,
         running_option,       done_option,         auto_option,         automatic_option,    consistent_option,
         turn_option,          straighten_option,   crop_option,         add_mask_option,     mask_set_option,
         mask_exposure_option, brush_option,        brush_size_option,   mask_overlay_option, tool_option,
         about_option,         resume_batch_option, cancel_batch_option, pause_batch_option,  resume_after_option,
         cancel_auto_option});
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
    options.show_about = parser.isSet(about_option);
    if (parser.isSet(data_root_option)) {
        options.data_root = parser.value(data_root_option).trimmed();
        if (options.data_root.isEmpty()) {
            return fail(QStringLiteral("--data-root needs a folder."));
        }
    }
    for (const QString& edit : parser.values(set_option)) {
        const qsizetype equals = edit.indexOf(u'=');
        const QString key = equals > 0 ? edit.first(equals).trimmed() : QString();
        const QString value = equals > 0 ? edit.sliced(equals + 1).trimmed() : QString();
        bool valid = !key.isEmpty() && !value.isEmpty();
        if (valid && key == u"curve") {
            valid = parse_curve(value).has_value();
        } else if (valid && key == u"lock") {
            valid = is_lock_key(value);
        } else if (valid) {
            bool number = false;
            QString(value).remove(u'+').toDouble(&number);
            valid = EditSession::is_control(key) && number;
        }
        if (!valid) {
            return fail(QStringLiteral("--set needs control=value with a slider key and a number, curve=x:y,..., or "
                                       "lock=<control> (got '%1').")
                            .arg(edit));
        }
        options.edits.emplace_back(key, value);
    }
    if (parser.isSet(drag_option)) {
        options.drag = parser.value(drag_option).trimmed();
        if (!EditSession::is_control(options.drag)) {
            return fail(QStringLiteral("Unknown --drag control '%1'.").arg(options.drag));
        }
    }
    if (parser.isSet(drag_steps_option)) {
        bool ok = false;
        options.drag_steps = parser.value(drag_steps_option).toInt(&ok);
        if (!ok || options.drag_steps < 10 || options.drag_steps > 1000) {
            return fail(QStringLiteral("--drag-steps needs a number from 10 to 1000."));
        }
    }
    if (parser.isSet(compare_option)) {
        options.compare = parser.value(compare_option).trimmed().toLower();
        if (options.compare != u"before" && options.compare != u"split") {
            return fail(QStringLiteral("Unknown --compare mode '%1': use before or split.").arg(options.compare));
        }
    }
    options.clipping = parser.isSet(clipping_option);
    if (parser.isSet(scroll_option)) {
        bool ok = false;
        options.panel_scroll = parser.value(scroll_option).toDouble(&ok);
        if (!ok || *options.panel_scroll < 0.0 || *options.panel_scroll > 1.0) {
            return fail(QStringLiteral("--scroll-panel needs a fraction from 0 to 1."));
        }
    }
    options.run_auto = parser.isSet(auto_option);
    // W9 geometry and masks.
    for (const QString& turn : parser.values(turn_option)) {
        const QString value = turn.trimmed().toLower();
        if (value != u"cw" && value != u"ccw" && value != u"flip") {
            return fail(QStringLiteral("Unknown --turn '%1': use cw, ccw or flip.").arg(turn));
        }
        options.turns << value;
    }
    if (parser.isSet(straighten_option)) {
        bool ok = false;
        const double degrees = parser.value(straighten_option).toDouble(&ok);
        if (!ok || !std::isfinite(degrees) || std::abs(degrees) > kMaxStraighten) {
            return fail(QStringLiteral("--straighten needs degrees from -45 to 45."));
        }
        options.straighten = degrees;
    }
    if (parser.isSet(crop_option)) {
        const std::optional<QList<double>> values = parse_numbers(parser.value(crop_option), 4, 4);
        if (!values || (*values)[0] < 0.0 || (*values)[1] < 0.0 || (*values)[2] <= 0.0 || (*values)[3] <= 0.0 ||
            (*values)[0] + (*values)[2] > 1.0 + 1e-9 || (*values)[1] + (*values)[3] > 1.0 + 1e-9) {
            return fail(QStringLiteral("--crop needs l,t,w,h: fractions of the image that stay inside it."));
        }
        options.crop = std::array<double, 4>{(*values)[0], (*values)[1], (*values)[2], (*values)[3]};
    }
    for (const QString& spec : parser.values(add_mask_option)) {
        if (!valid_mask_spec(spec.trimmed())) {
            return fail(QStringLiteral("--add-mask needs brush, linear[:x1,y1,x2,y2[,feather]] or "
                                       "radial[:cx,cy,rx,ry[,angle[,feather]]] (got '%1').")
                            .arg(spec));
        }
        options.add_masks << spec.trimmed();
    }
    auto mask_values = parser.values(mask_set_option);
    if (parser.isSet(mask_exposure_option)) {
        mask_values << QStringLiteral("maskExposure=") + parser.value(mask_exposure_option);
    }
    for (const QString& entry : mask_values) {
        const qsizetype equals = entry.indexOf(u'=');
        const QString key = equals > 0 ? entry.first(equals).trimmed() : QString();
        const QString value = equals > 0 ? entry.sliced(equals + 1).trimmed() : QString();
        bool number = false;
        QString(value).remove(u'+').toDouble(&number);
        if (!key.startsWith(u"mask") || !EditSession::is_control(key) || !number) {
            return fail(QStringLiteral("--mask-set needs maskExposure, maskContrast, ... = a number (got '%1').")
                            .arg(entry));
        }
        options.mask_values.emplace_back(key, value);
    }
    for (const QString& stroke : parser.values(brush_option)) {
        const std::optional<QList<QPointF>> points = parse_stroke(stroke);
        if (!points) {
            return fail(QStringLiteral("--brush needs x1,y1;x2,y2;... normalised positions (got '%1').").arg(stroke));
        }
        options.brushes.push_back(*points);
    }
    if (parser.isSet(brush_size_option)) {
        bool ok = false;
        options.brush_size = parser.value(brush_size_option).toDouble(&ok);
        if (!ok || options.brush_size < 0.1 || options.brush_size > 25.0) {
            return fail(QStringLiteral("--brush-size needs a percentage from 0.1 to 25."));
        }
    }
    options.mask_overlay = parser.isSet(mask_overlay_option);
    if (parser.isSet(tool_option)) {
        options.tool = parser.value(tool_option).trimmed().toLower();
        if (options.tool != u"crop" && options.tool != u"masks") {
            return fail(QStringLiteral("Unknown --tool '%1': use crop or masks.").arg(options.tool));
        }
    }
    const bool w9_editing = !options.turns.isEmpty() || options.straighten || options.crop ||
                            !options.add_masks.isEmpty() || !options.mask_values.empty() || !options.brushes.empty() ||
                            options.mask_overlay || !options.tool.isEmpty();
    if ((!options.edits.empty() || !options.drag.isEmpty() || !options.compare.isEmpty() || options.clipping ||
         options.panel_scroll || options.run_auto || w9_editing) &&
        !options.open_first) {
        return fail(QStringLiteral("--set, --drag, --compare, --clipping, --scroll-panel, --auto and the geometry and "
                                   "mask options need --open-first."));
    }
    options.resume_batch = parser.isSet(resume_batch_option);
    if (parser.isSet(batch_option)) {
        if (options.resume_batch) {
            return fail(QStringLiteral("--resume-batch follows a batch that exists: use it without --export-batch."));
        }
        options.batch_folder = parser.value(batch_option).trimmed();
        if (options.batch_folder.isEmpty()) {
            return fail(QStringLiteral("--export-batch needs a folder."));
        }
        options.batch_to = parser.value(to_option).trimmed();
        if (options.batch_to.isEmpty()) {
            return fail(QStringLiteral("--export-batch needs --to <folder>."));
        }
    } else if (parser.isSet(to_option) || parser.isSet(preset_option) || parser.isSet(batch_dialog_option) ||
               parser.isSet(automatic_option) || parser.isSet(consistent_option)) {
        return fail(QStringLiteral("--to, --preset, --show-batch-dialog, --automatic and --keep-consistent need "
                                   "--export-batch."));
    } else if (!options.resume_batch && (parser.isSet(running_option) || parser.isSet(done_option) ||
                                         parser.isSet(cancel_batch_option) || parser.isSet(pause_batch_option))) {
        return fail(QStringLiteral("--screenshot-running, --quit-when-done, --cancel-batch-after and "
                                   "--pause-batch-after need --export-batch or --resume-batch."));
    }
    if (parser.isSet(automatic_option)) {
        const QString mode = parser.value(automatic_option).trimmed().toLower();
        if (mode == u"auto") {
            options.batch_automatic = BatchAutomatic::Auto;
        } else if (mode == u"analyze-only" || mode == u"analyse-only") {
            options.batch_automatic = BatchAutomatic::AnalyzeOnly;
        } else if (mode != u"off") {
            return fail(QStringLiteral("Unknown --automatic mode '%1': use off, auto or analyze-only.").arg(mode));
        }
    }
    options.keep_consistent = parser.isSet(consistent_option);
    if (options.keep_consistent && options.batch_automatic == BatchAutomatic::Off) {
        return fail(QStringLiteral("--keep-consistent needs --automatic auto or analyze-only."));
    }
    if (parser.isSet(preset_option)) {
        const QString preset = parser.value(preset_option).trimmed().toLower();
        if (preset == u"tiff" || preset == u"tiff16") {
            options.batch_tiff = true;
        } else if (preset != u"jpeg" && preset != u"jpg") {
            return fail(QStringLiteral("Unknown preset '%1': use jpeg or tiff.").arg(preset));
        }
    }
    options.show_batch_dialog = parser.isSet(batch_dialog_option);
    options.quit_when_done = parser.isSet(done_option);
    if (parser.isSet(running_option)) {
        options.running_screenshot_path = parser.value(running_option);
        if (!options.running_screenshot_path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) {
            return fail(QStringLiteral("--screenshot-running needs a .png file name."));
        }
    }
    // W12 measurement hooks.
    const auto bounded = [&parser](const QCommandLineOption& option, int maximum) -> std::optional<int> {
        bool ok = false;
        const int value = parser.value(option).trimmed().toInt(&ok);
        return ok && value >= 0 && value <= maximum ? std::optional<int>(value) : std::nullopt;
    };
    if (parser.isSet(cancel_batch_option)) {
        options.cancel_batch_after = bounded(cancel_batch_option, kMaxBatchPhotos);
        if (!options.cancel_batch_after) {
            return fail(
                QStringLiteral("--cancel-batch-after needs a number of photos from 0 to %1.").arg(kMaxBatchPhotos));
        }
    }
    if (parser.isSet(pause_batch_option)) {
        options.pause_batch_after = bounded(pause_batch_option, kMaxBatchPhotos);
        if (!options.pause_batch_after) {
            return fail(
                QStringLiteral("--pause-batch-after needs a number of photos from 0 to %1.").arg(kMaxBatchPhotos));
        }
    }
    if (parser.isSet(resume_after_option)) {
        if (!options.pause_batch_after) {
            return fail(QStringLiteral("--resume-after needs --pause-batch-after."));
        }
        const std::optional<int> ms = bounded(resume_after_option, kMaxHookDelayMs);
        if (!ms) {
            return fail(QStringLiteral("--resume-after needs milliseconds from 0 to %1.").arg(kMaxHookDelayMs));
        }
        options.resume_after_ms = *ms;
    }
    if (parser.isSet(cancel_auto_option)) {
        if (!options.run_auto) {
            return fail(QStringLiteral("--cancel-auto-after needs --auto."));
        }
        options.cancel_auto_after_ms = bounded(cancel_auto_option, kMaxHookDelayMs);
        if (!options.cancel_auto_after_ms) {
            return fail(QStringLiteral("--cancel-auto-after needs milliseconds from 0 to %1.").arg(kMaxHookDelayMs));
        }
    }
    if (options.show_batch_dialog && (options.quit_when_done || !options.running_screenshot_path.isEmpty() ||
                                      options.cancel_batch_after || options.pause_batch_after)) {
        return fail(QStringLiteral("--show-batch-dialog does not start the batch: --screenshot-running, "
                                   "--quit-when-done, --cancel-batch-after and --pause-batch-after need it to run."));
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
    drag_timer_.setInterval(16);  // a 60 Hz mouse
    drag_timer_.setTimerType(Qt::PreciseTimer);
    connect(&drag_timer_, &QTimer::timeout, this, &DeveloperAutomation::drag_tick);
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
    connect(&controller_.batch_service(), &BatchService::submitted, this,
            [this](quint64 token, BatchId batch, const QString& error) {
                if (token == 0 || token != batch_token_) {
                    return;
                }
                if (batch == 0) {
                    report(QStringLiteral("batch not created: %1").arg(error));
                    batch_failed_to_start_ = true;
                    finish_when_done();
                    return;
                }
                batch_id_ = batch;
                batch_created_ms_ = clock_.elapsed();
                report(QStringLiteral("batch %1 created").arg(batch));
                if (const auto row = controller_.batches()->row_of(batch)) {
                    on_batch_changed(*controller_.batches()->batch_at(*row));
                }
            });
    connect(&controller_.batch_service(), &BatchService::submit_progress, this,
            [this](quint64 token, const QString& text, double) {
                if (token != 0 && token == batch_token_) {
                    report(QStringLiteral("batch preparing: %1").arg(text));
                }
            });
    connect(&controller_.batch_service(), &BatchService::batch_changed, this, &DeveloperAutomation::on_batch_changed);
    connect(&controller_.batch_service(), &BatchService::report_ready, this,
            [this](BatchId batch, const BatchReportInfo& report) {
                if (batch != 0 && batch == batch_id_ && batch_report_requested_ && !batch_report_printed_) {
                    report_batch_entries(report);
                }
            });
    connect(&controller_.batch_service(), &BatchService::batches_listed, this, &DeveloperAutomation::on_batches_listed);
    connect(&controller_.edit_service(), &EditService::auto_finished, this, [this](const AutoOutcome& outcome) {
        if (auto_started_ && !auto_outcome_ && outcome.item == controller_.edit()->item()) {
            auto_outcome_ = outcome;
            auto_finished_ms_ = auto_clock_.elapsed();  // not the poll's time: a frame may render first
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
    // W10 (ADR 0018): which camera support database the session uses (the packaging smoke run
    // checks the installed one is found and verified).
    const CameraSupportInfo support = controller_.library_service().camera_support();
    if (support.available) {
        report(QStringLiteral("camera support database %1 (qualified %2): %3 modes, %4 verified, from %5")
                   .arg(support.version, support.qualified_on)
                   .arg(support.modes)
                   .arg(support.verified)
                   .arg(support.location));
    } else {
        report(QStringLiteral("camera support database unavailable: %1")
                   .arg(support.problem.isEmpty() ? QStringLiteral("none configured") : support.problem));
    }
    for (const QString& note : support.notes) {
        report(QStringLiteral("camera support note: %1").arg(note));
    }
    if (options_.measure) {
        report(QStringLiteral("measure: window loaded %1 ms after the process started")
                   .arg(milliseconds_since_process_start()));
        probe_timer_.start();
        // W12: the first frame the window presented. frameSwapped is emitted on the scene graph's render
        // thread (with the threaded render loop): handled there, once, touching nothing of this object
        // (the window, the connection's context, may outlive it).
        auto seen = std::make_shared<std::atomic<bool>>(false);
        connect(
            &window_, &QQuickWindow::frameSwapped, &window_,
            [seen] {
                if (!seen->exchange(true)) {
                    report(QStringLiteral("measure: first frame presented %1 ms after the process started")
                               .arg(milliseconds_since_process_start()));
                }
            },
            Qt::DirectConnection);
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
        if (drag_measuring_) {
            drag_gaps_ms_.push_back(now - last_tick_ms_);
        }
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
    if (!options_.measure || measurements_reported_) {
        return;
    }
    measurements_reported_ = true;
    if (first_thumbnail_ms_ < 0) {
        report(QStringLiteral("measure: no thumbnail became visible"));
    }
    if (gaps_ms_.empty()) {
        report_process();
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
    report_process();
}

void DeveloperAutomation::report_process() {
    if (!options_.measure || process_reported_) {
        return;
    }
    process_reported_ = true;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters)) ||
        !GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        report(QStringLiteral("measure: process: unavailable"));
        return;
    }
    report(QStringLiteral("measure: process: peak_working_set_mib=%1 peak_commit_mib=%2 commit_mib=%3 cpu_s=%4 "
                          "user_s=%5 kernel_s=%6 wall_s=%7")
               .arg(double(counters.PeakWorkingSetSize) / kMebibyte, 0, 'f', 1)
               .arg(double(counters.PeakPagefileUsage) / kMebibyte, 0, 'f', 1)
               .arg(double(counters.PrivateUsage) / kMebibyte, 0, 'f', 1)
               .arg(filetime_seconds(user) + filetime_seconds(kernel), 0, 'f', 2)
               .arg(filetime_seconds(user), 0, 'f', 2)
               .arg(filetime_seconds(kernel), 0, 'f', 2)
               .arg(double(milliseconds_since_process_start()) / 1000.0, 0, 'f', 2));
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
            // W12: from adding the photo (the import request) to its first RAW-rendered frame on screen;
            // the camera's own preview shown meanwhile does not count.
            report(QStringLiteral("measure: first RAW preview shown %1 ms after the import started").arg(clock_.elapsed()));
        }
        QQuickItem* viewer = window_.findChild<QQuickItem*>(QStringLiteral("previewViewer"));
        if (status == kImageStatusReady && wants_editing()) {
            stage_ = Stage::Editing;
            return;
        }
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
        if (const EditSession& edit = *controller_.edit(); edit.active()) {
            // An edit session: a quick proxy frame first, then the full-resolution final frame.
            if (!edit.frame_error().isEmpty()) {
                report(QStringLiteral("the 100 % preview failed: %1").arg(edit.frame_error()));
                start_settling();
            } else if (edit.frame_full_resolution() && edit.frame_phase() == u"final" && !edit.busy()) {
                if (options_.measure) {
                    report_preview("100 %", clock_.elapsed() - zoom_ms_);
                }
                start_settling();
            } else if (!grab_clock_.isValid() || grab_clock_.hasExpired(kGrabIntervalMs)) {
                static_cast<void>(grab());  // lay the view out (and load the frames) without a display
                grab_clock_.start();
            }
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
    case Stage::Editing:
        edit_step();
        return;
    case Stage::Settling: {
        if (settle_clock_.hasExpired(options_.settle_timeout_ms)) {
            report(QStringLiteral("the window did not settle within %1 ms").arg(options_.settle_timeout_ms));
            take_screenshot(false);
            return;
        }
        if (controller_.busy() || batch_pending() || controller_.batch_report()->loading() ||
            (options_.show_batch_dialog && controller_.batch_setup()->planning())) {
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
        if ((!options_.batch_folder.isEmpty() || options_.resume_batch) && options_.screenshot_path.isEmpty()) {
            return;  // the batch's own signals finish the run (--quit-when-done)
        }
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

void DeveloperAutomation::start_batch() {
    const QString folder = QDir::cleanPath(QDir::current().absoluteFilePath(options_.batch_folder));
    const QString to = QDir::cleanPath(QDir::current().absoluteFilePath(options_.batch_to));
    BatchSetupModel& setup = *controller_.batch_setup();
    const auto configure = [&] {
        setup.setIncludeSubfolders(options_.include_subfolders);
        setup.setOutputFolder(QUrl::fromLocalFile(to));
        setup.setPreset(options_.batch_tiff ? BatchSetupModel::Tiff16 : BatchSetupModel::Jpeg);
        setup.setDisplayP3(options_.export_options.color_space == ExportColorSpace::DisplayP3);
        setup.setAutomatic(static_cast<BatchSetupModel::Automatic>(options_.batch_automatic));
        setup.setKeepConsistent(options_.keep_consistent);
    };
    if (options_.show_batch_dialog) {
        // As the Library's "Export a folder" does: the dialog opens with the folder, then the options.
        if (!controller_.openFolderBatchExport(QUrl::fromLocalFile(folder))) {
            report(QStringLiteral("--show-batch-dialog: batches are unavailable: %1")
                       .arg(controller_.batch_service().unavailable_reason()));
            return;
        }
        configure();
        return;
    }
    setup.prepare(controller_.library()->selected_items(), controller_.library()->all_items());
    setup.prepare_folder(QUrl::fromLocalFile(folder), options_.include_subfolders);
    configure();
    batch_submit_ms_ = clock_.elapsed();
    if (!controller_.startBatch()) {
        report(QStringLiteral("--export-batch: the batch cannot start: %1").arg(setup.start_hint()));
        batch_failed_to_start_ = true;
        finish_when_done();
        return;
    }
    batch_token_ = controller_.last_batch_token();
    QString mode = options_.batch_tiff ? QStringLiteral("16-bit TIFF") : QStringLiteral("JPEG sRGB q95");
    if (options_.batch_automatic == BatchAutomatic::Auto) {
        mode += QStringLiteral(", automatic settings");
    } else if (options_.batch_automatic == BatchAutomatic::AnalyzeOnly) {
        mode = QStringLiteral("analyse only");
    }
    if (options_.keep_consistent) {
        mode += QStringLiteral(", related shots kept consistent");
    }
    report(QStringLiteral("batch started: %1 -> %2 (%3)")
               .arg(QDir::toNativeSeparators(folder), QDir::toNativeSeparators(to), mode));
}

void DeveloperAutomation::report_batch_entries(const BatchReportInfo& info) {
    batch_report_printed_ = true;
    if (!info.ok) {
        report(QStringLiteral("batch %1 report: %2").arg(batch_id_).arg(info.error));
        finish_when_done();
        return;
    }
    report(QStringLiteral("batch %1 report: %2 exported, %3 analysed, %4 skipped, %5 failed, %6 for review")
               .arg(batch_id_)
               .arg(info.exported)
               .arg(info.analysed)
               .arg(info.skipped)
               .arg(info.failed)
               .arg(info.review));
    for (const BatchReportEntry& entry : info.entries) {
        QString line =
            QStringLiteral("report: %1: %2").arg(entry.source_name, presentation::outcome_label(entry.outcome));
        if (!entry.output.isEmpty()) {
            line += QStringLiteral(" -> ") + QFileInfo(entry.output).fileName();
        }
        if (entry.automatic && entry.revision_number > 0) {
            line += QStringLiteral("; automatic revision %1, confidence %2, attempt %3")
                        .arg(entry.revision_number)
                        .arg(entry.confidence, 0, 'f', 2)
                        .arg(entry.attempt);
            if (!entry.group.isEmpty()) {
                line += QStringLiteral(", group %1").arg(entry.group);
            }
        }
        if (!entry.reason.isEmpty() && entry.outcome != BatchOutcome::Exported &&
            entry.outcome != BatchOutcome::Analysed) {
            line += QStringLiteral("; ") + entry.reason;
        }
        report(line);
        for (const QString& review : entry.review) {
            report(QStringLiteral("report:   review: %1").arg(review));
        }
        for (const QString& flag : entry.flags) {
            if (!entry.review.contains(flag)) {
                report(QStringLiteral("report:   note: %1").arg(flag));
            }
        }
        for (const QString& lock : entry.locks) {
            report(QStringLiteral("report:   lock: %1").arg(lock));
        }
    }
    finish_when_done();
}

void DeveloperAutomation::on_batches_listed(const QList<BatchSnapshot>& batches) {
    if (!options_.resume_batch || resume_listed_ || batch_failed_to_start_) {
        return;
    }
    resume_listed_ = true;
    // Newest first: the newest batch with work left, as the Exports view lists them.
    const auto found = std::find_if(batches.begin(), batches.end(), [](const BatchSnapshot& batch) {
        return batch.state != BatchState::Draft && !batch.finished_state();
    });
    if (found == batches.end()) {
        report(QStringLiteral("--resume-batch: no batch of this library has work left"));
        batch_failed_to_start_ = true;
        finish_when_done();
        return;
    }
    batch_id_ = found->id;
    batch_created_ms_ = clock_.elapsed();
    report(QStringLiteral("batch %1 found with work left: %2 of %3 done, %4 remaining%5")
               .arg(found->id)
               .arg(found->completed + found->failed + found->skipped + found->cancelled)
               .arg(found->total)
               .arg(found->remaining())
               .arg(found->state == BatchState::Paused ? QStringLiteral(" (paused: resumed)") : QString()));
    if (found->state == BatchState::Paused) {
        controller_.batch_service().resume(found->id);
    }
    on_batch_changed(*found);
}

void DeveloperAutomation::apply_batch_hooks(const BatchSnapshot& batch, qint64 done) {
    BatchService& service = controller_.batch_service();
    // --pause-batch-after: pause once; resume --resume-after ms after its last running photo finished.
    if (options_.pause_batch_after && !batch_pause_requested_ && batch.state == BatchState::Running &&
        done >= *options_.pause_batch_after && batch.remaining() > 0) {
        batch_pause_requested_ = true;
        batch_pause_ms_ = clock_.elapsed();
        batch_pause_running_ = batch.running;
        report(QStringLiteral("batch %1: pause requested after %2 photos done, %3 running")
                   .arg(batch.id)
                   .arg(done)
                   .arg(batch.running));
        service.pause(batch.id);
        return;  // this snapshot is from before the pause
    }
    if (batch_pause_requested_ && !batch_pause_settled_ && batch.state == BatchState::Paused && batch.running == 0) {
        batch_pause_settled_ = true;
        if (options_.measure) {
            report(QStringLiteral("measure: batch %1 pause: requested_ms=%2 running=%3 settled_after_ms=%4 "
                                  "resume_after_ms=%5")
                       .arg(batch.id)
                       .arg(batch_pause_ms_)
                       .arg(batch_pause_running_)
                       .arg(clock_.elapsed() - batch_pause_ms_)
                       .arg(options_.resume_after_ms));
        }
        const BatchId id = batch.id;
        QTimer::singleShot(options_.resume_after_ms, this, [this, id] {
            report(QStringLiteral("batch %1: resume requested").arg(id));
            controller_.batch_service().resume(id);
        });
    }
    // --cancel-batch-after: cancel once, while a photo is being processed.
    if (options_.cancel_batch_after && !batch_cancel_requested_ && done >= *options_.cancel_batch_after &&
        batch.running > 0 && (batch.state == BatchState::Running || batch.state == BatchState::Paused)) {
        batch_cancel_requested_ = true;
        batch_cancel_ms_ = clock_.elapsed();
        batch_cancel_done_ = done;
        batch_cancel_running_ = batch.running;
        report(QStringLiteral("batch %1: cancel requested after %2 photos done, %3 running")
                   .arg(batch.id)
                   .arg(done)
                   .arg(batch.running));
        service.cancel(batch.id);
    }
}

void DeveloperAutomation::report_batch_timeline(const BatchSnapshot& batch) {
    if (!options_.measure) {
        return;
    }
    // This session's share: a batch --resume-batch found had photos done already.
    const BatchSnapshot& first = batch_first_seen_ ? *batch_first_seen_ : batch;
    const qint64 exported = batch.completed - first.completed;
    const qint64 failed = batch.failed - first.failed;
    const qint64 skipped = batch.skipped - first.skipped;
    const qint64 cancelled = batch.cancelled - first.cancelled;
    const double seconds = batch_created_ms_ >= 0 && batch_finished_ms_ >= batch_created_ms_
                               ? double(batch_finished_ms_ - batch_created_ms_) / 1000.0
                               : -1.0;
    const double per_minute = seconds > 0.0 ? double(exported) * 60.0 / seconds : -1.0;
    report(QStringLiteral("measure: batch %1 timeline: submit_ms=%2 created_ms=%3 first_done_ms=%4 finished_ms=%5 "
                          "done=%6 exported=%7 failed=%8 skipped=%9 cancelled=%10 seconds=%11 per_minute=%12")
               .arg(batch.id)
               .arg(batch_submit_ms_)
               .arg(batch_created_ms_)
               .arg(batch_first_done_ms_)
               .arg(batch_finished_ms_)
               .arg(exported + failed + skipped + cancelled)
               .arg(exported)
               .arg(failed)
               .arg(skipped)
               .arg(cancelled)
               .arg(seconds, 0, 'f', 3)
               .arg(per_minute, 0, 'f', 2));
    if (batch_cancel_requested_) {
        report(QStringLiteral("measure: batch %1 cancel: requested_ms=%2 done=%3 running=%4 stopped_after_ms=%5")
                   .arg(batch.id)
                   .arg(batch_cancel_ms_)
                   .arg(batch_cancel_done_)
                   .arg(batch_cancel_running_)
                   .arg(batch_finished_ms_ - batch_cancel_ms_));
    }
    if (const QString scheduler = controller_.batch_service().measurement_summary(); !scheduler.isEmpty()) {
        report(QStringLiteral("measure: batch %1 scheduler: %2").arg(batch.id).arg(scheduler));
    }
}

void DeveloperAutomation::on_batch_changed(const BatchSnapshot& batch) {
    if (batch_id_ == 0 || batch.id != batch_id_) {
        return;
    }
    if (!batch_first_seen_) {
        batch_first_seen_ = batch;
    }
    const qint64 done = batch.completed + batch.failed + batch.skipped + batch.cancelled;
    const qint64 done_when_seen = batch_first_seen_->completed + batch_first_seen_->failed +
                                  batch_first_seen_->skipped + batch_first_seen_->cancelled;
    if (batch_first_done_ms_ < 0 && done > done_when_seen) {
        batch_first_done_ms_ = clock_.elapsed();
    }
    apply_batch_hooks(batch, done);
    if (done != batch_last_done_) {
        batch_last_done_ = done;
        report(QStringLiteral("batch %1: %2 of %3 done (%4 %5, %6 failed, %7 skipped), %8 remaining")
                   .arg(batch.id)
                   .arg(done)
                   .arg(batch.total)
                   .arg(batch.completed)
                   .arg(batch.automatic == BatchAutomatic::AnalyzeOnly ? QStringLiteral("analysed")
                                                                       : QStringLiteral("exported"))
                   .arg(batch.failed)
                   .arg(batch.skipped)
                   .arg(batch.remaining()));
    }
    if (batch.attention != BatchAttention::None) {
        report(QStringLiteral("batch %1 needs attention: %2").arg(batch.id).arg(batch.attention_message));
    }
    if (batch.finished_state()) {
        batch_finished_ = true;
        if (batch_finished_ms_ < 0) {
            batch_finished_ms_ = clock_.elapsed();  // the review count may arrive with a later snapshot
        }
        if (batch.review >= 0 && !batch_reported_) {
            batch_reported_ = true;
            report(QStringLiteral("batch %1 finished (%2): %3 %4, %5 skipped, %6 failed, %7 cancelled, %8 "
                                  "recommended for review; output folder %9")
                       .arg(batch.id)
                       .arg(batch.state == BatchState::Cancelled ? QStringLiteral("cancelled")
                                                                 : QStringLiteral("completed"))
                       .arg(batch.completed)
                       .arg(batch.automatic == BatchAutomatic::AnalyzeOnly ? QStringLiteral("analysed")
                                                                           : QStringLiteral("exported"))
                       .arg(batch.skipped)
                       .arg(batch.failed)
                       .arg(batch.cancelled)
                       .arg(batch.review)
                       .arg(batch.folder));
            if (!batch.report_path.isEmpty()) {
                report(QStringLiteral("batch %1 report: %2").arg(batch.id).arg(batch.report_path));
            }
            report_batch_timeline(batch);
        }
        if (batch_reported_ && !batch_report_requested_) {
            // Its rows: what happened to each photo (and what Auto did).
            batch_report_requested_ = true;
            controller_.batch_service().request_report(batch.id);
        }
    }
    if (!running_shot_taken_ && !options_.running_screenshot_path.isEmpty() && batch.completed >= 1 &&
        batch.remaining() >= 1 && batch.state == BatchState::Running) {
        take_running_screenshot();
    }
    finish_when_done();
}

bool DeveloperAutomation::batch_pending() const {
    if ((options_.batch_folder.isEmpty() && !options_.resume_batch) || options_.show_batch_dialog ||
        batch_failed_to_start_) {
        return false;
    }
    if (batch_id_ == 0) {
        return true;  // still being prepared (--resume-batch: not listed yet)
    }
    if (!batch_reported_ || !batch_report_printed_) {
        return true;  // running, or its final review count or its report is still being read
    }
    return controller_.batch_report()->loading();
}

void DeveloperAutomation::take_running_screenshot() {
    running_shot_taken_ = true;
    const QString path = QFileInfo(options_.running_screenshot_path).absoluteFilePath();
    const QImage image = grab();
    if (image.isNull() || !QDir().mkpath(QFileInfo(path).absolutePath()) || !image.save(path, "PNG")) {
        report(QStringLiteral("could not save the running screenshot %1").arg(QDir::toNativeSeparators(path)));
        return;
    }
    report(QStringLiteral("running screenshot saved: %1 (%2 x %3 px)")
               .arg(QDir::toNativeSeparators(path))
               .arg(image.width())
               .arg(image.height()));
}

void DeveloperAutomation::finish_when_done() {
    if (!options_.quit_when_done) {
        return;
    }
    if (!batch_failed_to_start_ && (!batch_finished_ || !batch_reported_ || !batch_report_printed_)) {
        return;
    }
    if (!options_.screenshot_path.isEmpty() && !screenshot_done_) {
        return;  // the screenshot quits (or finishes) first
    }
    int code = 0;
    if (batch_failed_to_start_) {
        code = 5;
    } else if (const auto row = controller_.batches()->row_of(batch_id_)) {
        code = controller_.batches()->batch_at(*row)->failed > 0 ? 4 : 0;
    }
    report_measurements();  // --measure: the UI thread and process lines before the exit
    emit quitRequested(code);
}

void DeveloperAutomation::after_imports() {
    settle_clock_.start();
    if (options_.resume_batch && !resume_listed_) {
        controller_.batch_service().refresh();  // on_batches_listed finds the batch to follow
    }
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
    if (!options_.batch_folder.isEmpty()) {
        start_batch();
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
                                (options_.measure || options_.zoom_full.has_value() || wants_editing());
    if (follow_preview) {
        stage_ = Stage::WaitingForFitPreview;
        return;
    }
    start_settling();
}

bool DeveloperAutomation::wants_editing() const {
    return !options_.edits.empty() || !options_.drag.isEmpty() || !options_.compare.isEmpty() || options_.clipping ||
           options_.panel_scroll.has_value() || options_.run_auto || !options_.turns.isEmpty() ||
           options_.straighten.has_value() || options_.crop.has_value() || !options_.add_masks.isEmpty() ||
           !options_.brushes.empty() || options_.mask_overlay || !options_.tool.isEmpty();
}

std::deque<std::pair<QString, std::function<bool()>>> DeveloperAutomation::geometry_and_mask_steps() {
    std::deque<std::pair<QString, std::function<bool()>>> steps;
    EditSession& edit = *controller_.edit();
    for (const QString& turn : options_.turns) {
        steps.emplace_back(QStringLiteral("turn=") + turn, [&edit, turn] {
            if (turn == u"cw") {
                edit.crop()->rotateRight();
            } else if (turn == u"ccw") {
                edit.crop()->rotateLeft();
            } else {
                edit.crop()->flip();
            }
            return true;
        });
    }
    if (options_.straighten) {
        const double degrees = *options_.straighten;
        steps.emplace_back(QStringLiteral("straighten=%1").arg(degrees), [&edit, degrees] {
            return edit.setValueText(QStringLiteral("straighten"), QString::number(degrees, 'f', 2));
        });
    }
    if (options_.crop) {
        const std::array<double, 4> crop = *options_.crop;
        steps.emplace_back(QStringLiteral("crop"), [&edit, crop] {
            edit.crop()->setCrop(crop[0], crop[1], crop[2], crop[3]);
            return true;
        });
    }
    for (const QString& spec : options_.add_masks) {
        steps.emplace_back(QStringLiteral("add-mask=") + spec, [this, spec] { return add_mask(spec); });
        if (!options_.mask_values.empty()) {
            steps.emplace_back(QStringLiteral("mask controls"), [this] { return apply_mask_values(); });
        }
    }
    for (const QList<QPointF>& stroke : options_.brushes) {
        steps.emplace_back(QStringLiteral("brush (%1 points)").arg(stroke.size()),
                           [this, stroke] { return paint_brush(stroke); });
        if (!options_.mask_values.empty()) {
            steps.emplace_back(QStringLiteral("mask controls"), [this] { return apply_mask_values(); });
        }
    }
    return steps;
}

bool DeveloperAutomation::add_mask(const QString& spec) {
    EditSession& edit = *controller_.edit();
    const QString kind = spec.section(u':', 0, 0).trimmed();
    if (!edit.masks()->add(kind)) {
        report(QStringLiteral("--add-mask %1: the mask could not be added").arg(spec));
        return false;
    }
    const QString values_text = spec.section(u':', 1);
    if (values_text.isEmpty()) {
        return true;
    }
    const QList<double> values = parse_numbers(values_text, 4, 6).value_or(QList<double>{});
    EditSettings next = edit.settings();
    for (EditMask& mask : next.masks) {
        if (mask.id != edit.selected_mask()) {
            continue;
        }
        if (mask.kind == MaskKind::Radial) {
            mask.center = QPointF(values[0], values[1]);
            mask.radius_x = values[2];
            mask.radius_y = values[3];
            mask.angle_degrees = values.size() > 4 ? values[4] : 0.0;
            mask.feather = values.size() > 5 ? values[5] : mask.feather;
        } else if (mask.kind == MaskKind::Linear) {
            mask.start = QPointF(values[0], values[1]);
            mask.end = QPointF(values[2], values[3]);
            mask.feather = values.size() > 4 ? values[4] : mask.feather;
        }
    }
    edit.edit_settings(next);  // one more revision: the shape as given
    return true;
}

bool DeveloperAutomation::apply_mask_values() {
    EditSession& edit = *controller_.edit();
    bool changed = false;
    for (const auto& [key, value] : options_.mask_values) {
        const double before = edit.value(key);
        if (!edit.setValueText(key, value)) {
            report(QStringLiteral("--mask-set %1=%2: not accepted").arg(key, value));
        }
        changed |= edit.value(key) != before;
    }
    return changed;
}

bool DeveloperAutomation::paint_brush(const QList<QPointF>& points) {
    EditSession& edit = *controller_.edit();
    MaskEditor& masks = *edit.masks();
    masks.startBrush();
    edit.setValue(QStringLiteral("brushSize"), options_.brush_size);
    static_cast<void>(grab());  // lay the overlay out (a session that presents no frames)
    QQuickItem* overlay = find_visible_item(window_.contentItem(), QStringLiteral("maskOverlay"));
    if (!masks.brush_active() || overlay == nullptr) {
        report(QStringLiteral("--brush: the masks tool cannot paint on this photo"));
        return false;
    }
    // The stroke's corners on screen, then --drag-steps positions evenly along it.
    const QRectF image = overlay->property("imageRect").toRectF();
    QList<QPointF> corners;
    for (const QPointF& point : points) {
        const std::optional<QPointF> at = masks.displayed_at(point);
        if (!at) {
            return false;
        }
        corners << overlay->mapToScene(QPointF(image.x() + at->x() * image.width(), image.y() + at->y() * image.height()));
    }
    std::vector<double> lengths{0.0};
    for (qsizetype i = 1; i < corners.size(); ++i) {
        lengths.push_back(lengths.back() + std::hypot(corners[i].x() - corners[i - 1].x(),
                                                      corners[i].y() - corners[i - 1].y()));
    }
    drag_path_.clear();
    const int moves = options_.drag_steps;
    for (int step = 0; step <= moves; ++step) {
        const double at = lengths.back() * step / moves;
        qsizetype segment = 1;
        while (segment + 1 < corners.size() && lengths[static_cast<std::size_t>(segment)] < at) {
            ++segment;
        }
        if (corners.size() == 1) {
            drag_path_ << corners.front();
            continue;
        }
        const double from = lengths[static_cast<std::size_t>(segment - 1)];
        const double span = std::max(1e-9, lengths[static_cast<std::size_t>(segment)] - from);
        const double t = std::clamp((at - from) / span, 0.0, 1.0);
        drag_path_ << corners[segment - 1] + (corners[segment] - corners[segment - 1]) * t;
    }
    edit.reset_latencies();
    finals_before_ = edit.final_frames();
    drag_label_ = QStringLiteral("brush stroke");
    drag_moves_ = moves;
    drag_settings_ = edit.settings();
    drag_gaps_ms_.clear();
    last_tick_ms_ = -1;
    drag_step_ = 0;
    drag_active_ = true;
    drag_measuring_ = true;
    drag_start_ms_ = clock_.elapsed();
    if (!probe_timer_.isActive()) {
        probe_timer_.start();
    }
    send_mouse(QEvent::MouseButtonPress, drag_path_.front());
    drag_timer_.start();
    return false;  // the stroke's release waits for its final frame
}

bool DeveloperAutomation::auto_step() {
    if (!options_.run_auto || auto_done_) {
        return false;
    }
    EditSession& edit = *controller_.edit();
    if (!auto_started_) {
        if (!edit.can_run_auto()) {
            report(QStringLiteral("--auto: Auto is not available for this photo"));
            auto_done_ = true;
            return false;
        }
        auto_started_ = true;
        edit.reset_latencies();
        finals_before_ = edit.final_frames();
        auto_clock_.start();
        report(QStringLiteral("auto: started on %1").arg(controller_.current()->file_name()));
        edit.runAuto();
        if (options_.cancel_auto_after_ms) {
            // W12: Cancel as the Automatic settings panel does, unless the run has ended already.
            QTimer::singleShot(*options_.cancel_auto_after_ms, this, [this] {
                if (auto_outcome_ || auto_cancel_ms_ >= 0) {
                    return;
                }
                EditSession& session = *controller_.edit();
                auto_cancel_ms_ = auto_clock_.elapsed();
                auto_cancel_progress_ = session.automatic()->progress();
                report(QStringLiteral("auto: cancel requested %1 ms after the start (%2 % of the run)")
                           .arg(auto_cancel_ms_)
                           .arg(auto_cancel_progress_ * 100.0, 0, 'f', 0));
                session.cancelAuto();
            });
        }
        return true;
    }
    if (!auto_outcome_) {
        return true;  // still running (the poll waits while the controller is busy anyway)
    }
    auto_done_ = true;
    report(QStringLiteral("auto: finished after %1 ms: %2").arg(auto_clock_.elapsed()).arg(auto_outcome_->message));
    if (options_.measure) {
        report(QStringLiteral("measure: auto run: ended_after_ms=%1 cancelled=%2")
                   .arg(auto_finished_ms_)
                   .arg(auto_outcome_->cancelled ? 1 : 0));
        if (auto_cancel_ms_ >= 0) {
            report(QStringLiteral("measure: auto cancel: requested_after_ms=%1 ended_after_request_ms=%2 cancelled=%3 "
                                  "progress=%4")
                       .arg(auto_cancel_ms_)
                       .arg(auto_finished_ms_ - auto_cancel_ms_)
                       .arg(auto_outcome_->cancelled ? 1 : 0)
                       .arg(auto_cancel_progress_, 0, 'f', 3));
        }
    }
    if (auto_outcome_->saved && auto_outcome_->made_current) {
        // The frame of the automatic revision, then the summary (edit_step reports it once it came).
        edit_waiting_ = true;
        edit_label_ = QStringLiteral("auto");
        return true;
    }
    report_auto_summary();
    return false;
}

void DeveloperAutomation::report_auto_summary() {
    const AutoSettingsModel& automatic = *controller_.edit()->automatic();
    if (!automatic.has_summary()) {
        report(QStringLiteral("auto: the current revision is not automatic"));
        return;
    }
    const QString confidence = automatic.confidence_text();
    report(QStringLiteral("auto: %1; %2review recommended: %3")
               .arg(automatic.origin(), confidence.isEmpty() ? QString() : confidence + QStringLiteral("; "),
                    automatic.review() ? QStringLiteral("yes") : QStringLiteral("no")));
    report(QStringLiteral("auto: changed: %1").arg(automatic.changed_text()));
    for (const QString& reason : automatic.review_reasons()) {
        report(QStringLiteral("auto: review: %1").arg(reason));
    }
    if (!automatic.locked_text().isEmpty()) {
        report(QStringLiteral("auto: %1").arg(automatic.locked_text()));
    }
    for (const QString& note : automatic.lock_notes()) {
        report(QStringLiteral("auto: lock: %1").arg(note));
    }
    if (!automatic.fallback_text().isEmpty()) {
        report(QStringLiteral("auto: fallback: %1 (%2)").arg(automatic.fallback_text(), automatic.fallback_detail()));
    }
    for (const QString& note : automatic.notes()) {
        report(QStringLiteral("auto: note: %1").arg(note));
    }
}

bool DeveloperAutomation::apply_edit(const QString& key, const QString& value) {
    EditSession& edit = *controller_.edit();
    if (key == u"curve") {
        const std::optional<QList<QPointF>> points = parse_curve(value);
        if (!points || !edit.can_edit()) {
            return false;
        }
        // As the curve editor makes them: the end points moved, the others added on the curve and
        // moved; one gesture, one revision.
        edit.beginGesture();
        edit.moveCurvePoint(0, points->front().x(), points->front().y());
        edit.moveCurvePoint(static_cast<int>(edit.settings().tone_curve.size()) - 1, points->back().x(),
                            points->back().y());
        for (qsizetype i = 1; i + 1 < points->size(); ++i) {
            const int index = edit.addCurvePoint((*points)[i].x());
            if (index >= 0) {
                edit.moveCurvePoint(index, (*points)[i].x(), (*points)[i].y());
            }
        }
        edit.endGesture();
        return true;
    }
    if (key == u"lock") {
        edit.setLocked(value, true);
        return true;
    }
    return edit.setValueText(key, value);
}

void DeveloperAutomation::edit_step() {
    EditSession& edit = *controller_.edit();
    if (settle_clock_.hasExpired(options_.settle_timeout_ms)) {
        report(QStringLiteral("the edits did not finish within %1 ms").arg(options_.settle_timeout_ms));
        drag_timer_.stop();
        drag_measuring_ = false;
        start_settling();
        return;
    }
    if (drag_active_ || controller_.busy() || edit.busy()) {
        return;
    }
    if (edit_waiting_) {
        if (edit.final_frames() <= finals_before_ && edit.frame_error().isEmpty()) {
            return;  // the final frame of the edit has not arrived yet
        }
        edit_waiting_ = false;
        if (drag_measuring_) {
            drag_measuring_ = false;
            if (options_.measure) {
                report_drag();
            }
            if (!drag_path_.isEmpty()) {
                for (const EditMask& mask : edit.settings().masks) {
                    if (mask.id == edit.selected_mask() && !mask.strokes.isEmpty()) {
                        report(QStringLiteral("brush: %1 mouse moves became a stroke of %2 points (%3, %4 strokes)")
                                   .arg(drag_moves_)
                                   .arg(mask.strokes.back().points.size())
                                   .arg(edit.masks()->selected_name())
                                   .arg(mask.strokes.size()));
                    }
                }
            }
        } else if (auto_started_ && edit_label_ == u"auto") {
            report_auto_summary();  // the automatic revision is shown
        } else {
            report_edit_latency(edit_label_);
        }
    }
    if (next_edit_ < options_.edits.size()) {
        const auto& [key, value] = options_.edits[next_edit_++];
        edit.reset_latencies();
        finals_before_ = edit.final_frames();
        edit_label_ = key + u'=' + value;
        if (!apply_edit(key, value)) {
            report(QStringLiteral("--set %1: the edit was not accepted").arg(edit_label_));
            return;
        }
        // A lock is a revision but changes no pixels: no frame follows.
        edit_waiting_ = key != u"lock";
        return;
    }
    if (!steps_built_) {
        steps_built_ = true;
        steps_ = geometry_and_mask_steps();
    }
    if (!steps_.empty()) {
        auto [label, step] = std::move(steps_.front());
        steps_.pop_front();
        edit.reset_latencies();
        finals_before_ = edit.final_frames();
        edit_label_ = label;
        edit_waiting_ = step();
        if (!edit.message().isEmpty()) {
            report(QStringLiteral("edit message after %1: %2").arg(label, edit.message()));
        }
        return;
    }
    if (!options_.drag.isEmpty() && !drag_done_) {
        start_drag();
        return;
    }
    if (auto_step()) {
        return;
    }
    if (options_.mask_overlay && !edit.mask_overlay()) {
        edit.reset_latencies();
        finals_before_ = edit.final_frames();
        edit_label_ = QStringLiteral("mask overlay");
        edit.set_mask_overlay(true);
        edit_waiting_ = edit.selected_mask() != 0;
        return;
    }
    if (!options_.tool.isEmpty() && edit.tool() != options_.tool) {
        edit.reset_latencies();
        finals_before_ = edit.final_frames();
        edit_label_ = QStringLiteral("tool=") + options_.tool;
        edit.setTool(options_.tool);
        edit_waiting_ = options_.tool == u"crop";  // the uncropped frame
        return;
    }
    if (options_.clipping) {
        controller_.set_clipping_overlay(true);
    }
    if (options_.panel_scroll) {
        if (auto* panel = window_.findChild<QQuickItem*>(QStringLiteral("editPanel")); panel != nullptr) {
            const double range = std::max(0.0, panel->property("contentHeight").toDouble() - panel->height());
            panel->setProperty("contentY", range * *options_.panel_scroll);
        }
    }
    if (options_.compare == u"before") {
        edit.set_compare(true);
    } else if (options_.compare == u"split") {
        edit.set_split(true);
    }
    if (!edit.message().isEmpty()) {
        report(QStringLiteral("edit message: %1").arg(edit.message()));
    }
    QQuickItem* viewer = window_.findChild<QQuickItem*>(QStringLiteral("previewViewer"));
    if (options_.zoom_full.value_or(false) && viewer != nullptr) {
        zoom_ms_ = clock_.elapsed();
        QMetaObject::invokeMethod(viewer, "setFit", Q_ARG(QVariant, false), Q_ARG(QVariant, QVariant()),
                                  Q_ARG(QVariant, QVariant()));
        grab_clock_.invalidate();
        stage_ = Stage::WaitingForZoomedPreview;
        return;
    }
    start_settling();
}

void DeveloperAutomation::report_edit_latency(const QString& what) {
    if (!options_.measure) {
        return;
    }
    const EditSession& edit = *controller_.edit();
    const std::vector<double>& published = edit.published_latencies();
    const std::vector<double>& shown = edit.shown_latencies();
    report(QStringLiteral("measure: edit %1: %2 quick and %3 final frames; first frame published %4 ms and last "
                          "%5 ms after the change; last frame shown %6 ms after it")
               .arg(what)
               .arg(edit.quick_frames())
               .arg(edit.final_frames() - finals_before_)
               .arg(published.empty() ? -1.0 : published.front(), 0, 'f', 0)
               .arg(published.empty() ? -1.0 : published.back(), 0, 'f', 0)
               .arg(shown.empty() ? -1.0 : shown.back(), 0, 'f', 0));
}

void DeveloperAutomation::send_mouse(QEvent::Type type, QPointF position) {
    const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const Qt::MouseButtons buttons = type == QEvent::MouseButtonRelease ? Qt::MouseButtons() : Qt::LeftButton;
    QMouseEvent event(type, position, position, window_.mapToGlobal(position), button, buttons, Qt::NoModifier,
                      QPointingDevice::primaryPointingDevice());
    QCoreApplication::sendEvent(&window_, &event);
}

void DeveloperAutomation::start_drag() {
    drag_done_ = true;
    QQuickItem* slider = find_visible_item(window_.contentItem(), QStringLiteral("slider_") + options_.drag);
    auto* handle = slider != nullptr ? slider->property("handle").value<QQuickItem*>() : nullptr;
    if (slider == nullptr || handle == nullptr || !slider->isVisible() || !slider->isEnabled()) {
        report(QStringLiteral("--drag: the %1 slider is not available").arg(options_.drag));
        return;
    }
    // The slider in view: the side panel scrolls it into its upper third (the W9 sliders sit below the
    // tone curve).
    if (auto* panel = window_.findChild<QQuickItem*>(QStringLiteral("editPanel")); panel != nullptr) {
        if (auto* content = panel->property("contentItem").value<QQuickItem*>(); content != nullptr) {
            const double top = slider->mapToItem(content, QPointF(0.0, 0.0)).y();
            const double range = std::max(0.0, panel->property("contentHeight").toDouble() - panel->height());
            const double visible_top = panel->property("contentY").toDouble();
            if (top < visible_top || top + slider->height() > visible_top + panel->height()) {
                panel->setProperty("contentY", std::clamp(top - panel->height() / 3.0, 0.0, range));
            }
        }
    }
    // Lay the scene out first (a session that presents no frames, see --measure).
    static_cast<void>(grab());
    drag_from_ = handle->mapToScene(QPointF(handle->width() / 2.0, handle->height() / 2.0));
    const double travel = slider->property("availableWidth").toDouble() / 3.0;
    const bool rightwards = slider->property("visualPosition").toDouble() <= 0.5;
    drag_to_ = drag_from_ + QPointF(rightwards ? travel : -travel, 0.0);
    drag_path_.clear();
    drag_label_ = options_.drag;
    drag_moves_ = options_.drag_steps;
    EditSession& edit = *controller_.edit();
    drag_settings_ = edit.settings();
    edit.reset_latencies();
    finals_before_ = edit.final_frames();
    drag_gaps_ms_.clear();
    last_tick_ms_ = -1;
    drag_step_ = 0;
    drag_active_ = true;
    drag_measuring_ = true;
    drag_start_ms_ = clock_.elapsed();
    if (!probe_timer_.isActive()) {
        probe_timer_.start();
    }
    send_mouse(QEvent::MouseButtonPress, drag_from_);
    drag_timer_.start();
}

void DeveloperAutomation::drag_tick() {
    ++drag_step_;
    const double t = std::min(1.0, double(drag_step_) / double(drag_moves_));
    const QPointF at = drag_path_.isEmpty()
                           ? drag_from_ + (drag_to_ - drag_from_) * t
                           : drag_path_[std::min<qsizetype>(drag_step_, drag_path_.size() - 1)];
    if (drag_step_ <= drag_moves_) {
        send_mouse(QEvent::MouseMove, at);
        return;
    }
    send_mouse(QEvent::MouseButtonRelease, at);
    drag_timer_.stop();
    drag_active_ = false;
    drag_release_ms_ = clock_.elapsed();
    if (controller_.edit()->settings() == drag_settings_) {
        // Nothing moved (the pointer missed the control): no revision, no frame to wait for.
        report(QStringLiteral("--drag %1: the pointer changed nothing").arg(drag_label_));
        drag_measuring_ = false;
        return;
    }
    edit_waiting_ = true;  // until the commit's final frame
}

void DeveloperAutomation::report_drag() {
    const EditSession& edit = *controller_.edit();
    const std::vector<double>& shown = edit.shown_latencies();
    const std::vector<double>& published = edit.published_latencies();
    const std::vector<qint64>& gaps = drag_gaps_ms_;
    const qint64 now = clock_.elapsed();
    const auto over = [&gaps](qint64 limit) {
        return static_cast<qint64>(
            std::count_if(gaps.begin(), gaps.end(), [limit](qint64 gap) { return gap > limit; }));
    };
    const double shown_max = shown.empty() ? 0.0 : *std::max_element(shown.begin(), shown.end());
    const double published_max = published.empty() ? 0.0 : *std::max_element(published.begin(), published.end());
    const qint64 gap_max = gaps.empty() ? 0 : *std::max_element(gaps.begin(), gaps.end());
    report(QStringLiteral("measure: drag of %1: %2 mouse moves over %3 ms; %4 quick and %5 final frames published; "
                          "the photo is at revision %6")
               .arg(drag_label_)
               .arg(drag_moves_)
               .arg(drag_release_ms_ - drag_start_ms_)
               .arg(edit.quick_frames())
               .arg(edit.final_frames() - finals_before_)
               .arg(edit.revision_number()));
    const std::vector<double>& service = edit.service_latencies();
    report(QStringLiteral("measure: slider-to-frame (position to its frame shown): median %1 ms, 90th percentile "
                          "%2 ms, max %3 ms over %4 frames; to frame published on the UI thread: median %5 ms, "
                          "max %6 ms; of which the develop service (request to frame ready): median %7 ms; "
                          "final frame %8 ms after the release")
               .arg(percentile_of(shown, 0.5), 0, 'f', 0)
               .arg(percentile_of(shown, 0.9), 0, 'f', 0)
               .arg(shown_max, 0, 'f', 0)
               .arg(static_cast<qint64>(shown.size()))
               .arg(percentile_of(published, 0.5), 0, 'f', 0)
               .arg(published_max, 0, 'f', 0)
               .arg(percentile_of(service, 0.5), 0, 'f', 0)
               .arg(now - drag_release_ms_));
    report(QStringLiteral("measure: UI thread during the drag (press to final frame): %1 ticks of a %2 ms timer; "
                          "gap median %3 ms, 99th percentile %4 ms, max %5 ms; %6 gaps over 50 ms, %7 over 100 ms")
               .arg(static_cast<qint64>(gaps.size()))
               .arg(kProbeIntervalMs)
               .arg(percentile_of(gaps, 0.5))
               .arg(percentile_of(gaps, 0.99))
               .arg(gap_max)
               .arg(over(50))
               .arg(over(100)));
}

void DeveloperAutomation::start_settling() {
    if (options_.screenshot_path.isEmpty()) {
        stage_ = Stage::Done;
        if (!options_.measure && options_.batch_folder.isEmpty() && !options_.resume_batch) {
            poll_timer_.stop();
        }
        return;
    }
    if (options_.show_export_dialog) {
        if (QObject* dialog = window_.findChild<QObject*>(QStringLiteral("exportDialog")); dialog != nullptr) {
            QMetaObject::invokeMethod(dialog, "open");
        }
    }
    if (options_.show_about) {
        if (QObject* dialog = window_.findChild<QObject*>(QStringLiteral("aboutDialog")); dialog != nullptr) {
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
    screenshot_done_ = true;
    emit screenshotTaken(saved, path);
    if (options_.quit_after_screenshot) {
        emit quitRequested(!saved ? 1 : (settled ? 0 : 3));
        return;
    }
    finish_when_done();
}

}  // namespace lucent::ui
