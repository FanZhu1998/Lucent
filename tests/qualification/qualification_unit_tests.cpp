// Unit tests of the qualification harness (label "qualification"; ADR 0016): mode keys, the
// expectations schema and its evidence rule, the state rules, family matching and the report
// writers, on synthetic data; plus the validity of the committed expectations and families files.
// W10 phase 3 (ADR 0018): the camera support database export, and that the committed asset
// (assets/camera-support) is exactly the export of the committed report.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <lucent/image/cfa.hpp>
#include <lucent/raw/decoded_raw.hpp>

#include "expectations.hpp"
#include "mode_key.hpp"
#include "qualification.hpp"
#include "report.hpp"
#include "support_export.hpp"

namespace lucent::qualify {
namespace {

using Json = nlohmann::json;

// ---- Mode key ---------------------------------------------------------------------------------------

TEST(ModeKey, SlugsKeepLettersDigitsAndStars) {
    EXPECT_EQ(slug("Canon CRX lossless (RAW)"), "canon-crx-lossless-raw");
    EXPECT_EQ(slug("Canon CRX lossy (C-RAW)"), "canon-crx-lossy-c-raw");
    EXPECT_EQ(slug("Nikon NEF high efficiency (HE)"), "nikon-nef-high-efficiency-he");
    EXPECT_EQ(slug("Nikon NEF high efficiency (HE*)"), "nikon-nef-high-efficiency-he-star");
    EXPECT_EQ(slug("  M MONOCHROM (Typ 246) "), "m-monochrom-typ-246");
    EXPECT_EQ(slug(""), "");
}

TEST(ModeKey, CanonicalMakersAndModels) {
    EXPECT_EQ(canonical_make("NIKON CORPORATION"), "nikon");
    EXPECT_EQ(canonical_make("Nikon"), "nikon");
    EXPECT_EQ(canonical_make("RICOH IMAGING COMPANY, LTD."), "pentax");
    EXPECT_EQ(canonical_make("OM Digital Solutions"), "om-system");
    EXPECT_EQ(canonical_make("OM Digital"), "om-system");
    EXPECT_EQ(canonical_make("Arashi Vision"), "insta360");
    EXPECT_EQ(canonical_make("Leica Camera AG"), "leica");
    EXPECT_EQ(canonical_make("SONY"), "sony");
    EXPECT_EQ(canonical_make("Some New Maker Inc."), "some-new-maker-inc");
    EXPECT_EQ(canonical_model("canon", "Canon EOS R5"), "eos-r5");
    EXPECT_EQ(canonical_model("canon", "EOS R5"), "eos-r5");
    EXPECT_EQ(canonical_model("pentax", "PENTAX K-3 Mark III"), "k-3-mark-iii");
    EXPECT_EQ(canonical_model("insta360", "Insta360 One2"), "one2");
    EXPECT_EQ(canonical_model("fujifilm", "GFX50S II"), "gfx50s-ii");
    EXPECT_EQ(canonical_model("nikon", "Z 6"), "z-6");
    // A model that is only the maker's name is kept.
    EXPECT_EQ(canonical_model("dji", "DJI"), "dji");
}

TEST(ModeKey, BitsFollowTheWhiteLevel) {
    EXPECT_EQ(bits_for_white_level(16383), 14u);
    EXPECT_EQ(bits_for_white_level(15100), 14u);
    EXPECT_EQ(bits_for_white_level(16384), 15u);
    EXPECT_EQ(bits_for_white_level(17536), 15u);
    EXPECT_EQ(bits_for_white_level(4095), 12u);
    EXPECT_EQ(bits_for_white_level(65535), 16u);
    EXPECT_EQ(bits_for_white_level(1), 1u);
    EXPECT_EQ(bits_for_white_level(0), 0u);
    EXPECT_EQ(bits_for_white_level(-5), 0u);
}

raw::RawMetadata sony_metadata() {
    raw::RawMetadata m;
    m.stage = raw::MetadataStage::AfterUnpack;
    m.camera.make = "Sony";
    m.camera.model = "ILCE-7M4";
    m.format.container = raw::ContainerFormat::Tiff;
    m.format.compression.name = "Sony ARW compressed (lossy)";
    m.format.compression.lossiness = raw::Lossiness::Lossy;
    m.geometry.raw_width = 7040;
    m.geometry.raw_height = 4688;
    m.geometry.visible_area = image::PixelRect{0, 0, 7028, 4688};
    m.representation.kind = raw::RawRepresentation::Mosaic;
    m.representation.sample_encoding = raw::SampleEncoding::UInt16;
    m.representation.channels = 1;
    m.representation.color_count = 3;
    m.representation.channel_colors = {image::CfaColor::Red, image::CfaColor::Green, image::CfaColor::Blue,
                                       image::CfaColor::Green2};
    m.representation.cfa = image::CfaPattern::bayer(image::BayerLayout::Rggb);
    m.levels.white = 16383;
    return m;
}

TEST(ModeKey, DerivedFromTheContract) {
    const ModeKey key = derive_mode_key(sony_metadata());
    EXPECT_EQ(key.to_string(),
              "mk1|sony|ilce-7m4|tiff|sony-arw-compressed-lossy|7028x4688|14bit|mosaic:RGGB|uint16|single-"
              "view|primary:0/1");
    EXPECT_EQ(key.model_key(), "sony|ilce-7m4");
    EXPECT_EQ(key.bits_provenance, "lucent_rule");
    EXPECT_EQ(key.compression_lossiness, "lossy");
    EXPECT_EQ(key.stage, "after_unpack");
}

TEST(ModeKey, ComponentsDistinguishModes) {
    // The CFA is taken at the visible-area origin: an odd left margin turns RGGB into GRBG.
    raw::RawMetadata m = sony_metadata();
    m.geometry.visible_area = image::PixelRect{1, 0, 7028, 4688};
    EXPECT_NE(derive_mode_key(m).to_string().find("|mosaic:GRBG|"), std::string::npos);

    // A different visible area (crop mode) is a different key.
    m = sony_metadata();
    m.geometry.visible_area = image::PixelRect{0, 0, 4692, 3132};
    EXPECT_NE(derive_mode_key(m).to_string(), derive_mode_key(sony_metadata()).to_string());

    // DNG: bits from the raw IFD's BitsPerSample, not from the white level.
    m = sony_metadata();
    m.format.container = raw::ContainerFormat::Dng;
    m.dng.emplace();
    m.dng->raw_ifd.bits_per_sample = {12};
    const ModeKey dng = derive_mode_key(m);
    EXPECT_EQ(dng.bits, "12");
    EXPECT_EQ(dng.bits_provenance, "dng_tag");

    // Floating-point data, full-colour and monochrome representations.
    m = sony_metadata();
    m.representation.sample_encoding = raw::SampleEncoding::Float32;
    EXPECT_EQ(derive_mode_key(m).bits, "f32");
    m = sony_metadata();
    m.representation.kind = raw::RawRepresentation::LinearColor;
    m.representation.cfa = image::CfaPattern();
    EXPECT_EQ(derive_mode_key(m).representation, "linear-rgb");
    m.representation.kind = raw::RawRepresentation::Monochrome;
    EXPECT_EQ(derive_mode_key(m).representation, "monochrome");

    // Subimage role and view layout.
    m = sony_metadata();
    m.subimage_index = 1;
    m.subimage_count = 2;
    m.subimage_role = raw::SubimageRole::DualPixelComponent;
    m.projection.layout = raw::ViewLayout::DualFisheye360;
    const std::string text = derive_mode_key(m).to_string();
    EXPECT_NE(text.find("|dual-fisheye-360|dual-pixel-component:1/2"), std::string::npos) << text;

    // Ricoh Imaging's maker string with a non-Pentax model is "ricoh".
    m = sony_metadata();
    m.camera.make = "RICOH IMAGING COMPANY, LTD.";
    m.camera.model = "GR III";
    EXPECT_EQ(derive_mode_key(m).make, "ricoh");
    m.camera.model = "PENTAX K-3 Mark III";
    EXPECT_EQ(derive_mode_key(m).make, "pentax");
    EXPECT_EQ(derive_mode_key(m).model, "k-3-mark-iii");
}

// ---- Expectations -----------------------------------------------------------------------------------

Json minimal_expectations() {
    Json checks = Json::object();
    for (const std::string_view id : check_ids()) {
        checks[std::string(id)] = "pass";
    }
    const Json fact = {{"status", "verified"}, {"sources", {"spec"}}};
    return Json{
        {"schema", "lucent.qualification-expectations"},
        {"schema_version", 1},
        {"mode_key_version", kModeKeyVersion},
        {"reviewed", "2026-10-05"},
        {"sources",
         {{"spec", {{"kind", "manufacturer_spec"}, {"title", "Spec"}, {"url", "https://example.com"}}},
          {"rawpy", {{"kind", "decoder_reference"}, {"title", "rawpy"}}}}},
        {"fixtures",
         {{{"id", "cam-a"},
           {"camera",
            {{"family", "other"},
             {"file_make", "Make"},
             {"file_model", "Model"},
             {"product", "Make Model"},
             {"identity", fact}}},
           {"mode", {{"key", "mk1|x"}, {"label", "RAW"}, {"compression", fact}}},
           {"dimensions",
            {{"rendered", {100, 80}},
             {"camera_image_area", {{"size", {100, 80}}, {"status", "verified"}, {"sources", {"spec"}}}}}},
           {"orientation", {{"exif", 1}, {"status", "verified"}, {"sources", {"spec"}}}},
           {"representation", {{"value", "mosaic:RGGB"}, {"status", "verified"}, {"sources", {"spec"}}}},
           {"mandatory_steps", Json::array()},
           {"expected", {{"state", "verified"}, {"checks", checks}, {"limitations", Json::array()}}}}}}};
}

TEST(Expectations, ParsesAValidDocument) {
    auto parsed = parse_expectations(minimal_expectations());
    ASSERT_TRUE(parsed) << parsed.error().describe();
    ASSERT_EQ(parsed->fixtures.size(), 1u);
    const FixtureExpectation& f = parsed->fixtures.front();
    EXPECT_EQ(f.id, "cam-a");
    EXPECT_TRUE(f.identity.verified());
    ASSERT_TRUE(f.rendered);
    EXPECT_EQ(f.rendered->width, 100);
    EXPECT_EQ(f.expected_checks.size(), check_ids().size());
    EXPECT_NE(parsed->find("cam-a"), nullptr);
    EXPECT_EQ(parsed->find("cam-b"), nullptr);
}

TEST(Expectations, TheImageAreaSourceIsOptionalAndChecked) {
    Json doc = minimal_expectations();
    auto plain = parse_expectations(doc);
    ASSERT_TRUE(plain) << plain.error().describe();
    EXPECT_FALSE(plain->fixtures.front().image_area_source);  // absent: not asserted
    for (const char* source :
         {"dng_default_crop", "dng_default_whole_image", "decoder_inset_crop", "unknown"}) {
        doc["fixtures"][0]["dimensions"]["image_area_source"] = source;
        auto parsed = parse_expectations(doc);
        ASSERT_TRUE(parsed) << parsed.error().describe();
        EXPECT_EQ(parsed->fixtures.front().image_area_source, std::optional<std::string>(source));
    }
    doc["fixtures"][0]["dimensions"]["image_area_source"] = "default_crop";
    auto bad = parse_expectations(doc);
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, ErrorCode::InvalidArgument);
    EXPECT_NE(bad.error().message.find("image_area_source"), std::string::npos) << bad.error().message;
}

// QUA-07: where the area is placed is an asserted outcome too.
TEST(Expectations, TheAppliedImageAreaIsOptionalAndChecked) {
    Json doc = minimal_expectations();
    auto plain = parse_expectations(doc);
    ASSERT_TRUE(plain) << plain.error().describe();
    EXPECT_FALSE(plain->fixtures.front().image_area_applied_asserted);
    doc["fixtures"][0]["dimensions"]["image_area_applied"] = nullptr;
    auto whole = parse_expectations(doc);
    ASSERT_TRUE(whole) << whole.error().describe();
    EXPECT_TRUE(whole->fixtures.front().image_area_applied_asserted);
    EXPECT_FALSE(whole->fixtures.front().image_area_applied.has_value()) << "null: the whole image is the frame";
    doc["fixtures"][0]["dimensions"]["image_area_applied"] = {12, 8, 76, 64};
    auto placed = parse_expectations(doc);
    ASSERT_TRUE(placed) << placed.error().describe();
    EXPECT_EQ(placed->fixtures.front().image_area_applied, (std::array<std::int32_t, 4>{12, 8, 76, 64}));
    doc["fixtures"][0]["dimensions"]["image_area_applied"] = {12, 8, 76};
    EXPECT_FALSE(parse_expectations(doc));
    doc["fixtures"][0]["dimensions"]["image_area_applied"] = "12,8";
    EXPECT_FALSE(parse_expectations(doc));
}

TEST(Expectations, AVerifiedFactNeedsAnIndependentSource) {
    Json doc = minimal_expectations();
    doc["fixtures"][0]["orientation"]["sources"] = {"rawpy"};  // the reference decoder alone
    auto parsed = parse_expectations(doc);
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().code, ErrorCode::InvalidArgument);
    EXPECT_NE(parsed.error().message.find("independent"), std::string::npos) << parsed.error().message;

