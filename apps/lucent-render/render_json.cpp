#include "render_json.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <lucent/application/recipe.hpp>
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

Json rect_json(const image::PixelRect& rect) {
    return Json{{"x", rect.x}, {"y", rect.y}, {"width", rect.width}, {"height", rect.height}};
}

Json chroma_denoise_json(const std::optional<engine::ChromaDenoiseRecord>& r) {
    if (!r) {
        return nullptr;
    }
    return Json{{"version", r->version},
                {"strength", number(r->strength)},
                {"radius", r->radius},
                {"epsilon", number(r->epsilon)},
                {"band_rows", r->band_rows},
                {"halo_rows", r->halo_rows},
                {"bands", r->bands},
                {"working_bytes", r->working_bytes}};
}

Json local_adjustments_json(const std::optional<engine::LocalAdjustmentsRecord>& r) {
    if (!r) {
        return nullptr;
    }
    return Json{{"version", r->version},
                {"masks", r->masks},
                {"rasterized", r->rasterized},
                {"cache_hits", r->cache_hits},
                {"raster_bytes", r->raster_bytes},
                {"tone_radius", r->tone_radius},
                {"tone_region", rect_json(r->tone_region)},
                {"changed_pixels", r->changed_pixels}};
}

Json geometry_json(const std::optional<engine::GeometryRecord>& r) {
    if (!r) {
        return nullptr;
    }
    return Json{{"version", r->version},
                {"straighten_degrees", number(r->straighten_degrees)},
                {"crop", rect_json(r->crop)},
                {"orientation", std::string(image::to_string(r->orientation))},
                {"orientation_exif", image::to_exif(r->orientation)},
                {"input_size", size_json(r->input_size)},
                {"output_size", size_json(r->output_size)},
                {"resampled", r->resampled},
                {"uncovered_pixels", r->uncovered_pixels}};
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

Json neutral_v0_json(const engine::NeutralV0Record& n) {
    return Json{{"version", n.version},
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
                {"apply_orientation", s.apply_orientation},
                {"apply_dng_opcodes", s.apply_dng_opcodes}};
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
    return Json{{"pipeline_version", r.pipeline_version},
                {"settings", settings_json(r.settings)},
                {"stages", std::move(stages)},
                {"interpretation", Json{{"status", std::string(raw::to_string(r.interpretation.status))},
                                        {"issues", std::move(issues)}}},
                {"warnings", r.warnings},
                {"memory", std::move(memory)},
                {"color", color_json(r.color)},
                {"working_space_matrix", matrix_json(r.working_space_matrix)},
                {"rendering", neutral_v0_json(r.rendering)},
                {"orientation", std::string(image::to_string(r.orientation))},
                {"orientation_exif", image::to_exif(r.orientation)},
                {"output_size", size_json(r.output_size)},
                {"total_seconds", number(r.total_seconds)}};
}

Json color_json(const engine::CameraColorRecord& c) {
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
    return color;
}

Json develop_report_json(const engine::DevelopReport& r) {
    Json settings = nullptr;
    if (auto document = app::write_recipe(r.settings)) {
        settings = Json::parse(document->json);
    }
    Json operations = Json::array();
    for (const engine::OperationRecord& op : r.operations) {
        Json parameters = Json::object();
        for (const engine::StageParameter& p : op.parameters) {
            parameters[p.name] = parameter_value(p.value);
        }
        operations.push_back(Json{{"operation", std::string(engine::to_string(op.operation))},
                                  {"algorithm", op.algorithm},
                                  {"version", op.version},
                                  {"applied", op.applied},
                                  {"parameters", std::move(parameters)},
                                  {"seconds", number(op.seconds)}});
    }
    Json stage_one_stages = Json::array();
    for (const engine::StageRecord& s : r.stage_one_stages) {
        stage_one_stages.push_back(stage_json(s));
    }
    const engine::WhiteBalanceRecord& wb = r.stage_one_white_balance;
    Json white_balance{
        {"target_multipliers", numbers(r.target_multipliers, 3)},
        {"sample_multipliers", numbers(r.sample_multipliers, 3)},
        {"correction", numbers(r.correction, 3)},
        {"target_temperature", r.target_temperature
                                   ? Json{{"temperature_k", number(r.target_temperature->temperature_k)},
                                          {"tint", number(r.target_temperature->tint)}}
                                   : Json(nullptr)},
        {"clip_level", number(r.clip_level)}};
    Json local_tone = nullptr;
    if (r.local_tone) {
        local_tone = Json{{"version", r.local_tone->version},
                          {"radius", r.local_tone->radius},
                          {"epsilon", number(r.local_tone->epsilon)},
                          {"min_gain_ev", number(r.local_tone->min_gain_ev)},
                          {"max_gain_ev", number(r.local_tone->max_gain_ev)}};
    }
    const engine::BaseLookRecord& look = r.base_look;
    Json base_look{{"id", std::string(look.id)},
                   {"version", look.version},
                   {"white", number(look.white)},
                   {"regime", std::string(engine::to_string(look.regime))},
                   {"top_start_ev", number(look.top_start_ev)},
                   {"end_slope", number(look.end_slope)},
                   {"toe_ev", Json::array({number(look.toe_start_ev), number(look.toe_end_ev)})},
                   {"limited_pixels", look.limited_pixels},
                   {"negative_samples", look.negative_samples}};
    Json downscale = nullptr;
    if (r.downscale) {
        downscale = Json{{"version", r.downscale->version},
                         {"from", size_json(r.downscale->from)},
                         {"to", size_json(r.downscale->to)},
                         {"seconds", number(r.downscale->seconds)}};
    }
    Json stage_one{{"kind", std::string(engine::to_string(r.stage_one_kind))},
                   {"stages", std::move(stage_one_stages)},
                   {"white_balance", Json{{"source", std::string(engine::to_string(wb.applied.source))},
                                          {"multipliers", numbers(wb.applied.multipliers, 4)},
                                          {"highlights", std::string(engine::to_string(wb.highlights))},
                                          {"clip_level", number(static_cast<double>(wb.clip_level))},
                                          {"clipped_samples", wb.clipped_samples},
                                          {"basis", wb.applied.basis}}},
                   {"demosaic", Json{{"route", std::string(engine::to_string(r.demosaic.route))},
                                     {"algorithm", std::string(r.demosaic.algorithm)},
                                     {"algorithm_version", r.demosaic.algorithm_version}}},
                   {"orientation", std::string(image::to_string(r.orientation))},
                   {"orientation_exif", image::to_exif(r.orientation)},
                   {"downscale", std::move(downscale)},
                   {"seconds", number(r.stage_one_seconds)}};
    return Json{{"pipeline_version", r.pipeline_version},
                {"settings", std::move(settings)},
                {"full_size", size_json(r.full_size)},
                {"upright_size", size_json(r.upright_size)},
                {"output_size", size_json(r.output_size)},
                {"scale", number(r.scale)},
                {"stage_one", std::move(stage_one)},
                {"white_balance", std::move(white_balance)},
                {"color", color_json(r.color)},
                {"working_space_matrix", matrix_json(r.working_space_matrix)},
                {"baseline_exposure_ev", number(r.baseline_exposure_ev)},
                {"gain", number(r.gain)},
                {"scene_white", number(r.scene_white)},
                {"white", number(r.white)},
                {"chroma_denoise", chroma_denoise_json(r.chroma_denoise)},
                {"local_tone", std::move(local_tone)},
                {"local_adjustments", local_adjustments_json(r.local_adjustments)},
                {"base_look", std::move(base_look)},
                {"geometry", geometry_json(r.geometry)},
                {"operations", std::move(operations)},
                {"warnings", r.warnings},
                {"stage_two_seconds", number(r.stage_two_seconds)}};
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
                {"pipeline_version", engine::kRenderPipelineVersion},
                {"develop_pipeline_version", engine::kDevelopPipelineVersion},
                {"recipe_schema", std::string(app::kRecipeSchema)},
                {"recipe_schema_version", app::kRecipeSchemaVersion}};
}

}  // namespace lucent::render_tool
