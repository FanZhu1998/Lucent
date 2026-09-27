#pragma once

// Private Win32 file-system helpers of the application module: canonical paths, case folding
// with file-system rules, directory listing with reparse-point information, and free space.
// <windows.h> stays in the .cpp.
//
// Thread-safety: all functions are safe to call concurrently.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/core/error.hpp>

namespace lucent::app::detail {

// The string to hand to wide-character Win32 file APIs: paths of MAX_PATH - 12 characters or
// more are made absolute and given the "\\?\" (or "\\?\UNC\") prefix so the MAX_PATH limit does
// not apply (the rule of core's and the exporter's private helpers). InvalidArgument for empty
// paths and paths containing NUL.
[[nodiscard]] Expected<std::wstring> api_path(const std::filesystem::path& path, std::string_view where);

// The final path of the file or directory at `path` after every symbolic link, junction and
// mount point has been resolved (GetFinalPathNameByHandleW, FILE_NAME_NORMALIZED): absolute,
// with the on-disk spelling of every component. Drive paths are returned without the "\\?\"
// prefix ("C:\Photos\a.cr3") and UNC paths as "\\server\share\..."; a volume without a drive
// letter keeps its "\\?\Volume{GUID}\" form. Errors: NotFound, PermissionDenied, IoError (Win32
// mapping of core::win32_error), InvalidArgument for invalid paths. `where` names the caller.
[[nodiscard]] Expected<std::filesystem::path> canonical_path(const std::filesystem::path& path,
                                                             std::string_view where);

// `text` converted to upper case with the file system's (non-linguistic) casing rules
// (LCMapStringEx, LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE), so two names that a case-insensitive
// directory treats as the same name fold to the same string.
[[nodiscard]] std::wstring fold_case(std::wstring_view text);

// True when both paths are the same text after fold_case (no resolution is performed).
[[nodiscard]] bool same_path_text(const std::filesystem::path& a, const std::filesystem::path& b);

// True when `path` equals `directory` or lies below it, compared component-wise after
// fold_case (both should be canonical paths).
[[nodiscard]] bool is_same_or_within(const std::filesystem::path& path,
                                     const std::filesystem::path& directory);

struct DirectoryEntry {
    std::wstring name;              // file name only
    bool directory = false;         // FILE_ATTRIBUTE_DIRECTORY
    bool reparse_point = false;     // FILE_ATTRIBUTE_REPARSE_POINT (junction, symbolic link, ...)
    std::uint32_t reparse_tag = 0;  // IO_REPARSE_TAG_* when reparse_point
    bool device = false;            // FILE_ATTRIBUTE_DEVICE
};

// Entries of `directory` (without "." and ".."), sorted by fold_case(name) then name, so the
// order never depends on the file system. Errors: NotFound, PermissionDenied, IoError.
[[nodiscard]] Expected<std::vector<DirectoryEntry>> list_directory(const std::filesystem::path& directory,
                                                                   std::string_view where);

// True when a directory exists at `path` (following links).
[[nodiscard]] bool is_directory(const std::filesystem::path& path);

// Bytes available to the calling user on the volume holding `directory` (GetDiskFreeSpaceExW).
[[nodiscard]] Expected<std::uint64_t> free_disk_bytes(const std::filesystem::path& directory,
                                                      std::string_view where);

}  // namespace lucent::app::detail
