#pragma once

// Command line of Lucent.exe and developer-only automation for headless verification.
//
// DEVELOPER-ONLY OPTIONS. They exist so that builds can be verified without a person clicking
// through the window (screenshots of each view, smoke tests of import and export). They are not
// part of the photographer's workflow, are not shown in the user interface, may change without
// notice, and must not be relied on by installers, file associations or shortcuts.
//
//   --import <path>            Add a photo or folder to the library (repeatable).
//   --include-subfolders       Scan --import folders recursively.
//   --open-first               When the imports have finished, open the first photo in Edit.
//   --view <library|edit|exports>
//                              When the imports have finished (and after --open-first), show
//                              this view.
//   --export-all <folder>      When the imports have finished, export every library photo into
//                              <folder> with the export options below (default JPEG, sRGB, 95).
//   --export-format <jpeg|tiff16>
//   --export-color-space <srgb|display-p3>
//   --export-quality <1..100>  Export options for --export-all (and the Export dialog's defaults).
//   --zoom <fit|100>           With --open-first: after the Fit preview is shown, switch the Edit
//                              view to this zoom (100 renders the full-resolution preview).
//   --show-export-dialog       With --open-first: open the Export dialog before the screenshot.
//   --measure                  Report timings on stderr: start-up, first visible thumbnail after
//                              the import started, preview latency (visible, and the services'
//                              render and preview times), and UI-thread responsiveness (gaps
//                              between ticks of a 5 ms timer, window grabs excluded).
//   --screenshot <file.png>    When the window has settled, save it as a PNG. Settling first
//                              waits (without rendering) until no import, background or export
//                              work is pending, then renders a frame with
//                              QQuickWindow::grabWindow() at most every 250 ms until two
//                              consecutive frames equal their predecessor with no visible image
//                              loading. Rendering also lays the scene out, so this works while
//                              the window is occluded or the session is locked.
//   --quit-after-screenshot    Exit after the screenshot: 0 on success, 1 when saving failed,
//                              3 when the window did not settle in time (the screenshot is still
//                              saved).
//   --settle-timeout <ms>      Longest wait for the window to settle (default 60000).
//   --data-root <folder>       Use <folder> instead of %LOCALAPPDATA%\Lucent for the catalog
//                              (<folder>\data) and the caches (<folder>\cache): screenshots and
//                              smoke tests without touching the user's library. Created if needed.
//
// Progress lines ("lucent: ...") go to stderr, which a WIN32 program only has when the caller
// redirects it (for example `Lucent.exe --screenshot a.png 2> log.txt`). Set
// QT_FORCE_STDERR_LOGGING=1 to also get Qt's and QML's own warnings there.
//
// Positional arguments are photos or folders to add to the library, like --import; this is the
// one user-facing part of the command line (drag-and-drop onto Lucent.exe, "Open with").

#include <optional>
#include <vector>

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "app_controller.hpp"

class QQuickWindow;

namespace lucent::ui {

struct DeveloperOptions {
    QStringList imports;  // --import values followed by positional arguments
    bool include_subfolders = false;
    bool open_first = false;
    std::optional<AppController::View> view;
    QString export_folder;
    ExportOptions export_options;
    std::optional<bool> zoom_full;  // --zoom: true for 100 %, false for fit
    bool measure = false;
    bool show_export_dialog = false;
    QString screenshot_path;
    bool quit_after_screenshot = false;
    int settle_timeout_ms = 60000;
    QString data_root;  // --data-root; empty: the user's own folders
};

struct CommandLine {
    enum class Action { Run, ShowHelp, ShowVersion, Error };

    Action action = Action::Run;
    DeveloperOptions options;
    QString message;  // help text, version text or error description (for the non-Run actions)
};

// Parses the full argument list (arguments[0] is the program). Qt's own options such as -platform
// must already have been removed (QCoreApplication::arguments() does that).
[[nodiscard]] CommandLine parse_command_line(const QStringList& arguments);

// Runs the automation DeveloperOptions describes against a loaded window. With no automation
// options it only starts the imports (positional arguments) and does nothing else.
//
// Thread-safety: UI thread only. `controller` and `window` must outlive this object.
class DeveloperAutomation : public QObject {
    Q_OBJECT

public:
    DeveloperAutomation(DeveloperOptions options, AppController& controller, QQuickWindow& window,
                        QObject* parent = nullptr);

    // Starts the imports and the polling. Call once.
    void start();

    // True while a visible Image-like item (QQuickImageBase) of `window` is loading.
    [[nodiscard]] static bool has_loading_images(const QQuickWindow& window);

signals:
    // Emitted once the automation is complete when --quit-after-screenshot was given; the
    // application should exit with `exit_code`.
    void quitRequested(int exit_code);
    // Emitted after a screenshot attempt (saved or not).
    void screenshotTaken(bool saved, const QString& path);

private:
    enum class Stage { WaitingForImports, WaitingForFitPreview, WaitingForZoomedPreview, Settling, Done };

    void poll();
    void after_imports();
    void start_settling();
    void take_screenshot(bool settled);
    [[nodiscard]] QImage grab();
    void probe_tick();
    void report_measurements();
    void report_preview(const char* what, qint64 visible_ms);
    [[nodiscard]] int viewer_status() const;

    DeveloperOptions options_;
    AppController& controller_;
    QQuickWindow& window_;
    QTimer poll_timer_;
    QElapsedTimer settle_clock_;
    Stage stage_ = Stage::WaitingForImports;
    int imports_pending_ = 0;
    int stable_frames_ = 0;
    QElapsedTimer grab_clock_;  // since the last settling frame
    QImage last_frame_;  // the frame rendered by the previous settling poll

    // --measure
    QTimer probe_timer_;
    QElapsedTimer clock_;          // since start()
    qint64 last_tick_ms_ = -1;     // -1: no tick yet, or the previous interval contained a grab
    std::vector<qint64> gaps_ms_;  // intervals between probe ticks
    qint64 last_walk_ms_ = -1;
    qint64 first_thumbnail_ms_ = -1;
    qint64 open_ms_ = -1;          // when --open-first opened the photo
    qint64 zoom_ms_ = -1;          // when --zoom switched the view
    bool zoom_loading_seen_ = false;
};

// Writes "lucent: <message>" and a newline to stderr (when the process has one).
void report(const QString& message);

}  // namespace lucent::ui
