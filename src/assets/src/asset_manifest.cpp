#include <algorithm>
#include <cctype>
#include <format>
#include <utility>

#include <lucent/assets/asset_manifest.hpp>

#include "json_reader.hpp"

namespace lucent::assets {

namespace {

constexpr std::string_view kWhere = "assets.manifest";

char lower_ascii(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equal_ignoring_case(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower_ascii(a[i]) != lower_ascii(b[i])) {
            return false;
        }
    }
    return true;
}

// Sorted by path; InvalidArgument-style message for the first problem, empty when valid.
std::string files_problem(const std::vector<AssetFile>& files) {
    if (files.empty()) {
        return "lists no files";
    }
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (!valid_asset_path(files[i].path)) {
            return std::format("has an invalid file path '{}'", files[i].path);
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (equal_ignoring_case(files[i].path, files[j].path)) {
                return std::format("lists '{}' twice", files[i].path);
            }
        }
    }
    return {};
}

void sort_files(std::vector<AssetFile>& files) {
    std::sort(files.begin(), files.end(),
              [](const AssetFile& a, const AssetFile& b) { return a.path < b.path; });
}

}  // namespace

const AssetFile* AssetManifest::find(std::string_view path) const noexcept {
    for (const AssetFile& file : files) {
        if (file.path == path) {
            return &file;
        }
    }
    return nullptr;
}

bool valid_asset_id(std::string_view id) noexcept {
    if (id.empty() || id.size() > 64 || id.front() == '-' || id.back() == '-') {
        return false;
    }
    char previous = '\0';
    for (const char c : id) {
        const bool word = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!word && !(c == '-' && previous != '-')) {
            return false;
        }
        previous = c;
    }
    return true;
}

bool valid_asset_path(std::string_view path) noexcept {
    if (path.empty() || path.size() > 200 || path.front() == '/' || path.back() == '/') {
        return false;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = std::min(path.find('/', start), path.size());
        const std::string_view segment = path.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == ".." || segment.back() == '.' ||
            segment.back() == ' ') {
            return false;
        }
        for (const char c : segment) {
            const auto u = static_cast<unsigned char>(c);
            if (u < 0x20 || u == 0x7F || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
                c == '<' || c == '>' || c == '|') {
                return false;
            }
        }
        start = end + 1;
    }
    return true;
}

Expected<AssetManifest> parse_asset_manifest(std::string_view json) {
    return detail::guarded(kWhere, "asset manifest", [&]() -> Expected<AssetManifest> {
        using detail::Json;
        const Json in = detail::parse(json);
        detail::require_object(in, "the manifest");
        const std::string schema = detail::text(in, "schema", "manifest");
        if (schema != kAssetManifestSchema) {
            throw detail::DocumentError{ErrorCode::VersionMismatch,
                                        std::format("schema '{}' is not '{}'", schema, kAssetManifestSchema)};
        }
        const std::uint64_t version = detail::unsigned_number(in, "schema_version", "manifest");
        if (version > kAssetManifestSchemaVersion) {
            throw detail::DocumentError{ErrorCode::VersionMismatch,
                                        std::format("schema_version {} is newer than this build reads ({})",
                                                    version, kAssetManifestSchemaVersion)};
        }
        if (version == 0) {
            detail::corrupt("schema_version 0 is not a version");
        }
        detail::only_members(
            in, {"schema", "schema_version", "id", "version", "minimum_app_version", "content", "files"},
            "manifest");
        AssetManifest manifest;
        manifest.id = detail::text(in, "id", "manifest");
        if (!valid_asset_id(manifest.id)) {
            detail::corrupt(std::format("id '{}' is not a valid asset id", manifest.id));
        }
        auto asset_version = core::SemanticVersion::parse(detail::text(in, "version", "manifest"));
        if (!asset_version) {
            detail::corrupt("version: " + asset_version.error().message);
        }
        manifest.version = std::move(*asset_version);
        auto minimum = core::SemanticVersion::parse(detail::text(in, "minimum_app_version", "manifest"));
        if (!minimum) {
            detail::corrupt("minimum_app_version: " + minimum.error().message);
        }
        manifest.minimum_app_version = std::move(*minimum);

        const Json& content = detail::member(in, "content", "manifest");
        detail::require_object(content, "manifest.content");
        detail::only_members(content, {"schema", "schema_version"}, "manifest.content");
        manifest.content.schema = detail::text(content, "schema", "manifest.content");
        manifest.content.schema_version = detail::small_number(content, "schema_version", "manifest.content");
        if (manifest.content.schema.empty() || manifest.content.schema_version == 0) {
            detail::corrupt("manifest.content names no schema and version");
        }

        for (const Json& entry : detail::array(in, "files", "manifest")) {
            detail::require_object(entry, "a manifest file entry");
            detail::only_members(entry, {"path", "size", "sha256"}, "a manifest file entry");
            AssetFile file;
            file.path = detail::text(entry, "path", "manifest file");
            file.size = detail::unsigned_number(entry, "size", "manifest file");
            const std::string hex = detail::text(entry, "sha256", "manifest file");
            auto digest = core::digest_from_hex(hex);
            if (!digest || hex != core::to_hex(*digest)) {
                detail::corrupt(std::format("the sha256 of '{}' is not 64 lower-case hex digits", file.path));
            }
            file.sha256 = *digest;
            manifest.files.push_back(std::move(file));
        }
        sort_files(manifest.files);
        if (const std::string problem = files_problem(manifest.files); !problem.empty()) {
            detail::corrupt("the manifest " + problem);
        }
        return manifest;
    });
}

