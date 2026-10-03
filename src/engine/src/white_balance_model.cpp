// Temperature/tint conversions and the camera white-balance model (white_balance_model.hpp).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <lucent/engine/white_balance_model.hpp>

#include "robertson_table.hpp"

namespace lucent::engine {

namespace {

constexpr const char* kWhere = "engine.develop.white_balance";

using detail::kRobertson;

bool positive(double v) noexcept { return std::isfinite(v) && v > 0.0; }

bool positive(const RgbMultipliers& m) noexcept { return positive(m[0]) && positive(m[1]) && positive(m[2]); }

RgbMultipliers green_normalized(const RgbMultipliers& m) noexcept { return {m[0] / m[1], 1.0, m[2] / m[1]}; }

std::string format_rgb(const RgbMultipliers& m) { return std::format("({:.6g}, {:.6g}, {:.6g})", m[0], m[1], m[2]); }

// The DNG SDK pins camera-white entries to [0.001, 1] after scaling to a maximum of 1.
constexpr double kMinNeutral = 0.001;

// Multipliers from a camera neutral (camera values of a neutral surface): the neutral is scaled
// to a maximum of 1 and pinned to [0.001, 1] (dng_color_spec::SetWhiteXY), then 1 / neutral,
// green = 1. Corrupt when no entry is positive.
Expected<RgbMultipliers> multipliers_from_neutral(const image::Vec3d& neutral) {
    const double max_entry = std::max({neutral[0], neutral[1], neutral[2]});
    if (!std::isfinite(neutral[0]) || !std::isfinite(neutral[1]) || !std::isfinite(neutral[2]) ||
        !(max_entry > 0.0)) {
        return fail(ErrorCode::Corrupt,
                    std::format("the calibration gives the camera neutral ({}, {}, {}), which has no positive entry",
                                neutral[0], neutral[1], neutral[2]),
                    kWhere);
    }
    RgbMultipliers m{};
    for (std::size_t c = 0; c < 3; ++c) {
        m[c] = 1.0 / std::clamp(neutral[c] / max_entry, kMinNeutral, 1.0);
    }
    return green_normalized(m);
}

// Whites outside the spectral locus (x + y >= 1, reached by extreme tints at the ends of the
// temperature range) are accepted: the mapping is linear algebra on XYZ, and the camera neutral is
// checked for positivity afterwards.
Status check_chromaticity(image::Chromaticity xy) {
    if (!std::isfinite(xy.x) || !std::isfinite(xy.y) || !(xy.x > 0.0) || !(xy.y > 0.0)) {
        return fail(ErrorCode::InvalidArgument, std::format("({}, {}) is not a valid white chromaticity", xy.x, xy.y),
                    kWhere);
    }
    return ok();
}

}  // namespace

// ---- dng_temperature ---------------------------------------------------------------------------

Expected<TemperatureTint> temperature_tint_from_xy(image::Chromaticity xy) {
    if (!std::isfinite(xy.x) || !std::isfinite(xy.y)) {
        return fail(ErrorCode::InvalidArgument, "the chromaticity is not finite", kWhere);
    }
    const double den = 1.5 - xy.x + 6.0 * xy.y;
    if (!(den > 0.0)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("chromaticity ({}, {}) has no CIE 1960 (u, v) coordinates", xy.x, xy.y), kWhere);
    }
    const double u = 2.0 * xy.x / den;
    const double v = 3.0 * xy.y / den;
    double last_dt = 0.0;
    double last_du = 0.0;
    double last_dv = 0.0;
    for (std::size_t index = 1; index < kRobertson.size(); ++index) {
        // Unit vector along isotemperature line `index`.
        double du = 1.0;
        double dv = kRobertson[index].t;
        const double len = std::sqrt(1.0 + dv * dv);
        du /= len;
        dv /= len;
        // Signed distance of (u, v) from the line; <= 0 once the point lies below it.
        double uu = u - kRobertson[index].u;
        double vv = v - kRobertson[index].v;
        double dt = -uu * dv + vv * du;
        if (dt <= 0.0 || index == kRobertson.size() - 1) {
            if (dt > 0.0) {
                dt = 0.0;
            }
            dt = -dt;
            // Weight of line index - 1.
            const double f = index == 1 ? 0.0 : dt / (last_dt + dt);
            TemperatureTint result;
            result.temperature_k = 1.0e6 / (kRobertson[index - 1].mired * f + kRobertson[index].mired * (1.0 - f));
            // Offset of the point from the interpolated locus point, along the interpolated
            // isotemperature direction.
            uu = u - (kRobertson[index - 1].u * f + kRobertson[index].u * (1.0 - f));
            vv = v - (kRobertson[index - 1].v * f + kRobertson[index].v * (1.0 - f));
            du = du * (1.0 - f) + last_du * f;
            dv = dv * (1.0 - f) + last_dv * f;
            const double dlen = std::sqrt(du * du + dv * dv);
            du /= dlen;
            dv /= dlen;
            result.tint = (uu * du + vv * dv) * kTintScale;
            return result;
        }
        last_dt = dt;
        last_du = du;
        last_dv = dv;
    }
    return fail(ErrorCode::Internal, "the isotemperature search did not terminate", kWhere);  // unreachable
}

