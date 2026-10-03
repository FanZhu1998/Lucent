#pragma once

// Small Win32 file helpers private to lucent_catalog (long-path aware). <windows.h> stays in
// files.cpp.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lucent/core/error.hpp"

namespace lucent::catalog::detail {

struct FileFacts {
    bool directory = false;
    std::uint64_t size_bytes = 0;
};

// Attributes of whatever is at `path` (links followed for the size of files); std::nullopt when
// nothing exists there. Other failures (access denied, ...) are errors.
[[nodiscard]] Expected<std::optional<FileFacts>> file_facts(const std::filesystem::path& path,
                                                            std::string_view where);

// Renames `from` to `to` (same volume), never replacing an existing `to` (AlreadyExists), with
// MOVEFILE_WRITE_THROUGH.
[[nodiscard]] Status rename_no_replace(const std::filesystem::path& from, const std::filesystem::path& to,
                                       std::string_view where);

// Renames `from` over `to`, replacing it (MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH).
[[nodiscard]] Status rename_replace(const std::filesystem::path& from, const std::filesystem::path& to,
                                    std::string_view where);

// Deletes a file; ok() when it does not exist.
[[nodiscard]] Status delete_file_if_exists(const std::filesystem::path& path, std::string_view where);

// Creates `directory` and missing parents (core::create_directory_tree); ok() when it exists.
[[nodiscard]] Status ensure_directory(const std::filesystem::path& directory, std::string_view where);

// Names (not paths) of the regular files directly in `directory`; empty when it does not exist.
[[nodiscard]] Expected<std::vector<std::wstring>> list_file_names(const std::filesystem::path& directory,
                                                                  std::string_view where);

// Holds "<catalog>.lock" open with no sharing and delete-on-close: the writer lock of a catalog.
// AlreadyExists when another holder (this or another process) has it.
class CatalogLock {
public:
    CatalogLock() noexcept = default;
    ~CatalogLock();
    CatalogLock(CatalogLock&& other) noexcept;
    CatalogLock& operator=(CatalogLock&& other) noexcept;
    CatalogLock(const CatalogLock&) = delete;
    CatalogLock& operator=(const CatalogLock&) = delete;

    [[nodiscard]] static Expected<CatalogLock> acquire(const std::filesystem::path& catalog_path,
                                                       std::string_view where);
    [[nodiscard]] static std::filesystem::path lock_path(const std::filesystem::path& catalog_path);

    void release() noexcept;

private:
    void* handle_ = nullptr;
};

// 16 random lowercase hex digits for temporary names.
[[nodiscard]] std::wstring random_hex_suffix();

}  // namespace lucent::catalog::detail
