// Develop pipeline on camera fixtures (tools\fetch-fixtures.ps1); every case skips a fixture that
// is not downloaded.
//
//   * KelvinRoundTrips: on every renderable colour fixture, the as-shot multipliers map to a
//     temperature/tint and back to themselves (DNG profiles: dual-illuminant interpolation and
//     ForwardMatrix files; decoder matrices: LibRaw's cam_xyz).
//   * EveryKelvinSettingRendersOnEveryDngCalibration: stage two renders the DevelopSettings Kelvin
//     range (2000 to 50000 K, tints -150 to +150) on every DNG fixture, ColorMatrix and
//     ForwardMatrix, with finite pixels.
//   * InteractiveMatchesExport: the interactive path (cached as-shot stage one, then stage two)
//     against the export render (the settings' white balance before demosaicing), at full
//     resolution and as a 2048-pixel proxy against the export box-downscaled to the proxy grid.
//     CIEDE2000 of display-linear Rec.2020 (D65 white, unclipped), statistics over every pixel of
//     the compared grid. Gates are stated at the comparison.
//   * MasksDenoiseAndGeometryOnTheR5: DevelopSettings v2 (three masks, chroma denoising, geometry):
//     stage-two timings on the 2048-pixel proxy and the interactive renders against the export.
//   * DISABLED_Benchmark: stage timings and memory for the performance report (run with
//     --gtest_also_run_disabled_tests --gtest_filter=*Benchmark*).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>
#include <psapi.h>

#include <lucent/engine/develop.hpp>
#include <lucent/image/color_math.hpp>
#include <lucent/raw/decoder_registry.hpp>

#include "develop_test_support.hpp"

namespace lucent::engine {
namespace {

namespace ts = test_support;
using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }

// User + kernel CPU time of the whole process, seconds (all threads).
double process_cpu_seconds() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return 0.0;
    }
    const auto ticks = [](const FILETIME& t) {
        return static_cast<double>((static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime);
    };
    return (ticks(kernel) + ticks(user)) * 1e-7;
}

double peak_working_set_mb() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters))) {
        return static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0);
    }
    return 0.0;
}

// ---- CIEDE2000 ------------------------------------------------------------------------------------

struct Lab {
    double l = 0.0;
    double a = 0.0;
    double b = 0.0;
};

double lab_f(double t) {
    constexpr double e = 216.0 / 24389.0;
    return t > e ? std::cbrt(t) : t / (3.0 * (6.0 / 29.0) * (6.0 / 29.0)) + 4.0 / 29.0;
}

// G. Sharma, W. Wu and E. N. Dalal, "The CIEDE2000 color-difference formula: implementation notes,
// supplementary test data, and mathematical observations", Color Res. Appl. 30(1), 2005.
double ciede2000(const Lab& x, const Lab& y) {
    constexpr double kDeg = 180.0 / std::numbers::pi;
    const double c1 = std::hypot(x.a, x.b);
    const double c2 = std::hypot(y.a, y.b);
    const double cbar = 0.5 * (c1 + c2);
    const double c7 = std::pow(cbar, 7.0);
    const double g = 0.5 * (1.0 - std::sqrt(c7 / (c7 + std::pow(25.0, 7.0))));
    const double a1 = (1.0 + g) * x.a;
    const double a2 = (1.0 + g) * y.a;
    const double cp1 = std::hypot(a1, x.b);
    const double cp2 = std::hypot(a2, y.b);
    const auto hue = [&](double b, double a) {
        if (a == 0.0 && b == 0.0) {
            return 0.0;
        }
        const double h = std::atan2(b, a) * kDeg;
        return h < 0.0 ? h + 360.0 : h;
    };
    const double h1 = hue(x.b, a1);
    const double h2 = hue(y.b, a2);
    const double dl = y.l - x.l;
    const double dc = cp2 - cp1;
    double dh = 0.0;
    if (cp1 * cp2 != 0.0) {
        dh = h2 - h1;
        if (dh > 180.0) {
            dh -= 360.0;
        } else if (dh < -180.0) {
            dh += 360.0;
        }
    }
    const double dH = 2.0 * std::sqrt(cp1 * cp2) * std::sin(dh / (2.0 * kDeg));
    const double lbar = 0.5 * (x.l + y.l);
    const double cpbar = 0.5 * (cp1 + cp2);
    double hbar = h1 + h2;
    if (cp1 * cp2 != 0.0) {
        if (std::abs(h1 - h2) <= 180.0) {
            hbar = 0.5 * (h1 + h2);
        } else {
            hbar = h1 + h2 < 360.0 ? 0.5 * (h1 + h2 + 360.0) : 0.5 * (h1 + h2 - 360.0);
        }
    }
    const double t = 1.0 - 0.17 * std::cos((hbar - 30.0) / kDeg) + 0.24 * std::cos(2.0 * hbar / kDeg) +
                     0.32 * std::cos((3.0 * hbar + 6.0) / kDeg) - 0.20 * std::cos((4.0 * hbar - 63.0) / kDeg);
    const double dtheta = 30.0 * std::exp(-std::pow((hbar - 275.0) / 25.0, 2.0));
    const double cp7 = std::pow(cpbar, 7.0);
    const double rc = 2.0 * std::sqrt(cp7 / (cp7 + std::pow(25.0, 7.0)));
    const double sl = 1.0 + 0.015 * (lbar - 50.0) * (lbar - 50.0) / std::sqrt(20.0 + (lbar - 50.0) * (lbar - 50.0));
    const double sc = 1.0 + 0.045 * cpbar;
    const double sh = 1.0 + 0.015 * cpbar * t;
    const double rt = -std::sin(2.0 * dtheta / kDeg) * rc;
    const double tl = dl / sl;
    const double tc = dc / sc;
    const double th = dH / sh;
    return std::sqrt(tl * tl + tc * tc + th * th + rt * tc * th);
}

