// The support rules (camera_support.hpp; W10 phase 3, ADR 0018): how a probe's interpretation and
// the camera support database combine into the blueprint's four states, how stored assessments
// follow a new database without reading files, and how the catalog keeps them across a restart.

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/application/camera_support.hpp>
#include <lucent/application/catalog_library.hpp>
#include <lucent/application/import.hpp>
#include <lucent/application/library.hpp>
#include <lucent/assets/mode_key.hpp>

#include "app_test_support.hpp"
#include "catalog_mapping.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;
using raw::InterpretationIssueKind;
using raw::InterpretationStatus;

core::SemanticVersion semver(const char* text) {
    auto parsed = core::SemanticVersion::parse(text);
    EXPECT_TRUE(parsed.has_value());
    return parsed ? *parsed : core::SemanticVersion{};
}

// A probed Sony ARW as the decoded-RAW contract describes it.
raw::RawMetadata sony(InterpretationStatus status = InterpretationStatus::Complete) {
    raw::RawMetadata m;
    m.camera.make = "Sony";
    m.camera.model = "ILCE-7M4";
    m.format.container = raw::ContainerFormat::Tiff;
    m.format.compression.name = "Sony ARW compressed (lossy)";
    m.geometry.raw_width = 7040;
    m.geometry.raw_height = 4688;
    m.geometry.visible_area = image::PixelRect{0, 0, 7028, 4688};
    m.representation.kind = raw::RawRepresentation::Mosaic;
    m.representation.sample_encoding = raw::SampleEncoding::UInt16;
    m.representation.cfa = image::CfaPattern::bayer(image::BayerLayout::Rggb);
    m.levels.white = 16383;
    m.interpretation.status = status;
    if (status == InterpretationStatus::OptionalOperationsSkipped) {
        m.interpretation.issues.push_back(
            {InterpretationIssueKind::OptionalOperationUnsupported, "opcode X"});
    } else if (status == InterpretationStatus::RequiresUnsupportedOperation) {
        m.interpretation.issues.push_back(
            {InterpretationIssueKind::MandatoryOperationUnsupported, "opcode Y"});
    }
    return m;
}

std::string key_of(const raw::RawMetadata& metadata) {
    return assets::derive_mode_key(metadata).to_string();
}

assets::CameraSupportEntry entry_for(const std::string& key, assets::SupportState state,
                                     std::vector<std::string> limitations = {}) {
    assets::CameraSupportEntry entry;
    entry.match = key;
    entry.qualified_key = key;
    entry.state = state;
    entry.camera = "Sony A7 IV (ILCE-7M4)";
    entry.mode = "ARW compressed (lossy)";
    entry.family = "sony";
    entry.evidence = "13 of 13 checks strong";
    for (std::string& code : limitations) {
        entry.limitations.push_back({std::move(code), "measured"});
    }
    return entry;
}

// An active database "camera-support" `version` holding `entries`; `basis_holds` false makes its
// Verified entries not hold for this build.
std::shared_ptr<const assets::CameraSupport> database(const char* version,
                                                      std::vector<assets::CameraSupportEntry> entries,
                                                      bool basis_holds = true) {
    auto support = std::make_shared<assets::CameraSupport>();
    support->asset_id = "camera-support";
    support->version = semver(version);
    support->database.generated = "2026-10-08T03:13:58Z";
    support->database.date = "2026-10-08";
    support->database.basis = assets::running_basis();
    support->database.limitations = {{"xtrans_baseline_demosaic", "X-Trans demosaicing is baseline quality",
                                      "Fine detail can look softer."}};
    support->database.entries = std::move(entries);
    support->database.summary = support->database.count();
    if (!basis_holds) {
        support->basis_differences = {
            "decoder: qualified with LibRaw 0.21.0-Release, this build uses another"};
    }
    return support;
}

std::size_t count_kind(const SupportAssessment& support, SupportReasonKind kind) {
    std::size_t n = 0;
    for (const SupportReason& reason : support.reasons) {
        n += reason.kind == kind ? 1 : 0;
    }
    return n;
}

// ---- Rules -----------------------------------------------------------------------------------------

