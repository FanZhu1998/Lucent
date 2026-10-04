#include "batch_documents.hpp"

#include <charconv>
#include <cstdio>
#include <exception>
#include <format>
#include <new>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <lucent/core/hash.hpp>
#include <lucent/core/text.hpp>

namespace lucent::app {

namespace detail {
namespace {

using Json = nlohmann::json;  // std::map objects: dump() writes members sorted by name

constexpr std::string_view kWhere = "app.batch.document";
constexpr std::string_view kDestinationSchema = "lucent.batch-destination";
constexpr std::string_view kExportSchema = "lucent.batch-export";
constexpr std::string_view kPolicySchema = "lucent.batch-policy";
constexpr std::uint32_t kDocumentVersion = 1;

// Thrown inside the readers and turned into an Error at their boundary.
struct DocumentError {
    ErrorCode code;
    std::string message;
};

[[noreturn]] void corrupt(std::string message) { throw DocumentError{ErrorCode::Corrupt, std::move(message)}; }

const Json& member(const Json& object, const char* name) {
    const auto found = object.find(name);
    if (found == object.end()) {
        corrupt(std::format("member '{}' is missing", name));
    }
    return *found;
}

std::string text(const Json& object, const char* name) {
    const Json& value = member(object, name);
    if (!value.is_string()) {
        corrupt(std::format("member '{}' is not text", name));
    }
    return value.get<std::string>();
}

std::optional<std::string> optional_text(const Json& object, const char* name) {
    const Json& value = member(object, name);
    if (value.is_null()) {
        return std::nullopt;
    }
    if (!value.is_string()) {
        corrupt(std::format("member '{}' is not text", name));
    }
    return value.get<std::string>();
}

bool boolean(const Json& object, const char* name) {
    const Json& value = member(object, name);
    if (!value.is_boolean()) {
        corrupt(std::format("member '{}' is not a boolean", name));
    }
    return value.get<bool>();
}

std::uint64_t unsigned_number(const Json& object, const char* name) {
    const Json& value = member(object, name);
    if (!value.is_number_unsigned() && !(value.is_number_integer() && value.get<std::int64_t>() >= 0)) {
        corrupt(std::format("member '{}' is not an unsigned integer", name));
    }
    return value.get<std::uint64_t>();
}

double real_number(const Json& object, const char* name) {
    const Json& value = member(object, name);
    if (!value.is_number()) {
        corrupt(std::format("member '{}' is not a number", name));
    }
    return value.get<double>();
}

std::filesystem::path path_of(const std::string& utf8) {
    auto path = core::path_from_utf8(utf8);
    if (!path) {
        corrupt("a path is not valid UTF-8");
    }
    return std::move(*path);
}

std::string path_text(const std::filesystem::path& path) {
    auto utf8 = core::path_to_utf8(path);
    if (!utf8) {
        throw DocumentError{ErrorCode::InvalidArgument,
                            std::format("the path '{}' is not valid Unicode", core::path_to_utf8_lossy(path))};
    }
    return std::move(*utf8);
}

core::Digest256 digest(const Json& object, const char* name) {
    auto parsed = core::digest_from_hex(text(object, name));
    if (!parsed) {
        corrupt(std::format("member '{}' is not a SHA-256", name));
    }
    return *parsed;
}

void check_header(const Json& document, std::string_view schema) {
    if (!document.is_object()) {
        corrupt("not a JSON object");
    }
    if (text(document, "schema") != schema) {
        corrupt(std::format("not a '{}' document", schema));
    }
    const std::uint64_t version = unsigned_number(document, "version");
    if (version > kDocumentVersion) {
        throw DocumentError{ErrorCode::VersionMismatch,
                            std::format("'{}' version {} was written by a newer Lucent", schema, version)};
    }
    if (version != kDocumentVersion) {
        corrupt(std::format("'{}' version {} is not supported", schema, version));
    }
}

template <class F>
auto guarded(std::string_view what, F&& body) -> decltype(body()) {
    try {
        return body();
    } catch (const DocumentError& e) {
        return fail(e.code, std::format("{}: {}", what, e.message), std::string(kWhere));
    } catch (const Json::exception& e) {
        return fail(ErrorCode::Corrupt, std::format("{}: {}", what, e.what()), std::string(kWhere));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, std::format("{}: out of memory", what), std::string(kWhere));
    }
}

Json parse(std::string_view json) {
    Json document = Json::parse(json.begin(), json.end(), nullptr, true, false);
    return document;
}

std::string hex_bytes(const std::uint8_t* data, std::size_t size) {
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out += std::format("{:02x}", data[i]);
    }
    return out;
}

bool parse_hex_bytes(std::string_view hex, std::uint8_t* out, std::size_t size) {
    if (hex.size() != size * 2) {
        return false;
    }
    for (std::size_t i = 0; i < size; ++i) {
        unsigned value = 0;
        const auto [ptr, ec] = std::from_chars(hex.data() + 2 * i, hex.data() + 2 * i + 2, value, 16);
        if (ec != std::errc{} || ptr != hex.data() + 2 * i + 2) {
            return false;
        }
        out[i] = static_cast<std::uint8_t>(value);
    }
    return true;
}

core::StorageKind storage_from_text(std::string_view text) {
    if (text == "fixed") {
        return core::StorageKind::Fixed;
    }
    if (text == "removable") {
        return core::StorageKind::Removable;
    }
    if (text == "network") {
        return core::StorageKind::Network;
    }
    return core::StorageKind::Unknown;
}

Json settings_json(const exporter::ExportSettings& settings) {
    Json out = Json::object();
    out["format"] = static_cast<unsigned>(settings.format);
    out["profile"] = static_cast<unsigned>(settings.profile);
    out["intent"] = static_cast<unsigned>(settings.transform.intent);
    out["black_point_compensation"] = settings.transform.black_point_compensation;
    out["jpeg_quality"] = settings.jpeg.quality;
    out["tiff_compression"] = static_cast<unsigned>(settings.tiff.compression);
    Json metadata = Json::object();
    metadata["capture"] = settings.metadata.include_capture;
    metadata["copyright"] = settings.metadata.include_copyright;
    metadata["gps"] = settings.metadata.include_gps;
    metadata["software"] = settings.metadata.include_software;
    out["metadata"] = std::move(metadata);
    return out;
}

exporter::ExportSettings settings_from_json(const Json& in) {
    if (!in.is_object()) {
        corrupt("the export settings are not an object");
    }
    exporter::ExportSettings settings;
    settings.format = static_cast<exporter::ExportFormat>(unsigned_number(in, "format"));
    settings.profile = static_cast<exporter::OutputProfile>(unsigned_number(in, "profile"));
    settings.transform.intent = static_cast<exporter::RenderingIntent>(unsigned_number(in, "intent"));
    settings.transform.black_point_compensation = boolean(in, "black_point_compensation");
    settings.jpeg.quality = static_cast<std::int32_t>(unsigned_number(in, "jpeg_quality"));
    settings.tiff.compression = static_cast<exporter::TiffCompression>(unsigned_number(in, "tiff_compression"));
    const Json& metadata = member(in, "metadata");
    settings.metadata.include_capture = boolean(metadata, "capture");
    settings.metadata.include_copyright = boolean(metadata, "copyright");
    settings.metadata.include_gps = boolean(metadata, "gps");
    settings.metadata.include_software = boolean(metadata, "software");
    if (!settings.validate()) {
        corrupt("the export settings are invalid");
    }
    return settings;
}

Json string_array(const std::vector<std::string>& values) {
    Json out = Json::array();
    for (const std::string& value : values) {
        out.push_back(core::sanitize_utf8(value));
    }
    return out;
}

std::vector<std::string> strings_from(const Json& object, const char* name) {
    const Json& value = member(object, name);
    if (!value.is_array()) {
        corrupt(std::format("member '{}' is not an array", name));
    }
    std::vector<std::string> out;
    for (const Json& entry : value) {
        if (!entry.is_string()) {
            corrupt(std::format("member '{}' holds a value that is not text", name));
        }
        out.push_back(entry.get<std::string>());
    }
    return out;
}

}  // namespace

