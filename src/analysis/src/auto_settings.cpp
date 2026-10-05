#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <execution>
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
constexpr std::uint32_t kConsistencyPasses = 2;
constexpr double kInfeasibleTotal = 1e9;
// Log-gain step of the colour probes of the consistency stage.
constexpr double kColourProbeStep = 0.02;

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

double clamp_gain(double g) noexcept {
    return std::clamp(g, 1.0 - kWhiteBalanceResidualBound, 1.0 + kWhiteBalanceResidualBound);
}

std::int32_t long_edge(image::Size s) noexcept {
    return std::max(s.width, s.height);
}

bool has_local_tone(const engine::DevelopSettings& s) noexcept {
    return s.highlights != 0.0 || s.shadows != 0.0;
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
        o.recheck_count < 1 || o.recheck_count > 8 || o.native_crop_size < 32 || o.native_crop_size > 1024 ||
        o.consistency_budget < 4 || o.consistency_budget > 64) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("invalid auto options: analysis {} px, proxy {} px, budget {}, recheck {}, crop "
                                "{} px, consistency budget {}",
                                o.analysis_long_edge, o.proxy_long_edge, o.candidate_budget, o.recheck_count,
                                o.native_crop_size, o.consistency_budget),
                    kWhere);
    }
    return ok();
}

Status validate_target(const ConsistencyTarget& t) {
    const bool finite = std::isfinite(t.weight) && std::isfinite(t.lightness) && std::isfinite(t.u) &&
                        std::isfinite(t.v) && std::isfinite(t.log_red) && std::isfinite(t.log_blue) &&
                        std::isfinite(t.white_balance_confidence);
    if (!finite || t.weight < 0.0 || t.weight > 10.0) {
        return fail(ErrorCode::InvalidArgument, "invalid consistency target: a value is not finite or the weight "
                                                "is outside [0, 10]",
                    kWhere);
    }
    return ok();
}

bool fatal(const Error& e) noexcept {
    return e.code == ErrorCode::Cancelled || e.code == ErrorCode::OutOfMemory;
}

}  // namespace

// ---- The controller ----------------------------------------------------------------------------

class AutoSession::Impl {
public:
    // The evaluation uses `current` without its geometry and local adjustments (auto_settings.hpp,
    // "Global controls only"); results carry them unchanged.
    Impl(const engine::DevelopSettings& current, const AutoOptions& options, Clock::time_point start)
        : current_(evaluation_settings(current)), user_(current), options_(options), start_(start) {}

    // The independent analysis (steps 1-8).
    Status open(raw::DecodedRaw&& decoded, const core::CancellationToken& cancel);
    Expected<AutoResult> regularise(const ConsistencyTarget& target, const core::CancellationToken& cancel);
    Expected<Appearance> appearance_of(const engine::DevelopSettings& settings,
                                       const core::CancellationToken& cancel);
    Expected<engine::ColorImage> preview(const engine::DevelopSettings& settings, std::int32_t edge,
                                         const core::CancellationToken& cancel);

    // The independent result as callers see it (carrying the current geometry and masks).
    [[nodiscard]] const AutoResult& independent() const noexcept { return independent_out_; }
    [[nodiscard]] AutoResult take_independent() noexcept { return std::move(independent_out_); }
    void set_decode_seconds(double seconds) noexcept {
        independent_.timing.decode_seconds = seconds;
        independent_out_.timing.decode_seconds = seconds;
    }
    [[nodiscard]] std::uint64_t retained_bytes() const noexcept { return independent_.memory.retained_bytes; }

private:
    enum class Resolution : std::uint8_t { Proxy, Analysis };

    // The white-balance axis of the current stage: gains origin + s (target - origin).
    struct WhiteBalanceAxis {
        double origin_red = 1.0;
        double origin_blue = 1.0;
        double target_red = 1.0;
        double target_blue = 1.0;
        bool active = false;
    };

    [[nodiscard]] const core::CancellationToken& cancel() const noexcept { return *cancel_; }
    [[nodiscard]] bool locked(engine::DevelopControl c) const { return current_.locks.contains(c); }
    [[nodiscard]] bool white_balance_free() const {
        return color_ && wb_model_.has_value() && !locked(engine::DevelopControl::WhiteBalance);
    }
    [[nodiscard]] bool searchable(Coordinate c) const;
    [[nodiscard]] engine::DevelopSettings make_baseline() const;
    [[nodiscard]] engine::DevelopSettings settings_for(const SearchPoint& p) const;
    [[nodiscard]] EditVector edit_of(const engine::DevelopSettings& s) const;
    [[nodiscard]] SearchPoint normalise(SearchPoint p) const;
    [[nodiscard]] RankKey rank(std::size_t index, Resolution r) const;
    [[nodiscard]] bool budget_left() const { return stage_evaluations_ < stage_budget_; }
    [[nodiscard]] Appearance appearance_from(const RenderStats& stats, const engine::DevelopSettings& s) const;
    void rescore(CandidateScore& score) const;

    // One native crop's render and measurement for some settings.
    struct CropOutcome {
        std::optional<Error> render_error;
        Expected<CropMeasurement> measurement = CropMeasurement{};
    };
    // Every crop of `s` (the baseline's: `baseline`, filling crop_features_), in crop order.
    std::vector<CropOutcome> measure_crops(const engine::DevelopSettings& s, bool baseline);

    Status setup(raw::DecodedRaw&& decoded);
    Status render_baseline();
    Status propose_white_balance_first();
    Status propose_tone_after_baseline();
    Expected<CandidateScore> evaluate(Resolution r, std::size_t index, std::vector<MetricResult>* metrics);
    // Adds a candidate and evaluates it on the proxy; an already evaluated setting returns its index
    // without a render. std::nullopt when the budget is used up. A new candidate with a point is a
    // starting point of the stage's coordinate search unless `stage_point` is false (probes).
    Expected<std::optional<std::size_t>> add_proxy_candidate(CandidateOrigin origin, std::string note,
                                                             const engine::DevelopSettings& settings,
                                                             std::optional<SearchPoint> point,
                                                             bool stage_point = true);
    // The coordinate search from `best` (steps `steps`, `passes` passes) over the stage's points.
    Status coordinate_search(std::size_t best, std::array<double, 6> steps, std::uint32_t passes,
                             CandidateOrigin origin, std::string_view label);
    Status search();
    Status consistency_search();
    Expected<std::size_t> recheck(std::span<const std::size_t> always);
    Status finish(std::size_t chosen);
    void add_flags();
    [[nodiscard]] ConfidenceBreakdown confidence_of(const CandidateRecord& c) const;
    // select_best() over the candidates `indices` at resolution `r` (candidates.size() when empty).
    [[nodiscard]] std::size_t select(std::span<const std::size_t> indices, Resolution r) const;
    [[nodiscard]] std::size_t best_stage_point() const;
    // Adds (or finds) the fallback candidate of `settings` derived from `source` and returns its
    // index; candidates.size() when `source` has no search point to derive one from.
    std::size_t add_fallback(std::size_t source, const engine::DevelopSettings& settings);
    // `result` with the current geometry and local adjustments in every settings value (internal
    // results never have them).
    [[nodiscard]] AutoResult with_user_parts(AutoResult result) const;

    engine::DevelopSettings current_;  // evaluation_settings() of the current settings
    engine::DevelopSettings user_;     // the current settings as given
    AutoOptions options_;
    Clock::time_point start_;
    const core::CancellationToken* cancel_ = nullptr;

