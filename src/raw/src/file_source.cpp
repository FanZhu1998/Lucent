#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <utility>

#include <lucent/raw/file_source.hpp>

namespace lucent::raw {

Expected<FileSource> FileSource::create(const core::ReadOnlyFile& file) {
    auto size = file.size();
    if (!size) {
        return Unexpected(std::move(size.error()));
    }
    return FileSource(file, *size);
}

Expected<std::size_t> FileSource::read_at(std::uint64_t offset, std::span<std::byte> destination) {
    if (offset >= size_ || destination.empty()) {
        return std::size_t{0};
    }
    const auto expected =
        static_cast<std::size_t>(std::min<std::uint64_t>(destination.size(), size_ - offset));
    auto read = file_->read_at(offset, destination.first(expected));
    if (!read) {
        return Unexpected(std::move(read.error()));
    }
    if (*read != expected) {
        return fail(ErrorCode::SourceChanged,
                    std::format("file ended at byte {} although it had {} bytes when it was opened",
                                offset + *read, size_),
                    "raw.file_source.read");
    }
    return expected;
}

}  // namespace lucent::raw
