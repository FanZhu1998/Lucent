#include "lucent/core/file_copy.hpp"

#include <algorithm>
#include <format>
#include <memory>
#include <string>
#include <utility>

#include "lucent/core/storage.hpp"
#include "lucent/core/win32_error.hpp"

#include "file_copy_internal.hpp"
#include "storage_internal.hpp"
#include "win32.hpp"

namespace lucent::core {

namespace {

constexpr const char* kWhere = "core.file_copy";
constexpr std::size_t kChunkAlignment = std::size_t{64} << 10;  // 64 KiB
constexpr std::uint32_t kMaxUniqueNameIndex = 9999;

struct VirtualFreeDeleter {
    void operator()(std::byte* memory) const noexcept { ::VirtualFree(memory, 0, MEM_RELEASE); }
};
using AlignedBuffer = std::unique_ptr<std::byte, VirtualFreeDeleter>;

// Characters Windows never accepts in a file name, plus ':' (alternate data streams).
bool is_valid_file_name(const std::wstring& name) {
    if (name.empty() || name == L"." || name == L"..") {
        return false;
    }
    for (const wchar_t c : name) {
        if (c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' || c == L'\\' ||
            c == L'|' || c == L'?' || c == L'*') {
            return false;
        }
    }
    // Win32 strips trailing dots and spaces, so such a name would not be the one requested.
    return name.back() != L'.' && name.back() != L' ';
}

FILETIME filetime_from_ticks(std::int64_t ticks) noexcept {
    const auto value = static_cast<std::uint64_t>(ticks);
    FILETIME time{};
    time.dwLowDateTime = static_cast<DWORD>(value & 0xFFFF'FFFFu);
    time.dwHighDateTime = static_cast<DWORD>(value >> 32);
    return time;
}

Unexpected source_changed(const std::filesystem::path& path, const std::string& detail_text) {
    return fail(
        ErrorCode::SourceChanged,
        "source file changed while it was being copied: " + detail::quoted(path) + " (" + detail_text + ")",
        kWhere);
}

void report(const CopyProgressCallback& progress, CopyPhase phase, std::uint64_t done, std::uint64_t total) {
    if (progress) {
        progress(CopyProgress{phase, done, total});
    }
}

// Reads the flushed copy back, bypassing the system cache where the volume allows it, and
// returns its SHA-256.
Expected<Digest256> hash_copy(HANDLE copy, std::uint64_t expected_size, std::byte* buffer, std::size_t chunk,
                              const CancellationToken& cancel, const CopyProgressCallback& progress,
                              const std::filesystem::path& name_for_errors) {
    constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    detail::UniqueHandle reader(
        ::ReOpenFile(copy, GENERIC_READ, kShareAll, FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN));
    if (!reader) {
        // Some redirectors refuse unbuffered handles; a cached read still checks the file system
        // path end to end.
        reader.reset(::ReOpenFile(copy, GENERIC_READ, kShareAll, FILE_FLAG_SEQUENTIAL_SCAN));
    }
    if (!reader) {
        const DWORD code = ::GetLastError();
        return Unexpected(win32_error(
            code, kWhere, "cannot reopen the copy of " + detail::quoted(name_for_errors) + " to verify it"));
    }

    report(progress, CopyPhase::Verifying, 0, expected_size);
    Sha256 hasher;
    std::uint64_t total = 0;
    for (;;) {
        if (auto status = cancel.check(kWhere); !status) {
            return Unexpected(std::move(status.error()));
        }
        DWORD read = 0;
        // Full aligned requests even at the end: unbuffered reads return the short tail.
        if (!::ReadFile(reader.get(), buffer, static_cast<DWORD>(chunk), &read, nullptr)) {
            const DWORD code = ::GetLastError();
            if (code == ERROR_HANDLE_EOF) {
                break;
            }
            return Unexpected(
                win32_error(code, kWhere, "cannot read back the copy of " + detail::quoted(name_for_errors)));
        }
        if (read == 0) {
            break;
        }
        total += read;
        if (total > expected_size) {
            break;  // reported below
        }
        hasher.update(std::span<const std::byte>(buffer, read));
        report(progress, CopyPhase::Verifying, total, expected_size);
    }
    if (total != expected_size) {
        return fail(ErrorCode::Corrupt,
                    std::format("copy verification failed: the copy holds {} bytes, the source {}", total,
                                expected_size) +
                        " for " + detail::quoted(name_for_errors),
                    kWhere);
    }
    return hasher.finish();
}

}  // namespace

std::filesystem::path unique_copy_name(const std::filesystem::path& file_name, std::uint32_t n) {
    if (n <= 1) {
        return file_name;
    }
    std::wstring name = file_name.stem().native();
    name += L"_" + std::to_wstring(n);
    name += file_name.extension().native();
    return std::filesystem::path(std::move(name));
}

Expected<CopyReceipt> copy_file_verified(const std::filesystem::path& source,
                                         const std::filesystem::path& destination_dir,
                                         const std::filesystem::path& file_name, const CopyOptions& options,
                                         CancellationToken cancel, const CopyProgressCallback& progress) {
    return detail::copy_file_verified_with_hooks(source, destination_dir, file_name, options, cancel,
                                                 progress, {});
}

Expected<CopyReceipt> detail::copy_file_verified_with_hooks(
    const std::filesystem::path& source, const std::filesystem::path& destination_dir,
    const std::filesystem::path& file_name, const CopyOptions& options, const CancellationToken& cancel,
    const CopyProgressCallback& progress, const CopyTestHooks& hooks) {
    // ---- 1. arguments ---------------------------------------------------------------------
    const std::size_t chunk = options.chunk_bytes;
    if (chunk < kChunkAlignment || chunk > kMaxHashChunkBytes || chunk % kChunkAlignment != 0) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("copy chunk size must be a multiple of 64 KiB in 64 KiB..{} bytes, got {}",
                                kMaxHashChunkBytes, chunk),
                    kWhere);
    }
    const std::filesystem::path name = file_name.empty() ? source.filename() : file_name;
    if (!is_valid_file_name(name.native()) || is_temporary_file_name(name)) {
        return fail(ErrorCode::InvalidArgument,
                    "invalid destination file name " + detail::quoted(name) +
                        " (a plain Windows file name without separators is required)",
                    kWhere);
    }
    if (auto status = cancel.check(kWhere); !status) {
        return Unexpected(std::move(status.error()));
    }