std::string file_object_text(const core::FileIdentity& identity) {
    return std::format("{:016x}:{}", identity.volume_serial, hex_bytes(identity.file_id.data(), identity.file_id.size()));
}

Expected<std::string> write_destination(const DestinationDocument& document) {
    return guarded("destination document", [&]() -> Expected<std::string> {
        Json out = Json::object();
        out["schema"] = kDestinationSchema;
        out["version"] = kDocumentVersion;
        out["folder"] = path_text(document.folder);
        out["folder_object"] = document.folder_identity ? Json(file_object_text(*document.folder_identity)) : Json();
        out["name_pattern"] = document.name_pattern;
        out["mirror_subfolders"] = document.mirror_subfolders;
        out["mirror_root"] = document.mirror_root.empty() ? Json() : Json(path_text(document.mirror_root));
        out["collisions"] = std::string(to_string(document.collisions));
        out["temp_tag"] = core::wide_to_utf8_lossy(document.temp_tag);
        out["write_report_files"] = document.write_report_files;
        out["storage"] = std::string(core::to_string(document.storage));
        return out.dump();
    });
}

Expected<DestinationDocument> read_destination(std::string_view json) {
    return guarded("destination document", [&]() -> Expected<DestinationDocument> {
        const Json in = parse(json);
        check_header(in, kDestinationSchema);
        DestinationDocument document;
        document.folder = path_of(text(in, "folder"));
        if (auto object = optional_text(in, "folder_object")) {
            const std::size_t colon = object->find(':');
            core::FileIdentity identity;
            if (colon != 16 || !parse_hex_bytes(std::string_view(*object).substr(colon + 1), identity.file_id.data(),
                                                identity.file_id.size())) {
                corrupt("member 'folder_object' is malformed");
            }
            std::uint64_t serial = 0;
            const auto [ptr, ec] = std::from_chars(object->data(), object->data() + 16, serial, 16);
            if (ec != std::errc{} || ptr != object->data() + 16) {
                corrupt("member 'folder_object' is malformed");
            }
            identity.volume_serial = serial;
            document.folder_identity = identity;
        }
        document.name_pattern = text(in, "name_pattern");
        document.mirror_subfolders = boolean(in, "mirror_subfolders");
        if (auto root = optional_text(in, "mirror_root")) {
            document.mirror_root = path_of(*root);
        }
        auto collisions = collision_policy_from_string(text(in, "collisions"));
        if (!collisions) {
            corrupt("member 'collisions' is unknown");
        }
        document.collisions = *collisions;
        const std::string tag = text(in, "temp_tag");
        if (tag.size() != 8 || tag.find_first_not_of("0123456789abcdef") != std::string::npos) {
            corrupt("member 'temp_tag' is malformed");
        }
        document.temp_tag = core::utf8_to_wide_lossy(tag);
        document.write_report_files = boolean(in, "write_report_files");
        document.storage = storage_from_text(text(in, "storage"));
        return document;
    });
}

