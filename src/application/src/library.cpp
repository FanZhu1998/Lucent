#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <lucent/application/library.hpp>

#include "observer_list.hpp"
#include "win32_paths.hpp"

namespace lucent::app {

namespace {

constexpr std::string_view kWhereAdd = "app.library.add";
constexpr std::string_view kWhereFlags = "app.library.flags";
constexpr std::string_view kWhereRefresh = "app.library.refresh";
constexpr std::string_view kWhereRemove = "app.library.remove";

// Lookup key of (canonical path, subimage), compared with file-system casing rules.
std::wstring path_key(const std::filesystem::path& path, std::uint32_t subimage) {
    return detail::fold_case(path.native()) + L'|' + std::to_wstring(subimage);
}

bool has_file_id(const core::FileIdentity& identity) noexcept {
    return std::any_of(identity.file_id.begin(), identity.file_id.end(),
                       [](std::uint8_t b) { return b != 0; });
}

// Lookup key of (volume serial, file id, subimage); empty when the identity has no file id.
std::string file_key(const core::FileIdentity& identity, std::uint32_t subimage) {
    if (!has_file_id(identity)) {
        return {};
    }
    std::string key(sizeof(identity.volume_serial) + identity.file_id.size() + sizeof(subimage), '\0');
    std::memcpy(key.data(), &identity.volume_serial, sizeof(identity.volume_serial));
    std::memcpy(key.data() + sizeof(identity.volume_serial), identity.file_id.data(),
                identity.file_id.size());
    std::memcpy(key.data() + sizeof(identity.volume_serial) + identity.file_id.size(), &subimage,
                sizeof(subimage));
    return key;
}

Status validate_item(const SourceItem& item, std::size_t index, std::string_view where = kWhereAdd) {
    if (item.path.empty() || !item.path.is_absolute()) {
        return fail(ErrorCode::InvalidArgument, std::format("item {} has an empty or relative path", index),
                    std::string(where));
    }
    if (item.subimage_count == 0 || item.subimage >= item.subimage_count) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("item {} selects image {} of {}", index, item.subimage, item.subimage_count),
                    std::string(where));
    }
    if (auto flags = item.flags.validate(); !flags) {
        return fail(ErrorCode::InvalidArgument, std::format("item {}: {}", index, flags.error().message),
                    std::string(where));
    }
    return ok();
}

}  // namespace

struct SessionLibrary::State {
    mutable std::mutex mutex;
    std::map<ItemId, SourceItem> items;  // ids grow with insertion, so this is library order
    std::unordered_map<std::wstring, ItemId> by_path;
    std::unordered_map<std::string, ItemId> by_file;
    std::uint64_t next_id = 1;
    std::uint64_t sequence = 0;
    detail::ObserverList<LibraryChange> observers{"app.library"};

    void erase_indices(const SourceItem& item) {
        by_path.erase(path_key(item.path, item.subimage));
        if (const std::string key = file_key(item.identity, item.subimage); !key.empty()) {
            const auto it = by_file.find(key);
            if (it != by_file.end() && it->second == item.id) {
                by_file.erase(it);
            }
        }
    }
};

SessionLibrary::SessionLibrary() : state_(std::make_shared<State>()) {}

SessionLibrary::~SessionLibrary() = default;

std::size_t SessionLibrary::size() const {
    std::lock_guard lock(state_->mutex);
    return state_->items.size();
}

std::vector<SourceItem> SessionLibrary::items() const {
    std::lock_guard lock(state_->mutex);
    std::vector<SourceItem> out;
    out.reserve(state_->items.size());
    for (const auto& [id, item] : state_->items) {
        out.push_back(item);
    }
    return out;
}

