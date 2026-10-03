#pragma once

// Lucent's per-user directories (blueprint section 8, "Keep resources separate").
//
// Layout under the root (by default %LOCALAPPDATA%\Lucent, i.e. FOLDERID_LocalAppData):
//
//   <root>\data            per-user application data: catalog, settings, recipe records, job
//                          journal, installed asset versions. Never deleted by Lucent.
//   <root>\cache           per-user cache: disposable thumbnails, proxies, temporary analysis
//                          data and bounded logs. Deleting it at any time (even while Lucent
//                          runs) loses no edits; the application recreates it.
//   <root>\cache\logs      rotated log files (bounded; the blueprint places logs in the cache)
//
// The local (not roaming) AppData folder is used because the catalog and caches are machine
// specific and potentially large. Nothing here is created implicitly: call
// create_app_directories() at startup.
//
// Thread-safety: all functions are safe to call concurrently.

#include <filesystem>

#include "lucent/core/error.hpp"

namespace lucent::core {

struct AppDirectories {
    std::filesystem::path root;   // e.g. C:\Users\Ann\AppData\Local\Lucent
    std::filesystem::path data;   // <root>\data
    std::filesystem::path cache;  // <root>\cache
    std::filesystem::path logs;   // <root>\cache\logs
};

// The directory layout above for an explicit root (tests, portable installs, diagnostics).
// Pure: performs no I/O and does not validate `root`.
[[nodiscard]] AppDirectories app_directories_at(const std::filesystem::path& root);

// The current user's local application data folder (SHGetKnownFolderPath with
// FOLDERID_LocalAppData; honours folder redirection). Errors: NotFound when the shell reports no
// such folder for this user, IoError otherwise (HRESULT in native_code).
[[nodiscard]] Expected<std::filesystem::path> local_app_data_folder();

// app_directories_at(local_app_data_folder() / "Lucent").
[[nodiscard]] Expected<AppDirectories> default_app_directories();

// Creates root, data, cache and logs (and missing parents) with create_directory_tree() from
// storage.hpp. Idempotent; call at startup and whenever a cache write finds its directory gone.
// Errors: as create_directory_tree().
[[nodiscard]] Status create_app_directories(const AppDirectories& directories);

}  // namespace lucent::core