Expected<std::string> write_export_document(const ExportDocument& document) {
    return guarded("export document", [&]() -> Expected<std::string> {
        Json out = Json::object();
        out["schema"] = kExportSchema;
        out["version"] = kDocumentVersion;
        out["settings"] = settings_json(document.settings);
        out["artist"] = document.artist ? Json(core::sanitize_utf8(*document.artist)) : Json();
        out["copyright"] = document.copyright ? Json(core::sanitize_utf8(*document.copyright)) : Json();
        out["allow_unsupported"] = document.allow_unsupported;
        return out.dump();
    });
}

Expected<ExportDocument> read_export_document(std::string_view json) {
    return guarded("export document", [&]() -> Expected<ExportDocument> {
        const Json in = parse(json);
        check_header(in, kExportSchema);
        ExportDocument document;
        document.settings = settings_from_json(member(in, "settings"));
        document.artist = optional_text(in, "artist");
        document.copyright = optional_text(in, "copyright");
        document.allow_unsupported = boolean(in, "allow_unsupported");
        return document;
    });
}

Expected<std::string> write_policy(const PolicyDocument& document) {
    return guarded("policy document", [&]() -> Expected<std::string> {
        Json out = Json::object();
        out["schema"] = kPolicySchema;
        out["version"] = kDocumentVersion;
        out["existing_outputs"] = document.existing_outputs == ExistingOutputs::Skip ? "skip" : "export_again";
        Json retry = Json::object();
        retry["max_transient_failures"] = document.retry.max_transient_failures;
        retry["initial_backoff_ms"] = static_cast<std::uint64_t>(document.retry.initial_backoff.count());
        retry["max_backoff_ms"] = static_cast<std::uint64_t>(document.retry.max_backoff.count());
        out["retry"] = std::move(retry);
        Json excluded = Json::array();
        for (const PlanExclusion& exclusion : document.excluded) {
            Json entry = Json::object();
            entry["item"] = exclusion.item.value;
            entry["source"] = core::path_to_utf8_lossy(exclusion.source);
            entry["code"] = static_cast<unsigned>(exclusion.reason.code);
            entry["where"] = core::sanitize_utf8(exclusion.reason.where);
            entry["message"] = core::sanitize_utf8(exclusion.reason.message);
            excluded.push_back(std::move(entry));
        }
        out["excluded"] = std::move(excluded);
        out["excluded_count"] = document.excluded_count;
        return out.dump();
    });
}

