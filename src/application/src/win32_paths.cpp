#include "win32_paths.hpp"

#include <algorithm>
#include <format>
#include <memory>
#include <utility>

#include <windows.h>

#include <lucent/core/text.hpp>
#include <lucent/core/win32_error.hpp>

namespace lucent::app::detail {

namespace {

constexpr std::wstring_view kVerbatimPrefix = L"\\\\?\\";
constexpr std::wstring_view kDevicePrefix = L"\\\\.\\";
constexpr std::wstring_view kVerbatimUncPrefix = L"\\\\?\\UNC\\";
constexpr std::size_t kMaxWin32PathChars = 32767;
// CreateDirectoryW's documented limit is MAX_PATH - 12 (room for an 8.3 name); use it for all.
constexpr std::size_t kShortPathLimit = MAX_PATH - 12;

std::string quoted(const std::filesystem::path& path) {
    return "'" + core::path_to_utf8_lossy(path) + "'";
}

class Handle {
public:
    explicit Handle(HANDLE handle) noexcept : handle_(handle == INVALID_HANDLE_VALUE ? nullptr : handle) {}
    ~Handle() {
        if (handle_ != nullptr) {
            ::CloseHandle(handle_);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    HANDLE handle_ = nullptr;
};

struct FindCloser {
    void operator()(HANDLE handle) const noexcept { ::FindClose(handle); }
};

}  // namespace

Expected<std::wstring> api_path(const std::filesystem::path& path, std::string_view where) {
    const std::wstring& native = path.native();
    if (native.empty()) {
        return fail(ErrorCode::InvalidArgument, "path is empty", std::string(where));
    }
    if (native.find(L'\0') != std::wstring::npos) {
        return fail(ErrorCode::InvalidArgument, "path contains a NUL character: " + quoted(path),
                    std::string(where));
    }
    if (native.size() > kMaxWin32PathChars) {
        return fail(ErrorCode::LimitExceeded, "path is longer than 32767 characters: " + quoted(path),
                    std::string(where));
    }
    if (native.size() < kShortPathLimit || native.starts_with(kVerbatimPrefix) ||
        native.starts_with(kDevicePrefix)) {
        return native;
    }
    std::wstring full(native.size() + 16, L'\0');
    for (;;) {
        const DWORD length =
            ::GetFullPathNameW(native.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
        if (length == 0) {
            const DWORD code = ::GetLastError();
            return Unexpected(core::win32_error(code, std::string(where), "cannot resolve " + quoted(path)));
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

Expected<std::filesystem::path> canonical_path(const std::filesystem::path& path, std::string_view where) {
    auto api = api_path(path, where);
    if (!api) {
        return Unexpected(std::move(api.error()));
    }
    // Attribute-only access with full sharing: never blocks, and is never blocked by, readers or
    // writers. FILE_FLAG_BACKUP_SEMANTICS is required to open directories.
    Handle handle(::CreateFileW(api->c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!handle) {
        const DWORD code = ::GetLastError();
        return Unexpected(core::win32_error(code, std::string(where), "cannot open " + quoted(path)));
    }
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const DWORD length = ::GetFinalPathNameByHandleW(handle.get(), buffer.data(),
                                                         static_cast<DWORD>(buffer.size()), flags);
        if (length == 0) {
            const DWORD code = ::GetLastError();
            if (code == ERROR_PATH_NOT_FOUND && flags == (FILE_NAME_NORMALIZED | VOLUME_NAME_DOS)) {
                // A volume mounted without a drive letter has no DOS name; use its GUID path.
                flags = FILE_NAME_NORMALIZED | VOLUME_NAME_GUID;
                continue;
            }
            return Unexpected(core::win32_error(code, std::string(where), "cannot resolve " + quoted(path)));
        }
        if (length >= buffer.size()) {
            buffer.resize(static_cast<std::size_t>(length) + 1);
            continue;
        }
        buffer.resize(length);
        if (buffer.starts_with(kVerbatimUncPrefix)) {
            return std::filesystem::path(L"\\\\" + buffer.substr(kVerbatimUncPrefix.size()));
        }
        // "\\?\C:\..." -> "C:\..."; GUID volume paths keep their prefix.
        if (buffer.starts_with(kVerbatimPrefix) && buffer.size() >= kVerbatimPrefix.size() + 2 &&
            buffer[kVerbatimPrefix.size() + 1] == L':') {
            return std::filesystem::path(buffer.substr(kVerbatimPrefix.size()));
        }
        return std::filesystem::path(std::move(buffer));
    }
    return fail(ErrorCode::IoError, "the final path of " + quoted(path) + " kept changing length",
                std::string(where));
}

std::wstring fold_case(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    std::wstring folded(text.size(), L'\0');
    const int length =
        ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, text.data(), static_cast<int>(text.size()),
                        folded.data(), static_cast<int>(folded.size()), nullptr, nullptr, 0);
    if (length <= 0) {
        // Upper-casing never changes the length with file-system rules; fall back to the input
        // (exact comparison) rather than fail a comparison.
        return std::wstring(text);
    }
    folded.resize(static_cast<std::size_t>(length));
    return folded;
}

bool same_path_text(const std::filesystem::path& a, const std::filesystem::path& b) {
    return fold_case(a.native()) == fold_case(b.native());
}

bool is_same_or_within(const std::filesystem::path& path, const std::filesystem::path& directory) {
    std::wstring p = fold_case(path.native());
    std::wstring d = fold_case(directory.native());
    while (d.size() > 1 && (d.back() == L'\\' || d.back() == L'/') && !(d.size() == 3 && d[1] == L':')) {
        d.pop_back();
    }
    if (d.empty()) {
        return false;
    }
    if (p == d) {
        return true;
    }
    if (p.size() <= d.size() || !p.starts_with(d)) {
        return false;
    }
    return d.back() == L'\\' || p[d.size()] == L'\\' || p[d.size()] == L'/';
}

Expected<std::vector<DirectoryEntry>> list_directory(const std::filesystem::path& directory,
                                                     std::string_view where) {
    auto api = api_path(directory / L"*", where);
    if (!api) {
        return Unexpected(std::move(api.error()));
    }
    WIN32_FIND_DATAW data{};
    HANDLE raw = ::FindFirstFileExW(api->c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
    if (raw == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND) {
            return std::vector<DirectoryEntry>{};  // an empty directory (no "." on some file systems)
        }
        return Unexpected(core::win32_error(code, std::string(where), "cannot list " + quoted(directory)));
    }
    std::unique_ptr<void, FindCloser> find(raw);
    std::vector<DirectoryEntry> entries;
    for (;;) {
        const std::wstring_view name(data.cFileName);
        if (name != L"." && name != L"..") {
            DirectoryEntry entry;
            entry.name = std::wstring(name);
            entry.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            entry.reparse_point = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            entry.reparse_tag = entry.reparse_point ? data.dwReserved0 : 0;
            entry.device = (data.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) != 0;
            entries.push_back(std::move(entry));
        }
        if (!::FindNextFileW(find.get(), &data)) {
            const DWORD code = ::GetLastError();
            if (code == ERROR_NO_MORE_FILES) {
                break;
            }
            return Unexpected(
                core::win32_error(code, std::string(where), "cannot list " + quoted(directory)));
        }
    }
    std::vector<std::pair<std::wstring, DirectoryEntry>> keyed;
    keyed.reserve(entries.size());
    for (DirectoryEntry& entry : entries) {
        keyed.emplace_back(fold_case(entry.name), std::move(entry));
    }
    std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) {
            return a.first < b.first;
        }
        return a.second.name < b.second.name;
    });
    entries.clear();
    for (auto& [key, entry] : keyed) {
        entries.push_back(std::move(entry));
    }
    return entries;
}

bool is_directory(const std::filesystem::path& path) {
    auto api = api_path(path, "app.path.is_directory");
    if (!api) {
        return false;
    }
    const DWORD attributes = ::GetFileAttributesW(api->c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        return true;
    }
    // A link: it is a directory only when its target resolves.
    return static_cast<bool>(canonical_path(path, "app.path.is_directory"));
}

Expected<std::uint64_t> free_disk_bytes(const std::filesystem::path& directory, std::string_view where) {
    auto api = api_path(directory, where);
    if (!api) {
        return Unexpected(std::move(api.error()));
    }
    std::wstring root = *api;
    if (!root.empty() && root.back() != L'\\') {
        root.push_back(L'\\');  // GetDiskFreeSpaceExW requires a trailing backslash for UNC roots
    }
    ULARGE_INTEGER available{};
    if (!::GetDiskFreeSpaceExW(root.c_str(), &available, nullptr, nullptr)) {
        const DWORD code = ::GetLastError();
        return Unexpected(core::win32_error(code, std::string(where),
                                            "cannot query the free space of " + quoted(directory)));
    }
    return static_cast<std::uint64_t>(available.QuadPart);
}

bool indicates_absence(const Error& error) noexcept {
    if (error.code == ErrorCode::NotFound) {
        return true;
    }
    return error.code == ErrorCode::IoError && error.native_code > 0 &&
           error.native_code <= std::int64_t{0xFFFF'FFFF} &&
           core::is_volume_absent_win32_error(static_cast<std::uint32_t>(error.native_code));
}

}  // namespace lucent::app::detail