    // The same fact marked unverified is accepted.
    doc["fixtures"][0]["orientation"]["status"] = "unverified";
    EXPECT_TRUE(parse_expectations(doc));
}

TEST(Expectations, RejectsUnknownIdsAndVersions) {
    Json doc = minimal_expectations();
    doc["schema_version"] = 2;
    EXPECT_EQ(parse_expectations(doc).error().code, ErrorCode::VersionMismatch);

    doc = minimal_expectations();
    doc["mode_key_version"] = kModeKeyVersion + 1;
    EXPECT_EQ(parse_expectations(doc).error().code, ErrorCode::VersionMismatch);

    doc = minimal_expectations();
    doc["schema"] = "something.else";
    EXPECT_EQ(parse_expectations(doc).error().code, ErrorCode::VersionMismatch);

    doc = minimal_expectations();
    doc["fixtures"][0]["expected"]["checks"]["no_such_check"] = "pass";
    EXPECT_EQ(parse_expectations(doc).error().code, ErrorCode::InvalidArgument);

    doc = minimal_expectations();
    doc["fixtures"][0]["expected"]["checks"].erase("colour");
    EXPECT_EQ(parse_expectations(doc).error().code, ErrorCode::InvalidArgument);

    doc = minimal_expectations();
    doc["fixtures"][0]["expected"]["limitations"] = {"no_such_limitation"};
    EXPECT_EQ(parse_expectations(doc).error().code, ErrorCode::InvalidArgument);

    doc = minimal_expectations();
    doc["fixtures"][0]["expected"]["state"] = "supported";
    EXPECT_EQ(parse_expectations(doc).error().code, ErrorCode::InvalidArgument);

    doc = minimal_expectations();
    doc["fixtures"][0]["camera"]["identity"]["sources"] = {"nowhere"};
    EXPECT_EQ(parse_expectations(doc).error().code, ErrorCode::InvalidArgument);

    doc = minimal_expectations();
    doc["fixtures"].push_back(doc["fixtures"][0]);
    EXPECT_EQ(parse_expectations(doc).error().code, ErrorCode::InvalidArgument);

    doc = minimal_expectations();
    doc["sources"]["spec"]["kind"] = "blog";
    EXPECT_EQ(parse_expectations(doc).error().code, ErrorCode::InvalidArgument);
}

