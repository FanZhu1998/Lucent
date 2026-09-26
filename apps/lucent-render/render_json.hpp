#pragma once

// JSON documents written by lucent-render (developer tool; not a module API):
//
//   lucent.render-result (schema_version 1): one line on standard output per run,
//     {"schema", "schema_version", "status": "ok"|"error", "output", "format", "profile",
//      "width", "height", "sha256", "seconds", "warnings", "error"}.
//   lucent.render-report (schema_version 1): the --report file, holding the generator, source,
//     the decoded metadata (lucent-probe's "metadata" object), the engine's RenderReport
//     ("render"), the export receipt ("export"), the --dump-linear file ("dump_linear"), process
//     memory ("process") and "status"/"error".
//
// Numbers: finite doubles are written as JSON numbers; non-finite values as the strings "nan",
// "inf", "-inf" (as lucent-probe does). Enumerations use the modules' stable to_string() names.

#include <string>

#include <nlohmann/json.hpp>

#include <lucent/core/error.hpp>
#include <lucent/engine/render.hpp>
#include <lucent/export/export.hpp>
#include <lucent/image/color_encoding.hpp>

namespace lucent::render_tool {

using Json = nlohmann::ordered_json;

inline constexpr const char* kResultSchema = "lucent.render-result";
inline constexpr const char* kReportSchema = "lucent.render-report";
inline constexpr int kSchemaVersion = 1;

[[nodiscard]] Json number(double value);
[[nodiscard]] Json error_json(const Error& error);
[[nodiscard]] Json encoding_json(const image::ColorEncoding& encoding);
[[nodiscard]] Json matrix_json(const image::Matrix3d& matrix);
[[nodiscard]] Json settings_json(const engine::RenderSettings& settings);
[[nodiscard]] Json stage_json(const engine::StageRecord& stage);
[[nodiscard]] Json render_report_json(const engine::RenderReport& report);
[[nodiscard]] Json receipt_json(const exporter::ExportReceipt& receipt);

// Build identity of this tool ("name", "version", "git_commit", "compiler", "build_type").
[[nodiscard]] Json generator_json();

}  // namespace lucent::render_tool