Expected<PolicyDocument> read_policy(std::string_view json) {
    return guarded("policy document", [&]() -> Expected<PolicyDocument> {
        const Json in = parse(json);
        check_header(in, kPolicySchema);
        PolicyDocument document;
        const std::string existing = text(in, "existing_outputs");
        if (existing == "skip") {
            document.existing_outputs = ExistingOutputs::Skip;
        } else if (existing == "export_again") {
            document.existing_outputs = ExistingOutputs::ExportAgain;
        } else {
            corrupt("member 'existing_outputs' is unknown");
        }
        const Json& retry = member(in, "retry");
        document.retry.max_transient_failures = static_cast<std::uint32_t>(unsigned_number(retry, "max_transient_failures"));
        document.retry.initial_backoff = std::chrono::milliseconds(unsigned_number(retry, "initial_backoff_ms"));
        document.retry.max_backoff = std::chrono::milliseconds(unsigned_number(retry, "max_backoff_ms"));
        const Json& excluded = member(in, "excluded");
        if (!excluded.is_array()) {
            corrupt("member 'excluded' is not an array");
        }
        for (const Json& entry : excluded) {
            PlanExclusion exclusion;
            exclusion.item = ItemId{unsigned_number(entry, "item")};
            exclusion.source = path_of(text(entry, "source"));
            exclusion.reason.code = static_cast<ErrorCode>(unsigned_number(entry, "code"));
            exclusion.reason.where = text(entry, "where");
            exclusion.reason.message = text(entry, "message");
            document.excluded.push_back(std::move(exclusion));
        }
        document.excluded_count = unsigned_number(in, "excluded_count");
        return document;
    });
}

}  // namespace detail

// ---- Receipt details (public: batch_service.hpp) ---------------------------------------------

namespace {
constexpr std::string_view kReceiptSchema = "lucent.batch-receipt";
}

Expected<std::string> write_receipt_details(const ReceiptDetails& details) {
    using detail::Json;
    return detail::guarded("receipt details", [&]() -> Expected<std::string> {
        Json out = Json::object();
        out["schema"] = kReceiptSchema;
        out["version"] = detail::kDocumentVersion;
        out["revision"] = details.revision ? Json(details.revision->value) : Json();
        out["revision_number"] = details.revision_number;
        out["recipe_sha256"] = core::to_hex(details.recipe_sha256);
        out["export_settings_sha256"] = core::to_hex(details.export_settings_sha256);
        out["settings"] = detail::settings_json(details.settings);
        Json output = Json::object();
        output["format"] = static_cast<unsigned>(details.format);
        output["profile"] = static_cast<unsigned>(details.profile);
        output["bit_depth"] = static_cast<unsigned>(details.bit_depth);
        output["width"] = details.width;
        output["height"] = details.height;
        output["icc_sha256"] = core::to_hex(details.icc_sha256);
        output["chroma_subsampling"] =
            details.chroma_subsampling ? Json(static_cast<unsigned>(*details.chroma_subsampling)) : Json();
        output["encoder"] = core::sanitize_utf8(details.encoder);
        output["metadata_notes"] = detail::string_array(details.metadata_notes);
        out["output"] = std::move(output);
        Json render = Json::object();
        render["interpretation"] = static_cast<unsigned>(details.interpretation);
        render["warnings"] = detail::string_array(details.warnings);
        render["demosaic"] = core::sanitize_utf8(details.demosaic);
        render["camera_color"] = core::sanitize_utf8(details.camera_color);
        render["pipeline_version"] = details.pipeline_version;
        render["look"] = core::sanitize_utf8(details.look);
        render["look_version"] = details.look_version;
        render["clipped_raw_samples"] = details.clipped_raw_samples;
        render["limited_display_pixels"] = details.limited_display_pixels;
        render["estimated_peak_bytes"] = details.estimated_peak_bytes;
        render["worker_peak_commit_bytes"] = details.worker_peak_commit_bytes;
        out["render"] = std::move(render);
        Json timings = Json::object();
        timings["prepare"] = details.prepare_seconds;
        timings["render"] = details.render_seconds;
        timings["encode"] = details.encode_seconds;
        timings["write"] = details.write_seconds;
        timings["verify"] = details.verify_seconds;
        timings["total"] = details.total_seconds;
        out["seconds"] = std::move(timings);
        out["attempt"] = details.attempt;
        out["name_index"] = details.name_index;
        out["replaced_existing"] = details.replaced_existing;
        out["allow_unsupported"] = details.allow_unsupported;
        out["review"] = detail::string_array(details.review);
        return out.dump();
    });
}

