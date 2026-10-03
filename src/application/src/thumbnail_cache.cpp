#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <new>
#include <span>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

#include <lucent/application/thumbnail_cache.hpp>
#include <lucent/core/file.hpp>
#include <lucent/core/storage.hpp>
#include <lucent/core/win32_error.hpp>

namespace lucent::app {

namespace {

constexpr const char* kWhere = "app.thumbnail_cache";
constexpr std::array<std::uint8_t, 4> kMagic = {'L', 'T', 'H', 'B'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kKeyBytes = 75;
constexpr std::size_t kNameHexDigits = 64;
constexpr std::wstring_view kVersionDir = L"v1";
constexpr std::wstring_view kEntryExtension = L".lthumb";
constexpr std::int32_t kMaxDimension = 65535;

static_assert(4 + 4 + kKeyBytes + 4 + 4 + 8 + 32 == kThumbnailHeaderBytes);

class Writer {
public:
    explicit Writer(std::vector<std::byte>& out) : out_(out) {}
    void bytes(std::span<const std::uint8_t> data) {
        for (const std::uint8_t b : data) {
            out_.push_back(static_cast<std::byte>(b));
        }
    }
    template <class T>
    void little_endian(T value) {
        const auto v = static_cast<std::uint64_t>(value);
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            out_.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
        }
    }

private:
    std::vector<std::byte>& out_;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> in) : in_(in) {}
    std::span<const std::byte> take(std::size_t count) {
        const auto piece = in_.subspan(offset_, count);  // callers checked the total size
        offset_ += count;
        return piece;
    }
    template <class T>
    T little_endian() {
        std::uint64_t v = 0;
        const auto piece = take(sizeof(T));
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(piece[i])) << (8 * i);
        }
        return static_cast<T>(v);
    }

private:
    std::span<const std::byte> in_;
    std::size_t offset_ = 0;
};

std::vector<std::byte> serialize_key(const ThumbnailKey& key) {
    std::vector<std::byte> out;
    out.reserve(kKeyBytes);
    Writer w(out);
    w.bytes(key.source_sha256);
    w.little_endian(key.subimage);
    w.little_endian(static_cast<std::uint16_t>(key.generator));
    w.little_endian(key.generator_version);
    w.little_endian(static_cast<std::uint8_t>(key.size_class));
    w.bytes(key.variant);
    return out;
}

// Entry name: SHA-256 of the serialised key, lowercase hex.
std::string entry_name(const ThumbnailKey& key) {
    auto digest = core::sha256(serialize_key(key));
    if (!digest) {
        // CNG failure (out of memory): fall back to a name that cannot collide with a real entry
        // and is never valid on read, so the operation degrades to a miss.
        return std::string(kNameHexDigits, 'x');
    }
    return core::to_hex(*digest);
}

bool is_lower_hex(wchar_t c) noexcept {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
}

// "<64 lowercase hex>.lthumb" -> the 64 digits; empty for any other name.
std::string entry_name_from_file(const std::wstring& file_name) {
    if (file_name.size() != kNameHexDigits + kEntryExtension.size() ||
        !file_name.ends_with(kEntryExtension)) {
        return {};
    }
    std::string name;
    name.reserve(kNameHexDigits);
    for (std::size_t i = 0; i < kNameHexDigits; ++i) {
        if (!is_lower_hex(file_name[i])) {
            return {};
        }
        name.push_back(static_cast<char>(file_name[i]));
    }
    return name;
}

bool is_shard_name(const std::wstring& name) {
    return name.size() == 2 && is_lower_hex(name[0]) && is_lower_hex(name[1]);
}

bool starts_like_jpeg(std::span<const std::byte> bytes) {
    return bytes.size() >= 3 && bytes[0] == std::byte{0xFF} && bytes[1] == std::byte{0xD8} &&
           bytes[2] == std::byte{0xFF};
}