// ---- State rules ------------------------------------------------------------------------------------

FixtureExpectation verified_expectation() {
    auto parsed = parse_expectations(minimal_expectations());
    return parsed->fixtures.front();
}

FixtureResult passing_result() {
    FixtureResult r;
    r.id = "cam-a";
    r.decoded = true;
    for (const std::string_view id : check_ids()) {
        CheckResult c;
        c.id = std::string(id);
        c.status = CheckStatus::Pass;
        c.evidence = Evidence::Strong;
        r.checks.push_back(c);
    }
    ModeKey key;
    key.make = "x";
    r.mode_key = key;
    return r;
}

CheckResult& check_named(FixtureResult& r, std::string_view id) {
    return *std::find_if(r.checks.begin(), r.checks.end(), [&](const CheckResult& c) { return c.id == id; });
}

TEST(StateRules, VerifiedNeedsStrongEvidenceAndVerifiedFacts) {
    FixtureExpectation e = verified_expectation();
    FixtureResult r = passing_result();
    e.mode_key = r.mode_key->to_string();
    propose_state(r, &e);
    EXPECT_EQ(r.state, SupportState::Verified);
    EXPECT_TRUE(r.evidence_gaps.empty());
    EXPECT_TRUE(r.expectation_mismatches.empty());

    // A weak check (e.g. colour not like for like) is an evidence gap.
    FixtureResult weak = passing_result();
    check_named(weak, kCheckColour).evidence = Evidence::Weak;
    propose_state(weak, &e);
    EXPECT_EQ(weak.state, SupportState::CompatibleUnverified);
    ASSERT_EQ(weak.evidence_gaps.size(), 1u);
    EXPECT_NE(weak.evidence_gaps.front().find("colour"), std::string::npos);

    // An unverified fact is an evidence gap.
    FixtureExpectation unverified = e;
    unverified.camera_image_area_fact.status = "unverified";
    FixtureResult r2 = passing_result();
    propose_state(r2, &unverified);
    EXPECT_EQ(r2.state, SupportState::CompatibleUnverified);

    // Without an expectation nothing is verified.
    FixtureResult r3 = passing_result();
    propose_state(r3, nullptr);
    EXPECT_EQ(r3.state, SupportState::CompatibleUnverified);
    EXPECT_FALSE(r3.expected_state.has_value());
}

