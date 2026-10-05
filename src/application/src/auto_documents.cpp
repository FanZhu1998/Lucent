// The documents of automatic settings (auto_develop.hpp): "lucent.auto-request" v1, "lucent.auto-run"
// v1 and the analysis record "lucent.auto-record" v1. Written with sorted members and shortest
// round-trip numbers; read strictly (every member required, unknown members refused).

#include <lucent/application/auto_develop.hpp>

#include <cmath>
#include <format>
#include <limits>
#include <new>
#include <set>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <lucent/application/recipe.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/core/text.hpp>

namespace lucent::app {

namespace {

using Json = nlohmann::json;

constexpr std::string_view kWhere = "app.auto.document";

struct DocumentError {
    ErrorCode code;
    std::string message;
};

[[noreturn]] void corrupt(std::string message) { throw DocumentError{ErrorCode::Corrupt, std::move(message)}; }

// A JSON object read member by member; finish() refuses members nobody asked for.
class Obj {
public:
    Obj(const Json& json, std::string path) : json_(json), path_(std::move(path)) {
        if (!json_.is_object()) {
            corrupt(std::format("{} is not an object", path_));
        }
    }

    const Json& member(const char* name) {
        const auto found = json_.find(name);
        if (found == json_.end()) {
            corrupt(std::format("{}.{} is missing", path_, name));
        }
        seen_.insert(name);
        return *found;
    }
    std::string text(const char* name) {
        const Json& v = member(name);
        if (!v.is_string()) {
            corrupt(std::format("{}.{} is not text", path_, name));
        }
        return v.get<std::string>();
    }
    std::optional<std::string> optional_text(const char* name) {
        const Json& v = member(name);
        if (v.is_null()) {
            return std::nullopt;
        }
        if (!v.is_string()) {
            corrupt(std::format("{}.{} is not text", path_, name));
        }
        return v.get<std::string>();
    }
    bool boolean(const char* name) {
        const Json& v = member(name);
        if (!v.is_boolean()) {
            corrupt(std::format("{}.{} is not a boolean", path_, name));
        }
        return v.get<bool>();
    }
    std::uint64_t unsigned_number(const char* name, std::uint64_t max = std::numeric_limits<std::uint64_t>::max()) {
        const Json& v = member(name);
        if (!v.is_number_unsigned() && !(v.is_number_integer() && v.get<std::int64_t>() >= 0)) {
            corrupt(std::format("{}.{} is not an unsigned integer", path_, name));
        }
        const auto value = v.get<std::uint64_t>();
        if (value > max) {
            corrupt(std::format("{}.{} is out of range", path_, name));
        }
        return value;
    }
    std::uint32_t u32(const char* name) {
        return static_cast<std::uint32_t>(unsigned_number(name, std::numeric_limits<std::uint32_t>::max()));
    }
    std::int64_t integer(const char* name) {
        const Json& v = member(name);
        if (!v.is_number_integer()) {
            corrupt(std::format("{}.{} is not an integer", path_, name));
        }
        return v.get<std::int64_t>();
    }
    double number(const char* name) {
        const Json& v = member(name);
        if (!v.is_number()) {
            corrupt(std::format("{}.{} is not a number", path_, name));
        }
        const double value = v.get<double>();
        if (!std::isfinite(value)) {
            corrupt(std::format("{}.{} is not finite", path_, name));
        }
        return value;
    }
    std::optional<double> optional_number(const char* name) {
        if (member(name).is_null()) {
            return std::nullopt;
        }
        return number(name);
    }
    const Json& array(const char* name) {
        const Json& v = member(name);
        if (!v.is_array()) {
            corrupt(std::format("{}.{} is not an array", path_, name));
        }
        return v;
    }
    Obj object(const char* name) { return Obj(member(name), path_ + "." + name); }
    bool is_null(const char* name) { return member(name).is_null(); }

