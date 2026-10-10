// Recipe documents: canonical writing (RFC 8785) of "lucent.develop" v3 and strict reading of v1,
// v2 and v3 (recipe.hpp).

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <initializer_list>
#include <limits>
#include <new>
#include <optional>
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
using Names = std::vector<std::string_view>;

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
// Added by version 2.
constexpr std::string_view kChromaDenoiseKey = "chroma_denoise";
constexpr std::string_view kLocalAdjustmentsKey = "local_adjustments";
constexpr std::string_view kGeometryKey = "geometry";
// Added by version 3.
constexpr std::string_view kLensCorrectionKey = "lens_correction";

// Operation names in "operations".
constexpr std::string_view kOpWhiteBalance = "white_balance";
constexpr std::string_view kOpExposure = "exposure";
constexpr std::string_view kOpLocalTone = "local_tone";
constexpr std::string_view kOpContrast = "contrast";
constexpr std::string_view kOpToneCurve = "tone_curve";
constexpr std::string_view kOpColor = "color";
// Added by version 2.
constexpr std::string_view kOpChromaDenoise = "chroma_denoise";
constexpr std::string_view kOpLocalAdjustments = "local_adjustments";
constexpr std::string_view kOpGeometry = "geometry";
// Added by version 3.
constexpr std::string_view kOpLensCorrection = "lens_correction";

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

Json point_json(const engine::MaskPoint& p) {
    return Json::array({p.x, p.y});
}

Json mask_json(const engine::MaskShape& shape) {
    Json out = Json::object();
    out["kind"] = std::string(engine::to_string(engine::kind_of(shape)));
    if (const auto* brush = std::get_if<engine::BrushMask>(&shape)) {
        Json strokes = Json::array();
        for (const engine::BrushStroke& stroke : brush->strokes) {
            Json points = Json::array();
            for (const engine::MaskPoint& p : stroke.points) {
                points.push_back(point_json(p));
            }
            strokes.push_back(Json{{"mode", std::string(engine::to_string(stroke.mode))},
                                   {"radius", stroke.radius},
                                   {"hardness", stroke.hardness},
                                   {"flow", stroke.flow},
                                   {"points", std::move(points)}});
        }
        out["strokes"] = std::move(strokes);
    } else if (const auto* linear = std::get_if<engine::LinearGradientMask>(&shape)) {
        out["start"] = point_json(linear->start);
        out["end"] = point_json(linear->end);
        out["feather"] = linear->feather;
    } else if (const auto* radial = std::get_if<engine::RadialGradientMask>(&shape)) {
        out["center"] = point_json(radial->center);
        out["radius_x"] = radial->radius_x;
        out["radius_y"] = radial->radius_y;
        out["angle_degrees"] = radial->angle_degrees;
        out["feather"] = radial->feather;
    }
    return out;
}

Json adjustment_json(const engine::LocalAdjustment& a) {
    const engine::LocalControls& c = a.controls;
    return Json{{"id", a.id},
                {"enabled", a.enabled},
                {"invert", a.invert},
                {"opacity", a.opacity},
                {"mask", mask_json(a.shape)},
                {"controls", Json{{"exposure_ev", c.exposure_ev},
                                  {"contrast", c.contrast},
                                  {"highlights", c.highlights},
                                  {"shadows", c.shadows},
                                  {"saturation", c.saturation},
                                  {"temperature", c.temperature},
                                  {"tint", c.tint}}}};
}

Json geometry_json(const engine::GeometrySetting& g) {
    return Json{{"straighten_degrees", g.straighten_degrees},
                {"crop", Json{{"left", g.crop.left},
                              {"top", g.crop.top},
                              {"right", g.crop.right},
                              {"bottom", g.crop.bottom}}},
                {"quarter_turns", g.quarter_turns},
                {"flip_horizontal", g.flip_horizontal}};
}

Json lens_correction_json(const engine::LensCorrectionSetting& l) {
    Json profile = nullptr;
    if (l.profile) {
        profile = Json{{"maker", l.profile->maker},
                       {"model", l.profile->model},
                       {"crop_factor", l.profile->crop_factor}};
    }
    return Json{{"distortion", l.distortion},
                {"lateral_chromatic_aberration", l.lateral_chromatic_aberration},
                {"vignetting", l.vignetting},
                {"profile", std::move(profile)}};
}

