#include <algorithm>
#include <format>
#include <new>
#include <set>
#include <utility>

#include <lucent/assets/camera_support.hpp>
#include <lucent/assets/mode_key.hpp>
#include <lucent/engine/develop.hpp>
#include <lucent/engine/develop_settings.hpp>
#include <lucent/engine/dng_opcode_processing.hpp>
#include <lucent/engine/image_area.hpp>
#include <lucent/engine/render.hpp>
#include <lucent/export/export.hpp>
#include <lucent/raw/decoded_raw.hpp>
#include <lucent/raw/libraw_decoder.hpp>

#include "json_reader.hpp"

namespace lucent::assets {

namespace {

constexpr std::string_view kWhere = "assets.camera_support";
constexpr std::size_t kModeKeyComponents = 10;  // after "mk1"

// Splits `text` at '|'.
std::vector<std::string_view> fields(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t bar = text.find('|', start);
        if (bar == std::string_view::npos) {
            out.push_back(text.substr(start));
            return out;
        }
        out.push_back(text.substr(start, bar - start));
        start = bar + 1;
    }
}

bool digits(std::string_view text, std::initializer_list<std::size_t> at) {
    for (const std::size_t i : at) {
        if (i >= text.size() || text[i] < '0' || text[i] > '9') {
            return false;
        }
    }
    return true;
}

// "YYYY-MM-DDTHH:MM:SSZ".
bool utc_time(std::string_view text) {
    return text.size() == 20 && text[4] == '-' && text[7] == '-' && text[10] == 'T' && text[13] == ':' &&
           text[16] == ':' && text[19] == 'Z' &&
           digits(text, {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18});
}

std::string entry_name(const CameraSupportEntry& entry) {
    return entry.match.empty() ? std::string("an entry") : std::format("entry '{}'", entry.match);
}

// The rules of parse_camera_support() that the writer also enforces; empty when they hold.
std::string database_problem(const CameraSupportDatabase& database) {
    if (!utc_time(database.generated)) {
        return std::format("qualification.generated '{}' is not a UTC time YYYY-MM-DDTHH:MM:SSZ",
                           database.generated);
    }
    if (database.date != database.generated.substr(0, 10)) {
        return std::format("qualification.date '{}' is not the date of '{}'", database.date,
                           database.generated);
    }
    if (database.basis.decoder.empty() ||
        !core::ContractVersion::parse(database.basis.decoded_raw_contract) || database.basis.mode_key == 0) {
        return "qualification.basis names no decoder, contract or mode-key version";
    }
    if (database.basis.develop_engine.empty() || database.basis.neutral_look == 0 ||
        database.basis.export_version == 0) {
        return "qualification.basis names no develop-engine, neutral-look or export version";
    }
    std::set<std::string_view> codes;
    for (std::size_t i = 0; i < database.limitations.size(); ++i) {
        const LimitationText& text = database.limitations[i];
        if (text.code.empty() || text.summary.empty() || text.meaning.empty()) {
            return std::format("limitation '{}' has an empty code, summary or meaning", text.code);
        }
        if (!codes.insert(text.code).second) {
            return std::format("limitation '{}' is listed twice", text.code);
        }
    }
    std::set<std::string_view> matches;
    for (const CameraSupportEntry& entry : database.entries) {
        auto pattern = ModePattern::parse(entry.match);
        if (!pattern) {
            return pattern.error().message;
        }
        if (!matches.insert(entry.match).second) {
            return std::format("{} is listed twice", entry_name(entry));
        }
        if (static_cast<std::uint8_t>(entry.state) < 1 || static_cast<std::uint8_t>(entry.state) > 4) {
            return std::format("{} has an invalid state", entry_name(entry));
        }
        if (entry.state == SupportState::Verified && pattern->wildcards() > 0) {
            return std::format("{} is Verified but not an exact mode key", entry_name(entry));
        }
        auto qualified = ModePattern::parse(entry.qualified_key);
        if (!qualified) {
            return std::format("{} has an invalid qualified_key: {}", entry_name(entry),
                               qualified.error().message);
        }
        if (entry.state == SupportState::Verified && qualified->wildcards() > 0) {
            return std::format("{} is Verified but its qualified_key is not an exact mode key",
                               entry_name(entry));
        }
        if (entry.image_area.empty()) {
            return std::format("{} names no image area", entry_name(entry));
        }
        if (entry.state == SupportState::Verified && entry.image_area == "*") {
            return std::format("{} is Verified but its image area is a wildcard", entry_name(entry));
        }
        const bool limited = entry.state == SupportState::Limited;
        const bool clean =
            entry.state == SupportState::Verified || entry.state == SupportState::CompatibleUnverified;
        if (limited && entry.limitations.empty()) {
            return std::format("{} is Limited but names no limitation", entry_name(entry));
        }
        if (clean && !entry.limitations.empty()) {
            return std::format("{} is {} but names a limitation", entry_name(entry), to_string(entry.state));
        }
        std::set<std::string_view> own;
        for (const EntryLimitation& limitation : entry.limitations) {
            if (!codes.contains(limitation.code)) {
                return std::format("{} names limitation '{}', which has no text", entry_name(entry),
                                   limitation.code);
            }
            if (!own.insert(limitation.code).second) {
                return std::format("{} names limitation '{}' twice", entry_name(entry), limitation.code);
            }
        }
    }
    return {};
}

void sort_database(CameraSupportDatabase& database) {
    std::sort(database.limitations.begin(), database.limitations.end(),
              [](const LimitationText& a, const LimitationText& b) { return a.code < b.code; });
    for (CameraSupportEntry& entry : database.entries) {
        std::sort(entry.limitations.begin(), entry.limitations.end(),
                  [](const EntryLimitation& a, const EntryLimitation& b) { return a.code < b.code; });
    }
    std::sort(database.entries.begin(), database.entries.end(),
              [](const CameraSupportEntry& a, const CameraSupportEntry& b) { return a.match < b.match; });
}

detail::OrderedJson strings_json(const std::vector<std::string>& values) {
    detail::OrderedJson out = detail::OrderedJson::array();
    for (const std::string& value : values) {
        out.push_back(value);
    }
    return out;
}

}  // namespace