TEST(StateRules, LimitationsFailuresAndRefusals) {
    FixtureExpectation e = verified_expectation();

    FixtureResult limited = passing_result();
    limited.limitations.push_back({std::string(kLimitXTransBaseline), "baseline"});
    propose_state(limited, &e);
    EXPECT_EQ(limited.state, SupportState::Limited);

    // A failed check outranks a limitation.
    FixtureResult failed = limited;
    check_named(failed, kCheckBlackWhite).status = CheckStatus::Fail;
    propose_state(failed, &e);
    EXPECT_EQ(failed.state, SupportState::Unsupported);
    EXPECT_NE(failed.state_reasons.front().find("black_white"), std::string::npos);

    // A refused decode is Unsupported.
    FixtureResult refused = passing_result();
    refused.decoded = false;
    refused.decode_error = "unsupported at raw.libraw.unpack: Unsupported file format";
    propose_state(refused, &e);
    EXPECT_EQ(refused.state, SupportState::Unsupported);

    // A harness problem is not a camera state.
    FixtureResult missing = passing_result();
    check_named(missing, kCheckFixtureIntegrity).status = CheckStatus::Fail;
    missing.harness_error = "fixture file missing";
    propose_state(missing, &e);
    EXPECT_EQ(missing.state, SupportState::NotQualified);
}

TEST(StateRules, MismatchesWithTheExpectationAreListed) {
    FixtureExpectation e = verified_expectation();
    FixtureResult r = passing_result();
    check_named(r, kCheckDeterminism).evidence = Evidence::Weak;
    propose_state(r, &e);
    // State differs (compatible_unverified vs verified) and the mode key differs ("mk1|x").
    EXPECT_EQ(r.expectation_mismatches.size(), 2u);

    e.expected_state = "compatible_unverified";
    e.mode_key = r.mode_key->to_string();
    e.expected_checks[std::string(kCheckDeterminism)] = "fail";
    e.expected_limitations = {std::string(kLimitDefaultCrop)};
    propose_state(r, &e);
    ASSERT_EQ(r.expectation_mismatches.size(), 2u);
    EXPECT_NE(r.expectation_mismatches[0].find("determinism"), std::string::npos);
    EXPECT_NE(r.expectation_mismatches[1].find("limitations"), std::string::npos);
}

// ---- Families and report ----------------------------------------------------------------------------

OrderedJson fixture_json(const std::string& id, const std::string& state, const std::string& area,
                         const std::string& limitation = {}) {
    FixtureResult r = passing_result();
    r.id = id;
    r.camera_make = "Sony";
    r.camera_model = "ILCE-7M4";
    raw::RawMetadata m = sony_metadata();
    if (area == "small") {
        m.geometry.visible_area = image::PixelRect{0, 0, 4692, 3132};
    }
    r.mode_key = derive_mode_key(m);
    if (!limitation.empty()) {
        r.limitations.push_back({limitation, "detail"});
    }
    propose_state(r, nullptr);
    OrderedJson j = r.to_json();
    j["proposed_state"] = state;
    return j;
}

TEST(Families, TargetsMatchModeKeyComponents) {
    const OrderedJson key = fixture_json("a", "verified", "full").at("mode_key");
    TargetMatch m;
    m.make = "sony";
    EXPECT_TRUE(matches(m, key));
    m.models = {"ilce-7rm5", "ilce-7m4"};
    EXPECT_TRUE(matches(m, key));
    m.models_not = {"ilce-7m4"};
    EXPECT_FALSE(matches(m, key));
    m.models_not.clear();
    m.area = "7028x4688";
    m.compression = "sony-arw-compressed-lossy";
    m.lossiness = "lossy";
    m.representation_prefix = "mosaic:";
    m.containers = {"tiff", "dng"};
    EXPECT_TRUE(matches(m, key));
    m.subimage_count_max = 1;
    EXPECT_TRUE(matches(m, key));
    m.subimage_count_min = 2;
    EXPECT_FALSE(matches(m, key));
    TargetMatch never;
    never.never = true;
    EXPECT_FALSE(matches(never, key));
    EXPECT_FALSE(matches(TargetMatch{}, OrderedJson(nullptr)));
}

