// RecipeService (recipe_service.hpp).

#include <algorithm>
#include <charconv>
#include <exception>
#include <format>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <lucent/application/recipe_service.hpp>
#include <lucent/core/log.hpp>
#include <lucent/core/text.hpp>
#include <lucent/core/version.hpp>
#include <lucent/engine/white_balance_model.hpp>

#include "observer_list.hpp"

namespace lucent::app {

namespace {

constexpr std::string_view kWhere = "app.recipe";
constexpr std::string_view kWherePaste = "app.recipe.paste";
constexpr std::string_view kWhereUnreadable = kRecipeUnreadableWhere;

std::string join_controls(const std::vector<engine::DevelopControl>& controls) {
    std::string out;
    for (const engine::DevelopControl control : controls) {
        out += out.empty() ? "" : ",";
        out += engine::to_string(control);
    }
    return out;
}

std::vector<engine::DevelopControl> parse_controls(std::string_view text) {
    std::vector<engine::DevelopControl> out;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string_view name = text.substr(0, comma);
        if (const auto control = engine::parse_develop_control(name)) {
            out.push_back(*control);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    return out;
}

// The note of a revision written by the service (see the header).
std::string make_note(const engine::DevelopSettings& from, const engine::DevelopSettings& to,
                      std::optional<std::uint32_t> reverted_from = std::nullopt) {
    std::string note = "changed=" + join_controls(changed_controls(from, to));
    if (const auto locks = changed_locks(from, to); !locks.empty()) {
        note += ";locks=" + join_controls(locks);
    }
    if (reverted_from) {
        note += std::format(";from={}", *reverted_from);
    }
    return note;
}

void parse_note(std::string_view note, HistoryEntry& entry) {
    while (!note.empty()) {
        const std::size_t semicolon = note.find(';');
        const std::string_view field = note.substr(0, semicolon);
        const std::size_t equals = field.find('=');
        if (equals != std::string_view::npos) {
            const std::string_view key = field.substr(0, equals);
            const std::string_view value = field.substr(equals + 1);
            if (key == "changed") {
                entry.changed = parse_controls(value);
            } else if (key == "locks") {
                entry.locks = parse_controls(value);
            } else if (key == "from") {
                std::uint32_t number = 0;
                if (std::from_chars(value.data(), value.data() + value.size(), number).ec == std::errc{}) {
                    entry.reverted_from = number;
                }
            }
        }
        if (semicolon == std::string_view::npos) {
            break;
        }
        note.remove_prefix(semicolon + 1);
    }
}

// The settings of a stored revision, or why this build cannot use them.
Expected<engine::DevelopSettings> settings_of(const catalog::RecipeRevision& revision) {
    if (revision.info.schema_name != kRecipeSchema) {
        return fail(ErrorCode::Unsupported,
                    std::format("revision {} holds a '{}' document, not a '{}' recipe",
                                revision.info.revision_number, revision.info.schema_name, kRecipeSchema),
                    std::string(kWhere));
    }
    if (revision.info.schema_version > kRecipeSchemaVersion) {
        return fail(
            ErrorCode::VersionMismatch,
            std::format("revision {} is a version {} recipe; this build reads version {} (it was made by a "
                        "newer Lucent)",
                        revision.info.revision_number, revision.info.schema_version, kRecipeSchemaVersion),
            std::string(kWhere));
    }
    return read_recipe(revision.settings_json);
}

// A store error that describes stored revisions (a field this build cannot decode, a document that
// fails its SHA-256 check) rather than the store failing to work (I/O, memory, a missing item): the
// item stays usable, read-only where its current recipe is affected.
bool is_stored_data_error(const Error& error) noexcept {
    return error.code == ErrorCode::Corrupt || error.code == ErrorCode::VersionMismatch;
}

// `error` as the reason the item's current recipe cannot be used: where "app.recipe.unreadable" (so
// user interfaces can say that the edit, not the photo, cannot be read), code kept, the original
// location kept in the message for logs.
Error unreadable_recipe(const Error& error) {
    if (error.where == kWhereUnreadable) {
        return error;
    }
    return Error{error.code, std::format("{} [{}]", error.message, error.where),
                 std::string(kWhereUnreadable), error.native_code};
}

// Exceptions never leave the module (conventions): allocation failures become OutOfMemory.
Unexpected out_of_memory() {
    return fail(ErrorCode::OutOfMemory, "the recipe service ran out of memory", std::string(kWhere));
}

std::string camera_name(const CameraCapabilities& camera) {
    if (camera.make.empty() && camera.model.empty()) {
        return "an unknown camera";
    }
    return camera.make + " " + camera.model;
}

}  // namespace

CameraCapabilities camera_capabilities(const raw::RawMetadata& metadata) {
    CameraCapabilities caps;
    caps.make =
        metadata.camera.normalized_make.empty() ? metadata.camera.make : metadata.camera.normalized_make;
    caps.model =
        metadata.camera.normalized_model.empty() ? metadata.camera.model : metadata.camera.normalized_model;
    caps.probed = true;
    auto model = engine::WhiteBalanceModel::create(metadata);
    if (model) {
        caps.kelvin_available = model->kelvin_available();
        caps.monochrome = model->monochrome();
        caps.kelvin_unavailable_reason = model->kelvin_unavailable_reason();
    } else {
        caps.kelvin_available = false;
        caps.kelvin_unavailable_reason = "no white-balance model: " + model.error().message;
    }
    return caps;
}

struct RecipeService::State {
    State(Library& l, std::shared_ptr<RecipeStore> s, RecipeServiceOptions o)
        : library(l), store(std::move(s)), options(std::move(o)), observers("app.recipe") {}

