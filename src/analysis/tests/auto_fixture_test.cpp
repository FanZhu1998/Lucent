// The smoke-tier fixture sweep of the automatic settings: every smoke fixture (tools\fetch-fixtures.ps1)
// is analysed twice; the results must be finite, inside the search bounds, feasible and identical,
// and the timings, candidate counts and memory are reported. Cases whose fixture is not downloaded
// skip. Under ctest every case runs in its own process, so the reported peak working set is the
// fixture's own.

// clang-format off: windows.h must precede psapi.h.
#include <windows.h>
#include <psapi.h>
// clang-format on

#include <cmath>
#include <cstdio>
#include <format>
#include <string>

#include <gtest/gtest.h>

#include <lucent/analysis/auto_settings.hpp>
#include <lucent/engine/develop_operations.hpp>
#include <lucent/raw/decoder_registry.hpp>

#include "analysis_test_support.hpp"

namespace lucent::analysis {
namespace {

std::uint64_t peak_working_set() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
        return 0;
    }
    return counters.PeakWorkingSetSize;
}

std::string summary(const std::string& id, const AutoResult& r, std::uint64_t peak) {
    const EditVector& e = r.candidates[r.chosen].edit;
    std::string flags;
    for (const ReviewFlag& f : r.flags) {
        flags += std::format("{}{}{}", flags.empty() ? "" : ",", to_string(f.code),
                             f.metric ? std::format("({})", to_string(*f.metric)) : std::string());
    }
    const MetricResult* tones = r.diagnostic(MetricId::TonalDistribution);
    const MetricResult* native = r.diagnostic(MetricId::NativeDetail);
    const MetricResult* noise = r.diagnostic(MetricId::ShadowNoise);
    const auto value = [](const MetricResult* m, std::string_view name) {
        return m != nullptr ? m->value(name).value_or(-1.0) : -1.0;
    };
    if (r.white_balance) {
        std::string estimates;
        for (const WhiteBalanceEstimate& w : r.white_balance->estimates) {
            estimates += std::format(" {}{}({:+.3f},{:+.3f} c{:.2f}, {})", to_string(w.method),
                                     w.available ? "" : "-", w.log_rg, w.log_bg, w.confidence, w.detail);
        }
        std::printf("[ AUTO     ] %s: white balance estimates:%s\n", id.c_str(), estimates.c_str());
    }
    std::printf(
        "[ AUTO     ] %s: p50 %.3f -> %.3f (band [%.2f, %.2f], low key %.2f), needed %+.2f EV, headroom %.2f "
        "EV, "
        "signal %.2f; source saturation %.4f, noise %s; wb %s; halo %.2f L*, shadow noise %.2f -> %.2f L*\n",
        id.c_str(), r.candidates[0].analysis.median_lightness, value(tones, "p50"), r.tone.band_low,
        r.tone.band_high, r.tone.low_key, r.tone.needed_ev, r.tone.headroom_ev,
        value(tones, "signal_fraction"), r.source.max_channel_fraction(),
        std::string(to_string(r.source.noise.source)).c_str(),
        r.white_balance ? r.white_balance->reason.c_str() : "locked or monochrome",
        value(native, "halo_lstar"), value(noise, "baseline_sigma_lstar"),
        value(noise, "predicted_sigma_lstar"));
    return std::format(
        "{}: {}x{} -> analysis {}x{}, proxy {}x{}; total {:.2f} s (decode {:.2f}, source {:.2f}, stage one "
        "{:.2f}, "
        "proxies {:.2f}, crops {:.2f}, baseline {:.2f}, proposals {:.2f}, search {:.2f}, recheck {:.2f}); "
        "candidates "
        "{} proxy / {} analysis / {} crop renders; chosen {} ({}): exposure {:+.3f} EV, wb r {:.4f} b "
        "{:.4f}, shadows "
        "{:.3f} EV, highlights {:.3f} EV, contrast {:+.3f}, saturation x{:.3f}; confidence {:.2f}{}; flags "
        "[{}]; "
        "memory: estimated peak {} MiB, retained {} MiB, process peak working set {} MiB",
        id, r.full_size.width, r.full_size.height, r.analysis_size.width, r.analysis_size.height,
        r.proxy_size.width, r.proxy_size.height, r.timing.total_seconds, r.timing.decode_seconds,
        r.timing.source_seconds, r.timing.stage_one_seconds, r.timing.proxies_seconds, r.timing.crops_seconds,
        r.timing.baseline_seconds, r.timing.proposals_seconds, r.timing.search_seconds,
        r.timing.recheck_seconds, r.timing.proxy_candidates, r.timing.analysis_candidates,
        r.timing.crop_renders, r.chosen, to_string(r.candidates[r.chosen].origin), e.exposure_ev, e.red_gain,
        e.blue_gain, e.shadows_ev, e.highlights_ev, e.contrast, e.saturation, r.confidence,
        r.restrained ? " (restrained)" : "", flags, r.memory.estimated_peak_bytes >> 20,
        r.memory.retained_bytes >> 20, peak >> 20);
}

class AutoFixture : public ::testing::TestWithParam<const char*> {};

