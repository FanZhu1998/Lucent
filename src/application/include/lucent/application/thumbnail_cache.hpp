#pragma once

// Bounded on-disk cache of small JPEG thumbnails and previews (blueprint section 7, "Memory and
// scheduling": disposable caches under a size limit, keyed by source hash and versions; section 8:
// the per-user cache directory holds disposable thumbnails and the application must start after
// cache deletion).
//
// What is stored
//   Opaque JPEG bytes supplied by the caller, plus the pixel size. The cache never decodes or
//   re-encodes them. W4 stores the camera's embedded JPEG reduced to 1024 pixels
//   (ThumbnailGenerator::EmbeddedJpeg, app::camera_thumbnail_entry in source_maintenance.hpp:
//   about 100 to 250 KB per photo, where many cameras' full-size previews take several MB); it is
//   what the contact sheet shows ("labeled until replaced by Lucent's own render", section 2).
//   Lucent's own renders (ThumbnailGenerator::LucentRender) are W6's. The only content check is
//   the JPEG SOI marker (FF D8 FF) on insert and read.
//
// Keys
//   source SHA-256 + subimage + generator kind + generator version + size class + a variant digest
//   (hash of whatever else determines the pixels: recipe revision, decoder build, operation
//   versions; all zero when nothing else applies). The key is hashed (SHA-256) into the entry file
//   name and also stored inside the entry, so a misplaced or colliding file is detected. Bump
//   generator_version whenever the bytes a generator produces for the same inputs change.
//
// Layout and durability
//   <directory>\v1\<first two hex digits>\<64 hex digits>.lthumb. An entry file is a 131-byte
//   little-endian header (magic "LTHB", format version 1, the serialised key, width, height,
//   payload size and payload SHA-256) followed by the JPEG bytes. Entries are published with
//   core::write_file_atomic (temporary sibling + rename) without flushing: after a power loss an
//   entry may be truncated or empty, which the payload hash detects. A future format uses a new
//   "v2" directory; files of other versions are never read.
//
// Size limit and eviction
//   The index (entry name -> size, recency) lives in memory and is rebuilt at open() by
//   enumerating the directory (one FindFirstFile pass, no file is opened); the initial recency
//   order is the files' last write times, newest most recent. A hit updates the in-memory order
//   and, once per entry per process, the file's last write time, so recency survives restarts at
//   session granularity. Whenever the indexed total exceeds capacity_bytes the least recently used
//   entries are deleted. Accounting uses file sizes (header included), not allocation units.
//
// Robustness
//   Every problem with an entry is a miss, never an error for the caller: a missing file is a
//   miss; an entry that fails validation (truncated, wrong magic/version/key, bad hash, oversized,
//   not JPEG) is deleted and counted in corrupt_entries. If the whole directory is deleted while
//   the cache is open, finds miss and the next insert recreates the directory and resets the
//   index. Stale temporary files from crashes are removed at open(). Several processes may share a
//   directory without corrupting it; each enforces the limit on the entries it knows about.
//   Replacing an entry that another thread or process is touching at that instant can fail
//   transiently (sharing violation / access denied); insert retries for up to ~60 ms.
//
// Thread-safety: all members are safe to call concurrently. Entry reads and writes run outside
// the internal lock; only deletions (eviction, corrupt entries, clear) and the once-per-entry
// timestamp refresh run under it.

#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <lucent/core/error.hpp>
#include <lucent/core/hash.hpp>

namespace lucent::app {

// What produced a thumbnail. Persisted in entry keys: never renumber, append only.
enum class ThumbnailGenerator : std::uint16_t {
    EmbeddedJpeg = 1,  // the camera's embedded preview JPEG (reduced; see the generator version)
    LucentRender = 2,  // Lucent's own render of the source with a recipe (see variant)
};

// Nominal size of a cached image (long edge). Persisted in entry keys: never renumber.
enum class ThumbnailSizeClass : std::uint8_t {
    Grid = 1,     // contact-sheet cell, long edge up to about 320 px
    Large = 2,    // filmstrip / hover, up to about 1024 px
    Preview = 3,  // full-window browse preview, up to about 2560 px
};

struct ThumbnailKey {
    core::Digest256 source_sha256{};  // content identity of the source file
    std::uint32_t subimage = 0;
    ThumbnailGenerator generator = ThumbnailGenerator::EmbeddedJpeg;
    std::uint32_t generator_version = 1;
    ThumbnailSizeClass size_class = ThumbnailSizeClass::Grid;
    core::Digest256 variant{};  // recipe/decoder/operation versions digest; zero when unused