class LabConverter {
public:
    LabConverter() {
        to_xyz_ = *image::rgb_to_xyz(image::kRec2020Primaries, image::kD65White);
        white_ = *image::xy_to_XYZ(image::kD65White);
    }
    Lab operator()(const float* rgb) const {
        const image::Vec3d xyz = to_xyz_ * image::Vec3d{rgb[0], rgb[1], rgb[2]};
        const double fx = lab_f(xyz[0] / white_[0]);
        const double fy = lab_f(xyz[1] / white_[1]);
        const double fz = lab_f(xyz[2] / white_[2]);
        return Lab{116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
    }

private:
    image::Matrix3d to_xyz_;
    image::Vec3d white_;
};

struct DeltaE {
    double mean = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double max = 0.0;
};

// CIEDE2000 statistics over the pixels (x, y) with x % stride == 0 and y % stride == 0.
DeltaE compare(const image::ImageBuffer<float>& a, const image::ImageBuffer<float>& b, std::int32_t stride = 1) {
    EXPECT_EQ(a.size(), b.size());
    const LabConverter lab;
    std::vector<double> values;
    values.reserve(static_cast<std::size_t>(a.size().area() / (stride * stride) + a.width() + a.height()));
    for (std::int32_t y = 0; y < a.height(); y += stride) {
        const float* pa = a.row(y);
        const float* pb = b.row(y);
        for (std::int32_t x = 0; x < a.width(); x += stride) {
            values.push_back(ciede2000(lab(pa + 3 * x), lab(pb + 3 * x)));
        }
    }
    DeltaE d;
    if (values.empty()) {
        return d;
    }
    double sum = 0.0;
    for (const double v : values) {
        sum += v;
    }
    d.mean = sum / static_cast<double>(values.size());
    std::sort(values.begin(), values.end());
    d.p95 = values[static_cast<std::size_t>(0.95 * static_cast<double>(values.size() - 1))];
    d.p99 = values[static_cast<std::size_t>(0.99 * static_cast<double>(values.size() - 1))];
    d.max = values.back();
    return d;
}

image::ImageBuffer<float> downscaled(const image::ImageBuffer<float>& source, image::Size size) {
    auto out = image::ImageBuffer<float>::create(size.width, size.height, source.channels());
    EXPECT_TRUE(out);
    EXPECT_TRUE(box_downscale(source.const_view(), out->view()));
    return std::move(*out);
}

std::ostream& operator<<(std::ostream& os, const DeltaE& d) {
    return os << std::fixed << std::setprecision(3) << "mean " << d.mean << " p95 " << d.p95 << " p99 " << d.p99
              << " max " << d.max;
}

TEST(DevelopFixtureSupport, Ciede2000MatchesSharmasTestData) {
    // Sharma et al. 2005, table 1, pairs 1, 7 and 17.
    EXPECT_NEAR(ciede2000({50.0, 2.6772, -79.7751}, {50.0, 0.0, -82.7485}), 2.0425, 1e-4);
    EXPECT_NEAR(ciede2000({50.0, 0.0, 0.0}, {50.0, -1.0, 2.0}), 2.3669, 1e-4);
    EXPECT_NEAR(ciede2000({50.0, 2.5, 0.0}, {73.0, 25.0, -18.0}), 27.1492, 1e-4);
}

// ---- Kelvin round trips -----------------------------------------------------------------------------

TEST(DevelopFixtures, KelvinRoundTripsTheAsShotBalance) {
    const std::vector<std::string> fixtures = {
        "canon-eos-r5-cr3-raw",       "nikon-z-6-nef-lossless",          "sony-ilce-7m4-arw-compressed",
        "fujifilm-x-t4-raf-lossless", "panasonic-dc-s5m2-rw2",           "om-system-om-1-orf",
        "pentax-k-3-mark-iii-pef",    "hasselblad-x1d-ii-50c-3fr",       "canon-eos-5d-mark-iv-cr2-sraw",
        "dji-fc4382-dng",             "dji-fc220-dng",                   "leica-m10-dng",
        "hasselblad-l2d-20c-dng",     "insta360-one-x-dng",              "adobe-dng-canon-eos-5d-mark-iii-linear-dng",
        "canon-eos-7d-mark-ii-dng-jxl"};
    auto registry = raw::create_default_registry();
    ASSERT_TRUE(registry);
    std::size_t checked = 0;
    for (const std::string& id : fixtures) {
        const auto file = ts::fixture_file(id);
        if (!file) {
            continue;
        }
        SCOPED_TRACE(id);
        auto selection = registry->select(*file, raw::ProbeOptions{}, {});
        ASSERT_TRUE(selection) << selection.error().describe();
        const raw::RawMetadata& metadata = selection->probe.metadata;
        auto model = WhiteBalanceModel::create(metadata);
        ASSERT_TRUE(model) << model.error().describe();
        ASSERT_TRUE(model->kelvin_available()) << model->kelvin_unavailable_reason();
        auto tt = model->temperature_for(model->as_shot());
        ASSERT_TRUE(tt) << tt.error().describe();
        auto m = model->multipliers_for(*tt);
        ASSERT_TRUE(m) << m.error().describe();
        double worst = 0.0;
        for (std::size_t c = 0; c < 3; ++c) {
            worst = std::max(worst, std::abs((*m)[c] / model->as_shot()[c] - 1.0));
        }
        // Temperature/tint are inverse to 1.1e-5 / 2.7e-5; the DNG white search stops at 1e-7.
        EXPECT_LT(worst, 5e-5);
        // Decoder matrices: the D65 white maps to LibRaw's daylight multipliers when LibRaw derived
        // them from the same matrix (reported, not gated: some files store their own pre_mul).
        std::string daylight = "-";
        if (model->path() == CalibrationPath::DecoderMatrix) {
            auto d65 = model->multipliers_for_white(image::kD65White);
            ASSERT_TRUE(d65);
            const auto& pre = metadata.color.daylight_multipliers;
            if (pre[1] > 0.0f) {
                const double r = (*d65)[0] / (pre[0] / pre[1]) - 1.0;
                const double b = (*d65)[2] / (pre[2] / pre[1]) - 1.0;
                const double deviation = std::max(std::abs(r), std::abs(b));
                daylight = std::to_string(deviation);
                // LibRaw derives pre_mul from cam_xyz with its D65 white (0.95047, 1, 1.08883), which
                // differs from xy (0.3127, 0.3290) by 1.4e-4 in X and 2.1e-4 in Z.
                EXPECT_LT(deviation, 5e-4);
            }
        }
        std::cout << std::left << std::setw(44) << id << " path " << std::setw(18) << to_string(model->path())
                  << " as-shot " << std::fixed << std::setprecision(0) << tt->temperature_k << " K tint "
                  << std::setprecision(1) << tt->tint << "  round trip " << std::scientific << std::setprecision(1)
                  << worst << "  D65 vs daylight " << daylight << std::defaultfloat << "\n";
        ++checked;
    }
    if (checked == 0) {
        GTEST_SKIP() << "no camera fixture is downloaded; run tools\\fetch-fixtures.ps1";
    }
}

// ---- The whole Kelvin range renders --------------------------------------------------------------------

TEST(DevelopFixtures, EveryKelvinSettingRendersOnEveryDngCalibration) {
    // Strong green tints at low temperatures are whites near or beyond the spectral locus with a
    // negative Bradford S-cone response; the ColorMatrix files (DJI, Leica, Hasselblad, Insta360)
    // must adapt to them as the DNG SDK does instead of failing. The ForwardMatrix DNGs complete
    // the DNG paths. Stage one is a small proxy: the colour path does not depend on the size.
    const std::vector<std::string> fixtures = {
        "dji-fc4382-dng",     "dji-fc220-dng", "leica-m10-dng", "hasselblad-l2d-20c-dng", "insta360-one-x-dng",
        "adobe-dng-canon-eos-5d-mark-iii-linear-dng", "canon-eos-7d-mark-ii-dng-jxl"};
    const std::vector<double> temperatures = {kMinTemperatureK, 2200.0, 2500.0, 3000.0, 4000.0,
                                              6500.0,           10000.0, 20000.0, kMaxTemperatureK};
    auto registry = raw::create_default_registry();
    ASSERT_TRUE(registry);
    std::size_t checked = 0;
    for (const std::string& id : fixtures) {
        const auto file = ts::fixture_file(id);
        if (!file) {
            continue;
        }
        SCOPED_TRACE(id);
        StageOneRequest request;
        request.proxy_long_edge = 64;
        auto one = render_stage_one_file(*registry, *file, 0u, request);
        ASSERT_TRUE(one) << one.error().describe();
        std::size_t failures = 0;
        for (const double temperature : temperatures) {
            for (double tint = -kMaxTint; tint <= kMaxTint; tint += 10.0) {
                DevelopSettings settings;
                settings.white_balance = KelvinWhiteBalance{temperature, tint};
                auto result = render_stage_two(*one, settings);
                if (!result) {
                    ++failures;
                    ADD_FAILURE() << temperature << " K / " << tint << ": " << result.error().describe();
                    continue;
                }
                const image::ImageBuffer<float>& pixels = result->display.pixels;
                bool finite = true;
                for (std::int32_t y = 0; y < pixels.height() && finite; ++y) {
                    const float* row = pixels.row(y);
                    for (std::int32_t x = 0; x < pixels.width() * pixels.channels(); ++x) {
                        finite = finite && std::isfinite(row[x]);
                    }
                }
                EXPECT_TRUE(finite) << temperature << " K / " << tint;
            }
        }
        std::cout << std::left << std::setw(44) << id << " Kelvin grid: " << temperatures.size() * 31 - failures
                  << " of " << temperatures.size() * 31 << " settings rendered\n";
        ++checked;
    }
    if (checked == 0) {
        GTEST_SKIP() << "no DNG fixture is downloaded; run tools\\fetch-fixtures.ps1";
    }
}

// ---- Interactive versus export ------------------------------------------------------------------------

struct NamedSettings {
    std::string name;
    DevelopSettings settings;
};

std::vector<NamedSettings> comparison_settings(const TemperatureTint& as_shot) {
    std::vector<NamedSettings> out;
    out.push_back({"defaults", DevelopSettings{}});
    DevelopSettings warm;
    warm.white_balance = KelvinWhiteBalance{std::clamp(as_shot.temperature_k - 1500.0, kMinTemperatureK, kMaxTemperatureK),
                                            std::clamp(as_shot.tint + 10.0, -kMaxTint, kMaxTint)};
    out.push_back({"kelvin-1500K", warm});
    DevelopSettings edit;
    edit.exposure_ev = 0.4;
    edit.contrast = 0.25;
    edit.highlights = -0.6;
    edit.shadows = 0.5;
    edit.saturation = 0.15;
    edit.vibrance = 0.25;
    edit.tone_curve.points = {{0.0, 0.02}, {0.25, 0.22}, {0.75, 0.8}, {1.0, 1.0}};
    out.push_back({"edit", edit});
    return out;
}

TEST(DevelopFixtures, InteractiveMatchesExport) {
    // The R5 is the performance-reference camera (decoder matrix, Malvar-He-Cutler); the DJI DNG
    // exercises the dual-illuminant DNG path.
    const std::vector<std::string> fixtures = {"canon-eos-r5-cr3-raw", "dji-fc4382-dng"};
    constexpr std::int32_t kProxyEdge = 2048;
    // Full-resolution statistics use every third pixel in each direction (CIEDE2000 of 45 MP
    // takes seconds); proxies use every pixel.
    constexpr std::int32_t kFullStride = 3;
    auto registry = raw::create_default_registry();
    ASSERT_TRUE(registry);
    std::size_t checked = 0;
    for (const std::string& id : fixtures) {
        const auto file = ts::fixture_file(id);
        if (!file) {
            continue;
        }
        SCOPED_TRACE(id);
        auto one = render_stage_one_file(*registry, *file, 0u, StageOneRequest{});
        ASSERT_TRUE(one) << one.error().describe();
        auto proxy = make_proxy(*one, kProxyEdge);
        ASSERT_TRUE(proxy) << proxy.error().describe();
        auto model = WhiteBalanceModel::create(one->metadata);
        ASSERT_TRUE(model);
        auto as_shot = model->temperature_for(model->as_shot());
        ASSERT_TRUE(as_shot);
        for (const NamedSettings& c : comparison_settings(*as_shot)) {
            SCOPED_TRACE(c.name);
            const bool rebalanced = mode_of(c.settings.white_balance) != WhiteBalanceMode::AsShot;
            auto exported = render_develop_file(*registry, *file, 0u, c.settings);
            ASSERT_TRUE(exported) << exported.error().describe();
            // Quick: the as-shot cache, corrected in stage two.
            auto full = render_stage_two(*one, c.settings);
            ASSERT_TRUE(full) << full.error().describe();
            ASSERT_TRUE(ts::all_finite(full->display.pixels));
            const DeltaE quick_full = compare(full->display.pixels, exported->display.pixels, kFullStride);
            full = Unexpected(Error{});  // release
            auto small = render_stage_two(*proxy, c.settings);
            ASSERT_TRUE(small) << small.error().describe();
            const image::ImageBuffer<float> reduced = downscaled(exported->display.pixels, small->display.pixels.size());
            const DeltaE quick_proxy = compare(small->display.pixels, reduced);
            std::cout << std::left << std::setw(22) << id << std::setw(14) << c.name << "quick   full-res: " << quick_full
                      << " | proxy " << small->display.pixels.width() << "x" << small->display.pixels.height()
                      << " (radius " << (small->report.local_tone ? small->report.local_tone->radius : 0)
                      << "): " << quick_proxy << "\n";
            // Gates (CIEDE2000; 1 is about one just-noticeable difference).
            if (!rebalanced) {
                // Same balance before demosaicing: only the clip placement differs (within 2 pixels
                // of clipped sensor sites), plus the proxy's non-commuting downscale.
                EXPECT_LT(quick_full.mean, 0.01);
                EXPECT_LT(quick_full.p99, 0.05);
                EXPECT_LT(quick_proxy.mean, 0.1);
                EXPECT_LT(quick_proxy.p99, 0.5);
                continue;
            }
            // A 1500 K change corrected after demosaicing: edges differ (Malvar-He-Cutler mixes
            // channels). Acceptable while dragging the slider on the proxy.
            EXPECT_LT(quick_proxy.mean, 0.3);
            EXPECT_LT(quick_proxy.p99, 2.5);
            // Refine: the interactive cache rebuilt at the settings' white balance.
            StageOneRequest refine;
            refine.white_balance = c.settings.white_balance;
            auto refined_one = render_stage_one_file(*registry, *file, 0u, refine);
            ASSERT_TRUE(refined_one) << refined_one.error().describe();
            auto refined = render_stage_two(*refined_one, c.settings);
            ASSERT_TRUE(refined) << refined.error().describe();
            const DeltaE refined_full = compare(refined->display.pixels, exported->display.pixels, kFullStride);
            refined = Unexpected(Error{});
            auto refined_proxy_one = make_proxy(*refined_one, kProxyEdge);
            ASSERT_TRUE(refined_proxy_one);
            refined_one = Unexpected(Error{});
            auto refined_small = render_stage_two(*refined_proxy_one, c.settings);
            ASSERT_TRUE(refined_small);
            const DeltaE refined_proxy = compare(refined_small->display.pixels, reduced);
            std::cout << std::left << std::setw(22) << id << std::setw(14) << c.name << "refined full-res: "
                      << refined_full << " | proxy: " << refined_proxy << "\n";
            EXPECT_LT(refined_full.mean, 0.01);
            EXPECT_LT(refined_full.p99, 0.05);
            EXPECT_LT(refined_proxy.mean, 0.1);
            EXPECT_LT(refined_proxy.p99, 0.5);
        }
        ++checked;
    }
    if (checked == 0) {
        GTEST_SKIP() << "no fixture of this test is downloaded; run tools\\fetch-fixtures.ps1";
    }
}

// ---- Benchmark ----------------------------------------------------------------------------------------

struct Timing {
    double min = 0.0;
    double median = 0.0;
};

Timing time_runs(const std::function<void()>& run, int repeats) {
    std::vector<double> t;
    for (int i = 0; i < repeats; ++i) {
        const auto start = Clock::now();
        run();
        t.push_back(seconds_since(start));
    }
    std::sort(t.begin(), t.end());
    return Timing{t.front(), t[t.size() / 2]};
}

std::ostream& operator<<(std::ostream& os, const Timing& t) {
    return os << std::fixed << std::setprecision(1) << t.min * 1000.0 << " ms (median " << t.median * 1000.0
              << " ms)";
}

// ---- DevelopSettings v2 on the R5 ---------------------------------------------------------------------

BrushStroke fixture_stroke(std::vector<MaskPoint> points, double radius, double hardness, double flow) {
    BrushStroke s;
    s.points = std::move(points);
    s.radius = radius;
    s.hardness = hardness;
    s.flow = flow;
    return s;
}

// Three masks (radial, linear, brush) with several local controls, and chroma denoising.
DevelopSettings masks_and_denoise() {
    DevelopSettings s;
    s.chroma_denoise.strength = 0.7;
    s.chroma_denoise.radius = 6.0;
    LocalAdjustment radial;
    radial.id = 1;
    RadialGradientMask r;
    r.center = {0.45, 0.5};
    r.radius_x = 0.22;
    r.radius_y = 0.15;
    r.angle_degrees = 15.0;
    r.feather = 0.6;
    radial.shape = r;
    radial.controls.exposure_ev = 0.5;
    radial.controls.saturation = 0.2;
    radial.controls.temperature = 0.2;
    LocalAdjustment sky;
    sky.id = 2;
    sky.shape = LinearGradientMask{{0.5, 0.0}, {0.5, 0.4}, 1.0};
    sky.controls.highlights = -0.6;
    sky.controls.contrast = 0.2;
    LocalAdjustment brush;
    brush.id = 3;
    BrushMask paint;
    std::vector<MaskPoint> points;
    for (int i = 0; i <= 200; ++i) {  // a 200-segment stroke across the frame
        const double t = i / 200.0;
        points.push_back({0.1 + 0.8 * t, 0.75 + 0.08 * std::sin(6.0 * t)});
    }
    paint.strokes.push_back(fixture_stroke(points, 0.03, 0.4, 0.9));
    paint.strokes.push_back(fixture_stroke({{0.3, 0.6}, {0.35, 0.9}}, 0.02, 0.7, 1.0));
    brush.shape = paint;
    brush.controls.shadows = 0.5;
    brush.controls.exposure_ev = -0.3;
    s.local_adjustments = {radial, sky, brush};
    return s;
}

TEST(DevelopFixtures, MasksDenoiseAndGeometryOnTheR5) {
    // The performance-reference camera with three masks and chroma denoising: stage-two timings on
    // the 2048-pixel proxy (the editor's quick frame), and the interactive renders (full resolution
    // and proxy) against the export render of the same settings. CIEDE2000 gates as
    // InteractiveMatchesExport's (as-shot balance): the full-resolution interactive render differs
    // only by the clip placement; the proxy also by the non-commuting downscale (the guided filters
    // at scaled radii, the mask edges at proxy pixels).
    LUCENT_ENGINE_REQUIRE_FIXTURE(file, "canon-eos-r5-cr3-raw");
    auto registry = raw::create_default_registry();
    ASSERT_TRUE(registry);
    auto one = render_stage_one_file(*registry, file, 0u, StageOneRequest{});
    ASSERT_TRUE(one) << one.error().describe();
    auto proxy = make_proxy(*one, 2048);
    ASSERT_TRUE(proxy) << proxy.error().describe();
    const DevelopSettings s = masks_and_denoise();
    DevelopSettings turned = s;
    turned.geometry.straighten_degrees = 2.5;
    turned.geometry.crop = largest_inside_crop(2.5, proxy->pixels.size());
    turned.geometry.quarter_turns = 1;

    // ---- Timings on the proxy -----------------------------------------------------------------------
    const auto run = [&](const DevelopSettings& settings, const DevelopOptions& options) {
        auto r = render_stage_two(*proxy, settings, options);
        EXPECT_TRUE(r) << r.error().describe();
        return r;
    };
    DevelopOptions cached;
    cached.mask_cache = std::make_shared<MaskRasterCache>();
    (void)run(s, cached);  // warm the thread pool and the cache
    const Timing defaults = time_runs([&] { (void)run(DevelopSettings{}, DevelopOptions{}); }, 9);
    const Timing uncached = time_runs([&] { (void)run(s, DevelopOptions{}); }, 9);
    std::vector<std::vector<double>> per_operation;
    std::vector<DevelopOperation> names;
    const Timing with_cache = time_runs(
        [&] {
            auto r = run(s, cached);
            if (r) {
                per_operation.resize(r->report.operations.size());
                names.resize(r->report.operations.size());
                for (std::size_t i = 0; i < r->report.operations.size(); ++i) {
                    per_operation[i].push_back(r->report.operations[i].seconds);
                    names[i] = r->report.operations[i].operation;
                }
            }
        },
        9);
    DevelopOptions session = cached;  // the edit session's caches: a mask or tone control is moving
    session.stage_two_cache = std::make_shared<StageTwoCache>();
    (void)run(s, session);
    const Timing reused = time_runs([&] { (void)run(s, session); }, 9);
    const Timing geometry = time_runs([&] { (void)run(turned, session); }, 9);
    EXPECT_EQ(session.stage_two_cache->stats().misses, 1u);
    auto sample = run(s, cached);
    ASSERT_TRUE(sample);
    ASSERT_TRUE(sample->report.local_adjustments.has_value());
    EXPECT_EQ(sample->report.local_adjustments->cache_hits, 1u);
    std::cout << "R5 proxy " << proxy->pixels.width() << "x" << proxy->pixels.height() << " stage two: defaults "
              << defaults << "; 3 masks + chroma denoise " << uncached << " (brush rasterised), " << with_cache
              << " (brush cached), " << reused << " (the denoised image reused as well); + straighten 2.5 deg,"
              << " crop and a quarter turn " << geometry << "\n  per operation (min of 9, brush cached, ms):";
    for (std::size_t i = 0; i < per_operation.size(); ++i) {
        std::cout << " " << to_string(names[i]) << " " << std::fixed << std::setprecision(1)
                  << *std::min_element(per_operation[i].begin(), per_operation[i].end()) * 1000.0 << ";";
    }
    std::cout << "\n  brush raster " << sample->report.local_adjustments->raster_bytes / 1024 << " KiB, mask cache "
              << cached.mask_cache->stats().bytes / 1024 << " KiB, chroma-denoise working memory "
              << sample->report.chroma_denoise->working_bytes / (1024 * 1024) << " MiB (radius "
              << sample->report.chroma_denoise->radius << ")\n";
    // Interactive bounds on the reference laptop (generous: the machine may be busy); the measured
    // values are in ADR 0015.
    EXPECT_LT(with_cache.median, 1.0);
    EXPECT_LT(reused.median, with_cache.median);

    // ---- Interactive against export ---------------------------------------------------------------------
    auto full = render_stage_two(*one, s);
    ASSERT_TRUE(full) << full.error().describe();
    one = Unexpected(Error{});  // release the full-resolution stage one
    const auto start = Clock::now();
    auto exported = render_develop_file(*registry, file, 0u, s);
    const double export_seconds = seconds_since(start);
    ASSERT_TRUE(exported) << exported.error().describe();
    const DeltaE full_delta = compare(full->display.pixels, exported->display.pixels, 3);
    full = Unexpected(Error{});
    std::cout << "  export render with masks and denoising (decode included): " << std::setprecision(2)
              << export_seconds << " s; per operation:";
    for (const OperationRecord& op : exported->report.operations) {
        if (op.seconds > 0.0) {
            std::cout << " " << to_string(op.operation) << " " << std::setprecision(0) << op.seconds * 1000.0 << " ms;";
        }
    }
    std::cout << "\n  interactive full resolution vs export: " << full_delta << "\n";
    EXPECT_LT(full_delta.mean, 0.01);
    EXPECT_LT(full_delta.p99, 0.05);
    // The proxy against the export for the masks, the denoising and both (each its own export).
    DevelopSettings masks_only = s;
    masks_only.chroma_denoise = ChromaDenoiseSetting{};
    DevelopSettings denoise_only;
    denoise_only.chroma_denoise = s.chroma_denoise;
    struct Case {
        const char* name;
        const DevelopSettings* settings;
        double mean;
        double p99;
    };
    const Case cases[] = {{"both", &s, 0.1, 0.8}, {"masks", &masks_only, 0.05, 0.3}, {"denoise", &denoise_only, 0.1, 0.8}};
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        if (c.settings != &s) {
            exported = render_develop_file(*registry, file, 0u, *c.settings);
            ASSERT_TRUE(exported) << exported.error().describe();
        }
        auto small = render_stage_two(*proxy, *c.settings);
        ASSERT_TRUE(small);
        const image::ImageBuffer<float> reduced = downscaled(exported->display.pixels, small->display.pixels.size());
        const DeltaE d = compare(small->display.pixels, reduced);
        std::cout << "  proxy vs export, " << c.name << ": " << d << "\n";
        EXPECT_LT(d.mean, c.mean);
        EXPECT_LT(d.p99, c.p99);
    }
}

