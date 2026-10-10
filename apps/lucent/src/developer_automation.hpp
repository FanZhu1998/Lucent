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
//   --show-about               W10: open the About dialog (the camera support database and the
//                              meaning of the support states) before the screenshot. At start-up
//                              every run reports the camera support database it uses on stderr
//                              ("lucent: camera support database 1.0.0 ..." or "... unavailable:").
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
//                              Downloaded updates stay in %LOCALAPPDATA%\Lucent\cache\updates
//                              (service_factory.cpp: a folder only this user can change).
//   --set <control>=<value>    With --open-first (W6): once the photo's first frame is shown, make
//                              this edit (one revision each, in order, each waiting for its final
//                              frame). <control> is a slider key (exposure, contrast, highlights,
//                              shadows, saturation, vibrance, temperature, tint, redBlue,
//                              greenMagenta; display units: exposure in EV, temperature in kelvin,
//                              the others -100..100), "curve" with points "x:y,x:y,..." (0..1), or
//                              "lock" with a lock key (whiteBalance, exposure, ..., toneCurve).
//                              Repeatable. With --measure, each edit's frame latency is reported.
//   --drag <control>           With --open-first: after the --set edits, drag that slider with
//                              synthetic mouse events (press, --drag-steps moves 16 ms apart over a
//                              third of the track, release: one gesture). With --measure, reports
//                              slider-to-frame latency (call to frame shown and to frame published),
//                              the Quick and Final frames and the largest UI-thread gap during the
//                              drag.
//   --drag-steps <n>           Mouse moves of --drag (default 60, 10..1000).
//   --compare <before|split>   With --open-first: show the as-shot render (before) or before/after
//                              side by side before the screenshot.
//   --clipping                 With --open-first: turn the clipping overlay on before the
//                              screenshot.
//   --scroll-panel <fraction>  With --open-first: scroll the Edit view's side panel (0 top, 1
//                              bottom) before the screenshot, to show the history and details.
//   --auto                     W8, with --open-first: after the --set edits and --drag, run Auto on the
//                              photo (as the Auto button does), wait for it and for the frame of its
//                              revision, and report the outcome and the Automatic settings panel's
//                              summary (confidence, review reasons, what changed, locks, fallback).
//   W9 geometry and masks, with --open-first, after the --set edits and before --drag, each one
//   revision (or two) waiting for its final frame, in this order:
//   --turn <cw|ccw|flip>       A quarter turn or the mirror, as the crop panel's buttons (repeatable).
//   --straighten <degrees>     The straighten slider's value (-45..45; the crop follows it while
//                              "Constrain to image" is on).
//   --crop <l,t,w,h>           The crop as fractions of the straightened image (left, top, width,
//                              height).
//   --add-mask <spec>          Adds a mask as the panel's buttons do, then sets its shape when given:
//                              radial[:cx,cy,rx,ry[,angle[,feather]]] (normalised upright positions,
//                              radii as fractions of the long edge), linear[:x1,y1,x2,y2[,feather]] or
//                              brush. Repeatable; each added mask then gets the --mask-set values.
//   --mask-set <control=value> A local control of every mask the automation adds or paints
//                              (maskExposure, maskContrast, maskHighlights, maskShadows,
//                              maskSaturation, maskTemperature, maskTint, maskOpacity, maskFeather;
//                              display units). Repeatable.
//   --mask-exposure <ev>       Shorthand for --mask-set maskExposure=<ev>.
//   --brush <x1,y1;x2,y2;...>  Paints one stroke through these normalised upright points with the
//                              masks tool: synthetic mouse events on the brush overlay (press,
//                              --drag-steps moves 16 ms apart along the path, release), into the
//                              selected brush mask or a new one. With --measure it is reported like
//                              --drag (pointer to frame latency, frames, UI-thread gaps). Repeatable.
//   --brush-size <percent>     The brush size for --brush, percent of the long edge (default 3).
//   --mask-overlay             Shows the selected mask's weight overlay (O) before the screenshot.
//   --tool <crop|masks>        Opens that tool before the screenshot (crop: waits for the uncropped
//                              frame).
//   W13 lens corrections (ADR 0023), with --open-first, after the --set edits and before the geometry
//   and mask options, each one revision waiting for its final frame (a spec that changes nothing
//   makes none):
//   --lens <spec>              As the Lens corrections panel does: "on" (turn corrections on for a
//                              photo edited before Lucent had them), "auto" (the automatic profile),
//                              "off" (all three corrections off), "all" (all three on),
//                              "distortion=on|off", "ca=on|off" (chromatic aberration),
//                              "vignetting=on|off", "profile=<search>" (search the lens database by
//                              maker and model and choose the first profile found), or "search=<text>"
//                              (open the profile choice with that search, choosing nothing). Repeatable.
//                              Before the screenshot the panel's state is reported ("lucent: lens: ...").
//   --show-panel <lens>        Scrolls the Edit view's side panel so that this section is at its top
//                              (and expands it) before the screenshot; reports the panel's state too.
//                              At start-up every run reports the lens database it uses on stderr
//                              ("lucent: lens database 1.0.0 ..." or "... unavailable:").
//   --export-batch <folder>    W7: when the imports have finished, export every photo of <folder>
//                              (and its subfolders with --include-subfolders) as one durable batch,
//                              as the Export batch dialog's Start does: the folder is added to the
//                              library first. Progress and the outcome are reported on stderr.
//   --to <folder>              The output folder of --export-batch (required with it).
//   --preset <jpeg|tiff>       The preset of --export-batch: JPEG / sRGB / quality 95 (default) or
//                              16-bit TIFF (profile from --export-color-space, default sRGB).
//   --automatic <off|auto|analyze-only>
//                              W8: the automatic settings of --export-batch (default off).
//   --keep-consistent          W8: with --automatic auto or analyze-only, keep related shots
//                              consistent. When the batch has finished its report's rows are
//                              reported (outcome, what Auto did, review notes).
//   --show-batch-dialog        With --export-batch: open the Export batch dialog with these settings
//                              instead of starting (a screenshot then shows its example paths).
//   --screenshot-running <file.png>
//                              With --export-batch: also save the window while the batch runs (once
//                              a photo is exported and another remains), without waiting to settle.
//   --quit-when-done           With --export-batch: exit when the batch has finished (after the
//                              screenshots): 0 when no photo failed, 4 otherwise, 5 when the batch
//                              could not be created.
//   W12 measurement hooks (the performance baseline, docs/performance, ADR 0021):
//   --resume-batch             Instead of creating a batch: follow the newest batch of the library
//                              that has work left until it has finished, and report it as
//                              --export-batch does (a batch an earlier session left running, for
//                              example one whose Lucent.exe was ended, continues by itself when
//                              Lucent starts; a paused one is resumed). With --quit-when-done, exit
//                              then, or with 5 at once when no batch has work left.
//   --cancel-batch-after <n>   With --export-batch or --resume-batch: cancel the batch (as the
//                              Exports view's Cancel does) once <n> photos are done and another one
//                              is being processed.
//   --pause-batch-after <n>    With --export-batch or --resume-batch: pause the batch once <n>
//                              photos are done, then resume it --resume-after <ms> (default 5000,
//                              0..600000) after its last running photo finished.
//   --cancel-auto-after <ms>   With --auto: cancel the run <ms> after it started (as the Automatic
//                              settings panel's Cancel does).
//   With --measure these runs add lines of key=value fields (milliseconds since the automation
//   started, "_ms"; mebibytes, "_mib"), which tools/bench parses:
//     measure: first frame presented <n> ms after the process started
//     measure: first RAW preview shown <n> ms after the import started   (--open-first: the photo's
//              first frame rendered from the RAW data, not the camera's preview)
//     measure: batch <id> timeline: submit_ms= created_ms= first_done_ms= finished_ms= done=
//              exported= failed= skipped= cancelled= seconds= per_minute=
//              (created_ms is when --resume-batch found the batch; done and the outcome counts are
//              those reached after the automation first saw the batch, so photos left out when it
//              was created, or done before a restart, are not among them; per_minute is exported
//              (or analysed) photos per minute from created_ms to finished_ms)
//     measure: batch <id> scheduler: <the batch service's statistics of this session>
//     measure: batch <id> cancel: requested_ms= done= running= stopped_after_ms=
//     measure: batch <id> pause: requested_ms= running= settled_after_ms= resume_after_ms=
//     measure: auto run: ended_after_ms= cancelled=
//     measure: auto cancel: requested_after_ms= ended_after_request_ms= cancelled= progress=
//     measure: process: peak_working_set_mib= peak_commit_mib= commit_mib= cpu_s= user_s= kernel_s=
//              wall_s=
//   (the process line ends every --measure run: at the screenshot, when the services are idle, or
//   before --quit-when-done exits).
//
// Progress lines ("lucent: ...") go to stderr, which a WIN32 program only has when the caller
// redirects it (for example `Lucent.exe --screenshot a.png 2> log.txt`). Set
// QT_FORCE_STDERR_LOGGING=1 to also get Qt's and QML's own warnings there.
//
// Positional arguments are photos or folders to add to the library, like --import; this is the
// one user-facing part of the command line (drag-and-drop onto Lucent.exe, "Open with").

