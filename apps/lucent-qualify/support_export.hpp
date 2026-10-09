#pragma once

// The camera support database exported from a qualification report (W10 phase 3, ADR 0018).
//
//   lucent-qualify --from-report docs/qualification/qualification-report.json
//                  --export-support assets/camera-support --asset-version 1.0.0
//
// writes the asset folder: camera-support.json ("lucent.camera-support" v2,
// lucent/assets/camera_support.hpp) and manifest.json ("lucent.asset-manifest" v1). The export is a
// pure function of the report and the options: the same report gives byte-identical files, which
// tests/qualification checks against the committed asset.
//
// What is taken from the report:
//   entries        one per support-table row (one per mode key; its state is the worst of its
//                  fixtures, ADR 0016): match = the row's lookup key (the key of its fixtures'
//                  probes, which the application derives at import; exactly one, and no two rows
//                  share one), qualified_key = the row's mode key (the decoded key), image_area and
//                  dng_opcodes = the row's lookup variant (its fixtures' probe facts; exactly one),
//                  state, camera = the product name, mode = the mode label, family, limitations (codes,
//                  each with the qualification's detail from the row's first fixture that reports it),
//                  evidence, evidence gaps and fixtures;
//   qualification  generated (and its date), the tool and its commit, the report's path, the basis
//                  (decoder, decoded-RAW contract and the versions block, including the decoder's
//                  capabilities, the develop-engine, neutral-look and export versions), and the counts;
//   limitations    the user-facing summary and meaning of every code the entries use
//                  (limitation_text(); an unknown code fails the export: it needs wording first).
// Refused (InvalidArgument, "qualify.export"): another schema, a run that is not the complete
// corpus (selection tier "all"), a run with expectation mismatches or harness errors (the reviewed
// expectations are what the database may claim), a row the harness could not qualify, or a report
// missing a member the export needs. The command line adds two refusals (main.cpp): a report made by
// a build of uncommitted changes (its commit ends in "-dirty") or of an unknown commit, unless
// --allow-dirty-build is given (QUA-06); and writing content that differs from an existing asset
// folder of the same version (AST-02: any content change needs a new --asset-version).

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
    core::SemanticVersion version;  // the asset's version
};

// The user-facing words of a limitation code (assets::standard_limitation_text()); std::nullopt for a
// code without wording.
[[nodiscard]] std::optional<assets::LimitationText> limitation_text(std::string_view code);

// The database of a "lucent.qualification-report" v1 document (see the file comment).
[[nodiscard]] Expected<assets::CameraSupportDatabase> camera_support_from_report(
    const OrderedJson& report, std::string_view report_path);

// The database and its manifest, as canonical text.
[[nodiscard]] Expected<SupportExport> export_camera_support(const OrderedJson& report,
                                                            const SupportExportOptions& options);

// QUA-06: the report was made by a build of a known commit without uncommitted changes (its
// tool.git_commit is a bare hexadecimal commit, not "unknown", "<commit>-dirty" or
// "<commit>-unknown-state"), unless `allow_dirty` (InvalidArgument, "qualify.export.provenance").
[[nodiscard]] Status check_report_provenance(const OrderedJson& report, bool allow_dirty);

// AST-02: an asset version names one content. `existing_manifest` and `existing_database` are the
// texts already in the target folder (empty when absent). InvalidArgument ("qualify.export.version")
// when the folder holds a manifest of `exported`'s version whose files differ from the export: any
// change of the content needs a new version, which the application's stored assessments notice.
[[nodiscard]] Status check_version_change(std::string_view existing_manifest, std::string_view existing_database,
                                          const SupportExport& exported);

}  // namespace lucent::qualify
