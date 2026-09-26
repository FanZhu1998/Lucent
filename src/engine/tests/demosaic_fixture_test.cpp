// Full-resolution normalisation, white balance and reconstruction on camera fixtures
// (tools\fetch-fixtures.ps1): route selection per layout, output sanity, and time and memory
// measurements for the performance report. Each test skips when its fixture is absent.
//
// Memory figures come from GetProcessMemoryInfo: PeakWorkingSetSize and PeakPagefileUsage (peak
// private commit) of the test process. ctest runs every test case in its own process, so they
// cover exactly one decode plus the engine steps. The engine's own buffers are also computed from
// the image sizes.

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <variant>

#include <gtest/gtest.h>
#include <windows.h>
#include <psapi.h>

#include <lucent/engine/demosaic.hpp>
#include <lucent/engine/sensor.hpp>
#include <lucent/raw/libraw_decoder.hpp>

#include "sensor_test_support.hpp"

namespace lucent::engine {
namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

struct MemorySnapshot {
    double working_set_mb = 0.0;
    double peak_working_set_mb = 0.0;
    double peak_commit_mb = 0.0;
};

MemorySnapshot memory() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    MemorySnapshot snapshot;
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                               sizeof(counters))) {
        constexpr double kMb = 1024.0 * 1024.0;
        snapshot.working_set_mb = static_cast<double>(counters.WorkingSetSize) / kMb;
        snapshot.peak_working_set_mb = static_cast<double>(counters.PeakWorkingSetSize) / kMb;
        snapshot.peak_commit_mb = static_cast<double>(counters.PeakPagefileUsage) / kMb;
    }
    return snapshot;
}