    auto dir_api = win32_api_path(destination_dir, kWhere);
    if (!dir_api) {
        return Unexpected(std::move(dir_api.error()));
    }
    const DWORD dir_attributes = ::GetFileAttributesW(dir_api->c_str());
    if (dir_attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = ::GetLastError();
        return Unexpected(win32_error(
            code, kWhere, "cannot access the destination folder " + detail::quoted(destination_dir)));
    }
    if ((dir_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "the destination " + detail::quoted(destination_dir) + " is not a folder", kWhere);
    }

    // ---- source ---------------------------------------------------------------------------
    auto file = ReadOnlyFile::open(source, AccessHint::Sequential);
    if (!file) {
        return Unexpected(std::move(file.error()));
    }
    auto before = file->identity();
    if (!before) {
        return Unexpected(std::move(before.error()));
    }
    const std::uint64_t size = before->size_bytes;

    if (options.name_policy == CopyNamePolicy::FailIfExists) {
        auto target_api = win32_api_path(destination_dir / name, kWhere);
        if (!target_api) {
            return Unexpected(std::move(target_api.error()));
        }
        if (::GetFileAttributesW(target_api->c_str()) != INVALID_FILE_ATTRIBUTES) {
            return fail(ErrorCode::AlreadyExists, detail::quoted(destination_dir / name) + " already exists",
                        kWhere, ERROR_FILE_EXISTS);
        }
    }

    // ---- 2. free space --------------------------------------------------------------------
    std::uint64_t available = 0;
    if (hooks.available_bytes) {
        available = *hooks.available_bytes;
    } else {
        auto queried = available_disk_bytes(destination_dir);
        if (!queried) {
            return Unexpected(std::move(queried.error()));
        }
        available = *queried;
    }
    const std::uint64_t reserve = options.free_space_reserve_bytes;
    if (available < size || available - size < reserve) {
        return fail(
            ErrorCode::DiskFull,
            std::format("not enough free space in {}: {} bytes available, {} needed ({} file + {} reserve)",
                        detail::quoted(destination_dir), available, size + reserve, size, reserve),
            kWhere, ERROR_DISK_FULL);
    }