    struct Entry {
        catalog::RevisionHistory history;
        std::optional<catalog::RevisionId> revision;
        std::uint32_t revision_number = 0;
        engine::DevelopSettings committed;
        RecipeDocument committed_document;
        std::optional<Error> unreadable;  // the current revision's settings cannot be used
        std::optional<Error> history_error;  // the history cannot be listed (history() fails with it)
        std::optional<engine::DevelopSettings> transient;
        RecipeDocument transient_document;
        std::uint64_t transient_generation = 0;
        // The revision undo() last left: redo() returns to it while it is a child of the current one.
        std::optional<catalog::RevisionId> redo_target;
    };

    struct CameraEntry {
        core::FileIdentity identity;
        CameraCapabilities caps;
    };

    Library& library;
    std::shared_ptr<RecipeStore> store;
    RecipeServiceOptions options;
    detail::ObserverList<RecipeChange> observers;

    mutable std::mutex mutex;  // entries, sequence; held across store calls (serialises changes)
    mutable std::unordered_map<std::uint64_t, Entry> entries;
    std::uint64_t sequence = 0;

    mutable std::mutex camera_mutex;
    mutable std::unordered_map<std::uint64_t, CameraEntry> cameras;

    // ---- Loading ------------------------------------------------------------------------------

    // Re-reads the item's revisions from the store into `entry` (the transient edit is kept). The
    // history listing and the current revision are read separately: stored data this build cannot
    // use (is_stored_data_error) in the current revision makes the item read-only (`unreadable`),
    // in the listing only the listing fails; neither makes the item unusable. Store failures fail.
    Status reload_locked(ItemId item, Entry& entry) const {
        auto history = store->history(item);
        if (!history && !is_stored_data_error(history.error())) {
            return Unexpected(std::move(history.error()));
        }
        auto current = store->current(item);
        if (!current && !is_stored_data_error(current.error())) {
            return Unexpected(std::move(current.error()));
        }
        entry.history_error.reset();
        if (history) {
            entry.history = std::move(*history);
        } else {
            core::log_warn("app.recipe", "the revision history of item {} cannot be listed: {}", item.value,
                           history.error().describe());
            entry.history = catalog::RevisionHistory{};
            entry.history.item = catalog::ItemId{static_cast<std::int64_t>(item.value)};
            entry.history_error = history.error();
        }
        entry.revision = entry.history.current;
        entry.revision_number = 0;
        entry.unreadable.reset();
        entry.committed = engine::DevelopSettings{};
        if (!current) {
            // The current revision fails its integrity check or holds values this build cannot
            // decode: read-only with that reason; undo, redo, select and reset still work.
            core::log_warn("app.recipe", "the current recipe of item {} cannot be read: {}", item.value,
                           current.error().describe());
            entry.unreadable = unreadable_recipe(current.error());
            for (const catalog::RevisionInfo& info : entry.history.revisions) {
                if (entry.revision == info.id) {
                    entry.revision_number = info.revision_number;
                }
            }
        } else if (*current) {
            entry.revision = (*current)->info.id;
            entry.revision_number = (*current)->info.revision_number;
            auto settings = settings_of(**current);
            if (settings) {
                entry.committed = std::move(*settings);
            } else {
                entry.unreadable = unreadable_recipe(settings.error());
            }
        }
        if (!entry.unreadable) {
            auto document = write_recipe(entry.committed);
            if (!document) {
                entry.unreadable = unreadable_recipe(document.error());
            } else {
                entry.committed_document = std::move(*document);
            }
        } else {
            entry.committed_document = RecipeDocument{};
        }
        return ok();
    }

    Expected<Entry*> load_locked(ItemId item) const {
        if (!library.find(item)) {
            return fail(ErrorCode::NotFound, std::format("no library item {}", item.value),
                        std::string(kWhere));
        }
        if (const auto found = entries.find(item.value); found != entries.end()) {
            return &found->second;
        }
        try {
            Entry entry;
            if (auto status = reload_locked(item, entry); !status) {
                return Unexpected(std::move(status.error()));
            }
            return &entries.insert_or_assign(item.value, std::move(entry)).first->second;
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "cannot load the recipe", std::string(kWhere));
        }
    }

