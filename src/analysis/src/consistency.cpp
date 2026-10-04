#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <vector>

#include <lucent/analysis/consistency.hpp>
#include <lucent/engine/display_rendering.hpp>
#include <lucent/image/color_math.hpp>

namespace lucent::analysis {

namespace {

constexpr const char* kWhere = "analysis.consistency";

double sq(double v) noexcept {
    return v * v;
}

// FNV-1a over the bytes of fixed-width values: a stable fingerprint, not a security hash.
class Fingerprint {
public:
    void add(std::uint64_t value) noexcept {
        for (int k = 0; k < 8; ++k) {
            hash_ ^= (value >> (8 * k)) & 0xFFu;
            hash_ *= 0x100000001B3ull;
        }
    }
    // Rounded to 1e-6 so that the same calibration read through float or double hashes alike.
    void add(double value) noexcept {
        const double rounded = std::isfinite(value) ? std::round(value * 1e6) : 0.0;
        add(static_cast<std::uint64_t>(static_cast<std::int64_t>(rounded)));
    }
    void add(const image::Matrix3d& m) noexcept {
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t j = 0; j < 3; ++j) {
                add(m(i, j));
            }
        }
    }
    void add(std::string_view text) noexcept {
        add(static_cast<std::uint64_t>(text.size()));
        for (const char c : text) {
            add(static_cast<std::uint64_t>(static_cast<unsigned char>(c)));
        }
    }
    [[nodiscard]] std::uint64_t value() const noexcept { return hash_; }

private:
    std::uint64_t hash_ = 0xCBF29CE484222325ull;
};

std::string camera_name(const raw::CameraIdentity& c) {
    const std::string& make = c.normalized_make.empty() ? c.make : c.normalized_make;
    const std::string& model = c.normalized_model.empty() ? c.model : c.normalized_model;
    if (make.empty()) {
        return model.empty() ? std::string("unknown camera") : model;
    }
    return model.empty() ? make : make + " " + model;
}

// Days from 1970-01-01 of a proleptic Gregorian date (H. Hinnant's days_from_civil).
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

struct Cie1960 {
    double u = 0.0;
    double v = 0.0;
};

std::optional<Cie1960> uv_1960(image::Chromaticity xy) noexcept {
    const double d = -2.0 * xy.x + 12.0 * xy.y + 3.0;
    if (!(d > 0.0) || !std::isfinite(d)) {
        return std::nullopt;
    }
    return Cie1960{4.0 * xy.x / d, 6.0 * xy.y / d};
}

}  // namespace

UvPrime uv_prime(double X, double Y, double Z) noexcept {
    const double d = X + 15.0 * Y + 3.0 * Z;
    if (!(d > 0.0) || !std::isfinite(d)) {
        return {};
    }
    return {4.0 * X / d, 9.0 * Y / d};
}

std::optional<std::int64_t> parse_capture_time(std::string_view text) noexcept {
    // YYYY-MM-DDTHH:MM:SS (a space is accepted for the T).
    if (text.size() < 19) {
        return std::nullopt;
    }
    const auto digits = [&](std::size_t at, std::size_t n) -> std::optional<int> {
        int v = 0;
        for (std::size_t k = at; k < at + n; ++k) {
            if (text[k] < '0' || text[k] > '9') {
                return std::nullopt;
            }
            v = v * 10 + (text[k] - '0');
        }
        return v;
    };
    if (text[4] != '-' || text[7] != '-' || (text[10] != 'T' && text[10] != ' ') || text[13] != ':' ||
        text[16] != ':') {
        return std::nullopt;
    }
    const auto year = digits(0, 4);
    const auto month = digits(5, 2);
    const auto day = digits(8, 2);
    const auto hour = digits(11, 2);
    const auto minute = digits(14, 2);
    const auto second = digits(17, 2);
    if (!year || !month || !day || !hour || !minute || !second || *month < 1 || *month > 12 || *day < 1 ||
        *day > 31 || *hour > 23 || *minute > 59 || *second > 60 || *year == 0) {
        return std::nullopt;
    }
    const std::int64_t days =
        days_from_civil(*year, static_cast<unsigned>(*month), static_cast<unsigned>(*day));
    return days * 86400 + static_cast<std::int64_t>(*hour) * 3600 + static_cast<std::int64_t>(*minute) * 60 +
           *second;
}

