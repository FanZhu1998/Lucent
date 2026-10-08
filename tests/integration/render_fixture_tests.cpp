// End-to-end render tests on camera fixtures (labels "fixtures", "integration"; ADR 0006).
//
// For every fixture of fixtures/manifest.json that is cached and has committed reference
// statistics (tests/integration/data/render_reference.json, written by
// research/tools/render_reference.py --write-stats from rawpy renderings):
//   * the file is rendered through engine::render_file with the default settings but without DNG
//     opcodes and without the camera's image area, which the reference decoder does not apply
//     either (a file that the reference decoder refuses is recorded as "refused" and must fail with
//     the recorded error code);
//   * the result must be upright with the reference's dimensions and orientation (rawpy applies
//     the file's orientation itself, so neither value comes from Lucent), finite,
//     display-referred with max(R, G, B) <= 1, and every stage edge must match;
//   * SCENE-LINEAR GATES: the scene-linear image, converted to linear sRGB and clipped to [0, 1]
//     like the reference, is averaged over the reference's grid; CIEDE2000 between Lucent's and
//     rawpy's region means and the ratio of their mean luminances must pass
//       - the outer gross-error gates (every fixture): mean <= 2.0, max <= 6.0, ratio 0.93..1.07;
//       - the per-fixture regression gates, derived from the committed measurement of this
//         fixture ("measured"): mean <= 1.25 x measured + 0.02, max <= 1.25 x measured + 0.05,
//         |ratio - measured ratio| <= 0.003.
//     Smoke fixtures also prove that the gates are sensitive: simulated errors applied to
//     Lucent's own image (+-0.1 EV, white balance R x1.06 / B x0.94, a 5 % green gain, red/blue
//     swap) must each fail them;
//   * DISPLAY RENDERING on the real image (Neutral v0, display_rendering.hpp): every display pixel
//     equals the scene-linear pixel times the gain, mapped by the documented curve (bit-exact:
//     the identity part is exactly scene x gain), and fully clipped neutrals (scene-linear pixels
//     at the clip level in every channel) are display white;
//   * smoke-tier fixtures are also exported as JPEG (sRGB, q95) and 16-bit TIFF (Display P3); the
//     receipts are checked against the published files, the files are decoded by independent
//     libjpeg / libtiff readers and compared with the quantised image (TIFF exactly, JPEG within
//     quantisation), and a second export to the same name must be refused.
// A copy of dji-fc4382-dng whose IFD0 Orientation is patched to 6 and to 8 must render as the
// original rotated 90 degrees clockwise / counter-clockwise (EXIF definition), end to end.
// Qualification-tier fixtures are rendered only when LUCENT_TEST_FIXTURE_TIER=all (they take
// about a minute together); smoke fixtures always run when cached. Every test in this executable
// holds the ctest RESOURCE_LOCK lucent_full_resolution_render (tests/CMakeLists.txt), so at most
// one full-resolution render of this suite runs at a time on 12 GB machines.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include <lucent/core/hash.hpp>
#include <lucent/engine/render.hpp>
#include <lucent/export/export.hpp>
#include <lucent/export/output_profile.hpp>
#include <lucent/image/color_math.hpp>
#include <lucent/raw/decoder_registry.hpp>

#include "color_difference.hpp"
#include "fixture_manifest.hpp"
#include "image_readers.hpp"

