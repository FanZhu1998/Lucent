#pragma once

// Natural v1 automatic settings for one photograph (blueprint section 6, "Automatic settings for
// every photograph"; ADR 0011). W8 phase 1: the deterministic per-image controller; phase 2: the
// session that keeps an image's analysis for related-shot consistency (AutoSession::regularise,
// consistency.hpp, related_shots.hpp). Learned proposals and the application's "Auto" and batch
// flows come later.
//
// auto_develop() runs, for one decoded image:
//   1. Source diagnostics on the decoded samples (source_diagnostics.hpp): per-channel saturation,
//      the saturation coverage of the analysis and proxy grids, the sensor noise model.
//   2. One stage one (engine::render_stage_one, Interactive, as shot, full resolution), then the
//      analysis image (long edge AutoOptions::analysis_long_edge, 1536) and the search proxy
//      (proxy_long_edge, 512) with engine::make_proxy, and the deterministic native crops
//      (native_crops.hpp) cut from the full-resolution image twice, with the local-tone halo and
//      halo-free (NativeCrop::measured), which is then released. One decode.
//   3. The white-balance proposal on the proxy (proposals.hpp) and, with evidence of at least
//      kNeutralEvidenceMinConfidence, the masks of the surfaces it treats as neutral (the
//      white-balance hard check measures them in every render).
//   4. The baseline (as shot for every unlocked searched control; locked controls at their current
//      values; tone curve, vibrance and base look carried over from the current settings) rendered
//      on the proxy, the analysis image and the crops: the reference of every relative metric; then
//      the shot features that grouping compares (consistency.hpp, scene luminance included), and the
//      key-aware exposure/headroom proposal from the baseline's tones, the scene headroom and the
//      scene luminance. With exposure locked the brightness term is 0 (objective.hpp).
//   5. Candidates on the proxy: the baseline and the identity (the current settings unchanged) are
//      always evaluated; then the exposure, white-balance, global (exposure and white balance) and
//      combined (with the local-tone proposals) proposals; then a bounded deterministic coordinate
//      search from the best evaluated point over exposure, shadows, highlights, white-balance
//      strength (along the proposal), contrast and saturation, in that order, each step +- its
//      size, sizes halved after every pass, for at most four passes or until
//      AutoOptions::candidate_budget proxy renders (32) are used. Locked controls never move.
//      Already evaluated settings are not rendered again. Hard checks gate feasibility; the
//      Natural v1 objective ranks (objective.hpp).
//   6. Recheck: the leading recheck_count (3) feasible proxy candidates, the best feasible proxy
//      candidate WITHOUT local tone (the leaders are often near-duplicates sharing one local-tone
//      family, so the native checks could otherwise only pass or fail that family) and the
//      baseline are rendered at analysis resolution and on the native crops and evaluated again
//      (native metrics included); select_best() picks among the feasible ones (ties to the smaller
//      edit). When every rechecked candidate but the baseline fails a hard check, a bounded
//      conservative fallback follows (blueprint section 6 step 7, at analysis resolution): the
//      proxy leader's conservative_fallbacks() - local tone halved, local tone removed, or for a
//      leader without local tone every change halved - at most two more analysis renders
//      (CandidateOrigin::Fallback), then the baseline; the result is flagged ConservativeFallback.
//      A candidate without local tone (highlights and shadows 0) is rendered on the halo-free
//      crops: stage two is then per pixel and every crop measurement reads only
//      NativeCrop::measured, so the result is the same as with the halo (native_crops.hpp).
//   7. Confidence: the smallest of the evidence confidences that apply to what the winner changes
//      (source interpretation, the signal evidence for brightness, white-balance evidence, noise
//      model, native crops) and of the policy confidence. Each group of controls is restrained by
//      the evidence that governs it: when the smallest of the source, brightness, noise and native
//      confidences is below kRestrainConfidence, exposure, shadows, highlights, contrast and
//      saturation move towards the baseline by the factor max(0.25, confidence /
//      kRestrainConfidence); when the smaller of the source and white-balance confidences is, the
//      white-balance residual does. Restraint always starts from an unrestrained point (a
//      restrained winner is restrained again from its source, never compounded). The restrained
//      candidate is re-verified and flagged; if it fails a hard check the baseline is chosen. The
//      policy confidence does not restrain (the objective already decided the edit's size); it
//      reports how much the decision rests on the policy's interpretation: kLargeEditConfidence
//      (or kAmbiguousLargeEditConfidence when the tone proposal's key was ambiguous) for a large
//      edit (|exposure| >= kLargeExposureEv or |shadows| + |highlights| >= kLargeLocalToneEv,
//      flagged LargeEdit), kFallbackConfidence after a conservative fallback, and
//      kUnmetFallbackConfidence when the fallback ended at the baseline with a brightness term
//      above 1 (the objective's own target is unmet).
//   8. Review flags with reasons, per-metric diagnostics of the chosen settings at analysis
//      resolution, every evaluated candidate, timings and memory figures.
//
// AutoSession::regularise(target) continues from an image's independent result towards a related
// group's consistency target (consistency.hpp), on the analysis images, proxy and crops the session
// kept (nothing is decoded again). When the independent choice's consistency term is already 0
// nothing can rank above it for consistency, and the independent result stands (with its record);
// otherwise:
//   a. Every evaluated candidate is ranked again with the consistency term added to its objective
//      (proxy and analysis scores, from the appearance stored with each; no render). A target
//      carrying a camera white balance of another calibration domain than the member's is
//      InvalidArgument (camera gains are never compared across domains).
//   b. White-balance axis of the stage: from the gains of the best re-ranked candidate that the
//      independent recheck did not find infeasible at analysis resolution (strength 0)
//      to the consistency gains (strength 1): within the member's own calibration domain, the gains
//      that give the domain target's camera multipliers; across domains (colour target only), the
//      gains that move the rendered mid-tone u'v' to the target, solved from two probe renders
//      (log-gain steps of 0.02 in red and blue) on the proxy. Gains stay within the +-10 % bound.
//   c. Seeds (ConsistencyProposal): from the best re-ranked point, the exposure that moves its
//      median lightness to the target (display_lightness_inverse) with and without the
//      white-balance move, and the white-balance move alone; then coordinate steps
//      (ConsistencyStep) from the best point at half the initial step sizes, at most two passes,
//      within AutoOptions::consistency_budget (16) new proxy renders.
//   d. Recheck (revalidation) as in 6 - the leading recheck_count feasible proxy candidates, the best
//      one without local tone, the baseline, the conservative fallback when the leaders fail - plus
//      the independent winner (unrestrained), at analysis resolution and on the crops, ranked with
//      the term.
//   e. Restraint as in 7 (the independent winner enters d. unrestrained, so restraint is applied
//      once, to the stage's winner), except that a white-balance residual is governed by the
//      larger of the image's white-balance evidence and the target's white_balance_confidence;
//      then flags. The
//      result records the independent choice, both consistency terms and the adjustment
//      (ConsistencyRecord). Locks and hard checks apply exactly as in the independent search: the
//      term only ranks feasible candidates.
//
// Global controls only (DevelopSettings v2, ADR 0015). The search sets global controls; the current
// settings' geometry (crop, straighten, quarter turns, flip) and local adjustments (masks) are the
// photographer's and are set aside for the whole evaluation: the baseline, every candidate, the
// recheck, the appearances (AutoSession::appearance_of) and the consistency stage render
// evaluation_settings() of them (the identity geometry, no local adjustment). Native crops are
// stage-one images of a sub-rectangle, so a crop or a rotation would not line up with them and a
// mask (positioned relative to the image it is rendered on) would land on the wrong content; and a
// crop would change the measured statistics, so the decision is the same whatever the crop is.
// Chroma denoising is global and kept (it is never searched). Every settings value of a result
// (AutoResult::settings, every CandidateRecord::settings, ConsistencyRecord::independent_settings)
// carries the current geometry and local adjustments unchanged (and the chroma denoising, which the
// search never changes), so the chosen settings render with them; AutoResult::set_aside_geometry /
// set_aside_masks say whether there was anything to set aside. A full-resolution check of the
// chosen settings with their geometry and masks is the application's (auto_develop.hpp).
//
// Search coordinates and their mapping to DevelopSettings (blueprint section 6 bounds):
//   exposure    -1.5 .. +1.5 EV            exposure_ev (relative to the baseline's, 0 when unlocked)
//   white bal.  red, blue gains 0.9 .. 1.1  CameraNeutralWhiteBalance{as_shot r x gr, g, as_shot b x gb}
//               relative to the as-shot camera multipliers (the route's neutral representation);
//               searched as a strength 0 .. 1 along an axis: the evidence-based proposal from as
//               shot (independent search), or b. above; AsShot when 1, 1
//   shadows     0 .. +0.75 EV-equivalent   shadows = EV / engine::kShadowsMaxEv  (0 .. 0.375)
//   highlights  -0.75 .. 0 EV-equivalent    highlights = EV / engine::kHighlightsMaxEv (-0.5 .. 0)
//   contrast    -0.15 .. +0.15              contrast (narrow around the identity)
//   saturation  0.85 .. 1.15 multiplier     saturation = multiplier - 1 (engine chroma factor 1 + s)
// Monochrome data has no white balance or saturation coordinate.
//
// Determinism: the same decoded input, current settings and options (and target) give the same
// result with the same binary on the same CPU feature set (every render and reduction is schedule
// independent, serial or parallel; candidates are generated and ranked in a fixed order; no
// randomness); regularise() always starts from the independent result, so a session reopened from
// the file regularises exactly like one kept in memory. Across machines the MSVC x64 runtime picks
// FMA3 or SSE2 code for cbrt, log and exp at run time (_set_FMA3_enable), so a measurement can
// differ in the last bit and a near-tie can then resolve differently; a process that replays
// results across machines must disable FMA3 in the CRT before analysing. Timings differ between
// runs and are not part of the decision.
//
// Errors: InvalidArgument (analysis.auto) for invalid options or current settings
// (DevelopSettings::validate()); the source analysis', stage one's and proxies' errors unchanged
// (decoder, memory budget, Unsupported data); Internal when the baseline render is numerically
// invalid ("A mandatory interpretation failure or invalid numerical result still fails
// explicitly"); Cancelled. A candidate that fails to render is recorded as infeasible.
//
// Thread-safety: auto_develop is reentrant; an AutoSession is not thread-safe (one caller at a time).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/analysis/consistency.hpp>
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