// Parses and validates an entry file; nullopt when it is not a valid entry for `key`.
std::optional<Thumbnail> parse_entry(std::span<const std::byte> file, const ThumbnailKey& key) {
    if (file.size() < kThumbnailHeaderBytes) {
        return std::nullopt;
    }
    Reader r(file);
    const auto magic = r.take(kMagic.size());
    for (std::size_t i = 0; i < kMagic.size(); ++i) {
        if (std::to_integer<std::uint8_t>(magic[i]) != kMagic[i]) {
            return std::nullopt;
        }
    }
    if (r.little_endian<std::uint32_t>() != kFormatVersion) {
        return std::nullopt;
    }
    const std::vector<std::byte> expected_key = serialize_key(key);
    const auto stored_key = r.take(kKeyBytes);
    if (!std::equal(stored_key.begin(), stored_key.end(), expected_key.begin(), expected_key.end())) {
        return std::nullopt;
    }
    Thumbnail thumbnail;
    thumbnail.width = static_cast<std::int32_t>(r.little_endian<std::uint32_t>());
    thumbnail.height = static_cast<std::int32_t>(r.little_endian<std::uint32_t>());
    const auto payload_size = r.little_endian<std::uint64_t>();
    const auto stored_digest = r.take(32);
    if (thumbnail.width < 1 || thumbnail.width > kMaxDimension || thumbnail.height < 1 ||
        thumbnail.height > kMaxDimension || payload_size != file.size() - kThumbnailHeaderBytes) {
        return std::nullopt;
    }
    const auto payload = file.subspan(kThumbnailHeaderBytes);
    if (!starts_like_jpeg(payload)) {
        return std::nullopt;
    }
    auto digest = core::sha256(payload);
    if (!digest || !std::equal(stored_digest.begin(), stored_digest.end(),
                               reinterpret_cast<const std::byte*>(digest->data()),
                               reinterpret_cast<const std::byte*>(digest->data()) + digest->size())) {
        return std::nullopt;
    }
    thumbnail.jpeg.assign(payload.begin(), payload.end());
    return thumbnail;
}

// Publishes an entry file. Replacing a file that another thread or process is touching at that
// instant (a timestamp refresh, a deletion or another replacement in progress) fails with a
// sharing violation or ERROR_ACCESS_DENIED; such contention clears within milliseconds, so retry
// a few times before giving up. A genuine permission problem costs at most ~60 ms extra.
Status publish(const std::filesystem::path& path, std::span<const std::byte> file) {
    constexpr int kAttempts = 6;
    constexpr std::int64_t kAccessDenied = 5;  // ERROR_ACCESS_DENIED
    const core::AtomicWriteOptions options{core::ExistingFile::Replace, false};
    Status written = core::write_file_atomic(path, file, options);
    for (int attempt = 1; attempt < kAttempts && !written; ++attempt) {
        const std::int64_t native = written.error().native_code;
        const bool contended =
            native == kAccessDenied ||
            (native > 0 && core::is_transient_win32_error(static_cast<std::uint32_t>(native)));
        if (!contended) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1 << attempt));  // 2, 4, ... 32 ms
        written = core::write_file_atomic(path, file, options);
    }
    return written;
}

void remove_quietly(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
}

}  // namespace

// ---- lifetime ---------------------------------------------------------------------------------

ThumbnailCache::ThumbnailCache(ThumbnailCacheOptions options)
    : options_(std::move(options)), version_dir_(options_.directory / kVersionDir) {
    stats_.capacity_bytes = options_.capacity_bytes;
}

ThumbnailCache::~ThumbnailCache() = default;

Expected<std::unique_ptr<ThumbnailCache>> ThumbnailCache::open(ThumbnailCacheOptions options) {
    if (options.directory.empty()) {
        return fail(ErrorCode::InvalidArgument, "thumbnail cache directory is empty", kWhere);
    }
    if (options.max_entry_bytes <= kThumbnailHeaderBytes ||
        options.capacity_bytes < options.max_entry_bytes) {
        return fail(
            ErrorCode::InvalidArgument,
            std::format("thumbnail cache limits are inconsistent: capacity {} bytes, max entry {} bytes "
                        "(need capacity >= max entry > {})",
                        options.capacity_bytes, options.max_entry_bytes, kThumbnailHeaderBytes),
            kWhere);
    }
    try {
        std::unique_ptr<ThumbnailCache> cache(new ThumbnailCache(std::move(options)));
        if (auto status = core::create_directory_tree(cache->version_dir_); !status) {
            return Unexpected(std::move(status.error()));
        }
        cache->scan();
        return cache;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while opening the thumbnail cache", kWhere);
    }
}

void ThumbnailCache::scan() {
    struct Found {
        std::filesystem::file_time_type modified;
        std::string name;
        std::uint64_t bytes;
    };
    std::vector<Found> found;
    std::error_code error;
    for (std::filesystem::directory_iterator shards(version_dir_, error), end; !error && shards != end;
         shards.increment(error)) {
        const std::wstring shard = shards->path().filename().native();
        std::error_code kind_error;
        if (!is_shard_name(shard) || !shards->is_directory(kind_error)) {
            continue;
        }
        std::error_code inner;
        for (std::filesystem::directory_iterator files(shards->path(), inner); !inner && files != end;
             files.increment(inner)) {
            const std::filesystem::path file_name = files->path().filename();
            if (core::is_temporary_file_name(file_name)) {
                // Left by a crash, or being written by another process (then deletion fails).
                remove_quietly(files->path());
                continue;
            }
            std::string name = entry_name_from_file(file_name.native());
            if (name.empty() || static_cast<wchar_t>(name[0]) != shard[0] ||
                static_cast<wchar_t>(name[1]) != shard[1]) {
                continue;  // not ours; never delete unknown files
            }
            std::error_code stat_error;
            const std::uint64_t bytes = files->file_size(stat_error);
            const auto modified = files->last_write_time(stat_error);
            if (stat_error) {
                continue;
            }
            found.push_back(Found{modified, std::move(name), bytes});
        }
    }
    // Oldest first, ties by name: deterministic regardless of enumeration order.
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        return std::tie(a.modified, a.name) < std::tie(b.modified, b.name);
    });

    const std::lock_guard lock(mutex_);
    for (const Found& entry : found) {
        record_locked(entry.name, entry.bytes);  // each becomes the most recent so far
    }
    evict_locked(options_.capacity_bytes);
}

