#pragma once

// Private hashing entry points. Not part of the public API.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>

#include "lucent/core/cancellation.hpp"
#include "lucent/core/error.hpp"
#include "lucent/core/hash.hpp"

namespace lucent::core::detail {

// Invoked by hash_file_observed() after each chunk has been hashed, with the total number of
// bytes hashed so far. White-box test seam: it lets tests modify, replace or delete the file at
// a precise point of a stable read. Production code passes an empty function.
using HashChunkObserver = std::function<void(std::uint64_t bytes_hashed)>;

// hash_file() with an optional per-chunk observer; identical behaviour otherwise.
[[nodiscard]] Expected<FileHashResult> hash_file_observed(const std::filesystem::path& path,
                                                          const CancellationToken& cancel,
                                                          std::size_t chunk_bytes,
                                                          const HashChunkObserver& after_chunk);

}  // namespace lucent::core::detail
