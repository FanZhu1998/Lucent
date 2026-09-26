// Neutral v0 display rendering (display_rendering.hpp).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <vector>

#include <lucent/engine/display_rendering.hpp>

#include "parallel_rows.hpp"

namespace lucent::engine {

namespace {

constexpr const char* kWhere = "engine.render.neutral";
constexpr double kMaxExposureEv = 16.0;

struct RowCounts {
    std::uint64_t compressed = 0;
    std::uint64_t lifted = 0;
    std::uint64_t limited = 0;
    std::uint64_t negative = 0;
    std::uint64_t non_finite = 0;
};

NeutralV0Curve curve_for(double white) noexcept {
    if (white > 1.0) {
        return NeutralV0Curve::Shoulder;
    }
    return white < 1.0 ? NeutralV0Curve::Lift : NeutralV0Curve::Identity;
}

// The DNG SDK exposure tone coefficients for slope `white` (display_rendering.hpp, Lift).
std::array<double, 3> lift_coefficients(double white) noexcept {
    const double a = 16.0 / 9.0 * (1.0 - white);
    const double b = white - 0.5 * a;
    return {a, b, 1.0 - a - b};
}

// Largest m that the mapping leaves unchanged (the identity part of the branch).
double linear_limit(NeutralV0Curve curve, double knee, double white) noexcept {
    switch (curve) {
        case NeutralV0Curve::Shoulder:
            return knee;
        case NeutralV0Curve::Lift:
            return kNeutralV0LiftBreak * white;
        case NeutralV0Curve::Identity:
            break;
    }
    return white;
}

}  // namespace

std::string_view to_string(NeutralV0Curve curve) noexcept {
    switch (curve) {
        case NeutralV0Curve::Identity:
            return "identity";
        case NeutralV0Curve::Shoulder:
            return "shoulder";
        case NeutralV0Curve::Lift:
            return "lift";
    }
    return "invalid";
}

double neutral_v0_curve(double m, double knee, double white) noexcept {
    if (m >= white) {
        return 1.0;
    }
    const NeutralV0Curve curve = curve_for(white);
    if (m <= linear_limit(curve, knee, white)) {
        return m;
    }
    if (curve == NeutralV0Curve::Lift) {
        const std::array<double, 3> k = lift_coefficients(white);
        const double u = m / white;
        return (k[0] * u + k[1]) * u + k[2];
    }
    const double p = (white - knee) / (1.0 - knee);
    const double u = (m - knee) / (white - knee);
    return knee + (1.0 - knee) * (1.0 - std::pow(1.0 - u, p));
}

Status validate(const NeutralV0Parameters& parameters) {
    const auto ev_ok = [](double ev) { return std::isfinite(ev) && std::abs(ev) <= kMaxExposureEv; };
    if (!ev_ok(parameters.exposure_ev) || !ev_ok(parameters.baseline_exposure_ev)) {
        return fail(
            ErrorCode::InvalidArgument,
            std::format("exposure {} EV and baseline exposure {} EV must be finite and within +/-{} EV",
                        parameters.exposure_ev, parameters.baseline_exposure_ev, kMaxExposureEv),
            kWhere);
    }
    if (!std::isfinite(parameters.knee) || !(parameters.knee > 0.0) || !(parameters.knee < 1.0)) {
        return fail(
            ErrorCode::InvalidArgument,
            std::format("the highlight knee must lie strictly between 0 and 1 (got {})", parameters.knee),
            kWhere);
    }
    if (!std::isfinite(parameters.scene_white) || !(parameters.scene_white > 0.0)) {
        return fail(
            ErrorCode::InvalidArgument,
            std::format("the scene white must be finite and positive (got {})", parameters.scene_white),
            kWhere);
    }
    return ok();
}

Expected<NeutralV0Record> render_neutral_v0(ColorImage& image, const NeutralV0Parameters& parameters,
                                            const core::CancellationToken& cancel, bool parallel) {
    if (auto valid = validate(parameters); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    if (image.pixels.empty() || image.pixels.channels() != 3) {
        return fail(ErrorCode::InvalidArgument, "Neutral v0 needs a non-empty 3-channel image", kWhere);
    }
    if (image.encoding != image::ColorEncoding::scene_linear_rec2020_d65()) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("Neutral v0 renders scene-linear Rec.2020 / D65 (got {})",
                                image.encoding.to_string()),
                    kWhere);
    }
    NeutralV0Record record;
    record.parameters = parameters;
    record.gain = std::exp2(parameters.exposure_ev + parameters.baseline_exposure_ev);
    record.white = record.gain * parameters.scene_white;
    if (!std::isfinite(record.white) || !(record.white > 0.0)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the white point gain x scene white = {} is not a finite positive number",
                                record.white),
                    kWhere);
    }
    record.curve = curve_for(record.white);
    if (record.curve == NeutralV0Curve::Shoulder) {
        record.exponent = (record.white - parameters.knee) / (1.0 - parameters.knee);
    } else if (record.curve == NeutralV0Curve::Lift) {
        record.lift = lift_coefficients(record.white);
    }

    const float gain = static_cast<float>(record.gain);
    const double knee = parameters.knee;
    const double white = record.white;
    const NeutralV0Curve curve = record.curve;
    // Pixels with m at or below this level keep the plain gain (bit-exact).
    const double identity_limit = linear_limit(curve, knee, white);
    const image::ImageView<float> view = image.pixels.view();
    const std::int32_t width = view.width();
    std::vector<RowCounts> counts(static_cast<std::size_t>(view.height()));
    auto status = detail::for_each_row(view.height(), parallel, cancel, kWhere, [&](std::int32_t y) {
        RowCounts c;
        float* p = view.row(y);
        for (std::int32_t x = 0; x < width; ++x, p += 3) {
            float r = p[0] * gain;
            float g = p[1] * gain;
            float b = p[2] * gain;
            const double md = std::max({r, g, b});
            if (md > identity_limit || md >= white) {
                const float scale = static_cast<float>(neutral_v0_curve(md, knee, white) / md);
                if (md >= white) {
                    ++c.limited;
                } else if (curve == NeutralV0Curve::Lift) {
                    ++c.lifted;
                } else {
                    ++c.compressed;
                }
                r *= scale;
                g *= scale;
                b *= scale;
            }
            c.negative += static_cast<std::uint64_t>(r < 0.0f) + static_cast<std::uint64_t>(g < 0.0f) +
                          static_cast<std::uint64_t>(b < 0.0f);
            c.non_finite +=
                static_cast<std::uint64_t>(!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b));
            p[0] = r;
            p[1] = g;
            p[2] = b;
        }
        counts[static_cast<std::size_t>(y)] = c;
    });
    if (!status) {
        return Unexpected(std::move(status.error()));
    }
    std::uint64_t non_finite = 0;
    for (const RowCounts& c : counts) {
        record.compressed_pixels += c.compressed;
        record.lifted_pixels += c.lifted;
        record.limited_pixels += c.limited;
        record.negative_samples += c.negative;
        non_finite += c.non_finite;
    }
    if (non_finite != 0) {
        return fail(ErrorCode::Internal, std::format("Neutral v0 produced {} non-finite pixels", non_finite),
                    kWhere);
    }
    image.encoding = display_linear_rec2020();
    return record;
}

}  // namespace lucent::engine