TEST(Report, SupportTableTakesTheWorstStatePerModeAndListsGaps) {
    std::vector<OrderedJson> fixtures{fixture_json("a", "verified", "full"),
                                      fixture_json("b", "limited", "full"),
                                      fixture_json("c", "verified", "small")};
    std::vector<Family> families(1);
    families[0].id = "sony";
    families[0].name = "Sony";
    FamilyTarget full;
    full.id = "full";
    full.description = "full frame";
    full.match.make = "sony";
    full.match.area = "7028x4688";
    FamilyTarget absent;
    absent.id = "absent";
    absent.description = "a mode without fixtures";
    absent.match.never = true;
    families[0].targets = {full, absent};
    const Json manifest = {
        {"gaps", {{{"target", "Scene diversity"}, {"status", "absent"}, {"detail", "daylight only"}}}}};
    ReportInputs inputs{CorpusPaths{std::filesystem::path(LUCENT_SOURCE_DIR)}, "ids", {"a", "b", "c"}, false};
    const OrderedJson report = build_report(fixtures, inputs, families, manifest);

    EXPECT_EQ(report.at("schema").get<std::string>(), kReportSchema);
    EXPECT_EQ(report.at("schema_version").get<int>(), kReportSchemaVersion);
    EXPECT_EQ(report.at("summary").at("fixtures").get<int>(), 3);
    ASSERT_EQ(report.at("support_table").size(), 2u);  // two mode keys
    EXPECT_EQ(report.at("support_table")[0].at("proposed_state").get<std::string>(), "limited");
    EXPECT_EQ(report.at("support_table")[0].at("fixtures").size(), 2u);
    EXPECT_EQ(report.at("support_table")[1].at("proposed_state").get<std::string>(), "verified");
    const OrderedJson& targets = report.at("coverage").at("families")[0].at("targets");
    // "a" is verified, "b" limited: the target is only partly verified.
    EXPECT_EQ(targets[0].at("status").get<std::string>(), "partially_verified");
    EXPECT_EQ(targets[1].at("status").get<std::string>(), "absent");
    EXPECT_EQ(report.at("coverage").at("corpus_gaps").size(), 1u);

    const std::string md = support_table_markdown(report);
    EXPECT_NE(md.find("| Family | Camera"), std::string::npos);
    EXPECT_NE(md.find("`a`, `b`"), std::string::npos);
    EXPECT_NE(md.find("**absent**"), std::string::npos);
    EXPECT_NE(md.find("Scene diversity"), std::string::npos);
    EXPECT_NE(md.find("mk1|sony|ilce-7m4"), std::string::npos);
}

TEST(Report, TheCameraImageAreaTableShowsTheDeclaredAndAppliedAreas) {
    std::vector<OrderedJson> fixtures{fixture_json("a", "verified", "full"),
                                      fixture_json("b", "unsupported", "full"),
                                      fixture_json("c", "limited", "full")};
    const OrderedJson declared = {{"x", 12}, {"y", 8}, {"width", 7008}, {"height", 4672}};
    fixtures[0]["camera_area"] = {{"source", "decoder_inset_crop"},
                                  {"provenance", "decoder"},
                                  {"declared", declared},
                                  {"user_crop", nullptr},
                                  {"basis", "b"},
                                  {"applied", declared},
                                  {"output", {7008, 4672}}};
    fixtures[1]["camera_area"] = fixtures[0]["camera_area"];
    fixtures[1]["camera_area"]["applied"] = nullptr;  // not rendered
    fixtures[1]["camera_area"]["output"] = nullptr;
    // "c" has no camera_area (a report of check suite 1).
    ReportInputs inputs{CorpusPaths{std::filesystem::path(LUCENT_SOURCE_DIR)}, "ids", {"a", "b", "c"}, false};
    const std::string md = support_table_markdown(build_report(fixtures, inputs, {}, Json::object()));
    EXPECT_NE(md.find("## Camera image area"), std::string::npos);
    EXPECT_NE(
        md.find("| `a` | decoder_inset_crop | 7008x4672 at (12, 8) | 7008x4672 at (12, 8) | 7008x4672 |"),
        std::string::npos)
        << md;
    EXPECT_NE(md.find("| `b` | decoder_inset_crop | 7008x4672 at (12, 8) | - | - |"), std::string::npos)
        << md;
    EXPECT_NE(md.find("| `c` | - | - | - | - |"), std::string::npos) << md;
}

TEST(Report, CoverageStatusNeedsEveryMatchingFixtureVerified) {
    EXPECT_EQ(coverage_status({}), "absent");
    EXPECT_EQ(coverage_status({"verified", "verified"}), "verified");
    EXPECT_EQ(coverage_status({"verified", "limited"}), "partially_verified");
    EXPECT_EQ(coverage_status({"limited", "unsupported"}), "covered_not_verified");
}

TEST(Report, StateSeverityOrdersWorstFirst) {
    EXPECT_LT(state_severity("not_qualified"), state_severity("unsupported"));
    EXPECT_LT(state_severity("unsupported"), state_severity("limited"));
    EXPECT_LT(state_severity("limited"), state_severity("compatible_unverified"));
    EXPECT_LT(state_severity("compatible_unverified"), state_severity("verified"));
    EXPECT_EQ(state_severity("anything"), -1);
}

// ---- Committed data ---------------------------------------------------------------------------------

