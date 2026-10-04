// Recipe documents: canonical writing (RFC 8785) and strict reading of "lucent.develop" v1
// (recipe.hpp).

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <initializer_list>
#include <limits>
#include <new>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include <lucent/application/recipe.hpp>

namespace lucent::app {

namespace {

using Json = nlohmann::json;  // std::map objects: members iterate in byte order of their names

constexpr std::string_view kWhere = "app.recipe";

// Member names of a version-1 document.
constexpr std::string_view kSchemaKey = "schema";
constexpr std::string_view kVersionKey = "version";
constexpr std::string_view kOperationsKey = "operations";
constexpr std::string_view kWhiteBalanceKey = "white_balance";
constexpr std::string_view kExposureKey = "exposure_ev";
constexpr std::string_view kContrastKey = "contrast";
constexpr std::string_view kHighlightsKey = "highlights";
constexpr std::string_view kShadowsKey = "shadows";
constexpr std::string_view kToneCurveKey = "tone_curve";
constexpr std::string_view kSaturationKey = "saturation";
constexpr std::string_view kVibranceKey = "vibrance";
constexpr std::string_view kBaseLookKey = "base_look";
constexpr std::string_view kLocksKey = "locks";

// Operation names in "operations".
constexpr std::string_view kOpWhiteBalance = "white_balance";
constexpr std::string_view kOpExposure = "exposure";
constexpr std::string_view kOpLocalTone = "local_tone";
constexpr std::string_view kOpContrast = "contrast";
constexpr std::string_view kOpToneCurve = "tone_curve";
constexpr std::string_view kOpColor = "color";

Error invalid(std::string message) {
    return Error{ErrorCode::InvalidArgument, std::move(message), std::string(kWhere), 0};
}

Error newer(std::string message) {
    return Error{ErrorCode::VersionMismatch, std::move(message), std::string(kWhere), 0};
}

// ---- Canonical writer -----------------------------------------------------------------------

void append_string(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
}

Status append_canonical(std::string& out, const Json& value) {
    switch (value.type()) {
        case Json::value_t::null:
            out += "null";
            return ok();
        case Json::value_t::boolean:
            out += value.get<bool>() ? "true" : "false";
            return ok();
        case Json::value_t::number_unsigned:
            out += std::to_string(value.get<std::uint64_t>());
            return ok();
        case Json::value_t::number_integer:
            out += std::to_string(value.get<std::int64_t>());
            return ok();
        case Json::value_t::number_float: {
            auto text = canonical_number(value.get<double>());
            if (!text) {
                return Unexpected(std::move(text.error()));
            }
            out += *text;
            return ok();
        }
        case Json::value_t::string:
            append_string(out, value.get_ref<const std::string&>());
            return ok();
        case Json::value_t::array: {
            out.push_back('[');
            bool first = true;
            for (const Json& element : value) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                if (auto status = append_canonical(out, element); !status) {
                    return status;
                }
            }
            out.push_back(']');
            return ok();
        }
        case Json::value_t::object: {
            // std::map orders names by std::char_traits<char>, i.e. by unsigned bytes: for ASCII
            // names that is RFC 8785's UTF-16 code-unit order.
            out.push_back('{');
            bool first = true;
            for (const auto& [name, member] : value.items()) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                append_string(out, name);
                out.push_back(':');
                if (auto status = append_canonical(out, member); !status) {
                    return status;
                }
            }
            out.push_back('}');
            return ok();
        }
        case Json::value_t::binary:
        case Json::value_t::discarded:
            break;
    }
    return fail(ErrorCode::Internal, "a recipe value has no JSON form", std::string(kWhere));
}

Json white_balance_json(const engine::WhiteBalanceSetting& setting) {
    Json out = Json::object();
    out["mode"] = std::string(engine::to_string(engine::mode_of(setting)));
    if (const auto* kelvin = std::get_if<engine::KelvinWhiteBalance>(&setting)) {
        out["temperature_k"] = kelvin->temperature_k;
        out["tint"] = kelvin->tint;
    } else if (const auto* neutral = std::get_if<engine::CameraNeutralWhiteBalance>(&setting)) {
        out["r"] = neutral->r;
        out["g"] = neutral->g;
        out["b"] = neutral->b;
    }
    return out;
}

