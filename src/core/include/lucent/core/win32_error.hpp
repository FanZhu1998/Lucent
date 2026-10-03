#pragma once

// Translation of Win32 error codes (GetLastError values) into Lucent's error model.
//
// The mapping is deliberately coarse: callers branch on lucent::ErrorCode, while the exact
// Win32 code is preserved in Error::native_code for diagnostics.
//
// | Win32 error                                                     | ErrorCode        |
// |-----------------------------------------------------------------|------------------|
// | FILE_NOT_FOUND, PATH_NOT_FOUND, INVALID_DRIVE, BAD_NETPATH,     | NotFound         |
// | BAD_NET_NAME                                                    |                  |
// | ACCESS_DENIED                                                   | PermissionDenied |
// | DISK_FULL, HANDLE_DISK_FULL                                     | DiskFull         |
// | FILE_EXISTS, ALREADY_EXISTS                                     | AlreadyExists    |
// | NOT_ENOUGH_MEMORY, OUTOFMEMORY                                  | OutOfMemory      |
// | OPERATION_ABORTED, CANCELLED                                    | Cancelled        |
// | SHARING_VIOLATION, LOCK_VIOLATION                               | IoError          |
// | anything else                                                   | IoError          |
//
// SHARING_VIOLATION and LOCK_VIOLATION mean another process currently holds the file with an
// incompatible share mode or byte-range lock. That is a transient condition, not a lack of
// permission, so it maps to IoError (which batch processing treats as retryable) rather than
// PermissionDenied (which asks the user to fix access rights). is_transient_win32_error()
// lets retry policies distinguish it from other I/O errors via Error::native_code.
//
// Thread-safety: all functions are safe to call concurrently. last_error() reads the calling
// thread's last-error value.

#include <cstdint>
#include <string>

#include "lucent/core/error.hpp"

namespace lucent::core {

// Maps a Win32 error code to a Lucent category using the table above. 0 (ERROR_SUCCESS) maps
// to ErrorCode::Internal: reporting success as a failure is a Lucent defect.
[[nodiscard]] ErrorCode error_code_from_win32(std::uint32_t win32_code) noexcept;

// True for Win32 errors that are expected to clear on their own (sharing and lock
// violations). Retry policies may use this; the ErrorCode for these is IoError.
[[nodiscard]] bool is_transient_win32_error(std::uint32_t win32_code) noexcept;

// True for Win32 errors that mean the volume a path is on is not there: a card reader or other
// removable drive without media (NOT_READY, NO_MEDIA_IN_DRIVE), a device that was disconnected
// (DEV_NOT_EXIST, DEVICE_NOT_CONNECTED, DEVICE_REMOVED, NO_SUCH_DEVICE) or a volume without a
// recognised file system (UNRECOGNIZED_VOLUME). They map to IoError (the table above); presence
// checks treat them like NotFound: the file is not reachable until the media comes back.
[[nodiscard]] bool is_volume_absent_win32_error(std::uint32_t win32_code) noexcept;

// System message text for a Win32 error code as UTF-8, without trailing whitespace or line
// breaks. English text is requested first (Error::message is developer-facing English); if the
// English resources are unavailable the user's default language is used; if the system has no
// text for the code at all, the result is "Win32 error <code>".
[[nodiscard]] std::string win32_message(std::uint32_t win32_code);

// Builds an Error for `win32_code`: code per the table above, native_code = win32_code and
// message "<context>: <system text>" (just the system text when `context` is empty). `where`
// is the stable dotted location, e.g. "core.file.open".
[[nodiscard]] Error win32_error(std::uint32_t win32_code, std::string where, std::string context);

// win32_error() for the calling thread's GetLastError() value, read on entry. Arguments are
// evaluated before the call, so when building `context` might itself call Win32 APIs (for
// example by allocating or formatting), capture GetLastError() first and use win32_error().
[[nodiscard]] Error last_error(std::string where, std::string context);

}  // namespace lucent::core