// ---- index (mutex_ held) ----------------------------------------------------------------------

std::filesystem::path ThumbnailCache::path_for_name(const std::string& name) const {
    const std::wstring wide(name.begin(), name.end());  // ASCII hex
    return version_dir_ / wide.substr(0, 2) / (wide + std::wstring(kEntryExtension));
}

std::filesystem::path ThumbnailCache::entry_path(const ThumbnailKey& key) const {
    return path_for_name(entry_name(key));
}

void ThumbnailCache::record_locked(const std::string& name, std::uint64_t bytes) {
    if (auto it = index_.find(name); it != index_.end()) {
        stats_.bytes -= it->second.bytes;
        it->second.bytes = bytes;
        lru_.splice(lru_.begin(), lru_, it->second.position);
    } else {
        lru_.push_front(name);
        index_.emplace(name, Slot{lru_.begin(), bytes, false});
    }
    stats_.bytes += bytes;
    stats_.entries = index_.size();
}

void ThumbnailCache::forget_locked(const std::string& name) {
    if (auto it = index_.find(name); it != index_.end()) {
        stats_.bytes -= it->second.bytes;
        lru_.erase(it->second.position);
        index_.erase(it);
        stats_.entries = index_.size();
    }
}

void ThumbnailCache::evict_locked(std::uint64_t capacity) {
    while (stats_.bytes > capacity && !lru_.empty()) {
        const std::string victim = lru_.back();
        remove_quietly(path_for_name(victim));  // already gone is fine
        forget_locked(victim);
        ++stats_.evictions;
    }
}

void ThumbnailCache::reset_index_locked() {
    lru_.clear();
    index_.clear();
    stats_.bytes = 0;
    stats_.entries = 0;
}

// ---- operations -------------------------------------------------------------------------------

std::optional<Thumbnail> ThumbnailCache::find(const ThumbnailKey& key) {
    try {
        const std::string name = entry_name(key);
        const std::filesystem::path path = path_for_name(name);

        // A concurrent hit refreshing the last write time makes a read look unstable
        // (SourceChanged); one retry separates that from real trouble.
        Expected<std::vector<std::byte>> bytes = core::read_file_bytes(path, options_.max_entry_bytes);
        if (!bytes && bytes.error().code == ErrorCode::SourceChanged) {
            bytes = core::read_file_bytes(path, options_.max_entry_bytes);
        }

        if (!bytes) {
            const ErrorCode code = bytes.error().code;
            const std::lock_guard lock(mutex_);
            ++stats_.misses;
            if (code == ErrorCode::NotFound) {
                forget_locked(name);  // evicted by another process, or the directory was deleted
            } else if (code == ErrorCode::LimitExceeded) {
                remove_quietly(path);  // larger than any entry this cache writes
                forget_locked(name);
                ++stats_.corrupt_entries;
            }
            return std::nullopt;  // sharing violations and other I/O trouble: leave the file
        }

        std::optional<Thumbnail> thumbnail = parse_entry(*bytes, key);
        const std::lock_guard lock(mutex_);
        if (!thumbnail) {
            remove_quietly(path);
            forget_locked(name);
            ++stats_.corrupt_entries;
            ++stats_.misses;
            return std::nullopt;
        }
        ++stats_.hits;
        record_locked(name, bytes->size());
        Slot& slot = index_.at(name);
        if (!slot.touched) {
            slot.touched = true;
            std::error_code error;  // best effort: recency across sessions
            std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), error);
        }
        return thumbnail;
    } catch (const std::bad_alloc&) {
        const std::lock_guard lock(mutex_);
        ++stats_.misses;
        return std::nullopt;
    }
}

