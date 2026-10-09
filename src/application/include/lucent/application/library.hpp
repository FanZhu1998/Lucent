#pragma once

// The photo library as the application services see it (blueprint section 2, "Import and
// organize"; section 4, "Application services: import, recipes, catalog, export").
//
// Library is an interface so that the W3 in-memory SessionLibrary and the W4 catalog-backed
// CatalogLibrary (catalog_library.hpp) serve the same services (import, previews, export) and user
// interface. Implementations keep these contracts:
//
//   * Identity. Every item has an ItemId assigned on add. A photograph is identified by (canonical
//     path, subimage): add() refuses an item whose subimage equals an existing item's and whose
//     canonical path is the same (compared with the file system's case-insensitive rules) or
//     whose file version is the same (volume serial, file id, size and last write time: another
//     path to the same file, such as a hard link). Such an item is reported as a duplicate of the
//     existing one. A file id alone is not enough: FAT and exFAT (most camera cards) derive file
//     ids from directory positions and reuse them for new files, so a file object with another
//     size or modification time than the library's file is treated as a different file.
//     Content duplicates under different files (same bytes) are NOT merged; blueprint section 2
//     lets intentional duplicates remain separate. A library that hashes contents
//     (capabilities().source_maintenance) flags them in SourceItem::content_duplicates.
//   * Atomicity. add() and remove() apply a whole batch or nothing (on InvalidArgument / NotFound).
//   * Changed sources. When the file of an item changes on disk after import (its identity no
//     longer matches), renders and exports refuse it with SourceChanged; importing the file again
//     calls refresh(), which replaces the item's identity, probe and support in place (blueprint
//     section 7: "reimport a changed source instead of using stale diagnostics").
//   * Snapshots. items() and find() return copies consistent with one committed state.
//   * Support assessments (W10, ADR 0018). set_support() replaces the support assessment of
//     existing items when the camera support database changed (refresh_support() in
//     camera_support.hpp): nothing about their files changed, so it publishes Updated, not
//     Refreshed, and caches stay valid.
//   * Source maintenance (W4; libraries whose capabilities() say so, the others fail with
//     Unsupported): the content hash, the presence state and the location of a source file are
//     facts of the FILE, so set_content_hash(), set_source_state() and relocate_source() name any
//     item of the file and change every item of it. They publish one Updated change listing those
//     items and, when content duplicates appear or disappear, the other items whose
//     content_duplicates changed.
//
// Observers and their threading contract:
//   * An observer is called once per committed change (a batch add is one Added change),
//     synchronously on the thread that made the change, after the library's internal lock has
//     been released. It may therefore call any library method, including mutations (whose
//     changes it then observes re-entrantly) and subscribe / unsubscribe.
//   * When several threads change the library concurrently, observers may run concurrently on
//     those threads, and deliveries may arrive out of commit order; LibraryChange::sequence is
//     strictly increasing in commit order, so an observer can restore it. User interfaces
//     typically post the change to their own thread (for Qt, a queued invocation).
//   * Subscription::reset() (or its destructor) returns only when the observer is no longer
//     running on any other thread; after it returns the observer is never called again. Calling
//     reset() from inside the observer itself is allowed and does not wait for that call.
//   * Observers must not throw; an exception escaping an observer is caught and logged
//     ("app.library") and the other observers are still called.
//
// Thread-safety: all members of Library implementations are safe to call concurrently.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <lucent/application/source_item.hpp>
#include <lucent/core/error.hpp>
#include <lucent/core/file.hpp>

