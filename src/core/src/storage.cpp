#include "lucent/core/storage.hpp"

// clang-format off: <winioctl.h> requires the types declared by <windows.h>.
#include <windows.h>
#include <winioctl.h>
// clang-format on

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <format>
#include <limits>
#include <new>
#include <random>
#include <vector>

#include "lucent/core/win32_error.hpp"

#include "storage_internal.hpp"
#include "win32.hpp"

namespace lucent::core {

namespace detail {

namespace {

constexpr std::wstring_view kVerbatimUnc = L"\\\\?\\UNC\\";
constexpr std::wstring_view kVerbatim = L"\\\\?\\";
constexpr std::wstring_view kDevice = L"\\\\.\\";

wchar_t ascii_upper(wchar_t c) noexcept {
    return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - L'a' + L'A') : c;
}

bool is_drive_letter(wchar_t c) noexcept {
    const wchar_t u = ascii_upper(c);
    return u >= L'A' && u <= L'Z';
}

bool starts_with_ci(std::wstring_view text, std::wstring_view prefix) noexcept {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (ascii_upper(text[i]) != ascii_upper(prefix[i])) {
            return false;
        }
    }
    return true;
}

// "server\share[\...]" -> "\\server\share\"; None when either component is empty.
VolumeRoot unc_root(std::wstring_view rest) {
    const std::size_t server_end = rest.find(L'\\');
    if (server_end == std::wstring_view::npos || server_end == 0) {
        return {};
    }
    const std::wstring_view after = rest.substr(server_end + 1);
    const std::size_t share_end = std::min(after.find(L'\\'), after.size());
    if (share_end == 0) {
        return {};
    }
    std::wstring root = L"\\\\";
    root.append(rest.substr(0, server_end));
    root.push_back(L'\\');
    root.append(after.substr(0, share_end));
    root.push_back(L'\\');
    return {VolumeRootKind::Unc, std::move(root)};
}

// "X:[\...]" -> "X:\"; None otherwise.
VolumeRoot drive_root(std::wstring_view text) {
    if (text.size() >= 2 && is_drive_letter(text[0]) && text[1] == L':' &&
        (text.size() == 2 || text[2] == L'\\')) {
        return {VolumeRootKind::DriveLetter, std::wstring{ascii_upper(text[0]), L':', L'\\'}};
    }
    return {};
}

}  // namespace

VolumeRoot lexical_volume_root(std::wstring_view absolute_path) {
    if (starts_with_ci(absolute_path, kVerbatimUnc)) {
        return unc_root(absolute_path.substr(kVerbatimUnc.size()));
    }
    if (absolute_path.starts_with(kVerbatim) || absolute_path.starts_with(kDevice)) {
        const std::wstring_view rest = absolute_path.substr(kVerbatim.size());
        if (VolumeRoot drive = drive_root(rest); drive.kind != VolumeRootKind::None) {
            return drive;
        }
        // "Volume{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" is 44 characters.
        constexpr std::size_t kVolumeNameChars = 44;
        if (starts_with_ci(rest, L"Volume{") && rest.size() >= kVolumeNameChars &&
            rest[kVolumeNameChars - 1] == L'}' &&
            (rest.size() == kVolumeNameChars || rest[kVolumeNameChars] == L'\\')) {
            std::wstring root(kVerbatim);
            root.append(rest.substr(0, kVolumeNameChars));
            root.push_back(L'\\');
            return {VolumeRootKind::Volume, std::move(root)};
        }
        return {};  // other device paths: GLOBALROOT, PhysicalDrive, pipes, ...
    }
    std::wstring normalised(absolute_path);
    std::replace(normalised.begin(), normalised.end(), L'/', L'\\');
    if (normalised.starts_with(L"\\\\")) {
        return unc_root(std::wstring_view(normalised).substr(2));
    }
    return drive_root(normalised);
}