    AutoResult result_;
    engine::StageOneImage analysis_;
    engine::StageOneImage proxy_;
    std::vector<engine::StageOneImage> crop_images_;       // with the local-tone halo
    std::vector<engine::StageOneImage> crop_halo_free_;    // NativeCrop::measured
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
    WhiteBalanceAxis axis_;
    bool fallback_ = false;  // the last recheck fell back (step 6)
    std::uint32_t stage_budget_ = 0;
    std::uint32_t stage_evaluations_ = 0;
    std::vector<std::vector<MetricResult>> analysis_metrics_;

    // The consistency stage.
    const ConsistencyTarget* target_ = nullptr;
    std::vector<std::optional<SearchPoint>> stage_points_;  // per candidate, in the stage's axis
    ConsistencyRecord record_;

    // The independent result and its metrics (regularise() starts from them), and the result as
    // independent() returns it.
    AutoResult independent_;
    std::vector<std::vector<MetricResult>> independent_metrics_;
    AutoResult independent_out_;
};

AutoResult AutoSession::Impl::with_user_parts(AutoResult result) const {
    const auto carry = [this](engine::DevelopSettings& s) {
        s.geometry = user_.geometry;
        s.local_adjustments = user_.local_adjustments;
    };
    carry(result.settings);
    for (CandidateRecord& c : result.candidates) {
        carry(c.settings);
    }
    if (result.consistency) {
        carry(result.consistency->independent_settings);
    }
    result.set_aside_geometry = !user_.geometry.is_identity();
    result.set_aside_masks = !user_.local_adjustments.empty();
    return result;
}

bool AutoSession::Impl::searchable(Coordinate c) const {
    switch (c) {
        case Coordinate::Exposure:
            return !locked(engine::DevelopControl::Exposure);
        case Coordinate::Shadows:
            return !locked(engine::DevelopControl::Shadows);
        case Coordinate::Highlights:
            return !locked(engine::DevelopControl::Highlights);
        case Coordinate::WhiteBalance:
            return white_balance_free() && axis_.active;
        case Coordinate::Contrast:
            return !locked(engine::DevelopControl::Contrast);
        case Coordinate::Saturation:
            return color_ && !locked(engine::DevelopControl::Saturation);
    }
    return false;
}

