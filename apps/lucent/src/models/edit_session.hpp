#pragma once

// EditSession: the Edit view's editing state as bindable QML properties and commands (blueprint
// section 2, "Detailed individual editing"; ADR 0010). It follows the photo shown in the Edit view
// (AppController opens and closes the service's session) and turns slider drags, key presses, typed
// values, curve edits, history clicks and copy/paste into EditService calls.
//
// Controls. Every slider is a control key: "temperature" and "tint" (Kelvin white balance),
// "redBlue" and "greenMagenta" (camera-neutral white balance: the camera's own channel balance,
// relative to as shot, when the calibration has no colour temperature), "exposure", "contrast",
// "highlights", "shadows", "saturation", "vibrance". Version 2 (W9, ADR 0015) adds "straighten"
// (degrees; the crop follows it when CropTool.constrain), "noiseReduction" (0..100) and
// "noiseRadius" (full-image pixels) for the guided chroma noise reduction, the selected mask's local
// controls "maskExposure" (EV), "maskContrast", "maskHighlights", "maskShadows",
// "maskSaturation", "maskTemperature", "maskTint" (-100..+100), "maskOpacity" and "maskFeather"
// (0..100), and the brush of the masks tool, "brushSize" (percent of the long edge),
// "brushFeather" and "brushFlow" (0..100), which are tool settings, not part of the recipe. Values
// are in display units (exposure in EV, temperature in kelvin, the others -100 to +100); control()
// describes each (range, steps, default, lock key) and position()/setPosition() map them to a 0..1
// slider (temperature in reciprocal kelvin, so equal slider distances are about equal colour
// changes; the brush size and the noise radius on a logarithmic scale). Moving a white-balance
// slider while the photo shows "as shot" starts a custom balance from the as-shot values.
//
// Tools (W9). `tool` is "" (none), "crop" (CropTool, `crop`: the frames are rendered without the
// crop while it is open) or "masks" (MaskEditor, `masks`: handles, outlines and brush painting).
// finishTool() (Enter) closes the tool; cancelTool() (Esc) closes it too and, for the crop tool,
// restores the geometry it was opened with (one revision when it changed). The newest frame's
// geometry (its full-resolution upright size, the geometry of its pixels, uncropped or not) gives
// the tools their mapping (frame_map(): EditService::map_geometry(), the engine's GeometryMapping).
// maskOverlay (O) tints the selected mask's weights over the image (EditService::set_mask_overlay).
//
// Gestures. beginGesture() ... setPosition()/setValue() ... endGesture() (a pointer press and its
// release) is ONE revision: the positions in between are transient (EditService::set_transient) and
// the end commits. nudge() (arrow keys; Shift for coarse steps) starts a gesture that endNudge()
// (key release) or a pause of kNudgeCommitMs ends, so holding a key down is one revision too.
// Without a gesture, setValue(), setValueText() and resetValue() are one step each
// (EditService::apply). A command during a drag (undo, redo, paste, a reset by key or double-click)
// commits what the drag changed so far; the rest of the drag is a new gesture, committed on release,
// never a revision per position.
//
// History commands (undo, redo, selectRevision, resetToAsShot, pasteSettings) are applied by the
// service, so the local values are stale until their state arrives: changes made meanwhile are
// refused (the controls show the held values again) rather than sent on top of the values the
// command removed.
//
// Tone curve. Points stay monotone: moveCurvePoint() keeps x strictly between the neighbours
// (kMinCurveGap apart) and y between the neighbours' y; the end points move within [0, 1]. Points
// are added on the curve and removed (except the end points) by explicit calls; the curve is drawn
// from the service's own evaluation (curveSamples).
//
// The view shows the newest frame (frameSource): Quick frames while settings change, then the Final
// frame; the as-shot frame for before/after (beforeSource) and the clipping overlay
// (clippingSource) come from the same session. Frame latency (a call's time to the frame showing
// its settings) is measured for --measure: when the view reports the frame shown (noteFrameShown).
//
// Local values versus the service: while a gesture runs, or while a newer call than the newest
// state answers is outstanding, the local values are kept (no jumping back); otherwise every state
// replaces them.
//
// Auto (W8; blueprint section 2: "Auto creates a new editable revision"). runAuto() commits a
// pending gesture, then asks the service to run Auto on the photo's committed settings (its locks
// included); `automatic` (AutoSettingsModel) shows the run, its outcome and what Auto did to the
// current revision. Editing stays possible while it runs: the run holds its memory but not the render
// place (auto_service.hpp), so other photos, refines and the 100 % view keep rendering as memory
// permits; a photo changed meanwhile keeps its edit and Auto's revision is kept as another branch of
// the history (redo after an undo returns to the photographer's edit, not to that branch). Undo after
// Auto returns to the recipe before it.
//
// Thread-safety: UI thread only.

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <QtQmlIntegration/qqmlintegration.h>
#include <memory>
#include <optional>
#include <vector>

