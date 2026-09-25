#pragma once

// Owning pixel storage (ImageBuffer) and non-owning views (ImageView / ConstImageView).
//
// Memory layout, identical for buffers and views:
// - Samples are interleaved: a pixel's channels are adjacent (R G B R G B ...).
// - Rows are stride() samples apart; stride() >= width() * channels(). The sample for pixel
//   (x, y), channel c is at data()[y * stride() + x * channels() + c].
// - For ImageBuffer every row starts on a kRowAlignment (64-byte) boundary, which covers one
//   cache line and the widest SIMD load. Sub-rectangle views keep the parent stride but their
//   first sample is generally not aligned.
//
// These types manage memory only. What the channels mean (colour encoding, CFA layout,
// orientation) is carried separately by the owner as data (see color_encoding.hpp and cfa.hpp).
//
// Sample types: std::uint16_t (integer sensor data) and float (working pixels; scene-linear
// values may be negative or exceed 1.0).
//
// Thread safety: no internal synchronisation. Concurrent reads are safe; concurrent writes to
// disjoint pixels (for example disjoint row bands) are safe; anything else needs external
// synchronisation. A view does not keep its buffer alive: the buffer must outlive every view
// taken from it, and moving a buffer leaves existing views pointing at the same (still valid)
// storage now owned by the destination.

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

#include <lucent/core/error.hpp>
#include <lucent/image/geometry.hpp>

namespace lucent::image {

// Row alignment of ImageBuffer storage in bytes.
inline constexpr std::size_t kRowAlignment = 64;

// Supported channel counts are 1..kMaxChannels (mono, two-plane, RGB, RGBA).
inline constexpr std::int32_t kMaxChannels = 4;

template <class T>
concept PixelSample = std::same_as<T, std::uint16_t> || std::same_as<T, float>;

// Per-buffer allocation limits. They reject corrupt or hostile dimensions before any memory is
// touched; they are not a memory budget (process-wide admission control is a separate concern,
// blueprint section 7).
//
// Defaults, chosen for 12-16 GB laptops:
// - kDefaultMaxBytes = 2 GiB per buffer (row padding included). This admits a 150-megapixel
//   medium-format frame as float RGB (about 1.8 GB) and a 100-megapixel frame as float RGBA
//   (about 1.6 GB), while a single buffer can never claim more than about a sixth of a 12 GB
//   machine. It matches LibRaw's own default allocation ceiling (LIBRAW_MAX_ALLOC_MB_DEFAULT
//   = 2048 MB in libraw/libraw_const.h). Larger images (pixel-shift composites, panoramas) must
//   be processed in tiles or with deliberately raised limits.
// - kDefaultMaxDimension = 65535 pixels per side: above every current sensor (largest about
//   23 000 px for pixel-shift output) and equal to the JPEG dimension limit.
struct AllocationLimits {
    static constexpr std::size_t kDefaultMaxBytes = std::size_t{2} << 30;
    static constexpr std::int32_t kDefaultMaxDimension = 65535;

    std::size_t max_bytes = kDefaultMaxBytes;
    std::int32_t max_dimension = kDefaultMaxDimension;
};

namespace detail {

// Row stride and total size for an interleaved buffer with 64-byte-aligned rows.
struct BufferLayout {
    std::ptrdiff_t stride = 0;  // samples per row, a multiple of kRowAlignment / sample_size
    std::size_t byte_size = 0;  // stride * height * sample_size
};

// Validates dimensions against `limits` with overflow-checked arithmetic.
// InvalidArgument: width or height < 1, channels outside 1..kMaxChannels.
// LimitExceeded: a dimension above limits.max_dimension, a size that overflows, or a total
// above limits.max_bytes.
[[nodiscard]] Expected<BufferLayout> plan_buffer_layout(std::int32_t width, std::int32_t height,
                                                        std::int32_t channels, std::size_t sample_size,
                                                        const AllocationLimits& limits);

// Allocates `bytes` (> 0) aligned to kRowAlignment. OutOfMemory when the allocation fails.
[[nodiscard]] Expected<void*> allocate_aligned(std::size_t bytes);

// Releases memory from allocate_aligned. Null is ignored.
void free_aligned(void* memory) noexcept;

struct AlignedFree {
    void operator()(void* memory) const noexcept { free_aligned(memory); }
};

}  // namespace detail

template <PixelSample T>
class ImageBuffer;

// Non-owning view of interleaved samples. E is the sample type, const-qualified for read-only
// views. Views are cheap to copy. A default-constructed view is empty (null data, zero size).
template <class E>
    requires PixelSample<std::remove_const_t<E>>
class BasicImageView {
public:
    using element_type = E;
    using value_type = std::remove_const_t<E>;