    RecipeState make_state(ItemId item, const Entry& entry) const {
        RecipeState state;
        state.item = item;
        state.committed = entry.committed;
        state.revision = entry.revision;
        state.revision_number = entry.revision_number;
        state.committed_sha256 = entry.committed_document.sha256;
        state.transient = entry.transient.has_value();
        state.transient_generation = entry.transient_generation;
        state.settings = entry.transient ? *entry.transient : entry.committed;
        state.settings_sha256 =
            entry.transient ? entry.transient_document.sha256 : entry.committed_document.sha256;
        state.can_undo = entry.revision.has_value();
        state.can_redo =
            std::any_of(entry.history.revisions.begin(), entry.history.revisions.end(),
                        [&](const catalog::RevisionInfo& info) { return info.parent == entry.revision; });
        if (auto reason = store->read_only_reason()) {
            state.read_only = std::move(reason);
        } else if (entry.unreadable) {
            state.read_only = entry.unreadable;
            state.recipe_unreadable = true;
        }
        if (entry.unreadable && state.read_only && !state.recipe_unreadable) {
            state.recipe_unreadable = true;  // both: the recipe itself is still unreadable
        }
        return state;
    }

    // Fails when the item cannot be edited at all.
    Status check_writable() const {
        if (auto reason = store->read_only_reason()) {
            return Unexpected(std::move(*reason));
        }
        return ok();
    }

    // Fails when the item's recipe cannot be edited (writable store and readable recipe).
    Status check_editable(const Entry& entry) const {
        if (auto status = check_writable(); !status) {
            return status;
        }
        if (entry.unreadable) {
            return Unexpected(*entry.unreadable);
        }
        return ok();
    }

    std::string engine_version() const {
        return options.engine_version.empty() ? std::string(core::build_info().version)
                                              : options.engine_version;
    }

    catalog::Timestamp now() const { return options.clock ? options.clock() : catalog::current_time(); }

    RecipeChange change_locked(RecipeChangeKind kind, ItemId item, const Entry& entry,
                               std::optional<catalog::RevisionOrigin> origin = std::nullopt) {
        RecipeChange change;
        change.kind = kind;
        change.sequence = ++sequence;
        change.state = make_state(item, entry);
        change.origin = origin;
        return change;
    }

    // Appends `settings` as the item's next revision unless they equal the committed ones (the
    // transient edit is dropped either way). `changes` receives the events to publish.
    Expected<bool> append_locked(ItemId item, Entry& entry, const engine::DevelopSettings& settings,
                                 catalog::RevisionOrigin origin, const std::string& note,
                                 std::vector<RecipeChange>& changes) {
        const bool had_transient = entry.transient.has_value();
        if (!entry.unreadable && settings == entry.committed) {
            if (had_transient) {
                entry.transient.reset();
                changes.push_back(change_locked(RecipeChangeKind::Discarded, item, entry));
            }
            return false;
        }
        auto document = write_recipe(settings);
        if (!document) {
            return Unexpected(std::move(document.error()));
        }
        catalog::NewRevision revision;
        revision.origin = origin;
        revision.schema_name = std::string(kRecipeSchema);
        revision.schema_version = kRecipeSchemaVersion;
        revision.settings_json = std::move(document->json);
        revision.engine_version = engine_version();
        revision.note = note;
        revision.created_at = now();
        auto appended = store->append(item, revision, entry.revision);
        if (!appended) {
            if (appended.error().code == ErrorCode::AlreadyExists) {
                // Another writer changed the item: show its state (the edit is not applied).
                entry.transient.reset();
                (void)reload_locked(item, entry);
            }
            return Unexpected(std::move(appended.error()));
        }
        entry.transient.reset();
        if (auto status = reload_locked(item, entry); !status) {
            return Unexpected(std::move(status.error()));
        }
        changes.push_back(change_locked(RecipeChangeKind::Committed, item, entry, origin));
        return true;
    }

    void publish(const std::vector<RecipeChange>& changes) const {
        for (const RecipeChange& change : changes) {
            observers.notify(change);
        }
    }

    // ---- Cameras ------------------------------------------------------------------------------

    Expected<CameraCapabilities> camera(ItemId id) const {
        std::optional<SourceItem> item = library.find(id);
        if (!item) {
            return fail(ErrorCode::NotFound, std::format("no library item {}", id.value),
                        std::string(kWhere));
        }
        {
            std::lock_guard lock(camera_mutex);
            if (const auto found = cameras.find(id.value);
                found != cameras.end() && found->second.identity == item->identity) {
                return found->second.caps;
            }
        }
        CameraCapabilities caps;
        if (item->probe) {
            caps.make = item->probe->camera_make;
            caps.model = item->probe->camera_model;
        }
        bool cacheable = false;
        if (options.registry) {
            raw::ProbeOptions probe;
            probe.subimage = item->subimage;
            probe.limits = options.probe_limits;
            probe.describe_subimages = false;
            auto selection = options.registry->select(item->path, probe, {});
            if (selection && selection->probe.metadata.source.identity == item->identity) {
                CameraCapabilities probed = camera_capabilities(selection->probe.metadata);
                if (probed.make.empty()) {
                    probed.make = caps.make;
                }
                if (probed.model.empty()) {
                    probed.model = caps.model;
                }
                caps = std::move(probed);
                cacheable = true;
            } else {
                caps.kelvin_unavailable_reason =
                    selection ? "the file changed since it was imported"
                              : "the file could not be probed: " + selection.error().message;
            }
        } else {
            caps.kelvin_unavailable_reason = "the camera's calibration is not known (no decoder registry)";
        }
        if (cacheable) {
            std::lock_guard lock(camera_mutex);
            try {
                cameras.insert_or_assign(id.value, CameraEntry{item->identity, caps});
            } catch (const std::bad_alloc&) {
                // A cache: the next call probes again.
            }
        }
        return caps;
    }

