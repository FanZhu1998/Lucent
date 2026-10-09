#include "colour_reference.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numeric>
#include <utility>

#include <lucent/engine/develop.hpp>

#include "color_difference.hpp"

namespace lucent::qualify {

namespace {

constexpr std::string_view kWhere = "qualify.colour_reference";

ChartStatistics statistics_of(const std::vector<double>& values) {
    ChartStatistics s;
    if (values.empty()) {
        return s;
    }
    s.mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    s.p90 = quantile(values, 0.9);
    s.max = *std::max_element(values.begin(), values.end());
    return s;
}

double delta_e(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return integration::ciede2000(integration::Lab{a[0], a[1], a[2]}, integration::Lab{b[0], b[1], b[2]});
}

const image::Vec3d& d50_white() {
    static const image::Vec3d white = *image::xy_to_XYZ(image::kD50White);
    return white;
}

// As-shot and chart-balanced CIELAB (D50) of the measured XYZ against `reference` (sample order).
struct Labs {
    std::vector<std::array<double, 3>> as_shot;
    std::vector<std::array<double, 3>> balanced;
    double scale = 0.0;
};

Labs labs_against(const std::vector<image::Vec3d>& measured_xyz, const std::vector<std::array<double, 3>>& reference,
                  std::size_t normalisation, const image::Vec3d& working_white) {
    Labs out;
    const image::Vec3d ref_n = xyz_from_lab(reference[normalisation], d50_white());
    const image::Vec3d& meas_n = measured_xyz[normalisation];
    // As shot: the rendering's white adapted to D50 (Bradford), then one exposure scale that gives
    // the normalisation patch its reference Y.
    const image::Matrix3d to_d50 =
        *image::chromatic_adaptation(working_white, d50_white(), image::ChromaticAdaptationMethod::Bradford);
    out.scale = ref_n[1] / (to_d50 * meas_n)[1];
    // Chart balanced: the measured normalisation patch mapped exactly onto its reference XYZ.
    const image::Matrix3d balance =
        *image::chromatic_adaptation(meas_n, ref_n, image::ChromaticAdaptationMethod::Bradford);
    for (const image::Vec3d& xyz : measured_xyz) {
        out.as_shot.push_back(lab_from_xyz(out.scale * (to_d50 * xyz), d50_white()));
        out.balanced.push_back(lab_from_xyz(balance * xyz, d50_white()));
    }
    return out;
}

}  // namespace

double ChartSample::clipped_fraction() const noexcept {
    return pixels == 0 ? 0.0 : static_cast<double>(clipped_samples) / (3.0 * static_cast<double>(pixels));
}

std::string ChartEvaluation::summary() const {
    if (!measurable) {
        return "the chart is not measurable: " + problem;
    }
    return std::format(
        "{} patches against the published values, chart-balanced CIEDE2000 mean {:.2f} (limit {:.1f}), 90th "
        "percentile {:.2f} (limit {:.1f}), max {:.2f} (limit {:.1f}); as shot mean {:.2f}, max {:.2f}{}",
        patches.size(), balanced.mean, kColourReferenceMeanLimit, balanced.p90, kColourReferenceP90Limit, balanced.max,
        kColourReferenceMaxLimit, as_shot.mean, as_shot.max,
        excluded.empty() ? std::string() : std::format("; {} clipped patches excluded", excluded.size()));
}

bool within_colour_reference_gate(const ChartStatistics& balanced) noexcept {
    return balanced.mean <= kColourReferenceMeanLimit && balanced.p90 <= kColourReferenceP90Limit &&
           balanced.max <= kColourReferenceMaxLimit;
}

double quantile(std::vector<double> values, double q) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double position = std::clamp(q, 0.0, 1.0) * static_cast<double>(values.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const std::size_t upper = std::min(lower + 1, values.size() - 1);
    const double fraction = position - static_cast<double>(lower);
    return values[lower] + (values[upper] - values[lower]) * fraction;
}

std::array<double, 3> lab_from_xyz(const image::Vec3d& xyz, const image::Vec3d& white) noexcept {
    constexpr double delta = 6.0 / 29.0;
    const auto f = [](double t) {
        return t > delta * delta * delta ? std::cbrt(t) : t / (3.0 * delta * delta) + 4.0 / 29.0;
    };
    const double fx = f(xyz[0] / white[0]);
    const double fy = f(xyz[1] / white[1]);
    const double fz = f(xyz[2] / white[2]);
    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

image::Vec3d xyz_from_lab(const std::array<double, 3>& lab, const image::Vec3d& white) noexcept {
    constexpr double delta = 6.0 / 29.0;
    const auto finv = [](double t) { return t > delta ? t * t * t : 3.0 * delta * delta * (t - 4.0 / 29.0); };
    const double fy = (lab[0] + 16.0) / 116.0;
    const double fx = fy + lab[1] / 500.0;
    const double fz = fy - lab[2] / 200.0;
    return image::Vec3d{white[0] * finv(fx), white[1] * finv(fy), white[2] * finv(fz)};
}

const image::Matrix3d& rec2020_to_xyz() {
    static const image::Matrix3d m = *image::rgb_to_xyz(image::kRec2020Primaries, image::kD65White);
    return m;
}

ChartEvaluation evaluate_chart(const ChartDefinitions& charts, const ChartFixture& chart,
                               const std::vector<ChartSample>& samples, const image::Matrix3d& working_to_xyz,
                               image::Chromaticity working_white) {
    ChartEvaluation e;
    const ChartReference* reference = charts.reference(chart.reference);
    if (reference == nullptr) {
        e.problem = "unknown reference " + chart.reference;
        return e;
    }
    auto white = image::xy_to_XYZ(working_white);
    if (!white) {
        e.problem = "invalid working white";
        return e;
    }
    std::vector<const ChartSample*> measured;
    for (const ChartSample& s : samples) {
        if (reference->find(s.patch) == nullptr) {
            continue;
        }
        if (s.pixels == 0) {
            e.problem = "patch " + s.patch + " has no pixels";
            return e;
        }
        if (s.clipped_fraction() > kColourReferenceClippedFraction) {
            e.excluded[s.patch] = s.clipped_fraction();
            continue;
        }
        measured.push_back(&s);
    }
    const auto normalisation = std::find_if(measured.begin(), measured.end(), [&](const ChartSample* s) {
        return s->patch == chart.normalisation_patch;
    });
    if (normalisation == measured.end()) {
        e.problem = "the normalisation patch " + chart.normalisation_patch +
                    (e.excluded.contains(chart.normalisation_patch) ? " is clipped" : " was not measured");
        return e;
    }
    if (static_cast<double>(measured.size()) <
        kColourReferenceMinMeasured * static_cast<double>(reference->patches.size())) {
        e.problem = std::format("only {} of {} patches are measured", measured.size(), reference->patches.size());
        return e;
    }
    std::vector<image::Vec3d> xyz;
    std::vector<std::array<double, 3>> ref_lab;
    for (const ChartSample* s : measured) {
        xyz.push_back(working_to_xyz * s->working_mean);
        ref_lab.push_back(reference->find(s->patch)->lab);
    }
    if (!(xyz[static_cast<std::size_t>(normalisation - measured.begin())][1] > 0.0)) {
        e.problem = "the normalisation patch has no positive luminance";
        return e;
    }
    const std::size_t n = static_cast<std::size_t>(normalisation - measured.begin());
    const Labs labs = labs_against(xyz, ref_lab, n, *white);
    e.normalisation_scale = labs.scale;
    std::vector<double> as_shot_de;
    std::vector<double> balanced_de;
    for (std::size_t i = 0; i < measured.size(); ++i) {
        ChartPatchResult p;
        p.patch = measured[i]->patch;
        p.reference_lab = ref_lab[i];
        p.as_shot_lab = labs.as_shot[i];
        p.balanced_lab = labs.balanced[i];
        p.as_shot_delta_e = delta_e(p.as_shot_lab, p.reference_lab);
        p.balanced_delta_e = delta_e(p.balanced_lab, p.reference_lab);
        as_shot_de.push_back(p.as_shot_delta_e);
        balanced_de.push_back(p.balanced_delta_e);
        e.patches.push_back(std::move(p));
    }
    e.as_shot = statistics_of(as_shot_de);
    e.balanced = statistics_of(balanced_de);
    for (const std::string& alt_id : chart.alternate_references) {
        const ChartReference* alt = charts.reference(alt_id);
        if (alt == nullptr) {
            continue;
        }
        std::vector<std::array<double, 3>> alt_lab;
        bool complete = true;
        for (const ChartSample* s : measured) {
            const ChartPatch* p = alt->find(s->patch);
            complete = complete && p != nullptr;
            alt_lab.push_back(p != nullptr ? p->lab : std::array<double, 3>{});
        }
        if (!complete) {
            continue;
        }
        const Labs alt_labs = labs_against(xyz, alt_lab, n, *white);
        std::vector<double> a;
        std::vector<double> b;
        for (std::size_t i = 0; i < measured.size(); ++i) {
            a.push_back(delta_e(alt_labs.as_shot[i], alt_lab[i]));
            b.push_back(delta_e(alt_labs.balanced[i], alt_lab[i]));
        }
        e.alternates[alt_id] = {statistics_of(a), statistics_of(b)};
    }
    e.measurable = true;
    e.passes = within_colour_reference_gate(e.balanced);
    return e;
}

Expected<ChartMeasurement> measure_chart(const raw::DecoderRegistry& registry, const std::filesystem::path& file,
                                         const ChartFixture& chart) {
    const engine::DevelopSettings settings;  // as shot (DevelopSettings{}), geometry version 2
    engine::StageOneRequest request;
    request.kind = engine::StageOneKind::Export;  // as render_develop_file() renders an export
    request.white_balance = settings.white_balance;
    auto one = engine::render_stage_one_file(registry, file, 0u, request, {}, {});
    if (!one) {
        return Unexpected(std::move(one.error()));
    }
    if (one->pixels.channels() != 3) {
        return fail(ErrorCode::InvalidArgument, "the chart's image is not 3-channel colour", std::string(kWhere));
    }
    ChartMeasurement m;
    {
        // Stage two's report: the matrices, gain, clip and frame of the develop render (its pixels
        // are released at once; only operation 1's values are measured).
        auto two = engine::render_stage_two(*one, settings, {}, {});
        if (!two) {
            return Unexpected(std::move(two.error()));
        }
        const engine::DevelopReport& r = two->report;
        m.frame = r.geometry_frame;
        m.gain = r.gain;
        m.clip_level = r.clip_level;
        m.correction = image::Vec3d{r.correction[0], r.correction[1], r.correction[2]};
        m.calibration_path = std::string(engine::to_string(r.color.path));
        const image::Matrix3d fused = r.working_space_matrix * r.color.camera_to_xyz;
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t j = 0; j < 3; ++j) {
                m.camera_to_working(i, j) = fused(i, j) * r.gain;
            }
        }
    }
    if (m.frame.width != chart.frame_size[0] || m.frame.height != chart.frame_size[1] ||
        m.frame.x != chart.frame_origin[0] || m.frame.y != chart.frame_origin[1]) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the develop render's frame {}x{}+{}+{} differs from the chart definition's {}x{}+{}+{}: "
                                "the sample boxes no longer locate the patches (re-locate the chart, charts.json)",
                                m.frame.width, m.frame.height, m.frame.x, m.frame.y, chart.frame_size[0],
                                chart.frame_size[1], chart.frame_origin[0], chart.frame_origin[1]),
                    std::string(kWhere));
    }
    // Operation 1 per pixel is out = M * min(s * d, clip); its mean over a box is M * mean(min(s * d, clip)).
    const float clip = static_cast<float>(m.clip_level);
    const std::array<float, 3> d{static_cast<float>(m.correction[0]), static_cast<float>(m.correction[1]),
                                 static_cast<float>(m.correction[2])};
    const float clip_test = clip * (1.0f - 1e-6f);
    for (const ChartBox& box : chart.boxes) {
        ChartSample s;
        s.patch = box.patch;
        std::array<double, 3> sum{};
        for (std::int32_t y = 0; y < box.height; ++y) {
            const float* row = one->pixels.row(m.frame.y + box.y + y) +
                               static_cast<std::ptrdiff_t>(m.frame.x + box.x) * 3;
            for (std::int32_t x = 0; x < box.width; ++x, row += 3) {
                for (std::size_t c = 0; c < 3; ++c) {
                    const float v = row[c] * d[c];
                    if (v >= clip_test) {
                        ++s.clipped_samples;
                    }
                    sum[c] += static_cast<double>(std::min(v, clip));
                }
            }
        }
        s.pixels = static_cast<std::uint64_t>(box.width) * static_cast<std::uint64_t>(box.height);
        const double count = static_cast<double>(s.pixels);
        s.camera_mean = image::Vec3d{sum[0] / count, sum[1] / count, sum[2] / count};
        s.working_mean = m.camera_to_working * s.camera_mean;
        m.samples.push_back(std::move(s));
    }
    return m;
}

}  // namespace lucent::qualify
