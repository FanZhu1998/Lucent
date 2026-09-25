#include "fixture_manifest.hpp"

#include <fstream>
#include <stdexcept>

namespace lucent::integration {

namespace {

constexpr int kManifestSchemaVersion = 1;

std::string string_field(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        throw std::runtime_error(std::string("fixture manifest entry lacks string field '") + key + "'");
    }
    return it->get<std::string>();
}

}  // namespace

std::filesystem::path fixture_root() {
    return std::filesystem::path(LUCENT_FIXTURE_DIR);
}

nlohmann::json read_json(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path.string());
    }
    try {
        return nlohmann::json::parse(in);
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error(path.string() + " is not valid JSON: " + e.what());
    }
}

std::vector<Fixture> load_manifest() {
    const nlohmann::json manifest = read_json(fixture_root() / "manifest.json");
    if (manifest.value("schema", std::string()) != "lucent.fixture-manifest") {
        throw std::runtime_error("fixtures/manifest.json is not a lucent.fixture-manifest");
    }
    const int version = manifest.value("schema_version", 0);
    if (version < 1 || version > kManifestSchemaVersion) {
        throw std::runtime_error("fixtures/manifest.json has unsupported schema_version " +
                                 std::to_string(version));
    }
    std::vector<Fixture> fixtures;
    for (const nlohmann::json& entry : manifest.at("fixtures")) {
        Fixture f;
        f.id = string_field(entry, "id");
        f.tier = string_field(entry, "tier");
        f.make = string_field(entry, "make");
        f.model = string_field(entry, "model");
        f.format = string_field(entry, "format");
        f.file_name = string_field(entry, "file_name");
        fixtures.push_back(std::move(f));
    }
    return fixtures;
}

std::filesystem::path cached_file(const Fixture& fixture) {
    // Manifest names are UTF-8.
    return fixture_root() / "cache" / fixture.id /
           std::filesystem::path(std::u8string(fixture.file_name.begin(), fixture.file_name.end()));
}

std::filesystem::path reference_file(const Fixture& fixture) {
    return fixture_root() / "reference" / (fixture.id + ".parity.json");
}

std::string test_name(const Fixture& fixture) {
    std::string name = fixture.id;
    for (char& c : name) {
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum) {
            c = '_';
        }
    }
    return name;
}

}  // namespace lucent::integration