namespace lucent::integration {
namespace {

using Json = nlohmann::json;

// ---- Gates -----------------------------------------------------------------------------------
// Outer gross-error gates, for every fixture. Measured 2026-09-26 with
// research/tools/render_reference.py over the 32 fixtures both decoders render (see ADR 0006 for
// the table): correct renders reach a region CIEDE2000 mean of at most about 1.3 (DNG
// ForwardMatrix files, which LibRaw renders with its D65 ColorMatrix instead), single regions
// about 3.2, luminance ratios 0.98 to 1.005.
constexpr double kGateRegionDeltaEMean = 2.0;
constexpr double kGateRegionDeltaEMax = 6.0;
constexpr double kGateLuminanceRatioLow = 0.93;
constexpr double kGateLuminanceRatioHigh = 1.07;
// Per-fixture regression gates around the committed measurement of the same fixture: Lucent is
// deterministic, so the margins only absorb floating-point differences between builds; any
// intended numerical change regenerates the statistics (ADR 0006, "Consequences").
constexpr double kRegressionFactor = 1.25;
constexpr double kRegressionMeanFloor = 0.02;
constexpr double kRegressionMaxFloor = 0.05;
constexpr double kRegressionRatioTolerance = 0.003;

// JPEG q95 4:4:4 round trip of real photographs (islow DCT): mean code difference well below 1,
// single samples up to a few tens at sharp edges and noise. Bounds with margin; a wrong row,
// swapped channel or mis-quantised image is far outside them.
constexpr double kJpegMeanCodeDifference = 1.5;
constexpr int kJpegMaxCodeDifference = 80;

std::filesystem::path stats_path() {
    return std::filesystem::path(LUCENT_SOURCE_DIR) / "tests" / "integration" / "data" /
           "render_reference.json";
}

const Json& reference_stats() {
    static const Json stats = [] {
        Json j = read_json(stats_path());
        if (j.value("schema", std::string()) != "lucent.render-reference" ||
            j.value("schema_version", 0) != 2) {
            throw std::runtime_error("render_reference.json is not a lucent.render-reference v2 document");
        }
        return j;
    }();
    return stats;
}

bool qualification_enabled() {
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, "LUCENT_TEST_FIXTURE_TIER") != 0 || value == nullptr) {
        return false;
    }
    const bool all = std::string(value) == "all";
    std::free(value);
    return all;
}

const Fixture* find_fixture(const std::vector<Fixture>& fixtures, const std::string& id) {
    const auto it = std::find_if(fixtures.begin(), fixtures.end(), [&](const Fixture& f) { return f.id == id; });
    return it == fixtures.end() ? nullptr : &*it;
}

// Region means of the scene-linear image in linear sRGB, after an optional 3x3 `perturbation`
// in linear sRGB (simulated errors), each pixel clipped to [0, 1], with the grid boundaries of
// render_reference.py (row i covers [floor(i H / G), floor((i + 1) H / G))). The Rec.2020 ->
// sRGB matrix is evaluated in float as image::apply_matrix does for the dump.
std::vector<Rgb> region_means(const engine::ColorImage& scene, std::int32_t grid,
                              const image::Matrix3d& perturbation = image::Matrix3d::identity()) {
    const image::Matrix3d m =
        perturbation * *image::conversion_matrix(scene.encoding, image::ColorEncoding::linear_srgb());
    std::array<float, 9> f{};
    for (std::size_t i = 0; i < 9; ++i) {
        f[i] = static_cast<float>(m(i / 3, i % 3));
    }
    const std::int32_t w = scene.pixels.width();
    const std::int32_t h = scene.pixels.height();
    const auto band = [grid](std::int32_t size) {
        std::vector<std::int32_t> index(static_cast<std::size_t>(size));
        for (std::int32_t cell = 0; cell < grid; ++cell) {
            const auto first = static_cast<std::int32_t>(static_cast<std::int64_t>(cell) * size / grid);
            const auto last = static_cast<std::int32_t>(static_cast<std::int64_t>(cell + 1) * size / grid);
            for (std::int32_t k = first; k < last; ++k) {
                index[static_cast<std::size_t>(k)] = cell;
            }
        }
        return index;
    };
    const std::vector<std::int32_t> row_cell = band(h);
    const std::vector<std::int32_t> col_cell = band(w);
    std::vector<Rgb> sums(static_cast<std::size_t>(grid * grid), Rgb{0, 0, 0});
    std::vector<std::uint64_t> counts(sums.size(), 0);
    const auto clip = [](float v) { return std::clamp(v, 0.0f, 1.0f); };
    for (std::int32_t y = 0; y < h; ++y) {
        const float* p = scene.pixels.row(y);
        const std::int32_t rc = row_cell[static_cast<std::size_t>(y)] * grid;
        for (std::int32_t x = 0; x < w; ++x, p += 3) {
            const auto cell = static_cast<std::size_t>(rc + col_cell[static_cast<std::size_t>(x)]);
            sums[cell][0] += clip(f[0] * p[0] + f[1] * p[1] + f[2] * p[2]);
            sums[cell][1] += clip(f[3] * p[0] + f[4] * p[1] + f[5] * p[2]);
            sums[cell][2] += clip(f[6] * p[0] + f[7] * p[1] + f[8] * p[2]);
            ++counts[cell];
        }
    }
    for (std::size_t i = 0; i < sums.size(); ++i) {
        for (double& v : sums[i]) {
            v /= static_cast<double>(counts[i]);
        }
    }
    return sums;
}

