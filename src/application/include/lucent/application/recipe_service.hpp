#pragma once

// RecipeService: the develop settings of library items, their immutable revisions and the editing
// commands on them (blueprint section 2, "Detailed individual editing"; section 8; ADR 0010).
//
// Current settings. An item shows the settings of its current revision, or "as shot" (default
// DevelopSettings, recipe.hpp) when it has none. state() reports them with their revision, the
// canonical document's SHA-256 (the recipe identity that previews, thumbnails and exports are
// keyed and recorded by) and what undo/redo can do.
//
// Gestures and coalescing. While a control is being dragged the user interface calls
// set_transient() for every position: the settings are validated and kept in memory as the item's
// transient settings (state().settings, with a transient generation that increases with every
// call), and no revision is written. commit() ends the gesture: when the transient settings differ
// from the current revision's, exactly ONE immutable revision is appended (origin User, the
// changed controls in its note); otherwise nothing is written. discard_transient() drops them.
// apply() is set_transient() + commit() for one-step edits (a typed value, a checkbox).
// Undo, redo, select, revert, reset, set_lock and paste drop a pending transient edit of the item
// first (callers commit a gesture before them).
//
// History. undo() moves to the parent revision ("as shot" at the root); redo() to the child undo()
// last left in this session when it is a child of the current revision (redo returns along the path
// the photographer came from), otherwise to the most recently created child that is not an automatic
// revision written as a branch (recipe_store.hpp, kAutomaticBranchNote: an Auto result that was not
// applied because the photo was edited meanwhile), otherwise to the most recently created child;
// select_revision() to any revision of the item (or "as shot"): they only
// move the current pointer, so an edit after an undo starts a new branch and the undone branch
// stays (recipe_store.hpp). reset_to_as_shot() appends a Reset revision whose settings are the
// defaults (locks are kept: they are the photographer's instructions to the optimiser, not
// photographic values), and revert_to() appends a copy of an earlier revision's settings: both are
// recorded steps that undo takes back. history() lists every revision with its parent, origin,
// changed controls and whether it lies on the path from "as shot" to the current one.
//
// Notes. Revisions written by the service carry a note of ';'-separated key=value fields:
// "changed=<controls>" (DevelopControl names, recipe.hpp, comma separated) and, for revert_to(),
// "from=<revision number>"; "locks=<controls>" lists the controls whose lock changed and
// "parts=<parts>" the parts that changed (RecipePart names: "chroma_denoise", "masks", "geometry",
// "lens_correction"; written only when one did). Readers ignore keys they do not know.
//
// Masks and geometry are edited like any control: a brush stroke or a crop drag is a gesture whose
// positions are set_transient() calls (each adds points to the stroke or moves the crop) and whose
// end is commit(), so a stroke or a drag is exactly one revision ("parts=masks" or
// "parts=geometry"). A lens-correction toggle or a profile choice is one apply() ("parts=
// lens_correction").
//
// Locks. set_lock() records a lock change as a revision (locks are part of the settings, which are
// immutable per revision). Locks are the optimiser's instructions (auto_develop.hpp): it changes only
// unlocked controls; nothing here enforces them.
//
// Automatic revisions (ADR 0013). apply_automatic() appends the revisions of an automatic run
// (recipe_store.hpp, "Automatic revisions": origin Auto, derived from the revision the run started
// from, each with its analysis record) and makes the effective one current when the item still
// shows that revision and no gesture is pending; undo then returns to the recipe before the run.
// write_automatic() is the same decision for a writer that appends the revisions itself (the batch
// service binds a job to its effective revision in the same catalog transaction): the write runs
// under the service's lock, so no commit, gesture or move of the item can interleave, and a gesture
// in progress keeps its own base (the automatic revisions are then a branch). refresh() re-reads an
// item whose revisions another writer changed in the store.
//
// Copy and paste by category (recipe.hpp, RecipeCategory). copy() takes the item's current
// settings and its camera; paste() applies the chosen categories to each target as ONE Paste
// revision per target that changes: every value of a pasted category becomes the copied one (the
// Masks category, chosen only deliberately, replaces the target's masks by the copied list, also by
// an empty one; the default categories never touch the masks).
// Camera-specific values never cross camera models:
//   * white balance "camera_neutral" (multipliers relative to one camera's sensor) is pasted only
//     to a target of the same camera make and model;
//   * white balance "kelvin" is pasted only to a target whose calibration supports Kelvin
//     (engine::WhiteBalanceModel::kelvin_available());
//   * "as_shot" and every other category are camera independent.
// Positions normalised to the image keep their place only on an image of the same shape (masks
// and the crop are normalised per axis to the upright image, develop_settings.hpp):
//   * the Masks category is pasted only when the copied list is empty (nothing to place) or the
//     source and the target have the same upright aspect ratio (same_upright_aspect()); otherwise
//     it is skipped (PasteSkip::part LocalAdjustments), since the masks would land on other content
//     and their shapes would distort;
//   * the LensCorrections category (version 3; not pasted unless chosen) belongs to one camera and
//     lens: a named profile describes one lens, and the corrections chosen for it (or the automatic
//     profile's) were judged on that lens's photos. It is pasted only to a target of the same camera
//     make and model (CameraCapabilities::same_camera) AND the same lens (same_lens: the lens model
//     both files record, equal); otherwise, also when either lens is unknown, it is skipped
//     (PasteSkip::part LensCorrection) with the reason. Copied settings made before lens corrections
//     existed (lens-correction operation version 0) hold none, so the category is skipped for every
//     target with that reason instead of switching the target's corrections off (W13 closure);
//   * the Geometry category (not pasted unless chosen: kDefaultRecipeCategories) is pasted when
//     its crop is the whole image (a straighten angle and quarter turns fit any photo) or the
//     upright aspect ratios are the same; otherwise it is skipped as a whole;
//   * an unknown upright size (no probe) counts as a different shape.
// Whatever is not pasted is reported per target (PasteOutcome::skipped) with the reason, never
// dropped silently; the locks of pasted controls travel with them. Kelvin edits by set_transient()
// and apply() are refused (Unsupported) for an item whose calibration does not support Kelvin.
//
// Camera capabilities. Paste and the Kelvin check need the camera make/model, the lens, the upright
// image size and whether Kelvin is available. They come from the item's probe (make, model, size) and from a
// probe of its file
// (RecipeServiceOptions::registry) the first time they are needed, cached per item until its file
// changes (LibraryChangeKind::Refreshed). An edit session that has decoded the file reports its
// metadata with note_camera() so no extra probe is needed. Without a registry, Kelvin availability
// is unknown: Kelvin values are then not pasted (reported) and Kelvin edits are not checked here
// (rendering still refuses them, develop.hpp).
//
// Read-only items (blueprint section 8: "Open incompatible recipes read-only with a clear
// reason"). When the store is read-only (a catalog written by a newer Lucent), every change fails
// with its reason. When the item's current revision cannot be read by this build (a newer schema,
// operation or base-look version, or values a newer catalog wrote: VersionMismatch; a damaged
// document, or one that fails its SHA-256 check: Corrupt or InvalidArgument), state().read_only says
// why (where kRecipeUnreadableWhere) and current_settings() fails with that error, so previews and
// exports refuse the item instead of rendering something else; edits, locks and pastes into it fail
// with the same error. Moving away from it stays possible (undo, redo, select_revision,
// reset_to_as_shot, revert_to a readable revision): those never interpret the unreadable recipe.
// Only the current revision matters: a damaged or newer revision elsewhere in the history never
// makes the item unusable (a history that cannot be listed fails history() alone).
//
// Observers (subscribe) are called after every change of an item (RecipeChange), on the thread that
// made it, after the service's lock is released, with the contract of library.hpp's observers.
//
// Thread-safety: all members may be called concurrently from any thread. Changes are serialised
// per service. Store writes are synchronous (a catalog commit, about a millisecond); paste() to many
// items and capability probes read files, so user interfaces call them off their UI thread.
// Lifetime: the library and the store must outlive the service.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/application/library.hpp>
#include <lucent/application/recipe.hpp>
#include <lucent/application/recipe_store.hpp>
#include <lucent/catalog/types.hpp>
#include <lucent/core/error.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/engine/develop_settings.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/raw/decoded_raw.hpp>
#include <lucent/raw/decoder_registry.hpp>