    // ---- 3. temporary sibling -------------------------------------------------------------
    auto temporary =
        create_temporary_file(destination_dir, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, kWhere);
    if (!temporary) {
        return Unexpected(std::move(temporary.error()));
    }
    if (size > 0) {
        FILE_ALLOCATION_INFO allocation{};
        allocation.AllocationSize.QuadPart = static_cast<LONGLONG>(size);
        if (!::SetFileInformationByHandle(temporary->get(), FileAllocationInfo, &allocation,
                                          sizeof(allocation))) {
            const DWORD code = ::GetLastError();
            if (code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL) {
                return Unexpected(
                    win32_error(code, kWhere, "not enough space to copy " + detail::quoted(source)));
            }
            // Preallocation is an optimisation; other refusals (some file systems) are harmless.
        }
    }

    AlignedBuffer buffer(
        static_cast<std::byte*>(::VirtualAlloc(nullptr, chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)));
    if (!buffer) {
        return fail(ErrorCode::OutOfMemory, std::format("cannot allocate a {} byte copy buffer", chunk),
                    kWhere);
    }

    // ---- 4. copy and hash -----------------------------------------------------------------
    report(progress, CopyPhase::Copying, 0, size);
    Sha256 source_hasher;
    std::uint64_t offset = 0;
    while (offset < size) {
        if (auto status = cancel.check(kWhere); !status) {
            return Unexpected(std::move(status.error()));
        }
        const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(chunk, size - offset));
        const std::span<std::byte> piece(buffer.get(), wanted);
        auto read = file->read_at(offset, piece);
        if (!read) {
            return Unexpected(std::move(read.error()));
        }
        if (*read != wanted) {
            return source_changed(
                source, std::format("it was truncated: end of file at byte {} of {}", offset + *read, size));
        }
        source_hasher.update(piece);
        if (hooks.before_write) {
            hooks.before_write(offset, piece);
        }
        DWORD code = hooks.write_fault ? static_cast<DWORD>(hooks.write_fault(offset)) : ERROR_SUCCESS;
        if (code == ERROR_SUCCESS) {
            code = write_all(temporary->get(), piece.data(), piece.size());
        }
        if (code != ERROR_SUCCESS) {
            return Unexpected(win32_error(code, kWhere,
                                          "cannot write the copy of " + detail::quoted(source) + " into " +
                                              detail::quoted(destination_dir)));
        }
        offset += wanted;
        report(progress, CopyPhase::Copying, offset, size);
        if (hooks.after_chunk) {
            hooks.after_chunk(offset);
        }
    }
    auto source_digest = source_hasher.finish();
    if (!source_digest) {
        return Unexpected(std::move(source_digest.error()));
    }

    // ---- 5. stable-read checks (as hash_file) and flush -----------------------------------
    auto after_handle = file->identity();
    if (!after_handle) {
        return Unexpected(std::move(after_handle.error()));
    }
    if (*after_handle != *before) {
        return source_changed(
            source, "it was modified: before " + to_string(*before) + "; after " + to_string(*after_handle));
    }
    auto deleted = file->is_delete_pending();
    if (!deleted) {
        return Unexpected(std::move(deleted.error()));
    }
    if (*deleted) {
        return source_changed(source, "it was deleted");
    }
    auto after_path = stat_file(source);
    if (!after_path) {
        if (after_path.error().code == ErrorCode::NotFound) {
            return source_changed(source, "it was removed or renamed");
        }
        return Unexpected(std::move(after_path.error()));
    }
    if (*after_path != *before) {
        return source_changed(source, "the path now refers to a different or modified file");
    }
    file->close();  // the source is no longer needed

    if (options.preserve_last_write_time) {
        const FILETIME modified = filetime_from_ticks(before->last_write_time);
        if (!::SetFileTime(temporary->get(), nullptr, nullptr, &modified)) {
            const DWORD code = ::GetLastError();
            return Unexpected(win32_error(code, kWhere, "cannot set the modification time of the copy"));
        }
    }
    if (!::FlushFileBuffers(temporary->get())) {
        const DWORD code = ::GetLastError();
        return Unexpected(win32_error(code, kWhere, "cannot flush the copy of " + detail::quoted(source)));
    }

    // ---- 6. verify ------------------------------------------------------------------------
    auto copy_digest = hash_copy(temporary->get(), size, buffer.get(), chunk, cancel, progress, source);
    if (!copy_digest) {
        return Unexpected(std::move(copy_digest.error()));
    }
    if (*copy_digest != *source_digest) {
        return fail(ErrorCode::Corrupt,
                    "copy verification failed: the copy of " + detail::quoted(source) + " in " +
                        detail::quoted(destination_dir) + " has SHA-256 " + to_hex(*copy_digest) +
                        " but the source has " + to_hex(*source_digest),
                    kWhere);
    }
    if (auto status = cancel.check(kWhere); !status) {
        return Unexpected(std::move(status.error()));
    }

    // ---- 7. no-clobber publication --------------------------------------------------------
    const std::uint32_t attempts =
        options.name_policy == CopyNamePolicy::MakeUnique ? kMaxUniqueNameIndex : 1;
    std::filesystem::path final_path;
    for (std::uint32_t n = 1; n <= attempts; ++n) {
        const std::filesystem::path candidate = destination_dir / unique_copy_name(name, n);
        if (hooks.before_publish) {
            hooks.before_publish(candidate);
        }
        auto candidate_api = win32_api_path(candidate, kWhere);
        if (!candidate_api) {
            return Unexpected(std::move(candidate_api.error()));
        }
        const DWORD code = rename_by_handle(temporary->get(), *candidate_api, false);
        if (code == ERROR_SUCCESS) {
            final_path = candidate;
            break;
        }
        if (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS) {
            return Unexpected(
                win32_error(code, kWhere, "cannot publish the copy as " + detail::quoted(candidate)));
        }
    }
    if (final_path.empty()) {
        return fail(ErrorCode::AlreadyExists,
                    options.name_policy == CopyNamePolicy::MakeUnique
                        ? std::format("no free name for {} in {} (tried up to _{})", detail::quoted(name),
                                      detail::quoted(destination_dir), kMaxUniqueNameIndex)
                        : detail::quoted(destination_dir / name) + " already exists",
                    kWhere, ERROR_FILE_EXISTS);
    }

    // Persist the rename where the file system supports it; the data was flushed already.
    ::FlushFileBuffers(temporary->get());
    // Attribute queries are exempt from sharing checks; our handle (no FILE_SHARE_DELETE or
    // FILE_SHARE_WRITE) keeps the published file from being replaced while it is queried.
    auto copy_identity = stat_file(final_path);
    if (!copy_identity) {
        return Unexpected(std::move(copy_identity.error()));  // the destructor unpublishes the copy
    }
    temporary->keep();

    CopyReceipt receipt;
    receipt.final_path = std::move(final_path);
    receipt.sha256 = *source_digest;
    receipt.size_bytes = size;
    receipt.source_identity = *before;
    receipt.copy_identity = *copy_identity;
    return receipt;
}

