#include "file_util.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

#include <lucent/core/text.hpp>
#include <lucent/core/win32_error.hpp>

namespace lucent::exporter::detail {
namespace {

// Room for the 8.3-name rule that CreateDirectoryW applies (MAX_PATH - 12), as in lucent_core.
constexpr std::size_t kShortPathLimit = MAX_PATH - 12;

// Chunk size of write_file_bytes / compare_file_bytes (a cancellation check per chunk).
constexpr std::size_t kIoChunkBytes = std::size_t{4} << 20;

constexpr int kTransientRetries = 10;
constexpr std::chrono::milliseconds kTransientDelay{50};

class Handle {
public:
    explicit Handle(HANDLE handle) noexcept : handle_(handle) {}
    ~Handle() {
        if (valid()) {
            ::CloseHandle(handle_);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    [[nodiscard]] bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_;
};

std::string display(const std::filesystem::path& path) {
    return core::path_to_utf8_lossy(path);
}

}  // namespace

Expected<std::wstring> api_path(const std::filesystem::path& path, std::string_view where) {
    const std::wstring& native = path.native();
    if (native.empty()) {
        return fail(ErrorCode::InvalidArgument, "path is empty", std::string(where));
    }
    if (native.find(L'\0') != std::wstring::npos) {
        return fail(ErrorCode::InvalidArgument, "path contains a NUL character", std::string(where));
    }
    if (native.starts_with(L"\\\\?\\") || native.starts_with(L"\\\\.\\") || native.size() < kShortPathLimit) {
        return native;
    }
    const DWORD needed = ::GetFullPathNameW(native.c_str(), 0, nullptr, nullptr);
    if (needed == 0) {
        return Unexpected(core::last_error(std::string(where), "cannot resolve " + display(path)));
    }
    std::wstring full(needed, L'\0');
    const DWORD written = ::GetFullPathNameW(native.c_str(), needed, full.data(), nullptr);
    if (written == 0 || written >= needed) {
        return Unexpected(core::last_error(std::string(where), "cannot resolve " + display(path)));
    }
    full.resize(written);
    if (full.starts_with(L"\\\\")) {
        return L"\\\\?\\UNC\\" + full.substr(2);
    }
    return L"\\\\?\\" + full;
}

std::filesystem::path destination_directory(const std::filesystem::path& destination) {
    std::filesystem::path parent = destination.parent_path();
    if (parent.empty()) {
        return std::filesystem::path(L".");
    }
    return parent;
}

Status check_destination(const std::filesystem::path& destination) {
    constexpr const char* kWhere = "export.destination.check";
    if (!destination.has_filename()) {
        return fail(ErrorCode::InvalidArgument, "destination does not name a file: " + display(destination), kWhere);
    }
    auto target = api_path(destination, kWhere);
    if (!target) {
        return Unexpected(std::move(target.error()));
    }
    if (::GetFileAttributesW(target->c_str()) != INVALID_FILE_ATTRIBUTES) {
        return fail(ErrorCode::AlreadyExists, "destination already exists: " + display(destination), kWhere,
                    ERROR_FILE_EXISTS);
    }
    const DWORD target_error = ::GetLastError();
    if (target_error != ERROR_FILE_NOT_FOUND && target_error != ERROR_PATH_NOT_FOUND) {
        return Unexpected(core::win32_error(target_error, kWhere, "cannot inspect " + display(destination)));
    }
    const std::filesystem::path directory = destination_directory(destination);
    auto dir = api_path(directory, kWhere);
    if (!dir) {
        return Unexpected(std::move(dir.error()));
    }
    const DWORD attributes = ::GetFileAttributesW(dir->c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = ::GetLastError();
        return Unexpected(
            core::win32_error(error, kWhere, "destination directory is not accessible: " + display(directory)));
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return fail(ErrorCode::NotFound, "destination directory is not a directory: " + display(directory), kWhere);
    }
    return ok();
}

Expected<std::filesystem::path> reserve_temporary_sibling(const std::filesystem::path& directory) {
    constexpr const char* kWhere = "export.temp.reserve";
    std::random_device random;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const std::uint64_t token = (std::uint64_t{random()} << 32) ^ std::uint64_t{random()};
        wchar_t name[40];
        std::swprintf(name, std::size(name), L"~lucent-%016llx.tmp", static_cast<unsigned long long>(token));
        std::filesystem::path candidate = directory / name;
        auto native = api_path(candidate, kWhere);
        if (!native) {
            return Unexpected(std::move(native.error()));
        }
        Handle handle(::CreateFileW(native->c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr));
        if (handle.valid()) {
            return candidate;
        }
        const DWORD error = ::GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            return Unexpected(
                core::win32_error(error, kWhere, "cannot create a temporary file in " + display(directory)));
        }
    }
    return fail(ErrorCode::IoError, "cannot find a free temporary file name in " + display(directory), kWhere);
}

Status flush_to_disk(const std::filesystem::path& path) {
    constexpr const char* kWhere = "export.temp.flush";
    auto native = api_path(path, kWhere);
    if (!native) {
        return Unexpected(std::move(native.error()));
    }
    Handle handle(::CreateFileW(native->c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle.valid()) {
        return Unexpected(core::last_error(kWhere, "cannot open " + display(path) + " to flush it"));
    }
    if (!::FlushFileBuffers(handle.get())) {
        return Unexpected(core::last_error(kWhere, "cannot flush " + display(path)));
    }
    return ok();
}

Status write_file_bytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                        const core::CancellationToken& cancel) {
    constexpr const char* kWhere = "export.file.write";
    auto native = api_path(path, kWhere);
    if (!native) {
        return Unexpected(std::move(native.error()));
    }
    Handle handle(::CreateFileW(native->c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle.valid()) {
        return Unexpected(core::last_error(kWhere, "cannot open " + display(path) + " for writing"));
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (auto status = cancel.check(kWhere); !status) {
            return status;
        }
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, kIoChunkBytes));
        DWORD written = 0;
        if (!::WriteFile(handle.get(), bytes.data() + offset, chunk, &written, nullptr)) {
            return Unexpected(core::last_error(kWhere, "cannot write " + display(path)));
        }
        if (written == 0) {
            return fail(ErrorCode::IoError, "the system wrote no bytes to " + display(path), kWhere);
        }
        offset += written;
    }
    if (!::FlushFileBuffers(handle.get())) {
        return Unexpected(core::last_error(kWhere, "cannot flush " + display(path)));
    }
    return ok();
}