TEST(DevelopFixtures, DISABLED_BenchmarkMasksAndDenoise) {
    // Breakdown of the v2 operations on the R5 proxy (run with --gtest_also_run_disabled_tests
    // --gtest_filter=*BenchmarkMasks*): each mask kind alone, chroma denoising in bands and whole,
    // serial and parallel.
    LUCENT_ENGINE_REQUIRE_FIXTURE(file, "canon-eos-r5-cr3-raw");
    auto registry = raw::create_default_registry();
    ASSERT_TRUE(registry);
    StageOneRequest request;
    request.proxy_long_edge = 2048;
    auto proxy = render_stage_one_file(*registry, file, 0u, request);
    ASSERT_TRUE(proxy) << proxy.error().describe();
    const DevelopSettings all = masks_and_denoise();
    const auto stage_two_ms = [&](const DevelopSettings& s, DevelopOperation op) {
        std::vector<double> t;
        for (int i = 0; i < 7; ++i) {
            auto r = render_stage_two(*proxy, s);
            EXPECT_TRUE(r);
            for (const OperationRecord& o : r->report.operations) {
                if (o.operation == op) {
                    t.push_back(o.seconds * 1000.0);
                }
            }
        }
        std::sort(t.begin(), t.end());
        return t.empty() ? 0.0 : t[t.size() / 2];
    };
    for (std::size_t k = 0; k < all.local_adjustments.size(); ++k) {
        DevelopSettings one_mask;
        one_mask.local_adjustments = {all.local_adjustments[k]};
        std::cout << "mask " << to_string(all.local_adjustments[k].kind()) << " alone: "
                  << stage_two_ms(one_mask, DevelopOperation::LocalAdjustments) << " ms\n";
        DevelopSettings no_tone = one_mask;
        no_tone.local_adjustments[0].controls.highlights = 0.0;
        no_tone.local_adjustments[0].controls.shadows = 0.0;
        if (!no_tone.local_adjustments[0].controls.is_identity()) {
            std::cout << "  without highlights/shadows: " << stage_two_ms(no_tone, DevelopOperation::LocalAdjustments)
                      << " ms\n";
        }
    }
    DevelopSettings masks_only = all;
    masks_only.chroma_denoise = ChromaDenoiseSetting{};
    std::cout << "three masks: " << stage_two_ms(masks_only, DevelopOperation::LocalAdjustments) << " ms\n";
    DevelopSettings denoise_only;
    denoise_only.chroma_denoise = all.chroma_denoise;
    std::cout << "chroma denoise (pipeline): " << stage_two_ms(denoise_only, DevelopOperation::ChromaDenoise)
              << " ms\n";
    auto camera = proxy->pixels.clone();
    ASSERT_TRUE(camera);
    for (const std::int32_t band : {kChromaDenoiseBandRows, 0}) {
        for (const bool parallel : {true, false}) {
            std::vector<double> t;
            for (int i = 0; i < (parallel ? 7 : 2); ++i) {
                auto pixels = proxy->pixels.clone();
                ASSERT_TRUE(pixels);
                ColorImage image{std::move(*pixels), image::ColorEncoding::scene_linear_rec2020_d65()};
                ChromaDenoiseParameters p;
                p.strength = 0.7;
                p.radius = 2;
                p.band_rows = band;
                const auto start = Clock::now();
                auto r = apply_chroma_denoise(image, p, {}, {}, parallel);
                t.push_back(seconds_since(start) * 1000.0);
                ASSERT_TRUE(r);
            }
            std::sort(t.begin(), t.end());
            std::cout << "chroma denoise direct, band rows " << band << (parallel ? ", parallel: " : ", serial: ")
                      << t[t.size() / 2] << " ms\n";
        }
    }
    {
        std::vector<double> t;
        for (int i = 0; i < 7; ++i) {
            auto pixels = proxy->pixels.clone();
            ASSERT_TRUE(pixels);
            auto plane = image::ImageBuffer<float>::create(pixels->width(), pixels->height(), 2);
            ASSERT_TRUE(plane);
            auto guide = image::ImageBuffer<float>::create(pixels->width(), pixels->height(), 1);
            ASSERT_TRUE(guide);
            const auto start = Clock::now();
            ASSERT_TRUE(guided_filter(guide->const_view(), plane->const_view(), plane->view(), 2, 0.01));
            t.push_back(seconds_since(start) * 1000.0);
        }
        std::sort(t.begin(), t.end());
        std::cout << "guided filter alone (whole proxy, 2-channel input, radius 2): " << t[t.size() / 2] << " ms\n";
    }
}