std::string calibration_domain(const raw::RawMetadata& md) {
    const std::string camera = camera_name(md.camera);
    if (md.representation.kind == raw::RawRepresentation::Monochrome) {
        return "monochrome:" + camera;
    }
    Fingerprint f;
    f.add(std::string_view(md.camera.unique_camera_model));
    std::string route = "unknown";
    auto model = engine::WhiteBalanceModel::create(md);
    if (model) {
        route = std::string(engine::to_string(model->path()));
        f.add(static_cast<std::uint64_t>(model->path()));
        if (model->path() == engine::CalibrationPath::DngColorMatrix ||
            model->path() == engine::CalibrationPath::DngForwardMatrix) {
            if (auto profile = engine::dng_profile_from_metadata(md)) {
                for (std::size_t k = 0; k < 2; ++k) {
                    f.add(static_cast<std::uint64_t>(profile->illuminants[k]));
                    for (const auto* m : {&profile->color_matrix[k], &profile->camera_calibration[k],
                                          &profile->forward_matrix[k]}) {
                        f.add(static_cast<std::uint64_t>(m->has_value() ? 1 : 0));
                        if (m->has_value()) {
                            f.add(**m);
                        }
                    }
                }
                for (std::size_t c = 0; c < 3; ++c) {
                    f.add(profile->analog_balance[c]);
                }
            }
        } else {
            for (std::size_t i = 0; i < 3; ++i) {
                for (std::size_t j = 0; j < 3; ++j) {
                    f.add(static_cast<double>(md.color.xyz_to_camera[i][j]));
                }
            }
        }
    }
    return std::format("{}|{}|{:016x}", camera, route, f.value());
}

void describe_capture(const raw::RawMetadata& md, const engine::WhiteBalanceModel* model,
                      image::Size upright_full, ShotFeatures& s) {
    s.camera = camera_name(md.camera);
    s.calibration_domain = calibration_domain(md);
    s.size = upright_full;
    s.color = md.representation.kind != raw::RawRepresentation::Monochrome;
    if (md.capture.capture_time_local) {
        s.capture_time = *md.capture.capture_time_local;
        s.capture_seconds = parse_capture_time(s.capture_time);
    }
    const auto& c = md.capture;
    if (c.f_number && c.exposure_time_s && c.iso && *c.f_number > 0.0f && *c.exposure_time_s > 0.0f &&
        *c.iso > 0.0f && std::isfinite(*c.f_number) && std::isfinite(*c.exposure_time_s) &&
        std::isfinite(*c.iso)) {
        const double n = *c.f_number;
        s.exposure_ev100 = std::log2(n * n / static_cast<double>(*c.exposure_time_s)) -
                           std::log2(static_cast<double>(*c.iso) / 100.0);
    }
    if (model != nullptr && !model->monochrome() && model->kelvin_available()) {
        if (auto white = model->white_for(model->as_shot())) {
            if (auto uv = uv_1960(*white)) {
                s.white_u = uv->u;
                s.white_v = uv->v;
            }
            if (auto cct = engine::correlated_color_temperature(*white)) {
                s.white_kelvin = *cct;
            }
        }
    }
}