// Policy "natural" version 2 (ADR 0011, "Amendment: W8 core closure"): the key-aware tone policy,
// the exposure-invariant contrast terms, the local-contrast term, relative near-black, the policy
// confidence. Search version 2: the diverse recheck pool, the conservative fallback, the two-step
// tie rule (select_best), single restraint.
inline constexpr std::string_view kNaturalPolicyId = "natural";
inline constexpr std::uint32_t kNaturalPolicyVersion = 2;  // thresholds, scales, proposals' policy
inline constexpr std::uint32_t kSearchVersion = 2;         // candidate generation, steps, ranking

struct AutoVersions {
    std::string policy{kNaturalPolicyId};
    std::uint32_t policy_version = kNaturalPolicyVersion;
    std::uint32_t search_version = kSearchVersion;
    std::uint32_t metric_contract_version = kMetricContractVersion;
    std::uint32_t develop_pipeline_version = engine::kDevelopPipelineVersion;
    std::uint32_t develop_settings_version = engine::kDevelopSettingsVersion;
    std::uint32_t consistency_version = kConsistencyVersion;  // grouping, targets, consistency term

    friend bool operator==(const AutoVersions&, const AutoVersions&) = default;
};

// ---- Options -----------------------------------------------------------------------------------

inline constexpr std::int32_t kDefaultAnalysisLongEdge = 1536;
inline constexpr std::int32_t kDefaultProxyLongEdge = 512;
inline constexpr std::uint32_t kDefaultCandidateBudget = 32;
inline constexpr std::uint32_t kDefaultRecheckCount = 3;
inline constexpr std::uint32_t kDefaultConsistencyBudget = 16;
inline constexpr double kRestrainConfidence = 0.5;
// The policy confidence (step 7 above; uncalibrated).
inline constexpr double kLargeExposureEv = 1.0;
inline constexpr double kLargeLocalToneEv = 0.4;
inline constexpr double kLargeEditConfidence = 0.75;
inline constexpr double kAmbiguousLargeEditConfidence = 0.6;
inline constexpr double kFallbackConfidence = 0.6;
inline constexpr double kUnmetFallbackConfidence = 0.5;
inline constexpr std::uint32_t kMaxFallbackRechecks = 2;