std::string_view to_string(SupportState state) noexcept {
    switch (state) {
        case SupportState::Verified:
            return "verified";
        case SupportState::CompatibleUnverified:
            return "compatible_unverified";
        case SupportState::Limited:
            return "limited";
        case SupportState::Unsupported:
            return "unsupported";
    }
    return "invalid";
}

std::optional<SupportState> parse_support_state(std::string_view text) noexcept {
    for (const SupportState state : {SupportState::Verified, SupportState::CompatibleUnverified,
                                     SupportState::Limited, SupportState::Unsupported}) {
        if (to_string(state) == text) {
            return state;
        }
    }
    return std::nullopt;
}

// ---- ModePattern --------------------------------------------------------------------------------

Expected<ModePattern> ModePattern::parse(std::string_view text) {
    const std::vector<std::string_view> parts = fields(text);
    if (parts.size() != kModeKeyComponents + 1 || parts.front() != "mk1") {
        return fail(ErrorCode::Corrupt,
                    std::format("'{}' is not a version-1 mode-key pattern (mk1 and ten components)", text),
                    std::string(kWhere));
    }
    ModePattern pattern;
    pattern.text_ = std::string(text);
    for (std::size_t i = 1; i < parts.size(); ++i) {
        const std::string_view part = parts[i];
        if (part.empty() || (part != "*" && part.find('*') != std::string_view::npos)) {
            return fail(
                ErrorCode::Corrupt,
                std::format("component {} of pattern '{}' must be '*' or a value without '*'", i, text),
                std::string(kWhere));
        }
        pattern.components_.emplace_back(part);
    }
    return pattern;
}

bool ModePattern::matches(std::string_view mode_key) const noexcept {
    const std::vector<std::string_view> parts = fields(mode_key);
    if (parts.size() != components_.size() + 1 || parts.front() != "mk1") {
        return false;
    }
    for (std::size_t i = 0; i < components_.size(); ++i) {
        if (components_[i] != "*" && components_[i] != parts[i + 1]) {
            return false;
        }
    }
    return true;
}