struct GateStats {
    double mean_de = 0.0;
    double max_de = 0.0;
    double ratio = 0.0;
};

std::ostream& operator<<(std::ostream& out, const GateStats& s) {
    return out << "region CIEDE2000 mean " << s.mean_de << ", max " << s.max_de << ", luminance ratio " << s.ratio;
}

GateStats compare_regions(const std::vector<Rgb>& ours, const Json& theirs) {
    GateStats s;
    double sum_de = 0.0;
    double y_ours = 0.0;
    double y_theirs = 0.0;
    for (std::size_t i = 0; i < ours.size(); ++i) {
        const Rgb reference{theirs[i][0].get<double>(), theirs[i][1].get<double>(), theirs[i][2].get<double>()};
        const double de = ciede2000(lab_from_linear_srgb(ours[i]), lab_from_linear_srgb(reference));
        sum_de += de;
        s.max_de = std::max(s.max_de, de);
        y_ours += luminance_from_linear_srgb(ours[i]);
        y_theirs += luminance_from_linear_srgb(reference);
    }
    s.mean_de = sum_de / static_cast<double>(ours.size());
    s.ratio = y_ours / y_theirs;
    return s;
}

// Empty when `s` passes both the outer and the per-fixture regression gates; otherwise the
// first violated gate.
std::string gate_violation(const GateStats& s, const Json& measured) {
    std::ostringstream why;
    const double mean_limit =
        std::min(kGateRegionDeltaEMean,
                 kRegressionFactor * measured.at("region_delta_e_mean").get<double>() + kRegressionMeanFloor);
    const double max_limit =
        std::min(kGateRegionDeltaEMax,
                 kRegressionFactor * measured.at("region_delta_e_max").get<double>() + kRegressionMaxFloor);
    const double ratio = measured.at("region_luminance_ratio").get<double>();
    if (s.mean_de > mean_limit) {
        why << "region CIEDE2000 mean " << s.mean_de << " > " << mean_limit;
    } else if (s.max_de > max_limit) {
        why << "region CIEDE2000 max " << s.max_de << " > " << max_limit;
    } else if (s.ratio < kGateLuminanceRatioLow || s.ratio > kGateLuminanceRatioHigh ||
               std::abs(s.ratio - ratio) > kRegressionRatioTolerance) {
        why << "luminance ratio " << s.ratio << " outside " << ratio << " +- " << kRegressionRatioTolerance
            << " (outer " << kGateLuminanceRatioLow << ".." << kGateLuminanceRatioHigh << ")";
    }
    return why.str();
}

// Neutral v0 (display_rendering.hpp) applied to the scene-linear image must reproduce the
// display image bit for bit, and clipped neutrals must be display white.
struct DisplayCheck {
    std::uint64_t mismatched_pixels = 0;
    std::uint64_t clipped_neutrals = 0;         // scene pixels at the clip level in every channel
    std::uint64_t clipped_neutrals_not_white = 0;
    float max_display = 0.0f;
    float min_clipped_display = 1.0f;
};

DisplayCheck check_display(const engine::ColorImage& scene, const engine::ColorImage& display,
                           const engine::NeutralV0Record& r) {
    DisplayCheck c;
    const auto gain = static_cast<float>(r.gain);
    const double knee = r.parameters.knee;
    const double white = r.white;
    const double limit = r.curve == engine::NeutralV0Curve::Shoulder ? knee
                         : r.curve == engine::NeutralV0Curve::Lift ? engine::kNeutralV0LiftBreak * white
                                                                   : white;
    const double scene_white = r.parameters.scene_white;
    for (std::int32_t y = 0; y < scene.pixels.height(); ++y) {
        const float* s = scene.pixels.row(y);
        const float* d = display.pixels.row(y);
        for (std::int32_t x = 0; x < scene.pixels.width(); ++x, s += 3, d += 3) {
            std::array<float, 3> e{s[0] * gain, s[1] * gain, s[2] * gain};
            const double m = std::max({e[0], e[1], e[2]});
            if (m > limit || m >= white) {
                const auto scale = static_cast<float>(engine::neutral_v0_curve(m, knee, white) / m);
                for (float& v : e) {
                    v *= scale;
                }
            }
            if (e[0] != d[0] || e[1] != d[1] || e[2] != d[2]) {
                ++c.mismatched_pixels;
            }
            const float display_max = std::max({d[0], d[1], d[2]});
            c.max_display = std::max(c.max_display, display_max);
            const auto near_clip = [&](float v) { return std::abs(v - scene_white) <= 1e-4 * scene_white; };
            if (near_clip(s[0]) && near_clip(s[1]) && near_clip(s[2])) {
                ++c.clipped_neutrals;
                const float display_min = std::min({d[0], d[1], d[2]});
                c.min_clipped_display = std::min(c.min_clipped_display, display_min);
                if (display_min < 1.0f - 1e-3f) {
                    ++c.clipped_neutrals_not_white;
                }
            }
        }
    }
    return c;
}

