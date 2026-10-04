#pragma once

// Where an item's recipe revisions live (blueprint section 2, "undo/redo, and reset to as-shot or
// an earlier recipe"; section 8; ADR 0010). The RecipeService reads and writes revisions only
// through this interface, so the same editing rules hold for the catalog-backed library (the
// revisions persist) and for the in-memory session library (they last for the session).
//
// The model is the catalog's (lucent/catalog/types.hpp, "Recipe revisions"): revisions are
// immutable and never deleted; an item points at its current revision (none: "as shot"); an
// append derives the new revision from the current one (its parent) and makes it current; undo
// moves to the parent, redo to the most recently created child, so an edit after an undo starts a
// new branch and the undone branch stays in the history. The settings document is opaque here.
//
// Item ids: app::ItemId values are the catalog's item ids (catalog_library.hpp); revision ids are
// unique within one store.
//
// Thread-safety: implementations are safe to call concurrently; each call is atomic.

#include <cstdint>
#include <memory>
#include <optional>

#include <lucent/application/source_item.hpp>
#include <lucent/catalog/catalog.hpp>
#include <lucent/catalog/types.hpp>
#include <lucent/core/error.hpp>

namespace lucent::app {

class RecipeStore {
public:
    virtual ~RecipeStore() = default;
    RecipeStore(const RecipeStore&) = delete;
    RecipeStore& operator=(const RecipeStore&) = delete;

    // Why every change fails (a catalog opened read-only, or written by a newer Lucent);
    // std::nullopt when changes are possible.
    [[nodiscard]] virtual std::optional<Error> read_only_reason() const = 0;

    // Every revision of the item (oldest first) and its current pointer. An item without
    // revisions has an empty history (the memory store knows every item; the catalog store
    // fails with NotFound for an item the catalog does not have).
    [[nodiscard]] virtual Expected<catalog::RevisionHistory> history(ItemId item) const = 0;

    // The item's current revision with its settings document; std::nullopt: "as shot".
    [[nodiscard]] virtual Expected<std::optional<catalog::RecipeRevision>> current(ItemId item) const = 0;

    // A revision with its settings document; std::nullopt when there is none with that id.
    [[nodiscard]] virtual Expected<std::optional<catalog::RecipeRevision>> find(
        catalog::RevisionId id) const = 0;

    // Appends `revision` to the item, derived from its current revision, and makes it current,
    // provided the current revision is still `expected_current` (none: "as shot"). Otherwise
    // nothing changes and the result is AlreadyExists ("app.recipe.conflict"): another writer
    // changed the item meanwhile. Errors: those of the catalog (NotFound, NewRevision::validate's,
    // the read-only reason).
    [[nodiscard]] virtual Expected<catalog::RevisionInfo> append(
        ItemId item, const catalog::NewRevision& revision,
        std::optional<catalog::RevisionId> expected_current) = 0;

    // Moves the current pointer (catalog::WriteTransaction::set_current_revision).
    [[nodiscard]] virtual Status set_current(ItemId item, std::optional<catalog::RevisionId> revision) = 0;

    // catalog::WriteTransaction::undo_revision / redo_revision.
    [[nodiscard]] virtual Expected<std::optional<catalog::RevisionId>> undo(ItemId item) = 0;
    [[nodiscard]] virtual Expected<catalog::RevisionId> redo(ItemId item) = 0;

protected:
    RecipeStore() = default;
};

// Revisions in the catalog (schema v2, recipe_revisions): they persist across sessions and go with
// their item when it is removed. InvalidArgument ("app.recipe.store") for a null catalog.
[[nodiscard]] Expected<std::shared_ptr<RecipeStore>> create_catalog_recipe_store(
    std::shared_ptr<catalog::Catalog> catalog);

// Revisions in memory, with the catalog's semantics (validation, SHA-256, numbering, undo and redo
// rules), for libraries that persist nothing. Every item id is accepted.
[[nodiscard]] std::shared_ptr<RecipeStore> create_memory_recipe_store();

}  // namespace lucent::app
