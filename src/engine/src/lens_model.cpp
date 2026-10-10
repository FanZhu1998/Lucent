// Lensfun's coordinate model in double precision: the normalisation, the distortion, lateral
// chromatic aberration and vignetting models and the point mapping (lens_correction.hpp, section
// 3). No Lensfun type is used here; lens_correction_test.cpp checks these against lfModifier.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>

#include <lucent/engine/lens_correction.hpp>

namespace lucent::engine {

namespace {

constexpr const char* kWhereModel = "engine.lens.model";

class Fnv1a {
public:
    void add(std::uint64_t v) noexcept {
        for (int i = 0; i < 8; ++i) {
            hash_ ^= (v >> (8 * i)) & 0xFFu;
            hash_ *= 0x100000001b3ull;
        }
    }
    void add(double v) noexcept { add(std::bit_cast<std::uint64_t>(v == 0.0 ? 0.0 : v)); }
    [[nodiscard]] std::uint64_t value() const noexcept { return hash_; }

private:
    std::uint64_t hash_ = 0xcbf29ce484222325ull;
};

void add_frame(Fnv1a& h, const LensfunFrame& f) noexcept {
    h.add(static_cast<std::uint64_t>(f.size.width));
    h.add(static_cast<std::uint64_t>(f.size.height));
    h.add(f.crop_factor);
    h.add(f.calibration_crop_factor);
    h.add(f.calibration_aspect_ratio);
    h.add(f.lens_center_x);
    h.add(f.lens_center_y);
}

// The radial factor of the distortion model at the undistorted (scaled) radius squared ru2:
// r_d = r_u * factor.
double distortion_factor(const LensDistortionCalibration& d, double ru2) noexcept {
    const auto& k = d.terms;
    switch (d.model) {
        case LensDistortionModel::Poly3:
            return 1.0 - k[0] + k[0] * ru2;
        case LensDistortionModel::Poly5:
            return 1.0 + k[0] * ru2 + k[1] * ru2 * ru2;
        case LensDistortionModel::PTLens: {
            const double r = std::sqrt(ru2);
            return k[0] * ru2 * r + k[1] * ru2 + k[2] * r + 1.0 - k[0] - k[1] - k[2];
        }
    }
    return 1.0;
}

// d(r factor(r^2)) / dr: the radial derivative of r_d at r.
double distortion_derivative(const LensDistortionCalibration& d, double r) noexcept {
    const auto& k = d.terms;
    const double r2 = r * r;
    switch (d.model) {
        case LensDistortionModel::Poly3:
            return 1.0 - k[0] + 3.0 * k[0] * r2;
        case LensDistortionModel::Poly5:
            return 1.0 + 3.0 * k[0] * r2 + 5.0 * k[1] * r2 * r2;
        case LensDistortionModel::PTLens:
            return 4.0 * k[0] * r2 * r + 3.0 * k[1] * r2 + 2.0 * k[2] * r + 1.0 - k[0] - k[1] - k[2];
    }
    return 1.0;
}

// The radial factor of channel `channel`'s lateral CA at the distorted radius squared r2 (green: 1).
double tca_factor(const LensTcaCalibration& t, double r2, std::uint32_t channel) noexcept {
    if (channel == 1) {
        return 1.0;
    }
    const bool red = channel == 0;
    const auto& k = t.terms;
    if (t.model == LensTcaModel::Linear) {
        return red ? k[0] : k[1];
    }
    // Lensfun's poly3 order: vr, vb, cr, cb, br, bb.
    const double v = red ? k[0] : k[1];
    const double c = red ? k[2] : k[3];
    const double b = red ? k[4] : k[5];
    if (c == 0.0) {
        return b * r2 + v;
    }
    return b * r2 + c * std::sqrt(r2) + v;
}

}  // namespace

// ---- LensfunFrame ---------------------------------------------------------------------------------

Expected<LensfunFrame> LensfunFrame::create(image::Size stored_size, double image_crop, double calibration_crop,
                                            double calibration_aspect, double centre_x, double centre_y) {
    const auto positive = [](double v) { return std::isfinite(v) && v > 0.0; };
    if (stored_size.width < 2 || stored_size.height < 2 || !positive(image_crop) || !positive(calibration_crop) ||
        !positive(calibration_aspect) || !std::isfinite(centre_x) || !std::isfinite(centre_y)) {
        return fail(
            ErrorCode::InvalidArgument,
            std::format("a lens model needs an image of at least 2 x 2 pixels (got {} x {}) and finite, "
                        "positive crop factors and aspect ratio",
                        stored_size.width, stored_size.height),
            kWhereModel);
    }
    LensfunFrame f;
    f.size = stored_size;
    f.crop_factor = image_crop;
    f.calibration_crop_factor = calibration_crop;
    f.calibration_aspect_ratio = calibration_aspect;
    f.lens_center_x = centre_x;
    f.lens_center_y = centre_y;
    // lfModifier::lfModifier: the extent is measured between the outer pixel centres.
    const double w = static_cast<double>(stored_size.width - 1);
    const double h = static_cast<double>(stored_size.height - 1);
    const double extent = std::min(w, h);
    const double image_aspect = std::max(w, h) / extent;
    f.aspect_ratio_correction = std::sqrt(calibration_aspect * calibration_aspect + 1.0);
    const double correction = 1.0 / std::sqrt(image_aspect * image_aspect + 1.0) * calibration_crop / image_crop *
                              f.aspect_ratio_correction;
    f.norm_scale = 2.0 / extent * correction;
    f.center_x = (w / extent + centre_x) * correction;
    f.center_y = (h / extent + centre_y) * correction;
    return f;
}

image::PointD LensfunFrame::normalized(image::PointD stored_point) const noexcept {
    // Lensfun's pixel coordinates have the pixel centres at integers.
    return image::PointD{(stored_point.x - 0.5) * norm_scale - center_x,
                         (stored_point.y - 0.5) * norm_scale - center_y};
}

image::PointD LensfunFrame::stored(image::PointD n) const noexcept {
    return image::PointD{(n.x + center_x) / norm_scale + 0.5, (n.y + center_y) / norm_scale + 0.5};
}

// ---- LensGeometryMap ------------------------------------------------------------------------------

image::PointD LensGeometryMap::normalized_source(image::PointD n, std::uint32_t channel) const noexcept {
    // The coordinate callbacks in Lensfun's priority order (scale, then distortion), then the
    // subpixel callback (lateral CA) on the distorted position.
    double x = n.x / scale;
    double y = n.y / scale;
    if (distortion) {
        const double f = distortion_factor(*distortion, x * x + y * y);
        x *= f;
        y *= f;
    }
    if (tca) {
        const double f = tca_factor(*tca, x * x + y * y, channel);
        x *= f;
        y *= f;
    }
    return image::PointD{x, y};
}

image::PointD LensGeometryMap::source_of(image::PointD corrected, std::uint32_t channel) const noexcept {
    const image::Size stored_size = frame.size;
    const image::PointD stored = image::map_from_oriented(corrected, stored_size, orientation);
    const image::PointD source = frame.stored(normalized_source(frame.normalized(stored), channel));
    return image::map_to_oriented(source, stored_size, orientation);
}

std::optional<image::PointD> LensGeometryMap::corrected_of(image::PointD source) const noexcept {
    const image::Size stored_size = frame.size;
    const image::PointD s = frame.normalized(image::map_from_oriented(source, stored_size, orientation));
    const double rd = std::hypot(s.x, s.y);
    double ru = rd;  // the scaled undistorted radius
    if (distortion && rd > 0.0) {
        bool converged = false;
        for (int step = 0; step < 50; ++step) {
            const double value = ru * distortion_factor(*distortion, ru * ru) - rd;
            const double slope = distortion_derivative(*distortion, ru);
            if (!std::isfinite(value) || !std::isfinite(slope) || !(slope > 0.0)) {
                return std::nullopt;
            }
            const double next = ru - value / slope;
            if (!(next >= 0.0)) {
                return std::nullopt;
            }
            if (std::abs(next - ru) <= 1e-12 * std::max(1.0, ru)) {
                ru = next;
                converged = true;
                break;
            }
            ru = next;
        }
        if (!converged) {
            return std::nullopt;
        }
    }
    const double k = rd > 0.0 ? ru / rd * scale : scale;
    const image::PointD n{s.x * k, s.y * k};
    return image::map_to_oriented(frame.stored(n), stored_size, orientation);
}

std::uint64_t LensGeometryMap::revision() const noexcept {
    Fnv1a h;
    h.add(std::uint64_t{kLensCorrectionOperationVersion});
    add_frame(h, frame);
    h.add(std::uint64_t{static_cast<std::uint8_t>(orientation)});
    h.add(static_cast<std::uint64_t>(upright_size.width));
    h.add(static_cast<std::uint64_t>(upright_size.height));
    h.add(scale);
    if (distortion) {
        h.add(std::uint64_t{static_cast<std::uint8_t>(distortion->model)});
        for (const double t : distortion->terms) {
            h.add(t);
        }
    } else {
        h.add(std::uint64_t{0});
    }
    if (tca) {
        h.add(std::uint64_t{static_cast<std::uint8_t>(tca->model)});
        for (const double t : tca->terms) {
            h.add(t);
        }
    } else {
        h.add(std::uint64_t{0});
    }
    return h.value();
}

// ---- LensVignettingCorrection ---------------------------------------------------------------------

double LensVignettingCorrection::gain_at_normalized(image::PointD n) const noexcept {
    const double a = frame.aspect_ratio_correction;
    const double r2 = (n.x * n.x + n.y * n.y) / (a * a);
    const auto& k = calibration.terms;
    return 1.0 / (1.0 + k[0] * r2 + k[1] * r2 * r2 + k[2] * r2 * r2 * r2);
}

double LensVignettingCorrection::gain_at(image::PointD upright_source) const noexcept {
    const image::PointD stored = image::map_from_oriented(upright_source, frame.size, orientation);
    return gain_at_normalized(frame.normalized(stored));
}

// ---- LensCorrectionPlan ---------------------------------------------------------------------------

std::uint64_t LensCorrectionPlan::revision() const noexcept {
    if (!any_applied()) {
        return 0;
    }
    Fnv1a h;
    h.add(std::uint64_t{operation_version});
    h.add(geometry ? geometry->revision() : std::uint64_t{0});
    if (vignetting) {
        add_frame(h, vignetting->frame);
        h.add(std::uint64_t{static_cast<std::uint8_t>(vignetting->orientation)});
        for (const double t : vignetting->calibration.terms) {
            h.add(t);
        }
    } else {
        h.add(std::uint64_t{0});
    }
    return h.value();
}

}  // namespace lucent::engine
