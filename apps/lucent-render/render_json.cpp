#include "render_json.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include <lucent/core/hash.hpp>
#include <lucent/core/text.hpp>
#include <lucent/core/version.hpp>
#include <lucent/export/output_profile.hpp>
#include <lucent/image/geometry.hpp>

namespace lucent::render_tool {

namespace {

Json size_json(const image::Size& size) {
    return Json{{"width", size.width}, {"height", size.height}};
}

template <class Array>
Json numbers(const Array& values, std::size_t count) {
    Json out = Json::array();
    for (std::size_t i = 0; i < count && i < values.size(); ++i) {
        out.push_back(number(static_cast<double>(values[i])));
    }
    return out;
}

Json parameter_value(const engine::ParameterValue& value) {
    return std::visit(
        [](const auto& v) -> Json {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, double>) {
                return number(v);
            } else if constexpr (std::is_same_v<T, std::vector<double>>) {
                return numbers(v, v.size());
            } else {
                return Json(v);
            }
        },
        value);
}

}  // namespace

Json number(double value) {
    if (std::isnan(value)) {
        return "nan";
    }
    if (std::isinf(value)) {
        return value > 0 ? "inf" : "-inf";
    }
    return value;
}

Json error_json(const Error& error) {
    return Json{{"code", std::string(to_string(error.code))},
                {"where", error.where},
                {"message", core::sanitize_utf8(error.message)},
                {"native_code", error.native_code}};
}

Json encoding_json(const image::ColorEncoding& e) {
    Json j{{"text", e.to_string()},
           {"layout", std::string(image::to_string(e.layout))},
           {"primaries", std::string(image::to_string(e.primaries))},
           {"white_point", std::string(image::to_string(e.white_point))},
           {"transfer", std::string(image::to_string(e.transfer))},
           {"range", std::string(image::to_string(e.range))},
           {"reference", std::string(image::to_string(e.reference))}};
    return j;
}

Json matrix_json(const image::Matrix3d& m) {
    Json out = Json::array();
    for (std::size_t i = 0; i < 3; ++i) {
        out.push_back(Json::array({number(m(i, 0)), number(m(i, 1)), number(m(i, 2))}));
    }
    return out;
}

Json settings_json(const engine::RenderSettings& s) {
    return Json{{"schema", s.schema.to_string()},
                {"white_balance", static_cast<int>(s.white_balance)},
                {"custom_multipliers", numbers(s.custom_multipliers, 4)},
                {"highlights", std::string(engine::to_string(s.highlights))},
                {"exposure_ev", number(s.exposure_ev)},
                {"apply_baseline_exposure", s.apply_baseline_exposure},
                {"rendering", std::string(engine::to_string(s.rendering))},
                {"highlight_knee", number(s.highlight_knee)},
                {"apply_orientation", s.apply_orientation}};
}

Json stage_json(const engine::StageRecord& s) {
    Json parameters = Json::object();
    for (const engine::StageParameter& p : s.parameters) {
        parameters[p.name] = parameter_value(p.value);
    }
    return Json{{"stage", std::string(engine::to_string(s.stage))},
                {"algorithm", s.algorithm},
                {"version", s.version},
                {"provisional", s.provisional},
                {"input", s.input ? encoding_json(*s.input) : Json(nullptr)},
                {"output", encoding_json(s.output)},
                {"input_size", size_json(s.input_size)},
                {"output_size", size_json(s.output_size)},
                {"parameters", std::move(parameters)},
                {"seconds", number(s.seconds)},
                {"clipped_samples", s.clipped_samples}};
}

