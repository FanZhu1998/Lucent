#include "expectations.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <stdexcept>
#include <utility>

#include <lucent/core/text.hpp>
#include <lucent/raw/decoded_raw.hpp>

#include "mode_key.hpp"

namespace lucent::qualify {

namespace {

using Json = nlohmann::json;

constexpr std::string_view kWhere = "qualify.expectations";

// Internal: thrown while parsing, converted to an Error at the boundary.
struct ParseError : std::runtime_error {
    ErrorCode code;
    ParseError(ErrorCode c, const std::string& message) : std::runtime_error(message), code(c) {}
};

[[noreturn]] void invalid(const std::string& message) {
    throw ParseError(ErrorCode::InvalidArgument, message);
}

std::string text(const Json& object, const char* key, bool required = true) {
    if (!object.contains(key) || object.at(key).is_null()) {
        if (required) {
            invalid(std::string("missing \"") + key + "\"");
        }
        return {};
    }
    if (!object.at(key).is_string()) {
        invalid(std::string("\"") + key + "\" is not a string");
    }
    return object.at(key).get<std::string>();
}

std::optional<Size2> size_of(const Json& value, const std::string& what) {
    if (value.is_null()) {
        return std::nullopt;
    }
    if (!value.is_array() || value.size() != 2 || !value[0].is_number_integer() ||
        !value[1].is_number_integer()) {
        invalid(what + " is not [width, height]");
    }
    Size2 size{value[0].get<std::int32_t>(), value[1].get<std::int32_t>()};
    if (size.width < 1 || size.height < 1) {
        invalid(what + " is not positive");
    }
    return size;
}

Fact fact_of(const Json& object, const std::string& what, const std::map<std::string, Source>& sources) {
    Fact fact;
    fact.status = text(object, "status");
    if (fact.status != "verified" && fact.status != "unverified" && fact.status != "not_applicable") {
        invalid(what + ": unknown status \"" + fact.status + "\"");
    }
    if (object.contains("sources")) {
        for (const Json& id : object.at("sources")) {
            if (!id.is_string()) {
                invalid(what + ": a source id is not a string");
            }
            fact.sources.push_back(id.get<std::string>());
        }
    }
    fact.note = text(object, "note", false);
    bool independent = false;
    for (const std::string& id : fact.sources) {
        const auto it = sources.find(id);
        if (it == sources.end()) {
            invalid(what + ": unknown source \"" + id + "\"");
        }
        independent = independent || is_independent_source_kind(it->second.kind);
    }
    if (fact.verified() && !independent) {
        invalid(what +
                " is \"verified\" without an independent source (manufacturer_spec, file_tag, "
                "independent_decoder or reviewer_visual)");
    }
    return fact;
}

FixtureExpectation fixture_of(const Json& f, const std::map<std::string, Source>& sources) {
    FixtureExpectation e;
    e.id = text(f, "id");
    const std::string at = "fixture " + e.id;

    const Json& camera = f.at("camera");
    e.family = text(camera, "family");
    e.file_make = text(camera, "file_make");
    e.file_model = text(camera, "file_model");
    e.product = text(camera, "product");
    e.identity = fact_of(camera.at("identity"), at + " camera.identity", sources);

    const Json& mode = f.at("mode");
    e.mode_key = text(mode, "key");
    e.mode_label = text(mode, "label");
    e.compression = fact_of(mode.at("compression"), at + " mode.compression", sources);

    const Json& dims = f.at("dimensions");
    e.rendered = size_of(dims.at("rendered"), at + " dimensions.rendered");
    const Json& area = dims.at("camera_image_area");
    e.camera_image_area = size_of(area.at("size"), at + " dimensions.camera_image_area.size");
    e.camera_image_area_fact = fact_of(area, at + " dimensions.camera_image_area", sources);
    e.dimensions_note = text(dims, "note", false);
    if (dims.contains("image_area_source")) {
        e.image_area_source = text(dims, "image_area_source");
        constexpr std::array kAreaSources{raw::ImageAreaSource::Unknown, raw::ImageAreaSource::DngDefaultCrop,
                                          raw::ImageAreaSource::DngDefaultWholeImage,
                                          raw::ImageAreaSource::DecoderInsetCrop};
        if (std::none_of(kAreaSources.begin(), kAreaSources.end(),
                         [&](raw::ImageAreaSource s) { return raw::to_string(s) == *e.image_area_source; })) {
            invalid(at + ": dimensions.image_area_source '" + *e.image_area_source +
                    "' is not an image area source");
        }
    }
    if (dims.contains("image_area_applied")) {
        const Json& applied = dims.at("image_area_applied");
        e.image_area_applied_asserted = true;
        if (!applied.is_null()) {
            if (!applied.is_array() || applied.size() != 4 ||
                std::any_of(applied.begin(), applied.end(), [](const Json& v) { return !v.is_number_integer(); })) {
                invalid(at + ": dimensions.image_area_applied must be null or [x, y, width, height]");
            }
            e.image_area_applied = std::array<std::int32_t, 4>{applied[0].get<std::int32_t>(), applied[1].get<std::int32_t>(),
                                                               applied[2].get<std::int32_t>(), applied[3].get<std::int32_t>()};
        }
    }

    const Json& orientation = f.at("orientation");
    e.orientation_exif = orientation.at("exif").get<int>();
    if (e.orientation_exif < 1 || e.orientation_exif > 8) {
        invalid(at + ": orientation.exif outside 1..8");
    }
    e.orientation = fact_of(orientation, at + " orientation", sources);

    const Json& rep = f.at("representation");
    e.representation = text(rep, "value");
    e.representation_fact = fact_of(rep, at + " representation", sources);

    if (f.contains("mandatory_steps")) {
        for (const Json& s : f.at("mandatory_steps")) {
            MandatoryStep step{text(s, "id"), text(s, "lucent_support"), text(s, "description", false)};
            if (step.lucent_support != "applied" && step.lucent_support != "decoder" &&
                step.lucent_support != "unsupported") {
                invalid(at + ": mandatory step " + step.id + " has an unknown lucent_support");
            }
            e.mandatory_steps.push_back(std::move(step));
        }
    }

    const Json& expected = f.at("expected");
    e.expected_state = text(expected, "state");
    if (!is_state_id(e.expected_state) || e.expected_state == "not_qualified") {
        invalid(at + ": unknown expected state \"" + e.expected_state + "\"");
    }
    for (const auto& [check, status] : expected.at("checks").items()) {
        const auto& ids = check_ids();
        if (std::find(ids.begin(), ids.end(), check) == ids.end()) {
            invalid(at + ": unknown check \"" + check + "\"");
        }
        if (!status.is_string() || !is_check_status_id(status.get<std::string>())) {
            invalid(at + ": check " + check + " has an unknown status");
        }
        e.expected_checks[check] = status.get<std::string>();
    }
    for (const auto& id : check_ids()) {
        if (!e.expected_checks.contains(std::string(id))) {
            invalid(at + ": expected.checks lacks \"" + std::string(id) + "\"");
        }
    }
    for (const Json& l : expected.at("limitations")) {
        const std::string id = l.get<std::string>();
        const auto& ids = limitation_ids();
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
            invalid(at + ": unknown limitation \"" + id + "\"");
        }
        e.expected_limitations.push_back(id);
    }
    std::sort(e.expected_limitations.begin(), e.expected_limitations.end());
    e.notes = text(f, "notes", false);
    return e;
}

}  // namespace

