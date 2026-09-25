#pragma once

// RandomAccessSource over an open lucent::core::ReadOnlyFile, so that the TIFF/DNG inspector
// (tiff_inspector.hpp) can read files without loading them into memory.

#include <cstddef>
#include <cstdint>
#include <span>

#include <lucent/core/error.hpp>
#include <lucent/core/file.hpp>
#include <lucent/raw/tiff_inspector.hpp>

namespace lucent::raw {

// Non-owning source over `file`, which must stay open and outlive the source. size() is the file
// size captured by create(); RandomAccessSource requires it to be constant.
//
// read_at() returns exactly min(destination.size(), size() - offset) bytes (0 at or past the
// captured end). When the file has become shorter than the captured size, so that this promise
// cannot be kept, it fails with ErrorCode::SourceChanged ("raw.file_source.read") instead of
// returning a short count. Read failures are passed through from ReadOnlyFile.
//
// Thread-safety: read_at() may be called concurrently (ReadOnlyFile reads carry their own
// offsets).
class FileSource final : public RandomAccessSource {
public:
    // Captures the current size of `file`. Fails as ReadOnlyFile::size() does (Internal for a
    // file that is not open).
    [[nodiscard]] static Expected<FileSource> create(const core::ReadOnlyFile& file);

    FileSource(FileSource&& other) noexcept = default;
    FileSource& operator=(FileSource&& other) noexcept = default;
    ~FileSource() override = default;

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }

    [[nodiscard]] Expected<std::size_t> read_at(std::uint64_t offset,
                                                std::span<std::byte> destination) override;

private:
    FileSource(const core::ReadOnlyFile& file, std::uint64_t size) noexcept : file_(&file), size_(size) {}

    const core::ReadOnlyFile* file_ = nullptr;
    std::uint64_t size_ = 0;
};

}  // namespace lucent::raw