    constexpr BasicImageView() noexcept = default;

    // A mutable view converts implicitly to a read-only view of the same samples.
    template <class U>
        requires(std::is_const_v<E> && std::same_as<U, value_type>)
    constexpr BasicImageView(const BasicImageView<U>& other) noexcept  // NOLINT(google-explicit-constructor)
        : data_(other.data()),
          width_(other.width()),
          height_(other.height()),
          channels_(other.channels()),
          stride_(other.stride()) {}

    // Wraps caller-owned memory (for example a decoder's output) without copying.
    // Requirements: data is non-null; width, height >= 1; channels in 1..kMaxChannels;
    // stride >= width * channels samples. No alignment is required. The caller guarantees that
    // height rows of stride samples are addressable for the lifetime of the view.
    // Fails with InvalidArgument.
    [[nodiscard]] static Expected<BasicImageView> wrap(E* data, std::int32_t width, std::int32_t height,
                                                       std::int32_t channels, std::ptrdiff_t stride) {
        if (data == nullptr || width < 1 || height < 1 || channels < 1 || channels > kMaxChannels ||
            stride < std::ptrdiff_t{width} * channels) {
            return fail(ErrorCode::InvalidArgument,
                        "view requires non-null data, width and height >= 1, 1..4 channels and "
                        "stride >= width * channels",
                        "image.view.wrap");
        }
        return BasicImageView(data, width, height, channels, stride);
    }

    [[nodiscard]] constexpr E* data() const noexcept { return data_; }
    [[nodiscard]] constexpr std::int32_t width() const noexcept { return width_; }
    [[nodiscard]] constexpr std::int32_t height() const noexcept { return height_; }
    [[nodiscard]] constexpr std::int32_t channels() const noexcept { return channels_; }
    // Distance between the starts of consecutive rows, in samples (not bytes).
    [[nodiscard]] constexpr std::ptrdiff_t stride() const noexcept { return stride_; }
    [[nodiscard]] constexpr Size size() const noexcept { return Size{width_, height_}; }
    [[nodiscard]] constexpr bool empty() const noexcept { return data_ == nullptr; }

    // First sample of row y. Precondition (asserted): 0 <= y < height().
    [[nodiscard]] E* row(std::int32_t y) const noexcept {
        assert(y >= 0 && y < height_);
        return data_ + static_cast<std::ptrdiff_t>(y) * stride_;
    }

    // The width() * channels() samples of row y, excluding padding.
    [[nodiscard]] std::span<E> row_span(std::int32_t y) const noexcept {
        return std::span<E>(row(y), static_cast<std::size_t>(width_) * static_cast<std::size_t>(channels_));
    }

    // First channel of pixel (x, y). Precondition (asserted): the pixel is inside the view.
    [[nodiscard]] E* pixel(std::int32_t x, std::int32_t y) const noexcept {
        assert(x >= 0 && x < width_);
        return row(y) + static_cast<std::ptrdiff_t>(x) * channels_;
    }

    // Sample (x, y, c). Precondition (asserted in debug builds): inside the view and
    // 0 <= c < channels().
    [[nodiscard]] E& at(std::int32_t x, std::int32_t y, std::int32_t c) const noexcept {
        assert(c >= 0 && c < channels_);
        return pixel(x, y)[c];
    }

    // View of a sub-rectangle sharing this view's storage and stride. The rectangle must be
    // non-empty and lie inside the view; otherwise InvalidArgument.
    [[nodiscard]] Expected<BasicImageView> view(const PixelRect& rect) const {
        if (auto status = rect.validate_within(size()); !status) {
            return Unexpected(std::move(status.error()));
        }
        return BasicImageView(pixel(rect.x, rect.y), rect.width, rect.height, channels_, stride_);
    }

private:
    template <PixelSample>
    friend class ImageBuffer;

