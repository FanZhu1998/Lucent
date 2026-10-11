// GTest wrapper of the camera qualification harness (labels "qualification", "fixtures"; ADR 0016).
//
// For every fixture of fixtures/manifest.json that is cached, the full check suite of
// lucent::qualify::qualify_fixture() runs in this process and its outcome must equal the one
// fixtures/qualification/expectations.json records: the proposed state, each check's status, the
// limitations, the mode key, the source of the camera's image area and the develop render's output
// size (ADR 0017). A change in how Lucent handles a camera (a decoder upgrade, a new
// visible area, a colour regression, an opcode no longer applied, an export defect) therefore fails
// here and has to be reviewed: either the change is wrong, or the expectation is updated together
// with the support table (docs/qualification/README.md).
//
// Smoke fixtures and every fixture whose expected state is Verified always run when cached (QUA-07:
// a regression in a mode Lucent shows as Verified must fail the default test run); the other
// qualification-tier fixtures run with LUCENT_TEST_FIXTURE_TIER=all (about 20 s each). Fixtures
// that are not downloaded skip with a message. Besides the outcome, the camera area's placement
// (where the develop render put it, not only its size) must equal the expectation's
// "image_area_applied".
//
// W12 (ADR 0020): ColourReferenceFixture measures the develop path on every fixture that shows a
// colour chart (fixtures/qualification/charts.json): within the gate against the chart's published
// values, alike to LibRaw's own rendering measured without Lucent's code, and failing for gross
// errors made from the same measured patches. The rotated fixtures (orientation 6 and 8, one per
// image-area source) run through OutcomeMatchesItsExpectation like every fixture, with their
// placement asserted.
//
// W10 phase 3 (ADR 0018): the application assesses a library item by the mode key of the probe it
// makes at import, not of a decode. ProbeKeyFindsTheQualifiedState probes every cached fixture as
// the importer does (any tier: a probe takes milliseconds) and asserts that the bundled camera
// support database finds, under the probe's key, the entry of the fixture's qualified (decoded) key
// with the expected state. The two keys differ where a probe sees a provisional value (the Canon
// S-RAW white level: 15 bits at the probe, 14 after unpacking); the database lists such a mode under
// the probe's key (support_export.hpp).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include <lucent/assets/asset_store.hpp>
#include <lucent/assets/camera_support.hpp>
#include <lucent/assets/lens_database.hpp>
#include <lucent/core/text.hpp>
#include <lucent/image/color_math.hpp>
#include <lucent/raw/decoder_registry.hpp>

#include "charts.hpp"
#include "colour_reference.hpp"
#include "expectations.hpp"
#include "mode_key.hpp"
#include "qualification.hpp"

namespace lucent::qualify {

// Test names and failure messages show the fixture id rather than the parameter's bytes.
void PrintTo(const ManifestFixture& fixture, std::ostream* out) {
    *out << fixture.id;
}

namespace {

bool qualification_tier_enabled() {
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, "LUCENT_TEST_FIXTURE_TIER") != 0 || value == nullptr) {
        return false;
    }
    const bool all = std::string(value) == "all";
    std::free(value);
    return all;
}

CorpusPaths paths() {
    return CorpusPaths{std::filesystem::path(LUCENT_SOURCE_DIR)};
}

const QualifyCorpus& corpus() {
    static const QualifyCorpus loaded = [] {
        auto c = QualifyCorpus::load(paths(), /*require_expectations=*/true);
        if (!c) {
            throw std::runtime_error("qualification corpus: " + c.error().describe());
        }
        return std::move(*c);
    }();
    return loaded;
}

std::vector<ManifestFixture> manifest_fixtures() {
    auto manifest = load_manifest(paths().manifest());
    if (!manifest) {
        throw std::runtime_error("manifest: " + manifest.error().describe());
    }
    return *manifest;
}

class QualificationFixture : public ::testing::TestWithParam<ManifestFixture> {};

// The bundled lens database next to this test, as lucent-qualify loads it (ADR 0016's W13 closure
// amendment: the qualification renders lenses as Lucent's exports do).
std::shared_ptr<const engine::LensDatabase> qualification_lens_database() {
    static const std::shared_ptr<const engine::LensDatabase> database =
        assets::load_lens_database(assets::default_asset_locations(nullptr)).database;
    return database;
}

