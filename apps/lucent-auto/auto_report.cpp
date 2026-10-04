#include "auto_report.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <variant>

#include <lucent/application/recipe.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/core/text.hpp>
#include <lucent/engine/develop_operations.hpp>

#include "render_json.hpp"

namespace lucent::auto_tool {

namespace {

using render_tool::number;
namespace an = analysis;

Json named_values(const std::vector<an::NamedValue>& values) {
    Json out = Json::object();
    for (const an::NamedValue& v : values) {
        out[v.name] = number(v.value);
    }
    return out;
}

Json optional_number(const std::optional<double>& v) {
    return v ? number(*v) : Json(nullptr);
}

Json white_balance_json(const engine::WhiteBalanceSetting& wb) {
    Json out{{"mode", std::string(engine::to_string(engine::mode_of(wb)))}};
    if (const auto* k = std::get_if<engine::KelvinWhiteBalance>(&wb)) {
        out["temperature_k"] = number(k->temperature_k);
        out["tint"] = number(k->tint);
    } else if (const auto* c = std::get_if<engine::CameraNeutralWhiteBalance>(&wb)) {
        out["multipliers"] = Json::array({number(c->r), number(c->g), number(c->b)});
    }
    return out;
}

// The chosen settings relative to as shot (the tool's items start from as shot without locks, so
// these are the settings themselves), with the canonical recipe.
Json settings_json(const engine::DevelopSettings& s, const an::EditVector& e) {
    Json out{{"exposure_ev", number(e.exposure_ev)},
             {"white_balance", white_balance_json(s.white_balance)},
             {"red_gain", number(e.red_gain)},
             {"blue_gain", number(e.blue_gain)},
             {"shadows_ev", number(e.shadows_ev)},
             {"highlights_ev", number(e.highlights_ev)},
             {"contrast", number(e.contrast)},
             {"saturation", number(e.saturation)}};
    if (auto recipe = app::write_recipe(s)) {
        out["recipe_sha256"] = core::to_hex(recipe->sha256);
        out["recipe"] = Json::parse(recipe->json);
    } else {
        out["recipe_sha256"] = nullptr;
        out["recipe"] = nullptr;
    }
    return out;
}

Json edit_json(const an::EditVector& e) {
    return Json{{"exposure_ev", number(e.exposure_ev)}, {"red_gain", number(e.red_gain)},
                {"blue_gain", number(e.blue_gain)},     {"shadows_ev", number(e.shadows_ev)},
                {"highlights_ev", number(e.highlights_ev)}, {"contrast", number(e.contrast)},
                {"saturation", number(e.saturation)}};
}

Json terms_json(const an::ObjectiveTerms& t) {
    return Json{{"brightness", number(t.brightness)},
                {"highlights", number(t.highlights)},
                {"shadows", number(t.shadows)},
                {"noise", number(t.noise)},
                {"contrast", number(t.contrast)},
                {"local_contrast", number(t.local_contrast)},
                {"color", number(t.color)},
                {"white_balance", number(t.white_balance)},
                {"native", number(t.native)},
                {"edit", number(t.edit)},
                {"consistency", number(t.consistency)},
                {"total", number(t.total)}};
}

Json appearance_json(const an::Appearance& a) {
    if (!a.valid) {
        return nullptr;
    }
    return Json{{"lightness", number(a.lightness)},
                {"u", a.colour ? number(a.u) : Json(nullptr)},
                {"v", a.colour ? number(a.v) : Json(nullptr)},
                {"midtone_pixels", a.midtone_pixels},
                {"log_red", a.camera_gains ? number(a.log_red) : Json(nullptr)},
                {"log_blue", a.camera_gains ? number(a.log_blue) : Json(nullptr)}};
}

Json score_json(const an::CandidateScore& s) {
    if (!s.evaluated) {
        return nullptr;
    }
    return Json{{"feasible", s.feasible},
                {"total", number(s.terms.total)},
                {"terms", terms_json(s.terms)},
                {"median_lightness", number(s.median_lightness)},
                {"new_clip_fraction", number(s.new_clip_fraction)},
                {"failures", s.failures}};
}

Json consistency_terms_json(const an::ConsistencyTerms& t) {
    return Json{{"lightness", number(t.lightness)},
                {"colour", number(t.colour)},
                {"white_balance", number(t.white_balance)},
                {"total", number(t.total)}};
}

Json target_json(const an::ConsistencyTarget& t) {
    return Json{{"group", t.group_id},
                {"version", t.version},
                {"weight", number(t.weight)},
                {"contributors", t.contributors},
                {"anchored", t.anchored},
                {"lightness", number(t.lightness)},
                {"colour", t.colour ? Json{{"u", number(t.u)}, {"v", number(t.v)}} : Json(nullptr)},
                {"camera_white_balance",
                 t.camera_white_balance ? Json{{"domain", t.domain},
                                               {"log_red", number(t.log_red)},
                                               {"log_blue", number(t.log_blue)},
                                               {"contributors", t.domain_contributors}}
                                        : Json(nullptr)},
                {"white_balance_confidence", number(t.white_balance_confidence)}};
}

Json consistency_json(const an::ConsistencyRecord& k) {
    return Json{{"target", target_json(k.target)},
                {"independent_candidate", k.independent_chosen},
                {"independent_appearance", appearance_json(k.independent_appearance)},
                {"independent_terms", consistency_terms_json(k.independent_terms)},
                {"independent_total", number(k.independent_total)},
                {"appearance", appearance_json(k.appearance)},
                {"terms", consistency_terms_json(k.terms)},
                {"total", number(k.total)},
                {"adjustment", edit_json(k.adjustment)},
                {"changed", k.changed},
                {"added_candidates", k.added_candidates},
                {"white_balance_axis", k.white_balance_axis},
                {"axis_gains", Json::array({number(k.axis_red_gain), number(k.axis_blue_gain)})}};
}

Json shot_json(const an::ShotFeatures& s) {
    return Json{{"capture_time", s.capture_time.empty() ? Json(nullptr) : Json(s.capture_time)},
                {"camera", s.camera},
                {"calibration_domain", s.calibration_domain},
                {"size", Json::array({s.size.width, s.size.height})},
                {"exposure_ev100", optional_number(s.exposure_ev100)},
                {"median_signal", number(s.median_signal)},
                {"scene_ev", optional_number(s.scene_ev)},
                {"white_kelvin", optional_number(s.white_kelvin)},
                {"white_uv", s.white_u && s.white_v ? Json::array({number(*s.white_u), number(*s.white_v)})
                                                    : Json(nullptr)},
                {"rendered_uv", s.rendered_colour ? Json::array({number(s.rendered_u), number(s.rendered_v)})
                                                  : Json(nullptr)},
                {"descriptors", s.descriptors}};
}

Json comparison_json(const an::ShotComparison& c) {
    return Json{{"gap_seconds", optional_number(c.gap_seconds)},
                {"comparable", c.comparable},
                {"structure_ev", number(c.structure_ev)},
                {"colour_layout", number(c.colour_layout)},
                {"tone_distance", number(c.tone_distance)},
                {"similar", c.similar},
                {"scene_ev_change", optional_number(c.scene_ev_change)},
                {"white_uv_change", optional_number(c.white_uv_change)},
                {"rendered_uv_change", optional_number(c.rendered_uv_change)},
                {"illumination_verified", c.illumination_verified},
                {"illumination_change", c.illumination_change},
                {"reason", c.reason}};
}

Json result_json(const an::AutoResult& r) {
    const an::CandidateRecord& chosen = r.candidates.at(r.chosen);
    Json flags = Json::array();
    for (const an::ReviewFlag& f : r.flags) {
        flags.push_back(Json{{"code", std::string(an::to_string(f.code))},
                             {"metric", f.metric ? Json(std::string(an::to_string(*f.metric))) : Json(nullptr)},
                             {"reason", core::sanitize_utf8(f.reason)}});
    }
    Json diagnostics = Json::array();
    for (const an::MetricResult& m : r.diagnostics) {
        diagnostics.push_back(Json{{"metric", std::string(an::to_string(m.id))},
                                   {"status", std::string(an::to_string(m.status))},
                                   {"valid_samples", m.valid_samples},
                                   {"values", named_values(m.values)},
                                   {"detail", core::sanitize_utf8(m.detail)}});
    }
    const an::LightnessQuantiles& q = r.tone.baseline;
    Json tone{{"evidence", r.tone.evidence},
              {"signal_evidence", number(r.tone.signal_evidence)},
              {"baseline_quantiles",
               Json{{"p01", number(q.p01)}, {"p05", number(q.p05)}, {"p10", number(q.p10)}, {"p25", number(q.p25)},
                    {"p50", number(q.p50)}, {"p75", number(q.p75)}, {"p90", number(q.p90)}, {"p95", number(q.p95)},
                    {"p99", number(q.p99)}, {"p995", number(q.p995)}}},
              {"scene_ev", optional_number(r.tone.scene_ev)},
              {"low_key", number(r.tone.low_key)},
              {"dimness", number(r.tone.dimness)},
              {"high_key", number(r.tone.high_key)},
              {"band", Json::array({number(r.tone.band_low), number(r.tone.band_high)})},
              {"bright_floor", number(r.tone.bright_floor)},
              {"median_deficit_ev", number(r.tone.median_deficit_ev)},
              {"bright_deficit_ev", number(r.tone.bright_deficit_ev)},
              {"excess_ev", number(r.tone.excess_ev)},
              {"key_ambiguous", r.tone.key_ambiguous},
              {"needed_ev", number(r.tone.needed_ev)},
              {"headroom_ev", number(r.tone.headroom_ev)},
              {"exposure_ev", number(r.tone.exposure_ev)},
              {"shadows_ev", number(r.tone.shadows_ev)},
              {"highlights_ev", number(r.tone.highlights_ev)},
              {"reason", r.tone.reason}};
    Json wb = nullptr;
    if (r.white_balance) {
        const an::WhiteBalanceProposal& w = *r.white_balance;
        Json estimates = Json::array();
        for (const an::WhiteBalanceEstimate& e : w.estimates) {
            estimates.push_back(Json{{"method", std::string(an::to_string(e.method))},
                                     {"available", e.available},
                                     {"samples", e.samples},
                                     {"log_rg", number(e.log_rg)},
                                     {"log_bg", number(e.log_bg)},
                                     {"confidence", number(e.confidence)},
                                     {"detail", e.detail}});
        }
        wb = Json{{"evidence", w.evidence},
                  {"cast", Json::array({number(w.cast_log_rg), number(w.cast_log_bg)})},
                  {"warmth", number(w.warmth)},
                  {"tint", number(w.tint)},
                  {"spread", number(w.spread)},
                  {"confidence", number(w.confidence)},
                  {"gains", Json::array({number(w.red_gain), number(w.blue_gain)})},
                  {"preserved_fraction", number(w.preserved_fraction)},
                  {"reason", w.reason},
                  {"estimates", estimates}};
    }
    Json candidates = Json::array();
    for (const an::CandidateRecord& c : r.candidates) {
        candidates.push_back(Json{{"index", c.index},
                                  {"origin", std::string(an::to_string(c.origin))},
                                  {"source", c.source ? Json(*c.source) : Json(nullptr)},
                                  {"note", c.note},
                                  {"edit", edit_json(c.edit)},
                                  {"edit_size", number(c.edit_size)},
                                  {"proxy", score_json(c.proxy)},
                                  {"analysis", score_json(c.analysis)}});
    }
    const an::AutoTiming& t = r.timing;
    Json timing{{"decode_seconds", number(t.decode_seconds)},
                {"source_seconds", number(t.source_seconds)},
                {"stage_one_seconds", number(t.stage_one_seconds)},
                {"proxies_seconds", number(t.proxies_seconds)},
                {"crops_seconds", number(t.crops_seconds)},
                {"baseline_seconds", number(t.baseline_seconds)},
                {"proposals_seconds", number(t.proposals_seconds)},
                {"search_seconds", number(t.search_seconds)},
                {"recheck_seconds", number(t.recheck_seconds)},
                {"consistency_seconds", number(t.consistency_seconds)},
                {"total_seconds", number(t.total_seconds)},
                {"proxy_candidates", t.proxy_candidates},
                {"analysis_candidates", t.analysis_candidates},
                {"crop_renders", t.crop_renders},
                {"halo_free_crop_renders", t.halo_free_crop_renders},
                {"consistency_candidates", t.consistency_candidates}};
    const an::AutoMemory& m = r.memory;
    Json memory{{"stage_one_peak_estimate", m.stage_one_peak_estimate},
                {"full_resolution_bytes", m.full_resolution_bytes},
                {"analysis_bytes", m.analysis_bytes},
                {"proxy_bytes", m.proxy_bytes},
                {"crop_bytes", m.crop_bytes},
                {"retained_bytes", m.retained_bytes},
                {"estimated_peak_bytes", m.estimated_peak_bytes}};
    Json crops = Json::array();
    for (const an::NativeCrop& c : r.crops) {
        crops.push_back(Json{{"kind", std::string(an::to_string(c.kind))},
                             {"evaluation", Json::array({c.evaluation.x, c.evaluation.y, c.evaluation.width,
                                                         c.evaluation.height})},
                             {"statistic", number(c.statistic)}});
    }
    return Json{{"settings", settings_json(r.settings, chosen.edit)},
                {"chosen_candidate", r.chosen},
                {"chosen_origin", std::string(an::to_string(chosen.origin))},
                {"confidence", number(r.confidence)},
                {"confidence_detail",
                 Json{{"source", number(r.confidence_detail.source)},
                      {"brightness", number(r.confidence_detail.brightness)},
                      {"white_balance", number(r.confidence_detail.white_balance)},
                      {"noise", number(r.confidence_detail.noise)},
                      {"native", number(r.confidence_detail.native)},
                      {"policy", number(r.confidence_detail.policy)},
                      {"reasons", r.confidence_detail.reasons}}},
                {"restrained", r.restrained},
                {"fallback", r.fallback},
                {"flags", flags},
                {"appearance", appearance_json(r.appearance)},
                {"diagnostics", diagnostics},
                {"proposals", Json{{"tone", tone}, {"white_balance", wb}}},
                {"consistency", r.consistency ? consistency_json(*r.consistency) : Json(nullptr)},
                {"warnings", r.warnings},
                {"sizes", Json{{"full", Json::array({r.full_size.width, r.full_size.height})},
                               {"analysis", Json::array({r.analysis_size.width, r.analysis_size.height})},
                               {"proxy", Json::array({r.proxy_size.width, r.proxy_size.height})}}},
                {"crops", crops},
                {"timing", timing},
                {"memory", memory},
                {"candidates", candidates}};
}

Json versions_json(const an::AutoVersions& v) {
    return Json{{"policy", v.policy},
                {"policy_version", v.policy_version},
                {"search_version", v.search_version},
                {"metric_contract_version", v.metric_contract_version},
                {"develop_pipeline_version", v.develop_pipeline_version},
                {"develop_settings_version", v.develop_settings_version},
                {"consistency_version", v.consistency_version}};
}

std::string flag_list(const an::AutoResult& r) {
    std::string out;
    for (const an::ReviewFlag& f : r.flags) {
        out += std::format("{}{}", out.empty() ? "" : ",", an::to_string(f.code));
    }
    return out.empty() ? "-" : out;
}

std::string signed2(double v) {
    return std::format("{:+.2f}", std::abs(v) < 0.005 ? 0.0 : v);
}

// ---- HTML --------------------------------------------------------------------------------------

constexpr const char* kStyle = R"CSS(
:root{--bg:#f6f5f2;--card:#ffffff;--ink:#1d1d1f;--muted:#6b6b70;--line:#e2e0da;--accent:#2f5d8a;
--flag:#8a4b0f;--flag-bg:#fbf1e4;--ok:#2e6b3a;--chip:#eef2f7}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){--bg:#141416;--card:#1d1d21;--ink:#ececf0;
--muted:#9a9aa3;--line:#2e2e35;--accent:#8fb6de;--flag:#f0b878;--flag-bg:#33281b;--ok:#8fd09c;--chip:#262b33}}
:root[data-theme="dark"]{--bg:#141416;--card:#1d1d21;--ink:#ececf0;--muted:#9a9aa3;--line:#2e2e35;
--accent:#8fb6de;--flag:#f0b878;--flag-bg:#33281b;--ok:#8fd09c;--chip:#262b33}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif}
header,main{max-width:1280px;margin:0 auto;padding:16px}
h1{font-size:1.5rem;margin:8px 0 4px}h2{font-size:1.15rem;margin:28px 0 8px;color:var(--accent)}
h3{font-size:1rem;margin:0 0 8px}
.meta{color:var(--muted);font-weight:400;font-size:.9rem}
.lede{color:var(--muted);margin:0;max-width:72ch}
.item{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px;margin:12px 0}
.pair{display:grid;grid-template-columns:1fr 1fr;gap:10px}
@media (max-width:720px){.pair{grid-template-columns:1fr}}
figure{margin:0}figure img{width:100%;height:auto;display:block;border-radius:6px;background:#000}
figcaption{font-size:.85rem;color:var(--muted);margin-top:4px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:6px 14px;margin:10px 0 0;padding:0}
.grid div{display:flex;justify-content:space-between;border-bottom:1px dotted var(--line);padding:2px 0}
.grid dt{color:var(--muted)}.grid dd{margin:0;font-variant-numeric:tabular-nums}
ul.flags{list-style:none;padding:0;margin:10px 0 0;display:flex;flex-direction:column;gap:4px}
ul.flags li{background:var(--flag-bg);color:var(--flag);border-radius:6px;padding:4px 8px;font-size:.85rem;overflow-wrap:anywhere}
ul.flags b{font-weight:600}
.chip{display:inline-block;background:var(--chip);border-radius:999px;padding:1px 8px;font-size:.8rem;margin-left:6px}
.error{color:var(--flag)}
.choice{display:flex;flex-wrap:wrap;gap:14px;margin-top:10px;align-items:center}
.choice label{display:flex;gap:6px;align-items:center;cursor:pointer}
.choice input[type=text]{flex:1;min-width:200px;padding:4px 8px;border:1px solid var(--line);border-radius:6px;
background:var(--bg);color:var(--ink)}
button{font:inherit;padding:8px 14px;border-radius:8px;border:1px solid var(--accent);background:var(--accent);
color:var(--card);cursor:pointer}
)CSS";

std::string img_tag(const std::filesystem::path& out_dir, const std::string& relative, JpegReader jpeg,
                    const std::string& alt) {
    if (relative.empty()) {
        return "<p class=\"error\">no preview</p>";
    }
    const std::vector<std::uint8_t> bytes = jpeg(out_dir, relative);
    if (bytes.empty()) {
        return "<p class=\"error\">preview missing</p>";
    }
    return std::format("<img loading=\"lazy\" alt=\"{}\" src=\"data:image/jpeg;base64,{}\">", html_escape(alt),
                       base64(bytes));
}

std::string head(const std::string& title) {
    return std::format("<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
                       "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                       "<title>{}</title><style>{}</style></head><body>\n",
                       html_escape(title), kStyle);
}

std::string row(const std::string& label, const std::string& value) {
    return std::format("<div><dt>{}</dt><dd>{}</dd></div>", html_escape(label), html_escape(value));
}

}  // namespace

std::string item_stem(std::size_t index, const std::string& name) {
    return std::format("{:03}-{}", index, name);
}

std::string base64(std::span<const std::uint8_t> bytes) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const std::uint32_t v = (std::uint32_t{bytes[i]} << 16) | (std::uint32_t{bytes[i + 1]} << 8) | bytes[i + 2];
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    if (i + 1 == bytes.size()) {
        const std::uint32_t v = std::uint32_t{bytes[i]} << 16;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == bytes.size()) {
        const std::uint32_t v = (std::uint32_t{bytes[i]} << 16) | (std::uint32_t{bytes[i + 1]} << 8);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

std::string html_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : core::sanitize_utf8(text)) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&#39;";
                break;
            default:
                out += c;
        }
    }
    return out;
}

