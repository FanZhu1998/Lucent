#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <lucent/application/catalog_library.hpp>
#include <lucent/catalog/location.hpp>
#include <lucent/core/log.hpp>
#include <lucent/core/text.hpp>

#include "catalog_mapping.hpp"
#include "observer_list.hpp"

namespace lucent::app {

namespace {

constexpr std::string_view kWhereOpen = "app.library.open";
constexpr std::string_view kWhereAdd = "app.library.add";
constexpr std::string_view kWhereFlags = "app.library.flags";
constexpr std::string_view kWhereRefresh = "app.library.refresh";
constexpr std::string_view kWhereRemove = "app.library.remove";
constexpr std::string_view kWhereHash = "app.library.hash";
constexpr std::string_view kWhereState = "app.library.state";
constexpr std::string_view kWhereRelocate = "app.library.relocate";
constexpr std::string_view kWhereJournal = "app.library.journal";
constexpr std::string_view kWhereSupport = "app.library.support";

ItemId to_app(catalog::ItemId id) noexcept {
    return ItemId{static_cast<std::uint64_t>(id.value)};
}

catalog::ItemId to_catalog_id(ItemId id) noexcept {
    return catalog::ItemId{static_cast<std::int64_t>(id.value)};
}

bool has_file_id(const core::FileIdentity& identity) noexcept {
    return std::any_of(identity.file_id.begin(), identity.file_id.end(),
                       [](std::uint8_t b) { return b != 0; });
}

// Lookup key of a file version: volume serial, file id, size and last write time; empty when the
// identity has no file id. A file id alone does not identify a file: FAT and exFAT derive it from
// the directory position and reuse it for new files (library.hpp).
std::string file_key(const core::FileIdentity& identity) {
    if (!has_file_id(identity)) {
        return {};
    }
    std::string key(sizeof(identity.volume_serial) + identity.file_id.size() + sizeof(identity.size_bytes) +
                        sizeof(identity.last_write_time),
                    '\0');
    char* out = key.data();
    std::memcpy(out, &identity.volume_serial, sizeof(identity.volume_serial));
    out += sizeof(identity.volume_serial);
    std::memcpy(out, identity.file_id.data(), identity.file_id.size());
    out += identity.file_id.size();
    std::memcpy(out, &identity.size_bytes, sizeof(identity.size_bytes));
    out += sizeof(identity.size_bytes);
    std::memcpy(out, &identity.last_write_time, sizeof(identity.last_write_time));
    return key;
}

Status validate_item(const SourceItem& item, std::size_t index, std::string_view where) {
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
    if (item.import_mode == ImportMode::Copied && !item.origin_path) {
        return fail(ErrorCode::InvalidArgument, std::format("item {} is a copy without an origin", index),
                    std::string(where));
    }
    return ok();
}

// The catalog's key of `path`, with the error reported at `where`.
Expected<std::string> path_key(const std::filesystem::path& path, std::string_view where) {
    Expected<std::string> key = catalog::make_path_key(path);
    if (!key) {
        return fail(ErrorCode::InvalidArgument, key.error().message, std::string(where));
    }
    return key;
}

// Wraps a failure of the catalog with the library operation's location (the catalog's own
// location and native code are kept in the message for diagnostics).
Error from_catalog_error(const Error& error, std::string_view where) {
    Error out = error;
    out.message = std::format("{} ({})", error.message, error.where);
    out.where = std::string(where);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------

struct CatalogLibrary::State {
    struct Source {
        catalog::SourceId id;
        std::string path_key;
        std::string file_key;            // version key (file_key()); empty without a file id
        std::string origin_key;          // Copied sources: path key of the origin; empty otherwise
        std::string origin_version_key;  // Copied sources: file_key() of the origin identity, if known
        std::vector<ItemId> items;       // ordered by subimage
    };

    // A planned change, published after every lock has been released.
    struct Pending {
        std::vector<LibraryChange> changes;
    };

    explicit State(std::shared_ptr<catalog::Catalog> c) : store(std::move(c)) {}

    std::shared_ptr<catalog::Catalog> store;
    std::uint64_t interrupted_imports = 0;

    std::mutex write_mutex;  // serialises mutations: catalog write, memory update, sequence

    mutable std::mutex mutex;                                     // guards everything below
    std::map<ItemId, SourceItem> items;                           // ids grow with insertion: library order
    std::unordered_map<std::uint64_t, std::int64_t> item_source;  // item id -> source id
    std::unordered_map<std::int64_t, Source> sources;
    std::unordered_map<std::string, std::int64_t> by_path;            // path key -> source
    std::unordered_map<std::string, std::int64_t> by_file;            // file version key -> source
    std::unordered_map<std::string, std::int64_t> by_origin;          // origin path key -> source
    std::unordered_map<std::string, std::int64_t> by_origin_version;  // origin version key -> source
    std::map<core::Digest256, std::set<std::int64_t>> by_hash;
    std::uint64_t sequence = 0;
    detail::ObserverList<LibraryChange> observers{"app.library"};

    // ---- Lookups (require `mutex`) --------------------------------------------------------------

    Source* source_of(ItemId item) {
        const auto found = item_source.find(item.value);
        if (found == item_source.end()) {
            return nullptr;
        }
        const auto source = sources.find(found->second);
        return source == sources.end() ? nullptr : &source->second;
    }

    std::optional<ItemId> item_with_subimage(const Source& source, std::uint32_t subimage) const {
        for (const ItemId id : source.items) {
            const auto found = items.find(id);
            if (found != items.end() && found->second.subimage == subimage) {
                return id;
            }
        }
        return std::nullopt;
    }

    const SourceItem* first_item(const Source& source) const {
        if (source.items.empty()) {
            return nullptr;
        }
        const auto found = items.find(source.items.front());
        return found == items.end() ? nullptr : &found->second;
    }

    // ---- Index maintenance (require `mutex`) ----------------------------------------------------

    void index_source(std::int64_t id, Source& source) {
        by_path[source.path_key] = id;
        if (!source.file_key.empty()) {
            by_file[source.file_key] = id;
        }
        if (!source.origin_key.empty()) {
            by_origin[source.origin_key] = id;
        }
        if (!source.origin_version_key.empty()) {
            by_origin_version[source.origin_version_key] = id;
        }
    }

    void unindex_source(std::int64_t id, const Source& source) {
        const auto erase_if_owner = [id](auto& map, const std::string& key) {
            if (key.empty()) {
                return;
            }
            const auto found = map.find(key);
            if (found != map.end() && found->second == id) {
                map.erase(found);
            }
        };
        erase_if_owner(by_path, source.path_key);
        erase_if_owner(by_file, source.file_key);
        erase_if_owner(by_origin, source.origin_key);
        erase_if_owner(by_origin_version, source.origin_version_key);
    }

    void set_hash_membership(std::int64_t id, const std::optional<core::Digest256>& old_hash,
                             const std::optional<core::Digest256>& new_hash) {
        if (old_hash == new_hash) {
            return;
        }
        if (old_hash) {
            const auto group = by_hash.find(*old_hash);
            if (group != by_hash.end()) {
                group->second.erase(id);
                if (group->second.empty()) {
                    by_hash.erase(group);
                }
            }
        }
        if (new_hash) {
            by_hash[*new_hash].insert(id);
        }
    }

    // Recomputes content_duplicates of every item of the sources in the hash group of `hash`
    // (and of `extra`, a source that may just have left it), appending items whose list changed
    // (their revision is incremented) to `changed`.
    void recompute_duplicates(const std::optional<core::Digest256>& hash, std::int64_t extra,
                              std::vector<ItemId>& changed) {
        std::vector<std::int64_t> group;
        if (hash) {
            if (const auto found = by_hash.find(*hash); found != by_hash.end()) {
                group.assign(found->second.begin(), found->second.end());
            }
        }
        if (extra != 0 && std::find(group.begin(), group.end(), extra) == group.end()) {
            group.push_back(extra);
        }
        for (const std::int64_t source_id : group) {
            const auto source = sources.find(source_id);
            if (source == sources.end()) {
                continue;
            }
            for (const ItemId id : source->second.items) {
                const auto item = items.find(id);
                if (item == items.end()) {
                    continue;
                }
                std::vector<ItemId> duplicates;
                if (item->second.sha256) {
                    const auto members = by_hash.find(*item->second.sha256);
                    if (members != by_hash.end()) {
                        for (const std::int64_t other : members->second) {
                            if (other == source_id) {
                                continue;
                            }
                            const auto other_source = sources.find(other);
                            if (other_source == sources.end()) {
                                continue;
                            }
                            if (auto twin = item_with_subimage(other_source->second, item->second.subimage)) {
                                duplicates.push_back(*twin);
                            }
                        }
                    }
                }
                std::sort(duplicates.begin(), duplicates.end());
                if (duplicates != item->second.content_duplicates) {
                    item->second.content_duplicates = std::move(duplicates);
                    ++item->second.revision;
                    changed.push_back(id);
                }
            }
        }
    }

    // Appends a change of `kind` listing `ids` (without repetitions, in first-seen order) to
    // `pending`, with the next sequence number. Nothing for an empty list.
    void publish_later(Pending& pending, LibraryChangeKind kind, std::vector<ItemId> ids) {
        std::vector<ItemId> unique;
        std::unordered_set<std::uint64_t> seen;
        for (const ItemId id : ids) {
            if (seen.insert(id.value).second) {
                unique.push_back(id);
            }
        }
        if (unique.empty()) {
            return;
        }
        LibraryChange change;
        change.kind = kind;
        change.items = std::move(unique);
        change.sequence = ++sequence;
        pending.changes.push_back(std::move(change));
    }

    void notify(const Pending& pending) const {
        for (const LibraryChange& change : pending.changes) {
            observers.notify(change);
        }
    }

    // ---- Loading ------------------------------------------------------------------------------

    // Replaces the in-memory state with the catalog's (one read transaction). Requires `mutex`
    // when other threads may read; open() calls it before the object is shared.
    Status load_locked() {
        std::vector<catalog::SourceRecord> source_records;
        std::vector<catalog::ItemRecord> item_records;
        Status read = store->read([&](catalog::ReadTransaction& tx) -> Status {
            catalog::SourceQuery query;
            query.limit = catalog::kMaxPageSize;
            while (true) {
                auto page = tx.list_sources(query);
                if (!page) {
                    return Unexpected(std::move(page.error()));
                }
                if (page->empty()) {
                    break;
                }
                query.after = page->back().id;
                source_records.insert(source_records.end(), std::make_move_iterator(page->begin()),
                                      std::make_move_iterator(page->end()));
                if (page->size() < query.limit) {
                    break;
                }
            }
            catalog::ItemQuery items_query;
            items_query.order = catalog::ItemOrder::Added;
            items_query.limit = catalog::kMaxPageSize;
            while (true) {
                auto page = tx.list_items(items_query);
                if (!page) {
                    return Unexpected(std::move(page.error()));
                }
                item_records.insert(item_records.end(), std::make_move_iterator(page->items.begin()),
                                    std::make_move_iterator(page->items.end()));
                if (!page->next) {
                    break;
                }
                items_query.after = page->next;
            }
            return ok();
        });
        if (!read) {
            return Unexpected(from_catalog_error(read.error(), kWhereOpen));
        }

        items.clear();
        item_source.clear();
        sources.clear();
        by_path.clear();
        by_file.clear();
        by_origin.clear();
        by_origin_version.clear();
        by_hash.clear();

        std::unordered_map<std::int64_t, const catalog::SourceRecord*> records;
        for (const catalog::SourceRecord& record : source_records) {
            records.emplace(record.id.value, &record);
            Source source;
            source.id = record.id;
            source.path_key = record.path_key;
            source.file_key = file_key(record.identity);
            if (record.import_mode == catalog::ImportMode::Copied && record.origin_path) {
                if (auto key = catalog::make_path_key(*record.origin_path)) {
                    source.origin_key = std::move(*key);
                }
                if (record.origin_identity) {
                    source.origin_version_key = file_key(*record.origin_identity);
                }
            }
            auto [it, inserted] = sources.emplace(record.id.value, std::move(source));
            index_source(record.id.value, it->second);
            if (record.sha256) {
                by_hash[*record.sha256].insert(record.id.value);
            }
        }
        // Image counts per source: the larger of the catalogued images and the highest index + 1.
        std::unordered_map<std::int64_t, std::uint32_t> counts;
        std::unordered_map<std::int64_t, std::uint32_t> highest;
        for (const catalog::ItemRecord& record : item_records) {
            ++counts[record.source_id.value];
            auto& top = highest[record.source_id.value];
            top = std::max(top, record.metadata.subimage_index + 1);
        }
        for (const catalog::ItemRecord& record : item_records) {
            const auto source_record = records.find(record.source_id.value);
            if (source_record == records.end()) {
                continue;  // cannot happen: items reference their source (foreign key)
            }
            const catalog::SourceRecord& src = *source_record->second;
            SourceItem item;
            item.id = to_app(record.id);
            item.path = src.path;
            item.subimage = record.metadata.subimage_index;
            item.subimage_count = std::max(counts[record.source_id.value], highest[record.source_id.value]);
            item.identity = src.identity;
            item.support =
                detail::decode_support(record.metadata.support_status, record.metadata.support_reasons_json);
            item.probe = detail::probe_from_metadata(record.metadata, item.support);
            item.flags = detail::from_catalog(record.flags);
            item.revision = 1;
            item.source_state = detail::from_catalog(src.state);
            item.storage = src.storage_kind;
            item.import_mode = detail::from_catalog(src.import_mode);
            item.origin_path = src.origin_path;
            item.origin_identity = src.origin_identity;
            item.sha256 = src.sha256;
            const ItemId id = item.id;
            items.emplace(id, std::move(item));
            item_source.emplace(id.value, record.source_id.value);
            sources[record.source_id.value].items.push_back(id);
        }
        for (auto& [id, source] : sources) {
            std::sort(source.items.begin(), source.items.end(),
                      [this](ItemId a, ItemId b) { return items.at(a).subimage < items.at(b).subimage; });
        }
        std::vector<ItemId> unused;
        for (const auto& [hash, group] : by_hash) {
            if (group.size() > 1) {
                recompute_duplicates(hash, 0, unused);
            }
        }
        for (auto& [id, item] : items) {
            item.revision = 1;  // recompute_duplicates counted the duplicate lists as changes
        }
        return ok();
    }

    // After a failure to apply a committed change in memory: reload everything (best effort).
    Status reload_after_failure(std::string_view where) {
        core::log_error("app.library", "memory ran out while applying a committed change ({}); reloading",
                        where);
        try {
            if (Status loaded = load_locked(); !loaded) {
                return loaded;
            }
        } catch (const std::bad_alloc&) {
            // Fall through.
        }
        return fail(ErrorCode::OutOfMemory, "the library ran out of memory; restart Lucent to reload it",
                    std::string(where));
    }
};

// ---------------------------------------------------------------------------------------------
// CatalogLibrary
// ---------------------------------------------------------------------------------------------

CatalogLibrary::CatalogLibrary(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

CatalogLibrary::~CatalogLibrary() = default;

Expected<std::unique_ptr<CatalogLibrary>> CatalogLibrary::open(std::shared_ptr<catalog::Catalog> catalog,
                                                               const CatalogLibraryOptions& options) {
    if (!catalog) {
        return fail(ErrorCode::InvalidArgument, "the catalog is null", std::string(kWhereOpen));
    }
    try {
        auto state = std::make_shared<State>(std::move(catalog));
        if (options.mark_interrupted_imports && !state->store->is_read_only()) {
            auto marked = state->store->transact([](catalog::WriteTransaction& tx) {
                return tx.mark_interrupted_imports(catalog::current_time());
            });
            if (!marked) {
                return Unexpected(from_catalog_error(marked.error(), kWhereOpen));
            }
            state->interrupted_imports = *marked;
            if (*marked > 0) {
                core::log_warn("app.library", "{} import(s) of an earlier session were interrupted", *marked);
            }
        }
        {
            std::lock_guard lock(state->mutex);
            if (Status loaded = state->load_locked(); !loaded) {
                return Unexpected(std::move(loaded.error()));
            }
        }
        return std::unique_ptr<CatalogLibrary>(new CatalogLibrary(std::move(state)));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "loading the library ran out of memory", std::string(kWhereOpen));
    }
}

const std::shared_ptr<catalog::Catalog>& CatalogLibrary::shared_catalog() const noexcept {
    return state_->store;
}

std::uint64_t CatalogLibrary::interrupted_imports() const noexcept {
    return state_->interrupted_imports;
}

LibraryCapabilities CatalogLibrary::capabilities() const {
    const bool writable = !state_->store->is_read_only();
    return LibraryCapabilities{true, writable, writable};
}

std::size_t CatalogLibrary::size() const {
    std::lock_guard lock(state_->mutex);
    return state_->items.size();
}

std::vector<SourceItem> CatalogLibrary::items() const {
    std::lock_guard lock(state_->mutex);
    std::vector<SourceItem> out;
    out.reserve(state_->items.size());
    for (const auto& [id, item] : state_->items) {
        out.push_back(item);
    }
    return out;
}

std::optional<SourceItem> CatalogLibrary::find(ItemId id) const {
    std::lock_guard lock(state_->mutex);
    const auto found = state_->items.find(id);
    if (found == state_->items.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<ItemId> CatalogLibrary::find_source(const std::filesystem::path& canonical_path,
                                                  const core::FileIdentity& identity,
                                                  std::uint32_t subimage) const {
    auto key = catalog::make_path_key(canonical_path);
    const std::string by_file = file_key(identity);
    std::lock_guard lock(state_->mutex);
    if (key) {
        if (const auto found = state_->by_path.find(*key); found != state_->by_path.end()) {
            if (auto id = state_->item_with_subimage(state_->sources.at(found->second), subimage)) {
                return id;
            }
        }
    }
    if (!by_file.empty()) {
        if (const auto found = state_->by_file.find(by_file); found != state_->by_file.end()) {
            if (auto id = state_->item_with_subimage(state_->sources.at(found->second), subimage)) {
                return id;
            }
        }
    }
    return std::nullopt;
}

Expected<std::vector<AddOutcome>> CatalogLibrary::add(std::vector<SourceItem> items) {
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (Status valid = validate_item(items[i], i, kWhereAdd); !valid) {
            return Unexpected(std::move(valid.error()));
        }
    }
    State& s = *state_;
    State::Pending pending;
    std::vector<AddOutcome> outcomes;
    try {
        std::vector<std::string> path_keys;
        std::vector<std::string> file_keys;
        path_keys.reserve(items.size());
        file_keys.reserve(items.size());
        for (const SourceItem& item : items) {
            auto key = path_key(item.path, kWhereAdd);
            if (!key) {
                return Unexpected(std::move(key.error()));
            }
            path_keys.push_back(std::move(*key));
            file_keys.push_back(file_key(item.identity));
        }
        outcomes.resize(items.size());

        std::lock_guard write(s.write_mutex);

        // Duplicates of library items and of earlier items of the batch.
        std::vector<std::optional<std::size_t>> earlier(items.size());  // batch duplicate -> first index
        std::vector<std::size_t> accepted;
        {
            std::lock_guard lock(s.mutex);
            std::map<std::pair<std::string, std::uint32_t>, std::size_t> batch_paths;
            std::map<std::pair<std::string, std::uint32_t>, std::size_t> batch_files;
            for (std::size_t i = 0; i < items.size(); ++i) {
                const std::uint32_t subimage = items[i].subimage;
                if (const auto found = s.by_path.find(path_keys[i]); found != s.by_path.end()) {
                    if (auto id = s.item_with_subimage(s.sources.at(found->second), subimage)) {
                        outcomes[i] = AddOutcome{*id, false, DuplicateKind::SamePath};
                        continue;
                    }
                }
                if (!file_keys[i].empty()) {
                    if (const auto found = s.by_file.find(file_keys[i]); found != s.by_file.end()) {
                        const State::Source& other = s.sources.at(found->second);
                        if (other.path_key != path_keys[i]) {
                            if (auto id = s.item_with_subimage(other, subimage)) {
                                outcomes[i] = AddOutcome{*id, false, DuplicateKind::SameFileObject};
                                continue;
                            }
                        }
                    }
                }
                if (const auto found = batch_paths.find({path_keys[i], subimage});
                    found != batch_paths.end()) {
                    earlier[i] = found->second;
                    outcomes[i].duplicate = DuplicateKind::SamePath;
                    continue;
                }
                if (!file_keys[i].empty()) {
                    if (const auto found = batch_files.find({file_keys[i], subimage});
                        found != batch_files.end()) {
                        if (path_keys[found->second] != path_keys[i]) {
                            earlier[i] = found->second;
                            outcomes[i].duplicate = DuplicateKind::SameFileObject;
                            continue;
                        }
                    }
                    batch_files.emplace(std::pair{file_keys[i], subimage}, i);
                }
                batch_paths.emplace(std::pair{path_keys[i], subimage}, i);
                accepted.push_back(i);
            }
        }

        // One group per file, in the order of first appearance.
        std::vector<std::vector<std::size_t>> groups;
        {
            std::unordered_map<std::string, std::size_t> group_of;
            for (const std::size_t i : accepted) {
                const auto [it, inserted] = group_of.emplace(path_keys[i], groups.size());
                if (inserted) {
                    groups.emplace_back();
                }
                groups[it->second].push_back(i);
            }
        }

        // How a file entered the library is a fact of the file: a library copy stays a copy (with its
        // origin) when an import in place adds images of it.
        {
            std::lock_guard lock(s.mutex);
            for (const std::vector<std::size_t>& group : groups) {
                const auto found = s.by_path.find(path_keys[group.front()]);
                if (found == s.by_path.end()) {
                    continue;
                }
                const SourceItem* existing = s.first_item(s.sources.at(found->second));
                if (existing == nullptr || existing->import_mode != ImportMode::Copied) {
                    continue;
                }
                for (const std::size_t i : group) {
                    if (items[i].import_mode == ImportMode::Referenced) {
                        items[i].import_mode = ImportMode::Copied;
                        items[i].origin_path = existing->origin_path;
                        items[i].origin_identity = existing->origin_identity;
                    }
                }
            }
        }

        struct GroupResult {
            catalog::SourceUpsertResult source;
            std::vector<catalog::ItemId> ids;
        };
        std::vector<GroupResult> results(groups.size());
        if (!groups.empty()) {
            const catalog::Timestamp now = catalog::current_time();
            Status written = s.store->write([&](catalog::WriteTransaction& tx) -> Status {
                for (std::size_t g = 0; g < groups.size(); ++g) {
                    const SourceItem& first = items[groups[g].front()];
                    catalog::SourceObservation observation;
                    observation.path = first.path;
                    observation.identity = first.identity;
                    observation.sha256 = first.sha256;
                    observation.import_mode = detail::to_catalog(first.import_mode);
                    if (first.import_mode == ImportMode::Copied) {
                        observation.origin_path = first.origin_path;
                        observation.origin_identity = first.origin_identity;
                    }
                    observation.storage_kind = first.storage;
                    observation.seen_at = now;
                    auto source = tx.upsert_source(observation);
                    if (!source) {
                        return Unexpected(std::move(source.error()));
                    }
                    std::vector<catalog::ItemMetadata> metadata;
                    metadata.reserve(groups[g].size());
                    for (const std::size_t i : groups[g]) {
                        metadata.push_back(detail::to_metadata(items[i], now));
                    }
                    auto ids = tx.upsert_items(source->id, metadata);
                    if (!ids) {
                        return Unexpected(std::move(ids.error()));
                    }
                    for (std::size_t k = 0; k < groups[g].size(); ++k) {
                        const ItemFlags& flags = items[groups[g][k]].flags;
                        if (flags != ItemFlags{}) {
                            if (Status set = tx.set_item_flags((*ids)[k], detail::to_catalog(flags)); !set) {
                                return set;
                            }
                        }
                    }
                    results[g] = GroupResult{*source, std::move(*ids)};
                }
                return ok();
            });
            if (!written) {
                return Unexpected(from_catalog_error(written.error(), kWhereAdd));
            }
        }

        // Committed: apply to memory.
        try {
            std::lock_guard lock(s.mutex);
            std::vector<ItemId> added;
            std::vector<ItemId> updated;
            std::vector<std::optional<core::Digest256>> touched_hashes;
            std::vector<std::int64_t> touched_sources;
            for (std::size_t g = 0; g < groups.size(); ++g) {
                // The facts of the file, copied before the group's items are moved into the index.
                const SourceItem& head = items[groups[g].front()];
                const std::filesystem::path file_path = head.path;
                const core::FileIdentity file_identity = head.identity;
                const core::StorageKind file_storage = head.storage;
                const ImportMode file_mode = head.import_mode;
                const std::optional<std::filesystem::path> file_origin =
                    head.import_mode == ImportMode::Copied ? head.origin_path : std::nullopt;
                const std::optional<core::FileIdentity> file_origin_identity =
                    head.import_mode == ImportMode::Copied ? head.origin_identity : std::nullopt;
                const std::optional<core::Digest256> observed_hash = head.sha256;
                const std::int64_t source_id = results[g].source.id.value;
                auto [entry, created] = s.sources.try_emplace(source_id);
                State::Source& source = entry->second;
                std::optional<core::Digest256> old_hash;
                if (!created) {
                    if (const SourceItem* sample = s.first_item(source)) {
                        old_hash = sample->sha256;
                    }
                    s.unindex_source(source_id, source);
                }
                std::optional<core::Digest256> new_hash = observed_hash;
                if (!new_hash && (results[g].source.outcome == catalog::SourceUpsertOutcome::Unchanged ||
                                  results[g].source.outcome == catalog::SourceUpsertOutcome::Restored)) {
                    new_hash = old_hash;  // the catalog kept the stored hash
                }
                source.id = results[g].source.id;
                source.path_key = path_keys[groups[g].front()];
                source.file_key = file_keys[groups[g].front()];
                source.origin_key.clear();
                source.origin_version_key.clear();
                if (file_origin) {
                    if (auto key = catalog::make_path_key(*file_origin)) {
                        source.origin_key = std::move(*key);
                    }
                    if (file_origin_identity) {
                        source.origin_version_key = file_key(*file_origin_identity);
                    }
                }
                s.index_source(source_id, source);
                s.set_hash_membership(source_id, old_hash, new_hash);

                const auto apply_source_facts = [&](SourceItem& item) {
                    item.path = file_path;
                    item.identity = file_identity;
                    item.sha256 = new_hash;
                    item.source_state = SourceState::Present;
                    item.storage = file_storage;
                    item.import_mode = file_mode;
                    item.origin_path = file_origin;
                    item.origin_identity = file_origin_identity;
                };
                // Existing items of the file (other images) share the new facts.
                for (const ItemId id : source.items) {
                    SourceItem& existing = s.items.at(id);
                    const SourceItem before = existing;
                    apply_source_facts(existing);
                    if (existing.path != before.path || existing.identity != before.identity ||
                        existing.sha256 != before.sha256 || existing.source_state != before.source_state ||
                        existing.storage != before.storage || existing.import_mode != before.import_mode ||
                        existing.origin_path != before.origin_path ||
                        existing.origin_identity != before.origin_identity) {
                        ++existing.revision;
                        updated.push_back(id);
                    }
                }
                for (std::size_t k = 0; k < groups[g].size(); ++k) {
                    const std::size_t i = groups[g][k];
                    SourceItem item = std::move(items[i]);
                    item.id = to_app(results[g].ids[k]);
                    item.revision = 1;
                    item.content_duplicates.clear();
                    apply_source_facts(item);
                    const ItemId id = item.id;
                    s.items.insert_or_assign(id, std::move(item));
                    s.item_source[id.value] = source_id;
                    source.items.push_back(id);
                    outcomes[i] = AddOutcome{id, true, DuplicateKind::None};
                    added.push_back(id);
                }
                std::sort(source.items.begin(), source.items.end(), [&](ItemId a, ItemId b) {
                    return s.items.at(a).subimage < s.items.at(b).subimage;
                });
                touched_hashes.push_back(old_hash);
                touched_hashes.push_back(new_hash);
                touched_sources.push_back(source_id);
            }
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (earlier[i]) {
                    outcomes[i].id = outcomes[*earlier[i]].id;
                }
            }
            std::vector<ItemId> duplicates_changed;
            for (std::size_t t = 0; t < touched_hashes.size(); ++t) {
                s.recompute_duplicates(touched_hashes[t], touched_sources[t / 2], duplicates_changed);
            }
            // New items are announced by Added; their duplicate lists are part of that snapshot.
            std::erase_if(duplicates_changed, [&](ItemId id) {
                return std::find(added.begin(), added.end(), id) != added.end();
            });
            for (const ItemId id : added) {
                s.items.at(id).revision = 1;
            }
            updated.insert(updated.end(), duplicates_changed.begin(), duplicates_changed.end());
            s.publish_later(pending, LibraryChangeKind::Added, std::move(added));
            s.publish_later(pending, LibraryChangeKind::Updated, std::move(updated));
        } catch (const std::bad_alloc&) {
            std::lock_guard lock(s.mutex);
            return Unexpected(s.reload_after_failure(kWhereAdd).error());
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "adding items to the library ran out of memory",
                    std::string(kWhereAdd));
    }
    s.notify(pending);
    return outcomes;
}

Status CatalogLibrary::set_flags(ItemId id, const ItemFlags& flags) {
    if (Status valid = flags.validate(); !valid) {
        return valid;
    }
    State& s = *state_;
    State::Pending pending;
    try {
        std::lock_guard write(s.write_mutex);
        {
            std::lock_guard lock(s.mutex);
            const auto found = s.items.find(id);
            if (found == s.items.end()) {
                return fail(ErrorCode::NotFound, std::format("no library item {}", id.value),
                            std::string(kWhereFlags));
            }
            if (found->second.flags == flags) {
                return ok();
            }
        }
        Status written = s.store->write([&](catalog::WriteTransaction& tx) {
            return tx.set_item_flags(to_catalog_id(id), detail::to_catalog(flags));
        });
        if (!written) {
            return Unexpected(from_catalog_error(written.error(), kWhereFlags));
        }
        std::lock_guard lock(s.mutex);
        SourceItem& item = s.items.at(id);
        item.flags = flags;
        ++item.revision;
        s.publish_later(pending, LibraryChangeKind::Updated, {id});
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "changing flags ran out of memory", std::string(kWhereFlags));
    }
    s.notify(pending);
    return ok();
}

Status CatalogLibrary::set_support(std::span<const SupportUpdate> updates) {
    if (updates.empty()) {
        return ok();
    }
    State& s = *state_;
    State::Pending pending;
    try {
        std::lock_guard write(s.write_mutex);
        {
            std::lock_guard lock(s.mutex);
            for (const SupportUpdate& update : updates) {
                if (!s.items.contains(update.id)) {
                    return fail(ErrorCode::NotFound, std::format("no library item {}", update.id.value),
                                std::string(kWhereSupport));
                }
            }
        }
        // Only the support columns change: every other stored column (probe time, capture fields
        // this build does not read back) is written back as stored.
        Status written = s.store->write([&](catalog::WriteTransaction& tx) -> Status {
            for (const SupportUpdate& update : updates) {
                auto record = tx.find_item(to_catalog_id(update.id));
                if (!record) {
                    return Unexpected(std::move(record.error()));
                }
                if (!*record) {
                    return fail(ErrorCode::NotFound, std::format("no catalog item {}", update.id.value),
                                std::string(kWhereSupport));
                }
                catalog::ItemMetadata metadata = (*record)->metadata;
                metadata.support_status = detail::to_catalog(update.support.status);
                metadata.support_reasons_json = detail::encode_support_reasons(update.support);
                auto ids = tx.upsert_items((*record)->source_id, std::span(&metadata, 1));
                if (!ids) {
                    return Unexpected(std::move(ids.error()));
                }
            }
            return ok();
        });
        if (!written) {
            return Unexpected(from_catalog_error(written.error(), kWhereSupport));
        }
        std::vector<ItemId> changed;
        changed.reserve(updates.size());
        std::lock_guard lock(s.mutex);
        for (const SupportUpdate& update : updates) {
            SourceItem& item = s.items.at(update.id);
            item.support = update.support;
            ++item.revision;
            if (std::find(changed.begin(), changed.end(), update.id) == changed.end()) {
                changed.push_back(update.id);
            }
        }
        s.publish_later(pending, LibraryChangeKind::Updated, std::move(changed));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "updating support assessments ran out of memory",
                    std::string(kWhereSupport));
    }
    s.notify(pending);
    return ok();
}

Status CatalogLibrary::refresh(const SourceItem& fresh) {
    if (Status valid = validate_item(fresh, 0, kWhereRefresh); !valid) {
        return valid;
    }
    State& s = *state_;
    State::Pending pending;
    try {
        auto key = path_key(fresh.path, kWhereRefresh);
        if (!key) {
            return Unexpected(std::move(key.error()));
        }
        const std::string fresh_file = file_key(fresh.identity);

        std::lock_guard write(s.write_mutex);
        std::int64_t source_id = 0;
        catalog::SourceObservation observation;
        {
            std::lock_guard lock(s.mutex);
            const auto found = s.items.find(fresh.id);
            if (found == s.items.end()) {
                return fail(ErrorCode::NotFound, std::format("no library item {}", fresh.id.value),
                            std::string(kWhereRefresh));
            }
            const SourceItem& item = found->second;
            State::Source* source = s.source_of(fresh.id);
            if (source == nullptr || item.subimage != fresh.subimage || source->path_key != *key) {
                return fail(ErrorCode::InvalidArgument,
                            std::format("item {} is image {} of another file", fresh.id.value, item.subimage),
                            std::string(kWhereRefresh));
            }
            source_id = source->id.value;
            if (!fresh_file.empty()) {
                const auto owner = s.by_file.find(fresh_file);
                if (owner != s.by_file.end() && owner->second != source_id) {
                    return fail(ErrorCode::AlreadyExists,
                                std::format("the file of item {} is now the file of another library item",
                                            fresh.id.value),
                                std::string(kWhereRefresh));
                }
            }
            observation.path = fresh.path;
            observation.identity = fresh.identity;
            observation.sha256 = fresh.sha256;
            observation.import_mode = detail::to_catalog(item.import_mode);
            observation.origin_path =
                item.import_mode == ImportMode::Copied ? item.origin_path : std::nullopt;
            observation.origin_identity =
                item.import_mode == ImportMode::Copied ? item.origin_identity : std::nullopt;
            observation.storage_kind =
                fresh.storage != core::StorageKind::Unknown ? fresh.storage : item.storage;
        }
        const catalog::Timestamp now = catalog::current_time();
        observation.seen_at = now;
        catalog::SourceUpsertOutcome outcome = catalog::SourceUpsertOutcome::Unchanged;
        Status written = s.store->write([&](catalog::WriteTransaction& tx) -> Status {
            auto upserted = tx.upsert_source(observation);
            if (!upserted) {
                return Unexpected(std::move(upserted.error()));
            }
            outcome = upserted->outcome;
            const catalog::ItemMetadata metadata = detail::to_metadata(fresh, now);
            auto ids = tx.upsert_items(catalog::SourceId{source_id}, std::span(&metadata, 1));
            if (!ids) {
                return Unexpected(std::move(ids.error()));
            }
            return ok();
        });
        if (!written) {
            return Unexpected(from_catalog_error(written.error(), kWhereRefresh));
        }
        try {
            std::lock_guard lock(s.mutex);
            State::Source& source = s.sources.at(source_id);
            const std::optional<core::Digest256> old_hash = s.items.at(fresh.id).sha256;
            std::optional<core::Digest256> new_hash = fresh.sha256;
            if (!new_hash && outcome != catalog::SourceUpsertOutcome::Replaced) {
                new_hash = old_hash;
            }
            s.unindex_source(source_id, source);
            source.file_key = fresh_file;
            s.index_source(source_id, source);
            s.set_hash_membership(source_id, old_hash, new_hash);
            std::vector<ItemId> siblings;
            for (const ItemId id : source.items) {
                SourceItem& item = s.items.at(id);
                const SourceItem before = item;
                item.path = fresh.path;
                item.identity = fresh.identity;
                item.sha256 = new_hash;
                item.source_state = SourceState::Present;
                item.storage = observation.storage_kind;
                if (id == fresh.id) {
                    item.subimage_count = fresh.subimage_count;
                    item.probe = fresh.probe;
                    item.support = fresh.support;
                    ++item.revision;
                } else if (item.identity != before.identity || item.sha256 != before.sha256 ||
                           item.path != before.path || item.source_state != before.source_state ||
                           item.storage != before.storage) {
                    ++item.revision;
                    siblings.push_back(id);
                }
            }
            std::vector<ItemId> duplicates_changed;
            s.recompute_duplicates(old_hash, source_id, duplicates_changed);
            s.recompute_duplicates(new_hash, source_id, duplicates_changed);
            std::erase(duplicates_changed, fresh.id);
            siblings.insert(siblings.end(), duplicates_changed.begin(), duplicates_changed.end());
            s.publish_later(pending, LibraryChangeKind::Refreshed, {fresh.id});
            s.publish_later(pending, LibraryChangeKind::Updated, std::move(siblings));
        } catch (const std::bad_alloc&) {
            std::lock_guard lock(s.mutex);
            return Unexpected(s.reload_after_failure(kWhereRefresh).error());
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "refreshing a library item ran out of memory",
                    std::string(kWhereRefresh));
    }
    s.notify(pending);
    return ok();
}

Status CatalogLibrary::remove(std::span<const ItemId> ids) {
    State& s = *state_;
    State::Pending pending;
    try {
        std::lock_guard write(s.write_mutex);
        std::vector<ItemId> ordered;
        std::vector<catalog::ItemId> item_ids;
        std::vector<catalog::SourceId> whole_sources;
        {
            std::lock_guard lock(s.mutex);
            std::unordered_set<std::uint64_t> seen;
            std::unordered_map<std::int64_t, std::size_t> removed_per_source;
            for (const ItemId id : ids) {
                if (!s.items.contains(id)) {
                    return fail(ErrorCode::NotFound, std::format("no library item {}", id.value),
                                std::string(kWhereRemove));
                }
                if (seen.insert(id.value).second) {
                    ordered.push_back(id);
                    ++removed_per_source[s.item_source.at(id.value)];
                }
            }
            if (ordered.empty()) {
                return ok();
            }
            for (const auto& [source_id, count] : removed_per_source) {
                if (count == s.sources.at(source_id).items.size()) {
                    whole_sources.push_back(catalog::SourceId{source_id});
                }
            }
            for (const ItemId id : ordered) {
                const std::int64_t source_id = s.item_source.at(id.value);
                const bool whole = std::any_of(whole_sources.begin(), whole_sources.end(),
                                               [&](catalog::SourceId w) { return w.value == source_id; });
                if (!whole) {
                    item_ids.push_back(to_catalog_id(id));
                }
            }
        }
        Status written = s.store->write([&](catalog::WriteTransaction& tx) -> Status {
            if (!item_ids.empty()) {
                if (Status removed = tx.remove_items(item_ids); !removed) {
                    return removed;
                }
            }
            for (const catalog::SourceId source : whole_sources) {
                if (Status removed = tx.remove_source(source); !removed) {
                    return removed;
                }
            }
            return ok();
        });
        if (!written) {
            return Unexpected(from_catalog_error(written.error(), kWhereRemove));
        }
        try {
            std::lock_guard lock(s.mutex);
            std::vector<std::optional<core::Digest256>> hashes;
            std::vector<std::int64_t> affected;
            for (const ItemId id : ordered) {
                const std::int64_t source_id = s.item_source.at(id.value);
                State::Source& source = s.sources.at(source_id);
                hashes.push_back(s.items.at(id).sha256);
                affected.push_back(source_id);
                std::erase(source.items, id);
                s.items.erase(id);
                s.item_source.erase(id.value);
                if (source.items.empty()) {
                    s.set_hash_membership(source_id, hashes.back(), std::nullopt);
                    s.unindex_source(source_id, source);
                    s.sources.erase(source_id);
                }
            }
            std::vector<ItemId> duplicates_changed;
            for (std::size_t i = 0; i < hashes.size(); ++i) {
                if (hashes[i]) {
                    s.recompute_duplicates(hashes[i], 0, duplicates_changed);
                }
            }
            s.publish_later(pending, LibraryChangeKind::Removed, ordered);
            s.publish_later(pending, LibraryChangeKind::Updated, std::move(duplicates_changed));
        } catch (const std::bad_alloc&) {
            std::lock_guard lock(s.mutex);
            return Unexpected(s.reload_after_failure(kWhereRemove).error());
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "removing items ran out of memory", std::string(kWhereRemove));
    }
    s.notify(pending);
    return ok();
}

Subscription CatalogLibrary::subscribe(LibraryObserver observer) {
    return state_->observers.add(std::move(observer));
}

Status CatalogLibrary::set_content_hash(ItemId item, const core::Digest256& sha256,
                                        const core::FileIdentity& hashed) {
    State& s = *state_;
    State::Pending pending;
    try {
        std::lock_guard write(s.write_mutex);
        std::int64_t source_id = 0;
        {
            std::lock_guard lock(s.mutex);
            const auto found = s.items.find(item);
            if (found == s.items.end()) {
                return fail(ErrorCode::NotFound, std::format("no library item {}", item.value),
                            std::string(kWhereHash));
            }
            if (found->second.identity != hashed) {
                return fail(ErrorCode::SourceChanged,
                            "the hashed file is not the version of the file the library knows; re-read it",
                            std::string(kWhereHash));
            }
            if (found->second.sha256 == sha256) {
                return ok();
            }
            source_id = s.item_source.at(item.value);
        }
        Status written = s.store->write([&](catalog::WriteTransaction& tx) {
            return tx.set_source_hash(catalog::SourceId{source_id}, sha256, hashed);
        });
        if (!written) {
            return Unexpected(from_catalog_error(written.error(), kWhereHash));
        }
        try {
            std::lock_guard lock(s.mutex);
            State::Source& source = s.sources.at(source_id);
            const std::optional<core::Digest256> old_hash = s.items.at(item).sha256;
            s.set_hash_membership(source_id, old_hash, sha256);
            std::vector<ItemId> changed;
            for (const ItemId id : source.items) {
                SourceItem& member = s.items.at(id);
                member.sha256 = sha256;
                ++member.revision;
                changed.push_back(id);
            }
            s.recompute_duplicates(old_hash, source_id, changed);
            s.recompute_duplicates(sha256, source_id, changed);
            s.publish_later(pending, LibraryChangeKind::Updated, std::move(changed));
        } catch (const std::bad_alloc&) {
            std::lock_guard lock(s.mutex);
            return Unexpected(s.reload_after_failure(kWhereHash).error());
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "recording a content hash ran out of memory",
                    std::string(kWhereHash));
    }
    s.notify(pending);
    return ok();
}

Status CatalogLibrary::set_source_state(ItemId item, SourceState state) {
    if (state != SourceState::Present && state != SourceState::Missing && state != SourceState::Changed) {
        return fail(ErrorCode::InvalidArgument, "unknown source state", std::string(kWhereState));
    }
    State& s = *state_;
    State::Pending pending;
    try {
        std::lock_guard write(s.write_mutex);
        std::int64_t source_id = 0;
        {
            std::lock_guard lock(s.mutex);
            const auto found = s.items.find(item);
            if (found == s.items.end()) {
                return fail(ErrorCode::NotFound, std::format("no library item {}", item.value),
                            std::string(kWhereState));
            }
            if (found->second.source_state == state) {
                return ok();
            }
            source_id = s.item_source.at(item.value);
        }
        Status written = s.store->write([&](catalog::WriteTransaction& tx) {
            return tx.set_source_state(catalog::SourceId{source_id}, detail::to_catalog(state),
                                       catalog::current_time());
        });
        if (!written) {
            return Unexpected(from_catalog_error(written.error(), kWhereState));
        }
        std::lock_guard lock(s.mutex);
        std::vector<ItemId> changed;
        for (const ItemId id : s.sources.at(source_id).items) {
            SourceItem& member = s.items.at(id);
            if (member.source_state != state) {
                member.source_state = state;
                ++member.revision;
                changed.push_back(id);
            }
        }
        s.publish_later(pending, LibraryChangeKind::Updated, std::move(changed));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "recording a source state ran out of memory",
                    std::string(kWhereState));
    }
    s.notify(pending);
    return ok();
}

