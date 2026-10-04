#pragma once

// JSON documents written by lucent-render (developer tool; not a module API):
//
//   lucent.render-result (schema_version 2): one line on standard output per run,
//     {"schema", "schema_version", "status": "ok"|"error", "pipeline": "develop"|"neutral-v0",
//      "output", "format", "profile", "width", "height", "sha256", "settings_sha256", "seconds",
//      "warnings", "error"}. settings_sha256 is the recipe identity (develop pipeline; null for
//      neutral-v0).
//   lucent.render-report (schema_version 2): the --report file, holding the generator, source,
//     "pipeline", the recipe ("recipe": schema, version, sha256 of the canonical document, the
//     document itself and the --develop file it came from; null for neutral-v0), the decoded
//     metadata (lucent-probe's "metadata" object), the render record ("render": the develop
//     pipeline's DevelopReport, develop_report_json, or the W2 RenderReport for
//     --legacy-neutral-v0), the export receipt ("export"), the --dump-linear file ("dump_linear"),
//     process memory ("process") and "status"/"error".
//   Version 1 (W2) had no "pipeline" and "recipe" and always held a RenderReport.
//
// Numbers: finite doubles are written as JSON numbers; non-finite values as the strings "nan",
// "inf", "-inf" (as lucent-probe does). Enumerations use the modules' stable to_string() names.

#include <string>

#include <nlohmann/json.hpp>

#include <lucent/core/error.hpp>
#include <lucent/engine/camera_color.hpp>
#include <lucent/engine/develop.hpp>
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
[[nodiscard]] Json receipt_json(const exporter::ExportReceipt& receipt);

// Build identity of this tool ("name", "version", "git_commit", "compiler", "build_type") and the
// contract versions it renders with.
[[nodiscard]] Json generator_json();

}  // namespace lucent::render_tool