exporter::OutputImage side_by_side(const exporter::OutputImage& left, const exporter::OutputImage& right) {
    constexpr std::int32_t kGap = 16;
    constexpr std::uint8_t kBackground = 28;
    exporter::OutputImage out;
    out.width = left.width + kGap + right.width;
    out.height = std::max(left.height, right.height);
    out.bit_depth = exporter::BitDepth::Eight;
    out.profile = left.profile;
    out.samples8.assign(out.row_samples() * static_cast<std::size_t>(out.height), kBackground);
    const auto blit = [&](const exporter::OutputImage& src, std::int32_t x0) {
        for (std::int32_t y = 0; y < src.height; ++y) {
            const auto* from = src.samples8.data() + static_cast<std::size_t>(y) * src.row_samples();
            auto* to = out.samples8.data() + static_cast<std::size_t>(y) * out.row_samples() +
                       static_cast<std::size_t>(x0) * 3;
            std::copy(from, from + src.row_samples(), to);
        }
    };
    blit(left, 0);
    blit(right, left.width + kGap);
    return out;
}

bool visibly_identical(const exporter::OutputImage& a, const exporter::OutputImage& b) {
    if (a.width != b.width || a.height != b.height || a.samples8.size() != b.samples8.size() ||
        a.bit_depth != exporter::BitDepth::Eight || b.bit_depth != exporter::BitDepth::Eight) {
        return false;
    }
    for (std::size_t i = 0; i < a.samples8.size(); ++i) {
        if (std::abs(static_cast<int>(a.samples8[i]) - static_cast<int>(b.samples8[i])) > kUnchangedCodeValues) {
            return false;
        }
    }
    return true;
}

