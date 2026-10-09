// Auto on a photo larger than any camera fixture (ADR 0013, amendment "W12: the final check by
// bands"): a synthetic 100-megapixel DNG, written row by row so that the test itself never holds the
// image, runs the analysis and the full-resolution final check within the default 4 GiB budget.
// Before W12 the check kept a full-resolution context and the baseline's features (26 bytes per
// pixel beside its stage one and renders) and refused Auto above about 69 MP on that budget.
//
// Runs in lucent_application_fixture_tests, which holds the full-resolution render lock
// (tests/CMakeLists.txt), although it needs no downloaded fixture.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include <gtest/gtest.h>

// clang-format off: psapi.h needs the declarations of windows.h.
#include <windows.h>
#include <psapi.h>
// clang-format on

#include <lucent/application/auto_develop.hpp>
#include <lucent/raw/dng_tags.hpp>

#include "app_test_support.hpp"
#include "tiff_test_writer.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;
namespace rts = raw::test_support;

// A 16-bit RGGB DNG of `width` x `height` (the tags of rts::build_synthetic_dng) whose strip is
// written row by row from `scene` (stored value at (x, y) for CFA colour 0 red, 1 green, 2 blue).
template <class Scene>
void write_large_dng(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height, Scene scene) {
    namespace t = raw::tiff_tag;
    const std::uint64_t strip_bytes = std::uint64_t{width} * height * 2;
    rts::Ifd ifd;
    ifd.entries = {
        rts::long_entry(t::kNewSubFileType, {0}),
        rts::long_entry(t::kImageWidth, {width}),
        rts::long_entry(t::kImageLength, {height}),
        rts::short_entry(t::kBitsPerSample, {16}),
        rts::short_entry(t::kCompression, {1}),
        rts::short_entry(t::kPhotometricInterpretation, {32803}),
        rts::long_entry(t::kStripOffsets, {0}),  // patched below
        rts::short_entry(t::kSamplesPerPixel, {1}),
        rts::long_entry(t::kRowsPerStrip, {height}),
        rts::long_entry(t::kStripByteCounts, {strip_bytes}),
        rts::short_entry(t::kPlanarConfiguration, {1}),
        rts::short_entry(t::kSampleFormat, {1}),
        rts::rational_entry(t::kBlackLevel, {{256, 1}}),
        rts::long_entry(t::kWhiteLevel, {65535}),
        rts::double_entry(t::kNoiseProfile, {2e-5, 2e-7, 2e-5, 2e-7, 2e-5, 2e-7}),
        rts::short_entry(t::kCfaRepeatPatternDim, {2, 2}),
        rts::byte_entry(t::kCfaPattern, {0, 1, 1, 2}),
        rts::byte_entry(t::kCfaPlaneColor, {0, 1, 2}),
        rts::short_entry(t::kCfaLayout, {1}),
        rts::ascii_entry(t::kMake, "Lucent"),
        rts::ascii_entry(t::kModel, "Synthetic DNG"),
        rts::short_entry(t::kOrientation, {1}),
        rts::byte_entry(t::kDngVersion, {1, 4, 0, 0}),
        rts::ascii_entry(t::kUniqueCameraModel, "Lucent Synthetic DNG"),
        rts::srational_entry(t::kColorMatrix1, {{8000, 10000},
                                                {-2000, 10000},
                                                {-1000, 10000},
                                                {-4000, 10000},
                                                {12000, 10000},
                                                {1500, 10000},
                                                {-500, 10000},
                                                {1800, 10000},
                                                {5500, 10000}}),
        rts::short_entry(t::kCalibrationIlluminant1, {21}),
        rts::rational_entry(t::kAsShotNeutral, {{1, 2}, {1, 1}, {2, 3}}),
    };
    rts::Document doc;
    doc.chain = {ifd};
    rts::Built built = rts::build(doc);
    const std::uint64_t data_offset = (built.bytes.size() + 15) / 16 * 16;
    built.bytes.resize(static_cast<std::size_t>(data_offset));
    built.put(built.value("ifd0", t::kStripOffsets), data_offset, 4);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out);
    out.write(reinterpret_cast<const char*>(built.bytes.data()), static_cast<std::streamsize>(built.bytes.size()));
    std::vector<std::uint16_t> row(width);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t color = (y % 2 == 0) ? (x % 2 == 0 ? 0u : 1u) : (x % 2 == 0 ? 1u : 2u);
            row[x] = scene(x, y, color);
        }
        out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size() * 2));
    }
    ASSERT_TRUE(out);
}