StorageKind classify_storage(std::uint32_t drive_type,
                             const std::optional<StorageDeviceInfo>& device) noexcept {
    switch (drive_type) {
        case DRIVE_REMOVABLE:
        case DRIVE_CDROM:
            return StorageKind::Removable;
        case DRIVE_REMOTE:
            return StorageKind::Network;
        case DRIVE_FIXED:
        case DRIVE_RAMDISK:
            if (device) {
                switch (static_cast<STORAGE_BUS_TYPE>(device->bus_type)) {
                    case BusTypeUsb:
                    case BusTypeSd:
                    case BusTypeMmc:
                    case BusType1394:
                        return StorageKind::Removable;
                    default:
                        break;
                }
                if (device->removable_media) {
                    return StorageKind::Removable;
                }
            }
            return StorageKind::Fixed;
        default:  // DRIVE_UNKNOWN, DRIVE_NO_ROOT_DIR, anything new
            return StorageKind::Unknown;
    }
}

// ---- temporary files ------------------------------------------------------------------------

std::wstring make_temporary_file_name() {
    // std::random_device is non-deterministic on MSVC (rand_s); the counter separates names
    // generated in quick succession even if it were not.
    static std::atomic<std::uint64_t> counter{0};
    thread_local std::random_device device;
    const std::uint64_t random = (static_cast<std::uint64_t>(device()) << 32) ^ device();
    const std::uint64_t value =
        random ^ (counter.fetch_add(1, std::memory_order_relaxed) * 0x9E3779B97F4A7C15ull);
    return std::format(L".lucent-{:016x}.tmp", value);
}

namespace {

// Layout-compatible with FILE_RENAME_INFO in both SDK variants (the union of ReplaceIfExists and
// Flags exists only when the SDK targets Windows 10 RS1).
struct RenameInfoHeader {
    DWORD flags;  // ReplaceIfExists (BOOLEAN) or FILE_RENAME_FLAG_* (DWORD)
    HANDLE root_directory;
    DWORD file_name_length;  // bytes, without terminator
    WCHAR file_name[1];
};
static_assert(offsetof(RenameInfoHeader, root_directory) == offsetof(FILE_RENAME_INFO, RootDirectory));
static_assert(offsetof(RenameInfoHeader, file_name_length) == offsetof(FILE_RENAME_INFO, FileNameLength));
static_assert(offsetof(RenameInfoHeader, file_name) == offsetof(FILE_RENAME_INFO, FileName));

constexpr DWORD kRenameReplaceIfExists = 0x1;  // FILE_RENAME_FLAG_REPLACE_IF_EXISTS
constexpr DWORD kRenamePosixSemantics = 0x2;   // FILE_RENAME_FLAG_POSIX_SEMANTICS
constexpr auto kFileRenameInfoEx = static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);  // FileRenameInfoEx

DWORD set_rename_info(HANDLE handle, const std::wstring& target, FILE_INFO_BY_HANDLE_CLASS info_class,
                      DWORD flags) {
    const std::size_t name_bytes = target.size() * sizeof(wchar_t);
    if (name_bytes > std::numeric_limits<DWORD>::max() / 2) {
        return ERROR_FILENAME_EXCED_RANGE;
    }
    const std::size_t total = offsetof(RenameInfoHeader, file_name) + name_bytes + sizeof(wchar_t);
    std::vector<std::byte> buffer;
    try {
        buffer.resize(std::max(total, sizeof(RenameInfoHeader)));
    } catch (const std::bad_alloc&) {
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    auto* info = reinterpret_cast<RenameInfoHeader*>(buffer.data());
    info->flags = flags;
    info->root_directory = nullptr;
    info->file_name_length = static_cast<DWORD>(name_bytes);
    std::memcpy(info->file_name, target.c_str(), name_bytes + sizeof(wchar_t));
    if (!::SetFileInformationByHandle(handle, info_class, buffer.data(), static_cast<DWORD>(buffer.size()))) {
        return ::GetLastError();
    }
    return ERROR_SUCCESS;
}

}  // namespace

DWORD rename_by_handle(HANDLE handle, const std::wstring& target_api_path, bool replace) {
    if (!replace) {
        return set_rename_info(handle, target_api_path, FileRenameInfo, 0);
    }
    const DWORD posix = set_rename_info(handle, target_api_path, kFileRenameInfoEx,
                                        kRenameReplaceIfExists | kRenamePosixSemantics);
    if (posix == ERROR_INVALID_PARAMETER || posix == ERROR_NOT_SUPPORTED || posix == ERROR_INVALID_FUNCTION) {
        // FAT/exFAT, some redirectors and older systems: classic replace.
        return set_rename_info(handle, target_api_path, FileRenameInfo, TRUE);
    }
    return posix;
}

