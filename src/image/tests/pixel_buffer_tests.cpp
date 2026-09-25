#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/image/pixel_buffer.hpp>

namespace lucent::image {
namespace {

static_assert(!std::is_copy_constructible_v<ImageBuffer<float>>);
static_assert(!std::is_copy_assignable_v<ImageBuffer<float>>);
static_assert(std::is_nothrow_move_constructible_v<ImageBuffer<float>>);
static_assert(std::is_nothrow_move_assignable_v<ImageBuffer<std::uint16_t>>);
static_assert(std::is_convertible_v<ImageView<float>, ConstImageView<float>>);
static_assert(!std::is_convertible_v<ConstImageView<float>, ImageView<float>>);
static_assert(std::is_same_v<decltype(std::declval<ConstImageView<float>>().at(0, 0, 0)), const float&>);

constexpr AllocationLimits kUnlimited{std::numeric_limits<std::size_t>::max(),
                                      std::numeric_limits<std::int32_t>::max()};

bool all_bytes_zero(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        if (bytes[i] != 0) {
            return false;
        }
    }
    return true;
}

template <class T>
void check_layout(std::int32_t width, std::int32_t height, std::int32_t channels) {
    auto buffer = ImageBuffer<T>::create(width, height, channels);
    ASSERT_TRUE(buffer) << buffer.error().describe();
    const std::ptrdiff_t used = std::ptrdiff_t{width} * channels;
    const auto row_bytes = static_cast<std::size_t>(buffer->stride()) * sizeof(T);
    EXPECT_GE(buffer->stride(), used);
    EXPECT_EQ(row_bytes % kRowAlignment, 0u);
    // Padding is minimal: less than one alignment unit per row.
    EXPECT_LT(row_bytes - static_cast<std::size_t>(used) * sizeof(T), kRowAlignment);
    EXPECT_EQ(buffer->byte_size(), row_bytes * static_cast<std::size_t>(height));
    for (std::int32_t y = 0; y < height; ++y) {
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(buffer->row(y)) % kRowAlignment, 0u)
            << width << "x" << height << "x" << channels << " row " << y;
    }
}

TEST(ImageBuffer, RowsAre64ByteAlignedWithMinimalPadding) {
    for (std::int32_t channels = 1; channels <= kMaxChannels; ++channels) {
        for (std::int32_t width : {1, 3, 15, 16, 17, 31, 32, 33, 63, 64, 65, 1000}) {
            check_layout<float>(width, 3, channels);
            check_layout<std::uint16_t>(width, 3, channels);
        }
    }
}

TEST(ImageBuffer, CreateZeroFillsSamplesAndPadding) {
    auto buffer = ImageBuffer<float>::create(5, 3, 3);
    ASSERT_TRUE(buffer);
    EXPECT_EQ(buffer->width(), 5);
    EXPECT_EQ(buffer->height(), 3);
    EXPECT_EQ(buffer->channels(), 3);
    EXPECT_EQ(buffer->size(), (Size{5, 3}));
    EXPECT_FALSE(buffer->empty());
    EXPECT_TRUE(all_bytes_zero(buffer->data(), buffer->byte_size()));
}

TEST(ImageBuffer, CreateUninitializedZeroesRowPadding) {
    auto buffer = ImageBuffer<std::uint16_t>::create_uninitialized(3, 4, 3);  // 18 bytes used of 64
    ASSERT_TRUE(buffer);
    const std::ptrdiff_t used = std::ptrdiff_t{3} * 3;
    ASSERT_GT(buffer->stride(), used);
    for (std::int32_t y = 0; y < buffer->height(); ++y) {
        const auto padding = static_cast<std::size_t>(buffer->stride() - used) * sizeof(std::uint16_t);
        EXPECT_TRUE(all_bytes_zero(buffer->row(y) + used, padding)) << "row " << y;
    }
}