Status CatalogLibrary::relocate_source(ItemId item, const SourceRelocation& relocation) {
    if (relocation.path.empty() || !relocation.path.is_absolute()) {
        return fail(ErrorCode::InvalidArgument, "the new location is empty or relative",
                    std::string(kWhereRelocate));
    }
    if (relocation.import_mode == ImportMode::Copied && !relocation.origin_path) {
        return fail(ErrorCode::InvalidArgument, "a copy needs its origin", std::string(kWhereRelocate));
    }
    State& s = *state_;
    State::Pending pending;
    try {
        auto key = path_key(relocation.path, kWhereRelocate);
        if (!key) {
            return Unexpected(std::move(key.error()));
        }
        const std::string new_file = file_key(relocation.identity);
        std::string origin_key;
        if (relocation.import_mode == ImportMode::Copied) {
            auto origin = path_key(*relocation.origin_path, kWhereRelocate);
            if (!origin) {
                return Unexpected(std::move(origin.error()));
            }
            origin_key = std::move(*origin);
        }

        std::lock_guard write(s.write_mutex);
        std::int64_t source_id = 0;
        ImportMode mode = ImportMode::Referenced;
        std::optional<std::filesystem::path> origin;
        std::optional<core::FileIdentity> origin_identity;
        std::optional<core::Digest256> old_hash;
        {
            std::lock_guard lock(s.mutex);
            const auto found = s.items.find(item);
            if (found == s.items.end()) {
                return fail(ErrorCode::NotFound, std::format("no library item {}", item.value),
                            std::string(kWhereRelocate));
            }
            const SourceItem& current = found->second;
            // Verified by content (both hashes known and equal) or by identity (the library's own
            // file version, found at another path); a contradicting hash is never accepted.
            if (current.sha256 && relocation.sha256 && *current.sha256 != *relocation.sha256) {
                return fail(ErrorCode::InvalidArgument,
                            "the new file has other content than the library's file",
                            "app.library.relocate.content_mismatch");
            }
            const bool by_content = current.sha256 && relocation.sha256;
            if (!by_content && current.identity != relocation.identity) {
                return fail(ErrorCode::InvalidArgument,
                            current.sha256 ? "the new file was not hashed and is another file version, so it "
                                             "cannot be verified"
                                           : "the file was never hashed and the new file is another file "
                                             "version, so it cannot be verified",
                            "app.library.relocate.unverified");
            }
            old_hash = current.sha256;
            source_id = s.item_source.at(item.value);
            if (const auto owner = s.by_path.find(*key);
                owner != s.by_path.end() && owner->second != source_id) {
                return fail(ErrorCode::AlreadyExists, "another library file has that path",
                            std::string(kWhereRelocate));
            }
            if (const auto owner = s.by_file.find(new_file);
                !new_file.empty() && owner != s.by_file.end() && owner->second != source_id) {
                return fail(ErrorCode::AlreadyExists, "the new file is another library file (a hard link)",
                            std::string(kWhereRelocate));
            }
            mode = relocation.import_mode.value_or(current.import_mode);
            origin = relocation.import_mode ? relocation.origin_path : current.origin_path;
            origin_identity = relocation.import_mode ? relocation.origin_identity : current.origin_identity;
            if (mode != ImportMode::Copied) {
                origin.reset();
                origin_identity.reset();
            }
        }
        const std::optional<core::Digest256> new_hash = old_hash ? old_hash : relocation.sha256;
        const catalog::Timestamp now = catalog::current_time();
        Status written = s.store->write([&](catalog::WriteTransaction& tx) -> Status {
            catalog::RelinkRequest request;
            request.new_path = relocation.path;
            request.identity = relocation.identity;
            request.sha256 = relocation.sha256;
            request.storage_kind = relocation.storage;
            request.seen_at = now;
            auto relinked = tx.relink_source(catalog::SourceId{source_id}, request);
            if (!relinked) {
                return Unexpected(std::move(relinked.error()));
            }
            if (relocation.import_mode) {
                catalog::SourceObservation observation;
                observation.path = relocation.path;
                observation.identity = relocation.identity;
                observation.sha256 = relocation.sha256;
                observation.import_mode = detail::to_catalog(mode);
                observation.origin_path = origin;
                observation.origin_identity = origin_identity;
                observation.storage_kind = relocation.storage;
                observation.seen_at = now;
                auto upserted = tx.upsert_source(observation);
                if (!upserted) {
                    return Unexpected(std::move(upserted.error()));
                }
            }
            return ok();
        });
        if (!written) {
            return Unexpected(from_catalog_error(written.error(), kWhereRelocate));
        }
        try {
            std::lock_guard lock(s.mutex);
            State::Source& source = s.sources.at(source_id);
            s.unindex_source(source_id, source);
            source.path_key = *key;
            source.file_key = new_file;
            if (relocation.import_mode) {
                source.origin_key = origin_key;
                source.origin_version_key = origin_identity ? file_key(*origin_identity) : std::string();
            }
            s.index_source(source_id, source);
            s.set_hash_membership(source_id, old_hash, new_hash);
            std::vector<ItemId> changed;
            for (const ItemId id : source.items) {
                SourceItem& member = s.items.at(id);
                member.path = relocation.path;
                member.identity = relocation.identity;
                member.storage = relocation.storage;
                member.source_state = SourceState::Present;
                member.import_mode = mode;
                member.origin_path = origin;
                member.origin_identity = origin_identity;
                member.sha256 = new_hash;
                ++member.revision;
                changed.push_back(id);
            }
            if (new_hash != old_hash) {
                s.recompute_duplicates(new_hash, source_id, changed);
            }
            s.publish_later(pending, LibraryChangeKind::Updated, std::move(changed));
        } catch (const std::bad_alloc&) {
            std::lock_guard lock(s.mutex);
            return Unexpected(s.reload_after_failure(kWhereRelocate).error());
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "relocating a file ran out of memory",
                    std::string(kWhereRelocate));
    }
    s.notify(pending);
    return ok();
}

