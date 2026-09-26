// Malvar-He-Cutler parity with the colour-demosaicing reference (blueprint Appendix A.3).
//
// fixtures/reference/demosaic/*.json are written by research/tools/demosaic_reference.py: each
// holds a 64 x 64 crop of a Bayer camera fixture, normalised as engine::normalize_sensor does,
// and colour-demosaicing's demosaicing_CFA_Bayer_Malvar2004 result for it (computed in float64,
// stored as float32). The reference convolves with mode "reflect", which does not keep the Bayer
// phase at the crop edge, so only pixels at least `interior_margin` (the kernel radius, 2) from
// the edge are compared.
//
// Tolerance 1e-5 (absolute, normalised units): Lucent evaluates the kernels in float32 with at
// most 13 products whose coefficients total at most 2.5 in magnitude; for the reference values
// (below 1.2) the accumulated float32 rounding is below about 3e-6, and storing the reference as
// float32 adds at most 6e-8.
//
// The committed-crop test needs no camera files. The second test also decodes each fixture and
// checks that Lucent's own normalisation reproduces the crop and that the full-image route
// reproduces the reference; it skips when the fixture is not downloaded.

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <lucent/core/hash.hpp>
#include <lucent/engine/demosaic.hpp>
#include <lucent/engine/sensor.hpp>
#include <lucent/raw/libraw_decoder.hpp>

#include "sensor_test_support.hpp"