std::size_t ModePattern::wildcards() const noexcept {
    return static_cast<std::size_t>(std::count(components_.begin(), components_.end(), "*"));
}

// ---- CameraSupportDatabase ----------------------------------------------------------------------

std::optional<CameraSupportMatch> CameraSupportDatabase::lookup(std::string_view mode_key) const {
    CameraSupportMatch match;
    std::size_t best_wildcards = 0;
    std::set<std::string> codes;
    for (const CameraSupportEntry& entry : entries) {
        // An exact key (every exported entry) matches exactly its own text; only patterns with
        // wildcards are compared component by component.
        std::size_t wildcards = 0;
        if (entry.match.find('*') == std::string::npos) {
            if (entry.match != mode_key) {
                continue;
            }
        } else {
            auto pattern = ModePattern::parse(entry.match);
            if (!pattern || !pattern->matches(mode_key)) {
                continue;
            }
            wildcards = pattern->wildcards();
        }
        if (match.entry == nullptr || wildcards < best_wildcards ||
            (wildcards == best_wildcards && entry.match < match.entry->match)) {
            match.entry = &entry;
            best_wildcards = wildcards;
        }
        match.state = match.matches == 0 ? entry.state : worse(match.state, entry.state);
        ++match.matches;
        for (const EntryLimitation& limitation : entry.limitations) {
            codes.insert(limitation.code);
        }
    }
    if (match.matches == 0) {
        return std::nullopt;
    }
    match.limitations.assign(codes.begin(), codes.end());
    return match;
}

const LimitationText* CameraSupportDatabase::limitation(std::string_view code) const noexcept {
    for (const LimitationText& text : limitations) {
        if (text.code == code) {
            return &text;
        }
    }
    return nullptr;
}

SupportSummary CameraSupportDatabase::count() const noexcept {
    SupportSummary counts;
    counts.modes = static_cast<std::uint32_t>(entries.size());
    for (const CameraSupportEntry& entry : entries) {
        switch (entry.state) {
            case SupportState::Verified:
                ++counts.verified;
                break;
            case SupportState::CompatibleUnverified:
                ++counts.compatible_unverified;
                break;
            case SupportState::Limited:
                ++counts.limited;
                break;
            case SupportState::Unsupported:
                ++counts.unsupported;
                break;
        }
    }
    return counts;
}

// ---- Documents ----------------------------------------------------------------------------------

