#include <algorithm>
#include <cstring>

#include <lucent/raw/tiff_inspector.hpp>

namespace lucent::raw {

Expected<std::size_t> MemorySource::read_at(std::uint64_t offset, std::span<std::byte> destination) {
    if (offset >= bytes_.size()) {
        return std::size_t{0};
    }
    const auto start = static_cast<std::size_t>(offset);
    const std::size_t count = std::min(destination.size(), bytes_.size() - start);
    if (count != 0) {
        std::memcpy(destination.data(), bytes_.data() + start, count);
    }
    return count;
}

}  // namespace lucent::raw
