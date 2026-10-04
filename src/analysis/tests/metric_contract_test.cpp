#include <set>
#include <string>

#include <gtest/gtest.h>

#include <lucent/analysis/metric_contract.hpp>
#include <lucent/analysis/objective.hpp>

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
    EXPECT_TRUE(better(small, large));
    EXPECT_FALSE(better(large, small));
    const RankKey clearly{true, 0.9, 2.0, 2};
    EXPECT_TRUE(better(clearly, small));
    const RankKey infeasible{false, 0.0, 0.0, 3};
    EXPECT_TRUE(better(small, infeasible));
}

}  // namespace
}  // namespace lucent::analysis