DWORD delete_by_handle(HANDLE handle) noexcept {
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    if (!::SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition))) {
        return ::GetLastError();
    }
    return ERROR_SUCCESS;
}

DWORD write_all(HANDLE handle, const void* data, std::size_t size) noexcept {
    constexpr std::size_t kMaxPiece = std::size_t{1} << 30;
    const auto* bytes = static_cast<const std::byte*>(data);
    while (size > 0) {
        const DWORD piece = static_cast<DWORD>(std::min(size, kMaxPiece));
        DWORD written = 0;
        if (!::WriteFile(handle, bytes, piece, &written, nullptr)) {
            return ::GetLastError();
        }
        if (written == 0) {
            return ERROR_WRITE_FAULT;
        }
        bytes += written;
        size -= written;
    }
    return ERROR_SUCCESS;
}

void TemporaryFile::discard() noexcept {
    if (!handle_) {
        return;
    }
    const bool deleted = delete_by_handle(handle_.get()) == ERROR_SUCCESS;
    handle_.reset();
    if (!deleted && !api_path_.empty()) {
        ::DeleteFileW(api_path_.c_str());
    }
}

Expected<TemporaryFile> create_temporary_file(const std::filesystem::path& directory, DWORD access,
                                              DWORD share, std::string_view where) {
    constexpr int kAttempts = 16;
    DWORD last = ERROR_FILE_EXISTS;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        const std::filesystem::path candidate = directory / make_temporary_file_name();
        auto api = win32_api_path(candidate, where);
        if (!api) {
            return Unexpected(std::move(api.error()));
        }
        UniqueHandle handle(::CreateFileW(api->c_str(), access | DELETE, share, nullptr, CREATE_NEW,
                                          FILE_ATTRIBUTE_NORMAL, nullptr));
        if (handle) {
            return TemporaryFile(std::move(handle), std::move(*api));
        }
        last = ::GetLastError();
        if (last != ERROR_FILE_EXISTS && last != ERROR_ALREADY_EXISTS) {
            break;
        }
    }
    return Unexpected(
        win32_error(last, std::string(where), "cannot create a temporary file in " + quoted(directory)));
}

}  // namespace detail

// ---- public API -----------------------------------------------------------------------------

std::string_view to_string(StorageKind kind) noexcept {
    switch (kind) {
        case StorageKind::Unknown:
            return "unknown";
        case StorageKind::Fixed:
            return "fixed";
        case StorageKind::Removable:
            return "removable";
        case StorageKind::Network:
            return "network";
    }
    return "unknown";
}

namespace {

// Suppresses "There is no disk in the drive" dialogs for the calling thread only.
class ThreadErrorModeScope {
public:
    ThreadErrorModeScope() noexcept {
        changed_ = ::SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous_) != FALSE;
    }
    ~ThreadErrorModeScope() {
        if (changed_) {
            ::SetThreadErrorMode(previous_, nullptr);
        }
    }
    ThreadErrorModeScope(const ThreadErrorModeScope&) = delete;
    ThreadErrorModeScope& operator=(const ThreadErrorModeScope&) = delete;

private:
    DWORD previous_ = 0;
    bool changed_ = false;
};

std::optional<detail::StorageDeviceInfo> query_device(const std::wstring& root) {
    std::array<wchar_t, 64> volume_name{};
    if (!::GetVolumeNameForVolumeMountPointW(root.c_str(), volume_name.data(),
                                             static_cast<DWORD>(volume_name.size()))) {
        return std::nullopt;
    }
    std::wstring device(volume_name.data());
    if (device.ends_with(L'\\')) {
        device.pop_back();  // a volume device is opened without the trailing backslash
    }
    // Zero access rights: metadata IOCTLs only, no elevation needed, no conflict with others.
    detail::UniqueHandle handle(::CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                              OPEN_EXISTING, 0, nullptr));
    if (!handle) {
        return std::nullopt;
    }
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    STORAGE_DEVICE_DESCRIPTOR descriptor{};
    DWORD returned = 0;
    if (!::DeviceIoControl(handle.get(), IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &descriptor,
                           sizeof(descriptor), &returned, nullptr) ||
        returned < offsetof(STORAGE_DEVICE_DESCRIPTOR, BusType) + sizeof(descriptor.BusType)) {
        return std::nullopt;
    }
    return detail::StorageDeviceInfo{static_cast<std::uint32_t>(descriptor.BusType),
                                     descriptor.RemovableMedia != FALSE};
}

