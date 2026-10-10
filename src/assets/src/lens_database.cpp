// The bundled lens database (lens_database.hpp).

#include <format>
#include <string>
#include <utility>
#include <vector>

#include <lucent/assets/bundled_assets.hpp>
#include <lucent/assets/lens_database.hpp>
#include <lucent/core/hash.hpp>

namespace lucent::assets {

Expected<std::shared_ptr<const engine::LensDatabase>> lens_database_from(const VerifiedAsset& asset) {
    const AssetManifest& m = asset.manifest;
    if (m.content.schema != kLensDatabaseContentSchema ||
        m.content.schema_version != kLensDatabaseContentSchemaVersion) {
        return fail(ErrorCode::VersionMismatch,
                    std::format("the asset '{}' holds {} version {}; this build reads {} version {}", m.id,
                                m.content.schema, m.content.schema_version, kLensDatabaseContentSchema,
                                kLensDatabaseContentSchemaVersion),
                    "assets.lens.content");
    }
    std::vector<engine::LensDatabaseFile> files;
    for (const AssetFile& file : m.files) {  // sorted by path: a fixed load order
        if (file.path.size() < 4 || file.path.compare(file.path.size() - 4, 4, ".xml") != 0) {
            continue;  // the licence text and the provenance record
        }
        const std::string* bytes = asset.file(file.path);
        if (bytes == nullptr) {
            return fail(ErrorCode::Internal, std::format("the verified asset has no bytes for {}", file.path),
                        "assets.lens.files");
        }
        files.push_back(engine::LensDatabaseFile{file.path, *bytes});
    }
    if (files.empty()) {
        return fail(ErrorCode::Corrupt, std::format("the asset '{}' lists no XML file", m.id),
                    "assets.lens.files");
    }
    return engine::LensDatabase::load(
        files,
        engine::LensDatabaseIdentity{m.id, m.version.to_string(), core::to_hex(asset.manifest_sha256)});
}

LensDatabaseLookup load_lens_database(const AssetLocations& locations) {
    LensDatabaseLookup out;
    auto request = bundled_asset_request(kLensDatabaseAssetId);
    if (!request) {
        out.problem = std::move(request.error());
        return out;
    }
    out.lookup = find_asset(locations, *request);
    if (!out.lookup.asset) {
        out.problem = out.lookup.problem;
        return out;
    }
    auto database = lens_database_from(*out.lookup.asset);
    if (!database) {
        out.problem = std::move(database.error());
    } else {
        out.database = std::move(*database);
    }
    out.lookup.asset->files.clear();  // parsed (or refused): the manifest stays for diagnostics
    return out;
}

}  // namespace lucent::assets
