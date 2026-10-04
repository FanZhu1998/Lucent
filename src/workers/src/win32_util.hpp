#pragma once

// Win32 helpers private to lucent_workers: an owning HANDLE and long-path spelling.

#include <filesystem>
#include <string>
#include <utility>

#include <windows.h>

namespace lucent::workers::detail {

// Owns a Win32 handle (nullptr and INVALID_HANDLE_VALUE both mean "none"). Move-only.
class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle == INVALID_HANDLE_VALUE ? nullptr : handle) {}
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

// The "\\?\" (or "\\?\UNC\") spelling of an absolute path, so that Win32 calls work beyond MAX_PATH
// without the process-wide long-path opt-in. Relative paths are returned unchanged.
[[nodiscard]] std::wstring verbatim(const std::filesystem::path& path);

}  // namespace lucent::workers::detail
