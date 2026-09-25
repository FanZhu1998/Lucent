#pragma once

// Lucent's error model.
//
// Every fallible operation returns lucent::Expected<T> (or lucent::Status for no value).
// Exceptions are not used to report expected failures such as unreadable files, unsupported
// cameras or cancellation; they remain reserved for programming errors and allocation failure
// inside third-party code, which module boundaries translate into Error values.
//
// Expected is tl::expected today. It is API-compatible with C++23 std::expected, so the alias
// can be switched once the toolchain baseline moves to C++23.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <tl/expected.hpp>

namespace lucent {

// Broad, stable failure categories. The numeric values are persisted in job records and batch
// reports, so existing enumerators must never be renumbered or reused; append new ones only.
enum class ErrorCode : std::uint16_t {
    Unknown = 1,
    InvalidArgument = 2,   // caller supplied a value outside the documented contract
    NotFound = 3,          // file, record, asset or subimage does not exist
    AlreadyExists = 4,     // publication target or record already present
    PermissionDenied = 5,  // OS refused access
    IoError = 6,           // read/write failure not covered by a more specific code
    DiskFull = 7,          // destination has insufficient space
    Corrupt = 8,           // input is truncated or fails structural validation
    Unsupported = 9,       // well-formed input that this build cannot interpret
    LimitExceeded = 10,    // file size, dimension or memory limit would be exceeded
    OutOfMemory = 11,      // allocation failed
    Cancelled = 12,        // cooperative cancellation was requested
    Timeout = 13,          // operation exceeded its deadline
    VersionMismatch = 14,  // schema, protocol or asset version is incompatible
    Internal = 15,         // invariant violation; indicates a Lucent defect
};

// Stable lowercase identifier for logs and reports, e.g. "not_found".
[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

struct Error {
    ErrorCode code = ErrorCode::Internal;

    // Developer-facing English description. User-visible text is produced by the UI from
    // `code`, `where` and structured context, never by showing this string verbatim.
    std::string message;

    // Stable dotted location of the failure, e.g. "raw.libraw.unpack" or "core.file.read".
    std::string where;

    // Native error from the underlying library or OS (LibRaw code, Win32 error, SQLite code),
    // or 0 when not applicable. Recorded for diagnostics only.
    std::int64_t native_code = 0;

    // "where: message (code, native N)" for logs.
    [[nodiscard]] std::string describe() const;
};

template <class T>
using Expected = tl::expected<T, Error>;

using Status = Expected<void>;

using Unexpected = tl::unexpected<Error>;

// Builds the unexpected value to return from a function yielding Expected<T>:
//     return lucent::fail(ErrorCode::NotFound, "no such file", "core.file.open");
[[nodiscard]] inline Unexpected fail(ErrorCode code, std::string message, std::string where = {},
                                     std::int64_t native_code = 0) {
    return Unexpected(Error{code, std::move(message), std::move(where), native_code});
}

// Success value for Status-returning functions: `return lucent::ok();`
[[nodiscard]] inline Status ok() noexcept { return {}; }

}  // namespace lucent