TEST(CommittedData, ExpectationsAreValidAndCoverTheManifestExactly) {
    const CorpusPaths paths{std::filesystem::path(LUCENT_SOURCE_DIR)};
    auto expectations = load_expectations(paths.expectations());
    ASSERT_TRUE(expectations) << expectations.error().describe();
    auto manifest = load_manifest(paths.manifest());
    ASSERT_TRUE(manifest) << manifest.error().describe();
    std::set<std::string> manifest_ids;
    for (const ManifestFixture& f : *manifest) {
        manifest_ids.insert(f.id);
        EXPECT_NE(expectations->find(f.id), nullptr) << "no expectation for " << f.id;
    }
    for (const FixtureExpectation& e : expectations->fixtures) {
        EXPECT_TRUE(manifest_ids.contains(e.id)) << "expectation without a manifest entry: " << e.id;
        // The recorded outcome is consistent with the state rules: a Verified expectation has every
        // fact verified, no limitation and no failed check; a failed check means Unsupported.
        bool any_failed = false;
        for (const auto& [check, status] : e.expected_checks) {
            any_failed = any_failed || status == "fail";
        }
        if (e.expected_state == "verified") {
            EXPECT_TRUE(e.identity.verified() && e.compression.verified() &&
                        e.camera_image_area_fact.verified() && e.orientation.verified() &&
                        e.representation_fact.verified())
                << e.id;
            EXPECT_TRUE(e.expected_limitations.empty()) << e.id;
        }
        if (any_failed) {
            EXPECT_EQ(e.expected_state, "unsupported") << e.id;
        }
        if (e.expected_state == "limited") {
            EXPECT_FALSE(e.expected_limitations.empty()) << e.id;
        }
        EXPECT_EQ(e.mode_key.rfind("mk1|", 0), 0u) << e.id;
        // QUA-07: every rendered fixture pins where its camera area is placed.
        if (e.rendered) {
            EXPECT_TRUE(e.image_area_applied_asserted) << e.id << " does not pin dimensions.image_area_applied";
            if (e.image_area_applied) {
                EXPECT_EQ((*e.image_area_applied)[2], e.rendered->width) << e.id;
                EXPECT_EQ((*e.image_area_applied)[3], e.rendered->height) << e.id;
            }
        }
    }
}

TEST(CommittedData, PublishedReportAgreesWithTheExpectations) {
    // docs/qualification/qualification-report.json (and the support table generated from it) must
    // not contradict the expectations: every fixture's proposed state, mode key and limitations
    // equal the expected ones. Changing an expectation therefore means regenerating the report.
    const CorpusPaths paths{std::filesystem::path(LUCENT_SOURCE_DIR)};
    auto expectations = load_expectations(paths.expectations());
    ASSERT_TRUE(expectations) << expectations.error().describe();
    auto report = read_json_file(paths.root / "docs" / "qualification" / "qualification-report.json");
    ASSERT_TRUE(report) << report.error().describe();
    ASSERT_EQ(report->value("schema", std::string()), kReportSchema);
    ASSERT_EQ(report->value("schema_version", 0), kReportSchemaVersion);
    EXPECT_EQ(report->at("summary").at("expectation_mismatches").get<int>(), 0);
    std::set<std::string> reported;
    for (const Json& f : report->at("fixtures")) {
        const std::string id = f.at("id").get<std::string>();
        reported.insert(id);
        const FixtureExpectation* e = expectations->find(id);
        ASSERT_NE(e, nullptr) << id;
        EXPECT_EQ(f.at("proposed_state").get<std::string>(), e->expected_state) << id;
        ASSERT_TRUE(f.at("mode_key").is_object()) << id;
        EXPECT_EQ(f.at("mode_key").at("text").get<std::string>(), e->mode_key) << id;
        std::vector<std::string> limitations;
        for (const Json& l : f.at("limitations")) {
            limitations.push_back(l.at("id").get<std::string>());
        }
        std::sort(limitations.begin(), limitations.end());
        EXPECT_EQ(limitations, e->expected_limitations) << id;
    }
    EXPECT_EQ(reported.size(), expectations->fixtures.size()) << "the published report must cover every fixture";
}

TEST(CommittedData, FamiliesFileLoads) {
    const CorpusPaths paths{std::filesystem::path(LUCENT_SOURCE_DIR)};
    auto families = load_families(paths.families());
    ASSERT_TRUE(families) << families.error().describe();
    std::set<std::string> ids;
    for (const Family& f : *families) {
        EXPECT_FALSE(f.targets.empty()) << f.id;
        for (const FamilyTarget& t : f.targets) {
            EXPECT_TRUE(ids.insert(t.id).second) << "duplicate target " << t.id;
        }
    }
    // The six blueprint families plus "other brands".
    EXPECT_EQ(families->size(), 7u);
}

// ---- Camera support database export (ADR 0018) --------------------------------------------------

std::string file_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

core::SemanticVersion semver(const char* text) {
    auto parsed = core::SemanticVersion::parse(text);
    EXPECT_TRUE(parsed.has_value()) << text;
    return parsed ? *parsed : core::SemanticVersion{};
}

