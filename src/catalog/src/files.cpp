#include "files.hpp"

#include <format>
#include <random>
#include <string>
#include <utility>

#include <windows.h>

#include "lucent/core/storage.hpp"
#include "lucent/core/text.hpp"
#include "lucent/core/win32_error.hpp"

namespace lucent::catalog::detail {
namespace {

// The form of `path` for wide Win32 file APIs: paths of MAX_PATH - 12 characters or more are made
// absolute and given the "\\?\" (or "\\?\UNC\") prefix so the MAX_PATH limit does not apply.
Expected<std::wstring> api_path(const std::filesystem::path& path, std::string_view where) {
    const std::wstring& native = path.native();
    if (native.empty() || native.find(L'\0') != std::wstring::npos) {
        return fail(ErrorCode::InvalidArgument, "empty path or path with NUL characters", std::string(where));
    }
    if (native.size() < MAX_PATH - 12 || native.starts_with(L"\\\\?\\") || native.starts_with(L"\\\\.\\")) {
        return native;
    }
    const DWORD needed = ::GetFullPathNameW(native.c_str(), 0, nullptr, nullptr);
    if (needed == 0) {
        return Unexpected(core::last_error(std::string(where), "GetFullPathNameW"));
    }
    std::wstring full(needed, L'\0');
    const DWORD written = ::GetFullPathNameW(native.c_str(), needed, full.data(), nullptr);
    if (written == 0 || written >= needed) {
        return Unexpected(core::last_error(std::string(where), "GetFullPathNameW"));
    }
    full.resize(written);
    if (full.starts_with(L"\\\\")) {
        return L"\\\\?\\UNC\\" + full.substr(2);
    }
    return L"\\\\?\\" + full;
}

std::string quoted(const std::filesystem::path& path) {
    return "'" + core::path_to_utf8_lossy(path) + "'";
}

}  // namespace

Expected<std::optional<FileFacts>> file_facts(const std::filesystem::path& path, std::string_view where) {
    Expected<std::wstring> name = api_path(path, where);
    if (!name) {
        return Unexpected(std::move(name.error()));
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(name->c_str(), GetFileExInfoStandard, &data)) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return std::optional<FileFacts>();
        }
        return Unexpected(core::win32_error(error, std::string(where), "cannot query " + quoted(path)));
    }
    FileFacts facts;
    facts.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    facts.size_bytes = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    return std::optional<FileFacts>(facts);
}

Status rename_no_replace(const std::filesystem::path& from, const std::filesystem::path& to,
                         std::string_view where) {
    Expected<std::wstring> source = api_path(from, where);
    Expected<std::wstring> target = api_path(to, where);
    if (!source || !target) {
        return Unexpected(!source ? std::move(source.error()) : std::move(target.error()));
    }
    if (!::MoveFileExW(source->c_str(), target->c_str(), MOVEFILE_WRITE_THROUGH)) {
        return Unexpected(core::win32_error(::GetLastError(), std::string(where),
                                            "cannot rename " + quoted(from) + " to " + quoted(to)));
    }
    return ok();
}

Status rename_replace(const std::filesystem::path& from, const std::filesystem::path& to,
                      std::string_view where) {
    Expected<std::wstring> source = api_path(from, where);
    Expected<std::wstring> target = api_path(to, where);
    if (!source || !target) {
        return Unexpected(!source ? std::move(source.error()) : std::move(target.error()));
    }
    if (!::MoveFileExW(source->c_str(), target->c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return Unexpected(core::win32_error(::GetLastError(), std::string(where),
                                            "cannot replace " + quoted(to) + " with " + quoted(from)));
    }
    return ok();
}

Status delete_file_if_exists(const std::filesystem::path& path, std::string_view where) {
    Expected<std::wstring> name = api_path(path, where);
    if (!name) {
        return Unexpected(std::move(name.error()));
    }
    if (!::DeleteFileW(name->c_str())) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return ok();
        }
        return Unexpected(core::win32_error(error, std::string(where), "cannot delete " + quoted(path)));
    }
    return ok();
}

Status ensure_directory(const std::filesystem::path& directory, std::string_view where) {
    Status created = core::create_directory_tree(directory);
    if (!created) {
        Error error = std::move(created.error());
        error.where = std::string(where);
        return Unexpected(std::move(error));
    }
    return ok();
}

Expected<std::vector<std::wstring>> list_file_names(const std::filesystem::path& directory,
                                                    std::string_view where) {
    Expected<std::wstring> name = api_path(directory / L"*", where);
    if (!name) {
        return Unexpected(std::move(name.error()));
    }
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW data{};
    const HANDLE find = ::FindFirstFileExW(name->c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                           nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return names;
        }
        return Unexpected(core::win32_error(error, std::string(where), "cannot list " + quoted(directory)));
    }
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            names.emplace_back(data.cFileName);
        }
    } while (::FindNextFileW(find, &data));
    const DWORD error = ::GetLastError();
    ::FindClose(find);
    if (error != ERROR_NO_MORE_FILES) {
        return Unexpected(core::win32_error(error, std::string(where), "cannot list " + quoted(directory)));
    }
    return names;
}

// ---------------------------------------------------------------------------------------------
// CatalogLock
// ---------------------------------------------------------------------------------------------

CatalogLock::~CatalogLock() {
    release();
}

CatalogLock::CatalogLock(CatalogLock&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

CatalogLock& CatalogLock::operator=(CatalogLock&& other) noexcept {
    if (this != &other) {
        release();
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

std::filesystem::path CatalogLock::lock_path(const std::filesystem::path& catalog_path) {
    std::filesystem::path lock = catalog_path;
    lock += L".lock";
    return lock;
}

Expected<CatalogLock> CatalogLock::acquire(const std::filesystem::path& catalog_path,
                                           std::string_view where) {
    const std::filesystem::path lock = lock_path(catalog_path);
    Expected<std::wstring> name = api_path(lock, where);
    if (!name) {
        return Unexpected(std::move(name.error()));
    }
    // No sharing: any second open (another Catalog, another process, restore) fails with a sharing
    // violation while this handle lives. Delete-on-close removes the file even after a crash,
    // because the system closes the handle when the process ends.
    const HANDLE handle = ::CreateFileW(
        name->c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
            return fail(ErrorCode::AlreadyExists,
                        "the catalog " + quoted(catalog_path) + " is already open for writing (lock file " +
                            quoted(lock) + " is held by this or another Lucent process)",
                        std::string(where), static_cast<std::int64_t>(error));
        }
        return Unexpected(
            core::win32_error(error, std::string(where), "cannot create the lock file " + quoted(lock)));
    }
    CatalogLock result;
    result.handle_ = handle;
    return result;
}

void CatalogLock::release() noexcept {
    if (handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
}

std::wstring random_hex_suffix() {
    std::random_device random;
    const std::uint64_t value = (static_cast<std::uint64_t>(random()) << 32) ^ random();
    return std::format(L"{:016x}", value);
}

}  // namespace lucent::catalog::detail