Json settings_json(const engine::DevelopSettings& s) {
    Json doc = Json::object();
    doc[std::string(kSchemaKey)] = std::string(kRecipeSchema);
    doc[std::string(kVersionKey)] = s.version;
    doc[std::string(kOperationsKey)] = Json{{std::string(kOpWhiteBalance), s.operations.white_balance},
                                            {std::string(kOpExposure), s.operations.exposure},
                                            {std::string(kOpLocalTone), s.operations.local_tone},
                                            {std::string(kOpContrast), s.operations.contrast},
                                            {std::string(kOpToneCurve), s.operations.tone_curve},
                                            {std::string(kOpColor), s.operations.color}};
    doc[std::string(kWhiteBalanceKey)] = white_balance_json(s.white_balance);
    doc[std::string(kExposureKey)] = s.exposure_ev;
    doc[std::string(kContrastKey)] = s.contrast;
    doc[std::string(kHighlightsKey)] = s.highlights;
    doc[std::string(kShadowsKey)] = s.shadows;
    Json curve = Json::array();
    for (const engine::ToneCurvePoint& p : s.tone_curve.points) {
        curve.push_back(Json::array({p.x, p.y}));
    }
    doc[std::string(kToneCurveKey)] = std::move(curve);
    doc[std::string(kSaturationKey)] = s.saturation;
    doc[std::string(kVibranceKey)] = s.vibrance;
    doc[std::string(kBaseLookKey)] = Json{{"id", s.base_look.id}, {"version", s.base_look.version}};
    Json locks = Json::array();
    for (const engine::DevelopControl control : s.locks) {  // std::set: numeric order
        locks.push_back(std::string(engine::to_string(control)));
    }
    doc[std::string(kLocksKey)] = std::move(locks);
    return doc;
}

// ---- Strict reader --------------------------------------------------------------------------

std::string member_list(std::initializer_list<std::string_view> names) {
    std::string out;
    for (const std::string_view name : names) {
        out += out.empty() ? "" : ", ";
        out += name;
    }
    return out;
}

// `object` must have exactly the members `names`.
Status exact_members(const Json& object, std::initializer_list<std::string_view> names,
                     std::string_view what) {
    if (!object.is_object()) {
        return Unexpected(invalid(std::format("{} must be a JSON object", what)));
    }
    for (const auto& [name, value] : object.items()) {
        if (std::find(names.begin(), names.end(), std::string_view(name)) == names.end()) {
            return Unexpected(
                invalid(std::format("{} has an unknown member '{}' (a version {} recipe has {})", what, name,
                                    kRecipeSchemaVersion, member_list(names))));
        }
    }
    for (const std::string_view name : names) {
        if (!object.contains(std::string(name))) {
            return Unexpected(invalid(std::format("{} lacks the member '{}'", what, name)));
        }
    }
    return ok();
}

Expected<double> number_of(const Json& value, std::string_view what) {
    if (!value.is_number()) {
        return Unexpected(invalid(std::format("{} must be a number", what)));
    }
    const double v = value.get<double>();
    if (!std::isfinite(v)) {
        return Unexpected(invalid(std::format("{} is not a finite number", what)));
    }
    return v;
}

Expected<std::uint32_t> version_of(const Json& value, std::string_view what) {
    if (!value.is_number_unsigned()) {
        return Unexpected(invalid(std::format("{} must be an unsigned integer", what)));
    }
    const std::uint64_t v = value.get<std::uint64_t>();
    if (v == 0 || v > std::numeric_limits<std::uint32_t>::max()) {
        return Unexpected(invalid(std::format("{} {} is not a valid version", what, v)));
    }
    return static_cast<std::uint32_t>(v);
}

Status operation_version(const Json& operations, std::string_view name, std::uint32_t supported,
                         std::uint32_t& out) {
    auto version = version_of(operations.at(std::string(name)), std::format("operation '{}' version", name));
    if (!version) {
        return Unexpected(std::move(version.error()));
    }
    if (*version != supported) {
        return Unexpected(
            newer(std::format("the recipe needs version {} of the '{}' operation; this build implements "
                              "version {}",
                              *version, name, supported)));
    }
    out = *version;
    return ok();
}

