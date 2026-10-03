#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/application/thumbnail_cache.hpp>
#include <lucent/core/storage.hpp>

#include "app_test_support.hpp"

namespace lucent::app {
namespace {

using test_support::ScratchDir;

ThumbnailKey make_key(std::uint8_t id) {
    ThumbnailKey key;
    key.source_sha256.fill(0x5A);
    key.source_sha256[0] = id;
    return key;
}

// A JPEG-looking payload (SOI ... EOI) whose bytes depend on `seed`; `size` >= 5.
Thumbnail make_thumbnail(std::uint32_t seed, std::size_t size = 1000) {
    Thumbnail thumbnail;
    thumbnail.width = 160 + static_cast<std::int32_t>(seed % 7);
    thumbnail.height = 120;
    thumbnail.jpeg.resize(size);
    std::mt19937 engine(seed);
    for (auto& b : thumbnail.jpeg) {
        b = static_cast<std::byte>(engine() & 0xFF);
    }
    thumbnail.jpeg[0] = std::byte{0xFF};
    thumbnail.jpeg[1] = std::byte{0xD8};
    thumbnail.jpeg[2] = std::byte{0xFF};
    thumbnail.jpeg[size - 2] = std::byte{0xFF};
    thumbnail.jpeg[size - 1] = std::byte{0xD9};
    return thumbnail;
}

bool same(const Thumbnail& a, const Thumbnail& b) {
    return a.width == b.width && a.height == b.height && a.jpeg == b.jpeg;
}

// Entry file size for a payload of `payload` bytes.
constexpr std::uint64_t entry_size(std::uint64_t payload) {
    return kThumbnailHeaderBytes + payload;
}

ThumbnailCacheOptions options_for(const std::filesystem::path& dir, std::uint64_t capacity,
                                  std::uint64_t max_entry = entry_size(4096)) {
    ThumbnailCacheOptions options;
    options.directory = dir;
    options.capacity_bytes = capacity;
    options.max_entry_bytes = max_entry;
    return options;
}

std::unique_ptr<ThumbnailCache> open_cache(const ThumbnailCacheOptions& options) {
    auto cache = ThumbnailCache::open(options);
    EXPECT_TRUE(cache) << (cache ? "" : cache.error().describe());
    return cache ? std::move(*cache) : nullptr;
}

std::vector<char> read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in), {});
}