namespace lucent::engine {
namespace {

namespace ts = test_support;
static_assert(std::endian::native == std::endian::little, "the references store little-endian float32");

constexpr double kTolerance = 1e-5;

std::filesystem::path reference_dir() {
    return std::filesystem::path(LUCENT_FIXTURE_DIR) / "reference" / "demosaic";
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

struct Reference {
    std::string fixture_id;
    std::string fixture_sha256;
    image::PixelRect crop;
    std::string pattern;
    std::int32_t margin = 0;
    std::vector<double> black;
    double white = 0.0;
    image::ImageBuffer<float> mosaic;  // crop.width x crop.height x 1
    image::ImageBuffer<float> rgb;     // crop.width x crop.height x 3
};

// Decodes one float32 block and checks its recorded shape and SHA-256.
image::ImageBuffer<float> read_block(const nlohmann::json& block, std::int32_t width, std::int32_t height,
                                     std::int32_t channels) {
    EXPECT_EQ(block.at("dtype"), "float32-le");
    std::vector<std::int64_t> shape = block.at("shape").get<std::vector<std::int64_t>>();
    std::vector<std::int64_t> expected_shape = {height, width};
    if (channels > 1) {
        expected_shape.push_back(channels);
    }
    EXPECT_EQ(shape, expected_shape);
    const auto bytes = ts::base64_decode(block.at("base64").get<std::string>());
    EXPECT_TRUE(bytes);
    auto buffer = image::ImageBuffer<float>::create(width, height, channels);
    EXPECT_TRUE(buffer);
    if (!bytes || !buffer) {
        return {};
    }
    const auto digest = core::sha256(*bytes);
    EXPECT_TRUE(digest);
    EXPECT_EQ(core::to_hex(*digest), block.at("sha256").get<std::string>());
    const std::size_t row_bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(channels) * sizeof(float);
    EXPECT_EQ(bytes->size(), row_bytes * static_cast<std::size_t>(height));
    if (bytes->size() != row_bytes * static_cast<std::size_t>(height)) {
        return {};
    }
    for (std::int32_t y = 0; y < height; ++y) {
        std::memcpy(buffer->row(y), bytes->data() + row_bytes * static_cast<std::size_t>(y), row_bytes);
    }
    return std::move(*buffer);
}

Reference load(const std::filesystem::path& path) {
    std::ifstream in(path);
    const nlohmann::json doc = nlohmann::json::parse(in);
    Reference r;
    EXPECT_EQ(doc.at("schema"), "lucent.demosaic-reference");
    EXPECT_EQ(doc.at("schema_version"), 1);
    EXPECT_EQ(doc.at("algorithm").at("function"), "colour_demosaicing.demosaicing_CFA_Bayer_Malvar2004");
    r.fixture_id = doc.at("fixture").at("id").get<std::string>();
    r.fixture_sha256 = doc.at("fixture").at("sha256").get<std::string>();
    const auto& crop = doc.at("crop");
    EXPECT_EQ(crop.at("frame"), "visible_area");
    r.crop = image::PixelRect{crop.at("x").get<std::int32_t>(), crop.at("y").get<std::int32_t>(),
                              crop.at("width").get<std::int32_t>(), crop.at("height").get<std::int32_t>()};
    r.pattern = doc.at("cfa_pattern").get<std::string>();
    r.margin = doc.at("interior_margin").get<std::int32_t>();
    r.black = doc.at("normalization").at("black_per_channel").get<std::vector<double>>();
    r.white = doc.at("normalization").at("white").get<double>();
    r.mosaic = read_block(doc.at("mosaic"), r.crop.width, r.crop.height, 1);
    r.rgb = read_block(doc.at("rgb"), r.crop.width, r.crop.height, 3);
    return r;
}

std::optional<image::BayerLayout> layout_of(const std::string& pattern) {
    for (const image::BayerLayout layout : {image::BayerLayout::Rggb, image::BayerLayout::Bggr,
                                            image::BayerLayout::Grbg, image::BayerLayout::Gbrg}) {
        if (image::to_string(layout) == pattern) {
            return layout;
        }
    }
    return std::nullopt;
}

// Largest |lucent - reference| over the reference's interior; `lucent` is offset by (ox, oy).
double interior_difference(const image::ImageBuffer<float>& lucent, std::int32_t ox, std::int32_t oy,
                           const Reference& r) {
    double worst = 0.0;
    for (std::int32_t y = r.margin; y < r.crop.height - r.margin; ++y) {
        for (std::int32_t x = r.margin; x < r.crop.width - r.margin; ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                worst = std::max(worst,
                                 std::abs(static_cast<double>(lucent.at(ox + x, oy + y, c)) - r.rgb.at(x, y, c)));
            }
        }
    }
    return worst;
}

TEST(MalvarReference, CommittedCropsMatchInTheInterior) {
    const auto files = reference_files();
    ASSERT_GE(files.size(), 3u) << "expected at least three references in " << reference_dir().string();
    std::set<std::string> fixtures;
    std::set<std::string> phases;
    for (const auto& path : files) {
        SCOPED_TRACE(path.filename().string());
        const Reference r = load(path);
        ASSERT_FALSE(r.mosaic.empty());
        ASSERT_FALSE(r.rgb.empty());
        ASSERT_EQ(r.margin, 2);
        const auto layout = layout_of(r.pattern);
        ASSERT_TRUE(layout) << r.pattern;
        fixtures.insert(r.fixture_id);
        phases.insert(r.pattern);

        auto rgb = image::ImageBuffer<float>::create(r.crop.width, r.crop.height, 3);
        ASSERT_TRUE(rgb);
        ASSERT_TRUE(demosaic_bayer_mhc(r.mosaic.const_view(), image::CfaPattern::bayer(*layout), rgb->view()));
        const double worst = interior_difference(*rgb, 0, 0, r);
        std::cout << "[ reference ] " << r.fixture_id << " " << r.pattern << ": max |difference| " << worst << "\n";
        RecordProperty(r.fixture_id + "_max_abs_difference", std::to_string(worst));
        EXPECT_LE(worst, kTolerance);
    }
    EXPECT_GE(fixtures.size(), 3u);
    EXPECT_EQ(phases.size(), 4u) << "the references must cover all four Bayer phases";
}

class MalvarFixtureReference : public ::testing::TestWithParam<std::filesystem::path> {};

TEST_P(MalvarFixtureReference, LucentNormalisationAndRouteReproduceTheReference) {
    const Reference r = load(GetParam());
    LUCENT_ENGINE_REQUIRE_FIXTURE(file, r.fixture_id);
    auto file_hash = core::hash_file(file, {});
    ASSERT_TRUE(file_hash);
    ASSERT_EQ(core::to_hex(file_hash->digest), r.fixture_sha256) << "the cached fixture differs from the reference's";

    auto decoder = raw::create_libraw_decoder();
    ASSERT_TRUE(decoder);
    auto decoded = (*decoder)->decode(file, {}, {});
    ASSERT_TRUE(decoded) << decoded.error().describe();
    auto sensor = normalize_sensor(*decoded);
    ASSERT_TRUE(sensor) << sensor.error().describe();

    // The recorded normalisation inputs are Lucent's (black per channel index, white).
    ASSERT_EQ(r.black.size(), 4u);
    for (std::size_t c = 0; c < 4; ++c) {
        EXPECT_EQ(sensor->normalization.black_max[c], r.black[c]) << "channel " << c;
    }
    EXPECT_EQ(sensor->normalization.white, r.white);
    const auto layout = layout_of(r.pattern);
    ASSERT_TRUE(layout);
    EXPECT_EQ(sensor->cfa.shifted(r.crop.x, r.crop.y), image::CfaPattern::bayer(*layout));

    // Lucent's normalised crop equals the reference input bit for bit: both compute
    // (raw - black) / (white - black) in double with one rounding to float.
    std::size_t mismatches = 0;
    for (std::int32_t y = 0; y < r.crop.height; ++y) {
        for (std::int32_t x = 0; x < r.crop.width; ++x) {
            mismatches += sensor->pixels.at(r.crop.x + x, r.crop.y + y, 0) != r.mosaic.at(x, y, 0) ? 1 : 0;
        }
    }
    EXPECT_EQ(mismatches, 0u);

    // The route on the crop plus a 2-pixel halo (so every compared pixel sees real neighbours,
    // in the CFA phase the full image has there) matches the reference interior.
    const image::PixelRect halo = image::PixelRect{r.crop.x - 2, r.crop.y - 2, r.crop.width + 4, r.crop.height + 4}
                                      .intersect(image::PixelRect::from_size(sensor->pixels.size()));
    auto view = sensor->pixels.view(halo);
    ASSERT_TRUE(view);
    auto rgb = image::ImageBuffer<float>::create(halo.width, halo.height, 3);
    ASSERT_TRUE(rgb);
    ASSERT_TRUE(demosaic_bayer_mhc(*view, sensor->cfa.shifted(halo.x, halo.y), rgb->view()));
    const double worst = interior_difference(*rgb, r.crop.x - halo.x, r.crop.y - halo.y, r);
    std::cout << "[ fixture   ] " << r.fixture_id << ": max |difference| " << worst << "\n";
    EXPECT_LE(worst, kTolerance);
}

std::string parameter_name(const ::testing::TestParamInfo<std::filesystem::path>& info) {
    std::string name = info.param.stem().string();
    std::replace_if(name.begin(), name.end(), [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); }, '_');
    return name;
}

INSTANTIATE_TEST_SUITE_P(Fixtures, MalvarFixtureReference, ::testing::ValuesIn(reference_files()), parameter_name);

}  // namespace
}  // namespace lucent::engine