    void forget(const std::vector<ItemId>& items, bool recipes) {
        {
            std::lock_guard lock(camera_mutex);
            for (const ItemId id : items) {
                cameras.erase(id.value);
            }
        }
        if (recipes) {
            std::lock_guard lock(mutex);
            for (const ItemId id : items) {
                entries.erase(id.value);
            }
        }
    }
};

RecipeService::RecipeService(Library& library, std::shared_ptr<RecipeStore> store,
                             RecipeServiceOptions options)
    : state_(std::make_shared<State>(library, std::move(store), std::move(options))) {
    library_subscription_ =
        library.subscribe([weak = std::weak_ptr<State>(state_)](const LibraryChange& change) {
            if (change.kind != LibraryChangeKind::Removed && change.kind != LibraryChangeKind::Refreshed) {
                return;
            }
            if (auto state = weak.lock()) {
                // A removed item's revisions went with it (catalog cascade); a refreshed file may be
                // another camera's.
                state->forget(change.items, change.kind == LibraryChangeKind::Removed);
            }
        });
}

RecipeService::~RecipeService() {
    library_subscription_.reset();
}

std::optional<Error> RecipeService::read_only_reason() const {
    return state_->store->read_only_reason();
}

Expected<RecipeState> RecipeService::state(ItemId item) const {
    try {
        std::lock_guard lock(state_->mutex);
        auto entry = state_->load_locked(item);
        if (!entry) {
            return Unexpected(std::move(entry.error()));
        }
        return state_->make_state(item, **entry);
    } catch (const std::bad_alloc&) {
        return out_of_memory();
    }
}

Expected<RecipeService::Current> RecipeService::current_settings(ItemId item) const {
    try {
        std::lock_guard lock(state_->mutex);
        auto entry = state_->load_locked(item);
        if (!entry) {
            return Unexpected(std::move(entry.error()));
        }
        const State::Entry& e = **entry;
        if (e.unreadable) {
            return Unexpected(*e.unreadable);
        }
        return Current{e.committed, e.revision, e.revision_number, e.committed_document};
    } catch (const std::bad_alloc&) {
        return out_of_memory();
    }
}

Expected<engine::DevelopSettings> RecipeService::revision_settings(ItemId item,
                                                                   catalog::RevisionId revision) const {
    try {
        auto found = state_->store->find(revision);
        if (!found) {
            return Unexpected(std::move(found.error()));
        }
        if (!*found || (*found)->info.item.value != static_cast<std::int64_t>(item.value)) {
            return fail(ErrorCode::NotFound,
                        std::format("revision {} is not a revision of item {}", revision.value, item.value),
                        std::string(kWhere));
        }
        return settings_of(**found);
    } catch (const std::bad_alloc&) {
        return out_of_memory();
    }
}

Expected<RecipeState> RecipeService::set_transient(ItemId item, const engine::DevelopSettings& settings) {
    try {
        if (auto valid = settings.validate(); !valid) {
            return Unexpected(std::move(valid.error()));
        }
        if (engine::mode_of(settings.white_balance) == engine::WhiteBalanceMode::Kelvin) {
            auto caps = state_->camera(item);
            if (!caps) {
                return Unexpected(std::move(caps.error()));
            }
            if (caps->probed && !caps->kelvin_available) {
                return fail(ErrorCode::Unsupported,
                            "Kelvin white balance is not available for this camera: " +
                                caps->kelvin_unavailable_reason,
                            "app.recipe.kelvin");
            }
        }
        auto document = write_recipe(settings);
        if (!document) {
            return Unexpected(std::move(document.error()));
        }
        std::vector<RecipeChange> changes;
        RecipeState result;
        {
            std::lock_guard lock(state_->mutex);
            auto entry = state_->load_locked(item);
            if (!entry) {
                return Unexpected(std::move(entry.error()));
            }
            State::Entry& e = **entry;
            if (auto status = state_->check_editable(e); !status) {
                return Unexpected(std::move(status.error()));
            }
            e.transient = settings;
            e.transient_document = std::move(*document);
            ++e.transient_generation;
            changes.push_back(state_->change_locked(RecipeChangeKind::Transient, item, e));
            result = changes.back().state;
        }
        state_->publish(changes);
        return result;
    } catch (const std::bad_alloc&) {
        return out_of_memory();
    }
}

Expected<RecipeState> RecipeService::commit(ItemId item) {
    try {
        std::vector<RecipeChange> changes;
        RecipeState result;
        {
            std::lock_guard lock(state_->mutex);
            auto entry = state_->load_locked(item);
            if (!entry) {
                return Unexpected(std::move(entry.error()));
            }
            State::Entry& e = **entry;
            if (e.transient) {
                if (auto status = state_->check_editable(e); !status) {
                    return Unexpected(std::move(status.error()));
                }
                const engine::DevelopSettings settings = *e.transient;
                auto appended = state_->append_locked(item, e, settings, catalog::RevisionOrigin::User,
                                                      make_note(e.committed, settings), changes);
                if (!appended) {
                    return Unexpected(std::move(appended.error()));
                }
            }
            result = state_->make_state(item, e);
        }
        state_->publish(changes);
        return result;
    } catch (const std::bad_alloc&) {
        return out_of_memory();
    }
}

Expected<RecipeState> RecipeService::discard_transient(ItemId item) {
    try {
        std::vector<RecipeChange> changes;
        RecipeState result;
        {
            std::lock_guard lock(state_->mutex);
            auto entry = state_->load_locked(item);
            if (!entry) {
                return Unexpected(std::move(entry.error()));
            }
            State::Entry& e = **entry;
            if (e.transient) {
                e.transient.reset();
                changes.push_back(state_->change_locked(RecipeChangeKind::Discarded, item, e));
            }
            result = state_->make_state(item, e);
        }
        state_->publish(changes);
        return result;
    } catch (const std::bad_alloc&) {
        return out_of_memory();
    }
}

Expected<RecipeState> RecipeService::apply(ItemId item, const engine::DevelopSettings& settings) {
    try {
        auto transient = set_transient(item, settings);
        if (!transient) {
            return transient;
        }
        return commit(item);
    } catch (const std::bad_alloc&) {
        return out_of_memory();
    }
}

Expected<AutomaticOutcome> RecipeService::apply_automatic(ItemId item, std::optional<catalog::RevisionId> base,
                                                          std::span<const AutomaticRevision> revisions) {
    return write_automatic(item, base, [&](bool make_current) {
        return state_->store->append_automatic(item, base, revisions, make_current);
    });
}

Expected<AutomaticOutcome> RecipeService::write_automatic(ItemId item, std::optional<catalog::RevisionId> base,
                                                          const AutomaticWrite& write) {
    try {
        std::vector<RecipeChange> changes;
        AutomaticOutcome outcome;
        {
            std::lock_guard lock(state_->mutex);
            auto entry = state_->load_locked(item);
            if (!entry) {
                return Unexpected(std::move(entry.error()));
            }
            State::Entry& e = **entry;
            if (auto status = state_->check_writable(); !status) {
                return Unexpected(std::move(status.error()));
            }
            // The item's committed state may have been changed by another writer since it was loaded.
            if (auto status = state_->reload_locked(item, e); !status) {
                return Unexpected(std::move(status.error()));
            }
            const bool make_current = e.revision == base && !e.transient;
            auto appended = write(make_current);
            if (!appended) {
                (void)state_->reload_locked(item, e);
                return Unexpected(std::move(appended.error()));
            }
            if (auto status = state_->reload_locked(item, e); !status) {
                return Unexpected(std::move(status.error()));
            }
            changes.push_back(state_->change_locked(RecipeChangeKind::Committed, item, e, catalog::RevisionOrigin::Auto));
            outcome.revisions = std::move(appended->revisions);
            outcome.made_current = appended->made_current;
            outcome.state = changes.back().state;
        }
        state_->publish(changes);
        return outcome;
    } catch (const std::bad_alloc&) {
        return out_of_memory();
    }
}

Expected<RecipeState> RecipeService::refresh(ItemId item) {
    try {
        std::vector<RecipeChange> changes;
        RecipeState result;
        {
            std::lock_guard lock(state_->mutex);
            auto entry = state_->load_locked(item);
            if (!entry) {
                return Unexpected(std::move(entry.error()));
            }
            State::Entry& e = **entry;
            if (auto status = state_->reload_locked(item, e); !status) {
                return Unexpected(std::move(status.error()));
            }
            changes.push_back(state_->change_locked(RecipeChangeKind::Committed, item, e, catalog::RevisionOrigin::Auto));
            result = changes.back().state;
        }
        state_->publish(changes);
        return result;
    } catch (const std::bad_alloc&) {
        return out_of_memory();
    }
}

Expected<std::vector<catalog::AutoAnalysisRecord>> RecipeService::automatic_analyses(ItemId item) const {
    if (!state_->library.find(item)) {
        return fail(ErrorCode::NotFound, std::format("no library item {}", item.value), std::string(kWhere));
    }
    return state_->store->auto_analyses(item);
}

std::string RecipeService::engine_version() const { return state_->engine_version(); }

catalog::Timestamp RecipeService::now() const { return state_->now(); }

Expected<RecipeState> RecipeService::undo(ItemId item) {
    try {
        std::vector<RecipeChange> changes;
        RecipeState result;
        {
            std::lock_guard lock(state_->mutex);
            auto entry = state_->load_locked(item);
            if (!entry) {
                return Unexpected(std::move(entry.error()));
            }
            State::Entry& e = **entry;
            if (auto status = state_->check_writable(); !status) {
                return Unexpected(std::move(status.error()));
            }
            e.transient.reset();
            if (!e.revision) {
                return fail(ErrorCode::InvalidArgument,
                            std::format("item {} shows its as-shot settings: nothing to undo", item.value),
                            "app.recipe.nothing_to_undo");
            }
            const std::optional<catalog::RevisionId> left = e.revision;
            auto moved = state_->store->undo(item);
            if (!moved) {
                (void)state_->reload_locked(item, e);
                return Unexpected(std::move(moved.error()));
            }
            if (auto status = state_->reload_locked(item, e); !status) {
                return Unexpected(std::move(status.error()));
            }
            e.redo_target = left;
            changes.push_back(state_->change_locked(RecipeChangeKind::Moved, item, e));
            result = changes.back().state;
        }
        state_->publish(changes);
        return result;
    } catch (const std::bad_alloc&) {
        return out_of_memory();
    }
}

Expected<RecipeState> RecipeService::redo(ItemId item) {
    try {
        std::vector<RecipeChange> changes;
        RecipeState result;
        {
            std::lock_guard lock(state_->mutex);
            auto entry = state_->load_locked(item);
            if (!entry) {
                return Unexpected(std::move(entry.error()));
            }
            State::Entry& e = **entry;
            if (auto status = state_->check_writable(); !status) {
                return Unexpected(std::move(status.error()));
            }
            e.transient.reset();
            if (!state_->make_state(item, e).can_redo) {
                return fail(
                    ErrorCode::InvalidArgument,
                    std::format("item {} has no revision derived from its current one: nothing to redo",
                                item.value),
                    "app.recipe.nothing_to_redo");
            }
            // The child undo() left, else the newest child that is not an automatic branch, else the
            // newest child (the history lists revisions oldest first).
            std::optional<catalog::RevisionId> target;
            std::optional<catalog::RevisionId> newest;
            bool remembered = false;
            for (const catalog::RevisionInfo& info : e.history.revisions) {
                if (info.parent != e.revision) {
                    continue;
                }
                newest = info.id;
                if (e.redo_target == info.id) {
                    remembered = true;
                }
                if (!is_automatic_branch_note(info.note)) {
                    target = info.id;
                }
            }
            if (remembered) {
                target = e.redo_target;
            } else if (!target) {
                target = newest;
            }
            if (!target) {  // can_redo above: the item has a child of its current revision
                return fail(ErrorCode::InvalidArgument,
                            std::format("item {} has no revision derived from its current one: nothing to redo",
                                        item.value),
                            "app.recipe.nothing_to_redo");
            }
            Status moved = state_->store->set_current(item, target);
            if (!moved) {
                (void)state_->reload_locked(item, e);
                return Unexpected(std::move(moved.error()));
            }
            if (auto status = state_->reload_locked(item, e); !status) {
                return Unexpected(std::move(status.error()));
            }
            changes.push_back(state_->change_locked(RecipeChangeKind::Moved, item, e));
            result = changes.back().state;
        }
        state_->publish(changes);
        return result;
    } catch (const std::bad_alloc&) {
        return out_of_memory();
    }
}

Expected<RecipeState> RecipeService::select_revision(ItemId item,
                                                     std::optional<catalog::RevisionId> revision) {
    try {
        std::vector<RecipeChange> changes;
        RecipeState result;
        {
            std::lock_guard lock(state_->mutex);
            auto entry = state_->load_locked(item);
            if (!entry) {
                return Unexpected(std::move(entry.error()));
            }
            State::Entry& e = **entry;
            if (auto status = state_->check_writable(); !status) {
                return Unexpected(std::move(status.error()));
            }
            const bool had_transient = e.transient.has_value();
            e.transient.reset();
            if (revision != e.revision) {
                if (auto status = state_->store->set_current(item, revision); !status) {
                    return Unexpected(std::move(status.error()));
                }
                if (auto status = state_->reload_locked(item, e); !status) {
                    return Unexpected(std::move(status.error()));
                }
                changes.push_back(state_->change_locked(RecipeChangeKind::Moved, item, e));
            } else if (had_transient) {
                changes.push_back(state_->change_locked(RecipeChangeKind::Discarded, item, e));
            }
            result = state_->make_state(item, e);
        }
        state_->publish(changes);
        return result;
    } catch (const std::bad_alloc&) {
        return out_of_memory();
    }
}

Expected<RecipeState> RecipeService::reset_to_as_shot(ItemId item) {
    try {
        std::vector<RecipeChange> changes;
        RecipeState result;
        {
            std::lock_guard lock(state_->mutex);
            auto entry = state_->load_locked(item);
            if (!entry) {
                return Unexpected(std::move(entry.error()));
            }
            State::Entry& e = **entry;
            if (auto status = state_->check_writable(); !status) {
                return Unexpected(std::move(status.error()));
            }
            engine::DevelopSettings target;
            if (!e.unreadable) {
                target.locks = e.committed.locks;
            }
            const engine::DevelopSettings from = e.unreadable ? engine::DevelopSettings{} : e.committed;
            auto appended = state_->append_locked(item, e, target, catalog::RevisionOrigin::Reset,
                                                  make_note(from, target), changes);
            if (!appended) {
                return Unexpected(std::move(appended.error()));
            }
            result = state_->make_state(item, e);
        }
        state_->publish(changes);
        return result;
    } catch (const std::bad_alloc&) {
        return out_of_memory();
    }
}

Expected<RecipeState> RecipeService::revert_to(ItemId item, catalog::RevisionId revision) {
    try {
        auto found = state_->store->find(revision);
        if (!found) {
            return Unexpected(std::move(found.error()));
        }
        if (!*found || (*found)->info.item.value != static_cast<std::int64_t>(item.value)) {
            return fail(ErrorCode::NotFound,
                        std::format("revision {} is not a revision of item {}", revision.value, item.value),
                        std::string(kWhere));
        }
        auto target = settings_of(**found);
        if (!target) {
            return Unexpected(std::move(target.error()));
        }
        std::vector<RecipeChange> changes;
        RecipeState result;
        {
            std::lock_guard lock(state_->mutex);
            auto entry = state_->load_locked(item);
            if (!entry) {
                return Unexpected(std::move(entry.error()));
            }
            State::Entry& e = **entry;
            if (auto status = state_->check_writable(); !status) {
                return Unexpected(std::move(status.error()));
            }
            const engine::DevelopSettings from = e.unreadable ? engine::DevelopSettings{} : e.committed;
            auto appended =
                state_->append_locked(item, e, *target, catalog::RevisionOrigin::User,
                                      make_note(from, *target, (*found)->info.revision_number), changes);
            if (!appended) {
                return Unexpected(std::move(appended.error()));
            }
            result = state_->make_state(item, e);
        }
        state_->publish(changes);
        return result;
    } catch (const std::bad_alloc&) {
        return out_of_memory();
    }
}

Expected<RecipeState> RecipeService::set_lock(ItemId item, engine::DevelopControl control, bool locked) {
    try {
        if (engine::to_string(control) == "invalid") {
            return fail(ErrorCode::InvalidArgument, "unknown control", std::string(kWhere));
        }
        std::vector<RecipeChange> changes;
        RecipeState result;
        {
            std::lock_guard lock(state_->mutex);
            auto entry = state_->load_locked(item);
            if (!entry) {
                return Unexpected(std::move(entry.error()));
            }
            State::Entry& e = **entry;
            if (auto status = state_->check_editable(e); !status) {
                return Unexpected(std::move(status.error()));
            }
            engine::DevelopSettings target = e.committed;
            if (locked) {
                target.locks.insert(control);
            } else {
                target.locks.erase(control);
            }
            auto appended = state_->append_locked(item, e, target, catalog::RevisionOrigin::User,
                                                  make_note(e.committed, target), changes);
            if (!appended) {
                return Unexpected(std::move(appended.error()));
            }
            result = state_->make_state(item, e);
        }
        state_->publish(changes);
        return result;
    } catch (const std::bad_alloc&) {
        return out_of_memory();
    }
}

Expected<RecipeHistory> RecipeService::history(ItemId item) const {
    try {
        std::lock_guard lock(state_->mutex);
        auto entry = state_->load_locked(item);
        if (!entry) {
            return Unexpected(std::move(entry.error()));
        }
        const State::Entry& e = **entry;
        if (e.history_error) {
            return Unexpected(*e.history_error);
        }
        try {
            RecipeHistory out;
            out.item = item;
            out.current = e.revision;
            std::map<std::int64_t, const catalog::RevisionInfo*> by_id;
            for (const catalog::RevisionInfo& info : e.history.revisions) {
                by_id.emplace(info.id.value, &info);
            }
            std::vector<std::int64_t> path;
            for (std::optional<catalog::RevisionId> at = e.revision; at;) {
                path.push_back(at->value);
                const auto found = by_id.find(at->value);
                if (found == by_id.end() || path.size() > by_id.size()) {
                    break;
                }
                at = found->second->parent;
            }
            for (const catalog::RevisionInfo& info : e.history.revisions) {
                HistoryEntry h;
                h.info = info;
                h.current = e.revision == info.id;
                h.on_current_path = std::find(path.begin(), path.end(), info.id.value) != path.end();
                parse_note(info.note, h);
                out.entries.push_back(std::move(h));
            }
            return out;
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "cannot list the history", std::string(kWhere));
        }
    } catch (const std::bad_alloc&) {
        return out_of_memory();
    }
}

