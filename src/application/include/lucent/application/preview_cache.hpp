#pragma once

// Bounded in-memory cache of finished previews (blueprint section 7, "Cache previews ... under a
// user-configurable size limit ... Cached data is disposable").
//
// Least-recently-used eviction by bytes: an insertion that takes the total above the capacity
// evicts the least recently used entries until it fits. An entry larger than the whole capacity
// is not stored. find() and insert() mark an entry most recently used. Values are shared
// immutable previews, so an evicted preview stays valid for holders of its pointer.
//
// Thread-safety: all members are safe to call concurrently.

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <lucent/application/preview.hpp>

namespace lucent::app {

struct PreviewCacheStats {
    std::uint64_t capacity_bytes = 0;
    std::uint64_t bytes = 0;
    std::uint64_t entries = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t insertions = 0;
    std::uint64_t evictions = 0;
    std::uint64_t rejected = 0;  // insertions larger than the capacity
};

class PreviewCache {
public:
    explicit PreviewCache(std::uint64_t capacity_bytes);

    PreviewCache(const PreviewCache&) = delete;
    PreviewCache& operator=(const PreviewCache&) = delete;

    // The cached preview (now most recently used), or nullptr; counts a hit or a miss.
    [[nodiscard]] std::shared_ptr<const PreviewImage> find(const PreviewKey& key);

    // Stores `preview` under `key` (replacing an existing entry), charging preview->byte_size()
    // bytes, then evicts least recently used entries until the total fits. Returns false, storing
    // nothing, for a null preview or one larger than the capacity.
    bool insert(const PreviewKey& key, std::shared_ptr<const PreviewImage> preview);

    // Drops every entry of `item`.
    void erase_item(ItemId item);
    void clear();

    // Changes the capacity, evicting as needed.
    void set_capacity(std::uint64_t capacity_bytes);

    [[nodiscard]] PreviewCacheStats stats() const;

private:
    struct Entry {
        PreviewKey key;
        std::shared_ptr<const PreviewImage> preview;
        std::uint64_t bytes = 0;
    };
    using List = std::list<Entry>;  // front = most recently used

    void evict_to(std::uint64_t capacity);  // requires mutex_

    mutable std::mutex mutex_;
    List lru_;
    std::unordered_map<PreviewKey, List::iterator, PreviewKeyHash> index_;
    PreviewCacheStats stats_;
};

}  // namespace lucent::app
