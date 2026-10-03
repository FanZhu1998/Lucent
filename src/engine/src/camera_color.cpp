// Camera colour interpretation: calibration choice, application and working-space conversion
// (camera_color.hpp). The DNG profile evaluation itself lives in dng_color.cpp.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <lucent/engine/camera_color.hpp>
#include <lucent/image/color_math.hpp>

#include "parallel_rows.hpp"
#include "pixel_ops.hpp"

namespace lucent::engine {

namespace {

constexpr const char* kWhereCalibration = "engine.color.calibration";
constexpr const char* kWhereApply = "engine.color.apply";
constexpr const char* kWhereWorking = "engine.color.working";

// DNG colour-plane codes (CFAPlaneColor / CFAPattern values).
constexpr std::uint8_t kPlaneRed = 0;
constexpr std::uint8_t kPlaneGreen = 1;
constexpr std::uint8_t kPlaneBlue = 2;

std::string format_matrix(const image::Matrix3d& m) {
    return std::format("[[{:.6g}, {:.6g}, {:.6g}], [{:.6g}, {:.6g}, {:.6g}], [{:.6g}, {:.6g}, {:.6g}]]",
                       m(0, 0), m(0, 1), m(0, 2), m(1, 0), m(1, 1), m(1, 2), m(2, 0), m(2, 1), m(2, 2));
}

// Channel indices (RepresentationInfo::channel_colors) of camera red, green and blue; green
// falls back to Green2 when the data has no Green channel.
struct RgbChannels {
    std::array<std::int32_t, 3> index{-1, -1, -1};

