// Recipe stores: the catalog's revisions, or an in-memory equivalent (recipe_store.hpp).

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <lucent/application/recipe_store.hpp>
#include <lucent/core/hash.hpp>

namespace lucent::app {

namespace {

constexpr std::string_view kWhere = "app.recipe.store";
constexpr std::string_view kWhereConflict = "app.recipe.conflict";

Expected<catalog::ItemId> catalog_item(ItemId item) {
    if (item.value == 0 ||
        item.value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return fail(ErrorCode::NotFound, std::format("no catalog item {}", item.value), std::string(kWhere));
    }
    return catalog::ItemId{static_cast<std::int64_t>(item.value)};
}

std::string describe(std::optional<catalog::RevisionId> revision) {
    return revision ? std::format("revision {}", revision->value) : std::string("as shot");
}

Status check_automatic(std::span<const AutomaticRevision> revisions) {
    if (revisions.empty()) {
        return fail(ErrorCode::InvalidArgument, "an automatic run appends at least one revision", std::string(kWhere));
    }
    for (const AutomaticRevision& entry : revisions) {
        if (entry.revision.origin != catalog::RevisionOrigin::Auto) {
            return fail(ErrorCode::InvalidArgument, "automatic revisions have origin Auto", std::string(kWhere));
        }
    }
    return ok();
}

// An automatic revision written as a branch: its note says so (kAutomaticBranchNote).
catalog::NewRevision as_branch(const catalog::NewRevision& revision) {
    catalog::NewRevision out = revision;
    if (!is_automatic_branch_note(out.note)) {
        out.note += out.note.empty() ? std::string(kAutomaticBranchNote) : ";" + std::string(kAutomaticBranchNote);
    }
    return out;
}

Error conflict(ItemId item, std::optional<catalog::RevisionId> found,
               std::optional<catalog::RevisionId> expected) {
    return Error{ErrorCode::AlreadyExists,
                 std::format("the recipe of item {} changed meanwhile: it shows {}, the edit was made on {}",
                             item.value, describe(found), describe(expected)),
                 std::string(kWhereConflict), 0};
}

// ---- Catalog ------------------------------------------------------------------------------------

class CatalogRecipeStore final : public RecipeStore {
public:
    explicit CatalogRecipeStore(std::shared_ptr<catalog::Catalog> catalog) : catalog_(std::move(catalog)) {}

    std::optional<Error> read_only_reason() const override {
        if (!catalog_->is_read_only()) {
            return std::nullopt;
        }
        if (catalog_->schema().read_only_reason) {
            return catalog_->schema().read_only_reason;
        }
        return Error{ErrorCode::PermissionDenied, "the catalog is open read-only", std::string(kWhere), 0};
    }

    Expected<catalog::RevisionHistory> history(ItemId item) const override {
        auto id = catalog_item(item);
        if (!id) {
            return Unexpected(std::move(id.error()));
        }
        return catalog_->query([&](catalog::ReadTransaction& tx) { return tx.revision_history(*id); });
    }

    Expected<std::optional<catalog::RecipeRevision>> current(ItemId item) const override {
        auto id = catalog_item(item);
        if (!id) {
            return Unexpected(std::move(id.error()));
        }
        return catalog_->query([&](catalog::ReadTransaction& tx) { return tx.current_revision(*id); });
    }

    Expected<std::optional<catalog::RecipeRevision>> find(catalog::RevisionId revision) const override {
        return catalog_->query([&](catalog::ReadTransaction& tx) { return tx.find_revision(revision); });
    }

    Expected<catalog::RevisionInfo> append(ItemId item, const catalog::NewRevision& revision,
                                           std::optional<catalog::RevisionId> expected_current) override {
        auto id = catalog_item(item);
        if (!id) {
            return Unexpected(std::move(id.error()));
        }
        return catalog_->transact([&](catalog::WriteTransaction& tx) -> Expected<catalog::RevisionInfo> {
            auto record = tx.find_item(*id);
            if (!record) {
                return Unexpected(std::move(record.error()));
            }
            if (!*record) {
                return fail(ErrorCode::NotFound, std::format("no catalog item {}", item.value),
                            std::string(kWhere));
            }
            if ((*record)->current_revision != expected_current) {
                return Unexpected(conflict(item, (*record)->current_revision, expected_current));
            }
            return tx.append_revision(*id, revision);
        });
    }

    Status set_current(ItemId item, std::optional<catalog::RevisionId> revision) override {
        auto id = catalog_item(item);
        if (!id) {
            return Unexpected(std::move(id.error()));
        }
        return catalog_->write(
            [&](catalog::WriteTransaction& tx) { return tx.set_current_revision(*id, revision); });
    }