const std::vector<std::string_view>& check_ids() {
    static const std::vector<std::string_view> ids{kCheckFixtureIntegrity,
                                                   kCheckDecodeParity,
                                                   kCheckModeKey,
                                                   kCheckSubimages,
                                                   kCheckInterpretation,
                                                   kCheckCfa,
                                                   kCheckBlackWhite,
                                                   kCheckColour,
                                                   kCheckDimensions,
                                                   kCheckOrientation,
                                                   kCheckRequiredCorrections,
                                                   kCheckNumericalValidity,
                                                   kCheckExportRoundTrip,
                                                   kCheckDeterminism};
    return ids;
}

const std::vector<std::string_view>& limitation_ids() {
    static const std::vector<std::string_view> ids{
        kLimitDefaultCrop,        kLimitXTransBaseline,  kLimitDualPixel, kLimitEnhancedImage,
        kLimitOptionalOperations, kLimitNonSquarePixels, kLimitPixelShift};
    return ids;
}

bool is_state_id(std::string_view id) noexcept {
    return id == "verified" || id == "compatible_unverified" || id == "limited" || id == "unsupported" ||
           id == "not_qualified";
}

bool is_check_status_id(std::string_view id) noexcept {
    return id == "pass" || id == "fail" || id == "not_applicable" || id == "not_run";
}

