#include <string>

#include <gtest/gtest.h>

#include <lucent/assets/camera_support.hpp>
#include <lucent/assets/mode_key.hpp>

#include "asset_test_support.hpp"

namespace lucent::assets {
namespace {

using test::sample_database;

constexpr const char* kR5 =
    "mk1|canon|eos-r5|cr3|canon-crx-lossless-raw|8191x5463|14bit|mosaic:RGGB|uint16|single-view|primary:0/1";

TEST(CameraSupport, StatesHaveStableSpellingsAndSeverity) {
    EXPECT_EQ(to_string(SupportState::Verified), "verified");
    EXPECT_EQ(to_string(SupportState::CompatibleUnverified), "compatible_unverified");
    EXPECT_EQ(to_string(SupportState::Limited), "limited");
    EXPECT_EQ(to_string(SupportState::Unsupported), "unsupported");
    for (const SupportState state : {SupportState::Verified, SupportState::CompatibleUnverified,
                                     SupportState::Limited, SupportState::Unsupported}) {
        EXPECT_EQ(parse_support_state(to_string(state)), state);
    }
    EXPECT_FALSE(parse_support_state("supported").has_value());
    EXPECT_EQ(worse(SupportState::Verified, SupportState::Limited), SupportState::Limited);
    EXPECT_EQ(worse(SupportState::Unsupported, SupportState::CompatibleUnverified),
              SupportState::Unsupported);
    EXPECT_EQ(worse(SupportState::Verified, SupportState::Verified), SupportState::Verified);
}

TEST(CameraSupport, PatternsMatchComponentWise) {
    auto exact = ModePattern::parse(kR5);
    ASSERT_TRUE(exact.has_value()) << exact.error().describe();
    EXPECT_EQ(exact->wildcards(), 0u);
    EXPECT_TRUE(exact->matches(kR5));
    EXPECT_FALSE(
        exact->matches("mk1|canon|eos-r5|cr3|canon-crx-lossy-c-raw|8191x5463|14bit|mosaic:RGGB|uint16|single-"
                       "view|primary:0/1"));

    auto any_size =
        ModePattern::parse("mk1|canon|eos-r5|cr3|canon-crx-lossless-raw|*|14bit|*|uint16|single-view|*");
    ASSERT_TRUE(any_size.has_value());
    EXPECT_EQ(any_size->wildcards(), 3u);
    EXPECT_TRUE(any_size->matches(kR5));
    EXPECT_TRUE(
        any_size->matches("mk1|canon|eos-r5|cr3|canon-crx-lossless-raw|5087x3391|14bit|mosaic:RGGB|uint16|"
                          "single-view|primary:0/2"));
    EXPECT_FALSE(
        any_size->matches("mk2|canon|eos-r5|cr3|canon-crx-lossless-raw|5087x3391|14bit|mosaic:RGGB|"
                          "uint16|single-view|primary:0/2"));
    EXPECT_FALSE(any_size->matches("mk1|canon|eos-r5"));

    EXPECT_TRUE(ModePattern::parse("mk1|a|b|c|d|e|f|g|h|i|j").has_value());
    for (const char* bad :
         {"", "mk1", "mk2|a|b|c|d|e|f|g|h|i|j", "mk1|a|b|c|d|e|f|g|h|i", "mk1|a|b|c|d|e|f|g|h|i|j|k",
          "mk1|a||c|d|e|f|g|h|i|j", "mk1|a*|b|c|d|e|f|g|h|i|j"}) {
        auto parsed = ModePattern::parse(bad);
        EXPECT_FALSE(parsed.has_value()) << bad;
        if (!parsed) {
            EXPECT_EQ(parsed.error().code, ErrorCode::Corrupt);
        }
    }
}

TEST(CameraSupport, WritesCanonicalDocumentsThatReadBack) {
    CameraSupportDatabase db = sample_database();
    std::swap(db.entries[0], db.entries[2]);  // the writer sorts
    auto text = write_camera_support(db);
    ASSERT_TRUE(text.has_value()) << text.error().describe();
    EXPECT_EQ(text->back(), '\n');
    EXPECT_EQ(text->find('\r'), std::string::npos);
    auto parsed = parse_camera_support(*text);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().describe();
    EXPECT_EQ(parsed->entries.size(), 3u);
    EXPECT_LT(parsed->entries[0].match, parsed->entries[1].match);
    EXPECT_EQ(parsed->summary.verified, 1u);
    EXPECT_EQ(parsed->summary.limited, 1u);
    EXPECT_EQ(parsed->summary.unsupported, 1u);
    EXPECT_EQ(parsed->basis, db.basis);
    auto again = write_camera_support(*parsed);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(*again, *text);
}

TEST(CameraSupport, LookupTakesTheWorstMatchAndTheMostSpecificLabels) {
    CameraSupportDatabase db = sample_database();
    const auto exact = db.lookup(kR5);
    ASSERT_TRUE(exact.has_value());
    EXPECT_EQ(exact->state, SupportState::Verified);
    EXPECT_EQ(exact->matches, 1u);
    ASSERT_NE(exact->entry, nullptr);
    EXPECT_EQ(exact->entry->camera, "Canon EOS R5");
    EXPECT_TRUE(exact->limitations.empty());

    EXPECT_FALSE(db.lookup("mk1|sony|ilce-1|tiff|uncompressed|8640x5760|14bit|mosaic:RGGB|uint16|single-view|"
                           "primary:0/1")
                     .has_value());

    // A curated pattern for every R5 mode with a limitation makes the exact Verified answer Limited.
    db.limitations.push_back({"dual_pixel_not_processed", "Dual Pixel data is not processed", "Unused."});
    CameraSupportEntry broad;
    broad.match = "mk1|canon|eos-r5|*|*|*|*|*|*|*|*";
    broad.qualified_key = broad.match;
    broad.state = SupportState::Limited;
    broad.camera = "Canon EOS R5 (any mode)";
    broad.limitations = {{"dual_pixel_not_processed", ""}};
    db.entries.push_back(broad);
    const auto combined = db.lookup(kR5);
    ASSERT_TRUE(combined.has_value());
    EXPECT_EQ(combined->state, SupportState::Limited);
    EXPECT_EQ(combined->matches, 2u);
    EXPECT_EQ(combined->entry->camera, "Canon EOS R5");  // the exact entry's labels
    EXPECT_EQ(combined->limitations, std::vector<std::string>{"dual_pixel_not_processed"});
    // Such a database still writes and reads.
    auto text = write_camera_support(db);
    ASSERT_TRUE(text.has_value()) << text.error().describe();
    EXPECT_TRUE(parse_camera_support(*text).has_value());
}

TEST(CameraSupport, RefusesInconsistentDatabases) {
    const auto refused = [](const CameraSupportDatabase& db, const char* why) {
        auto written = write_camera_support(db);
        EXPECT_FALSE(written.has_value()) << why;
        if (!written) {
            EXPECT_EQ(written.error().code, ErrorCode::InvalidArgument) << why;
        }
    };
    CameraSupportDatabase db = sample_database();
    db.entries[0].match =
        "mk1|canon|*|cr3|canon-crx-lossless-raw|8191x5463|14bit|mosaic:RGGB|uint16|single-view|"
        "primary:0/1";
    refused(db, "a Verified pattern");
    db = sample_database();
    db.entries[1].limitations.clear();
    refused(db, "Limited without a limitation");
    db = sample_database();
    db.entries[0].limitations = {{"xtrans_baseline_demosaic", ""}};
    refused(db, "Verified with a limitation");
    db = sample_database();
    db.entries[1].limitations = {{"unknown_code", ""}};
    refused(db, "a code without text");
    db = sample_database();
    db.entries.push_back(db.entries[0]);
    refused(db, "a duplicate match");
    db = sample_database();
    db.entries[0].qualified_key = "mk1|canon|eos-r5|*|*|*|*|*|*|*|*";
    refused(db, "a Verified entry qualified on a pattern");
    db = sample_database();
    db.entries[2].qualified_key = "not a key";
    refused(db, "an invalid qualified key");
    db = sample_database();
    db.generated = "2026-10-08 03:13:58";
    refused(db, "a malformed time");
    db = sample_database();
    db.date = "2026-10-09";
    refused(db, "a date that is not the run's");
}

TEST(CameraSupport, ParserIsStrict) {
    auto text = write_camera_support(sample_database());
    ASSERT_TRUE(text.has_value());
    const auto corrupt = [](const std::string& document, const char* why) {
        auto parsed = parse_camera_support(document);
        EXPECT_FALSE(parsed.has_value()) << why;
        if (!parsed) {
            EXPECT_EQ(parsed.error().code, ErrorCode::Corrupt) << why << ": " << parsed.error().describe();
            EXPECT_EQ(parsed.error().where, "assets.camera_support");
        }
    };
    std::string unknown = *text;
    unknown.insert(1, "\"comment\": \"x\", ");
    corrupt(unknown, "an unknown member");
    std::string summary = *text;
    summary.replace(summary.find("\"verified\": 1"), 13, "\"verified\": 2");
    corrupt(summary, "a summary that does not count the entries");
    std::string state = *text;
    state.replace(state.find("\"state\": \"limited\""), 18, "\"state\": \"limitd\"");
    corrupt(state, "an unknown state");
    corrupt("{}", "an empty object");

    std::string newer = *text;
    newer.replace(newer.find("\"schema_version\": 1"), 19, "\"schema_version\": 2");
    auto parsed = parse_camera_support(newer);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, ErrorCode::VersionMismatch);
}

TEST(CameraSupport, BasisDifferencesIgnoreHowEvidenceWasWeighed) {
    const QualificationBasis running = running_basis();
    EXPECT_EQ(running.decoder.rfind("LibRaw ", 0), 0u);
    EXPECT_FALSE(running.decoded_raw_contract.empty());
    EXPECT_EQ(running.mode_key, kModeKeyVersion);
    QualificationBasis qualified = running;
    qualified.check_suite = 7;
    qualified.state_rules = 3;
    EXPECT_TRUE(basis_differences(qualified, running).empty());
    qualified.decoder = "LibRaw 0.21.0-Release";
    qualified.render_pipeline = running.render_pipeline + 1;
    const auto differences = basis_differences(qualified, running);
    ASSERT_EQ(differences.size(), 2u);
    EXPECT_NE(differences[0].find("LibRaw 0.21.0-Release"), std::string::npos);
    EXPECT_NE(differences[1].find("render pipeline"), std::string::npos);
}

TEST(CameraSupport, ActivationChecksTheContentAndTheBasis) {
    auto text = write_camera_support(sample_database());
    ASSERT_TRUE(text.has_value());
    VerifiedAsset asset;
    asset.manifest.id = "camera-support";
    asset.manifest.version = test::version("1.0.0");
    asset.manifest.content = {"lucent.camera-support", 1};
    asset.files.emplace("camera-support.json", *text);
    auto active = activate_camera_support(asset);
    ASSERT_TRUE(active.has_value()) << active.error().describe();
    EXPECT_EQ((*active)->version_text(), "1.0.0");
    EXPECT_TRUE((*active)->verified_entries_hold());
    EXPECT_EQ((*active)->database.entries.size(), 3u);

    asset.manifest.content.schema_version = 2;
    auto newer = activate_camera_support(asset);
    ASSERT_FALSE(newer.has_value());
    EXPECT_EQ(newer.error().code, ErrorCode::VersionMismatch);

    asset.manifest.content = {"lucent.camera-support", 1};
    asset.files.clear();
    auto missing = activate_camera_support(asset);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);