void expect_receipt_matches_file(const exporter::ExportReceipt& receipt, const std::filesystem::path& path,
                                 exporter::ExportFormat format, exporter::OutputProfile profile,
                                 const image::Size& size) {
    ASSERT_TRUE(std::filesystem::is_regular_file(path)) << path.string();
    EXPECT_EQ(receipt.path, path);
    EXPECT_EQ(receipt.format, format);
    EXPECT_EQ(receipt.profile, profile);
    EXPECT_EQ(receipt.bit_depth, exporter::bit_depth_for(format));
    EXPECT_EQ(receipt.width, size.width);
    EXPECT_EQ(receipt.height, size.height);
    EXPECT_EQ(receipt.byte_size, std::filesystem::file_size(path));
    auto hashed = core::hash_file(path, {});
    ASSERT_TRUE(hashed) << hashed.error().describe();
    EXPECT_EQ(hashed->digest, receipt.sha256);
    auto icc = exporter::icc_profile(profile);
    ASSERT_TRUE(icc);
    auto icc_hash = core::sha256(std::as_bytes(*icc));
    ASSERT_TRUE(icc_hash);
    EXPECT_EQ(*icc_hash, receipt.icc_sha256);
}

// Quantises the display image, publishes it and checks the published file with an independent
// decoder against the quantised samples.
void export_and_check(const engine::ColorImage& display, const std::filesystem::path& path,
                      const exporter::ExportSettings& settings) {
    auto encoded = exporter::encode_for_output(display.pixels.const_view(), display.encoding, settings.profile,
                                               exporter::bit_depth_for(settings.format), settings.transform);
    ASSERT_TRUE(encoded) << encoded.error().describe();
    auto receipt = exporter::write_verified(*encoded, path, settings);
    ASSERT_TRUE(receipt) << receipt.error().describe();
    expect_receipt_matches_file(*receipt, path, settings.format, settings.profile, display.pixels.size());

    DecodedHeader header;
    std::string error;
    const std::size_t row_samples = encoded->row_samples();
    if (settings.format == exporter::ExportFormat::Tiff) {
        std::int64_t mismatched_rows = 0;
        ASSERT_TRUE(read_tiff16_rows(path, header, error, [&](std::int32_t y, std::span<const std::uint16_t> row) {
            const std::uint16_t* expected = encoded->samples16.data() + row_samples * static_cast<std::size_t>(y);
            if (row.size() != row_samples || !std::equal(row.begin(), row.end(), expected)) {
                ++mismatched_rows;
            }
        })) << error;
        EXPECT_EQ(mismatched_rows, 0);
    } else {
        double sum = 0.0;
        int worst = 0;
        std::uint64_t samples = 0;
        ASSERT_TRUE(read_jpeg_rows(path, header, error, [&](std::int32_t y, std::span<const std::uint8_t> row) {
            const std::uint8_t* expected = encoded->samples8.data() + row_samples * static_cast<std::size_t>(y);
            for (std::size_t i = 0; i < row.size() && i < row_samples; ++i) {
                const int diff = std::abs(int{row[i]} - int{expected[i]});
                sum += diff;
                worst = std::max(worst, diff);
            }
            samples += row.size();
        })) << error;
        const double mean = sum / static_cast<double>(std::max<std::uint64_t>(samples, 1));
        std::cout << "[ export   ] " << path.filename().string() << ": JPEG vs quantised image mean |diff| "
                  << mean << " codes, max " << worst << "\n";
        EXPECT_EQ(samples, encoded->samples8.size());
        EXPECT_LT(mean, kJpegMeanCodeDifference);
        EXPECT_LE(worst, kJpegMaxCodeDifference);
    }
    EXPECT_EQ(header.width, display.pixels.width());
    EXPECT_EQ(header.height, display.pixels.height());
    EXPECT_EQ(header.channels, 3);
}

