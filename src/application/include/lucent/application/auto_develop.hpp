#pragma once

// Automatic settings for one library photograph, as the application runs them (blueprint section 2,
// "Detailed individual editing": "Auto creates a new editable revision ... The optimizer may change
// only unlocked controls and must report when the locks prevent its usual technical targets";
// section 6, "Analysis and candidate search" steps 5-7; ADR 0011 item 22; ADR 0013). Shared by
// AutoService (the interactive "Auto", in process) and lucent-worker (automatic batches, isolated),
// so both decide exactly alike.
//
// run_automatic(), for an item's file and its current settings (their locks and locked values):
//   1. Probe, memory check, one decode. A decoded interpretation that Lucent cannot render
//      faithfully (SupportStatus::Unsupported, a mandatory operation missing) fails with
//      Unsupported ("app.auto.support") unless allowed: never analysed or exported silently.
//   2. The Natural analysis (analysis::AutoSession::open): proposals, bounded search, recheck at
//      analysis resolution and on native crops, confidence, restraint and review flags.
//   3. With a related-shot consistency target, the session's regularisation towards it (soft,
//      revalidated; locks and hard checks unchanged). For a group anchor, the appearance of its
//      current settings (what it contributes to its group's target).
//   4. With final_check, the full-resolution final check (step 7): the decode of step 1 is reused
//      (a copy of the decoded samples is kept through the analysis, so the file is decoded once),
//      its source diagnostics measured at full resolution (saturation coverage, noise model), and one
//      interactive stage one (as shot) rendered; the as-shot baseline (AutoResult::candidates[0])
//      is rendered and measured with analysis::make_image_context() and measure_render(), then the
//      chosen settings, judged by analysis::evaluate_candidate() against that baseline with the
//      analysis' own policy context (tone proposal, white-balance evidence and its neutral surfaces,
//      locked exposure) and the same metric contract. A failure walks a bounded ladder: the chosen
//      settings' analysis::conservative_fallbacks() (at most analysis::kMaxFallbackRechecks), then
//      the baseline itself (as shot for unlocked controls, locked controls kept), each rendered once
//      and checked; the first that passes is the effective attempt. The baseline only fails when it
//      is numerically invalid (non-finite samples, an empty render, a non-monotone curve): then no
//      attempt is effective and the photo fails (check_failed(), never an endless loop), also when the
//      baseline render that every attempt is judged against is itself invalid. Every attempt is
//      recorded with the reason it exists (the failures of the one before). When the choice IS the
//      baseline (the analysis kept the camera's rendering, or every searched control is locked) the
//      ladder has that one rung, which evaluate_candidate() can fail on numerical validity only: the
//      check then renders it once at full resolution and scans it for validity (same outcome by
//      construction, without the measurements; its check values are not recorded).
//      The check's stage one is the interactive kind the analysis judged (highlights clipped after
//      demosaicing); the export render clips before it, which differs within two pixels of
//      saturated sites only (ADR 0010).
//      Geometry, masks and denoising (DevelopSettings v2, ADR 0015): Auto adjusts global controls
//      only. The analysis evaluates its candidates on the current settings without their geometry
//      and local adjustments (analysis::evaluation_settings(): native crops line up, masks never land
//      on the wrong content, and the decision is the same whatever the crop), and every settings
//      value it returns carries the current geometry, masks and chroma denoising unchanged. The
//      final check renders the full settings, geometry and masks included, for the choice, its
//      fallbacks and the baseline alike (they share the photographer's geometry and masks), and
//      measures them in the geometry's output frame: the image context (saturation, signal, the
//      neutral surfaces) is mapped through the same geometry (analysis::map_image_context()).
//   5. The locks report: the controls the optimiser sets that are locked, and whether they kept it
//      from its usual targets (a brightness deficit or excess of more than kLockedTargetEv left with
//      exposure, shadows or highlights locked; locked settings that clip), each with a user-facing
//      reason; every searched control locked is noted (kEveryControlLockedNote) but is not a missed
//      target. The brightness findings are measured on the optimiser's choice at analysis
//      resolution. A locked white balance is listed but cannot be judged: the analysis makes no
//      white-balance proposal for it (analysis::AutoResult::white_balance).
//
// Documents (JSON, versioned, read strictly: schema and version must match, every member is
// required, unknown members are refused; numbers are written in shortest round-trip form so a
// document read back gives bit-identical values):
//   "lucent.auto-request" v1  AutoRunRequest (what lucent-worker's analyze job carries);
//   "lucent.auto-run" v1      AutoRun (the analyze job's answer; a batch job's analysis document).
//
// Determinism: the analysis' (ADR 0011 item 28); the final check renders and measures
// deterministically as well. A session reopened from the file regularises exactly like a kept one.
//
// Thread-safety: run_automatic is reentrant; the value types are plain data.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/analysis/auto_settings.hpp>
#include <lucent/analysis/consistency.hpp>
#include <lucent/analysis/metric_contract.hpp>
#include <lucent/application/recipe_store.hpp>
#include <lucent/catalog/jobs.hpp>
#include <lucent/catalog/types.hpp>
#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/engine/develop_settings.hpp>
#include <lucent/raw/decoded_raw.hpp>
#include <lucent/raw/decoder_registry.hpp>