#include "imaging/frame_store.hpp"
#include "models/auto_settings_model.hpp"
#include "models/crop_tool.hpp"
#include "models/edit_history_model.hpp"
#include "models/mask_editor.hpp"
#include "services/edit_service.hpp"

namespace lucent::ui {

class EditSession : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("EditSession is provided by AppController.edit.")
    Q_PROPERTY(bool active READ active NOTIFY activeChanged FINAL)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool canEdit READ can_edit NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool readOnly READ read_only NOTIFY stateChanged FINAL)
    // False while the photo's recipe cannot be read: the controls show no values (valueText "–",
    // no curve) rather than defaults that would look like an unedited photo.
    Q_PROPERTY(bool settingsKnown READ settings_known NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString readOnlyReason READ read_only_reason NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool historyAvailable READ history_available NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool canUndo READ can_undo NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool canRedo READ can_redo NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool edited READ edited NOTIFY stateChanged FINAL)
    Q_PROPERTY(int revisionNumber READ revision_number NOTIFY stateChanged FINAL)
    // Increases whenever a value shown changes: bindings on value(), position(), isLocked() and
    // valueText() depend on it.
    Q_PROPERTY(int settingsRevision READ settings_revision NOTIFY settingsChanged FINAL)
    // "asShot", "kelvin" or "cameraNeutral".
    Q_PROPERTY(QString whiteBalanceMode READ white_balance_mode NOTIFY settingsChanged FINAL)
    // The white-balance sliders to show: "kelvin" (temperature, tint), "neutral" (camera channels)
    // or "none" (monochrome, or the calibration could not be read).
    Q_PROPERTY(QString whiteBalanceControls READ white_balance_controls NOTIFY whiteBalanceChanged FINAL)
    Q_PROPERTY(QString whiteBalanceNote READ white_balance_note NOTIFY whiteBalanceChanged FINAL)
    Q_PROPERTY(QVariantList curvePoints READ curve_points NOTIFY settingsChanged FINAL)
    Q_PROPERTY(QList<double> curveSamples READ curve_samples NOTIFY settingsChanged FINAL)
    Q_PROPERTY(bool curveIsIdentity READ curve_is_identity NOTIFY settingsChanged FINAL)
    Q_PROPERTY(int selectedCurvePoint READ selected_curve_point WRITE set_selected_curve_point NOTIFY
                   selectedCurvePointChanged FINAL)
    Q_PROPERTY(QString frameSource READ frame_source NOTIFY frameChanged FINAL)
    Q_PROPERTY(QString beforeSource READ before_source NOTIFY frameChanged FINAL)
    Q_PROPERTY(QString clippingSource READ clipping_source NOTIFY frameChanged FINAL)
    // RenderInfoPtr of the newest frame (histogram); null until one arrived.
    Q_PROPERTY(QVariant frameInfo READ frame_info NOTIFY frameChanged FINAL)
    Q_PROPERTY(bool hasFrame READ has_frame NOTIFY frameChanged FINAL)
    // "quick" or "final" (of the newest frame); empty without one.
    Q_PROPERTY(QString framePhase READ frame_phase NOTIFY frameChanged FINAL)
    // Why the photo cannot be rendered; empty when it can.
    Q_PROPERTY(QString frameError READ frame_error NOTIFY frameChanged FINAL)
    // Before/after (the \ key): show the as-shot render; `split` shows both side by side.
    Q_PROPERTY(bool compare READ compare WRITE set_compare NOTIFY compareChanged FINAL)
    Q_PROPERTY(bool split READ split WRITE set_split NOTIFY compareChanged FINAL)
    Q_PROPERTY(bool clipping READ clipping WRITE set_clipping NOTIFY clippingChanged FINAL)
    Q_PROPERTY(lucent::ui::EditHistoryModel* history READ history CONSTANT FINAL)
    Q_PROPERTY(bool hasClipboard READ has_clipboard NOTIFY clipboardChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged FINAL)
    Q_PROPERTY(bool gestureActive READ gesture_active NOTIFY gestureChanged FINAL)
    // The newest measured call-to-frame-shown latency, in milliseconds (0: none yet).
    Q_PROPERTY(double lastLatencyMs READ last_latency_ms NOTIFY latencyChanged FINAL)
    // W8: Auto's run and what it did to the current revision.
    Q_PROPERTY(lucent::ui::AutoSettingsModel* automatic READ automatic CONSTANT FINAL)
    // Auto can start: the photo can be edited, its settings are known and no run of it is in progress.
    Q_PROPERTY(bool canRunAuto READ can_run_auto NOTIFY canRunAutoChanged FINAL)
    // W9 (see the file comment): the open tool, the crop and mask tools, the newest frame's geometry.
    Q_PROPERTY(QString tool READ tool NOTIFY toolChanged FINAL)
    Q_PROPERTY(bool toolActive READ tool_active NOTIFY toolChanged FINAL)
    Q_PROPERTY(lucent::ui::CropTool* crop READ crop CONSTANT FINAL)
    Q_PROPERTY(lucent::ui::MaskEditor* masks READ masks CONSTANT FINAL)
    // The full-resolution size of the newest frame's image (after its geometry); 0 without a frame.
    Q_PROPERTY(int frameOutputWidth READ frame_output_width NOTIFY frameChanged FINAL)
    Q_PROPERTY(int frameOutputHeight READ frame_output_height NOTIFY frameChanged FINAL)
    Q_PROPERTY(bool frameUncropped READ frame_uncropped NOTIFY frameChanged FINAL)
    Q_PROPERTY(bool maskOverlay READ mask_overlay WRITE set_mask_overlay NOTIFY maskOverlayChanged FINAL)
    // The selected mask's weight overlay of the newest frame (empty while off or not rendered yet).
    Q_PROPERTY(QString maskOverlaySource READ mask_overlay_source NOTIFY frameChanged FINAL)

