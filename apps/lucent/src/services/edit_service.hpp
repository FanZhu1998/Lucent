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
// Version 2 (W9 part 2; ADR 0015): chroma noise reduction, masks and geometry are part of the
// settings like any control. Brush strokes and crop drags are gestures (set_transient() per
// position, commit() at the end: one revision each). While the crop tool is open the frames are
// rendered without the crop (set_crop_editing(): EditFrame::uncropped) so the whole straightened
// image shows under the crop overlay; set_mask_overlay() adds a mask's weight tint to the frames.
// The geometry helpers (map_geometry(), crop_inside_image(), largest_inside_crop(), turned(),
// mask_outline()) are the engine's (develop_geometry.hpp, local_adjustments.hpp): the view maps
// handles and pointer positions through them and never re-implements the frames.
//
// Version 3 (W13; ADR 0023): lens-profile corrections are part of the settings (EditSettings::lens):
// automatic profile or a named one, and separate switches for distortion, chromatic aberration and
// vignetting; each change is one apply() (one revision). Every frame says what its render did for the
// lens (EditFrame::lens: the database match with its quality and reasons, the profile used, each
// correction's outcome, what the file corrects itself) and carries the correction's point mapping
// (EditFrame::lens_map), which the tools put between mask positions (defined in the uncorrected
// image) and the displayed pixels; the crop and the straightening stay in the corrected frame.
// lens_database() says which database the session uses (or why none); search_lens_profiles()
// searches it for the manual profile choice. Auto never changes the lens settings.
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
#include <optional>

#include <QDateTime>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QPolygonF>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTransform>

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

// Copy and paste categories (the recipe contract's RecipeCategory values; stable). The editor offers
// all nine (kAllSettingsCategories); Masks and Geometry are not selected by default (placed on one
// photo's content), nor LensCorrections (one camera's and lens's: pasted only to photos of the same
// camera and lens): kDefaultCategoryBits, the recipe contract's kDefaultRecipeCategories.
enum class SettingsCategory : std::uint8_t {
    WhiteBalance = 1,
    Exposure = 2,
    Tone = 3,             // contrast, tone curve, base look
    Color = 4,            // saturation, vibrance
    Local = 5,            // highlights and shadows
    Detail = 6,           // chroma noise reduction
    Geometry = 7,         // crop, straighten, quarter turns
    Masks = 8,            // the masks (pasting them replaces the photo's own)
    LensCorrections = 9,  // the lens-profile corrections (version 3)
};

[[nodiscard]] constexpr quint32 category_bit(SettingsCategory category) noexcept {
    return quint32{1} << static_cast<unsigned>(category);
}

inline constexpr std::array<SettingsCategory, 9> kAllSettingsCategories = {
    SettingsCategory::WhiteBalance, SettingsCategory::Exposure, SettingsCategory::Tone,
    SettingsCategory::Color,        SettingsCategory::Local,    SettingsCategory::Masks,
    SettingsCategory::Detail,       SettingsCategory::Geometry, SettingsCategory::LensCorrections};

inline constexpr quint32 kDefaultCategoryBits =
    category_bit(SettingsCategory::WhiteBalance) | category_bit(SettingsCategory::Exposure) |
    category_bit(SettingsCategory::Tone) | category_bit(SettingsCategory::Color) |
    category_bit(SettingsCategory::Local) | category_bit(SettingsCategory::Detail);

inline constexpr quint32 kAllCategoryBits = kDefaultCategoryBits | category_bit(SettingsCategory::Masks) |
                                            category_bit(SettingsCategory::Geometry) |
                                            category_bit(SettingsCategory::LensCorrections);

enum class WhiteBalanceMode : std::uint8_t {
    AsShot = 1,
    Kelvin = 2,         // temperature and tint (DNG SDK units); only with a Kelvin calibration
    CameraNeutral = 3,  // camera red, green, blue multipliers (relative to the native values)
};