Json item_json(const ItemReport& item) {
    const an::RelatedItemResult* r = item.result;
    Json j{{"schema", kItemSchema},
           {"schema_version", kSchemaVersion},
           {"index", item.index},
           {"name", item.input.name},
           {"source",
            Json{{"path", core::path_to_utf8_lossy(item.input.path)},
                 {"file_name", core::path_to_utf8_lossy(item.input.path.filename())},
                 {"subimage", item.subimage ? Json(*item.subimage) : Json(nullptr)},
                 {"subimage_count", item.subimage_count},
                 {"fixture", item.input.fixture.empty()
                                 ? Json(nullptr)
                                 : Json{{"id", item.input.fixture}, {"tier", item.input.tier}}}}},
           {"status", r != nullptr && !r->error ? "ok" : "error"},
           {"error", r != nullptr && r->error ? render_tool::error_json(*r->error) : Json(nullptr)},
           {"group", Json{{"id", item.group_id},
                          {"basis", std::string(an::to_string(item.group_basis))},
                          {"size", item.group_size},
                          {"decision", item.decision}}},
           {"previews", Json{{"as_shot", item.as_shot_jpg.empty() ? Json(nullptr) : Json(item.as_shot_jpg)},
                             {"auto", item.auto_jpg.empty() ? Json(nullptr) : Json(item.auto_jpg)},
                             {"pair", item.pair_jpg.empty() ? Json(nullptr) : Json(item.pair_jpg)},
                             {"unchanged", item.unchanged},
                             {"error", item.preview_error ? render_tool::error_json(*item.preview_error)
                                                          : Json(nullptr)},
                             {"source", "analysis image (interactive stage one, stage two, box-reduced), sRGB "
                                        "8-bit JPEG"}}}};
    if (r == nullptr || r->error || !r->independent) {
        return j;
    }
    const an::AutoResult& final_result = *r->final_result();
    j["versions"] = versions_json(final_result.versions);
    j["shot"] = shot_json(r->independent->shot);
    j["seconds"] = number(r->seconds);
    j["regularised"] = r->regularised.has_value();
    j["consistency_error"] = r->consistency_error ? render_tool::error_json(*r->consistency_error) : Json(nullptr);
    j["result"] = result_json(final_result);
    if (r->regularised) {
        const an::AutoResult& ind = *r->independent;
        j["independent"] = Json{{"settings", settings_json(ind.settings, ind.candidates.at(ind.chosen).edit)},
                                {"confidence", number(ind.confidence)},
                                {"flags", flag_list(ind)},
                                {"appearance", appearance_json(ind.appearance)},
                                {"total_seconds", number(ind.timing.total_seconds)}};
    }
    return j;
}