// The version-3 document of current (version-3) settings.
Json settings_json(const engine::DevelopSettings& s) {
    Json doc = Json::object();
    doc[std::string(kSchemaKey)] = std::string(kRecipeSchema);
    doc[std::string(kVersionKey)] = s.version;
    doc[std::string(kOperationsKey)] =
        Json{{std::string(kOpWhiteBalance), s.operations.white_balance},
             {std::string(kOpExposure), s.operations.exposure},
             {std::string(kOpLocalTone), s.operations.local_tone},
             {std::string(kOpContrast), s.operations.contrast},
             {std::string(kOpToneCurve), s.operations.tone_curve},
             {std::string(kOpColor), s.operations.color},
             {std::string(kOpChromaDenoise), s.operations.chroma_denoise},
             {std::string(kOpLocalAdjustments), s.operations.local_adjustments},
             {std::string(kOpGeometry), s.operations.geometry},
             {std::string(kOpLensCorrection), s.operations.lens_correction}};
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
    doc[std::string(kChromaDenoiseKey)] =
        Json{{"strength", s.chroma_denoise.strength}, {"radius", s.chroma_denoise.radius}};
    Json adjustments = Json::array();
    for (const engine::LocalAdjustment& a : s.local_adjustments) {
        adjustments.push_back(adjustment_json(a));
    }
    doc[std::string(kLocalAdjustmentsKey)] = std::move(adjustments);
    doc[std::string(kGeometryKey)] = geometry_json(s.geometry);
    doc[std::string(kLensCorrectionKey)] = lens_correction_json(s.lens_correction);
    return doc;
}

// ---- Strict reader --------------------------------------------------------------------------

std::string member_list(const Names& names) {
    std::string out;
    for (const std::string_view name : names) {
        out += out.empty() ? "" : ", ";
        out += name;
    }
    return out;
}