std::optional<ItemId> CatalogLibrary::find_copied_from(const std::filesystem::path& origin,
                                                       const core::FileIdentity& identity) const {
    auto key = catalog::make_path_key(origin);
    const std::string version = file_key(identity);
    std::lock_guard lock(state_->mutex);
    // A copy of exactly this original version, under any path or drive letter.
    if (!version.empty()) {
        if (const auto found = state_->by_origin_version.find(version);
            found != state_->by_origin_version.end()) {
            if (const SourceItem* item = state_->first_item(state_->sources.at(found->second))) {
                return item->id;
            }
        }
    }
    // A candidate: a copy of the file at this path with this size.
    if (!key) {
        return std::nullopt;
    }
    const auto found = state_->by_origin.find(*key);
    if (found == state_->by_origin.end()) {
        return std::nullopt;
    }
    const SourceItem* item = state_->first_item(state_->sources.at(found->second));
    if (item == nullptr || item->identity.size_bytes != identity.size_bytes) {
        return std::nullopt;
    }
    return item->id;
}

Expected<std::uint64_t> CatalogLibrary::begin_import(const ImportJournalStart& start) {
    try {
        catalog::ImportStart record;
        record.started_at = catalog::current_time();
        for (const ImportJournalRoot& root : start.roots) {
            record.roots.push_back(catalog::ImportRoot{root.path, root.include_subfolders});
        }
        record.options_json = detail::encode_import_options(start);
        auto id =
            state_->store->transact([&](catalog::WriteTransaction& tx) { return tx.begin_import(record); });
        if (!id) {
            return Unexpected(from_catalog_error(id.error(), kWhereJournal));
        }
        return static_cast<std::uint64_t>(id->value);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "recording an import ran out of memory",
                    std::string(kWhereJournal));
    }
}

Status CatalogLibrary::finish_import(std::uint64_t record, const ImportJournalFinish& finish) {
    if (record == 0) {
        return ok();
    }
    catalog::ImportFinish result;
    result.finished_at = catalog::current_time();
    result.outcome = detail::to_catalog(finish.outcome);
    result.counts.files_seen = finish.files_seen;
    result.counts.added = finish.added;
    result.counts.refreshed = finish.refreshed;
    result.counts.unchanged = finish.unchanged;
    result.counts.duplicates = finish.duplicates;
    result.counts.skipped = finish.skipped;
    result.counts.failed = finish.failed;
    Status written = state_->store->write([&](catalog::WriteTransaction& tx) {
        return tx.finish_import(catalog::ImportId{static_cast<std::int64_t>(record)}, result);
    });
    if (!written) {
        return Unexpected(from_catalog_error(written.error(), kWhereJournal));
    }
    return ok();
}

}  // namespace lucent::app
