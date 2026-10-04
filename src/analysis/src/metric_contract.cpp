#include <array>

#include <lucent/analysis/metric_contract.hpp>

namespace lucent::analysis {

namespace {

constexpr std::array<MetricThreshold, 2> kNumericalThresholds{{
    {"nonfinite_samples", 0.0, "any NaN or infinite output sample fails"},
    {"nonmonotone_curves", 0.0, "a contrast or tone curve that is not non-decreasing fails"},
}};

constexpr std::array<MetricThreshold, 1> kSourceThresholds{{
    {"max_channel_fraction", kSourceSaturationFlagFraction,
     "report: an acquisition limitation is flagged above this fraction of any channel's sites"},
}};

constexpr std::array<MetricThreshold, 1> kClippingThresholds{{
    {"new_fraction", kMaxNewHighlightClipFraction,
     "fail: pixels newly clipped high relative to the baseline"},
}};

constexpr std::array<MetricThreshold, 1> kBlockingThresholds{{
    {"new_fraction", kMaxNewShadowBlockFraction,
     "fail: pixels newly blocked to code 0 relative to the baseline"},
}};

constexpr std::array<MetricThreshold, 1> kWhiteBalanceThresholds{{
    {"neutral_shift_de", kMaxNeutralShiftDeltaE,
     "fail: mean CIELAB a*b* shift of the baseline's near-neutral pixels"},
}};

constexpr std::array<MetricThreshold, 3> kNoiseThresholds{{
    {"predicted_sigma_lstar", kMaxShadowNoiseLStar, "fail when exceeded together with the amplification"},
    {"amplification", kMaxShadowNoiseAmplification, "fail when exceeded together with the noise level"},
    {"predicted_sigma_lstar", kVisibleNoiseLStar, "preference: increases above this level are penalised"},
}};

constexpr std::array<MetricThreshold, 3> kNativeThresholds{{
    {"halo_lstar", kMaxHaloLStar,
     "fail: mean lightness change next to strong edges beyond the change away from them"},
    {"shadow_texture_lstar", kMaxNativeNoiseLStar, "fail when exceeded together with the amplification"},
    {"shadow_texture_ratio", kMaxNativeNoiseAmplification, "fail when exceeded together with the level"},
}};

constexpr std::array<MetricThreshold, 2> kColorThresholds{{
    {"new_out_of_gamut_fraction", kMaxNewGamutFraction, "fail: pixels newly outside the output gamut"},
    {"hue_shift_degrees", kMaxHueShiftDegrees, "fail: chroma-weighted mean hue change of chromatic pixels"},
}};

constexpr std::array<MetricSpec, 9> kSpecs{{
    {MetricId::NumericalValidity, "numerical_validity", true, false,
     "rendered display-linear Rec.2020 output of the candidate and its DevelopSettings",
     "every output sample", "count of non-finite samples; curve monotonicity", "absolute (no baseline)",
     kNumericalThresholds, 1,
     "an empty render is a failure; a non-finite baseline fails the whole analysis (ErrorCode::Internal)"},
    {MetricId::SourceSaturation, "source_saturation", false, false,
     "decoded RAW samples in stored units, visible area, before black subtraction, white balance and any "
     "resampling",
     "every visible sensor site, per channel index (both Bayer greens separately)",
     "fraction of sites at or above the white level (SensorLevels::white); coverage of each analysis pixel",
     "absolute (acquisition)", kSourceThresholds, 1,
     "never applies to editing; with no sites the image cannot be analysed"},
    {MetricId::RenderClipping, "render_clipping", true, true,
     "output encoding srgb-8bit of the candidate render",
     "every pixel; pixels covered by saturated sensor sites are reported (source_fraction) but not excluded",
     "fraction of pixels with any channel at code 255", "the baseline render at the same resolution",
     kClippingThresholds, kMinPixels,
     "insufficient pixels: only the baseline remains feasible and the result is flagged"},
    {MetricId::ShadowBlocking, "shadow_blocking", true, true,
     "output encoding srgb-8bit of the candidate render", "every pixel",
     "fraction of pixels with every channel at code 0", "the baseline render at the same resolution",
     kBlockingThresholds, kMinPixels,
     "insufficient pixels: only the baseline remains feasible and the result is flagged"},
    {MetricId::TonalDistribution,
     "tonal_distribution",
     false,
     true,
     "display lightness L*/100 of the luminance of the display-linear Rec.2020 render (Y = BT.2020 "
     "coefficients); scene headroom from stage one's balanced camera RGB against the stage-two clip level",
     "every pixel for the quantiles; evidence = pixels whose scene signal exceeds 3 sigma of the noise model",
     "L*/100 quantiles (p01 .. p99.5), fractions, EV",
     "the baseline's quantiles and a key-aware band",
     {},
     kMinSignalPixels,
     "insufficient signal: the brightness term becomes a penalty on exposure and shadow edits, confidence "
     "drops and the result is flagged"},
    {MetricId::WhiteBalance, "white_balance", true, true,
     "residual camera multipliers relative to as shot (the route's neutral representation); CIELAB a*b* "
     "(D65) of the baseline's near-neutral pixels in the render",
     "the surfaces the white-balance evidence treats as neutral (camera log ratios within 0.08 of the "
     "estimated cast), else near-neutral baseline pixels (C*ab < 5); 0.15 < L*/100 < 0.9, unclipped",
     "residual log gains; Delta E*ab; estimator confidence [0, 1]", "as shot (white balance of the baseline)",
     kWhiteBalanceThresholds, kMinNeutralPixels,
     "no neutral evidence: the proposal is as shot (no residual), any residual is penalised and the "
     "result is flagged"},
    {MetricId::ShadowNoise, "shadow_noise", true, true,
     "predicted noise in display lightness: the sensor noise model (DNG NoiseProfile, masked-area read "
     "noise, "
     "ISO assumption) propagated through the candidate's local display gain",
     "baseline shadow pixels (0.02 < L*/100 < 0.35) without saturated sites and with positive scene signal",
     "L* units (p75 over the mask) and the ratio to the baseline", "the baseline render", kNoiseThresholds,
     kMinShadowPixels,
     "no noise model: shadow lifts and exposure increases are penalised and the result flagged; too few "
     "shadow pixels: not applicable (nothing to amplify)"},
    {MetricId::NativeDetail, "native_detail", true, true,
     "display lightness of stage two rendered on deterministic native-resolution crops (with the local "
     "tone halo), evaluated on each crop's central region",
     "detail crop (texture), shadow crop (texture as noise indicator), every crop's strong step edges with a "
     "baseline plateau on at least one side (halos)",
     "halo and texture in L* units, texture ratios", "the baseline's crops", kNativeThresholds,
     kMinEdgeSamples,
     "no crops (small image, option off): not applicable and the confidence drops; too few edge samples: "
     "the halo check is insufficient and local tone edits are penalised"},
    {MetricId::ColorGamut, "color_gamut", true, true,
     "CIELAB (D65 display white) of the display-linear render; output encoding srgb-8bit for the gamut",
     "chromatic baseline pixels (C*ab >= 8, 0.25 < L*/100 < 0.95, unclipped) for hue and chroma; every pixel "
     "for the gamut",
     "log ratio of linear-light chroma |d - Y| / Y (invariant to tone operations); C*-weighted CIELAB hue "
     "change in degrees; fractions",
     "the baseline render", kColorThresholds, kMinChromaticPixels,
     "too few chromatic pixels: saturation edits are penalised; monochrome data: not applicable"},
}};

}  // namespace

std::string_view to_string(MetricId id) noexcept {
    switch (id) {
        case MetricId::NumericalValidity:
            return "numerical_validity";
        case MetricId::SourceSaturation:
            return "source_saturation";
        case MetricId::RenderClipping:
            return "render_clipping";
        case MetricId::ShadowBlocking:
            return "shadow_blocking";
        case MetricId::TonalDistribution:
            return "tonal_distribution";
        case MetricId::WhiteBalance:
            return "white_balance";
        case MetricId::ShadowNoise:
            return "shadow_noise";
        case MetricId::NativeDetail:
            return "native_detail";
        case MetricId::ColorGamut:
            return "color_gamut";
    }
    return "invalid";
}

std::string_view to_string(MetricStatus status) noexcept {
    switch (status) {
        case MetricStatus::Pass:
            return "pass";
        case MetricStatus::Fail:
            return "fail";
        case MetricStatus::InsufficientEvidence:
            return "insufficient_evidence";
        case MetricStatus::NotApplicable:
            return "not_applicable";
    }
    return "invalid";
}

std::span<const MetricSpec> metric_specs() noexcept {
    return kSpecs;
}

const MetricSpec& metric_spec(MetricId id) noexcept {
    for (const MetricSpec& spec : kSpecs) {
        if (spec.id == id) {
            return spec;
        }
    }
    return kSpecs[0];  // unreachable for valid enumerators
}

std::optional<double> MetricResult::value(std::string_view name) const {
    for (const NamedValue& v : values) {
        if (v.name == name) {
            return v.value;
        }
    }
    return std::nullopt;
}

}  // namespace lucent::analysis