namespace lucent::app {

// The `where` of the error that makes an item's current recipe unreadable (RecipeState::read_only
// with recipe_unreadable, current_settings()); its code says why (VersionMismatch: made by a newer
// Lucent; otherwise damaged).
inline constexpr std::string_view kRecipeUnreadableWhere = "app.recipe.unreadable";

// What a camera supports, as far as recipes are concerned.
struct CameraCapabilities {
    std::string make;   // normalised (ProbeSummary::camera_make); empty when unknown
    std::string model;  // normalised; empty when unknown
    // The lens as the file records it ("<make> <model>", ProbeSummary::lens); empty when unknown.
    std::string lens;
    // The probe of the file succeeded: kelvin_available and monochrome are known.
    bool probed = false;
    bool kelvin_available = false;
    bool monochrome = false;
    std::string kelvin_unavailable_reason;  // developer-facing English
    // The full-resolution upright size (the visible area after the camera orientation); 0 x 0 when
    // unknown. What mask and crop positions are normalised to (see same_upright_aspect()).
    image::Size upright_size;

    // Same make and model, both known.
    [[nodiscard]] bool same_camera(const CameraCapabilities& other) const noexcept {
        return !make.empty() && !model.empty() && make == other.make && model == other.model;
    }
    // The same camera and the same lens, both recorded (the lens-correction paste rule).
    [[nodiscard]] bool same_lens(const CameraCapabilities& other) const noexcept {
        return same_camera(other) && !lens.empty() && lens == other.lens;
    }
};

// Relative tolerance of same_upright_aspect(): 3:2 sensors of different pixel counts (6000 x 4000,
// 8192 x 5464, 6048 x 4024) agree within 0.25 %, while 4:3 against 3:2 differs by 12 % and portrait
// against landscape by far more. A normalised position then lands within half the tolerance of the
// image size of the same relative place (15 pixels of 6000).
inline constexpr double kPasteAspectTolerance = 0.005;

// True when both sizes are known (not empty) and their aspect ratios (width / height) agree within
// kPasteAspectTolerance (relative).
[[nodiscard]] bool same_upright_aspect(image::Size a, image::Size b) noexcept;

struct RecipeState {
    ItemId item;
    // What the item shows now: the transient settings while a gesture is pending, otherwise the
    // current revision's (as shot without one). Meaningless when `read_only` holds a reason that
    // came from the recipe itself (see current_settings()).
    engine::DevelopSettings settings;
    engine::DevelopSettings committed;            // the current revision's settings (as shot without one)
    std::optional<catalog::RevisionId> revision;  // current revision; std::nullopt: as shot
    std::uint32_t revision_number = 0;            // of `revision` (0: as shot)
    core::Digest256 committed_sha256{};           // canonical document of `committed`
    core::Digest256 settings_sha256{};            // canonical document of `settings`
    bool transient = false;                       // a gesture is pending
    std::uint64_t transient_generation = 0;       // increases with every set_transient() of the item
    bool can_undo = false;
    bool can_redo = false;
    // Why the item cannot be edited (the store is read-only, or its recipe cannot be read).
    std::optional<Error> read_only;
    bool recipe_unreadable = false;  // read_only comes from the current revision itself
};

enum class RecipeChangeKind : std::uint8_t {
    Transient = 1,  // set_transient(): no revision
    Discarded = 2,  // a pending transient edit was dropped (discard_transient, or commit without change)
    Committed = 3,  // a revision was appended (an edit, a lock, a reset, a revert or a paste)
    Moved = 4,      // the current pointer moved (undo, redo, select_revision)
};

struct RecipeChange {
    RecipeChangeKind kind = RecipeChangeKind::Transient;
    std::uint64_t sequence = 0;                     // 1, 2, 3, ... per service in change order
    RecipeState state;                              // the item's state after the change
    std::optional<catalog::RevisionOrigin> origin;  // Committed
};

using RecipeObserver = std::function<void(const RecipeChange&)>;

// The result of apply_automatic().
struct AutomaticOutcome {
    std::vector<catalog::RevisionInfo> revisions;  // in the order given
    bool made_current = false;  // false: the item changed meanwhile; the revisions are another branch
    RecipeState state;          // the item's state afterwards
};

struct HistoryEntry {
    catalog::RevisionInfo info;
    bool current = false;                         // the item's current revision
    bool on_current_path = false;                 // the current revision or one of its ancestors
    std::vector<engine::DevelopControl> changed;  // from the note ("changed=")
    std::vector<engine::DevelopControl> locks;    // from the note ("locks=")
    std::vector<RecipePart> parts;                // from the note ("parts="): masks, geometry, denoising
    std::optional<std::uint32_t> reverted_from;   // from the note ("from="): a revert_to() copy
};

struct RecipeHistory {
    ItemId item;
    std::optional<catalog::RevisionId> current;
    std::vector<HistoryEntry> entries;  // oldest first
};

// The result of copy(): settings to paste and where they came from.
struct RecipeClipboard {
    ItemId source;
    engine::DevelopSettings settings;
    std::set<RecipeCategory> categories;
    CameraCapabilities camera;  // of the source
};

struct PasteSkip {
    RecipeCategory category = RecipeCategory::WhiteBalance;
    // The part of the category that was not pasted (Masks, Geometry: the whole category, which is
    // then not in PasteOutcome::applied); std::nullopt for a category whose controls were not pasted
    // (white balance).
    std::optional<RecipePart> part;
    std::string reason;  // developer-facing English
};

struct PasteOutcome {
    ItemId item;
    // Categories whose values were pasted (changed or not), wholly or but for a skipped part.
    std::vector<RecipeCategory> applied;
    std::vector<PasteSkip> skipped;               // categories not pasted, with the reason
    std::optional<catalog::RevisionId> revision;  // the Paste revision (std::nullopt: nothing changed)
    std::optional<Error> error;  // the target could not be pasted into (read-only, unknown, a store error)
};

struct RecipeServiceOptions {
    // Probes files for CameraCapabilities (see the file comment); null: Kelvin availability unknown.
    std::shared_ptr<const raw::DecoderRegistry> registry;
    raw::DecodeLimits probe_limits;
    // Recorded in every revision (NewRevision::engine_version); empty: core::build_info().version.
    std::string engine_version;
    // W12 (APL-06): when set, the engine version is this function's result instead (empty result:
    // core::build_info().version). It is asked for once, by the first revision written or the first
    // engine_version() call, outside the service's lock, and kept. It may block: Lucent.exe's names the
    // lens database, which loads off the UI thread (lens_database_source.hpp), so a revision never names
    // another database than the one the session renders with.
    std::function<std::string()> engine_version_source;
    // Time stamps of new revisions; empty: catalog::current_time().
    std::function<catalog::Timestamp()> clock;
};

class RecipeService {
public:
    RecipeService(Library& library, std::shared_ptr<RecipeStore> store, RecipeServiceOptions options = {});
    ~RecipeService();
    RecipeService(const RecipeService&) = delete;
    RecipeService& operator=(const RecipeService&) = delete;

