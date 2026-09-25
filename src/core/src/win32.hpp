#pragma once

// Private Win32 helpers shared by lucent_core's implementation files. Not part of the public
// API; <windows.h> must never leak into public headers.

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <windows.h>

#include "lucent/core/error.hpp"

namespace lucent::core::detail {

// Move-only owner of a Win32 kernel handle. Stores nullptr for "no handle", normalising
// INVALID_HANDLE_VALUE on construction, so callers test one sentinel only.
class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept
        : handle_(handle == INVALID_HANDLE_VALUE ? nullptr : handle) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.handle_, nullptr));
        }
        return *this;
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }
    [[nodiscard]] HANDLE release() noexcept { return std::exchange(handle_, nullptr); }

    void reset(HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr) {
            ::CloseHandle(handle_);
        }
        handle_ = handle == INVALID_HANDLE_VALUE ? nullptr : handle;
    }

private:
    HANDLE handle_ = nullptr;
};

// The string to hand to wide-character Win32 file APIs for `path`:
//  - empty paths and paths containing NUL are InvalidArgument (a NUL would silently truncate
//    the name at the API boundary);
//  - paths already in "\\?\" or "\\.\" form, and paths shorter than MAX_PATH, pass unchanged;
//  - longer paths are made absolute with GetFullPathNameW (which also normalises separators
//    and "." / ".." segments, as Win32 would) and prefixed with "\\?\" (or "\\?\UNC\" for
//    UNC paths) so the MAX_PATH limit does not apply.
[[nodiscard]] Expected<std::wstring> win32_api_path(const std::filesystem::path& path,
                                                    std::string_view where);

// "'<path>'" as UTF-8 for error messages (lossy for names that are not valid UTF-16).
[[nodiscard]] std::string quoted(const std::filesystem::path& path);

}  // namespace lucent::core::detail
