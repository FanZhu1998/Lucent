#include <algorithm>
#include <string>

#include <gtest/gtest.h>

#include <lucent/assets/camera_support.hpp>
#include <lucent/assets/lens_database.hpp>
#include <lucent/assets/mode_key.hpp>
#include <lucent/engine/develop.hpp>

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
    broad.image_area = "*";  // any image area: a curated entry, never Verified
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
    db = sample_database();
    db.entries[0].image_area = "*";
    refused(db, "a Verified entry for any image area");
    db = sample_database();
    db.entries[1].image_area.clear();
    refused(db, "an entry without its image area");
    db = sample_database();
    db.basis.develop_engine.clear();
    refused(db, "a basis without the develop-engine versions");
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

    std::string probe_facts = *text;
    probe_facts.replace(probe_facts.find("\"dng_opcodes\": []"), 17, "\"dng_opcodes\": 3");
    corrupt(probe_facts, "DNG opcodes that are not a list");

    std::string newer = *text;
    newer.replace(newer.find("\"schema_version\": 3"), 19, "\"schema_version\": 4");
    auto parsed = parse_camera_support(newer);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, ErrorCode::VersionMismatch);
    // Version 1 lacked the basis and the probe facts a Verified state needs (QUA-02, QUA-08).
    std::string older = *text;
    older.replace(older.find("\"schema_version\": 3"), 19, "\"schema_version\": 1");
    parsed = parse_camera_support(older);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, ErrorCode::VersionMismatch);
    // Version 3 requires the basis' lens database; version 2 has none (and may not name one).
    std::string no_lens = *text;
    const std::size_t key = no_lens.find("\"lens_database\"");
    ASSERT_NE(key, std::string::npos) << *text;
    const std::size_t comma = no_lens.rfind(',', key);  // after the member before it
    no_lens.erase(comma, no_lens.find('\n', key) - comma);
    corrupt(no_lens, "a version-3 basis without the lens database");
    std::string version2 = no_lens;
    version2.replace(version2.find("\"schema_version\": 3"), 19, "\"schema_version\": 2");
    auto read2 = parse_camera_support(version2);
    ASSERT_TRUE(read2.has_value()) << read2.error().describe();
    EXPECT_TRUE(read2->basis.lens_database.empty());
    // A version-3 basis names a lens database: an empty name is corrupt.
    std::string empty_lens = *text;
    const std::string named = lens_database_basis();
    ASSERT_NE(empty_lens.find(named), std::string::npos);
    empty_lens.replace(empty_lens.find(named), named.size(), "");
    corrupt(empty_lens, "a version-3 basis with an empty lens database");

    // The writer writes version 2 for a basis without a lens database (the export of a report made
    // before lens corrections): the document such a report always exported, read back unchanged.
    CameraSupportDatabase old = sample_database();
    old.basis.lens_database.clear();
    EXPECT_EQ(camera_support_schema_version(old), 2u);
    EXPECT_EQ(camera_support_schema_version(sample_database()), kCameraSupportSchemaVersion);
    auto old_text = write_camera_support(old);
    ASSERT_TRUE(old_text) << old_text.error().describe();
    EXPECT_NE(old_text->find("\"schema_version\": 2"), std::string::npos);
    EXPECT_EQ(old_text->find("lens_database"), std::string::npos);
    auto old_read = parse_camera_support(*old_text);
    ASSERT_TRUE(old_read) << old_read.error().describe();
    EXPECT_EQ(old_read->basis, old.basis);
}

