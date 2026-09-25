#include "lucent/core/file.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <iterator>
#include <limits>
#include <new>
#include <utility>

#include "lucent/core/win32_error.hpp"

#include "win32.hpp"

namespace lucent::core {

namespace {

// See file.hpp: never block other readers, writers or renames/deletes of the user's files.
constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

// Largest single ReadFile request. Larger reads are split; some network redirectors reject
// very large requests with ERROR_NO_SYSTEM_RESOURCES.
constexpr std::size_t kMaxReadPerCall = std::size_t{32} << 20;  // 32 MiB

constexpr std::uint64_t kMaxOffset = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

HANDLE as_handle(void* handle) noexcept {
    return static_cast<HANDLE>(handle);
}

std::int64_t filetime_ticks(const FILETIME& time) noexcept {
    return static_cast<std::int64_t>((static_cast<std::uint64_t>(time.dwHighDateTime) << 32) |
                                     time.dwLowDateTime);
}

Expected<FileIdentity> query_identity(HANDLE handle, const std::filesystem::path& path, const char* where) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(handle, &info)) {
        const DWORD code = ::GetLastError();
        return Unexpected(
            win32_error(code, where, "cannot query file information for " + detail::quoted(path)));
    }
    FileIdentity identity;
    identity.size_bytes = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    identity.last_write_time = filetime_ticks(info.ftLastWriteTime);

    FILE_ID_INFO id_info{};
    if (::GetFileInformationByHandleEx(handle, FileIdInfo, &id_info, sizeof(id_info))) {
        identity.volume_serial = id_info.VolumeSerialNumber;
        static_assert(sizeof(id_info.FileId.Identifier) == std::tuple_size_v<decltype(identity.file_id)>);
        std::memcpy(identity.file_id.data(), id_info.FileId.Identifier, identity.file_id.size());
    } else {
        // Documented fallback (file.hpp): 32-bit serial and 64-bit index, zero-extended.
        identity.volume_serial = info.dwVolumeSerialNumber;
        const std::uint64_t index =
            (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        for (std::size_t i = 0; i < 8; ++i) {
            identity.file_id[i] = static_cast<std::uint8_t>(index >> (8 * i));
        }
    }
    return identity;
}

}  // namespace

// ---- FileIdentity -----------------------------------------------------------------------

bool is_same_file(const FileIdentity& a, const FileIdentity& b) noexcept {
    return a.volume_serial == b.volume_serial && a.file_id == b.file_id;
}

std::string to_string(const FileIdentity& identity) {
    std::string id_hex;
    id_hex.reserve(identity.file_id.size() * 2);
    for (const std::uint8_t byte : identity.file_id) {
        std::format_to(std::back_inserter(id_hex), "{:02x}", byte);
    }
    return std::format("volume={:016x} id={} size={} mtime={}", identity.volume_serial, id_hex,
                       identity.size_bytes, identity.last_write_time);
}

Expected<FileIdentity> stat_file(const std::filesystem::path& path) {
    constexpr const char* kWhere = "core.file.stat";
    auto api_path = detail::win32_api_path(path, kWhere);
    if (!api_path) {
        return Unexpected(std::move(api_path.error()));
    }
    // Attribute-only access is exempt from share-mode checks, so this never conflicts with
    // other openers. FILE_FLAG_BACKUP_SEMANTICS is required to open directories.
    detail::UniqueHandle handle(::CreateFileW(api_path->c_str(), FILE_READ_ATTRIBUTES, kShareAll, nullptr,
                                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!handle) {
        const DWORD code = ::GetLastError();
        return Unexpected(win32_error(code, kWhere, "cannot query " + detail::quoted(path)));
    }
    return query_identity(handle.get(), path, kWhere);
}

// ---- ReadOnlyFile -----------------------------------------------------------------------

ReadOnlyFile::ReadOnlyFile(void* handle, std::filesystem::path path) noexcept
    : handle_(handle), path_(std::move(path)) {}

ReadOnlyFile::~ReadOnlyFile() {
    close();
}

ReadOnlyFile::ReadOnlyFile(ReadOnlyFile&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)), path_(std::move(other.path_)) {
    other.path_.clear();
}