// A complete-run report with two modes, as build_report() writes it.
OrderedJson export_report() {
    return OrderedJson::parse(R"({
     "schema": "lucent.qualification-report", "schema_version": 1, "generated": "2026-10-08T03:13:58Z",
     "tool": {"name": "lucent-qualify", "lucent_version": "0.1.0", "git_commit": "abc123"},
     "decoder": "LibRaw 0.22.2-Release; decoded-RAW 1.4",
     "versions": {"mode_key": 1, "check_suite": 2, "state_rules": 1, "decoded_raw_contract": "1.4",
                  "render_pipeline": 3, "develop_pipeline": 3, "dng_opcode_processing": 1,
                  "geometry_operation": 2, "image_area_placement": 1,
                  "decoder_capabilities": ["jpeg", "zlib"], "develop_engine": [3, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1],
                  "neutral_look": 1, "export": 1},
     "selection": {"tier": "all", "ids": ["a", "b"], "isolated_processes": true},
     "summary": {"fixtures": 2, "by_state": {}, "expectation_mismatches": 0, "harness_errors": 0},
     "fixtures": [
      {"id": "a", "limitations": []},
      {"id": "b", "limitations": [{"id": "xtrans_baseline_demosaic", "detail": "baseline route"}]}],
     "support_table": [
      {"mode_key": "mk1|sony|ilce-7m4|tiff|uncompressed|7028x4688|14bit|mosaic:RGGB|uint16|single-view|primary:0/1",
       "lookup_keys": ["mk1|sony|ilce-7m4|tiff|uncompressed|7028x4688|14bit|mosaic:RGGB|uint16|single-view|primary:0/1"],
       "lookup_variants": [{"image_area": "decoder_inset_crop 7008x4672", "dng_opcodes": []}],
       "model_key": "sony|ilce-7m4", "family": "sony", "camera": "Sony ILCE-7M4", "product": "Sony A7 IV",
       "mode_label": "ARW uncompressed", "fixtures": ["a"], "proposed_state": "verified",
       "evidence": "13 of 13 checks strong", "limitations": [], "evidence_gaps": []},
      {"mode_key": "mk1|fujifilm|x-t4|raf|fujifilm-raf-lossless-compressed|6246x4170|14bit|mosaic:GGRGGB/GGBGGR/BRGRBG/GGBGGR/GGRGGB/RBGBRG|uint16|single-view|primary:0/1",
       "lookup_keys": ["mk1|fujifilm|x-t4|raf|fujifilm-raf-lossless-compressed|6246x4170|15bit|mosaic:GGRGGB/GGBGGR/BRGRBG/GGBGGR/GGRGGB/RBGBRG|uint16|single-view|primary:0/1"],
       "lookup_variants": [{"image_area": "decoder_inset_crop 6240x4160", "dng_opcodes": []}],
       "model_key": "fujifilm|x-t4", "family": "fujifilm", "camera": "Fujifilm X-T4", "product": "",
       "mode_label": "RAF lossless", "fixtures": ["b"], "proposed_state": "limited",
       "evidence": "13 of 13 checks strong", "limitations": ["xtrans_baseline_demosaic"], "evidence_gaps": []}]
    })");
}

SupportExportOptions export_options() {
    SupportExportOptions options;
    options.asset_version = semver("1.0.0");
    options.minimum_app_version = semver("0.1.0");
    return options;
}

TEST(SupportExport, MapsTheSupportTableToTheDatabase) {
    auto exported = export_camera_support(export_report(), export_options());
    ASSERT_TRUE(exported) << exported.error().describe();
    const assets::CameraSupportDatabase& db = exported->database;
    EXPECT_EQ(db.date, "2026-10-08");
    EXPECT_EQ(db.tool, "lucent-qualify 0.1.0 (abc123)");
    EXPECT_EQ(db.basis.decoder, "LibRaw 0.22.2-Release");
    EXPECT_EQ(db.basis.decoded_raw_contract, "1.4");
    EXPECT_EQ(db.basis.check_suite, 2u);
    EXPECT_EQ(db.summary.modes, 2u);
    EXPECT_EQ(db.summary.verified, 1u);
    EXPECT_EQ(db.summary.limited, 1u);
    ASSERT_EQ(db.entries.size(), 2u);
    const auto fuji = db.lookup(db.entries[0].match);  // sorted: fujifilm first
    ASSERT_TRUE(fuji.has_value());
    EXPECT_EQ(fuji->state, assets::SupportState::Limited);
    // Found under its probe's key (here a provisional 15 bits), recorded with its decoded key.
    EXPECT_NE(fuji->entry->match.find("|15bit|"), std::string::npos);
    EXPECT_NE(fuji->entry->qualified_key.find("|14bit|"), std::string::npos);
    EXPECT_FALSE(db.lookup(fuji->entry->qualified_key).has_value());
    EXPECT_EQ(fuji->entry->camera, "Fujifilm X-T4");  // the product is empty: the camera's name
    ASSERT_EQ(fuji->entry->limitations.size(), 1u);
    EXPECT_EQ(fuji->entry->limitations[0].detail, "baseline route");
    ASSERT_NE(db.limitation("xtrans_baseline_demosaic"), nullptr);
    EXPECT_EQ(db.entries[1].camera, "Sony A7 IV");
    // QUA-08: the probe facts; QUA-02: the whole basis.
    EXPECT_EQ(fuji->entry->image_area, "decoder_inset_crop 6240x4160");
    EXPECT_TRUE(fuji->entry->dng_opcodes.empty());
    EXPECT_EQ(db.basis.decoder_capabilities, (std::vector<std::string>{"jpeg", "zlib"}));
    EXPECT_EQ(db.basis.develop_engine.size(), 12u);
    EXPECT_EQ(db.basis.neutral_look, 1u);
    EXPECT_EQ(db.basis.export_version, 1u);
    // The manifest lists the database with its hash.
    auto manifest = assets::parse_asset_manifest(exported->manifest_text);
    ASSERT_TRUE(manifest) << manifest.error().describe();
    EXPECT_EQ(manifest->id, "camera-support");
    ASSERT_EQ(manifest->files.size(), 1u);
    EXPECT_EQ(manifest->files[0].size, exported->database_text.size());
    // Deterministic.
    auto again = export_camera_support(export_report(), export_options());
    ASSERT_TRUE(again);
    EXPECT_EQ(again->database_text, exported->database_text);
    EXPECT_EQ(again->manifest_text, exported->manifest_text);
}