struct AutoOptions {
    std::int32_t analysis_long_edge = kDefaultAnalysisLongEdge;  // [256, 8192]
    std::int32_t proxy_long_edge = kDefaultProxyLongEdge;        // [128, analysis_long_edge]
    std::uint32_t candidate_budget = kDefaultCandidateBudget;    // proxy renders, seeds included: [8, 256]
    std::uint32_t recheck_count = kDefaultRecheckCount;          // [1, 8]
    std::uint32_t consistency_budget = kDefaultConsistencyBudget;  // new proxy renders of regularise(): [4, 64]
    bool native_crops = true;
    std::int32_t native_crop_size = kNativeCropSize;  // [32, 1024]
    engine::DevelopOptions develop;                   // limits, memory budget, parallelism
};

// ---- Candidates --------------------------------------------------------------------------------

// A point of the search space (see the file comment). The gains follow from the strength along
// the stage's white-balance axis (or are set directly for a restrained candidate).
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
    ConsistencyProposal = 9,   // a seed (or white-balance probe) of the consistency stage
    ConsistencyStep = 10,      // a coordinate step of the consistency stage
    Fallback = 11,             // a conservative fallback of the proxy leader (recheck only)
};

[[nodiscard]] std::string_view to_string(CandidateOrigin origin) noexcept;