Expected<RecipeClipboard> RecipeService::copy(ItemId item, const std::set<RecipeCategory>& categories) {
    try {
        if (categories.empty()) {
            return fail(ErrorCode::InvalidArgument, "copy needs at least one category", std::string(kWhere));
        }
        for (const RecipeCategory category : categories) {
            if (to_string(category) == "invalid") {
                return fail(ErrorCode::InvalidArgument, "unknown recipe category", std::string(kWhere));
            }
        }
        auto current = current_settings(item);
        if (!current) {
            return Unexpected(std::move(current.error()));
        }
        auto caps = state_->camera(item);
        if (!caps) {
            return Unexpected(std::move(caps.error()));
        }
        return RecipeClipboard{item, std::move(current->settings), categories, std::move(*caps)};
    } catch (const std::bad_alloc&) {
        return out_of_memory();
    }
}

Expected<std::vector<PasteOutcome>> RecipeService::paste(const RecipeClipboard& clipboard,
                                                         std::span<const ItemId> targets) {
    try {
        if (clipboard.categories.empty()) {
            return fail(ErrorCode::InvalidArgument, "the clipboard has no category",
                        std::string(kWherePaste));
        }
        for (const RecipeCategory category : clipboard.categories) {
            if (to_string(category) == "invalid") {
                return fail(ErrorCode::InvalidArgument, "unknown recipe category", std::string(kWherePaste));
            }
        }
        if (auto valid = clipboard.settings.validate(); !valid) {
            return Unexpected(std::move(valid.error()));
        }
        std::vector<PasteOutcome> outcomes;
        try {
            outcomes.reserve(targets.size());
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "cannot paste", std::string(kWherePaste));
        }
        const engine::WhiteBalanceMode mode = engine::mode_of(clipboard.settings.white_balance);
        for (const ItemId target : targets) {
            PasteOutcome outcome;
            outcome.item = target;
            auto caps = state_->camera(target);
            if (!caps) {
                outcome.error = caps.error();
                outcomes.push_back(std::move(outcome));
                continue;
            }
            for (const RecipeCategory category : clipboard.categories) {
                if (category == RecipeCategory::WhiteBalance) {
                    if (mode == engine::WhiteBalanceMode::CameraNeutral &&
                        !clipboard.camera.same_camera(*caps)) {
                        outcome.skipped.push_back(PasteSkip{
                            category,
                            std::format("camera-neutral white balance belongs to {}; the photo is from {}",
                                        camera_name(clipboard.camera), camera_name(*caps))});
                        continue;
                    }
                    if (mode == engine::WhiteBalanceMode::Kelvin &&
                        !(caps->probed && caps->kelvin_available)) {
                        outcome.skipped.push_back(PasteSkip{
                            category,
                            "Kelvin white balance is not available for this photo: " +
                                (caps->kelvin_unavailable_reason.empty() ? std::string("unknown calibration")
                                                                         : caps->kelvin_unavailable_reason)});
                        continue;
                    }
                }
                outcome.applied.push_back(category);
            }
            std::vector<RecipeChange> changes;
            {
                std::lock_guard lock(state_->mutex);
                auto entry = state_->load_locked(target);
                if (!entry) {
                    outcome.error = entry.error();
                } else if (auto status = state_->check_editable(**entry); !status) {
                    outcome.error = status.error();
                } else {
                    State::Entry& e = **entry;
                    engine::DevelopSettings settings = e.committed;
                    for (const RecipeCategory category : outcome.applied) {
                        for (const engine::DevelopControl control : controls_of(category)) {
                            copy_value(clipboard.settings, settings, control);
                            if (clipboard.settings.locks.contains(control)) {
                                settings.locks.insert(control);
                            } else {
                                settings.locks.erase(control);
                            }
                        }
                    }
                    auto appended = state_->append_locked(target, e, settings, catalog::RevisionOrigin::Paste,
                                                          make_note(e.committed, settings), changes);
                    if (!appended) {
                        outcome.error = appended.error();
                    } else if (*appended) {
                        outcome.revision = e.revision;
                    }
                }
            }
            state_->publish(changes);
            outcomes.push_back(std::move(outcome));
        }
        return outcomes;
    } catch (const std::bad_alloc&) {
        return out_of_memory();
    }
}

Expected<CameraCapabilities> RecipeService::camera(ItemId item) const {
    try {
        return state_->camera(item);
    } catch (const std::bad_alloc&) {
        return out_of_memory();
    }
}

void RecipeService::note_camera(ItemId item, const raw::RawMetadata& metadata) {
    try {
        std::optional<SourceItem> found = state_->library.find(item);
        if (!found || metadata.source.identity != found->identity) {
            return;
        }
        CameraCapabilities caps = camera_capabilities(metadata);
        if (found->probe) {
            if (caps.make.empty()) {
                caps.make = found->probe->camera_make;
            }
            if (caps.model.empty()) {
                caps.model = found->probe->camera_model;
            }
        }
        std::lock_guard lock(state_->camera_mutex);
        state_->cameras.insert_or_assign(item.value, State::CameraEntry{found->identity, std::move(caps)});
    } catch (const std::bad_alloc&) {
        // A cache: the next camera() call probes the file instead.
    }
}

Subscription RecipeService::subscribe(RecipeObserver observer) {
    return state_->observers.add(std::move(observer));
}

}  // namespace lucent::app
