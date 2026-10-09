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
// W10 phase 3 (ADR 0018): the application assesses a library item by the mode key of the probe it
// makes at import, not of a decode. ProbeKeyFindsTheQualifiedState probes every cached fixture as
// the importer does (any tier: a probe takes milliseconds) and asserts that the bundled camera
// support database finds, under the probe's key, the entry of the fixture's qualified (decoded) key
// with the expected state. The two keys differ where a probe sees a provisional value (the Canon
// S-RAW white level: 15 bits at the probe, 14 after unpacking); the database lists such a mode under
// the probe's key (support_export.hpp).

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
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
#include <lucent/raw/decoder_registry.hpp>

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
}

// The bundled database, as Lucent.exe loads it (next to this executable, verified against the pin).
std::shared_ptr<const assets::CameraSupport> bundled_database() {
    static const std::shared_ptr<const assets::CameraSupport> loaded = [] {
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

}  // namespace
}  // namespace lucent::qualify
