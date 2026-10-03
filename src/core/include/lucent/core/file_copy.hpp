#pragma once

// Verified copy of an original into a library folder (blueprint section 2: "Copy into library
// folder ... verifies each copy before registering it, and leaves the source intact").
//
// copy_file_verified() never writes, renames, deletes or locks the source: it is opened with
// core::ReadOnlyFile (read-only, sharing read/write/delete with everyone). The copy is published
// under its final name only after its bytes have been read back from the destination and their
// SHA-256 equals the SHA-256 of the source taken with a stable read.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>

#include "lucent/core/cancellation.hpp"
#include "lucent/core/error.hpp"
#include "lucent/core/file.hpp"
#include "lucent/core/hash.hpp"

namespace lucent::core {

// What to do when the destination name is taken (compared as the file system compares names,
// i.e. case-insensitively on NTFS).
enum class CopyNamePolicy : std::uint8_t {
    // Fail with AlreadyExists. Checked before copying (fast failure) and again atomically at
    // publication, so a file created meanwhile is never overwritten.
    FailIfExists = 0,
    // Use the first free name of "<stem>_2<ext>", "<stem>_3<ext>", ... ("IMG_1.CR3" ->
    // "IMG_1_2.CR3"), up to "<stem>_9999<ext>"; AlreadyExists when all are taken. Each attempt
    // is an atomic no-clobber rename, so concurrent copies into one folder never collide.
    MakeUnique = 1,
};

struct CopyOptions {
    CopyNamePolicy name_policy = CopyNamePolicy::MakeUnique;

    // Read/write unit; also the granularity of cancellation and progress. Must be a multiple of
    // 64 KiB (the verification read bypasses the system cache, which needs sector-aligned I/O)
    // in 64 KiB .. kMaxHashChunkBytes.
    std::size_t chunk_bytes = std::size_t{1} << 20;  // 1 MiB

    // Space that must remain free on the destination volume after the copy. The pre-flight
    // check requires available >= source size + this margin, else DiskFull before any byte is
    // written. (Space can still run out during the copy; that is DiskFull too.)
    std::uint64_t free_space_reserve_bytes = std::uint64_t{64} << 20;  // 64 MiB

    // Keep the source's last write time on the copy (as Explorer and CopyFileW do). The creation
    // time of the copy is the time of the copy.
    bool preserve_last_write_time = true;
};

enum class CopyPhase : std::uint8_t {
    Copying = 0,    // reading the source, hashing it and writing the temporary copy
    Verifying = 1,  // reading the flushed copy back from the volume and hashing it
};

struct CopyProgress {
    CopyPhase phase = CopyPhase::Copying;
    std::uint64_t bytes_done = 0;   // within the phase
    std::uint64_t bytes_total = 0;  // source size; identical in both phases
};

// Called synchronously on the copying thread after every chunk of each phase (and once with
// bytes_done == 0 at the start of each phase). Must be cheap and must not throw; use the
// cancellation token, not the callback, to stop a copy.
using CopyProgressCallback = std::function<void(const CopyProgress&)>;

// Proof of a verified copy, suitable for persisting with the catalog record.
struct CopyReceipt {
    std::filesystem::path final_path;  // where the copy was published
    Digest256 sha256{};                // of the source (stable read) == of the copy (read back)
    std::uint64_t size_bytes = 0;
    FileIdentity source_identity;  // unchanged throughout the copy
    FileIdentity copy_identity;    // of the published copy
};

// Copies `source` into the existing directory `destination_dir` as `file_name` (a plain name
// without separators; empty means the source's own file name).
//
// Procedure:
//  1. validate arguments; open the source read-only and capture its identity; resolve the name
//     (FailIfExists fails early when the name is taken);
//  2. check free space (CopyOptions::free_space_reserve_bytes);
//  3. create a temporary sibling ".lucent-<hex>.tmp" in `destination_dir` (CREATE_NEW, other
//     writers excluded) and preallocate it;
//  4. read the source in chunks, hashing each chunk and writing it to the sibling; check
//     cancellation before every chunk;
//  5. stable-read checks on the source exactly as hash_file() does (identity through the handle
//     and the path unchanged, not deleted); FlushFileBuffers on the copy;
//  6. read the copy back with FILE_FLAG_NO_BUFFERING, so the bytes come from the volume rather
//     than the system cache, hash them and compare with the source digest;
//  7. publish by renaming the sibling by handle without replace semantics (no-clobber), trying
//     further names under MakeUnique.
// On every failure, cancellation included, the temporary sibling is deleted (through its open
// handle, so even a concurrent rename of it cannot leak it) and nothing is published. A process
// crash can leave a sibling behind; discovery skips such names (is_temporary_file_name()).
//
// Errors: InvalidArgument (bad name or options, destination not a directory, source is a
// directory); NotFound (source or destination missing); AlreadyExists (name policy); DiskFull
// (pre-flight or during the write); Cancelled; SourceChanged (the source was modified, truncated,
// replaced or deleted during the copy -- re-import it); Corrupt (the read-back copy does not
// match the source: faulty media, driver or cable); PermissionDenied / IoError otherwise.
//
// Thread-safety: safe to call concurrently, including into the same directory.
[[nodiscard]] Expected<CopyReceipt> copy_file_verified(const std::filesystem::path& source,
                                                       const std::filesystem::path& destination_dir,
                                                       const std::filesystem::path& file_name,
                                                       const CopyOptions& options, CancellationToken cancel,
                                                       const CopyProgressCallback& progress = {});

// Removes a copy that copy_file_verified() published and the caller could not use (for example
// because it could not be registered): the file at `path` is deleted only while it is still the
// file object and version `copy_identity` (CopyReceipt::copy_identity). The check and the deletion
// use one handle that keeps the file from being renamed, replaced or written meanwhile, so another
// file put at `path` is never deleted. Only for files Lucent made itself; never for a source.
// Errors ("core.file_copy.remove"): NotFound when nothing is at `path`; SourceChanged (nothing
// deleted) when another file or another version is there; PermissionDenied / IoError otherwise
// (for example while another program holds the file open).
[[nodiscard]] Status remove_verified_copy(const std::filesystem::path& path,
                                          const FileIdentity& copy_identity);

// The n-th candidate name of CopyNamePolicy::MakeUnique: n == 1 is `file_name` itself, n >= 2
// inserts "_<n>" before the extension ("a.b.CR3", 3 -> "a.b_3.CR3"; ".hidden", 2 -> ".hidden_2").
[[nodiscard]] std::filesystem::path unique_copy_name(const std::filesystem::path& file_name, std::uint32_t n);

}  // namespace lucent::core
