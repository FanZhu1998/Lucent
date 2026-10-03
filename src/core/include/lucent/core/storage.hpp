#pragma once

// Storage classification and generic directory/file operations (blueprint section 2: "A
// removable-card source remains visibly identified"; section 7: "Test writable destination and
// free space"; section 8: resource locations).
//
// Thread-safety: every function is safe to call concurrently. None of them changes process-wide
// state (error mode, current directory).

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

#include "lucent/core/error.hpp"

namespace lucent::core {

// What kind of storage holds a path. Persisted values (catalog, import reports): never renumber,
// append only.
enum class StorageKind : std::uint8_t {
    Unknown = 0,    // the volume could not be identified (no such drive, unmounted, error)
    Fixed = 1,      // internal disk or RAM disk
    Removable = 2,  // card, USB stick, external USB/SD/1394 disk, optical disc
    Network = 3,    // UNC path, mapped network drive or other redirector
};

// Stable lowercase identifier: "unknown", "fixed", "removable", "network".
[[nodiscard]] std::string_view to_string(StorageKind kind) noexcept;

// Classifies the storage that holds `path`. The path need not exist, but its volume must be
// mounted to be anything other than Unknown (or Network, see below).
//
// Decision procedure (each step is a unit-tested seam in src/storage_internal.hpp):
//  1. The lexical volume root is derived from the path text: "C:\..." -> "C:\";
//     "\\server\share\..." and "\\?\UNC\server\share\..." -> "\\server\share\";
//     "\\?\C:\..." and "\\.\C:\..." -> "C:\"; "\\?\Volume{GUID}\..." -> that volume. Relative
//     paths are first made absolute against the current directory.
//  2. A UNC root is Network immediately, without any I/O: probing an unreachable server can
//     block for the redirector's timeout.
//  3. GetDriveTypeW on the root: DRIVE_REMOTE (mapped drive) -> Network without further I/O.
//  4. For local roots the volume mount point that contains the path is resolved
//     (GetVolumePathNameW), so a removable volume mounted in a folder (C:\Cards\SD) is seen as
//     that volume, and its drive type is queried again.
//  5. DRIVE_REMOVABLE and DRIVE_CDROM -> Removable; DRIVE_FIXED and DRIVE_RAMDISK -> Fixed, except
//     that a "fixed" volume whose device sits on a USB, SD, MMC or IEEE 1394 bus
//     (IOCTL_STORAGE_QUERY_PROPERTY, no elevation needed) is Removable: external USB disks
//     and many card readers report DRIVE_FIXED but can be unplugged like a card. When the bus
//     cannot be queried the drive type alone decides. DRIVE_UNKNOWN / DRIVE_NO_ROOT_DIR and
//     every failure -> Unknown.
// SUBST drives report the drive type of the volume they alias.
//
// Never fails: errors map to StorageKind::Unknown. Cost: a few metadata calls, no file is read.
[[nodiscard]] StorageKind storage_kind(const std::filesystem::path& path);

// Bytes available to the calling user (quota-aware, GetDiskFreeSpaceExW) on the volume holding
// `directory`, which must exist. Errors: NotFound, PermissionDenied, IoError (win32 mapping),
// InvalidArgument for empty paths.
[[nodiscard]] Expected<std::uint64_t> available_disk_bytes(const std::filesystem::path& directory);

// Creates `directory` and any missing parents (long paths supported). Succeeds when it already
// exists as a directory (or a link to one). Errors: AlreadyExists when a component exists as a
// file; NotFound when the drive or share does not exist; PermissionDenied; IoError;
// InvalidArgument for empty paths.
[[nodiscard]] Status create_directory_tree(const std::filesystem::path& directory);

// How write_file_atomic() treats an existing file at the target path.
enum class ExistingFile : std::uint8_t {
    Replace = 0,  // atomically replace it (rename with replace semantics)
    Keep = 1,     // fail with AlreadyExists; a file that appears concurrently is never clobbered
};

struct AtomicWriteOptions {
    ExistingFile existing = ExistingFile::Replace;
    // FlushFileBuffers before publishing. Without it a crash or power loss shortly after the
    // call can leave a published file with missing data (never a partially renamed one), so use
    // false only for disposable data whose readers validate content (caches).
    bool flush = true;
};

// Writes `bytes` to `target` so that readers only ever see the old file or the complete new one:
// the bytes go to a temporary sibling (see is_temporary_file_name) in the same directory, are
// optionally flushed, and the sibling is renamed onto `target` by handle
// (SetFileInformationByHandle / FileRenameInfo). The parent directory must exist. On every
// failure the temporary file is removed and `target` is unchanged.
//
// Errors: NotFound (parent missing), AlreadyExists (ExistingFile::Keep and the target exists),
// PermissionDenied (including replacing a file that another process holds open without
// FILE_SHARE_DELETE), DiskFull, IoError, InvalidArgument (empty target or no file name).
[[nodiscard]] Status write_file_atomic(const std::filesystem::path& target, std::span<const std::byte> bytes,
                                       const AtomicWriteOptions& options = {});

// True when `file_name` (a name only, not a path) has the form of the temporary siblings that
// write_file_atomic() and copy_file_verified() create (".lucent-<16 hex digits>.tmp", compared
// case-insensitively). Such files are never complete user data: discovery must skip them, and a
// file left behind by a crash may be deleted once no process holds it open.
[[nodiscard]] bool is_temporary_file_name(const std::filesystem::path& file_name) noexcept;

}  // namespace lucent::core