// One evaluation of a candidate at one resolution.
struct CandidateScore {
    bool evaluated = false;
    bool feasible = false;
    ObjectiveTerms terms;  // terms.consistency: 0 unless ranked against a consistency target
    std::vector<std::string> failures;
    double median_lightness = 0.0;
    double new_clip_fraction = 0.0;
    Appearance appearance;  // what consistency compares (consistency.hpp)
};

struct CandidateRecord {
    std::uint32_t index = 0;
    CandidateOrigin origin = CandidateOrigin::Baseline;
    std::string note;  // the coordinate and step of a search candidate
    engine::DevelopSettings settings;
    EditVector edit;
    double edit_size = 0.0;
    std::optional<SearchPoint> point;  // not for an identity outside the search space
    // A restrained or fallback candidate: the candidate it was derived from.
    std::optional<std::uint32_t> source;
    CandidateScore proxy;     // on the search proxy (not for restrained and fallback candidates)
    CandidateScore analysis;  // at analysis resolution with native crops (recheck)
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
    ConsistencyLimited = 12,      // the group's look was not reached within the checks and locks
    LargeEdit = 13,               // a large automatic edit: review (step 7)
    ConservativeFallback = 14,    // the leaders failed the recheck: a conservative fallback was chosen
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
    // The signal evidence for a brightness edit (ToneProposal::signal_evidence): whether the frame
    // carries a measurable signal, not how certain the edit's size is. 1 when exposure and shadows
    // are unchanged.
    double brightness = 1.0;
    double white_balance = 1.0;  // 1 when white balance is unchanged
    double noise = 1.0;          // 1 unless shadows are lifted or exposure raised
    double native = 1.0;         // 1 when native crops were checked (or nothing local changed)
    // How much the decision rests on the policy's interpretation (step 7); not used for restraint.
    double policy = 1.0;
    std::vector<std::string> reasons;
};

// ---- Consistency record ------------------------------------------------------------------------