    Expected<std::optional<catalog::RevisionId>> undo(ItemId item) override {
        auto id = catalog_item(item);
        if (!id) {
            return Unexpected(std::move(id.error()));
        }
        return catalog_->transact([&](catalog::WriteTransaction& tx) { return tx.undo_revision(*id); });
    }

    Expected<catalog::RevisionId> redo(ItemId item) override {
        auto id = catalog_item(item);
        if (!id) {
            return Unexpected(std::move(id.error()));
        }
        return catalog_->transact([&](catalog::WriteTransaction& tx) { return tx.redo_revision(*id); });
    }

    Expected<AutomaticAppend> append_automatic(ItemId item, std::optional<catalog::RevisionId> parent,
                                               std::span<const AutomaticRevision> revisions,
                                               bool make_current) override {
        auto id = catalog_item(item);
        if (!id) {
            return Unexpected(std::move(id.error()));
        }
        return catalog_->transact([&](catalog::WriteTransaction& tx) {
            return append_automatic_revisions(tx, *id, parent, revisions, make_current);
        });
    }

    Expected<std::vector<catalog::AutoAnalysisRecord>> auto_analyses(ItemId item) const override {
        auto id = catalog_item(item);
        if (!id) {
            return Unexpected(std::move(id.error()));
        }
        return catalog_->query([&](catalog::ReadTransaction& tx) { return tx.list_auto_analyses(*id); });
    }

private:
    std::shared_ptr<catalog::Catalog> catalog_;
};

// ---- Memory -------------------------------------------------------------------------------------

class MemoryRecipeStore final : public RecipeStore {
public:
    std::optional<Error> read_only_reason() const override { return std::nullopt; }

    Expected<catalog::RevisionHistory> history(ItemId item) const override {
        std::lock_guard lock(mutex_);
        try {
            catalog::RevisionHistory history;
            history.item = catalog::ItemId{static_cast<std::int64_t>(item.value)};
            if (const auto found = items_.find(item.value); found != items_.end()) {
                history.current = found->second.current;
                for (const catalog::RevisionId id : found->second.revisions) {
                    history.revisions.push_back(revisions_.at(id.value).info);
                }
            }
            return history;
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "cannot list the revisions", std::string(kWhere));
        }
    }

    Expected<std::optional<catalog::RecipeRevision>> current(ItemId item) const override {
        std::lock_guard lock(mutex_);
        const auto found = items_.find(item.value);
        if (found == items_.end() || !found->second.current) {
            return std::optional<catalog::RecipeRevision>{};
        }
        return std::optional<catalog::RecipeRevision>(revisions_.at(found->second.current->value));
    }

    Expected<std::optional<catalog::RecipeRevision>> find(catalog::RevisionId id) const override {
        std::lock_guard lock(mutex_);
        const auto found = revisions_.find(id.value);
        if (found == revisions_.end()) {
            return std::optional<catalog::RecipeRevision>{};
        }
        return std::optional<catalog::RecipeRevision>(found->second);
    }

    Expected<catalog::RevisionInfo> append(ItemId item, const catalog::NewRevision& revision,
                                           std::optional<catalog::RevisionId> expected_current) override {
        auto digest = check_revision(revision);
        if (!digest) {
            return Unexpected(std::move(digest.error()));
        }
        std::lock_guard lock(mutex_);
        try {
            Item& entry = items_[item.value];
            if (entry.current != expected_current) {
                return Unexpected(conflict(item, entry.current, expected_current));
            }
            return store_locked(item, entry, revision, *digest, entry.current);
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "cannot store the revision", std::string(kWhere));
        }
    }