TEST_P(QualificationFixture, OutcomeMatchesItsExpectation) {
    const ManifestFixture& fixture = GetParam();
    const QualifyCorpus& c = corpus();
    ASSERT_TRUE(c.expectations.has_value());
    const FixtureExpectation* expectation = c.expectations->find(fixture.id);
    ASSERT_NE(expectation, nullptr) << "no expectation for " << fixture.id;
    if (fixture.tier != "smoke" && expectation->expected_state != "verified" && !qualification_tier_enabled()) {
        GTEST_SKIP() << "qualification-tier fixture that is not Verified; set LUCENT_TEST_FIXTURE_TIER=all to "
                        "qualify it";
    }
    if (!std::filesystem::exists(c.paths.cached_file(fixture))) {
        GTEST_SKIP() << "fixture '" << fixture.id
                     << "' is not downloaded; run tools\\fetch-fixtures.ps1 -Tier " << fixture.tier;
    }

    QualifyOptions options;
    options.work_dir = std::filesystem::temp_directory_path() /
                       ("lucent-qualification-test-" + std::to_string(GetCurrentProcessId()));
    options.lens_database = qualification_lens_database();
    ASSERT_TRUE(options.lens_database) << "the bundled lens database (assets/lens-database next to this test) is missing";
    const FixtureResult result = qualify_fixture(c, fixture, options);
    std::error_code ec;
    std::filesystem::remove_all(options.work_dir, ec);

    std::cout << "[ qualify  ] " << fixture.id << ": " << to_string(result.state) << " in "
              << result.measurements.total_seconds << " s, peak working set "
              << result.measurements.peak_working_set_bytes / (1024 * 1024) << " MB\n";
    for (const CheckResult& check : result.checks) {
        std::cout << "[ check    ] " << check.id << " " << to_string(check.status) << " ("
                  << to_string(check.evidence) << "): " << check.summary << "\n";
    }

    ASSERT_TRUE(result.harness_error.empty()) << result.harness_error;
    EXPECT_EQ(std::string(to_string(result.state)), expectation->expected_state);
    for (const CheckResult& check : result.checks) {
        const auto it = expectation->expected_checks.find(check.id);
        ASSERT_NE(it, expectation->expected_checks.end()) << check.id;
        EXPECT_EQ(std::string(to_string(check.status)), it->second) << check.id << ": " << check.summary;
    }
    std::vector<std::string> limitations;
    for (const Limitation& l : result.limitations) {
        limitations.push_back(l.id);
    }
    std::sort(limitations.begin(), limitations.end());
    EXPECT_EQ(limitations, expectation->expected_limitations);
    ASSERT_TRUE(result.mode_key.has_value());
    EXPECT_EQ(result.mode_key->to_string(), expectation->mode_key);
    // The camera's image area (ADR 0017): where it comes from, and the develop render's output size
    // (the identity geometry of version 2 renders the area).
    if (expectation->image_area_source) {
        ASSERT_TRUE(result.camera_area.is_object());
        EXPECT_EQ(result.camera_area.value("source", std::string()), *expectation->image_area_source);
    }
    if (expectation->rendered && result.camera_area.is_object() &&
        result.camera_area.at("output").is_array()) {
        EXPECT_EQ(result.camera_area.at("output").at(0).get<std::int32_t>(), expectation->rendered->width);
        EXPECT_EQ(result.camera_area.at("output").at(1).get<std::int32_t>(), expectation->rendered->height);
        // QUA-07: and where it was placed.
        if (expectation->image_area_applied_asserted) {
            const OrderedJson& applied = result.camera_area.at("applied");
            ASSERT_EQ(applied.is_object(), expectation->image_area_applied.has_value()) << applied.dump();
            if (expectation->image_area_applied) {
                const std::array<std::int32_t, 4>& e = *expectation->image_area_applied;
                EXPECT_EQ(applied.value("x", -1), e[0]);
                EXPECT_EQ(applied.value("y", -1), e[1]);
                EXPECT_EQ(applied.value("width", -1), e[2]);
                EXPECT_EQ(applied.value("height", -1), e[3]);
            }
        }
    }
    EXPECT_TRUE(result.expectation_mismatches.empty());
    for (const std::string& m : result.expectation_mismatches) {
        ADD_FAILURE() << m;
    }
    // The develop render planned the lens with the bundled database (W13 closure).
    if (result.lens_correction.is_object()) {
        EXPECT_TRUE(result.lens_correction.at("database").is_string()) << result.lens_correction.dump();
    }
}