class RenderFixture : public ::testing::TestWithParam<Fixture> {};

TEST_P(RenderFixture, RendersWithinGatesAndDisplaysAsDefined) {
    const Fixture& fixture = GetParam();
    if (fixture.tier != "smoke" && !qualification_enabled()) {
        GTEST_SKIP() << "qualification-tier fixture; set LUCENT_TEST_FIXTURE_TIER=all to render it";
    }
    const std::filesystem::path file = cached_file(fixture);
    if (!std::filesystem::exists(file)) {
        GTEST_SKIP() << "fixture '" << fixture.id << "' is not downloaded; run tools\\fetch-fixtures.ps1";
    }
    const Json& fixtures = reference_stats().at("fixtures");
    if (!fixtures.contains(fixture.id)) {
        GTEST_SKIP() << "no reference statistics for '" << fixture.id
                     << "' (the reference decoder refused it, or render_reference.py has not been run)";
    }
    const Json& ref = fixtures.at(fixture.id);

    auto registry = raw::create_default_registry();
    ASSERT_TRUE(registry);
    engine::RenderOptions options;
    options.keep_scene_linear = true;
    // The reference decoder (LibRaw) applies no DNG opcodes, so the comparison renders without
    // them (ADR 0014); the drone DNGs' opcodes are checked against an independent reference by
    // the engine's DngOpcodeFixtures tests and tests/tools/dng_opcode_reference.py. Nor does it
    // cut its visible area to the camera's image area, so neither does this render (ADR 0017).
    engine::RenderSettings settings;
    settings.apply_dng_opcodes = false;
    settings.apply_camera_area = false;
    auto result = engine::render_file(*registry, file, 0u, settings, options, {});
    if (ref.contains("refused")) {
        // The reference decoder refuses this file too: Lucent must refuse it with the same code.
        ASSERT_FALSE(result) << fixture.id << " rendered although the reference records a refusal";
        EXPECT_EQ(std::string(to_string(result.error().code)),
                  ref.at("refused").at("lucent_error_code").get<std::string>())
            << result.error().describe();
        return;
    }
    ASSERT_TRUE(result) << result.error().describe();

    // ---- Structure ---------------------------------------------------------------------------
    const engine::RenderReport& report = result->report;
    // The eight stages of the W2 path, plus one record per opcode list the file has (every
    // opcode recorded as disabled, and the render then never reported as complete).
    std::size_t opcode_stages = 0;
    for (const engine::StageRecord& s : report.stages) {
        if (s.stage == engine::RenderStage::DngOpcodeList1 || s.stage == engine::RenderStage::DngOpcodeList2 ||
            s.stage == engine::RenderStage::DngOpcodeList3) {
            ++opcode_stages;
        }
    }
    ASSERT_EQ(report.stages.size(), 8u + opcode_stages);
    EXPECT_EQ(report.stages.front().stage, engine::RenderStage::Decode);
    EXPECT_EQ(report.stages.back().stage, engine::RenderStage::Orientation);
    if (!report.dng_opcodes.opcodes.empty()) {
        EXPECT_FALSE(report.dng_opcodes.any_applied());
        EXPECT_NE(report.interpretation.status, raw::InterpretationStatus::Complete);
    }
    EXPECT_EQ(result->display.encoding, engine::display_linear_rec2020());
    ASSERT_TRUE(result->scene_linear);
    EXPECT_EQ(result->scene_linear->encoding, image::ColorEncoding::scene_linear_rec2020_d65());

    // Upright dimensions and orientation as rawpy produced them from the file's own orientation.
    const image::Size visible = result->metadata.geometry.visible_area.size();
    const image::Size upright = image::oriented_size(visible, result->metadata.geometry.orientation);
    EXPECT_EQ(report.orientation, result->metadata.geometry.orientation);
    EXPECT_EQ(result->display.pixels.size(), upright);
    EXPECT_EQ(result->scene_linear->pixels.size(), upright);
    EXPECT_EQ(upright.width, ref.at("size").at(0).get<std::int32_t>());
    EXPECT_EQ(upright.height, ref.at("size").at(1).get<std::int32_t>());
    EXPECT_EQ(static_cast<int>(image::to_exif(report.orientation)),
              ref.at("reference_orientation_exif").get<int>());

    // Finite everywhere; display max(R, G, B) <= 1.
    std::uint64_t non_finite = 0;
    std::uint64_t above_one = 0;
    for (const engine::ColorImage* img : {&result->display, &*result->scene_linear}) {
        for (std::int32_t y = 0; y < img->pixels.height(); ++y) {
            const float* p = img->pixels.row(y);
            for (std::int32_t x = 0; x < img->pixels.width() * 3; ++x) {
                non_finite += std::isfinite(p[x]) ? 0 : 1;
                if (img == &result->display && p[x] > 1.0f) {
                    ++above_one;
                }
            }
        }
    }
    EXPECT_EQ(non_finite, 0u);
    EXPECT_EQ(above_one, 0u);

    // ---- Display rendering -----------------------------------------------------------------------
    const DisplayCheck display = check_display(*result->scene_linear, result->display, report.rendering);
    std::cout << "[ display  ] " << fixture.id << ": curve " << engine::to_string(report.rendering.curve)
              << ", gain " << report.rendering.gain << ", white point " << report.rendering.white << ", "
              << display.clipped_neutrals << " clipped-neutral pixels (lowest display value "
              << display.min_clipped_display << "), display max " << display.max_display << "\n";
    EXPECT_EQ(display.mismatched_pixels, 0u) << "display differs from Neutral v0 applied to the scene-linear image";
    EXPECT_EQ(display.clipped_neutrals_not_white, 0u) << "clipped highlights must render as display white";
    if (display.clipped_neutrals > 0) {
        EXPECT_GE(display.max_display, 1.0f - 1e-6f);
    }

    // ---- Scene-linear gates ------------------------------------------------------------------------
    const auto grid = reference_stats().at("grid").get<std::int32_t>();
    const Json& theirs = ref.at("reference_region_means");
    const Json& measured = ref.at("measured");
    const std::vector<Rgb> ours = region_means(*result->scene_linear, grid);
    ASSERT_EQ(theirs.size(), ours.size());
    const GateStats stats = compare_regions(ours, theirs);
    std::cout << "[ gates    ] " << fixture.id << ": " << stats << " (" << engine::to_string(report.color.path)
              << "; committed mean " << measured.at("region_delta_e_mean").get<double>() << ", ratio "
              << measured.at("region_luminance_ratio").get<double>() << ")\n";
    EXPECT_LE(stats.mean_de, kGateRegionDeltaEMean);
    EXPECT_LE(stats.max_de, kGateRegionDeltaEMax);
    EXPECT_GE(stats.ratio, kGateLuminanceRatioLow);
    EXPECT_LE(stats.ratio, kGateLuminanceRatioHigh);
    EXPECT_EQ(gate_violation(stats, measured), "") << "regression against the committed measurement";

    if (fixture.tier != "smoke") {
        return;
    }

    // ---- Gate sensitivity: simulated errors must fail --------------------------------------------
    struct Simulated {
        const char* name;
        image::Matrix3d matrix;
    };
    const double up = std::exp2(0.1);
    const std::array<Simulated, 5> simulated = {{
        {"+0.1 EV", image::Matrix3d::diagonal({up, up, up})},
        {"-0.1 EV", image::Matrix3d::diagonal({1.0 / up, 1.0 / up, 1.0 / up})},
        {"white balance R x1.06 B x0.94", image::Matrix3d::diagonal({1.06, 1.0, 0.94})},
        {"green x1.05", image::Matrix3d::diagonal({1.0, 1.05, 1.0})},
        {"red/blue swap", image::Matrix3d{{0.0, 0.0, 1.0}, {0.0, 1.0, 0.0}, {1.0, 0.0, 0.0}}},
    }};
    for (const Simulated& error : simulated) {
        const GateStats perturbed = compare_regions(region_means(*result->scene_linear, grid, error.matrix), theirs);
        const std::string violation = gate_violation(perturbed, measured);
        std::cout << "[ simulate ] " << fixture.id << " " << error.name << ": " << perturbed << " -> "
                  << (violation.empty() ? "PASSES (not detected)" : "fails: " + violation) << "\n";
        EXPECT_FALSE(violation.empty()) << "the gates do not detect a simulated error: " << error.name;
    }

    // ---- Export and receipts --------------------------------------------------------------------
    result->scene_linear.reset();
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("lucent-render-test-" + fixture.id + "-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    ASSERT_TRUE(std::filesystem::create_directories(dir));

    const std::filesystem::path jpeg = dir / "render.jpg";
    export_and_check(result->display, jpeg, exporter::ExportSettings::jpeg_srgb_q95());
    export_and_check(result->display, dir / "render.tif",
                     exporter::ExportSettings::tiff16(exporter::OutputProfile::DisplayP3));

    // No-clobber: a second export to the same name is refused and leaves the file untouched.
    auto before = core::hash_file(jpeg, {});
    ASSERT_TRUE(before);
    auto again = exporter::export_image(result->display.pixels.const_view(), result->display.encoding, jpeg,
                                        exporter::ExportSettings::jpeg_srgb_q95());
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, ErrorCode::AlreadyExists);
    auto rehash = core::hash_file(jpeg, {});
    ASSERT_TRUE(rehash);
    EXPECT_EQ(rehash->digest, before->digest);

    // Only the two published files remain (no temporary files).
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        (void)entry;
        ++files;
    }
    EXPECT_EQ(files, 2u);
    std::filesystem::remove_all(dir, ec);
}