    Expected<AutomaticAppend> append_automatic(ItemId item, std::optional<catalog::RevisionId> parent,
                                               std::span<const AutomaticRevision> revisions,
                                               bool make_current) override {
        if (Status valid = check_automatic(revisions); !valid) {
            return Unexpected(std::move(valid.error()));
        }
        std::vector<core::Digest256> digests;
        for (const AutomaticRevision& entry : revisions) {
            auto digest = check_revision(entry.revision);
            if (!digest) {
                return Unexpected(std::move(digest.error()));
            }
            if (entry.analysis) {
                catalog::NewAutoAnalysis probe = *entry.analysis;
                probe.revision = catalog::RevisionId{1};
                if (Status valid = probe.validate(); !valid) {
                    return Unexpected(std::move(valid.error()));
                }
            }
            digests.push_back(*digest);
        }
        std::lock_guard lock(mutex_);
        try {
            if (parent) {
                const auto found = revisions_.find(parent->value);
                if (found == revisions_.end() ||
                    found->second.info.item.value != static_cast<std::int64_t>(item.value)) {
                    return fail(ErrorCode::InvalidArgument,
                                std::format("recipe revision {} is not one of item {}'s", parent->value, item.value),
                                std::string(kWhere));
                }
            }
            Item& entry = items_[item.value];
            AutomaticAppend result;
            const std::optional<catalog::RevisionId> found_current = entry.current;
            result.made_current = make_current && found_current == parent;
            for (std::size_t i = 0; i < revisions.size(); ++i) {
                auto stored = result.made_current
                                  ? store_locked(item, entry, revisions[i].revision, digests[i], parent)
                                  : store_locked(item, entry, as_branch(revisions[i].revision), digests[i], parent);
                if (!stored) {
                    return Unexpected(std::move(stored.error()));
                }
                if (revisions[i].analysis) {
                    catalog::AutoAnalysisRecord record;
                    const catalog::NewAutoAnalysis& a = *revisions[i].analysis;
                    record.id = catalog::AutoAnalysisId{next_analysis_id_++};
                    record.revision = stored->id;
                    record.item = stored->item;
                    record.job = a.job;
                    record.policy = a.policy;
                    record.policy_version = a.policy_version;
                    record.search_version = a.search_version;
                    record.metric_contract_version = a.metric_contract_version;
                    record.consistency_version = a.consistency_version;
                    record.develop_pipeline_version = a.develop_pipeline_version;
                    record.develop_settings_version = a.develop_settings_version;
                    record.attempt = a.attempt;
                    record.final_check = a.final_check;
                    record.confidence = a.confidence;
                    record.review = a.review;
                    record.flags_json = a.flags_json;
                    record.record_json = a.record_json;
                    record.created_at = a.created_at;
                    analyses_.push_back(std::move(record));
                }
                result.revisions.push_back(*stored);
            }
            entry.current = result.made_current ? std::optional(result.revisions.back().id) : found_current;
            return result;
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "cannot store the revisions", std::string(kWhere));
        }
    }

    Expected<std::vector<catalog::AutoAnalysisRecord>> auto_analyses(ItemId item) const override {
        std::lock_guard lock(mutex_);
        std::vector<catalog::AutoAnalysisRecord> out;
        for (const catalog::AutoAnalysisRecord& record : analyses_) {
            if (record.item.value == static_cast<std::int64_t>(item.value)) {
                out.push_back(record);
            }
        }
        return out;
    }

    Status set_current(ItemId item, std::optional<catalog::RevisionId> revision) override {
        std::lock_guard lock(mutex_);
        if (revision) {
            const auto found = revisions_.find(revision->value);
            if (found == revisions_.end()) {
                return fail(ErrorCode::NotFound,
                            std::format("no recipe revision with id {}", revision->value),
                            std::string(kWhere));
            }
            if (found->second.info.item.value != static_cast<std::int64_t>(item.value)) {
                return fail(ErrorCode::InvalidArgument,
                            std::format("recipe revision {} belongs to another item", revision->value),
                            std::string(kWhere));
            }
        }
        try {
            items_[item.value].current = revision;
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "cannot move the current revision", std::string(kWhere));
        }
        return ok();
    }

    Expected<std::optional<catalog::RevisionId>> undo(ItemId item) override {
        std::lock_guard lock(mutex_);
        const auto found = items_.find(item.value);
        if (found == items_.end() || !found->second.current) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("item {} shows its as-shot settings: nothing to undo", item.value),
                        "app.recipe.store.nothing_to_undo");
        }
        found->second.current = revisions_.at(found->second.current->value).info.parent;
        return found->second.current;
    }

    Expected<catalog::RevisionId> redo(ItemId item) override {
        std::lock_guard lock(mutex_);
        const auto found = items_.find(item.value);
        if (found != items_.end()) {
            // The most recently created child of the current revision (or of "as shot").
            for (auto it = found->second.revisions.rbegin(); it != found->second.revisions.rend(); ++it) {
                if (revisions_.at(it->value).info.parent == found->second.current) {
                    found->second.current = *it;
                    return *it;
                }
            }
        }
        return fail(
            ErrorCode::InvalidArgument,
            std::format("item {} has no revision derived from its current one: nothing to redo", item.value),
            "app.recipe.store.nothing_to_redo");
    }

