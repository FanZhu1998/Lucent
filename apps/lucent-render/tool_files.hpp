#pragma once

// File-system helpers for lucent-render's own outputs (the --report JSON and the --dump-linear
// float TIFF). The exported image itself goes through lucent::exporter, which has its own
// verified protocol; these helpers give the side files the same two guarantees that matter for
// a developer tool run against real photographs: nothing existing is ever replaced, and long
// paths work without the process-wide long-path opt-in.
//
// Thread-safety: all functions are reentrant.

#include <filesystem>
#include <functional>
#include <string>

#include <lucent/core/error.hpp>

namespace lucent::render_tool {

// The path to hand to wide-character Win32 / C runtime / std::filesystem calls: unchanged when it
// is already in "\\?\" or "\\.\" form or shorter than MAX_PATH - 12 (the CreateDirectoryW limit);
// otherwise made absolute (GetFullPathNameW) and prefixed with "\\?\" (or "\\?\UNC\").
// InvalidArgument for an empty path or one containing NUL. This mirrors the private helpers of
// lucent_core and lucent_export (not public API there).
[[nodiscard]] Expected<std::filesystem::path> long_path(const std::filesystem::path& path);

// std::filesystem::create_directories through long_path(). IoError (lucent-render.output) with
// the system message when the directory cannot be created or a non-directory is in the way.
[[nodiscard]] Status create_directories(const std::filesystem::path& directory);

// True when `a` and `b` name the same file: their full paths are equal ignoring case (so a path
// that does not exist yet is compared by name), or both exist and are the same file object
// (hard links, 8.3 short names, junctions). Errors only for paths that cannot be resolved.
[[nodiscard]] Expected<bool> same_target(const std::filesystem::path& a, const std::filesystem::path& b);

// True when something (file or directory) exists at `path` (long paths supported).
[[nodiscard]] bool exists(const std::filesystem::path& path) noexcept;

// Creates the NEW file `destination` with the contents that `write` produces, never replacing
// anything: `write` fills a fresh temporary sibling "~lucent-<16 hex>.tmp" (created with
// CREATE_NEW; `write` receives its long_path() and may truncate it), which is then renamed with
// MoveFileExW(MOVEFILE_WRITE_THROUGH) without MOVEFILE_REPLACE_EXISTING. Errors
// (lucent-render.publish): AlreadyExists when the destination exists, before or at the rename;
// the error of `write`; Win32 failures. The temporary file is removed on every failure.
[[nodiscard]] Status publish_new_file(const std::filesystem::path& destination,
                                      const std::function<Status(const std::filesystem::path&)>& write);

// publish_new_file() with `text` as the contents.
[[nodiscard]] Status publish_new_text_file(const std::filesystem::path& destination, const std::string& text);

}  // namespace lucent::render_tool
