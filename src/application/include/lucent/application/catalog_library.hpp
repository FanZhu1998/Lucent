#pragma once

// CatalogLibrary: the persistent photo library (W4; blueprint section 2, "Import and organize";
// section 7, "Store source identity ... in local SQLite"; ADR 0008). It implements Library over a
// lucent::catalog::Catalog, so the import, preview and export services and the user interface
// work unchanged, and adds the W4 source maintenance (content hashes and duplicates, presence
// states, relocation) and import records.
//
// Model
//   * One catalog source per file (canonical path; catalog::make_path_key compares paths with the
//     file system's case rules) and one catalog item per photograph of the file (its subimage).
//     ItemId values are the catalog's item ids: stable across restarts, never reused.
//   * The identity (FileIdentity), content hash, presence state, storage kind, import mode and
//     origin are facts of the source, shared by all of its items. SourceItem::content_duplicates
//     lists the items of OTHER sources with the same content hash and subimage.
//   * What the catalog does not store (ISO, exposure, aperture, focal length and the probe's
//     format details; see src/catalog_mapping.hpp) is known only for items probed in this process.
//
// Reads come from memory. open() loads every source and item in one read transaction (the
// catalog file only: no source file is opened, stat'ed or hashed), so the contact sheet can show
// the whole library before any source I/O. items(), find() and find_source() never touch the
// database.
//
// Writes go to the catalog first, then to memory. Each mutation is one catalog write transaction
// (an import batch of add() is one commit, blueprint section 7: "Use transactions and a single
// coordinated writer"); only after it committed is the in-memory state changed and the change
// published, so a failed write changes nothing and is reported with the catalog's error. Mutations
// are serialised by the library (in addition to the catalog's own writer lock). Should memory run
// out while applying a committed change, the library reloads its state from the catalog
// (OutOfMemory is returned if that fails too; the catalog itself is correct).
//
// A read-only catalog (opened ReadOnly, or a newer schema opened read-only, catalog.hpp) can be
// listed but every mutation fails with the catalog's reason (PermissionDenied / VersionMismatch).
//
// Thread-safety: all members may be called concurrently (Library contract). The catalog must
// outlive the library (it is shared).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <lucent/application/library.hpp>
#include <lucent/catalog/catalog.hpp>
#include <lucent/core/error.hpp>

namespace lucent::app {

struct CatalogLibraryOptions {
    // At open, import records still marked Running (a previous session ended during an import)
    // are marked Interrupted. Skipped for a read-only catalog.
    bool mark_interrupted_imports = true;
};

class CatalogLibrary final : public Library {
public:
    // Loads the library from `catalog` (see the file comment). Errors ("app.library.open"):
    // InvalidArgument for a null catalog; the catalog's read errors (Corrupt, VersionMismatch for
    // values a newer Lucent wrote, IoError); OutOfMemory.
    [[nodiscard]] static Expected<std::unique_ptr<CatalogLibrary>> open(
        std::shared_ptr<catalog::Catalog> catalog, const CatalogLibraryOptions& options = {});

    ~CatalogLibrary() override;

    [[nodiscard]] std::size_t size() const override;
    [[nodiscard]] std::vector<SourceItem> items() const override;
    [[nodiscard]] std::optional<SourceItem> find(ItemId id) const override;
    [[nodiscard]] std::optional<ItemId> find_source(const std::filesystem::path& canonical_path,
                                                    const core::FileIdentity& identity,
                                                    std::uint32_t subimage) const override;
    // One catalog transaction per call. Items of one file share a catalog source; a file already
    // in the catalog with the same identity keeps its stored content hash.
    [[nodiscard]] Expected<std::vector<AddOutcome>> add(std::vector<SourceItem> items) override;
    [[nodiscard]] Status set_flags(ItemId id, const ItemFlags& flags) override;
    // The file's identity changes for every item of the file (they share the source); the stored
    // content hash is cleared when the identity changed (the file must be hashed again).
    [[nodiscard]] Status refresh(const SourceItem& fresh) override;
    // A file whose last item is removed is removed from the catalog as well.
    [[nodiscard]] Status remove(std::span<const ItemId> ids) override;
    [[nodiscard]] Subscription subscribe(LibraryObserver observer) override;

    [[nodiscard]] LibraryCapabilities capabilities() const override;
    [[nodiscard]] Status set_content_hash(ItemId item, const core::Digest256& sha256,
                                          const core::FileIdentity& hashed) override;
    [[nodiscard]] Status set_source_state(ItemId item, SourceState state) override;
    [[nodiscard]] Status relocate_source(ItemId item, const SourceRelocation& relocation) override;
    [[nodiscard]] std::optional<ItemId> find_copied_from(const std::filesystem::path& origin,
                                                         const core::FileIdentity& identity) const override;
    [[nodiscard]] Expected<std::uint64_t> begin_import(const ImportJournalStart& start) override;
    [[nodiscard]] Status finish_import(std::uint64_t record, const ImportJournalFinish& finish) override;

    [[nodiscard]] const std::shared_ptr<catalog::Catalog>& shared_catalog() const noexcept;
    // Import records marked Interrupted by open().
    [[nodiscard]] std::uint64_t interrupted_imports() const noexcept;

    struct State;  // implementation detail

private:
    explicit CatalogLibrary(std::shared_ptr<State> state) noexcept;
    std::shared_ptr<State> state_;
};

}  // namespace lucent::app