Expected<std::string> write_asset_manifest(const AssetManifest& manifest) {
    if (!valid_asset_id(manifest.id)) {
        return fail(ErrorCode::InvalidArgument, std::format("'{}' is not a valid asset id", manifest.id),
                    std::string(kWhere));
    }
    if (manifest.content.schema.empty() || manifest.content.schema_version == 0) {
        return fail(ErrorCode::InvalidArgument, "the manifest's content names no schema and version",
                    std::string(kWhere));
    }
    std::vector<AssetFile> files = manifest.files;
    sort_files(files);
    if (const std::string problem = files_problem(files); !problem.empty()) {
        return fail(ErrorCode::InvalidArgument, "the manifest " + problem, std::string(kWhere));
    }
    return detail::guarded(kWhere, "asset manifest", [&]() -> Expected<std::string> {
        detail::OrderedJson out = detail::OrderedJson::object();
        out["schema"] = kAssetManifestSchema;
        out["schema_version"] = kAssetManifestSchemaVersion;
        out["id"] = manifest.id;
        out["version"] = manifest.version.to_string();
        out["minimum_app_version"] = manifest.minimum_app_version.to_string();
        detail::OrderedJson content = detail::OrderedJson::object();
        content["schema"] = manifest.content.schema;
        content["schema_version"] = manifest.content.schema_version;
        out["content"] = std::move(content);
        detail::OrderedJson list = detail::OrderedJson::array();
        for (const AssetFile& file : files) {
            detail::OrderedJson entry = detail::OrderedJson::object();
            entry["path"] = file.path;
            entry["size"] = file.size;
            entry["sha256"] = core::to_hex(file.sha256);
            list.push_back(std::move(entry));
        }
        out["files"] = std::move(list);
        return out.dump(1) + "\n";
    });
}

Expected<AssetFile> describe_asset_file(std::string path, std::span<const std::byte> bytes) {
    if (!valid_asset_path(path)) {
        return fail(ErrorCode::InvalidArgument, std::format("'{}' is not a valid asset file path", path),
                    std::string(kWhere));
    }
    auto digest = core::sha256(bytes);
    if (!digest) {
        return Unexpected(std::move(digest.error()));
    }
    return AssetFile{std::move(path), static_cast<std::uint64_t>(bytes.size()), *digest};
}

Expected<AssetFile> describe_asset_file(std::string path, std::string_view bytes) {
    return describe_asset_file(std::move(path), std::as_bytes(std::span(bytes.data(), bytes.size())));
}

}  // namespace lucent::assets
