#pragma once

// Private: the coordinator's side of publishing a worker's output (blueprint section 7, "Export
// integrity": "Close/flush, reopen with the relevant decoder, and verify ... before final
// publication. Default publication uses a no-clobber same-filesystem primitive ... Recheck
// destination identity"; ADR 0012).
//
// HeldOutput closes the gap ADR 0006 left open ("holding the reserved temporary file's handle for
// the whole protocol instead of reopening it by name"): the temporary file a worker wrote is opened
// ONCE, with read, write (for FlushFileBuffers only; nothing is written) and DELETE access, shared
// for reading only. While it is held nobody can modify, truncate, rename, replace or delete it, so
// the bytes hashed and inspected through the handle are the bytes published, and publication renames
// THAT file object (SetFileInformationByHandle, FileRenameInfo without ReplaceIfExists) instead of
// whatever might be at its name. Readers that share writing and deletion (core::ReadOnlyFile, which
// the metadata reader uses) can still open it by path for inspection.
//
// Header inspection reads the container's own structure through the handle: JPEG (SOI, the frame
// header, APP2 ICC_PROFILE chunks in sequence order) and classic TIFF (IFD 0: width, length, bits
// per sample, samples per pixel, the ICC profile tag 34675), so the coordinator checks dimensions,
// sample format and the embedded profile independently of the worker's report; the full decode and
// byte comparison are the worker's (protocol.hpp, OutputVerification), bound to these bytes by the
// SHA-256 the handle reproduces.
//
// Thread-safety: a HeldOutput is used by one thread; the free functions are safe to call
// concurrently for different paths.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/core/file.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/export/export.hpp>

namespace lucent::app::detail {

struct OutputDigest {
    std::uint64_t size_bytes = 0;
    core::Digest256 sha256{};
};

struct OutputHeader {
    exporter::ExportFormat format = exporter::ExportFormat::Jpeg;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t components = 0;       // channels / samples per pixel
    std::uint32_t bits_per_sample = 0;  // 8 (JPEG) or the TIFF BitsPerSample (all samples equal)
    std::optional<core::Digest256> icc_sha256;  // of the embedded ICC profile bytes, when present
};

class HeldOutput {
public:
    HeldOutput() noexcept = default;
    ~HeldOutput();
    HeldOutput(HeldOutput&& other) noexcept;
    HeldOutput& operator=(HeldOutput&& other) noexcept;
    HeldOutput(const HeldOutput&) = delete;
    HeldOutput& operator=(const HeldOutput&) = delete;

    // Opens the regular file at `path` as described in the file comment. Errors (`where`):
    // NotFound; IoError with ERROR_SHARING_VIOLATION when another program has it open for writing
    // or deletion; PermissionDenied; InvalidArgument for a directory.
    [[nodiscard]] static Expected<HeldOutput> open(const std::filesystem::path& path, std::string_view where);

    // Opens an existing file that publication is about to replace (CollisionPolicy::Overwrite), or
    // a leftover to delete, with DELETE access and no sharing at all: it fails (IoError, sharing
    // violation) while any program has the file open, and while it is held nobody can open it. The
    // caller checks identity() before renaming it aside or removing it. Only identity(),
    // rename_no_clobber() and remove() may be used on such a handle.
    [[nodiscard]] static Expected<HeldOutput> open_exclusive(const std::filesystem::path& path,
                                                             std::string_view where);

    [[nodiscard]] bool is_open() const noexcept { return handle_ != nullptr; }
    // The file's current name (updated by a successful rename).
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    [[nodiscard]] Expected<core::FileIdentity> identity() const;
    // Size and SHA-256 of the whole file, read through the handle. Cancelled between 1 MiB reads;
    // `between_reads` runs before each read (the coordinator renews its lease there: hashing a
    // large output on a slow destination can take minutes).
    [[nodiscard]] Expected<OutputDigest> digest(const core::CancellationToken& cancel,
                                                const std::function<void()>& between_reads = {}) const;
    // See the file comment. Corrupt ("app.batch.verify") for a file that is neither a JPEG nor a
    // classic TIFF, or whose structure is malformed.
    [[nodiscard]] Expected<OutputHeader> inspect_header() const;

    // Renames the held file to `target` (same volume) without replacing anything: AlreadyExists
    // when `target` exists. path() follows on success.
    [[nodiscard]] Status rename_no_clobber(const std::filesystem::path& target);
    // FlushFileBuffers: makes the rename and the file's metadata durable.
    [[nodiscard]] Status flush();
    // Deletes the held file (POSIX semantics where available) and closes the handle.
    [[nodiscard]] Status remove();
    void close() noexcept;

private:
    HeldOutput(void* handle, std::filesystem::path path) noexcept : handle_(handle), path_(std::move(path)) {}
    [[nodiscard]] Expected<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> buffer) const;
    [[nodiscard]] Status read_exact_at(std::uint64_t offset, std::span<std::byte> buffer) const;
    [[nodiscard]] Expected<OutputHeader> inspect_jpeg(std::uint64_t size) const;
    [[nodiscard]] Expected<OutputHeader> inspect_tiff(std::uint64_t size) const;

    void* handle_ = nullptr;
    std::filesystem::path path_;
};

// Removes the regular file at `path` if nothing holds it open (opened with DELETE access and no
// sharing, then deleted through that handle). ok() when it does not exist; IoError (sharing
// violation) when it is in use.
[[nodiscard]] Status remove_unused_file(const std::filesystem::path& path, std::string_view where);

// Writes and deletes a probe file "~lucent-<tag>-probe.tmp" in `directory` to prove that files can
// be created there. PermissionDenied, NotFound, DiskFull or IoError otherwise.
[[nodiscard]] Status probe_writable(const std::filesystem::path& directory, std::wstring_view tag);

// File names of interrupted publications (see ADR 0012, "Names"). `tag` is a batch's 8-hex-digit
// temporary tag.
[[nodiscard]] std::wstring temp_file_name(std::wstring_view tag, std::int64_t job, std::uint32_t attempt);
[[nodiscard]] std::wstring aside_file_name(std::wstring_view tag, std::int64_t job, std::uint32_t attempt);

struct SweepResult {
    std::uint64_t removed = 0;  // files and scratch folders removed
    std::uint64_t in_use = 0;   // leftovers skipped because a program holds them
    std::vector<std::filesystem::path> removed_paths;
};

// Removes leftovers of interrupted publications directly inside `directory`: every
// "~lucent-<tag>-*.tmp" file of the given batch tags and its ".parts" scratch folder
// (workers/job_output.hpp), except the paths in `keep`, and the exporter's own temporaries
// "~lucent-<16 hex digits>.tmp" (exporter/export.hpp, step 3; left by a process killed during an
// in-process export) whose last write is older than `exporter_min_age`. Files in use are skipped.
// A missing directory is not an error.
[[nodiscard]] SweepResult sweep_publication_leftovers(const std::filesystem::path& directory,
                                                      std::span<const std::wstring> tags,
                                                      std::span<const std::filesystem::path> keep,
                                                      std::chrono::seconds exporter_min_age);

}  // namespace lucent::app::detail