TEST(SupportExport, RefusesReportsThatCannotBeTheShippedDatabase) {
    const auto refused = [](const OrderedJson& report, const char* why) {
        auto exported = export_camera_support(report, export_options());
        EXPECT_FALSE(exported.has_value()) << why;
        if (!exported) {
            EXPECT_EQ(exported.error().code, ErrorCode::InvalidArgument) << why;
            EXPECT_EQ(exported.error().where, "qualify.export") << why;
        }
    };
    OrderedJson partial = export_report();
    partial["selection"]["tier"] = "smoke";
    refused(partial, "a partial run");
    OrderedJson mismatched = export_report();
    mismatched["summary"]["expectation_mismatches"] = 1;
    refused(mismatched, "a run with mismatches");
    OrderedJson unqualified = export_report();
    unqualified["support_table"][0]["proposed_state"] = "not_qualified";
    refused(unqualified, "a mode the harness could not qualify");
    OrderedJson unknown = export_report();
    unknown["support_table"][1]["limitations"] = OrderedJson::array({"brand_new_code"});
    refused(unknown, "a limitation without wording");
    OrderedJson decoder = export_report();
    decoder["decoder"] = "LibRaw 0.22.2-Release; decoded-RAW 1.3";
    refused(decoder, "a decoder string that disagrees with the versions");
    OrderedJson no_probe = export_report();
    no_probe["support_table"][0]["lookup_keys"] = OrderedJson::array();
    refused(no_probe, "a mode without a probe key");
    OrderedJson shared = export_report();
    shared["support_table"][1]["lookup_keys"] = shared["support_table"][0]["lookup_keys"];
    refused(shared, "two modes under one probe key");
    OrderedJson two_variants = export_report();
    two_variants["support_table"][0]["lookup_variants"].push_back(
        OrderedJson{{"image_area", "unknown"}, {"dng_opcodes", OrderedJson::array()}});
    refused(two_variants, "a mode whose files differ in their image area");
    OrderedJson no_engine = export_report();
    no_engine["versions"].erase("develop_engine");
    refused(no_engine, "a report without the develop-engine versions");
    refused(OrderedJson::parse(R"({"schema": "something-else"})"), "another document");
}

// QUA-06: a published database names the commit that produced its evidence.
TEST(SupportExport, RefusesReportsOfUncommittedBuildsUnlessAllowed) {
    OrderedJson report = export_report();
    EXPECT_TRUE(check_report_provenance(report, false)) << "abc123 is a clean commit";
    for (const char* commit : {"abc123-dirty", "unknown", "abc123-unknown-state", ""}) {
        report["tool"]["git_commit"] = commit;
        const Status checked = check_report_provenance(report, false);
        ASSERT_FALSE(checked) << commit;
        EXPECT_EQ(checked.error().where, "qualify.export.provenance");
        EXPECT_TRUE(check_report_provenance(report, true)) << commit;
    }
}

// AST-02: an asset version names exactly one content.
TEST(SupportExport, RefusesNewContentUnderAnExistingVersion) {
    auto exported = export_camera_support(export_report(), export_options());
    ASSERT_TRUE(exported) << exported.error().describe();
    EXPECT_TRUE(check_version_change("", "", *exported)) << "a new folder";
    EXPECT_TRUE(check_version_change(exported->manifest_text, exported->database_text, *exported))
        << "the same content again";
    OrderedJson changed_report = export_report();
    changed_report["generated"] = "2026-11-02T10:00:00Z";
    auto changed = export_camera_support(changed_report, export_options());
    ASSERT_TRUE(changed);
    const Status same_version = check_version_change(exported->manifest_text, exported->database_text, *changed);
    ASSERT_FALSE(same_version);
    EXPECT_EQ(same_version.error().where, "qualify.export.version");
    SupportExportOptions next = export_options();
    next.asset_version = semver("1.1.0");
    auto bumped = export_camera_support(changed_report, next);
    ASSERT_TRUE(bumped);
    EXPECT_TRUE(check_version_change(exported->manifest_text, exported->database_text, *bumped));
}

TEST(SupportExport, EveryLimitationCodeHasWording) {
    for (const std::string_view code : limitation_ids()) {
        const auto text = limitation_text(code);
        ASSERT_TRUE(text.has_value()) << code;
        EXPECT_FALSE(text->summary.empty());
        EXPECT_FALSE(text->meaning.empty());
    }
    EXPECT_FALSE(limitation_text("not_a_code").has_value());
}

TEST(CommittedData, TheCameraSupportAssetIsTheExportOfTheReport) {
    const std::filesystem::path root(LUCENT_SOURCE_DIR);
    auto report = read_json_file(root / "docs" / "qualification" / "qualification-report.json");
    ASSERT_TRUE(report) << report.error().describe();
    const std::filesystem::path folder = root / "assets" / "camera-support";
    const std::string manifest_text = file_text(folder / "manifest.json");
    auto manifest = assets::parse_asset_manifest(manifest_text);
    ASSERT_TRUE(manifest) << manifest.error().describe();
    SupportExportOptions options;
    options.asset_version = manifest->version;
    options.minimum_app_version = manifest->minimum_app_version;
    auto exported = export_camera_support(OrderedJson::parse(report->dump()), options);
    ASSERT_TRUE(exported) << exported.error().describe();
    EXPECT_EQ(exported->database_text, file_text(folder / "camera-support.json"))
        << "assets/camera-support/camera-support.json is not the export of the committed report; re-export "
           "it "
           "(docs/qualification/README.md)";
    EXPECT_EQ(exported->manifest_text, manifest_text);
    // The database's states are the report's, found under each mode's probe key.
    for (const Json& row : report->at("support_table")) {
        ASSERT_EQ(row.at("lookup_keys").size(), 1u) << row.at("mode_key");
        const auto match = exported->database.lookup(row.at("lookup_keys").at(0).get<std::string>());
        ASSERT_TRUE(match.has_value());
        EXPECT_EQ(match->entry->qualified_key, row.at("mode_key").get<std::string>());
        EXPECT_EQ(std::string(assets::to_string(match->state)), row.at("proposed_state").get<std::string>());
    }
}

}  // namespace
}  // namespace lucent::qualify