    CameraSupportDatabase old = sample_database();
    old.basis.decoder = "LibRaw 0.20.0-Release";
    auto old_text = write_camera_support(old);
    ASSERT_TRUE(old_text.has_value());
    asset.files.emplace("camera-support.json", *old_text);
    auto stale = activate_camera_support(asset);
    ASSERT_TRUE(stale.has_value());
    EXPECT_FALSE((*stale)->verified_entries_hold());
    EXPECT_EQ((*stale)->basis_differences.size(), 1u);
}

TEST(CameraSupport, LoadsFromAssetFoldersEndToEnd) {
    test::TempDir dir;
    AssetLocations locations;
    locations.bundled = dir.path() / "assets";
    test::AssetSpec spec;
    spec.id = "camera-support";
    spec.content = {"lucent.camera-support", 1};
    auto text = write_camera_support(sample_database());
    ASSERT_TRUE(text.has_value());
    spec.files = {{"camera-support.json", *text}};
    const std::string manifest = test::write_asset(locations.bundled / "camera-support", spec);
    AssetRequest request = test::request_for("camera-support");
    request.pinned_manifest = test::digest_of(manifest);

    CameraSupportLoad load = load_camera_support(locations, request);
    ASSERT_NE(load.active, nullptr) << (load.problem ? load.problem->describe() : std::string());
    EXPECT_EQ(load.active->origin, AssetOrigin::Bundled);
    EXPECT_EQ(load.active->version_text(), "1.2.3");
    EXPECT_FALSE(load.problem.has_value());

    // A database file that is not a valid database is refused even with correct hashes.
    spec.files = {{"camera-support.json", "{\"schema\": \"lucent.camera-support\", \"schema_version\": 1}"}};
    test::write_asset(locations.bundled / "camera-support", spec);
    request.pinned_manifest.reset();
    load = load_camera_support(locations, request);
    EXPECT_EQ(load.active, nullptr);
    ASSERT_TRUE(load.problem.has_value());
    EXPECT_EQ(load.problem->code, ErrorCode::Corrupt);
    ASSERT_FALSE(load.candidates.empty());
    EXPECT_TRUE(load.candidates.back().refused.has_value());
}

}  // namespace
}  // namespace lucent::assets