// Ranges of the recipe contract ("lucent.develop" v2, engine/develop_settings.hpp).
inline constexpr double kExposureLimit = 5.0;     // exposure_ev in [-5, 5]
inline constexpr double kAdjustmentLimit = 1.0;   // contrast .. vibrance in [-1, 1]
inline constexpr double kMinTemperature = 2000.0;
inline constexpr double kMaxTemperature = 50000.0;
inline constexpr double kTintLimit = 150.0;
inline constexpr double kMaxMultiplierRatio = 64.0;
inline constexpr int kMinCurvePoints = 2;
inline constexpr int kMaxCurvePoints = 32;
// Version 2 (ADR 0015).
inline constexpr double kMinDenoiseRadius = 1.0;  // full-image pixels
inline constexpr double kMaxDenoiseRadius = 64.0;
inline constexpr double kDefaultDenoiseRadius = 8.0;
inline constexpr double kMaxStraighten = 45.0;   // degrees
inline constexpr double kMinCropExtent = 0.001;  // of the width and of the height
inline constexpr int kMaxMasks = 32;
inline constexpr int kMaxBrushStrokes = 256;
inline constexpr int kMaxStrokePoints = 4096;
inline constexpr int kMaxBrushPoints = 65536;    // all strokes of all masks
inline constexpr double kMinBrushRadius = 1e-4;  // fractions of the upright long edge
inline constexpr double kMaxBrushRadius = 0.5;
inline constexpr double kMinRadialRadius = 1e-4;
inline constexpr double kMaxRadialRadius = 2.0;
inline constexpr double kMaskPositionMargin = 1.0;  // positions within [-1, 2]
inline constexpr double kMinGradientLength = 1e-4;
inline constexpr double kMaxLocalExposure = 4.0;  // EV; the other local controls in [-1, 1]

// Guided chroma noise reduction (engine ChromaDenoiseSetting): strength 0 (off) .. 1, radius in
// full-image pixels.
struct EditDenoise {
    double strength = 0.0;
    double radius = kDefaultDenoiseRadius;

    friend bool operator==(const EditDenoise&, const EditDenoise&) = default;
};

// A crop normalised in the straightened frame (fractions of the width and height; the default keeps
// the whole image).
struct EditCrop {
    double left = 0.0;
    double top = 0.0;
    double right = 1.0;
    double bottom = 1.0;

    [[nodiscard]] bool is_full() const noexcept { return left == 0.0 && top == 0.0 && right == 1.0 && bottom == 1.0; }
    [[nodiscard]] double width() const noexcept { return right - left; }
    [[nodiscard]] double height() const noexcept { return bottom - top; }

    friend bool operator==(const EditCrop&, const EditCrop&) = default;
};

// Straighten (degrees; positive turns the content counter-clockwise as displayed), crop, quarter
// turns clockwise and a left-right mirror, applied in that order after everything else (engine
// GeometrySetting).
struct EditGeometry {
    double straighten_degrees = 0.0;
    EditCrop crop;
    int quarter_turns = 0;  // 0..3
    bool flip_horizontal = false;

    [[nodiscard]] bool is_identity() const noexcept {
        return straighten_degrees == 0.0 && crop.is_full() && quarter_turns == 0 && !flip_horizontal;
    }

    friend bool operator==(const EditGeometry&, const EditGeometry&) = default;
};

// The engine's mask kinds and brush modes (stable values).
enum class MaskKind : std::uint8_t {
    Brush = 1,
    Linear = 2,
    Radial = 3,
};

enum class BrushMode : std::uint8_t {
    Add = 1,
    Erase = 2,
};

// One brush stroke: the polyline through `points` (normalised upright coordinates) painted with a
// round brush of `radius` (fraction of the long edge), `hardness` (fraction of the radius at full
// strength) and `flow`.
struct EditBrushStroke {
    BrushMode mode = BrushMode::Add;
    double radius = 0.02;
    double hardness = 0.5;
    double flow = 1.0;
    QList<QPointF> points;