Status compare_file_bytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                          const core::CancellationToken& cancel) {
    constexpr const char* kWhere = "export.file.compare";
    auto native = api_path(path, kWhere);
    if (!native) {
        return Unexpected(std::move(native.error()));
    }
    Handle handle(::CreateFileW(native->c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!handle.valid()) {
        return Unexpected(core::last_error(kWhere, "cannot reopen " + display(path)));
    }
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(handle.get(), &size)) {
        return Unexpected(core::last_error(kWhere, "cannot inspect " + display(path)));
    }
    if (static_cast<std::uint64_t>(size.QuadPart) != bytes.size()) {
        return fail(ErrorCode::Corrupt,
                    "written file failed verification: " + std::to_string(size.QuadPart) + " bytes instead of " +
                        std::to_string(bytes.size()),
                    kWhere);
    }
    std::vector<std::uint8_t> buffer(std::min<std::size_t>(bytes.size(), kIoChunkBytes));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (auto status = cancel.check(kWhere); !status) {
            return status;
        }
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, buffer.size()));
        DWORD got = 0;
        if (!::ReadFile(handle.get(), buffer.data(), chunk, &got, nullptr)) {
            return Unexpected(core::last_error(kWhere, "cannot read back " + display(path)));
        }
        if (got == 0) {
            return fail(ErrorCode::Corrupt, "written file failed verification: it ends early", kWhere);
        }
        const auto* expected = bytes.data() + offset;
        const auto mismatch = std::mismatch(buffer.begin(), buffer.begin() + got, expected);
        if (mismatch.first != buffer.begin() + got) {
            return fail(ErrorCode::Corrupt,
                        "written file failed verification: byte " +
                            std::to_string(offset + static_cast<std::size_t>(mismatch.first - buffer.begin())) +
                            " differs from the encoder's output",
                        kWhere);
        }
        offset += got;
    }
    return ok();
}

Status publish_no_clobber(const std::filesystem::path& from, const std::filesystem::path& to) {
    constexpr const char* kWhere = "export.publish";
    auto source = api_path(from, kWhere);
    if (!source) {
        return Unexpected(std::move(source.error()));
    }
    auto target = api_path(to, kWhere);
    if (!target) {
        return Unexpected(std::move(target.error()));
    }
    for (int attempt = 0;; ++attempt) {
        // No MOVEFILE_REPLACE_EXISTING: an existing target makes the call fail, atomically.
        if (::MoveFileExW(source->c_str(), target->c_str(), MOVEFILE_WRITE_THROUGH)) {
            return ok();
        }
        const DWORD error = ::GetLastError();
        if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
            return Unexpected(core::win32_error(error, kWhere, "destination already exists: " + display(to)));
        }
        if (!core::is_transient_win32_error(error) || attempt + 1 >= kTransientRetries) {
            return Unexpected(core::win32_error(error, kWhere, "cannot publish " + display(to)));
        }
        std::this_thread::sleep_for(kTransientDelay);
    }
}

bool remove_file_quietly(const std::filesystem::path& path) noexcept {
    try {
        auto native = api_path(path, "export.temp.remove");
        if (!native) {
            return false;
        }
        for (int attempt = 0;; ++attempt) {
            if (::DeleteFileW(native->c_str())) {
                return true;
            }
            const DWORD error = ::GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                return true;
            }
            if ((!core::is_transient_win32_error(error) && error != ERROR_ACCESS_DENIED) ||
                attempt + 1 >= kTransientRetries) {
                return false;
            }
            std::this_thread::sleep_for(kTransientDelay);
        }
    } catch (...) {
        return false;
    }
}

TemporaryFile::~TemporaryFile() {
    if (!released_ && !path_.empty()) {
        (void)remove_file_quietly(path_);
    }
}

Error crt_file_error(std::string where, std::string context) {
    unsigned long dos_error = 0;
    (void)_get_doserrno(&dos_error);
    if (dos_error != 0) {
        return core::win32_error(static_cast<std::uint32_t>(dos_error), std::move(where), std::move(context));
    }
    int crt_errno = 0;
    (void)_get_errno(&crt_errno);
    char text[128] = {};
    (void)strerror_s(text, sizeof(text), crt_errno);
    ErrorCode code = ErrorCode::IoError;
    switch (crt_errno) {
        case ENOENT:
            code = ErrorCode::NotFound;
            break;
        case EACCES:
            code = ErrorCode::PermissionDenied;
            break;
        case ENOSPC:
            code = ErrorCode::DiskFull;
            break;
        case ENOMEM:
            code = ErrorCode::OutOfMemory;
            break;
        default:
            break;
    }
    return Error{code, context + ": " + text, std::move(where), crt_errno};
}

}  // namespace lucent::exporter::detail