// What regularise() did to one member of a related group (all appearances and totals at analysis
// resolution, the totals with the consistency term).
struct ConsistencyRecord {
    ConsistencyTarget target;
    std::size_t independent_chosen = 0;  // index into AutoResult::candidates
    engine::DevelopSettings independent_settings;
    Appearance independent_appearance;
    ConsistencyTerms independent_terms;  // the term of the independent choice
    double independent_total = 0.0;      // its objective total with the term
    Appearance appearance;               // of the chosen settings
    ConsistencyTerms terms;              // of the chosen settings
    double total = 0.0;                  // their objective total with the term
    // The chosen settings relative to the independent choice (gains and saturation as ratios).
    EditVector adjustment;
    bool changed = false;  // the chosen settings differ from the independent choice
    std::uint32_t added_candidates = 0;  // proxy renders of the stage
    // The stage's white-balance axis: "camera" (domain target), "colour" (probes), or "" (none).
    std::string white_balance_axis;
    double axis_red_gain = 1.0;  // the axis' strength-1 gains relative to as shot
    double axis_blue_gain = 1.0;
};

// ---- Result ------------------------------------------------------------------------------------

struct AutoTiming {
    double decode_seconds = 0.0;  // auto_develop_file / AutoSession::open_file only
    double source_seconds = 0.0;
    double stage_one_seconds = 0.0;
    double proxies_seconds = 0.0;
    double crops_seconds = 0.0;
    double baseline_seconds = 0.0;
    double proposals_seconds = 0.0;
    double search_seconds = 0.0;
    double recheck_seconds = 0.0;
    double consistency_seconds = 0.0;  // regularise(): search and recheck of the stage
    double total_seconds = 0.0;        // from the call (decode included for *_file), plus regularise()
    std::uint32_t proxy_candidates = 0;
    std::uint32_t analysis_candidates = 0;
    std::uint32_t crop_renders = 0;
    std::uint32_t halo_free_crop_renders = 0;  // of crop_renders, on NativeCrop::measured
    std::uint32_t consistency_candidates = 0;  // proxy renders of regularise()
};

// Pixel memory (bytes) of the images the controller holds.
struct AutoMemory {
    std::uint64_t stage_one_peak_estimate = 0;  // engine estimate for the full-resolution stage one
    std::uint64_t full_resolution_bytes = 0;    // released after the crops are cut
    std::uint64_t analysis_bytes = 0;
    std::uint64_t proxy_bytes = 0;
    std::uint64_t crop_bytes = 0;      // halo and halo-free crops
    std::uint64_t retained_bytes = 0;  // held during the search (analysis, proxy, crops, contexts)
    std::uint64_t estimated_peak_bytes = 0;
};

struct AutoResult {
    // The chosen settings (current locks kept; the current geometry, local adjustments and chroma
    // denoising carried unchanged, see the file comment).
    engine::DevelopSettings settings;
    AutoVersions versions;
    double confidence = 0.0;
    ConfidenceBreakdown confidence_detail;
    bool restrained = false;
    bool fallback = false;  // the leaders failed the recheck (step 6)
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
    ShotFeatures shot;                  // what grouping compares (consistency.hpp)
    Appearance appearance;              // of the chosen settings at analysis resolution
    std::optional<ConsistencyRecord> consistency;  // regularise() only
    // The current settings had a geometry other than the identity / local adjustments, which the
    // evaluation set aside and the result carries unchanged (see the file comment).
    bool set_aside_geometry = false;
    bool set_aside_masks = false;
    AutoTiming timing;
    AutoMemory memory;

    [[nodiscard]] bool has_flag(ReviewFlagCode code) const noexcept;
    [[nodiscard]] const MetricResult* diagnostic(MetricId id) const noexcept;
};

// ---- Entry points ------------------------------------------------------------------------------

// `settings` as the automatic settings evaluate them (see the file comment): the identity geometry
// and no local adjustment; everything else (chroma denoising included) unchanged.
[[nodiscard]] engine::DevelopSettings evaluation_settings(engine::DevelopSettings settings);

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