double mb(std::size_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

struct PipelineResult {
    DemosaicedImage image;
    raw::RawMetadata metadata;
};

// Decode, normalise, white balance (as shot, clip policy) and reconstruct, reporting each step.
PipelineResult run_pipeline(const std::filesystem::path& file, const std::string& label) {
    PipelineResult result;
    const MemorySnapshot start = memory();
    auto decoder = raw::create_libraw_decoder();
    EXPECT_TRUE(decoder);
    auto t = Clock::now();
    auto decoded = (*decoder)->decode(file, {}, {});
    EXPECT_TRUE(decoded) << decoded.error().describe();
    if (!decoded) {
        return result;
    }
    const double decode_s = seconds_since(t);
    const std::size_t raw_bytes = std::visit([](const auto& b) { return b.byte_size(); }, raw::pixels_of(decoded->image));
    const MemorySnapshot after_decode = memory();

    t = Clock::now();
    auto sensor = normalize_sensor(*decoded);
    EXPECT_TRUE(sensor) << sensor.error().describe();
    if (!sensor) {
        return result;
    }
    const double normalize_s = seconds_since(t);
    const std::size_t sensor_bytes = sensor->pixels.byte_size() +
                                     static_cast<std::size_t>(sensor->saturation.width()) *
                                         static_cast<std::size_t>(sensor->saturation.height());
    result.metadata = std::move(decoded->metadata);
    decoded = raw::DecodedRaw{};  // the decoded frame is no longer needed

    t = Clock::now();
    const Status balanced = white_balance_sensor(*sensor, result.metadata);
    EXPECT_TRUE(balanced) << balanced.error().describe();
    const double white_balance_s = seconds_since(t);

    t = Clock::now();
    auto image = demosaic(std::move(*sensor));
    EXPECT_TRUE(image) << image.error().describe();
    if (!image) {
        return result;
    }
    const double demosaic_s = seconds_since(t);
    const MemorySnapshot end = memory();

    std::cout << "[ perf ] " << label << ": " << image->pixels.width() << " x " << image->pixels.height() << " "
              << to_string(image->demosaic.route) << "\n"
              << "[ perf ]   decode " << decode_s << " s, normalise " << normalize_s << " s, white balance "
              << white_balance_s << " s, demosaic " << demosaic_s << " s (engine total "
              << normalize_s + white_balance_s + demosaic_s << " s)\n"
              << "[ perf ]   buffers: raw frame " << mb(raw_bytes) << " MB, sensor float + mask " << mb(sensor_bytes)
              << " MB, RGB float " << mb(image->pixels.byte_size()) << " MB\n"
              << "[ perf ]   process: start " << start.working_set_mb << " MB, after decode "
              << after_decode.working_set_mb << " MB (peak " << after_decode.peak_working_set_mb
              << " MB), peak working set " << end.peak_working_set_mb << " MB, peak commit " << end.peak_commit_mb
              << " MB\n";
    ::testing::Test::RecordProperty("engine_seconds", std::to_string(normalize_s + white_balance_s + demosaic_s));
    ::testing::Test::RecordProperty("peak_working_set_mb", std::to_string(end.peak_working_set_mb));
    result.image = std::move(*image);
    return result;
}

// Every sample finite; counts of values outside [0, 1] are reported (they are kept, not clipped).
void expect_finite(const DemosaicedImage& image) {
    std::size_t non_finite = 0;
    std::size_t below = 0;
    std::size_t above = 0;
    const std::int32_t samples = image.pixels.width() * image.pixels.channels();
    for (std::int32_t y = 0; y < image.pixels.height(); ++y) {
        const float* row = image.pixels.row(y);
        for (std::int32_t i = 0; i < samples; ++i) {
            non_finite += std::isfinite(row[i]) ? 0 : 1;
            below += row[i] < 0.0f ? 1 : 0;
            above += row[i] > 1.0f ? 1 : 0;
        }
    }
    EXPECT_EQ(non_finite, 0u);
    std::cout << "[ perf ]   samples below 0: " << below << ", above 1: " << above << "\n";
}

TEST(EngineFixtures, CanonR5FullResolutionBayer) {
    LUCENT_ENGINE_REQUIRE_FIXTURE(file, "canon-eos-r5-cr3-raw");
    const PipelineResult r = run_pipeline(file, "canon-eos-r5-cr3-raw");
    ASSERT_FALSE(r.image.pixels.empty());
    EXPECT_EQ(r.image.demosaic.route, DemosaicRoute::BayerMalvarHeCutler);
    EXPECT_EQ(r.image.pixels.width(), r.metadata.geometry.visible_area.width);
    EXPECT_EQ(r.image.pixels.height(), r.metadata.geometry.visible_area.height);
    EXPECT_EQ(r.image.pixels.channels(), 3);
    ASSERT_TRUE(r.image.white_balance);
    EXPECT_EQ(r.image.white_balance->applied.source, WhiteBalanceSource::AsShot);
    EXPECT_DOUBLE_EQ(r.image.white_balance->applied.multipliers[1], 1.0);
    expect_finite(r.image);
}

TEST(EngineFixtures, FujifilmXTransFullResolutionBaseline) {
    LUCENT_ENGINE_REQUIRE_FIXTURE(file, "fujifilm-x-t4-raf-lossless");
    const PipelineResult r = run_pipeline(file, "fujifilm-x-t4-raf-lossless");
    ASSERT_FALSE(r.image.pixels.empty());
    EXPECT_EQ(r.image.demosaic.route, DemosaicRoute::XTransBaseline);
    EXPECT_EQ(r.image.demosaic.maturity, RouteMaturity::Baseline);
    EXPECT_TRUE(r.image.demosaic.cfa.is_xtrans());
    EXPECT_TRUE(r.image.normalization.black_pattern_applied);  // the RAF's 6 x 6 black pattern
    EXPECT_EQ(r.image.pixels.width(), r.metadata.geometry.visible_area.width);
    expect_finite(r.image);
}

TEST(EngineFixtures, FullColourAndMonochromeFixturesBypassReconstruction) {
    const std::pair<const char*, DemosaicRoute> cases[] = {
        {"adobe-dng-canon-eos-5d-mark-iii-linear-dng", DemosaicRoute::LinearColorBypass},
        {"canon-eos-5d-mark-iv-cr2-sraw", DemosaicRoute::LinearColorBypass},
        {"leica-m-monochrom-typ-246-dng", DemosaicRoute::MonochromeBypass},
        {"pentax-k-3-mark-iii-monochrome-dng", DemosaicRoute::MonochromeBypass},
    };
    std::size_t ran = 0;
    for (const auto& [id, route] : cases) {
        SCOPED_TRACE(id);
        const auto file = test_support::fixture_file(id);
        if (!file) {
            continue;
        }
        ++ran;
        const PipelineResult r = run_pipeline(*file, id);
        ASSERT_FALSE(r.image.pixels.empty());
        EXPECT_EQ(r.image.demosaic.route, route);
        EXPECT_EQ(r.image.demosaic.maturity, RouteMaturity::Bypass);
        EXPECT_EQ(r.image.pixels.channels(), route == DemosaicRoute::MonochromeBypass ? 1 : 3);
        ASSERT_TRUE(r.image.white_balance);
        if (route == DemosaicRoute::MonochromeBypass) {
            EXPECT_EQ(r.image.white_balance->applied.source, WhiteBalanceSource::NotApplicable);
        }
        if (r.metadata.has_prior_processing(raw::PriorProcessingKind::WhiteBalanceApplied)) {
            EXPECT_EQ(r.image.white_balance->applied.source, WhiteBalanceSource::AlreadyApplied);
        }
        std::cout << "[ perf ]   white balance: " << to_string(r.image.white_balance->applied.source) << " ("
                  << r.image.white_balance->applied.basis << ")\n";
        expect_finite(r.image);
    }
    if (ran == 0) {
        GTEST_SKIP() << "no full-colour or monochrome fixture is downloaded; run tools\\fetch-fixtures.ps1";
    }
}

}  // namespace
}  // namespace lucent::engine