Expected<image::Chromaticity> xy_from_temperature_tint(const TemperatureTint& value) {
    if (!std::isfinite(value.temperature_k) || !std::isfinite(value.tint) || !(value.temperature_k > 0.0)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("temperature {} K and tint {} must be finite with a positive temperature",
                                value.temperature_k, value.tint),
                    kWhere);
    }
    const double r = 1.0e6 / value.temperature_k;
    const double offset = value.tint * (1.0 / kTintScale);
    const std::size_t last = kRobertson.size() - 2;  // index of the last line pair
    for (std::size_t index = 0; index <= last; ++index) {
        if (r < kRobertson[index + 1].mired || index == last) {
            // Weight of line `index`.
            const double f =
                (kRobertson[index + 1].mired - r) / (kRobertson[index + 1].mired - kRobertson[index].mired);
            double u = kRobertson[index].u * f + kRobertson[index + 1].u * (1.0 - f);
            double v = kRobertson[index].v * f + kRobertson[index + 1].v * (1.0 - f);
            double uu1 = 1.0;
            double vv1 = kRobertson[index].t;
            double uu2 = 1.0;
            double vv2 = kRobertson[index + 1].t;
            const double len1 = std::sqrt(1.0 + vv1 * vv1);
            const double len2 = std::sqrt(1.0 + vv2 * vv2);
            uu1 /= len1;
            vv1 /= len1;
            uu2 /= len2;
            vv2 /= len2;
            double uu3 = uu1 * f + uu2 * (1.0 - f);
            double vv3 = vv1 * f + vv2 * (1.0 - f);
            const double len3 = std::sqrt(uu3 * uu3 + vv3 * vv3);
            uu3 /= len3;
            vv3 /= len3;
            u += uu3 * offset;
            v += vv3 * offset;
            const double den = u - 4.0 * v + 2.0;
            const image::Chromaticity xy{1.5 * u / den, v / den};
            if (!(den > 0.0) || !std::isfinite(xy.x) || !std::isfinite(xy.y) || !(xy.x > 0.0) || !(xy.y > 0.0)) {
                return fail(ErrorCode::Corrupt,
                            std::format("temperature {} K with tint {} has no valid xy chromaticity",
                                        value.temperature_k, value.tint),
                            kWhere);
            }
            return xy;
        }
    }
    return fail(ErrorCode::Internal, "the isotemperature search did not terminate", kWhere);  // unreachable
}

// ---- WhiteBalanceModel ---------------------------------------------------------------------------