    void finish() const {
        for (auto it = json_.begin(); it != json_.end(); ++it) {
            if (!seen_.contains(it.key())) {
                corrupt(std::format("{}.{} is not a member of this document", path_, it.key()));
            }
        }
    }
    const std::string& path() const noexcept { return path_; }

private:
    const Json& json_;
    std::string path_;
    std::set<std::string, std::less<>> seen_;
};

template <class Body>
auto guarded(std::string_view what, Body&& body) -> decltype(body()) {
    try {
        return body();
    } catch (const DocumentError& e) {
        return fail(e.code, std::format("the {} cannot be read: {}", what, e.message), std::string(kWhere));
    } catch (const Json::exception& e) {
        return fail(ErrorCode::Corrupt, std::format("the {} is not valid JSON: {}", what, e.what()), std::string(kWhere));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, std::format("out of memory with the {}", what), std::string(kWhere));
    }
}

Json parse(std::string_view text) {
    Json json = Json::parse(text.begin(), text.end());  // throws on invalid JSON or UTF-8
    if (!json.is_object()) {
        corrupt("the document is not a JSON object");
    }
    return json;
}

void check_header(Obj& in, std::string_view schema) {
    if (in.text("schema") != schema) {
        corrupt(std::format("not a '{}' document", schema));
    }
    const std::uint64_t version = in.unsigned_number("version");
    if (version > kAutoDocumentVersion) {
        throw DocumentError{ErrorCode::VersionMismatch,
                            std::format("version {} is newer than this build's version {} (written by a newer Lucent)",
                                        version, kAutoDocumentVersion)};
    }
    if (version != kAutoDocumentVersion) {
        corrupt(std::format("version {} is not version {}", version, kAutoDocumentVersion));
    }
}

std::string dump(const Json& json) {
    // Strict: text that is not valid UTF-8 throws instead of being replaced.
    return json.dump(-1, ' ', /*ensure_ascii=*/false, Json::error_handler_t::strict);
}

// Non-finite measurement values are written as null and read back as NaN.
Json measurement(double value) { return std::isfinite(value) ? Json(value) : Json(); }
double measurement_of(const Json& value, const std::string& where) {
    if (value.is_null()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (!value.is_number()) {
        corrupt(where + " is not a number");
    }
    return value.get<double>();
}

// ---- Values -------------------------------------------------------------------------------------

Json settings_json(const engine::DevelopSettings& settings) {
    auto document = write_recipe(settings);
    if (!document) {
        throw DocumentError{document.error().code, "settings: " + document.error().message};
    }
    return Json(std::move(document->json));
}

engine::DevelopSettings settings_of(Obj& in, const char* name) {
    auto settings = read_recipe(in.text(name));
    if (!settings) {
        corrupt(std::format("{}.{}: {}", in.path(), name, settings.error().message));
    }
    return std::move(*settings);
}

Json versions_json(const analysis::AutoVersions& v) {
    return Json{{"policy", v.policy},
                {"policy_version", v.policy_version},
                {"search_version", v.search_version},
                {"metric_contract_version", v.metric_contract_version},
                {"develop_pipeline_version", v.develop_pipeline_version},
                {"develop_settings_version", v.develop_settings_version},
                {"consistency_version", v.consistency_version}};
}

analysis::AutoVersions versions_of(Obj in) {
    analysis::AutoVersions v;
    v.policy = in.text("policy");
    v.policy_version = in.u32("policy_version");
    v.search_version = in.u32("search_version");
    v.metric_contract_version = in.u32("metric_contract_version");
    v.develop_pipeline_version = in.u32("develop_pipeline_version");
    v.develop_settings_version = in.u32("develop_settings_version");
    v.consistency_version = in.u32("consistency_version");
    in.finish();
    return v;
}

Json text_list(const std::vector<std::string>& texts) {
    Json out = Json::array();
    for (const std::string& text : texts) {
        out.push_back(core::sanitize_utf8(text));
    }
    return out;
}

std::vector<std::string> text_list_of(const Json& array, const std::string& where) {
    std::vector<std::string> out;
    for (const Json& value : array) {
        if (!value.is_string()) {
            corrupt(where + " holds a value that is not text");
        }
        out.push_back(value.get<std::string>());
    }
    return out;
}

Json confidence_json(const analysis::ConfidenceBreakdown& c) {
    return Json{{"overall", c.overall}, {"source", c.source},     {"brightness", c.brightness},
                {"white_balance", c.white_balance}, {"noise", c.noise}, {"native", c.native},
                {"policy", c.policy},   {"reasons", text_list(c.reasons)}};
}

analysis::ConfidenceBreakdown confidence_of(Obj in) {
    analysis::ConfidenceBreakdown c;
    c.overall = in.number("overall");
    c.source = in.number("source");
    c.brightness = in.number("brightness");
    c.white_balance = in.number("white_balance");
    c.noise = in.number("noise");
    c.native = in.number("native");
    c.policy = in.number("policy");
    c.reasons = text_list_of(in.array("reasons"), in.path() + ".reasons");
    in.finish();
    return c;
}

Json flags_json(const std::vector<AutoFlag>& flags) {
    Json out = Json::array();
    for (const AutoFlag& flag : flags) {
        out.push_back(Json{{"code", flag.code},
                           {"name", flag.name},
                           {"metric", flag.metric ? Json(*flag.metric) : Json()},
                           {"reason", core::sanitize_utf8(flag.reason)},
                           {"review", flag.review}});
    }
    return out;
}

std::vector<AutoFlag> flags_of(const Json& array, const std::string& where) {
    std::vector<AutoFlag> out;
    for (const Json& value : array) {
        Obj in(value, where + "[]");
        AutoFlag flag;
        flag.code = static_cast<std::uint16_t>(in.unsigned_number("code", 0xFFFF));
        flag.name = in.text("name");
        flag.metric = in.optional_text("metric");
        flag.reason = in.text("reason");
        flag.review = in.boolean("review");
        in.finish();
        out.push_back(std::move(flag));
    }
    return out;
}

Json named_values_json(const std::vector<analysis::NamedValue>& values) {
    Json out = Json::array();
    for (const analysis::NamedValue& v : values) {
        out.push_back(Json::array({v.name, measurement(v.value)}));
    }
    return out;
}

std::vector<analysis::NamedValue> named_values_of(const Json& array, const std::string& where) {
    std::vector<analysis::NamedValue> out;
    for (const Json& pair : array) {
        if (!pair.is_array() || pair.size() != 2 || !pair[0].is_string()) {
            corrupt(where + " holds an entry that is not [name, value]");
        }
        out.push_back(analysis::NamedValue{pair[0].get<std::string>(), measurement_of(pair[1], where)});
    }
    return out;
}

Json metrics_json(const std::vector<analysis::MetricResult>& metrics) {
    Json out = Json::array();
    for (const analysis::MetricResult& m : metrics) {
        out.push_back(Json{{"id", static_cast<unsigned>(m.id)},
                           {"status", static_cast<unsigned>(m.status)},
                           {"valid_samples", m.valid_samples},
                           {"values", named_values_json(m.values)},
                           {"detail", core::sanitize_utf8(m.detail)}});
    }
    return out;
}

std::vector<analysis::MetricResult> metrics_of(const Json& array, const std::string& where) {
    std::vector<analysis::MetricResult> out;
    for (const Json& value : array) {
        Obj in(value, where + "[]");
        analysis::MetricResult m;
        const std::uint64_t id = in.unsigned_number("id", 255);
        if (id < 1 || id > static_cast<std::uint64_t>(analysis::MetricId::ColorGamut)) {
            corrupt(where + ": an unknown metric");
        }
        m.id = static_cast<analysis::MetricId>(id);
        const std::uint64_t status = in.unsigned_number("status", 255);
        if (status < 1 || status > static_cast<std::uint64_t>(analysis::MetricStatus::NotApplicable)) {
            corrupt(where + ": an unknown metric status");
        }
        m.status = static_cast<analysis::MetricStatus>(status);
        m.valid_samples = in.unsigned_number("valid_samples");
        m.values = named_values_of(in.array("values"), where + ".values");
        m.detail = in.text("detail");
        in.finish();
        out.push_back(std::move(m));
    }
    return out;
}

template <std::size_t N>
Json floats_json(const std::array<float, N>& values) {
    Json out = Json::array();
    for (const float v : values) {
        out.push_back(static_cast<double>(v));
    }
    return out;
}

template <std::size_t N>
std::array<float, N> floats_of(const Json& array, const std::string& where) {
    if (array.size() != N) {
        corrupt(std::format("{} must have {} values", where, N));
    }
    std::array<float, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        if (!array[i].is_number()) {
            corrupt(where + " holds a value that is not a number");
        }
        out[i] = static_cast<float>(array[i].get<double>());
    }
    return out;
}

Json optional_number_json(const std::optional<double>& v) { return v ? Json(*v) : Json(); }

Json shot_json(const analysis::ShotFeatures& s) {
    return Json{{"capture_seconds", s.capture_seconds ? Json(*s.capture_seconds) : Json()},
                {"capture_time", s.capture_time},
                {"camera", core::sanitize_utf8(s.camera)},
                {"calibration_domain", core::sanitize_utf8(s.calibration_domain)},
                {"width", s.size.width},
                {"height", s.size.height},
                {"color", s.color},
                {"exposure_ev100", optional_number_json(s.exposure_ev100)},
                {"median_signal", s.median_signal},
                {"scene_ev", optional_number_json(s.scene_ev)},
                {"white_u", optional_number_json(s.white_u)},
                {"white_v", optional_number_json(s.white_v)},
                {"white_kelvin", optional_number_json(s.white_kelvin)},
                {"rendered_colour", s.rendered_colour},
                {"rendered_u", s.rendered_u},
                {"rendered_v", s.rendered_v},
                {"descriptors", s.descriptors},
                {"structure", floats_json(s.structure)},
                {"colour_u", floats_json(s.colour_u)},
                {"colour_v", floats_json(s.colour_v)},
                {"tones", floats_json(s.tones)}};
}

analysis::ShotFeatures shot_of(Obj in) {
    analysis::ShotFeatures s;
    if (!in.is_null("capture_seconds")) {
        s.capture_seconds = in.integer("capture_seconds");
    }
    s.capture_time = in.text("capture_time");
    s.camera = in.text("camera");
    s.calibration_domain = in.text("calibration_domain");
    s.size.width = static_cast<std::int32_t>(in.integer("width"));
    s.size.height = static_cast<std::int32_t>(in.integer("height"));
    s.color = in.boolean("color");
    s.exposure_ev100 = in.optional_number("exposure_ev100");
    s.median_signal = in.number("median_signal");
    s.scene_ev = in.optional_number("scene_ev");
    s.white_u = in.optional_number("white_u");
    s.white_v = in.optional_number("white_v");
    s.white_kelvin = in.optional_number("white_kelvin");
    s.rendered_colour = in.boolean("rendered_colour");
    s.rendered_u = in.number("rendered_u");
    s.rendered_v = in.number("rendered_v");
    s.descriptors = in.boolean("descriptors");
    s.structure = floats_of<analysis::kShotCells>(in.array("structure"), in.path() + ".structure");
    s.colour_u = floats_of<analysis::kShotCells>(in.array("colour_u"), in.path() + ".colour_u");
    s.colour_v = floats_of<analysis::kShotCells>(in.array("colour_v"), in.path() + ".colour_v");
    s.tones = floats_of<analysis::kShotToneBins>(in.array("tones"), in.path() + ".tones");
    in.finish();
    return s;
}

Json appearance_json(const analysis::Appearance& a) {
    return Json{{"valid", a.valid},     {"lightness", a.lightness},       {"colour", a.colour},
                {"u", a.u},             {"v", a.v},                       {"midtone_pixels", a.midtone_pixels},
                {"camera_gains", a.camera_gains}, {"log_red", a.log_red}, {"log_blue", a.log_blue}};
}

analysis::Appearance appearance_of(Obj in) {
    analysis::Appearance a;
    a.valid = in.boolean("valid");
    a.lightness = in.number("lightness");
    a.colour = in.boolean("colour");
    a.u = in.number("u");
    a.v = in.number("v");
    a.midtone_pixels = in.unsigned_number("midtone_pixels");
    a.camera_gains = in.boolean("camera_gains");
    a.log_red = in.number("log_red");
    a.log_blue = in.number("log_blue");
    in.finish();
    return a;
}

Json target_json(const analysis::ConsistencyTarget& t) {
    return Json{{"group_id", core::sanitize_utf8(t.group_id)},
                {"version", t.version},
                {"weight", t.weight},
                {"contributors", t.contributors},
                {"anchored", t.anchored},
                {"lightness", t.lightness},
                {"colour", t.colour},
                {"u", t.u},
                {"v", t.v},
                {"camera_white_balance", t.camera_white_balance},
                {"domain", core::sanitize_utf8(t.domain)},
                {"log_red", t.log_red},
                {"log_blue", t.log_blue},
                {"domain_contributors", t.domain_contributors},
                {"white_balance_confidence", t.white_balance_confidence}};
}

analysis::ConsistencyTarget target_of(Obj in) {
    analysis::ConsistencyTarget t;
    t.group_id = in.text("group_id");
    t.version = in.u32("version");
    t.weight = in.number("weight");
    t.contributors = static_cast<std::size_t>(in.unsigned_number("contributors"));
    t.anchored = in.boolean("anchored");
    t.lightness = in.number("lightness");
    t.colour = in.boolean("colour");
    t.u = in.number("u");
    t.v = in.number("v");
    t.camera_white_balance = in.boolean("camera_white_balance");
    t.domain = in.text("domain");
    t.log_red = in.number("log_red");
    t.log_blue = in.number("log_blue");
    t.domain_contributors = static_cast<std::size_t>(in.unsigned_number("domain_contributors"));
    t.white_balance_confidence = in.number("white_balance_confidence");
    in.finish();
    return t;
}

catalog::AutoCheckOutcome outcome_of(const std::string& text) {
    for (const auto outcome : {catalog::AutoCheckOutcome::Passed, catalog::AutoCheckOutcome::Failed,
                               catalog::AutoCheckOutcome::NotRun}) {
        if (catalog::to_string(outcome) == text) {
            return outcome;
        }
    }
    corrupt("an unknown final-check outcome '" + core::sanitize_utf8(text) + "'");
}

AutoAttemptKind kind_of(const std::string& text) {
    for (const auto kind : {AutoAttemptKind::Chosen, AutoAttemptKind::Fallback, AutoAttemptKind::Baseline,
                            AutoAttemptKind::Anchor}) {
        if (to_string(kind) == text) {
            return kind;
        }
    }
    corrupt("an unknown attempt kind '" + core::sanitize_utf8(text) + "'");
}

Json check_json(const AutoCheck& c) {
    return Json{{"outcome", std::string(catalog::to_string(c.outcome))},
                {"failures", text_list(c.failures)},
                {"forced", c.forced},
                {"values", named_values_json(c.values)},
                {"seconds", c.seconds}};
}

AutoCheck check_of(Obj in) {
    AutoCheck c;
    c.outcome = outcome_of(in.text("outcome"));
    c.failures = text_list_of(in.array("failures"), in.path() + ".failures");
    c.forced = in.boolean("forced");
    c.values = named_values_of(in.array("values"), in.path() + ".values");
    c.seconds = in.number("seconds");
    in.finish();
    return c;
}

Json attempt_json(const AutoAttempt& a) {
    return Json{{"index", a.index},
                {"kind", std::string(to_string(a.kind))},
                {"settings", settings_json(a.settings)},
                {"settings_sha256", core::to_hex(a.settings_sha256)},
                {"reason", core::sanitize_utf8(a.reason)},
                {"check", check_json(a.check)}};
}

AutoAttempt attempt_of(Obj in) {
    AutoAttempt a;
    a.index = in.u32("index");
    a.kind = kind_of(in.text("kind"));
    a.settings = settings_of(in, "settings");
    auto digest = core::digest_from_hex(in.text("settings_sha256"));
    if (!digest) {
        corrupt(in.path() + ".settings_sha256 is not a SHA-256");
    }
    a.settings_sha256 = *digest;
    a.reason = in.text("reason");
    a.check = check_of(in.object("check"));
    in.finish();
    return a;
}

Json controls_json(const std::vector<engine::DevelopControl>& controls) {
    Json out = Json::array();
    for (const engine::DevelopControl control : controls) {
        out.push_back(std::string(engine::to_string(control)));
    }
    return out;
}

std::vector<engine::DevelopControl> controls_of(const Json& array, const std::string& where) {
    std::vector<engine::DevelopControl> out;
    for (const Json& value : array) {
        if (!value.is_string()) {
            corrupt(where + " holds a value that is not text");
        }
        const auto control = engine::parse_develop_control(value.get<std::string>());
        if (!control) {
            corrupt(where + " names an unknown control");
        }
        out.push_back(*control);
    }
    return out;
}

Json locks_json(const AutoLocks& l) {
    return Json{{"locked", controls_json(l.locked)},
                {"prevented_targets", l.prevented_targets},
                {"reasons", text_list(l.reasons)}};
}

AutoLocks locks_of(Obj in) {
    AutoLocks l;
    l.locked = controls_of(in.array("locked"), in.path() + ".locked");
    l.prevented_targets = in.boolean("prevented_targets");
    l.reasons = text_list_of(in.array("reasons"), in.path() + ".reasons");
    in.finish();
    return l;
}

Json consistency_json(const AutoConsistency& c) {
    return Json{{"target", target_json(c.target)},
                {"independent_term", c.independent_term},
                {"term", c.term},
                {"changed", c.changed},
                {"added_candidates", c.added_candidates}};
}

AutoConsistency consistency_of(Obj in) {
    AutoConsistency c;
    c.target = target_of(in.object("target"));
    c.independent_term = in.number("independent_term");
    c.term = in.number("term");
    c.changed = in.boolean("changed");
    c.added_candidates = in.u32("added_candidates");
    in.finish();
    return c;
}

raw::InterpretationStatus interpretation_of(std::uint64_t value) {
    if (value < 1 || value > static_cast<std::uint64_t>(raw::InterpretationStatus::NotDecodable)) {
        corrupt("an unknown interpretation status");
    }
    return static_cast<raw::InterpretationStatus>(value);
}

}  // namespace