#include <array>
#include <deque>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include <QElapsedTimer>
#include <QEvent>
#include <QImage>
#include <QPointF>
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
    bool show_about = false;  // --show-about (W10)
    QString screenshot_path;
    bool quit_after_screenshot = false;
    int settle_timeout_ms = 60000;
    QString data_root;  // --data-root; empty: the user's own folders
    // W6 editing automation (see the file comment).
    std::vector<std::pair<QString, QString>> edits;  // --set key=value, in order
    QString drag;                                     // --drag: a slider key
    int drag_steps = 60;
    QString compare;  // --compare: "", "before" or "split"
    bool clipping = false;
    std::optional<double> panel_scroll;  // --scroll-panel
    bool run_auto = false;               // --auto (W8)
    // W9 geometry and masks (see the file comment).
    QStringList turns;                                     // --turn
    std::optional<double> straighten;                      // --straighten
    std::optional<std::array<double, 4>> crop;             // --crop l,t,w,h
    QStringList add_masks;                                 // --add-mask specs
    std::vector<std::pair<QString, QString>> mask_values;  // --mask-set / --mask-exposure
    std::vector<QList<QPointF>> brushes;                   // --brush strokes
    double brush_size = 3.0;                               // --brush-size, percent
    bool mask_overlay = false;                             // --mask-overlay
    QString tool;                                          // --tool
    // W13 lens corrections (see the file comment).
    QStringList lens_edits;  // --lens specs
    QString show_panel;      // --show-panel
    // W7 batch automation (see the file comment).
    QString batch_folder;  // --export-batch
    QString batch_to;      // --to
    bool batch_tiff = false;
    bool show_batch_dialog = false;
    QString running_screenshot_path;
    bool quit_when_done = false;
    BatchAutomatic batch_automatic = BatchAutomatic::Off;  // --automatic (W8)
    bool keep_consistent = false;                          // --keep-consistent (W8)
    // W12 measurement hooks (see the file comment).
    bool resume_batch = false;                // --resume-batch
    std::optional<int> cancel_batch_after;    // --cancel-batch-after <n>
    std::optional<int> pause_batch_after;     // --pause-batch-after <n>
    int resume_after_ms = 5000;               // --resume-after <ms>
    std::optional<int> cancel_auto_after_ms;  // --cancel-auto-after <ms>
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
    enum class Stage { WaitingForImports, WaitingForFitPreview, WaitingForZoomedPreview, Editing, Settling, Done };

    void poll();
    void after_imports();
    void start_settling();
    void take_screenshot(bool settled);
    [[nodiscard]] QImage grab();
    void probe_tick();
    void report_measurements();
    void report_preview(const char* what, qint64 visible_ms);
    [[nodiscard]] int viewer_status() const;
    // W6 editing automation.
    [[nodiscard]] bool wants_editing() const;
    void edit_step();
    bool apply_edit(const QString& key, const QString& value);
    // W9: the geometry and mask steps (see the file comment), in order; each returns whether a final
    // frame follows.
    [[nodiscard]] std::deque<std::pair<QString, std::function<bool()>>> geometry_and_mask_steps();
    bool add_mask(const QString& spec);
    bool apply_mask_values();
    // W13 --lens: one lens-correction change (see the file comment); whether a final frame follows.
    bool apply_lens(const QString& spec);
    // The Lens corrections panel's state ("lens: ..." lines).
    void report_lens();
    // Starts a synthetic stroke (see --brush); false when the masks tool cannot paint.
    bool paint_brush(const QList<QPointF>& points);
    void start_drag();
    void drag_tick();
    void send_mouse(QEvent::Type type, QPointF position);
    void report_edit_latency(const QString& what);
    void report_drag();
    // W8 --auto: true while Auto still needs the poll (started, running, or its frame pending).
    bool auto_step();
    void report_auto_summary();
    void report_batch_entries(const BatchReportInfo& report);
    // W7 batch automation.
    void start_batch();
    void on_batch_changed(const BatchSnapshot& batch);
    // W12 measurement hooks (see the file comment).
    void on_batches_listed(const QList<BatchSnapshot>& batches);
    void apply_batch_hooks(const BatchSnapshot& batch, qint64 done);
    void report_batch_timeline(const BatchSnapshot& batch);
    // The "measure: process" line, once.
    void report_process();
    // True while the automation's batch is preparing, running, or finished without its final review
    // count and report (the screenshot waits for them).
    [[nodiscard]] bool batch_pending() const;
    void take_running_screenshot();
    void finish_when_done();

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

    // --set, --drag
    std::size_t next_edit_ = 0;
    bool edit_waiting_ = false;    // an edit was made; waiting for its final frame
    int finals_before_ = 0;
    QString edit_label_;
    bool steps_built_ = false;
    std::deque<std::pair<QString, std::function<bool()>>> steps_;  // W9 steps not run yet
    QTimer drag_timer_;
    bool drag_active_ = false;     // between the press and the release
    bool drag_measuring_ = false;  // from the press until the drag's final frame
    bool drag_done_ = false;
    int drag_step_ = 0;
    QPointF drag_from_;
    QPointF drag_to_;
    QList<QPointF> drag_path_;  // --brush: the scene positions of the moves (empty: a slider drag)
    QString drag_label_;
    int drag_moves_ = 0;
    EditSettings drag_settings_;  // at the press (a drag that changed nothing waits for no frame)
    qint64 drag_start_ms_ = -1;
    qint64 drag_release_ms_ = -1;
    std::vector<qint64> drag_gaps_ms_;

    // --auto
    bool auto_started_ = false;
    bool auto_done_ = false;
    std::optional<AutoOutcome> auto_outcome_;
    QElapsedTimer auto_clock_;

    // --export-batch
    bool batch_report_requested_ = false;
    bool batch_report_printed_ = false;
    quint64 batch_token_ = 0;
    BatchId batch_id_ = 0;
    bool batch_failed_to_start_ = false;
    bool batch_finished_ = false;
    bool batch_reported_ = false;
    qint64 batch_last_done_ = -1;
    bool running_shot_taken_ = false;
    bool screenshot_done_ = false;

    // W12 measurement hooks. clock_ times; -1 until known.
    bool measurements_reported_ = false;
    bool process_reported_ = false;
    qint64 batch_submit_ms_ = -1;
    qint64 batch_created_ms_ = -1;  // --resume-batch: when the batch was found
    qint64 batch_first_done_ms_ = -1;
    qint64 batch_finished_ms_ = -1;
    // The batch's first snapshot this session saw (a batch it created: its counts at creation).
    std::optional<BatchSnapshot> batch_first_seen_;
    bool resume_listed_ = false;
    bool batch_cancel_requested_ = false;
    qint64 batch_cancel_ms_ = -1;
    qint64 batch_cancel_done_ = 0;
    qint64 batch_cancel_running_ = 0;
    bool batch_pause_requested_ = false;
    bool batch_pause_settled_ = false;
    qint64 batch_pause_ms_ = -1;
    qint64 batch_pause_running_ = 0;
    qint64 auto_finished_ms_ = -1;  // auto_clock_ when auto_finished arrived
    qint64 auto_cancel_ms_ = -1;    // auto_clock_ when the cancel was requested
    double auto_cancel_progress_ = -1.0;
};

// Writes "lucent: <message>" and a newline to stderr (when the process has one).
void report(const QString& message);

}  // namespace lucent::ui