Json run_json(const Arguments& args, const Discovery& discovery, const an::RelatedResult& result,
              std::span<const ItemReport> items, double seconds) {
    Json inputs = Json::array();
    for (const auto& p : args.inputs) {
        inputs.push_back(core::path_to_utf8_lossy(p));
    }
    Json skipped = Json::array();
    for (const SkippedFile& s : discovery.skipped) {
        skipped.push_back(Json{{"path", core::path_to_utf8_lossy(s.path)}, {"reason", s.reason}});
    }
    Json groups = Json::array();
    for (const an::RelatedGroup& g : result.grouping.groups) {
        Json members = Json::array();
        for (const std::size_t m : g.members) {
            members.push_back(Json{{"index", m}, {"name", items[m].input.name}});
        }
        groups.push_back(Json{{"id", g.id}, {"basis", std::string(an::to_string(g.basis))}, {"members", members}});
    }
    Json summary = Json::array();
    std::size_t failed = 0;
    std::size_t regularised = 0;
    for (const ItemReport& item : items) {
        const an::RelatedItemResult* r = item.result;
        Json s{{"index", item.index}, {"name", item.input.name}, {"item", item.item_json},
               {"group", item.group_id}};
        if (r == nullptr || r->error || !r->independent) {
            ++failed;
            s["status"] = "error";
            s["error"] = r != nullptr && r->error ? render_tool::error_json(*r->error) : Json(nullptr);
        } else {
            const an::AutoResult& f = *r->final_result();
            const an::EditVector& e = f.candidates.at(f.chosen).edit;
            regularised += r->regularised ? 1u : 0u;
            s["status"] = "ok";
            s["exposure_ev"] = number(e.exposure_ev);
            s["red_gain"] = number(e.red_gain);
            s["blue_gain"] = number(e.blue_gain);
            s["shadows_ev"] = number(e.shadows_ev);
            s["highlights_ev"] = number(e.highlights_ev);
            s["contrast"] = number(e.contrast);
            s["saturation"] = number(e.saturation);
            s["confidence"] = number(f.confidence);
            s["flags"] = flag_list(f);
            s["regularised"] = r->regularised.has_value();
            s["seconds"] = number(r->seconds);
            s["analysis_seconds"] = number(f.timing.total_seconds);
        }
        summary.push_back(std::move(s));
    }
    Json decisions = Json::array();
    for (std::size_t i = 0; i < result.grouping.decisions.size(); ++i) {
        const an::GroupDecision& d = result.grouping.decisions[i];
        decisions.push_back(Json{{"index", i},
                                 {"grouped", d.grouped},
                                 {"joined", d.joined},
                                 {"previous", d.previous ? Json(*d.previous) : Json(nullptr)},
                                 {"reason", d.reason},
                                 {"comparison", d.comparison ? comparison_json(*d.comparison) : Json(nullptr)}});
    }
    const an::AutoVersions versions;
    return Json{{"schema", kRunSchema},
                {"schema_version", kSchemaVersion},
                {"generator", render_tool::generator_json()},
                {"arguments",
                 Json{{"inputs", inputs},
                      {"out", core::path_to_utf8_lossy(args.out_dir)},
                      {"group_by", args.group_by == an::GroupingMode::None ? "none" : "time"},
                      {"tier", !args.tier ? Json(nullptr) : Json(*args.tier == Tier::Smoke ? "smoke" : "all")},
                      {"max_gap_seconds", number(args.max_gap_seconds)},
                      {"preview_edge", args.preview_edge},
                      {"seed", args.seed}}},
                {"versions", versions_json(versions)},
                {"skipped", skipped},
                {"groups", groups},
                {"decisions", decisions},
                {"items", summary},
                {"totals", Json{{"items", items.size()},
                                {"failed", failed},
                                {"regularised", regularised},
                                {"reopened_sessions", result.reopened},
                                {"peak_session_bytes", result.peak_session_bytes},
                                {"analysis_seconds", number(result.seconds)},
                                {"seconds", number(seconds)}}}};
}

