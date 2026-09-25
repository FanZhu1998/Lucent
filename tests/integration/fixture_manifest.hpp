#pragma once

// Test helpers for the camera fixture corpus (fixtures/README.md): the manifest
// (lucent.fixture-manifest v1), cached files and committed rawpy reference reports.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lucent::integration {

struct Fixture {
    std::string id;    // e.g. "canon-eos-r5-cr3-raw"
    std::string tier;  // "smoke" or "qualification"
    std::string make;
    std::string model;
    std::string format;     // "CR3", "DNG", ...
    std::string file_name;  // name inside fixtures/cache/<id>/
};

// fixtures/ in the source tree (LUCENT_FIXTURE_DIR).
[[nodiscard]] std::filesystem::path fixture_root();

// Fixtures listed by fixtures/manifest.json, in manifest order. Throws std::runtime_error when
// the manifest is missing, malformed or of a newer schema version (a test-setup failure).
[[nodiscard]] std::vector<Fixture> load_manifest();

// fixtures/cache/<id>/<file_name>; the file may not exist.
[[nodiscard]] std::filesystem::path cached_file(const Fixture& fixture);

// fixtures/reference/<id>.parity.json.
[[nodiscard]] std::filesystem::path reference_file(const Fixture& fixture);

// Parses a JSON file; throws std::runtime_error on failure.
[[nodiscard]] nlohmann::json read_json(const std::filesystem::path& path);

// A fixture id as a GoogleTest parameter name ("canon_eos_r5_cr3_raw").
[[nodiscard]] std::string test_name(const Fixture& fixture);

}  // namespace lucent::integration