Expected<engine::WhiteBalanceSetting> read_white_balance(const Json& value) {
    if (!value.is_object() || !value.contains("mode") || !value.at("mode").is_string()) {
        return Unexpected(invalid("white_balance must be an object with a string 'mode'"));
    }
    const std::string& mode = value.at("mode").get_ref<const std::string&>();
    if (mode == engine::to_string(engine::WhiteBalanceMode::AsShot)) {
        if (auto status = exact_members(value, {"mode"}, "white_balance (as_shot)"); !status) {
            return Unexpected(std::move(status.error()));
        }
        return engine::WhiteBalanceSetting{engine::AsShotWhiteBalance{}};
    }
    if (mode == engine::to_string(engine::WhiteBalanceMode::Kelvin)) {
        if (auto status = exact_members(value, {"mode", "temperature_k", "tint"}, "white_balance (kelvin)");
            !status) {
            return Unexpected(std::move(status.error()));
        }
        auto t = number_of(value.at("temperature_k"), "white_balance.temperature_k");
        auto tint = number_of(value.at("tint"), "white_balance.tint");
        if (!t || !tint) {
            return Unexpected(!t ? std::move(t.error()) : std::move(tint.error()));
        }
        return engine::WhiteBalanceSetting{engine::KelvinWhiteBalance{*t, *tint}};
    }
    if (mode == engine::to_string(engine::WhiteBalanceMode::CameraNeutral)) {
        if (auto status = exact_members(value, {"mode", "r", "g", "b"}, "white_balance (camera_neutral)");
            !status) {
            return Unexpected(std::move(status.error()));
        }
        auto r = number_of(value.at("r"), "white_balance.r");
        auto g = number_of(value.at("g"), "white_balance.g");
        auto b = number_of(value.at("b"), "white_balance.b");
        if (!r || !g || !b) {
            return Unexpected(!r ? std::move(r.error()) : !g ? std::move(g.error()) : std::move(b.error()));
        }
        return engine::WhiteBalanceSetting{engine::CameraNeutralWhiteBalance{*r, *g, *b}};
    }
    return Unexpected(invalid(
        std::format("unknown white-balance mode '{}' in a version {} recipe", mode, kRecipeSchemaVersion)));
}

Expected<engine::ToneCurveSetting> read_tone_curve(const Json& value) {
    if (!value.is_array()) {
        return Unexpected(invalid("tone_curve must be an array of [x, y] points"));
    }
    engine::ToneCurveSetting curve;
    curve.points.clear();
    std::size_t index = 0;
    for (const Json& point : value) {
        if (!point.is_array() || point.size() != 2) {
            return Unexpected(invalid(std::format("tone_curve point {} must be an [x, y] array", index)));
        }
        auto x = number_of(point.at(0), std::format("tone_curve point {} x", index));
        auto y = number_of(point.at(1), std::format("tone_curve point {} y", index));
        if (!x || !y) {
            return Unexpected(!x ? std::move(x.error()) : std::move(y.error()));
        }
        curve.points.push_back(engine::ToneCurvePoint{*x, *y});
        ++index;
    }
    return curve;
}

Expected<std::set<engine::DevelopControl>> read_locks(const Json& value) {
    if (!value.is_array()) {
        return Unexpected(invalid("locks must be an array of control names"));
    }
    std::set<engine::DevelopControl> locks;
    for (const Json& name : value) {
        if (!name.is_string()) {
            return Unexpected(invalid("locks must hold control names (strings)"));
        }
        const std::string& text = name.get_ref<const std::string&>();
        const std::optional<engine::DevelopControl> control = engine::parse_develop_control(text);
        if (!control) {
            return Unexpected(invalid(std::format("unknown locked control '{}' in a version {} recipe", text,
                                                  kRecipeSchemaVersion)));
        }
        if (!locks.insert(*control).second) {
            return Unexpected(invalid(std::format("the control '{}' is locked twice", text)));
        }
    }
    return locks;
}

