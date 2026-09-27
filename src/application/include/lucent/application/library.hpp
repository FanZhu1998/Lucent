#pragma once

// The photo library as the application services see it (blueprint section 2, "Import and
// organize"; section 4, "Application services: import, recipes, catalog, export").
//
// Library is an interface so that the W3 in-memory SessionLibrary and the W4 SQLite catalog can
// serve the same services (import, previews, export) and user interface. Implementations keep
// these contracts:
//
//   * Identity. Every item has an ItemId assigned on add. A photograph is identified by (canonical
//     path, subimage): add() refuses an item whose subimage equals an existing item's and whose
//     canonical path is the same (compared with the file system's case-insensitive rules) or
//     whose file object is the same (volume serial and file id, i.e. another path to the same
//     file, such as a hard link). Such an item is reported as a duplicate of the existing one.
//     Content duplicates under different files (same bytes) are NOT merged; blueprint section 2
//     lets intentional duplicates remain separate (content hashing is W4's).
//   * Atomicity. add() and remove() apply a whole batch or nothing (on InvalidArgument / NotFound).
//   * Changed sources. When the file of an item changes on disk after import (its identity no
//     longer matches), renders and exports refuse it with SourceChanged; importing the file again
//     calls refresh(), which replaces the item's identity, probe and support in place (blueprint
//     section 7: "reimport a changed source instead of using stale diagnostics").
//   * Snapshots. items() and find() return copies consistent with one committed state.
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
    Updated = 2,  // flags (or, later, other mutable state) of existing items changed
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
    SameFileObject = 2,  // another path to the same file object (volume serial + file id)
};

struct AddOutcome {
    ItemId id;  // the new item, or the existing item it duplicates
    bool added = false;
    DuplicateKind duplicate = DuplicateKind::None;
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

    // The item for image `subimage` of the file with canonical path `canonical_path` or with
    // file object `identity` (volume serial + file id; ignored when the file id is all zero).
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

    // Registers an observer (see the file comment). A null observer yields an inactive
    // Subscription.
    [[nodiscard]] virtual Subscription subscribe(LibraryObserver observer) = 0;

protected:
    Library() = default;
};

// The W3 library: items live in memory for the lifetime of the object (nothing is persisted).
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
    [[nodiscard]] Subscription subscribe(LibraryObserver observer) override;

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace lucent::app