// ---- Requests -----------------------------------------------------------------------------------

Expected<std::string> write_auto_request(const AutoRunRequest& request) {
    return guarded("automatic-settings request", [&]() -> Expected<std::string> {
        Json out = Json::object();
        out["schema"] = kAutoRequestSchema;
        out["version"] = kAutoDocumentVersion;
        out["target"] = request.target ? target_json(*request.target) : Json();
        out["anchor"] = request.anchor;
        out["final_check"] = request.final_check;
        out["fail_final_checks"] = request.fail_final_checks;
        return dump(out);
    });
}

Expected<AutoRunRequest> read_auto_request(std::string_view json) {
    return guarded("automatic-settings request", [&]() -> Expected<AutoRunRequest> {
        const Json parsed = parse(json);
        Obj in(parsed, "auto-request");
        check_header(in, kAutoRequestSchema);
        AutoRunRequest request;
        if (!in.is_null("target")) {
            request.target = target_of(in.object("target"));
        }
        request.anchor = in.boolean("anchor");
        request.final_check = in.boolean("final_check");
        request.fail_final_checks = in.u32("fail_final_checks");
        in.finish();
        return request;
    });
}

// ---- Runs ---------------------------------------------------------------------------------------

Expected<std::string> write_auto_run(const AutoRun& run) {
    return guarded("automatic-settings result", [&]() -> Expected<std::string> {
        Json out = Json::object();
        out["schema"] = kAutoRunSchema;
        out["version"] = kAutoDocumentVersion;
        out["versions"] = versions_json(run.versions);
        out["start"] = settings_json(run.start);
        out["chosen"] = settings_json(run.chosen);
        out["confidence"] = run.confidence;
        out["confidence_detail"] = confidence_json(run.confidence_detail);
        out["restrained"] = run.restrained;
        out["analysis_fallback"] = run.analysis_fallback;
        out["flags"] = flags_json(run.flags);
        out["diagnostics"] = metrics_json(run.diagnostics);
        out["shot"] = shot_json(run.shot);
        out["appearance"] = appearance_json(run.appearance);
        out["anchor_appearance"] = run.anchor_appearance ? appearance_json(*run.anchor_appearance) : Json();
        out["manual"] = run.manual;
        out["consistency"] = run.consistency ? consistency_json(*run.consistency) : Json();
        Json attempts = Json::array();
        for (const AutoAttempt& attempt : run.attempts) {
            attempts.push_back(attempt_json(attempt));
        }
        out["attempts"] = std::move(attempts);
        out["effective"] = run.effective ? Json(*run.effective) : Json();
        out["locks"] = locks_json(run.locks);
        out["interpretation"] = static_cast<unsigned>(run.interpretation);
        out["warnings"] = text_list(run.warnings);
        out["width"] = run.width;
        out["height"] = run.height;
        out["timing"] = Json{{"analysis_s", run.analysis_seconds},
                             {"consistency_s", run.consistency_seconds},
                             {"check_s", run.check_seconds},
                             {"total_s", run.total_seconds}};
        out["estimated_peak_bytes"] = run.estimated_peak_bytes;
        return dump(out);
    });
}

