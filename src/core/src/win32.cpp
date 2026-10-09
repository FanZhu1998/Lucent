#include "win32.hpp"

#include <cstddef>

#include "lucent/core/text.hpp"
#include "lucent/core/win32_error.hpp"

namespace lucent::core::detail {

namespace {

constexpr std::wstring_view kVerbatimPrefix = L"\\\\?\\";          // \\?\    (no normalisation)
constexpr std::wstring_view kDevicePrefix = L"\\\\.\\";            // \\.\    (device namespace)
constexpr std::wstring_view kVerbatimUncPrefix = L"\\\\?\\UNC\\";  // \\?\UNC\ (verbatim UNC)
constexpr std::size_t kMaxWin32PathChars = 32767;  // longest path the wide Win32 file APIs accept
// CreateDirectoryW refuses a path of MAX_PATH - 12 characters or more (room for an 8.3 name must
// remain), so the "\\?\" form starts there rather than at MAX_PATH.
constexpr std::size_t kVerbatimFromChars = MAX_PATH - 12;

}  // namespace

Expected<std::wstring> win32_api_path(const std::filesystem::path& path, std::string_view where) {
    const std::wstring& native = path.native();
    if (native.empty()) {
        return fail(ErrorCode::InvalidArgument, "path is empty", std::string(where));
    }
    if (native.find(L'\0') != std::wstring::npos) {
        return fail(ErrorCode::InvalidArgument, "path contains a NUL character: " + quoted(path),
                    std::string(where));
    }
    if (native.size() > kMaxWin32PathChars) {
        return fail(ErrorCode::LimitExceeded,
                    "path is longer than the Windows limit of 32767 characters: " + quoted(path),
                    std::string(where));
    }
    if (native.size() < kVerbatimFromChars || native.starts_with(kVerbatimPrefix) ||
        native.starts_with(kDevicePrefix)) {
        return native;
    }

    // GetFullPathNameW returns the required size including the terminator when the buffer is
    // too small, and the length excluding it on success. The current directory can change
    // between calls, so retry until the result fits.
    std::wstring full(native.size() + 16, L'\0');
    for (;;) {
        const DWORD length =
            ::GetFullPathNameW(native.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
        if (length == 0) {
            const DWORD code = ::GetLastError();
            return Unexpected(
                win32_error(code, std::string(where), "cannot resolve the full path of " + quoted(path)));
        }
        if (length < full.size()) {
            full.resize(length);
            break;
        }
        full.resize(length);
    }

    if (full.starts_with(kVerbatimPrefix) || full.starts_with(kDevicePrefix)) {
        return full;
    }
    if (full.starts_with(L"\\\\")) {
        return std::wstring(kVerbatimUncPrefix) + full.substr(2);
    }
    return std::wstring(kVerbatimPrefix) + full;
}

std::string quoted(const std::filesystem::path& path) {
    std::string text;
    text.reserve(path.native().size() + 2);
    text.push_back('\'');
    text += path_to_utf8_lossy(path);
    text.push_back('\'');
    return text;
}

}  // namespace lucent::core::detail