TEST(SupportRules, WithoutADatabaseTheInterpretationDecidesAndTheKeyIsRecorded) {
    const SupportAssessment clean = assess_support(sony(), nullptr);
    EXPECT_EQ(clean.status, SupportStatus::CompatibleUnverified);
    EXPECT_TRUE(clean.reasons.empty());
    ASSERT_TRUE(clean.camera.has_value());
    EXPECT_EQ(clean.camera->mode_key, key_of(sony()));
    EXPECT_EQ(clean.camera->rules, kSupportRulesVersion);
    EXPECT_TRUE(clean.camera->database_id.empty());
    EXPECT_FALSE(clean.camera->listed.has_value());

    const SupportAssessment optional =
        assess_support(sony(InterpretationStatus::OptionalOperationsSkipped), nullptr);
    EXPECT_EQ(optional.status, SupportStatus::Limited);
    EXPECT_EQ(count_kind(optional, SupportReasonKind::InterpretationIssue), 1u);

    const SupportAssessment mandatory =
        assess_support(sony(InterpretationStatus::RequiresUnsupportedOperation), nullptr);
    EXPECT_EQ(mandatory.status, SupportStatus::Unsupported);
}

TEST(SupportRules, AVerifiedEntryNeedsACompleteInterpretationAndThisBuildsBasis) {
    const auto db = database("1.0.0", {entry_for(key_of(sony()), assets::SupportState::Verified)});
    const SupportAssessment verified = assess_support(sony(), db.get());
    EXPECT_EQ(verified.status, SupportStatus::Verified);
    EXPECT_TRUE(verified.reasons.empty());
    ASSERT_TRUE(verified.camera.has_value());
    EXPECT_EQ(verified.camera->database_id, "camera-support");
    EXPECT_EQ(verified.camera->database_version, "1.0.0");
    EXPECT_EQ(verified.camera->qualified_on, "2026-10-08");
    EXPECT_EQ(verified.camera->listed, SupportStatus::Verified);
    EXPECT_EQ(verified.camera->camera, "Sony A7 IV (ILCE-7M4)");
    EXPECT_EQ(verified.camera->qualified_key, key_of(sony()));

    // Optional gaps: Limited with the reason, whatever the database says.
    const SupportAssessment optional =
        assess_support(sony(InterpretationStatus::OptionalOperationsSkipped), db.get());
    EXPECT_EQ(optional.status, SupportStatus::Limited);
    EXPECT_EQ(count_kind(optional, SupportReasonKind::InterpretationIssue), 1u);

    // A missing mandatory step is always Unsupported.
    for (const InterpretationStatus status :
         {InterpretationStatus::RequiresUnsupportedOperation, InterpretationStatus::Ambiguous,
          InterpretationStatus::NotDecodable}) {
        EXPECT_EQ(assess_support(sony(status), db.get()).status, SupportStatus::Unsupported);
    }

    // Qualified with another decoder or pipeline: not Verified for this build.
    const auto stale = database("1.0.0", {entry_for(key_of(sony()), assets::SupportState::Verified)}, false);
    const SupportAssessment unverified = assess_support(sony(), stale.get());
    EXPECT_EQ(unverified.status, SupportStatus::CompatibleUnverified);
    ASSERT_TRUE(unverified.camera.has_value());
    EXPECT_FALSE(unverified.camera->basis_matches);
    EXPECT_EQ(unverified.camera->listed, SupportStatus::Verified);
}

TEST(SupportRules, TheDatabaseNamesLimitationsAndUnsupportedModes) {
    const std::string key = key_of(sony());
    const auto limited =
        database("1.0.0", {entry_for(key, assets::SupportState::Limited, {"xtrans_baseline_demosaic"})});
    const SupportAssessment a = assess_support(sony(), limited.get());
    EXPECT_EQ(a.status, SupportStatus::Limited);
    ASSERT_EQ(count_kind(a, SupportReasonKind::CameraLimitation), 1u);
    EXPECT_EQ(a.reasons.back().limitation, "xtrans_baseline_demosaic");
    EXPECT_EQ(a.reasons.back().message,
              "X-Trans demosaicing is baseline quality: Fine detail can look softer.");

    const auto unsupported = database("1.0.0", {entry_for(key, assets::SupportState::Unsupported)});
    const SupportAssessment u = assess_support(sony(), unsupported.get());
    EXPECT_EQ(u.status, SupportStatus::Unsupported);
    EXPECT_EQ(count_kind(u, SupportReasonKind::CameraUnsupported), 1u);

    const auto compatible = database("1.0.0", {entry_for(key, assets::SupportState::CompatibleUnverified)});
    EXPECT_EQ(assess_support(sony(), compatible.get()).status, SupportStatus::CompatibleUnverified);

    // An unknown camera that decodes cleanly is Compatible, unverified.
    raw::RawMetadata other = sony();
    other.camera.model = "ILCE-9M3";
    const SupportAssessment unknown = assess_support(other, limited.get());
    EXPECT_EQ(unknown.status, SupportStatus::CompatibleUnverified);
    ASSERT_TRUE(unknown.camera.has_value());
    EXPECT_FALSE(unknown.camera->listed.has_value());
    EXPECT_EQ(unknown.camera->database_version, "1.0.0");
}