ReadOnlyFile& ReadOnlyFile::operator=(ReadOnlyFile&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, nullptr);
        path_ = std::move(other.path_);
        other.path_.clear();
    }
    return *this;
}

void ReadOnlyFile::close() noexcept {
    if (handle_ != nullptr) {
        ::CloseHandle(as_handle(handle_));
        handle_ = nullptr;
    }
    path_.clear();
}

Expected<ReadOnlyFile> ReadOnlyFile::open(const std::filesystem::path& path, AccessHint hint) {
    constexpr const char* kWhere = "core.file.open";
    auto api_path = detail::win32_api_path(path, kWhere);
    if (!api_path) {
        return Unexpected(std::move(api_path.error()));
    }

    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    switch (hint) {
        case AccessHint::Normal:
            break;
        case AccessHint::Sequential:
            flags |= FILE_FLAG_SEQUENTIAL_SCAN;
            break;
        case AccessHint::Random:
            flags |= FILE_FLAG_RANDOM_ACCESS;
            break;
    }

    detail::UniqueHandle handle(
        ::CreateFileW(api_path->c_str(), GENERIC_READ, kShareAll, nullptr, OPEN_EXISTING, flags, nullptr));
    if (!handle) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_ACCESS_DENIED) {
            // Without FILE_FLAG_BACKUP_SEMANTICS a directory fails with ACCESS_DENIED; report
            // it as what it is rather than as a permission problem.
            const DWORD attributes = ::GetFileAttributesW(api_path->c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                return fail(ErrorCode::InvalidArgument, detail::quoted(path) + " is a directory, not a file",
                            kWhere, code);
            }
        }
        return Unexpected(win32_error(code, kWhere, "cannot open " + detail::quoted(path) + " for reading"));
    }

    if (::GetFileType(handle.get()) != FILE_TYPE_DISK) {
        return fail(ErrorCode::Unsupported,
                    detail::quoted(path) + " is not a regular file (it is a device, pipe or console)",
                    kWhere);
    }
    return ReadOnlyFile(handle.release(), path);
}

Expected<std::uint64_t> ReadOnlyFile::size() const {
    constexpr const char* kWhere = "core.file.size";
    if (handle_ == nullptr) {
        return fail(ErrorCode::Internal, "size() called on a closed ReadOnlyFile", kWhere);
    }
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(as_handle(handle_), &size)) {
        const DWORD code = ::GetLastError();
        return Unexpected(win32_error(code, kWhere, "cannot query the size of " + detail::quoted(path_)));
    }
    return static_cast<std::uint64_t>(size.QuadPart);
}

Expected<FileIdentity> ReadOnlyFile::identity() const {
    constexpr const char* kWhere = "core.file.identity";
    if (handle_ == nullptr) {
        return fail(ErrorCode::Internal, "identity() called on a closed ReadOnlyFile", kWhere);
    }
    return query_identity(as_handle(handle_), path_, kWhere);
}

Expected<bool> ReadOnlyFile::is_delete_pending() const {
    constexpr const char* kWhere = "core.file.is_delete_pending";
    if (handle_ == nullptr) {
        return fail(ErrorCode::Internal, "is_delete_pending() called on a closed ReadOnlyFile", kWhere);
    }
    FILE_STANDARD_INFO info{};
    if (!::GetFileInformationByHandleEx(as_handle(handle_), FileStandardInfo, &info, sizeof(info))) {
        const DWORD code = ::GetLastError();
        return Unexpected(win32_error(code, kWhere, "cannot query the state of " + detail::quoted(path_)));
    }
    return info.DeletePending != FALSE;
}

