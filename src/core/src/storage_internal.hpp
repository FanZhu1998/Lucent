#pragma once

// Private storage helpers of lucent_core: the pure decision seams behind storage_kind() and the
// handle-based rename/delete/temporary-file primitives shared by write_file_atomic() and
// copy_file_verified(). Not part of the public API.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>

#include "lucent/core/error.hpp"
#include "lucent/core/storage.hpp"

#include "win32.hpp"

namespace lucent::core::detail {

// ---- storage_kind() seams -------------------------------------------------------------------

enum class VolumeRootKind : std::uint8_t {
    None,         // no recognisable root (relative path, device path, malformed UNC)
    DriveLetter,  // root is "X:\" (upper-case letter)
    Unc,          // root is "\\server\share\"
    Volume,       // root is "\\?\Volume{GUID}\"
};

struct VolumeRoot {
    VolumeRootKind kind = VolumeRootKind::None;
    std::wstring root;  // empty for None; always ends with a backslash otherwise
};

// The volume root named by an absolute path's text (no I/O). Accepts '/' as a separator outside
// "\\?\" paths. See storage_kind() in storage.hpp for the forms.
[[nodiscard]] VolumeRoot lexical_volume_root(std::wstring_view absolute_path);

// What IOCTL_STORAGE_QUERY_PROPERTY (StorageDeviceProperty) reported for a volume's device.
struct StorageDeviceInfo {
    std::uint32_t bus_type = 0;  // STORAGE_BUS_TYPE value
    bool removable_media = false;
};

// Final classification from a GetDriveTypeW result and, when it could be queried, the device.
[[nodiscard]] StorageKind classify_storage(std::uint32_t drive_type,
                                           const std::optional<StorageDeviceInfo>& device) noexcept;

// ---- temporary files and handle-based publication -------------------------------------------

// A fresh ".lucent-<16 hex>.tmp" name (random, see is_temporary_file_name()).
[[nodiscard]] std::wstring make_temporary_file_name();

// Renames the file open as `handle` (opened with DELETE access) to `target_api_path` (a
// win32_api_path() result). replace == false never overwrites an existing file. With replace ==
// true POSIX semantics are requested first, so a target that readers hold open with
// FILE_SHARE_DELETE can still be replaced; volumes without them fall back to classic replace.
// Returns the Win32 error, or ERROR_SUCCESS.
[[nodiscard]] DWORD rename_by_handle(HANDLE handle, const std::wstring& target_api_path, bool replace);

// Marks the file open as `handle` (opened with DELETE access) for deletion when its last handle
// closes. Returns the Win32 error, or ERROR_SUCCESS.
[[nodiscard]] DWORD delete_by_handle(HANDLE handle) noexcept;

// Writes all of `size` bytes (in pieces of at most 1 GiB). Returns the Win32 error, or
// ERROR_SUCCESS.
[[nodiscard]] DWORD write_all(HANDLE handle, const void* data, std::size_t size) noexcept;

// Owner of a temporary sibling: deletes it through its handle on destruction unless released
// after successful publication.
class TemporaryFile {
public:
    TemporaryFile() noexcept = default;
    TemporaryFile(UniqueHandle handle, std::wstring api_path) noexcept
        : handle_(std::move(handle)), api_path_(std::move(api_path)) {}
    ~TemporaryFile() { discard(); }

    TemporaryFile(TemporaryFile&&) noexcept = default;
    TemporaryFile& operator=(TemporaryFile&&) = delete;
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_.get(); }
    [[nodiscard]] const std::wstring& api_path() const noexcept { return api_path_; }

    // Deletes the file (by handle; by path if that fails) and closes the handle.
    void discard() noexcept;
    // Keeps the file (it has been published) and closes the handle.
    void keep() noexcept { handle_.reset(); }

private:
    UniqueHandle handle_;
    std::wstring api_path_;
};

// Creates a new temporary sibling in `directory` with CREATE_NEW and the given access and
// share mode (DELETE access is always added), retrying on name collisions.
[[nodiscard]] Expected<TemporaryFile> create_temporary_file(const std::filesystem::path& directory,
                                                            DWORD access, DWORD share,
                                                            std::string_view where);

}  // namespace lucent::core::detail