std::string summary_tsv(std::span<const ItemReport> items) {
    std::string out =
        "index\tname\ttier\tcamera\tgroup\tstatus\texposure_ev\tred_gain\tblue_gain\tshadows_ev\thighlights_ev\t"
        "contrast\tsaturation\tconfidence\tflags\tregularised\tconsistency_before\tconsistency_after\tseconds\n";
    for (const ItemReport& item : items) {
        const an::RelatedItemResult* r = item.result;
        if (r == nullptr || r->error || !r->independent) {
            out += std::format("{}\t{}\t{}\t\t{}\terror\t\t\t\t\t\t\t\t\t{}\t\t\t\t\n", item.index, item.input.name,
                               item.input.tier, item.group_id,
                               r != nullptr && r->error ? std::string(to_string(r->error->code)) : "");
            continue;
        }
        const an::AutoResult& f = *r->final_result();
        const an::EditVector& e = f.candidates.at(f.chosen).edit;
        const auto* k = f.consistency ? &*f.consistency : nullptr;
        out += std::format("{}\t{}\t{}\t{}\t{}\tok\t{:.3f}\t{:.4f}\t{:.4f}\t{:.3f}\t{:.3f}\t{:.3f}\t{:.3f}\t{:.2f}\t{}\t{}"
                           "\t{}\t{}\t{:.2f}\n",
                           item.index, item.input.name, item.input.tier, r->independent->shot.camera, item.group_id,
                           e.exposure_ev, e.red_gain, e.blue_gain, e.shadows_ev, e.highlights_ev, e.contrast,
                           e.saturation, f.confidence, flag_list(f), r->regularised ? "yes" : "no",
                           k ? std::format("{:.3f}", k->independent_terms.total) : "",
                           k ? std::format("{:.3f}", k->terms.total) : "", r->seconds);
    }
    return out;
}

