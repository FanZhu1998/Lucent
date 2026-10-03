#pragma once

// Where the catalog lives and how source paths are compared.
//
// Location (blueprint section 7: "keep the live database on a local disk, not an SMB/network
// share"; section 8: the local catalog belongs to per-user application data). SQLite's WAL mode
// keeps its index in shared memory that only works between processes of one machine, and network
// file systems do not implement the locking SQLite relies on, so a catalog on a network share can
// be corrupted. Catalog::open() therefore classifies the catalog's location with
// core::storage_kind() and refuses anything that is not a local disk (check_catalog_storage).
// Backups and exports may be written anywhere.
//
// Path keys. A source is identified in the catalog by the key of its canonical path. Windows file
// systems compare names case-insensitively, so two spellings that differ only in case name the
// same file and must produce the same key. make_path_key() is purely lexical: it never touches the
// file system, so callers pass paths that are already canonical (links resolved, as the importer
// does) and the key only removes spelling differences.
//
// Thread-safety: all functions are safe to call concurrently.

#include <filesystem>
#include <string>

#include "lucent/catalog/types.hpp"
#include "lucent/core/error.hpp"

namespace lucent::catalog {

// The decision applied to the catalog's own location:
//   Fixed               ok
//   Removable           ok (a portable catalog on a USB disk is the user's choice); Catalog::open
//                       logs a warning and Catalog::storage_kind() reports it
//   Network             Unsupported: "the catalog must be on a local disk"
//   Unknown             Unsupported: the drive type could not be determined, so the catalog is not
//                       known to be local (an unmounted volume, a failing redirector, ...)
// `path` is used in the message only. Where: "catalog.location".
[[nodiscard]] Status check_catalog_storage(StorageKind kind, const std::filesystem::path& path);

// %LOCALAPPDATA%\Lucent\data\lucent.catalog for the current user: the data folder of
// core::default_app_directories() (FOLDERID_LocalAppData), where Lucent.exe keeps its catalog
// (ADR 0008). Nothing is created. Errors as core::local_app_data_folder().
[[nodiscard]] Expected<std::filesystem::path> default_catalog_path();

// File name of the default catalog: "lucent.catalog".
[[nodiscard]] std::filesystem::path default_catalog_file_name();

// Default backup folder of a catalog: a "backups" folder next to it.
[[nodiscard]] std::filesystem::path default_backup_directory(const std::filesystem::path& catalog_path);

// Comparison key of an absolute path, as stored in sources.path_key (UTF-8):
//   * a "\\?\" prefix is removed ("\\?\UNC\server\share" becomes "\\server\share");
//   * '/' becomes '\', repeated separators collapse, "." and ".." segments are resolved
//     lexically (std::filesystem::path::lexically_normal) and a trailing separator is removed
//     (except after a drive root, "C:\");
//   * the result is upper-cased with the operating system's invariant, non-linguistic casing
//     rules (LCMapStringEx, LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE), the rule NTFS directories
//     follow for most names, so "C:\Photos\a.CR3" and "c:\photos\A.cr3" have the same key;
//   * nothing else is normalised: Unicode normalisation forms (NFC/NFD) stay distinct, as they
//     are distinct names on NTFS.
// Errors (where "catalog.path_key"): InvalidArgument for an empty or relative path, a path with
// NUL characters, or one that is not valid UTF-16.
[[nodiscard]] Expected<std::string> make_path_key(const std::filesystem::path& path);

}  // namespace lucent::catalog