TEST(SupportRules, ReassessmentFollowsANewDatabaseWithoutTheFile) {
    const std::string key = key_of(sony());
    const auto v1 = database("1.0.0", {});
    const auto v2 = database("1.1.0", {entry_for(key, assets::SupportState::Verified)});
    const auto v3 =
        database("1.2.0", {entry_for(key, assets::SupportState::Limited, {"xtrans_baseline_demosaic"})});

    const SupportAssessment old = assess_support(sony(), v1.get());
    EXPECT_EQ(old.status, SupportStatus::CompatibleUnverified);
    EXPECT_TRUE(assessed_with(old, v1.get()));
    EXPECT_FALSE(assessed_with(old, v2.get()));
    EXPECT_FALSE(assessed_with(old, nullptr));

    auto upgraded = reassess_support(old, v2.get());
    ASSERT_TRUE(upgraded.has_value());
    EXPECT_EQ(upgraded->status, SupportStatus::Verified);
    EXPECT_TRUE(assessed_with(*upgraded, v2.get()));

    auto limited = reassess_support(*upgraded, v3.get());
    ASSERT_TRUE(limited.has_value());
    EXPECT_EQ(limited->status, SupportStatus::Limited);
    EXPECT_EQ(count_kind(*limited, SupportReasonKind::CameraLimitation), 1u);

    // Back to a database that verifies it: the database's limitation goes, nothing is duplicated.
    auto again = reassess_support(*limited, v2.get());
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->status, SupportStatus::Verified);
    EXPECT_TRUE(again->reasons.empty());

    // The live part is kept: an optional gap stays Limited with its own reason, once.
    const SupportAssessment optional =
        assess_support(sony(InterpretationStatus::OptionalOperationsSkipped), v1.get());
    auto kept = reassess_support(optional, v2.get());
    ASSERT_TRUE(kept.has_value());
    EXPECT_EQ(kept->status, SupportStatus::Limited);
    EXPECT_EQ(count_kind(*kept, SupportReasonKind::InterpretationIssue), 1u);
    EXPECT_EQ(kept->reasons.size(), 1u);

    // No database at all: the mode is not listed anywhere.
    auto none = reassess_support(*upgraded, nullptr);
    ASSERT_TRUE(none.has_value());
    EXPECT_EQ(none->status, SupportStatus::CompatibleUnverified);
    EXPECT_TRUE(assessed_with(*none, nullptr));

    // An assessment without a record cannot be re-assessed: the file must be probed again.
    EXPECT_FALSE(reassess_support(assess_interpretation(sony().interpretation), v2.get()).has_value());
    EXPECT_FALSE(reassess_support(assess_probe_failure(Error{ErrorCode::Corrupt, "x", "raw", 0}), v2.get()));
}

TEST(SupportRules, ItemsWithoutARecordAreProbedAgain) {
    SourceItem item = ts::fake_item(L"C:\\photos\\a.arw");
    item.probe->contract = raw::kDecodedRawContractVersion;
    EXPECT_TRUE(probe_outdated(item));  // no evaluation: catalogued before ADR 0018
    item.support = assess_support(sony(), nullptr);
    EXPECT_FALSE(probe_outdated(item));
    item.support.camera->rules = 0;
    EXPECT_TRUE(probe_outdated(item));
}

// ---- Libraries ---------------------------------------------------------------------------------------

