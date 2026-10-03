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
// History. undo() moves to the parent revision ("as shot" at the root), redo() to the most
// recently created child, select_revision() to any revision of the item (or "as shot"): they only
// move the current pointer, so an edit after an undo starts a new branch and the undone branch
// stays (recipe_store.hpp). reset_to_as_shot() appends a Reset revision whose settings are the
// defaults (locks are kept: they are the photographer's instructions to the optimiser, not
// photographic values), and revert_to() appends a copy of an earlier revision's settings: both are
// recorded steps that undo takes back. history() lists every revision with its parent, origin,
// changed controls and whether it lies on the path from "as shot" to the current one.
//
// Notes. Revisions written by the service carry a note of ';'-separated key=value fields:
// "changed=<controls>" (DevelopControl names, recipe.hpp, comma separated) and, for revert_to(),
// "from=<revision number>"; "locks=<controls>" lists the controls whose lock changed. Readers
// ignore keys they do not know.
//
// Locks. set_lock() records a lock change as a revision (locks are part of the settings, which are
// immutable per revision). Locks are stored for the optimiser (W8); nothing here enforces them.
//
// Copy and paste by category (recipe.hpp, RecipeCategory). copy() takes the item's current
// settings and its camera; paste() applies the chosen categories to each target as ONE Paste
// revision per target that changes. Camera-specific values never cross camera models:
//   * white balance "camera_neutral" (multipliers relative to one camera's sensor) is pasted only
//     to a target of the same camera make and model;
//   * white balance "kelvin" is pasted only to a target whose calibration supports Kelvin
//     (engine::WhiteBalanceModel::kelvin_available());
//   * "as_shot" and every other category are camera independent.
// Whatever is not pasted is reported per target (PasteOutcome::skipped) with the reason, never
// dropped silently; the locks of pasted controls travel with them. Kelvin edits by set_transient()
// and apply() are refused (Unsupported) for an item whose calibration does not support Kelvin.
//
// Camera capabilities. Paste and the Kelvin check need the camera make/model and whether Kelvin is
// available. They come from the item's probe (make, model) and from a probe of its file
// (RecipeServiceOptions::registry) the first time they are needed, cached per item until its file
// changes (LibraryChangeKind::Refreshed). An edit session that has decoded the file reports its
// metadata with note_camera() so no extra probe is needed. Without a registry, Kelvin availability
// is unknown: Kelvin values are then not pasted (reported) and Kelvin edits are not checked here
// (rendering still refuses them, develop.hpp).
//
// Read-only items (blueprint section 8: "Open incompatible recipes read-only with a clear
// reason"). When the store is read-only (a catalog written by a newer Lucent), every change fails
// with its reason. When the item's current revision cannot be read by this build (a newer schema,
// operation or base-look version: VersionMismatch; or a damaged document), state().read_only says
// why and current_settings() fails with that error, so previews and exports refuse the item
// instead of rendering something else; edits, locks and pastes into it fail with the same error.
// Moving away from it stays possible (undo, redo, select_revision, reset_to_as_shot, revert_to a
// readable revision): those never interpret the unreadable recipe.
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
#include <vector>

#include <lucent/application/library.hpp>
#include <lucent/application/recipe.hpp>
#include <lucent/application/recipe_store.hpp>
#include <lucent/catalog/types.hpp>
#include <lucent/core/error.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/engine/develop_settings.hpp>
#include <lucent/raw/decoded_raw.hpp>
#include <lucent/raw/decoder_registry.hpp>

namespace lucent::app {

// What a camera supports, as far as recipes are concerned.
struct CameraCapabilities {
    std::string make;   // normalised (ProbeSummary::camera_make); empty when unknown
    std::string model;  // normalised; empty when unknown
    // The probe of the file succeeded: kelvin_available and monochrome are known.
    bool probed = false;
    bool kelvin_available = false;
    bool monochrome = false;
    std::string kelvin_unavailable_reason;  // developer-facing English

    // Same make and model, both known.
    [[nodiscard]] bool same_camera(const CameraCapabilities& other) const noexcept {
        return !make.empty() && !model.empty() && make == other.make && model == other.model;
    }
};

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

struct HistoryEntry {
    catalog::RevisionInfo info;
    bool current = false;                         // the item's current revision
    bool on_current_path = false;                 // the current revision or one of its ancestors
    std::vector<engine::DevelopControl> changed;  // from the note ("changed=")
    std::vector<engine::DevelopControl> locks;    // from the note ("locks=")
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
    std::string reason;  // developer-facing English
};

struct PasteOutcome {
    ItemId item;
    std::vector<RecipeCategory> applied;          // categories whose values were pasted (changed or not)
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
// identity; Kelvin from engine::WhiteBalanceModel).
[[nodiscard]] CameraCapabilities camera_capabilities(const raw::RawMetadata& metadata);

}  // namespace lucent::app