public:
    static constexpr int kNudgeCommitMs = 600;
    static constexpr double kMinCurveGap = 0.01;
    static constexpr int kCurveSamples = 129;

    // `service` must outlive this object; `store` is shared with the frame image providers.
    EditSession(EditService& service, std::shared_ptr<FrameStore> store, QObject* parent = nullptr);
    ~EditSession() override;

    // Follows `item` (0: none): commits a pending gesture of the previous photo, closes its session
    // and opens the new one.
    void set_item(ItemId item);
    [[nodiscard]] ItemId item() const noexcept { return item_; }

    // ---- QML: controls -------------------------------------------------------------------------

    // {label, minimum, maximum, step, coarseStep, decimals, unit, defaultValue, lockKey, reciprocal};
    // empty for an unknown key. `reciprocal`: the slider runs in position() (temperature), otherwise
    // directly in display values.
    Q_INVOKABLE QVariantMap control(const QString& key) const;
    Q_INVOKABLE double value(const QString& key) const;
    // The value as shown in the numeric field ("+0.35", "-12", "5500"; "–" when the recipe cannot be
    // read).
    Q_INVOKABLE QString valueText(const QString& key) const;
    Q_INVOKABLE double position(const QString& key) const;
    Q_INVOKABLE void setPosition(const QString& key, double position);
    Q_INVOKABLE void setValue(const QString& key, double value);
    // Parses a typed value (locale decimal separator or '.', optional sign); false when it is not
    // a number. Values outside the range are clamped.
    Q_INVOKABLE bool setValueText(const QString& key, const QString& text);
    Q_INVOKABLE void resetValue(const QString& key);
    Q_INVOKABLE void beginGesture();
    Q_INVOKABLE void endGesture();
    Q_INVOKABLE void nudge(const QString& key, int steps, bool coarse);
    Q_INVOKABLE void endNudge();
    Q_INVOKABLE bool isLocked(const QString& lock_key) const;
    Q_INVOKABLE void setLocked(const QString& lock_key, bool locked);
    // "As shot" white balance (one step).
    Q_INVOKABLE void resetWhiteBalance();

    // ---- QML: tone curve -----------------------------------------------------------------------

    // Adds a point on the curve at x (as part of a running gesture when there is one); the new
    // point's index, or -1 when it cannot be added (too close to another, 32 points, read-only).
    Q_INVOKABLE int addCurvePoint(double x);
    // Moves point `index` towards (x, y), constrained (see the file comment); returns where it went.
    Q_INVOKABLE QPointF moveCurvePoint(int index, double x, double y);
    Q_INVOKABLE void nudgeCurvePoint(int index, double dx, double dy);
    Q_INVOKABLE bool removeCurvePoint(int index);
    Q_INVOKABLE void resetCurve();
    Q_INVOKABLE double curveValueAt(double x) const;

    // ---- QML: history, comparison, copy and paste ------------------------------------------------

    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void selectRevision(int row);
    Q_INVOKABLE void resetToAsShot();
    // [{key, label}] of the copy categories.
    Q_INVOKABLE QVariantList categories() const;
    Q_INVOKABLE void copySettings(const QStringList& category_keys);
    Q_INVOKABLE void pasteSettings();
    Q_INVOKABLE void setFullResolution(bool full);
    // The view shows `source` (QML Image ready): completes the latency measurement of its frame.
    Q_INVOKABLE void noteFrameShown(const QString& source);
    Q_INVOKABLE void clearMessage();

    // ---- QML: Auto (see the file comment) -----------------------------------------------------------

    Q_INVOKABLE void runAuto();
    Q_INVOKABLE void cancelAuto();

    // ---- QML: tools (W9; see the file comment) ----------------------------------------------------

    // "", "crop" or "masks".
    Q_INVOKABLE void setTool(const QString& tool);
    Q_INVOKABLE void finishTool();
    Q_INVOKABLE void cancelTool();

    // ---- Property accessors ----------------------------------------------------------------------

    [[nodiscard]] bool active() const noexcept { return item_ != 0; }
    [[nodiscard]] bool ready() const noexcept { return state_.has_value(); }
    [[nodiscard]] bool can_edit() const noexcept { return state_ && !state_->read_only; }
    [[nodiscard]] bool read_only() const noexcept { return state_ && state_->read_only; }
    [[nodiscard]] bool settings_known() const noexcept { return !state_ || !state_->settings_unknown; }
    [[nodiscard]] QString read_only_reason() const { return state_ ? state_->read_only_reason : QString(); }
    [[nodiscard]] bool history_available() const noexcept { return state_ && state_->history_available; }
    [[nodiscard]] bool can_undo() const noexcept { return history_available() && state_->can_undo; }
    [[nodiscard]] bool can_redo() const noexcept { return history_available() && state_->can_redo; }
    [[nodiscard]] bool edited() const noexcept { return state_ && state_->edited; }
    [[nodiscard]] int revision_number() const noexcept { return state_ ? int(state_->revision_number) : 0; }
    [[nodiscard]] int settings_revision() const noexcept { return settings_revision_; }
    [[nodiscard]] QString white_balance_mode() const;
    [[nodiscard]] QString white_balance_controls() const;
    [[nodiscard]] QString white_balance_note() const;
    [[nodiscard]] QVariantList curve_points() const;
    [[nodiscard]] QList<double> curve_samples() const {
        return settings_known() ? curve_samples_ : QList<double>{};
    }
    [[nodiscard]] bool curve_is_identity() const { return settings_.tone_curve == EditSettings{}.tone_curve; }
    [[nodiscard]] int selected_curve_point() const noexcept { return selected_point_; }
    void set_selected_curve_point(int index);
    [[nodiscard]] QString frame_source() const { return frame_source_; }
    [[nodiscard]] QString before_source() const { return before_source_; }
    [[nodiscard]] QString clipping_source() const { return clipping_ ? clipping_source_ : QString(); }
    [[nodiscard]] QVariant frame_info() const { return QVariant::fromValue(frame_info_); }
    [[nodiscard]] bool has_frame() const noexcept { return !frame_source_.isEmpty(); }
    [[nodiscard]] QString frame_phase() const;
    [[nodiscard]] QString frame_error() const { return frame_error_; }
    // The newest frame was rendered at full resolution (100 %).
    [[nodiscard]] bool frame_full_resolution() const noexcept { return frame_full_resolution_; }
    [[nodiscard]] bool compare() const noexcept { return compare_; }
    void set_compare(bool compare);
    [[nodiscard]] bool split() const noexcept { return split_; }
    void set_split(bool split);
    [[nodiscard]] bool clipping() const noexcept { return clipping_; }
    void set_clipping(bool clipping);
    [[nodiscard]] EditHistoryModel* history() const noexcept { return history_.get(); }
    [[nodiscard]] bool has_clipboard() const noexcept { return has_clipboard_; }
    [[nodiscard]] QString message() const { return message_; }
    [[nodiscard]] bool busy() const;
    [[nodiscard]] bool gesture_active() const noexcept { return gesture_; }
    [[nodiscard]] double last_latency_ms() const noexcept { return last_latency_ms_; }
    [[nodiscard]] AutoSettingsModel* automatic() const noexcept { return automatic_.get(); }
    [[nodiscard]] bool can_run_auto() const;
    [[nodiscard]] QString tool() const { return tool_; }
    [[nodiscard]] bool tool_active() const noexcept { return !tool_.isEmpty(); }
    [[nodiscard]] CropTool* crop() const noexcept { return crop_.get(); }
    [[nodiscard]] MaskEditor* masks() const noexcept { return masks_.get(); }
    [[nodiscard]] int frame_output_width() const noexcept { return frame_map_.valid ? frame_map_.output_size.width() : 0; }
    [[nodiscard]] int frame_output_height() const noexcept {
        return frame_map_.valid ? frame_map_.output_size.height() : 0;
    }
    [[nodiscard]] bool frame_uncropped() const noexcept { return frame_uncropped_; }
    [[nodiscard]] bool mask_overlay() const noexcept { return mask_overlay_; }
    void set_mask_overlay(bool overlay);
    [[nodiscard]] QString mask_overlay_source() const { return mask_overlay_ ? mask_overlay_source_ : QString(); }
    // A control's committed value as the Auto panel lists it ("Exposure +0.75 EV", "White balance
    // 5450 K, tint +3", "Tone curve").
    [[nodiscard]] QString describe_committed(EditControl control) const;

    // ---- C++ ---------------------------------------------------------------------------------------

    [[nodiscard]] const EditSettings& settings() const noexcept { return settings_; }
    [[nodiscard]] EditService& service() const noexcept { return service_; }
    // For the tools: a change (transient within a gesture, otherwise one step), a pointer gesture,
    // and ending whatever gesture is pending before a one-step command.
    void edit_settings(const EditSettings& next) { change(next); }
    void begin_pointer_gesture() { beginGesture(); }
    void end_pointer_gesture() { endGesture(); }
    void finish_pending_gesture();
    // A user-facing message for the panel and the status line.
    void show_message(const QString& text);
    // The newest frame (not the as-shot one): the photo's full-resolution upright size, the geometry
    // of its pixels and its mapping at full resolution (invalid without a frame), and its masks.
    [[nodiscard]] QSize frame_full_upright_size() const noexcept { return frame_full_upright_; }
    [[nodiscard]] const EditGeometry& frame_geometry() const noexcept { return frame_geometry_; }
    [[nodiscard]] const GeometryMap& frame_map() const noexcept { return frame_map_; }
    [[nodiscard]] const QList<FrameMaskOutline>& frame_masks() const noexcept { return frame_masks_; }
    // The mask the local controls and the mask tool edit (0: none).
    [[nodiscard]] quint32 selected_mask() const noexcept { return selected_mask_; }
    void set_selected_mask(quint32 id);
    // The masks tool's brush: size in percent of the long edge, feather and flow 0..100.
    [[nodiscard]] double brush_size() const noexcept { return brush_size_; }
    [[nodiscard]] double brush_feather() const noexcept { return brush_feather_; }
    [[nodiscard]] double brush_flow() const noexcept { return brush_flow_; }
    [[nodiscard]] const std::optional<EditState>& state() const noexcept { return state_; }
    [[nodiscard]] const WhiteBalanceInfo& white_balance() const noexcept { return white_balance_; }
    // Latencies measured since the last reset, in milliseconds: call to frame shown, and call to
    // frame published (arrived on the UI thread).
    [[nodiscard]] const std::vector<double>& shown_latencies() const noexcept { return shown_latencies_; }
    [[nodiscard]] const std::vector<double>& published_latencies() const noexcept { return published_latencies_; }
    // The services' own part of the same frames: develop request to frame ready (EditFrame).
    [[nodiscard]] const std::vector<double>& service_latencies() const noexcept { return service_latencies_; }
    void reset_latencies();
    // Frames received since the last reset, by phase.
    [[nodiscard]] int quick_frames() const noexcept { return quick_frames_; }
    [[nodiscard]] int final_frames() const noexcept { return final_frames_; }

    // The constrained position of point `index` moved to `wanted` (see the file comment).
    [[nodiscard]] static QPointF constrain_curve_point(const QList<QPointF>& points, int index, QPointF wanted);
    // Display-value helpers (exposed for tests).
    [[nodiscard]] static bool is_control(const QString& key);