namespace lucent::app {

enum class LibraryChangeKind : std::uint8_t {
    Added = 1,
    // Mutable state of existing items changed: flags, source facts (content hash, presence
    // state, location after a relink, content duplicates) or the support assessment against a new
    // camera support database. The pixels of the photograph did not.
    Updated = 2,
    Removed = 3,
    // The source files of existing items changed on disk and were probed again (refresh()):
    // identity, probe and support were replaced. Previews and caches of these items are stale.
    Refreshed = 4,
};

struct LibraryChange {
    LibraryChangeKind kind = LibraryChangeKind::Added;
    std::uint64_t sequence = 0;  // 1, 2, 3, ... in commit order, per library
    std::vector<ItemId> items;   // affected items, in library order for Added, request order otherwise
};

using LibraryObserver = std::function<void(const LibraryChange&)>;

// Keeps an observer registered while it lives. Move-only. See the file comment for the waiting
// rule of reset().
class Subscription {
public:
    Subscription() noexcept = default;
    // `cancel` unregisters the observer and waits as documented; it is called at most once.
    explicit Subscription(std::function<void()> cancel) noexcept : cancel_(std::move(cancel)) {}
    ~Subscription() { reset(); }

    Subscription(Subscription&& other) noexcept : cancel_(std::exchange(other.cancel_, nullptr)) {}
    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) {
            reset();
            cancel_ = std::exchange(other.cancel_, nullptr);
        }
        return *this;
    }
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    void reset() noexcept {
        if (auto cancel = std::exchange(cancel_, nullptr)) {
            cancel();
        }
    }
    [[nodiscard]] bool active() const noexcept { return static_cast<bool>(cancel_); }

private:
    std::function<void()> cancel_;
};

// Why add() did not add an item.
enum class DuplicateKind : std::uint8_t {
    None = 0,
    SamePath = 1,        // same canonical path and subimage
    SameFileObject = 2,  // another path to the same file version (volume serial, file id, size, mtime)
};

struct AddOutcome {
    ItemId id;  // the new item, or the existing item it duplicates
    bool added = false;
    DuplicateKind duplicate = DuplicateKind::None;
};

// A new support assessment of an existing item (Library::set_support).
struct SupportUpdate {
    ItemId id;
    SupportAssessment support;
};

// What an implementation supports beyond the W3 contract.
struct LibraryCapabilities {
    bool persistent = false;          // items survive the process (catalog-backed)
    bool source_maintenance = false;  // set_content_hash, set_source_state, relocate_source, find_copied_from
    bool import_journal = false;      // begin_import / finish_import keep records
};

// The verified file a source moves to (Library::relocate_source): a relinked original that was
// found at another path (moved, or on a card that came back under another drive letter), or a
// verified copy in a library folder that replaces a referenced original ("Copy into library
// folder" for a photo already in the library).
struct SourceRelocation {
    std::filesystem::path path;   // canonical absolute path of the verified file
    core::FileIdentity identity;  // its identity while it was hashed (or stat'ed)
    // Its content hash (core::hash_file), or std::nullopt when `identity` alone proves that it is
    // the library's file version (see Library::relocate_source).
    std::optional<core::Digest256> sha256;
    core::StorageKind storage = core::StorageKind::Unknown;
    // Set to Copied (with origin_path and, when known, origin_identity) when the file is a copy
    // Lucent made; std::nullopt keeps the source's import mode and origin.
    std::optional<ImportMode> import_mode;
    std::optional<std::filesystem::path> origin_path;
    std::optional<core::FileIdentity> origin_identity;
};

// Import records (blueprint section 7: durable records of what ran). The outcome values are those
// of the catalog's ImportOutcome.
struct ImportJournalRoot {
    std::filesystem::path path;  // a selected file or folder
    bool include_subfolders = false;
};

struct ImportJournalStart {
    std::vector<ImportJournalRoot> roots;
    std::optional<std::filesystem::path> copy_to;  // "Copy into library folder" destination
};

enum class ImportJournalOutcome : std::uint8_t {
    Completed = 2,
    Cancelled = 3,
    Failed = 4,
};

