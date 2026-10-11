#pragma once

// JSON documents written by lucent-render (developer tool; not a module API):
//
//   lucent.render-result (schema_version 2): one line on standard output per run,
//     {"schema", "schema_version", "status": "ok"|"error", "pipeline": "develop"|"neutral-v0",
//      "output", "format", "profile", "width", "height", "sha256", "settings_sha256", "seconds",
//      "warnings", "error"}. settings_sha256 is the recipe identity (develop pipeline; null for
//      neutral-v0).
//   lucent.render-report (schema_version 2): the --report file, holding the generator, source,
//     "pipeline", the recipe ("recipe": schema, version (of the canonical document: always the
//     current recipe version), sha256 of the canonical document, the document itself, the
//     --develop file it came from and that file's own version ("file_version": 1, 2 or 3; an
//     older file is upgraded on read, recipe.hpp); null for neutral-v0), the decoded
//     metadata (lucent-probe's "metadata" object), the render record ("render": the develop
//     pipeline's DevelopReport, develop_report_json, or the W2 RenderReport for
//     --legacy-neutral-v0), the export receipt ("export"), the --dump-linear file ("dump_linear"),
//     process memory ("process") and "status"/"error".
//   Version 1 (W2) had no "pipeline" and "recipe" and always held a RenderReport. W9 (ADR 0015)
//     added, within version 2, the develop report's "upright_size" (the stage-one image, before
//     the geometry; "output_size" is after it) and its "chroma_denoise", "local_adjustments" and
//     "geometry" records (null when the operation was not applied). W10 (ADR 0017) added, within
//     version 2, the camera's image area: the develop report's "image_area" (the stage one's
//     placement, full-resolution upright pixels; null when the file declares none),
//     "geometry_version" and "geometry_frame" (the frame the geometry worked in), the geometry
//     record's "frame", and the RenderReport's "image_area" (stored orientation),
//     "camera_area_applied", settings "apply_camera_area" and memory "opcode_peak" /
//     "camera_area_peak". W13 (ADR 0023) added, within version 2, the lens-profile corrections: the
//     report's "lens_database" (the database the develop pipeline corrected lenses with: "status"
//     "active", "unavailable" (with its "problem") or "disabled" (--no-lens-corrections), its
//     "identity" (asset id, version, manifest SHA-256), the Lensfun library and the counts; null for
//     --legacy-neutral-v0), the develop report's "lens_correction" record (lens_correction_json: the
//     plan's operation version, setting, database identity, the file's query, the match with its
//     kind, candidates and reasons, the profile used, the camera, the crop factor and its basis, what
//     the file carries, every correction's decision, the geometry and vignetting models, the plan's
//     revision, and what the render did), and the generator's "lens_profile_match_version" and
//     "lensfun_data_format". The recipe's "file_version" may be 3.
//
// Numbers: finite doubles are written as JSON numbers; non-finite values as the strings "nan",
// "inf", "-inf" (as lucent-probe does). Enumerations use the modules' stable to_string() names.

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include <lucent/core/error.hpp>
#include <lucent/engine/camera_color.hpp>
#include <lucent/engine/develop.hpp>
#include <lucent/engine/lens_correction.hpp>
#include <lucent/engine/render.hpp>
#include <lucent/export/export.hpp>
#include <lucent/image/color_encoding.hpp>

namespace lucent::render_tool {

using Json = nlohmann::ordered_json;

inline constexpr const char* kResultSchema = "lucent.render-result";
inline constexpr const char* kReportSchema = "lucent.render-report";
inline constexpr int kSchemaVersion = 2;

// "pipeline" values.
inline constexpr const char* kDevelopPipeline = "develop";
inline constexpr const char* kLegacyPipeline = "neutral-v0";

[[nodiscard]] Json number(double value);
[[nodiscard]] Json error_json(const Error& error);
[[nodiscard]] Json encoding_json(const image::ColorEncoding& encoding);
[[nodiscard]] Json matrix_json(const image::Matrix3d& matrix);
[[nodiscard]] Json settings_json(const engine::RenderSettings& settings);
[[nodiscard]] Json stage_json(const engine::StageRecord& stage);
[[nodiscard]] Json color_json(const engine::CameraColorRecord& color);
[[nodiscard]] Json render_report_json(const engine::RenderReport& report);
// The develop pipeline's record (stage one and stage two). "settings" is the canonical recipe
// document parsed as JSON (or null when the settings cannot be written).
[[nodiscard]] Json develop_report_json(const engine::DevelopReport& report);
// The lens-profile correction of a develop render (DevelopReport::lens_correction).
[[nodiscard]] Json lens_correction_json(const engine::LensCorrectionRecord& record);
// The report's "lens_database": `database` when one is active; otherwise why not (`problem`, or
// `disabled` by --no-lens-corrections).
[[nodiscard]] Json lens_database_json(const engine::LensDatabase* database,
                                      const std::optional<Error>& problem, bool disabled);
[[nodiscard]] Json receipt_json(const exporter::ExportReceipt& receipt);

// Build identity of this tool ("name", "version", "git_commit", "compiler", "build_type") and the
// contract versions it renders with.
[[nodiscard]] Json generator_json();

}  // namespace lucent::render_tool