Expected<std::size_t> ReadOnlyFile::read_at(std::uint64_t offset, std::span<std::byte> buffer) const {
    constexpr const char* kWhere = "core.file.read";
    if (handle_ == nullptr) {
        return fail(ErrorCode::Internal, "read_at() called on a closed ReadOnlyFile", kWhere);
    }
    if (offset > kMaxOffset) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("read offset {} exceeds the largest file offset", offset), kWhere);
    }
    // No file can extend past INT64_MAX, so never request bytes beyond it.
    const std::size_t wanted =
        static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), kMaxOffset - offset));

    std::size_t total = 0;
    while (total < wanted) {
        const std::uint64_t position = offset + total;
        const DWORD request = static_cast<DWORD>(std::min(wanted - total, kMaxReadPerCall));
        // On a synchronous handle, ReadFile with an OVERLAPPED reads at the given offset and
        // returns when complete. Each call has its own OVERLAPPED, so concurrent calls never
        // share position state.
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(position & 0xFFFF'FFFFu);
        overlapped.OffsetHigh = static_cast<DWORD>(position >> 32);
        DWORD transferred = 0;
        if (!::ReadFile(as_handle(handle_), buffer.data() + total, request, &transferred, &overlapped)) {
            const DWORD code = ::GetLastError();
            if (code == ERROR_HANDLE_EOF) {
                break;  // offset at or beyond end-of-file
            }
            return Unexpected(
                win32_error(code, kWhere,
                            std::format("cannot read {} bytes at offset {} of ", request, position) +
                                detail::quoted(path_)));
        }
        if (transferred == 0) {
            break;  // end-of-file
        }
        total += transferred;
    }
    return total;
}

Status ReadOnlyFile::read_exact_at(std::uint64_t offset, std::span<std::byte> buffer) const {
    auto read = read_at(offset, buffer);
    if (!read) {
        return Unexpected(std::move(read.error()));
    }
    if (*read != buffer.size()) {
        return fail(
            ErrorCode::Corrupt,
            std::format("unexpected end of file: needed {} bytes at offset {} but only {} are available in ",
                        buffer.size(), offset, *read) +
                detail::quoted(path_),
            "core.file.read");
    }
    return ok();
}

// ---- read_file_bytes --------------------------------------------------------------------

Expected<std::vector<std::byte>> read_file_bytes(const std::filesystem::path& path, std::uint64_t max_bytes) {
    constexpr const char* kWhere = "core.file.read_file_bytes";
    auto file = ReadOnlyFile::open(path, AccessHint::Sequential);
    if (!file) {
        return Unexpected(std::move(file.error()));
    }
    auto before = file->identity();
    if (!before) {
        return Unexpected(std::move(before.error()));
    }
    if (before->size_bytes > max_bytes) {
        return fail(ErrorCode::LimitExceeded,
                    std::format("file is {} bytes, more than the limit of {} bytes: ", before->size_bytes,
                                max_bytes) +
                        detail::quoted(path),
                    kWhere);
    }
    if (before->size_bytes > std::numeric_limits<std::size_t>::max()) {
        return fail(ErrorCode::LimitExceeded,
                    "file does not fit in the address space: " + detail::quoted(path), kWhere);
    }

    std::vector<std::byte> bytes;
    try {
        bytes.resize(static_cast<std::size_t>(before->size_bytes));
    } catch (const std::bad_alloc&) {
        return fail(
            ErrorCode::OutOfMemory,
            std::format("cannot allocate {} bytes to read ", before->size_bytes) + detail::quoted(path),
            kWhere);
    }

    auto read = file->read_at(0, bytes);
    if (!read) {
        return Unexpected(std::move(read.error()));
    }
    auto after = file->identity();
    if (!after) {
        return Unexpected(std::move(after.error()));
    }
    if (*read != bytes.size() || *after != *before) {
        return fail(ErrorCode::SourceChanged,
                    "source file changed while it was being read: " + detail::quoted(path) +
                        std::format(" (before: {}; after: {}; bytes read: {})", to_string(*before),
                                    to_string(*after), *read),
                    kWhere);
    }
    return bytes;
}

}  // namespace lucent::core
