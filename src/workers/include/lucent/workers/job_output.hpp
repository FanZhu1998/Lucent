#pragma once

// The files of one job's output (protocol.hpp, "Output files"). A job writes exactly one file, the
// request's temp_path, which the host later publishes under its final name (no-clobber). While
// writing, the worker keeps the exporter's own temporary file and the verified file in a scratch
// directory beside it, job_scratch_directory(temp_path), which it removes before answering. So
// everything a worker can leave behind for a job is known from temp_path alone, and a host that
// lost a worker mid-job (crash, kill, timeout) removes it with remove_job_output().
//
// The temp path should be unique per job attempt and on the destination's file system (the final
// publication is then a same-volume rename). Paths of any length work (long paths are passed to
// Win32 in "\\?\" form).
//
// Thread-safety: functions are safe to call concurrently for different temp paths.

#include <filesystem>

#include <lucent/core/error.hpp>

namespace lucent::workers {

// "<temp_path>.parts".
[[nodiscard]] std::filesystem::path job_scratch_directory(const std::filesystem::path& temp_path);

// Creates job_scratch_directory(temp_path). AlreadyExists when it exists (an earlier attempt was
// not cleaned up: remove_job_output first); NotFound when temp_path's directory does not exist;
// other Win32 failures per core::win32_error.
[[nodiscard]] Status create_job_scratch_directory(const std::filesystem::path& temp_path);

// Renames `from` to `to` on the same volume with write-through, never replacing anything:
// AlreadyExists when `to` exists. Other Win32 failures per core::win32_error.
[[nodiscard]] Status move_no_clobber(const std::filesystem::path& from, const std::filesystem::path& to);

// Removes the regular files directly inside job_scratch_directory(temp_path) and the directory
// itself. Subdirectories and directory links inside are never followed or removed (the directory
// then stays, and the result is an IoError naming it). ok() when the directory does not exist.
[[nodiscard]] Status remove_job_scratch_directory(const std::filesystem::path& temp_path);

// Removes temp_path (when it exists) and the scratch directory (remove_job_scratch_directory).
// Read-only attributes are cleared first. ok() when neither exists. The first failure is returned
// after attempting both.
[[nodiscard]] Status remove_job_output(const std::filesystem::path& temp_path);

}  // namespace lucent::workers