    friend bool operator==(const EditBrushStroke&, const EditBrushStroke&) = default;
};

// A mask's local controls (engine LocalControls): exposure in EV, the others -1..+1.
struct EditLocalControls {
    double exposure_ev = 0.0;
    double contrast = 0.0;
    double highlights = 0.0;
    double shadows = 0.0;
    double saturation = 0.0;
    double temperature = 0.0;
    double tint = 0.0;

    [[nodiscard]] bool is_identity() const noexcept { return *this == EditLocalControls{}; }

    friend bool operator==(const EditLocalControls&, const EditLocalControls&) = default;
};

// One mask and its controls (engine LocalAdjustment). Positions are normalised upright coordinates
// (develop_settings.hpp, "Coordinates"); lengths are fractions of the upright long edge. Only the
// members of `kind` are used: Linear start, end, feather; Radial center, radius_x, radius_y,
// angle_degrees, feather; Brush strokes. The recipe stores no names: the editor names masks by
// kind and order (MaskListModel).
struct EditMask {
    quint32 id = 1;  // >= 1, unique within the settings
    bool enabled = true;
    bool invert = false;
    double opacity = 1.0;
    MaskKind kind = MaskKind::Radial;
    QPointF start{0.5, 0.25};
    QPointF end{0.5, 0.75};
    QPointF center{0.5, 0.5};
    double radius_x = 0.25;
    double radius_y = 0.25;
    double angle_degrees = 0.0;
    double feather = 0.5;  // linear: 1 by default; radial: 0.5
    QList<EditBrushStroke> strokes;
    EditLocalControls controls;

    // Enabled, opacity above 0 and a control not 0 (engine LocalAdjustment::has_effect()).
    [[nodiscard]] bool has_effect() const noexcept { return enabled && opacity > 0.0 && !controls.is_identity(); }

    friend bool operator==(const EditMask&, const EditMask&) = default;
};

// A lens profile of the lens database (engine LensProfileId: the database's maker and model, and the
// crop factor of the camera the lens was calibrated with).
struct EditLensProfile {
    QString maker;
    QString model;
    double crop_factor = 1.0;

    friend bool operator==(const EditLensProfile&, const EditLensProfile&) = default;
};

// The lens-profile corrections (engine LensCorrectionSetting with its operation version; ADR 0023).
struct EditLensCorrection {
    // False: the settings were made before Lucent had lens corrections (operation version 0): nothing
    // is corrected and the members below are not used. Turning corrections on makes it true (version 1).
    bool enabled = true;
    bool distortion = true;
    bool chromatic_aberration = true;  // lateral chromatic aberration (colour fringes)
    bool vignetting = true;
    // std::nullopt: automatic, the database's exact match for the photo's camera and lens only.
    std::optional<EditLensProfile> profile;

    friend bool operator==(const EditLensCorrection&, const EditLensCorrection&) = default;
};

// The photographic settings the editor shows and changes (DevelopSettings v3 without the parts the
// editor does not change: the base look and the other operation versions are kept by the service).
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
    // Version 2.
    EditDenoise chroma_denoise;
    QList<EditMask> masks;  // applied in this order
    EditGeometry geometry;
    // Version 3.
    EditLensCorrection lens;

    [[nodiscard]] bool locked(EditControl control) const noexcept { return (locks & control_bit(control)) != 0; }
    void set_locked(EditControl control, bool locked) noexcept {
        locks = locked ? (locks | control_bit(control)) : (locks & ~control_bit(control));
    }
    // Every photographic value is the default (locks ignored): the photo renders as shot. The lens
    // corrections count as shot when they are the defaults (automatic, all on) or not used (made before
    // Lucent had them), as the recipe contract says.
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