Expected<WhiteBalanceModel> WhiteBalanceModel::create(const raw::RawMetadata& metadata) {
    WhiteBalanceModel model;
    auto as_shot = resolve_white_balance(metadata, WhiteBalanceOptions{});
    if (!as_shot) {
        return Unexpected(std::move(as_shot.error()));
    }
    model.as_shot_source_ = as_shot->source;
    const raw::RepresentationInfo& rep = metadata.representation;

    if (rep.kind == raw::RawRepresentation::Monochrome) {
        model.path_ = CalibrationPath::MonochromeNeutral;
        model.channel_rgb_ = {1, -1, -1, -1};
        model.unavailable_reason_ = "monochrome data has no colour to balance";
        model.basis_ = "monochrome sensor: white balance does not apply";
        return model;
    }

    // Channel index -> camera R, G, B (Green2 takes green, as LibRaw's fourth multiplier does).
    std::array<std::int32_t, 3> index{-1, -1, -1};
    std::int32_t green2 = -1;
    for (std::size_t i = 0; i < rep.channel_colors.size() && i < 4; ++i) {
        const auto idx = static_cast<std::int32_t>(i);
        switch (rep.channel_colors[i]) {
            case image::CfaColor::Red:
                model.channel_rgb_[i] = 0;
                if (index[0] < 0) index[0] = idx;
                break;
            case image::CfaColor::Green:
                model.channel_rgb_[i] = 1;
                if (index[1] < 0) index[1] = idx;
                break;
            case image::CfaColor::Green2:
                model.channel_rgb_[i] = 1;
                if (green2 < 0) green2 = idx;
                break;
            case image::CfaColor::Blue:
                model.channel_rgb_[i] = 2;
                if (index[2] < 0) index[2] = idx;
                break;
            default:
                break;
        }
    }
    if (index[1] < 0) {
        index[1] = green2;
    }
    if (index[0] < 0 || index[1] < 0 || index[2] < 0) {
        return fail(ErrorCode::Unsupported, "the data has no red, green and blue channels to balance", kWhere);
    }
    RgbMultipliers applied{};
    for (std::size_t c = 0; c < 3; ++c) {
        applied[c] = as_shot->multipliers[static_cast<std::size_t>(index[c])];
    }

    // ---- Balance the decoder already applied -------------------------------------------------
    const bool balanced_by_decoder = metadata.has_prior_processing(raw::PriorProcessingKind::WhiteBalanceApplied);
    std::string prior_problem;
    std::optional<DngProfile> profile;
    std::string profile_problem;
    if (metadata.dng && metadata.dng->info.color.color_matrix[0]) {
        auto p = dng_profile_from_metadata(metadata);
        if (p) {
            profile = std::move(*p);
        } else {
            profile_problem = p.error().message;
        }
    }
    if (balanced_by_decoder) {
        if (metadata.dng) {
            // AsShotNeutral in DNG plane order; the profile reader validated the planes.
            const std::vector<double>& asn = metadata.dng->info.color.as_shot_neutral;
            std::vector<std::uint8_t> plane_color = metadata.dng->raw_ifd.cfa_plane_color;
            if (plane_color.empty()) {
                plane_color = {0, 1, 2};
            }
            bool ok_prior = asn.size() == 3 && plane_color.size() == 3;
            RgbMultipliers neutral{};
            for (std::size_t c = 0; ok_prior && c < 3; ++c) {
                const auto it = std::find(plane_color.begin(), plane_color.end(), static_cast<std::uint8_t>(c));
                ok_prior = it != plane_color.end() && positive(asn[static_cast<std::size_t>(it - plane_color.begin())]);
                if (ok_prior) {
                    neutral[c] = asn[static_cast<std::size_t>(it - plane_color.begin())];
                }
            }
            if (ok_prior) {
                model.prior_ = green_normalized(RgbMultipliers{1.0 / neutral[0], 1.0 / neutral[1], 1.0 / neutral[2]});
            } else {
                prior_problem = "the decoder applied the as-shot white balance and the DNG has no usable AsShotNeutral";
            }
        } else {
            RgbMultipliers cam{};
            const auto& m = metadata.color.as_shot_multipliers;
            for (std::size_t c = 0; c < 3; ++c) {
                cam[c] = static_cast<double>(m[static_cast<std::size_t>(index[c])]);
            }
            if (cam[1] <= 0.0 && green2 >= 0) {
                cam[1] = static_cast<double>(m[static_cast<std::size_t>(green2)]);
            }
            if (positive(cam)) {
                model.prior_ = green_normalized(cam);
            } else {
                prior_problem = "the decoder applied the as-shot white balance but reports no as-shot multipliers";
            }
        }
    }
    model.as_shot_ = green_normalized(RgbMultipliers{applied[0] * model.prior_[0], applied[1] * model.prior_[1],
                                                     applied[2] * model.prior_[2]});

    // ---- Calibration --------------------------------------------------------------------------
    if (profile) {
        model.path_ = CalibrationPath::DngColorMatrix;  // refined below when a ForwardMatrix exists
        if (profile->forward_matrix[0] || profile->forward_matrix[1]) {
            model.path_ = CalibrationPath::DngForwardMatrix;
        }
        model.dng_ = std::move(profile);
        model.basis_ = "DNG camera profile: the white maps to the camera neutral through the calibration "
                       "interpolated at the white's temperature (DNG SDK dng_color_spec)";
    } else if (!profile_problem.empty()) {
        model.path_ = CalibrationPath::DngColorMatrix;
        model.unavailable_reason_ = "the DNG camera profile cannot be used: " + profile_problem;
    } else {
        model.path_ = CalibrationPath::DecoderMatrix;
        bool any_zero_row = false;
        for (std::size_t c = 0; c < 3; ++c) {
            const auto& row = metadata.color.xyz_to_camera[static_cast<std::size_t>(index[c])];
            bool zero = true;
            for (std::size_t j = 0; j < 3; ++j) {
                model.xyz_to_camera_(c, j) = static_cast<double>(row[j]);
                zero = zero && row[j] == 0.0f;
            }
            any_zero_row = any_zero_row || zero;
        }
        auto inverse = image::inverse(model.xyz_to_camera_);
        const image::Vec3d d65_camera = model.xyz_to_camera_ * *image::xy_to_XYZ(image::kD65White);
        if (any_zero_row) {
            model.unavailable_reason_ = "no camera calibration: the decoder reports no XYZ-to-camera matrix";
        } else if (!image::is_finite(model.xyz_to_camera_) || !inverse) {
            model.unavailable_reason_ = "the decoder's XYZ-to-camera matrix is not finite or is singular";
        } else if (!positive(d65_camera[0]) || !positive(d65_camera[1]) || !positive(d65_camera[2])) {
            model.unavailable_reason_ = "the decoder's XYZ-to-camera matrix maps the D65 white to non-positive values";
        } else {
            model.camera_to_xyz_ = *inverse;
            model.basis_ = "decoder XYZ(D65)-to-camera matrix: camera neutral = matrix * XYZ(white), as dcraw/LibRaw "
                           "derive daylight multipliers; a single matrix approximates whites far from D65";
        }
    }
    if (model.unavailable_reason_.empty() && !prior_problem.empty()) {
        model.unavailable_reason_ = prior_problem;
    }
    if (model.unavailable_reason_.empty()) {
        // The as-shot balance must have a white the calibration can express.
        auto white = model.compute_white(model.as_shot_);
        if (!white) {
            model.unavailable_reason_ = "the as-shot white balance has no white in this calibration: " +
                                        white.error().message;
        }
    }
    return model;
}