    constexpr BasicImageView(E* data, std::int32_t width, std::int32_t height, std::int32_t channels,
                             std::ptrdiff_t stride) noexcept
        : data_(data), width_(width), height_(height), channels_(channels), stride_(stride) {}

    E* data_ = nullptr;
    std::int32_t width_ = 0;
    std::int32_t height_ = 0;
    std::int32_t channels_ = 0;
    std::ptrdiff_t stride_ = 0;
};

template <PixelSample T>
using ImageView = BasicImageView<T>;

template <PixelSample T>
using ConstImageView = BasicImageView<const T>;

// Owning, move-only interleaved image with 64-byte-aligned rows. Copies are explicit (clone())
// because a full-resolution float image is hundreds of megabytes.
//
// A default-constructed or moved-from buffer is empty: data() is null and all dimensions are 0.
template <PixelSample T>
class ImageBuffer {
public:
    using value_type = T;

    ImageBuffer() noexcept = default;
    ImageBuffer(ImageBuffer&& other) noexcept
        : data_(std::move(other.data_)),
          width_(std::exchange(other.width_, 0)),
          height_(std::exchange(other.height_, 0)),
          channels_(std::exchange(other.channels_, 0)),
          stride_(std::exchange(other.stride_, 0)),
          byte_size_(std::exchange(other.byte_size_, 0)) {}
    ImageBuffer& operator=(ImageBuffer&& other) noexcept {
        if (this != &other) {
            data_ = std::move(other.data_);
            width_ = std::exchange(other.width_, 0);
            height_ = std::exchange(other.height_, 0);
            channels_ = std::exchange(other.channels_, 0);
            stride_ = std::exchange(other.stride_, 0);
            byte_size_ = std::exchange(other.byte_size_, 0);
        }
        return *this;
    }
    ImageBuffer(const ImageBuffer&) = delete;
    ImageBuffer& operator=(const ImageBuffer&) = delete;
    ~ImageBuffer() = default;

    // Allocates a width x height image with `channels` interleaved channels and every byte,
    // row padding included, set to zero (so results never depend on uninitialised memory).
    // InvalidArgument: width or height < 1, or channels outside 1..kMaxChannels.
    // LimitExceeded: a dimension above limits.max_dimension, or a total size (with padding)
    // above limits.max_bytes or not representable.
    // OutOfMemory: the allocation failed.
    [[nodiscard]] static Expected<ImageBuffer> create(std::int32_t width, std::int32_t height,
                                                      std::int32_t channels,
                                                      const AllocationLimits& limits = {}) {
        return allocate(width, height, channels, limits, /*zero_samples=*/true);
    }

    // As create(), but pixel samples are left indeterminate; only the row padding is zeroed.
    // For hot paths that overwrite every sample before anything reads the buffer.
    [[nodiscard]] static Expected<ImageBuffer> create_uninitialized(std::int32_t width, std::int32_t height,
                                                                    std::int32_t channels,
                                                                    const AllocationLimits& limits = {}) {
        return allocate(width, height, channels, limits, /*zero_samples=*/false);
    }

    // Deep copy with identical dimensions, stride and contents. Limits are not re-applied (the
    // source was admitted when it was created). OutOfMemory when the allocation fails. Cloning
    // an empty buffer yields an empty buffer.
    [[nodiscard]] Expected<ImageBuffer> clone() const {
        if (empty()) {
            return ImageBuffer{};
        }
        auto memory = detail::allocate_aligned(byte_size_);
        if (!memory) {
            return Unexpected(std::move(memory.error()));
        }
        ImageBuffer copy;
        copy.data_.reset(static_cast<T*>(*memory));
        std::memcpy(copy.data_.get(), data_.get(), byte_size_);
        copy.width_ = width_;
        copy.height_ = height_;
        copy.channels_ = channels_;
        copy.stride_ = stride_;
        copy.byte_size_ = byte_size_;
        return copy;
    }