engine::DevelopSettings AutoSession::Impl::make_baseline() const {
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

SearchPoint AutoSession::Impl::normalise(SearchPoint p) const {
    p.exposure_ev =
        searchable(Coordinate::Exposure) ? snap(std::clamp(p.exposure_ev, kExposureMin, kExposureMax)) : 0.0;
    p.white_balance_strength =
        searchable(Coordinate::WhiteBalance) ? snap(std::clamp(p.white_balance_strength, 0.0, 1.0)) : 0.0;
    if (axis_.origin_red == 1.0 && axis_.origin_blue == 1.0) {
        // The independent search's axis (exactly the phase-1 arithmetic).
        p.red_gain = snap(1.0 + p.white_balance_strength * (axis_.target_red - 1.0));
        p.blue_gain = snap(1.0 + p.white_balance_strength * (axis_.target_blue - 1.0));
    } else {
        p.red_gain = snap(clamp_gain(axis_.origin_red + p.white_balance_strength * (axis_.target_red - axis_.origin_red)));
        p.blue_gain =
            snap(clamp_gain(axis_.origin_blue + p.white_balance_strength * (axis_.target_blue - axis_.origin_blue)));
    }
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

engine::DevelopSettings AutoSession::Impl::settings_for(const SearchPoint& p) const {
    engine::DevelopSettings s = baseline_;
    if (searchable(Coordinate::Exposure)) {
        s.exposure_ev = p.exposure_ev;
    }
    if (white_balance_free()) {
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

EditVector AutoSession::Impl::edit_of(const engine::DevelopSettings& s) const {
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

RankKey AutoSession::Impl::rank(std::size_t index, Resolution r) const {
    const CandidateRecord& c = result_.candidates[index];
    const CandidateScore& s = r == Resolution::Proxy ? c.proxy : c.analysis;
    return RankKey{s.evaluated && s.feasible, s.evaluated ? s.terms.total : kInfeasibleTotal, c.edit_size,
                   index};
}

Appearance AutoSession::Impl::appearance_from(const RenderStats& stats, const engine::DevelopSettings& s) const {
    Appearance a;
    a.valid = stats.pixels > 0;
    a.lightness = stats.quantiles.p50;
    a.midtone_pixels = stats.midtone_pixels;
    if (color_ && stats.midtone_pixels >= kMinMidtonePixels) {
        const UvPrime uv = uv_prime(stats.midtone_x, stats.midtone_y, stats.midtone_z);
        a.colour = uv.u > 0.0 && uv.v > 0.0;
        a.u = uv.u;
        a.v = uv.v;
    }
    if (color_ && wb_model_) {
        auto m = wb_model_->target_multipliers(s.white_balance);
        if (m && (*m)[0] > 0.0 && (*m)[1] > 0.0 && (*m)[2] > 0.0) {
            a.camera_gains = true;
            a.log_red = std::log((*m)[0] / (*m)[1]);
            a.log_blue = std::log((*m)[2] / (*m)[1]);
        }
    }
    return a;
}

void AutoSession::Impl::rescore(CandidateScore& s) const {
    // A score without an appearance failed to render: its total stays the infeasible one.
    if (!s.evaluated || !s.appearance.valid) {
        return;
    }
    s.terms.consistency = target_ != nullptr ? consistency_term(s.appearance, *target_).total : 0.0;
    s.terms.total = objective_total(s.terms);
}

Status AutoSession::Impl::setup(raw::DecodedRaw&& decoded) {
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
    auto source = analyze_source(decoded, grids, options_.develop.parallel, cancel());
    if (!source) {
        return Unexpected(std::move(source.error()));
    }
    result_.source = source->diagnostics;
    result_.timing.source_seconds = seconds_since(t);

    // 2. Stage one at full resolution.
    t = Clock::now();
    auto one = engine::render_stage_one(std::move(decoded), request, options_.develop, cancel());
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
                        ? engine::make_proxy(*one, options_.analysis_long_edge, options_.develop, cancel())
                        : detail::clone_stage_one(*one);
    if (!analysis) {
        return Unexpected(std::move(analysis.error()));
    }
    auto proxy = full_edge > options_.proxy_long_edge
                     ? engine::make_proxy(*one, options_.proxy_long_edge, options_.develop, cancel())
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

    // 4. Native crops (with the halo and halo-free), then release the full-resolution image.
    t = Clock::now();
    if (options_.native_crops) {
        result_.crops = select_native_crops(analysis_, ctx_a_, one->full_size, options_.native_crop_size);
        for (const NativeCrop& crop : result_.crops) {
            auto image = detail::crop_stage_one(*one, crop.rendered, options_.develop.limits);
            if (!image) {
                return Unexpected(std::move(image.error()));
            }
            auto halo_free = detail::crop_stage_one(*one, crop.measured, options_.develop.limits);
            if (!halo_free) {
                return Unexpected(std::move(halo_free.error()));
            }
            result_.memory.crop_bytes += image->byte_size() + halo_free->byte_size();
            crop_images_.push_back(std::move(*image));
            crop_halo_free_.push_back(std::move(*halo_free));
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
    describe_capture(analysis_.metadata, wb_model_ ? &*wb_model_ : nullptr, result_.full_size, result_.shot);
    return ok();
}

Status AutoSession::Impl::render_baseline() {
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
                                        BaselineFeatures& features,
                                        bool describe) -> Expected<RenderStats> {
        auto rendered = engine::render_stage_two(image, baseline_, options_.develop, cancel());
        if (!rendered) {
            return Unexpected(std::move(rendered.error()));
        }
        auto stats =
            measure_render(rendered->display, ctx, nullptr, &features, options_.develop.parallel, cancel());
        if (!stats) {
            return Unexpected(std::move(stats.error()));
        }
        if (stats->nonfinite_samples > 0 || stats->pixels == 0) {
            return fail(ErrorCode::Internal,
                        std::format("the baseline render has {} non-finite samples ({} valid pixels)",
                                    stats->nonfinite_samples, stats->pixels),
                        kWhere);
        }
        if (describe) {
            // The shot features grouping compares (consistency.hpp), from the as-shot render.
            if (auto described = describe_scene(ctx.signal, ctx.size, rendered->display, result_.shot);
                !described) {
                return Unexpected(std::move(described.error()));
            }
        }
        return stats;
    };
    auto stats_p = render_and_measure(proxy_, ctx_p_, feat_p_, false);
    if (!stats_p) {
        return Unexpected(std::move(stats_p.error()));
    }
    stats_p_ = *stats_p;
    auto stats_a = render_and_measure(analysis_, ctx_a_, feat_a_, true);
    if (!stats_a) {
        return Unexpected(std::move(stats_a.error()));
    }
    stats_a_ = *stats_a;
    const Appearance as_rendered = appearance_from(stats_a_, baseline_);
    result_.shot.rendered_colour = as_rendered.colour;
    result_.shot.rendered_u = as_rendered.u;
    result_.shot.rendered_v = as_rendered.v;

    crop_features_.resize(crop_images_.size());
    auto crops = measure_crops(baseline_, true);
    for (CropOutcome& o : crops) {
        if (o.render_error) {
            return Unexpected(std::move(*o.render_error));
        }
        if (!o.measurement) {
            return Unexpected(std::move(o.measurement.error()));
        }
        if (o.measurement->nonfinite_samples > 0) {
            return fail(ErrorCode::Internal, "the baseline render of a native crop has non-finite samples",
                        kWhere);
        }
        crop_base_.push_back(*o.measurement);
    }
    result_.timing.baseline_seconds = seconds_since(t);
    return ok();
}

std::vector<AutoSession::Impl::CropOutcome> AutoSession::Impl::measure_crops(const engine::DevelopSettings& s,
                                                                             bool baseline) {
    // Without local tone stage two is per pixel: the halo-free crop measures the same.
    const bool halo = has_local_tone(s);
    const std::size_t n = crop_images_.size();
    std::vector<CropOutcome> out(n);
    // The crops are independent renders, each written to its own slot: running them at once changes
    // nothing but the time (every render is itself schedule independent).
    const auto one = [&](std::size_t k) noexcept {
        try {
            const NativeCrop& crop = result_.crops[k];
            auto rendered =
                engine::render_stage_two(halo ? crop_images_[k] : crop_halo_free_[k], s, options_.develop, cancel());
            if (!rendered) {
                out[k].render_error = std::move(rendered.error());
                return;
            }
            out[k].measurement = measure_crop(rendered->display, halo ? crop.rendered : crop.measured, crop,
                                              baseline ? nullptr : &crop_features_[k],
                                              baseline ? &crop_features_[k] : nullptr);
        } catch (const std::bad_alloc&) {
            out[k].render_error = Error{ErrorCode::OutOfMemory, "out of memory rendering a native crop", kWhere, 0};
        }
    };
    std::vector<std::size_t> indices(n);
    std::iota(indices.begin(), indices.end(), std::size_t{0});
    if (options_.develop.parallel && n > 1) {
        std::for_each(std::execution::par, indices.begin(), indices.end(), one);
    } else {
        std::for_each(indices.begin(), indices.end(), one);
    }
    result_.timing.crop_renders += static_cast<std::uint32_t>(n);
    result_.timing.halo_free_crop_renders += halo ? 0u : static_cast<std::uint32_t>(n);
    return out;
}

Status AutoSession::Impl::propose_white_balance_first() {
    const auto t = Clock::now();
    policy_.color = color_;
    if (color_ && wb_model_ && !locked(engine::DevelopControl::WhiteBalance)) {
        // On the search proxy: its area averaging attenuates demosaicing artefacts at edges.
        const double clip = as_shot_clip_level(proxy_);
        auto wb = propose_white_balance(proxy_, ctx_p_, clip, options_.develop.parallel, cancel());
        if (!wb) {
            return Unexpected(std::move(wb.error()));
        }
        policy_.white_balance_evidence = wb->evidence;
        policy_.white_balance_confidence = wb->confidence;
        policy_.red_target = wb->red_gain;
        policy_.blue_target = wb->blue_gain;
        if (wb->evidence && wb->confidence >= kNeutralEvidenceMinConfidence) {
            // The surfaces the evidence treats as neutral, for the white-balance hard check of every
            // render (the baseline's included, so the masks precede it). A cast the estimators do
            // not agree on names arbitrary surfaces: the check then uses near-neutral pixels.
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
    // The independent search's white-balance axis: from as shot along the proposal.
    axis_ = WhiteBalanceAxis{1.0, 1.0, policy_.red_target, policy_.blue_target,
                             policy_.white_balance_evidence &&
                                 (policy_.red_target != 1.0 || policy_.blue_target != 1.0)};
    result_.timing.proposals_seconds += seconds_since(t);
    return ok();
}

Status AutoSession::Impl::propose_tone_after_baseline() {
    const auto t = Clock::now();
    auto scene = measure_scene(analysis_, ctx_a_, options_.develop.parallel, cancel());
    if (!scene) {
        return Unexpected(std::move(scene.error()));
    }
    result_.tone = propose_tone(stats_a_, *scene, result_.shot.scene_ev);
    policy_.tone = result_.tone;
    policy_.exposure_locked = locked(engine::DevelopControl::Exposure);
    result_.timing.proposals_seconds += seconds_since(t);
    return ok();
}

Expected<CandidateScore> AutoSession::Impl::evaluate(Resolution r, std::size_t index,
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
        auto rendered = engine::render_stage_two(image, c.settings, options_.develop, cancel());
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
                           nullptr, options_.develop.parallel, cancel());
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
            auto crops = measure_crops(c.settings, false);
            for (CropOutcome& o : crops) {
                if (o.render_error) {
                    if (fatal(*o.render_error)) {
                        return Unexpected(std::move(*o.render_error));
                    }
                    score.feasible = false;
                    score.terms.total = kInfeasibleTotal;
                    score.failures.push_back("native crop render: " + o.render_error->describe());
                    return score;
                }
                if (!o.measurement) {
                    return Unexpected(std::move(o.measurement.error()));
                }
                measured.push_back(*o.measurement);
            }
        }
        native = compare_native(measured, crop_base_);
        // Stage two without local tone is per pixel: it cannot create a halo (native_crops.hpp).
        native->halo_applies = has_local_tone(c.settings);
    }
    const NativeComparison* native_ptr = native ? &*native : nullptr;
    CandidateEvaluation evaluation = evaluate_candidate(stats, base, c.edit, policy_, native_ptr, is_baseline,
                                                        curves_monotone(c.settings));
    score.feasible = evaluation.feasible;
    score.failures = std::move(evaluation.failures);
    score.terms = natural_v1_objective(stats, base, c.edit, policy_, native_ptr);
    score.median_lightness = stats.quantiles.p50;
    score.new_clip_fraction = stats.fraction(stats.new_highlight_clipped);
    score.appearance = appearance_from(stats, c.settings);
    rescore(score);
    if (metrics != nullptr) {
        *metrics = std::move(evaluation.metrics);
    }
    return score;
}

Expected<std::optional<std::size_t>> AutoSession::Impl::add_proxy_candidate(
    CandidateOrigin origin, std::string note, const engine::DevelopSettings& settings,
    std::optional<SearchPoint> point, bool stage_point) {
    for (std::size_t i = 0; i < result_.candidates.size(); ++i) {
        if (!(result_.candidates[i].settings == settings)) {
            continue;
        }
        if (!result_.candidates[i].proxy.evaluated) {
            // A restrained candidate of the independent stage has no proxy score yet.
            if (!budget_left()) {
                return std::optional<std::size_t>{};
            }
            auto score = evaluate(Resolution::Proxy, i, nullptr);
            if (!score) {
                return Unexpected(std::move(score.error()));
            }
            result_.candidates[i].proxy = std::move(*score);
            ++stage_evaluations_;
            ++result_.timing.proxy_candidates;
        }
        return std::optional<std::size_t>{i};
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
    stage_points_.emplace_back(stage_point ? point : std::nullopt);
    const std::size_t index = result_.candidates.size() - 1;
    auto score = evaluate(Resolution::Proxy, index, nullptr);
    if (!score) {
        return Unexpected(std::move(score.error()));
    }
    result_.candidates[index].proxy = std::move(*score);
    ++stage_evaluations_;
    ++result_.timing.proxy_candidates;
    return std::optional<std::size_t>{index};
}

std::size_t AutoSession::Impl::select(std::span<const std::size_t> indices, Resolution r) const {
    std::vector<RankKey> keys;
    keys.reserve(indices.size());
    for (const std::size_t i : indices) {
        keys.push_back(rank(i, r));
    }
    const std::size_t at = select_best(keys);
    return at < indices.size() ? indices[at] : result_.candidates.size();
}

std::size_t AutoSession::Impl::best_stage_point() const {
    // Points a recheck already found infeasible at analysis resolution are not searched around.
    std::vector<std::size_t> points;
    for (std::size_t i = 0; i < result_.candidates.size() && i < stage_points_.size(); ++i) {
        const CandidateScore& a = result_.candidates[i].analysis;
        if (stage_points_[i] && !(a.evaluated && !a.feasible)) {
            points.push_back(i);
        }
    }
    return select(points, Resolution::Proxy);
}

Status AutoSession::Impl::coordinate_search(std::size_t best, std::array<double, 6> steps,
                                            std::uint32_t passes, CandidateOrigin origin_kind,
                                            std::string_view label) {
    for (std::uint32_t pass = 0; pass < passes && budget_left(); ++pass) {
        for (std::size_t k = 0; k < kOrder.size() && budget_left(); ++k) {
            const Coordinate coordinate = kOrder[k];
            if (!searchable(coordinate) || best >= stage_points_.size() || !stage_points_[best]) {
                continue;
            }
            const SearchPoint origin = *stage_points_[best];
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
                auto added = add_proxy_candidate(
                    origin_kind,
                    std::format("{}pass {} {} {:+.4g}", label, pass + 1, coordinate_name(coordinate), step),
                    settings_for(p), p);
                if (!added) {
                    return Unexpected(std::move(added.error()));
                }
                if (*added) {
                    const std::size_t i = **added;
                    // In the consistency stage an earlier candidate reached again takes this
                    // stage's point (its own lies on the independent axis).
                    if (target_ != nullptr && !stage_points_[i]) {
                        stage_points_[i] = p;
                    }
                    best = best_stage_point();
                }
            }
        }
        for (double& s : steps) {
            s *= 0.5;
        }
    }
    return ok();
}

Status AutoSession::Impl::search() {
    const auto t = Clock::now();
    stage_budget_ = options_.candidate_budget;
    stage_evaluations_ = 0;
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
    if (auto status = coordinate_search(best_stage_point(), kInitialSteps, kMaxPasses,
                                        CandidateOrigin::CoordinateStep, "");
        !status) {
        return status;
    }
    result_.timing.search_seconds = seconds_since(t);
    return ok();
}

Status AutoSession::Impl::consistency_search() {
    const ConsistencyTarget& target = *target_;
    stage_budget_ = options_.consistency_budget;
    stage_evaluations_ = 0;
    const std::uint32_t renders_before = result_.timing.proxy_candidates;

    // a. Rank every evaluated candidate again with the consistency term (no render).
    for (CandidateRecord& c : result_.candidates) {
        rescore(c.proxy);
        rescore(c.analysis);
    }
    // The start: the best re-ranked candidate of the search space that the independent recheck did
    // not find infeasible at analysis resolution (the baseline always qualifies).
    std::vector<std::size_t> eligible;
    for (std::size_t i = 0; i < result_.candidates.size(); ++i) {
        const CandidateRecord& c = result_.candidates[i];
        if (c.point && (i == 0 || !(c.analysis.evaluated && !c.analysis.feasible))) {
            eligible.push_back(i);
        }
    }
    std::size_t start_index = select(eligible, Resolution::Proxy);
    if (start_index >= result_.candidates.size()) {
        start_index = 0;
    }
    const SearchPoint start_independent = *result_.candidates[start_index].point;

    // b. The stage's white-balance axis, from the start's gains.
    axis_ = WhiteBalanceAxis{start_independent.red_gain, start_independent.blue_gain,
                             start_independent.red_gain, start_independent.blue_gain, false};
    record_.white_balance_axis.clear();
    const Appearance& start_look = result_.candidates[start_index].proxy.appearance;
    if (white_balance_free()) {
        if (target.camera_white_balance && target.domain == result_.shot.calibration_domain) {
            // The domain target's camera multipliers, relative to as shot (the baseline's).
            const double as_red = std::log(baseline_multipliers_[0] / baseline_multipliers_[1]);
            const double as_blue = std::log(baseline_multipliers_[2] / baseline_multipliers_[1]);
            axis_.target_red = snap(clamp_gain(std::exp(target.log_red - as_red)));
            axis_.target_blue = snap(clamp_gain(std::exp(target.log_blue - as_blue)));
            record_.white_balance_axis = "camera";
        } else if (target.colour && start_look.colour) {
            // Two probes on the proxy give the rendered u'v' response to log gains (colour-managed:
            // camera gains are never compared across domains).
            std::array<std::optional<Appearance>, 2> probes;
            std::array<double, 2> steps{};
            for (std::size_t k = 0; k < 2; ++k) {
                SearchPoint p = start_independent;
                double& gain = k == 0 ? p.red_gain : p.blue_gain;
                double step = kColourProbeStep;
                if (gain * std::exp(step) > 1.0 + kWhiteBalanceResidualBound) {
                    step = -step;
                }
                gain = snap(clamp_gain(gain * std::exp(step)));
                steps[k] = std::log(gain / (k == 0 ? start_independent.red_gain : start_independent.blue_gain));
                p.white_balance_strength = 0.0;
                auto added =
                    add_proxy_candidate(CandidateOrigin::ConsistencyProposal,
                                        k == 0 ? "consistency colour probe (red)" : "consistency colour probe (blue)",
                                        settings_for(p), p, false);
                if (!added) {
                    return Unexpected(std::move(added.error()));
                }
                if (*added) {
                    const CandidateScore& s = result_.candidates[**added].proxy;
                    if (s.evaluated && s.appearance.colour && steps[k] != 0.0) {
                        probes[k] = s.appearance;
                    }
                }
            }
            if (probes[0] && probes[1]) {
                const double j00 = (probes[0]->u - start_look.u) / steps[0];
                const double j10 = (probes[0]->v - start_look.v) / steps[0];
                const double j01 = (probes[1]->u - start_look.u) / steps[1];
                const double j11 = (probes[1]->v - start_look.v) / steps[1];
                const double det = j00 * j11 - j01 * j10;
                const double scale = std::max({std::abs(j00), std::abs(j01), std::abs(j10), std::abs(j11)});
                if (std::isfinite(det) && scale > 0.0 && std::abs(det) > 1e-6 * scale * scale) {
                    const double du = target.u - start_look.u;
                    const double dv = target.v - start_look.v;
                    const double dr = (j11 * du - j01 * dv) / det;
                    const double db = (-j10 * du + j00 * dv) / det;
                    axis_.target_red = snap(clamp_gain(start_independent.red_gain * std::exp(dr)));
                    axis_.target_blue = snap(clamp_gain(start_independent.blue_gain * std::exp(db)));
                    record_.white_balance_axis = "colour";
                }
            }
        }
        axis_.active = axis_.target_red != axis_.origin_red || axis_.target_blue != axis_.origin_blue;
        if (!axis_.active) {
            record_.white_balance_axis.clear();
        }
    }
    record_.axis_red_gain = axis_.target_red;
    record_.axis_blue_gain = axis_.target_blue;

    // The start in the stage's coordinates (strength 0: its own gains).
    SearchPoint start = start_independent;
    start.white_balance_strength = 0.0;
    start = normalise(start);
    std::fill(stage_points_.begin(), stage_points_.end(), std::nullopt);
    stage_points_.resize(result_.candidates.size());
    stage_points_[start_index] = start;

    // c. Seeds: the exposure that reaches the target lightness, with and without the white-balance
    // move, and the white-balance move alone.
    std::vector<std::pair<std::string, SearchPoint>> seeds;
    if (searchable(Coordinate::Exposure) && start_look.valid && start_look.lightness > 0.0) {
        const double ev = std::log2(engine::display_lightness_inverse(std::clamp(target.lightness, 0.01, 0.99)) /
                                    engine::display_lightness_inverse(start_look.lightness));
        if (std::isfinite(ev)) {
            SearchPoint p = start;
            p.exposure_ev = start.exposure_ev + ev;
            SearchPoint both = p;
            both.white_balance_strength = 1.0;
            seeds.emplace_back("consistency exposure and white balance", normalise(both));
            seeds.emplace_back("consistency exposure", normalise(p));
        }
    }
    if (axis_.active) {
        SearchPoint p = start;
        p.white_balance_strength = 1.0;
        seeds.emplace_back("consistency white balance", normalise(p));
    }
    for (const auto& [note, point] : seeds) {
        if (point == start) {
            continue;
        }
        auto added = add_proxy_candidate(CandidateOrigin::ConsistencyProposal, note, settings_for(point), point);
        if (!added) {
            return Unexpected(std::move(added.error()));
        }
        if (*added && !stage_points_[**added]) {
            stage_points_[**added] = point;
        }
    }

    // Coordinate steps at half the initial sizes.
    std::array<double, 6> steps = kInitialSteps;
    for (double& s : steps) {
        s *= 0.5;
    }
    if (auto status = coordinate_search(best_stage_point(), steps, kConsistencyPasses,
                                        CandidateOrigin::ConsistencyStep, "consistency ");
        !status) {
        return status;
    }
    result_.timing.consistency_candidates = result_.timing.proxy_candidates - renders_before;
    record_.added_candidates = result_.timing.consistency_candidates;
    return ok();
}

Expected<std::size_t> AutoSession::Impl::recheck(std::span<const std::size_t> always) {
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
    const auto add = [&](std::size_t i) {
        if (i < result_.candidates.size() && std::find(pool.begin(), pool.end(), i) == pool.end()) {
            pool.push_back(i);
        }
    };
    for (const std::size_t i : order) {
        if (pool.size() >= options_.recheck_count) {
            break;
        }
        if (rank(i, Resolution::Proxy).feasible) {
            pool.push_back(i);
        }
    }
    const std::size_t leader = pool.empty() ? result_.candidates.size() : pool.front();
    // Diversity: the best feasible candidate without local tone, so the native checks can trade
    // the leaders' local tone against an edit that has none.
    std::vector<std::size_t> global;
    for (std::size_t i = 1; i < result_.candidates.size(); ++i) {
        const CandidateRecord& c = result_.candidates[i];
        if (rank(i, Resolution::Proxy).feasible && !changes_local_tone(c.edit) && !is_identity_edit(c.edit)) {
            global.push_back(i);
        }
    }
    add(select(global, Resolution::Proxy));
    add(0);  // the baseline is always the last resort
    for (const std::size_t i : always) {
        add(i);
    }
    const auto evaluate_pool = [&]() -> Status {
        for (const std::size_t i : pool) {
            if (result_.candidates[i].analysis.evaluated) {
                continue;  // rechecked by the independent stage (and re-ranked)
            }
            auto score = evaluate(Resolution::Analysis, i, &analysis_metrics_[i]);
            if (!score) {
                return Unexpected(std::move(score.error()));
            }
            result_.candidates[i].analysis = std::move(*score);
            ++result_.timing.analysis_candidates;
        }
        return ok();
    };
    if (auto status = evaluate_pool(); !status) {
        return Unexpected(std::move(status.error()));
    }
    // Bounded conservative fallback (blueprint section 6 step 7, at analysis resolution): when every
    // rechecked candidate but the baseline failed a hard check, the proxy leader's conservative
    // variants are rechecked before the baseline is accepted.
    const bool rejected = std::none_of(pool.begin(), pool.end(), [&](std::size_t i) {
        return i != 0 && result_.candidates[i].analysis.evaluated && result_.candidates[i].analysis.feasible;
    });
    fallback_ = false;
    if (rejected && leader < result_.candidates.size() && leader != 0) {
        fallback_ = true;
        std::uint32_t added = 0;
        for (const engine::DevelopSettings& s :
             conservative_fallbacks(result_.candidates[leader].settings, baseline_)) {
            if (added >= kMaxFallbackRechecks) {
                break;
            }
            const std::size_t f = add_fallback(leader, s);
            if (f < result_.candidates.size()) {
                add(f);
                ++added;
            }
        }
        if (auto status = evaluate_pool(); !status) {
            return Unexpected(std::move(status.error()));
        }
    }
    std::size_t chosen = select(pool, Resolution::Analysis);
    if (chosen >= result_.candidates.size() || !rank(chosen, Resolution::Analysis).feasible) {
        chosen = 0;
    }
    return chosen;
}

std::size_t AutoSession::Impl::add_fallback(std::size_t source, const engine::DevelopSettings& settings) {
    for (std::size_t i = 0; i < result_.candidates.size(); ++i) {
        if (result_.candidates[i].settings == settings) {
            return i;
        }
    }
    const CandidateRecord& from = result_.candidates[source];
    if (!from.point) {
        return result_.candidates.size();
    }
    CandidateRecord c;
    c.index = static_cast<std::uint32_t>(result_.candidates.size());
    c.origin = CandidateOrigin::Fallback;
    c.settings = settings;
    c.edit = edit_of(settings);
    c.edit_size = edit_size(c.edit);
    c.note = std::format("fallback of {}: exposure {:+.3f}, shadows {:.3f}, highlights {:.3f}", source,
                         c.edit.exposure_ev, c.edit.shadows_ev, c.edit.highlights_ev);
    // The point in the search's coordinates (the white balance is the source's).
    SearchPoint p = *from.point;
    p.exposure_ev = c.edit.exposure_ev;
    p.shadows_ev = c.edit.shadows_ev;
    p.highlights_ev = c.edit.highlights_ev;
    p.contrast = c.edit.contrast;
    p.saturation = c.edit.saturation;
    c.point = p;
    c.source = static_cast<std::uint32_t>(source);
    result_.candidates.push_back(std::move(c));
    analysis_metrics_.emplace_back();
    stage_points_.emplace_back();
    return result_.candidates.size() - 1;
}

ConfidenceBreakdown AutoSession::Impl::confidence_of(const CandidateRecord& c) const {
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
        b.brightness = result_.tone.evidence ? result_.tone.signal_evidence : 0.0;
        if (b.brightness < 1.0) {
            b.reasons.push_back(
                std::format("signal evidence {:.2f}: {}", b.brightness, result_.tone.reason));
        }
    }
    if (changes_white_balance_materially(e)) {
        b.white_balance = policy_.white_balance_evidence ? policy_.white_balance_confidence : 0.0;
        if (target_ != nullptr && !record_.white_balance_axis.empty() &&
            target_->white_balance_confidence > b.white_balance) {
            b.white_balance = target_->white_balance_confidence;
            b.reasons.push_back(std::format("white balance follows the group target {} (confidence {:.2f})",
                                            target_->group_id, b.white_balance));
        } else if (b.white_balance < 1.0) {
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

Status AutoSession::Impl::finish(std::size_t chosen) {
    result_.diagnostics.clear();
    result_.flags.clear();
    result_.restrained = false;
    result_.fallback = fallback_;
    // Confidence and restraint, always from an unrestrained point: a restrained winner (the
    // independent result entering the consistency stage) is restrained again from its source.
    std::size_t unrestrained = chosen;
    if (result_.candidates[chosen].origin == CandidateOrigin::Restrained && result_.candidates[chosen].source &&
        *result_.candidates[chosen].source < result_.candidates.size()) {
        unrestrained = *result_.candidates[chosen].source;
    }
    ConfidenceBreakdown confidence = confidence_of(result_.candidates[unrestrained]);
    const CandidateRecord winner = result_.candidates[unrestrained];
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
        p.shadows_ev = f_tone * w.shadows_ev;
        p.highlights_ev = f_tone * w.highlights_ev;
        p.contrast = f_tone * w.contrast;
        p.saturation = 1.0 + f_tone * (w.saturation - 1.0);
        if (target_ == nullptr) {
            // Along the proposal from as shot (the independent axis).
            p.white_balance_strength = f_wb * w.white_balance_strength;
            p = normalise(p);
        } else {
            // Towards as shot from the winner's gains.
            p.white_balance_strength = 0.0;
            p = normalise(p);
            if (white_balance_free()) {
                p.red_gain = snap(clamp_gain(1.0 + f_wb * (w.red_gain - 1.0)));
                p.blue_gain = snap(clamp_gain(1.0 + f_wb * (w.blue_gain - 1.0)));
            }
        }
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
            c.source = winner.index;
            result_.candidates.push_back(std::move(c));
            analysis_metrics_.emplace_back();
            stage_points_.emplace_back();
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
    } else if (unrestrained != chosen && result_.candidates[unrestrained].analysis.evaluated &&
               result_.candidates[unrestrained].analysis.feasible) {
        // A restrained winner whose source now needs no restraint (a group target's white-balance
        // confidence): the source itself.
        chosen = unrestrained;
    }

    // The policy confidence (auto_settings.hpp, step 7): reported, never restraining.
    const EditVector& e = result_.candidates[chosen].edit;
    const bool large = std::abs(e.exposure_ev) >= kLargeExposureEv ||
                       std::abs(e.shadows_ev) + std::abs(e.highlights_ev) >= kLargeLocalToneEv;
    if (large) {
        confidence.policy = result_.tone.key_ambiguous ? kAmbiguousLargeEditConfidence : kLargeEditConfidence;
        confidence.reasons.push_back(std::format(
            "large edit (exposure {:+.2f} EV, shadows {:+.2f} EV, highlights {:+.2f} EV){}", e.exposure_ev,
            e.shadows_ev, e.highlights_ev,
            result_.tone.key_ambiguous ? ": the key cues reduced the correction, so its size rests on the policy" : ""));
    }
    if (fallback_) {
        const CandidateScore& base = result_.candidates[0].analysis;
        const bool unmet = chosen == 0 && base.evaluated && base.terms.brightness > 1.0;
        confidence.policy = std::min(confidence.policy, unmet ? kUnmetFallbackConfidence : kFallbackConfidence);
        confidence.reasons.push_back(
            unmet ? std::format("every rechecked candidate and conservative fallback failed a hard check: as shot is "
                                "kept although its brightness term is {:.2f}",
                                base.terms.brightness)
                  : std::string("the leading candidates failed a hard check at analysis resolution: a conservative "
                                "fallback was chosen"));
    }
    confidence.overall = std::min(confidence.overall, confidence.policy);
    result_.chosen = chosen;
    result_.settings = result_.candidates[chosen].settings;
    result_.confidence = confidence.overall;
    result_.confidence_detail = confidence;
    result_.appearance = result_.candidates[chosen].analysis.appearance;

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

void AutoSession::Impl::add_flags() {
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
    if (result_.fallback) {
        const CandidateRecord& c = result_.candidates[result_.chosen];
        flags.push_back({ReviewFlagCode::ConservativeFallback, std::nullopt,
                         c.origin == CandidateOrigin::Baseline
                             ? std::string("the leading candidates and their conservative fallbacks failed a hard "
                                           "check at analysis resolution: as shot is kept")
                             : std::format("the leading candidates failed a hard check at analysis resolution: "
                                           "conservative fallback {} ({}) chosen",
                                           c.index, c.note)});
    }
    {
        const EditVector& e = result_.candidates[result_.chosen].edit;
        if (std::abs(e.exposure_ev) >= kLargeExposureEv ||
            std::abs(e.shadows_ev) + std::abs(e.highlights_ev) >= kLargeLocalToneEv) {
            flags.push_back({ReviewFlagCode::LargeEdit, MetricId::TonalDistribution,
                             std::format("large automatic edit: exposure {:+.2f} EV, shadows {:+.2f} EV, highlights "
                                         "{:+.2f} EV{}",
                                         e.exposure_ev, e.shadows_ev, e.highlights_ev,
                                         result_.tone.key_ambiguous ? " (the key was ambiguous)" : "")});
        }
    }
    if (!result_.warnings.empty()) {
        std::string text;
        for (const std::string& w : result_.warnings) {
            text += text.empty() ? w : "; " + w;
        }
        flags.push_back({ReviewFlagCode::InterpretationLimited, std::nullopt, text});
    }
}

Status AutoSession::Impl::open(raw::DecodedRaw&& decoded, const core::CancellationToken& cancel) {
    cancel_ = &cancel;
    if (auto valid = validate_options(options_); !valid) {
        return valid;
    }
    if (auto valid = user_.validate(); !valid) {
        return fail(ErrorCode::InvalidArgument, "the current settings are invalid: " + valid.error().message,
                    kWhere);
    }
    if (auto status = setup(std::move(decoded)); !status) {
        return status;
    }
    if (auto status = propose_white_balance_first(); !status) {
        return status;
    }
    if (auto status = render_baseline(); !status) {
        return status;
    }
    if (auto status = propose_tone_after_baseline(); !status) {
        return status;
    }
    if (auto status = search(); !status) {
        return status;
    }
    const auto t = Clock::now();
    auto chosen = recheck({});
    if (!chosen) {
        return Unexpected(std::move(chosen.error()));
    }
    result_.timing.recheck_seconds = seconds_since(t);
    if (auto status = finish(*chosen); !status) {
        return status;
    }
    result_.timing.total_seconds = seconds_since(start_);
    independent_ = result_;
    independent_metrics_ = analysis_metrics_;
    independent_out_ = with_user_parts(independent_);
    cancel_ = nullptr;
    return ok();
}

Expected<AutoResult> AutoSession::Impl::regularise(const ConsistencyTarget& target,
                                                   const core::CancellationToken& cancel) {
    if (auto valid = validate_target(target); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    if (target.camera_white_balance && target.domain != independent_.shot.calibration_domain) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the consistency target's camera white balance belongs to calibration domain '{}', "
                                "not this image's '{}': camera gains are never compared across domains",
                                target.domain, independent_.shot.calibration_domain),
                    kWhere);
    }
    const auto start = Clock::now();
    cancel_ = &cancel;
    target_ = &target;
    record_ = ConsistencyRecord{};
    record_.target = target;
    // Start from the independent result.
    result_ = independent_;
    result_.consistency.reset();
    analysis_metrics_ = independent_metrics_;
    stage_points_.assign(result_.candidates.size(), std::nullopt);
    const std::size_t independent_chosen = independent_.chosen;

    // Without consistency pressure on the independent choice nothing can rank above it for
    // consistency: the independent result stands (the stage would only re-run the image's own search).
    const CandidateRecord& independent_winner = independent_.candidates[independent_chosen];
    if (independent_winner.analysis.evaluated && independent_winner.analysis.appearance.valid &&
        consistency_term(independent_winner.analysis.appearance, target).total == 0.0) {
        AutoResult out = independent_;
        record_.independent_chosen = independent_chosen;
        record_.independent_settings = independent_winner.settings;
        record_.independent_appearance = independent_winner.analysis.appearance;
        record_.independent_total = independent_winner.analysis.terms.total;
        record_.appearance = record_.independent_appearance;
        record_.total = record_.independent_total;
        record_.adjustment = EditVector{};
        out.consistency = record_;
        out.timing.consistency_seconds = seconds_since(start);
        out.timing.total_seconds = independent_.timing.total_seconds + out.timing.consistency_seconds;
        target_ = nullptr;
        cancel_ = nullptr;
        result_ = AutoResult{};
        return with_user_parts(std::move(out));
    }

    auto status = consistency_search();
    if (status) {
        // The independent winner enters unrestrained: restraint is applied once, to the stage's winner.
        const CandidateRecord& w = result_.candidates[independent_chosen];
        const std::size_t unrestrained =
            w.origin == CandidateOrigin::Restrained && w.source ? std::size_t{*w.source} : independent_chosen;
        const std::array<std::size_t, 1> always{unrestrained};
        auto chosen = recheck(always);
        if (!chosen) {
            status = Unexpected(std::move(chosen.error()));
        } else {
            status = finish(*chosen);
        }
    }
    if (!status) {
        target_ = nullptr;
        cancel_ = nullptr;
        return Unexpected(std::move(status.error()));
    }

    // The record: the independent choice and the chosen settings, both ranked with the term.
    const CandidateRecord& before = result_.candidates[independent_chosen];
    const CandidateRecord& after = result_.candidates[result_.chosen];
    record_.independent_chosen = independent_chosen;
    record_.independent_settings = before.settings;
    record_.independent_appearance = before.analysis.appearance;
    record_.independent_terms = consistency_term(before.analysis.appearance, target);
    record_.independent_total = before.analysis.terms.total;
    record_.appearance = after.analysis.appearance;
    record_.terms = consistency_term(after.analysis.appearance, target);
    record_.total = after.analysis.terms.total;
    record_.changed = !(after.settings == before.settings);
    EditVector& a = record_.adjustment;
    a.exposure_ev = after.edit.exposure_ev - before.edit.exposure_ev;
    a.red_gain = after.edit.red_gain / before.edit.red_gain;
    a.blue_gain = after.edit.blue_gain / before.edit.blue_gain;
    a.shadows_ev = after.edit.shadows_ev - before.edit.shadows_ev;
    a.highlights_ev = after.edit.highlights_ev - before.edit.highlights_ev;
    a.contrast = after.edit.contrast - before.edit.contrast;
    a.saturation = after.edit.saturation / before.edit.saturation;
    if (record_.terms.total > kConsistencyLimitedTerm) {
        result_.flags.push_back(
            {ReviewFlagCode::ConsistencyLimited, std::nullopt,
             std::format("group {}: consistency term {:.2f} remains (lightness {:.2f}, colour {:.2f}, white "
                         "balance {:.2f}) within the hard checks, locks and confidence restraint",
                         target.group_id, record_.terms.total, record_.terms.lightness, record_.terms.colour,
                         record_.terms.white_balance)});
    }
    result_.consistency = record_;
    result_.timing.consistency_seconds = seconds_since(start);
    result_.timing.total_seconds = independent_.timing.total_seconds + result_.timing.consistency_seconds;

    // Leave the session as the independent analysis.
    axis_ = WhiteBalanceAxis{1.0, 1.0, policy_.red_target, policy_.blue_target,
                             policy_.white_balance_evidence &&
                                 (policy_.red_target != 1.0 || policy_.blue_target != 1.0)};
    target_ = nullptr;
    cancel_ = nullptr;
    AutoResult out = std::move(result_);
    result_ = AutoResult{};
    return with_user_parts(std::move(out));
}

Expected<Appearance> AutoSession::Impl::appearance_of(const engine::DevelopSettings& settings,
                                                      const core::CancellationToken& cancel) {
    if (auto valid = settings.validate(); !valid) {
        return fail(ErrorCode::InvalidArgument, "invalid settings: " + valid.error().message, kWhere);
    }
    const engine::DevelopSettings evaluated = evaluation_settings(settings);
    auto rendered = engine::render_stage_two(analysis_, evaluated, options_.develop, cancel);
    if (!rendered) {
        return Unexpected(std::move(rendered.error()));
    }
    auto stats = measure_render(rendered->display, ctx_a_, &feat_a_, nullptr, options_.develop.parallel, cancel);
    if (!stats) {
        return Unexpected(std::move(stats.error()));
    }
    return appearance_from(*stats, evaluated);
}

Expected<engine::ColorImage> AutoSession::Impl::preview(const engine::DevelopSettings& settings,
                                                        std::int32_t edge, const core::CancellationToken& cancel) {
    if (edge < 16) {
        return fail(ErrorCode::InvalidArgument, std::format("preview long edge {} is below 16", edge), kWhere);
    }
    if (auto valid = settings.validate(); !valid) {
        return fail(ErrorCode::InvalidArgument, "invalid settings: " + valid.error().message, kWhere);
    }
    auto rendered = engine::render_stage_two(analysis_, settings, options_.develop, cancel);
    if (!rendered) {
        return Unexpected(std::move(rendered.error()));
    }
    const image::Size size = engine::proxy_size(rendered->display.pixels.size(), edge);
    if (size == rendered->display.pixels.size()) {
        return std::move(rendered->display);
    }
    auto buffer = image::ImageBuffer<float>::create_uninitialized(size.width, size.height, 3, options_.develop.limits);
    if (!buffer) {
        return Unexpected(std::move(buffer.error()));
    }
    if (auto scaled = engine::box_downscale(rendered->display.pixels.const_view(), buffer->view(), cancel,
                                            options_.develop.parallel);
        !scaled) {
        return Unexpected(std::move(scaled.error()));
    }
    engine::ColorImage out;
    out.pixels = std::move(*buffer);
    out.encoding = rendered->display.encoding;
    return out;
}

// ---- Names -------------------------------------------------------------------------------------

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
        case CandidateOrigin::ConsistencyProposal:
            return "consistency_proposal";
        case CandidateOrigin::ConsistencyStep:
            return "consistency_step";
        case CandidateOrigin::Fallback:
            return "fallback";
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
        case ReviewFlagCode::ConsistencyLimited:
            return "consistency_limited";
        case ReviewFlagCode::LargeEdit:
            return "large_edit";
        case ReviewFlagCode::ConservativeFallback:
            return "conservative_fallback";
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

// ---- Entry points ------------------------------------------------------------------------------

engine::DevelopSettings evaluation_settings(engine::DevelopSettings settings) {
    settings.geometry = engine::GeometrySetting{};
    settings.local_adjustments.clear();
    return settings;
}

namespace {

// Probe, memory check and decode of auto_develop_file / AutoSession::open_file.
Expected<raw::DecodedRaw> decode_for_auto(const raw::DecoderRegistry& registry, const std::filesystem::path& path,
                                          std::optional<std::uint32_t> subimage, const AutoOptions& options,
                                          const core::CancellationToken& cancel, double& decode_seconds) {
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
    decode_seconds = seconds_since(t);
    return decoded;
}

}  // namespace

AutoSession::AutoSession(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
AutoSession::AutoSession(AutoSession&&) noexcept = default;
AutoSession& AutoSession::operator=(AutoSession&&) noexcept = default;
AutoSession::~AutoSession() = default;

Expected<AutoSession> AutoSession::open(raw::DecodedRaw&& decoded, const engine::DevelopSettings& current,
                                        const AutoOptions& options, const core::CancellationToken& cancel) {
    try {
        auto impl = std::make_unique<Impl>(current, options, Clock::now());
        if (auto status = impl->open(std::move(decoded), cancel); !status) {
            return Unexpected(std::move(status.error()));
        }
        return AutoSession(std::move(impl));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory during the automatic settings", kWhere);
    }
}

Expected<AutoSession> AutoSession::open_file(const raw::DecoderRegistry& registry, const std::filesystem::path& path,
                                             std::optional<std::uint32_t> subimage,
                                             const engine::DevelopSettings& current, const AutoOptions& options,
                                             const core::CancellationToken& cancel) {
    const auto start = Clock::now();
    double decode_seconds = 0.0;
    auto decoded = decode_for_auto(registry, path, subimage, options, cancel, decode_seconds);
    if (!decoded) {
        return Unexpected(std::move(decoded.error()));
    }
    try {
        auto impl = std::make_unique<Impl>(current, options, start);
        if (auto status = impl->open(std::move(*decoded), cancel); !status) {
            return Unexpected(std::move(status.error()));
        }
        impl->set_decode_seconds(decode_seconds);
        return AutoSession(std::move(impl));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory during the automatic settings", kWhere);
    }
}

const AutoResult& AutoSession::independent() const noexcept {
    return impl_->independent();
}

Expected<AutoResult> AutoSession::regularise(const ConsistencyTarget& target, const core::CancellationToken& cancel) {
    try {
        return impl_->regularise(target, cancel);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory during the consistency stage", kWhere);
    }
}

Expected<Appearance> AutoSession::appearance_of(const engine::DevelopSettings& settings,
                                                const core::CancellationToken& cancel) {
    try {
        return impl_->appearance_of(settings, cancel);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while measuring an appearance", kWhere);
    }
}

Expected<engine::ColorImage> AutoSession::preview(const engine::DevelopSettings& settings, std::int32_t long_edge,
                                                  const core::CancellationToken& cancel) {
    try {
        return impl_->preview(settings, long_edge, cancel);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while rendering a preview", kWhere);
    }
}

std::uint64_t AutoSession::retained_bytes() const noexcept {
    return impl_->retained_bytes();
}

Expected<AutoResult> auto_develop(raw::DecodedRaw&& decoded, const engine::DevelopSettings& current,
                                  const AutoOptions& options, const core::CancellationToken& cancel) {
    try {
        AutoSession::Impl impl(current, options, Clock::now());
        if (auto status = impl.open(std::move(decoded), cancel); !status) {
            return Unexpected(std::move(status.error()));
        }
        return impl.take_independent();
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
    double decode_seconds = 0.0;
    auto decoded = decode_for_auto(registry, path, subimage, options, cancel, decode_seconds);
    if (!decoded) {
        return Unexpected(std::move(decoded.error()));
    }
    try {
        AutoSession::Impl impl(current, options, start);
        if (auto status = impl.open(std::move(*decoded), cancel); !status) {
            return Unexpected(std::move(status.error()));
        }
        AutoResult result = impl.take_independent();
        result.timing.decode_seconds = decode_seconds;
        return result;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory during the automatic settings", kWhere);
    }
}

std::vector<engine::DevelopSettings> conservative_fallbacks(const engine::DevelopSettings& chosen,
                                                           const engine::DevelopSettings& baseline) {
    std::vector<engine::DevelopSettings> out;
    const auto push = [&](const engine::DevelopSettings& s) {
        if (s == chosen || s == baseline) {
            return;
        }
        if (std::none_of(out.begin(), out.end(), [&](const engine::DevelopSettings& o) { return o == s; })) {
            out.push_back(s);
        }
    };
    const auto half = [](double c, double b) { return snap(b + 0.5 * (c - b)); };
    if (chosen.shadows != baseline.shadows || chosen.highlights != baseline.highlights) {
        engine::DevelopSettings halved = chosen;
        halved.shadows = half(chosen.shadows, baseline.shadows);
        halved.highlights = half(chosen.highlights, baseline.highlights);
        push(halved);
        engine::DevelopSettings none = chosen;
        none.shadows = baseline.shadows;
        none.highlights = baseline.highlights;
        push(none);
    } else {
        engine::DevelopSettings halved = chosen;
        halved.exposure_ev = half(chosen.exposure_ev, baseline.exposure_ev);
        halved.contrast = half(chosen.contrast, baseline.contrast);
        halved.saturation = half(chosen.saturation, baseline.saturation);
        push(halved);
    }
    return out;
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
    const std::int64_t short_side = std::min(upright.width, upright.height);
    const std::int64_t side = std::min<std::int64_t>(options.native_crop_size + 2 * native_crop_halo(upright),
                                                     short_side);
    const std::int64_t halo_free = std::min<std::int64_t>(options.native_crop_size + 2 * kNativeMeasureMargin,
                                                          short_side);
    const std::uint64_t crops =
        options.native_crops
            ? 4u * static_cast<std::uint64_t>(side * side + halo_free * halo_free) * channels * 4u
            : 0u;
    // Kept besides the full image: both proxies, crops, contexts (two floats) and baseline features
    // (13 bytes) per analysis and proxy pixel.
    const auto per_pixel = static_cast<std::uint64_t>(analysis.area() + proxy.area()) * 21u;
    const std::uint64_t kept = plane(analysis) + plane(proxy) + crops + per_pixel;
    return std::max(estimate->stage_one_peak, estimate->stage_one_bytes + kept);
}

}  // namespace lucent::analysis