    // The item's state. Errors ("app.recipe"): NotFound for an item the library does not have; the
    // store's read errors. An unreadable recipe is not an error here (state().read_only).
    [[nodiscard]] Expected<RecipeState> state(ItemId item) const;

    // The committed settings to render or export the item with, and their identity: the current
    // revision's, or as shot. Errors: those of state(); the recipe's own error (VersionMismatch,
    // InvalidArgument) when this build cannot read it.
    struct Current {
        engine::DevelopSettings settings;
        std::optional<catalog::RevisionId> revision;
        std::uint32_t revision_number = 0;
        RecipeDocument document;  // canonical text and SHA-256
    };
    [[nodiscard]] Expected<Current> current_settings(ItemId item) const;

    // The settings of one revision of the item (before/after comparisons, history previews).
    // NotFound when the revision is not one of the item's; the recipe's error when unreadable.
    [[nodiscard]] Expected<engine::DevelopSettings> revision_settings(ItemId item,
                                                                      catalog::RevisionId revision) const;

    // ---- Editing (see the file comment) --------------------------------------------------------

    // Older settings are kept as their version-3 conversion (engine::upgrade_develop_settings).
    // Errors: those of state(); the read-only reason; settings.validate()'s; Unsupported for Kelvin
    // white balance on a camera without a Kelvin calibration.
    [[nodiscard]] Expected<RecipeState> set_transient(ItemId item, const engine::DevelopSettings& settings);
    // Errors: as set_transient(); the store's (an AlreadyExists conflict reloads the item).
    [[nodiscard]] Expected<RecipeState> commit(ItemId item);
    [[nodiscard]] Expected<RecipeState> discard_transient(ItemId item);
    [[nodiscard]] Expected<RecipeState> apply(ItemId item, const engine::DevelopSettings& settings);

