#pragma once

// EditService: the shell's view of detailed editing (blueprint section 2, "Detailed individual
// editing"; docs/adr/0010): the develop settings of one photo, its revisions (undo, redo, history,
// reset to as shot), copy and paste by category, and the interactive previews of an edit session.
//
// Threading contract: as LibraryService (library_service.hpp). The object lives on the UI thread,
// its member functions return promptly (recipe commands, catalog writes, file probes and renders
// run elsewhere), and its signals are emitted on the UI thread, never from inside the call that
// caused them.
//
// Sessions. open() starts an edit session for a photo: state_changed (its settings and what undo
// and redo can do), history_changed, white_balance_changed and frames follow. close() ends it and
// releases its memory. A session renders every change: frame_ready delivers a Quick frame (the
// cached proxy) for every position the service gets to, then, once the settings have settled, a
// Final frame (refined white balance, or full resolution at 100 %; the Quick frame is already final
// otherwise). Frames of a session arrive in increasing `sequence` order and never go back to an
// older position (blueprint section 4).
//
// Gestures (ADR 0010, "Coalescing"). While a control is dragged (or a key repeats), set_transient()
// is called for every position: nothing is written and frames follow the positions (positions the
// service could not get to are skipped). commit() ends the gesture with exactly ONE revision when
// the settings changed. apply() is one complete step (a typed value, a reset of one control).
//
// Tokens. Every call that changes what is shown returns a token (increasing per service, never 0);
// frames carry the token of the call whose settings they show, so the view can measure the time
// from a slider position to its frame.
//
// Read-only photos (blueprint section 8: "Open incompatible recipes read-only with a clear
// reason"): EditState::read_only with the reason; edits are refused (command_failed). When only
// the photo's own recipe is unreadable, moving to another revision (undo, redo, select, reset)
// stays possible (EditState::history_available).
//
// Automatic settings (W8; blueprint section 2: "Auto creates a new editable revision. The
// photographer can lock controls before rerunning it. The optimizer may change only unlocked
// controls and must report when the locks prevent its usual technical targets"; ADR 0013).
// run_auto() analyses the photo's committed settings (locks and locked values included) in the
// background: auto_progress follows (about every percent), then auto_finished exactly once. The
// automatic revision (origin "auto") arrives as state_changed and history_changed like any other
// change; undo returns to the recipe before Auto. auto_summary_changed describes the CURRENT
// revision whenever the history is published: what automatic settings did when it is an automatic
// revision (from its analysis record, also for revisions an automatic batch made), an empty summary
// otherwise. One run per photo at a time; a run continues when its session closes.
//
// Ownership: the shell owns the service (Services::edits) and destroys it after the QML engine. A
// service must not emit signals from its destructor.

#include <array>
#include <cstdint>
#include <memory>

#include <QDateTime>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QStringList>

#include "services/service_types.hpp"