Expected<CameraSupportDatabase> parse_camera_support(std::string_view json) {
    return detail::guarded(kWhere, "camera support database", [&]() -> Expected<CameraSupportDatabase> {
        using detail::Json;
        const Json in = detail::parse(json);
        detail::require_object(in, "the database");
        const std::string schema = detail::text(in, "schema", "database");
        if (schema != kCameraSupportSchema) {
            throw detail::DocumentError{ErrorCode::VersionMismatch,
                                        std::format("schema '{}' is not '{}'", schema, kCameraSupportSchema)};
        }
        const std::uint64_t version = detail::unsigned_number(in, "schema_version", "database");
        if (version == 0) {
            detail::corrupt("schema_version 0 is not a version");
        }
        if (version > kCameraSupportSchemaVersion) {
            throw detail::DocumentError{ErrorCode::VersionMismatch,
                                        std::format("schema_version {} is newer than this build reads ({})",
                                                    version, kCameraSupportSchemaVersion)};
        }
        if (version < kOldestCameraSupportSchemaVersion) {
            throw detail::DocumentError{
                ErrorCode::VersionMismatch,
                std::format("schema_version {} is older than this build reads ({}): it lacks the basis and the "
                            "probe facts a Verified state needs",
                            version, kOldestCameraSupportSchemaVersion)};
        }
        detail::only_members(in, {"schema", "schema_version", "qualification", "limitations", "entries"},
                             "database");

        CameraSupportDatabase database;
        const Json& q = detail::member(in, "qualification", "database");
        detail::require_object(q, "qualification");
        detail::only_members(q, {"generated", "date", "tool", "report", "basis", "summary"}, "qualification");
        database.generated = detail::text(q, "generated", "qualification");
        database.date = detail::text(q, "date", "qualification");
        database.tool = detail::text(q, "tool", "qualification");
        database.report = detail::text(q, "report", "qualification");
        const Json& b = detail::member(q, "basis", "qualification");
        detail::require_object(b, "qualification.basis");
        detail::only_members(b,
                             {"decoder", "decoder_capabilities", "decoded_raw_contract", "mode_key", "check_suite",
                              "state_rules", "render_pipeline", "develop_pipeline", "dng_opcode_processing",
                              "geometry_operation", "image_area_placement", "develop_engine", "neutral_look",
                              "export"},
                             "qualification.basis");
        QualificationBasis& basis = database.basis;
        basis.decoder = detail::text(b, "decoder", "basis");
        basis.decoder_capabilities = detail::strings(b, "decoder_capabilities", "basis");
        basis.decoded_raw_contract = detail::text(b, "decoded_raw_contract", "basis");
        basis.mode_key = detail::small_number(b, "mode_key", "basis");
        basis.check_suite = detail::small_number(b, "check_suite", "basis");
        basis.state_rules = detail::small_number(b, "state_rules", "basis");
        basis.render_pipeline = detail::small_number(b, "render_pipeline", "basis");
        basis.develop_pipeline = detail::small_number(b, "develop_pipeline", "basis");
        basis.dng_opcode_processing = detail::small_number(b, "dng_opcode_processing", "basis");
        basis.geometry_operation = detail::small_number(b, "geometry_operation", "basis");
        basis.image_area_placement = detail::small_number(b, "image_area_placement", "basis");
        basis.develop_engine = detail::small_numbers(b, "develop_engine", "basis");
        basis.neutral_look = detail::small_number(b, "neutral_look", "basis");
        basis.export_version = detail::small_number(b, "export", "basis");
        const Json& s = detail::member(q, "summary", "qualification");
        detail::require_object(s, "qualification.summary");
        detail::only_members(s, {"modes", "verified", "compatible_unverified", "limited", "unsupported"},
                             "qualification.summary");
        database.summary.modes = detail::small_number(s, "modes", "summary");
        database.summary.verified = detail::small_number(s, "verified", "summary");
        database.summary.compatible_unverified = detail::small_number(s, "compatible_unverified", "summary");
        database.summary.limited = detail::small_number(s, "limited", "summary");
        database.summary.unsupported = detail::small_number(s, "unsupported", "summary");

        for (const Json& l : detail::array(in, "limitations", "database")) {
            detail::require_object(l, "a limitation");
            detail::only_members(l, {"code", "summary", "meaning"}, "a limitation");
            database.limitations.push_back(LimitationText{detail::text(l, "code", "limitation"),
                                                          detail::text(l, "summary", "limitation"),
                                                          detail::text(l, "meaning", "limitation")});
        }
        for (const Json& e : detail::array(in, "entries", "database")) {
            detail::require_object(e, "an entry");
            detail::only_members(e,
                                 {"match", "qualified_key", "image_area", "dng_opcodes", "state", "camera", "mode",
                                  "family", "limitations", "evidence", "evidence_gaps", "fixtures"},
                                 "an entry");
            CameraSupportEntry entry;
            entry.match = detail::text(e, "match", "entry");
            entry.qualified_key = detail::text(e, "qualified_key", "entry");
            entry.image_area = detail::text(e, "image_area", "entry");
            entry.dng_opcodes = detail::strings(e, "dng_opcodes", "entry");
            const std::string state = detail::text(e, "state", "entry");
            const std::optional<SupportState> parsed = parse_support_state(state);
            if (!parsed) {
                detail::corrupt(std::format("entry '{}' has the unknown state '{}'", entry.match, state));
            }
            entry.state = *parsed;
            entry.camera = detail::text(e, "camera", "entry");
            entry.mode = detail::text(e, "mode", "entry");
            entry.family = detail::text(e, "family", "entry");
            for (const Json& l : detail::array(e, "limitations", "entry")) {
                detail::require_object(l, "an entry limitation");
                detail::only_members(l, {"code", "detail"}, "an entry limitation");
                entry.limitations.push_back(EntryLimitation{detail::text(l, "code", "limitation"),
                                                            detail::text(l, "detail", "limitation")});
            }
            entry.evidence = detail::text(e, "evidence", "entry");
            entry.evidence_gaps = detail::strings(e, "evidence_gaps", "entry");
            entry.fixtures = detail::strings(e, "fixtures", "entry");
            database.entries.push_back(std::move(entry));
        }
        if (const std::string problem = database_problem(database); !problem.empty()) {
            detail::corrupt(problem);
        }
        if (database.summary != database.count()) {
            detail::corrupt("qualification.summary does not count the entries");
        }
        sort_database(database);
        return database;
    });
}