    [[nodiscard]] bool complete() const noexcept { return index[0] >= 0 && index[1] >= 0 && index[2] >= 0; }
};

RgbChannels rgb_channels(const std::vector<image::CfaColor>& colors) {
    RgbChannels r;
    std::int32_t green2 = -1;
    for (std::size_t i = 0; i < colors.size() && i < 4; ++i) {
        const auto idx = static_cast<std::int32_t>(i);
        switch (colors[i]) {
            case image::CfaColor::Red:
                if (r.index[0] < 0)
                    r.index[0] = idx;
                break;
            case image::CfaColor::Green:
                if (r.index[1] < 0)
                    r.index[1] = idx;
                break;
            case image::CfaColor::Blue:
                if (r.index[2] < 0)
                    r.index[2] = idx;
                break;
            case image::CfaColor::Green2:
                if (green2 < 0)
                    green2 = idx;
                break;
            default:
                break;
        }
    }
    if (r.index[1] < 0) {
        r.index[1] = green2;
    }
    return r;
}

bool positive(double v) noexcept {
    return std::isfinite(v) && v > 0.0;
}

// Chooses the DNG colour plane of camera red, green and blue.
Expected<std::array<std::uint32_t, 3>> dng_planes(const raw::DngMetadata& dng) {
    std::vector<std::uint8_t> plane_color = dng.raw_ifd.cfa_plane_color;
    if (plane_color.empty()) {
        plane_color = {kPlaneRed, kPlaneGreen, kPlaneBlue};  // DNG default CFAPlaneColor
    }
    if (plane_color.size() != 3) {
        return fail(ErrorCode::Unsupported,
                    std::format("CFAPlaneColor lists {} planes; only 3-plane (RGB) profiles are supported",
                                plane_color.size()),
                    kWhereCalibration);
    }
    std::array<std::uint32_t, 3> plane{};
    const std::array<std::uint8_t, 3> wanted = {kPlaneRed, kPlaneGreen, kPlaneBlue};
    for (std::size_t c = 0; c < 3; ++c) {
        const auto count = std::count(plane_color.begin(), plane_color.end(), wanted[c]);
        if (count != 1) {
            return fail(
                ErrorCode::Unsupported,
                std::format("CFAPlaneColor {{{}, {}, {}}} does not hold red, green and blue exactly once",
                            plane_color[0], plane_color[1], plane_color[2]),
                kWhereCalibration);
        }
        plane[c] = static_cast<std::uint32_t>(
            std::distance(plane_color.begin(), std::find(plane_color.begin(), plane_color.end(), wanted[c])));
    }
    return plane;
}

Expected<image::Matrix3d> dng_matrix(const raw::DngMatrix& m, std::uint32_t rows, std::uint32_t cols,
                                     const std::array<std::uint32_t, 3>& plane, bool permute_rows,
                                     bool permute_cols, const char* name) {
    if (m.rows != rows || m.cols != cols || m.values.size() != static_cast<std::size_t>(rows) * cols) {
        return fail(ErrorCode::Corrupt,
                    std::format("{} is {}x{}, expected {}x{}", name, m.rows, m.cols, rows, cols),
                    kWhereCalibration);
    }
    image::Matrix3d r;
    for (std::uint32_t i = 0; i < 3; ++i) {
        for (std::uint32_t j = 0; j < 3; ++j) {
            r(i, j) = m.at(permute_rows ? plane[i] : i, permute_cols ? plane[j] : j);
        }
    }
    return r;
}

// The DNG profile of IFD 0 in camera R, G, B order.
Expected<DngProfile> dng_profile(const raw::DngMetadata& dng, const std::array<std::uint32_t, 3>& plane,
                                 std::vector<std::string>& notes) {
    const raw::DngColorInfo& info = dng.info.color;
    DngProfile profile;
    static constexpr std::array<const char*, 2> kColorMatrix = {"ColorMatrix1", "ColorMatrix2"};
    static constexpr std::array<const char*, 2> kCalibration = {"CameraCalibration1", "CameraCalibration2"};
    static constexpr std::array<const char*, 2> kForward = {"ForwardMatrix1", "ForwardMatrix2"};
    for (std::size_t i = 0; i < 2; ++i) {
        profile.illuminants[i] = info.calibration_illuminant[i].value_or(0);
        if (info.color_matrix[i]) {
            auto m = dng_matrix(*info.color_matrix[i], 3, 3, plane, true, false, kColorMatrix[i]);
            if (!m) {
                return Unexpected(std::move(m.error()));
            }
            profile.color_matrix[i] = *m;
        }
        if (info.camera_calibration[i]) {
            auto m = dng_matrix(*info.camera_calibration[i], 3, 3, plane, true, true, kCalibration[i]);
            if (!m) {
                return Unexpected(std::move(m.error()));
            }
            profile.camera_calibration[i] = *m;
        }
        if (info.forward_matrix[i]) {
            auto m = dng_matrix(*info.forward_matrix[i], 3, 3, plane, false, true, kForward[i]);
            if (!m) {
                return Unexpected(std::move(m.error()));
            }
            profile.forward_matrix[i] = *m;
        }
    }
    if (!info.analog_balance.empty()) {
        if (info.analog_balance.size() != 3) {
            return fail(ErrorCode::Corrupt,
                        std::format("AnalogBalance has {} values, expected 3", info.analog_balance.size()),
                        kWhereCalibration);
        }
        for (std::size_t c = 0; c < 3; ++c) {
            profile.analog_balance[c] = info.analog_balance[plane[c]];
        }
    }
    if (info.color_matrix[2] || info.calibration_illuminant[2]) {
        notes.emplace_back(
            "a third calibration illuminant (DNG 1.6 ColorMatrix3/CalibrationIlluminant3) is present and "
            "ignored; "
            "calibrations 1 and 2 are interpolated");
    }
    for (std::size_t i = 0; i < 2; ++i) {
        if (profile.illuminants[i] == 255) {
            notes.push_back(
                std::format("CalibrationIlluminant{} is 255 (defined by IlluminantData), which is not "
                            "supported; its temperature "
                            "is unknown, so ColorMatrix1 alone is used",
                            i + 1));
        }
    }
    notes.emplace_back(
        "CameraCalibrationSignature / ProfileCalibrationSignature are not inspected; CameraCalibration tags "
        "are "
        "applied as present");
    return profile;
}

}  // namespace

std::string_view to_string(CalibrationPath path) noexcept {
    switch (path) {
        case CalibrationPath::DngColorMatrix:
            return "dng_color_matrix";
        case CalibrationPath::DngForwardMatrix:
            return "dng_forward_matrix";
        case CalibrationPath::DecoderMatrix:
            return "decoder_matrix";
        case CalibrationPath::MonochromeNeutral:
            return "monochrome_neutral";
    }
    return "invalid";
}

image::ColorEncoding xyz_encoding(image::WhitePoint white) noexcept {
    image::ColorEncoding e = image::ColorEncoding::scene_linear_rec2020_d65();
    e.primaries = image::ColorPrimaries::CieXyz;
    e.white_point = white;
    return e;
}

Expected<DngProfile> dng_profile_from_metadata(const raw::RawMetadata& metadata, std::vector<std::string>* notes) {
    if (!metadata.dng || !metadata.dng->info.color.color_matrix[0]) {
        return fail(ErrorCode::Unsupported, "the file has no DNG camera profile (ColorMatrix1)", kWhereCalibration);
    }
    const raw::DngMetadata& dng = *metadata.dng;
    const std::uint32_t planes = dng.info.color_planes.value_or(3);
    if (planes != 3) {
        return fail(ErrorCode::Unsupported,
                    std::format("the DNG profile has {} colour planes; only 3 are supported", planes),
                    kWhereCalibration);
    }
    auto plane = dng_planes(dng);
    if (!plane) {
        return Unexpected(std::move(plane.error()));
    }
    std::vector<std::string> ignored;
    return dng_profile(dng, *plane, notes != nullptr ? *notes : ignored);
}

Expected<CameraColorRecord> resolve_camera_color(const raw::RawMetadata& metadata,
                                                 const WhiteBalanceRecord& white_balance) {
    CameraColorRecord record;
    const image::Vec3d d65 = *image::xy_to_XYZ(image::kD65White);

    // ---- Monochrome --------------------------------------------------------------------------
    if (metadata.representation.kind == raw::RawRepresentation::Monochrome) {
        const double m = white_balance.applied.multipliers[0];
        if (!positive(m)) {
            return fail(ErrorCode::InvalidArgument, "the monochrome white-balance multiplier is not positive",
                        kWhereCalibration);
        }
        record.path = CalibrationPath::MonochromeNeutral;
        record.provenance = raw::Provenance::LucentRule;
        record.multipliers = {m, m, m};
        record.neutral_scale = 1.0 / m;
        record.xyz_white = image::WhitePoint::D65;
        record.camera_to_xyz(0, 0) = d65[0] * record.neutral_scale;
        record.camera_to_xyz(1, 0) = d65[1] * record.neutral_scale;
        record.camera_to_xyz(2, 0) = d65[2] * record.neutral_scale;
        record.basis =
            "monochrome sensor: each value is a neutral of that luminance (XYZ = value * D65 white)";
        return record;
    }

    // ---- Applied white balance of camera R, G, B ------------------------------------------------
    const RgbChannels channels = rgb_channels(metadata.representation.channel_colors);
    if (!channels.complete()) {
        return fail(ErrorCode::Unsupported, "the data has no red, green and blue channels to interpret",
                    kWhereCalibration);
    }
    std::array<double, 3> applied{};
    for (std::size_t c = 0; c < 3; ++c) {
        applied[c] = white_balance.applied.multipliers[static_cast<std::size_t>(channels.index[c])];
        if (!positive(applied[c])) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("white-balance multiplier of channel {} is {}; red, green and blue need "
                                    "finite positive multipliers",
                                    channels.index[c], applied[c]),
                        kWhereCalibration);
        }
    }
    record.multipliers = applied;
    record.neutral_scale = 1.0 / std::min({applied[0], applied[1], applied[2]});
    const bool balanced_by_decoder =
        metadata.has_prior_processing(raw::PriorProcessingKind::WhiteBalanceApplied);

    // ---- DNG camera profile ------------------------------------------------------------------------
    const bool dng_profile_present =
        metadata.dng.has_value() && metadata.dng->info.color.color_matrix[0].has_value();
    if (dng_profile_present) {
        const raw::DngMetadata& dng = *metadata.dng;
        const std::uint32_t planes = dng.info.color_planes.value_or(3);
        if (planes != 3) {
            return fail(ErrorCode::Unsupported,
                        std::format("the DNG profile has {} colour planes; only 3 are supported", planes),
                        kWhereCalibration);
        }
        auto plane = dng_planes(dng);
        if (!plane) {
            return Unexpected(std::move(plane.error()));
        }
        auto profile = dng_profile(dng, *plane, record.notes);
        if (!profile) {
            return Unexpected(std::move(profile.error()));
        }
        // Total balance between the camera's native values and the values the matrix sees.
        std::array<double, 3> total = applied;
        if (balanced_by_decoder) {
            const std::vector<double>& asn = dng.info.color.as_shot_neutral;
            if (asn.size() != 3) {
                return fail(ErrorCode::Unsupported,
                            "the decoder already white balanced the data, but the DNG has no AsShotNeutral "
                            "to recover "
                            "the camera neutral",
                            kWhereCalibration);
            }
            const double green = asn[(*plane)[1]];
            for (std::size_t c = 0; c < 3; ++c) {
                const double n = asn[(*plane)[c]];
                if (!positive(n) || !positive(green)) {
                    return fail(ErrorCode::Corrupt, "AsShotNeutral is not positive", kWhereCalibration);
                }
                total[c] *= green / n;
            }
            record.notes.emplace_back(
                "the decoder had applied the as-shot white balance; AsShotNeutral supplies it for the camera "
                "neutral");
        }
        const image::Vec3d neutral{1.0 / total[0], 1.0 / total[1], 1.0 / total[2]};
        auto dng_record = evaluate_dng_profile(*profile, neutral);
        if (!dng_record) {
            return Unexpected(std::move(dng_record.error()));
        }
        const image::Matrix3d base =
            dng_record->camera_to_xyz_d50 *
            image::Matrix3d::diagonal(image::Vec3d{1.0 / total[0], 1.0 / total[1], 1.0 / total[2]});
        const image::Vec3d unity = base * image::Vec3d{1.0, 1.0, 1.0};
        if (!(unity[1] > 0.0) || !std::isfinite(unity[1])) {
            return fail(ErrorCode::Corrupt, "the DNG calibration maps white-balanced unity to Y <= 0",
                        kWhereCalibration);
        }
        record.camera_to_xyz = image::Matrix3d{record.neutral_scale / unity[1] * base.row(0),
                                               record.neutral_scale / unity[1] * base.row(1),
                                               record.neutral_scale / unity[1] * base.row(2)};
        record.xyz_white = image::WhitePoint::D50;
        record.path =
            dng_record->forward_matrix ? CalibrationPath::DngForwardMatrix : CalibrationPath::DngColorMatrix;
        record.provenance = raw::Provenance::DngTag;
        record.basis = std::format(
            "DNG {} path ({}): CalibrationIlluminant {} / {} ({:.0f} K / {:.0f} K){}; white xy ({:.5f}, "
            "{:.5f}) "
            "at {:.0f} K, weight of calibration 1 {:.4f}, {} pass(es){}; AnalogBalance {}, CameraCalibration "
            "{}",
            dng_record->forward_matrix ? "ForwardMatrix" : "ColorMatrix", dng.raw_ifd.path,
            profile->illuminants[0], profile->illuminants[1], dng_record->temperatures[0],
            dng_record->temperatures[1],
            dng_record->interpolated ? ", interpolated in inverse temperature" : ", single calibration",
            dng_record->white_xy.x, dng_record->white_xy.y, dng_record->white_cct_kelvin,
            dng_record->weight_matrix1, dng_record->iterations,
            dng_record->converged ? "" : " (not converged)",
            dng_record->analog_balance ? "applied" : "unity/absent",
            dng_record->camera_calibration ? "applied" : "absent");
        record.dng = std::move(*dng_record);
        return record;
    }

    // ---- Decoder matrix (LibRaw cam_xyz, D65) ---------------------------------------------------
    const raw::CameraColorData& color = metadata.color;
    image::Matrix3d xyz_to_camera;
    bool any_zero_row = false;
    for (std::size_t c = 0; c < 3; ++c) {
        const auto& row = color.xyz_to_camera[static_cast<std::size_t>(channels.index[c])];
        bool zero = true;
        for (std::size_t j = 0; j < 3; ++j) {
            xyz_to_camera(c, j) = static_cast<double>(row[j]);
            zero = zero && row[j] == 0.0f;
        }
        any_zero_row = any_zero_row || zero;
    }
    if (any_zero_row) {
        return fail(
            ErrorCode::Unsupported,
            metadata.dng
                ? "no camera calibration: the DNG has no ColorMatrix1 and the decoder reports no "
                  "XYZ-to-camera matrix"
                : "no camera calibration: the decoder reports no XYZ-to-camera matrix for this camera",
            kWhereCalibration);
    }
    if (!image::is_finite(xyz_to_camera)) {
        return fail(ErrorCode::Corrupt, "the decoder's XYZ-to-camera matrix is not finite",
                    kWhereCalibration);
    }
    image::Matrix3d normalized;
    for (std::size_t c = 0; c < 3; ++c) {
        const double sum =
            xyz_to_camera(c, 0) * d65[0] + xyz_to_camera(c, 1) * d65[1] + xyz_to_camera(c, 2) * d65[2];
        if (!(sum > 0.0)) {
            return fail(
                ErrorCode::Corrupt,
                std::format("the decoder's XYZ-to-camera row {} maps the D65 white to {}; it cannot be "
                            "normalised",
                            c, sum),
                kWhereCalibration);
        }
        for (std::size_t j = 0; j < 3; ++j) {
            normalized(c, j) = xyz_to_camera(c, j) / sum;
        }
    }
    auto camera_to_xyz = image::inverse(normalized);
    if (!camera_to_xyz) {
        return fail(ErrorCode::Corrupt, "the decoder's XYZ-to-camera matrix is singular", kWhereCalibration);
    }
    record.camera_to_xyz = image::Matrix3d{record.neutral_scale * camera_to_xyz->row(0),
                                           record.neutral_scale * camera_to_xyz->row(1),
                                           record.neutral_scale * camera_to_xyz->row(2)};
    record.xyz_white = image::WhitePoint::D65;
    record.path = CalibrationPath::DecoderMatrix;
    record.provenance = color.provenance;
    record.basis = std::format(
        "decoder XYZ(D65)-to-camera matrix {} (provenance {}), rows normalised so that the D65 white maps to "
        "camera "
        "(1, 1, 1), inverted",
        format_matrix(xyz_to_camera), raw::to_string(color.provenance));
    if (metadata.dng) {
        record.notes.emplace_back("the DNG has no ColorMatrix1; the decoder's matrix is used instead");
    }
    if (balanced_by_decoder) {
        record.notes.emplace_back(
            "the decoder had applied the as-shot white balance; the row-normalised matrix maps the balanced "
            "neutral "
            "to D65");
    }
    return record;
}

