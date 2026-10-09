#pragma once

// Asset manifests (blueprint section 8, "Recipes, assets, and forward compatibility": "Version these
// contracts independently: ... model manifest, and camera/profile database"; ADR 0018).
//
// An asset is a folder of data files that Lucent reads at run time (the camera support database
// first; camera and lens profiles and models later) together with its manifest, "manifest.json":
//
//   {
//    "schema": "lucent.asset-manifest",
//    "schema_version": 1,
//    "id": "camera-support",
//    "version": "1.0.0",
//    "minimum_app_version": "0.1.0",
//    "content": {"schema": "lucent.camera-support", "schema_version": 1},
//    "files": [{"path": "camera-support.json", "size": 23154, "sha256": "<64 lower-case hex digits>"}]
//   }
//
//   id                   the asset's identity: words of [a-z0-9] joined by single '-', 1 to 64
//                        characters ("camera-support"). The asset's folder is named after it.
//   version              Semantic Versioning 2.0.0 version of the content. Any change of a file is
//                        a new version; a reader treats two versions as two different assets.
//   minimum_app_version  the oldest Lucent (core::build_info().version, SemVer) that may activate
//                        the asset: an asset that needs a newer reader is refused, never half-read.
//   content              what the files hold: the schema of the asset's main document and its
//                        version (the asset's consumer checks that it can read them).
//   files                every data file of the asset, with its exact size and SHA-256. Paths are
//                        relative to the asset folder, use '/', and contain neither "." nor ".."
//                        segments, drive letters, colons, backslashes nor control characters; no two
//                        paths are equal ignoring ASCII case (Windows file names). At least one file;
//                        the manifest does not list itself.
//
// Strictness: a manifest is either understood completely or refused. Unknown members, a newer
// schema_version (VersionMismatch) and any malformed value (Corrupt) are errors; nothing is
// guessed. write_asset_manifest() produces the canonical text (members in the order above, files
// sorted by path, one-space indentation, LF line endings, a final newline), so a manifest
// written twice from the same files is byte-identical.
//
// Thread-safety: pure functions over values.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/core/error.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/core/version.hpp>

namespace lucent::assets {

inline constexpr std::string_view kAssetManifestSchema = "lucent.asset-manifest";
inline constexpr std::uint32_t kAssetManifestSchemaVersion = 1;
inline constexpr std::string_view kAssetManifestFileName = "manifest.json";

// Largest manifest and largest asset file a reader accepts (LimitExceeded above).
inline constexpr std::uint64_t kMaxManifestBytes = std::uint64_t{1} << 20;    // 1 MiB
inline constexpr std::uint64_t kMaxAssetFileBytes = std::uint64_t{64} << 20;  // 64 MiB

struct AssetFile {
    std::string path;  // relative, '/'-separated (see the file comment)
    std::uint64_t size = 0;
    core::Digest256 sha256{};

    friend bool operator==(const AssetFile&, const AssetFile&) = default;
};

struct AssetContent {
    std::string schema;  // e.g. "lucent.camera-support"
    std::uint32_t schema_version = 0;

    friend bool operator==(const AssetContent&, const AssetContent&) = default;
};

struct AssetManifest {
    std::string id;
    core::SemanticVersion version;
    core::SemanticVersion minimum_app_version;
    AssetContent content;
    std::vector<AssetFile> files;  // sorted by path (parse_asset_manifest sorts; the writer sorts)

    // The file entry for `path` (exact spelling), or nullptr.
    [[nodiscard]] const AssetFile* find(std::string_view path) const noexcept;
};

// True for a valid asset id (see the file comment).
[[nodiscard]] bool valid_asset_id(std::string_view id) noexcept;

// True for a valid manifest file path (see the file comment).
[[nodiscard]] bool valid_asset_path(std::string_view path) noexcept;

// Parses and validates a manifest. Errors (where "assets.manifest"): VersionMismatch for another
// schema name or a schema_version newer than kAssetManifestSchemaVersion; Corrupt for anything
// malformed (not JSON, missing or unknown members, wrong types, an invalid id, version, path or
// digest, no files, duplicate paths).
[[nodiscard]] Expected<AssetManifest> parse_asset_manifest(std::string_view json);

// The canonical text of `manifest` (see the file comment). Its files are written sorted by path.
// InvalidArgument ("assets.manifest") when the manifest would not parse back: an invalid id or
// path, no files, duplicate paths, an empty content schema or a zero content schema version.
[[nodiscard]] Expected<std::string> write_asset_manifest(const AssetManifest& manifest);

// The manifest entry of a file with these bytes: its size and SHA-256. InvalidArgument for an
// invalid path; the hashing errors of core::sha256 otherwise.
[[nodiscard]] Expected<AssetFile> describe_asset_file(std::string path, std::span<const std::byte> bytes);
[[nodiscard]] Expected<AssetFile> describe_asset_file(std::string path, std::string_view bytes);

}  // namespace lucent::assets