// The parts of the settings a revision changed (the recipe contract's RecipePart values; stable).
enum class EditPart : std::uint8_t {
    ChromaDenoise = 1,
    Masks = 2,
    Geometry = 3,
    LensCorrections = 4,  // version 3
};

// User-facing names: "White balance", "Exposure", "Contrast", "Highlights", "Shadows", "Tone curve",
// "Saturation", "Vibrance", "Base look"; the categories "White balance", "Exposure", "Tone",
// "Color", "Highlights and shadows", "Masks", "Noise reduction", "Crop and rotation", "Lens
// corrections"; the parts "Noise reduction", "Masks", "Crop and rotation", "Lens corrections"; the
// mask kinds "Brush", "Linear gradient", "Radial gradient".
[[nodiscard]] QString control_label(EditControl control);
[[nodiscard]] QString category_label(SettingsCategory category);
[[nodiscard]] QString part_label(EditPart part);
[[nodiscard]] QString mask_kind_label(MaskKind kind);

struct EditHistoryEntry {
    quint64 revision = 0;
    quint32 number = 0;
    quint32 parent_number = 0;  // 0: derived from as shot
    // "user", "auto", "reset", "paste", "import".
    QString origin;
    QDateTime created;  // UTC
    QList<EditControl> changed;  // controls whose values changed in this revision
    QList<EditControl> locks;    // controls whose lock changed
    QList<EditPart> parts;  // parts whose values changed (masks, crop, noise reduction, lens corrections)
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

// ---- Lens corrections of frames (version 3; ADR 0023) -------------------------------------------------

// The lens-profile correction's point mapping of a frame, in full-resolution upright pixels
// (continuous coordinates, pixel (i, j) covering [i, i + 1) x [j, j + 1)): the corrected image U'
// (the same size as the upright image U) shows at `corrected` the content of U at upright_of().
// Masks are defined in U; the crop and the straightening work in U'. Immutable; any thread.
class LensPointMap {
public:
    virtual ~LensPointMap() = default;
    [[nodiscard]] virtual QPointF upright_of(QPointF corrected) const = 0;
    // The corrected point that shows `upright`; std::nullopt where the model cannot be inverted (far
    // outside the image).
    [[nodiscard]] virtual std::optional<QPointF> corrected_of(QPointF upright) const = 0;
};

// The quality of the lens database's match for the photo's camera and lens (engine LensMatchKind).
enum class LensMatchQuality : std::uint8_t {
    None = 1,   // no profile for this camera and lens (or no lens recorded)
    Fuzzy = 2,  // similar profiles, but none is certainly this lens: nothing is applied automatically
    Exact = 3,  // this lens's profile: applied automatically
};

// What became of one correction in a frame's render (engine LensCorrectionOutcome).
enum class LensOutcome : std::uint8_t {
    Applied = 1,
    Disabled = 2,       // switched off (or the settings predate lens corrections)
    CarriedByFile = 3,  // the file already corrects it (DNG opcodes): never applied twice
    Withheld = 4,       // automatic profile, and the file corrects its lens itself
    NoProfile = 5,      // no profile (no exact match, no database, the named profile unknown)
    NotApplicable = 6,  // chromatic aberration of a monochrome photo
    NoCalibration = 7,  // the profile does not cover this photo (focal length, aperture, sensor)
};

// One correction of a frame (distortion, chromatic aberration or vignetting).
struct LensCorrectionState {
    LensOutcome outcome = LensOutcome::Disabled;
    QString detail;  // developer-facing English (the engine's), for the panel's details
};

// What the lens-profile correction of a frame's render did (the engine's LensCorrectionPlan).
struct LensInfo {
    bool database = false;  // a lens database was used (false: corrections unavailable)
    QString database_version;
    bool enabled = true;    // operation version 1 (false: the settings predate lens corrections)
    bool automatic = true;  // the profile is chosen automatically
    // The photo's camera and lens as the file records them, its focal length and f-number (0: unknown).
    QString camera;
    QString lens;
    double focal_length_mm = 0.0;
    double f_number = 0.0;
    LensMatchQuality match = LensMatchQuality::None;
    QString database_camera;  // the camera the database identified ("Nikon Z 6"); empty: none
    QString database_mount;   // its mount ("Nikon Z")
    // Mounts whose lenses fit that camera: its own and those of the lenses the database's search
    // returned for it (mounts it accepts through an adapter); empty without an identified camera.
    QStringList compatible_mounts;
    std::optional<EditLensProfile> matched;  // the exact match
    QList<EditLensProfile> candidates;       // a fuzzy match's candidates, best first
    QStringList reasons;                     // the match's reasons (developer-facing English)
    std::optional<EditLensProfile> profile;  // the profile used (automatic: the exact match), if any
    // Distortion, chromatic aberration, vignetting.
    std::array<LensCorrectionState, 3> corrections;
    // What the file corrects itself (applied DNG opcodes).
    bool carried_distortion = false;
    bool carried_chromatic_aberration = false;
    bool carried_vignetting = false;
    double scale = 1.0;  // the correction's magnification (1: nothing resampled)
};

using LensInfoPtr = std::shared_ptr<const LensInfo>;

// A profile of the lens database, as the manual choice lists them (engine LensProfileSummary).
struct LensProfileEntry {
    EditLensProfile profile;
    QString mounts;     // ", " separated
    QString lens_type;  // "rectilinear", "fisheye", ...
    double min_focal_mm = 0.0;
    double max_focal_mm = 0.0;
    double min_f_number = 0.0;
    bool distortion = false;  // calibrations the profile holds
    bool chromatic_aberration = false;
    bool vignetting = false;
};

// ---- Geometry and masks of frames -----------------------------------------------------------------

// The point mapping of a geometry for an upright image of `upright_size` pixels and its frame F
// (the engine's GeometryMapping, develop_geometry.hpp; F is the camera's image area for geometry
// version 2, the whole image otherwise). Every geometry step is affine, so the transforms are exact;
// a lens correction (ADR 0023) is not: `lens` is its point mapping, which lies between the masks'
// normalised upright coordinates and the transforms (normalised coordinates of the corrected image).
// Map mask positions with output_from_normalized() and normalized_from_output(), which apply both.
struct GeometryMap {
    bool valid = false;  // false: an empty size or an invalid geometry
    QSize upright_size;
    QRect frame;        // F in the upright image (the whole image when the geometry has no other)
    QSize output_size;
    QRect crop_pixels;  // the crop in the straightened frame, whole pixels
    // Normalised upright (mask) coordinates -> continuous output pixel coordinates.
    QTransform normalized_to_output;
    // Normalised straightened (crop) coordinates, relative to the frame F -> continuous output pixel
    // coordinates.
    QTransform straightened_to_output;
    // Output (and upright) pixels per normalised length: max(W, H).
    double pixels_per_unit_length = 0.0;
    // The lens correction of the pixels (null: none); full-resolution upright pixels.
    std::shared_ptr<const LensPointMap> lens;