    // InvalidArgument when there is nothing to undo / redo (state().can_undo / can_redo).
    [[nodiscard]] Expected<RecipeState> undo(ItemId item);
    [[nodiscard]] Expected<RecipeState> redo(ItemId item);
    // NotFound / InvalidArgument for a revision that is not the item's.
    [[nodiscard]] Expected<RecipeState> select_revision(ItemId item,
                                                        std::optional<catalog::RevisionId> revision);
    // Appends a Reset revision (also when the item shows as-shot values without one, so the step is
    // recorded; nothing is written when the current revision is already exactly as shot).
    [[nodiscard]] Expected<RecipeState> reset_to_as_shot(ItemId item);
    // Appends a copy of `revision`'s settings (origin User, note "from=<number>"); nothing is
    // written when they equal the current settings.
    [[nodiscard]] Expected<RecipeState> revert_to(ItemId item, catalog::RevisionId revision);
    [[nodiscard]] Expected<RecipeState> set_lock(ItemId item, engine::DevelopControl control, bool locked);

    [[nodiscard]] Expected<RecipeHistory> history(ItemId item) const;

    // ---- Automatic revisions (see the file comment) ----------------------------------------------

    // Errors: those of state(); the read-only reason; the store's (InvalidArgument for a revision
    // whose origin is not Auto or a base of another item; Unsupported for a store without automatic
    // revisions). Observers receive Committed with origin Auto.
    [[nodiscard]] Expected<AutomaticOutcome> apply_automatic(ItemId item, std::optional<catalog::RevisionId> base,
                                                             std::span<const AutomaticRevision> revisions);
    // Runs `write` under the service's lock with whether the effective revision may become current
    // (the item shows `base` and no gesture is pending); `write` appends the revisions (for example
    // with append_automatic_revisions() inside the caller's catalog transaction, honouring the flag)
    // and returns what it appended. The item is then re-read and observers receive Committed (origin
    // Auto). Errors: those of state(); the read-only reason; those of `write` (the item is re-read).
    using AutomaticWrite = std::function<Expected<AutomaticAppend>(bool make_current)>;
    [[nodiscard]] Expected<AutomaticOutcome> write_automatic(ItemId item, std::optional<catalog::RevisionId> base,
                                                             const AutomaticWrite& write);
    // Re-reads the item's revisions from the store (a pending gesture is kept) and notifies
    // observers (Committed, origin Auto). Errors: those of state().
    [[nodiscard]] Expected<RecipeState> refresh(ItemId item);
    // The analysis records of the item's automatic revisions, oldest first (the store's).
    [[nodiscard]] Expected<std::vector<catalog::AutoAnalysisRecord>> automatic_analyses(ItemId item) const;
    // The engine version recorded in new revisions (RecipeServiceOptions::engine_version_source's or
    // engine_version, or this build's version). The first call may wait for engine_version_source.
    [[nodiscard]] std::string engine_version() const;
    // Time stamps of new revisions (RecipeServiceOptions::clock, or the current time).
    [[nodiscard]] catalog::Timestamp now() const;