std::string summary_table(std::span<const ItemReport> items) {
    std::string out = std::format("{:>3}  {:<38} {:<10} {:>6} {:>6} {:>6} {:>6} {:>6} {:>5} {:>6}  {}\n", "#",
                                  "name", "group", "EV", "red", "blue", "shad", "high", "conf", "time", "flags");
    for (const ItemReport& item : items) {
        const an::RelatedItemResult* r = item.result;
        const std::string name = item.input.name.substr(0, 38);
        if (r == nullptr || r->error || !r->independent) {
            out += std::format("{:>3}  {:<38} {:<10} error: {}\n", item.index, name, item.group_id,
                               r != nullptr && r->error ? core::sanitize_utf8(r->error->describe()) : "");
            continue;
        }
        const an::AutoResult& f = *r->final_result();
        const an::EditVector& e = f.candidates.at(f.chosen).edit;
        out += std::format("{:>3}  {:<38} {:<10} {:>6} {:>6.3f} {:>6.3f} {:>6} {:>6} {:>5.2f} {:>5.1f}s  {}{}\n",
                           item.index, name, item.group_id.substr(0, 10), signed2(e.exposure_ev), e.red_gain,
                           e.blue_gain, signed2(e.shadows_ev), signed2(e.highlights_ev), f.confidence, r->seconds,
                           r->regularised ? "[group] " : "", flag_list(f));
    }
    return out;
}