struct ImportJournalFinish {
    ImportJournalOutcome outcome = ImportJournalOutcome::Completed;
    std::uint64_t files_seen = 0;
    std::uint64_t added = 0;
    std::uint64_t refreshed = 0;
    std::uint64_t unchanged = 0;   // files already in the library with the same identity
    std::uint64_t duplicates = 0;  // other duplicates (repeated paths, other paths to a library file)
    std::uint64_t skipped = 0;
    std::uint64_t failed = 0;
};

class Library {
public:
    virtual ~Library() = default;
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    [[nodiscard]] virtual std::size_t size() const = 0;

    // All items in library order (insertion order for the session library).
    [[nodiscard]] virtual std::vector<SourceItem> items() const = 0;

    [[nodiscard]] virtual std::optional<SourceItem> find(ItemId id) const = 0;

    // The item for image `subimage` of the file with canonical path `canonical_path` or, failing
    // that, of the file version `identity` (volume serial, file id, size and last write time all
    // equal to the library's identity of the file; ignored when the file id is all zero). A match
    // by identity may be at another path: a hard link, or the library's file moved there.
    [[nodiscard]] virtual std::optional<ItemId> find_source(const std::filesystem::path& canonical_path,
                                                            const core::FileIdentity& identity,
                                                            std::uint32_t subimage) const = 0;

    // Adds `items` (their id and revision are ignored) and returns one outcome per item, in
    // order. Duplicates of existing items, and of earlier items of the same batch, are not added.
    // One Added change lists the new items (none is published when nothing was added).
    // InvalidArgument ("app.library.add", nothing added) when an item has an empty or relative
    // path, subimage >= subimage_count, or invalid flags.
    [[nodiscard]] virtual Expected<std::vector<AddOutcome>> add(std::vector<SourceItem> items) = 0;

    // Replaces an item's flags; publishes Updated when they changed (revision + 1).
    // NotFound for an unknown id; InvalidArgument for invalid flags.
    [[nodiscard]] virtual Status set_flags(ItemId id, const ItemFlags& flags) = 0;

    // Replaces what the library knows about the file of the existing item `fresh.id` after the
    // file changed on disk and was probed again: identity, probe, support, subimage count and the
    // path's on-disk spelling come from `fresh`; id, subimage and flags stay. Publishes one
    // Refreshed change (revision + 1). Errors ("app.library.refresh"), with nothing changed:
    // NotFound for an unknown id; InvalidArgument when `fresh` is not a valid item (as for add())
    // or names another file or image (canonical path or subimage differ); AlreadyExists when its
    // file object (volume serial and file id) is another item's.
    [[nodiscard]] virtual Status refresh(const SourceItem& fresh) = 0;

    // Removes items; publishes one Removed change. NotFound (nothing removed) when any id is
    // unknown; duplicate ids are removed once.
    [[nodiscard]] virtual Status remove(std::span<const ItemId> ids) = 0;

    // W10 (ADR 0018). Replaces the support assessment of existing items, all or nothing (a
    // persistent library stores them in one transaction); identity, probe, flags and source facts
    // stay. Publishes one Updated change listing the items (revision + 1 each); nothing when
    // `updates` is empty. NotFound ("app.library.support", nothing changed) when an id is unknown;
    // the store's errors otherwise (a read-only catalog: VersionMismatch). Default: Unsupported.
    [[nodiscard]] virtual Status set_support(std::span<const SupportUpdate> updates);

    // Registers an observer (see the file comment). A null observer yields an inactive
    // Subscription.
    [[nodiscard]] virtual Subscription subscribe(LibraryObserver observer) = 0;

    // ---- W4 (see capabilities(); the defaults are those of a library without them) -------------

    [[nodiscard]] virtual LibraryCapabilities capabilities() const { return {}; }

    // Records the content hash of the file of `item`: `hashed` is the identity core::hash_file()
    // reported for the hashed bytes. SourceChanged ("app.library.hash"), with nothing changed, when
    // that is not the file version the library knows (the file changed after it was imported or
    // refreshed); NotFound for an unknown item. Default: Unsupported.
    [[nodiscard]] virtual Status set_content_hash(ItemId item, const core::Digest256& sha256,
                                                  const core::FileIdentity& hashed);