Expected<std::string> write_camera_support(const CameraSupportDatabase& input) {
    CameraSupportDatabase database = input;
    sort_database(database);
    database.summary = database.count();
    if (const std::string problem = database_problem(database); !problem.empty()) {
        return fail(ErrorCode::InvalidArgument, problem, std::string(kWhere));
    }
    return detail::guarded(kWhere, "camera support database", [&]() -> Expected<std::string> {
        using detail::OrderedJson;
        OrderedJson out = OrderedJson::object();
        out["schema"] = kCameraSupportSchema;
        out["schema_version"] = kCameraSupportSchemaVersion;
        OrderedJson q = OrderedJson::object();
        q["generated"] = database.generated;
        q["date"] = database.date;
        q["tool"] = database.tool;
        q["report"] = database.report;
        const QualificationBasis& basis = database.basis;
        OrderedJson engine_versions = OrderedJson::array();
        for (const std::uint32_t version : basis.develop_engine) {
            engine_versions.push_back(version);
        }
        q["basis"] = OrderedJson{{"decoder", basis.decoder},
                                 {"decoder_capabilities", strings_json(basis.decoder_capabilities)},
                                 {"decoded_raw_contract", basis.decoded_raw_contract},
                                 {"mode_key", basis.mode_key},
                                 {"check_suite", basis.check_suite},
                                 {"state_rules", basis.state_rules},
                                 {"render_pipeline", basis.render_pipeline},
                                 {"develop_pipeline", basis.develop_pipeline},
                                 {"dng_opcode_processing", basis.dng_opcode_processing},
                                 {"geometry_operation", basis.geometry_operation},
                                 {"image_area_placement", basis.image_area_placement},
                                 {"develop_engine", std::move(engine_versions)},
                                 {"neutral_look", basis.neutral_look},
                                 {"export", basis.export_version}};
        q["summary"] = OrderedJson{{"modes", database.summary.modes},
                                   {"verified", database.summary.verified},
                                   {"compatible_unverified", database.summary.compatible_unverified},
                                   {"limited", database.summary.limited},
                                   {"unsupported", database.summary.unsupported}};
        out["qualification"] = std::move(q);
        OrderedJson limitations = OrderedJson::array();
        for (const LimitationText& text : database.limitations) {
            limitations.push_back(
                OrderedJson{{"code", text.code}, {"summary", text.summary}, {"meaning", text.meaning}});
        }
        out["limitations"] = std::move(limitations);
        OrderedJson entries = OrderedJson::array();
        for (const CameraSupportEntry& entry : database.entries) {
            OrderedJson e = OrderedJson::object();
            e["match"] = entry.match;
            e["qualified_key"] = entry.qualified_key;
            e["image_area"] = entry.image_area;
            e["dng_opcodes"] = strings_json(entry.dng_opcodes);
            e["state"] = to_string(entry.state);
            e["camera"] = entry.camera;
            e["mode"] = entry.mode;
            e["family"] = entry.family;
            OrderedJson own = OrderedJson::array();
            for (const EntryLimitation& limitation : entry.limitations) {
                own.push_back(OrderedJson{{"code", limitation.code}, {"detail", limitation.detail}});
            }
            e["limitations"] = std::move(own);
            e["evidence"] = entry.evidence;
            e["evidence_gaps"] = strings_json(entry.evidence_gaps);
            e["fixtures"] = strings_json(entry.fixtures);
            entries.push_back(std::move(e));
        }
        out["entries"] = std::move(entries);
        return out.dump(1, ' ', false, nlohmann::ordered_json::error_handler_t::strict) + "\n";
    });
}