// W13 closure (ADR 0023): a version-2 database (qualified without lens corrections) does not certify a
// build that corrects lenses: the basis names the bundled lens database by its pinned manifest.
TEST(CameraSupport, TheBasisNamesTheLensDatabase) {
    const QualificationBasis running = running_basis();
    EXPECT_EQ(running.lens_database, lens_database_basis());
    EXPECT_NE(running.lens_database.find("lens-database (manifest "), std::string::npos) << running.lens_database;
    QualificationBasis version2 = running;
    version2.lens_database.clear();
    const auto differences = basis_differences(version2, running);
    ASSERT_EQ(differences.size(), 1u);
    EXPECT_NE(differences[0].find("lens database"), std::string::npos) << differences[0];
    QualificationBasis other = running;
    other.lens_database = "lens-database (manifest 0000)";
    EXPECT_EQ(basis_differences(other, running).size(), 1u);
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

// QUA-02: every version of what a Verified state certifies is part of the basis, so changing any of
// them demotes Verified until the mode is qualified again.
TEST(CameraSupport, TheBasisCoversEveryEngineDecoderAndExportVersion) {
    const QualificationBasis running = running_basis();
    EXPECT_EQ(running.develop_engine.size(), engine::kDevelopEngineVersions.size());
    EXPECT_GT(running.neutral_look, 0u);
    EXPECT_GT(running.export_version, 0u);
    EXPECT_TRUE(std::is_sorted(running.decoder_capabilities.begin(), running.decoder_capabilities.end()));
    for (std::size_t i = 0; i < running.develop_engine.size(); ++i) {
        QualificationBasis qualified = running;
        ++qualified.develop_engine[i];
        const auto differences = basis_differences(qualified, running);
        ASSERT_EQ(differences.size(), 1u) << "develop engine version " << i;
        EXPECT_NE(differences[0].find("develop engine"), std::string::npos) << differences[0];
    }
    QualificationBasis look = running;
    ++look.neutral_look;
    EXPECT_EQ(basis_differences(look, running).size(), 1u);
    QualificationBasis exported = running;
    ++exported.export_version;
    EXPECT_EQ(basis_differences(exported, running).size(), 1u);
    QualificationBasis codecs = running;
    codecs.decoder_capabilities.push_back("zz-test-codec");
    ASSERT_EQ(basis_differences(codecs, running).size(), 1u);
    EXPECT_NE(basis_differences(codecs, running)[0].find("zz-test-codec"), std::string::npos);
    QualificationBasis fewer = running;
    fewer.develop_engine.pop_back();
    EXPECT_EQ(basis_differences(fewer, running).size(), 1u) << "an appended engine version differs";
}

// QUA-08: the probe facts beside the key a Verified entry is qualified for.
TEST(ModeVariant, NamesTheImageAreaAndTheDngOpcodes) {
    raw::RawMetadata m;
    EXPECT_EQ(derive_mode_variant(m).image_area, "unknown");
    EXPECT_TRUE(derive_mode_variant(m).dng_opcodes.empty());
    EXPECT_EQ(to_string(derive_mode_variant(m)), "unknown; no DNG opcodes");

    m.image_area.source = raw::ImageAreaSource::DecoderInsetCrop;
    m.image_area.rect = raw::AreaRect{144.0, 112.0, 8192.0, 5464.0};
    m.image_area.user_crop = raw::NormalizedCrop{0.0, 0.1665, 1.0, 0.8335};  // not part of the variant
    EXPECT_EQ(derive_mode_variant(m).image_area, "decoder_inset_crop 8192x5464");
    m.image_area.source = raw::ImageAreaSource::DngDefaultCrop;
    m.image_area.rect = raw::AreaRect{8.0, 8.0, 6000.5, 4000.0};
    EXPECT_EQ(derive_mode_variant(m).image_area, "dng_default_crop 6000.5x4000");

    raw::RequiredOperation warp;
    warp.kind = raw::RequiredOperationKind::DngOpcode;
    warp.stage = raw::OperationStage::AfterDemosaic;
    warp.name = "WarpRectilinear";
    raw::RequiredOperation vignette = warp;
    vignette.name = "FixVignetteRadial";
    vignette.mandatory = false;
    raw::RequiredOperation gain = warp;
    gain.stage = raw::OperationStage::LinearRaw;
    gain.name = "GainMap";
    m.required_operations = {gain, warp, vignette};
    const ModeVariant variant = derive_mode_variant(m);
    EXPECT_EQ(variant.dng_opcodes,
              (std::vector<std::string>{"list2:GainMap", "list3:WarpRectilinear", "list3:FixVignetteRadial optional"}));
    EXPECT_EQ(to_string(variant),
              "dng_default_crop 6000.5x4000; list2:GainMap, list3:WarpRectilinear, list3:FixVignetteRadial optional");
    ModeVariant other = variant;
    other.dng_opcodes.pop_back();
    EXPECT_NE(other, variant);
}

TEST(CameraSupport, StandardLimitationTextsCoverEveryCode) {
    for (const std::string_view code :
         {kLimitationDefaultCrop, kLimitationXTransBaseline, kLimitationDualPixel, kLimitationEnhancedImage,
          kLimitationOptionalOperations, kLimitationNonSquarePixels, kLimitationPixelShift}) {
        const auto text = standard_limitation_text(code);
        ASSERT_TRUE(text.has_value()) << code;
        EXPECT_EQ(text->code, code);
        EXPECT_FALSE(text->summary.empty());
        EXPECT_FALSE(text->meaning.empty());
    }
    EXPECT_FALSE(standard_limitation_text("not_a_code").has_value());
}

TEST(CameraSupport, ActivationChecksTheContentAndTheBasis) {
    auto text = write_camera_support(sample_database());
    ASSERT_TRUE(text.has_value());
    VerifiedAsset asset;
    asset.manifest.id = "camera-support";
    asset.manifest.version = test::version("1.0.0");
    asset.manifest.content = {"lucent.camera-support", kCameraSupportSchemaVersion};
    asset.files.emplace("camera-support.json", *text);
    auto active = activate_camera_support(asset);
    ASSERT_TRUE(active.has_value()) << active.error().describe();
    EXPECT_EQ((*active)->version_text(), "1.0.0");
    EXPECT_TRUE((*active)->verified_entries_hold());
    EXPECT_EQ((*active)->database.entries.size(), 3u);

    asset.manifest.content.schema_version = kCameraSupportSchemaVersion + 1;
    auto newer = activate_camera_support(asset);
    ASSERT_FALSE(newer.has_value());
    EXPECT_EQ(newer.error().code, ErrorCode::VersionMismatch);

    asset.manifest.content = {"lucent.camera-support", kCameraSupportSchemaVersion};
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
    spec.content = {"lucent.camera-support", kCameraSupportSchemaVersion};
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
    spec.files = {{"camera-support.json", "{\"schema\": \"lucent.camera-support\", \"schema_version\": 2}"}};
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