signals:
    void activeChanged();
    void stateChanged();
    void settingsChanged();
    void whiteBalanceChanged();
    void selectedCurvePointChanged();
    void frameChanged();
    void compareChanged();
    void clippingChanged();
    void clipboardChanged();
    void messageChanged();
    void busyChanged();
    void gestureChanged();
    void latencyChanged();
    void canRunAutoChanged();
    void toolChanged();
    void maskOverlayChanged();
    void maskSelectionChanged();
    // A user-facing outcome for the status line (paste results, refused edits).
    void notice(const QString& text);

private:
    // Starts a gesture (no-op while one runs).
    void start_gesture();
    // Commits the running gesture's changes as one revision; the gesture ends unless the pointer is
    // still down (then its later changes form the next revision, committed on release).
    void commit_gesture();
    [[nodiscard]] bool awaiting_history() const noexcept {
        return answered_token_ < history_token_ || paste_pending_;
    }
    void remember_history_token(quint64 token);

    void on_state(const EditState& state);
    void on_history(ItemId item, const QList<EditHistoryEntry>& entries);
    void on_white_balance(ItemId item, const WhiteBalanceInfo& info);
    void on_frame(const EditFramePtr& frame);
    void on_frame_failed(ItemId item, const QString& message);
    void on_command_failed(ItemId item, const QString& message);
    void on_copied(ItemId item, quint32 categories);
    void on_pasted(const PasteReport& report);

    // Applies `next` locally and sends it: transient within a gesture, otherwise one step.
    void change(const EditSettings& next);
    void set_local(const EditSettings& settings);
    void send_comparison();
    void remember_token(quint64 token);
    void set_message(const QString& message);
    void reset_frames();
    [[nodiscard]] double display_value(const EditSettings& settings, const QString& key) const;
    // valueText() of `settings` (without the unknown-settings dash).
    [[nodiscard]] QString value_text_of(const EditSettings& settings, const QString& key) const;
    [[nodiscard]] EditSettings with_value(const EditSettings& settings, const QString& key, double value) const;
    [[nodiscard]] std::array<double, 3> as_shot_neutral() const;
    // The default of a control key (the mask feather's depends on the selected mask's kind).
    [[nodiscard]] double default_of(const QString& key) const;
    // The brush keys are tool settings: they change these members, never the recipe.
    void set_tool_value(const QString& key, double value);
    // Keeps the selected mask one of the settings' masks (the last one, or none).
    void validate_selection();
    void send_mask_overlay();

    EditService& service_;
    std::shared_ptr<FrameStore> store_;
    std::unique_ptr<EditHistoryModel> history_;
    std::unique_ptr<AutoSettingsModel> automatic_;
    std::unique_ptr<CropTool> crop_;
    std::unique_ptr<MaskEditor> masks_;
    ItemId item_ = 0;
    std::optional<EditState> state_;
    EditSettings settings_;
    int settings_revision_ = 0;
    WhiteBalanceInfo white_balance_;
    QList<double> curve_samples_;
    int selected_point_ = -1;

    bool gesture_ = false;
    bool gesture_changed_ = false;
    bool pointer_down_ = false;  // between beginGesture() and endGesture()
    bool nudging_ = false;
    QTimer nudge_timer_;
    quint64 last_token_ = 0;      // newest call made
    quint64 answered_token_ = 0;  // newest call a state answered
    quint64 history_token_ = 0;   // newest undo, redo, select or reset
    bool paste_pending_ = false;  // a paste into this photo is not reported yet

    QString frame_source_;
    QString before_source_;
    QString clipping_source_;
    RenderInfoPtr frame_info_;
    FramePhase frame_phase_ = FramePhase::Quick;
    bool frame_full_resolution_ = false;
    QString frame_error_;
    quint64 last_sequence_ = 0;
    // W9: the newest frame's geometry and masks, tools, the mask overlay and the brush.
    QSize frame_full_upright_;
    EditGeometry frame_geometry_;
    GeometryMap frame_map_;
    bool frame_uncropped_ = false;
    QList<FrameMaskOutline> frame_masks_;
    QString mask_overlay_source_;
    bool mask_overlay_ = false;
    QString tool_;
    EditGeometry crop_opened_with_;  // the geometry when the crop tool opened (cancelTool restores it)
    quint32 selected_mask_ = 0;
    double brush_size_ = 2.0;
    double brush_feather_ = 50.0;
    double brush_flow_ = 100.0;

    bool compare_ = false;
    bool split_ = false;
    bool clipping_ = false;
    bool full_resolution_ = false;
    bool has_clipboard_ = false;
    QString message_;

    // Latency measurement.
    QElapsedTimer clock_;
    QHash<quint64, qint64> token_times_;       // token -> call time (ms on clock_)
    QHash<QString, qint64> pending_shown_;     // frame URL -> call time of its token
    std::vector<double> shown_latencies_;
    std::vector<double> published_latencies_;
    std::vector<double> service_latencies_;
    double last_latency_ms_ = 0.0;
    int quick_frames_ = 0;
    int final_frames_ = 0;
};

}  // namespace lucent::ui