Status describe_scene(std::span<const float> signal, image::Size size, const engine::ColorImage& render,
                      ShotFeatures& s) {
    const auto n = static_cast<std::size_t>(std::max<std::int64_t>(0, size.area()));
    const auto& px = render.pixels;
    if (n == 0 || signal.size() != n || px.size() != size || px.channels() != 3) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("shot description needs a signal plane and a 3-channel render of {}x{}",
                                size.width, size.height),
                    kWhere);
    }
    auto to_xyz = image::rgb_to_xyz(image::kRec2020Primaries, image::kD65White);
    if (!to_xyz) {
        return fail(ErrorCode::Internal, "no Rec.2020 matrix", kWhere);
    }
    const image::Matrix3d& m = *to_xyz;

    // Median scene signal (deterministic: nth_element over a copy).
    std::vector<float> sorted(signal.begin(), signal.end());
    for (float& v : sorted) {
        v = std::isfinite(v) ? std::max(v, 0.0f) : 0.0f;
    }
    const auto mid = sorted.begin() + static_cast<std::ptrdiff_t>(n / 2);
    std::nth_element(sorted.begin(), mid, sorted.end());
    s.median_signal = *mid;
    sorted = {};
    if (s.exposure_ev100 && s.median_signal > 0.0) {
        s.scene_ev = *s.exposure_ev100 + std::log2(s.median_signal / kMidGreySignal);
    }

    // Grid cells: structure from the scene signal, colour from the as-shot render.
    std::array<double, kShotCells> sig{};
    std::array<double, kShotCells> count{};
    std::array<double, kShotCells> cx{};
    std::array<double, kShotCells> cy{};
    std::array<double, kShotCells> cz{};
    double tx = 0.0;
    double ty = 0.0;
    double tz = 0.0;
    std::array<double, kShotToneBins> tones{};
    const double median = std::max(s.median_signal, 1e-6);
    const double bin_width = (kShotToneHighEv - kShotToneLowEv) / static_cast<double>(kShotToneBins);
    for (std::int32_t y = 0; y < size.height; ++y) {
        const auto gy = static_cast<std::size_t>(
            std::min<std::int64_t>(kShotGrid - 1, static_cast<std::int64_t>(y) * kShotGrid / size.height));
        const float* row = px.row(y);
        for (std::int32_t x = 0; x < size.width; ++x, row += 3) {
            const auto gx = static_cast<std::size_t>(
                std::min<std::int64_t>(kShotGrid - 1, static_cast<std::int64_t>(x) * kShotGrid / size.width));
            const std::size_t cell = gy * kShotGrid + gx;
            const std::size_t i =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(size.width) + static_cast<std::size_t>(x);
            const double v = std::isfinite(signal[i]) ? std::max(0.0, static_cast<double>(signal[i])) : 0.0;
            sig[cell] += v;
            count[cell] += 1.0;
            const double ev = std::log2(std::max(v, 1e-6) / median);
            const auto bin = static_cast<std::size_t>(
                std::clamp((ev - kShotToneLowEv) / bin_width, 0.0, static_cast<double>(kShotToneBins - 1)));
            tones[bin] += 1.0;
            const double r = row[0];
            const double g = row[1];
            const double b = row[2];
            if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) {
                continue;
            }
            const double X = m(0, 0) * r + m(0, 1) * g + m(0, 2) * b;
            const double Y = m(1, 0) * r + m(1, 1) * g + m(1, 2) * b;
            const double Z = m(2, 0) * r + m(2, 1) * g + m(2, 2) * b;
            cx[cell] += X;
            cy[cell] += Y;
            cz[cell] += Z;
            tx += X;
            ty += Y;
            tz += Z;
        }
    }
    std::array<double, kShotCells> logs{};
    for (std::size_t k = 0; k < kShotCells; ++k) {
        logs[k] = std::log2(std::max(count[k] > 0.0 ? sig[k] / count[k] : 0.0, 1e-6));
    }
    std::array<double, kShotCells> ordered = logs;
    std::sort(ordered.begin(), ordered.end());
    const double median_cell = 0.5 * (ordered[kShotCells / 2 - 1] + ordered[kShotCells / 2]);
    const UvPrime whole = uv_prime(tx, ty, tz);
    for (std::size_t k = 0; k < kShotCells; ++k) {
        s.structure[k] = static_cast<float>(logs[k] - median_cell);
        // Cells far below the image's mean luminance have no reliable colour.
        if (count[k] > 0.0 && cy[k] / count[k] > 1e-4 * std::max(ty / static_cast<double>(n), 1e-12)) {
            const UvPrime c = uv_prime(cx[k], cy[k], cz[k]);
            s.colour_u[k] = static_cast<float>(c.u - whole.u);
            s.colour_v[k] = static_cast<float>(c.v - whole.v);
        } else {
            s.colour_u[k] = 0.0f;
            s.colour_v[k] = 0.0f;
        }
    }
    for (std::size_t k = 0; k < kShotToneBins; ++k) {
        s.tones[k] = static_cast<float>(tones[k] / static_cast<double>(n));
    }
    s.descriptors = true;
    return ok();
}

ConsistencyTerms consistency_term(const Appearance& a, const ConsistencyTarget& t) noexcept {
    ConsistencyTerms out;
    if (!a.valid) {
        return out;
    }
    out.lightness = sq(std::max(0.0, std::abs(a.lightness - t.lightness) - kConsistencyLightnessDeadZone) /
                       kConsistencyLightnessScale);
    if (t.colour && a.colour) {
        const double d = std::hypot(a.u - t.u, a.v - t.v);
        out.colour = sq(std::max(0.0, d - kConsistencyColourDeadZone) / kConsistencyColourScale);
    }
    if (t.camera_white_balance && a.camera_gains) {
        const double d = std::hypot(a.log_red - t.log_red, a.log_blue - t.log_blue);
        out.white_balance =
            sq(std::max(0.0, d - kConsistencyWhiteBalanceDeadZone) / kConsistencyWhiteBalanceScale);
    }
    out.total = t.weight * (out.lightness + out.colour + out.white_balance);
    return out;
}

}  // namespace lucent::analysis