TEST_P(AutoFixture, FiniteBoundedDeterministic) {
    const std::string id = GetParam();
    LUCENT_ENGINE_REQUIRE_FIXTURE(path, id);
    auto registry = raw::create_default_registry();
    ASSERT_TRUE(registry) << registry.error().describe();

    auto first = auto_develop_file(*registry, path, std::nullopt, engine::DevelopSettings{});
    ASSERT_TRUE(first) << first.error().describe();
    const std::uint64_t peak = peak_working_set();
    auto second = auto_develop_file(*registry, path, std::nullopt, engine::DevelopSettings{});
    ASSERT_TRUE(second) << second.error().describe();
    const AutoResult& r = *first;

    // Finite and bounded.
    EXPECT_TRUE(r.settings.validate());
    EXPECT_TRUE(std::isfinite(r.settings.exposure_ev));
    EXPECT_LE(std::abs(r.settings.exposure_ev), 1.5);
    EXPECT_GE(r.settings.shadows * engine::kShadowsMaxEv, 0.0);
    EXPECT_LE(r.settings.shadows * engine::kShadowsMaxEv, 0.75 + 1e-9);
    EXPECT_GE(r.settings.highlights * engine::kHighlightsMaxEv, -0.75 - 1e-9);
    EXPECT_LE(r.settings.highlights, 0.0);
    EXPECT_LE(std::abs(r.settings.contrast), 0.15 + 1e-9);
    EXPECT_LE(std::abs(r.settings.saturation), 0.15 + 1e-9);
    const EditVector& e = r.candidates[r.chosen].edit;
    EXPECT_LE(std::abs(e.red_gain - 1.0), kWhiteBalanceResidualBound + 1e-9);
    EXPECT_LE(std::abs(e.blue_gain - 1.0), kWhiteBalanceResidualBound + 1e-9);
    EXPECT_TRUE(std::isfinite(r.confidence));
    EXPECT_TRUE(r.candidates[r.chosen].analysis.feasible);
    EXPECT_LE(r.timing.proxy_candidates, kDefaultCandidateBudget);
    for (const MetricResult& m : r.diagnostics) {
        for (const NamedValue& v : m.values) {
            EXPECT_TRUE(std::isfinite(v.value)) << to_string(m.id) << "." << v.name;
        }
    }
    const MetricResult* validity = r.diagnostic(MetricId::NumericalValidity);
    ASSERT_NE(validity, nullptr);
    EXPECT_EQ(validity->status, MetricStatus::Pass);

    // Deterministic.
    EXPECT_EQ(r.settings, second->settings);
    EXPECT_EQ(r.chosen, second->chosen);
    EXPECT_EQ(r.confidence, second->confidence);
    ASSERT_EQ(r.candidates.size(), second->candidates.size());
    for (std::size_t i = 0; i < r.candidates.size(); ++i) {
        EXPECT_EQ(r.candidates[i].settings, second->candidates[i].settings) << i;
        EXPECT_EQ(r.candidates[i].proxy.terms, second->candidates[i].proxy.terms) << i;
        EXPECT_EQ(r.candidates[i].analysis.terms, second->candidates[i].analysis.terms) << i;
    }

    // The up-front estimate (from the probe, as a scheduler would use it) covers the controller's own.
    raw::ProbeOptions probe_options;
    auto selection = registry->select(path, probe_options, {});
    ASSERT_TRUE(selection) << selection.error().describe();
    auto estimate = estimate_auto_memory(selection->probe.metadata);
    ASSERT_TRUE(estimate) << estimate.error().describe();
    EXPECT_GE(*estimate, r.memory.full_resolution_bytes);
    std::printf("[ AUTO     ] %s: estimate_auto_memory %llu MiB; warnings: %s\n", id.c_str(),
                static_cast<unsigned long long>(*estimate >> 20),
                r.warnings.empty() ? "none" : r.warnings.front().c_str());

    const std::string line = summary(id, r, peak);
    std::printf("[ AUTO     ] %s\n[ AUTO     ] second run %.2f s\n", line.c_str(),
                second->timing.total_seconds);
    RecordProperty("total_seconds", std::format("{:.3f}", r.timing.total_seconds));
    RecordProperty("second_total_seconds", std::format("{:.3f}", second->timing.total_seconds));
    RecordProperty("proxy_candidates", static_cast<int>(r.timing.proxy_candidates));
    RecordProperty("peak_working_set_mib", static_cast<int>(peak >> 20));
}

INSTANTIATE_TEST_SUITE_P(Smoke, AutoFixture,
                         ::testing::Values("canon-eos-r5-cr3-raw", "sony-ilce-7m4-arw-compressed",
                                           "dji-fc4382-dng", "fujifilm-x-t4-raf-lossless",
                                           "nikon-z-6-nef-lossless"),
                         [](const ::testing::TestParamInfo<const char*>& info) {
                             std::string name = info.param;
                             for (char& c : name) {
                                 if (c == '-') {
                                     c = '_';
                                 }
                             }
                             return name;
                         });

}  // namespace
}  // namespace lucent::analysis