Expected<engine::DevelopSettings> read_v1(const Json& doc, std::uint32_t version) {
    if (auto status = exact_members(doc,
                                    {kSchemaKey, kVersionKey, kOperationsKey, kWhiteBalanceKey, kExposureKey,
                                     kContrastKey, kHighlightsKey, kShadowsKey, kToneCurveKey, kSaturationKey,
                                     kVibranceKey, kBaseLookKey, kLocksKey},
                                    "the recipe");
        !status) {
        return Unexpected(std::move(status.error()));
    }
    engine::DevelopSettings s;
    s.version = version;

    const Json& operations = doc.at(std::string(kOperationsKey));
    if (auto status = exact_members(
            operations, {kOpWhiteBalance, kOpExposure, kOpLocalTone, kOpContrast, kOpToneCurve, kOpColor},
            "operations");
        !status) {
        if (operations.is_object()) {
            // An operation name this version does not define is reported as such.
            return Unexpected(
                Error{ErrorCode::InvalidArgument,
                      status.error().message + " (unknown operations are never treated as no-ops)",
                      std::string(kWhere), 0});
        }
        return Unexpected(std::move(status.error()));
    }
    const std::initializer_list<std::tuple<std::string_view, std::uint32_t, std::uint32_t*>> ops = {
        {kOpWhiteBalance, engine::kWhiteBalanceOperationVersion, &s.operations.white_balance},
        {kOpExposure, engine::kExposureOperationVersion, &s.operations.exposure},
        {kOpLocalTone, engine::kLocalToneOperationVersion, &s.operations.local_tone},
        {kOpContrast, engine::kContrastOperationVersion, &s.operations.contrast},
        {kOpToneCurve, engine::kToneCurveOperationVersion, &s.operations.tone_curve},
        {kOpColor, engine::kColorOperationVersion, &s.operations.color},
    };
    for (const auto& [name, supported, out] : ops) {
        if (auto status = operation_version(operations, name, supported, *out); !status) {
            return Unexpected(std::move(status.error()));
        }
    }

    const Json& look = doc.at(std::string(kBaseLookKey));
    if (auto status = exact_members(look, {"id", "version"}, "base_look"); !status) {
        return Unexpected(std::move(status.error()));
    }
    if (!look.at("id").is_string()) {
        return Unexpected(invalid("base_look.id must be a string"));
    }
    s.base_look.id = look.at("id").get<std::string>();
    auto look_version = version_of(look.at("version"), "base_look.version");
    if (!look_version) {
        return Unexpected(std::move(look_version.error()));
    }
    s.base_look.version = *look_version;
    if (s.base_look.id != engine::kNeutralLookId) {
        return Unexpected(newer(std::format(
            "the recipe uses the base look '{}', which this build does not know", s.base_look.id)));
    }
    if (s.base_look.version != engine::kNeutralLookVersion) {
        return Unexpected(
            newer(std::format("the recipe uses version {} of the base look '{}'; this build implements "
                              "version {}",
                              s.base_look.version, s.base_look.id, engine::kNeutralLookVersion)));
    }

    auto white_balance = read_white_balance(doc.at(std::string(kWhiteBalanceKey)));
    if (!white_balance) {
        return Unexpected(std::move(white_balance.error()));
    }
    s.white_balance = *white_balance;

    const std::initializer_list<std::pair<std::string_view, double*>> numbers = {
        {kExposureKey, &s.exposure_ev}, {kContrastKey, &s.contrast},     {kHighlightsKey, &s.highlights},
        {kShadowsKey, &s.shadows},      {kSaturationKey, &s.saturation}, {kVibranceKey, &s.vibrance},
    };
    for (const auto& [name, out] : numbers) {
        auto value = number_of(doc.at(std::string(name)), name);
        if (!value) {
            return Unexpected(std::move(value.error()));
        }
        *out = *value;
    }

    auto curve = read_tone_curve(doc.at(std::string(kToneCurveKey)));
    if (!curve) {
        return Unexpected(std::move(curve.error()));
    }
    s.tone_curve = std::move(*curve);

    auto locks = read_locks(doc.at(std::string(kLocksKey)));
    if (!locks) {
        return Unexpected(std::move(locks.error()));
    }
    s.locks = std::move(*locks);

    if (auto valid = s.validate(); !valid) {
        const Error& e = valid.error();
        return Unexpected(
            Error{e.code, "the recipe's values are not valid: " + e.message, std::string(kWhere), 0});
    }
    return s;
}

}  // namespace