TEST(ImageBuffer, RejectsInvalidDimensionsAndChannels) {
    struct Case {
        std::int32_t width, height, channels;
    };
    for (const Case& c : {Case{0, 1, 1}, Case{1, 0, 1}, Case{-5, 3, 3}, Case{3, -5, 3}, Case{4, 4, 0},
                          Case{4, 4, 5}, Case{4, 4, -1}}) {
        auto buffer = ImageBuffer<float>::create(c.width, c.height, c.channels);
        ASSERT_FALSE(buffer) << c.width << "x" << c.height << "x" << c.channels;
        EXPECT_EQ(buffer.error().code, ErrorCode::InvalidArgument);
        EXPECT_EQ(buffer.error().where, "image.buffer.create");
    }
}

TEST(ImageBuffer, EnforcesDimensionLimit) {
    auto too_wide = ImageBuffer<std::uint16_t>::create(AllocationLimits::kDefaultMaxDimension + 1, 1, 1);
    ASSERT_FALSE(too_wide);
    EXPECT_EQ(too_wide.error().code, ErrorCode::LimitExceeded);

    const AllocationLimits small{AllocationLimits::kDefaultMaxBytes, 10};
    EXPECT_TRUE((ImageBuffer<float>::create(10, 10, 1, small)));
    auto too_tall = ImageBuffer<float>::create(10, 11, 1, small);
    ASSERT_FALSE(too_tall);
    EXPECT_EQ(too_tall.error().code, ErrorCode::LimitExceeded);
}

TEST(ImageBuffer, EnforcesByteLimitIncludingPadding) {
    // float, 10 x 2 x 3: 120 bytes of samples per row, padded to 128; 256 bytes in total.
    AllocationLimits limits;
    limits.max_bytes = 256;
    auto fits = ImageBuffer<float>::create(10, 2, 3, limits);
    ASSERT_TRUE(fits);
    EXPECT_EQ(fits->byte_size(), 256u);
    limits.max_bytes = 255;  // would fit without padding (240 bytes), but padding counts
    auto too_big = ImageBuffer<float>::create(10, 2, 3, limits);
    ASSERT_FALSE(too_big);
    EXPECT_EQ(too_big.error().code, ErrorCode::LimitExceeded);
}

TEST(ImageBuffer, DefaultLimitsAdmitLargeSensorsAndRejectOversizedBuffers) {
    // The size planner is checked without allocating: a 150 MP float RGB frame (14204 x 10652,
    // Phase One IQ4 150MP class) fits the 2 GiB default; the same frame as RGBA does not.
    auto rgb = detail::plan_buffer_layout(14204, 10652, 3, sizeof(float), AllocationLimits{});
    ASSERT_TRUE(rgb);
    EXPECT_LE(rgb->byte_size, AllocationLimits::kDefaultMaxBytes);
    auto rgba = detail::plan_buffer_layout(14204, 10652, 4, sizeof(float), AllocationLimits{});
    ASSERT_FALSE(rgba);
    EXPECT_EQ(rgba.error().code, ErrorCode::LimitExceeded);
}

TEST(ImageBuffer, SizeOverflowIsLimitExceeded) {
    // 2^31 x 2^31 x 4 floats = 2^66 bytes: the product wraps a 64-bit size_t. It must be
    // reported as LimitExceeded, never wrapped into a small allocation.
    constexpr std::int32_t kMax = std::numeric_limits<std::int32_t>::max();
    auto buffer = ImageBuffer<float>::create(kMax, kMax, 4, kUnlimited);
    ASSERT_FALSE(buffer);
    EXPECT_EQ(buffer.error().code, ErrorCode::LimitExceeded);
}

TEST(ImageBuffer, AllocationFailureIsOutOfMemory) {
    // 2^30 x 2^14 x 4 floats = 2^48 bytes (256 TiB): representable, allowed by the limits, and
    // larger than the 128 TiB user address space of 64-bit Windows, so the allocation fails
    // deterministically without touching memory.
    auto buffer = ImageBuffer<float>::create(1 << 30, 1 << 14, 4, kUnlimited);
    ASSERT_FALSE(buffer);
    EXPECT_EQ(buffer.error().code, ErrorCode::OutOfMemory);
    EXPECT_EQ(buffer.error().where, "image.buffer.allocate");
}