    // ---- Copy and paste ------------------------------------------------------------------------

    // InvalidArgument for an empty or unknown category; the errors of current_settings().
    [[nodiscard]] Expected<RecipeClipboard> copy(ItemId item, const std::set<RecipeCategory>& categories);
    // One outcome per target, in order (a target's failure is reported in its outcome; the call
    // itself fails only for an invalid clipboard: InvalidArgument). Pasting into the source item
    // itself is allowed (it re-applies the copied values).
    [[nodiscard]] Expected<std::vector<PasteOutcome>> paste(const RecipeClipboard& clipboard,
                                                            std::span<const ItemId> targets);

    // ---- Cameras -------------------------------------------------------------------------------

    // The item's camera capabilities (see the file comment); a probe failure is reported as
    // capabilities with probed = false, not as an error. NotFound for an unknown item.
    [[nodiscard]] Expected<CameraCapabilities> camera(ItemId item) const;
    // Records what an edit session decoded for `item` (WhiteBalanceModel of its metadata).
    void note_camera(ItemId item, const raw::RawMetadata& metadata);

    // Registers an observer (library.hpp contract).
    [[nodiscard]] Subscription subscribe(RecipeObserver observer);

    // Why every change fails (the store's read-only reason).
    [[nodiscard]] std::optional<Error> read_only_reason() const;

    struct State;  // implementation detail

private:
    std::shared_ptr<State> state_;
    Subscription library_subscription_;
};

// Camera capabilities of decoded or probed metadata (make/model from the decoder's normalised
// identity; the lens as summarize_probe() records it; Kelvin from engine::WhiteBalanceModel).
[[nodiscard]] CameraCapabilities camera_capabilities(const raw::RawMetadata& metadata);

}  // namespace lucent::app
