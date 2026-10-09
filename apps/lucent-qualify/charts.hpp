#pragma once

// Colour-chart definitions for the qualification check colour_reference
// (fixtures/qualification/charts.json, schema "lucent.qualification-charts" version 1; ADR 0020).
//
// The document lists published colour-chart reference data and, for every corpus fixture whose
// photograph shows a chart, where its patches are:
//
//   references   id -> {chart, publisher, title, url, retrieved, illuminant ("D50"), observer
//                ("CIE 1931 2 degree"), patches: id -> {name, lab: [L*, a*, b*]}, ...}: the
//                manufacturer's published CIE L*a*b* values, cited with URL and retrieval date
//                (other members are documentary and not read here);
//   fixtures     [{id, reference, alternate_references?, frame: {size: [w, h], origin: [x, y]},
//                normalisation_patch, patches: id -> [x, y, width, height],
//                independent_measurement?: {renderer, measured, as_shot: {mean, p90, max},
//                chart_balanced: {mean, p90, max}}, ...}]: sample boxes in the fixture's
//                camera-area frame (the upright image the develop render outputs, geometry version
//                2, ADR 0017; `origin` is that frame's top-left corner in the decoder's whole upright
//                visible area, where renderers that ignore the camera's area find it), and the
//                statistics research/tools/chart_reference.py measured on a rendering that uses no
//                Lucent code (reported beside Lucent's, never gated on).
//
// Patches are held sorted by id (the JSON reader's object order); statistics do not depend on it.
//
// The loader rejects (InvalidArgument, where "qualify.charts") anything the measurement could
// silently get wrong: an unknown reference, a reference whose illuminant is not D50 or whose
// observer is not the CIE 1931 2 degree observer, a patch without finite L*a*b* (0 <= L* <= 100),
// a box that is empty or leaves its frame, a box for a patch the reference does not list, a
// reference patch without a box, a normalisation patch without a box, or a duplicate fixture;
// VersionMismatch for another schema or a newer schema_version.

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <lucent/core/error.hpp>

namespace lucent::qualify {

inline constexpr std::string_view kChartsSchema = "lucent.qualification-charts";
inline constexpr int kChartsSchemaVersion = 1;

struct ChartPatch {
    std::string id;    // e.g. "1A" (SpyderCHECKR) or "A1" (ColorChecker CGATS sample name)
    std::string name;  // e.g. "Low Sat. Red"
    std::array<double, 3> lab{};  // CIE L*a*b*, D50 white, CIE 1931 2 degree observer
};

struct ChartReference {
    std::string id;
    std::string chart;
    std::string publisher;
    std::string url;
    std::string retrieved;  // YYYY-MM-DD
    std::vector<ChartPatch> patches;  // sorted by id

    [[nodiscard]] const ChartPatch* find(std::string_view patch) const noexcept;
};

// Mean, 90th percentile and maximum of a set of CIEDE2000 values.
struct ChartStatistics {
    double mean = 0.0;
    double p90 = 0.0;
    double max = 0.0;
};

// What an independent rendering measured on the chart (research/tools/chart_reference.py).
struct IndependentChartMeasurement {
    std::string renderer;  // e.g. "rawpy 0.27.1 (LibRaw 0.22.1), LibRaw's camera matrix"
    std::string measured;  // YYYY-MM-DD
    ChartStatistics as_shot;
    ChartStatistics chart_balanced;
};

struct ChartBox {
    std::string patch;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

struct ChartFixture {
    std::string id;
    std::string reference;
    std::vector<std::string> alternate_references;
    std::array<std::int32_t, 2> frame_size{};    // width, height of the camera-area frame
    std::array<std::int32_t, 2> frame_origin{};  // its top-left corner in the upright visible area
    std::string normalisation_patch;
    std::vector<ChartBox> boxes;  // in the reference's patch order
    std::optional<IndependentChartMeasurement> independent;

    [[nodiscard]] const ChartBox* box(std::string_view patch) const noexcept;
};

struct ChartDefinitions {
    int schema_version = kChartsSchemaVersion;
    std::string reviewed;  // YYYY-MM-DD
    std::map<std::string, ChartReference> references;
    std::vector<ChartFixture> fixtures;

    [[nodiscard]] const ChartFixture* find(std::string_view fixture_id) const noexcept;
    [[nodiscard]] const ChartReference* reference(std::string_view id) const noexcept;
};

[[nodiscard]] Expected<ChartDefinitions> parse_charts(const nlohmann::json& document);
[[nodiscard]] Expected<ChartDefinitions> load_charts(const std::filesystem::path& path);

}  // namespace lucent::qualify
