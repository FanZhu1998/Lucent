#pragma once

// Finding, verifying and activating assets (blueprint section 8: "Keep resources separate";
// "Verify ... standalone profile/model packs against an authenticated release manifest or
// signature tied to the trusted publisher identity before activation; hashes alone do not
// authenticate a download"; section 10, "Update compatibility": "old missing assets are not
// silently replaced"; ADR 0018).
//
// Locations, in lookup order:
//   1. Per-user data packs: <per-user application data>\assets\<id> (core::AppDirectories::data,
//      by default %LOCALAPPDATA%\Lucent\data\assets). Reserved for future signed data packs (new
//      camera profiles for files the engine already understands, blueprint section 8). This
//      version cannot verify a publisher's signature, so a pack found there is REFUSED (Unsupported,
//      "assets.load.unsigned") and reported as a refused candidate; it never replaces the bundled
//      asset, and Lucent never writes this folder.
//   2. The bundled asset: <folder of the running executable>\assets\<id>, installed read-only with
//      Lucent (the installed application directory of section 8). Build trees mirror it in
//      <build>\bin\assets (src/assets/CMakeLists.txt).
//
// Verification before activation (verify_asset_folder), on bytes read once into memory, so what
// is verified is exactly what the consumer parses (no second read that could see another file):
//   * the manifest (at most kMaxManifestBytes) parses (parse_asset_manifest) and names the
//     requested id ("assets.load.id", Corrupt otherwise);
//   * for the bundled asset, when the request carries a pin (bundled_manifest_pin(): the SHA-256 of
//     the manifest the build was made with, compiled into the executable), the manifest's bytes
//     hash to the pin ("assets.load.pin", Corrupt otherwise). The executable is what the
//     installer and Authenticode signing protect, so the pin ties the bundled data to it: the
//     per-file hashes alone could be rewritten together with the manifest;
//   * the running Lucent is at least the manifest's minimum_app_version ("assets.load.app_version",
//     VersionMismatch otherwise);
//   * every listed file exists ("assets.load.missing", NotFound), is a regular file of at most
//     kMaxAssetFileBytes (LimitExceeded) and has exactly the listed size and SHA-256
//     ("assets.load.hash", Corrupt).
// Files in the folder that the manifest does not list are never read. A failure leaves nothing
// activated for that candidate; the lookup reports it instead of falling back to some other copy.
//
// Thread-safety: functions are reentrant; results are plain values.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/assets/asset_manifest.hpp>
#include <lucent/core/app_paths.hpp>
#include <lucent/core/error.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/core/version.hpp>

namespace lucent::assets {

// Where an asset came from (append only).
enum class AssetOrigin : std::uint8_t {
    Bundled = 1,       // installed with Lucent (read-only)
    UserDataPack = 2,  // the per-user data-pack folder (refused in this version)
};

// "bundled", "user_data_pack".
[[nodiscard]] std::string_view to_string(AssetOrigin origin) noexcept;

struct AssetLocations {
    // Parent of the bundled asset folders: <folder of the running executable>\assets.
    std::filesystem::path bundled;
    // Parent of the per-user data-pack folders: <per-user data>\assets; empty when unknown (the
    // per-user folders are unavailable), which skips that location.
    std::filesystem::path user;
};

// The folder of the running executable (GetModuleFileNameW). Empty when it cannot be determined.
[[nodiscard]] std::filesystem::path executable_directory();

// <executable_directory()>\assets and, when `directories` is given, <directories->data>\assets.
// Pure apart from the executable lookup: creates nothing.
[[nodiscard]] AssetLocations default_asset_locations(const core::AppDirectories* directories);

struct AssetRequest {
    std::string id;
    // The running Lucent, compared with the manifest's minimum_app_version.
    core::SemanticVersion app_version;
    // SHA-256 the bundled manifest must have (see the file comment); std::nullopt: not pinned
    // (tools and tests that verify an arbitrary folder).
    std::optional<core::Digest256> pinned_manifest;
};

// The request for asset `id` as this build makes it: the build's version (core::build_info())
// and the pin recorded for `id` at configure time (bundled_manifest_pin()). NotFound
// ("assets.load.unpinned") when the build bundles no such asset: an asset without a pin would be
// trusted on its own hashes (AST-09), so only tools and tests build unpinned requests themselves.
[[nodiscard]] Expected<AssetRequest> bundled_asset_request(std::string_view id);

// An activated asset: its manifest and the verified bytes of every listed file.
struct VerifiedAsset {
    AssetManifest manifest;
    AssetOrigin origin = AssetOrigin::Bundled;
    std::filesystem::path folder;
    core::Digest256 manifest_sha256{};
    std::map<std::string, std::string, std::less<>> files;  // manifest path -> bytes

    // The verified bytes of `path`, or nullptr when the manifest does not list it.
    [[nodiscard]] const std::string* file(std::string_view path) const noexcept;
};

// Verifies the asset in `folder` as described in the file comment and returns it with its bytes.
// `origin` is recorded; it does not change the checks (the pin applies whenever the request has
// one). Errors as listed in the file comment, plus the I/O errors of reading the files.
[[nodiscard]] Expected<VerifiedAsset> verify_asset_folder(const std::filesystem::path& folder,
                                                          AssetOrigin origin, const AssetRequest& request);

// One place the asset was looked for.
struct AssetCandidate {
    AssetOrigin origin = AssetOrigin::Bundled;
    std::filesystem::path folder;
    // Something is there: the folder exists. Absent candidates are listed too (diagnostics).
    bool present = false;
    // Why a present candidate was not activated; std::nullopt for the activated one.
    std::optional<Error> refused;
    // The version its manifest declares, when the manifest could be read.
    std::optional<core::SemanticVersion> version;
};

struct AssetLookup {
    std::optional<VerifiedAsset> asset;      // the activated asset, if any
    std::vector<AssetCandidate> candidates;  // every location, in lookup order
    // Why nothing was activated (asset empty): the bundled candidate's error, or NotFound
    // ("assets.load.missing") when no candidate is present.
    std::optional<Error> problem;
};

// Looks for asset `request.id` in lookup order (see the file comment) and activates the first
// candidate that is accepted: in this version that can only be the bundled one, since per-user
// data packs are refused ("assets.load.unsigned").
[[nodiscard]] AssetLookup find_asset(const AssetLocations& locations, const AssetRequest& request);

}  // namespace lucent::assets