// ---- Basis --------------------------------------------------------------------------------------

QualificationBasis running_basis() {
    QualificationBasis basis;
    const raw::LibRawBuildInfo decoder = raw::libraw_build_info();
    basis.decoder = "LibRaw " + decoder.version;
    basis.decoder_capabilities = decoder.capabilities;
    std::sort(basis.decoder_capabilities.begin(), basis.decoder_capabilities.end());
    basis.decoded_raw_contract = raw::kDecodedRawContractVersion.to_string();
    basis.mode_key = kModeKeyVersion;
    basis.render_pipeline = engine::kRenderPipelineVersion;
    basis.develop_pipeline = engine::kDevelopPipelineVersion;
    basis.dng_opcode_processing = engine::kDngOpcodeProcessingVersion;
    basis.geometry_operation = engine::kGeometryOperationVersion;
    basis.image_area_placement = engine::kImageAreaPlacementVersion;
    basis.develop_engine.assign(engine::kDevelopEngineVersions.begin(), engine::kDevelopEngineVersions.end());
    basis.neutral_look = engine::kNeutralLookVersion;
    basis.export_version = exporter::kExportVersion;
    return basis;
}

std::vector<std::string> basis_differences(const QualificationBasis& qualified,
                                           const QualificationBasis& running) {
    std::vector<std::string> out;
    const auto text = [&](std::string_view what, const std::string& a, const std::string& b) {
        if (a != b) {
            out.push_back(std::format("{}: qualified with {}, this build uses {}", what, a, b));
        }
    };
    const auto number = [&](std::string_view what, std::uint32_t a, std::uint32_t b) {
        if (a != b) {
            out.push_back(
                std::format("{}: qualified with version {}, this build uses version {}", what, a, b));
        }
    };
    const auto list = [](const auto& values) {
        std::string joined;
        for (const auto& value : values) {
            joined += (joined.empty() ? "" : ", ") + std::format("{}", value);
        }
        return "[" + joined + "]";
    };
    text("decoder", qualified.decoder, running.decoder);
    if (qualified.decoder_capabilities != running.decoder_capabilities) {
        out.push_back(std::format("decoder capabilities: qualified with {}, this build has {}",
                                  list(qualified.decoder_capabilities), list(running.decoder_capabilities)));
    }
    text("decoded-RAW contract", qualified.decoded_raw_contract, running.decoded_raw_contract);
    number("mode key", qualified.mode_key, running.mode_key);
    number("render pipeline", qualified.render_pipeline, running.render_pipeline);
    number("develop pipeline", qualified.develop_pipeline, running.develop_pipeline);
    number("DNG opcode processing", qualified.dng_opcode_processing, running.dng_opcode_processing);
    number("geometry operation", qualified.geometry_operation, running.geometry_operation);
    number("image area placement", qualified.image_area_placement, running.image_area_placement);
    if (qualified.develop_engine != running.develop_engine) {
        out.push_back(std::format(
            "develop engine (stage graph, sensor normalisation, white balance, demosaicing, camera colour, "
            "working space, guided filter, proxy downscale, DNG opcodes, image area): qualified with versions "
            "{}, this build uses {}",
            list(qualified.develop_engine), list(running.develop_engine)));
    }
    number("neutral look", qualified.neutral_look, running.neutral_look);
    number("export", qualified.export_version, running.export_version);
    return out;
}

// ---- Limitation wording -------------------------------------------------------------------------