namespace lucent::ui {

// The photographic controls, with the numeric values of the recipe contract's control identifiers
// (engine::DevelopControl; stable).
enum class EditControl : std::uint8_t {
    WhiteBalance = 1,
    Exposure = 2,
    Contrast = 3,
    Highlights = 4,
    Shadows = 5,
    ToneCurve = 6,
    Saturation = 7,
    Vibrance = 8,
    BaseLook = 9,
};

[[nodiscard]] constexpr quint32 control_bit(EditControl control) noexcept {
    return quint32{1} << static_cast<unsigned>(control);
}

// Copy and paste categories (the recipe contract's RecipeCategory values; stable).
enum class SettingsCategory : std::uint8_t {
    WhiteBalance = 1,
    Exposure = 2,
    Tone = 3,    // contrast, tone curve, base look
    Color = 4,   // saturation, vibrance
    Local = 5,   // highlights, shadows
};

[[nodiscard]] constexpr quint32 category_bit(SettingsCategory category) noexcept {
    return quint32{1} << static_cast<unsigned>(category);
}

inline constexpr std::array<SettingsCategory, 5> kAllSettingsCategories = {
    SettingsCategory::WhiteBalance, SettingsCategory::Exposure, SettingsCategory::Tone,
    SettingsCategory::Color, SettingsCategory::Local};

inline constexpr quint32 kAllCategoryBits =
    category_bit(SettingsCategory::WhiteBalance) | category_bit(SettingsCategory::Exposure) |
    category_bit(SettingsCategory::Tone) | category_bit(SettingsCategory::Color) |
    category_bit(SettingsCategory::Local);

enum class WhiteBalanceMode : std::uint8_t {
    AsShot = 1,
    Kelvin = 2,         // temperature and tint (DNG SDK units); only with a Kelvin calibration
    CameraNeutral = 3,  // camera red, green, blue multipliers (relative to the native values)
};

// Ranges of the recipe contract ("lucent.develop" v1, engine/develop_settings.hpp).
inline constexpr double kExposureLimit = 5.0;     // exposure_ev in [-5, 5]
inline constexpr double kAdjustmentLimit = 1.0;   // contrast .. vibrance in [-1, 1]
inline constexpr double kMinTemperature = 2000.0;
inline constexpr double kMaxTemperature = 50000.0;
inline constexpr double kTintLimit = 150.0;
inline constexpr double kMaxMultiplierRatio = 64.0;
inline constexpr int kMinCurvePoints = 2;
inline constexpr int kMaxCurvePoints = 32;

// The photographic settings the editor shows and changes (DevelopSettings v1 without the parts the
// editor does not change: the base look and the operation versions are kept by the service).
struct EditSettings {
    WhiteBalanceMode white_balance = WhiteBalanceMode::AsShot;
    double temperature_k = 5000.0;          // Kelvin
    double tint = 0.0;                      // Kelvin
    std::array<double, 3> neutral{1.0, 1.0, 1.0};  // CameraNeutral: camera R, G, B multipliers
    double exposure_ev = 0.0;
    double contrast = 0.0;
    double highlights = 0.0;
    double shadows = 0.0;
    double saturation = 0.0;
    double vibrance = 0.0;
    // Monotone control points in display lightness (x strictly increasing, y non-decreasing, both
    // in [0, 1], 2 to 32 points); {(0, 0), (1, 1)} is the identity.
    QList<QPointF> tone_curve{QPointF(0.0, 0.0), QPointF(1.0, 1.0)};
    quint32 locks = 0;  // control_bit() of the locked controls

    [[nodiscard]] bool locked(EditControl control) const noexcept { return (locks & control_bit(control)) != 0; }
    void set_locked(EditControl control, bool locked) noexcept {
        locks = locked ? (locks | control_bit(control)) : (locks & ~control_bit(control));
    }
    // Every photographic value is the default (locks ignored): the photo renders as shot.
    [[nodiscard]] bool renders_as_shot() const noexcept;

