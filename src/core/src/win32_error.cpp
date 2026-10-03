#include "lucent/core/win32_error.hpp"

#include <format>
#include <memory>
#include <string_view>
#include <utility>

#include "lucent/core/text.hpp"

#include "win32.hpp"

namespace lucent::core {

static_assert(sizeof(DWORD) == sizeof(std::uint32_t), "Win32 error codes are 32-bit");

ErrorCode error_code_from_win32(std::uint32_t win32_code) noexcept {
    switch (static_cast<DWORD>(win32_code)) {
        case ERROR_SUCCESS:
            return ErrorCode::Internal;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
            return ErrorCode::NotFound;
        case ERROR_ACCESS_DENIED:
            return ErrorCode::PermissionDenied;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return ErrorCode::DiskFull;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
            return ErrorCode::AlreadyExists;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return ErrorCode::OutOfMemory;
        case ERROR_OPERATION_ABORTED:
        case ERROR_CANCELLED:
            return ErrorCode::Cancelled;
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return ErrorCode::IoError;  // transient; see is_transient_win32_error()
        default:
            return ErrorCode::IoError;
    }
}

bool is_transient_win32_error(std::uint32_t win32_code) noexcept {
    return win32_code == ERROR_SHARING_VIOLATION || win32_code == ERROR_LOCK_VIOLATION;
}

bool is_volume_absent_win32_error(std::uint32_t win32_code) noexcept {
    switch (static_cast<DWORD>(win32_code)) {
        case ERROR_NOT_READY:
        case ERROR_NO_MEDIA_IN_DRIVE:
        case ERROR_DEV_NOT_EXIST:
        case ERROR_DEVICE_NOT_CONNECTED:
        case ERROR_DEVICE_REMOVED:
        case ERROR_NO_SUCH_DEVICE:
        case ERROR_UNRECOGNIZED_VOLUME:
            return true;
        default:
            return false;
    }
}

std::string win32_message(std::uint32_t win32_code) {
    struct LocalFreeDeleter {
        void operator()(wchar_t* buffer) const noexcept { ::LocalFree(buffer); }
    };

    const auto format = [win32_code](DWORD language) {
        wchar_t* buffer = nullptr;
        const DWORD length = ::FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS |
                FORMAT_MESSAGE_MAX_WIDTH_MASK,
            nullptr, static_cast<DWORD>(win32_code), language, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
        std::unique_ptr<wchar_t, LocalFreeDeleter> owned(buffer);
        std::wstring_view text =
            length != 0 && owned ? std::wstring_view(owned.get(), length) : std::wstring_view();
        // FORMAT_MESSAGE_MAX_WIDTH_MASK turns line breaks into spaces; trim what remains.
        while (!text.empty() && (text.back() == L' ' || text.back() == L'\r' || text.back() == L'\n' ||
                                 text.back() == L'\t')) {
            text.remove_suffix(1);
        }
        return wide_to_utf8_lossy(text);
    };

    std::string text = format(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
    if (text.empty()) {
        text = format(0);  // English resources unavailable: user's default language
    }
    if (text.empty()) {
        text = std::format("Win32 error {}", win32_code);
    }
    return text;
}

Error win32_error(std::uint32_t win32_code, std::string where, std::string context) {
    std::string system_text = win32_message(win32_code);
    std::string message;
    if (context.empty()) {
        message = std::move(system_text);
    } else {
        message = std::move(context);
        message += ": ";
        message += system_text;
    }
    return Error{error_code_from_win32(win32_code), std::move(message), std::move(where),
                 static_cast<std::int64_t>(win32_code)};
}

Error last_error(std::string where, std::string context) {
    const DWORD code = ::GetLastError();
    return win32_error(code, std::move(where), std::move(context));
}

}  // namespace lucent::core
