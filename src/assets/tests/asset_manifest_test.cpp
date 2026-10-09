#include <string>

#include <gtest/gtest.h>

#include <lucent/assets/asset_manifest.hpp>

#include "asset_test_support.hpp"

namespace lucent::assets {
namespace {

using test::version;

constexpr const char* kEmptySha = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

std::string manifest_text(const std::string& files, const std::string& extra = {},
                          const std::string& schema_version = "1") {
    return std::string("{\"schema\": \"lucent.asset-manifest\", \"schema_version\": ") + schema_version +
           ", \"id\": \"camera-support\", \"version\": \"1.0.0\", \"minimum_app_version\": \"0.1.0\", "
           "\"content\": {\"schema\": \"lucent.camera-support\", \"schema_version\": 1}, \"files\": " +
           files + extra + "}";
}

std::string one_file(const std::string& path, const std::string& sha = kEmptySha) {
    return "[{\"path\": \"" + path + "\", \"size\": 0, \"sha256\": \"" + sha + "\"}]";
}

TEST(AssetManifest, ParsesAValidManifest) {
    auto manifest = parse_asset_manifest(manifest_text(one_file("camera-support.json")));
    ASSERT_TRUE(manifest.has_value()) << manifest.error().describe();
    EXPECT_EQ(manifest->id, "camera-support");
    EXPECT_EQ(manifest->version, version("1.0.0"));
    EXPECT_EQ(manifest->minimum_app_version, version("0.1.0"));
    EXPECT_EQ(manifest->content.schema, "lucent.camera-support");
    EXPECT_EQ(manifest->content.schema_version, 1u);
    ASSERT_EQ(manifest->files.size(), 1u);
    EXPECT_EQ(manifest->files[0].path, "camera-support.json");
    EXPECT_EQ(core::to_hex(manifest->files[0].sha256), kEmptySha);
    EXPECT_NE(manifest->find("camera-support.json"), nullptr);
    EXPECT_EQ(manifest->find("other.json"), nullptr);
}

TEST(AssetManifest, WritesCanonicalTextThatReadsBack) {
    AssetManifest manifest;
    manifest.id = "lens-profiles";
    manifest.version = version("2.0.1-rc.1");
    manifest.minimum_app_version = version("0.3.0");
    manifest.content = {"lucent.lens-profiles", 3};
    auto b = describe_asset_file("profiles/b.json", std::string("bbb"));
    auto a = describe_asset_file("a.json", std::string("a"));
    ASSERT_TRUE(a && b);
    manifest.files = {*b, *a};  // unsorted: the writer sorts
    auto text = write_asset_manifest(manifest);
    ASSERT_TRUE(text.has_value()) << text.error().describe();
    EXPECT_EQ(text->back(), '\n');
    EXPECT_EQ(text->find('\r'), std::string::npos);
    EXPECT_LT(text->find("\"a.json\""), text->find("\"profiles/b.json\""));
    auto parsed = parse_asset_manifest(*text);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().describe();
    EXPECT_EQ(parsed->id, manifest.id);
    EXPECT_EQ(parsed->version.to_string(), "2.0.1-rc.1");
    EXPECT_EQ(parsed->content, manifest.content);
    ASSERT_EQ(parsed->files.size(), 2u);
    EXPECT_EQ(parsed->files[0], *a);
    EXPECT_EQ(parsed->files[1], *b);
    // Byte-stable: writing what was read gives the same text.
    auto again = write_asset_manifest(*parsed);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(*again, *text);
}

TEST(AssetManifest, RefusesNewerOrForeignSchemas) {
    auto newer = parse_asset_manifest(manifest_text(one_file("x.json"), ", \"signature\": \"...\"", "2"));
    ASSERT_FALSE(newer.has_value());
    EXPECT_EQ(newer.error().code, ErrorCode::VersionMismatch);
    EXPECT_EQ(newer.error().where, "assets.manifest");

    auto foreign = parse_asset_manifest(R"({"schema": "lucent.model-manifest", "schema_version": 1})");
    ASSERT_FALSE(foreign.has_value());
    EXPECT_EQ(foreign.error().code, ErrorCode::VersionMismatch);
}

TEST(AssetManifest, RefusesMalformedManifests) {
    const auto corrupt = [](const std::string& text) {
        auto parsed = parse_asset_manifest(text);
        EXPECT_FALSE(parsed.has_value()) << text;
        if (!parsed) {
            EXPECT_EQ(parsed.error().code, ErrorCode::Corrupt) << text << ": " << parsed.error().describe();
        }
    };
    corrupt("not json");
    corrupt("[]");
    corrupt(manifest_text(one_file("x.json"), ", \"extra\": 1"));                  // unknown member
    corrupt(manifest_text("[]"));                                                  // no files
    corrupt(manifest_text(one_file("x.json", std::string(kEmptySha).substr(1))));  // short digest
    corrupt(manifest_text(
        one_file("x.json", "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855")));
    corrupt(manifest_text(one_file("../x.json")));
    corrupt(manifest_text(one_file("a/./x.json")));
    corrupt(manifest_text(one_file("a//x.json")));
    corrupt(manifest_text(one_file("/x.json")));
    corrupt(manifest_text(one_file("C:x.json")));
    corrupt(manifest_text(one_file("a\\\\x.json")));
    corrupt(manifest_text("[{\"path\": \"x.json\", \"size\": 0, \"sha256\": \"" + std::string(kEmptySha) +
                          "\"}, {\"path\": \"X.JSON\", \"size\": 0, \"sha256\": \"" + kEmptySha + "\"}]"));
    corrupt(manifest_text("[{\"path\": \"x.json\", \"size\": -1, \"sha256\": \"" + std::string(kEmptySha) +
                          "\"}]"));
    corrupt(R"({"schema": "lucent.asset-manifest", "schema_version": 1, "id": "Camera", "version": "1.0.0",
                "minimum_app_version": "0.1.0", "content": {"schema": "s", "schema_version": 1}, "files": []})");
    corrupt(R"({"schema": "lucent.asset-manifest", "schema_version": 1, "id": "camera", "version": "v1",
                "minimum_app_version": "0.1.0", "content": {"schema": "s", "schema_version": 1},
                "files": [{"path": "x", "size": 0, "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}]})");
}

TEST(AssetManifest, AssetIdsAndPaths) {
    EXPECT_TRUE(valid_asset_id("camera-support"));
    EXPECT_TRUE(valid_asset_id("a"));
    EXPECT_TRUE(valid_asset_id("lens2-profiles"));
    EXPECT_FALSE(valid_asset_id(""));
    EXPECT_FALSE(valid_asset_id("-a"));
    EXPECT_FALSE(valid_asset_id("a-"));
    EXPECT_FALSE(valid_asset_id("a--b"));
    EXPECT_FALSE(valid_asset_id("A"));
    EXPECT_FALSE(valid_asset_id("a_b"));
    EXPECT_FALSE(valid_asset_id(std::string(65, 'a')));

    EXPECT_TRUE(valid_asset_path("camera-support.json"));
    EXPECT_TRUE(valid_asset_path("profiles/canon/eos r5.json"));
    EXPECT_FALSE(valid_asset_path(""));
    EXPECT_FALSE(valid_asset_path("x/"));
    EXPECT_FALSE(valid_asset_path("x."));
    EXPECT_FALSE(valid_asset_path("x\t"));
    EXPECT_FALSE(valid_asset_path("a|b"));
}

TEST(AssetManifest, WriterRefusesWhatTheReaderWouldRefuse) {
    AssetManifest manifest;
    manifest.id = "camera-support";
    manifest.content = {"lucent.camera-support", 1};
    auto empty = write_asset_manifest(manifest);
    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error().code, ErrorCode::InvalidArgument);
    auto file = describe_asset_file("x.json", std::string());
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(core::to_hex(file->sha256), kEmptySha);
    manifest.files = {*file, *file};
    EXPECT_FALSE(write_asset_manifest(manifest).has_value());
    EXPECT_FALSE(describe_asset_file("../x", std::string()).has_value());
}

}  // namespace
}  // namespace lucent::assets
