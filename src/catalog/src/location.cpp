#include "lucent/catalog/location.hpp"

#include <algorithm>
#include <climits>
#include <filesystem>
#include <string>
#include <utility>

#include <windows.h>

#include "lucent/core/app_paths.hpp"
#include "lucent/core/text.hpp"

#include "codec.hpp"

namespace lucent::catalog {
namespace {

constexpr const char* kWhereKey = "catalog.path_key";

bool is_drive_letter(wchar_t c) noexcept {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

// "\\?\C:\x" -> "C:\x"; "\\?\UNC\s\x" -> "\\s\x"; "\\.\C:\x" -> "C:\x". Other verbatim forms
// ("\\?\Volume{GUID}\x") are kept, because without the prefix they would not be absolute.
std::wstring strip_verbatim_prefix(std::wstring text) {
    if (text.starts_with(L"\\\\?\\UNC\\")) {
        return L"\\\\" + text.substr(8);
    }
    if ((text.starts_with(L"\\\\?\\") || text.starts_with(L"\\\\.\\")) && text.size() >= 6 &&
        is_drive_letter(text[4]) && text[5] == L':') {
        return text.substr(4);
    }
    return text;
}

Expected<std::wstring> upper_case_invariant(const std::wstring& text) {
    if (text.empty()) {
        return text;
    }
    if (text.size() > static_cast<std::size_t>(INT_MAX)) {
        return fail(ErrorCode::LimitExceeded, "path too long", kWhereKey);
    }
    const int length = static_cast<int>(text.size());
    const int needed = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, text.data(), length, nullptr,
                                       0, nullptr, nullptr, 0);
    if (needed <= 0) {
        return fail(ErrorCode::Internal, "LCMapStringEx failed", kWhereKey,
                    static_cast<std::int64_t>(::GetLastError()));
    }
    std::wstring upper(static_cast<std::size_t>(needed), L'\0');
    const int written = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, text.data(), length,
                                        upper.data(), needed, nullptr, nullptr, 0);
    if (written <= 0) {
        return fail(ErrorCode::Internal, "LCMapStringEx failed", kWhereKey,
                    static_cast<std::int64_t>(::GetLastError()));
    }
    upper.resize(static_cast<std::size_t>(written));
    return upper;
}

}  // namespace

Status check_catalog_storage(StorageKind kind, const std::filesystem::path& path) {
    const std::string shown = core::path_to_utf8_lossy(path);
    switch (kind) {
        case StorageKind::Fixed:
        case StorageKind::Removable:
            return ok();
        case StorageKind::Network:
            return fail(ErrorCode::Unsupported,
                        "the catalog '" + shown +
                            "' is on a network location; the live catalog must be on a local disk (SQLite's "
                            "write-ahead log and locking are not reliable on network file systems)",
                        "catalog.location");
        case StorageKind::Unknown:
            return fail(ErrorCode::Unsupported,
                        "cannot determine the drive type of '" + shown +
                            "' (is the drive mounted?); the live catalog must be on a local disk",
                        "catalog.location");
    }
    return fail(ErrorCode::InvalidArgument, "unknown storage kind", "catalog.location");
}

std::filesystem::path default_catalog_file_name() {
    return std::filesystem::path(L"lucent.catalog");
}

Expected<std::filesystem::path> default_catalog_path() {
    Expected<core::AppDirectories> directories = core::default_app_directories();
    if (!directories) {
        return Unexpected(std::move(directories.error()));
    }
    return directories->data / default_catalog_file_name();
}

std::filesystem::path default_backup_directory(const std::filesystem::path& catalog_path) {
    return catalog_path.parent_path() / L"backups";
}

Expected<std::string> make_path_key(const std::filesystem::path& path) {
    const std::wstring& native = path.native();
    if (native.empty()) {
        return fail(ErrorCode::InvalidArgument, "empty path", kWhereKey);
    }
    if (native.find(L'\0') != std::wstring::npos) {
        return fail(ErrorCode::InvalidArgument, "path contains NUL characters", kWhereKey);
    }
    std::wstring text = native;
    std::replace(text.begin(), text.end(), L'/', L'\\');
    text = strip_verbatim_prefix(std::move(text));

    std::wstring normal;
    if (text.starts_with(L"\\\\?\\") || text.starts_with(L"\\\\.\\")) {
        normal = std::move(text);  // a volume GUID path: kept verbatim
    } else {
        const std::filesystem::path lexical = std::filesystem::path(text).lexically_normal();
        if (!lexical.is_absolute()) {
            return fail(ErrorCode::InvalidArgument,
                        "path is not absolute: '" + core::path_to_utf8_lossy(path) + "'", kWhereKey);
        }
        normal = lexical.native();
        const std::size_t root = lexical.root_path().native().size();
        while (normal.size() > root && normal.back() == L'\\') {
            normal.pop_back();
        }
    }

    Expected<std::wstring> upper = upper_case_invariant(normal);
    if (!upper) {
        return Unexpected(std::move(upper.error()));
    }
    Expected<std::string> utf8 = core::wide_to_utf8(*upper);
    if (!utf8) {
        return fail(ErrorCode::InvalidArgument,
                    "path is not valid UTF-16: '" + core::path_to_utf8_lossy(path) + "'", kWhereKey);
    }
    return std::move(*utf8);
}

}  // namespace lucent::catalog
