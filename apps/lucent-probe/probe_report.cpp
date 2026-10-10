#include "probe_report.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <lucent/core/file.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/core/text.hpp>
#include <lucent/core/version.hpp>
#include <lucent/image/cfa.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/raw/libraw_decoder.hpp>

namespace lucent::probe {

namespace {

Json number(double value) {
    if (std::isnan(value)) {
        return "nan";
    }
    if (std::isinf(value)) {
        return value > 0 ? "inf" : "-inf";
    }
    return value;
}

template <std::size_t N>
Json float_list(const std::array<float, N>& values) {
    Json list = Json::array();
    for (const float v : values) {
        list.push_back(number(static_cast<double>(v)));
    }
    return list;
}

template <std::size_t N>
Json double_list(const std::array<double, N>& values) {
    Json list = Json::array();
    for (const double v : values) {
        list.push_back(number(v));
    }
    return list;
}

Json double_list(const std::vector<double>& values) {
    Json list = Json::array();
    for (const double v : values) {
        list.push_back(number(v));
    }
    return list;
}

template <std::size_t R, std::size_t C>
Json float_matrix(const std::array<std::array<float, C>, R>& rows) {
    Json matrix = Json::array();
    for (const auto& row : rows) {
        matrix.push_back(float_list(row));
    }
    return matrix;
}

template <class T>
Json optional_value(const std::optional<T>& value) {
    if (!value) {
        return nullptr;
    }
    if constexpr (std::is_floating_point_v<T>) {
        return number(static_cast<double>(*value));
    } else {
        return *value;
    }
}

Json rect_json(const image::PixelRect& rect) {
    return Json{{"x", rect.x}, {"y", rect.y}, {"width", rect.width}, {"height", rect.height}};
}

Json cfa_json(const image::CfaPattern& cfa) {
    if (cfa.empty()) {
        return nullptr;
    }
    Json rows = Json::array();
    for (std::int32_t r = 0; r < cfa.height(); ++r) {
        Json row = Json::array();
        for (std::int32_t c = 0; c < cfa.width(); ++c) {
            row.push_back(std::string(image::to_string(cfa.color_at(r, c))));
        }
        rows.push_back(std::move(row));
    }
    return Json{{"width", cfa.width()},
                {"height", cfa.height()},
                {"text", cfa.to_string()},
                {"colors", std::move(rows)}};
}

Json dng_matrix_json(const std::optional<raw::DngMatrix>& matrix) {
    if (!matrix) {
        return nullptr;
    }
    Json rows = Json::array();
    for (std::uint32_t r = 0; r < matrix->rows; ++r) {
        Json row = Json::array();
        for (std::uint32_t c = 0; c < matrix->cols; ++c) {
            row.push_back(number(matrix->at(r, c)));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

// BlackLevelDeltaH/V can hold one value per column or row; the report gives their extent only.
Json delta_summary(const std::vector<double>& deltas) {
    if (deltas.empty()) {
        return nullptr;
    }
    const auto [low, high] = std::minmax_element(deltas.begin(), deltas.end());
    return Json{{"count", deltas.size()}, {"min", number(*low)}, {"max", number(*high)}};
}

Json dng_json(const raw::DngMetadata& dng) {
    const raw::DngInfo& info = dng.info;
    const raw::TiffIfdSummary& ifd = dng.raw_ifd;
    Json color{
        {"calibration_illuminant", Json::array()},
        {"color_matrix", Json::array()},
        {"camera_calibration", Json::array()},
        {"forward_matrix", Json::array()},
        {"analog_balance", double_list(info.color.analog_balance)},
        {"as_shot_neutral", double_list(info.color.as_shot_neutral)},
        {"as_shot_white_xy",
         info.color.as_shot_white_xy ? double_list(*info.color.as_shot_white_xy) : Json(nullptr)},
        {"baseline_exposure", optional_value(info.color.baseline_exposure)},
    };
    for (std::size_t i = 0; i < 3; ++i) {
        color["calibration_illuminant"].push_back(optional_value(info.color.calibration_illuminant[i]));
        color["color_matrix"].push_back(dng_matrix_json(info.color.color_matrix[i]));
        color["camera_calibration"].push_back(dng_matrix_json(info.color.camera_calibration[i]));
        color["forward_matrix"].push_back(dng_matrix_json(info.color.forward_matrix[i]));
    }
    Json opcodes = Json::array();
    for (const raw::DngOpcode& op : ifd.opcodes) {
        opcodes.push_back(Json{{"list", static_cast<int>(op.list)},
                               {"index", op.index},
                               {"id", op.id},
                               {"name", std::string(op.name())},
                               {"dng_version", op.min_dng_version.to_string()},
                               {"flags", op.flags},
                               {"mandatory", op.is_mandatory()},
                               {"parameter_bytes", op.parameter_bytes}});
    }
    Json warnings = Json::array();
    for (const raw::TiffWarning& w : dng.warnings) {
        warnings.push_back(Json{{"where", w.where}, {"message", w.message}});
    }
    Json masked_areas = Json::array();
    for (const raw::DngRect& area : ifd.masked_areas) {
        masked_areas.push_back(
            Json{{"top", area.top}, {"left", area.left}, {"bottom", area.bottom}, {"right", area.right}});
    }
    return Json{
        {"version", info.version.to_string()},
        {"backward_version", info.backward_version.to_string()},
        {"backward_version_present", info.backward_version_present},
        {"unique_camera_model", optional_value(info.unique_camera_model)},
        {"color_planes", optional_value(info.color_planes)},
        {"raw_ifd", ifd.path},
        {"photometric", optional_value(ifd.photometric)},
        {"bits_per_sample", ifd.bits_per_sample},
        {"samples_per_pixel", optional_value(ifd.samples_per_pixel)},
        {"active_area", ifd.active_area ? Json{{"top", ifd.active_area->top},
                                               {"left", ifd.active_area->left},
                                               {"bottom", ifd.active_area->bottom},
                                               {"right", ifd.active_area->right}}
                                        : Json(nullptr)},
        {"default_crop_origin",
         ifd.default_crop_origin ? double_list(*ifd.default_crop_origin) : Json(nullptr)},
        {"default_crop_size", ifd.default_crop_size ? double_list(*ifd.default_crop_size) : Json(nullptr)},
        {"black_level_repeat_dim",
         ifd.black_level_repeat_dim ? Json(*ifd.black_level_repeat_dim) : Json(nullptr)},
        {"black_level", double_list(ifd.black_level)},
        {"black_level_delta_h", delta_summary(ifd.black_level_delta_h)},
        {"black_level_delta_v", delta_summary(ifd.black_level_delta_v)},
        {"masked_areas", std::move(masked_areas)},
        {"white_level", ifd.white_level},
        {"linearization_table", ifd.linearization_table.has_value()},
        {"opcodes", std::move(opcodes)},
        {"color", std::move(color)},
        {"warnings", std::move(warnings)},
    };
}

}  // namespace

Json generator_json() {
    const core::BuildInfo build = core::build_info();
    const raw::LibRawBuildInfo libraw = raw::libraw_build_info();
    return Json{
        {"name", std::string(kGeneratorName)},
        {"version", std::string(build.version)},
        {"git_commit", std::string(build.git_commit)},
        {"compiler", std::string(build.compiler)},
        {"build_type", std::string(build.build_type)},
        {"contract", raw::kDecodedRawContractVersion.to_string()},
        {"libraw", libraw.version},
        {"libraw_capabilities", libraw.capabilities},
    };
}

Json error_json(const Error& error) {
    return Json{{"code", std::string(to_string(error.code))},
                {"where", error.where},
                {"message", core::sanitize_utf8(error.message)},
                {"native_code", error.native_code}};
}

Json parity_json(const raw::ReferenceParity& p, const std::optional<std::string>& raw_data_sha256) {
    Json pattern = nullptr;
    if (p.raw_pattern) {
        pattern = Json::array();
        for (const auto& row : *p.raw_pattern) {
            pattern.push_back(row);
        }
    }
    Json parity{
        {"raw_width", p.raw_width},
        {"raw_height", p.raw_height},
        {"width", p.width},
        {"height", p.height},
        {"top_margin", p.top_margin},
        {"left_margin", p.left_margin},
        {"iwidth", p.iwidth},
        {"iheight", p.iheight},
        {"flip", p.flip},
        {"num_colors", p.num_colors},
        {"color_desc", p.color_desc},
        {"raw_pattern", std::move(pattern)},
        {"black_level_per_channel", p.black_level_per_channel},
        {"white_level", p.white_level},
        {"camera_white_level_per_channel",
         p.camera_white_level_per_channel ? Json(*p.camera_white_level_per_channel) : Json(nullptr)},
        {"camera_whitebalance", float_list(p.camera_whitebalance)},
        {"daylight_whitebalance", float_list(p.daylight_whitebalance)},
        {"rgb_xyz_matrix", float_matrix(p.rgb_xyz_matrix)},
        {"color_matrix", float_matrix(p.color_matrix)},
        {"raw_type", p.raw_type},
    };
    if (raw_data_sha256) {
        parity["raw_data_sha256"] = *raw_data_sha256;
    }
    return parity;
}

Json metadata_json(const raw::RawMetadata& m) {
    Json channel_colors = Json::array();
    for (const image::CfaColor color : m.representation.channel_colors) {
        channel_colors.push_back(std::string(image::to_string(color)));
    }
    Json prior = Json::array();
    for (const raw::PriorProcessingStep& step : m.prior_processing) {
        prior.push_back(Json{{"kind", std::string(raw::to_string(step.kind))}, {"detail", step.detail}});
    }
    Json operations = Json::array();
    for (const raw::RequiredOperation& op : m.required_operations) {
        operations.push_back(
            Json{{"kind", std::string(raw::to_string(op.kind))},
                 {"stage", std::string(raw::to_string(op.stage))},
                 {"name", op.name},
                 {"opcode_id", op.opcode_id},
                 {"index", op.index},
                 {"min_dng_version", op.min_dng_version.to_string()},
                 {"flags", op.flags},
                 {"mandatory", op.mandatory},
                 {"preview_skippable", op.preview_skippable},
                 {"follows_optional_warp_rectilinear2", op.follows_optional_warp_rectilinear2},
                 {"supported_by_lucent", op.supported_by_lucent},
                 {"applied_by_decoder", op.applied_by_decoder}});
    }
    Json issues = Json::array();
    for (const raw::InterpretationIssue& issue : m.interpretation.issues) {
        issues.push_back(Json{{"kind", std::string(raw::to_string(issue.kind))}, {"message", issue.message}});
    }
    Json noise = nullptr;
    if (m.noise) {
        Json planes = Json::array();
        for (const raw::NoiseModel& plane : m.noise->planes) {
            planes.push_back(Json{{"scale", number(plane.scale)}, {"offset", number(plane.offset)}});
        }
        noise = Json{{"provenance", std::string(raw::to_string(m.noise->provenance))},
                     {"source_ifd", m.noise->source_ifd},
                     {"planes", std::move(planes)}};
    }
    const std::string path_text = core::path_to_utf8_lossy(m.source.path);
    return Json{
        {"stage", std::string(raw::to_string(m.stage))},
        {"source", Json{{"path", path_text},
                        {"size_bytes", m.source.size_bytes},
                        {"identity", core::to_string(m.source.identity)}}},
        {"format",
         Json{{"container", std::string(raw::to_string(m.format.container))},
              {"extension_hint", m.format.extension_hint},
              {"dng_version", m.format.dng_version ? Json(m.format.dng_version->to_string()) : Json(nullptr)},
              {"compression",
               Json{{"tiff_code", optional_value(m.format.compression.tiff_code)},
                    {"name", m.format.compression.name},
                    {"lossiness", std::string(raw::to_string(m.format.compression.lossiness))},
                    {"provenance", std::string(raw::to_string(m.format.compression.provenance))},
                    {"basis", m.format.compression.basis}}}}},
        {"subimage", Json{{"index", m.subimage_index},
                          {"count", m.subimage_count},
                          {"role", std::string(raw::to_string(m.subimage_role))}}},
        {"projection", Json{{"layout", std::string(raw::to_string(m.projection.layout))},
                            {"provenance", std::string(raw::to_string(m.projection.provenance))},
                            {"basis", m.projection.basis}}},
        {"camera", Json{{"make", m.camera.make},
                        {"model", m.camera.model},
                        {"normalized_make", m.camera.normalized_make},
                        {"normalized_model", m.camera.normalized_model},
                        {"software", m.camera.software},
                        {"unique_camera_model", m.camera.unique_camera_model}}},
        {"lens",
         Json{{"make", m.lens.make},
              {"model", m.lens.model},
              {"min_focal_length_mm", optional_value(m.lens.min_focal_length_mm)},
              {"max_focal_length_mm", optional_value(m.lens.max_focal_length_mm)},
              {"max_aperture_at_min_focal", optional_value(m.lens.max_aperture_at_min_focal)},
              {"max_aperture_at_max_focal", optional_value(m.lens.max_aperture_at_max_focal)},
              {"focal_length_35mm_equivalent_mm", optional_value(m.lens.focal_length_35mm_equivalent_mm)}}},
        {"sensor_crop", m.sensor_crop ? Json{{"factor", optional_value(m.sensor_crop->factor)},
                                             {"provenance", std::string(raw::to_string(m.sensor_crop->provenance))},
                                             {"basis", m.sensor_crop->basis}}
                                      : Json(nullptr)},
        {"capture", Json{{"iso", optional_value(m.capture.iso)},
                         {"exposure_time_s", optional_value(m.capture.exposure_time_s)},
                         {"f_number", optional_value(m.capture.f_number)},
                         {"focal_length_mm", optional_value(m.capture.focal_length_mm)},
                         {"capture_time_local", optional_value(m.capture.capture_time_local)}}},
        {"geometry", Json{{"raw_width", m.geometry.raw_width},
                          {"raw_height", m.geometry.raw_height},
                          {"visible_area", rect_json(m.geometry.visible_area)},
                          {"pixel_aspect", number(m.geometry.pixel_aspect)},
                          {"orientation", std::string(image::to_string(m.geometry.orientation))},
                          {"orientation_exif", image::to_exif(m.geometry.orientation)}}},
        {"representation",
         Json{{"kind", std::string(raw::to_string(m.representation.kind))},
              {"sample_encoding", std::string(raw::to_string(m.representation.sample_encoding))},
              {"channels", m.representation.channels},
              {"color_count", m.representation.color_count},
              {"color_description", m.representation.color_description},
              {"channel_colors", std::move(channel_colors)},
              {"cfa_raw_origin", cfa_json(m.representation.cfa)},
              {"cfa_visible_origin", cfa_json(raw::cfa_at_visible_origin(m))}}},
        {"levels", Json{{"provenance", std::string(raw::to_string(m.levels.provenance))},
                        {"black", number(m.levels.black)},
                        {"channel_black", double_list(m.levels.channel_black)},
                        {"pattern_rows", m.levels.pattern_rows},
                        {"pattern_cols", m.levels.pattern_cols},
                        {"black_pattern", double_list(m.levels.black_pattern)},
                        {"white", number(m.levels.white)},
                        {"linear_max", double_list(m.levels.linear_max)}}},
        {"color", Json{{"provenance", std::string(raw::to_string(m.color.provenance))},
                       {"as_shot_multipliers", float_list(m.color.as_shot_multipliers)},
                       {"daylight_multipliers", float_list(m.color.daylight_multipliers)},
                       {"xyz_to_camera", float_matrix(m.color.xyz_to_camera)},
                       {"camera_to_srgb_from_file", float_matrix(m.color.camera_to_srgb_from_file)},
                       {"camera_to_srgb", float_matrix(m.color.camera_to_srgb)}}},
        {"noise_profile", std::move(noise)},
        {"prior_processing", std::move(prior)},
        {"required_operations", std::move(operations)},
        {"interpretation", Json{{"status", std::string(raw::to_string(m.interpretation.status))},
                                {"issues", std::move(issues)}}},
        {"dng", m.dng ? dng_json(*m.dng) : Json(nullptr)},
        {"decoder", Json{{"adapter_id", m.decoder.adapter_id},
                         {"library", m.decoder.library},
                         {"library_version", m.decoder.library_version},
                         {"capabilities", m.decoder.capabilities},
                         {"unpacker", m.decoder.unpacker},
                         {"unpacker_flags", m.decoder.unpacker_flags},
                         {"process_warnings", m.decoder.process_warnings},
                         {"warnings", m.decoder.warnings}}},
    };
}

Json subimages_json(const std::vector<raw::SubimageInfo>& subimages) {
    Json list = Json::array();
    for (const raw::SubimageInfo& s : subimages) {
        if (s.error) {
            list.push_back(Json{{"index", s.index}, {"error", error_json(*s.error)}});
            continue;
        }
        list.push_back(Json{{"index", s.index},
                            {"role", std::string(raw::to_string(s.role))},
                            {"interpretation", std::string(raw::to_string(s.interpretation))},
                            {"raw_width", s.raw_width},
                            {"raw_height", s.raw_height},
                            {"visible_area", rect_json(s.visible_area)},
                            {"representation", std::string(raw::to_string(s.representation))},
                            {"unpacker", s.unpacker}});
    }
    return list;
}

Expected<std::optional<std::string>> raw_data_sha256(const raw::DecodedRaw& decoded) {
    const raw::RawPixels* pixels = nullptr;
    if (const auto* mosaic = std::get_if<raw::MosaicFrame>(&decoded.image)) {
        pixels = &mosaic->pixels;
    } else if (const auto* mono = std::get_if<raw::MonochromeFrame>(&decoded.image)) {
        pixels = &mono->pixels;
    }
    if (pixels == nullptr) {
        return std::optional<std::string>{};
    }
    const auto* buffer = std::get_if<image::ImageBuffer<std::uint16_t>>(pixels);
    if (buffer == nullptr || buffer->channels() != 1) {
        return std::optional<std::string>{};
    }
    static_assert(std::endian::native == std::endian::little, "raw_data_sha256 hashes little-endian samples");
    core::Sha256 hasher;
    const std::size_t row_bytes = static_cast<std::size_t>(buffer->width()) * sizeof(std::uint16_t);
    for (std::int32_t y = 0; y < buffer->height(); ++y) {
        hasher.update(
            std::as_bytes(std::span<const std::uint16_t>(buffer->row(y), row_bytes / sizeof(std::uint16_t))));
    }
    auto digest = hasher.finish();
    if (!digest) {
        return Unexpected(std::move(digest.error()));
    }
    return std::optional<std::string>(core::to_hex(*digest));
}

Report build_report(const raw::DecoderRegistry& registry, const std::filesystem::path& path,
                    const ReportOptions& options, core::CancellationToken cancel) {
    Report report;
    Json& doc = report.document;
    doc["schema"] = std::string(kReportSchema);
    doc["schema_version"] = kReportSchemaVersion;
    doc["file_name"] = core::path_to_utf8_lossy(path.filename());
    doc["file_size"] = nullptr;
    doc["file_sha256"] = nullptr;  // removed below unless the file was hashed
    doc["generator"] = generator_json();
    doc["options"] = Json{{"decode", options.decode},
                          {"subimage", options.subimage ? Json(*options.subimage) : Json(nullptr)}};
    doc["status"] = "error";
    doc["error"] = nullptr;
    doc["parity_stage"] = nullptr;
    doc["parity"] = nullptr;
    doc["metadata"] = nullptr;
    doc["subimages"] = nullptr;

    // A report carries file_sha256 only when the whole file was hashed; a null would not match a
    // reference that has one (research/tools/compare_probe.py).
    const auto finish = [&](bool ok) {
        if (doc["file_sha256"].is_null()) {
            doc.erase("file_sha256");
        }
        report.ok = ok;
        return std::move(report);
    };
    const auto fail_with = [&](const Error& error) {
        doc["error"] = error_json(error);
        return finish(false);
    };

    try {
        auto identity = core::stat_file(path);
        if (!identity) {
            return fail_with(identity.error());
        }
        doc["file_size"] = identity->size_bytes;
        // Every step opens the file again; each must have read the file version that was hashed
        // (or, without hashing, stat'ed), or the report would pair values of different versions.
        core::FileIdentity reported = *identity;
        if (options.hash_file) {
            auto hashed = core::hash_file(path, cancel);
            if (!hashed) {
                return fail_with(hashed.error());
            }
            doc["file_sha256"] = core::to_hex(hashed->digest);
            reported = hashed->identity;
        }
        const auto same_version = [&](const core::FileIdentity& seen, std::string_view step) -> Status {
            if (seen == reported) {
                return ok();
            }
            return fail(ErrorCode::SourceChanged,
                        std::format("the file changed between reading it for the report and {} ({} then {})",
                                    step, core::to_string(reported), core::to_string(seen)),
                        "probe.report");
        };

        raw::ProbeOptions probe_options;
        probe_options.subimage = options.subimage;
        auto selection = registry.select(path, probe_options, cancel);
        if (!selection) {
            return fail_with(selection.error());
        }
        if (auto same = same_version(selection->probe.metadata.source.identity, "probing it"); !same) {
            return fail_with(same.error());
        }
        doc["metadata"] = metadata_json(selection->probe.metadata);
        doc["subimages"] = subimages_json(selection->probe.subimages);

        if (!options.decode) {
            const raw::RawMetadata& m = selection->probe.metadata;
            doc["parity_stage"] = std::string(raw::to_string(m.stage));
            if (m.decoder.reference_parity) {
                doc["parity"] = parity_json(*m.decoder.reference_parity, std::nullopt);
            }
            doc["status"] = "ok";
            return finish(true);
        }

        raw::DecodeOptions decode_options;
        decode_options.subimage = options.subimage;
        auto decoded = selection->decoder->decode(path, decode_options, cancel);
        if (!decoded) {
            return fail_with(decoded.error());
        }
        if (auto same = same_version(decoded->metadata.source.identity, "decoding it"); !same) {
            return fail_with(same.error());
        }
        doc["metadata"] = metadata_json(decoded->metadata);
        auto hash = raw_data_sha256(*decoded);
        if (!hash) {
            return fail_with(hash.error());
        }
        doc["parity_stage"] = std::string(raw::to_string(decoded->metadata.stage));
        if (decoded->metadata.decoder.reference_parity) {
            const raw::ReferenceParity& parity = *decoded->metadata.decoder.reference_parity;
            doc["parity"] = parity_json(parity, parity.raw_type == "flat" ? *hash : std::nullopt);
        }
        doc["status"] = "ok";
        return finish(true);
    } catch (const std::bad_alloc&) {
        return fail_with(
            Error{ErrorCode::OutOfMemory, "out of memory while building the report", "probe.report", 0});
    } catch (const std::exception& e) {
        return fail_with(Error{ErrorCode::Internal,
                               std::format("exception while building the report: {}", e.what()),
                               "probe.report", 0});
    }
}

}  // namespace lucent::probe