    friend bool operator==(const EditSettings&, const EditSettings&) = default;
};

// What the photo's camera calibration allows for white balance (blueprint section 2: "temperature/
// tint where calibration permits ... When an accurate Kelvin conversion is unavailable, preserve
// and expose camera-neutral/tint adjustments without inventing a physical temperature").
struct WhiteBalanceInfo {
    bool known = false;  // the file's calibration was read
    bool kelvin_available = false;
    bool monochrome = false;
    // As-shot white: temperature and tint when kelvin_available; the camera R, G, B multipliers
    // (green = 1) in every case when known.
    double as_shot_temperature_k = 0.0;
    double as_shot_tint = 0.0;
    std::array<double, 3> as_shot_neutral{1.0, 1.0, 1.0};
    // User-facing: why Kelvin is unavailable (or why nothing is known); empty otherwise.
    QString explanation;
};

struct EditState {
    ItemId item = 0;
    // The call this state answers (0: none, for example the state an open() reports). A refused
    // call also answers with the state the service holds.
    quint64 token = 0;
    EditSettings settings;   // what is shown: the gesture's position while one is pending
    EditSettings committed;  // the current revision's (as shot without one)
    quint64 revision = 0;    // the current revision; 0: as shot (no revision)
    quint32 revision_number = 0;
    bool transient = false;  // a gesture is pending
    quint64 transient_generation = 0;
    bool can_undo = false;
    bool can_redo = false;
    // Edits are refused, for read_only_reason (user-facing).
    bool read_only = false;
    QString read_only_reason;
    // Undo, redo, selecting a revision and reset to as shot are possible (false only when every
    // change is refused, for example in a library saved by a newer Lucent).
    bool history_available = true;
    // The committed settings differ from as shot, or cannot be read by this Lucent.
    bool edited = false;
    // The recipe cannot be read (read_only says why): `settings` and `committed` are placeholders, not
    // the photo's values, and are not shown.
    bool settings_unknown = false;
};

// User-facing names: "White balance", "Exposure", "Contrast", "Highlights", "Shadows", "Tone curve",
// "Saturation", "Vibrance", "Base look"; and the categories "White balance", "Exposure", "Tone",
// "Color", "Highlights and shadows".
[[nodiscard]] QString control_label(EditControl control);
[[nodiscard]] QString category_label(SettingsCategory category);

struct EditHistoryEntry {
    quint64 revision = 0;
    quint32 number = 0;
    quint32 parent_number = 0;  // 0: derived from as shot
    // "user", "auto", "reset", "paste", "import".
    QString origin;
    QDateTime created;  // UTC
    QList<EditControl> changed;  // controls whose values changed in this revision
    QList<EditControl> locks;    // controls whose lock changed
    quint32 reverted_from = 0;   // a copy of this revision number (0: none)
    bool current = false;
    bool on_current_path = false;  // the current revision or one of its ancestors
    // Automatic revisions (origin "auto"): which attempt of Auto's full-resolution check it is
    // ("chosen", "fallback", "baseline"; empty for other origins), and whether it failed that check
    // (it was not used: a more conservative attempt follows it in the history).
    QString auto_kind;
    bool auto_failed = false;
};

// ---- Automatic settings ------------------------------------------------------------------------

enum class AutoStage : std::uint8_t {
    Queued = 1,     // waiting for a thread
    Analysing = 2,  // decode and analysis
    Checking = 3,   // the full-resolution final check
    Saving = 4,     // writing the automatic revisions
    WaitingForMemory = 5,  // admitted once other work releases memory
};

struct AutoProgress {
    ItemId item = 0;
    AutoStage stage = AutoStage::Queued;
    double fraction = 0.0;  // of the whole run, 0..1
};

// The end of one Auto run (EditService::auto_finished).
struct AutoOutcome {
    ItemId item = 0;
    bool saved = false;           // automatic revisions were written
    bool cancelled = false;
    bool made_current = false;    // the result became the current revision (false: kept as a branch)
    quint32 revision_number = 0;  // the effective automatic revision (0: none)
    int revisions = 0;            // automatic revisions written (an attempt that failed the check included)
    bool review = false;          // the result recommends a review
    QString message;              // user-facing: what happened, or why nothing was saved
};

// What automatic settings did to the photo's current revision, from its analysis record (ADR 0013:
// versions, confidence, flags with reasons, the locks report and the final-check attempt).
// `revision` 0: the current revision is not an automatic one.
struct AutoSummary {
    quint64 revision = 0;
    quint32 revision_number = 0;
    bool from_batch = false;        // made by an automatic batch (otherwise by the editor's Auto)
    double confidence = 0.0;        // 0..1
    bool review = false;            // a review is recommended
    QStringList review_reasons;     // user-facing: why
    QStringList notes;              // user-facing: the other findings
    QList<EditControl> changed;     // the controls the revision changed
    QList<EditControl> locked;      // locked controls Auto would otherwise have set
    bool locks_prevented_targets = false;
    QStringList lock_notes;         // user-facing: what the locks did
    quint32 attempt = 0;            // 0: the optimiser's choice; n: the n-th conservative fallback
    bool baseline = false;          // the last resort: as shot for the controls Auto sets
    QString fallback_detail;        // why the earlier attempt was replaced (the failed checks; diagnostics)
    QString group;                  // the related-shot group it was matched to; empty: none
    bool anchor = false;            // an automatic batch kept it as its related shots' anchor

    friend bool operator==(const AutoSummary&, const AutoSummary&) = default;
};

enum class FramePhase : std::uint8_t {
    Quick = 1,  // the cached proxy; a Final frame of the same request may follow
    Final = 2,  // the settled result at the requested quality
};

// One rendered frame of an edit session. Immutable once published.
struct EditFrame {
    ItemId item = 0;
    quint64 sequence = 0;    // increasing per service in delivery order
    quint64 generation = 0;  // the session's request generation
    quint64 revision = 0;    // revision of the settings shown (0: as shot or a gesture position)
    quint64 transient_generation = 0;
    quint64 token = 0;       // the call whose settings the frame shows (0: none)
    FramePhase phase = FramePhase::Quick;
    bool before = false;           // the as-shot comparison render (before/after)
    bool full_resolution = false;  // rendered at 100 %
    QImage image;                  // upright; opaque
    // Clipping overlay of `image` (same size): highlights clipped in red, shadows in blue, other
    // pixels transparent. Null unless the overlay was asked for (EditService::set_clipping).
    QImage clipping;
    RenderInfoPtr info;  // histogram and render facts of `image`
    double latency_seconds = 0.0;     // the service's request to frame ready
    double stage_two_seconds = 0.0;
    double display_seconds = 0.0;
};

using EditFramePtr = std::shared_ptr<const EditFrame>;

// Outcome of a paste (blueprint section 2: "Copy/paste adjustments offers specific categories;
// copying a whole recipe across cameras must not copy camera calibration ... or camera-specific
// white-balance gains").
struct PasteReport {
    int targets = 0;
    int changed = 0;            // targets that got a new revision
    QList<SettingsCategory> applied;  // categories pasted into at least one target
    // User-facing: what was not pasted and why ("White balance: ..."), and targets that could not
    // be pasted into at all.
    QStringList skipped;
    QStringList errors;
};

class EditService : public QObject {
    Q_OBJECT

public:
    explicit EditService(QObject* parent = nullptr) : QObject(parent) {}
    ~EditService() override = default;

