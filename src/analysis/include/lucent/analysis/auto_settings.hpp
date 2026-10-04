#pragma once

// Natural v1 automatic settings for one photograph (blueprint section 6, "Automatic settings for
// every photograph"; ADR 0011). Phase 1 of W8: the deterministic per-image controller. Related-shot
// consistency, learned proposals and the application's "Auto" and batch flows come later.
//
// auto_develop() runs, for one decoded image:
//   1. Source diagnostics on the decoded samples (source_diagnostics.hpp): per-channel saturation,
//      the saturation coverage of the analysis and proxy grids, the sensor noise model.
//   2. One stage one (engine::render_stage_one, Interactive, as shot, full resolution), then the
//      analysis image (long edge AutoOptions::analysis_long_edge, 1536) and the search proxy
//      (proxy_long_edge, 512) with engine::make_proxy, and the deterministic native crops
//      (native_crops.hpp) cut from the full-resolution image, which is then released. One decode.
//   3. The white-balance proposal on the proxy (proposals.hpp) and, with evidence, the masks of the
//      surfaces it treats as neutral (the white-balance hard check measures them in every render).
//   4. The baseline (as shot for every unlocked searched control; locked controls at their current
//      values; tone curve, vibrance and base look carried over from the current settings) rendered
//      on the proxy, the analysis image and the crops: the reference of every relative metric; then
//      the exposure/headroom proposal from its tones and the scene headroom.
//   5. Candidates on the proxy: the baseline and the identity (the current settings unchanged) are
//      always evaluated; then the exposure, white-balance, global (exposure and white balance) and
//      combined (with the local-tone proposals) proposals; then a bounded deterministic coordinate
//      search from the best evaluated point over exposure, shadows, highlights, white-balance
//      strength (along the proposal), contrast and saturation, in that order, each step +- its
//      size, sizes halved after every pass, for at most four passes or until
//      AutoOptions::candidate_budget proxy renders (32) are used. Locked controls never move.
//      Already evaluated settings are not rendered again. Hard checks gate feasibility; the
//      Natural v1 objective ranks (objective.hpp).
//   6. The leading recheck_count (3) feasible proxy candidates, and the baseline, are rendered at
//      analysis resolution and on the native crops and evaluated again (native metrics included);
//      the best feasible one wins (ties to the smaller edit).
//   7. Confidence: the smallest of the evidence confidences that apply to what the winner changes
//      (source interpretation, brightness evidence, white-balance evidence, noise model, native
//      crops). Each group of controls is restrained by the evidence that governs it: when the
//      smallest of the source, brightness, noise and native confidences is below
//      kRestrainConfidence, exposure, shadows, highlights, contrast and saturation move towards the
//      baseline by the factor max(0.25, confidence / kRestrainConfidence); when the smaller of the
//      source and white-balance confidences is, the white-balance strength does. The restrained
//      candidate is re-verified and flagged; if it fails a hard check the baseline is chosen.
//   8. Review flags with reasons, per-metric diagnostics of the chosen settings at analysis
//      resolution, every evaluated candidate, timings and memory figures.
//
// Search coordinates and their mapping to DevelopSettings v1 (blueprint section 6 bounds):
//   exposure    -1.5 .. +1.5 EV            exposure_ev (relative to the baseline's, 0 when unlocked)
//   white bal.  red, blue gains 0.9 .. 1.1  CameraNeutralWhiteBalance{as_shot r x gr, g, as_shot b x gb}
//               relative to the as-shot camera multipliers (the route's neutral representation);
//               searched as a strength 0 .. 1 along the evidence-based proposal; AsShot when 1, 1
//   shadows     0 .. +0.75 EV-equivalent   shadows = EV / engine::kShadowsMaxEv  (0 .. 0.375)
//   highlights  -0.75 .. 0 EV-equivalent    highlights = EV / engine::kHighlightsMaxEv (-0.5 .. 0)
//   contrast    -0.15 .. +0.15              contrast (narrow around the identity)
//   saturation  0.85 .. 1.15 multiplier     saturation = multiplier - 1 (engine chroma factor 1 + s)
// Monochrome data has no white balance or saturation coordinate.
//
// Determinism: the same decoded input, current settings and options give the same result (every
// render and reduction is schedule independent; candidates are generated and ranked in a fixed
// order; no randomness). Timings differ between runs and are not part of the decision.
//
// Errors: InvalidArgument (analysis.auto) for invalid options or current settings
// (DevelopSettings::validate()); the source analysis', stage one's and proxies' errors unchanged
// (decoder, memory budget, Unsupported data); Internal when the baseline render is numerically
// invalid ("A mandatory interpretation failure or invalid numerical result still fails
// explicitly"); Cancelled. A candidate that fails to render is recorded as infeasible.
//
// Thread-safety: reentrant.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/analysis/metric_contract.hpp>
#include <lucent/analysis/native_crops.hpp>
#include <lucent/analysis/objective.hpp>
#include <lucent/analysis/proposals.hpp>
#include <lucent/analysis/render_metrics.hpp>
#include <lucent/analysis/source_diagnostics.hpp>
#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/develop.hpp>
#include <lucent/engine/develop_settings.hpp>
#include <lucent/raw/decoded_raw.hpp>
#include <lucent/raw/decoder_registry.hpp>