INSTANTIATE_TEST_SUITE_P(Manifest, RenderFixture, ::testing::ValuesIn(load_manifest()),
                         [](const ::testing::TestParamInfo<Fixture>& info) { return test_name(info.param); });

// ---- Orientation, end to end ---------------------------------------------------------------------

// Byte offset of the value of IFD0's Orientation tag (SHORT, count 1) in a TIFF-based file, or 0.
std::size_t ifd0_orientation_offset(const std::vector<char>& data) {
    if (data.size() < 8) {
        return 0;
    }
    const bool little = data[0] == 'I' && data[1] == 'I';
    const auto u8 = [&](std::size_t i) { return static_cast<std::uint32_t>(static_cast<unsigned char>(data[i])); };
    const auto u16 = [&](std::size_t i) { return little ? u8(i) | (u8(i + 1) << 8) : (u8(i) << 8) | u8(i + 1); };
    const auto u32 = [&](std::size_t i) {
        return little ? u16(i) | (u16(i + 2) << 16) : (u16(i) << 16) | u16(i + 2);
    };
    const std::size_t ifd = u32(4);
    if (ifd + 2 > data.size()) {
        return 0;
    }
    const std::uint32_t count = u16(ifd);
    for (std::uint32_t k = 0; k < count && ifd + 2 + 12 * (k + 1) <= data.size(); ++k) {
        const std::size_t entry = ifd + 2 + 12 * k;
        if (u16(entry) == 0x0112 && u16(entry + 2) == 3 && u32(entry + 4) == 1) {
            return entry + 8;
        }
    }
    return 0;
}