bool is_independent_source_kind(std::string_view kind) noexcept {
    return kind == "manufacturer_spec" || kind == "file_tag" || kind == "independent_decoder" ||
           kind == "reviewer_visual";
}

bool is_source_kind(std::string_view kind) noexcept {
    return is_independent_source_kind(kind) || kind == "decoder_reference" || kind == "archive_metadata";
}

const FixtureExpectation* Expectations::find(std::string_view id) const noexcept {
    const auto it = std::find_if(fixtures.begin(), fixtures.end(),
                                 [&](const FixtureExpectation& f) { return f.id == id; });
    return it == fixtures.end() ? nullptr : &*it;
}

Expected<Expectations> parse_expectations(const nlohmann::json& document) {
    try {
        if (!document.is_object() || document.value("schema", std::string()) != kExpectationsSchema) {
            return fail(ErrorCode::VersionMismatch, "not a lucent.qualification-expectations document",
                        std::string(kWhere));
        }
        Expectations e;
        e.schema_version = document.at("schema_version").get<int>();
        if (e.schema_version > kExpectationsSchemaVersion || e.schema_version < 1) {
            return fail(
                ErrorCode::VersionMismatch,
                "expectations schema_version " + std::to_string(e.schema_version) + " is not readable",
                std::string(kWhere));
        }
        e.mode_key_version = document.at("mode_key_version").get<std::uint32_t>();
        if (e.mode_key_version != kModeKeyVersion) {
            return fail(ErrorCode::VersionMismatch,
                        "expectations use mode key version " + std::to_string(e.mode_key_version) +
                            "; this build derives version " + std::to_string(kModeKeyVersion),
                        std::string(kWhere));
        }
        e.reviewed = text(document, "reviewed");
        for (const auto& [id, s] : document.at("sources").items()) {
            Source source{id,
                          text(s, "kind"),
                          text(s, "title"),
                          text(s, "url", false),
                          text(s, "retrieved", false),
                          text(s, "quote", false)};
            if (!is_source_kind(source.kind)) {
                invalid("source " + id + ": unknown kind \"" + source.kind + "\"");
            }
            e.sources.emplace(id, std::move(source));
        }
        std::set<std::string> seen;
        for (const Json& f : document.at("fixtures")) {
            FixtureExpectation fixture = fixture_of(f, e.sources);
            if (!seen.insert(fixture.id).second) {
                invalid("duplicate fixture " + fixture.id);
            }
            e.fixtures.push_back(std::move(fixture));
        }
        return e;
    } catch (const ParseError& error) {
        return fail(error.code, error.what(), std::string(kWhere));
    } catch (const nlohmann::json::exception& error) {
        return fail(ErrorCode::InvalidArgument, std::string("malformed expectations: ") + error.what(),
                    std::string(kWhere));
    }
}

Expected<nlohmann::json> read_json_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return fail(ErrorCode::NotFound, "cannot open " + core::path_to_utf8_lossy(path), "qualify.json");
    }
    try {
        return nlohmann::json::parse(in);
    } catch (const nlohmann::json::exception& error) {
        return fail(ErrorCode::InvalidArgument, core::path_to_utf8_lossy(path) + ": " + error.what(),
                    "qualify.json");
    }
}

Expected<Expectations> load_expectations(const std::filesystem::path& path) {
    auto document = read_json_file(path);
    if (!document) {
        return Unexpected(std::move(document.error()));
    }
    return parse_expectations(*document);
}

}  // namespace lucent::qualify