TEST(DevelopFixtures, DISABLED_Benchmark) {
    LUCENT_ENGINE_REQUIRE_FIXTURE(file, "canon-eos-r5-cr3-raw");
    auto registry = raw::create_default_registry();
    ASSERT_TRUE(registry);
    auto start = Clock::now();
    auto one = render_stage_one_file(*registry, file, 0u, StageOneRequest{});
    const double stage_one_s = seconds_since(start);
    ASSERT_TRUE(one) << one.error().describe();
    const double after_stage_one_mb = peak_working_set_mb();
    start = Clock::now();
    auto proxy = make_proxy(*one, 2048);
    const double proxy_s = seconds_since(start);
    ASSERT_TRUE(proxy);
    DevelopSettings all = comparison_settings(TemperatureTint{5200.0, 5.0})[2].settings;
    all.white_balance = KelvinWhiteBalance{4800.0, 4.0};
    std::vector<std::vector<double>> per_operation;
    std::vector<DevelopOperation> operation_order;
    const auto stage_two = [&](const StageOneImage& image, const DevelopSettings& s, bool record) {
        auto r = render_stage_two(image, s);
        ASSERT_TRUE(r) << r.error().describe();
        if (record) {
            per_operation.resize(r->report.operations.size());
            operation_order.resize(r->report.operations.size());
            for (std::size_t i = 0; i < r->report.operations.size(); ++i) {
                per_operation[i].push_back(r->report.operations[i].seconds);
                operation_order[i] = r->report.operations[i].operation;
            }
        }
    };
    stage_two(*proxy, all, false);  // warm the thread pool
    const Timing proxy_defaults = time_runs([&] { stage_two(*proxy, DevelopSettings{}, false); }, 15);
    const double cpu_before = process_cpu_seconds();
    const Timing proxy_all = time_runs([&] { stage_two(*proxy, all, true); }, 15);
    const double cpu_per_run = (process_cpu_seconds() - cpu_before) / 15.0;
    DevelopOptions serial;
    serial.parallel = false;
    const Timing proxy_all_serial = time_runs(
        [&] {
            auto r = render_stage_two(*proxy, all, serial);
            ASSERT_TRUE(r);
        },
        3);
    const Timing full_defaults = time_runs([&] { stage_two(*one, DevelopSettings{}, false); }, 3);
    const Timing full_all = time_runs([&] { stage_two(*one, all, false); }, 3);
    const std::size_t full_bytes = one->byte_size();
    const std::size_t proxy_bytes = proxy->byte_size();
    const image::Size full_size = one->pixels.size();
    one = Unexpected(Error{});
    start = Clock::now();
    auto exported = render_develop_file(*registry, file, 0u, all);
    const double export_s = seconds_since(start);
    ASSERT_TRUE(exported);
    std::cout << std::fixed << std::setprecision(3) << "R5 " << full_size.width << "x" << full_size.height
              << ", " << std::thread::hardware_concurrency() << " hardware threads"
              << "\n  stage one (decode + normalise + WB + demosaic + orient): " << stage_one_s << " s"
              << "\n  proxy 2048 (box downscale): " << proxy_s << " s"
              << "\n  stage two, proxy " << proxy->pixels.width() << "x" << proxy->pixels.height()
              << ": defaults " << proxy_defaults << ", all operations " << proxy_all << ", CPU time per run "
              << cpu_per_run * 1000.0 << " ms, one thread " << proxy_all_serial
              << "\n  stage two, full resolution: defaults " << full_defaults << ", all operations " << full_all
              << std::setprecision(3) << "\n  export render (all operations, decode included): " << export_s << " s"
              << "\n  cached stage one: full " << static_cast<double>(full_bytes) / (1024.0 * 1024.0)
              << " MiB, proxy " << static_cast<double>(proxy_bytes) / (1024.0 * 1024.0) << " MiB"
              << "\n  peak working set after stage one " << after_stage_one_mb << " MiB, at the end "
              << peak_working_set_mb() << " MiB\n  proxy, all operations, per operation (min of 15, ms):";
    for (std::size_t i = 0; i < per_operation.size(); ++i) {
        std::cout << " " << to_string(operation_order[i]) << " " << std::setprecision(1)
                  << *std::min_element(per_operation[i].begin(), per_operation[i].end()) * 1000.0 << ";";
    }
    std::cout << "\n";
}

}  // namespace
}  // namespace lucent::engine