    // Normalised upright (mask) coordinates <-> continuous output pixels, through the lens correction
    // when there is one; std::nullopt when the map is not valid or the point cannot be mapped.
    [[nodiscard]] std::optional<QPointF> output_from_normalized(QPointF normalized) const;
    [[nodiscard]] std::optional<QPointF> normalized_from_output(QPointF output) const;
};

// What the user turns or mirrors as displayed (engine rotated_clockwise() and the others).
enum class GeometryTurn : std::uint8_t {
    Clockwise = 1,
    CounterClockwise = 2,
    FlipHorizontal = 3,
};

// A mask's outline in a frame (DevelopFrame::masks): engine::mask_outline() mapped to the frame and
// expressed as fractions of the frame image's width and height (0..1 inside the image).
struct FrameMaskOutline {
    quint32 id = 0;
    MaskKind kind = MaskKind::Radial;
    bool enabled = true;
    bool invert = false;
    QList<QPolygonF> polylines;
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
    // Version 2 (ADR 0015). The photo's upright size at full resolution (before the geometry) and
    // the image's size at full resolution (after it); empty when unknown.
    QSize full_upright_size;
    QSize full_output_size;
    // The frame F the geometry works in, in full-resolution upright pixels (ADR 0017): the camera's
    // image area for a geometry-version-2 recipe of a file that declares one, the whole upright image
    // otherwise (also when empty). Crops are normalised in it; masks stay normalised in the whole
    // upright image (QUA-05: the tools map handles and pointer positions through it).
    QRect full_frame;
    EditGeometry geometry;   // of the pixels (the crop is the whole image when `uncropped`)
    bool uncropped = false;  // rendered without the crop for the crop tool (set_crop_editing)
    QList<FrameMaskOutline> masks;  // every mask of the settings shown, in their order
    // Version 3 (ADR 0023): what the render did for the lens, and its point mapping (null: nothing
    // resampled; the masks are mapped through it).
    LensInfoPtr lens;
    std::shared_ptr<const LensPointMap> lens_map;
    // Weight overlay of mask `mask_overlay_id` (set_mask_overlay): premultiplied ARGB, the size of
    // the image or smaller (stretched over it). Null unless asked for.
    QImage mask_overlay;
    quint32 mask_overlay_id = 0;
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
    // While `editing`, frames of the photo are rendered without the crop (straighten, quarter turns
    // and mirror kept; EditFrame::uncropped) so the crop tool shows the whole image.
    virtual void set_crop_editing(ItemId item, bool editing) = 0;
    // Frames carry the weight overlay of mask `mask_id` (EditFrame::mask_overlay; 0: none); a change
    // re-sends the newest frame with it.
    virtual void set_mask_overlay(ItemId item, quint32 mask_id) = 0;

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