namespace lucent::app {

inline constexpr std::string_view kAutoRequestSchema = "lucent.auto-request";
inline constexpr std::string_view kAutoRunSchema = "lucent.auto-run";
inline constexpr std::string_view kAutoRecordSchema = "lucent.auto-record";
inline constexpr std::uint32_t kAutoDocumentVersion = 1;

// The brightness deficit or excess (EV of display luminance) left by the chosen settings above which
// locked exposure, shadows or highlights count as having prevented the usual target.
inline constexpr double kLockedTargetEv = 0.25;
// The locks report's note when every control the optimiser sets is locked (information, not a missed
// target).
inline constexpr std::string_view kEveryControlLockedNote =
    "Every control Lucent sets automatically is locked, so nothing was changed.";

// ---- Requests ---------------------------------------------------------------------------------

struct AutoRunRequest {
    // Regularise towards this related-shot target (analysis::AutoSession::regularise).
    std::optional<analysis::ConsistencyTarget> target;
    // The item is a caller-marked anchor of its group: also measure the appearance of its current
    // settings.
    bool anchor = false;
    // Run the full-resolution final check and its fallbacks (step 4).
    bool final_check = true;
    // Test only (lucent-worker's fail-final-check hook, AutoServiceOptions): the first n attempts of
    // the final check fail as if a hard check had.
    std::uint32_t fail_final_checks = 0;
};

[[nodiscard]] Expected<std::string> write_auto_request(const AutoRunRequest& request);
// Corrupt ("app.auto.document") for anything but a "lucent.auto-request" v1 document;
// VersionMismatch for a newer version.
[[nodiscard]] Expected<AutoRunRequest> read_auto_request(std::string_view json);

struct AutoRunOptions {
    analysis::AutoOptions analysis;  // its develop options carry the decode limits and memory budget
    // Analyse a photo whose decoded interpretation is Unsupported (recorded by the caller).
    bool allow_unsupported = false;
    // Test only: the final check treats the full-resolution baseline render as numerically invalid
    // (the path no real render takes; see check_failed()).
    bool test_invalid_baseline = false;
};

// Progress: the stage (analysing, checking) and the fraction of the whole run.
// The fraction reflects measured cost (a 45 MP photo: the analysis about a quarter of the run, the
// final check the rest, with steps after its stage one, source pass, baseline render and
// measurement and each attempt).
enum class AutoRunStage : std::uint8_t {
    Analysing = 1,
    Checking = 2,
};
using AutoRunProgress = std::function<void(AutoRunStage stage, double fraction)>;

// ---- Results ----------------------------------------------------------------------------------

// Why an attempt of the final check exists (persisted as to_string(); append only).
enum class AutoAttemptKind : std::uint8_t {
    Chosen = 1,    // the optimiser's choice
    Fallback = 2,  // a conservative fallback of the choice (analysis::conservative_fallbacks)
    Baseline = 3,  // the as-shot baseline (locked controls kept): the last resort
    Anchor = 4,    // a related-shot anchor's own settings, kept (anchor_run(); not checked)
};

// "chosen", "fallback", "baseline", "anchor".
[[nodiscard]] std::string_view to_string(AutoAttemptKind kind) noexcept;

struct AutoCheck {
    catalog::AutoCheckOutcome outcome = catalog::AutoCheckOutcome::NotRun;
    std::vector<std::string> failures;  // developer-facing English, one per failed hard check
    bool forced = false;                // failed by the test hook (AutoRunRequest::fail_final_checks)
    // Full-resolution measurements of the attempt against the baseline (stable names:
    // median_lightness, new_clipped_fraction, total_clipped_fraction, new_blocked_fraction,
    // new_out_of_gamut_fraction, hue_shift_degrees, neutral_shift_de, shadow_noise_lstar,
    // saturation_log_ratio).
    std::vector<analysis::NamedValue> values;
    double seconds = 0.0;  // its render and measurement
};

struct AutoAttempt {
    std::uint32_t index = 0;  // 0: the optimiser's choice; n: the n-th rung of the ladder
    AutoAttemptKind kind = AutoAttemptKind::Chosen;
    engine::DevelopSettings settings;
    core::Digest256 settings_sha256{};  // of the canonical recipe (recipe.hpp)
    std::string reason;                 // developer-facing English: why it exists
    AutoCheck check;
};

// Review flags of an automatic result: the analysis' (analysis::ReviewFlagCode values) and the
// application's own below (persisted; append only).
inline constexpr std::uint16_t kFlagFinalCheckFallback = 1001;     // the full-resolution check failed the choice
inline constexpr std::uint16_t kFlagLocksPreventedTargets = 1002;  // see AutoLocks

struct AutoFlag {
    std::uint16_t code = 0;
    std::string name;                   // "low_confidence", ..., "final_check_fallback"
    std::optional<std::string> metric;  // analysis::to_string(MetricId) when it concerns one metric
    std::string reason;                 // developer-facing English
    bool review = false;                // it recommends a review (see review_recommended)
};

// The name of a flag code ("final_check_fallback", or analysis::to_string(ReviewFlagCode)).
[[nodiscard]] std::string auto_flag_name(std::uint16_t code);
// True for the flags that recommend a review: low confidence (a restrained edit), a large edit, a
// conservative fallback (analysis or final check), no feasible change, a rejected recheck, a
// related-shot look not reached, visible shadow noise, locked settings that clip, a limited
// interpretation, and locks that prevented the usual targets. White-balance uncertainty, missing
// evidence and source saturation are reported but do not by themselves recommend a review.
[[nodiscard]] bool flag_recommends_review(std::uint16_t code) noexcept;
// User-facing English for a flag (reports, the editor).
[[nodiscard]] std::string describe_auto_flag(const AutoFlag& flag);

struct AutoLocks {
    std::vector<engine::DevelopControl> locked;  // locked controls the optimiser would set
    bool prevented_targets = false;              // the locks kept it from its usual targets
    std::vector<std::string> reasons;            // user-facing English, one per finding
};

struct AutoConsistency {
    analysis::ConsistencyTarget target;
    double independent_term = 0.0;  // the independent choice's consistency term
    double term = 0.0;              // the chosen settings'
    bool changed = false;           // regularisation changed the settings
    std::uint32_t added_candidates = 0;
};

struct AutoRun {
    analysis::AutoVersions versions;
    engine::DevelopSettings start;   // the current settings the analysis started from
    engine::DevelopSettings chosen;  // the analysis' choice (regularised with a target)
    double confidence = 0.0;
    analysis::ConfidenceBreakdown confidence_detail;
    bool restrained = false;          // the analysis restrained the edit for low confidence
    bool analysis_fallback = false;   // the analysis-resolution recheck fell back (ADR 0011 item 22)
    std::vector<AutoFlag> flags;      // in analysis order, then the application's
    // The chosen settings' metrics at analysis resolution (source saturation first).
    std::vector<analysis::MetricResult> diagnostics;
    analysis::ShotFeatures shot;            // what grouping compares
    analysis::Appearance appearance;        // of the INDEPENDENT choice at analysis resolution
    std::optional<analysis::Appearance> anchor_appearance;  // of the current settings (anchors)
    bool manual = false;                    // every searched control locked (analysis::is_manual)
    std::optional<AutoConsistency> consistency;
    // The final check's attempts in order; without the check one attempt, the choice, with outcome
    // NotRun. Never empty after a successful run_automatic().
    std::vector<AutoAttempt> attempts;
    std::optional<std::size_t> effective;   // index into attempts; std::nullopt: none passed
    AutoLocks locks;
    raw::InterpretationStatus interpretation = raw::InterpretationStatus::Complete;
    std::vector<std::string> warnings;      // stage one's (developer-facing)
    std::int32_t width = 0;                 // full-resolution upright size
    std::int32_t height = 0;
    double analysis_seconds = 0.0;
    double consistency_seconds = 0.0;
    double check_seconds = 0.0;
    double total_seconds = 0.0;
    std::uint64_t estimated_peak_bytes = 0;  // of the analysis and of the final check, the larger

