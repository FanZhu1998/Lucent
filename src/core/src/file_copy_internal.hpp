#pragma once

// Private entry point of copy_file_verified() with fault-injection hooks. White-box test seam;
// production code passes default (empty) hooks. Not part of the public API.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>

#include "lucent/core/file_copy.hpp"

namespace lucent::core::detail {

struct CopyTestHooks {
    // Replaces the free-space query of the pre-flight check (simulates a nearly full volume).
    std::optional<std::uint64_t> available_bytes;
    // Called before the chunk at `offset` is written; a non-zero Win32 error code fails that
    // write as if WriteFile had (ERROR_DISK_FULL simulates running out of space mid-copy).
    std::function<std::uint32_t(std::uint64_t offset)> write_fault;
    // Called with each chunk after it was hashed and before it is written; may alter the bytes
    // (simulates a corrupting write path, which verification must catch).
    std::function<void(std::uint64_t offset, std::span<std::byte> chunk)> before_write;
    // Called after each chunk was written, with the bytes copied so far (modify the source,
    // request cancellation, ...).
    std::function<void(std::uint64_t bytes_copied)> after_chunk;
    // Called before each publication attempt with the candidate final path (create a file there
    // to simulate a concurrent writer winning the name).
    std::function<void(const std::filesystem::path& candidate)> before_publish;
};

[[nodiscard]] Expected<CopyReceipt> copy_file_verified_with_hooks(
    const std::filesystem::path& source, const std::filesystem::path& destination_dir,
    const std::filesystem::path& file_name, const CopyOptions& options, const CancellationToken& cancel,
    const CopyProgressCallback& progress, const CopyTestHooks& hooks);

}  // namespace lucent::core::detail