Expected<ReceiptDetails> read_receipt_details(std::string_view json) {
    using detail::Json;
    auto result = detail::guarded("receipt details", [&]() -> Expected<ReceiptDetails> {
        const Json in = detail::parse(json);
        detail::check_header(in, kReceiptSchema);
        ReceiptDetails details;
        const Json& revision = detail::member(in, "revision");
        if (!revision.is_null()) {
            details.revision = catalog::RevisionId{static_cast<std::int64_t>(detail::unsigned_number(in, "revision"))};
        }
        details.revision_number = static_cast<std::uint32_t>(detail::unsigned_number(in, "revision_number"));
        details.recipe_sha256 = detail::digest(in, "recipe_sha256");
        details.export_settings_sha256 = detail::digest(in, "export_settings_sha256");
        details.settings = detail::settings_from_json(detail::member(in, "settings"));
        const Json& output = detail::member(in, "output");
        details.format = static_cast<exporter::ExportFormat>(detail::unsigned_number(output, "format"));
        details.profile = static_cast<exporter::OutputProfile>(detail::unsigned_number(output, "profile"));
        details.bit_depth = static_cast<exporter::BitDepth>(detail::unsigned_number(output, "bit_depth"));
        details.width = static_cast<std::uint32_t>(detail::unsigned_number(output, "width"));
        details.height = static_cast<std::uint32_t>(detail::unsigned_number(output, "height"));
        details.icc_sha256 = detail::digest(output, "icc_sha256");
        if (!detail::member(output, "chroma_subsampling").is_null()) {
            details.chroma_subsampling =
                static_cast<exporter::ChromaSubsampling>(detail::unsigned_number(output, "chroma_subsampling"));
        }
        details.encoder = detail::text(output, "encoder");
        details.metadata_notes = detail::strings_from(output, "metadata_notes");
        const Json& render = detail::member(in, "render");
        details.interpretation = static_cast<raw::InterpretationStatus>(detail::unsigned_number(render, "interpretation"));
        details.warnings = detail::strings_from(render, "warnings");
        details.demosaic = detail::text(render, "demosaic");
        details.camera_color = detail::text(render, "camera_color");
        details.pipeline_version = static_cast<std::uint32_t>(detail::unsigned_number(render, "pipeline_version"));
        details.look = detail::text(render, "look");
        details.look_version = static_cast<std::uint32_t>(detail::unsigned_number(render, "look_version"));
        details.clipped_raw_samples = detail::unsigned_number(render, "clipped_raw_samples");
        details.limited_display_pixels = detail::unsigned_number(render, "limited_display_pixels");
        details.estimated_peak_bytes = detail::unsigned_number(render, "estimated_peak_bytes");
        details.worker_peak_commit_bytes = detail::unsigned_number(render, "worker_peak_commit_bytes");
        const Json& timings = detail::member(in, "seconds");
        details.prepare_seconds = detail::real_number(timings, "prepare");
        details.render_seconds = detail::real_number(timings, "render");
        details.encode_seconds = detail::real_number(timings, "encode");
        details.write_seconds = detail::real_number(timings, "write");
        details.verify_seconds = detail::real_number(timings, "verify");
        details.total_seconds = detail::real_number(timings, "total");
        details.attempt = static_cast<std::uint32_t>(detail::unsigned_number(in, "attempt"));
        details.name_index = static_cast<std::uint32_t>(detail::unsigned_number(in, "name_index"));
        details.replaced_existing = detail::boolean(in, "replaced_existing");
        details.allow_unsupported = detail::boolean(in, "allow_unsupported");
        details.review = detail::strings_from(in, "review");
        return details;
    });
    if (!result && result.error().where == "app.batch.document") {
        result.error().where = "app.batch.receipt";
    }
    return result;
}

}  // namespace lucent::app