TEST(ImageBuffer, AtAndRowAddressInterleavedSamples) {
    auto buffer = ImageBuffer<std::uint16_t>::create(7, 5, 3);
    ASSERT_TRUE(buffer);
    for (std::int32_t y = 0; y < 5; ++y) {
        for (std::int32_t x = 0; x < 7; ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                buffer->at(x, y, c) = static_cast<std::uint16_t>(y * 100 + x * 10 + c);
            }
        }
    }
    for (std::int32_t y = 0; y < 5; ++y) {
        const std::uint16_t* row = buffer->row(y);
        EXPECT_EQ(row, buffer->data() + y * buffer->stride());
        for (std::int32_t x = 0; x < 7; ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                EXPECT_EQ(row[x * 3 + c], y * 100 + x * 10 + c);
            }
        }
    }
    const ImageBuffer<std::uint16_t>& const_buffer = *buffer;
    EXPECT_EQ(const_buffer.at(6, 4, 2), 462);
}

TEST(ImageBuffer, MoveTransfersOwnershipAndEmptiesSource) {
    auto created = ImageBuffer<float>::create(4, 4, 1);
    ASSERT_TRUE(created);
    ImageBuffer<float> a = std::move(*created);
    const float* storage = a.data();
    a.at(1, 2, 0) = 3.5f;

    ImageBuffer<float> b(std::move(a));
    EXPECT_EQ(b.data(), storage);
    EXPECT_EQ(b.at(1, 2, 0), 3.5f);
    EXPECT_TRUE(a.empty());  // NOLINT(bugprone-use-after-move): moved-from state is specified
    EXPECT_EQ(a.width(), 0);
    EXPECT_EQ(a.height(), 0);
    EXPECT_EQ(a.channels(), 0);
    EXPECT_EQ(a.stride(), 0);
    EXPECT_EQ(a.byte_size(), 0u);

    ImageBuffer<float> c;
    c = std::move(b);
    EXPECT_EQ(c.data(), storage);
    EXPECT_TRUE(b.empty());  // NOLINT(bugprone-use-after-move)
}

TEST(ImageBuffer, CloneIsAnIndependentDeepCopy) {
    auto original = ImageBuffer<float>::create(9, 4, 4);
    ASSERT_TRUE(original);
    for (std::int32_t y = 0; y < 4; ++y) {
        for (std::int32_t x = 0; x < 9; ++x) {
            for (std::int32_t c = 0; c < 4; ++c) {
                original->at(x, y, c) = static_cast<float>(x - y) * 0.25f + static_cast<float>(c);
            }
        }
    }
    auto copy = original->clone();
    ASSERT_TRUE(copy);
    EXPECT_NE(copy->data(), original->data());
    EXPECT_EQ(copy->size(), original->size());
    EXPECT_EQ(copy->channels(), original->channels());
    EXPECT_EQ(copy->stride(), original->stride());
    ASSERT_EQ(copy->byte_size(), original->byte_size());
    EXPECT_EQ(std::memcmp(copy->data(), original->data(), copy->byte_size()), 0);

    copy->at(3, 2, 1) = -42.0f;
    EXPECT_NE(original->at(3, 2, 1), -42.0f);

    auto empty_copy = ImageBuffer<float>{}.clone();
    ASSERT_TRUE(empty_copy);
    EXPECT_TRUE(empty_copy->empty());
}

TEST(ImageView, WholeImageViewMatchesBuffer) {
    auto buffer = ImageBuffer<float>::create(6, 3, 2);
    ASSERT_TRUE(buffer);
    ImageView<float> view = buffer->view();
    EXPECT_EQ(view.data(), buffer->data());
    EXPECT_EQ(view.size(), buffer->size());
    EXPECT_EQ(view.channels(), 2);
    EXPECT_EQ(view.stride(), buffer->stride());
    view.at(5, 2, 1) = 7.0f;
    EXPECT_EQ(buffer->at(5, 2, 1), 7.0f);
    EXPECT_EQ(view.row_span(1).size(), 12u);
    EXPECT_EQ(view.row_span(1).data(), buffer->row(1));

    const ConstImageView<float> read_only = view;  // implicit mutable -> const conversion
    EXPECT_EQ(read_only.at(5, 2, 1), 7.0f);
    EXPECT_EQ(std::as_const(*buffer).view().data(), buffer->data());

    EXPECT_TRUE(ImageView<float>{}.empty());
}