TEST(SupportRefreshTest, ReassessesStaleItemsOfASessionLibrary) {
    SessionLibrary library;
    const std::string key = key_of(sony());
    const auto v1 = database("1.0.0", {});
    SourceItem a = ts::fake_item(L"C:\\photos\\a.arw");
    a.support = assess_support(sony(), v1.get());
    SourceItem b = ts::fake_item(L"C:\\photos\\b.arw");
    b.support = assess_support(sony(InterpretationStatus::RequiresUnsupportedOperation), v1.get());
    SourceItem c = ts::fake_item(L"C:\\photos\\c.arw");  // catalogued before the database
    ASSERT_TRUE(library.add({a, b, c}));

    std::vector<LibraryChange> changes;
    Subscription subscription =
        library.subscribe([&](const LibraryChange& change) { changes.push_back(change); });
    const auto v2 = database("2.0.0", {entry_for(key, assets::SupportState::Verified)});
    auto refreshed = refresh_support(library, v2.get());
    ASSERT_TRUE(refreshed) << refreshed.error().describe();
    EXPECT_EQ(refreshed->items, 3u);
    EXPECT_EQ(refreshed->reassessed, 2u);
    EXPECT_EQ(refreshed->changed, 1u);
    EXPECT_EQ(refreshed->needs_probe, 1u);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].kind, LibraryChangeKind::Updated);
    EXPECT_EQ(changes[0].items.size(), 2u);
    EXPECT_EQ(ts::item_named(library, L"a.arw")->support.status, SupportStatus::Verified);
    EXPECT_EQ(ts::item_named(library, L"b.arw")->support.status, SupportStatus::Unsupported);

    // Nothing left to do for the same database.
    auto again = refresh_support(library, v2.get());
    ASSERT_TRUE(again);
    EXPECT_EQ(again->reassessed, 0u);
    EXPECT_EQ(changes.size(), 1u);

    const std::vector<SupportUpdate> unknown{SupportUpdate{ItemId{999}, a.support}};
    EXPECT_FALSE(library.set_support(unknown).has_value());
}

TEST(SupportRefreshTest, TheCatalogKeepsTheFourStatesAndTheirRecords) {
    ts::ScratchDir dir;
    const std::filesystem::path catalog_path = dir.path() / L"lucent.catalog";
    const std::string key = key_of(sony());
    const auto v1 = database("1.0.0", {entry_for(key, assets::SupportState::Verified)});
    ItemId id;
    {
        auto fixture = ts::CatalogFixture::open(catalog_path);
        ASSERT_TRUE(fixture.library);
        SourceItem item = ts::fake_item(dir.path() / L"a.arw");
        item.probe->contract = raw::kDecodedRawContractVersion;
        item.support = assess_support(sony(), v1.get());
        auto added = fixture.library->add({item});
        ASSERT_TRUE(added);
        id = (*added)[0].id;
    }
    {
        auto fixture = ts::CatalogFixture::open(catalog_path);
        ASSERT_TRUE(fixture.library);
        const auto item = fixture.library->find(id);
        ASSERT_TRUE(item.has_value());
        EXPECT_EQ(item->support.status, SupportStatus::Verified);
        ASSERT_TRUE(item->support.camera.has_value());
        EXPECT_EQ(item->support.camera->mode_key, key);
        EXPECT_EQ(item->support.camera->database_version, "1.0.0");
        EXPECT_TRUE(assessed_with(item->support, v1.get()));
        EXPECT_FALSE(probe_outdated(*item));

        // A new database version: re-assessed and stored.
        const auto v2 =
            database("1.1.0", {entry_for(key, assets::SupportState::Limited, {"xtrans_baseline_demosaic"})});
        auto refreshed = refresh_support(*fixture.library, v2.get());
        ASSERT_TRUE(refreshed) << refreshed.error().describe();
        EXPECT_EQ(refreshed->changed, 1u);
    }
    {
        auto fixture = ts::CatalogFixture::open(catalog_path);
        ASSERT_TRUE(fixture.library);
        const auto item = fixture.library->find(id);
        ASSERT_TRUE(item.has_value());
        EXPECT_EQ(item->support.status, SupportStatus::Limited);
        EXPECT_EQ(count_kind(item->support, SupportReasonKind::CameraLimitation), 1u);
        ASSERT_TRUE(item->support.camera.has_value());
        EXPECT_EQ(item->support.camera->database_version, "1.1.0");
        // The catalog's status column keeps a spelling every reader knows.
        auto record = fixture.catalog->query([&](catalog::ReadTransaction& tx) {
            return tx.find_item(catalog::ItemId{static_cast<std::int64_t>(id.value)});
        });
        ASSERT_TRUE(record && *record);
        EXPECT_EQ((*record)->metadata.support_status, catalog::SupportStatus::Limited);
    }
}

