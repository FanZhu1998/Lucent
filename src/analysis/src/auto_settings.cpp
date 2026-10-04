#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <new>
#include <numeric>
#include <utility>

#include <lucent/analysis/auto_settings.hpp>
#include <lucent/engine/develop_operations.hpp>
#include <lucent/engine/white_balance_model.hpp>

#include "stage_one_copy.hpp"

namespace lucent::analysis {

namespace {

constexpr const char* kWhere = "analysis.auto";
using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

// Search bounds (blueprint section 6 table).
constexpr double kExposureMin = -1.5;
constexpr double kExposureMax = 1.5;
constexpr double kShadowsMin = 0.0;
constexpr double kShadowsMax = 0.75;
constexpr double kHighlightsMin = -0.75;
constexpr double kHighlightsMax = 0.0;
constexpr double kContrastMin = -0.15;
constexpr double kContrastMax = 0.15;
constexpr double kSaturationMin = 0.85;
constexpr double kSaturationMax = 1.15;
constexpr std::uint32_t kMaxPasses = 4;
constexpr double kInfeasibleTotal = 1e9;

enum class Coordinate : std::uint8_t { Exposure, Shadows, Highlights, WhiteBalance, Contrast, Saturation };
constexpr std::array<Coordinate, 6> kOrder{Coordinate::Exposure,   Coordinate::Shadows,
                                           Coordinate::Highlights, Coordinate::WhiteBalance,
                                           Coordinate::Contrast,   Coordinate::Saturation};
constexpr std::array<double, 6> kInitialSteps{0.375, 0.25, 0.25, 0.5, 0.075, 0.075};

std::string_view coordinate_name(Coordinate c) noexcept {
    switch (c) {
        case Coordinate::Exposure:
            return "exposure";
        case Coordinate::Shadows:
            return "shadows";
        case Coordinate::Highlights:
            return "highlights";
        case Coordinate::WhiteBalance:
            return "white_balance";
        case Coordinate::Contrast:
            return "contrast";
        case Coordinate::Saturation:
            return "saturation";
    }
    return "invalid";
}

double snap(double v) noexcept {
    return std::round(v * 1e6) / 1e6;
}

std::int32_t long_edge(image::Size s) noexcept {
    return std::max(s.width, s.height);
}

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

std::string_view color_name(image::CfaColor c) noexcept {
    switch (c) {
        case image::CfaColor::Red:
            return "red";
        case image::CfaColor::Green:
            return "green";
        case image::CfaColor::Blue:
            return "blue";
        case image::CfaColor::Green2:
            return "green2";
        case image::CfaColor::Cyan:
            return "cyan";
        case image::CfaColor::Magenta:
            return "magenta";
        case image::CfaColor::Yellow:
            return "yellow";
        case image::CfaColor::White:
            return "white";
    }
    return "invalid";
}

Status validate_options(const AutoOptions& o) {
    if (o.analysis_long_edge < 256 || o.analysis_long_edge > 8192 || o.proxy_long_edge < 128 ||
        o.proxy_long_edge > o.analysis_long_edge || o.candidate_budget < 8 || o.candidate_budget > 256 ||
        o.recheck_count < 1 || o.recheck_count > 8 || o.native_crop_size < 32 || o.native_crop_size > 1024) {
        return fail(
            ErrorCode::InvalidArgument,
            std::format(
                "invalid auto options: analysis {} px, proxy {} px, budget {}, recheck {}, crop {} px",
                o.analysis_long_edge, o.proxy_long_edge, o.candidate_budget, o.recheck_count,
                o.native_crop_size),
            kWhere);
    }
    return ok();
}

bool fatal(const Error& e) noexcept {
    return e.code == ErrorCode::Cancelled || e.code == ErrorCode::OutOfMemory;
}

class Controller {
public:
    Controller(const engine::DevelopSettings& current, const AutoOptions& options,
               const core::CancellationToken& cancel, Clock::time_point start)
        : current_(current), options_(options), cancel_(cancel), start_(start) {}

    Expected<AutoResult> run(raw::DecodedRaw&& decoded);

private:
    enum class Resolution : std::uint8_t { Proxy, Analysis };

    [[nodiscard]] bool locked(engine::DevelopControl c) const { return current_.locks.contains(c); }
    [[nodiscard]] bool searchable(Coordinate c) const;
    [[nodiscard]] engine::DevelopSettings make_baseline() const;
    [[nodiscard]] engine::DevelopSettings settings_for(const SearchPoint& p) const;
    [[nodiscard]] EditVector edit_of(const engine::DevelopSettings& s) const;
    [[nodiscard]] SearchPoint normalise(SearchPoint p) const;
    [[nodiscard]] RankKey rank(std::size_t index, Resolution r) const;
    [[nodiscard]] bool budget_left() const { return proxy_evaluations_ < options_.candidate_budget; }

    Status setup(raw::DecodedRaw&& decoded);
    Status render_baseline();
    Status propose_white_balance_first();
    Status propose_tone_after_baseline();
    Expected<CandidateScore> evaluate(Resolution r, std::size_t index, std::vector<MetricResult>* metrics);
    // Adds a candidate and evaluates it on the proxy; an already evaluated setting returns its index
    // without a render. std::nullopt when the budget is used up.
    Expected<std::optional<std::size_t>> add_proxy_candidate(CandidateOrigin origin, std::string note,
                                                             const engine::DevelopSettings& settings,
                                                             std::optional<SearchPoint> point);
    Status search();
    Expected<std::size_t> recheck();
    Status finish(std::size_t chosen);
    void add_flags();
    [[nodiscard]] ConfidenceBreakdown confidence_of(const CandidateRecord& c) const;

    const engine::DevelopSettings& current_;
    const AutoOptions& options_;
    const core::CancellationToken& cancel_;
    Clock::time_point start_;

