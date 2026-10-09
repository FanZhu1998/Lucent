#include "support_export.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <utility>

#include <lucent/assets/asset_manifest.hpp>

#include "expectations.hpp"
#include "report.hpp"

namespace lucent::qualify {

namespace {

constexpr std::string_view kWhere = "qualify.export";
constexpr std::string_view kDecoderMarker = "; decoded-RAW ";

struct ExportError {
    std::string message;
};

[[noreturn]] void refuse(std::string message) {
    throw ExportError{std::move(message)};
}

const OrderedJson& at(const OrderedJson& object, std::string_view key, std::string_view what) {
    if (!object.is_object() || !object.contains(key)) {
        refuse(std::format("the report has no {}.{}", what, key));
    }
    return object.at(key);
}

std::string text(const OrderedJson& object, std::string_view key, std::string_view what) {
    const OrderedJson& value = at(object, key, what);
    if (!value.is_string()) {
        refuse(std::format("the report's {}.{} is not a string", what, key));
    }
    return value.get<std::string>();
}

std::uint32_t number(const OrderedJson& object, std::string_view key, std::string_view what) {
    const OrderedJson& value = at(object, key, what);
    if (!value.is_number_unsigned()) {
        refuse(std::format("the report's {}.{} is not a non-negative integer", what, key));
    }
    return value.get<std::uint32_t>();
}

std::vector<std::string> strings(const OrderedJson& object, std::string_view key, std::string_view what) {
    std::vector<std::string> out;
    const OrderedJson& value = at(object, key, what);
    if (!value.is_array()) {
        refuse(std::format("the report's {}.{} is not an array", what, key));
    }
    for (const OrderedJson& v : value) {
        if (!v.is_string()) {
            refuse(std::format("the report's {}.{} holds a value that is not a string", what, key));
        }
        out.push_back(v.get<std::string>());
    }
    return out;
}

}  // namespace

std::optional<assets::LimitationText> limitation_text(std::string_view code) {
    // One wording for the database and the application's own limitations (AST-03).
    return assets::standard_limitation_text(code);
}

Expected<assets::CameraSupportDatabase> camera_support_from_report(const OrderedJson& report,
                                                                   std::string_view report_path) {
    try {
        if (!report.is_object() || report.value("schema", std::string()) != kReportSchema ||
            report.value("schema_version", 0) != kReportSchemaVersion) {
            refuse(std::format("not a {} v{} document", kReportSchema, kReportSchemaVersion));
        }
        const OrderedJson& selection = at(report, "selection", "report");
        if (text(selection, "tier", "selection") != "all") {
            refuse(
                "the database is exported from a complete qualification run (--tier all); this report is a "
                "partial run");
        }
        const OrderedJson& summary = at(report, "summary", "report");
        if (number(summary, "expectation_mismatches", "summary") != 0 ||
            number(summary, "harness_errors", "summary") != 0) {
            refuse(
                "the report has expectation mismatches or harness errors: review the run and its "
                "expectations before exporting the database");
        }

        assets::CameraSupportDatabase database;
        database.generated = text(report, "generated", "report");
        database.date = database.generated.substr(0, 10);
        const OrderedJson& tool = at(report, "tool", "report");
        database.tool = std::format("{} {} ({})", text(tool, "name", "tool"),
                                    text(tool, "lucent_version", "tool"), text(tool, "git_commit", "tool"));
        database.report = std::string(report_path);

        const std::string decoder = text(report, "decoder", "report");
        const std::size_t marker = decoder.find(kDecoderMarker);
        if (marker == std::string::npos) {
            refuse(std::format("the report's decoder '{}' does not name the decoded-RAW contract", decoder));
        }
        const OrderedJson& versions = at(report, "versions", "report");
        assets::QualificationBasis& basis = database.basis;
        basis.decoder = decoder.substr(0, marker);
        basis.decoded_raw_contract = text(versions, "decoded_raw_contract", "versions");
        if (decoder.substr(marker + kDecoderMarker.size()) != basis.decoded_raw_contract) {
            refuse(std::format("the report's decoder '{}' and versions.decoded_raw_contract '{}' disagree",
                               decoder, basis.decoded_raw_contract));
        }
        basis.mode_key = number(versions, "mode_key", "versions");
        basis.check_suite = number(versions, "check_suite", "versions");
        basis.state_rules = number(versions, "state_rules", "versions");
        basis.render_pipeline = number(versions, "render_pipeline", "versions");
        basis.develop_pipeline = number(versions, "develop_pipeline", "versions");
        basis.dng_opcode_processing = number(versions, "dng_opcode_processing", "versions");
        basis.geometry_operation = number(versions, "geometry_operation", "versions");
        basis.image_area_placement = number(versions, "image_area_placement", "versions");
        // QUA-02: what else a Verified state certifies.
        basis.decoder_capabilities = strings(versions, "decoder_capabilities", "versions");
        const OrderedJson& engine_versions = at(versions, "develop_engine", "versions");
        if (!engine_versions.is_array() || engine_versions.empty()) {
            refuse("the report's versions.develop_engine is not a non-empty array");
        }
        for (const OrderedJson& v : engine_versions) {
            if (!v.is_number_unsigned()) {
                refuse("the report's versions.develop_engine holds a value that is not a version");
            }
            basis.develop_engine.push_back(v.get<std::uint32_t>());
        }
        basis.neutral_look = number(versions, "neutral_look", "versions");
        basis.export_version = number(versions, "export", "versions");

        // The qualification's detail of each limitation, per fixture.
        std::map<std::string, std::map<std::string, std::string>> details;  // fixture -> code -> detail
        const OrderedJson& fixtures = at(report, "fixtures", "report");
        if (!fixtures.is_array()) {
            refuse("the report's fixtures are not an array");
        }
        for (const OrderedJson& fixture : fixtures) {
            const std::string id = text(fixture, "id", "fixture");
            for (const OrderedJson& limitation : at(fixture, "limitations", "fixture")) {
                details[id].emplace(text(limitation, "id", "limitation"),
                                    text(limitation, "detail", "limitation"));
            }
        }

        std::set<std::string> used;
        std::set<std::string> matches;
        const OrderedJson& rows = at(report, "support_table", "report");
        if (!rows.is_array()) {
            refuse("the report's support_table is not an array");
        }
        for (const OrderedJson& row : rows) {
            assets::CameraSupportEntry entry;
            entry.qualified_key = text(row, "mode_key", "support_table row");
            // The application finds the mode under the key of its import probe (ADR 0018).
            const std::vector<std::string> lookup = strings(row, "lookup_keys", "support_table row");
            if (lookup.size() != 1) {
                refuse(std::format("the mode {} has {} probe keys; the database needs exactly one",
                                   entry.qualified_key, lookup.size()));
            }
            entry.match = lookup.front();
            if (!matches.insert(entry.match).second) {
                refuse(
                    std::format("two qualified modes share the probe key {}: the application could not tell "
                                "them apart",
                                entry.match));
            }
            // QUA-08: the probe facts beside the key; a Verified entry holds only for files that share
            // them, so they must be one set per mode.
            const OrderedJson& variants = at(row, "lookup_variants", "support_table row");
            if (!variants.is_array() || variants.size() != 1) {
                refuse(std::format("the mode {} has {} probe variants (image area and DNG opcodes); the database "
                                   "needs exactly one",
                                   entry.qualified_key, variants.is_array() ? variants.size() : 0));
            }
            entry.image_area = text(variants.at(0), "image_area", "lookup variant");
            entry.dng_opcodes = strings(variants.at(0), "dng_opcodes", "lookup variant");
            const std::string state = text(row, "proposed_state", "support_table row");
            const std::optional<assets::SupportState> parsed = assets::parse_support_state(state);
            if (!parsed) {
                refuse(std::format("the mode {} has the state '{}', which the database cannot record",
                                   entry.qualified_key, state));
            }
            entry.state = *parsed;
            entry.camera = text(row, "product", "support_table row");
            if (entry.camera.empty()) {
                entry.camera = text(row, "camera", "support_table row");
            }
            entry.mode = text(row, "mode_label", "support_table row");
            entry.family = text(row, "family", "support_table row");
            entry.evidence = text(row, "evidence", "support_table row");
            entry.evidence_gaps = strings(row, "evidence_gaps", "support_table row");
            entry.fixtures = strings(row, "fixtures", "support_table row");
            for (const std::string& code : strings(row, "limitations", "support_table row")) {
                if (!limitation_text(code)) {
                    refuse(
                        std::format("the limitation '{}' of {} has no user-facing wording (limitation_text)",
                                    code, entry.qualified_key));
                }
                assets::EntryLimitation limitation{code, {}};
                for (const std::string& fixture : entry.fixtures) {
                    const auto found = details.find(fixture);
                    if (found != details.end() && found->second.contains(code)) {
                        limitation.detail = found->second.at(code);
                        break;
                    }
                }
                used.insert(code);
                entry.limitations.push_back(std::move(limitation));
            }
            database.entries.push_back(std::move(entry));
        }
        for (const std::string& code : used) {
            database.limitations.push_back(*limitation_text(code));
        }
        database.summary = database.count();
        return database;
    } catch (const ExportError& e) {
        return fail(ErrorCode::InvalidArgument, e.message, std::string(kWhere));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "exporting the camera support database ran out of memory",
                    std::string(kWhere));
    } catch (const std::exception& e) {
        return fail(ErrorCode::InvalidArgument, std::format("the report is malformed: {}", e.what()),
                    std::string(kWhere));
    }
}