// The bounded conservative fallbacks of blueprint section 6 step 7 for `chosen` (an automatic
// result's settings) relative to `baseline` (its as-shot baseline, AutoResult::candidates[0]), in
// order: local tone (shadows, highlights) halved towards the baseline; local tone at the
// baseline's; then, when `chosen` has no local tone, exposure, contrast and saturation halved
// towards the baseline. The white balance, the tone curve, vibrance, the base look and the locks
// stay `chosen`'s (a locked control equals the baseline's, so it never moves). Settings equal to
// `chosen`, to `baseline` or to an earlier entry are left out; the baseline itself is the last
// resort and not listed. The recheck uses it at analysis resolution (step 6); W7's final
// full-resolution check (render once, measure_render() and evaluate_candidate() against the
// full-resolution baseline) is meant to walk the same ladder and record each attempt.
[[nodiscard]] std::vector<engine::DevelopSettings> conservative_fallbacks(const engine::DevelopSettings& chosen,
                                                                         const engine::DevelopSettings& baseline);

// Estimated peak pixel memory of auto_develop() for `metadata` (probe or decode metadata): the
// full-resolution stage one's peak, or the full image with its proxies and crops if larger.
// Errors: those of engine::estimate_develop_memory().
[[nodiscard]] Expected<std::uint64_t> estimate_auto_memory(const raw::RawMetadata& metadata,
                                                           const AutoOptions& options = {});

// An image's analysis kept for related-shot consistency: open() is auto_develop() that keeps the
// analysis image, proxy, crops, contexts and every evaluated candidate (independent() is exactly
// auto_develop()'s result); regularise() then needs no decode. Move-only.
class AutoSession {
public:
    [[nodiscard]] static Expected<AutoSession> open(raw::DecodedRaw&& decoded,
                                                    const engine::DevelopSettings& current,
                                                    const AutoOptions& options = {},
                                                    const core::CancellationToken& cancel = {});
    [[nodiscard]] static Expected<AutoSession> open_file(const raw::DecoderRegistry& registry,
                                                         const std::filesystem::path& path,
                                                         std::optional<std::uint32_t> subimage,
                                                         const engine::DevelopSettings& current,
                                                         const AutoOptions& options = {},
                                                         const core::CancellationToken& cancel = {});

    AutoSession(AutoSession&&) noexcept;
    AutoSession& operator=(AutoSession&&) noexcept;
    AutoSession(const AutoSession&) = delete;
    AutoSession& operator=(const AutoSession&) = delete;
    ~AutoSession();

    // The independent Natural v1 result.
    [[nodiscard]] const AutoResult& independent() const noexcept;

    // The result regularised towards `target` (see the file comment). Each call starts from the
    // independent result. Errors: InvalidArgument for a target with a non-finite value; render
    // and measurement errors; Cancelled; OutOfMemory.
    [[nodiscard]] Expected<AutoResult> regularise(const ConsistencyTarget& target,
                                                  const core::CancellationToken& cancel = {});

    // The appearance of `settings` (valid DevelopSettings with the current locks' values) at
    // analysis resolution, evaluated as the candidates are (evaluation_settings(): without geometry
    // and local adjustments): what an anchor contributes to its group's target.
    [[nodiscard]] Expected<Appearance> appearance_of(const engine::DevelopSettings& settings,
                                                     const core::CancellationToken& cancel = {});

    // A reduced preview of `settings`: stage two on the analysis image (the interactive path the
    // controller judged, not the export render), box-downscaled in display-linear light to
    // `long_edge` (at most the analysis size). Display-linear Rec.2020. The geometry and masks of
    // `settings` are rendered (the analysis image is the whole upright image, so they land as in
    // the export): the long edge applies to the cropped and turned output.
    [[nodiscard]] Expected<engine::ColorImage> preview(const engine::DevelopSettings& settings,
                                                       std::int32_t long_edge,
                                                       const core::CancellationToken& cancel = {});

    // Pixel and per-pixel memory the session holds.
    [[nodiscard]] std::uint64_t retained_bytes() const noexcept;

    class Impl;

private:
    explicit AutoSession(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lucent::analysis