    AutoResult result_;
    engine::StageOneImage analysis_;
    engine::StageOneImage proxy_;
    std::vector<engine::StageOneImage> crop_images_;
    ImageContext ctx_a_;
    ImageContext ctx_p_;
    bool color_ = true;
    std::optional<engine::WhiteBalanceModel> wb_model_;
    engine::RgbMultipliers baseline_multipliers_{1.0, 1.0, 1.0};
    engine::DevelopSettings baseline_;
    SearchPoint base_point_;
    BaselineFeatures feat_a_;
    BaselineFeatures feat_p_;
    RenderStats stats_a_;
    RenderStats stats_p_;
    std::vector<CropFeatures> crop_features_;
    std::vector<CropMeasurement> crop_base_;
    NaturalV1Context policy_;
    std::uint32_t proxy_evaluations_ = 0;
    std::vector<std::vector<MetricResult>> analysis_metrics_;
};

bool Controller::searchable(Coordinate c) const {
    switch (c) {
        case Coordinate::Exposure:
            return !locked(engine::DevelopControl::Exposure);
        case Coordinate::Shadows:
            return !locked(engine::DevelopControl::Shadows);
        case Coordinate::Highlights:
            return !locked(engine::DevelopControl::Highlights);
        case Coordinate::WhiteBalance:
            return color_ && wb_model_ && !locked(engine::DevelopControl::WhiteBalance) &&
                   policy_.white_balance_evidence &&
                   (policy_.red_target != 1.0 || policy_.blue_target != 1.0);
        case Coordinate::Contrast:
            return !locked(engine::DevelopControl::Contrast);
        case Coordinate::Saturation:
            return color_ && !locked(engine::DevelopControl::Saturation);
    }
    return false;
}

engine::DevelopSettings Controller::make_baseline() const {
    engine::DevelopSettings s = current_;
    if (!locked(engine::DevelopControl::WhiteBalance)) {
        s.white_balance = engine::AsShotWhiteBalance{};
    }
    if (!locked(engine::DevelopControl::Exposure)) {
        s.exposure_ev = 0.0;
    }
    if (!locked(engine::DevelopControl::Shadows)) {
        s.shadows = 0.0;
    }
    if (!locked(engine::DevelopControl::Highlights)) {
        s.highlights = 0.0;
    }
    if (!locked(engine::DevelopControl::Contrast)) {
        s.contrast = 0.0;
    }
    if (!locked(engine::DevelopControl::Saturation)) {
        s.saturation = 0.0;
    }
    return s;
}

SearchPoint Controller::normalise(SearchPoint p) const {
    p.exposure_ev =
        searchable(Coordinate::Exposure) ? snap(std::clamp(p.exposure_ev, kExposureMin, kExposureMax)) : 0.0;
    p.white_balance_strength =
        searchable(Coordinate::WhiteBalance) ? snap(std::clamp(p.white_balance_strength, 0.0, 1.0)) : 0.0;
    p.red_gain = snap(1.0 + p.white_balance_strength * (policy_.red_target - 1.0));
    p.blue_gain = snap(1.0 + p.white_balance_strength * (policy_.blue_target - 1.0));
    p.shadows_ev =
        searchable(Coordinate::Shadows) ? snap(std::clamp(p.shadows_ev, kShadowsMin, kShadowsMax)) : 0.0;
    p.highlights_ev = searchable(Coordinate::Highlights)
                          ? snap(std::clamp(p.highlights_ev, kHighlightsMin, kHighlightsMax))
                          : 0.0;
    p.contrast =
        searchable(Coordinate::Contrast) ? snap(std::clamp(p.contrast, kContrastMin, kContrastMax)) : 0.0;
    p.saturation = searchable(Coordinate::Saturation)
                       ? snap(std::clamp(p.saturation, kSaturationMin, kSaturationMax))
                       : 1.0;
    return p;
}

engine::DevelopSettings Controller::settings_for(const SearchPoint& p) const {
    engine::DevelopSettings s = baseline_;
    if (searchable(Coordinate::Exposure)) {
        s.exposure_ev = p.exposure_ev;
    }
    if (color_ && wb_model_ && !locked(engine::DevelopControl::WhiteBalance)) {
        if (p.red_gain == 1.0 && p.blue_gain == 1.0) {
            s.white_balance = engine::AsShotWhiteBalance{};
        } else {
            const engine::RgbMultipliers& a = wb_model_->as_shot();
            s.white_balance = engine::CameraNeutralWhiteBalance{a[0] * p.red_gain, a[1], a[2] * p.blue_gain};
        }
    }
    if (searchable(Coordinate::Shadows)) {
        s.shadows = p.shadows_ev / engine::kShadowsMaxEv;
    }
    if (searchable(Coordinate::Highlights)) {
        s.highlights = p.highlights_ev / engine::kHighlightsMaxEv;
    }
    if (searchable(Coordinate::Contrast)) {
        s.contrast = p.contrast;
    }
    if (searchable(Coordinate::Saturation)) {
        s.saturation = p.saturation - 1.0;
    }
    return s;
}

EditVector Controller::edit_of(const engine::DevelopSettings& s) const {
    EditVector e;
    e.exposure_ev = s.exposure_ev - baseline_.exposure_ev;
    if (color_ && wb_model_ && !(s.white_balance == baseline_.white_balance)) {
        auto target = wb_model_->target_multipliers(s.white_balance);
        if (target && (*target)[1] > 0.0 && baseline_multipliers_[1] > 0.0) {
            e.red_gain =
                ((*target)[0] / (*target)[1]) / (baseline_multipliers_[0] / baseline_multipliers_[1]);
            e.blue_gain =
                ((*target)[2] / (*target)[1]) / (baseline_multipliers_[2] / baseline_multipliers_[1]);
        } else {
            // Not expressible: counted as the full residual range in both gains.
            e.red_gain = 1.0 + 2.0 * kWhiteBalanceResidualBound;
            e.blue_gain = 1.0 + 2.0 * kWhiteBalanceResidualBound;
        }
    }
    e.shadows_ev = (s.shadows - baseline_.shadows) * engine::kShadowsMaxEv;
    e.highlights_ev = (s.highlights - baseline_.highlights) * engine::kHighlightsMaxEv;
    e.contrast = s.contrast - baseline_.contrast;
    e.saturation = (1.0 + s.saturation) / (1.0 + baseline_.saturation);
    return e;
}

RankKey Controller::rank(std::size_t index, Resolution r) const {
    const CandidateRecord& c = result_.candidates[index];
    const CandidateScore& s = r == Resolution::Proxy ? c.proxy : c.analysis;
    return RankKey{s.evaluated && s.feasible, s.evaluated ? s.terms.total : kInfeasibleTotal, c.edit_size,
                   index};
}

Status Controller::setup(raw::DecodedRaw&& decoded) {
    const raw::RawMetadata& md = decoded.metadata;
    const image::Size upright =
        image::oriented_size(md.geometry.visible_area.size(), md.geometry.orientation);
    const image::Size analysis_size = engine::proxy_size(upright, options_.analysis_long_edge);
    const image::Size proxy_size = engine::proxy_size(upright, options_.proxy_long_edge);

    engine::StageOneRequest request;
    request.kind = engine::StageOneKind::Interactive;
    request.white_balance = engine::AsShotWhiteBalance{};
    request.proxy_long_edge = 0;
    if (auto estimate = engine::estimate_develop_memory(md, request, options_.develop)) {
        result_.memory.stage_one_peak_estimate = estimate->stage_one_peak;
    }

    // 1. Source diagnostics, before stage one consumes the samples.
    auto t = Clock::now();
    const std::array<image::Size, 2> grids{analysis_size, proxy_size};
    auto source = analyze_source(decoded, grids, options_.develop.parallel, cancel_);
    if (!source) {
        return Unexpected(std::move(source.error()));
    }
    result_.source = source->diagnostics;
    result_.timing.source_seconds = seconds_since(t);

    // 2. Stage one at full resolution.
    t = Clock::now();
    auto one = engine::render_stage_one(std::move(decoded), request, options_.develop, cancel_);
    if (!one) {
        return Unexpected(std::move(one.error()));
    }
    result_.timing.stage_one_seconds = seconds_since(t);
    result_.full_size = one->full_size;
    result_.warnings = one->warnings;
    result_.memory.full_resolution_bytes = one->byte_size();
    const bool coverage_ok = one->full_size == upright;
    if (!coverage_ok) {
        result_.warnings.push_back(std::format(
            "stage one is {}x{} but the visible area is {}x{} upright: saturation coverage is not used",
            one->full_size.width, one->full_size.height, upright.width, upright.height));
    }

    // 3. Analysis image and search proxy.
    t = Clock::now();
    const std::int32_t full_edge = long_edge(one->full_size);
    auto analysis = full_edge > options_.analysis_long_edge
                        ? engine::make_proxy(*one, options_.analysis_long_edge, options_.develop, cancel_)
                        : detail::clone_stage_one(*one);
    if (!analysis) {
        return Unexpected(std::move(analysis.error()));
    }
    auto proxy = full_edge > options_.proxy_long_edge
                     ? engine::make_proxy(*one, options_.proxy_long_edge, options_.develop, cancel_)
                     : detail::clone_stage_one(*one);
    if (!proxy) {
        return Unexpected(std::move(proxy.error()));
    }
    analysis_ = std::move(*analysis);
    proxy_ = std::move(*proxy);
    result_.analysis_size = analysis_.pixels.size();
    result_.proxy_size = proxy_.pixels.size();
    result_.timing.proxies_seconds = seconds_since(t);
    color_ = analysis_.pixels.channels() == 3;

    const SensorNoiseModel& noise = result_.source.noise;
    const CoverageMap* coverage_a =
        coverage_ok && source->coverage[0].size == analysis_.pixels.size() ? &source->coverage[0] : nullptr;
    const CoverageMap* coverage_p =
        coverage_ok && source->coverage[1].size == proxy_.pixels.size() ? &source->coverage[1] : nullptr;
    auto ctx_a = make_image_context(analysis_, coverage_a, noise);
    if (!ctx_a) {
        return Unexpected(std::move(ctx_a.error()));
    }
    auto ctx_p = make_image_context(proxy_, coverage_p, noise);
    if (!ctx_p) {
        return Unexpected(std::move(ctx_p.error()));
    }
    ctx_a_ = std::move(*ctx_a);
    ctx_p_ = std::move(*ctx_p);

    // 4. Native crops, then release the full-resolution image.
    t = Clock::now();
    if (options_.native_crops) {
        result_.crops = select_native_crops(analysis_, ctx_a_, one->full_size, options_.native_crop_size);
        for (const NativeCrop& crop : result_.crops) {
            auto image = detail::crop_stage_one(*one, crop.rendered, options_.develop.limits);
            if (!image) {
                return Unexpected(std::move(image.error()));
            }
            result_.memory.crop_bytes += image->byte_size();
            crop_images_.push_back(std::move(*image));
        }
    }
    one->pixels = image::ImageBuffer<float>{};
    result_.timing.crops_seconds = seconds_since(t);

    result_.memory.analysis_bytes = analysis_.byte_size();
    result_.memory.proxy_bytes = proxy_.byte_size();
    const std::uint64_t contexts =
        (ctx_a_.signal.size() + ctx_a_.saturation.size() + ctx_p_.signal.size() + ctx_p_.saturation.size()) *
            sizeof(float) +
        static_cast<std::uint64_t>(result_.analysis_size.area() + result_.proxy_size.area()) * 13u;
    result_.memory.retained_bytes =
        result_.memory.analysis_bytes + result_.memory.proxy_bytes + result_.memory.crop_bytes + contexts;
    result_.memory.estimated_peak_bytes =
        std::max(result_.memory.stage_one_peak_estimate,
                 result_.memory.full_resolution_bytes + result_.memory.retained_bytes);

    if (color_) {
        auto model = engine::WhiteBalanceModel::create(analysis_.metadata);
        if (!model) {
            return Unexpected(std::move(model.error()));
        }
        wb_model_ = std::move(*model);
    }
    return ok();
}

Status Controller::render_baseline() {
    const auto t = Clock::now();
    baseline_ = make_baseline();
    if (color_ && wb_model_) {
        auto multipliers = wb_model_->target_multipliers(baseline_.white_balance);
        if (!multipliers) {
            return Unexpected(std::move(multipliers.error()));
        }
        baseline_multipliers_ = *multipliers;
    }
    const auto render_and_measure = [&](const engine::StageOneImage& image, const ImageContext& ctx,
                                        BaselineFeatures& features) -> Expected<RenderStats> {
        auto rendered = engine::render_stage_two(image, baseline_, options_.develop, cancel_);
        if (!rendered) {
            return Unexpected(std::move(rendered.error()));
        }
        auto stats =
            measure_render(rendered->display, ctx, nullptr, &features, options_.develop.parallel, cancel_);
        if (!stats) {
            return Unexpected(std::move(stats.error()));
        }
        if (stats->nonfinite_samples > 0 || stats->pixels == 0) {
            return fail(ErrorCode::Internal,
                        std::format("the baseline render has {} non-finite samples ({} valid pixels)",
                                    stats->nonfinite_samples, stats->pixels),
                        kWhere);
        }
        return stats;
    };
    auto stats_p = render_and_measure(proxy_, ctx_p_, feat_p_);
    if (!stats_p) {
        return Unexpected(std::move(stats_p.error()));
    }
    stats_p_ = *stats_p;
    auto stats_a = render_and_measure(analysis_, ctx_a_, feat_a_);
    if (!stats_a) {
        return Unexpected(std::move(stats_a.error()));
    }
    stats_a_ = *stats_a;
    crop_features_.resize(crop_images_.size());
    for (std::size_t k = 0; k < crop_images_.size(); ++k) {
        auto rendered = engine::render_stage_two(crop_images_[k], baseline_, options_.develop, cancel_);
        if (!rendered) {
            return Unexpected(std::move(rendered.error()));
        }
        auto measured = measure_crop(rendered->display, result_.crops[k], nullptr, &crop_features_[k]);
        if (!measured) {
            return Unexpected(std::move(measured.error()));
        }
        if (measured->nonfinite_samples > 0) {
            return fail(ErrorCode::Internal, "the baseline render of a native crop has non-finite samples",
                        kWhere);
        }
        crop_base_.push_back(*measured);
        ++result_.timing.crop_renders;
    }
    result_.timing.baseline_seconds = seconds_since(t);
    return ok();
}

Status Controller::propose_white_balance_first() {
    const auto t = Clock::now();
    policy_.color = color_;
    if (color_ && wb_model_ && !locked(engine::DevelopControl::WhiteBalance)) {
        // On the search proxy: its area averaging attenuates demosaicing artefacts at edges.
        const double clip = as_shot_clip_level(proxy_);
        auto wb = propose_white_balance(proxy_, ctx_p_, clip, options_.develop.parallel, cancel_);
        if (!wb) {
            return Unexpected(std::move(wb.error()));
        }
        policy_.white_balance_evidence = wb->evidence;
        policy_.white_balance_confidence = wb->confidence;
        policy_.red_target = wb->red_gain;
        policy_.blue_target = wb->blue_gain;
        if (wb->evidence) {
            // The surfaces the evidence treats as neutral, for the white-balance hard check of every
            // render (the baseline's included, so the masks precede it).
            auto mask_p = neutral_evidence_mask(proxy_, ctx_p_, clip, wb->cast_log_rg, wb->cast_log_bg);
            if (!mask_p) {
                return Unexpected(std::move(mask_p.error()));
            }
            auto mask_a = neutral_evidence_mask(analysis_, ctx_a_, as_shot_clip_level(analysis_),
                                                wb->cast_log_rg, wb->cast_log_bg);
            if (!mask_a) {
                return Unexpected(std::move(mask_a.error()));
            }
            ctx_p_.neutral_evidence = std::move(*mask_p);
            ctx_a_.neutral_evidence = std::move(*mask_a);
            const std::uint64_t masks = ctx_p_.neutral_evidence.size() + ctx_a_.neutral_evidence.size();
            result_.memory.retained_bytes += masks;
            result_.memory.estimated_peak_bytes =
                std::max(result_.memory.estimated_peak_bytes,
                         result_.memory.full_resolution_bytes + result_.memory.retained_bytes);
        }
        result_.white_balance = std::move(*wb);
    }
    result_.timing.proposals_seconds += seconds_since(t);
    return ok();
}

Status Controller::propose_tone_after_baseline() {
    const auto t = Clock::now();
    auto scene = measure_scene(analysis_, ctx_a_, options_.develop.parallel, cancel_);
    if (!scene) {
        return Unexpected(std::move(scene.error()));
    }
    result_.tone = propose_tone(stats_a_, *scene);
    policy_.tone = result_.tone;
    result_.timing.proposals_seconds += seconds_since(t);
    return ok();
}

Expected<CandidateScore> Controller::evaluate(Resolution r, std::size_t index,
                                              std::vector<MetricResult>* metrics) {
    const bool analysis = r == Resolution::Analysis;
    const CandidateRecord& c = result_.candidates[index];
    const bool is_baseline = c.origin == CandidateOrigin::Baseline;
    const RenderStats& base = analysis ? stats_a_ : stats_p_;
    CandidateScore score;
    score.evaluated = true;

    RenderStats stats;
    if (is_baseline) {
        stats = base;
    } else {
        const engine::StageOneImage& image = analysis ? analysis_ : proxy_;
        auto rendered = engine::render_stage_two(image, c.settings, options_.develop, cancel_);
        if (!rendered) {
            if (fatal(rendered.error())) {
                return Unexpected(std::move(rendered.error()));
            }
            score.feasible = false;
            score.terms.total = kInfeasibleTotal;
            score.failures.push_back("render: " + rendered.error().describe());
            return score;
        }
        auto measured =
            measure_render(rendered->display, analysis ? ctx_a_ : ctx_p_, analysis ? &feat_a_ : &feat_p_,
                           nullptr, options_.develop.parallel, cancel_);
        if (!measured) {
            return Unexpected(std::move(measured.error()));
        }
        stats = *measured;
    }

    std::optional<NativeComparison> native;
    if (analysis && !crop_images_.empty()) {
        std::vector<CropMeasurement> measured;
        if (is_baseline) {
            measured = crop_base_;
        } else {
            for (std::size_t k = 0; k < crop_images_.size(); ++k) {
                auto rendered =
                    engine::render_stage_two(crop_images_[k], c.settings, options_.develop, cancel_);
                ++result_.timing.crop_renders;
                if (!rendered) {
                    if (fatal(rendered.error())) {
                        return Unexpected(std::move(rendered.error()));
                    }
                    score.feasible = false;
                    score.terms.total = kInfeasibleTotal;
                    score.failures.push_back("native crop render: " + rendered.error().describe());
                    return score;
                }
                auto m = measure_crop(rendered->display, result_.crops[k], &crop_features_[k], nullptr);
                if (!m) {
                    return Unexpected(std::move(m.error()));
                }
                measured.push_back(*m);
            }
        }
        native = compare_native(measured, crop_base_);
    }
    const NativeComparison* native_ptr = native ? &*native : nullptr;
    CandidateEvaluation evaluation = evaluate_candidate(stats, base, c.edit, policy_, native_ptr, is_baseline,
                                                        curves_monotone(c.settings));
    score.feasible = evaluation.feasible;
    score.failures = std::move(evaluation.failures);
    score.terms = natural_v1_objective(stats, base, c.edit, policy_, native_ptr);
    score.median_lightness = stats.quantiles.p50;
    score.new_clip_fraction = stats.fraction(stats.new_highlight_clipped);
    if (metrics != nullptr) {
        *metrics = std::move(evaluation.metrics);
    }
    return score;
}

Expected<std::optional<std::size_t>> Controller::add_proxy_candidate(CandidateOrigin origin, std::string note,
                                                                     const engine::DevelopSettings& settings,
                                                                     std::optional<SearchPoint> point) {
    for (std::size_t i = 0; i < result_.candidates.size(); ++i) {
        if (result_.candidates[i].settings == settings) {
            return std::optional<std::size_t>{i};
        }
    }
    if (!budget_left()) {
        return std::optional<std::size_t>{};
    }
    CandidateRecord c;
    c.index = static_cast<std::uint32_t>(result_.candidates.size());
    c.origin = origin;
    c.note = std::move(note);
    c.settings = settings;
    c.edit = edit_of(settings);
    c.edit_size = edit_size(c.edit);
    c.point = point;
    result_.candidates.push_back(std::move(c));
    analysis_metrics_.emplace_back();
    const std::size_t index = result_.candidates.size() - 1;
    auto score = evaluate(Resolution::Proxy, index, nullptr);
    if (!score) {
        return Unexpected(std::move(score.error()));
    }
    result_.candidates[index].proxy = std::move(*score);
    ++proxy_evaluations_;
    ++result_.timing.proxy_candidates;
    return std::optional<std::size_t>{index};
}

Status Controller::search() {
    const auto t = Clock::now();
    base_point_ = normalise(SearchPoint{});

    // Seeds: the baseline, the identity, the proposals.
    auto added = add_proxy_candidate(CandidateOrigin::Baseline, "as shot", baseline_, base_point_);
    if (!added) {
        return Unexpected(std::move(added.error()));
    }
    if (!(current_ == baseline_)) {
        std::optional<SearchPoint> point;  // the current settings are generally outside the search space
        added = add_proxy_candidate(CandidateOrigin::Identity, "current settings", current_, point);
        if (!added) {
            return Unexpected(std::move(added.error()));
        }
    }
    const ToneProposal& tone = result_.tone;
    SearchPoint exposure = base_point_;
    exposure.exposure_ev = tone.exposure_ev;
    exposure = normalise(exposure);
    SearchPoint white = base_point_;
    white.white_balance_strength = 1.0;
    white = normalise(white);
    SearchPoint global = base_point_;
    global.exposure_ev = tone.exposure_ev;
    global.white_balance_strength = 1.0;
    global = normalise(global);
    SearchPoint combined = global;
    combined.shadows_ev = tone.shadows_ev;
    combined.highlights_ev = tone.highlights_ev;
    combined = normalise(combined);
    const std::array<std::pair<CandidateOrigin, SearchPoint>, 4> seeds{{
        {CandidateOrigin::ExposureProposal, exposure},
        {CandidateOrigin::WhiteBalanceProposal, white},
        {CandidateOrigin::GlobalProposal, global},
        {CandidateOrigin::CombinedProposal, combined},
    }};
    for (const auto& [origin, point] : seeds) {
        if (point == base_point_) {
            continue;
        }
        added = add_proxy_candidate(origin, std::string(to_string(origin)), settings_for(point), point);
        if (!added) {
            return Unexpected(std::move(added.error()));
        }
    }

    // Coordinate search from the best point.
    const auto best_with_point = [&]() {
        std::size_t best = 0;  // the baseline
        for (std::size_t i = 1; i < result_.candidates.size(); ++i) {
            if (result_.candidates[i].point &&
                better(rank(i, Resolution::Proxy), rank(best, Resolution::Proxy))) {
                best = i;
            }
        }
        return best;
    };
    std::size_t best = best_with_point();
    std::array<double, 6> steps = kInitialSteps;
    for (std::uint32_t pass = 0; pass < kMaxPasses && budget_left(); ++pass) {
        for (std::size_t k = 0; k < kOrder.size() && budget_left(); ++k) {
            const Coordinate coordinate = kOrder[k];
            if (!searchable(coordinate)) {
                continue;
            }
            const SearchPoint origin = *result_.candidates[best].point;
            for (const double direction : {1.0, -1.0}) {
                if (!budget_left()) {
                    break;
                }
                SearchPoint p = origin;
                const double step = direction * steps[k];
                switch (coordinate) {
                    case Coordinate::Exposure:
                        p.exposure_ev += step;
                        break;
                    case Coordinate::Shadows:
                        p.shadows_ev += step;
                        break;
                    case Coordinate::Highlights:
                        p.highlights_ev += step;
                        break;
                    case Coordinate::WhiteBalance:
                        p.white_balance_strength += step;
                        break;
                    case Coordinate::Contrast:
                        p.contrast += step;
                        break;
                    case Coordinate::Saturation:
                        p.saturation += step;
                        break;
                }
                p = normalise(p);
                if (p == origin) {
                    continue;
                }
                added = add_proxy_candidate(
                    CandidateOrigin::CoordinateStep,
                    std::format("pass {} {} {:+.4g}", pass + 1, coordinate_name(coordinate), step),
                    settings_for(p), p);
                if (!added) {
                    return Unexpected(std::move(added.error()));
                }
                if (*added && result_.candidates[**added].point &&
                    better(rank(**added, Resolution::Proxy), rank(best, Resolution::Proxy))) {
                    best = **added;
                }
            }
        }
        for (double& s : steps) {
            s *= 0.5;
        }
    }
    result_.timing.search_seconds = seconds_since(t);
    return ok();
}

Expected<std::size_t> Controller::recheck() {
    const auto t = Clock::now();
    std::vector<std::size_t> order(result_.candidates.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const RankKey ka = rank(a, Resolution::Proxy);
        const RankKey kb = rank(b, Resolution::Proxy);
        if (ka.feasible != kb.feasible) {
            return ka.feasible;
        }
        if (ka.total != kb.total) {
            return ka.total < kb.total;
        }
        if (ka.edit_size != kb.edit_size) {
            return ka.edit_size < kb.edit_size;
        }
        return a < b;
    });
    std::vector<std::size_t> pool;
    for (const std::size_t i : order) {
        if (pool.size() >= options_.recheck_count) {
            break;
        }
        if (rank(i, Resolution::Proxy).feasible) {
            pool.push_back(i);
        }
    }
    if (std::find(pool.begin(), pool.end(), std::size_t{0}) == pool.end()) {
        pool.push_back(0);  // the baseline is always a fallback
    }
    for (const std::size_t i : pool) {
        auto score = evaluate(Resolution::Analysis, i, &analysis_metrics_[i]);
        if (!score) {
            return Unexpected(std::move(score.error()));
        }
        result_.candidates[i].analysis = std::move(*score);
        ++result_.timing.analysis_candidates;
    }
    std::size_t chosen = 0;
    for (const std::size_t i : pool) {
        if (better(rank(i, Resolution::Analysis), rank(chosen, Resolution::Analysis))) {
            chosen = i;
        }
    }
    result_.timing.recheck_seconds = seconds_since(t);
    return chosen;
}

ConfidenceBreakdown Controller::confidence_of(const CandidateRecord& c) const {
    ConfidenceBreakdown b;
    const EditVector& e = c.edit;
    // Source interpretation.
    if (analysis_.white_balance.applied.source == engine::WhiteBalanceSource::Unavailable) {
        b.source = std::min(b.source, 0.6);
        b.reasons.emplace_back("the file has no usable white balance");
    }
    if (analysis_.metadata.interpretation.status != raw::InterpretationStatus::Complete) {
        b.source = std::min(b.source, 0.7);
        b.reasons.emplace_back("the decoder's interpretation of the file is incomplete");
    }
    if (result_.source.max_channel_fraction() > 0.05) {
        b.source = std::min(b.source, 0.85);
        b.reasons.emplace_back(std::format("{:.1f} % of a sensor channel is saturated",
                                           100.0 * result_.source.max_channel_fraction()));
    }
    if (std::abs(e.exposure_ev) > 0.05 || std::abs(e.shadows_ev) > 0.05) {
        b.brightness = result_.tone.evidence ? result_.tone.confidence : 0.0;
        if (b.brightness < 1.0) {
            b.reasons.push_back(
                std::format("brightness evidence {:.2f}: {}", b.brightness, result_.tone.reason));
        }
    }
    if (changes_white_balance_materially(e)) {
        b.white_balance = policy_.white_balance_evidence ? policy_.white_balance_confidence : 0.0;
        if (b.white_balance < 1.0) {
            b.reasons.push_back(std::format("white-balance evidence {:.2f}", b.white_balance));
        }
    }
    if (e.shadows_ev > 0.05 || e.exposure_ev > 0.25) {
        b.noise = result_.source.noise.available() ? std::max(0.3, result_.source.noise.confidence) : 0.3;
        if (b.noise < 1.0) {
            b.reasons.push_back(
                std::format("noise model {} ({:.2f})", to_string(result_.source.noise.source), b.noise));
        }
    }
    if (crop_images_.empty() && (changes_local_tone(e) || std::abs(e.exposure_ev) > 0.05)) {
        b.native = 0.8;
        b.reasons.emplace_back("no native-resolution crops were checked");
    }
    b.overall = std::min({b.source, b.brightness, b.white_balance, b.noise, b.native});
    return b;
}

Status Controller::finish(std::size_t chosen) {
    // Confidence and restraint.
    ConfidenceBreakdown confidence = confidence_of(result_.candidates[chosen]);
    const CandidateRecord winner = result_.candidates[chosen];
    // Each group of controls is restrained by the evidence that governs it: tone and colour by the
    // source, brightness, noise and native-crop confidences; white balance by the source and the
    // white-balance evidence.
    const auto factor = [](double c) {
        return c < kRestrainConfidence ? std::clamp(c / kRestrainConfidence, 0.25, 1.0) : 1.0;
    };
    const double f_tone =
        factor(std::min({confidence.source, confidence.brightness, confidence.noise, confidence.native}));
    const double f_wb = factor(std::min(confidence.source, confidence.white_balance));
    if ((f_tone < 1.0 || f_wb < 1.0) && winner.point && winner.origin != CandidateOrigin::Baseline) {
        const SearchPoint& w = *winner.point;
        SearchPoint p = base_point_;
        p.exposure_ev = f_tone * w.exposure_ev;
        p.white_balance_strength = f_wb * w.white_balance_strength;
        p.shadows_ev = f_tone * w.shadows_ev;
        p.highlights_ev = f_tone * w.highlights_ev;
        p.contrast = f_tone * w.contrast;
        p.saturation = 1.0 + f_tone * (w.saturation - 1.0);
        p = normalise(p);
        const engine::DevelopSettings settings = settings_for(p);
        result_.restrained = !(settings == winner.settings);
        std::size_t restrained = result_.candidates.size();
        for (std::size_t i = 0; i < result_.candidates.size(); ++i) {
            if (result_.candidates[i].settings == settings) {
                restrained = i;
            }
        }
        if (restrained == result_.candidates.size()) {
            CandidateRecord c;
            c.index = static_cast<std::uint32_t>(restrained);
            c.origin = CandidateOrigin::Restrained;
            c.note = std::format("winner {} restrained: tone x{:.2f}, white balance x{:.2f}", winner.index,
                                 f_tone, f_wb);
            c.settings = settings;
            c.edit = edit_of(settings);
            c.edit_size = edit_size(c.edit);
            c.point = p;
            result_.candidates.push_back(std::move(c));
            analysis_metrics_.emplace_back();
        }
        if (!result_.candidates[restrained].analysis.evaluated) {
            auto score = evaluate(Resolution::Analysis, restrained, &analysis_metrics_[restrained]);
            if (!score) {
                return Unexpected(std::move(score.error()));
            }
            result_.candidates[restrained].analysis = std::move(*score);
            ++result_.timing.analysis_candidates;
        }
        // The reported confidence stays the winner's: it is why the edit was restrained.
        chosen = result_.candidates[restrained].analysis.feasible ? restrained : 0;
    }
    result_.chosen = chosen;
    result_.settings = result_.candidates[chosen].settings;
    result_.confidence = confidence.overall;
    result_.confidence_detail = confidence;

    // Diagnostics: source saturation, then the chosen candidate's metrics at analysis resolution.
    MetricResult source;
    source.id = MetricId::SourceSaturation;
    source.valid_samples = result_.source.sites;
    source.status = result_.source.sites == 0 ? MetricStatus::InsufficientEvidence : MetricStatus::Pass;
    for (const ChannelSaturation& ch : result_.source.channels) {
        source.values.push_back({std::format("{}_fraction", color_name(ch.color)), ch.fraction()});
    }
    source.values.push_back({"max_channel_fraction", result_.source.max_channel_fraction()});
    source.values.push_back({"saturated_sites", static_cast<double>(result_.source.saturated_sites)});
    source.values.push_back({"sites", static_cast<double>(result_.source.sites)});
    source.values.push_back({"analysis_fraction", stats_a_.fraction(stats_a_.source_saturated)});
    source.values.push_back({"noise_read_sigma", std::sqrt(result_.source.noise.read_variance)});
    source.values.push_back({"noise_shot_scale", result_.source.noise.shot_scale});
    source.values.push_back({"noise_confidence", result_.source.noise.confidence});
    if (result_.source.max_channel_fraction() > kSourceSaturationFlagFraction) {
        source.detail = "acquisition limitation: sensor channels saturated before any edit";
    }
    result_.diagnostics.push_back(std::move(source));
    for (MetricResult& m : analysis_metrics_[chosen]) {
        result_.diagnostics.push_back(m);
    }
    add_flags();
    return ok();
}

void Controller::add_flags() {
    auto& flags = result_.flags;
    if (result_.restrained || result_.confidence < kRestrainConfidence) {
        std::string reasons;
        for (const std::string& r : result_.confidence_detail.reasons) {
            reasons += reasons.empty() ? r : "; " + r;
        }
        flags.push_back(
            {ReviewFlagCode::LowConfidence, std::nullopt,
             std::format("confidence {:.2f}{}{}", result_.confidence, reasons.empty() ? "" : ": ", reasons)});
    }
    for (const MetricResult& m : result_.diagnostics) {
        if (m.status == MetricStatus::InsufficientEvidence) {
            flags.push_back({ReviewFlagCode::InsufficientEvidence, m.id, m.detail});
        }
    }
    if (result_.source.max_channel_fraction() > kSourceSaturationFlagFraction) {
        std::string channels;
        for (const ChannelSaturation& ch : result_.source.channels) {
            channels += std::format("{}{} {:.2f} %", channels.empty() ? "" : ", ", color_name(ch.color),
                                    100.0 * ch.fraction());
        }
        flags.push_back({ReviewFlagCode::SourceSaturation, MetricId::SourceSaturation,
                         "sensor saturation before any edit (acquisition limitation): " + channels});
    }
    bool searched_locks = false;
    for (const engine::DevelopControl control :
         {engine::DevelopControl::WhiteBalance, engine::DevelopControl::Exposure,
          engine::DevelopControl::Contrast, engine::DevelopControl::Highlights,
          engine::DevelopControl::Shadows, engine::DevelopControl::Saturation}) {
        searched_locks = searched_locks || locked(control);
    }
    const double base_clip = stats_a_.fraction(stats_a_.highlight_clipped);
    const double source_clip = stats_a_.fraction(stats_a_.source_saturated);
    if (searched_locks && base_clip > source_clip + 0.01) {
        flags.push_back({ReviewFlagCode::LockedControlsClip, MetricId::RenderClipping,
                         std::format("the locked settings clip {:.2f} % of pixels ({:.2f} % from the sensor)",
                                     100.0 * base_clip, 100.0 * source_clip)});
    }
    if (color_ && !locked(engine::DevelopControl::WhiteBalance) && result_.white_balance) {
        const WhiteBalanceProposal& wb = *result_.white_balance;
        EditVector proposed;
        proposed.red_gain = wb.red_gain;
        proposed.blue_gain = wb.blue_gain;
        const EditVector& chosen_edit = result_.candidates[result_.chosen].edit;
        if (!wb.evidence || wb.confidence < 0.5) {
            flags.push_back(
                {ReviewFlagCode::WhiteBalanceUncertain, MetricId::WhiteBalance,
                 wb.evidence ? std::format("weak neutral evidence (confidence {:.2f}): white balance kept "
                                           "near as shot",
                                           wb.confidence)
                             : wb.reason});
        } else if (changes_white_balance_materially(proposed) &&
                   !changes_white_balance_materially(chosen_edit)) {
            flags.push_back(
                {ReviewFlagCode::WhiteBalanceUncertain, MetricId::WhiteBalance,
                 std::format("the proposed residual (r {:.4f}, b {:.4f}) did not pass the checks: white "
                             "balance kept as shot",
                             wb.red_gain, wb.blue_gain)});
        }
    }
    if (!result_.source.noise.available()) {
        flags.push_back(
            {ReviewFlagCode::NoiseModelUnavailable, MetricId::ShadowNoise, result_.source.noise.basis});
    }
    for (const MetricResult& m : result_.diagnostics) {
        if (m.id != MetricId::ShadowNoise || m.status != MetricStatus::Pass) {
            continue;
        }
        const double sigma = m.value("predicted_sigma_lstar").value_or(0.0);
        const double base = m.value("baseline_sigma_lstar").value_or(0.0);
        if (sigma > kVisibleNoiseLStar && sigma > base + 0.25) {
            flags.push_back({ReviewFlagCode::VisibleShadowNoise, MetricId::ShadowNoise,
                             std::format("predicted shadow noise {:.2f} L* (baseline {:.2f})", sigma, base)});
        }
    }
    if (crop_images_.empty()) {
        flags.push_back(
            {ReviewFlagCode::NativeChecksUnavailable, MetricId::NativeDetail,
             options_.native_crops ? "the image has no native-resolution crop" : "native crops disabled"});
    }
    bool any_feasible = false;
    for (const CandidateRecord& r : result_.candidates) {
        any_feasible =
            any_feasible || (r.origin != CandidateOrigin::Baseline && r.proxy.evaluated && r.proxy.feasible);
    }
    if (!any_feasible && result_.candidates.size() > 1) {
        flags.push_back({ReviewFlagCode::NoFeasibleCandidate, std::nullopt,
                         "no candidate other than the baseline passed the hard checks"});
    }
    bool rechecked = false;
    bool recheck_feasible = false;
    for (const CandidateRecord& r : result_.candidates) {
        if (r.origin != CandidateOrigin::Baseline && r.analysis.evaluated) {
            rechecked = true;
            recheck_feasible = recheck_feasible || r.analysis.feasible;
        }
    }
    if (rechecked && !recheck_feasible) {
        flags.push_back({ReviewFlagCode::RecheckRejected, std::nullopt,
                         "every rechecked candidate failed a hard check at analysis resolution"});
    }
    if (!result_.warnings.empty()) {
        std::string text;
        for (const std::string& w : result_.warnings) {
            text += text.empty() ? w : "; " + w;
        }
        flags.push_back({ReviewFlagCode::InterpretationLimited, std::nullopt, text});
    }
}

Expected<AutoResult> Controller::run(raw::DecodedRaw&& decoded) {
    if (auto valid = validate_options(options_); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    if (auto valid = current_.validate(); !valid) {
        return fail(ErrorCode::InvalidArgument, "the current settings are invalid: " + valid.error().message,
                    kWhere);
    }
    if (auto status = setup(std::move(decoded)); !status) {
        return Unexpected(std::move(status.error()));
    }
    if (auto status = propose_white_balance_first(); !status) {
        return Unexpected(std::move(status.error()));
    }
    if (auto status = render_baseline(); !status) {
        return Unexpected(std::move(status.error()));
    }
    if (auto status = propose_tone_after_baseline(); !status) {
        return Unexpected(std::move(status.error()));
    }
    if (auto status = search(); !status) {
        return Unexpected(std::move(status.error()));
    }
    auto chosen = recheck();
    if (!chosen) {
        return Unexpected(std::move(chosen.error()));
    }
    if (auto status = finish(*chosen); !status) {
        return Unexpected(std::move(status.error()));
    }
    result_.timing.total_seconds = seconds_since(start_);
    return std::move(result_);
}

}  // namespace

std::string_view to_string(CandidateOrigin origin) noexcept {
    switch (origin) {
        case CandidateOrigin::Baseline:
            return "baseline";
        case CandidateOrigin::Identity:
            return "identity";
        case CandidateOrigin::ExposureProposal:
            return "exposure_proposal";
        case CandidateOrigin::WhiteBalanceProposal:
            return "white_balance_proposal";
        case CandidateOrigin::CombinedProposal:
            return "combined_proposal";
        case CandidateOrigin::CoordinateStep:
            return "coordinate_step";
        case CandidateOrigin::Restrained:
            return "restrained";
        case CandidateOrigin::GlobalProposal:
            return "global_proposal";
    }
    return "invalid";
}

std::string_view to_string(ReviewFlagCode code) noexcept {
    switch (code) {
        case ReviewFlagCode::LowConfidence:
            return "low_confidence";
        case ReviewFlagCode::InsufficientEvidence:
            return "insufficient_evidence";
        case ReviewFlagCode::SourceSaturation:
            return "source_saturation";
        case ReviewFlagCode::LockedControlsClip:
            return "locked_controls_clip";
        case ReviewFlagCode::WhiteBalanceUncertain:
            return "white_balance_uncertain";
        case ReviewFlagCode::NoiseModelUnavailable:
            return "noise_model_unavailable";
        case ReviewFlagCode::VisibleShadowNoise:
            return "visible_shadow_noise";
        case ReviewFlagCode::NativeChecksUnavailable:
            return "native_checks_unavailable";
        case ReviewFlagCode::NoFeasibleCandidate:
            return "no_feasible_candidate";
        case ReviewFlagCode::InterpretationLimited:
            return "interpretation_limited";
        case ReviewFlagCode::RecheckRejected:
            return "recheck_rejected";
    }
    return "invalid";
}

bool AutoResult::has_flag(ReviewFlagCode code) const noexcept {
    return std::any_of(flags.begin(), flags.end(), [code](const ReviewFlag& f) { return f.code == code; });
}

const MetricResult* AutoResult::diagnostic(MetricId id) const noexcept {
    for (const MetricResult& m : diagnostics) {
        if (m.id == id) {
            return &m;
        }
    }
    return nullptr;
}

Expected<AutoResult> auto_develop(raw::DecodedRaw&& decoded, const engine::DevelopSettings& current,
                                  const AutoOptions& options, const core::CancellationToken& cancel) {
    try {
        Controller controller(current, options, cancel, Clock::now());
        return controller.run(std::move(decoded));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory during the automatic settings", kWhere);
    }
}

Expected<AutoResult> auto_develop_file(const raw::DecoderRegistry& registry,
                                       const std::filesystem::path& path,
                                       std::optional<std::uint32_t> subimage,
                                       const engine::DevelopSettings& current, const AutoOptions& options,
                                       const core::CancellationToken& cancel) {
    const auto start = Clock::now();
    if (auto valid = validate_options(options); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    raw::ProbeOptions probe_options;
    probe_options.subimage = subimage;
    probe_options.limits = options.develop.decode_limits;
    probe_options.describe_subimages = false;
    auto selection = registry.select(path, probe_options, cancel);
    if (!selection) {
        return Unexpected(std::move(selection.error()));
    }
    engine::StageOneRequest request;
    request.kind = engine::StageOneKind::Interactive;
    auto estimate = engine::estimate_develop_memory(selection->probe.metadata, request, options.develop);
    if (!estimate) {
        return Unexpected(std::move(estimate.error()));
    }
    if (estimate->stage_one_peak > options.develop.memory_budget_bytes) {
        return fail(
            ErrorCode::LimitExceeded,
            std::format("automatic settings need about {} MiB of pixel memory, above the budget of {} MiB",
                        estimate->stage_one_peak >> 20, options.develop.memory_budget_bytes >> 20),
            kWhere);
    }
    raw::DecodeOptions decode_options;
    decode_options.subimage = subimage;
    decode_options.limits = options.develop.decode_limits;
    const auto t = Clock::now();
    auto decoded = selection->decoder->decode(path, decode_options, cancel);
    if (!decoded) {
        return Unexpected(std::move(decoded.error()));
    }
    const double decode_seconds = seconds_since(t);
    try {
        Controller controller(current, options, cancel, start);
        auto result = controller.run(std::move(*decoded));
        if (result) {
            result->timing.decode_seconds = decode_seconds;
        }
        return result;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory during the automatic settings", kWhere);
    }
}

Expected<std::uint64_t> estimate_auto_memory(const raw::RawMetadata& metadata, const AutoOptions& options) {
    engine::StageOneRequest request;
    request.kind = engine::StageOneKind::Interactive;
    auto estimate = engine::estimate_develop_memory(metadata, request, options.develop);
    if (!estimate) {
        return Unexpected(std::move(estimate.error()));
    }
    const image::Size upright =
        image::oriented_size(metadata.geometry.visible_area.size(), metadata.geometry.orientation);
    const std::uint64_t channels = metadata.representation.kind == raw::RawRepresentation::Monochrome ? 1 : 3;
    const auto plane = [&](image::Size s) {
        return static_cast<std::uint64_t>(std::max<std::int64_t>(0, s.area())) * channels * sizeof(float);
    };
    const image::Size analysis = engine::proxy_size(upright, options.analysis_long_edge);
    const image::Size proxy = engine::proxy_size(upright, options.proxy_long_edge);
    const std::int64_t side = std::min<std::int64_t>(options.native_crop_size + 2 * native_crop_halo(upright),
                                                     std::min(upright.width, upright.height));
    const std::uint64_t crops =
        options.native_crops ? 4u * static_cast<std::uint64_t>(side * side) * channels * 4u : 0u;
    // Kept besides the full image: both proxies, crops, contexts (two floats) and baseline features
    // (13 bytes) per analysis and proxy pixel.
    const auto per_pixel = static_cast<std::uint64_t>(analysis.area() + proxy.area()) * 21u;
    const std::uint64_t kept = plane(analysis) + plane(proxy) + crops + per_pixel;
    return std::max(estimate->stage_one_peak, estimate->stage_one_bytes + kept);
}

}  // namespace lucent::analysis
