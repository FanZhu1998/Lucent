#pragma once

// Test seam for the publication protocol (white-box tests only; nothing else may use it).
//
// A hook installed on a thread is called by write_verified() on that thread after each protocol
// step with the temporary file's path. Returning an error makes write_verified() fail with that
// error at that point (and clean up); the hook may also manipulate files, for example truncate
// the temporary file or create the destination to simulate a racing writer.

#include <filesystem>

#include <lucent/core/error.hpp>

namespace lucent::exporter::detail {

enum class ExportStage {
    TempReserved,  // the empty temporary sibling exists
    Written,       // the encoder closed and flushed the temporary file
    Verified,      // the temporary file was re-read and verified
    Hashed,        // the temporary file was hashed; publication is next
};

using FaultHook = Status (*)(ExportStage stage, const std::filesystem::path& temp_path, void* context);

// Installs (or with nullptr removes) the calling thread's hook.
void set_fault_hook(FaultHook hook, void* context) noexcept;

// Calls the calling thread's hook, if any; ok() otherwise.
[[nodiscard]] Status run_fault_hook(ExportStage stage, const std::filesystem::path& temp_path);

}  // namespace lucent::exporter::detail