    // The engine's geometry and mask helpers (develop_geometry.hpp, local_adjustments.hpp; pure).
    // `frame` is F in the upright image (EditFrame::full_frame at full resolution); an empty rectangle
    // is the whole image.
    [[nodiscard]] virtual GeometryMap map_geometry(const EditGeometry& geometry, QSize upright_size,
                                                   QRect frame) const = 0;
    // The straightened frame covers the whole crop (no black canvas in the output); `frame_size` is
    // F's size (the whole upright image's for geometry version 1).
    [[nodiscard]] virtual bool crop_inside_image(const EditGeometry& geometry, QSize frame_size) const = 0;
    // The largest crop centred on the frame with `aspect` (width / height in pixels before the
    // quarter turns; <= 0: the frame's own) that the straightened frame of `frame_size` covers.
    [[nodiscard]] virtual EditCrop largest_inside_crop(double straighten_degrees, QSize frame_size,
                                                       double aspect) const = 0;
    [[nodiscard]] virtual EditGeometry turned(const EditGeometry& geometry, GeometryTurn turn) const = 0;
    // engine::mask_outline() of `mask` in normalised upright coordinates (empty for an invalid mask).
    [[nodiscard]] virtual QList<QPolygonF> mask_outline(const EditMask& mask, QSize upright_size) const = 0;

    // ---- Lens corrections (version 3; see the file comment) -----------------------------------------

    // The lens database the session's renders correct lenses with, or why there is none. The default
    // has none.
    [[nodiscard]] virtual LensDatabaseInfo lens_database() const { return {}; }
    // Profiles of the lens database whose maker and model hold every word of `text` (case ignored;
    // an empty text lists them all), at most `limit`, by maker and model. The default has none.
    [[nodiscard]] virtual QList<LensProfileEntry> search_lens_profiles(const QString& text, int limit) const {
        (void)text;
        (void)limit;
        return {};
    }

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