Expected<ColorImage> apply_camera_color(DemosaicedImage&& image, const CameraColorRecord& record,
                                        const ColorStageOptions& options,
                                        const core::CancellationToken& cancel) {
    if (image.pixels.empty()) {
        return fail(ErrorCode::InvalidArgument, "the demosaiced image is empty", kWhereApply);
    }
    if (!image::is_finite(record.camera_to_xyz)) {
        return fail(ErrorCode::InvalidArgument, "the camera-to-XYZ matrix is not finite", kWhereApply);
    }
    const bool mono_record = record.path == CalibrationPath::MonochromeNeutral;
    ColorImage out;
    out.encoding = xyz_encoding(record.xyz_white);
    if (image.encoding == image::ColorEncoding::camera_native_linear() && image.pixels.channels() == 3) {
        if (mono_record) {
            return fail(ErrorCode::InvalidArgument,
                        "an RGB image needs an RGB calibration, not a monochrome one", kWhereApply);
        }
        if (auto status = detail::apply_matrix_rows(image.pixels.view(), record.camera_to_xyz, cancel,
                                                    options.parallel, kWhereApply);
            !status) {
            return Unexpected(std::move(status.error()));
        }
        out.pixels = std::move(image.pixels);
        return out;
    }
    if (image.encoding == raw::camera_native_monochrome() && image.pixels.channels() == 1) {
        if (!mono_record) {
            return fail(ErrorCode::InvalidArgument, "a monochrome image needs the monochrome calibration",
                        kWhereApply);
        }
        auto xyz = image::ImageBuffer<float>::create_uninitialized(image.pixels.width(),
                                                                   image.pixels.height(), 3, options.limits);
        if (!xyz) {
            return Unexpected(std::move(xyz.error()));
        }
        const float kx = static_cast<float>(record.camera_to_xyz(0, 0));
        const float ky = static_cast<float>(record.camera_to_xyz(1, 0));
        const float kz = static_cast<float>(record.camera_to_xyz(2, 0));
        const image::ConstImageView<float> src = image.pixels.const_view();
        const image::ImageView<float> dst = xyz->view();
        const std::int32_t width = src.width();
        auto status =
            detail::for_each_row(src.height(), options.parallel, cancel, kWhereApply, [&](std::int32_t y) {
                const float* s = src.row(y);
                float* d = dst.row(y);
                for (std::int32_t x = 0; x < width; ++x, d += 3) {
                    const float v = s[x];
                    d[0] = kx * v;
                    d[1] = ky * v;
                    d[2] = kz * v;
                }
            });
        if (!status) {
            return Unexpected(std::move(status.error()));
        }
        image.pixels = {};
        out.pixels = std::move(*xyz);
        return out;
    }
    return fail(
        ErrorCode::InvalidArgument,
        std::format(
            "camera colour needs camera-native linear RGB or monochrome data (got {} with {} channels)",
            image.encoding.to_string(), image.pixels.channels()),
        kWhereApply);
}

