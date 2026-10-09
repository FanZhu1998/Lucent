#pragma once

// Read-only access to source files and file identity (blueprint sections 3 and 7).
//
// Lucent never writes to a photograph it did not create. ReadOnlyFile opens files for reading
// only, with a share mode that never gets in the way of the user or other programs:
//
//   FILE_SHARE_READ   other readers are never blocked;
//   FILE_SHARE_WRITE  a program that saves the file while Lucent reads it is not blocked. The
//                     change is detected instead: callers compare FileIdentity snapshots taken
//                     before and after a read (hash_file() does this for them) and re-import;
//   FILE_SHARE_DELETE the user can rename, move or delete the file in Explorer while it is
//                     open. The open handle keeps reading the original data; a later path
//                     check (stat_file) sees the file gone or replaced.
//
// Opening fails with an IoError (Win32 ERROR_SHARING_VIOLATION in Error::native_code) only
// when another program holds the file with a share mode that excludes readers.
//
// Paths: all functions take std::filesystem::path and call the wide-character Win32 APIs.
// Absolute paths of 248 characters or more (CreateDirectoryW's limit, below MAX_PATH) are
// passed to Win32 in "\\?\" form automatically, so deep folder structures work without the
// process-wide long-path opt-in. Symbolic links and
// junctions are followed. Empty paths and paths containing NUL characters are rejected with
// InvalidArgument.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "lucent/core/error.hpp"

namespace lucent::core {

// Identity and version of a file object at one instant.
//
// `volume_serial` + `file_id` name the file object (they survive renames and are reused only
// after deletion); `size_bytes` + `last_write_time` describe its content version. Two
// snapshots compare equal (operator==) only when all four fields match; is_same_file()
// compares just the object identity.
//
// Sources, in order of preference:
//  - GetFileInformationByHandleEx(FileIdInfo): 64-bit volume serial and the file system's
//    128-bit file id (NTFS, ReFS, and most modern file systems);
//  - fallback when FileIdInfo is unavailable (for example FAT32/exFAT, some network
//    redirectors) or fails: GetFileInformationByHandle's 32-bit volume serial and 64-bit file
//    index, zero-extended (index stored little-endian in bytes 0..7, bytes 8..15 zero). On FAT
//    file systems this index is derived from the directory entry position and may change when
//    the file is renamed or its directory is compacted, which callers observe as a changed
//    identity (a safe false positive).
// The field values are opaque: compare them, persist them, never interpret them.
struct FileIdentity {
    std::uint64_t volume_serial = 0;
    std::array<std::uint8_t, 16> file_id{};
    std::uint64_t size_bytes = 0;  // 0 for directories
    // Last write time in 100-ns ticks since 1601-01-01 UTC (the FILETIME epoch). Reflects
    // WriteFile-based writes through any handle, as the file system reports them for open files.
    // It does NOT change for writes through a memory-mapped view, nor when a writer restores it
    // explicitly, so identity snapshots cannot detect such writers (see hash_file()).
    std::int64_t last_write_time = 0;

    friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};

// True when both snapshots name the same file object (volume serial and file id match),
// regardless of size or modification time.
[[nodiscard]] bool is_same_file(const FileIdentity& a, const FileIdentity& b) noexcept;

// "volume=<16 hex> id=<32 hex> size=<n> mtime=<ticks>" for logs and diagnostics.
[[nodiscard]] std::string to_string(const FileIdentity& identity);

// Snapshot of the file or directory currently at `path`, queried through a handle opened with
// attribute-only access and full sharing (it never blocks or is blocked by readers or writers).
// NotFound when nothing exists at `path`.
[[nodiscard]] Expected<FileIdentity> stat_file(const std::filesystem::path& path);

// Caching hint passed to the OS when opening a file.
enum class AccessHint : std::uint8_t {
    Normal = 0,      // no hint
    Sequential = 1,  // mostly front-to-back reads (FILE_FLAG_SEQUENTIAL_SCAN)
    Random = 2,      // scattered reads, e.g. parsing TIFF/DNG IFDs (FILE_FLAG_RANDOM_ACCESS)
};

// Move-only RAII owner of a read-only Win32 file handle. See the file comment for the share
// mode.
//
// Thread-safety: read_at(), read_exact_at(), size() and identity() are const and may be called
// concurrently on one object; every read carries its own offset and never uses or depends on
// the handle's file pointer. The handle is synchronous, so the OS serialises concurrent reads
// on one object; open separate ReadOnlyFile objects when parallel throughput matters.
// open(), close(), moves and destruction must not race with other calls on the same object.
class ReadOnlyFile {
public:
    // An empty object that owns no handle; operations on it fail with ErrorCode::Internal.
    ReadOnlyFile() noexcept = default;
    ~ReadOnlyFile();

