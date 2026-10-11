// The bundled lens database (lens_database.hpp, ADR 0023): the asset verifies against the build's
// pin and loads with Lensfun; a tampered or unlisted file, another content schema and a per-user
// data pack are refused; the committed manifest is canonical and the provenance record and licence
// text are part of the verified asset.

#include <filesystem>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <lucent/assets/asset_manifest.hpp>
#include <lucent/assets/bundled_assets.hpp>
#include <lucent/assets/lens_database.hpp>
#include <lucent/core/hash.hpp>

#include "asset_test_support.hpp"

namespace lucent::assets {
namespace {

AssetLocations bundled_only() {
    AssetLocations locations;
    locations.bundled = executable_directory() / L"assets";  // never a user data pack in tests
    return locations;
}

std::filesystem::path source_folder() {
    return std::filesystem::path(LUCENT_SOURCE_DIR) / "assets" / "lens-database";
}

TEST(AssetsLensDatabase, TheBuildPinsTheLensDatabaseManifest) {
    const auto pin = bundled_manifest_pin(kLensDatabaseAssetId);
    ASSERT_TRUE(pin.has_value());
    const std::string manifest = test::read_bytes(source_folder() / "manifest.json");
    auto digest = core::sha256(std::as_bytes(std::span(manifest.data(), manifest.size())));
    ASSERT_TRUE(digest);
    EXPECT_EQ(*digest, *pin);
}

TEST(AssetsLensDatabase, VerifiesAndLoadsWithLensfun) {
    const LensDatabaseLookup load = load_lens_database(bundled_only());
    ASSERT_NE(load.database, nullptr)
        << (load.problem ? load.problem->describe() : std::string("no problem"));
    const engine::LensDatabaseInfo& info = load.database->info();
    EXPECT_EQ(info.identity.asset_id, kLensDatabaseAssetId);
    EXPECT_EQ(info.identity.asset_version, "1.0.0");
    EXPECT_EQ(info.identity.manifest_sha256, core::to_hex(*bundled_manifest_pin(kLensDatabaseAssetId)));
    EXPECT_EQ(info.data_format, 1u);
    EXPECT_EQ(info.library, "Lensfun 0.3.4");
    EXPECT_EQ(info.files, 56u);
    EXPECT_GT(info.cameras, 1000u);
    EXPECT_GT(info.lenses, 1500u);
    EXPECT_GT(info.mounts, 50u);
    ASSERT_TRUE(load.lookup.asset.has_value());
    EXPECT_EQ(load.lookup.asset->origin, AssetOrigin::Bundled);
    EXPECT_TRUE(load.lookup.asset->files.empty()) << "the parsed bytes are released";
    // A lens of the corpus, by the database's spelling.
    EXPECT_TRUE(load.database->contains(engine::LensProfileId{"Sony", "FE 50mm f/2.5 G", 1.0}));
}

TEST(AssetsLensDatabase, ATamperedFileIsRefused) {
    test::TempDir dir;
    const std::filesystem::path folder = dir.path() / "lens-database";
    std::filesystem::copy(executable_directory() / L"assets" / L"lens-database", folder,
                          std::filesystem::copy_options::recursive);
    // One changed byte in one XML file: its SHA-256 no longer matches the manifest.
    std::string xml = test::read_bytes(folder / "mil-sony.xml");
    ASSERT_GT(xml.size(), 1000u);
    xml[500] = xml[500] == 'a' ? 'b' : 'a';
    test::write_bytes(folder / "mil-sony.xml", xml);
    auto request = bundled_asset_request(kLensDatabaseAssetId);
    ASSERT_TRUE(request);
    auto verified = verify_asset_folder(folder, AssetOrigin::Bundled, *request);
    ASSERT_FALSE(verified);
    EXPECT_EQ(verified.error().code, ErrorCode::Corrupt);
    EXPECT_EQ(verified.error().where, "assets.load.hash");
    // A per-user data pack (even an intact copy) is never activated: no signatures yet.
    test::TempDir user;
    std::filesystem::copy(executable_directory() / L"assets" / L"lens-database",
                          user.path() / "lens-database", std::filesystem::copy_options::recursive);
    AssetLocations locations = bundled_only();
    locations.user = user.path();
    const LensDatabaseLookup load = load_lens_database(locations);
    ASSERT_NE(load.database, nullptr);
    EXPECT_EQ(load.lookup.asset->origin, AssetOrigin::Bundled);
    bool refused = false;
    for (const AssetCandidate& c : load.lookup.candidates) {
        if (c.origin == AssetOrigin::UserDataPack && c.refused &&
            c.refused->where == "assets.load.unsigned") {
            refused = true;
        }
    }
    EXPECT_TRUE(refused);
}

TEST(AssetsLensDatabase, AnotherContentSchemaOrNoXmlIsRefused) {
    test::TempDir dir;
    test::AssetSpec spec;
    spec.id = "lens-database";
    spec.content = AssetContent{"lensfun.database", 2};
    spec.files = {{"a.xml", "<lensdatabase version=\"2\"></lensdatabase>\n"}};
    test::write_asset(dir.path() / "lens-database", spec);
    AssetRequest request;
    request.id = "lens-database";
    request.app_version = test::version("0.1.0");
    auto verified = verify_asset_folder(dir.path() / "lens-database", AssetOrigin::Bundled, request);
    ASSERT_TRUE(verified) << verified.error().describe();
    auto db = lens_database_from(*verified);
    ASSERT_FALSE(db);
    EXPECT_EQ(db.error().code, ErrorCode::VersionMismatch);
    EXPECT_EQ(db.error().where, "assets.lens.content");

    test::TempDir dir2;
    spec.content = AssetContent{"lensfun.database", 1};
    spec.files = {{"README.txt", "no data\n"}};
    test::write_asset(dir2.path() / "lens-database", spec);
    auto verified2 = verify_asset_folder(dir2.path() / "lens-database", AssetOrigin::Bundled, request);
    ASSERT_TRUE(verified2);
    auto db2 = lens_database_from(*verified2);
    ASSERT_FALSE(db2);
    EXPECT_EQ(db2.error().where, "assets.lens.files");

    // XML that Lensfun 0.3 refuses: a newer data format.
    test::TempDir dir3;
    spec.files = {{"a.xml", "<lensdatabase version=\"2\">\n</lensdatabase>\n"}};
    test::write_asset(dir3.path() / "lens-database", spec);
    auto verified3 = verify_asset_folder(dir3.path() / "lens-database", AssetOrigin::Bundled, request);
    ASSERT_TRUE(verified3);
    auto db3 = lens_database_from(*verified3);
    ASSERT_FALSE(db3);
    EXPECT_EQ(db3.error().code, ErrorCode::Corrupt);
}

TEST(AssetsLensDatabase, TheCommittedManifestIsCanonical) {
    const std::string text = test::read_bytes(source_folder() / "manifest.json");
    auto manifest = parse_asset_manifest(text);
    ASSERT_TRUE(manifest) << manifest.error().describe();
    EXPECT_EQ(manifest->id, kLensDatabaseAssetId);
    EXPECT_EQ(manifest->content.schema, kLensDatabaseContentSchema);
    EXPECT_EQ(manifest->content.schema_version, kLensDatabaseContentSchemaVersion);
    auto written = write_asset_manifest(*manifest);
    ASSERT_TRUE(written);
    EXPECT_EQ(*written, text);
    // 56 XML files, the licence text and the provenance record; nothing else in the folder.
    std::size_t xml = 0;
    for (const AssetFile& f : manifest->files) {
        xml += f.path.ends_with(".xml") ? 1u : 0u;
    }
    EXPECT_EQ(xml, 56u);
    EXPECT_NE(manifest->find("COPYING.CC_BY-SA_3.0"), nullptr);
    EXPECT_NE(manifest->find("provenance.json"), nullptr);
    std::size_t present = 0;
    for (const auto& entry : std::filesystem::directory_iterator(source_folder())) {
        const std::string name = entry.path().filename().string();
        if (name == "manifest.json") {
            continue;
        }
        ++present;
        EXPECT_NE(manifest->find(name), nullptr) << name << " is not listed";
    }
    EXPECT_EQ(present, manifest->files.size());
}

TEST(AssetsLensDatabase, TheProvenanceRecordPinsTheUpstreamSource) {
    const nlohmann::json p = nlohmann::json::parse(test::read_bytes(source_folder() / "provenance.json"));
    EXPECT_EQ(p.at("licence"), "CC-BY-SA-3.0");
    EXPECT_EQ(p.at("modifications"), "none");
    const nlohmann::json& s = p.at("source");
    EXPECT_EQ(s.at("archive_url"), "https://lensfun.github.io/db/version_1.tar.bz2");
    EXPECT_EQ(s.at("archive_sha256"), "97c9f8059accae0128a413bc610a04a4548e8da2764a6d3d60d82bf917639457");
    EXPECT_EQ(s.at("commit"), "43f9e001ab66c4fcdd4f400463f13b72f94b2288");
    EXPECT_EQ(s.at("data_format_version"), 1);
    const std::string licence = test::read_bytes(source_folder() / "COPYING.CC_BY-SA_3.0");
    EXPECT_NE(licence.find("Attribution-ShareAlike 3.0 Unported"), std::string::npos);
}

}  // namespace
}  // namespace lucent::assets