namespace lucent::analysis {

// ---- Versions ----------------------------------------------------------------------------------

inline constexpr std::string_view kNaturalPolicyId = "natural";
inline constexpr std::uint32_t kNaturalPolicyVersion = 1;  // thresholds, scales, proposals' policy
inline constexpr std::uint32_t kSearchVersion = 1;         // candidate generation, steps, ranking

struct AutoVersions {
    std::string policy{kNaturalPolicyId};
    std::uint32_t policy_version = kNaturalPolicyVersion;
    std::uint32_t search_version = kSearchVersion;
    std::uint32_t metric_contract_version = kMetricContractVersion;
    std::uint32_t develop_pipeline_version = engine::kDevelopPipelineVersion;
    std::uint32_t develop_settings_version = engine::kDevelopSettingsVersion;

    friend bool operator==(const AutoVersions&, const AutoVersions&) = default;
};

// ---- Options -----------------------------------------------------------------------------------

inline constexpr std::int32_t kDefaultAnalysisLongEdge = 1536;
inline constexpr std::int32_t kDefaultProxyLongEdge = 512;
inline constexpr std::uint32_t kDefaultCandidateBudget = 32;
inline constexpr std::uint32_t kDefaultRecheckCount = 3;
inline constexpr double kRestrainConfidence = 0.5;

struct AutoOptions {
    std::int32_t analysis_long_edge = kDefaultAnalysisLongEdge;  // [256, 8192]
    std::int32_t proxy_long_edge = kDefaultProxyLongEdge;        // [128, analysis_long_edge]
    std::uint32_t candidate_budget = kDefaultCandidateBudget;    // proxy renders, seeds included: [8, 256]
    std::uint32_t recheck_count = kDefaultRecheckCount;          // [1, 8]
    bool native_crops = true;
    std::int32_t native_crop_size = kNativeCropSize;  // [32, 1024]
    engine::DevelopOptions develop;                   // limits, memory budget, parallelism
};

// ---- Candidates --------------------------------------------------------------------------------

// A point of the search space (see the file comment). The gains follow from the strength along
// the white-balance proposal.
struct SearchPoint {
    double exposure_ev = 0.0;
    double white_balance_strength = 0.0;
    double red_gain = 1.0;
    double blue_gain = 1.0;
    double shadows_ev = 0.0;
    double highlights_ev = 0.0;
    double contrast = 0.0;
    double saturation = 1.0;