Status ThumbnailCache::insert(const ThumbnailKey& key, const Thumbnail& thumbnail) {
    auto reject = [this](ErrorCode code, std::string message) {
        const std::lock_guard lock(mutex_);
        ++stats_.rejected;
        return fail(code, std::move(message), kWhere);
    };
    if (thumbnail.width < 1 || thumbnail.width > kMaxDimension || thumbnail.height < 1 ||
        thumbnail.height > kMaxDimension) {
        return reject(ErrorCode::InvalidArgument, std::format("thumbnail size {}x{} is outside 1..65535",
                                                              thumbnail.width, thumbnail.height));
    }
    if (!starts_like_jpeg(thumbnail.jpeg)) {
        return reject(ErrorCode::InvalidArgument, "thumbnail bytes are not a JPEG (no SOI marker)");
    }
    const std::uint64_t entry_bytes = kThumbnailHeaderBytes + thumbnail.jpeg.size();
    {
        const std::lock_guard lock(mutex_);
        if (entry_bytes > options_.max_entry_bytes || entry_bytes > options_.capacity_bytes) {
            ++stats_.rejected;
            return fail(
                ErrorCode::LimitExceeded,
                std::format("thumbnail entry of {} bytes exceeds the limit (max entry {}, capacity {})",
                            entry_bytes, options_.max_entry_bytes, options_.capacity_bytes),
                kWhere);
        }
    }

    try {
        auto digest = core::sha256(thumbnail.jpeg);
        if (!digest) {
            return Unexpected(std::move(digest.error()));
        }
        std::vector<std::byte> file;
        file.reserve(static_cast<std::size_t>(entry_bytes));
        Writer w(file);
        w.bytes(kMagic);
        w.little_endian(kFormatVersion);
        const std::vector<std::byte> key_bytes = serialize_key(key);
        file.insert(file.end(), key_bytes.begin(), key_bytes.end());
        w.little_endian(static_cast<std::uint32_t>(thumbnail.width));
        w.little_endian(static_cast<std::uint32_t>(thumbnail.height));
        w.little_endian(static_cast<std::uint64_t>(thumbnail.jpeg.size()));
        w.bytes(*digest);
        file.insert(file.end(), thumbnail.jpeg.begin(), thumbnail.jpeg.end());

        const std::string name = entry_name(key);
        const std::filesystem::path path = path_for_name(name);
        Status written = publish(path, file);
        if (!written && written.error().code == ErrorCode::NotFound) {
            // The shard (or the whole cache) was deleted underneath us: recreate and retry once.
            std::error_code error;
            if (!std::filesystem::is_directory(version_dir_, error)) {
                const std::lock_guard lock(mutex_);
                reset_index_locked();  // every indexed file went with the directory
            }
            if (auto created = core::create_directory_tree(path.parent_path()); !created) {
                written = std::move(created);
            } else {
                written = publish(path, file);
            }
        }

        const std::lock_guard lock(mutex_);
        if (!written) {
            ++stats_.write_failures;
            return written;
        }
        ++stats_.insertions;
        record_locked(name, entry_bytes);
        index_.at(name).touched = true;  // just written: its last write time is current
        evict_locked(options_.capacity_bytes);
        return ok();
    } catch (const std::bad_alloc&) {
        const std::lock_guard lock(mutex_);
        ++stats_.write_failures;
        return fail(ErrorCode::OutOfMemory, "out of memory while storing a thumbnail", kWhere);
    }
}

void ThumbnailCache::erase(const ThumbnailKey& key) {
    const std::string name = entry_name(key);
    const std::lock_guard lock(mutex_);
    remove_quietly(path_for_name(name));
    forget_locked(name);
}

Status ThumbnailCache::clear() {
    const std::lock_guard lock(mutex_);
    reset_index_locked();
    Status first_failure = ok();
    std::error_code error;
    for (std::filesystem::directory_iterator shards(version_dir_, error), end; !error && shards != end;
         shards.increment(error)) {
        if (!is_shard_name(shards->path().filename().native())) {
            continue;
        }
        std::error_code inner;
        for (std::filesystem::directory_iterator files(shards->path(), inner); !inner && files != end;
             files.increment(inner)) {
            const std::filesystem::path file_name = files->path().filename();
            if (entry_name_from_file(file_name.native()).empty() &&
                !core::is_temporary_file_name(file_name)) {
                continue;
            }
            std::error_code removed;
            std::filesystem::remove(files->path(), removed);
            if (removed && first_failure) {
                first_failure =
                    fail(ErrorCode::IoError, "cannot delete a thumbnail cache file: " + removed.message(),
                         kWhere, removed.value());
            }
        }
    }
    return first_failure;
}

void ThumbnailCache::set_capacity(std::uint64_t capacity_bytes) {
    const std::lock_guard lock(mutex_);
    options_.capacity_bytes = std::max(capacity_bytes, options_.max_entry_bytes);
    stats_.capacity_bytes = options_.capacity_bytes;
    evict_locked(options_.capacity_bytes);
}

ThumbnailCacheStats ThumbnailCache::stats() const {
    const std::lock_guard lock(mutex_);
    return stats_;
}

}  // namespace lucent::app
