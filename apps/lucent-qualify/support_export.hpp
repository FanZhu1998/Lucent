#pragma once

// The camera support database exported from a qualification report (W10 phase 3, ADR 0018).
//
//   lucent-qualify --from-report docs/qualification/qualification-report.json
//                  --export-support assets/camera-support --asset-version 1.0.0
//
// writes the asset folder: camera-support.json ("lucent.camera-support" v1,
// lucent/assets/camera_support.hpp) and manifest.json ("lucent.asset-manifest" v1). The export is a
// pure function of the report and the options: the same report gives byte-identical files, which
// tests/qualification checks against the committed asset.
//
// What is taken from the report:
//   entries        one per support-table row (one per mode key; its state is the worst of its
//                  fixtures, ADR 0016): match = the row's lookup key (the key of its fixtures'
//                  probes, which the application derives at import; exactly one, and no two rows
//                  share one), qualified_key = the row's mode key (the decoded key), state,
//                  camera = the product name, mode = the mode label, family, limitations (codes, each with
//                  the qualification's detail from the row's first fixture that reports it), evidence,
//                  evidence gaps and fixtures;
//   qualification  generated (and its date), the tool and its commit, the report's path, the basis
//                  (decoder, decoded-RAW contract and the versions block), and the counts;
//   limitations    the user-facing summary and meaning of every code the entries use
//                  (limitation_text(); an unknown code fails the export: it needs wording first).
// Refused (InvalidArgument, "qualify.export"): another schema, a run that is not the complete
// corpus (selection tier "all"), a run with expectation mismatches or harness errors (the reviewed
// expectations are what the database may claim), a row the harness could not qualify, or a report
// missing a member the export needs.

#include <optional>
#include <string>
#include <string_view>

#include <lucent/assets/camera_support.hpp>
#include <lucent/core/error.hpp>
#include <lucent/core/version.hpp>

#include "qualification.hpp"

namespace lucent::qualify {

struct SupportExportOptions {
    core::SemanticVersion asset_version;
    core::SemanticVersion minimum_app_version;
    // Recorded as the database's "report": the report's path relative to the repository root,
    // with '/' separators.
    std::string report_path = "docs/qualification/qualification-report.json";
};

struct SupportExport {
    assets::CameraSupportDatabase database;
    std::string database_text;  // camera-support.json
    std::string manifest_text;  // manifest.json
};

// The user-facing words of a limitation code; std::nullopt for a code without wording.
[[nodiscard]] std::optional<assets::LimitationText> limitation_text(std::string_view code);

// The database of a "lucent.qualification-report" v1 document (see the file comment).
[[nodiscard]] Expected<assets::CameraSupportDatabase> camera_support_from_report(
    const OrderedJson& report, std::string_view report_path);

// The database and its manifest, as canonical text.
[[nodiscard]] Expected<SupportExport> export_camera_support(const OrderedJson& report,
                                                            const SupportExportOptions& options);

}  // namespace lucent::qualify