std::optional<SourceItem> SessionLibrary::find(ItemId id) const {
    std::lock_guard lock(state_->mutex);
    const auto it = state_->items.find(id);
    if (it == state_->items.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<ItemId> SessionLibrary::find_source(const std::filesystem::path& canonical_path,
                                                  const core::FileIdentity& identity,
                                                  std::uint32_t subimage) const {
    const std::wstring by_path = path_key(canonical_path, subimage);
    const std::string by_file = file_key(identity, subimage);
    std::lock_guard lock(state_->mutex);
    if (const auto it = state_->by_path.find(by_path); it != state_->by_path.end()) {
        return it->second;
    }
    if (!by_file.empty()) {
        if (const auto it = state_->by_file.find(by_file); it != state_->by_file.end()) {
            return it->second;
        }
    }
    return std::nullopt;
}

Expected<std::vector<AddOutcome>> SessionLibrary::add(std::vector<SourceItem> items) {
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (auto valid = validate_item(items[i], i); !valid) {
            return Unexpected(std::move(valid.error()));
        }
    }
    LibraryChange change;
    std::vector<AddOutcome> outcomes;
    try {
        std::vector<std::wstring> path_keys;
        std::vector<std::string> file_keys;
        path_keys.reserve(items.size());
        file_keys.reserve(items.size());
        for (const SourceItem& item : items) {
            path_keys.push_back(path_key(item.path, item.subimage));
            file_keys.push_back(file_key(item.identity, item.subimage));
        }
        outcomes.resize(items.size());
        change.kind = LibraryChangeKind::Added;
        change.items.reserve(items.size());

        std::lock_guard lock(state_->mutex);
        std::vector<ItemId> inserted;
        try {
            for (std::size_t i = 0; i < items.size(); ++i) {
                AddOutcome& outcome = outcomes[i];
                if (const auto it = state_->by_path.find(path_keys[i]); it != state_->by_path.end()) {
                    outcome = AddOutcome{it->second, false, DuplicateKind::SamePath};
                    continue;
                }
                if (!file_keys[i].empty()) {
                    if (const auto it = state_->by_file.find(file_keys[i]); it != state_->by_file.end()) {
                        outcome = AddOutcome{it->second, false, DuplicateKind::SameFileObject};
                        continue;
                    }
                }
                SourceItem item = std::move(items[i]);
                item.id = ItemId{state_->next_id};
                item.revision = 1;
                const ItemId id = item.id;
                state_->items.emplace(id, std::move(item));
                inserted.push_back(id);
                state_->by_path.emplace(path_keys[i], id);
                if (!file_keys[i].empty()) {
                    state_->by_file.emplace(file_keys[i], id);
                }
                ++state_->next_id;
                outcome = AddOutcome{id, true, DuplicateKind::None};
                change.items.push_back(id);
            }
        } catch (...) {
            // Roll the batch back: add() applies all of it or nothing.
            for (const ItemId id : inserted) {
                if (const auto it = state_->items.find(id); it != state_->items.end()) {
                    state_->erase_indices(it->second);
                    state_->items.erase(it);
                }
            }
            throw;
        }
        if (!change.items.empty()) {
            change.sequence = ++state_->sequence;
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "adding items to the library ran out of memory",
                    std::string(kWhereAdd));
    }
    if (!change.items.empty()) {
        state_->observers.notify(change);
    }
    return outcomes;
}

Status SessionLibrary::set_flags(ItemId id, const ItemFlags& flags) {
    if (auto valid = flags.validate(); !valid) {
        return valid;
    }
    LibraryChange change;
    {
        std::lock_guard lock(state_->mutex);
        const auto it = state_->items.find(id);
        if (it == state_->items.end()) {
            return fail(ErrorCode::NotFound, std::format("no library item {}", id.value),
                        std::string(kWhereFlags));
        }
        if (it->second.flags == flags) {
            return ok();
        }
        it->second.flags = flags;
        ++it->second.revision;
        change.kind = LibraryChangeKind::Updated;
        change.items = {id};
        change.sequence = ++state_->sequence;
    }
    state_->observers.notify(change);
    return ok();
}

Status SessionLibrary::refresh(const SourceItem& fresh) {
    if (auto valid = validate_item(fresh, 0, kWhereRefresh); !valid) {
        return valid;
    }
    LibraryChange change;
    try {
        // Keys and the change are built before anything is modified, so a failed allocation
        // leaves the library as it was.
        const std::wstring fresh_path = path_key(fresh.path, fresh.subimage);
        const std::string fresh_file = file_key(fresh.identity, fresh.subimage);
        change.kind = LibraryChangeKind::Refreshed;
        change.items = {fresh.id};

        std::lock_guard lock(state_->mutex);
        const auto it = state_->items.find(fresh.id);
        if (it == state_->items.end()) {
            return fail(ErrorCode::NotFound, std::format("no library item {}", fresh.id.value),
                        std::string(kWhereRefresh));
        }
        SourceItem& item = it->second;
        if (item.subimage != fresh.subimage || path_key(item.path, item.subimage) != fresh_path) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("item {} is image {} of another file", fresh.id.value, item.subimage),
                        std::string(kWhereRefresh));
        }
        const std::string old_file = file_key(item.identity, item.subimage);
        if (!fresh_file.empty() && fresh_file != old_file) {
            const auto owner = state_->by_file.find(fresh_file);
            if (owner != state_->by_file.end() && owner->second != item.id) {
                return fail(ErrorCode::AlreadyExists,
                            std::format("the file of item {} is now the file of item {}", fresh.id.value,
                                        owner->second.value),
                            std::string(kWhereRefresh));
            }
            state_->by_file.insert_or_assign(fresh_file, item.id);  // may allocate: before any erase
        }
        if (!old_file.empty() && old_file != fresh_file) {
            const auto owned = state_->by_file.find(old_file);
            if (owned != state_->by_file.end() && owned->second == item.id) {
                state_->by_file.erase(owned);
            }
        }
        item.path = fresh.path;  // the same canonical path; the on-disk spelling may differ
        item.subimage_count = fresh.subimage_count;
        item.identity = fresh.identity;
        item.probe = fresh.probe;
        item.support = fresh.support;
        ++item.revision;
        change.sequence = ++state_->sequence;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "refreshing a library item ran out of memory",
                    std::string(kWhereRefresh));
    }
    state_->observers.notify(change);
    return ok();
}