Expected<AutoRun> read_auto_run(std::string_view json) {
    return guarded("automatic-settings result", [&]() -> Expected<AutoRun> {
        const Json parsed = parse(json);
        Obj in(parsed, "auto-run");
        check_header(in, kAutoRunSchema);
        AutoRun run;
        run.versions = versions_of(in.object("versions"));
        run.start = settings_of(in, "start");
        run.chosen = settings_of(in, "chosen");
        run.confidence = in.number("confidence");
        run.confidence_detail = confidence_of(in.object("confidence_detail"));
        run.restrained = in.boolean("restrained");
        run.analysis_fallback = in.boolean("analysis_fallback");
        run.flags = flags_of(in.array("flags"), "auto-run.flags");
        run.diagnostics = metrics_of(in.array("diagnostics"), "auto-run.diagnostics");
        run.shot = shot_of(in.object("shot"));
        run.appearance = appearance_of(in.object("appearance"));
        if (!in.is_null("anchor_appearance")) {
            run.anchor_appearance = appearance_of(in.object("anchor_appearance"));
        }
        run.manual = in.boolean("manual");
        if (!in.is_null("consistency")) {
            run.consistency = consistency_of(in.object("consistency"));
        }
        for (const Json& attempt : in.array("attempts")) {
            run.attempts.push_back(attempt_of(Obj(attempt, "auto-run.attempts[]")));
        }
        if (!in.is_null("effective")) {
            const std::uint64_t effective = in.unsigned_number("effective");
            if (effective >= run.attempts.size()) {
                corrupt("auto-run.effective names no attempt");
            }
            run.effective = static_cast<std::size_t>(effective);
        }
        run.locks = locks_of(in.object("locks"));
        run.interpretation = interpretation_of(in.unsigned_number("interpretation", 255));
        run.warnings = text_list_of(in.array("warnings"), "auto-run.warnings");
        run.width = static_cast<std::int32_t>(in.integer("width"));
        run.height = static_cast<std::int32_t>(in.integer("height"));
        {
            Obj timing = in.object("timing");
            run.analysis_seconds = timing.number("analysis_s");
            run.consistency_seconds = timing.number("consistency_s");
            run.check_seconds = timing.number("check_s");
            run.total_seconds = timing.number("total_s");
            timing.finish();
        }
        run.estimated_peak_bytes = in.unsigned_number("estimated_peak_bytes");
        in.finish();
        return run;
    });
}