Expected<image::Matrix3d> convert_to_working_space(ColorImage& image, const ColorStageOptions& options,
                                                   const core::CancellationToken& cancel) {
    const image::ColorEncoding& e = image.encoding;
    if (image.pixels.empty() || image.pixels.channels() != 3) {
        return fail(ErrorCode::InvalidArgument,
                    "the working-space conversion needs a non-empty 3-channel image", kWhereWorking);
    }
    if (auto valid = e.validate(); !valid) {
        return fail(ErrorCode::InvalidArgument, "invalid source encoding: " + valid.error().message,
                    kWhereWorking);
    }
    if (e.layout != image::ChannelLayout::Rgb || e.transfer != image::TransferFunction::Linear ||
        e.reference != image::ImageReference::Scene || e.primaries == image::ColorPrimaries::CameraNative) {
        return fail(
            ErrorCode::InvalidArgument,
            std::format("the working-space conversion needs linear, scene-referred XYZ or RGB (got {})",
                        e.to_string()),
            kWhereWorking);
    }
    const image::ColorEncoding working = image::ColorEncoding::scene_linear_rec2020_d65();
    auto matrix = image::conversion_matrix(e, working, image::ChromaticAdaptationMethod::Bradford);
    if (!matrix) {
        return fail(ErrorCode::InvalidArgument,
                    "no conversion to the working space: " + matrix.error().message, kWhereWorking);
    }
    if (*matrix != image::Matrix3d::identity()) {
        if (auto status = detail::apply_matrix_rows(image.pixels.view(), *matrix, cancel, options.parallel,
                                                    kWhereWorking);
            !status) {
            return Unexpected(std::move(status.error()));
        }
    } else if (auto cancelled = cancel.check(kWhereWorking); !cancelled) {
        return Unexpected(std::move(cancelled.error()));
    }
    image.encoding = working;
    return *matrix;
}

}  // namespace lucent::engine