Expected<SupportExport> export_camera_support(const OrderedJson& report,
                                              const SupportExportOptions& options) {
    auto database = camera_support_from_report(report, options.report_path);
    if (!database) {
        return Unexpected(std::move(database.error()));
    }
    auto database_text = assets::write_camera_support(*database);
    if (!database_text) {
        return Unexpected(std::move(database_text.error()));
    }
    // The written text must read back as the same database: the reader is what Lucent.exe runs.
    auto reread = assets::parse_camera_support(*database_text);
    if (!reread) {
        return Unexpected(std::move(reread.error()));
    }
    assets::AssetManifest manifest;
    manifest.id = std::string(assets::kCameraSupportAssetId);
    manifest.version = options.asset_version;
    manifest.minimum_app_version = options.minimum_app_version;
    manifest.content = {std::string(assets::kCameraSupportSchema), assets::kCameraSupportSchemaVersion};
    auto file = assets::describe_asset_file(std::string(assets::kCameraSupportFileName), *database_text);
    if (!file) {
        return Unexpected(std::move(file.error()));
    }
    manifest.files.push_back(std::move(*file));
    auto manifest_text = assets::write_asset_manifest(manifest);
    if (!manifest_text) {
        return Unexpected(std::move(manifest_text.error()));
    }
    SupportExport out;
    out.database = std::move(*reread);
    out.database_text = std::move(*database_text);
    out.manifest_text = std::move(*manifest_text);
    out.version = manifest.version;
    return out;
}

