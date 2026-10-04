// Natural v1 on synthetic scenes (blueprint section 6): an underexposed chart is brightened;
// deliberate low-key and high-key scenes keep their key; a warm sunset keeps its colour while a
// small tint is corrected; sensor-clipped highlights are not worsened; locks are honoured; too
// little evidence gives a restrained edit with a flag; the result is deterministic.
//
// Every result is summarised in the log ("[ AUTO     ]" lines). Run with
// --gtest_also_run_disabled_tests to dump every evaluated candidate and its objective terms as well.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <string>

#include <gtest/gtest.h>

#include <lucent/analysis/auto_settings.hpp>
#include <lucent/engine/develop_operations.hpp>

#include "analysis_test_support.hpp"

namespace lucent::analysis {
namespace {

using test_support::chart_scene;
using test_support::make_scene;
using test_support::neutral;
using test_support::SceneOptions;
using test_support::texture;

// One line per result in the test log: what was chosen and why.
void trace(const AutoResult& r) {
    if (r.candidates.empty()) {
        return;
    }
    const CandidateRecord& c = r.candidates[r.chosen];
    std::string flags;
    for (const ReviewFlag& f : r.flags) {
        flags += std::format("{}{}", flags.empty() ? "" : ",", to_string(f.code));
    }
    const MetricResult* tones = r.diagnostic(MetricId::TonalDistribution);
    const MetricResult* native = r.diagnostic(MetricId::NativeDetail);
    if (native != nullptr && !native->values.empty()) {
        std::printf(
            "[ AUTO     ] native: halo %.2f L*, detail x%.3f, shadow texture %.2f L* (x%.2f), %g edge "
            "samples\n",
            native->value("halo_lstar").value_or(0.0), native->value("detail_ratio").value_or(0.0),
            native->value("shadow_texture_lstar").value_or(0.0),
            native->value("shadow_texture_ratio").value_or(0.0), native->value("edge_samples").value_or(0.0));
    }
    if (r.white_balance && r.white_balance->evidence) {
        std::string estimates;
        for (const WhiteBalanceEstimate& e : r.white_balance->estimates) {
            estimates += std::format(" {}{}({:+.3f},{:+.3f} c{:.2f})", to_string(e.method),
                                     e.available ? "" : "-", e.log_rg, e.log_bg, e.confidence);
        }
        std::printf("[ AUTO     ] white balance:%s -> confidence %.2f, spread %.3f\n", estimates.c_str(),
                    r.white_balance->confidence, r.white_balance->spread);
    }
    std::printf(
        "[ AUTO     ] %s\n",
        std::format(
            "chosen {} ({}) of {}: exposure {:+.3f}, wb r {:.4f} b {:.4f}, shadows {:.3f}, highlights "
            "{:.3f}, contrast {:+.3f}, saturation x{:.3f}; p50 {:.3f} (baseline {:.3f}, band [{:.2f}, "
            "{:.2f}]); score {:.3f} (baseline {:.3f}); confidence {:.2f}{}; flags [{}]; {:.2f} s",
            r.chosen, to_string(c.origin), r.candidates.size(), c.edit.exposure_ev, c.edit.red_gain,
            c.edit.blue_gain, c.edit.shadows_ev, c.edit.highlights_ev, c.edit.contrast, c.edit.saturation,
            tones != nullptr ? tones->value("p50").value_or(-1.0) : -1.0,
            r.candidates[0].analysis.median_lightness, r.tone.band_low, r.tone.band_high,
            c.analysis.terms.total, r.candidates[0].analysis.terms.total, r.confidence,
            r.restrained ? " (restrained)" : "", flags, r.timing.total_seconds)
            .c_str());
}

[[maybe_unused]] void dump(const AutoResult& r) {
    for (const CandidateRecord& c : r.candidates) {
        const auto score = [](const CandidateScore& s) {
            if (!s.evaluated) {
                return std::string("-");
            }
            const ObjectiveTerms& t = s.terms;
            return std::format(
                "{} {:.3f} [b {:.2f} h {:.2f} s {:.2f} n {:.2f} c {:.2f} col {:.2f} wb {:.2f} nat {:.2f} "
                "e {:.2f}] p50 {:.3f}{}",
                s.feasible ? "ok" : "NO", t.total, t.brightness, t.highlights, t.shadows, t.noise, t.contrast,
                t.color, t.white_balance, t.native, t.edit, s.median_lightness,
                s.failures.empty() ? "" : " " + s.failures.front());
        };
        std::printf("  %2u %-22s E%+.3f r%.3f b%.3f S%.3f H%.3f C%+.3f s%.3f | proxy %s | analysis %s\n",
                    c.index, std::string(to_string(c.origin)).c_str(), c.edit.exposure_ev, c.edit.red_gain,
                    c.edit.blue_gain, c.edit.shadows_ev, c.edit.highlights_ev, c.edit.contrast,
                    c.edit.saturation, score(c.proxy).c_str(), score(c.analysis).c_str());
    }
}

AutoResult run(const test_support::Scene& scene, const engine::DevelopSettings& current = {},
               const SceneOptions& o = {}, const AutoOptions& options = {}) {
    auto result = auto_develop(make_scene(scene, o), current, options);
    EXPECT_TRUE(result) << result.error().describe();
    if (!result) {
        return AutoResult{};
    }
    trace(*result);
    if (::testing::GTEST_FLAG(also_run_disabled_tests)) {
        dump(*result);
    }
    return std::move(*result);
}

const CandidateRecord& chosen(const AutoResult& r) {
    return r.candidates.at(r.chosen);
}
const CandidateRecord& baseline(const AutoResult& r) {
    return r.candidates.at(0);
}

double chosen_p50(const AutoResult& r) {
    const MetricResult* tones = r.diagnostic(MetricId::TonalDistribution);
    return tones != nullptr ? tones->value("p50").value_or(-1.0) : -1.0;
}

// Every result: valid settings inside the search bounds, a bounded number of candidates.
void expect_well_formed(const AutoResult& r, const AutoOptions& options = {}) {
    ASSERT_FALSE(r.candidates.empty());
    EXPECT_EQ(r.candidates[0].origin, CandidateOrigin::Baseline);
    EXPECT_LE(r.timing.proxy_candidates, options.candidate_budget);
    EXPECT_TRUE(r.settings.validate());
    EXPECT_TRUE(std::isfinite(r.confidence));
    EXPECT_GE(r.confidence, 0.0);
    EXPECT_LE(r.confidence, 1.0);
    EXPECT_EQ(r.versions.policy, "natural");
    EXPECT_EQ(r.versions.policy_version, kNaturalPolicyVersion);
    EXPECT_EQ(r.versions.search_version, kSearchVersion);
    for (const CandidateRecord& c : r.candidates) {
        EXPECT_TRUE(c.settings.validate()) << c.note;
        if (c.point) {
            EXPECT_GE(c.point->exposure_ev, -1.5);
            EXPECT_LE(c.point->exposure_ev, 1.5);
            EXPECT_GE(c.point->shadows_ev, 0.0);
            EXPECT_LE(c.point->shadows_ev, 0.75);
            EXPECT_GE(c.point->highlights_ev, -0.75);
            EXPECT_LE(c.point->highlights_ev, 0.0);
            EXPECT_LE(std::abs(c.point->contrast), 0.15 + 1e-12);
            EXPECT_GE(c.point->saturation, 0.85 - 1e-12);
            EXPECT_LE(c.point->saturation, 1.15 + 1e-12);
            EXPECT_LE(std::abs(c.point->red_gain - 1.0), kWhiteBalanceResidualBound + 1e-12);
            EXPECT_LE(std::abs(c.point->blue_gain - 1.0), kWhiteBalanceResidualBound + 1e-12);
        }
    }
    const CandidateRecord& c = chosen(r);
    EXPECT_TRUE(c.analysis.evaluated);
    EXPECT_TRUE(c.analysis.feasible) << (c.analysis.failures.empty() ? "" : c.analysis.failures.front());
    ASSERT_NE(r.diagnostic(MetricId::SourceSaturation), nullptr);
    ASSERT_NE(r.diagnostic(MetricId::RenderClipping), nullptr);
    EXPECT_LE(r.diagnostic(MetricId::RenderClipping)->value("new_fraction").value_or(1.0),
              kMaxNewHighlightClipFraction);
}

TEST(AutoSettings, BrightensAnUnderexposedChart) {
    const AutoResult r = run(chart_scene(std::exp2(-2.5)));
    expect_well_formed(r);
    EXPECT_GE(r.settings.exposure_ev, 1.0);
    EXPECT_TRUE(r.tone.evidence);
    EXPECT_GT(r.tone.needed_ev, 1.0);
    EXPECT_GT(chosen_p50(r), baseline(r).analysis.median_lightness + 0.1);
    EXPECT_FALSE(r.has_flag(ReviewFlagCode::LowConfidence));
    EXPECT_FALSE(r.has_flag(ReviewFlagCode::SourceSaturation));
    EXPECT_EQ(r.crops.size(), 4u);
    EXPECT_GT(r.timing.analysis_candidates, 1u);
    EXPECT_GE(r.timing.proxy_candidates, 24u);  // the budget is used for the search
}

TEST(AutoSettings, BrightensAnUnderexposedGreyCard) {
    // A frame-filling 18 % grey card, 2 EV under, with a faint texture: few edges, a clear median.
    const AutoResult r = run([](std::int32_t x, std::int32_t y) {
        return neutral(0.18 * test_support::kProperGain * 0.25 * (1.0 + 0.02 * texture(x, y)));
    });
    expect_well_formed(r);
    EXPECT_TRUE(r.tone.evidence);
    EXPECT_GE(r.settings.exposure_ev, 1.0);
    EXPECT_GT(chosen_p50(r), baseline(r).analysis.median_lightness + 0.1);
    EXPECT_LE(r.settings.shadows * engine::kShadowsMaxEv, 0.75);
}

// A bright sky over a dark silhouette: the dark foreground is deliberate, not underexposure.
test_support::Scene silhouette() {
    return [](std::int32_t x, std::int32_t y) -> std::array<double, 3> {
        const double ridge = 230.0 + 60.0 * std::sin(0.013 * x) + 25.0 * std::sin(0.05 * x);
        if (y < ridge) {
            const double level = 0.72 - 0.15 * y / 260.0 + 0.01 * texture(x, y);
            return neutral(level, 0.95, 1.10);
        }
        return neutral(0.004 * (1.0 + 0.3 * texture(x, y)));
    };
}

TEST(AutoSettings, KeepsADeliberateSilhouetteDark) {
    const AutoResult r = run(silhouette());
    expect_well_formed(r);
    EXPECT_GT(r.tone.low_key, 0.9);
    EXPECT_LE(r.settings.exposure_ev, 0.25);
    EXPECT_LE(r.settings.shadows * engine::kShadowsMaxEv, 0.25);
    EXPECT_LT(chosen_p50(r), 0.12);  // not forced towards mid-grey (0.5)
}

// Snow: most of the frame a bright neutral just below the clip, some dark trees.
test_support::Scene snow() {
    return [](std::int32_t x, std::int32_t y) -> std::array<double, 3> {
        const bool tree = (x % 160) < 22 && y > 200 + (x % 37);
        if (tree) {
            return neutral(0.03 * (1.0 + 0.2 * texture(x, y)));
        }
        return neutral(0.60 * (1.0 + 0.03 * texture(x, y)), 0.98, 1.03);
    };
}

TEST(AutoSettings, KeepsHighKeySnowBright) {
    const AutoResult r = run(snow());
    expect_well_formed(r);
    EXPECT_GE(r.settings.exposure_ev, -0.6);
    EXPECT_GE(chosen_p50(r), 0.75);  // not pulled towards mid-grey
}

// Relative remaining cast (log norm) after the chosen white balance.
double preserved_cast(const AutoResult& r, double cast_r, double cast_b) {
    const EditVector& e = chosen(r).edit;
    const double before = std::hypot(std::log(cast_r), std::log(cast_b));
    const double after =
        std::hypot(std::log(cast_r) + std::log(e.red_gain), std::log(cast_b) + std::log(e.blue_gain));
    return after / before;
}

TEST(AutoSettings, PreservesAWarmSunsetCast) {
    const AutoResult r = run(chart_scene(1.0, 1.35, 0.70));
    expect_well_formed(r);
    ASSERT_TRUE(r.white_balance.has_value());
    EXPECT_TRUE(r.white_balance->evidence);
    EXPECT_GT(r.white_balance->warmth, 0.2);  // the estimators see the warm light
    EXPECT_GE(preserved_cast(r, 1.35, 0.70), 0.85);
    EXPECT_GE(chosen(r).edit.red_gain, 1.0 - kWhiteBalanceResidualBound);
}

TEST(AutoSettings, CorrectsASmallTint) {
    const AutoResult r = run(chart_scene(1.0, 1.05, 1.05));
    expect_well_formed(r);
    ASSERT_TRUE(r.white_balance.has_value());
    EXPECT_GT(r.white_balance->confidence, 0.5);
    EXPECT_LT(chosen(r).edit.red_gain, 0.985);
    EXPECT_LT(chosen(r).edit.blue_gain, 0.985);
    EXPECT_LT(preserved_cast(r, 1.05, 1.05), 0.5);
    EXPECT_TRUE(std::holds_alternative<engine::CameraNeutralWhiteBalance>(r.settings.white_balance));
}

// A sensor-saturated sky over an unclipped bright gradient and mid-tones.
test_support::Scene clipped_sky() {
    return [](std::int32_t x, std::int32_t y) -> std::array<double, 3> {
        if (y < 190) {
            return neutral(2.5);  // every channel above the white level
        }
        if (y < 260) {
            return neutral(0.95 - 0.45 * (y - 190) / 70.0 + 0.005 * texture(x, y));
        }
        return chart_scene(1.0)(x, y);
    };
}

TEST(AutoSettings, DoesNotWorsenClippedHighlights) {
    const AutoResult r = run(clipped_sky());
    expect_well_formed(r);
    EXPECT_TRUE(r.has_flag(ReviewFlagCode::SourceSaturation));
    for (const ChannelSaturation& c : r.source.channels) {
        EXPECT_GT(c.fraction(), 0.25) << static_cast<int>(c.color);
    }
    const MetricResult* clip = r.diagnostic(MetricId::RenderClipping);
    ASSERT_NE(clip, nullptr);
    EXPECT_LE(clip->value("new_fraction").value(), kMaxNewHighlightClipFraction);
    EXPECT_LE(clip->value("total_fraction").value(),
              clip->value("baseline_fraction").value() + kMaxNewHighlightClipFraction);
    EXPECT_GT(clip->value("source_fraction").value(), 0.25);
    // The clipped sky is not turned into flat grey by highlight compression.
    EXPECT_LE(clip->value("saturated_greying").value(), 0.05);
}

TEST(AutoSettings, HonoursLocksEverywhere) {
    engine::DevelopSettings current;
    current.exposure_ev = 0.0;
    current.contrast = 0.3;  // outside the search range: kept because locked
    current.white_balance = engine::KelvinWhiteBalance{5000.0, 0.0};
    current.tone_curve.points = {{0.0, 0.0}, {0.5, 0.55}, {1.0, 1.0}};  // not searched: carried over
    current.locks = {engine::DevelopControl::Exposure, engine::DevelopControl::Contrast,
                     engine::DevelopControl::WhiteBalance};
    const AutoResult r = run(chart_scene(std::exp2(-2.5)), current);
    expect_well_formed(r);
    EXPECT_EQ(r.settings.exposure_ev, 0.0);
    EXPECT_EQ(r.settings.contrast, 0.3);
    EXPECT_EQ(r.settings.white_balance, current.white_balance);
    EXPECT_EQ(r.settings.tone_curve, current.tone_curve);
    EXPECT_EQ(r.settings.locks, current.locks);
    for (const CandidateRecord& c : r.candidates) {
        EXPECT_EQ(c.settings.exposure_ev, 0.0) << c.note;
        EXPECT_EQ(c.settings.contrast, 0.3) << c.note;
        EXPECT_EQ(c.settings.white_balance, current.white_balance) << c.note;
        EXPECT_EQ(c.settings.locks, current.locks) << c.note;
    }
    EXPECT_FALSE(r.white_balance.has_value());  // no white-balance proposal for a locked white balance
    // With exposure locked, the unlocked shadows carry the brightening.
    EXPECT_GT(r.settings.shadows, 0.0);
}

TEST(AutoSettings, InsufficientEvidenceGivesARestrainedFlaggedResult) {
    // A lens-cap frame: the signal is about one read-noise sigma.
    const AutoResult r = run([](std::int32_t, std::int32_t) { return neutral(3e-4); });
    expect_well_formed(r);
    EXPECT_FALSE(r.tone.evidence);
    EXPECT_LE(std::abs(r.settings.exposure_ev), 0.5);
    EXPECT_LE(r.settings.shadows * engine::kShadowsMaxEv, 0.25);
    EXPECT_TRUE(r.has_flag(ReviewFlagCode::InsufficientEvidence));
    EXPECT_TRUE(r.has_flag(ReviewFlagCode::WhiteBalanceUncertain));
    EXPECT_TRUE(std::holds_alternative<engine::AsShotWhiteBalance>(r.settings.white_balance));
    const MetricResult* tones = r.diagnostic(MetricId::TonalDistribution);
    ASSERT_NE(tones, nullptr);
    EXPECT_EQ(tones->status, MetricStatus::InsufficientEvidence);
}

TEST(AutoSettings, LowConfidenceRestrainsTheEdit) {
    // The underexposed chart again, from a file without ISO, noise profile or optical black: the
    // brightening amplifies noise nobody can predict, so it is restrained and flagged.
    SceneOptions no_model;
    no_model.iso.reset();
    const AutoResult free = run(chart_scene(std::exp2(-2.5)));
    const AutoResult r = run(chart_scene(std::exp2(-2.5)), {}, no_model);
    expect_well_formed(r);
    EXPECT_EQ(r.source.noise.source, NoiseModelSource::Unavailable);
    EXPECT_TRUE(r.has_flag(ReviewFlagCode::NoiseModelUnavailable));
    EXPECT_TRUE(r.restrained);
    EXPECT_TRUE(r.has_flag(ReviewFlagCode::LowConfidence));
    EXPECT_LT(r.confidence, kRestrainConfidence);
    EXPECT_EQ(chosen(r).origin, CandidateOrigin::Restrained);
    EXPECT_GT(r.settings.exposure_ev, 0.0);                        // still brighter than as shot
    EXPECT_LT(r.settings.exposure_ev, free.settings.exposure_ev);  // but less than with a noise model
}

TEST(AutoSettings, IsDeterministic) {
    const AutoResult a = run(chart_scene(0.5, 1.2, 0.85));
    const AutoResult b = run(chart_scene(0.5, 1.2, 0.85));
    EXPECT_EQ(a.settings, b.settings);
    EXPECT_EQ(a.chosen, b.chosen);
    EXPECT_EQ(a.confidence, b.confidence);
    ASSERT_EQ(a.candidates.size(), b.candidates.size());
    for (std::size_t i = 0; i < a.candidates.size(); ++i) {
        EXPECT_EQ(a.candidates[i].settings, b.candidates[i].settings) << i;
        EXPECT_EQ(a.candidates[i].proxy.terms, b.candidates[i].proxy.terms) << i;
        EXPECT_EQ(a.candidates[i].analysis.terms, b.candidates[i].analysis.terms) << i;
    }
    ASSERT_EQ(a.diagnostics.size(), b.diagnostics.size());
    for (std::size_t i = 0; i < a.diagnostics.size(); ++i) {
        EXPECT_EQ(a.diagnostics[i], b.diagnostics[i]) << to_string(a.diagnostics[i].id);
    }
    // Serial and parallel execution agree exactly.
    AutoOptions serial;
    serial.develop.parallel = false;
    const AutoResult c = run(chart_scene(0.5, 1.2, 0.85), {}, {}, serial);
    EXPECT_EQ(a.settings, c.settings);
    ASSERT_EQ(a.candidates.size(), c.candidates.size());
    for (std::size_t i = 0; i < a.candidates.size(); ++i) {
        EXPECT_EQ(a.candidates[i].proxy.terms, c.candidates[i].proxy.terms) << i;
    }
}

TEST(AutoSettings, TooSmallAnImageKeepsTheBaseline) {
    SceneOptions o;
    o.width = 48;
    o.height = 32;
    const AutoResult r = run([](std::int32_t, std::int32_t) { return neutral(0.02); }, {}, o);
    expect_well_formed(r);
    EXPECT_EQ(r.chosen, 0u);
    EXPECT_TRUE(r.settings.is_as_shot());
    EXPECT_TRUE(r.has_flag(ReviewFlagCode::InsufficientEvidence));
    EXPECT_TRUE(r.has_flag(ReviewFlagCode::NoFeasibleCandidate));
}

TEST(AutoSettings, MonochromeSearchesNoColour) {
    // An underexposed monochrome sensor: patches of reflectance, 2 EV under.
    constexpr std::int32_t w = 960;
    constexpr std::int32_t h = 640;
    auto decoded = engine::test_support::make_monochrome(
        w, h, image::PixelRect{0, 0, w, h},
        engine::test_support::simple_levels(test_support::kBlack, test_support::kWhite),
        [](std::int32_t x, std::int32_t y, std::int32_t) {
            const double rho = 0.05 + 0.6 * ((x / 120 + y / 160) % 6) / 5.0 + 0.01 * texture(x, y);
            const double v = rho * test_support::kProperGain * 0.25;
            const double noise = 3e-4 * test_support::gaussian(static_cast<std::uint64_t>(y) * 1000003ull +
                                                               static_cast<std::uint64_t>(x));
            return std::round(test_support::kBlack +
                              (v + noise) * (test_support::kWhite - test_support::kBlack));
        });
    decoded.metadata.capture.iso = 200.0f;
    auto result = auto_develop(std::move(decoded), {});
    ASSERT_TRUE(result) << result.error().describe();
    trace(*result);
    const AutoResult& r = *result;
    expect_well_formed(r);
    EXPECT_FALSE(r.white_balance.has_value());
    EXPECT_TRUE(std::holds_alternative<engine::AsShotWhiteBalance>(r.settings.white_balance));
    EXPECT_EQ(r.settings.saturation, 0.0);
    for (const CandidateRecord& c : r.candidates) {
        EXPECT_EQ(c.settings.saturation, 0.0) << c.note;
        EXPECT_TRUE(std::holds_alternative<engine::AsShotWhiteBalance>(c.settings.white_balance)) << c.note;
    }
    EXPECT_EQ(r.diagnostic(MetricId::ColorGamut)->status, MetricStatus::NotApplicable);
    EXPECT_EQ(r.diagnostic(MetricId::WhiteBalance)->status, MetricStatus::NotApplicable);
    EXPECT_GT(r.settings.exposure_ev, 0.5);  // still brightened
}

TEST(AutoSettings, RejectsInvalidInput) {
    AutoOptions options;
    options.candidate_budget = 2;
    auto bad_options = auto_develop(make_scene(chart_scene(1.0)), {}, options);
    ASSERT_FALSE(bad_options);
    EXPECT_EQ(bad_options.error().code, ErrorCode::InvalidArgument);
    engine::DevelopSettings bad;
    bad.exposure_ev = 99.0;
    auto bad_settings = auto_develop(make_scene(chart_scene(1.0)), bad);
    ASSERT_FALSE(bad_settings);
    EXPECT_EQ(bad_settings.error().code, ErrorCode::InvalidArgument);
}

TEST(AutoSettings, Cancels) {
    core::CancellationSource source;
    source.cancel();
    auto result = auto_develop(make_scene(chart_scene(1.0)), {}, {}, source.token());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::Cancelled);
}

}  // namespace
}  // namespace lucent::analysis