// The bundled database, as Lucent.exe loads it (next to this executable, verified against the pin).
// LUCENT_CAMERA_SUPPORT_DIR names a regenerated asset folder instead (its camera-support.json, parsed
// as the application parses it; no pin), to verify a change that adds fixtures against the database it
// regenerates before that is committed from a clean build (docs/qualification/README.md).
std::shared_ptr<const assets::CameraSupport> bundled_database() {
    static const std::shared_ptr<const assets::CameraSupport> loaded = [] {
        char* value = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&value, &length, "LUCENT_CAMERA_SUPPORT_DIR") == 0 && value != nullptr) {
            const std::filesystem::path folder(core::utf8_to_wide_lossy(value));
            std::free(value);
            std::ifstream in(folder / std::filesystem::path(assets::kCameraSupportFileName), std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto parsed = assets::parse_camera_support(text);
            if (!parsed) {
                return std::shared_ptr<const assets::CameraSupport>();
            }
            auto support = std::make_shared<assets::CameraSupport>();
            support->database = std::move(*parsed);
            return std::shared_ptr<const assets::CameraSupport>(std::move(support));
        }
        auto request = assets::bundled_asset_request(assets::kCameraSupportAssetId);
        if (!request) {
            return std::shared_ptr<const assets::CameraSupport>();
        }
        assets::AssetLocations locations;
        locations.bundled = assets::executable_directory() / L"assets";
        return assets::load_camera_support(locations, *request).active;
    }();
    return loaded;
}

TEST_P(QualificationFixture, ProbeKeyFindsTheQualifiedState) {
    const ManifestFixture& fixture = GetParam();
    const QualifyCorpus& c = corpus();
    const std::filesystem::path file = c.paths.cached_file(fixture);
    if (!std::filesystem::exists(file)) {
        GTEST_SKIP() << "fixture '" << fixture.id << "' is not downloaded";
    }
    ASSERT_TRUE(c.expectations.has_value());
    const FixtureExpectation* expectation = c.expectations->find(fixture.id);
    ASSERT_NE(expectation, nullptr);
    auto registry = raw::create_default_registry();
    ASSERT_TRUE(registry) << registry.error().describe();
    raw::ProbeOptions options;  // as app::import_sources() probes
    options.describe_subimages = true;
    auto selection = registry->select(file, options, {});
    ASSERT_TRUE(selection) << selection.error().describe();
    const ModeKey key = derive_mode_key(selection->probe.metadata);
    EXPECT_EQ(key.stage, "after_open");

    const std::shared_ptr<const assets::CameraSupport> database = bundled_database();
    ASSERT_NE(database, nullptr) << "the bundled camera support database did not load";
    const auto match = database->database.lookup(key.to_string());
    ASSERT_TRUE(match.has_value()) << "the probe's key " << key.to_string()
                                   << " is not in the camera support database";
    ASSERT_NE(match->entry, nullptr);
    EXPECT_EQ(match->entry->qualified_key, expectation->mode_key);
    EXPECT_EQ(std::string(assets::to_string(match->state)), expectation->expected_state);
    // QUA-08: the probe's facts beside the key are those the entry was qualified with, so the
    // application keeps a Verified state for exactly this kind of file.
    const assets::ModeVariant variant = assets::derive_mode_variant(selection->probe.metadata);
    EXPECT_EQ(variant.image_area, match->entry->image_area);
    EXPECT_EQ(variant.dng_opcodes, match->entry->dng_opcodes);
    if (key.to_string() != expectation->mode_key) {
        std::cout << "[ probe    ] " << fixture.id << ": probe key " << key.to_string()
                  << " differs from the "
                  << "decoded key " << expectation->mode_key << "\n";
    }
}

INSTANTIATE_TEST_SUITE_P(Manifest, QualificationFixture, ::testing::ValuesIn(manifest_fixtures()),
                         [](const ::testing::TestParamInfo<ManifestFixture>& info) {
                             std::string name = info.param.id;
                             for (char& ch : name) {
                                 if (ch == '-') {
                                     ch = '_';
                                 }
                             }
                             return name;
                         });

// ---- Colour reference (ADR 0020) ----------------------------------------------------------------------

std::vector<ManifestFixture> chart_fixtures() {
    auto charts = load_charts(paths().charts());
    if (!charts) {
        throw std::runtime_error("charts: " + charts.error().describe());
    }
    std::vector<ManifestFixture> out;
    for (const ManifestFixture& fixture : manifest_fixtures()) {
        if (charts->find(fixture.id) != nullptr) {
            out.push_back(fixture);
        }
    }
    return out;
}