std::optional<LimitationText> standard_limitation_text(std::string_view code) {
    struct Words {
        std::string_view code;
        std::string_view summary;
        std::string_view meaning;
    };
    static constexpr Words kWords[] = {
        {kLimitationDefaultCrop, "The camera's image area is not applied",
         "Lucent cannot find where this camera's intended image area lies in the file, so the photo "
         "keeps the narrow border of extra pixels the sensor records around it. Crop the edges if they "
         "look different."},
        {kLimitationXTransBaseline, "X-Trans demosaicing is baseline quality",
         "Fujifilm X-Trans files are developed with Lucent's baseline demosaicing: fine detail can look "
         "softer, and fine patterns can show colour artefacts."},
        {kLimitationDualPixel, "Dual Pixel data is not processed",
         "Lucent develops the photograph; the camera's second Dual Pixel image stays in the file unused, "
         "so Dual Pixel adjustments are not available."},
        {kLimitationEnhancedImage, "The DNG's enhanced image is not used",
         "This DNG also holds an enhanced, already processed version of the photo. Lucent develops the "
         "original raw image instead."},
        {kLimitationOptionalOperations, "Optional processing is skipped",
         "The file asks for an optional processing step that Lucent does not have yet; the photo is "
         "developed without it."},
        {kLimitationNonSquarePixels, "Non-square pixels are not corrected",
         "This camera records pixels that are not square. Lucent does not resample them, so the photo's "
         "proportions can look slightly stretched."},
        {kLimitationPixelShift, "Pixel-shift frames are not combined",
         "Each frame of a pixel-shift capture is developed on its own; Lucent does not merge them into one "
         "higher-resolution image."},
    };
    for (const Words& words : kWords) {
        if (words.code == code) {
            return LimitationText{std::string(words.code), std::string(words.summary), std::string(words.meaning)};
        }
    }
    return std::nullopt;
}

// ---- Activation ---------------------------------------------------------------------------------

Expected<std::shared_ptr<const CameraSupport>> activate_camera_support(const VerifiedAsset& asset) {
    try {
        const AssetContent& content = asset.manifest.content;
        if (content.schema != kCameraSupportSchema || content.schema_version > kCameraSupportSchemaVersion) {
            return fail(
                ErrorCode::VersionMismatch,
                std::format("asset '{}' {} holds '{}' version {}; this build reads '{}' up to version {}",
                            asset.manifest.id, asset.manifest.version.to_string(), content.schema,
                            content.schema_version, kCameraSupportSchema, kCameraSupportSchemaVersion),
                std::string(kWhere));
        }
        const std::string* document = asset.file(kCameraSupportFileName);
        if (document == nullptr) {
            return fail(ErrorCode::NotFound,
                        std::format("asset '{}' {} does not list {}", asset.manifest.id,
                                    asset.manifest.version.to_string(), kCameraSupportFileName),
                        std::string(kWhere));
        }
        auto database = parse_camera_support(*document);
        if (!database) {
            return Unexpected(std::move(database.error()));
        }
        auto support = std::make_shared<CameraSupport>();
        support->asset_id = asset.manifest.id;
        support->version = asset.manifest.version;
        support->origin = asset.origin;
        support->folder = asset.folder;
        support->manifest_sha256 = asset.manifest_sha256;
        support->database = std::move(*database);
        support->basis_differences = basis_differences(support->database.basis, running_basis());
        return std::shared_ptr<const CameraSupport>(std::move(support));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "activating the camera support database ran out of memory",
                    std::string(kWhere));
    }
}

CameraSupportLoad load_camera_support(const AssetLocations& locations, const AssetRequest& request) {
    CameraSupportLoad load;
    AssetLookup lookup = find_asset(locations, request);
    load.candidates = std::move(lookup.candidates);
    if (!lookup.asset) {
        load.problem = std::move(lookup.problem);
        return load;
    }
    auto active = activate_camera_support(*lookup.asset);
    if (!active) {
        // The activated candidate is refused after all: record why on it.
        for (AssetCandidate& candidate : load.candidates) {
            if (candidate.present && !candidate.refused && candidate.folder == lookup.asset->folder) {
                candidate.refused = active.error();
            }
        }
        load.problem = std::move(active.error());
        return load;
    }
    load.active = std::move(*active);
    return load;
}

}  // namespace lucent::assets
