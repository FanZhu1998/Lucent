#include "lucent/core/hash.hpp"

// clang-format off: <bcrypt.h> requires the types declared by <windows.h>.
#include <windows.h>
#include <bcrypt.h>
// clang-format on

#include <algorithm>
#include <format>
#include <limits>
#include <memory>
#include <new>
#include <utility>

#include "hash_internal.hpp"
#include "win32.hpp"

namespace lucent::core {

namespace {

constexpr std::string_view kHexDigits = "0123456789abcdef";

// NTSTATUS values used here (ntstatus.h cannot be included alongside windows.h cleanly).
constexpr NTSTATUS kStatusNoMemory = static_cast<NTSTATUS>(0xC0000017L);
constexpr NTSTATUS kStatusInvalidHandle = static_cast<NTSTATUS>(0xC0000008L);

constexpr bool succeeded(NTSTATUS status) noexcept {
    return status >= 0;
}

// The SHA-256 algorithm provider, opened once per process (thread-safe static
// initialisation) with BCRYPT_HASH_REUSABLE_FLAG so hash objects can be reused after finish.
// Deliberately never closed: hashers may run during static destruction, and the OS releases
// the provider at process exit.
struct Sha256Provider {
    BCRYPT_ALG_HANDLE handle = nullptr;
    NTSTATUS status = 0;
};

const Sha256Provider& sha256_provider() noexcept {
    static const Sha256Provider provider = [] {
        Sha256Provider p;
        p.status = ::BCryptOpenAlgorithmProvider(&p.handle, BCRYPT_SHA256_ALGORITHM, nullptr,
                                                 BCRYPT_HASH_REUSABLE_FLAG);
        if (!succeeded(p.status)) {
            p.handle = nullptr;
        }
        return p;
    }();
    return provider;
}

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

Unexpected source_changed(const std::filesystem::path& path, std::string detail_text) {
    return fail(
        ErrorCode::SourceChanged,
        "source file changed while it was being read: " + detail::quoted(path) + " (" + detail_text + ")",
        "core.hash.file");
}

}  // namespace

// ---- Digest helpers -----------------------------------------------------------------------

std::string to_hex(const Digest256& digest) {
    std::string hex(digest.size() * 2, '\0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        hex[2 * i] = kHexDigits[digest[i] >> 4];
        hex[2 * i + 1] = kHexDigits[digest[i] & 0x0F];
    }
    return hex;
}

Expected<Digest256> digest_from_hex(std::string_view hex) {
    Digest256 digest{};
    if (hex.size() != digest.size() * 2) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("a SHA-256 digest has 64 hex digits, got {} characters", hex.size()),
                    "core.hash.digest_from_hex");
    }
    for (std::size_t i = 0; i < digest.size(); ++i) {
        const int high = hex_value(hex[2 * i]);
        const int low = hex_value(hex[2 * i + 1]);
        if (high < 0 || low < 0) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("invalid hex digit in SHA-256 digest at position {}",
                                    high < 0 ? 2 * i : 2 * i + 1),
                        "core.hash.digest_from_hex");
        }
        digest[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return digest;
}

// ---- Sha256 -------------------------------------------------------------------------------

Sha256::Sha256() noexcept {
    const Sha256Provider& provider = sha256_provider();
    if (provider.handle == nullptr) {
        status_ = provider.status;
        return;
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    // With a null object buffer CNG allocates and owns the hash object's memory.
    const NTSTATUS status =
        ::BCryptCreateHash(provider.handle, &hash, nullptr, 0, nullptr, 0, BCRYPT_HASH_REUSABLE_FLAG);
    if (!succeeded(status)) {
        status_ = status;
        return;
    }
    hash_ = hash;
}

Sha256::~Sha256() {
    if (hash_ != nullptr) {
        ::BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
    }
}

Sha256::Sha256(Sha256&& other) noexcept
    : hash_(std::exchange(other.hash_, nullptr)), status_(std::exchange(other.status_, 0)) {}

Sha256& Sha256::operator=(Sha256&& other) noexcept {
    if (this != &other) {
        if (hash_ != nullptr) {
            ::BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
        }
        hash_ = std::exchange(other.hash_, nullptr);
        status_ = std::exchange(other.status_, 0);
    }
    return *this;
}

void Sha256::update(std::span<const std::byte> data) noexcept {
    if (hash_ == nullptr || status_ != 0) {
        return;  // failure is sticky and reported by finish()
    }
    constexpr std::size_t kMaxPiece = std::numeric_limits<ULONG>::max();
    while (!data.empty()) {
        const std::size_t piece = std::min(data.size(), kMaxPiece);
        // BCryptHashData takes a non-const pointer but does not modify the input.
        const NTSTATUS status = ::BCryptHashData(
            static_cast<BCRYPT_HASH_HANDLE>(hash_),
            reinterpret_cast<PUCHAR>(const_cast<std::byte*>(data.data())), static_cast<ULONG>(piece), 0);
        if (!succeeded(status)) {
            status_ = status;
            return;
        }
        data = data.subspan(piece);
    }
}

Expected<Digest256> Sha256::finish() {
    constexpr const char* kWhere = "core.hash.sha256";
    if (status_ == 0 && hash_ == nullptr) {
        return fail(ErrorCode::Internal, "finish() called on a moved-from Sha256", kWhere);
    }
    if (status_ == 0) {
        Digest256 digest{};
        const NTSTATUS status = ::BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(hash_), digest.data(),
                                                   static_cast<ULONG>(digest.size()), 0);
        if (succeeded(status)) {
            return digest;
        }
        status_ = status;
    }
    const ErrorCode code = status_ == kStatusNoMemory ? ErrorCode::OutOfMemory : ErrorCode::Internal;
    const char* what = status_ == kStatusInvalidHandle ? " (invalid handle)" : "";
    return fail(code,
                std::format("Windows CNG SHA-256 failed with NTSTATUS 0x{:08X}{}",
                            static_cast<std::uint32_t>(status_), what),
                kWhere, static_cast<std::int64_t>(static_cast<std::uint32_t>(status_)));
}