bool same_root(const std::wstring& a, const std::wstring& b) {
    return ::CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                  static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

}  // namespace

StorageKind storage_kind(const std::filesystem::path& path) {
    try {
        if (path.empty()) {
            return StorageKind::Unknown;
        }
        std::error_code error;
        const std::filesystem::path absolute =
            path.is_absolute() ? path : std::filesystem::absolute(path, error);
        if (error) {
            return StorageKind::Unknown;
        }
        const detail::VolumeRoot lexical = detail::lexical_volume_root(absolute.native());
        switch (lexical.kind) {
            case detail::VolumeRootKind::None:
                return StorageKind::Unknown;
            case detail::VolumeRootKind::Unc:
                return StorageKind::Network;
            case detail::VolumeRootKind::DriveLetter:
            case detail::VolumeRootKind::Volume:
                break;
        }

        const ThreadErrorModeScope error_mode;
        UINT drive_type = ::GetDriveTypeW(lexical.root.c_str());
        if (drive_type == DRIVE_REMOTE) {
            return StorageKind::Network;
        }
        if (drive_type == DRIVE_UNKNOWN || drive_type == DRIVE_NO_ROOT_DIR) {
            return StorageKind::Unknown;
        }

        // A different volume may be mounted in a folder on the path.
        std::wstring root = lexical.root;
        auto api = detail::win32_api_path(absolute, "core.storage.kind");
        if (api) {
            std::wstring mount(api->size() + 2, L'\0');
            if (::GetVolumePathNameW(api->c_str(), mount.data(), static_cast<DWORD>(mount.size()))) {
                mount.resize(std::wcslen(mount.c_str()));
                if (!mount.empty() && !same_root(mount, root)) {
                    const UINT mounted_type = ::GetDriveTypeW(mount.c_str());
                    if (mounted_type != DRIVE_UNKNOWN && mounted_type != DRIVE_NO_ROOT_DIR) {
                        drive_type = mounted_type;
                        root = std::move(mount);
                    }
                }
            }
        }
        if (drive_type == DRIVE_REMOTE) {
            return StorageKind::Network;
        }
        return detail::classify_storage(drive_type, query_device(root));
    } catch (...) {
        // std::bad_alloc from path handling; classification is best-effort by contract.
        return StorageKind::Unknown;
    }
}

Expected<std::uint64_t> available_disk_bytes(const std::filesystem::path& directory) {
    constexpr const char* kWhere = "core.storage.available_bytes";
    auto api = detail::win32_api_path(directory, kWhere);
    if (!api) {
        return Unexpected(std::move(api.error()));
    }
    std::wstring query = *api;
    if (!query.ends_with(L'\\')) {
        query.push_back(L'\\');  // required for UNC roots, harmless elsewhere
    }
    ULARGE_INTEGER available{};
    if (!::GetDiskFreeSpaceExW(query.c_str(), &available, nullptr, nullptr)) {
        const DWORD code = ::GetLastError();
        return Unexpected(
            win32_error(code, kWhere, "cannot query free space of " + detail::quoted(directory)));
    }
    return static_cast<std::uint64_t>(available.QuadPart);
}

