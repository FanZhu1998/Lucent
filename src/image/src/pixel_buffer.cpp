#include "lucent/image/pixel_buffer.hpp"

#include <format>
#include <limits>
#include <new>

namespace lucent::image {

namespace detail {

namespace {

constexpr std::size_t kSizeMax = std::numeric_limits<std::size_t>::max();

// a * b, or false when the product does not fit in size_t.
bool checked_multiply(std::size_t a, std::size_t b, std::size_t& product) noexcept {
    if (a != 0 && b > kSizeMax / a) {
        return false;
    }
    product = a * b;
    return true;
}

}  // namespace

Expected<BufferLayout> plan_buffer_layout(std::int32_t width, std::int32_t height, std::int32_t channels,
                                          std::size_t sample_size, const AllocationLimits& limits) {
    constexpr const char* kWhere = "image.buffer.create";
    if (width < 1 || height < 1) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("image dimensions must be at least 1x1 (got {}x{})", width, height), kWhere);
    }
    if (channels < 1 || channels > kMaxChannels) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("channel count must be 1..{} (got {})", kMaxChannels, channels), kWhere);
    }
    if (sample_size == 0 || kRowAlignment % sample_size != 0) {
        return fail(ErrorCode::Internal, std::format("unsupported sample size {}", sample_size), kWhere);
    }
    if (width > limits.max_dimension || height > limits.max_dimension) {
        return fail(ErrorCode::LimitExceeded,
                    std::format("image dimensions {}x{} exceed the {} pixel limit", width, height,
                                limits.max_dimension),
                    kWhere);
    }

    // Unpadded row size, then rounded up to the row alignment, then times the row count; every
    // step is overflow-checked so a hostile header cannot wrap the size to something small.
    std::size_t row_bytes = 0;
    std::size_t total = 0;
    const bool representable =
        checked_multiply(static_cast<std::size_t>(width) * static_cast<std::size_t>(channels), sample_size,
                         row_bytes) &&
        row_bytes <= kSizeMax - (kRowAlignment - 1) &&
        checked_multiply((row_bytes + kRowAlignment - 1) / kRowAlignment * kRowAlignment,
                         static_cast<std::size_t>(height), total);
    if (!representable || total > limits.max_bytes) {
        return fail(ErrorCode::LimitExceeded,
                    std::format("a {}x{}x{} image of {}-byte samples exceeds the {} byte allocation limit",
                                width, height, channels, sample_size, limits.max_bytes),
                    kWhere);
    }

    const std::size_t padded_row_bytes = (row_bytes + kRowAlignment - 1) / kRowAlignment * kRowAlignment;
    return BufferLayout{static_cast<std::ptrdiff_t>(padded_row_bytes / sample_size), total};
}

Expected<void*> allocate_aligned(std::size_t bytes) {
    if (bytes == 0) {
        return fail(ErrorCode::Internal, "zero-byte pixel allocation requested", "image.buffer.allocate");
    }
    try {
        return ::operator new(bytes, std::align_val_t{kRowAlignment});
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, std::format("could not allocate {} bytes for pixels", bytes),
                    "image.buffer.allocate");
    }
}

void free_aligned(void* memory) noexcept {
    if (memory != nullptr) {
        ::operator delete(memory, std::align_val_t{kRowAlignment});
    }
}

}  // namespace detail

template class BasicImageView<std::uint16_t>;
template class BasicImageView<const std::uint16_t>;
template class BasicImageView<float>;
template class BasicImageView<const float>;
template class ImageBuffer<std::uint16_t>;
template class ImageBuffer<float>;

}  // namespace lucent::image
