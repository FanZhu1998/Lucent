#pragma once

// Private Win32 file helpers for the export module: long-path handling, temporary sibling
// reservation, flushing, no-clobber publication and cleanup. <windows.h> stays in the .cpp.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>

namespace lucent::exporter::detail {

// The string to pass to wide-character file APIs (CreateFileW, _wfopen, TIFFOpenW):
// InvalidArgument for empty paths or paths containing NUL; paths already in "\\?\" form and
// paths shorter than MAX_PATH - 12 (room for the 8.3 short name rule of CreateDirectory) pass
// unchanged; longer paths are made absolute (GetFullPathNameW) and prefixed with "\\?\" or
// "\\?\UNC\" so the MAX_PATH limit does not apply.
//
// This mirrors the private helper in lucent_core (core::detail::win32_api_path), which is not
// part of core's public API.
[[nodiscard]] Expected<std::wstring> api_path(const std::filesystem::path& path, std::string_view where);

// The directory the destination lives in: its parent path, or the current directory when the
// destination is a bare file name.
[[nodiscard]] std::filesystem::path destination_directory(const std::filesystem::path& destination);

// Checks the destination before any work: InvalidArgument when it has no file name; NotFound
// when its directory does not exist or is not a directory; AlreadyExists when something (file
// or directory) already exists at the destination. Other Win32 failures are mapped with
// core::win32_error.
[[nodiscard]] Status check_destination(const std::filesystem::path& destination);

// Creates a new empty file "~lucent-<16 hex digits>.tmp" in `directory` with CREATE_NEW (never
// replacing anything; retried with a fresh random name on collision) and returns its path.
[[nodiscard]] Expected<std::filesystem::path> reserve_temporary_sibling(const std::filesystem::path& directory);

// Opens an existing file for writing and calls FlushFileBuffers so its data reaches the device.
[[nodiscard]] Status flush_to_disk(const std::filesystem::path& path);

// Replaces the contents of the existing file `path` (TRUNCATE_EXISTING, no sharing) with `bytes`,
// written in chunks with a cancellation check before each, then FlushFileBuffers through the
// same handle. Errors (export.file.write): Win32 failures mapped with core::win32_error
// (DiskFull, PermissionDenied, IoError, ...); Cancelled.
[[nodiscard]] Status write_file_bytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                                      const core::CancellationToken& cancel);

// Reads the file back and compares it byte for byte with `bytes`. Corrupt (export.file.compare)
// when the size or any byte differs (the message names the first differing offset); Win32
// failures mapped with core::win32_error; Cancelled.
[[nodiscard]] Status compare_file_bytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                                        const core::CancellationToken& cancel);

// MoveFileExW(from, to, MOVEFILE_WRITE_THROUGH) without MOVEFILE_REPLACE_EXISTING. An existing
// `to` is AlreadyExists and is never modified. Transient sharing violations (for example a
// virus scanner inspecting the new file) are retried a few times over about half a second.
[[nodiscard]] Status publish_no_clobber(const std::filesystem::path& from, const std::filesystem::path& to);

// Deletes a file, retrying transient failures briefly. Never throws; returns false when the
// file could not be deleted (a missing file counts as deleted).
bool remove_file_quietly(const std::filesystem::path& path) noexcept;

// Deletes the file on destruction unless release() was called.
class TemporaryFile {
public:
    explicit TemporaryFile(std::filesystem::path path) noexcept : path_(std::move(path)) {}
    ~TemporaryFile();

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    // Ownership passes to the caller (the file was published under another name).
    void release() noexcept { released_ = true; }

private:
    std::filesystem::path path_;
    bool released_ = false;
};

// Maps the calling thread's C runtime error state (_doserrno, then errno) after a failed
// _wfopen / fwrite / fclose to an Error.
[[nodiscard]] Error crt_file_error(std::string where, std::string context);

}  // namespace lucent::exporter::detail