    EditService(const EditService&) = delete;
    EditService& operator=(const EditService&) = delete;

    // ---- Sessions ------------------------------------------------------------------------------

    virtual void open(ItemId item) = 0;
    virtual void close(ItemId item) = 0;
    // Fit (the proxy) or 100 % (Final frames at full resolution).
    virtual void set_full_resolution(ItemId item, bool full) = 0;
    // Keeps an as-shot render of the photo available (EditFrame::before) for before/after.
    virtual void set_comparison(ItemId item, bool wanted) = 0;
    // Frames carry clipping overlays (EditFrame::clipping) while wanted; turning it on re-sends the
    // newest frame with its overlay.
    virtual void set_clipping(bool wanted) = 0;

    // ---- Editing (see the file comment) ----------------------------------------------------------

    virtual quint64 set_transient(ItemId item, const EditSettings& settings) = 0;
    virtual quint64 commit(ItemId item) = 0;
    virtual quint64 apply(ItemId item, const EditSettings& settings) = 0;
    virtual quint64 set_lock(ItemId item, EditControl control, bool locked) = 0;
    virtual quint64 undo(ItemId item) = 0;
    virtual quint64 redo(ItemId item) = 0;
    // `revision` 0 selects "as shot".
    virtual quint64 select_revision(ItemId item, quint64 revision) = 0;
    virtual quint64 reset_to_as_shot(ItemId item) = 0;

    // ---- Copy and paste ------------------------------------------------------------------------

    // Copies the photo's current settings of the categories in `categories` (category_bit()).
    virtual void copy_settings(ItemId item, quint32 categories) = 0;
    [[nodiscard]] virtual bool has_clipboard() const = 0;
    virtual void paste_settings(const QList<ItemId>& targets) = 0;

    // ---- Automatic settings (see the file comment) ---------------------------------------------

    virtual void run_auto(ItemId item) = 0;
    // Stops the photo's run at its next safe point; nothing is written (auto_finished reports it).
    virtual void cancel_auto(ItemId item) = 0;
    // A run of `item` is in progress (item 0: of any photo).
    [[nodiscard]] virtual bool is_auto_running(ItemId item = 0) const = 0;

    // ---- Helpers ---------------------------------------------------------------------------------

    // The tone curve through `points` (EditSettings::tone_curve) at `count` evenly spaced x in
    // [0, 1], as the renderer evaluates it; empty for invalid points.
    [[nodiscard]] virtual QList<double> sample_tone_curve(const QList<QPointF>& points, int count) const = 0;

    // True while a command is pending or the newest settings have not been rendered (Final).
    [[nodiscard]] virtual bool is_busy() const = 0;

signals:
    void state_changed(const lucent::ui::EditState& state);
    void history_changed(lucent::ui::ItemId item, const QList<lucent::ui::EditHistoryEntry>& entries);
    void white_balance_changed(lucent::ui::ItemId item, const lucent::ui::WhiteBalanceInfo& info);
    void frame_ready(const lucent::ui::EditFramePtr& frame);
    // The photo cannot be rendered (user-facing reason).
    void frame_failed(lucent::ui::ItemId item, const QString& message);
    // An edit or command was refused (user-facing reason); state_changed follows with the state.
    void command_failed(lucent::ui::ItemId item, const QString& message);
    void copied(lucent::ui::ItemId item, quint32 categories);
    void pasted(const lucent::ui::PasteReport& report);
    void busy_changed(bool busy);
    void auto_progress(const lucent::ui::AutoProgress& progress);
    void auto_finished(const lucent::ui::AutoOutcome& outcome);
    void auto_summary_changed(lucent::ui::ItemId item, const lucent::ui::AutoSummary& summary);
};

}  // namespace lucent::ui