Json render_report_json(const engine::RenderReport& r) {
    Json stages = Json::array();
    for (const engine::StageRecord& s : r.stages) {
        stages.push_back(stage_json(s));
    }
    Json issues = Json::array();
    for (const raw::InterpretationIssue& issue : r.interpretation.issues) {
        issues.push_back(Json{{"kind", std::string(raw::to_string(issue.kind))}, {"message", issue.message}});
    }
    const engine::MemoryEstimate& m = r.memory;
    Json memory{
        {"raw_frame_bytes", m.raw_frame_bytes}, {"sensor_bytes", m.sensor_bytes},
        {"mask_bytes", m.mask_bytes},           {"rgb_bytes", m.rgb_bytes},
        {"decode_peak", m.decode_peak},         {"normalize_peak", m.normalize_peak},
        {"demosaic_peak", m.demosaic_peak},     {"color_peak", m.color_peak},
        {"rendering_peak", m.rendering_peak},   {"orientation_peak", m.orientation_peak},
        {"peak_bytes", m.peak_bytes},           {"peak_stage", std::string(engine::to_string(m.peak_stage))},
        {"budget_bytes", m.budget_bytes}};
    const engine::CameraColorRecord& c = r.color;
    Json color{{"version", c.version},
               {"path", std::string(engine::to_string(c.path))},
               {"provenance", std::string(raw::to_string(c.provenance))},
               {"camera_to_xyz", matrix_json(c.camera_to_xyz)},
               {"xyz_white", std::string(image::to_string(c.xyz_white))},
               {"multipliers", numbers(c.multipliers, 3)},
               {"neutral_scale", number(c.neutral_scale)},
               {"basis", c.basis},
               {"notes", c.notes}};
    if (c.dng) {
        const engine::DngCalibrationRecord& d = *c.dng;
        color["dng"] =
            Json{{"illuminants", Json::array({d.illuminants[0], d.illuminants[1]})},
                 {"temperatures", numbers(d.temperatures, 2)},
                 {"interpolated", d.interpolated},
                 {"forward_matrix", d.forward_matrix},
                 {"camera_calibration", d.camera_calibration},
                 {"analog_balance", d.analog_balance},
                 {"white_xy", Json::array({number(d.white_xy.x), number(d.white_xy.y)})},
                 {"white_cct_kelvin", number(d.white_cct_kelvin)},
                 {"weight_matrix1", number(d.weight_matrix1)},
                 {"iterations", d.iterations},
                 {"converged", d.converged},
                 {"camera_neutral", Json::array({number(d.camera_neutral[0]), number(d.camera_neutral[1]),
                                                 number(d.camera_neutral[2])})},
                 {"camera_to_xyz_d50", matrix_json(d.camera_to_xyz_d50)}};
    } else {
        color["dng"] = nullptr;
    }
    const engine::NeutralV0Record& n = r.rendering;
    Json rendering{{"version", n.version},
                   {"exposure_ev", number(n.parameters.exposure_ev)},
                   {"baseline_exposure_ev", number(n.parameters.baseline_exposure_ev)},
                   {"knee", number(n.parameters.knee)},
                   {"scene_white", number(n.parameters.scene_white)},
                   {"gain", number(n.gain)},
                   {"white", number(n.white)},
                   {"curve", std::string(engine::to_string(n.curve))},
                   {"exponent", number(n.exponent)},
                   {"lift", Json::array({number(n.lift[0]), number(n.lift[1]), number(n.lift[2])})},
                   {"compressed_pixels", n.compressed_pixels},
                   {"lifted_pixels", n.lifted_pixels},
                   {"limited_pixels", n.limited_pixels},
                   {"negative_samples", n.negative_samples}};
    return Json{{"pipeline_version", r.pipeline_version},
                {"settings", settings_json(r.settings)},
                {"stages", std::move(stages)},
                {"interpretation", Json{{"status", std::string(raw::to_string(r.interpretation.status))},
                                        {"issues", std::move(issues)}}},
                {"warnings", r.warnings},
                {"memory", std::move(memory)},
                {"color", std::move(color)},
                {"working_space_matrix", matrix_json(r.working_space_matrix)},
                {"rendering", std::move(rendering)},
                {"orientation", std::string(image::to_string(r.orientation))},
                {"orientation_exif", image::to_exif(r.orientation)},
                {"output_size", size_json(r.output_size)},
                {"total_seconds", number(r.total_seconds)}};
}

Json receipt_json(const exporter::ExportReceipt& receipt) {
    Json settings{{"format", std::string(exporter::to_string(receipt.settings.format))},
                  {"profile", std::string(exporter::to_string(receipt.settings.profile))},
                  {"intent", std::string(exporter::to_string(receipt.settings.transform.intent))},
                  {"black_point_compensation", receipt.settings.transform.black_point_compensation},
                  {"jpeg_quality", receipt.settings.jpeg.quality},
                  {"tiff_compression", std::string(exporter::to_string(receipt.settings.tiff.compression))}};
    return Json{{"path", core::path_to_utf8_lossy(receipt.path)},
                {"format", std::string(exporter::to_string(receipt.format))},
                {"profile", std::string(exporter::to_string(receipt.profile))},
                {"bit_depth", std::string(exporter::to_string(receipt.bit_depth))},
                {"width", receipt.width},
                {"height", receipt.height},
                {"byte_size", receipt.byte_size},
                {"sha256", core::to_hex(receipt.sha256)},
                {"icc_sha256", core::to_hex(receipt.icc_sha256)},
                {"chroma_subsampling", std::string(exporter::to_string(receipt.chroma_subsampling))},
                {"encoder", receipt.encoder},
                {"settings", std::move(settings)}};
}

Json generator_json() {
    const core::BuildInfo build = core::build_info();
    return Json{{"name", "lucent-render"},
                {"version", std::string(build.version)},
                {"git_commit", std::string(build.git_commit)},
                {"compiler", std::string(build.compiler)},
                {"build_type", std::string(build.build_type)},
                {"render_settings_schema", engine::kRenderSettingsVersion.to_string()},
                {"pipeline_version", engine::kRenderPipelineVersion}};
}

}  // namespace lucent::render_tool