    [[nodiscard]] T* data() noexcept { return data_.get(); }
    [[nodiscard]] const T* data() const noexcept { return data_.get(); }
    [[nodiscard]] std::int32_t width() const noexcept { return width_; }
    [[nodiscard]] std::int32_t height() const noexcept { return height_; }
    [[nodiscard]] std::int32_t channels() const noexcept { return channels_; }
    // Distance between the starts of consecutive rows, in samples (not bytes).
    [[nodiscard]] std::ptrdiff_t stride() const noexcept { return stride_; }
    // Total allocation in bytes, row padding included.
    [[nodiscard]] std::size_t byte_size() const noexcept { return byte_size_; }
    [[nodiscard]] Size size() const noexcept { return Size{width_, height_}; }
    [[nodiscard]] bool empty() const noexcept { return data_ == nullptr; }

    // First sample of row y (64-byte aligned). Precondition (asserted): 0 <= y < height().
    [[nodiscard]] T* row(std::int32_t y) noexcept { return view().row(y); }
    [[nodiscard]] const T* row(std::int32_t y) const noexcept { return const_view().row(y); }

    // Sample (x, y, c). Precondition (asserted in debug builds): inside the image and
    // 0 <= c < channels().
    [[nodiscard]] T& at(std::int32_t x, std::int32_t y, std::int32_t c) noexcept {
        return view().at(x, y, c);
    }
    [[nodiscard]] const T& at(std::int32_t x, std::int32_t y, std::int32_t c) const noexcept {
        return const_view().at(x, y, c);
    }

    // Whole-image views. Valid until the storage is released (destruction or move-assignment
    // into this buffer).
    [[nodiscard]] ImageView<T> view() noexcept {
        return ImageView<T>(data_.get(), width_, height_, channels_, stride_);
    }
    [[nodiscard]] ConstImageView<T> view() const noexcept { return const_view(); }
    [[nodiscard]] ConstImageView<T> const_view() const noexcept {
        return ConstImageView<T>(data_.get(), width_, height_, channels_, stride_);
    }

    // Sub-rectangle views; InvalidArgument unless the rectangle is non-empty and inside.
    [[nodiscard]] Expected<ImageView<T>> view(const PixelRect& rect) { return view().view(rect); }
    [[nodiscard]] Expected<ConstImageView<T>> view(const PixelRect& rect) const {
        return const_view().view(rect);
    }

private:
    static Expected<ImageBuffer> allocate(std::int32_t width, std::int32_t height, std::int32_t channels,
                                          const AllocationLimits& limits, bool zero_samples) {
        auto layout = detail::plan_buffer_layout(width, height, channels, sizeof(T), limits);
        if (!layout) {
            return Unexpected(std::move(layout.error()));
        }
        auto memory = detail::allocate_aligned(layout->byte_size);
        if (!memory) {
            return Unexpected(std::move(memory.error()));
        }
        ImageBuffer buffer;
        buffer.data_.reset(static_cast<T*>(*memory));
        buffer.width_ = width;
        buffer.height_ = height;
        buffer.channels_ = channels;
        buffer.stride_ = layout->stride;
        buffer.byte_size_ = layout->byte_size;
        if (zero_samples) {
            std::memset(buffer.data_.get(), 0, buffer.byte_size_);
        } else {
            const std::ptrdiff_t used = std::ptrdiff_t{width} * channels;
            const auto padding_bytes = static_cast<std::size_t>(buffer.stride_ - used) * sizeof(T);
            if (padding_bytes != 0) {
                for (std::int32_t y = 0; y < height; ++y) {
                    std::memset(buffer.row(y) + used, 0, padding_bytes);
                }
            }
        }
        return buffer;
    }

    std::unique_ptr<T, detail::AlignedFree> data_;
    std::int32_t width_ = 0;
    std::int32_t height_ = 0;
    std::int32_t channels_ = 0;
    std::ptrdiff_t stride_ = 0;
    std::size_t byte_size_ = 0;
};

// Explicitly instantiated in pixel_buffer.cpp.
extern template class BasicImageView<std::uint16_t>;
extern template class BasicImageView<const std::uint16_t>;
extern template class BasicImageView<float>;
extern template class BasicImageView<const float>;
extern template class ImageBuffer<std::uint16_t>;
extern template class ImageBuffer<float>;

}  // namespace lucent::image