Status create_directory_tree(const std::filesystem::path& directory) {
    constexpr const char* kWhere = "core.storage.create_directory_tree";
    if (directory.empty()) {
        return fail(ErrorCode::InvalidArgument, "directory path is empty", kWhere);
    }
    std::error_code error;
    const std::filesystem::path absolute =
        directory.is_absolute() ? directory : std::filesystem::absolute(directory, error);
    if (error) {
        return fail(ErrorCode::InvalidArgument, "cannot make " + detail::quoted(directory) + " absolute",
                    kWhere);
    }

    // Walk up to the deepest existing ancestor, then create downwards.
    std::vector<std::filesystem::path> missing;
    std::filesystem::path current = absolute;
    for (;;) {
        auto api = detail::win32_api_path(current, kWhere);
        if (!api) {
            return Unexpected(std::move(api.error()));
        }
        const DWORD attributes = ::GetFileAttributesW(api->c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                return fail(ErrorCode::AlreadyExists,
                            detail::quoted(current) + " exists and is not a directory", kWhere,
                            ERROR_ALREADY_EXISTS);
            }
            break;
        }
        const DWORD code = ::GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
            return Unexpected(win32_error(code, kWhere, "cannot inspect " + detail::quoted(current)));
        }
        missing.push_back(current);
        const std::filesystem::path parent = current.parent_path();
        if (parent == current || parent.empty() || current == current.root_path()) {
            return Unexpected(
                win32_error(code, kWhere, "the volume of " + detail::quoted(directory) + " does not exist"));
        }
        current = parent;
    }

    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        auto api = detail::win32_api_path(*it, kWhere);
        if (!api) {
            return Unexpected(std::move(api.error()));
        }
        if (!::CreateDirectoryW(api->c_str(), nullptr)) {
            const DWORD code = ::GetLastError();
            if (code == ERROR_ALREADY_EXISTS) {
                // Created concurrently, or a file appeared: only a directory is acceptable.
                const DWORD attributes = ::GetFileAttributesW(api->c_str());
                if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    continue;
                }
            }
            return Unexpected(win32_error(code, kWhere, "cannot create directory " + detail::quoted(*it)));
        }
    }
    return ok();
}

Status write_file_atomic(const std::filesystem::path& target, std::span<const std::byte> bytes,
                         const AtomicWriteOptions& options) {
    constexpr const char* kWhere = "core.storage.write_atomic";
    if (target.empty() || !target.has_filename()) {
        return fail(ErrorCode::InvalidArgument, "target path has no file name: " + detail::quoted(target),
                    kWhere);
    }
    auto target_api = detail::win32_api_path(target, kWhere);
    if (!target_api) {
        return Unexpected(std::move(target_api.error()));
    }
    std::filesystem::path parent = target.parent_path();
    if (parent.empty()) {
        parent = L".";
    }

    auto temporary = detail::create_temporary_file(parent, GENERIC_WRITE, 0, kWhere);
    if (!temporary) {
        return Unexpected(std::move(temporary.error()));
    }
    if (const DWORD code = detail::write_all(temporary->get(), bytes.data(), bytes.size());
        code != ERROR_SUCCESS) {
        return Unexpected(
            win32_error(code, kWhere, "cannot write a temporary file for " + detail::quoted(target)));
    }
    if (options.flush && !::FlushFileBuffers(temporary->get())) {
        const DWORD code = ::GetLastError();
        return Unexpected(
            win32_error(code, kWhere, "cannot flush a temporary file for " + detail::quoted(target)));
    }
    const bool replace = options.existing == ExistingFile::Replace;
    if (const DWORD code = detail::rename_by_handle(temporary->get(), *target_api, replace);
        code != ERROR_SUCCESS) {
        return Unexpected(win32_error(code, kWhere, "cannot publish " + detail::quoted(target)));
    }
    temporary->keep();
    return ok();
}

bool is_temporary_file_name(const std::filesystem::path& file_name) noexcept {
    const std::wstring& name = file_name.native();
    constexpr std::wstring_view kPrefix = L".lucent-";
    constexpr std::wstring_view kSuffix = L".tmp";
    constexpr std::size_t kHexDigits = 16;
    if (name.size() != kPrefix.size() + kHexDigits + kSuffix.size()) {
        return false;
    }
    auto lower = [](wchar_t c) noexcept {
        return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c;
    };
    for (std::size_t i = 0; i < kPrefix.size(); ++i) {
        if (lower(name[i]) != kPrefix[i]) {
            return false;
        }
    }
    for (std::size_t i = 0; i < kHexDigits; ++i) {
        const wchar_t c = lower(name[kPrefix.size() + i]);
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) {
            return false;
        }
    }
    for (std::size_t i = 0; i < kSuffix.size(); ++i) {
        if (lower(name[kPrefix.size() + kHexDigits + i]) != kSuffix[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace lucent::core