Expected<RgbMultipliers> WhiteBalanceModel::multipliers_for_white(image::Chromaticity white) const {
    if (!kelvin_available()) {
        return fail(ErrorCode::Unsupported, "temperature white balance is unavailable: " + unavailable_reason_, kWhere);
    }
    if (auto valid = check_chromaticity(white); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    if (dng_) {
        auto neutral = dng_camera_neutral(*dng_, white);
        if (!neutral) {
            return Unexpected(std::move(neutral.error()));
        }
        return multipliers_from_neutral(*neutral);
    }
    auto xyz = image::xy_to_XYZ(white);
    if (!xyz) {
        return Unexpected(std::move(xyz.error()));
    }
    return multipliers_from_neutral(xyz_to_camera_ * *xyz);
}

Expected<RgbMultipliers> WhiteBalanceModel::multipliers_for(const TemperatureTint& value) const {
    if (!kelvin_available()) {
        return fail(ErrorCode::Unsupported, "temperature white balance is unavailable: " + unavailable_reason_, kWhere);
    }
    auto xy = xy_from_temperature_tint(value);
    if (!xy) {
        return Unexpected(std::move(xy.error()));
    }
    return multipliers_for_white(*xy);
}

Expected<image::Chromaticity> WhiteBalanceModel::white_for(const RgbMultipliers& multipliers) const {
    if (!kelvin_available()) {
        return fail(ErrorCode::Unsupported, "temperature white balance is unavailable: " + unavailable_reason_, kWhere);
    }
    return compute_white(multipliers);
}

Expected<image::Chromaticity> WhiteBalanceModel::compute_white(const RgbMultipliers& multipliers) const {
    if (!positive(multipliers)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("multipliers {} must be finite and positive", format_rgb(multipliers)), kWhere);
    }
    const image::Vec3d neutral{1.0 / multipliers[0], 1.0 / multipliers[1], 1.0 / multipliers[2]};
    if (dng_) {
        auto record = evaluate_dng_profile(*dng_, neutral);
        if (!record) {
            return Unexpected(std::move(record.error()));
        }
        return record->white_xy;
    }
    const image::Vec3d xyz = camera_to_xyz_ * neutral;
    const double sum = xyz[0] + xyz[1] + xyz[2];
    if (!positive(xyz[1]) || !positive(sum) || !std::isfinite(xyz[0]) || !std::isfinite(xyz[2])) {
        return fail(ErrorCode::Corrupt,
                    std::format("the neutral of multipliers {} maps outside positive XYZ ({}, {}, {})",
                                format_rgb(multipliers), xyz[0], xyz[1], xyz[2]),
                    kWhere);
    }
    return image::Chromaticity{xyz[0] / sum, xyz[1] / sum};
}

