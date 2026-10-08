// The camera support database this build bundles (assets/camera-support, copied next to the test
// executable as Lucent.exe finds it next to itself): it must verify against the build's pin, parse,
// and hold for this build's decoder and pipelines. When a decoder, contract or pipeline version
// changes, the last test fails until the qualification is run again and the database re-exported
// (docs/qualification/README.md): a Verified state is only valid for the versions it was
// established with (ADR 0016, ADR 0018).

#include <string>

#include <gtest/gtest.h>

#include <lucent/assets/bundled_assets.hpp>
#include <lucent/assets/camera_support.hpp>

namespace lucent::assets {
namespace {

CameraSupportLoad load_bundled() {
    auto request = bundled_asset_request(kCameraSupportAssetId);
    EXPECT_TRUE(request.has_value());
    AssetLocations locations;
    locations.bundled = executable_directory() / L"assets";  // never a user data pack in tests
    return load_camera_support(locations, request ? *request : AssetRequest{});
}

TEST(BundledCameraSupport, TheBuildPinsTheCameraSupportManifest) {
    bool found = false;
    for (const BundledAsset& asset : bundled_assets()) {
        if (asset.id == kCameraSupportAssetId) {
            found = true;
            EXPECT_EQ(asset.manifest_sha256.size(), 64u);
        }
    }
    EXPECT_TRUE(found);
    EXPECT_TRUE(bundled_manifest_pin(kCameraSupportAssetId).has_value());
    EXPECT_FALSE(bundled_manifest_pin("no-such-asset").has_value());
}

TEST(BundledCameraSupport, VerifiesAndParses) {
    const CameraSupportLoad load = load_bundled();
    ASSERT_NE(load.active, nullptr) << (load.problem ? load.problem->describe() : std::string("no problem"));
    const CameraSupport& support = *load.active;
    EXPECT_EQ(support.asset_id, kCameraSupportAssetId);
    EXPECT_EQ(support.origin, AssetOrigin::Bundled);
    const CameraSupportDatabase& db = support.database;
    EXPECT_FALSE(db.entries.empty());
    EXPECT_EQ(db.summary, db.count());
    EXPECT_EQ(db.summary.modes, db.entries.size());
    // Every Limited entry names a limitation with words for the photographer.
    for (const CameraSupportEntry& entry : db.entries) {
        for (const EntryLimitation& limitation : entry.limitations) {
            const LimitationText* text = db.limitation(limitation.code);
            ASSERT_NE(text, nullptr) << limitation.code;
            EXPECT_FALSE(text->summary.empty());
            EXPECT_FALSE(text->meaning.empty());
        }
    }
}

TEST(BundledCameraSupport, HoldsForThisBuildsDecoderAndPipelines) {
    const CameraSupportLoad load = load_bundled();
    ASSERT_NE(load.active, nullptr);
    std::string differences;
    for (const std::string& line : load.active->basis_differences) {
        differences += "\n  " + line;
    }
    EXPECT_TRUE(load.active->verified_entries_hold())
        << "the bundled camera support database was qualified with other versions; run the qualification "
           "again and re-export it (docs/qualification/README.md):"
        << differences;
}

}  // namespace
}  // namespace lucent::assets