    friend bool operator==(const SearchPoint&, const SearchPoint&) = default;
};

// Persisted (append only).
enum class CandidateOrigin : std::uint8_t {
    Baseline = 1,              // as shot for unlocked searched controls
    Identity = 2,              // the current settings unchanged
    ExposureProposal = 3,      // the exposure/headroom proposal
    WhiteBalanceProposal = 4,  // the white-balance proposal
    CombinedProposal = 5,      // exposure, local tone and white-balance proposals together
    CoordinateStep = 6,        // a step of the coordinate search
    Restrained = 7,            // the winner moved towards the baseline for low confidence
    GlobalProposal = 8,        // exposure and white-balance proposals, no local tone
};

[[nodiscard]] std::string_view to_string(CandidateOrigin origin) noexcept;

// One evaluation of a candidate at one resolution.
struct CandidateScore {
    bool evaluated = false;
    bool feasible = false;
    ObjectiveTerms terms;
    std::vector<std::string> failures;
    double median_lightness = 0.0;
    double new_clip_fraction = 0.0;
};

struct CandidateRecord {
    std::uint32_t index = 0;
    CandidateOrigin origin = CandidateOrigin::Baseline;
    std::string note;  // the coordinate and step of a search candidate
    engine::DevelopSettings settings;
    EditVector edit;
    double edit_size = 0.0;
    std::optional<SearchPoint> point;  // not for an identity outside the search space
    CandidateScore proxy;              // on the search proxy (not for a restrained candidate)
    CandidateScore analysis;           // at analysis resolution with native crops (recheck)
};

// ---- Flags and confidence ----------------------------------------------------------------------

// Persisted (append only).
enum class ReviewFlagCode : std::uint16_t {
    LowConfidence = 1,            // the edit was restrained
    InsufficientEvidence = 2,     // a metric lacked its valid samples (metric set)
    SourceSaturation = 3,         // sensor channels saturated: an acquisition limitation
    LockedControlsClip = 4,       // the locked settings clip highlights themselves
    WhiteBalanceUncertain = 5,    // weak or no neutral evidence: white balance kept near as shot
    NoiseModelUnavailable = 6,    // no sensor noise model: shadow lifts restrained
    VisibleShadowNoise = 7,       // the chosen settings leave visible predicted shadow noise
    NativeChecksUnavailable = 8,  // no native-resolution crops were checked
    NoFeasibleCandidate = 9,      // nothing but the baseline passed the hard checks
    InterpretationLimited = 10,   // the decode/stage one reported limitations
    RecheckRejected = 11,         // every rechecked candidate failed at analysis resolution
};

[[nodiscard]] std::string_view to_string(ReviewFlagCode code) noexcept;

struct ReviewFlag {
    ReviewFlagCode code = ReviewFlagCode::LowConfidence;
    std::optional<MetricId> metric;
    std::string reason;  // developer-facing English
};

struct ConfidenceBreakdown {
    double overall = 1.0;
    double source = 1.0;
    double brightness = 1.0;     // 1 when exposure and shadows are unchanged
    double white_balance = 1.0;  // 1 when white balance is unchanged
    double noise = 1.0;          // 1 unless shadows are lifted or exposure raised
    double native = 1.0;         // 1 when native crops were checked (or nothing local changed)
    std::vector<std::string> reasons;
};

// ---- Result ------------------------------------------------------------------------------------

struct AutoTiming {
    double decode_seconds = 0.0;  // auto_develop_file only
    double source_seconds = 0.0;
    double stage_one_seconds = 0.0;
    double proxies_seconds = 0.0;
    double crops_seconds = 0.0;
    double baseline_seconds = 0.0;
    double proposals_seconds = 0.0;
    double search_seconds = 0.0;
    double recheck_seconds = 0.0;
    double total_seconds = 0.0;  // from the call (decode included for auto_develop_file)
    std::uint32_t proxy_candidates = 0;
    std::uint32_t analysis_candidates = 0;
    std::uint32_t crop_renders = 0;
};

// Pixel memory (bytes) of the images the controller holds.
struct AutoMemory {
    std::uint64_t stage_one_peak_estimate = 0;  // engine estimate for the full-resolution stage one
    std::uint64_t full_resolution_bytes = 0;    // released after the crops are cut
    std::uint64_t analysis_bytes = 0;
    std::uint64_t proxy_bytes = 0;
    std::uint64_t crop_bytes = 0;
    std::uint64_t retained_bytes = 0;  // held during the search (analysis, proxy, crops, contexts)
    std::uint64_t estimated_peak_bytes = 0;
};

struct AutoResult {
    engine::DevelopSettings settings;  // the chosen settings (current locks kept)
    AutoVersions versions;
    double confidence = 0.0;
    ConfidenceBreakdown confidence_detail;
    bool restrained = false;
    std::vector<ReviewFlag> flags;
    // Source saturation, then every metric of the chosen settings at analysis resolution.
    std::vector<MetricResult> diagnostics;
    SourceDiagnostics source;
    ToneProposal tone;
    std::optional<WhiteBalanceProposal> white_balance;  // colour data with unlocked white balance
    std::vector<NativeCrop> crops;
    std::vector<CandidateRecord> candidates;  // in evaluation order
    std::size_t chosen = 0;                   // index into candidates
    image::Size full_size;
    image::Size analysis_size;
    image::Size proxy_size;
    std::vector<std::string> warnings;  // stage one's
    AutoTiming timing;
    AutoMemory memory;

    [[nodiscard]] bool has_flag(ReviewFlagCode code) const noexcept;
    [[nodiscard]] const MetricResult* diagnostic(MetricId id) const noexcept;
};

// ---- Entry points ------------------------------------------------------------------------------

// The automatic settings of `decoded` (consumed) for an item whose current settings are `current`
// (its locks, locked values and the controls the search does not set).
[[nodiscard]] Expected<AutoResult> auto_develop(raw::DecodedRaw&& decoded,
                                                const engine::DevelopSettings& current,
                                                const AutoOptions& options = {},
                                                const core::CancellationToken& cancel = {});

// auto_develop() of image `subimage` of a file: the registry's probe, the engine's memory estimate,
// one decode.
[[nodiscard]] Expected<AutoResult> auto_develop_file(const raw::DecoderRegistry& registry,
                                                     const std::filesystem::path& path,
                                                     std::optional<std::uint32_t> subimage,
                                                     const engine::DevelopSettings& current,
                                                     const AutoOptions& options = {},
                                                     const core::CancellationToken& cancel = {});

// Estimated peak pixel memory of auto_develop() for `metadata` (probe or decode metadata): the
// full-resolution stage one's peak, or the full image with its proxies and crops if larger.
// Errors: those of engine::estimate_develop_memory().
[[nodiscard]] Expected<std::uint64_t> estimate_auto_memory(const raw::RawMetadata& metadata,
                                                           const AutoOptions& options = {});

}  // namespace lucent::analysis