TEST(ImageView, SubViewSharesStorageAndStride) {
    auto buffer = ImageBuffer<std::uint16_t>::create(10, 8, 3);
    ASSERT_TRUE(buffer);
    auto sub = buffer->view(PixelRect{2, 3, 4, 5});
    ASSERT_TRUE(sub) << sub.error().describe();
    EXPECT_EQ(sub->size(), (Size{4, 5}));
    EXPECT_EQ(sub->stride(), buffer->stride());
    EXPECT_EQ(sub->pixel(0, 0), &buffer->at(2, 3, 0));
    sub->at(3, 4, 2) = 999;
    EXPECT_EQ(buffer->at(5, 7, 2), 999);

    // Nested sub-view, relative to the first one.
    auto nested = sub->view(PixelRect{1, 1, 2, 2});
    ASSERT_TRUE(nested);
    EXPECT_EQ(nested->pixel(0, 0), &buffer->at(3, 4, 0));

    // Const buffers yield const sub-views.
    auto const_sub = std::as_const(*buffer).view(PixelRect{0, 0, 1, 1});
    ASSERT_TRUE(const_sub);
    static_assert(std::is_same_v<decltype(const_sub->data()), const std::uint16_t*>);
}

TEST(ImageView, SubViewRejectsRectanglesOutsideTheImage) {
    auto buffer = ImageBuffer<float>::create(10, 8, 1);
    ASSERT_TRUE(buffer);
    for (const PixelRect& bad : {PixelRect{8, 0, 3, 1}, PixelRect{0, 7, 1, 2}, PixelRect{-1, 0, 2, 2},
                                 PixelRect{2, 2, 0, 2}, PixelRect{2, 2, 2, -1}}) {
        auto sub = buffer->view(bad);
        ASSERT_FALSE(sub);
        EXPECT_EQ(sub.error().code, ErrorCode::InvalidArgument);
    }
    auto whole = buffer->view(PixelRect{0, 0, 10, 8});
    EXPECT_TRUE(whole);
}

TEST(ImageView, WrapValidatesCallerMemory) {
    std::vector<std::uint16_t> samples(5 * 7, 0);  // 7 samples per row, 2 pixels of 3 channels
    auto view = ImageView<std::uint16_t>::wrap(samples.data(), 2, 5, 3, 7);
    ASSERT_TRUE(view);
    view->at(1, 4, 2) = 17;
    EXPECT_EQ(samples[4 * 7 + 1 * 3 + 2], 17);

    auto null_data = ImageView<std::uint16_t>::wrap(nullptr, 2, 5, 3, 7);
    auto small_stride = ImageView<std::uint16_t>::wrap(samples.data(), 3, 5, 3, 7);
    auto bad_channels = ImageView<std::uint16_t>::wrap(samples.data(), 1, 5, 5, 7);
    auto zero_height = ImageView<std::uint16_t>::wrap(samples.data(), 2, 0, 3, 7);
    for (const auto* result : {&null_data, &small_stride, &bad_channels, &zero_height}) {
        ASSERT_FALSE(*result);
        EXPECT_EQ(result->error().code, ErrorCode::InvalidArgument);
    }

    const std::vector<std::uint16_t>& const_samples = samples;
    auto read_only = ConstImageView<std::uint16_t>::wrap(const_samples.data(), 2, 5, 3, 7);
    ASSERT_TRUE(read_only);
    EXPECT_EQ(read_only->at(1, 4, 2), 17);
}

}  // namespace
}  // namespace lucent::image
