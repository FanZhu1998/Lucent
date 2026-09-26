#include "color_difference.hpp"

#include <cmath>
#include <numbers>

#include <lucent/image/color_math.hpp>

namespace lucent::integration {

namespace {

const image::Matrix3d& srgb_to_xyz() {
    static const image::Matrix3d m = *image::rgb_to_xyz(image::kRec709Primaries, image::kD65White);
    return m;
}

double deg(double radians) {
    return radians * 180.0 / std::numbers::pi;
}
double rad(double degrees) {
    return degrees * std::numbers::pi / 180.0;
}

}  // namespace

Lab lab_from_linear_srgb(const Rgb& rgb) {
    const image::Vec3d xyz = srgb_to_xyz() * image::Vec3d{rgb[0], rgb[1], rgb[2]};
    const image::Vec3d white = *image::xy_to_XYZ(image::kD65White);
    constexpr double delta = 6.0 / 29.0;
    const auto f = [](double t) {
        return t > delta * delta * delta ? std::cbrt(t) : t / (3.0 * delta * delta) + 4.0 / 29.0;
    };
    const double fx = f(xyz[0] / white[0]);
    const double fy = f(xyz[1] / white[1]);
    const double fz = f(xyz[2] / white[2]);
    return Lab{116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

double luminance_from_linear_srgb(const Rgb& rgb) {
    const image::Matrix3d& m = srgb_to_xyz();
    return m(1, 0) * rgb[0] + m(1, 1) * rgb[1] + m(1, 2) * rgb[2];
}

double ciede2000(const Lab& lab1, const Lab& lab2) {
    const double L1 = lab1[0], a1 = lab1[1], b1 = lab1[2];
    const double L2 = lab2[0], a2 = lab2[1], b2 = lab2[2];
    const double C1 = std::hypot(a1, b1);
    const double C2 = std::hypot(a2, b2);
    const double C_bar = (C1 + C2) / 2.0;
    const double C_bar7 = std::pow(C_bar, 7.0);
    const double G = 0.5 * (1.0 - std::sqrt(C_bar7 / (C_bar7 + std::pow(25.0, 7.0))));
    const double a1p = (1.0 + G) * a1;
    const double a2p = (1.0 + G) * a2;
    const double C1p = std::hypot(a1p, b1);
    const double C2p = std::hypot(a2p, b2);
    const auto hue = [](double b, double ap) {
        if (b == 0.0 && ap == 0.0) {
            return 0.0;
        }
        const double h = deg(std::atan2(b, ap));
        return h < 0.0 ? h + 360.0 : h;
    };
    const double h1p = hue(b1, a1p);
    const double h2p = hue(b2, a2p);

    const double dLp = L2 - L1;
    const double dCp = C2p - C1p;
    double dhp = 0.0;
    if (C1p * C2p != 0.0) {
        dhp = h2p - h1p;
        if (dhp > 180.0) {
            dhp -= 360.0;
        } else if (dhp < -180.0) {
            dhp += 360.0;
        }
    }
    const double dHp = 2.0 * std::sqrt(C1p * C2p) * std::sin(rad(dhp / 2.0));

    const double Lp_bar = (L1 + L2) / 2.0;
    const double Cp_bar = (C1p + C2p) / 2.0;
    double hp_bar = h1p + h2p;
    if (C1p * C2p != 0.0) {
        if (std::abs(h1p - h2p) <= 180.0) {
            hp_bar /= 2.0;
        } else if (h1p + h2p < 360.0) {
            hp_bar = (h1p + h2p + 360.0) / 2.0;
        } else {
            hp_bar = (h1p + h2p - 360.0) / 2.0;
        }
    }
    const double T = 1.0 - 0.17 * std::cos(rad(hp_bar - 30.0)) + 0.24 * std::cos(rad(2.0 * hp_bar)) +
                     0.32 * std::cos(rad(3.0 * hp_bar + 6.0)) - 0.20 * std::cos(rad(4.0 * hp_bar - 63.0));
    const double d_theta = 30.0 * std::exp(-std::pow((hp_bar - 275.0) / 25.0, 2.0));
    const double Cp_bar7 = std::pow(Cp_bar, 7.0);
    const double R_C = 2.0 * std::sqrt(Cp_bar7 / (Cp_bar7 + std::pow(25.0, 7.0)));
    const double S_L =
        1.0 + 0.015 * std::pow(Lp_bar - 50.0, 2.0) / std::sqrt(20.0 + std::pow(Lp_bar - 50.0, 2.0));
    const double S_C = 1.0 + 0.045 * Cp_bar;
    const double S_H = 1.0 + 0.015 * Cp_bar * T;
    const double R_T = -std::sin(rad(2.0 * d_theta)) * R_C;
    const double l = dLp / S_L;
    const double c = dCp / S_C;
    const double h = dHp / S_H;
    return std::sqrt(l * l + c * c + h * h + R_T * c * h);
}

}  // namespace lucent::integration