Status SessionLibrary::remove(std::span<const ItemId> ids) {
    LibraryChange change;
    change.kind = LibraryChangeKind::Removed;
    try {
        std::lock_guard lock(state_->mutex);
        for (const ItemId id : ids) {
            if (!state_->items.contains(id)) {
                return fail(ErrorCode::NotFound, std::format("no library item {}", id.value),
                            std::string(kWhereRemove));
            }
        }
        // Everything that allocates happens before the first erase, so the removal is all or
        // nothing even when memory runs out.
        std::vector<ItemId> unique(ids.begin(), ids.end());
        std::sort(unique.begin(), unique.end());
        unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
        std::vector<ItemId> ordered;
        ordered.reserve(unique.size());
        for (const ItemId id : ids) {
            const auto it = std::lower_bound(unique.begin(), unique.end(), id);
            if (it != unique.end() && *it == id) {
                ordered.push_back(id);
                unique.erase(it);
            }
        }
        change.items.reserve(ordered.size());
        for (const ItemId id : ordered) {
            const auto it = state_->items.find(id);
            state_->erase_indices(it->second);
            state_->items.erase(it);
            change.items.push_back(id);
        }
        if (change.items.empty()) {
            return ok();
        }
        change.sequence = ++state_->sequence;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "removing items ran out of memory", std::string(kWhereRemove));
    }
    state_->observers.notify(change);
    return ok();
}

Subscription SessionLibrary::subscribe(LibraryObserver observer) {
    return state_->observers.add(std::move(observer));
}

}  // namespace lucent::app
