// Automatic settings for one photograph: analysis, related-shot regularisation and the
// full-resolution final check with its bounded conservative fallbacks (auto_develop.hpp, ADR 0013).

#include <lucent/application/auto_develop.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include <lucent/analysis/objective.hpp>
#include <lucent/analysis/proposals.hpp>
#include <lucent/analysis/related_shots.hpp>
#include <lucent/analysis/render_metrics.hpp>
#include <lucent/analysis/source_diagnostics.hpp>
#include <lucent/application/recipe.hpp>
#include <lucent/application/source_item.hpp>
#include <lucent/core/log.hpp>
#include <lucent/core/text.hpp>
#include <lucent/engine/develop.hpp>
#include <lucent/engine/develop_operations.hpp>
#include <lucent/engine/white_balance_model.hpp>
#include <lucent/image/geometry.hpp>

namespace lucent::app {

namespace {

constexpr std::string_view kWhere = "app.auto";
constexpr std::string_view kWhereSupport = "app.auto.support";
constexpr std::string_view kWhereMemory = "app.auto.memory";
constexpr std::size_t kMaxNoteReasonBytes = 240;
// The analysis' share of the progress of a run with the final check (a 45 MP photo: 1.8 s of 7).
constexpr double kAnalysisShare = 0.25;

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

void report(const AutoRunProgress& progress, AutoRunStage stage, double fraction) {
    if (progress) {
        progress(stage, std::clamp(fraction, 0.0, 1.0));
    }
}

// The monotonicity check of the analysis (its contrast and tone curves sampled on 257 points).
bool curves_monotone(const engine::DevelopSettings& s) {
    auto contrast = engine::ContrastCurve::create(s.contrast);
    auto tone = engine::ToneCurve::create(s.tone_curve);
    if (!contrast || !tone) {
        return false;
    }
    double previous_contrast = -std::numeric_limits<double>::infinity();
    double previous_tone = -std::numeric_limits<double>::infinity();
    for (int k = 0; k <= 256; ++k) {
        const double x = k / 256.0;
        const double c = contrast->evaluate(x);
        const double t = tone->evaluate(x);
        if (!std::isfinite(c) || !std::isfinite(t) || c < previous_contrast || t < previous_tone) {
            return false;
        }
        previous_contrast = c;
        previous_tone = t;
    }
    return true;
}

// The edit of `s` relative to the baseline in the search's units (as the analysis computes it).
struct EditReference {
    engine::DevelopSettings baseline;
    bool color = true;
    std::optional<engine::WhiteBalanceModel> model;
    engine::RgbMultipliers baseline_multipliers{1.0, 1.0, 1.0};
};

analysis::EditVector edit_of(const engine::DevelopSettings& s, const EditReference& ref) {
    analysis::EditVector e;
    const engine::DevelopSettings& b = ref.baseline;
    e.exposure_ev = s.exposure_ev - b.exposure_ev;
    if (ref.color && ref.model && !(s.white_balance == b.white_balance)) {
        auto target = ref.model->target_multipliers(s.white_balance);
        const engine::RgbMultipliers& base = ref.baseline_multipliers;
        if (target && (*target)[1] > 0.0 && base[1] > 0.0) {
            e.red_gain = ((*target)[0] / (*target)[1]) / (base[0] / base[1]);
            e.blue_gain = ((*target)[2] / (*target)[1]) / (base[2] / base[1]);
        } else {
            e.red_gain = 1.0 + 2.0 * analysis::kWhiteBalanceResidualBound;
            e.blue_gain = 1.0 + 2.0 * analysis::kWhiteBalanceResidualBound;
        }
    }
    e.shadows_ev = (s.shadows - b.shadows) * engine::kShadowsMaxEv;
    e.highlights_ev = (s.highlights - b.highlights) * engine::kHighlightsMaxEv;
    e.contrast = s.contrast - b.contrast;
    e.saturation = (1.0 + s.saturation) / (1.0 + b.saturation);
    return e;
}

// Probe and decode; with the final check, the memory check of the whole run first (the decode is
// kept for the check, so the file is decoded once).
Expected<raw::DecodedRaw> decode_photo(const raw::DecoderRegistry& registry, const std::filesystem::path& path,
                                       std::uint32_t subimage, const engine::DevelopSettings& current,
                                       const AutoRunOptions& options, bool final_check,
                                       const core::CancellationToken& cancel) {
    raw::ProbeOptions probe_options;
    probe_options.subimage = subimage;
    probe_options.limits = options.analysis.develop.decode_limits;
    probe_options.describe_subimages = false;
    auto selection = registry.select(path, probe_options, cancel);
    if (!selection) {
        return Unexpected(std::move(selection.error()));
    }
    if (final_check) {
        auto estimate = estimate_automatic_memory(selection->probe.metadata, current, options);
        if (!estimate) {
            return Unexpected(std::move(estimate.error()));
        }
        if (*estimate > options.analysis.develop.memory_budget_bytes) {
            return fail(ErrorCode::LimitExceeded,
                        std::format("the automatic settings with their full-resolution check need about {} MiB of "
                                    "pixel memory, above the budget of {} MiB",
                                    *estimate >> 20, options.analysis.develop.memory_budget_bytes >> 20),
                        std::string(kWhereMemory));
        }
    }
    raw::DecodeOptions decode_options;
    decode_options.subimage = subimage;
    decode_options.limits = options.analysis.develop.decode_limits;
    return selection->decoder->decode(path, decode_options, cancel);
}

// A deep copy of decoded samples (the final check's, kept while the analysis consumes the original).
Expected<raw::RawPixels> clone_pixels(const raw::RawPixels& pixels) {
    return std::visit(
        [](const auto& buffer) -> Expected<raw::RawPixels> {
            auto copy = buffer.clone();
            if (!copy) {
                return Unexpected(std::move(copy.error()));
            }
            return raw::RawPixels(std::move(*copy));
        },
        pixels);
}

Expected<raw::DecodedRaw> clone_decoded(const raw::DecodedRaw& source) {
    raw::DecodedRaw out;
    out.contract = source.contract;
    out.metadata = source.metadata;
    auto image = std::visit(
        [](const auto& frame) -> Expected<raw::RawImage> {
            using Frame = std::decay_t<decltype(frame)>;
            auto pixels = clone_pixels(frame.pixels);
            if (!pixels) {
                return Unexpected(std::move(pixels.error()));
            }
            Frame copy;
            copy.pixels = std::move(*pixels);
            copy.encoding = frame.encoding;
            if constexpr (std::is_same_v<Frame, raw::MosaicFrame>) {
                copy.cfa = frame.cfa;
            }
            return raw::RawImage(std::move(copy));
        },
        source.image);
    if (!image) {
        return Unexpected(std::move(image.error()));
    }
    out.image = std::move(*image);
    return out;
}

// The numerical validity of a full-resolution render (evaluate_candidate()'s NumericalValidity, the
// only check a baseline can fail): non-finite samples and the finite pixels, as measure_render()
// counts them.
struct Validity {
    std::uint64_t nonfinite_samples = 0;
    std::uint64_t pixels = 0;
};

Validity scan_validity(const engine::ColorImage& display) {
    Validity v;
    const auto& px = display.pixels;
    const std::int32_t channels = px.channels();
    for (std::int32_t y = 0; y < px.height(); ++y) {
        const float* row = px.row(y);
        for (std::int32_t x = 0; x < px.width(); ++x, row += channels) {
            std::uint64_t bad = 0;
            for (std::int32_t c = 0; c < channels; ++c) {
                bad += static_cast<std::uint64_t>(!std::isfinite(row[c]));
            }
            v.nonfinite_samples += bad;
            v.pixels += static_cast<std::uint64_t>(bad == 0);
        }
    }
    return v;
}

// evaluate_candidate()'s wording of a numerical-validity failure; empty when valid.
std::string validity_failure(const Validity& v, bool curves_monotone) {
    if (v.pixels == 0) {
        return "numerical_validity: empty render";
    }
    if (v.nonfinite_samples > 0) {
        return std::format("numerical_validity: {} non-finite output samples", v.nonfinite_samples);
    }
    if (!curves_monotone) {
        return "numerical_validity: a contrast or tone curve is not non-decreasing";
    }
    return {};
}

Status check_support(const raw::DecodedRaw& decoded, const AutoRunOptions& options, const std::filesystem::path& path) {
    const raw::Interpretation& interpretation = decoded.metadata.interpretation;
    if (support_status_for(interpretation.status) != SupportStatus::Unsupported || options.allow_unsupported) {
        return ok();
    }
    std::string reasons(raw::to_string(interpretation.status));
    for (const raw::InterpretationIssue& issue : interpretation.issues) {
        reasons += "; " + issue.message;
    }
    return fail(ErrorCode::Unsupported,
                std::format("'{}' cannot be interpreted faithfully, so it is not analysed: {}",
                            core::path_to_utf8_lossy(path), reasons),
                std::string(kWhereSupport));
}

std::string join(const std::vector<std::string>& texts, std::string_view separator) {
    std::string out;
    for (const std::string& text : texts) {
        if (!out.empty()) {
            out += separator;
        }
        out += text;
    }
    return out;
}

// ---- Final check --------------------------------------------------------------------------------

struct Rung {
    AutoAttemptKind kind = AutoAttemptKind::Chosen;
    engine::DevelopSettings settings;
};

std::vector<analysis::NamedValue> check_values(const analysis::RenderStats& s) {
    return {{"median_lightness", s.quantiles.p50},
            {"new_clipped_fraction", s.fraction(s.new_highlight_clipped)},
            {"total_clipped_fraction", s.fraction(s.highlight_clipped)},
            {"new_blocked_fraction", s.fraction(s.new_shadow_blocked)},
            {"new_out_of_gamut_fraction", s.fraction(s.new_out_of_gamut)},
            {"hue_shift_degrees", s.hue_shift_degrees},
            {"neutral_shift_de", s.neutral_shift_de},
            {"shadow_noise_lstar", s.shadow_noise_lstar},
            {"saturation_log_ratio", s.saturation_log_ratio}};
}

// The first attempt of a ladder: the optimiser's choice.
Expected<AutoAttempt> first_attempt(const engine::DevelopSettings& chosen) {
    AutoAttempt attempt;
    attempt.index = 0;
    attempt.kind = AutoAttemptKind::Chosen;
    attempt.settings = chosen;
    auto document = write_recipe(chosen);
    if (!document) {
        return Unexpected(std::move(document.error()));
    }
    attempt.settings_sha256 = document->sha256;
    attempt.reason = "the optimiser's choice";
    return attempt;
}

// Progress of the final check (fractions of the check, by measured cost on a 45 MP photo: the source
// pass, stage one and context about a quarter, the baseline's render and measurement about 45 %, the
// choice's render and measurement the rest).
constexpr double kCheckAfterSource = 0.16;
constexpr double kCheckAfterStageOne = 0.21;
constexpr double kCheckAfterContext = 0.27;
constexpr double kCheckAfterBaselineRender = 0.33;
constexpr double kCheckAfterBaseline = 0.72;
constexpr double kCheckAfterFirstRender = 0.78;
constexpr double kCheckAfterFirstAttempt = 0.97;

// Step 4 of the file comment: the attempts of the ladder in order, until one passes. `decoded` is
// the run's decode (kept for the check).
Expected<std::vector<AutoAttempt>> final_check(raw::DecodedRaw&& decoded, const analysis::AutoResult& result,
                                               const engine::DevelopSettings& chosen, const AutoRunRequest& request,
                                               const AutoRunOptions& options, const core::CancellationToken& cancel,
                                               const std::function<void(double)>& progress) {
    const engine::DevelopOptions& develop = options.analysis.develop;
    if (result.candidates.empty() || result.candidates.front().origin != analysis::CandidateOrigin::Baseline) {
        return fail(ErrorCode::Internal, "the analysis has no baseline candidate", std::string(kWhere));
    }
    const engine::DevelopSettings& baseline = result.candidates.front().settings;
    engine::StageOneRequest stage_request;
    stage_request.kind = engine::StageOneKind::Interactive;
    stage_request.white_balance = engine::AsShotWhiteBalance{};

    // The choice is the baseline (the camera's rendering for the controls the optimiser sets, or
    // every one of them locked): the ladder is that one rung, and evaluate_candidate() can fail a
    // baseline on numerical validity only. Its full-resolution render is scanned for it: the same
    // outcome by construction, without the measurements.
    if (chosen == baseline) {
        const auto started = Clock::now();
        auto stage_one = engine::render_stage_one(std::move(decoded), stage_request, develop, cancel);
        if (!stage_one) {
            return Unexpected(std::move(stage_one.error()));
        }
        progress(0.4);
        auto attempt = first_attempt(chosen);
        if (!attempt) {
            return Unexpected(std::move(attempt.error()));
        }
        auto rendered = engine::render_stage_two(*stage_one, chosen, develop, cancel);
        if (!rendered) {
            if (rendered.error().code == ErrorCode::Cancelled || rendered.error().code == ErrorCode::OutOfMemory) {
                return Unexpected(std::move(rendered.error()));
            }
            attempt->check.failures.push_back("render: " + rendered.error().describe());
        } else {
            progress(0.9);
            const std::string failure = validity_failure(scan_validity(rendered->display), curves_monotone(chosen));
            if (!failure.empty()) {
                attempt->check.failures.push_back(failure);
            }
        }
        bool passed = attempt->check.failures.empty();
        if (request.fail_final_checks > 0) {
            passed = false;
            attempt->check.forced = true;
            attempt->check.failures.push_back("failed on request (test hook fail-final-check)");
        }
        attempt->check.outcome = passed ? catalog::AutoCheckOutcome::Passed : catalog::AutoCheckOutcome::Failed;
        attempt->check.seconds = seconds_since(started);
        progress(1.0);
        return std::vector<AutoAttempt>{std::move(*attempt)};
    }

    const raw::RawMetadata& md = decoded.metadata;
    const image::Size upright = image::oriented_size(md.geometry.visible_area.size(), md.geometry.orientation);
    const std::array<image::Size, 1> grids{upright};
    auto source = analysis::analyze_source(decoded, grids, develop.parallel, cancel);
    if (!source) {
        return Unexpected(std::move(source.error()));
    }
    progress(kCheckAfterSource);
    auto stage_one = engine::render_stage_one(std::move(decoded), stage_request, develop, cancel);
    if (!stage_one) {
        return Unexpected(std::move(stage_one.error()));
    }
    progress(kCheckAfterStageOne);
    const bool coverage_ok = stage_one->full_size == upright && stage_one->pixels.size() == upright &&
                             !source->coverage.empty() && source->coverage[0].size == upright;
    auto context = analysis::make_image_context(*stage_one, coverage_ok ? &source->coverage[0] : nullptr,
                                                source->diagnostics.noise);
    if (!context) {
        return Unexpected(std::move(context.error()));
    }
    source->coverage.clear();
    source->coverage.shrink_to_fit();

    // The analysis' policy context, at full resolution.
    const bool color = stage_one->pixels.channels() == 3;
    analysis::NaturalV1Context policy;
    policy.tone = result.tone;
    policy.exposure_locked = chosen.locks.contains(engine::DevelopControl::Exposure);
    policy.color = color;
    if (color && result.white_balance) {
        const analysis::WhiteBalanceProposal& wb = *result.white_balance;
        policy.white_balance_evidence = wb.evidence;
        policy.white_balance_confidence = wb.confidence;
        policy.red_target = wb.red_gain;
        policy.blue_target = wb.blue_gain;
        if (wb.evidence && wb.confidence >= analysis::kNeutralEvidenceMinConfidence) {
            auto mask = analysis::neutral_evidence_mask(*stage_one, *context, analysis::as_shot_clip_level(*stage_one),
                                                        wb.cast_log_rg, wb.cast_log_bg);
            if (!mask) {
                return Unexpected(std::move(mask.error()));
            }
            context->neutral_evidence = std::move(*mask);
        }
    }
    // The check renders the photographer's geometry (the choice, its fallbacks and the baseline all
    // carry it): measure in its output frame, the context mapped through the same geometry. The
    // white balance was decided on the whole frame's neutral evidence; a crop without neutral
    // surfaces does not fail it (NaturalV1Context::neutral_evidence_beyond_crop).
    if (!baseline.geometry.is_identity()) {
        const auto neutral = static_cast<std::uint64_t>(
            std::count_if(context->neutral_evidence.begin(), context->neutral_evidence.end(),
                          [](std::uint8_t v) { return v != 0; }));
        policy.neutral_evidence_beyond_crop = neutral >= analysis::kMinNeutralPixels;
        auto mapping = engine::GeometryMapping::create(baseline.geometry, stage_one->pixels.size());
        if (!mapping) {
            return Unexpected(std::move(mapping.error()));
        }
        auto mapped = analysis::map_image_context(*context, *mapping);
        if (!mapped) {
            return Unexpected(std::move(mapped.error()));
        }
        *context = std::move(*mapped);
    }
    EditReference reference;
    reference.baseline = baseline;
    reference.color = color;
    if (color) {
        auto model = engine::WhiteBalanceModel::create(stage_one->metadata);
        if (!model) {
            return Unexpected(std::move(model.error()));
        }
        auto multipliers = model->target_multipliers(baseline.white_balance);
        if (!multipliers) {
            return Unexpected(std::move(multipliers.error()));
        }
        reference.baseline_multipliers = *multipliers;
        reference.model = std::move(*model);
    }
    progress(kCheckAfterContext);

    // The baseline at full resolution: the reference of every relative check. When it is itself
    // numerically invalid nothing can be judged against it: the choice fails with that reason and,
    // the baseline being the last resort, no attempt passes (check_failed()).
    analysis::BaselineFeatures features;
    analysis::RenderStats base;
    {
        auto rendered = engine::render_stage_two(*stage_one, baseline, develop, cancel);
        if (!rendered) {
            return Unexpected(std::move(rendered.error()));
        }
        progress(kCheckAfterBaselineRender);
        auto measured = analysis::measure_render(rendered->display, *context, nullptr, &features, develop.parallel,
                                                 cancel);
        if (!measured) {
            return Unexpected(std::move(measured.error()));
        }
        if (measured->nonfinite_samples > 0 || measured->pixels == 0 || options.test_invalid_baseline) {
            auto attempt = first_attempt(chosen);
            if (!attempt) {
                return Unexpected(std::move(attempt.error()));
            }
            attempt->check.outcome = catalog::AutoCheckOutcome::Failed;
            attempt->check.forced = options.test_invalid_baseline;
            attempt->check.failures.push_back(
                std::format("numerical_validity: the full-resolution as-shot baseline render has {} non-finite "
                            "samples ({} valid pixels){}, so no attempt can be judged",
                            measured->nonfinite_samples, measured->pixels,
                            options.test_invalid_baseline ? " (declared invalid by the test hook)" : ""));
            return std::vector<AutoAttempt>{std::move(*attempt)};
        }
        base = *measured;
    }
    progress(kCheckAfterBaseline);

    // The ladder: the choice, its conservative fallbacks, the baseline.
    std::vector<Rung> ladder;
    ladder.push_back(Rung{AutoAttemptKind::Chosen, chosen});
    std::vector<engine::DevelopSettings> fallbacks = analysis::conservative_fallbacks(chosen, baseline);
    if (fallbacks.size() > analysis::kMaxFallbackRechecks) {
        fallbacks.resize(analysis::kMaxFallbackRechecks);
    }
    for (engine::DevelopSettings& fallback : fallbacks) {
        ladder.push_back(Rung{AutoAttemptKind::Fallback, std::move(fallback)});
    }
    ladder.push_back(Rung{AutoAttemptKind::Baseline, baseline});

    // After attempt i (the first takes the rest of the estimate; later ones, rare, the last percent).
    const auto attempt_done = [&](std::size_t i) {
        return i == 0 ? kCheckAfterFirstAttempt
                      : kCheckAfterFirstAttempt + (0.995 - kCheckAfterFirstAttempt) * static_cast<double>(i) /
                                                      static_cast<double>(ladder.size() - 1);
    };
    std::vector<AutoAttempt> attempts;
    for (std::size_t i = 0; i < ladder.size(); ++i) {
        if (Status c = cancel.check(kWhere); !c) {
            return Unexpected(std::move(c.error()));
        }
        const auto started = Clock::now();
        AutoAttempt attempt;
        attempt.index = static_cast<std::uint32_t>(i);
        attempt.kind = ladder[i].kind;
        attempt.settings = ladder[i].settings;
        auto document = write_recipe(attempt.settings);
        if (!document) {
            return Unexpected(std::move(document.error()));
        }
        attempt.settings_sha256 = document->sha256;
        if (i == 0) {
            attempt.reason = "the optimiser's choice";
        } else {
            const AutoAttempt& previous = attempts.back();
            attempt.reason = std::format("{} after attempt {} failed the full-resolution check: {}",
                                         attempt.kind == AutoAttemptKind::Baseline ? "the as-shot baseline"
                                                                                   : "a conservative fallback",
                                         previous.index, join(previous.check.failures, "; "));
        }
        const bool is_baseline = attempt.settings == baseline;
        analysis::RenderStats stats;
        if (is_baseline) {
            stats = base;
        } else if (!(attempt.settings.geometry == baseline.geometry)) {
            // Never by construction (every rung carries the photographer's geometry): a render of
            // another frame cannot be compared with the baseline.
            attempt.check.outcome = catalog::AutoCheckOutcome::Failed;
            attempt.check.failures.push_back("geometry: the attempt's geometry is not the baseline's");
            attempt.check.seconds = seconds_since(started);
            attempts.push_back(std::move(attempt));
            progress(attempt_done(i));
            continue;
        } else {
            auto rendered = engine::render_stage_two(*stage_one, attempt.settings, develop, cancel);
            if (!rendered) {
                if (rendered.error().code == ErrorCode::Cancelled || rendered.error().code == ErrorCode::OutOfMemory) {
                    return Unexpected(std::move(rendered.error()));
                }
                attempt.check.outcome = catalog::AutoCheckOutcome::Failed;
                attempt.check.failures.push_back("render: " + rendered.error().describe());
                attempt.check.seconds = seconds_since(started);
                attempts.push_back(std::move(attempt));
                progress(attempt_done(i));
                continue;
            }
            if (i == 0) {
                progress(kCheckAfterFirstRender);
            }
            auto measured = analysis::measure_render(rendered->display, *context, &features, nullptr,
                                                     develop.parallel, cancel);
            if (!measured) {
                return Unexpected(std::move(measured.error()));
            }
            stats = *measured;
        }
        const analysis::EditVector edit = edit_of(attempt.settings, reference);
        analysis::CandidateEvaluation evaluation = analysis::evaluate_candidate(
            stats, base, edit, policy, nullptr, is_baseline, curves_monotone(attempt.settings));
        attempt.check.values = check_values(stats);
        attempt.check.failures = std::move(evaluation.failures);
        bool passed = evaluation.feasible;
        if (i < request.fail_final_checks) {
            passed = false;
            attempt.check.forced = true;
            attempt.check.failures.push_back("failed on request (test hook fail-final-check)");
        }
        attempt.check.outcome = passed ? catalog::AutoCheckOutcome::Passed : catalog::AutoCheckOutcome::Failed;
        attempt.check.seconds = seconds_since(started);
        attempts.push_back(std::move(attempt));
        progress(attempt_done(i));
        if (passed) {
            break;
        }
    }
    return attempts;
}

// ---- Locks --------------------------------------------------------------------------------------

AutoLocks locks_report(const engine::DevelopSettings& current, const analysis::AutoResult& result, bool color,
                       bool manual) {
    using engine::DevelopControl;
    AutoLocks locks;
    for (const DevelopControl control : {DevelopControl::WhiteBalance, DevelopControl::Exposure,
                                         DevelopControl::Contrast, DevelopControl::Highlights,
                                         DevelopControl::Shadows, DevelopControl::Saturation}) {
        if (!color && (control == DevelopControl::WhiteBalance || control == DevelopControl::Saturation)) {
            continue;
        }
        if (current.locks.contains(control)) {
            locks.locked.push_back(control);
        }
    }
    if (locks.locked.empty()) {
        return locks;
    }
    const auto locked = [&](DevelopControl control) { return current.locks.contains(control); };
    double deficit = 0.0;
    double excess = 0.0;
    if (const analysis::MetricResult* tonal = result.diagnostic(analysis::MetricId::TonalDistribution)) {
        deficit = tonal->value("deficit_ev").value_or(0.0);
        excess = tonal->value("excess_ev").value_or(0.0);
    }
    if (deficit > kLockedTargetEv) {
        if (locked(DevelopControl::Exposure)) {
            locks.reasons.push_back(std::format(
                "Exposure is locked, so the photo stays about {:.1f} EV darker than Lucent's usual target.", deficit));
        } else if (locked(DevelopControl::Shadows) && result.tone.shadows_ev > 0.05) {
            locks.reasons.push_back(std::format(
                "Shadows are locked, so the shadows stay about {:.1f} EV darker than Lucent's usual target.", deficit));
        }
    }
    if (excess > kLockedTargetEv) {
        if (locked(DevelopControl::Exposure)) {
            locks.reasons.push_back(std::format(
                "Exposure is locked, so the photo stays about {:.1f} EV brighter than Lucent's usual target.", excess));
        } else if (locked(DevelopControl::Highlights) && result.tone.highlights_ev < -0.05) {
            locks.reasons.push_back(std::format(
                "Highlights are locked, so bright areas stay about {:.1f} EV brighter than Lucent's usual target.",
                excess));
        }
    }
    if (result.has_flag(analysis::ReviewFlagCode::LockedControlsClip)) {
        locks.reasons.push_back("Your locked settings clip highlights; Lucent does not change locked controls.");
    }
    locks.prevented_targets = !locks.reasons.empty();
    if (manual) {
        locks.reasons.emplace_back(kEveryControlLockedNote);
    }
    return locks;
}

AutoFlag make_flag(std::uint16_t code, std::optional<std::string> metric, std::string reason) {
    AutoFlag flag;
    flag.code = code;
    flag.name = auto_flag_name(code);
    flag.metric = std::move(metric);
    flag.reason = std::move(reason);
    flag.review = flag_recommends_review(code);
    return flag;
}

std::string sanitize_note_text(std::string_view text) {
    std::string out = core::sanitize_utf8(text);
    for (char& c : out) {
        if (c == ';' || c == '\n' || c == '\r') {
            c = ',';
        }
    }
    if (out.size() > kMaxNoteReasonBytes) {
        std::size_t end = kMaxNoteReasonBytes;
        while (end > 0 && (static_cast<unsigned char>(out[end]) & 0xC0) == 0x80) {
            --end;
        }
        out.resize(end);
    }
    return out;
}

}  // namespace

// ---- Names and flags ----------------------------------------------------------------------------

std::string_view to_string(AutoAttemptKind kind) noexcept {
    switch (kind) {
        case AutoAttemptKind::Chosen:
            return "chosen";
        case AutoAttemptKind::Fallback:
            return "fallback";
        case AutoAttemptKind::Baseline:
            return "baseline";
        case AutoAttemptKind::Anchor:
            return "anchor";
    }
    return "invalid";
}

std::string auto_flag_name(std::uint16_t code) {
    switch (code) {
        case kFlagFinalCheckFallback:
            return "final_check_fallback";
        case kFlagLocksPreventedTargets:
            return "locks_prevented_targets";
        default:
            return std::string(analysis::to_string(static_cast<analysis::ReviewFlagCode>(code)));
    }
}

bool flag_recommends_review(std::uint16_t code) noexcept {
    using analysis::ReviewFlagCode;
    switch (code) {
        case static_cast<std::uint16_t>(ReviewFlagCode::LowConfidence):
        case static_cast<std::uint16_t>(ReviewFlagCode::LockedControlsClip):
        case static_cast<std::uint16_t>(ReviewFlagCode::VisibleShadowNoise):
        case static_cast<std::uint16_t>(ReviewFlagCode::NoFeasibleCandidate):
        case static_cast<std::uint16_t>(ReviewFlagCode::InterpretationLimited):
        case static_cast<std::uint16_t>(ReviewFlagCode::RecheckRejected):
        case static_cast<std::uint16_t>(ReviewFlagCode::ConsistencyLimited):
        case static_cast<std::uint16_t>(ReviewFlagCode::LargeEdit):
        case static_cast<std::uint16_t>(ReviewFlagCode::ConservativeFallback):
        case kFlagFinalCheckFallback:
        case kFlagLocksPreventedTargets:
            return true;
        default:
            return false;
    }
}

std::string describe_auto_flag(const AutoFlag& flag) {
    using analysis::ReviewFlagCode;
    switch (flag.code) {
        case static_cast<std::uint16_t>(ReviewFlagCode::LowConfidence):
            return "Lucent was unsure about the automatic edit, so it kept the change small.";
        case static_cast<std::uint16_t>(ReviewFlagCode::InsufficientEvidence):
            return "Part of the photo gave too little evidence for one of Lucent's checks.";
        case static_cast<std::uint16_t>(ReviewFlagCode::SourceSaturation):
            return "Parts of the photo were overexposed in the camera.";
        case static_cast<std::uint16_t>(ReviewFlagCode::LockedControlsClip):
            return "Your locked settings clip highlights.";
        case static_cast<std::uint16_t>(ReviewFlagCode::WhiteBalanceUncertain):
            return "The colour of the light was uncertain, so the white balance was kept close to the camera's.";
        case static_cast<std::uint16_t>(ReviewFlagCode::NoiseModelUnavailable):
            return "The camera's noise is unknown, so shadows were lifted only carefully.";
        case static_cast<std::uint16_t>(ReviewFlagCode::VisibleShadowNoise):
            return "Noise may be visible in the shadows.";
        case static_cast<std::uint16_t>(ReviewFlagCode::NativeChecksUnavailable):
            return "Fine detail could not be checked at full size.";
        case static_cast<std::uint16_t>(ReviewFlagCode::NoFeasibleCandidate):
        case static_cast<std::uint16_t>(ReviewFlagCode::RecheckRejected):
            return "No automatic change passed Lucent's quality checks, so the photo keeps its original look.";
        case static_cast<std::uint16_t>(ReviewFlagCode::InterpretationLimited):
            return "Lucent could not apply every camera-specific correction to this file.";
        case static_cast<std::uint16_t>(ReviewFlagCode::ConsistencyLimited):
            return "The photo could not be matched to its related shots within the quality checks.";
        case static_cast<std::uint16_t>(ReviewFlagCode::LargeEdit):
            return "Lucent made a large automatic change; check that it suits the photo.";
        case static_cast<std::uint16_t>(ReviewFlagCode::ConservativeFallback):
        case kFlagFinalCheckFallback:
            return "The first automatic edit failed a quality check, so a more conservative one was used.";
        case kFlagLocksPreventedTargets:
            // The specifics are the locks report's (AutoLocks::reasons), listed once beside it.
            return "Locked controls kept Auto from its usual targets.";
        default:
            return flag.reason;
    }
}

// ---- AutoRun ------------------------------------------------------------------------------------

const engine::DevelopSettings& AutoRun::effective_settings() const noexcept {
    if (effective && *effective < attempts.size()) {
        return attempts[*effective].settings;
    }
    return chosen;
}

bool AutoRun::review_recommended() const noexcept {
    return std::any_of(flags.begin(), flags.end(), [](const AutoFlag& flag) { return flag.review; }) ||
           check_failed();
}

std::vector<AutoAttempt> revision_attempts(const AutoRun& run) {
    std::vector<AutoAttempt> out;
    if (!run.effective) {
        return out;
    }
    for (std::size_t i = 0; i <= *run.effective && i < run.attempts.size(); ++i) {
        out.push_back(run.attempts[i]);
    }
    return out;
}

Expected<std::vector<AutomaticRevision>> automatic_revisions(const AutoRun& run, std::optional<catalog::JobId> job,
                                                            catalog::Timestamp now, std::string_view engine_version) {
    std::vector<AutomaticRevision> out;
    for (const AutoAttempt& attempt : revision_attempts(run)) {
        auto document = write_recipe(attempt.settings);
        if (!document) {
            return Unexpected(std::move(document.error()));
        }
        AutomaticRevision entry;
        entry.revision.origin = catalog::RevisionOrigin::Auto;
        entry.revision.schema_name = std::string(kRecipeSchema);
        entry.revision.schema_version = kRecipeSchemaVersion;
        entry.revision.settings_json = std::move(document->json);
        entry.revision.engine_version = std::string(engine_version);
        entry.revision.note = auto_revision_note(run, attempt);
        entry.revision.created_at = now;
        auto record = auto_analysis_record(run, attempt, job, now);
        if (!record) {
            return Unexpected(std::move(record.error()));
        }
        entry.analysis = std::move(*record);
        out.push_back(std::move(entry));
    }
    return out;
}

std::string auto_revision_note(const AutoRun& run, const AutoAttempt& attempt) {
    std::string controls;
    for (const engine::DevelopControl control : changed_controls(run.start, attempt.settings)) {
        if (!controls.empty()) {
            controls += ",";
        }
        controls += engine::to_string(control);
    }
    return std::format("auto={};attempt={};check={};policy={}/{};changed={};reason={}", to_string(attempt.kind),
                       attempt.index, catalog::to_string(attempt.check.outcome), run.versions.policy,
                       run.versions.policy_version, controls, sanitize_note_text(attempt.reason));
}

// ---- Running ------------------------------------------------------------------------------------

namespace {

Expected<std::uint64_t> automatic_memory(const raw::RawMetadata& metadata, const engine::DevelopSettings* current,
                                         const AutoRunOptions& options) {
    auto analysis_bytes = analysis::estimate_auto_memory(metadata, options.analysis);
    if (!analysis_bytes) {
        return Unexpected(std::move(analysis_bytes.error()));
    }
    engine::StageOneRequest request;
    request.kind = engine::StageOneKind::Interactive;
    auto develop = current != nullptr
                       ? engine::estimate_develop_memory(metadata, request, *current, options.analysis.develop)
                       : engine::estimate_develop_memory(metadata, request, options.analysis.develop);
    if (!develop) {
        return Unexpected(std::move(develop.error()));
    }
    const image::Size upright =
        image::oriented_size(metadata.geometry.visible_area.size(), metadata.geometry.orientation);
    const auto pixels = static_cast<std::uint64_t>(std::max<std::int64_t>(0, upright.area()));
    // Decode and stage one with the full-resolution saturation coverage (4 bytes per pixel); then
    // the stage one kept, the image context (saturation, signal, neutral mask: 9 bytes per pixel),
    // the baseline's features (lightness, a, b, chroma, flags: 17 bytes per pixel) and one stage two.
    // The analysis runs beside the copy of the decoded samples the check reuses.
    const std::uint64_t analysis_with_copy = *analysis_bytes + develop->render.raw_frame_bytes;
    const std::uint64_t first = develop->stage_one_peak + pixels * 4;
    const std::uint64_t second = develop->stage_one_bytes + pixels * (9 + 17) + develop->stage_two_bytes;
    return std::max({analysis_with_copy, first, second});
}

}  // namespace

Expected<std::uint64_t> estimate_automatic_memory(const raw::RawMetadata& metadata, const AutoRunOptions& options) {
    return automatic_memory(metadata, nullptr, options);
}

Expected<std::uint64_t> estimate_automatic_memory(const raw::RawMetadata& metadata,
                                                  const engine::DevelopSettings& current,
                                                  const AutoRunOptions& options) {
    return automatic_memory(metadata, &current, options);
}

Expected<AutoRun> run_automatic(const raw::DecoderRegistry& registry, const std::filesystem::path& path,
                                std::uint32_t subimage, const engine::DevelopSettings& current,
                                const AutoRunRequest& request, const AutoRunOptions& options,
                                const core::CancellationToken& cancel, const AutoRunProgress& progress) {
    try {
        const auto started = Clock::now();
        if (Status valid = current.validate(); !valid) {
            return Unexpected(std::move(valid.error()));
        }
        AutoRun run;
        run.start = current;
        // The analysis' share of the progress (about a quarter of a 45 MP run with the check).
        const double analysis_share = request.final_check ? kAnalysisShare : 1.0;
        const auto analysing = [&](double f) { report(progress, AutoRunStage::Analysing, analysis_share * f); };
        analysing(0.0);

        // 1-2. Decode once (a copy is kept for the final check); the independent analysis.
        auto decoded = decode_photo(registry, path, subimage, current, options, request.final_check, cancel);
        if (!decoded) {
            return Unexpected(std::move(decoded.error()));
        }
        if (Status supported = check_support(*decoded, options, path); !supported) {
            return Unexpected(std::move(supported.error()));
        }
        run.interpretation = decoded->metadata.interpretation.status;
        if (auto estimate = estimate_automatic_memory(decoded->metadata, current, options)) {
            run.estimated_peak_bytes = *estimate;
        }
        std::optional<raw::DecodedRaw> kept;
        if (request.final_check) {
            auto copy = clone_decoded(*decoded);
            if (!copy) {
                return Unexpected(std::move(copy.error()));
            }
            kept = std::move(*copy);
        }
        analysing(0.3);
        std::optional<analysis::AutoResult> result;
        {
            auto session = analysis::AutoSession::open(std::move(*decoded), current, options.analysis, cancel);
            if (!session) {
                return Unexpected(std::move(session.error()));
            }
            const analysis::AutoResult& independent = session->independent();
            run.appearance = independent.appearance;
            run.analysis_seconds = seconds_since(started);
            analysing(0.9);
            if (request.anchor) {
                auto appearance = session->appearance_of(current, cancel);
                if (!appearance) {
                    return Unexpected(std::move(appearance.error()));
                }
                run.anchor_appearance = *appearance;
            }
            // 3. Related-shot regularisation.
            if (request.target) {
                const auto t = Clock::now();
                auto regularised = session->regularise(*request.target, cancel);
                if (!regularised) {
                    return Unexpected(std::move(regularised.error()));
                }
                result = std::move(*regularised);
                run.consistency_seconds = seconds_since(t);
                AutoConsistency consistency;
                consistency.target = *request.target;
                if (result->consistency) {
                    consistency.independent_term = result->consistency->independent_terms.total;
                    consistency.term = result->consistency->terms.total;
                    consistency.changed = result->consistency->changed;
                    consistency.added_candidates = result->consistency->added_candidates;
                }
                run.consistency = std::move(consistency);
            } else {
                result = independent;
            }
        }  // the session's images are released before the final check
        analysing(1.0);

        run.versions = result->versions;
        run.chosen = result->settings;
        run.confidence = result->confidence;
        run.confidence_detail = result->confidence_detail;
        run.restrained = result->restrained;
        run.analysis_fallback = result->fallback;
        for (const analysis::ReviewFlag& flag : result->flags) {
            run.flags.push_back(make_flag(static_cast<std::uint16_t>(flag.code),
                                          flag.metric ? std::optional<std::string>(analysis::to_string(*flag.metric))
                                                      : std::nullopt,
                                          flag.reason));
        }
        run.diagnostics = result->diagnostics;
        run.shot = result->shot;
        run.manual = analysis::is_manual(current, run.shot.color);
        run.warnings = result->warnings;
        run.width = result->full_size.width;
        run.height = result->full_size.height;
        run.locks = locks_report(current, *result, run.shot.color, run.manual);
        if (run.locks.prevented_targets) {
            std::vector<std::string> prevented(run.locks.reasons.begin(), run.locks.reasons.end());
            if (run.manual && !prevented.empty()) {
                prevented.pop_back();  // the "everything locked" note is not a missed target
            }
            run.flags.push_back(make_flag(kFlagLocksPreventedTargets, std::nullopt, join(prevented, " ")));
        }

        // 4. The full-resolution final check.
        if (request.final_check) {
            const auto t = Clock::now();
            auto attempts = final_check(std::move(*kept), *result, run.chosen, request, options, cancel, [&](double f) {
                report(progress, AutoRunStage::Checking, analysis_share + (1.0 - analysis_share) * f);
            });
            kept.reset();
            if (!attempts) {
                return Unexpected(std::move(attempts.error()));
            }
            run.attempts = std::move(*attempts);
            run.check_seconds = seconds_since(t);
            for (std::size_t i = 0; i < run.attempts.size(); ++i) {
                if (run.attempts[i].check.outcome == catalog::AutoCheckOutcome::Passed) {
                    run.effective = i;
                    break;
                }
            }
            if (!run.effective || *run.effective > 0) {
                const AutoAttempt& first = run.attempts.front();
                std::string reason = std::format("the full-resolution check failed the choice ({})",
                                                 join(first.check.failures, "; "));
                if (run.effective) {
                    reason += std::format("; attempt {} ({}) passed", *run.effective,
                                          to_string(run.attempts[*run.effective].kind));
                } else {
                    reason += "; no attempt passed";
                }
                run.flags.push_back(make_flag(kFlagFinalCheckFallback, std::nullopt, std::move(reason)));
            }
        } else {
            auto attempt = first_attempt(run.chosen);
            if (!attempt) {
                return Unexpected(std::move(attempt.error()));
            }
            run.attempts.push_back(std::move(*attempt));
            run.effective = 0;
        }
        run.total_seconds = seconds_since(started);
        report(progress, request.final_check ? AutoRunStage::Checking : AutoRunStage::Analysing, 1.0);
        return run;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory during the automatic settings", std::string(kWhere));
    }
}

// ---- Related-shot anchors -----------------------------------------------------------------------

Expected<AutoRun> anchor_run(const AutoRun& analysed, std::optional<analysis::ConsistencyTarget> group) {
    if (!analysed.anchor_appearance || analysed.attempts.empty()) {
        return fail(ErrorCode::InvalidArgument, "the analysis is not an anchor's (no anchor appearance)",
                    std::string(kWhere));
    }
    try {
        AutoRun run = analysed;
        run.chosen = analysed.start;
        run.confidence = 1.0;  // nothing automatic was decided
        run.confidence_detail = analysis::ConfidenceBreakdown{};
        run.restrained = false;
        run.analysis_fallback = false;
        // Only what describes the photo itself; the independent choice's findings do not apply.
        std::erase_if(run.flags, [](const AutoFlag& flag) {
            return flag.code != static_cast<std::uint16_t>(analysis::ReviewFlagCode::InterpretationLimited);
        });
        run.diagnostics.clear();
        run.locks.prevented_targets = false;
        run.locks.reasons.clear();
        run.consistency.reset();
        if (group) {
            AutoConsistency consistency;
            consistency.target = std::move(*group);
            run.consistency = std::move(consistency);
        }
        auto attempt = first_attempt(run.start);
        if (!attempt) {
            return Unexpected(std::move(attempt.error()));
        }
        attempt->kind = AutoAttemptKind::Anchor;
        attempt->reason =
            run.consistency
                ? std::format("the anchor of related-shot group {}: the photographer's settings are kept and set "
                              "the group's look (not optimised or checked)",
                              run.consistency->target.group_id)
                : std::string("a related-shot anchor: the photographer's settings are kept (not optimised or "
                              "checked)");
        attempt->check.outcome = catalog::AutoCheckOutcome::NotRun;
        run.attempts.assign(1, std::move(*attempt));
        run.effective = 0;
        run.consistency_seconds = 0.0;
        run.check_seconds = 0.0;
        return run;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory during the automatic settings", std::string(kWhere));
    }
}

}  // namespace lucent::app