void write_all(const std::filesystem::path& path, const std::vector<char>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Entry files and temporary files under the cache directory.
struct DiskCount {
    std::uint64_t entries = 0;
    std::uint64_t bytes = 0;
    std::uint64_t temporaries = 0;
};
DiskCount count_on_disk(const std::filesystem::path& dir) {
    DiskCount count;
    if (!std::filesystem::exists(dir)) {
        return count;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (core::is_temporary_file_name(entry.path().filename())) {
            ++count.temporaries;
        } else if (entry.path().extension() == L".lthumb") {
            ++count.entries;
            count.bytes += entry.file_size();
        }
    }
    return count;
}

// ---- basics -----------------------------------------------------------------------------------

TEST(ThumbnailCache, StoresAndFindsEntries) {
    ScratchDir scratch;
    const auto dir = scratch.path() / L"cache Ü" / L"thumbnails";  // created by open()
    auto cache = open_cache(options_for(dir, 1 << 20));
    ASSERT_TRUE(cache);
    EXPECT_EQ(cache->directory(), dir);

    const Thumbnail a = make_thumbnail(1);
    EXPECT_FALSE(cache->find(make_key(1)));
    ASSERT_TRUE(cache->insert(make_key(1), a));
    EXPECT_TRUE(std::filesystem::exists(cache->entry_path(make_key(1))));
    EXPECT_EQ(cache->entry_path(make_key(1)).extension(), L".lthumb");

    auto found = cache->find(make_key(1));
    ASSERT_TRUE(found);
    EXPECT_TRUE(same(*found, a));

    const ThumbnailCacheStats stats = cache->stats();
    EXPECT_EQ(stats.entries, 1u);
    EXPECT_EQ(stats.bytes, entry_size(a.jpeg.size()));
    EXPECT_EQ(stats.hits, 1u);
    EXPECT_EQ(stats.misses, 1u);
    EXPECT_EQ(stats.insertions, 1u);
    EXPECT_EQ(count_on_disk(dir).temporaries, 0u);
}

TEST(ThumbnailCache, EveryKeyComponentSelectsADifferentEntry) {
    ScratchDir scratch;
    auto cache = open_cache(options_for(scratch.path(), 1 << 20));
    ASSERT_TRUE(cache);
    const ThumbnailKey base = make_key(7);
    ASSERT_TRUE(cache->insert(base, make_thumbnail(7)));

    std::vector<ThumbnailKey> variants(6, base);
    variants[0].source_sha256[31] ^= 1;
    variants[1].subimage = 1;
    variants[2].generator = ThumbnailGenerator::LucentRender;
    variants[3].generator_version = 2;
    variants[4].size_class = ThumbnailSizeClass::Large;
    variants[5].variant[0] = 1;
    for (const ThumbnailKey& key : variants) {
        EXPECT_NE(cache->entry_path(key), cache->entry_path(base));
        EXPECT_FALSE(cache->find(key));
    }
    EXPECT_TRUE(cache->find(base));
    EXPECT_EQ(cache->stats().corrupt_entries, 0u);
}

TEST(ThumbnailCache, ReplacingAnEntryKeepsTheAccountingExact) {
    ScratchDir scratch;
    auto cache = open_cache(options_for(scratch.path(), 1 << 20));
    ASSERT_TRUE(cache);
    ASSERT_TRUE(cache->insert(make_key(1), make_thumbnail(1, 1000)));
    const Thumbnail replacement = make_thumbnail(2, 3000);
    ASSERT_TRUE(cache->insert(make_key(1), replacement));
    EXPECT_EQ(cache->stats().entries, 1u);
    EXPECT_EQ(cache->stats().bytes, entry_size(3000));
    auto found = cache->find(make_key(1));
    ASSERT_TRUE(found);
    EXPECT_TRUE(same(*found, replacement));
    cache->erase(make_key(1));
    EXPECT_EQ(cache->stats().bytes, 0u);
    EXPECT_FALSE(std::filesystem::exists(cache->entry_path(make_key(1))));
    cache->erase(make_key(1));  // absent: no-op
}

TEST(ThumbnailCache, RejectsInvalidInsertsAndOptions) {
    ScratchDir scratch;
    auto cache = open_cache(options_for(scratch.path(), entry_size(8192), entry_size(4096)));
    ASSERT_TRUE(cache);

    Thumbnail bad = make_thumbnail(1);
    bad.width = 0;
    EXPECT_EQ(cache->insert(make_key(1), bad).error().code, ErrorCode::InvalidArgument);
    bad = make_thumbnail(1);
    bad.height = 65536;
    EXPECT_EQ(cache->insert(make_key(1), bad).error().code, ErrorCode::InvalidArgument);
    bad = make_thumbnail(1);
    bad.jpeg[1] = std::byte{0x00};  // not a JPEG
    EXPECT_EQ(cache->insert(make_key(1), bad).error().code, ErrorCode::InvalidArgument);
    bad.jpeg.clear();
    EXPECT_EQ(cache->insert(make_key(1), bad).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(cache->insert(make_key(1), make_thumbnail(1, 4097)).error().code, ErrorCode::LimitExceeded);
    ASSERT_TRUE(cache->insert(make_key(1), make_thumbnail(1, 4096)));  // exactly the limit
    EXPECT_EQ(cache->stats().rejected, 5u);

    EXPECT_EQ(ThumbnailCache::open(options_for({}, 1 << 20)).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(ThumbnailCache::open(options_for(scratch.path(), 100, 200)).error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(ThumbnailCache::open(options_for(scratch.path(), 1 << 20, kThumbnailHeaderBytes)).error().code,
              ErrorCode::InvalidArgument);
}

TEST(ThumbnailCache, OpenFailsWhenTheDirectoryIsAFile) {
    ScratchDir scratch;
    const auto file = scratch.write_text("occupied", "x");
    auto cache = ThumbnailCache::open(options_for(file, 1 << 20));
    ASSERT_FALSE(cache);
    EXPECT_EQ(cache.error().code, ErrorCode::AlreadyExists);
}

// ---- size limit and eviction ------------------------------------------------------------------

TEST(ThumbnailCache, EvictsTheLeastRecentlyUsedEntries) {
    ScratchDir scratch;
    constexpr std::size_t kPayload = 1000;
    auto cache = open_cache(options_for(scratch.path(), 3 * entry_size(kPayload), entry_size(kPayload)));
    ASSERT_TRUE(cache);
    for (std::uint8_t id = 1; id <= 3; ++id) {
        ASSERT_TRUE(cache->insert(make_key(id), make_thumbnail(id, kPayload)));
    }
    ASSERT_TRUE(cache->find(make_key(1)));  // 1 becomes most recent; 2 is now the oldest
    ASSERT_TRUE(cache->insert(make_key(4), make_thumbnail(4, kPayload)));

    EXPECT_FALSE(std::filesystem::exists(cache->entry_path(make_key(2))));
    EXPECT_FALSE(cache->find(make_key(2)));
    EXPECT_TRUE(cache->find(make_key(1)));
    EXPECT_TRUE(cache->find(make_key(3)));
    EXPECT_TRUE(cache->find(make_key(4)));
    const auto stats = cache->stats();
    EXPECT_EQ(stats.evictions, 1u);
    EXPECT_EQ(stats.entries, 3u);
    EXPECT_LE(stats.bytes, stats.capacity_bytes);
    EXPECT_EQ(count_on_disk(scratch.path()).bytes, stats.bytes);

    // Shrinking evicts immediately, least recently used first (4 was used last).
    cache->set_capacity(entry_size(kPayload));
    EXPECT_EQ(cache->stats().entries, 1u);
    EXPECT_TRUE(cache->find(make_key(4)));
    EXPECT_EQ(count_on_disk(scratch.path()).entries, 1u);
    cache->set_capacity(0);  // clamped to max_entry_bytes
    EXPECT_EQ(cache->stats().capacity_bytes, entry_size(kPayload));
}

TEST(ThumbnailCache, ReopenRebuildsTheIndexAndHonoursRecency) {
    ScratchDir scratch;
    constexpr std::size_t kPayload = 500;
    {
        auto cache = open_cache(options_for(scratch.path(), 1 << 20, entry_size(kPayload)));
        ASSERT_TRUE(cache);
        for (std::uint8_t id = 1; id <= 4; ++id) {
            ASSERT_TRUE(cache->insert(make_key(id), make_thumbnail(id, kPayload)));
        }
    }
    // Make the recency order explicit: 3 oldest, then 1, 4, 2.
    const auto now = std::filesystem::file_time_type::clock::now();
    auto cache = open_cache(options_for(scratch.path(), 1 << 20, entry_size(kPayload)));
    ASSERT_TRUE(cache);
    int age = 4;
    for (const int id : {3, 1, 4, 2}) {
        std::filesystem::last_write_time(cache->entry_path(make_key(static_cast<std::uint8_t>(id))),
                                         now - std::chrono::hours(age--));
    }
    cache.reset();

    cache = open_cache(options_for(scratch.path(), 2 * entry_size(kPayload), entry_size(kPayload)));
    ASSERT_TRUE(cache);
    EXPECT_EQ(cache->stats().entries, 2u);
    EXPECT_EQ(cache->stats().evictions, 2u);
    EXPECT_FALSE(cache->find(make_key(3)));
    EXPECT_FALSE(cache->find(make_key(1)));
    auto found = cache->find(make_key(2));
    ASSERT_TRUE(found);
    EXPECT_TRUE(same(*found, make_thumbnail(2, kPayload)));
    EXPECT_TRUE(cache->find(make_key(4)));

    // A hit refreshes the file's timestamp, so it outlives older entries across sessions.
    EXPECT_GT(std::filesystem::last_write_time(cache->entry_path(make_key(4))),
              now - std::chrono::minutes(5));
}

TEST(ThumbnailCache, OpenRemovesStaleTemporariesButNeverUnknownFiles) {
    ScratchDir scratch;
    auto cache = open_cache(options_for(scratch.path(), 1 << 20));
    ASSERT_TRUE(cache);
    ASSERT_TRUE(cache->insert(make_key(1), make_thumbnail(1)));
    const auto shard = cache->entry_path(make_key(1)).parent_path();
    cache.reset();

    const auto temporary = shard / L".lucent-0123456789abcdef.tmp";
    const auto foreign = shard / L"notes.txt";
    const auto stray = scratch.path() / L"v1" / L"zz" / (std::wstring(64, L'a') + L".lthumb");
    write_all(temporary, {'p', 'a', 'r', 't'});
    write_all(foreign, {'k', 'e', 'e', 'p'});
    write_all(stray, {'x'});

    cache = open_cache(options_for(scratch.path(), 1 << 20));
    ASSERT_TRUE(cache);
    EXPECT_FALSE(std::filesystem::exists(temporary));
    EXPECT_TRUE(std::filesystem::exists(foreign));
    EXPECT_TRUE(std::filesystem::exists(stray));  // not in a valid shard: not ours
    EXPECT_EQ(cache->stats().entries, 1u);
}

// ---- corruption -------------------------------------------------------------------------------

TEST(ThumbnailCache, CorruptEntriesAreMissesAndAreDeleted) {
    ScratchDir scratch;
    auto cache = open_cache(options_for(scratch.path(), 1 << 20));
    ASSERT_TRUE(cache);

    using Mutation = std::function<void(std::vector<char>&)>;
    const std::vector<std::pair<const char*, Mutation>> mutations = {
        {"truncated payload", [](std::vector<char>& b) { b.resize(b.size() - 10); }},
        {"truncated header", [](std::vector<char>& b) { b.resize(kThumbnailHeaderBytes - 1); }},
        {"empty file", [](std::vector<char>& b) { b.clear(); }},
        {"flipped payload bit", [](std::vector<char>& b) { b[b.size() / 2] ^= 0x10; }},
        {"bad magic", [](std::vector<char>& b) { b[0] = 'X'; }},
        {"newer format version", [](std::vector<char>& b) { b[4] = 2; }},
        {"bad dimensions",
         [](std::vector<char>& b) {
             b[8 + 75] = 0;  // width (little-endian) becomes 0
             b[8 + 76] = 0;
         }},
        {"payload not JPEG", [](std::vector<char>& b) { b[kThumbnailHeaderBytes + 1] = 0; }},
        {"appended garbage", [](std::vector<char>& b) { b.push_back('!'); }},
    };
    std::uint64_t expected_corrupt = 0;
    for (const auto& [what, mutate] : mutations) {
        ASSERT_TRUE(cache->insert(make_key(1), make_thumbnail(1)));
        const auto path = cache->entry_path(make_key(1));
        std::vector<char> bytes = read_all(path);
        mutate(bytes);
        write_all(path, bytes);

        EXPECT_FALSE(cache->find(make_key(1))) << what;
        EXPECT_FALSE(std::filesystem::exists(path)) << what;
        EXPECT_EQ(cache->stats().corrupt_entries, ++expected_corrupt) << what;
        EXPECT_EQ(cache->stats().entries, 0u) << what;
    }
}

TEST(ThumbnailCache, AnEntryUnderTheWrongNameIsRejected) {
    ScratchDir scratch;
    auto cache = open_cache(options_for(scratch.path(), 1 << 20));
    ASSERT_TRUE(cache);
    ASSERT_TRUE(cache->insert(make_key(1), make_thumbnail(1)));
    // A valid entry of key 1 copied to key 2's location must not be served for key 2.
    write_all(cache->entry_path(make_key(2)), read_all(cache->entry_path(make_key(1))));
    EXPECT_FALSE(cache->find(make_key(2)));
    EXPECT_FALSE(std::filesystem::exists(cache->entry_path(make_key(2))));
    EXPECT_TRUE(cache->find(make_key(1)));
}

TEST(ThumbnailCache, OversizedFilesAreTreatedAsCorrupt) {
    ScratchDir scratch;
    auto cache = open_cache(options_for(scratch.path(), 1 << 20, entry_size(1000)));
    ASSERT_TRUE(cache);
    write_all(cache->entry_path(make_key(1)), std::vector<char>(entry_size(1000) + 1, 'x'));
    EXPECT_FALSE(cache->find(make_key(1)));
    EXPECT_FALSE(std::filesystem::exists(cache->entry_path(make_key(1))));
    EXPECT_EQ(cache->stats().corrupt_entries, 1u);
}

// ---- deleted directory and sharing --------------------------------------------------------------

TEST(ThumbnailCache, SurvivesDeletionOfItsDirectoryWhileOpen) {
    ScratchDir scratch;
    const auto dir = scratch.path() / L"thumbnails";
    auto cache = open_cache(options_for(dir, 1 << 20));
    ASSERT_TRUE(cache);
    for (std::uint8_t id = 1; id <= 3; ++id) {
        ASSERT_TRUE(cache->insert(make_key(id), make_thumbnail(id)));
    }

    std::filesystem::remove_all(dir);  // the user empties the cache folder

    EXPECT_FALSE(cache->find(make_key(1)));                      // a miss, not an error
    ASSERT_TRUE(cache->insert(make_key(4), make_thumbnail(4)));  // recreates the directory
    EXPECT_TRUE(cache->find(make_key(4)));
    EXPECT_EQ(cache->stats().entries, 1u);  // the vanished entries are no longer counted
    EXPECT_EQ(cache->stats().bytes, entry_size(1000));
    EXPECT_FALSE(cache->find(make_key(2)));
    EXPECT_FALSE(cache->find(make_key(3)));

    // The application starts again after the cache was deleted (blueprint section 8).
    cache.reset();
    std::filesystem::remove_all(scratch.path());
    cache = open_cache(options_for(dir, 1 << 20));
    ASSERT_TRUE(cache);
    EXPECT_TRUE(std::filesystem::is_directory(dir));
    EXPECT_EQ(cache->stats().entries, 0u);
}

TEST(ThumbnailCache, ClearRemovesEntriesOfEveryInstance) {
    ScratchDir scratch;
    auto first = open_cache(options_for(scratch.path(), 1 << 20));
    auto second = open_cache(options_for(scratch.path(), 1 << 20));
    ASSERT_TRUE(first && second);
    ASSERT_TRUE(first->insert(make_key(1), make_thumbnail(1)));
    ASSERT_TRUE(second->insert(make_key(2), make_thumbnail(2)));

    // Instances sharing a directory see each other's entries.
    auto shared = second->find(make_key(1));
    ASSERT_TRUE(shared);
    EXPECT_TRUE(same(*shared, make_thumbnail(1)));
    write_all(first->entry_path(make_key(1)).parent_path() / L".lucent-00000000000000aa.tmp", {'t'});

    ASSERT_TRUE(first->clear());
    EXPECT_EQ(first->stats().entries, 0u);
    EXPECT_EQ(count_on_disk(scratch.path()).entries, 0u);
    EXPECT_EQ(count_on_disk(scratch.path()).temporaries, 0u);
    EXPECT_FALSE(second->find(make_key(2)));
    EXPECT_EQ(second->stats().entries, 1u);  // key 1 (indexed by its hit) went with its miss
    EXPECT_FALSE(second->find(make_key(1)));
    EXPECT_EQ(second->stats().entries, 0u);
}

// ---- concurrency ------------------------------------------------------------------------------

TEST(ThumbnailCache, ConcurrentUseStaysConsistentAndWithinTheLimit) {
    ScratchDir scratch;
    constexpr int kKeys = 24;
    constexpr int kThreads = 6;
    constexpr int kOperations = 300;
    constexpr std::size_t kPayload = 2000;
    const std::uint64_t capacity = 8 * entry_size(kPayload);
    auto cache = open_cache(options_for(scratch.path(), capacity, entry_size(kPayload)));
    ASSERT_TRUE(cache);

    std::atomic<int> wrong{0};
    std::atomic<int> failures{0};
    std::mutex first_failure_mutex;
    std::string first_failure;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937 engine(static_cast<std::uint32_t>(1000 + t));  // fixed seeds
            for (int i = 0; i < kOperations; ++i) {
                const auto id = static_cast<std::uint8_t>(engine() % kKeys);
                switch (engine() % 6) {
                    case 0:
                    case 1:
                        if (auto status = cache->insert(make_key(id), make_thumbnail(id, kPayload));
                            !status) {
                            if (failures++ == 0) {
                                const std::lock_guard lock(first_failure_mutex);
                                first_failure = status.error().describe();
                            }
                        }
                        break;
                    case 5:
                        cache->erase(make_key(id));
                        break;
                    default:
                        if (auto found = cache->find(make_key(id));
                            found && !same(*found, make_thumbnail(id, kPayload))) {
                            ++wrong;
                        }
                        break;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(wrong.load(), 0);
    EXPECT_EQ(failures.load(), 0) << first_failure;
    const auto stats = cache->stats();
    EXPECT_LE(stats.bytes, capacity);
    EXPECT_EQ(stats.corrupt_entries, 0u);
    const DiskCount disk = count_on_disk(scratch.path());
    EXPECT_LE(disk.bytes, capacity);
    EXPECT_LE(disk.entries, stats.entries);
    EXPECT_EQ(disk.temporaries, 0u);
    for (int id = 0; id < kKeys; ++id) {
        if (auto found = cache->find(make_key(static_cast<std::uint8_t>(id)))) {
            EXPECT_TRUE(same(*found, make_thumbnail(static_cast<std::uint32_t>(id), kPayload)));
        }
    }
}

}  // namespace
}  // namespace lucent::app
