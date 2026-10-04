#include <lucent/workers/job_output.hpp>

#include <format>
#include <string>

#include <windows.h>

#include <lucent/core/text.hpp>
#include <lucent/core/win32_error.hpp>

#include "win32_util.hpp"

namespace lucent::workers {

namespace {

constexpr std::string_view kWhere = "workers.output";

bool is_not_found(DWORD error) noexcept { return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND; }

// Deletes one file; clears a read-only attribute and retries once. ok() when it does not exist.
Status delete_file(const std::filesystem::path& path) {
    const std::wstring name = detail::verbatim(path);
    if (::DeleteFileW(name.c_str())) {
        return ok();
    }
    DWORD error = ::GetLastError();
    if (is_not_found(error)) {
        return ok();
    }
    if (error == ERROR_ACCESS_DENIED) {
        const DWORD attributes = ::GetFileAttributesW(name.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0 &&
            ::SetFileAttributesW(name.c_str(), attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY)) &&
            ::DeleteFileW(name.c_str())) {
            return ok();
        }
        error = ::GetLastError();
    }
    return Unexpected(core::win32_error(error, std::string(kWhere),
                                        std::format("cannot delete '{}'", core::path_to_utf8_lossy(path))));
}

}  // namespace

std::filesystem::path job_scratch_directory(const std::filesystem::path& temp_path) {
    std::filesystem::path directory = temp_path;
    directory += L".parts";
    return directory;
}

Status create_job_scratch_directory(const std::filesystem::path& temp_path) {
    const std::filesystem::path directory = job_scratch_directory(temp_path);
    if (::CreateDirectoryW(detail::verbatim(directory).c_str(), nullptr)) {
        return ok();
    }
    const DWORD error = ::GetLastError();
    if (error == ERROR_ALREADY_EXISTS) {
        return fail(ErrorCode::AlreadyExists,
                    std::format("'{}' exists: an earlier attempt was not cleaned up",
                                core::path_to_utf8_lossy(directory)),
                    std::string(kWhere), static_cast<std::int64_t>(error));
    }
    return Unexpected(core::win32_error(
        error, std::string(kWhere), std::format("cannot create '{}'", core::path_to_utf8_lossy(directory))));
}

Status move_no_clobber(const std::filesystem::path& from, const std::filesystem::path& to) {
    // Without MOVEFILE_REPLACE_EXISTING the rename fails if anything exists at `to`, atomically.
    if (::MoveFileExW(detail::verbatim(from).c_str(), detail::verbatim(to).c_str(), MOVEFILE_WRITE_THROUGH)) {
        return ok();
    }
    const DWORD error = ::GetLastError();
    return Unexpected(core::win32_error(error, std::string(kWhere),
                                        std::format("cannot move '{}' to '{}'", core::path_to_utf8_lossy(from),
                                                    core::path_to_utf8_lossy(to))));
}

Status remove_job_scratch_directory(const std::filesystem::path& temp_path) {
    const std::filesystem::path directory = job_scratch_directory(temp_path);
    const std::wstring pattern = detail::verbatim(directory / L"*");
    WIN32_FIND_DATAW entry{};
    HANDLE find = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        if (is_not_found(error)) {
            return ok();
        }
        return Unexpected(core::win32_error(
            error, std::string(kWhere), std::format("cannot list '{}'", core::path_to_utf8_lossy(directory))));
    }
    Status first = ok();
    do {
        const std::wstring_view name = entry.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;  // never followed or removed; RemoveDirectoryW below then reports the leftover
        }
        if (auto removed = delete_file(directory / std::wstring(name)); !removed && first) {
            first = std::move(removed);
        }
    } while (::FindNextFileW(find, &entry));
    ::FindClose(find);
    if (!::RemoveDirectoryW(detail::verbatim(directory).c_str())) {
        const DWORD error = ::GetLastError();
        if (!is_not_found(error) && first) {
            first = Unexpected(core::win32_error(
                error, std::string(kWhere), std::format("cannot remove '{}'", core::path_to_utf8_lossy(directory))));
        }
    }
    return first;
}

Status remove_job_output(const std::filesystem::path& temp_path) {
    Status file = delete_file(temp_path);
    Status scratch = remove_job_scratch_directory(temp_path);
    return file ? scratch : file;
}

}  // namespace lucent::workers