std::string sheet_html(const Arguments& args, const an::RelatedResult& result, std::span<const ItemReport> items,
                       const std::filesystem::path& out_dir, JpegReader jpeg) {
    std::string html = head("Natural v1 evaluation");
    std::size_t ok = 0;
    for (const ItemReport& item : items) {
        ok += item.result != nullptr && item.result->independent ? 1u : 0u;
    }
    const an::AutoVersions v;
    html += std::format(
        "<header><h1>Natural v1 automatic settings</h1><p class=\"lede\">{} images ({} analysed) in {} groups; "
        "grouping {}. Left: as shot. Right: the automatic settings (policy {} v{}, search v{}, consistency v{}). "
        "Both are reduced renders of the 1536-pixel analysis image the optimiser judged, not export renders. The "
        "thresholds are uncalibrated (ADR 0011).</p></header><main>\n",
        items.size(), ok, result.grouping.groups.size(),
        args.group_by == an::GroupingMode::None ? "off" : "by capture time", v.policy, v.policy_version,
        v.search_version, v.consistency_version);
    std::vector<bool> shown(items.size(), false);
    const auto article = [&](const ItemReport& item) {
        const an::RelatedItemResult* r = item.result;
        std::string a = std::format("<article class=\"item\" id=\"item-{:03}\"><h3>{:03} {}", item.index, item.index,
                                    html_escape(item.input.name));
        if (r != nullptr && r->independent) {
            const an::ShotFeatures& s = r->independent->shot;
            a += std::format(" <span class=\"meta\">{}{}</span>", html_escape(s.camera),
                             s.capture_time.empty() ? "" : " &middot; " + html_escape(s.capture_time));
        }
        if (!item.input.tier.empty()) {
            a += std::format("<span class=\"chip\">{}</span>", html_escape(item.input.tier));
        }
        a += "</h3>";
        if (r == nullptr || r->error || !r->independent) {
            a += std::format("<p class=\"error\">Not analysed: {}</p></article>\n",
                             r != nullptr && r->error ? html_escape(r->error->describe()) : "unknown");
            return a;
        }
        const an::AutoResult& f = *r->final_result();
        const an::EditVector& e = f.candidates.at(f.chosen).edit;
        a += "<div class=\"pair\"><figure>" + img_tag(out_dir, item.as_shot_jpg, jpeg, "As shot") +
             "<figcaption>As shot</figcaption></figure><figure>" + img_tag(out_dir, item.auto_jpg, jpeg, "Natural v1") +
             "<figcaption>Natural v1 automatic</figcaption></figure></div>";
        a += "<dl class=\"grid\">";
        a += row("Exposure", signed2(e.exposure_ev) + " EV");
        a += row("White balance", e.red_gain == 1.0 && e.blue_gain == 1.0
                                      ? std::string("as shot")
                                      : std::format("R x{:.3f}, B x{:.3f}", e.red_gain, e.blue_gain));
        a += row("Shadows", signed2(e.shadows_ev) + " EV");
        a += row("Highlights", signed2(e.highlights_ev) + " EV");
        a += row("Contrast", signed2(e.contrast));
        a += row("Saturation", std::format("x{:.3f}", e.saturation));
        a += row("Confidence", std::format("{:.2f}{}", f.confidence, f.restrained ? " (restrained)" : ""));
        a += row("Median L*", std::format("{:.1f} (as shot {:.1f})", 100.0 * f.appearance.lightness,
                                          100.0 * f.candidates.at(0).analysis.median_lightness));
        a += row("Time", std::format("{:.2f} s", r->seconds));
        a += row("Chosen", std::string(an::to_string(f.candidates.at(f.chosen).origin)) +
                               (item.unchanged ? " (no visible change)" : ""));
        if (f.consistency) {
            const an::ConsistencyRecord& k = *f.consistency;
            a += row("Consistency", std::format("{:.2f} to {:.2f}", k.independent_terms.total, k.terms.total));
            a += row("Group change", std::format("{} EV{}", signed2(k.adjustment.exposure_ev),
                                                 k.white_balance_axis.empty() ? "" : ", WB " + k.white_balance_axis));
        }
        a += "</dl>";
        if (!f.flags.empty()) {
            a += "<ul class=\"flags\">";
            for (const an::ReviewFlag& flag : f.flags) {
                a += std::format("<li><b>{}</b> {}</li>", html_escape(an::to_string(flag.code)),
                                 html_escape(flag.reason));
            }
            a += "</ul>";
        }
        if (item.preview_error) {
            a += std::format("<p class=\"error\">Preview: {}</p>", html_escape(item.preview_error->describe()));
        }
        return a + "</article>\n";
    };
    // Groups of two or more first, then single images.
    for (const bool related : {true, false}) {
        if (!related) {
            html += "<h2>Single images</h2>\n";
        }
        for (const an::RelatedGroup& g : result.grouping.groups) {
            if ((g.members.size() >= 2) != related) {
                continue;
            }
            if (related) {
                html += std::format("<h2>Group {} <span class=\"meta\">{}, {} images</span></h2>\n", html_escape(g.id),
                                    html_escape(an::to_string(g.basis)), g.members.size());
            }
            for (const std::size_t m : g.members) {
                html += article(items[m]);
                shown[m] = true;
            }
        }
    }
    bool header = false;
    for (const ItemReport& item : items) {
        if (!shown[item.index]) {
            if (!header) {
                html += "<h2>Not analysed</h2>\n";
                header = true;
            }
            html += article(item);
        }
    }
    return html + "</main></body></html>\n";
}