Expected<std::string> canonical_number(double value) {
    if (!std::isfinite(value)) {
        return fail(ErrorCode::InvalidArgument, std::format("{} has no JSON form", value),
                    std::string(kWhere));
    }
    if (value == 0.0) {
        return std::string("0");  // also -0 (RFC 8785, ECMAScript ToString)
    }
    std::string out;
    if (value < 0.0) {
        out.push_back('-');
        value = -value;
    }
    // Shortest round-trip digits: "d[.ddd]e[+-]XX".
    char buffer[64];
    const auto [end, ec] =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::scientific);
    if (ec != std::errc{}) {
        return fail(ErrorCode::Internal, "cannot format a number", std::string(kWhere));
    }
    const std::string_view text(buffer, static_cast<std::size_t>(end - buffer));
    const std::size_t e = text.find('e');
    if (e == std::string_view::npos) {
        return fail(ErrorCode::Internal, "unexpected number format", std::string(kWhere));
    }
    std::string digits(1, text[0]);
    if (e > 2 && text[1] == '.') {
        digits.append(text.substr(2, e - 2));
    }
    std::string_view exponent_text = text.substr(e + 1);
    if (!exponent_text.empty() && exponent_text.front() == '+') {
        exponent_text.remove_prefix(1);
    }
    int exponent = 0;
    if (std::from_chars(exponent_text.data(), exponent_text.data() + exponent_text.size(), exponent).ec !=
        std::errc{}) {
        return fail(ErrorCode::Internal, "unexpected number exponent", std::string(kWhere));
    }
    // ECMAScript Number::toString: k digits, the decimal point after n of them.
    const int k = static_cast<int>(digits.size());
    const int n = exponent + 1;
    if (k <= n && n <= 21) {
        out += digits;
        out.append(static_cast<std::size_t>(n - k), '0');
    } else if (0 < n && n <= 21) {
        out += digits.substr(0, static_cast<std::size_t>(n));
        out.push_back('.');
        out += digits.substr(static_cast<std::size_t>(n));
    } else if (-6 < n && n <= 0) {
        out += "0.";
        out.append(static_cast<std::size_t>(-n), '0');
        out += digits;
    } else {
        out.push_back(digits[0]);
        if (k > 1) {
            out.push_back('.');
            out += digits.substr(1);
        }
        out.push_back('e');
        out.push_back(n - 1 >= 0 ? '+' : '-');
        out += std::to_string(std::abs(n - 1));
    }
    return out;
}