    // The settings to apply: the effective attempt's, or the choice when nothing was checked.
    [[nodiscard]] const engine::DevelopSettings& effective_settings() const noexcept;
    // Any flag recommends a review, or the final check had to fall back.
    [[nodiscard]] bool review_recommended() const noexcept;
    // The final check ran and no attempt passed (the photo must fail; blueprint step 7).
    [[nodiscard]] bool check_failed() const noexcept { return !attempts.empty() && !effective; }
};

[[nodiscard]] Expected<std::string> write_auto_run(const AutoRun& run);
// Corrupt ("app.auto.document") for anything but a "lucent.auto-run" v1 document (or settings that
// are not a readable recipe); VersionMismatch for a newer version.
[[nodiscard]] Expected<AutoRun> read_auto_run(std::string_view json);

// ---- Running ----------------------------------------------------------------------------------

// See the file comment. Errors: the registry's and decoder's (NotFound, SourceChanged, Unsupported,
// Corrupt, ...); Unsupported ("app.auto.support") for an unsupported interpretation; LimitExceeded
// ("app.auto.memory") when the analysis or the final check would exceed
// options.analysis.develop.memory_budget_bytes (checked before the analysis); InvalidArgument for
// invalid settings, options or target; Cancelled; OutOfMemory. A final check in which no attempt
// passes (a numerically invalid baseline) is NOT an error: the run reports it (check_failed()).
[[nodiscard]] Expected<AutoRun> run_automatic(const raw::DecoderRegistry& registry, const std::filesystem::path& path,
                                              std::uint32_t subimage, const engine::DevelopSettings& current,
                                              const AutoRunRequest& request, const AutoRunOptions& options = {},
                                              const core::CancellationToken& cancel = {},
                                              const AutoRunProgress& progress = {});

// Estimated peak pixel memory of run_automatic for `metadata` (probe metadata): the analysis'
// (analysis::estimate_auto_memory) with the copy of the decoded samples kept for the check, or the
// final check's (stage one, the full-resolution context and baseline features, one stage two), the
// larger. Without settings the final check's stage two is the engine's settings-agnostic estimate
// (denoising at its largest radius, no brush raster).
[[nodiscard]] Expected<std::uint64_t> estimate_automatic_memory(const raw::RawMetadata& metadata,
                                                                const AutoRunOptions& options = {});
// The same for the item's `current` settings: the final check's stage two is estimated for them
// (engine::estimate_develop_memory with settings: their denoising radius, brush masks and the masks'
// base layer), which callers whose photos may have masks should use.
[[nodiscard]] Expected<std::uint64_t> estimate_automatic_memory(const raw::RawMetadata& metadata,
                                                                const engine::DevelopSettings& current,
                                                                const AutoRunOptions& options = {});

// ---- Revisions and records ---------------------------------------------------------------------

// The note of the revision of `attempt` (recipe_service.hpp's ';'-separated key=value fields):
// "auto=<kind>;attempt=<n>;check=<outcome>;policy=<id>/<version>;changed=<controls>;reason=<text>"
// (changed relative to `run.start`; the reason without ';' and newlines, at most 240 bytes).
[[nodiscard]] std::string auto_revision_note(const AutoRun& run, const AutoAttempt& attempt);

// The analysis record of the revision of `attempt` (catalog::NewAutoAnalysis without its revision
// id): versions, the attempt and its check outcome, the run's confidence, whether a review is
// recommended, the flags with reasons (JSON array) and the "lucent.auto-record" v1 document (the
// attempt, its check, the run's confidence breakdown, compact diagnostics, consistency, locks and
// timings).
[[nodiscard]] Expected<catalog::NewAutoAnalysis> auto_analysis_record(const AutoRun& run, const AutoAttempt& attempt,
                                                                      std::optional<catalog::JobId> job,
                                                                      catalog::Timestamp created_at);

// The attempts whose revisions an automatic run writes, in order: every attempt up to and including
// the effective one (the earlier ones failed the final check and are kept with their reason).
// Empty when the check failed for every attempt.
[[nodiscard]] std::vector<AutoAttempt> revision_attempts(const AutoRun& run);

// The automatic revisions (recipe_store.hpp) of revision_attempts(run): origin Auto, the canonical
// recipe of each attempt, its note (auto_revision_note) and its analysis record
// (auto_analysis_record), stamped `now` with `engine_version`.
[[nodiscard]] Expected<std::vector<AutomaticRevision>> automatic_revisions(const AutoRun& run,
                                                                           std::optional<catalog::JobId> job,
                                                                           catalog::Timestamp now,
                                                                           std::string_view engine_version);

// ---- Related-shot anchors -----------------------------------------------------------------------

// The run of a caller-marked anchor of a related-shot group (an automatic batch that keeps related
// shots consistent; related_shots.hpp: "anchors keep their current settings"): `analysed`, the
// anchor's pre-pass analysis (with its anchor appearance), whose starting settings become the one
// effective attempt (kind Anchor, final check NotRun: the photographer's look is kept, never
// optimised or replaced by a fallback); confidence 1 (nothing automatic was decided), no
// diagnostics of the independent choice, only the flags that describe the photo itself (a limited
// interpretation), and `group` (the target of the group its look sets; none when it stands alone)
// as its consistency record. InvalidArgument when `analysed` has no anchor appearance or no attempt.
[[nodiscard]] Expected<AutoRun> anchor_run(const AutoRun& analysed, std::optional<analysis::ConsistencyTarget> group);

}  // namespace lucent::app
