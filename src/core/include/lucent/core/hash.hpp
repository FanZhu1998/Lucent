#pragma once

// SHA-256 hashing (Windows CNG) and stable-read file hashing (blueprint section 7: "Detect
// source modification with stable-read checks before/after hashing"; file path alone is not
// image identity).

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "lucent/core/cancellation.hpp"
#include "lucent/core/error.hpp"
#include "lucent/core/file.hpp"

namespace lucent::core {

// A SHA-256 digest in the standard big-endian byte order (the order of its hex form).
using Digest256 = std::array<std::uint8_t, 32>;

// 64 lowercase hex digits, e.g. "e3b0c442...b855" for the empty message.
[[nodiscard]] std::string to_hex(const Digest256& digest);

// Parses exactly 64 hex digits (either case; no prefix, separators or whitespace). Anything
// else is InvalidArgument. to_hex() is the canonical (lowercase) form.
[[nodiscard]] Expected<Digest256> digest_from_hex(std::string_view hex);

// Incremental SHA-256 (FIPS 180-4) over Windows CNG (BCrypt). The CNG algorithm provider is
// opened once per process on first use and shared by all hashers; it is intentionally never
// closed, so hashing remains valid during static destruction.
//
// Usage:
//     Sha256 hasher;
//     hasher.update(chunk1);
//     hasher.update(chunk2);
//     Expected<Digest256> digest = hasher.finish();
//
// Errors: CNG reports failures only when the provider or hash object cannot be created (for
// example out of memory) or when a handle is invalid. Such a failure is sticky: later update()
// calls are ignored and finish() reports it (ErrorCode::OutOfMemory or ErrorCode::Internal,
// with the NTSTATUS in Error::native_code).
//
// Thread-safety: a Sha256 object is not safe for concurrent use; use one per thread. Distinct
// objects may be used concurrently.
class Sha256 {
public:
    Sha256() noexcept;
    ~Sha256();

    Sha256(Sha256&& other) noexcept;
    Sha256& operator=(Sha256&& other) noexcept;
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    // Appends `data` to the message. Inputs of any size are accepted.
    void update(std::span<const std::byte> data) noexcept;

    // Returns the digest of everything passed to update() since construction or the previous
    // finish(), then resets the hasher to the empty message so it can be reused.
    [[nodiscard]] Expected<Digest256> finish();

private:
    void* hash_ = nullptr;     // BCRYPT_HASH_HANDLE; nullptr after a creation failure or a move
    std::int32_t status_ = 0;  // first failing NTSTATUS, 0 while healthy
};

// One-shot SHA-256 of an in-memory buffer.
[[nodiscard]] Expected<Digest256> sha256(std::span<const std::byte> data);

// Default read size for hash_file(): large enough to amortise system calls, small enough to
// keep cancellation responsive (about 1 ms of reading from a fast SSD).
inline constexpr std::size_t kDefaultHashChunkBytes = std::size_t{1} << 20;  // 1 MiB

// Largest accepted hash_file() chunk size.
inline constexpr std::size_t kMaxHashChunkBytes = std::size_t{1} << 30;  // 1 GiB

struct FileHashResult {
    Digest256 digest{};
    // The identity the file had throughout hashing (verified unchanged before and after).
    FileIdentity identity;
};

// Hashes the file at `path` with a stable read:
//  1. opens it with ReadOnlyFile (see file.hpp for the share mode) and captures its identity
//     through the handle;
//  2. hashes exactly identity.size_bytes bytes in `chunk_bytes` reads, checking `cancel` before
//     each read;
//  3. captures the identity again, both through the handle and by re-querying `path`
//     (stat_file), requires both to equal the first snapshot, and requires that the file was
//     not deleted while open (ReadOnlyFile::is_delete_pending).
// If the file was modified (size or last write time changed), replaced by another file at the
// same path (file id changed), truncated during the read, or removed or renamed away from
// `path`, the result is ErrorCode::SourceChanged ("source file changed while it was being
// read") and callers should re-import the source rather than trust stale data.
//
// Limitation: the check detects writers only through the file's size, last write time and file
// id. A writer that changes content without changing any of them goes unnoticed, and the digest
// is then computed over a mix of old and new bytes. Two such writers are known:
//   * writes through a memory-mapped view (CreateFileMapping/MapViewOfFile): Windows does not
//     update the last write time for them, even after the view and handles are closed
//     (documented in the MapViewOfFile remarks; confirmed on Windows 11 by the W1 review, where a
//     hash taken while two bytes were written through a view succeeded);
//   * a writer that explicitly restores the timestamp.
// Ordinary WriteFile-based writes through any handle do update the last write time.
// A caller that needs content identity to be exact despite such writers must re-hash and
// compare (for example on import and again before publishing), or use a change journal; the
// W4 import pipeline owns that decision (docs/development/roadmap.md).
//
// Other errors: InvalidArgument when chunk_bytes is 0 or above kMaxHashChunkBytes; Cancelled;
// OutOfMemory when the chunk buffer cannot be allocated; open/read failures per file.hpp.
// Thread-safety: safe to call concurrently (each call uses its own handle and hasher).
[[nodiscard]] Expected<FileHashResult> hash_file(const std::filesystem::path& path, CancellationToken cancel,
                                                 std::size_t chunk_bytes = kDefaultHashChunkBytes);

}  // namespace lucent::core
