#pragma once

// The qualification report (schema "lucent.qualification-report" version 1), the support table
// and the coverage-gap list (ADR 0016).
//
// build_report() assembles per-fixture results (FixtureResult::to_json(), possibly produced in
// child processes) into one versioned document:
//   schema, schema_version, generated (UTC), tool (Lucent build), decoder, versions (mode key,
//   check suite, state rules, pipelines), inputs (paths and SHA-256 of the manifest,
//   expectations, render reference and families files), selection, summary (counts by state,
//   expectation mismatches, harness errors), fixtures (the per-fixture results), support_table
//   (one row per mode key: the worst state of its fixtures), coverage (the blueprint section 3
//   families with each target covered or absent, and the manifest's corpus gaps).
// support_table_markdown() renders docs/qualification/support-table.md from a report.

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <lucent/core/error.hpp>

#include "qualification.hpp"

namespace lucent::qualify {

inline constexpr std::string_view kReportSchema = "lucent.qualification-report";
inline constexpr int kReportSchemaVersion = 1;
inline constexpr std::string_view kFamiliesSchema = "lucent.qualification-families";
inline constexpr int kFamiliesSchemaVersion = 1;

// One target of a family: a fixture matches when every given criterion holds for its mode key.
struct TargetMatch {
    // A target without criteria ("match": null) is documented but cannot be matched yet (its mode
    // key is not known without a fixture); it always reports "absent".
    bool never = false;
    std::string make;
    std::vector<std::string> models;      // any of
    std::vector<std::string> models_not;  // none of
    std::vector<std::string> containers;  // any of
    std::string compression;
    std::string lossiness;
    std::string area;
    std::string representation_prefix;
    std::string view;
    std::uint32_t subimage_count_min = 0;
    std::uint32_t subimage_count_max = 0;  // 0: no upper bound
};

struct FamilyTarget {
    std::string id;
    std::string description;
    std::string note;
    TargetMatch match;
};

struct Family {
    std::string id;
    std::string name;
    std::string blueprint_targets;
    std::string required_distinctions;
    std::vector<FamilyTarget> targets;
};

// fixtures/qualification/families.json. VersionMismatch / InvalidArgument, where
// "qualify.families".
[[nodiscard]] Expected<std::vector<Family>> load_families(const std::filesystem::path& path);

// True when the mode key (FixtureResult::to_json()["mode_key"]) satisfies `match`.
[[nodiscard]] bool matches(const TargetMatch& match, const nlohmann::ordered_json& mode_key);

// Coverage of one family target by the fixtures matching it: "absent" (none), "verified" (every
// matching fixture is Verified), "partially_verified" (some are), "covered_not_verified" (none is).
[[nodiscard]] std::string coverage_status(const std::vector<std::string>& matching_states);

struct ReportInputs {
    CorpusPaths paths;
    std::string tier;              // "smoke", "qualification", "all" or "ids"
    std::vector<std::string> ids;  // the fixtures selected, in run order
    bool isolated = true;          // fixtures ran in child processes
};

// Severity order used when several fixtures share a mode key (worst first): not_qualified,
// unsupported, limited, compatible_unverified, verified.
[[nodiscard]] int state_severity(std::string_view state) noexcept;

[[nodiscard]] OrderedJson build_report(const std::vector<OrderedJson>& fixtures, const ReportInputs& inputs,
                                       const std::vector<Family>& families,
                                       const nlohmann::json& manifest_document);

[[nodiscard]] std::string support_table_markdown(const OrderedJson& report);

}  // namespace lucent::qualify