TEST(RenderOrientation, RotatedDngRendersAsTheOriginalRotated) {
    // dji-fc4382-dng with its IFD0 Orientation patched: EXIF 6 means "rotate 90 degrees clockwise
    // to display", EXIF 8 "counter-clockwise" (TIFF/EP, EXIF 2.32 table 4). This checks the whole
    // chain (LibRaw flip -> Lucent orientation -> baked pixels) against the EXIF definition,
    // independently of image::map_to_oriented.
    const std::vector<Fixture> manifest = load_manifest();
    const Fixture* fixture = find_fixture(manifest, "dji-fc4382-dng");
    ASSERT_NE(fixture, nullptr);
    const std::filesystem::path source = cached_file(*fixture);
    if (!std::filesystem::exists(source)) {
        GTEST_SKIP() << "fixture 'dji-fc4382-dng' is not downloaded; run tools\\fetch-fixtures.ps1";
    }
    std::vector<char> bytes;
    {
        std::ifstream in(source, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::size_t offset = ifd0_orientation_offset(bytes);
    ASSERT_NE(offset, 0u) << "the fixture has no IFD0 Orientation tag to patch";
    const bool little = bytes[0] == 'I';

    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("lucent-orientation-test-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    ASSERT_TRUE(std::filesystem::create_directories(dir));

    auto registry = raw::create_default_registry();
    ASSERT_TRUE(registry);
    const auto render = [&](const std::filesystem::path& path) {
        return engine::render_file(*registry, path, 0u, engine::RenderSettings{}, engine::RenderOptions{}, {});
    };
    auto original = render(source);
    ASSERT_TRUE(original) << original.error().describe();
    ASSERT_EQ(original->report.orientation, image::Orientation::Normal);
    const image::ImageBuffer<float>& in = original->display.pixels;
    const std::int32_t w = in.width();
    const std::int32_t h = in.height();

    for (const int exif : {6, 8}) {
        std::vector<char> patched = bytes;
        patched[offset + (little ? 0 : 1)] = static_cast<char>(exif);
        patched[offset + (little ? 1 : 0)] = 0;
        const std::filesystem::path path = dir / ("orientation" + std::to_string(exif) + ".dng");
        {
            std::ofstream out(path, std::ios::binary);
            out.write(patched.data(), static_cast<std::streamsize>(patched.size()));
        }
        auto rotated = render(path);
        ASSERT_TRUE(rotated) << rotated.error().describe();
        EXPECT_EQ(image::to_exif(rotated->report.orientation), exif);
        const image::ImageBuffer<float>& out = rotated->display.pixels;
        ASSERT_EQ(out.width(), h);
        ASSERT_EQ(out.height(), w);
        std::uint64_t mismatched = 0;
        for (std::int32_t y = 0; y < out.height(); ++y) {
            for (std::int32_t x = 0; x < out.width(); ++x) {
                // Clockwise: the stored top row becomes the right column. Counter-clockwise: the
                // stored top row becomes the left column.
                const std::int32_t sx = exif == 6 ? y : w - 1 - y;
                const std::int32_t sy = exif == 6 ? h - 1 - x : x;
                for (std::int32_t c = 0; c < 3; ++c) {
                    mismatched += out.at(x, y, c) == in.at(sx, sy, c) ? 0 : 1;
                }
            }
        }
        EXPECT_EQ(mismatched, 0u) << "EXIF " << exif;
    }
    std::filesystem::remove_all(dir, ec);
}

TEST(RenderReference, StatisticsCoverOnlyManifestFixtures) {
    std::vector<std::string> ids;
    for (const Fixture& fixture : load_manifest()) {
        ids.push_back(fixture.id);
    }
    const Json& stats = reference_stats();
    const auto grid = stats.at("grid").get<std::int32_t>();
    ASSERT_GT(grid, 0);
    for (const auto& [id, entry] : stats.at("fixtures").items()) {
        EXPECT_NE(std::find(ids.begin(), ids.end(), id), ids.end())
            << "statistics without a manifest entry: " << id;
        if (entry.contains("refused")) {
            EXPECT_FALSE(entry.at("refused").at("lucent_error_code").get<std::string>().empty()) << id;
            continue;
        }
        EXPECT_EQ(entry.at("reference_region_means").size(), static_cast<std::size_t>(grid * grid)) << id;
        for (const Json& cell : entry.at("reference_region_means")) {
            for (const Json& v : cell) {
                EXPECT_GE(v.get<double>(), 0.0) << id;
                EXPECT_LE(v.get<double>(), 1.0) << id;
            }
        }
        const int orientation = entry.at("reference_orientation_exif").get<int>();
        EXPECT_GE(orientation, 1) << id;
        EXPECT_LE(orientation, 8) << id;
        const Json& measured = entry.at("measured");
        for (const char* key : {"region_delta_e_mean", "region_delta_e_max", "region_luminance_ratio"}) {
            EXPECT_TRUE(measured.contains(key)) << id << " " << key;
        }
    }
    // Every smoke fixture has statistics.
    for (const Fixture& fixture : load_manifest()) {
        if (fixture.tier == "smoke") {
            EXPECT_TRUE(stats.at("fixtures").contains(fixture.id)) << fixture.id;
        }
    }
}

}  // namespace
}  // namespace lucent::integration