Expected<TemperatureTint> WhiteBalanceModel::temperature_for(const RgbMultipliers& multipliers) const {
    auto white = white_for(multipliers);
    if (!white) {
        return Unexpected(std::move(white.error()));
    }
    return temperature_tint_from_xy(*white);
}

Expected<RgbMultipliers> WhiteBalanceModel::target_multipliers(const WhiteBalanceSetting& setting) const {
    if (const auto* kelvin = std::get_if<KelvinWhiteBalance>(&setting)) {
        if (!std::isfinite(kelvin->temperature_k) || !std::isfinite(kelvin->tint) ||
            kelvin->temperature_k < kMinTemperatureK || kelvin->temperature_k > kMaxTemperatureK ||
            std::abs(kelvin->tint) > kMaxTint) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("temperature {} K / tint {} is outside the supported range", kelvin->temperature_k,
                                    kelvin->tint),
                        kWhere);
        }
        return multipliers_for(TemperatureTint{kelvin->temperature_k, kelvin->tint});
    }
    if (const auto* neutral = std::get_if<CameraNeutralWhiteBalance>(&setting)) {
        if (monochrome()) {
            return fail(ErrorCode::Unsupported, "camera-neutral white balance does not apply to monochrome data",
                        kWhere);
        }
        const RgbMultipliers m{neutral->r, neutral->g, neutral->b};
        if (!positive(m)) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("camera-neutral multipliers {} must be finite and positive", format_rgb(m)), kWhere);
        }
        return green_normalized(m);
    }
    return as_shot_;
}

RgbMultipliers WhiteBalanceModel::sample_multipliers(const RgbMultipliers& absolute) const noexcept {
    return green_normalized(
        RgbMultipliers{absolute[0] / prior_[0], absolute[1] / prior_[1], absolute[2] / prior_[2]});
}

std::array<double, 4> WhiteBalanceModel::channel_multipliers(const RgbMultipliers& rgb) const noexcept {
    std::array<double, 4> out{};
    for (std::size_t i = 0; i < 4; ++i) {
        const std::int32_t c = channel_rgb_[i];
        if (c >= 0) {
            out[i] = monochrome() ? 1.0 : rgb[static_cast<std::size_t>(c)];
        }
    }
    return out;
}

}  // namespace lucent::engine
