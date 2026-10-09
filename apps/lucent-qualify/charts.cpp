#include "charts.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

#include "expectations.hpp"

namespace lucent::qualify {

namespace {

using Json = nlohmann::json;

constexpr std::string_view kWhere = "qualify.charts";

struct ChartsError : std::runtime_error {
    ErrorCode code;
    ChartsError(ErrorCode c, const std::string& message) : std::runtime_error(message), code(c) {}
};

[[noreturn]] void invalid(const std::string& message) {
    throw ChartsError(ErrorCode::InvalidArgument, message);
}

std::string text(const Json& object, const char* key, const std::string& where) {
    if (!object.is_object() || !object.contains(key) || !object.at(key).is_string()) {
        invalid(where + ": \"" + key + "\" is missing or not a string");
    }
    return object.at(key).get<std::string>();
}

std::array<std::int32_t, 2> pair_of(const Json& value, const std::string& what) {
    if (!value.is_array() || value.size() != 2 || !value[0].is_number_integer() || !value[1].is_number_integer()) {
        invalid(what + " is not a pair of integers");
    }
    return {value[0].get<std::int32_t>(), value[1].get<std::int32_t>()};
}

ChartStatistics statistics_of(const Json& s, const std::string& what) {
    ChartStatistics out;
    for (const auto& [key, member] : {std::pair{"mean", &out.mean}, std::pair{"p90", &out.p90}, std::pair{"max", &out.max}}) {
        if (!s.is_object() || !s.contains(key) || !s.at(key).is_number() || !std::isfinite(s.at(key).get<double>()) ||
            s.at(key).get<double>() < 0.0) {
            invalid(what + ": \"" + key + "\" is not a non-negative number");
        }
        *member = s.at(key).get<double>();
    }
    return out;
}

ChartReference reference_of(const std::string& id, const Json& r) {
    const std::string at = "reference " + id;
    ChartReference ref;
    ref.id = id;
    ref.chart = text(r, "chart", at);
    ref.publisher = text(r, "publisher", at);
    ref.url = text(r, "url", at);
    ref.retrieved = text(r, "retrieved", at);
    if (text(r, "illuminant", at) != "D50") {
        invalid(at + ": only D50 reference data is supported (the measurement adapts to the D50 white)");
    }
    if (text(r, "observer", at) != "CIE 1931 2 degree") {
        invalid(at + ": only the CIE 1931 2 degree observer is supported");
    }
    if (!r.contains("patches") || !r.at("patches").is_object() || r.at("patches").empty()) {
        invalid(at + ": no patches");
    }
    for (const auto& [patch_id, p] : r.at("patches").items()) {
        ChartPatch patch;
        patch.id = patch_id;
        patch.name = text(p, "name", at + " patch " + patch_id);
        const Json& lab = p.contains("lab") ? p.at("lab") : Json();
        if (!lab.is_array() || lab.size() != 3 ||
            std::any_of(lab.begin(), lab.end(), [](const Json& v) { return !v.is_number(); })) {
            invalid(at + " patch " + patch_id + ": \"lab\" is not [L*, a*, b*]");
        }
        for (std::size_t i = 0; i < 3; ++i) {
            patch.lab[i] = lab[i].get<double>();
            if (!std::isfinite(patch.lab[i])) {
                invalid(at + " patch " + patch_id + ": L*a*b* is not finite");
            }
        }
        if (patch.lab[0] < 0.0 || patch.lab[0] > 100.0) {
            invalid(at + " patch " + patch_id + ": L* outside 0..100");
        }
        ref.patches.push_back(std::move(patch));
    }
    return ref;
}

ChartFixture fixture_of(const Json& f, const std::map<std::string, ChartReference>& references) {
    ChartFixture fixture;
    fixture.id = text(f, "id", "chart fixture");
    const std::string at = "chart fixture " + fixture.id;
    fixture.reference = text(f, "reference", at);
    const auto ref = references.find(fixture.reference);
    if (ref == references.end()) {
        invalid(at + ": unknown reference \"" + fixture.reference + "\"");
    }
    if (f.contains("alternate_references")) {
        for (const Json& alt : f.at("alternate_references")) {
            if (!alt.is_string() || !references.contains(alt.get<std::string>())) {
                invalid(at + ": unknown alternate reference " + alt.dump());
            }
            fixture.alternate_references.push_back(alt.get<std::string>());
        }
    }
    if (!f.contains("frame") || !f.at("frame").is_object()) {
        invalid(at + ": no frame");
    }
    fixture.frame_size = pair_of(f.at("frame").value("size", Json()), at + " frame.size");
    fixture.frame_origin = pair_of(f.at("frame").value("origin", Json()), at + " frame.origin");
    if (fixture.frame_size[0] < 1 || fixture.frame_size[1] < 1 || fixture.frame_origin[0] < 0 ||
        fixture.frame_origin[1] < 0) {
        invalid(at + ": the frame is empty or has a negative origin");
    }
    fixture.normalisation_patch = text(f, "normalisation_patch", at);
    if (!f.contains("patches") || !f.at("patches").is_object()) {
        invalid(at + ": no patches");
    }
    const Json& boxes = f.at("patches");
    for (const auto& [patch_id, box] : boxes.items()) {
        if (ref->second.find(patch_id) == nullptr) {
            invalid(at + ": patch " + patch_id + " is not in the reference " + fixture.reference);
        }
    }
    // Every patch the reference lists has a box (a chart is measured whole), in the reference's order.
    for (const ChartPatch& patch : ref->second.patches) {
        if (!boxes.contains(patch.id)) {
            invalid(at + ": the reference's patch " + patch.id + " has no sample box");
        }
        const Json& b = boxes.at(patch.id);
        if (!b.is_array() || b.size() != 4 ||
            std::any_of(b.begin(), b.end(), [](const Json& v) { return !v.is_number_integer(); })) {
            invalid(at + ": the box of " + patch.id + " is not [x, y, width, height]");
        }
        ChartBox box{patch.id, b[0].get<std::int32_t>(), b[1].get<std::int32_t>(), b[2].get<std::int32_t>(),
                     b[3].get<std::int32_t>()};
        if (box.width < 1 || box.height < 1 || box.x < 0 || box.y < 0 ||
            box.x + box.width > fixture.frame_size[0] || box.y + box.height > fixture.frame_size[1]) {
            invalid(at + ": the box of " + patch.id + " is empty or leaves the frame");
        }
        fixture.boxes.push_back(box);
    }
    if (fixture.box(fixture.normalisation_patch) == nullptr) {
        invalid(at + ": the normalisation patch " + fixture.normalisation_patch + " has no box");
    }
    if (f.contains("independent_measurement")) {
        const Json& m = f.at("independent_measurement");
        IndependentChartMeasurement measurement;
        measurement.renderer = text(m, "renderer", at + " independent_measurement");
        measurement.measured = text(m, "measured", at + " independent_measurement");
        measurement.as_shot = statistics_of(m.value("as_shot", Json()), at + " independent_measurement.as_shot");
        measurement.chart_balanced =
            statistics_of(m.value("chart_balanced", Json()), at + " independent_measurement.chart_balanced");
        fixture.independent = std::move(measurement);
    }
    return fixture;
}

}  // namespace

const ChartPatch* ChartReference::find(std::string_view patch) const noexcept {
    const auto it = std::find_if(patches.begin(), patches.end(), [&](const ChartPatch& p) { return p.id == patch; });
    return it == patches.end() ? nullptr : &*it;
}

const ChartBox* ChartFixture::box(std::string_view patch) const noexcept {
    const auto it = std::find_if(boxes.begin(), boxes.end(), [&](const ChartBox& b) { return b.patch == patch; });
    return it == boxes.end() ? nullptr : &*it;
}

const ChartFixture* ChartDefinitions::find(std::string_view fixture_id) const noexcept {
    const auto it =
        std::find_if(fixtures.begin(), fixtures.end(), [&](const ChartFixture& f) { return f.id == fixture_id; });
    return it == fixtures.end() ? nullptr : &*it;
}

const ChartReference* ChartDefinitions::reference(std::string_view id) const noexcept {
    const auto it = references.find(std::string(id));
    return it == references.end() ? nullptr : &it->second;
}

Expected<ChartDefinitions> parse_charts(const nlohmann::json& document) {
    try {
        if (!document.is_object() || document.value("schema", std::string()) != kChartsSchema) {
            return fail(ErrorCode::VersionMismatch, "not a lucent.qualification-charts document", std::string(kWhere));
        }
        ChartDefinitions charts;
        charts.schema_version = document.at("schema_version").get<int>();
        if (charts.schema_version < 1 || charts.schema_version > kChartsSchemaVersion) {
            return fail(ErrorCode::VersionMismatch,
                        "charts schema_version " + std::to_string(charts.schema_version) + " is not readable",
                        std::string(kWhere));
        }
        charts.reviewed = text(document, "reviewed", "charts");
        if (!document.contains("references") || !document.at("references").is_object()) {
            invalid("charts: no references");
        }
        for (const auto& [id, r] : document.at("references").items()) {
            charts.references.emplace(id, reference_of(id, r));
        }
        std::set<std::string> seen;
        for (const Json& f : document.at("fixtures")) {
            ChartFixture fixture = fixture_of(f, charts.references);
            if (!seen.insert(fixture.id).second) {
                invalid("duplicate chart fixture " + fixture.id);
            }
            charts.fixtures.push_back(std::move(fixture));
        }
        return charts;
    } catch (const ChartsError& error) {
        return fail(error.code, error.what(), std::string(kWhere));
    } catch (const nlohmann::json::exception& error) {
        return fail(ErrorCode::InvalidArgument, std::string("malformed charts document: ") + error.what(),
                    std::string(kWhere));
    }
}

Expected<ChartDefinitions> load_charts(const std::filesystem::path& path) {
    auto document = read_json_file(path);
    if (!document) {
        return Unexpected(std::move(document.error()));
    }
    return parse_charts(*document);
}

}  // namespace lucent::qualify