Expected<Digest256> sha256(std::span<const std::byte> data) {
    Sha256 hasher;
    hasher.update(data);
    return hasher.finish();
}

// ---- hash_file ----------------------------------------------------------------------------

Expected<FileHashResult> hash_file(const std::filesystem::path& path, CancellationToken cancel,
                                   std::size_t chunk_bytes) {
    return detail::hash_file_observed(path, cancel, chunk_bytes, {});
}

Expected<FileHashResult> detail::hash_file_observed(const std::filesystem::path& path,
                                                    const CancellationToken& cancel, std::size_t chunk_bytes,
                                                    const HashChunkObserver& after_chunk) {
    constexpr const char* kWhere = "core.hash.file";
    if (chunk_bytes == 0 || chunk_bytes > kMaxHashChunkBytes) {
        return fail(
            ErrorCode::InvalidArgument,
            std::format("hash chunk size must be 1..{} bytes, got {}", kMaxHashChunkBytes, chunk_bytes),
            kWhere);
    }
    if (auto status = cancel.check(kWhere); !status) {
        return Unexpected(std::move(status.error()));
    }

    auto file = ReadOnlyFile::open(path, AccessHint::Sequential);
    if (!file) {
        return Unexpected(std::move(file.error()));
    }
    auto before = file->identity();
    if (!before) {
        return Unexpected(std::move(before.error()));
    }
    const std::uint64_t expected_size = before->size_bytes;

    // Never allocate more than the file needs.
    const std::size_t buffer_size =
        static_cast<std::size_t>(std::min<std::uint64_t>(chunk_bytes, expected_size));
    std::unique_ptr<std::byte[]> buffer;
    if (buffer_size > 0) {
        try {
            buffer = std::make_unique_for_overwrite<std::byte[]>(buffer_size);
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory,
                        std::format("cannot allocate a {} byte hash buffer", buffer_size), kWhere);
        }
    }

    Sha256 hasher;
    std::uint64_t offset = 0;
    while (offset < expected_size) {
        if (auto status = cancel.check(kWhere); !status) {
            return Unexpected(std::move(status.error()));
        }
        const std::size_t wanted =
            static_cast<std::size_t>(std::min<std::uint64_t>(buffer_size, expected_size - offset));
        const std::span<std::byte> chunk(buffer.get(), wanted);
        auto read = file->read_at(offset, chunk);
        if (!read) {
            return Unexpected(std::move(read.error()));
        }
        if (*read != wanted) {
            return source_changed(path, std::format("it was truncated: end of file at byte {} of {}",
                                                    offset + *read, expected_size));
        }
        hasher.update(chunk);
        offset += wanted;
        if (after_chunk) {
            after_chunk(offset);
        }
    }

    auto digest = hasher.finish();
    if (!digest) {
        return Unexpected(std::move(digest.error()));
    }

    // Stable-read check 1: the open file object itself was not modified.
    auto after_handle = file->identity();
    if (!after_handle) {
        return Unexpected(std::move(after_handle.error()));
    }
    if (*after_handle != *before) {
        return source_changed(
            path, "it was modified: before " + to_string(*before) + "; after " + to_string(*after_handle));
    }

    // Stable-read check 2: the file was not deleted while open. Without POSIX delete semantics
    // a deleted-but-open file cannot be reopened by path (ACCESS_DENIED), so ask the handle.
    auto deleted = file->is_delete_pending();
    if (!deleted) {
        return Unexpected(std::move(deleted.error()));
    }
    if (*deleted) {
        return source_changed(path, "it was deleted");
    }

    // Stable-read check 3: the path still names that same, unmodified file object (it was not
    // renamed away or replaced by another file).
    auto after_path = stat_file(path);
    if (!after_path) {
        if (after_path.error().code == ErrorCode::NotFound) {
            return source_changed(path, "it was removed or renamed");
        }
        return Unexpected(std::move(after_path.error()));
    }
    if (*after_path != *before) {
        return source_changed(path, "the path now refers to a different or modified file: before " +
                                        to_string(*before) + "; now " + to_string(*after_path));
    }

    return FileHashResult{*digest, *before};
}

}  // namespace lucent::core