    // Records the result of a check of the file of `item`. Missing and Changed keep the items,
    // their flags and their hash; Present restores a file that was Missing or Changed and has its
    // known identity again. Nothing is published when the state is already `state`. NotFound for
    // an unknown item. Default: Unsupported.
    [[nodiscard]] virtual Status set_source_state(ItemId item, SourceState state);

    // Moves the file of `item` to the verified file `relocation` describes (blueprint section 2:
    // "Missing files remain in the catalog with a relink action"). The file is verified when the
    // caller hashed it and its hash equals the source's recorded hash, or when relocation.identity
    // equals the source's identity exactly (the same version of the same file object: a file moved
    // within its volume, or a card under another drive letter; no hash is needed, and a supplied
    // one must not contradict the recorded hash). Otherwise InvalidArgument
    // ("app.library.relocate.unverified" / "app.library.relocate.content_mismatch"). AlreadyExists
    // when another library file has that path. Path, identity, storage, state (Present) and, when
    // given, import mode and origin change; ids, flags and probe stay; the hash stays, or is
    // recorded when the source had none and the relocation brings one. Default: Unsupported.
    [[nodiscard]] virtual Status relocate_source(ItemId item, const SourceRelocation& relocation);

    // An item whose file is a copy Lucent made of the original at `origin` (a canonical path)
    // whose current identity is `identity`, if any: "Copy into library folder" uses it to avoid
    // copying an original twice. Preferably a copy made of exactly that original version
    // (SourceItem::origin_identity == identity, under any path or drive letter); otherwise a copy
    // of a file at the same origin path with the same size, which is only a candidate: a path and
    // a size do not identify a photo (cameras restart their numbering, many RAW formats have a
    // fixed size), so the caller confirms it, e.g. by comparing the original's content hash with
    // the copy's. Default: std::nullopt.
    [[nodiscard]] virtual std::optional<ItemId> find_copied_from(const std::filesystem::path& origin,
                                                                 const core::FileIdentity& identity) const;

    // Starts an import record and returns its id (0 when the library keeps no records, the
    // default). Errors are those of the store.
    [[nodiscard]] virtual Expected<std::uint64_t> begin_import(const ImportJournalStart& start);

    // Completes the record `record` (nothing to do when it is 0, the default).
    [[nodiscard]] virtual Status finish_import(std::uint64_t record, const ImportJournalFinish& finish);

protected:
    Library() = default;
};

// The W3 library: items live in memory for the lifetime of the object (nothing is persisted). It
// stores the W4 source facts of added items as given and has none of the W4 capabilities (tests,
// and Lucent.exe's fallback when its catalog cannot be opened).
class SessionLibrary final : public Library {
public:
    SessionLibrary();
    ~SessionLibrary() override;

    [[nodiscard]] std::size_t size() const override;
    [[nodiscard]] std::vector<SourceItem> items() const override;
    [[nodiscard]] std::optional<SourceItem> find(ItemId id) const override;
    [[nodiscard]] std::optional<ItemId> find_source(const std::filesystem::path& canonical_path,
                                                    const core::FileIdentity& identity,
                                                    std::uint32_t subimage) const override;
    [[nodiscard]] Expected<std::vector<AddOutcome>> add(std::vector<SourceItem> items) override;
    [[nodiscard]] Status set_flags(ItemId id, const ItemFlags& flags) override;
    [[nodiscard]] Status refresh(const SourceItem& fresh) override;
    [[nodiscard]] Status remove(std::span<const ItemId> ids) override;
    [[nodiscard]] Status set_support(std::span<const SupportUpdate> updates) override;
    [[nodiscard]] Subscription subscribe(LibraryObserver observer) override;

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace lucent::app
