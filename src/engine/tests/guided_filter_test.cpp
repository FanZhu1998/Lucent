// Unit tests for the box mean and the guided filter (guided_filter.hpp) on synthetic data: an
// independent brute-force evaluation in double (explicit windows, explicit mirroring), edge and
// constant behaviour, aliasing, determinism, parallel/serial identity and failure paths. Parity
// with OpenCV itself is in guided_filter_reference_test.cpp.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/core/cancellation.hpp>
#include <lucent/engine/guided_filter.hpp>
#include <lucent/image/pixel_buffer.hpp>

#include "sensor_test_support.hpp"

namespace lucent::engine {
namespace {

namespace ts = test_support;

// "fedcba|abcdef" mirroring for any offset.
std::int32_t mirror(std::int64_t p, std::int32_t n) {
    if (n == 1) {
        return 0;
    }
    while (p < 0 || p >= n) {
        p = p < 0 ? -p - 1 : 2 * static_cast<std::int64_t>(n) - 1 - p;
    }
    return static_cast<std::int32_t>(p);
}

using Plane = std::vector<double>;  // row-major, one channel

Plane brute_box(const Plane& a, std::int32_t w, std::int32_t h, std::int32_t r) {
    Plane out(a.size());
    const double n = static_cast<double>((2 * r + 1) * (2 * r + 1));
    for (std::int32_t y = 0; y < h; ++y) {
        for (std::int32_t x = 0; x < w; ++x) {
            double s = 0.0;
            for (std::int32_t dy = -r; dy <= r; ++dy) {
                for (std::int32_t dx = -r; dx <= r; ++dx) {
                    s += a[static_cast<std::size_t>(mirror(y + dy, h)) * w + mirror(x + dx, w)];
                }
            }
            out[static_cast<std::size_t>(y) * w + x] = s / n;
        }
    }
    return out;
}

Plane brute_guided(const Plane& guide, const Plane& input, std::int32_t w, std::int32_t h, std::int32_t r,
                   double eps) {
    Plane ii(guide.size()), ip(guide.size());
    for (std::size_t i = 0; i < guide.size(); ++i) {
        ii[i] = guide[i] * guide[i];
        ip[i] = guide[i] * input[i];
    }
    const Plane mi = brute_box(guide, w, h, r);
    const Plane mp = brute_box(input, w, h, r);
    const Plane mii = brute_box(ii, w, h, r);
    const Plane mip = brute_box(ip, w, h, r);
    Plane a(guide.size()), b(guide.size());
    for (std::size_t i = 0; i < guide.size(); ++i) {
        a[i] = (mip[i] - mi[i] * mp[i]) / (mii[i] - mi[i] * mi[i] + eps);
        b[i] = mp[i] - a[i] * mi[i];
    }
    const Plane ma = brute_box(a, w, h, r);
    const Plane mb = brute_box(b, w, h, r);
    Plane q(guide.size());
    for (std::size_t i = 0; i < guide.size(); ++i) {
        q[i] = ma[i] * guide[i] + mb[i];
    }
    return q;
}

image::ImageBuffer<float> to_buffer(const Plane& p, std::int32_t w, std::int32_t h) {
    auto b = image::ImageBuffer<float>::create(w, h, 1);
    EXPECT_TRUE(b);
    for (std::int32_t y = 0; y < h; ++y) {
        for (std::int32_t x = 0; x < w; ++x) {
            b->at(x, y, 0) = static_cast<float>(p[static_cast<std::size_t>(y) * w + x]);
        }
    }
    return std::move(*b);
}

Plane random_plane(std::int32_t w, std::int32_t h, double lo, double hi, std::uint32_t seed) {
    std::mt19937 gen(seed);
    Plane p(static_cast<std::size_t>(w) * h);
    for (double& v : p) {
        // Exact float values so that the brute force sees what the filter sees.
        v = static_cast<float>(lo + (hi - lo) * (static_cast<double>(gen()) / 4294967296.0));
    }
    return p;
}

double max_difference(const image::ImageBuffer<float>& a, const Plane& b) {
    double m = 0.0;
    for (std::int32_t y = 0; y < a.height(); ++y) {
        for (std::int32_t x = 0; x < a.width(); ++x) {
            m = std::max(m, std::abs(a.at(x, y, 0) - b[static_cast<std::size_t>(y) * a.width() + x]));
        }
    }
    return m;
}

TEST(BoxMean, MatchesBruteForceIncludingWindowsLargerThanTheImage) {
    for (const auto [w, h, r] : {std::array<std::int32_t, 3>{17, 11, 1}, {17, 11, 3}, {9, 7, 12}, {1, 5, 2},
                                 {64, 3, 30}, {130, 70, 9}}) {
        SCOPED_TRACE(testing::Message() << w << "x" << h << " r" << r);
        const Plane p = random_plane(w, h, -3.0, 5.0, 7u * static_cast<std::uint32_t>(w + h + r));
        auto buffer = to_buffer(p, w, h);
        ASSERT_TRUE(box_mean(buffer.view(), r));
        // Two float32 roundings of means of values below 5.
        EXPECT_LE(max_difference(buffer, brute_box(p, w, h, r)), 2e-6);
    }
}

TEST(BoxMean, FiltersInterleavedChannelsIndependently) {
    const std::int32_t w = 23, h = 19, r = 4;
    const Plane a = random_plane(w, h, 0.0, 1.0, 11);
    const Plane b = random_plane(w, h, -2.0, 2.0, 12);
    auto rgb = image::ImageBuffer<float>::create(w, h, 2);
    ASSERT_TRUE(rgb);
    for (std::int32_t y = 0; y < h; ++y) {
        for (std::int32_t x = 0; x < w; ++x) {
            rgb->at(x, y, 0) = static_cast<float>(a[static_cast<std::size_t>(y) * w + x]);
            rgb->at(x, y, 1) = static_cast<float>(b[static_cast<std::size_t>(y) * w + x]);
        }
    }
    ASSERT_TRUE(box_mean(rgb->view(), r));
    const Plane ma = brute_box(a, w, h, r);
    const Plane mb = brute_box(b, w, h, r);
    for (std::int32_t y = 0; y < h; ++y) {
        for (std::int32_t x = 0; x < w; ++x) {
            ASSERT_NEAR(rgb->at(x, y, 0), ma[static_cast<std::size_t>(y) * w + x], 1e-6);
            ASSERT_NEAR(rgb->at(x, y, 1), mb[static_cast<std::size_t>(y) * w + x], 2e-6);
        }
    }
}

TEST(GuidedFilter, SelfGuidedMatchesBruteForce) {
    for (const auto [r, eps] : {std::pair<std::int32_t, double>{1, 0.001}, {3, 0.0625}, {8, 1.0}, {25, 0.01}}) {
        SCOPED_TRACE(testing::Message() << "r " << r << " eps " << eps);
        const std::int32_t w = 41, h = 29;
        const Plane p = random_plane(w, h, -8.0, -1.0, static_cast<std::uint32_t>(100 + r));
        auto guide = to_buffer(p, w, h);
        auto out = image::ImageBuffer<float>::create(w, h, 1);
        ASSERT_TRUE(out);
        ASSERT_TRUE(guided_filter(guide.const_view(), guide.const_view(), out->view(), r, eps));
        // Float32 window planes; |I - mean| <= 3.5 (see guided_filter_reference_test.cpp).
        EXPECT_LE(max_difference(*out, brute_guided(p, p, w, h, r, eps)), 2e-5 * 8.0 * 8.0);
    }
}

TEST(GuidedFilter, GuideAndMultiChannelInputMatchBruteForce) {
    const std::int32_t w = 33, h = 21, r = 2;
    const double eps = 0.02;
    const Plane g = random_plane(w, h, 0.0, 1.0, 5);
    const Plane p0 = random_plane(w, h, -1.0, 1.0, 6);
    const Plane p1 = random_plane(w, h, 2.0, 3.0, 7);
    auto guide = to_buffer(g, w, h);
    auto input = image::ImageBuffer<float>::create(w, h, 2);
    ASSERT_TRUE(input);
    for (std::int32_t y = 0; y < h; ++y) {
        for (std::int32_t x = 0; x < w; ++x) {
            input->at(x, y, 0) = static_cast<float>(p0[static_cast<std::size_t>(y) * w + x]);
            input->at(x, y, 1) = static_cast<float>(p1[static_cast<std::size_t>(y) * w + x]);
        }
    }
    auto out = image::ImageBuffer<float>::create(w, h, 2);
    ASSERT_TRUE(out);
    ASSERT_TRUE(guided_filter(guide.const_view(), input->const_view(), out->view(), r, eps));
    const Plane q0 = brute_guided(g, p0, w, h, r, eps);
    const Plane q1 = brute_guided(g, p1, w, h, r, eps);
    for (std::int32_t y = 0; y < h; ++y) {
        for (std::int32_t x = 0; x < w; ++x) {
            ASSERT_NEAR(out->at(x, y, 0), q0[static_cast<std::size_t>(y) * w + x], 2e-5);
            ASSERT_NEAR(out->at(x, y, 1), q1[static_cast<std::size_t>(y) * w + x], 6e-5);
        }
    }
}

TEST(GuidedFilter, KeepsConstantsAndStrongEdgesAndSmoothsWeakTexture) {
    const std::int32_t w = 48, h = 16, r = 4;
    // Constant: unchanged.
    {
        const Plane p(static_cast<std::size_t>(w) * h, -3.25);
        auto guide = to_buffer(p, w, h);
        auto out = image::ImageBuffer<float>::create(w, h, 1);
        ASSERT_TRUE(out);
        ASSERT_TRUE(guided_filter(guide.const_view(), guide.const_view(), out->view(), r, 0.0625));
        EXPECT_LE(max_difference(*out, p), 1e-6);
    }
    // A 4 EV step with +-0.05 texture: the step survives within 0.1 EV of the plateau, the
    // texture is attenuated and nothing overshoots the input range.
    {
        Plane p(static_cast<std::size_t>(w) * h);
        for (std::int32_t y = 0; y < h; ++y) {
            for (std::int32_t x = 0; x < w; ++x) {
                p[static_cast<std::size_t>(y) * w + x] = (x < 24 ? -6.0 : -2.0) + (((x + y) % 2) != 0 ? 0.05 : -0.05);
            }
        }
        auto guide = to_buffer(p, w, h);
        auto out = image::ImageBuffer<float>::create(w, h, 1);
        ASSERT_TRUE(out);
        ASSERT_TRUE(guided_filter(guide.const_view(), guide.const_view(), out->view(), r, 0.0625));
        for (std::int32_t y = 0; y < h; ++y) {
            EXPECT_NEAR(out->at(10, y, 0), -6.0, 0.02);
            EXPECT_NEAR(out->at(37, y, 0), -2.0, 0.02);
            EXPECT_LT(out->at(23, y, 0), -5.0);  // last dark column stays dark
            EXPECT_GT(out->at(24, y, 0), -3.0);  // first bright column stays bright
            for (std::int32_t x = 0; x < w; ++x) {
                EXPECT_GE(out->at(x, y, 0), -6.05 - 1e-4);
                EXPECT_LE(out->at(x, y, 0), -1.95 + 1e-4);
            }
        }
    }
}

TEST(GuidedFilter, IsDeterministicAndScheduleIndependent) {
    const std::int32_t w = 257, h = 131, r = 7;
    const Plane p = random_plane(w, h, -10.0, 0.0, 99);
    auto guide = to_buffer(p, w, h);
    auto a = image::ImageBuffer<float>::create(w, h, 1);
    auto b = image::ImageBuffer<float>::create(w, h, 1);
    auto c = image::ImageBuffer<float>::create(w, h, 1);
    ASSERT_TRUE(a && b && c);
    ASSERT_TRUE(guided_filter(guide.const_view(), guide.const_view(), a->view(), r, 0.0625, {}, {}, true));
    ASSERT_TRUE(guided_filter(guide.const_view(), guide.const_view(), b->view(), r, 0.0625, {}, {}, true));
    ASSERT_TRUE(guided_filter(guide.const_view(), guide.const_view(), c->view(), r, 0.0625, {}, {}, false));
    for (std::int32_t y = 0; y < h; ++y) {
        ASSERT_EQ(std::memcmp(a->row(y), b->row(y), static_cast<std::size_t>(w) * sizeof(float)), 0);
        ASSERT_EQ(std::memcmp(a->row(y), c->row(y), static_cast<std::size_t>(w) * sizeof(float)), 0);
    }
}

TEST(GuidedFilter, RejectsInvalidArgumentsAndHonoursCancellation) {
    auto one = image::ImageBuffer<float>::create(8, 8, 1);
    auto two = image::ImageBuffer<float>::create(8, 8, 2);
    auto small = image::ImageBuffer<float>::create(4, 8, 1);
    ASSERT_TRUE(one && two && small);
    const char* where = "engine.guided_filter";
    ts::expect_error(guided_filter(one->const_view(), one->const_view(), one->view(), 0, 0.1),
                     ErrorCode::InvalidArgument, where);
    ts::expect_error(guided_filter(one->const_view(), one->const_view(), one->view(), 2, 0.0),
                     ErrorCode::InvalidArgument, where);
    ts::expect_error(guided_filter(one->const_view(), one->const_view(), one->view(), 2, std::nan("")),
                     ErrorCode::InvalidArgument, where);
    ts::expect_error(guided_filter(two->const_view(), two->const_view(), two->view(), 2, 0.1),
                     ErrorCode::InvalidArgument, where);  // two-channel guide
    ts::expect_error(guided_filter(one->const_view(), small->const_view(), small->view(), 2, 0.1),
                     ErrorCode::InvalidArgument, where);
    ts::expect_error(guided_filter(one->const_view(), two->const_view(), one->view(), 2, 0.1),
                     ErrorCode::InvalidArgument, where);  // output channels differ from input
    ts::expect_error(guided_filter({}, one->const_view(), one->view(), 2, 0.1), ErrorCode::InvalidArgument, where);
    ts::expect_error(box_mean(one->view(), 0), ErrorCode::InvalidArgument, where);
    ts::expect_error(box_mean({}, 1), ErrorCode::InvalidArgument, where);

    core::CancellationSource source;
    source.cancel();
    ts::expect_error(guided_filter(one->const_view(), one->const_view(), one->view(), 2, 0.1, {}, source.token()),
                     ErrorCode::Cancelled, where);
    ts::expect_error(box_mean(one->view(), 1, source.token()), ErrorCode::Cancelled, where);

    image::AllocationLimits tiny;
    tiny.max_bytes = 16;
    ts::expect_error(guided_filter(one->const_view(), one->const_view(), one->view(), 2, 0.1, tiny),
                     ErrorCode::LimitExceeded, where);
}

}  // namespace
}  // namespace lucent::engine
