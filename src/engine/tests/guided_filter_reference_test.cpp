// Guided-filter parity with OpenCV's cv2.ximgproc.guidedFilter (blueprint Appendix A.2).
//
// src/engine/tests/data/guided_filter/*.json are written by tests/tools/guided_filter_reference.py
// (opencv-contrib-python 5.0.0.93, numpy 2.5.3): deterministic float32 guides and inputs, OpenCV's
// result and the exact (float64) evaluation of the same formula with OpenCV's conventions
// (normalised box mean, BORDER_REFLECT, eps in squared guide units), which the tool verifies
// against OpenCV before writing.
//
// Tolerances (absolute, in the units of the filtered values):
//   * against the exact result: 2e-7 * max(1, |guide|max). Lucent sums windows in double but
//     keeps the window means in float32 planes between the passes (relative error 6e-8); the
//     guide is shifted by its mean, so var_I loses nothing to the offset, and the output and the
//     stored reference are float32 (6e-8 relative each). Measured maxima: 6e-8 (unit_range),
//     1.2e-7 (two_image, large_radius), 4.8e-7 (step_noise, |I| up to 6.3), 9.5e-7
//     (ramp_texture, |I| up to 10).
//   * against OpenCV: the stored OpenCV-versus-exact deviation of the case (OpenCV's own float32
//     rounding, up to 1.2e-4 on step_noise) plus the tolerance above.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <lucent/core/hash.hpp>
#include <lucent/engine/guided_filter.hpp>
#include <lucent/image/pixel_buffer.hpp>

#include "sensor_test_support.hpp"

namespace lucent::engine {
namespace {

namespace ts = test_support;
static_assert(std::endian::native == std::endian::little, "the references store little-endian float32");

std::filesystem::path reference_dir() {
    return std::filesystem::path(LUCENT_SOURCE_DIR) / "src" / "engine" / "tests" / "data" / "guided_filter";
}

std::vector<std::filesystem::path> reference_files() {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(reference_dir(), ec)) {
        if (entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

// Decodes a {base64, sha256} float32 array of width x height x channels into a buffer.
std::optional<image::ImageBuffer<float>> decode_plane(const nlohmann::json& node, std::int32_t width,
                                                      std::int32_t height, std::int32_t channels) {
    const auto bytes = ts::base64_decode(node.at("base64").get<std::string>());
    if (!bytes) {
        return std::nullopt;
    }
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                              static_cast<std::size_t>(channels);
    if (bytes->size() != count * sizeof(float)) {
        return std::nullopt;
    }
    const auto digest = core::sha256(std::span<const std::byte>(*bytes));
    if (!digest || core::to_hex(*digest) != node.at("sha256").get<std::string>()) {
        return std::nullopt;
    }
    auto buffer = image::ImageBuffer<float>::create(width, height, channels);
    if (!buffer) {
        return std::nullopt;
    }
    const auto* values = reinterpret_cast<const float*>(bytes->data());
    for (std::int32_t y = 0; y < height; ++y) {
        std::memcpy(buffer->row(y), values + static_cast<std::size_t>(y) * width * channels,
                    static_cast<std::size_t>(width) * channels * sizeof(float));
    }
    return std::move(*buffer);
}

struct Comparison {
    double max_exact = 0.0;
    double max_opencv = 0.0;
};

TEST(GuidedFilterReference, MatchesOpenCvAndTheExactFormula) {
    const auto files = reference_files();
    ASSERT_GE(files.size(), 5u) << "reference files missing in " << reference_dir().string();
    for (const auto& path : files) {
        SCOPED_TRACE(path.filename().string());
        std::ifstream in(path, std::ios::binary);
        ASSERT_TRUE(in) << path.string();
        const nlohmann::json j = nlohmann::json::parse(in);
        ASSERT_EQ(j.at("schema").get<std::string>(), "lucent.guided-filter-reference");
        ASSERT_EQ(j.at("version").get<int>(), 1);
        const auto width = j.at("width").get<std::int32_t>();
        const auto height = j.at("height").get<std::int32_t>();
        const auto channels = j.at("input_channels").get<std::int32_t>();
        const auto radius = j.at("radius").get<std::int32_t>();
        const double eps = j.at("eps").get<double>();
        auto guide = decode_plane(j.at("guide"), width, height, 1);
        ASSERT_TRUE(guide.has_value());
        std::optional<image::ImageBuffer<float>> input;
        if (!j.at("input").is_null()) {
            input = decode_plane(j.at("input"), width, height, channels);
            ASSERT_TRUE(input.has_value());
        }
        auto opencv = decode_plane(j.at("opencv"), width, height, channels);
        auto exact = decode_plane(j.at("exact"), width, height, channels);
        ASSERT_TRUE(opencv.has_value());
        ASSERT_TRUE(exact.has_value());
        const double opencv_deviation = j.at("opencv_vs_exact_max").get<double>();

        float guide_max = 0.0f;
        for (std::int32_t y = 0; y < height; ++y) {
            for (std::int32_t x = 0; x < width; ++x) {
                guide_max = std::max(guide_max, std::abs(guide->at(x, y, 0)));
            }
        }
        const double scale = std::max(1.0, static_cast<double>(guide_max));
        const double tolerance = 2e-7 * scale;

        auto output = image::ImageBuffer<float>::create(width, height, channels);
        ASSERT_TRUE(output);
        const image::ConstImageView<float> in_view = input ? input->const_view() : guide->const_view();
        auto status = guided_filter(guide->const_view(), in_view, output->view(), radius, eps);
        ASSERT_TRUE(status) << status.error().describe();

        Comparison c;
        for (std::int32_t y = 0; y < height; ++y) {
            for (std::int32_t x = 0; x < width; ++x) {
                for (std::int32_t k = 0; k < channels; ++k) {
                    const double q = output->at(x, y, k);
                    c.max_exact = std::max(c.max_exact, std::abs(q - exact->at(x, y, k)));
                    c.max_opencv = std::max(c.max_opencv, std::abs(q - opencv->at(x, y, k)));
                }
            }
        }
        EXPECT_LE(c.max_exact, tolerance) << "radius " << radius << ", eps " << eps;
        EXPECT_LE(c.max_opencv, opencv_deviation + tolerance) << "radius " << radius << ", eps " << eps;
        std::cout << path.filename().string() << ": max |Lucent - exact| " << c.max_exact
                  << ", max |Lucent - OpenCV| " << c.max_opencv << " (OpenCV vs exact " << opencv_deviation
                  << ")\n";

        // In place (output aliasing the input) gives the identical result.
        if (!input) {
            auto copy = guide->clone();
            ASSERT_TRUE(copy);
            auto in_place = guided_filter(copy->const_view(), copy->const_view(), copy->view(), radius, eps);
            ASSERT_TRUE(in_place) << in_place.error().describe();
            for (std::int32_t y = 0; y < height; ++y) {
                ASSERT_EQ(std::memcmp(copy->row(y), output->row(y), static_cast<std::size_t>(width) * sizeof(float)),
                          0);
            }
        }
    }
}

}  // namespace
}  // namespace lucent::engine