TEST(AutoLargePhoto, AHundredMegapixelPhotoRunsWithinTheBudget) {
    constexpr std::uint32_t kWidth = 12000;
    constexpr std::uint32_t kHeight = 8400;  // 100.8 MP
    rts::ScratchDir dir;
    const std::filesystem::path file = dir.path() / L"large.dng";
    // An underexposed scene (Natural brightens it, so the check renders and measures a change):
    // smooth gradients with coloured blocks and fine texture, under the as-shot illuminant.
    write_large_dng(file, kWidth, kHeight, [](std::uint32_t x, std::uint32_t y, std::uint32_t color) {
        const double fx = static_cast<double>(x) / kWidth;
        const double fy = static_cast<double>(y) / kHeight;
        const bool block = ((x / 1500) + (y / 1400)) % 3 == 0;
        const std::array<double, 3> tint = block ? std::array<double, 3>{1.5, 1.0, 0.7} : std::array<double, 3>{1.0, 1.0, 1.0};
        const std::array<double, 3> as_shot{0.5, 1.0, 2.0 / 3.0};
        const double level = (0.015 + 0.06 * fx + 0.03 * fy) * (1.0 + 0.05 * std::sin(0.05 * x + 0.03 * y));
        const double v = level * tint[color] * as_shot[color];
        return static_cast<std::uint16_t>(std::clamp(256.0 + v * (65535.0 - 256.0), 0.0, 65535.0));
    });

    const auto registry = ts::default_registry();
    AutoRunOptions options;
    const std::uint64_t budget = options.analysis.develop.memory_budget_bytes;
    ASSERT_EQ(budget, std::uint64_t{4} << 30);
    const auto memory = [] {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        ::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                               sizeof(counters));
        return counters;
    };
    const PROCESS_MEMORY_COUNTERS_EX before = memory();
    const auto start = std::chrono::steady_clock::now();
    auto run = run_automatic(*registry, file, 0, engine::DevelopSettings{}, AutoRunRequest{}, options);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ASSERT_TRUE(run) << run.error().describe();
    const PROCESS_MEMORY_COUNTERS_EX after = memory();
    std::printf("  %u x %u Auto: %.1f s (analysis %.1f s, check %.1f s, %zu attempt(s)); exposure %+.2f EV; estimate "
                "%.0f MiB of a %.0f MiB budget; process commit %.0f MiB before, peak %.0f MiB\n",
                kWidth, kHeight, seconds, run->analysis_seconds, run->check_seconds, run->attempts.size(),
                run->chosen.exposure_ev, static_cast<double>(run->estimated_peak_bytes) / 1048576.0,
                static_cast<double>(budget) / 1048576.0, static_cast<double>(before.PagefileUsage) / 1048576.0,
                static_cast<double>(after.PeakPagefileUsage) / 1048576.0);
    EXPECT_EQ(run->width, static_cast<std::int32_t>(kWidth));
    EXPECT_LE(run->estimated_peak_bytes, budget);
    // The check really ran on a change (rendered and measured by bands), and passed.
    EXPECT_NE(run->chosen.exposure_ev, 0.0);
    ASSERT_TRUE(run->effective);
    EXPECT_EQ(run->attempts[*run->effective].check.outcome, catalog::AutoCheckOutcome::Passed);
    EXPECT_FALSE(run->attempts.front().check.values.empty());
    // What the process committed for the run stayed within the budget (the estimate counts pixel
    // memory; the process's own baseline is a few tens of MiB).
    EXPECT_LE(after.PeakPagefileUsage - before.PagefileUsage, budget);
}

}  // namespace
}  // namespace lucent::app
