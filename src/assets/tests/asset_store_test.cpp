#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include <lucent/assets/asset_store.hpp>

#include "asset_test_support.hpp"

namespace lucent::assets {
namespace {

using test::AssetSpec;
using test::request_for;
using test::TempDir;
using test::write_asset;
using test::write_bytes;

TEST(AssetStore, VerifiesAGoodFolderAndReturnsItsBytes) {
    TempDir dir;
    AssetSpec spec;
    spec.files = {{"data.json", "{\"a\": 1}\n"}, {"more/extra.bin", std::string("\x00\x01\x02", 3)}};
    const std::string manifest = write_asset(dir.path(), spec);
    AssetRequest request = request_for("test-asset");
    request.pinned_manifest = test::digest_of(manifest);
    auto asset = verify_asset_folder(dir.path(), AssetOrigin::Bundled, request);
    ASSERT_TRUE(asset.has_value()) << asset.error().describe();
    EXPECT_EQ(asset->manifest.id, "test-asset");
    EXPECT_EQ(asset->manifest.version.to_string(), "1.2.3");
    EXPECT_EQ(asset->origin, AssetOrigin::Bundled);
    EXPECT_EQ(asset->manifest_sha256, test::digest_of(manifest));
    ASSERT_NE(asset->file("data.json"), nullptr);
    EXPECT_EQ(*asset->file("data.json"), "{\"a\": 1}\n");
    ASSERT_NE(asset->file("more/extra.bin"), nullptr);
    EXPECT_EQ(asset->file("more/extra.bin")->size(), 3u);
    EXPECT_EQ(asset->file("manifest.json"), nullptr);
}

TEST(AssetStore, RefusesATamperedFile) {
    TempDir dir;
    write_asset(dir.path(), AssetSpec{});
    write_bytes(dir.path() / "data.json", "{\"hello\": 2}\n");  // same size, other bytes
    auto asset = verify_asset_folder(dir.path(), AssetOrigin::Bundled, request_for("test-asset"));
    ASSERT_FALSE(asset.has_value());
    EXPECT_EQ(asset.error().code, ErrorCode::Corrupt);
    EXPECT_EQ(asset.error().where, "assets.load.hash");
    EXPECT_NE(asset.error().message.find("data.json"), std::string::npos);

    write_bytes(dir.path() / "data.json", "{\"hello\": 1}\n\n");  // other size
    asset = verify_asset_folder(dir.path(), AssetOrigin::Bundled, request_for("test-asset"));
    ASSERT_FALSE(asset.has_value());
    EXPECT_EQ(asset.error().where, "assets.load.hash");
}

TEST(AssetStore, RefusesAManifestThatIsNotTheBuildsPin) {
    TempDir dir;
    const std::string manifest = write_asset(dir.path(), AssetSpec{});
    AssetRequest request = request_for("test-asset");
    request.pinned_manifest = test::digest_of(manifest + " ");
    auto asset = verify_asset_folder(dir.path(), AssetOrigin::Bundled, request);
    ASSERT_FALSE(asset.has_value());
    EXPECT_EQ(asset.error().code, ErrorCode::Corrupt);
    EXPECT_EQ(asset.error().where, "assets.load.pin");

    // A consistent rewrite of a data file and its manifest is still refused: the pin is the anchor.
    AssetSpec rewritten;
    rewritten.files = {{"data.json", "{\"hello\": 3}\n"}};
    write_asset(dir.path(), rewritten);
    request.pinned_manifest = test::digest_of(manifest);
    asset = verify_asset_folder(dir.path(), AssetOrigin::Bundled, request);
    ASSERT_FALSE(asset.has_value());
    EXPECT_EQ(asset.error().where, "assets.load.pin");
}

TEST(AssetStore, RefusesAnAssetThatNeedsANewerLucent) {
    TempDir dir;
    AssetSpec spec;
    spec.minimum_app_version = "0.2.0";
    write_asset(dir.path(), spec);
    auto asset = verify_asset_folder(dir.path(), AssetOrigin::Bundled, request_for("test-asset", "0.1.0"));
    ASSERT_FALSE(asset.has_value());
    EXPECT_EQ(asset.error().code, ErrorCode::VersionMismatch);
    EXPECT_EQ(asset.error().where, "assets.load.app_version");
    EXPECT_NE(asset.error().message.find("0.2.0"), std::string::npos);

    EXPECT_TRUE(verify_asset_folder(dir.path(), AssetOrigin::Bundled, request_for("test-asset", "0.2.0")));
    EXPECT_TRUE(verify_asset_folder(dir.path(), AssetOrigin::Bundled, request_for("test-asset", "1.0.0")));
    // A pre-release of the minimum version precedes it (SemVer).
    EXPECT_FALSE(
        verify_asset_folder(dir.path(), AssetOrigin::Bundled, request_for("test-asset", "0.2.0-rc.1")));
}

TEST(AssetStore, RefusesMissingFilesAndManifests) {
    TempDir dir;
    write_asset(dir.path(), AssetSpec{});
    std::filesystem::remove(dir.path() / "data.json");
    auto asset = verify_asset_folder(dir.path(), AssetOrigin::Bundled, request_for("test-asset"));
    ASSERT_FALSE(asset.has_value());
    EXPECT_EQ(asset.error().code, ErrorCode::NotFound);
    EXPECT_EQ(asset.error().where, "assets.load.missing");

    std::filesystem::remove(dir.path() / "manifest.json");
    asset = verify_asset_folder(dir.path(), AssetOrigin::Bundled, request_for("test-asset"));
    ASSERT_FALSE(asset.has_value());
    EXPECT_EQ(asset.error().code, ErrorCode::NotFound);
    EXPECT_EQ(asset.error().where, "assets.load.missing");
}

TEST(AssetStore, RefusesAnotherAssetsManifestAndUnreadableManifests) {
    TempDir dir;
    write_asset(dir.path(), AssetSpec{});
    auto asset = verify_asset_folder(dir.path(), AssetOrigin::Bundled, request_for("camera-support"));
    ASSERT_FALSE(asset.has_value());
    EXPECT_EQ(asset.error().code, ErrorCode::Corrupt);
    EXPECT_EQ(asset.error().where, "assets.load.id");

    write_bytes(dir.path() / "manifest.json",
                "{\"schema\": \"lucent.asset-manifest\", \"schema_version\": 9}");
    asset = verify_asset_folder(dir.path(), AssetOrigin::Bundled, request_for("test-asset"));
    ASSERT_FALSE(asset.has_value());
    EXPECT_EQ(asset.error().code, ErrorCode::VersionMismatch);
}

TEST(AssetStore, NeverReadsFilesTheManifestDoesNotList) {
    TempDir dir;
    write_asset(dir.path(), AssetSpec{});
    write_bytes(dir.path() / "unlisted.json", "anything");
    auto asset = verify_asset_folder(dir.path(), AssetOrigin::Bundled, request_for("test-asset"));
    ASSERT_TRUE(asset.has_value()) << asset.error().describe();
    EXPECT_EQ(asset->file("unlisted.json"), nullptr);
    EXPECT_EQ(asset->files.size(), 1u);
}

TEST(AssetStore, LookupRefusesUserDataPacksAndActivatesTheBundledAsset) {
    TempDir dir;
    AssetLocations locations;
    locations.bundled = dir.path() / "install" / "assets";
    locations.user = dir.path() / "data" / "assets";
    AssetSpec bundled;
    write_asset(locations.bundled / "test-asset", bundled);
    AssetSpec pack;
    pack.version = "9.0.0";  // a "newer" pack must not replace the bundled asset
    write_asset(locations.user / "test-asset", pack);

    const AssetLookup lookup = find_asset(locations, request_for("test-asset"));
    ASSERT_TRUE(lookup.asset.has_value());
    EXPECT_EQ(lookup.asset->origin, AssetOrigin::Bundled);
    EXPECT_EQ(lookup.asset->manifest.version.to_string(), "1.2.3");
    EXPECT_FALSE(lookup.problem.has_value());
    ASSERT_EQ(lookup.candidates.size(), 2u);
    const AssetCandidate& user = lookup.candidates[0];
    EXPECT_EQ(user.origin, AssetOrigin::UserDataPack);
    EXPECT_TRUE(user.present);
    ASSERT_TRUE(user.refused.has_value());
    EXPECT_EQ(user.refused->code, ErrorCode::Unsupported);
    EXPECT_EQ(user.refused->where, "assets.load.unsigned");
    ASSERT_TRUE(user.version.has_value());
    EXPECT_EQ(user.version->to_string(), "9.0.0");
    const AssetCandidate& installed = lookup.candidates[1];
    EXPECT_EQ(installed.origin, AssetOrigin::Bundled);
    EXPECT_TRUE(installed.present);
    EXPECT_FALSE(installed.refused.has_value());
}

TEST(AssetStore, LookupNeverFallsBackFromARefusedBundledAsset) {
    TempDir dir;
    AssetLocations locations;
    locations.bundled = dir.path() / "assets";
    locations.user = dir.path() / "user";
    write_asset(locations.bundled / "test-asset", AssetSpec{});
    write_bytes(locations.bundled / "test-asset" / "data.json", "tampered");
    write_asset(locations.user / "test-asset", AssetSpec{});  // a valid but unsigned copy

    const AssetLookup lookup = find_asset(locations, request_for("test-asset"));
    EXPECT_FALSE(lookup.asset.has_value());
    ASSERT_TRUE(lookup.problem.has_value());
    EXPECT_EQ(lookup.problem->where, "assets.load.hash");
    ASSERT_EQ(lookup.candidates.size(), 2u);
    EXPECT_TRUE(lookup.candidates[0].refused.has_value());
    EXPECT_TRUE(lookup.candidates[1].refused.has_value());
}

TEST(AssetStore, LookupReportsAMissingAsset) {
    TempDir dir;
    AssetLocations locations;
    locations.bundled = dir.path() / "assets";
    const AssetLookup lookup = find_asset(locations, request_for("test-asset"));
    EXPECT_FALSE(lookup.asset.has_value());
    ASSERT_TRUE(lookup.problem.has_value());
    EXPECT_EQ(lookup.problem->code, ErrorCode::NotFound);
    EXPECT_EQ(lookup.problem->where, "assets.load.missing");
    ASSERT_EQ(lookup.candidates.size(), 1u);  // no user location configured
    EXPECT_FALSE(lookup.candidates[0].present);

    const AssetLookup invalid = find_asset(locations, request_for("Not An Id"));
    ASSERT_TRUE(invalid.problem.has_value());
    EXPECT_EQ(invalid.problem->code, ErrorCode::InvalidArgument);
}

TEST(AssetStore, DefaultLocationsFollowTheBlueprint) {
    core::AppDirectories directories =
        core::app_directories_at(L"C:\\Users\\Someone\\AppData\\Local\\Lucent");
    const AssetLocations locations = default_asset_locations(&directories);
    EXPECT_EQ(locations.bundled, executable_directory() / L"assets");
    EXPECT_EQ(locations.user, directories.data / L"assets");
    EXPECT_TRUE(default_asset_locations(nullptr).user.empty());
    EXPECT_FALSE(executable_directory().empty());
}

TEST(AssetStore, BundledRequestsCarryTheBuildsVersionAndPin) {
    auto request = bundled_asset_request("camera-support");
    ASSERT_TRUE(request.has_value()) << request.error().describe();
    EXPECT_EQ(request->id, "camera-support");
    EXPECT_EQ(request->app_version.to_string(), std::string(core::build_info().version));
    EXPECT_TRUE(request->pinned_manifest.has_value());
    // AST-09: an id the build has no pin for is refused instead of being verified on hashes alone.
    auto unknown = bundled_asset_request("not-bundled");
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error().code, ErrorCode::NotFound);
    EXPECT_EQ(unknown.error().where, "assets.load.unpinned");
}

}  // namespace
}  // namespace lucent::assets