class ColourReferenceFixture : public ::testing::TestWithParam<ManifestFixture> {};

// The develop path measured on a chart fixture: within the gate against the chart's published
// values, alike to the same measurement of LibRaw's own rendering (research/tools/chart_reference.py,
// recorded in charts.json), and sensitive: the measured patches turned into gross interpretation
// errors (red and blue exchanged, no camera matrix, a display curve on scene-linear values) fail it.
TEST_P(ColourReferenceFixture, DevelopPathMatchesThePublishedValuesAndTheIndependentRendering) {
    const ManifestFixture& fixture = GetParam();
    const QualifyCorpus& c = corpus();
    ASSERT_TRUE(c.expectations.has_value());
    ASSERT_TRUE(c.charts.has_value());
    const FixtureExpectation* expectation = c.expectations->find(fixture.id);
    ASSERT_NE(expectation, nullptr);
    if (fixture.tier != "smoke" && expectation->expected_state != "verified" && !qualification_tier_enabled()) {
        GTEST_SKIP() << "qualification-tier fixture that is not Verified; set LUCENT_TEST_FIXTURE_TIER=all";
    }
    const std::filesystem::path file = c.paths.cached_file(fixture);
    if (!std::filesystem::exists(file)) {
        GTEST_SKIP() << "fixture '" << fixture.id << "' is not downloaded";
    }
    const ChartFixture& chart = *c.charts->find(fixture.id);
    auto registry = raw::create_default_registry();
    ASSERT_TRUE(registry) << registry.error().describe();
    auto measured = measure_chart(*registry, file, chart);
    ASSERT_TRUE(measured) << measured.error().describe();
    const ChartEvaluation e = evaluate_chart(*c.charts, chart, measured->samples, rec2020_to_xyz(), image::kD65White);
    ASSERT_TRUE(e.measurable) << e.problem;
    std::cout << "[ chart    ] " << fixture.id << ": " << e.summary() << "\n";
    EXPECT_TRUE(e.passes) << e.summary();

    ASSERT_TRUE(chart.independent.has_value());
    const IndependentChartMeasurement& independent = *chart.independent;
    EXPECT_NEAR(e.balanced.mean, independent.chart_balanced.mean, 0.25);
    EXPECT_NEAR(e.balanced.p90, independent.chart_balanced.p90, 0.5);
    EXPECT_NEAR(e.balanced.max, independent.chart_balanced.max, 0.5);
    EXPECT_NEAR(e.as_shot.mean, independent.as_shot.mean, 0.25);

    const auto distorted = [&](const auto& camera_to_working) {
        std::vector<ChartSample> samples = measured->samples;
        for (ChartSample& s : samples) {
            s.working_mean = camera_to_working(s.camera_mean);
        }
        return evaluate_chart(*c.charts, chart, samples, rec2020_to_xyz(), image::kD65White);
    };
    const image::Matrix3d& m = measured->camera_to_working;
    const ChartEvaluation swapped =
        distorted([&](const image::Vec3d& v) { return m * image::Vec3d{v[2], v[1], v[0]}; });
    EXPECT_FALSE(swapped.passes) << "red and blue exchanged: " << swapped.summary();
    const ChartEvaluation no_matrix =
        distorted([&](const image::Vec3d& v) { return measured->gain * v; });
    EXPECT_FALSE(no_matrix.passes) << "camera RGB taken for the working space: " << no_matrix.summary();
    const ChartEvaluation display_curve = distorted([&](const image::Vec3d& v) {
        const image::Vec3d w = m * v;
        return image::Vec3d{std::pow(std::max(w[0], 0.0), 1.0 / 2.2), std::pow(std::max(w[1], 0.0), 1.0 / 2.2),
                            std::pow(std::max(w[2], 0.0), 1.0 / 2.2)};
    });
    EXPECT_FALSE(display_curve.passes) << "a display curve on scene-linear values: " << display_curve.summary();
}

INSTANTIATE_TEST_SUITE_P(Charts, ColourReferenceFixture, ::testing::ValuesIn(chart_fixtures()),
                         [](const ::testing::TestParamInfo<ManifestFixture>& info) {
                             std::string name = info.param.id;
                             for (char& ch : name) {
                                 if (ch == '-') {
                                     ch = '_';
                                 }
                             }
                             return name;
                         });

}  // namespace
}  // namespace lucent::qualify