Status remove_verified_copy(const std::filesystem::path& path, const FileIdentity& copy_identity) {
    constexpr const char* kWhereRemove = "core.file_copy.remove";
    auto api = detail::win32_api_path(path, kWhereRemove);
    if (!api) {
        return Unexpected(std::move(api.error()));
    }
    // DELETE access; readers may share, but no writer, rename or delete while the handle is open,
    // so the identity queried below is the identity of the file this handle deletes. Links are
    // not followed: a copy is a regular file.
    detail::UniqueHandle handle(::CreateFileW(api->c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                                              nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle) {
        const DWORD code = ::GetLastError();
        return Unexpected(win32_error(code, kWhereRemove, "cannot open the copy " + detail::quoted(path)));
    }
    // Attribute queries are exempt from sharing checks.
    auto now = stat_file(path);
    if (!now) {
        return Unexpected(std::move(now.error()));
    }
    if (*now != copy_identity) {
        return fail(ErrorCode::SourceChanged,
                    detail::quoted(path) + " is no longer the copy Lucent made (" + to_string(*now) +
                        "); it was kept",
                    kWhereRemove);
    }
    if (const DWORD code = detail::delete_by_handle(handle.get()); code != ERROR_SUCCESS) {
        return Unexpected(win32_error(code, kWhereRemove, "cannot delete the copy " + detail::quoted(path)));
    }
    return ok();
}

}  // namespace lucent::core