    friend bool operator==(const ThumbnailKey&, const ThumbnailKey&) = default;
};

// A cached image: JPEG bytes and their pixel size (as declared by the producer).
struct Thumbnail {
    std::int32_t width = 0;   // 1..65535
    std::int32_t height = 0;  // 1..65535
    std::vector<std::byte> jpeg;
};

struct ThumbnailCacheOptions {
    // Cache root, e.g. core::AppDirectories::cache / "thumbnails". Created (with parents) by
    // open() and again whenever it disappears.
    std::filesystem::path directory;
    // Total size of all entry files. Must be >= max_entry_bytes.
    std::uint64_t capacity_bytes = std::uint64_t{256} << 20;  // 256 MiB
    // Largest single entry file (header + JPEG). Larger inserts are rejected and larger files
    // found on disk are treated as corrupt. Must be > kThumbnailHeaderBytes.
    std::uint64_t max_entry_bytes = std::uint64_t{8} << 20;  // 8 MiB
};

// Size of the entry header that precedes the JPEG bytes.
inline constexpr std::uint64_t kThumbnailHeaderBytes = 131;

struct ThumbnailCacheStats {
    std::uint64_t capacity_bytes = 0;
    std::uint64_t bytes = 0;    // indexed total
    std::uint64_t entries = 0;  // indexed entries
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t insertions = 0;
    std::uint64_t evictions = 0;
    std::uint64_t corrupt_entries = 0;  // invalid entries found (and deleted)
    std::uint64_t write_failures = 0;   // inserts that failed with an I/O error
    std::uint64_t rejected = 0;         // inserts refused by validation or size limits
};

class ThumbnailCache {
public:
    // Creates the directory if needed, removes stale temporary files, rebuilds the index and
    // evicts down to the capacity. Cost: one directory enumeration. Errors: InvalidArgument for
    // an empty directory or inconsistent limits; errors of core::create_directory_tree when the
    // directory cannot be created. The cache is disposable: on failure the application should
    // carry on without it.
    [[nodiscard]] static Expected<std::unique_ptr<ThumbnailCache>> open(ThumbnailCacheOptions options);

    ~ThumbnailCache();
    ThumbnailCache(const ThumbnailCache&) = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;

    // The cached image, or nullopt (missing, unreadable or invalid; see Robustness). Counts a
    // hit or a miss.
    [[nodiscard]] std::optional<Thumbnail> find(const ThumbnailKey& key);

    // Stores (or replaces) the entry for `key`, then evicts least recently used entries until the
    // total fits. Errors (nothing is stored): InvalidArgument when the size is outside 1..65535 or
    // the bytes are not a JPEG; LimitExceeded when the entry would exceed max_entry_bytes or the
    // capacity; I/O errors of core::write_file_atomic (DiskFull, PermissionDenied, IoError).
    // Callers may ignore failures: the cache is an optimisation.
    Status insert(const ThumbnailKey& key, const Thumbnail& thumbnail);

    // Removes the entry if present.
    void erase(const ThumbnailKey& key);

    // Removes every entry file (including ones this process has not indexed) and temporary
    // files. Returns the first deletion failure (for example a file another process holds open
    // without delete sharing); the index is empty either way.
    Status clear();

    // Changes the capacity (clamped up to max_entry_bytes), evicting as needed.
    void set_capacity(std::uint64_t capacity_bytes);

    [[nodiscard]] ThumbnailCacheStats stats() const;

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return options_.directory; }

    // Where the entry for `key` lives (whether or not it exists). For diagnostics and tests.
    [[nodiscard]] std::filesystem::path entry_path(const ThumbnailKey& key) const;

private:
    explicit ThumbnailCache(ThumbnailCacheOptions options);

    struct Slot {
        std::list<std::string>::iterator position;  // in lru_
        std::uint64_t bytes = 0;
        bool touched = false;  // last write time refreshed in this process
    };

    [[nodiscard]] std::filesystem::path path_for_name(const std::string& name) const;
    void scan();
    // The following require mutex_.
    void record_locked(const std::string& name, std::uint64_t bytes);
    void forget_locked(const std::string& name);
    void evict_locked(std::uint64_t capacity);
    void reset_index_locked();

    ThumbnailCacheOptions options_;
    std::filesystem::path version_dir_;  // <directory>\v1

    mutable std::mutex mutex_;
    std::list<std::string> lru_;  // entry names, front = most recently used
    std::unordered_map<std::string, Slot> index_;
    ThumbnailCacheStats stats_;
};

}  // namespace lucent::app
