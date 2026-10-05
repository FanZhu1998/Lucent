#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/analysis/metric_contract.hpp>
#include <lucent/analysis/objective.hpp>
#include <lucent/engine/develop_operations.hpp>

namespace lucent::analysis {
namespace {

TEST(MetricContract, EveryMetricHasACompleteSpecification) {
    std::set<std::string> names;
    std::uint8_t expected = 1;
    for (const MetricSpec& spec : metric_specs()) {
        EXPECT_EQ(static_cast<std::uint8_t>(spec.id), expected++) << "specs are in MetricId order";
        EXPECT_EQ(spec.name, to_string(spec.id));
        EXPECT_TRUE(names.insert(std::string(spec.name)).second) << spec.name;
        EXPECT_FALSE(spec.domain.empty()) << spec.name;
        EXPECT_FALSE(spec.mask.empty()) << spec.name;
        EXPECT_FALSE(spec.units.empty()) << spec.name;
        EXPECT_FALSE(spec.baseline.empty()) << spec.name;
        EXPECT_FALSE(spec.missing_data.empty()) << spec.name;
        EXPECT_GT(spec.min_valid_samples, 0u) << spec.name;
        EXPECT_EQ(&metric_spec(spec.id), &spec);
        if (spec.hard_check) {
            EXPECT_FALSE(spec.thresholds.empty()) << spec.name << " is a hard check without a threshold";
        }
    }
    EXPECT_EQ(names.size(), 9u);
}

TEST(MetricContract, ThresholdsAreTheConstantsTheCodeUses) {
    const auto threshold = [](MetricId id, std::string_view name) {
        for (const MetricThreshold& t : metric_spec(id).thresholds) {
            if (t.name == name) {
                return t.value;
            }
        }
        return -1.0;
    };
    EXPECT_EQ(threshold(MetricId::RenderClipping, "new_fraction"), kMaxNewHighlightClipFraction);
    EXPECT_EQ(threshold(MetricId::ShadowBlocking, "new_fraction"), kMaxNewShadowBlockFraction);
    EXPECT_EQ(threshold(MetricId::ColorGamut, "new_out_of_gamut_fraction"), kMaxNewGamutFraction);
    EXPECT_EQ(threshold(MetricId::ColorGamut, "hue_shift_degrees"), kMaxHueShiftDegrees);
    EXPECT_EQ(threshold(MetricId::WhiteBalance, "neutral_shift_de"), kMaxNeutralShiftDeltaE);
    EXPECT_EQ(threshold(MetricId::NativeDetail, "halo_lstar"), kMaxHaloLStar);
    EXPECT_EQ(threshold(MetricId::NativeDetail, "highlight_new_clip_fraction"), kMaxNativeNewClipFraction);
    EXPECT_EQ(threshold(MetricId::ShadowNoise, "amplification"), kMaxShadowNoiseAmplification);
}

TEST(MetricContract, ResultValuesAreLookedUpByName) {
    MetricResult m;
    m.values = {{"a", 1.0}, {"b", 2.0}};
    EXPECT_EQ(m.value("b"), 2.0);
    EXPECT_FALSE(m.value("c").has_value());
    EXPECT_EQ(to_string(MetricStatus::InsufficientEvidence), "insufficient_evidence");
}

// "Too little evidence never scores well": a preference metric without evidence turns into a penalty
// on the edit it governs, so the same edit never scores better without evidence than with it.
TEST(MetricContract, MissingEvidenceNeverScoresBetter) {
    RenderStats base;
    base.pixels = 100000;
    base.quantiles.p25 = 0.2;
    base.quantiles.p50 = 0.3;
    base.quantiles.p75 = 0.5;
    base.signal_pixels = 100000;
    base.noise_model_available = true;
    base.shadow_pixels = 20000;
    base.shadow_noise_lstar = 0.5;
    base.chromatic_pixels = 20000;
    RenderStats brighter = base;
    brighter.quantiles.p50 = 0.42;  // reaches the band
    brighter.quantiles.p25 = 0.3;
    brighter.quantiles.p75 = 0.62;

    NaturalV1Context with;
    with.tone.evidence = true;
    with.tone.band_low = 0.40;
    with.tone.band_high = 0.85;
    NaturalV1Context without = with;
    without.tone.evidence = false;

    EditVector edit;
    edit.exposure_ev = 0.6;
    const ObjectiveTerms a = natural_v1_objective(brighter, base, edit, with, nullptr);
    const ObjectiveTerms b = natural_v1_objective(brighter, base, edit, without, nullptr);
    EXPECT_GT(b.brightness, a.brightness);
    EXPECT_GT(b.total, a.total);
    // Without evidence the unchanged baseline is preferred over any exposure change.
    const ObjectiveTerms unchanged = natural_v1_objective(base, base, EditVector{}, without, nullptr);
    EXPECT_LT(unchanged.total + kTieTolerance, b.total);
}

TEST(MetricContract, TiesPreferTheSmallerEdit) {
    const RankKey large{true, 1.000, 2.0, 0};
    const RankKey small{true, 1.015, 0.5, 1};
    const std::vector<RankKey> tied{large, small};
    EXPECT_EQ(select_best(tied), 1u);
    const RankKey clearly{true, 0.9, 2.0, 2};
    const std::vector<RankKey> with_clear{large, small, clearly};
    EXPECT_EQ(select_best(with_clear), 2u);
    const RankKey infeasible{false, 0.0, 0.0, 3};
    const std::vector<RankKey> with_infeasible{infeasible, small};
    EXPECT_EQ(select_best(with_infeasible), 1u);
    // Nothing feasible: the lowest total among all.
    const std::vector<RankKey> none{RankKey{false, 2.0, 0.0, 0}, RankKey{false, 1.0, 1.0, 1}};
    EXPECT_EQ(select_best(none), 1u);
    EXPECT_EQ(select_best(std::vector<RankKey>{}), 0u);
}

TEST(MetricContract, TiesDoNotChainAndIgnoreTheOrder) {
    // Each candidate is within the tolerance of the next and smaller: a pairwise scan would walk
    // from the best (1.000) to the smallest edit (1.039). The two-step rule keeps every winner
    // within kTieTolerance of the lowest total, in any order.
    const std::vector<RankKey> chain{RankKey{true, 1.000, 2.0, 0}, RankKey{true, 1.017, 1.9, 1},
                                     RankKey{true, 1.034, 1.8, 2}, RankKey{true, 1.039, 0.1, 3}};
    EXPECT_EQ(select_best(chain), 1u);
    std::vector<RankKey> reversed(chain.rbegin(), chain.rend());
    EXPECT_EQ(reversed[select_best(reversed)].index, 1u);
    // Equal totals and edits: the lowest index.
    const std::vector<RankKey> equal{RankKey{true, 1.0, 1.0, 7}, RankKey{true, 1.0, 1.0, 3}};
    EXPECT_EQ(equal[select_best(equal)].index, 3u);
}

// Exposure-invariant contrast (ADR 0011, ANA-02): a pure exposure change in the base look's linear
// segment leaves the log-luminance range unchanged; a shadow lift that compresses the range is
// penalised; flattened local contrast is penalised, more local contrast is not.
TEST(MetricContract, ContrastTermsAreExposureInvariantAndPenaliseFlattening) {
    const auto at = [](double l, double ev) {
        // The lightness l after a gain of ev EV in display luminance.
        const double y = engine::display_lightness_inverse(l) * std::exp2(ev);
        return engine::display_lightness(y);
    };
    RenderStats base;
    base.pixels = 100000;
    base.signal_pixels = 100000;
    base.quantiles.p25 = 0.15;
    base.quantiles.p50 = 0.25;
    base.quantiles.p75 = 0.40;
    base.quantiles.p90 = 0.50;
    base.local_contrast_ev = 0.30;
    NaturalV1Context ctx;
    ctx.tone.evidence = true;
    EditVector exposure;
    exposure.exposure_ev = 1.0;
    RenderStats brighter = base;
    brighter.quantiles.p25 = at(0.15, 1.0);
    brighter.quantiles.p50 = at(0.25, 1.0);
    brighter.quantiles.p75 = at(0.40, 1.0);
    brighter.quantiles.p90 = at(0.50, 1.0);
    const ObjectiveTerms e = natural_v1_objective(brighter, base, exposure, ctx, nullptr);
    EXPECT_NEAR(e.contrast, 0.0, 1e-12);
    EXPECT_EQ(e.local_contrast, 0.0);
    // Phase 1 measured the IQR in L*: the same exposure change read as harshening.
    EXPECT_GT(std::abs(std::log((brighter.quantiles.p75 - brighter.quantiles.p25) / (0.40 - 0.15))), 0.2);

    // A lift that raises the lower quartile by 1 EV and the upper by 0.3 EV flattens the range.
    EditVector lift;
    lift.shadows_ev = 0.6;
    RenderStats lifted = base;
    lifted.quantiles.p25 = at(0.15, 1.0);
    lifted.quantiles.p75 = at(0.40, 0.3);
    lifted.local_contrast_ev = 0.22;
    const ObjectiveTerms f = natural_v1_objective(lifted, base, lift, ctx, nullptr);
    EXPECT_GT(f.contrast, 1.0);
    EXPECT_GT(f.local_contrast, 1.0);
    RenderStats crisper = base;
    crisper.local_contrast_ev = 0.40;
    EXPECT_EQ(natural_v1_objective(crisper, base, EditVector{}, ctx, nullptr).local_contrast, 0.0);
}

// The near-black term counts only near-black area a candidate adds (ADR 0011, ANA-07).
TEST(MetricContract, TheScenesOwnDeepShadowsAreNotPenalised) {
    RenderStats night;
    night.pixels = 100000;
    night.signal_pixels = 100000;
    night.quantiles.p50 = 0.05;
    night.quantiles.p90 = 0.30;
    night.dark = 60000;  // 60 % near black, as shot
    NaturalV1Context ctx;
    ctx.tone.evidence = true;
    EXPECT_EQ(natural_v1_objective(night, night, EditVector{}, ctx, nullptr).shadows, 0.0);
    RenderStats darker = night;
    darker.dark = 90000;
    EXPECT_GT(natural_v1_objective(darker, night, EditVector{}, ctx, nullptr).shadows, 0.4);
}

// With exposure locked the brightness term is 0: local tone is not used to get around the lock.
TEST(MetricContract, ALockedExposureIsTheUsersKey) {
    RenderStats dark;
    dark.pixels = 100000;
    dark.signal_pixels = 100000;
    dark.quantiles.p50 = 0.15;
    dark.quantiles.p90 = 0.25;
    NaturalV1Context ctx;
    ctx.tone.evidence = true;
    EXPECT_GT(natural_v1_objective(dark, dark, EditVector{}, ctx, nullptr).brightness, 3.0);
    ctx.exposure_locked = true;
    EXPECT_EQ(natural_v1_objective(dark, dark, EditVector{}, ctx, nullptr).brightness, 0.0);
}

// The brightness term grows linearly beyond one scale unit (Huber), so a large deficit cannot
// outweigh every other term.
TEST(MetricContract, BrightnessIsLinearBeyondOneScaleUnit) {
    NaturalV1Context ctx;
    ctx.tone.evidence = true;
    const auto brightness_at = [&](double deficit_ev) {
        RenderStats s;
        s.pixels = 100000;
        s.signal_pixels = 100000;
        const double y = engine::display_lightness_inverse(kBandLow) * std::exp2(-deficit_ev);
        s.quantiles.p50 = engine::display_lightness(y);
        s.quantiles.p90 = s.quantiles.p50;  // dark bright content too
        return natural_v1_objective(s, s, EditVector{}, ctx, nullptr).brightness;
    };
    EXPECT_NEAR(brightness_at(0.25), 0.25, 1e-9);   // (0.25 / 0.5)^2
    EXPECT_NEAR(brightness_at(0.5), 1.0, 1e-9);
    EXPECT_NEAR(brightness_at(2.0), 7.0, 1e-9);     // 2 x 4 - 1, not 16
}

// A per-pixel candidate (no local tone) cannot create a halo: its measured halo, which only shows how
// the tone change bends near and far lightness differently, is reported but neither scored nor
// checked.
TEST(MetricContract, HalosCountOnlyForLocalTone) {
    RenderStats s;
    s.pixels = 100000;
    s.signal_pixels = 100000;
    s.quantiles.p50 = 0.45;
    s.quantiles.p90 = 0.7;
    NaturalV1Context ctx;
    ctx.tone.evidence = true;
    NativeComparison native;
    native.available = true;
    native.edge_samples = 500;
    native.halo_lstar = 5.0;
    EditVector local;
    local.shadows_ev = 0.5;
    EXPECT_NEAR(natural_v1_objective(s, s, local, ctx, &native).native, (5.0 / 1.5) * (5.0 / 1.5), 1e-12);
    EXPECT_FALSE(evaluate_candidate(s, s, local, ctx, &native, false, true).feasible);
    native.halo_applies = false;
    EditVector exposure;
    exposure.exposure_ev = 0.8;
    EXPECT_EQ(natural_v1_objective(s, s, exposure, ctx, &native).native, 0.0);
    const CandidateEvaluation e = evaluate_candidate(s, s, exposure, ctx, &native, false, true);
    EXPECT_TRUE(e.feasible);
    const auto native_metric = std::find_if(e.metrics.begin(), e.metrics.end(),
                                            [](const MetricResult& m) { return m.id == MetricId::NativeDetail; });
    ASSERT_NE(native_metric, e.metrics.end());
    EXPECT_EQ(native_metric->value("halo_lstar"), 5.0);
    EXPECT_EQ(native_metric->value("halo_applies"), 0.0);
}

// A native highlight crop newly clipped beyond the limit fails; below it, it costs (ANA-10).
TEST(MetricContract, NativeHighlightClippingIsCheckedAndScored) {
    RenderStats s;
    s.pixels = 100000;
    s.signal_pixels = 100000;
    s.quantiles.p50 = 0.45;
    s.quantiles.p90 = 0.7;
    NaturalV1Context ctx;
    ctx.tone.evidence = true;
    NativeComparison native;
    native.available = true;
    native.highlight_available = true;
    native.highlight_new_clip_fraction = 0.005;
    EditVector edit;
    edit.exposure_ev = 0.3;
    const ObjectiveTerms t = natural_v1_objective(s, s, edit, ctx, &native);
    EXPECT_NEAR(t.native, 0.25, 1e-12);
    EXPECT_TRUE(evaluate_candidate(s, s, edit, ctx, &native, false, true).feasible);
    native.highlight_new_clip_fraction = 0.03;
    const CandidateEvaluation failed = evaluate_candidate(s, s, edit, ctx, &native, false, true);
    EXPECT_FALSE(failed.feasible);
    ASSERT_FALSE(failed.failures.empty());
    EXPECT_NE(failed.failures.front().find("highlight crop"), std::string::npos);
}

// The final check measures inside the photographer's crop: a white-balance change the whole
// frame's neutral evidence supports stays feasible when the crop holds no neutral surface (the
// outcome does not depend on the crop, ADR 0015), and still fails without evidence anywhere.
TEST(MetricContract, AWhiteBalanceChangeIsNotFailedByACropWithoutNeutralSurfaces) {
    RenderStats s;
    s.pixels = 100000;
    s.signal_pixels = 100000;
    s.quantiles.p50 = 0.45;
    s.quantiles.p90 = 0.7;
    s.neutral_pixels = 10;  // the crop shows foliage only
    NaturalV1Context ctx;
    ctx.tone.evidence = true;
    ctx.white_balance_evidence = true;
    ctx.white_balance_confidence = 0.8;
    EditVector edit;
    edit.red_gain = 0.95;
    edit.blue_gain = 1.04;
    const CandidateEvaluation without = evaluate_candidate(s, s, edit, ctx, nullptr, false, true);
    EXPECT_FALSE(without.feasible);
    ctx.neutral_evidence_beyond_crop = true;
    const CandidateEvaluation beyond = evaluate_candidate(s, s, edit, ctx, nullptr, false, true);
    EXPECT_TRUE(beyond.feasible) << (beyond.failures.empty() ? "" : beyond.failures.front());
    const auto wb = std::find_if(beyond.metrics.begin(), beyond.metrics.end(),
                                 [](const MetricResult& m) { return m.id == MetricId::WhiteBalance; });
    ASSERT_NE(wb, beyond.metrics.end());
    EXPECT_EQ(wb->status, MetricStatus::InsufficientEvidence);
    EXPECT_NE(wb->detail.find("outside the crop"), std::string::npos) << wb->detail;
    // With neutral surfaces inside the crop the change is verified there as before.
    s.neutral_pixels = 5000;
    s.neutral_shift_de = 50.0;
    EXPECT_FALSE(evaluate_candidate(s, s, edit, ctx, nullptr, false, true).feasible);
}

}  // namespace
}  // namespace lucent::analysis