TEST(SupportRefreshTest, ImportAssessesProbedFilesAgainstTheDatabase) {
    ts::ScratchDir dir;
    const std::filesystem::path path = ts::write_dng(dir, L"a.dng");
    auto registry = ts::default_registry();
    raw::ProbeOptions options;
    auto probed = registry->select(path, options, {});
    ASSERT_TRUE(probed) << probed.error().describe();
    const std::string key = key_of(probed->probe.metadata);
    const auto db = database("1.0.0", {entry_for(key, assets::SupportState::Verified)});

    SessionLibrary library;
    ImportRequest request;
    request.files = {path};
    request.camera_support = db;
    auto report = import_sources(library, *registry, request);
    ASSERT_TRUE(report) << report.error().describe();
    EXPECT_EQ(report->verified, 1u);
    EXPECT_EQ(report->compatible_unverified, 0u);
    ASSERT_EQ(library.size(), 1u);
    const SourceItem item = library.items().front();
    EXPECT_EQ(item.support.status, SupportStatus::Verified);
    ASSERT_TRUE(item.support.camera.has_value());
    EXPECT_EQ(item.support.camera->mode_key, key);
}

// ---- Catalog encoding ------------------------------------------------------------------------------

TEST(SupportEncoding, EveryStateRoundTripsAndFitsTheThreeCatalogSpellings) {
    const std::string key = key_of(sony());
    const std::vector<std::shared_ptr<const assets::CameraSupport>> databases = {
        database("1.0.0", {entry_for(key, assets::SupportState::Verified)}),
        database("1.0.0", {}),
        database("1.0.0", {entry_for(key, assets::SupportState::Limited, {"xtrans_baseline_demosaic"})}),
        database("1.0.0", {entry_for(key, assets::SupportState::Unsupported)}),
    };
    const SupportStatus expected[] = {SupportStatus::Verified, SupportStatus::CompatibleUnverified,
                                      SupportStatus::Limited, SupportStatus::Unsupported};
    const catalog::SupportStatus column[] = {
        catalog::SupportStatus::Supported, catalog::SupportStatus::Supported, catalog::SupportStatus::Limited,
        catalog::SupportStatus::Unsupported};
    for (std::size_t i = 0; i < databases.size(); ++i) {
        const SupportAssessment support = assess_support(sony(), databases[i].get());
        ASSERT_EQ(support.status, expected[i]);
        EXPECT_EQ(detail::to_catalog(support.status), column[i]);
        const SupportAssessment restored =
            detail::decode_support(column[i], detail::encode_support_reasons(support));
        EXPECT_EQ(restored.status, support.status) << i;
        EXPECT_EQ(restored.interpretation, support.interpretation) << i;
        ASSERT_TRUE(restored.camera.has_value()) << i;
        EXPECT_EQ(*restored.camera, *support.camera) << i;
        ASSERT_EQ(restored.reasons.size(), support.reasons.size()) << i;
        for (std::size_t r = 0; r < support.reasons.size(); ++r) {
            EXPECT_EQ(restored.reasons[r].kind, support.reasons[r].kind);
            EXPECT_EQ(restored.reasons[r].limitation, support.reasons[r].limitation);
            EXPECT_EQ(restored.reasons[r].message, support.reasons[r].message);
        }
    }
}

TEST(SupportEncoding, ARecordThatDoesNotFitTheColumnIsDropped) {
    const auto db = database("1.0.0", {entry_for(key_of(sony()), assets::SupportState::Verified)});
    const std::string json = detail::encode_support_reasons(assess_support(sony(), db.get()));
    // A Verified record under "limited" (an inconsistent row): the column wins, the record goes, so
    // the item is probed again.
    const SupportAssessment restored = detail::decode_support(catalog::SupportStatus::Limited, json);
    EXPECT_EQ(restored.status, SupportStatus::Limited);
    EXPECT_FALSE(restored.camera.has_value());
    // A pre-ADR 0018 "supported" row stays Compatible, unverified, without a record.
    const SupportAssessment old = detail::decode_support(catalog::SupportStatus::Supported, "[]");
    EXPECT_EQ(old.status, SupportStatus::CompatibleUnverified);
    EXPECT_FALSE(old.camera.has_value());
    // Records of newer support rules are not understood: dropped, not guessed.
    std::string newer = json;
    newer.replace(newer.find("\"rules\":1"), 9, "\"rules\":9");
    EXPECT_FALSE(detail::decode_support(catalog::SupportStatus::Supported, newer).camera.has_value());
}

}  // namespace
}  // namespace lucent::app