bool auto_first(std::uint64_t seed, std::size_t index) noexcept {
    // splitmix64 of the seed and the index: a fixed, documented order.
    std::uint64_t x = seed ^ (0x9E3779B97F4A7C15ull * (static_cast<std::uint64_t>(index) + 1));
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    return (x & 1u) != 0;
}

std::string blind_html(const Arguments& args, std::span<const ItemReport> items, const std::filesystem::path& out_dir,
                       JpegReader jpeg) {
    std::string html = head("Blind preference sheet");
    std::size_t unchanged = 0;
    for (const ItemReport& item : items) {
        unchanged += !item.as_shot_jpg.empty() && !item.auto_jpg.empty() && item.unchanged ? 1u : 0u;
    }
    html += std::format(
        "<header><h1>Blind preference sheet</h1><p class=\"lede\">For each photograph, choose the rendering you "
        "prefer, or \"No preference\". The order of A and B was drawn per photograph (seed {}); the key is kept "
        "elsewhere. Export your choices when done.{}</p></header><main>\n",
        args.seed,
        unchanged == 0 ? std::string()
                       : std::format(" {} photograph{} without a visible difference between the two renderings "
                                     "{} not shown.",
                                     unchanged, unchanged == 1 ? "" : "s", unchanged == 1 ? "is" : "are"));
    for (const ItemReport& item : items) {
        if (item.as_shot_jpg.empty() || item.auto_jpg.empty() || item.unchanged) {
            continue;
        }
        const bool first = auto_first(args.seed, item.index);
        const std::string& a = first ? item.auto_jpg : item.as_shot_jpg;
        const std::string& b = first ? item.as_shot_jpg : item.auto_jpg;
        const std::string id = std::format("{:03}", item.index);
        html += std::format("<article class=\"item\" data-item=\"{}\" data-name=\"{}\"><h3>{} {}</h3>", item.index,
                            html_escape(item.input.name), id, html_escape(item.input.name));
        html += "<div class=\"pair\"><figure>" + img_tag(out_dir, a, jpeg, "A") +
                "<figcaption>A</figcaption></figure><figure>" + img_tag(out_dir, b, jpeg, "B") +
                "<figcaption>B</figcaption></figure></div>";
        html += std::format(
            "<div class=\"choice\"><label><input type=\"radio\" name=\"c{0}\" value=\"A\">A</label>"
            "<label><input type=\"radio\" name=\"c{0}\" value=\"B\">B</label>"
            "<label><input type=\"radio\" name=\"c{0}\" value=\"same\">No preference</label>"
            "<input type=\"text\" name=\"n{0}\" placeholder=\"Note (optional)\" aria-label=\"Note for {0}\"></div>"
            "</article>\n",
            id);
    }
    html += std::format(R"JS(<p><button type="button" id="export">Export choices</button></p></main>
<script>
document.getElementById('export').addEventListener('click', function () {{
  var choices = [];
  document.querySelectorAll('article[data-item]').forEach(function (a) {{
    var id = ('00' + a.dataset.item).slice(-3);
    var picked = a.querySelector('input[name="c' + id + '"]:checked');
    var note = a.querySelector('input[name="n' + id + '"]');
    choices.push({{item: Number(a.dataset.item), name: a.dataset.name, choice: picked ? picked.value : null,
                  note: note ? note.value : ''}});
  }});
  var doc = {{schema: '{}', schema_version: {}, seed: {}, choices: choices}};
  var blob = new Blob([JSON.stringify(doc, null, 2)], {{type: 'application/json'}});
  var link = document.createElement('a');
  link.href = URL.createObjectURL(blob);
  link.download = 'blind-choices.json';
  link.click();
}});
</script></body></html>
)JS",
                        kBlindChoicesSchema, kSchemaVersion, args.seed);
    return html;
}

Json blind_key(const Arguments& args, std::span<const ItemReport> items) {
    Json entries = Json::array();
    Json unchanged = Json::array();
    for (const ItemReport& item : items) {
        if (item.as_shot_jpg.empty() || item.auto_jpg.empty()) {
            continue;
        }
        if (item.unchanged) {
            unchanged.push_back(Json{{"item", item.index}, {"name", item.input.name}});
            continue;
        }
        const bool first = auto_first(args.seed, item.index);
        entries.push_back(Json{{"item", item.index},
                               {"name", item.input.name},
                               {"A", first ? "auto" : "as_shot"},
                               {"B", first ? "as_shot" : "auto"},
                               {"auto_preview", item.auto_jpg},
                               {"as_shot_preview", item.as_shot_jpg}});
    }
    return Json{{"schema", kBlindKeySchema},
                {"schema_version", kSchemaVersion},
                {"seed", args.seed},
                {"order", "splitmix64(seed xor golden * (index + 1)) & 1: auto is A when 1"},
                {"items", entries},
                {"unchanged_rule", std::format("settings equal to as shot, or 8-bit previews within {} code value",
                                               kUnchangedCodeValues)},
                {"unchanged", unchanged}};
}

}  // namespace lucent::auto_tool