private:
    struct Item {
        std::optional<catalog::RevisionId> current;
        std::vector<catalog::RevisionId> revisions;  // by revision number
    };

    static Expected<core::Digest256> check_revision(const catalog::NewRevision& revision) {
        if (auto valid = revision.validate(); !valid) {
            return Unexpected(std::move(valid.error()));
        }
        if (!nlohmann::json::accept(revision.settings_json)) {
            return fail(ErrorCode::InvalidArgument,
                        "recipe revision rejected (settings_json must be valid JSON)", std::string(kWhere));
        }
        return core::sha256(std::as_bytes(std::span(revision.settings_json.data(), revision.settings_json.size())));
    }

    // Stores `revision` as the item's next revision with `parent` and makes it current. Caller holds
    // mutex_.
    Expected<catalog::RevisionInfo> store_locked(ItemId item, Item& entry, const catalog::NewRevision& revision,
                                                 const core::Digest256& digest,
                                                 std::optional<catalog::RevisionId> parent) {
        catalog::RecipeRevision stored;
        catalog::RevisionInfo& info = stored.info;
        info.id = catalog::RevisionId{next_id_++};
        info.item = catalog::ItemId{static_cast<std::int64_t>(item.value)};
        info.revision_number = static_cast<std::uint32_t>(entry.revisions.size() + 1);
        info.parent = parent;
        info.created_at = revision.created_at;
        info.origin = revision.origin;
        info.schema_name = revision.schema_name;
        info.schema_version = revision.schema_version;
        info.settings_sha256 = digest;
        info.settings_bytes = revision.settings_json.size();
        info.engine_version = revision.engine_version;
        info.note = revision.note;
        stored.settings_json = revision.settings_json;
        const catalog::RevisionInfo result = info;
        revisions_.emplace(info.id.value, std::move(stored));
        entry.revisions.push_back(result.id);
        entry.current = result.id;
        return result;
    }

    mutable std::mutex mutex_;
    std::int64_t next_id_ = 1;
    std::int64_t next_analysis_id_ = 1;
    std::map<std::int64_t, catalog::RecipeRevision> revisions_;
    std::unordered_map<std::uint64_t, Item> items_;
    std::vector<catalog::AutoAnalysisRecord> analyses_;
};

}  // namespace

bool is_automatic_branch_note(std::string_view note) noexcept {
    while (!note.empty()) {
        const std::size_t end = note.find(';');
        if (note.substr(0, end) == kAutomaticBranchNote) {
            return true;
        }
        if (end == std::string_view::npos) {
            break;
        }
        note.remove_prefix(end + 1);
    }
    return false;
}

Expected<AutomaticAppend> RecipeStore::append_automatic(ItemId item, std::optional<catalog::RevisionId>,
                                                        std::span<const AutomaticRevision>, bool) {
    return fail(ErrorCode::Unsupported, std::format("this store keeps no automatic revisions (item {})", item.value),
                std::string(kWhere));
}

Expected<std::vector<catalog::AutoAnalysisRecord>> RecipeStore::auto_analyses(ItemId item) const {
    return fail(ErrorCode::Unsupported, std::format("this store keeps no analysis records (item {})", item.value),
                std::string(kWhere));
}

Expected<AutomaticAppend> append_automatic_revisions(catalog::WriteTransaction& tx, catalog::ItemId item,
                                                     std::optional<catalog::RevisionId> parent,
                                                     std::span<const AutomaticRevision> revisions,
                                                     bool make_current) {
    if (Status valid = check_automatic(revisions); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    auto record = tx.find_item(item);
    if (!record) {
        return Unexpected(std::move(record.error()));
    }
    if (!*record) {
        return fail(ErrorCode::NotFound, std::format("no catalog item {}", item.value), std::string(kWhere));
    }
    const std::optional<catalog::RevisionId> found = (*record)->current_revision;
    AutomaticAppend result;
    result.made_current = make_current && found == parent;
    for (const AutomaticRevision& entry : revisions) {
        // Each revision is derived from `parent`: siblings, not a chain.
        if (Status moved = tx.set_current_revision(item, parent); !moved) {
            return Unexpected(std::move(moved.error()));
        }
        auto info = result.made_current ? tx.append_revision(item, entry.revision)
                                        : tx.append_revision(item, as_branch(entry.revision));
        if (!info) {
            return Unexpected(std::move(info.error()));
        }
        if (entry.analysis) {
            catalog::NewAutoAnalysis analysis = *entry.analysis;
            analysis.revision = info->id;
            if (auto stored = tx.record_auto_analysis(analysis); !stored) {
                return Unexpected(std::move(stored.error()));
            }
        }
        result.revisions.push_back(std::move(*info));
    }
    if (Status moved = tx.set_current_revision(item, result.made_current ? std::optional(result.revisions.back().id)
                                                                         : found);
        !moved) {
        return Unexpected(std::move(moved.error()));
    }
    return result;
}

Expected<std::shared_ptr<RecipeStore>> create_catalog_recipe_store(
    std::shared_ptr<catalog::Catalog> catalog) {
    if (!catalog) {
        return fail(ErrorCode::InvalidArgument, "the catalog is null", std::string(kWhere));
    }
    try {
        return std::shared_ptr<RecipeStore>(std::make_shared<CatalogRecipeStore>(std::move(catalog)));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot create the recipe store", std::string(kWhere));
    }
}

std::shared_ptr<RecipeStore> create_memory_recipe_store() {
    return std::make_shared<MemoryRecipeStore>();
}

}  // namespace lucent::app