Status check_report_provenance(const OrderedJson& report, bool allow_dirty) {
    std::string commit;
    if (report.is_object() && report.contains("tool") && report.at("tool").is_object()) {
        commit = report.at("tool").value("git_commit", std::string());
    }
    // A clean build records the bare abbreviated commit (cmake/LucentVersion.cmake); anything else
    // ("unknown", "<commit>-dirty", "<commit>-unknown-state") does not name what produced the evidence.
    const bool clean = !commit.empty() && std::all_of(commit.begin(), commit.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
    if (allow_dirty || clean) {
        return ok();
    }
    return fail(ErrorCode::InvalidArgument,
                std::format("the report was made by a build of {}: a published database must name the commit "
                            "that produced its evidence. Commit the changes, configure and build again, qualify "
                            "again and export, or pass --allow-dirty-build to export it as it is",
                            commit.empty() || commit == "unknown"
                                ? std::string("an unknown commit")
                                : "uncommitted changes or an unknown state (" + commit + ")"),
                "qualify.export.provenance");
}

Status check_version_change(std::string_view existing_manifest, std::string_view existing_database,
                            const SupportExport& exported) {
    if (existing_manifest.empty()) {
        return ok();
    }
    auto manifest = assets::parse_asset_manifest(existing_manifest);
    if (!manifest || manifest->version != exported.version) {
        return ok();  // another version (or not a manifest): replacing it is a version change
    }
    if (existing_manifest == exported.manifest_text && existing_database == exported.database_text) {
        return ok();
    }
    return fail(ErrorCode::InvalidArgument,
                std::format("the folder already holds camera-support {} with other content; every change of "
                            "the content needs a new --asset-version (ADR 0018), or Lucent would keep stored "
                            "assessments of the old content",
                            exported.version.to_string()),
                "qualify.export.version");
}

}  // namespace lucent::qualify