Expected<RecipeDocument> write_recipe(const engine::DevelopSettings& settings) {
    if (auto valid = settings.validate(); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    try {
        RecipeDocument document;
        if (auto status = append_canonical(document.json, settings_json(settings)); !status) {
            return Unexpected(std::move(status.error()));
        }
        auto digest = core::sha256(std::as_bytes(std::span(document.json.data(), document.json.size())));
        if (!digest) {
            return Unexpected(std::move(digest.error()));
        }
        document.sha256 = *digest;
        return document;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot write the recipe", std::string(kWhere));
    }
}

Expected<engine::DevelopSettings> read_recipe(std::string_view text) {
    try {
        // Repeated member names are refused (nlohmann keeps the last one silently).
        std::vector<std::set<std::string>> names;
        std::string repeated;
        const Json::parser_callback_t track = [&](int, Json::parse_event_t event, Json& parsed) {
            switch (event) {
                case Json::parse_event_t::object_start:
                    names.emplace_back();
                    break;
                case Json::parse_event_t::key:
                    if (!names.empty() && parsed.is_string() &&
                        !names.back().insert(parsed.get<std::string>()).second && repeated.empty()) {
                        repeated = parsed.get<std::string>();
                    }
                    break;
                case Json::parse_event_t::object_end:
                    if (!names.empty()) {
                        names.pop_back();
                    }
                    break;
                default:
                    break;
            }
            return true;
        };
        const Json doc = Json::parse(text.begin(), text.end(), track, /*allow_exceptions=*/false);
        if (doc.is_discarded()) {
            return Unexpected(invalid("the recipe is not valid JSON"));
        }
        if (!repeated.empty()) {
            return Unexpected(invalid(std::format("the recipe repeats the member '{}'", repeated)));
        }
        if (!doc.is_object()) {
            return Unexpected(invalid("a recipe is a JSON object"));
        }
        const auto schema = doc.find(std::string(kSchemaKey));
        if (schema == doc.end() || !schema->is_string() ||
            schema->get_ref<const std::string&>() != kRecipeSchema) {
            return Unexpected(invalid(std::format("the document is not a '{}' recipe", kRecipeSchema)));
        }
        const auto version_member = doc.find(std::string(kVersionKey));
        if (version_member == doc.end()) {
            return Unexpected(invalid("the recipe has no version"));
        }
        auto version = version_of(*version_member, "the recipe version");
        if (!version) {
            return Unexpected(std::move(version.error()));
        }
        if (*version > kRecipeSchemaVersion) {
            return Unexpected(
                newer(std::format("the recipe has version {} of '{}'; this build reads version {} (it "
                                  "was made by a newer Lucent)",
                                  *version, kRecipeSchema, kRecipeSchemaVersion)));
        }
        return read_v1(doc, *version);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot read the recipe", std::string(kWhere));
    } catch (const std::exception& e) {
        return Unexpected(invalid(std::format("the recipe cannot be read: {}", e.what())));
    }
}

// ---- Categories ------------------------------------------------------------------------------

std::string_view to_string(RecipeCategory category) noexcept {
    switch (category) {
        case RecipeCategory::WhiteBalance:
            return "white_balance";
        case RecipeCategory::Exposure:
            return "exposure";
        case RecipeCategory::Tone:
            return "tone";
        case RecipeCategory::Color:
            return "color";
        case RecipeCategory::Local:
            return "local";
    }
    return "invalid";
}

std::optional<RecipeCategory> parse_recipe_category(std::string_view text) noexcept {
    for (const RecipeCategory category : kAllRecipeCategories) {
        if (to_string(category) == text) {
            return category;
        }
    }
    return std::nullopt;
}

RecipeCategory category_of(engine::DevelopControl control) noexcept {
    switch (control) {
        case engine::DevelopControl::WhiteBalance:
            return RecipeCategory::WhiteBalance;
        case engine::DevelopControl::Exposure:
            return RecipeCategory::Exposure;
        case engine::DevelopControl::Contrast:
        case engine::DevelopControl::ToneCurve:
        case engine::DevelopControl::BaseLook:
            return RecipeCategory::Tone;
        case engine::DevelopControl::Saturation:
        case engine::DevelopControl::Vibrance:
            return RecipeCategory::Color;
        case engine::DevelopControl::Highlights:
        case engine::DevelopControl::Shadows:
            return RecipeCategory::Local;
    }
    return RecipeCategory::Tone;
}

std::vector<engine::DevelopControl> controls_of(RecipeCategory category) {
    std::vector<engine::DevelopControl> out;
    for (const engine::DevelopControl control : kAllDevelopControls) {
        if (category_of(control) == category) {
            out.push_back(control);
        }
    }
    return out;
}

bool same_value(const engine::DevelopSettings& a, const engine::DevelopSettings& b,
                engine::DevelopControl control) noexcept {
    switch (control) {
        case engine::DevelopControl::WhiteBalance:
            return a.white_balance == b.white_balance;
        case engine::DevelopControl::Exposure:
            return a.exposure_ev == b.exposure_ev;
        case engine::DevelopControl::Contrast:
            return a.contrast == b.contrast;
        case engine::DevelopControl::Highlights:
            return a.highlights == b.highlights;
        case engine::DevelopControl::Shadows:
            return a.shadows == b.shadows;
        case engine::DevelopControl::ToneCurve:
            return a.tone_curve == b.tone_curve;
        case engine::DevelopControl::Saturation:
            return a.saturation == b.saturation;
        case engine::DevelopControl::Vibrance:
            return a.vibrance == b.vibrance;
        case engine::DevelopControl::BaseLook:
            return a.base_look == b.base_look;
    }
    return true;
}

void copy_value(const engine::DevelopSettings& from, engine::DevelopSettings& to,
                engine::DevelopControl control) {
    switch (control) {
        case engine::DevelopControl::WhiteBalance:
            to.white_balance = from.white_balance;
            return;
        case engine::DevelopControl::Exposure:
            to.exposure_ev = from.exposure_ev;
            return;
        case engine::DevelopControl::Contrast:
            to.contrast = from.contrast;
            return;
        case engine::DevelopControl::Highlights:
            to.highlights = from.highlights;
            return;
        case engine::DevelopControl::Shadows:
            to.shadows = from.shadows;
            return;
        case engine::DevelopControl::ToneCurve:
            to.tone_curve = from.tone_curve;
            return;
        case engine::DevelopControl::Saturation:
            to.saturation = from.saturation;
            return;
        case engine::DevelopControl::Vibrance:
            to.vibrance = from.vibrance;
            return;
        case engine::DevelopControl::BaseLook:
            to.base_look = from.base_look;
            return;
    }
}

std::vector<engine::DevelopControl> changed_controls(const engine::DevelopSettings& a,
                                                     const engine::DevelopSettings& b) {
    std::vector<engine::DevelopControl> out;
    for (const engine::DevelopControl control : kAllDevelopControls) {
        if (!same_value(a, b, control)) {
            out.push_back(control);
        }
    }
    return out;
}

std::vector<engine::DevelopControl> changed_locks(const engine::DevelopSettings& a,
                                                  const engine::DevelopSettings& b) {
    std::vector<engine::DevelopControl> out;
    for (const engine::DevelopControl control : kAllDevelopControls) {
        if (a.locks.contains(control) != b.locks.contains(control)) {
            out.push_back(control);
        }
    }
    return out;
}

}  // namespace lucent::app
