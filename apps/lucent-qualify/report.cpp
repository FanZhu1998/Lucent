#include "report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>
#include <sstream>
#include <utility>

#include <lucent/core/hash.hpp>
#include <lucent/core/version.hpp>
#include <lucent/engine/develop.hpp>
#include <lucent/engine/dng_opcode_processing.hpp>
#include <lucent/engine/render.hpp>
#include <lucent/raw/decoded_raw.hpp>

namespace lucent::qualify {

namespace {

using Json = nlohmann::json;

std::vector<std::string> strings_of(const Json& value) {
    std::vector<std::string> out;
    if (value.is_string()) {
        out.push_back(value.get<std::string>());
    } else if (value.is_array()) {
        for (const Json& v : value) {
            out.push_back(v.get<std::string>());
        }
    }
    return out;
}

std::string file_sha256(const std::filesystem::path& path) {
    auto hashed = core::hash_file(path, {});
    return hashed ? core::to_hex(hashed->digest) : std::string();
}

std::string utc_now() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    gmtime_s(&tm, &now);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return text;
}

std::string limitation_label(std::string_view id) {
    if (id == kLimitDefaultCrop) {
        return "default crop not applied";
    }
    if (id == kLimitXTransBaseline) {
        return "X-Trans demosaic is baseline quality";
    }
    if (id == kLimitDualPixel) {
        return "Dual Pixel data not processed";
    }
    if (id == kLimitEnhancedImage) {
        return "DNG Enhanced image not offered";
    }
    if (id == kLimitOptionalOperations) {
        return "optional operations skipped";
    }
    if (id == kLimitNonSquarePixels) {
        return "non-square pixels not resampled";
    }
    if (id == kLimitPixelShift) {
        return "pixel-shift frames not combined";
    }
    return std::string(id);
}

std::string escape_cell(std::string text) {
    std::string out;
    for (const char c : text) {
        if (c == '|') {
            out += "\\|";
        } else if (c == '\n') {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

// "12 of 13 checks pass with strong evidence; weak: colour" for a fixture result.
std::string evidence_text(const OrderedJson& f) {
    int applicable = 0;
    int strong = 0;
    std::vector<std::string> weak;
    std::vector<std::string> failed;
    std::vector<std::string> not_run;
    for (const OrderedJson& c : f.at("checks")) {
        const std::string status = c.at("status").get<std::string>();
        const std::string id = c.at("id").get<std::string>();
        if (status == "not_applicable") {
            continue;
        }
        ++applicable;
        if (status == "pass" && c.at("evidence") == "strong") {
            ++strong;
        } else if (status == "pass") {
            weak.push_back(id);
        } else if (status == "fail") {
            failed.push_back(id);
        } else {
            not_run.push_back(id);
        }
    }
    std::ostringstream out;
    out << strong << " of " << applicable << " checks strong";
    const auto list = [&](const char* label, const std::vector<std::string>& ids) {
        if (!ids.empty()) {
            out << "; " << label << ": ";
            for (std::size_t i = 0; i < ids.size(); ++i) {
                out << (i ? ", " : "") << ids[i];
            }
        }
    };
    list("weak", weak);
    list("failed", failed);
    list("not run", not_run);
    return out.str();
}

std::string state_name(std::string_view state) {
    if (state == "verified") {
        return "**Verified**";
    }
    if (state == "compatible_unverified") {
        return "Compatible, unverified";
    }
    if (state == "limited") {
        return "Limited";
    }
    if (state == "unsupported") {
        return "Unsupported";
    }
    return "Not qualified";
}

std::string megabytes(const OrderedJson& value) {
    if (!value.is_number()) {
        return "";
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f", value.get<double>() / (1024.0 * 1024.0));
    return text;
}

std::string seconds_text(const OrderedJson& value) {
    if (!value.is_number()) {
        return "";
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", value.get<double>());
    return text;
}

}  // namespace

int state_severity(std::string_view state) noexcept {
    if (state == "not_qualified") {
        return 0;
    }
    if (state == "unsupported") {
        return 1;
    }
    if (state == "limited") {
        return 2;
    }
    if (state == "compatible_unverified") {
        return 3;
    }
    if (state == "verified") {
        return 4;
    }
    return -1;
}

Expected<std::vector<Family>> load_families(const std::filesystem::path& path) {
    auto document = read_json_file(path);
    if (!document) {
        return Unexpected(std::move(document.error()));
    }
    try {
        const Json& d = *document;
        if (d.value("schema", std::string()) != kFamiliesSchema) {
            return fail(ErrorCode::VersionMismatch, "not a lucent.qualification-families document",
                        "qualify.families");
        }
        if (d.at("schema_version").get<int>() > kFamiliesSchemaVersion) {
            return fail(ErrorCode::VersionMismatch, "newer families schema", "qualify.families");
        }
        std::vector<Family> families;
        for (const Json& f : d.at("families")) {
            Family family;
            family.id = f.at("id").get<std::string>();
            family.name = f.at("name").get<std::string>();
            family.blueprint_targets = f.value("blueprint_targets", std::string());
            family.required_distinctions = f.value("required_distinctions", std::string());
            for (const Json& t : f.at("targets")) {
                FamilyTarget target;
                target.id = t.at("id").get<std::string>();
                target.description = t.at("description").get<std::string>();
                target.note = t.value("note", std::string());
                const Json m = t.value("match", Json());
                if (m.is_null()) {
                    target.match.never = true;
                    family.targets.push_back(std::move(target));
                    continue;
                }
                target.match.make = m.value("make", std::string());
                target.match.models = strings_of(m.value("model", Json()));
                target.match.models_not = strings_of(m.value("model_not", Json()));
                target.match.containers = strings_of(m.value("container", Json()));
                target.match.compression = m.value("compression", std::string());
                target.match.lossiness = m.value("lossiness", std::string());
                target.match.area = m.value("area", std::string());
                target.match.representation_prefix = m.value("representation_prefix", std::string());
                target.match.view = m.value("view", std::string());
                target.match.subimage_count_min = m.value("subimage_count_min", 0u);
                target.match.subimage_count_max = m.value("subimage_count_max", 0u);
                family.targets.push_back(std::move(target));
            }
            families.push_back(std::move(family));
        }
        return families;
    } catch (const nlohmann::json::exception& error) {
        return fail(ErrorCode::InvalidArgument, std::string("malformed families file: ") + error.what(),
                    "qualify.families");
    }
}

bool matches(const TargetMatch& m, const OrderedJson& key) {
    if (m.never || !key.is_object()) {
        return false;
    }
    const auto field = [&](const char* name) { return key.value(name, std::string()); };
    const auto in = [](const std::vector<std::string>& list, const std::string& value) {
        return std::find(list.begin(), list.end(), value) != list.end();
    };
    if (!m.make.empty() && field("make") != m.make) {
        return false;
    }
    if (!m.models.empty() && !in(m.models, field("model"))) {
        return false;
    }
    if (in(m.models_not, field("model"))) {
        return false;
    }
    if (!m.containers.empty() && !in(m.containers, field("container"))) {
        return false;
    }
    if (!m.compression.empty() && field("compression") != m.compression) {
        return false;
    }
    if (!m.lossiness.empty() && field("lossiness") != m.lossiness) {
        return false;
    }
    if (!m.area.empty() && field("area") != m.area) {
        return false;
    }
    if (!m.representation_prefix.empty() && field("representation").rfind(m.representation_prefix, 0) != 0) {
        return false;
    }
    if (!m.view.empty() && field("view") != m.view) {
        return false;
    }
    if (m.subimage_count_min > 0 && key.value("subimage_count", 0u) < m.subimage_count_min) {
        return false;
    }
    if (m.subimage_count_max > 0 && key.value("subimage_count", 0u) > m.subimage_count_max) {
        return false;
    }
    return true;
}

std::string coverage_status(const std::vector<std::string>& matching_states) {
    if (matching_states.empty()) {
        return "absent";
    }
    const auto verified = std::count(matching_states.begin(), matching_states.end(), std::string("verified"));
    if (verified == static_cast<std::ptrdiff_t>(matching_states.size())) {
        return "verified";
    }
    return verified > 0 ? "partially_verified" : "covered_not_verified";
}

OrderedJson build_report(const std::vector<OrderedJson>& fixtures, const ReportInputs& inputs,
                         const std::vector<Family>& families, const nlohmann::json& manifest_document) {
    OrderedJson report;
    report["schema"] = kReportSchema;
    report["schema_version"] = kReportSchemaVersion;
    report["generated"] = utc_now();
    const core::BuildInfo build = core::build_info();
    report["tool"] = {{"name", "lucent-qualify"},
                      {"lucent_version", build.version},
                      {"git_commit", build.git_commit},
                      {"build_type", build.build_type},
                      {"compiler", build.compiler}};
    std::string decoder;
    for (const OrderedJson& f : fixtures) {
        if (f.contains("decoder") && f.at("decoder").is_string() &&
            !f.at("decoder").get<std::string>().empty()) {
            decoder = f.at("decoder").get<std::string>();
            break;
        }
    }
    report["decoder"] = decoder;
    report["versions"] = {{"mode_key", kModeKeyVersion},
                          {"check_suite", kCheckSuiteVersion},
                          {"state_rules", kStateRulesVersion},
                          {"decoded_raw_contract", raw::kDecodedRawContractVersion.to_string()},
                          {"render_pipeline", engine::kRenderPipelineVersion},
                          {"develop_pipeline", engine::kDevelopPipelineVersion},
                          {"dng_opcode_processing", engine::kDngOpcodeProcessingVersion},
                          {"geometry_operation", engine::kGeometryOperationVersion},
                          {"image_area_placement", engine::kImageAreaPlacementVersion}};
    const auto input = [](const std::filesystem::path& path, const std::filesystem::path& root) {
        std::error_code ec;
        const std::filesystem::path relative = std::filesystem::relative(path, root, ec);
        return OrderedJson{{"path", (ec ? path : relative).generic_string()}, {"sha256", file_sha256(path)}};
    };
    report["inputs"] = {{"manifest", input(inputs.paths.manifest(), inputs.paths.root)},
                        {"expectations", input(inputs.paths.expectations(), inputs.paths.root)},
                        {"families", input(inputs.paths.families(), inputs.paths.root)},
                        {"render_reference", input(inputs.paths.render_reference(), inputs.paths.root)}};
    report["selection"] = {
        {"tier", inputs.tier}, {"ids", inputs.ids}, {"isolated_processes", inputs.isolated}};

    // Summary.
    std::map<std::string, int> by_state;
    int mismatched = 0;
    int harness_errors = 0;
    for (const OrderedJson& f : fixtures) {
        ++by_state[f.value("proposed_state", std::string("not_qualified"))];
        if (f.contains("expectation_mismatches") && !f.at("expectation_mismatches").empty()) {
            ++mismatched;
        }
        if (f.contains("harness_error") && !f.at("harness_error").is_null()) {
            ++harness_errors;
        }
    }
    report["summary"] = {{"fixtures", fixtures.size()},
                         {"by_state", by_state},
                         {"expectation_mismatches", mismatched},
                         {"harness_errors", harness_errors}};
    report["fixtures"] = fixtures;

    // Support table: one row per mode key, the worst state of its fixtures.
    std::vector<std::string> order;
    std::map<std::string, std::vector<const OrderedJson*>> by_mode;
    for (const OrderedJson& f : fixtures) {
        const std::string key = f.at("mode_key").is_object()
                                    ? f.at("mode_key").at("text").get<std::string>()
                                    : "unidentified:" + f.at("id").get<std::string>();
        if (!by_mode.contains(key)) {
            order.push_back(key);
        }
        by_mode[key].push_back(&f);
    }
    OrderedJson table = OrderedJson::array();
    for (const std::string& key : order) {
        const auto& group = by_mode.at(key);
        const OrderedJson* worst = group.front();
        std::set<std::string> limitations;
        std::set<std::string> lookup_keys;
        std::vector<std::string> ids;
        std::vector<std::string> gaps;
        for (const OrderedJson* f : group) {
            if (f->contains("lookup_key") && f->at("lookup_key").is_string()) {
                lookup_keys.insert(f->at("lookup_key").get<std::string>());
            }
            if (state_severity(f->value("proposed_state", std::string())) <
                state_severity(worst->value("proposed_state", std::string()))) {
                worst = f;
            }
            ids.push_back(f->at("id").get<std::string>());
            for (const OrderedJson& l : f->at("limitations")) {
                limitations.insert(l.at("id").get<std::string>());
            }
            for (const OrderedJson& g : f->at("evidence_gaps")) {
                gaps.push_back(g.get<std::string>());
            }
        }
        table.push_back(
            {{"mode_key", key},
             {"lookup_keys", std::vector<std::string>(lookup_keys.begin(), lookup_keys.end())},
             {"model_key",
              worst->at("mode_key").is_object() ? worst->at("mode_key").at("model_key") : OrderedJson("")},
             {"family", worst->value("family", std::string())},
             {"camera", worst->at("camera").value("make", std::string()) + " " +
                            worst->at("camera").value("model", std::string())},
             {"product", worst->value("product", std::string())},
             {"mode_label", worst->value("mode_label", std::string())},
             {"fixtures", ids},
             {"proposed_state", worst->value("proposed_state", std::string())},
             {"evidence", evidence_text(*worst)},
             {"limitations", std::vector<std::string>(limitations.begin(), limitations.end())},
             {"evidence_gaps", gaps}});
    }
    report["support_table"] = std::move(table);

    // Coverage against the blueprint families.
    OrderedJson coverage_families = OrderedJson::array();
    for (const Family& family : families) {
        OrderedJson targets = OrderedJson::array();
        for (const FamilyTarget& target : family.targets) {
            OrderedJson matched = OrderedJson::array();
            std::vector<std::string> states;
            for (const OrderedJson& f : fixtures) {
                if (matches(target.match, f.at("mode_key"))) {
                    const std::string state = f.value("proposed_state", std::string());
                    matched.push_back({{"id", f.at("id")}, {"state", state}});
                    states.push_back(state);
                }
            }
            targets.push_back({{"id", target.id},
                               {"description", target.description},
                               {"note", target.note},
                               {"status", coverage_status(states)},
                               {"fixtures", std::move(matched)}});
        }
        coverage_families.push_back({{"id", family.id},
                                     {"name", family.name},
                                     {"blueprint_targets", family.blueprint_targets},
                                     {"required_distinctions", family.required_distinctions},
                                     {"targets", std::move(targets)}});
    }
    report["coverage"] = {{"families", std::move(coverage_families)},
                          {"corpus_gaps", manifest_document.contains("gaps")
                                              ? OrderedJson::parse(manifest_document.at("gaps").dump())
                                              : OrderedJson::array()}};
    return report;
}

std::string support_table_markdown(const OrderedJson& report) {
    std::ostringstream md;
    const std::string generated = report.value("generated", std::string());
    const OrderedJson& summary = report.at("summary");
    md << "# Camera support table\n\n";
    md << "<!-- Generated by lucent-qualify (" << kReportSchema << " v" << kReportSchemaVersion
       << "). Do not edit by hand: run `lucent-qualify --tier all --out-md "
          "docs/qualification/support-table.md`. -->\n\n";
    md << "Generated " << generated.substr(0, 10) << " from " << summary.at("fixtures")
       << " fixtures (selection: " << report.at("selection").value("tier", std::string()) << ") by Lucent "
       << report.at("tool").value("lucent_version", std::string()) << " ("
       << report.at("tool").value("git_commit", std::string()) << ", "
       << report.at("tool").value("build_type", std::string()) << ") with "
       << report.value("decoder", std::string()) << ". Mode key version "
       << report.at("versions").at("mode_key") << ", check suite " << report.at("versions").at("check_suite")
       << ", state rules " << report.at("versions").at("state_rules") << "; render pipeline "
       << report.at("versions").at("render_pipeline") << ", develop pipeline "
       << report.at("versions").at("develop_pipeline") << ", DNG opcode processing "
       << report.at("versions").at("dng_opcode_processing") << ", geometry operation "
       << report.at("versions").value("geometry_operation", 1) << ", image area placement "
       << report.at("versions").value("image_area_placement", 0) << " (ADR 0016, ADR 0017).\n\n";
    md << "These states are **proposals from Lucent's qualification harness** for the exact camera model and "
          "capture "
          "mode of each corpus fixture. They are not a claim about other modes of the same camera, other "
          "cameras of "
          "the same brand, or other files with the same extension. Every corpus mode is represented by a "
          "single "
          "daylight test file from raw.pixls.us; scene diversity (blueprint section 10) is not yet covered. "
          "Lens-profile corrections (Lensfun) are not available for any camera yet; they are optional by the "
          "blueprint and are not listed per mode. A Verified state holds for the decoder, contract and "
          "pipeline "
          "versions above. Lucent shows these states to the photographer through the camera support database "
          "exported from this report (`assets/camera-support`, ADR 0018), combined with each file's own "
          "interpretation: a photo of a mode not listed here is Compatible, unverified at best.\n\n";

    md << "| State | Meaning (blueprint section 3) |\n|---|---|\n";
    md << "| Verified | Exact model and capture mode passed Lucent's decoding, colour, geometry and export "
          "checks with "
          "strong evidence, against independently verified expectations |\n";
    md << "| Compatible, unverified | Decoder and required interpretation steps succeed, but the evidence or "
          "the "
          "expectations are insufficient |\n";
    md << "| Limited | A known optional capability is unavailable; the conservative path is described |\n";
    md << "| Unsupported | Decoding or a mandatory interpretation step is unavailable |\n\n";

    md << "Summary:";
    for (const auto& [state, count] : summary.at("by_state").items()) {
        md << " " << state_name(state) << " " << count << ";";
    }
    md << " expectation mismatches " << summary.at("expectation_mismatches") << ", harness errors "
       << summary.at("harness_errors") << ".\n\n";

    md << "## Support table\n\n";
    md << "Rows are grouped by the blueprint's qualification families, then by camera. \"Evidence\" counts "
          "the "
          "checks that apply and pass with strong evidence; the reasons and gaps of each fixture follow "
          "below.\n\n";
    md << "| Family | Camera (make and model as decoded) | Product | Mode | Fixture | Proposed state | "
          "Evidence | "
          "Limitations |\n";
    md << "|---|---|---|---|---|---|---|---|\n";
    std::vector<const OrderedJson*> rows;
    for (const OrderedJson& row : report.at("support_table")) {
        rows.push_back(&row);
    }
    static const std::vector<std::string> kFamilyOrder{"canon", "sony",     "dji",  "insta360",
                                                       "nikon", "fujifilm", "other"};
    const auto rank = [](const OrderedJson& row) {
        const auto it =
            std::find(kFamilyOrder.begin(), kFamilyOrder.end(), row.value("family", std::string()));
        return static_cast<std::size_t>(it - kFamilyOrder.begin());
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const OrderedJson* a, const OrderedJson* b) {
        if (rank(*a) != rank(*b)) {
            return rank(*a) < rank(*b);
        }
        return a->value("camera", std::string()) < b->value("camera", std::string());
    });
    for (const OrderedJson* row_ptr : rows) {
        const OrderedJson& row = *row_ptr;
        std::string limits;
        for (const OrderedJson& l : row.at("limitations")) {
            limits += (limits.empty() ? "" : "; ") + limitation_label(l.get<std::string>());
        }
        std::string ids;
        for (const OrderedJson& id : row.at("fixtures")) {
            ids += (ids.empty() ? "" : ", ") + std::string("`") + id.get<std::string>() + "`";
        }
        md << "| " << escape_cell(row.value("family", std::string())) << " | "
           << escape_cell(row.value("camera", std::string())) << " | "
           << escape_cell(row.value("product", std::string())) << " | "
           << escape_cell(row.value("mode_label", std::string())) << " | " << ids << " | "
           << state_name(row.value("proposed_state", std::string())) << " | "
           << escape_cell(row.value("evidence", std::string())) << " | "
           << escape_cell(limits.empty() ? "none" : limits) << " |\n";
    }

    md << "\n## Mode keys\n\n";
    md << "The mode key (`mk1`) names the exact mode as the decoder sees it: make, model, container, "
          "compression, "
          "visible area, bit depth, representation, sample type, view layout and subimage (ADR 0016).\n\n";
    md << "| Fixture | Mode key |\n|---|---|\n";
    for (const OrderedJson& f : report.at("fixtures")) {
        md << "| `" << f.at("id").get<std::string>() << "` | `"
           << (f.at("mode_key").is_object() ? f.at("mode_key").at("text").get<std::string>()
                                            : std::string("(not identified)"))
           << "` |\n";
    }

    md << "\n## Camera image area\n\n";
    md << "The image area each file declares (decoded-RAW contract 1.4, ADR 0017) and what the default "
          "develop render (geometry version 2) outputs. *Declared* is in raw-frame pixels as the source "
          "states it (DNG: ActiveArea origin plus DefaultCrop; other files: the decoder's reading of the "
          "camera's crop tags); *Frame* is where it was applied in the upright image; files that declare "
          "no area the decoder reads keep the decoder's visible area.\n\n";
    md << "| Fixture | Source | Declared | Frame (upright) | Output |\n|---|---|---|---|---|\n";
    for (const OrderedJson& f : report.at("fixtures")) {
        const OrderedJson area = f.contains("camera_area") ? f.at("camera_area") : OrderedJson(nullptr);
        const auto rect_text = [](const OrderedJson& r) -> std::string {
            if (!r.is_object()) {
                return "-";
            }
            const auto num = [](const OrderedJson& v) {
                std::ostringstream o;
                o << v.get<double>();
                return o.str();
            };
            return num(r.at("width")) + "x" + num(r.at("height")) + " at (" + num(r.at("x")) + ", " +
                   num(r.at("y")) + ")";
        };
        // The frame and the output only for a fixture the develop render ran on.
        std::string frame = "-";
        std::string output = "-";
        if (area.is_object() && area.at("output").is_array()) {
            frame =
                area.at("applied").is_object() ? rect_text(area.at("applied")) : std::string("whole image");
            output = std::to_string(area.at("output").at(0).get<std::int64_t>()) + "x" +
                     std::to_string(area.at("output").at(1).get<std::int64_t>());
        }
        md << "| `" << f.at("id").get<std::string>() << "` | "
           << (area.is_object() ? escape_cell(area.value("source", std::string())) : std::string("-"))
           << " | " << (area.is_object() ? rect_text(area.at("declared")) : std::string("-")) << " | "
           << frame << " | " << output << " |\n";
    }

    md << "\n## Reasons, limitations and evidence gaps\n\n";
    for (const OrderedJson& f : report.at("fixtures")) {
        md << "### `" << f.at("id").get<std::string>()
           << "`: " << state_name(f.value("proposed_state", std::string())) << "\n\n";
        if (!f.value("product", std::string()).empty()) {
            md << f.value("product", std::string()) << ", " << f.value("mode_label", std::string())
               << ".\n\n";
        }
        for (const OrderedJson& r : f.at("state_reasons")) {
            md << "- " << escape_cell(r.get<std::string>()) << "\n";
        }
        if (!f.at("evidence_gaps").empty()) {
            md << "- Evidence gaps:\n";
            for (const OrderedJson& g : f.at("evidence_gaps")) {
                md << "  - " << escape_cell(g.get<std::string>()) << "\n";
            }
        }
        if (f.contains("expectation_mismatches") && !f.at("expectation_mismatches").empty()) {
            md << "- **Differs from its expectation:**\n";
            for (const OrderedJson& g : f.at("expectation_mismatches")) {
                md << "  - " << escape_cell(g.get<std::string>()) << "\n";
            }
        }
        md << "\n";
    }

    md << "## Coverage against the blueprint's qualification families\n\n";
    md << "Targets from blueprint section 3 (\"Support files and capture modes, not just brand names\"), as "
          "listed in "
          "`fixtures/qualification/families.json`. A target is *absent* when no corpus fixture matches "
          "it.\n\n";
    for (const OrderedJson& family : report.at("coverage").at("families")) {
        md << "### " << family.value("name", std::string()) << "\n\n";
        md << "Blueprint targets: " << family.value("blueprint_targets", std::string())
           << " Required distinctions: " << family.value("required_distinctions", std::string()) << "\n\n";
        md << "| Target | Coverage | Fixtures (proposed state) | Note |\n|---|---|---|---|\n";
        for (const OrderedJson& t : family.at("targets")) {
            std::string ids;
            for (const OrderedJson& m : t.at("fixtures")) {
                ids += (ids.empty() ? "" : ", ") + std::string("`") + m.at("id").get<std::string>() + "` (" +
                       state_name(m.at("state").get<std::string>()) + ")";
            }
            const std::string status = t.value("status", std::string());
            md << "| " << escape_cell(t.value("description", std::string())) << " | "
               << (status == "absent"               ? "**absent**"
                   : status == "verified"           ? "verified"
                   : status == "partially_verified" ? "partly verified"
                                                    : "covered, not verified")
               << " | " << (ids.empty() ? "none" : ids) << " | "
               << escape_cell(t.value("note", std::string())) << " |\n";
        }
        md << "\n";
    }
    md << "### Corpus gaps recorded in `fixtures/manifest.json`\n\n| Target | Status | Detail "
          "|\n|---|---|---|\n";
    for (const OrderedJson& g : report.at("coverage").at("corpus_gaps")) {
        md << "| " << escape_cell(g.value("target", std::string())) << " | "
           << escape_cell(g.value("status", std::string())) << " | "
           << escape_cell(g.value("detail", std::string())) << " |\n";
    }

    md << "\n## Measurements\n\n";
    md << "Wall time per fixture for the whole suite (probe, decode, W2 render, two develop renders, exports "
          "and "
          "read-back) and the fixture process's peak memory"
       << (report.at("selection").value("isolated_processes", false)
               ? " (each fixture ran in its own process)"
               : "")
       << ". Engine estimates cover Lucent's own pixel buffers only (ADR 0006).\n\n";
    md << "| Fixture | Total s | Decode s | W2 render s | Develop s | Export s | Peak working set MB | Peak "
          "commit MB | "
          "Develop estimate MB |\n|---|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const OrderedJson& f : report.at("fixtures")) {
        const OrderedJson& m = f.at("measurements");
        const OrderedJson& phases = m.at("phase_seconds");
        const auto phase = [&](const char* name) {
            return phases.contains(name) ? seconds_text(phases.at(name)) : "";
        };
        md << "| `" << f.at("id").get<std::string>() << "` | " << seconds_text(m.at("total_seconds")) << " | "
           << phase("decode") << " | " << phase("w2_render") << " | " << phase("develop_1") << " | "
           << phase("export") << " | " << megabytes(m.at("peak_working_set_bytes")) << " | "
           << megabytes(m.at("peak_commit_bytes")) << " | "
           << megabytes(m.at("develop_export_estimate_bytes")) << " |\n";
    }
    return md.str();
}

}  // namespace lucent::qualify