// `object` must have exactly the members `names`.
Status exact_members(const Json& object, const Names& names, std::string_view what, std::uint32_t version) {
    if (!object.is_object()) {
        return Unexpected(invalid(std::format("{} must be a JSON object", what)));
    }
    for (const auto& [name, value] : object.items()) {
        if (std::find(names.begin(), names.end(), std::string_view(name)) == names.end()) {
            return Unexpected(invalid(std::format("{} has an unknown member '{}' (a version {} recipe has {})", what,
                                                  name, version, member_list(names))));
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

Expected<bool> bool_of(const Json& value, std::string_view what) {
    if (!value.is_boolean()) {
        return Unexpected(invalid(std::format("{} must be true or false", what)));
    }
    return value.get<bool>();
}

// An unsigned integer that fits 32 bits (0 allowed).
Expected<std::uint32_t> unsigned_of(const Json& value, std::string_view what) {
    if (!value.is_number_unsigned()) {
        return Unexpected(invalid(std::format("{} must be an unsigned integer", what)));
    }
    const std::uint64_t v = value.get<std::uint64_t>();
    if (v > std::numeric_limits<std::uint32_t>::max()) {
        return Unexpected(invalid(std::format("{} {} is out of range", what, v)));
    }
    return static_cast<std::uint32_t>(v);
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

// Reads numbers into their destinations; the first failure is returned.
Status read_numbers(const Json& object, std::initializer_list<std::pair<std::string_view, double*>> members,
                    std::string_view prefix) {
    for (const auto& [name, out] : members) {
        auto value = number_of(object.at(std::string(name)), std::format("{}{}", prefix, name));
        if (!value) {
            return Unexpected(std::move(value.error()));
        }
        *out = *value;
    }
    return ok();
}

// An operation version this build renders: `supported`, or for the geometry any version from
// `oldest` (version 1, the whole visible area, still rendered for the recipes made with it). With
// `zero_allowed` (the lens correction, whose version 0 means "none": settings made before the
// operation existed), 0 is a version too.
Status operation_version(const Json& operations, std::string_view name, std::uint32_t supported,
                         std::uint32_t& out, std::uint32_t oldest = 0, bool zero_allowed = false) {
    const std::string what = std::format("operation '{}' version", name);
    auto version = zero_allowed ? unsigned_of(operations.at(std::string(name)), what)
                                : version_of(operations.at(std::string(name)), what);
    if (!version) {
        return Unexpected(std::move(version.error()));
    }
    const std::uint32_t lowest = zero_allowed ? 0 : oldest == 0 ? supported : oldest;
    if (*version < lowest || *version > supported) {
        return Unexpected(newer(
            lowest == supported ? std::format("the recipe needs version {} of the '{}' operation; this build "
                                              "implements version {}",
                                              *version, name, supported)
                                : std::format("the recipe needs version {} of the '{}' operation; this build "
                                              "implements versions {} to {}",
                                              *version, name, lowest, supported)));
    }
    out = *version;
    return ok();
}

Expected<engine::WhiteBalanceSetting> read_white_balance(const Json& value, std::uint32_t version) {
    if (!value.is_object() || !value.contains("mode") || !value.at("mode").is_string()) {
        return Unexpected(invalid("white_balance must be an object with a string 'mode'"));
    }
    const std::string& mode = value.at("mode").get_ref<const std::string&>();
    if (mode == engine::to_string(engine::WhiteBalanceMode::AsShot)) {
        if (auto status = exact_members(value, {"mode"}, "white_balance (as_shot)", version); !status) {
            return Unexpected(std::move(status.error()));
        }
        return engine::WhiteBalanceSetting{engine::AsShotWhiteBalance{}};
    }
    if (mode == engine::to_string(engine::WhiteBalanceMode::Kelvin)) {
        if (auto status = exact_members(value, {"mode", "temperature_k", "tint"}, "white_balance (kelvin)", version);
            !status) {
            return Unexpected(std::move(status.error()));
        }
        engine::KelvinWhiteBalance kelvin;
        if (auto status = read_numbers(value, {{"temperature_k", &kelvin.temperature_k}, {"tint", &kelvin.tint}},
                                       "white_balance.");
            !status) {
            return Unexpected(std::move(status.error()));
        }
        return engine::WhiteBalanceSetting{kelvin};
    }
    if (mode == engine::to_string(engine::WhiteBalanceMode::CameraNeutral)) {
        if (auto status = exact_members(value, {"mode", "r", "g", "b"}, "white_balance (camera_neutral)", version);
            !status) {
            return Unexpected(std::move(status.error()));
        }
        engine::CameraNeutralWhiteBalance neutral;
        if (auto status =
                read_numbers(value, {{"r", &neutral.r}, {"g", &neutral.g}, {"b", &neutral.b}}, "white_balance.");
            !status) {
            return Unexpected(std::move(status.error()));
        }
        return engine::WhiteBalanceSetting{neutral};
    }
    return Unexpected(invalid(std::format("unknown white-balance mode '{}' in a version {} recipe", mode, version)));
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

Expected<std::set<engine::DevelopControl>> read_locks(const Json& value, std::uint32_t version) {
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
            return Unexpected(
                invalid(std::format("unknown locked control '{}' in a version {} recipe", text, version)));
        }
        if (!locks.insert(*control).second) {
            return Unexpected(invalid(std::format("the control '{}' is locked twice", text)));
        }
    }
    return locks;
}

// ---- Version-2 members ------------------------------------------------------------------------

Expected<engine::MaskPoint> read_point(const Json& value, std::string_view what) {
    if (!value.is_array() || value.size() != 2) {
        return Unexpected(invalid(std::format("{} must be an [x, y] array", what)));
    }
    auto x = number_of(value.at(0), std::format("{} x", what));
    auto y = number_of(value.at(1), std::format("{} y", what));
    if (!x || !y) {
        return Unexpected(!x ? std::move(x.error()) : std::move(y.error()));
    }
    return engine::MaskPoint{*x, *y};
}

Expected<engine::ChromaDenoiseSetting> read_chroma_denoise(const Json& value) {
    if (auto status = exact_members(value, {"radius", "strength"}, "chroma_denoise", 2); !status) {
        return Unexpected(std::move(status.error()));
    }
    engine::ChromaDenoiseSetting out;
    if (auto status = read_numbers(value, {{"strength", &out.strength}, {"radius", &out.radius}}, "chroma_denoise.");
        !status) {
        return Unexpected(std::move(status.error()));
    }
    return out;
}

Expected<engine::GeometrySetting> read_geometry(const Json& value) {
    if (auto status =
            exact_members(value, {"crop", "flip_horizontal", "quarter_turns", "straighten_degrees"}, "geometry", 2);
        !status) {
        return Unexpected(std::move(status.error()));
    }
    engine::GeometrySetting out;
    const Json& crop = value.at("crop");
    if (auto status = exact_members(crop, {"bottom", "left", "right", "top"}, "geometry.crop", 2); !status) {
        return Unexpected(std::move(status.error()));
    }
    if (auto status = read_numbers(crop,
                                   {{"left", &out.crop.left},
                                    {"top", &out.crop.top},
                                    {"right", &out.crop.right},
                                    {"bottom", &out.crop.bottom}},
                                   "geometry.crop.");
        !status) {
        return Unexpected(std::move(status.error()));
    }
    if (auto status = read_numbers(value, {{"straighten_degrees", &out.straighten_degrees}}, "geometry."); !status) {
        return Unexpected(std::move(status.error()));
    }
    auto turns = unsigned_of(value.at("quarter_turns"), "geometry.quarter_turns");
    if (!turns) {
        return Unexpected(std::move(turns.error()));
    }
    out.quarter_turns = *turns;
    auto flip = bool_of(value.at("flip_horizontal"), "geometry.flip_horizontal");
    if (!flip) {
        return Unexpected(std::move(flip.error()));
    }
    out.flip_horizontal = *flip;
    return out;
}

Expected<engine::BrushStroke> read_stroke(const Json& value, const std::string& what) {
    if (auto status = exact_members(value, {"flow", "hardness", "mode", "points", "radius"}, what, 2); !status) {
        return Unexpected(std::move(status.error()));
    }
    engine::BrushStroke out;
    const Json& mode = value.at("mode");
    if (!mode.is_string()) {
        return Unexpected(invalid(std::format("{}.mode must be a string", what)));
    }
    const std::optional<engine::BrushMode> parsed = engine::parse_brush_mode(mode.get_ref<const std::string&>());
    if (!parsed) {
        return Unexpected(invalid(std::format("unknown brush mode '{}' in a version 2 recipe",
                                              mode.get_ref<const std::string&>())));
    }
    out.mode = *parsed;
    if (auto status = read_numbers(value, {{"radius", &out.radius}, {"hardness", &out.hardness}, {"flow", &out.flow}},
                                   what + ".");
        !status) {
        return Unexpected(std::move(status.error()));
    }
    const Json& points = value.at("points");
    if (!points.is_array()) {
        return Unexpected(invalid(std::format("{}.points must be an array of [x, y] points", what)));
    }
    if (points.size() > engine::kMaxStrokePoints) {
        return Unexpected(invalid(std::format("{} has {} points; a stroke has at most {}", what, points.size(),
                                              engine::kMaxStrokePoints)));
    }
    out.points.reserve(points.size());
    std::size_t index = 0;
    for (const Json& p : points) {
        auto point = read_point(p, std::format("{} point {}", what, index));
        if (!point) {
            return Unexpected(std::move(point.error()));
        }
        out.points.push_back(*point);
        ++index;
    }
    return out;
}

Expected<engine::MaskShape> read_mask(const Json& value, const std::string& what) {
    if (!value.is_object() || !value.contains("kind") || !value.at("kind").is_string()) {
        return Unexpected(invalid(std::format("{} must be an object with a string 'kind'", what)));
    }
    const std::string& kind_text = value.at("kind").get_ref<const std::string&>();
    const std::optional<engine::MaskKind> kind = engine::parse_mask_kind(kind_text);
    if (!kind) {
        return Unexpected(invalid(std::format("unknown mask kind '{}' in a version 2 recipe", kind_text)));
    }
    switch (*kind) {
        case engine::MaskKind::Brush: {
            if (auto status = exact_members(value, {"kind", "strokes"}, what + " (brush)", 2); !status) {
                return Unexpected(std::move(status.error()));
            }
            const Json& strokes = value.at("strokes");
            if (!strokes.is_array()) {
                return Unexpected(invalid(std::format("{}.strokes must be an array", what)));
            }
            if (strokes.size() > engine::kMaxBrushStrokes) {
                return Unexpected(invalid(std::format("{} has {} strokes; a brush mask has at most {}", what,
                                                      strokes.size(), engine::kMaxBrushStrokes)));
            }
            engine::BrushMask brush;
            brush.strokes.reserve(strokes.size());
            std::size_t index = 0;
            for (const Json& s : strokes) {
                auto stroke = read_stroke(s, std::format("{} stroke {}", what, index));
                if (!stroke) {
                    return Unexpected(std::move(stroke.error()));
                }
                brush.strokes.push_back(std::move(*stroke));
                ++index;
            }
            return engine::MaskShape{std::move(brush)};
        }
        case engine::MaskKind::LinearGradient: {
            if (auto status = exact_members(value, {"end", "feather", "kind", "start"}, what + " (linear)", 2);
                !status) {
                return Unexpected(std::move(status.error()));
            }
            engine::LinearGradientMask linear;
            auto start = read_point(value.at("start"), what + ".start");
            auto end = read_point(value.at("end"), what + ".end");
            if (!start || !end) {
                return Unexpected(!start ? std::move(start.error()) : std::move(end.error()));
            }
            linear.start = *start;
            linear.end = *end;
            if (auto status = read_numbers(value, {{"feather", &linear.feather}}, what + "."); !status) {
                return Unexpected(std::move(status.error()));
            }
            return engine::MaskShape{linear};
        }
        case engine::MaskKind::RadialGradient: {
            if (auto status = exact_members(value, {"angle_degrees", "center", "feather", "kind", "radius_x", "radius_y"},
                                            what + " (radial)", 2);
                !status) {
                return Unexpected(std::move(status.error()));
            }
            engine::RadialGradientMask radial;
            auto center = read_point(value.at("center"), what + ".center");
            if (!center) {
                return Unexpected(std::move(center.error()));
            }
            radial.center = *center;
            if (auto status = read_numbers(value,
                                           {{"radius_x", &radial.radius_x},
                                            {"radius_y", &radial.radius_y},
                                            {"angle_degrees", &radial.angle_degrees},
                                            {"feather", &radial.feather}},
                                           what + ".");
                !status) {
                return Unexpected(std::move(status.error()));
            }
            return engine::MaskShape{radial};
        }
    }
    return Unexpected(invalid(std::format("unknown mask kind '{}' in a version 2 recipe", kind_text)));
}

// ---- Version-3 members ------------------------------------------------------------------------

Expected<std::string> string_of(const Json& value, std::string_view what) {
    if (!value.is_string()) {
        return Unexpected(invalid(std::format("{} must be a string", what)));
    }
    return value.get<std::string>();
}

Expected<engine::LensCorrectionSetting> read_lens_correction(const Json& value) {
    if (auto status =
            exact_members(value, {"distortion", "lateral_chromatic_aberration", "profile", "vignetting"},
                          "lens_correction", 3);
        !status) {
        return Unexpected(std::move(status.error()));
    }
    engine::LensCorrectionSetting out;
    for (const auto& [name, flag] : {std::pair<std::string_view, bool*>{"distortion", &out.distortion},
                                     {"lateral_chromatic_aberration", &out.lateral_chromatic_aberration},
                                     {"vignetting", &out.vignetting}}) {
        auto read = bool_of(value.at(std::string(name)), std::format("lens_correction.{}", name));
        if (!read) {
            return Unexpected(std::move(read.error()));
        }
        *flag = *read;
    }
    const Json& profile = value.at("profile");
    if (profile.is_null()) {
        return out;  // automatic
    }
    if (auto status = exact_members(profile, {"crop_factor", "maker", "model"}, "lens_correction.profile", 3);
        !status) {
        return Unexpected(std::move(status.error()));
    }
    engine::LensProfileId id;
    auto maker = string_of(profile.at("maker"), "lens_correction.profile.maker");
    auto model = string_of(profile.at("model"), "lens_correction.profile.model");
    if (!maker || !model) {
        return Unexpected(!maker ? std::move(maker.error()) : std::move(model.error()));
    }
    id.maker = std::move(*maker);
    id.model = std::move(*model);
    if (auto status = read_numbers(profile, {{"crop_factor", &id.crop_factor}}, "lens_correction.profile.");
        !status) {
        return Unexpected(std::move(status.error()));
    }
    out.profile = std::move(id);
    return out;
}

Expected<std::vector<engine::LocalAdjustment>> read_local_adjustments(const Json& value) {
    if (!value.is_array()) {
        return Unexpected(invalid("local_adjustments must be an array"));
    }
    if (value.size() > engine::kMaxLocalAdjustments) {
        return Unexpected(invalid(std::format("the recipe has {} local adjustments; at most {} are allowed",
                                              value.size(), engine::kMaxLocalAdjustments)));
    }
    std::vector<engine::LocalAdjustment> out;
    out.reserve(value.size());
    std::size_t index = 0;
    for (const Json& item : value) {
        const std::string what = std::format("local adjustment {}", index);
        if (auto status = exact_members(item, {"controls", "enabled", "id", "invert", "mask", "opacity"}, what, 2);
            !status) {
            return Unexpected(std::move(status.error()));
        }
        engine::LocalAdjustment a;
        auto id = unsigned_of(item.at("id"), what + " id");
        if (!id) {
            return Unexpected(std::move(id.error()));
        }
        a.id = *id;
        auto enabled = bool_of(item.at("enabled"), what + " enabled");
        auto inverted = bool_of(item.at("invert"), what + " invert");
        if (!enabled || !inverted) {
            return Unexpected(!enabled ? std::move(enabled.error()) : std::move(inverted.error()));
        }
        a.enabled = *enabled;
        a.invert = *inverted;
        if (auto status = read_numbers(item, {{"opacity", &a.opacity}}, what + " "); !status) {
            return Unexpected(std::move(status.error()));
        }
        auto shape = read_mask(item.at("mask"), what + " mask");
        if (!shape) {
            return Unexpected(std::move(shape.error()));
        }
        a.shape = std::move(*shape);
        const Json& controls = item.at("controls");
        if (auto status = exact_members(
                controls, {"contrast", "exposure_ev", "highlights", "saturation", "shadows", "temperature", "tint"},
                what + " controls", 2);
            !status) {
            return Unexpected(std::move(status.error()));
        }
        engine::LocalControls& c = a.controls;
        if (auto status = read_numbers(controls,
                                       {{"exposure_ev", &c.exposure_ev},
                                        {"contrast", &c.contrast},
                                        {"highlights", &c.highlights},
                                        {"shadows", &c.shadows},
                                        {"saturation", &c.saturation},
                                        {"temperature", &c.temperature},
                                        {"tint", &c.tint}},
                                       what + " controls.");
            !status) {
            return Unexpected(std::move(status.error()));
        }
        out.push_back(std::move(a));
        ++index;
    }
    return out;
}

// Reads a version-1, version-2 or version-3 document (`version` is the document's).
Expected<engine::DevelopSettings> read_document(const Json& doc, std::uint32_t version) {
    const bool v2 = version >= 2;
    const bool v3 = version >= 3;
    Names members = {kSchemaKey,     kVersionKey,    kOperationsKey, kWhiteBalanceKey, kExposureKey,
                     kContrastKey,   kHighlightsKey, kShadowsKey,    kToneCurveKey,    kSaturationKey,
                     kVibranceKey,   kBaseLookKey,   kLocksKey};
    Names operation_names = {kOpWhiteBalance, kOpExposure, kOpLocalTone, kOpContrast, kOpToneCurve, kOpColor};
    if (v2) {
        members.insert(members.end(), {kChromaDenoiseKey, kLocalAdjustmentsKey, kGeometryKey});
        operation_names.insert(operation_names.end(), {kOpChromaDenoise, kOpLocalAdjustments, kOpGeometry});
    }
    if (v3) {
        members.push_back(kLensCorrectionKey);
        operation_names.push_back(kOpLensCorrection);
    }
    if (auto status = exact_members(doc, members, "the recipe", version); !status) {
        return Unexpected(std::move(status.error()));
    }
    engine::DevelopSettings s;
    s.version = version;

    const Json& operations = doc.at(std::string(kOperationsKey));
    if (auto status = exact_members(operations, operation_names, "operations", version); !status) {
        if (operations.is_object()) {
            // An operation name this version does not define is reported as such.
            return Unexpected(Error{ErrorCode::InvalidArgument,
                                    status.error().message + " (unknown operations are never treated as no-ops)",
                                    std::string(kWhere), 0});
        }
        return Unexpected(std::move(status.error()));
    }
    std::vector<std::tuple<std::string_view, std::uint32_t, std::uint32_t*, std::uint32_t>> ops = {
        {kOpWhiteBalance, engine::kWhiteBalanceOperationVersion, &s.operations.white_balance, 0},
        {kOpExposure, engine::kExposureOperationVersion, &s.operations.exposure, 0},
        {kOpLocalTone, engine::kLocalToneOperationVersion, &s.operations.local_tone, 0},
        {kOpContrast, engine::kContrastOperationVersion, &s.operations.contrast, 0},
        {kOpToneCurve, engine::kToneCurveOperationVersion, &s.operations.tone_curve, 0},
        {kOpColor, engine::kColorOperationVersion, &s.operations.color, 0},
    };
    if (v2) {
        // The geometry operation is read in versions 1 (the whole visible area) and 2 (the camera's
        // image area, ADR 0017); a recipe keeps the version it was made with.
        ops.insert(ops.end(), {{kOpChromaDenoise, engine::kChromaDenoiseOperationVersion,
                                &s.operations.chroma_denoise, 0},
                               {kOpLocalAdjustments, engine::kLocalAdjustmentsOperationVersion,
                                &s.operations.local_adjustments, 0},
                               {kOpGeometry, engine::kGeometryOperationVersion, &s.operations.geometry,
                                engine::kOldestGeometryOperationVersion}});
    }
    for (const auto& [name, supported, out, oldest] : ops) {
        if (auto status = operation_version(operations, name, supported, *out, oldest); !status) {
            return Unexpected(std::move(status.error()));
        }
    }
    if (v3) {
        // The lens correction is read in versions 0 ("none": settings made before the operation
        // existed, which a version-1 or version-2 recipe converts to) and 1 (ADR 0023).
        if (auto status =
                operation_version(operations, kOpLensCorrection, engine::kLensCorrectionOperationVersion,
                                  s.operations.lens_correction, 0, /*zero_allowed=*/true);
            !status) {
            return Unexpected(std::move(status.error()));
        }
    }

    const Json& look = doc.at(std::string(kBaseLookKey));
    if (auto status = exact_members(look, {"id", "version"}, "base_look", version); !status) {
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
        return Unexpected(
            newer(std::format("the recipe uses the base look '{}', which this build does not know", s.base_look.id)));
    }
    if (s.base_look.version != engine::kNeutralLookVersion) {
        return Unexpected(newer(std::format("the recipe uses version {} of the base look '{}'; this build implements "
                                            "version {}",
                                            s.base_look.version, s.base_look.id, engine::kNeutralLookVersion)));
    }

    auto white_balance = read_white_balance(doc.at(std::string(kWhiteBalanceKey)), version);
    if (!white_balance) {
        return Unexpected(std::move(white_balance.error()));
    }
    s.white_balance = *white_balance;

    if (auto status = read_numbers(doc,
                                   {{kExposureKey, &s.exposure_ev},
                                    {kContrastKey, &s.contrast},
                                    {kHighlightsKey, &s.highlights},
                                    {kShadowsKey, &s.shadows},
                                    {kSaturationKey, &s.saturation},
                                    {kVibranceKey, &s.vibrance}},
                                   "");
        !status) {
        return Unexpected(std::move(status.error()));
    }

    auto curve = read_tone_curve(doc.at(std::string(kToneCurveKey)));
    if (!curve) {
        return Unexpected(std::move(curve.error()));
    }
    s.tone_curve = std::move(*curve);

    auto locks = read_locks(doc.at(std::string(kLocksKey)), version);
    if (!locks) {
        return Unexpected(std::move(locks.error()));
    }
    s.locks = std::move(*locks);

    if (v2) {
        auto denoise = read_chroma_denoise(doc.at(std::string(kChromaDenoiseKey)));
        if (!denoise) {
            return Unexpected(std::move(denoise.error()));
        }
        s.chroma_denoise = *denoise;
        auto adjustments = read_local_adjustments(doc.at(std::string(kLocalAdjustmentsKey)));
        if (!adjustments) {
            return Unexpected(std::move(adjustments.error()));
        }
        s.local_adjustments = std::move(*adjustments);
        auto geometry = read_geometry(doc.at(std::string(kGeometryKey)));
        if (!geometry) {
            return Unexpected(std::move(geometry.error()));
        }
        s.geometry = *geometry;
    }
    if (v3) {
        auto lens = read_lens_correction(doc.at(std::string(kLensCorrectionKey)));
        if (!lens) {
            return Unexpected(std::move(lens.error()));
        }
        s.lens_correction = std::move(*lens);
    }

    if (auto valid = s.validate(); !valid) {
        const Error& e = valid.error();
        return Unexpected(Error{e.code, "the recipe's values are not valid: " + e.message, std::string(kWhere), 0});
    }
    if (version < kRecipeSchemaVersion) {
        // Version 1 or 2: its conversion (version 1: no denoising, no masks, identity geometry of
        // version 1; both: lens-correction operation version 0, nothing corrected). It renders
        // exactly as the document did.
        auto upgraded = engine::upgrade_develop_settings(std::move(s));
        if (!upgraded) {
            const Error& e = upgraded.error();
            return Unexpected(
                Error{e.code, std::format("the version {} recipe cannot be upgraded: {}", version, e.message),
                      std::string(kWhere), 0});
        }
        return upgraded;
    }
    return s;
}

}  // namespace

Expected<std::string> canonical_number(double value) {
    if (!std::isfinite(value)) {
        return fail(ErrorCode::InvalidArgument, std::format("{} has no JSON form", value), std::string(kWhere));
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
    const auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::scientific);
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
        // Older (valid) settings are written as their conversion: one identity per rendering.
        std::optional<engine::DevelopSettings> upgraded;
        if (settings.version != kRecipeSchemaVersion) {
            auto converted = engine::upgrade_develop_settings(settings);
            if (!converted) {
                return Unexpected(std::move(converted.error()));
            }
            upgraded = std::move(*converted);
        }
        RecipeDocument document;
        if (auto status = append_canonical(document.json, settings_json(upgraded ? *upgraded : settings)); !status) {
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
        if (schema == doc.end() || !schema->is_string() || schema->get_ref<const std::string&>() != kRecipeSchema) {
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
            return Unexpected(newer(std::format("the recipe has version {} of '{}'; this build reads versions {} to {} "
                                                "(it was made by a newer Lucent)",
                                                *version, kRecipeSchema, kOldestRecipeSchemaVersion,
                                                kRecipeSchemaVersion)));
        }
        if (*version < kOldestRecipeSchemaVersion) {
            return Unexpected(invalid(std::format("version {} of '{}' does not exist", *version, kRecipeSchema)));
        }
        return read_document(doc, *version);
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
        case RecipeCategory::Masks:
            return "masks";
        case RecipeCategory::Detail:
            return "detail";
        case RecipeCategory::Geometry:
            return "geometry";
        case RecipeCategory::LensCorrections:
            return "lens_corrections";
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

void copy_value(const engine::DevelopSettings& from, engine::DevelopSettings& to, engine::DevelopControl control) {
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

// ---- Parts -----------------------------------------------------------------------------------

std::string_view to_string(RecipePart part) noexcept {
    switch (part) {
        case RecipePart::ChromaDenoise:
            return "chroma_denoise";
        case RecipePart::LocalAdjustments:
            return "masks";
        case RecipePart::Geometry:
            return "geometry";
        case RecipePart::LensCorrection:
            return "lens_correction";
    }
    return "invalid";
}

std::optional<RecipePart> parse_recipe_part(std::string_view text) noexcept {
    for (const RecipePart part : kAllRecipeParts) {
        if (to_string(part) == text) {
            return part;
        }
    }
    return std::nullopt;
}

RecipeCategory category_of(RecipePart part) noexcept {
    switch (part) {
        case RecipePart::ChromaDenoise:
            return RecipeCategory::Detail;
        case RecipePart::LocalAdjustments:
            return RecipeCategory::Masks;
        case RecipePart::Geometry:
            return RecipeCategory::Geometry;
        case RecipePart::LensCorrection:
            return RecipeCategory::LensCorrections;
    }
    return RecipeCategory::Geometry;
}

std::vector<RecipePart> parts_of(RecipeCategory category) {
    std::vector<RecipePart> out;
    for (const RecipePart part : kAllRecipeParts) {
        if (category_of(part) == category) {
            out.push_back(part);
        }
    }
    return out;
}

bool same_value(const engine::DevelopSettings& a, const engine::DevelopSettings& b, RecipePart part) noexcept {
    switch (part) {
        case RecipePart::ChromaDenoise:
            return a.chroma_denoise == b.chroma_denoise;
        case RecipePart::LocalAdjustments:
            return a.local_adjustments == b.local_adjustments;
        case RecipePart::Geometry:
            // The geometry part is the value and the frame its operation version places it in.
            return a.geometry == b.geometry &&
                   engine::geometry_operation_version(a) == engine::geometry_operation_version(b);
        case RecipePart::LensCorrection:
            // What renders: the operation version (0: none, the setting unused) and the setting.
            return engine::lens_correction_operation_version(a) ==
                       engine::lens_correction_operation_version(b) &&
                   engine::effective_lens_correction(a) == engine::effective_lens_correction(b);
    }
    return true;
}

void copy_value(const engine::DevelopSettings& from, engine::DevelopSettings& to, RecipePart part) {
    switch (part) {
        case RecipePart::ChromaDenoise:
            to.chroma_denoise = from.chroma_denoise;
            return;
        case RecipePart::LocalAdjustments:
            to.local_adjustments = from.local_adjustments;
            return;
        case RecipePart::Geometry:
            // With its operation version: a crop of version 1 is relative to the whole visible area,
            // one of version 2 to the camera's image area (ADR 0017).
            to.geometry = from.geometry;
            to.operations.geometry = engine::geometry_operation_version(from);
            return;
        case RecipePart::LensCorrection:
            // With its operation version: version 0 (settings made before lens corrections existed)
            // corrects nothing, whatever the member holds (it then holds the default).
            to.operations.lens_correction = engine::lens_correction_operation_version(from);
            to.lens_correction =
                to.operations.lens_correction == engine::kOldestLensCorrectionOperationVersion
                    ? engine::LensCorrectionSetting{}
                    : from.lens_correction;
            return;
    }
}

std::vector<RecipePart> changed_parts(const engine::DevelopSettings& a, const engine::DevelopSettings& b) {
    std::vector<RecipePart> out;
    for (const RecipePart part : kAllRecipeParts) {
        if (!same_value(a, b, part)) {
            out.push_back(part);
        }
    }
    return out;
}

}  // namespace lucent::app