    ReadOnlyFile(ReadOnlyFile&& other) noexcept;
    ReadOnlyFile& operator=(ReadOnlyFile&& other) noexcept;
    ReadOnlyFile(const ReadOnlyFile&) = delete;
    ReadOnlyFile& operator=(const ReadOnlyFile&) = delete;

    // Opens an existing regular file for reading (OPEN_EXISTING; never creates or truncates).
    // Directories are rejected with InvalidArgument; devices, pipes and other non-disk objects
    // (for example "NUL" or "\\.\pipe\x") with Unsupported. Win32 failures map per
    // win32_error.hpp (NotFound, PermissionDenied, IoError for sharing violations, ...).
    [[nodiscard]] static Expected<ReadOnlyFile> open(const std::filesystem::path& path,
                                                     AccessHint hint = AccessHint::Normal);

    [[nodiscard]] bool is_open() const noexcept { return handle_ != nullptr; }

    // The path passed to open(). Empty for an empty object.
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    // Current size in bytes, queried from the file system on every call (the file may be
    // growing or shrinking under a concurrent writer).
    [[nodiscard]] Expected<std::uint64_t> size() const;

    // Current identity of the open file object, queried through the handle. It stays
    // available after the path has been renamed or deleted.
    [[nodiscard]] Expected<FileIdentity> identity() const;

    // True when the file has been deleted while open: its name is already gone (POSIX delete
    // semantics) or will be removed when the last handle closes. On file systems without POSIX
    // semantics, reopening such a file by path fails with PermissionDenied, so this is the
    // reliable way to tell deletion from an access problem.
    [[nodiscard]] Expected<bool> is_delete_pending() const;

    // Reads up to buffer.size() bytes starting at byte `offset` and returns the number read.
    // The count is smaller than buffer.size() only when end-of-file is reached; an offset at or
    // beyond end-of-file returns 0 (not an error). Offsets above INT64_MAX are InvalidArgument.
    [[nodiscard]] Expected<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> buffer) const;

    // Reads exactly buffer.size() bytes at `offset`; a short read fails with ErrorCode::Corrupt
    // ("unexpected end of file"), the category for truncated input. On failure the buffer
    // contents are unspecified.
    [[nodiscard]] Status read_exact_at(std::uint64_t offset, std::span<std::byte> buffer) const;

    // Closes the handle (idempotent). The object becomes empty.
    void close() noexcept;

private:
    ReadOnlyFile(void* handle, std::filesystem::path path) noexcept;

    void* handle_ = nullptr;  // Win32 HANDLE; nullptr when empty (never INVALID_HANDLE_VALUE)
    std::filesystem::path path_;
};

// Reads a whole file into memory. Fails with LimitExceeded, before allocating, when the file is
// larger than `max_bytes`; with OutOfMemory when the buffer cannot be allocated; and with
// SourceChanged when the open file's size or last write time changes during the read (or it
// ends early), so the returned bytes are one consistent version of the file. The check uses the
// open handle only; hash_file() additionally verifies that the path still names that file.
[[nodiscard]] Expected<std::vector<std::byte>> read_file_bytes(const std::filesystem::path& path,
                                                               std::uint64_t max_bytes);

}  // namespace lucent::core