// ---- Records ------------------------------------------------------------------------------------

Expected<catalog::NewAutoAnalysis> auto_analysis_record(const AutoRun& run, const AutoAttempt& attempt,
                                                        std::optional<catalog::JobId> job,
                                                        catalog::Timestamp created_at) {
    return guarded("analysis record", [&]() -> Expected<catalog::NewAutoAnalysis> {
        catalog::NewAutoAnalysis record;
        record.job = job;
        record.policy = run.versions.policy;
        record.policy_version = run.versions.policy_version;
        record.search_version = run.versions.search_version;
        record.metric_contract_version = run.versions.metric_contract_version;
        if (run.consistency) {
            record.consistency_version = run.versions.consistency_version;
        }
        record.develop_pipeline_version = run.versions.develop_pipeline_version;
        record.develop_settings_version = run.versions.develop_settings_version;
        record.attempt = attempt.index;
        record.final_check = attempt.check.outcome;
        // A fallback is a less certain decision (ADR 0011, the policy confidence after a fallback).
        record.confidence = attempt.index == 0 ? run.confidence
                                               : std::min(run.confidence, analysis::kFallbackConfidence);
        record.confidence = std::clamp(record.confidence, 0.0, 1.0);
        const bool effective = run.effective && *run.effective == attempt.index;
        record.review = run.review_recommended() || attempt.index > 0;
        record.flags_json = dump(flags_json(run.flags));
        Json doc = Json::object();
        doc["schema"] = kAutoRecordSchema;
        doc["version"] = kAutoDocumentVersion;
        doc["attempt"] = attempt_json(attempt);
        doc["effective"] = effective;
        doc["start_sha256"] = [&] {
            auto document = write_recipe(run.start);
            return document ? Json(core::to_hex(document->sha256)) : Json();
        }();
        doc["chosen"] = settings_json(run.chosen);
        doc["confidence_detail"] = confidence_json(run.confidence_detail);
        doc["restrained"] = run.restrained;
        doc["analysis_fallback"] = run.analysis_fallback;
        doc["diagnostics"] = metrics_json(run.diagnostics);
        doc["consistency"] = run.consistency ? consistency_json(*run.consistency) : Json();
        doc["locks"] = locks_json(run.locks);
        doc["interpretation"] = static_cast<unsigned>(run.interpretation);
        doc["size"] = Json::array({run.width, run.height});
        doc["timing"] = Json{{"analysis_s", run.analysis_seconds},
                             {"consistency_s", run.consistency_seconds},
                             {"check_s", run.check_seconds},
                             {"total_s", run.total_seconds}};
        record.record_json = dump(doc);
        record.created_at = created_at;
        if (Status valid = [&] {
                catalog::NewAutoAnalysis probe = record;
                probe.revision = catalog::RevisionId{1};  // filled in by the writer
                return probe.validate();
            }();
            !valid) {
            return Unexpected(std::move(valid.error()));
        }
        return record;
    });
}

}  // namespace lucent::app
