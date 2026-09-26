// Fixture-based integration tests of the RAW decoder boundary (label "fixtures").
//
// For every fixture of fixtures/manifest.json whose file is cached (tools\fetch-fixtures.ps1),
// the file is decoded through the default decoder registry and
//   * the parity block Lucent reports is compared with the committed rawpy reference
//     (fixtures/reference/<id>.parity.json): integers, strings and CFA patterns exactly, floats
//     with |a - b| <= max(1e-6 * max(|a|, |b|), 1e-9), and the SHA-256 of the raw buffer;
//   * the decoded data is checked against contract invariants and against the reference's
//     independent view of geometry and CFA layout.
// Fixtures that are not downloaded are skipped with a message. The reference decoder is rawpy
// 0.27.1 (LibRaw 0.22.1); Lucent links LibRaw 0.22.2. A field that legitimately differs between
// the two builds gets a reviewed, per-fixture, per-field entry in kAllowances; blanket skips are
// not allowed. No allowance is needed today.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <lucent/core/file.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/image/cfa.hpp>
#include <lucent/image/geometry.hpp>
#include <lucent/raw/decoded_raw.hpp>
#include <lucent/raw/decoder_registry.hpp>

#include "fixture_manifest.hpp"
#include "probe_report.hpp"

namespace lucent::integration {
namespace {

using Json = nlohmann::json;

// A reviewed difference between Lucent's decoder build and the reference.
struct Allowance {
    std::string_view fixture;  // fixture id
    std::string_view field;    // parity field
    double rel_tol;            // relative tolerance replacing 1e-6 (float fields only)
    std::string_view reason;   // what differs, why it is correct, who reviewed it
};

// Empty: all 34 fixtures match the reference field for field (docs/adr/0005-raw-decoder-boundary.md).
constexpr std::array<Allowance, 0> kAllowances{};

constexpr double kRelTol = 1e-6;
constexpr double kAbsTol = 1e-9;

// ---- Parity comparison (the rules of research/tools/compare_probe.py) ------------------------

void compare(const Json& a, const Json& b, const std::string& path, double rel_tol,
             std::vector<std::string>& problems) {
    if (a.is_null() || b.is_null()) {
        if (!(a.is_null() && b.is_null())) {
            problems.push_back(path + ": " + a.dump() + " vs " + b.dump());
        }
        return;
    }
    if (a.is_array() && b.is_array()) {
        if (a.size() != b.size()) {
            problems.push_back(path + ": length " + std::to_string(a.size()) + " vs " +
                               std::to_string(b.size()));
            return;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            compare(a[i], b[i], path + "[" + std::to_string(i) + "]", rel_tol, problems);
        }
        return;
    }
    if (a.is_number() && b.is_number()) {
        if (a.is_number_integer() && b.is_number_integer()) {
            if (a.get<std::int64_t>() != b.get<std::int64_t>()) {
                problems.push_back(path + ": " + a.dump() + " vs " + b.dump());
            }
            return;
        }
        const double x = a.get<double>();
        const double y = b.get<double>();
        if (std::abs(x - y) > std::max(rel_tol * std::max(std::abs(x), std::abs(y)), kAbsTol)) {
            problems.push_back(path + ": " + a.dump() + " vs " + b.dump());
        }
        return;
    }
    if (a.is_string() && b.is_string()) {
        if (a != b) {
            problems.push_back(path + ": " + a.dump() + " vs " + b.dump());
        }
        return;
    }
    problems.push_back(path + ": type mismatch " + a.dump() + " vs " + b.dump());
}

std::vector<std::string> compare_parity(const Json& lucent, const Json& reference, std::string_view fixture,
                                        std::set<std::string_view>& used_allowances) {
    std::vector<std::string> problems;
    std::set<std::string> fields;
    for (const auto& [key, value] : lucent.items()) {
        fields.insert(key);
    }
    for (const auto& [key, value] : reference.items()) {
        fields.insert(key);
    }
    for (const std::string& field : fields) {
        const bool in_lucent = lucent.contains(field);
        const bool in_reference = reference.contains(field);
        if (!in_lucent || !in_reference) {
            problems.push_back(field + ": missing from " + (in_lucent ? "the reference" : "Lucent's report"));
            continue;
        }
        double rel_tol = kRelTol;
        for (const Allowance& allowance : kAllowances) {
            if (allowance.fixture == fixture && allowance.field == field) {
                rel_tol = allowance.rel_tol;
                used_allowances.insert(allowance.field);
            }
        }
        compare(lucent[field], reference[field], field, rel_tol, problems);
    }
    return problems;
}

std::string join(const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& line : lines) {
        text += "\n  " + line;
    }
    return text;
}

// ---- Contract invariants ---------------------------------------------------------------------

std::int32_t channel_index(const raw::RepresentationInfo& rep, image::CfaColor color) {
    const auto it = std::find(rep.channel_colors.begin(), rep.channel_colors.end(), color);
    return it == rep.channel_colors.end() ? -1 : static_cast<std::int32_t>(it - rep.channel_colors.begin());
}

void expect_invariants(const raw::DecodedRaw& decoded, const Json& reference) {
    const raw::RawMetadata& m = decoded.metadata;
    const raw::SensorGeometry& g = m.geometry;
    const raw::RepresentationInfo& rep = m.representation;

    const Status consistent = raw::check_consistency(decoded);
    EXPECT_TRUE(consistent) << (consistent ? "" : consistent.error().describe());

    // Geometry: visible area inside the raw frame and equal to the reference's view.
    const image::PixelRect frame{0, 0, static_cast<std::int32_t>(g.raw_width),
                                 static_cast<std::int32_t>(g.raw_height)};
    EXPECT_TRUE(frame.contains(g.visible_area));
    EXPECT_EQ(g.raw_width, reference["raw_width"].get<std::uint32_t>());
    EXPECT_EQ(g.raw_height, reference["raw_height"].get<std::uint32_t>());
    EXPECT_EQ(g.visible_area.x, reference["left_margin"].get<std::int32_t>());
    EXPECT_EQ(g.visible_area.y, reference["top_margin"].get<std::int32_t>());
    EXPECT_EQ(g.visible_area.width, reference["width"].get<std::int32_t>());
    EXPECT_EQ(g.visible_area.height, reference["height"].get<std::int32_t>());
    EXPECT_EQ(image::to_libraw_flip(g.orientation), reference["flip"].get<int>());

    // Buffer size equals the raw frame (margins included) with the representation's channels.
    const raw::PixelsShape shape = raw::shape_of(raw::pixels_of(decoded.image));
    EXPECT_EQ(shape.width, static_cast<std::int32_t>(g.raw_width));
    EXPECT_EQ(shape.height, static_cast<std::int32_t>(g.raw_height));
    EXPECT_EQ(shape.channels, rep.kind == raw::RawRepresentation::LinearColor ? 3 : 1);

    // Levels: every site's black is below the white level, which is LibRaw's maximum.
    double max_black =
        m.levels.black + *std::max_element(m.levels.channel_black.begin(), m.levels.channel_black.end());
    if (!m.levels.black_pattern.empty()) {
        max_black += *std::max_element(m.levels.black_pattern.begin(), m.levels.black_pattern.end());
    }
    EXPECT_LT(max_black, m.levels.white);
    if (rep.sample_encoding == raw::SampleEncoding::UInt16) {
        EXPECT_EQ(m.levels.white, reference["white_level"].get<double>());
    }

    EXPECT_EQ(rep.color_count, reference["num_colors"].get<int>());
    const Json& pattern = reference["raw_pattern"];
    switch (rep.kind) {
        case raw::RawRepresentation::Mosaic: {
            ASSERT_FALSE(rep.cfa.empty());
            // The CFA is consistent with num_colors: its distinct colours, counting the second
            // green as green, are exactly the decoder's colours.
            std::set<image::CfaColor> colors;
            for (std::int32_t r = 0; r < rep.cfa.height(); ++r) {
                for (std::int32_t c = 0; c < rep.cfa.width(); ++c) {
                    const image::CfaColor color = rep.cfa.color_at(r, c);
                    colors.insert(color == image::CfaColor::Green2 ? image::CfaColor::Green : color);
                }
            }
            EXPECT_EQ(static_cast<int>(colors.size()), rep.color_count);
            // The raw-origin CFA reproduces rawpy's raw_pattern, which LibRaw's COLOR() computes
            // independently of Lucent's CfaPattern.
            ASSERT_TRUE(pattern.is_array());
            for (std::size_t y = 0; y < pattern.size(); ++y) {
                for (std::size_t x = 0; x < pattern[y].size(); ++x) {
                    const image::CfaColor color =
                        rep.cfa.color_at(static_cast<std::int32_t>(y), static_cast<std::int32_t>(x));
                    EXPECT_EQ(channel_index(rep, color), pattern[y][x].get<int>())
                        << "raw_pattern[" << y << "][" << x << "]";
                }
            }
            break;
        }
        case raw::RawRepresentation::Monochrome:
            EXPECT_EQ(pattern, Json::parse("[[6]]"));
            break;
        case raw::RawRepresentation::LinearColor:
            EXPECT_TRUE(pattern.is_null());
            EXPECT_TRUE(m.has_prior_processing(raw::PriorProcessingKind::Demosaiced));
            break;
    }
    const bool flat = rep.kind != raw::RawRepresentation::LinearColor;
    EXPECT_EQ(reference["raw_type"].get<std::string>(), flat ? "flat" : "stack");
    EXPECT_NE(m.interpretation.status, raw::InterpretationStatus::NotDecodable);
}

// ---- Tests -----------------------------------------------------------------------------------

class RawFixture : public ::testing::TestWithParam<Fixture> {};

TEST_P(RawFixture, DecodesLikeTheRawpyReference) {
    const Fixture& fixture = GetParam();
    const std::filesystem::path file = cached_file(fixture);
    if (!std::filesystem::exists(file)) {
        GTEST_SKIP() << "fixture '" << fixture.id
                     << "' is not downloaded; run tools\\fetch-fixtures.ps1 -Tier " << fixture.tier;
    }
    const Json reference = read_json(reference_file(fixture));
    ASSERT_EQ(reference.value("schema", std::string()), "lucent.probe-report");
    ASSERT_EQ(reference.value("schema_version", 0), 1);

    // Same bytes as the reference was made from.
    ASSERT_EQ(std::filesystem::file_size(file), reference["file_size"].get<std::uint64_t>());
    auto hashed = core::hash_file(file, {});
    ASSERT_TRUE(hashed) << hashed.error().describe();
    ASSERT_EQ(core::to_hex(hashed->digest), reference["file_sha256"].get<std::string>())
        << "the cached file differs from the reference; re-run tools\\fetch-fixtures.ps1";

    auto registry = raw::create_default_registry();
    ASSERT_TRUE(registry) << registry.error().describe();
    raw::DecodeOptions options;
    options.subimage = 0;  // the reference decodes shot_select 0
    auto decoded = registry->decode(file, options, {});

    if (reference["parity"].is_null()) {
        // The reference decoder refused the file; Lucent must refuse it too, as Unsupported.
        ASSERT_FALSE(decoded) << "the reference refused this file but Lucent decoded it";
        EXPECT_EQ(decoded.error().code, ErrorCode::Unsupported) << decoded.error().describe();
        EXPECT_EQ(reference["status"].get<std::string>(), "error");
        return;
    }
    ASSERT_TRUE(decoded) << decoded.error().describe();
    const Json& reference_parity = reference["parity"];
    expect_invariants(*decoded, reference_parity);

    ASSERT_TRUE(decoded->metadata.decoder.reference_parity);
    const raw::ReferenceParity& parity = *decoded->metadata.decoder.reference_parity;
    auto raw_hash = probe::raw_data_sha256(*decoded);
    ASSERT_TRUE(raw_hash) << raw_hash.error().describe();
    const Json lucent =
        Json::parse(probe::parity_json(parity, parity.raw_type == "flat" ? *raw_hash : std::nullopt).dump());
    std::set<std::string_view> used;
    const std::vector<std::string> problems = compare_parity(lucent, reference_parity, fixture.id, used);
    EXPECT_TRUE(problems.empty()) << fixture.id << " differs from the rawpy reference:" << join(problems);
    if (reference_parity.contains("raw_data_sha256")) {
        EXPECT_TRUE(lucent.contains("raw_data_sha256")) << "the raw buffer hash was not compared";
    }
}

INSTANTIATE_TEST_SUITE_P(Manifest, RawFixture, ::testing::ValuesIn(load_manifest()),
                         [](const ::testing::TestParamInfo<Fixture>& info) { return test_name(info.param); });

TEST(FixtureReferences, CoverTheManifestExactly) {
    std::set<std::string> expected;
    for (const Fixture& fixture : load_manifest()) {
        expected.insert(fixture.id + ".parity.json");
        EXPECT_TRUE(std::filesystem::exists(reference_file(fixture))) << "no reference for " << fixture.id;
    }
    // Subdirectories hold other reference kinds (reference/demosaic: Malvar-He-Cutler crops of
    // research/tools/demosaic_reference.py); only the parity files map one-to-one to the manifest.
    for (const auto& entry : std::filesystem::directory_iterator(fixture_root() / "reference")) {
        if (entry.is_directory()) {
            continue;
        }
        EXPECT_TRUE(expected.contains(entry.path().filename().string()))
            << "reference without a manifest entry: " << entry.path().filename().string();
    }
    for (const Allowance& allowance : kAllowances) {
        EXPECT_FALSE(allowance.reason.empty()) << allowance.fixture << "/" << allowance.field;
    }
}

TEST(ProbeReport, FollowsSchemaVersion1) {
    const std::vector<Fixture> fixtures = load_manifest();
    const auto smoke = std::find_if(fixtures.begin(), fixtures.end(), [](const Fixture& f) {
        return f.tier == "smoke" && std::filesystem::exists(cached_file(f));
    });
    if (smoke == fixtures.end()) {
        GTEST_SKIP() << "no smoke fixture is downloaded; run tools\\fetch-fixtures.ps1";
    }
    auto registry = raw::create_default_registry();
    ASSERT_TRUE(registry);
    probe::ReportOptions options;
    options.decode = true;
    options.subimage = 0;
    const probe::Report report = probe::build_report(*registry, cached_file(*smoke), options, {});
    ASSERT_TRUE(report.ok) << report.document.dump(2);
    const Json doc = Json::parse(report.document.dump());
    EXPECT_EQ(doc["schema"], "lucent.probe-report");
    EXPECT_EQ(doc["schema_version"], 1);
    EXPECT_EQ(doc["file_name"], smoke->file_name);
    EXPECT_EQ(doc["file_size"].get<std::uint64_t>(), std::filesystem::file_size(cached_file(*smoke)));
    EXPECT_EQ(doc["file_sha256"].get<std::string>().size(), 64u);
    EXPECT_EQ(doc["generator"]["name"], "lucent-probe");
    EXPECT_EQ(doc["generator"]["contract"], raw::kDecodedRawContractVersion.to_string());
    EXPECT_EQ(doc["status"], "ok");
    EXPECT_TRUE(doc["error"].is_null());
    EXPECT_EQ(doc["parity_stage"], "after_unpack");
    EXPECT_EQ(doc["metadata"]["stage"], "after_unpack");
    EXPECT_EQ(doc["metadata"]["decoder"]["adapter_id"], "libraw");
    EXPECT_TRUE(doc["subimages"].is_array());
    // Contract 1.1 fields.
    const Json& compression = doc["metadata"]["format"]["compression"];
    EXPECT_FALSE(compression["name"].get<std::string>().empty());
    EXPECT_TRUE(compression["lossiness"].is_string());
    EXPECT_TRUE(compression["provenance"].is_string());
    EXPECT_TRUE(compression["basis"].is_string());
    EXPECT_TRUE(doc["metadata"]["subimage"]["role"].is_string());
    EXPECT_TRUE(doc["metadata"]["projection"]["layout"].is_string());

    const Json reference = read_json(reference_file(*smoke));
    std::set<std::string_view> used;
    const auto problems = compare_parity(doc["parity"], reference["parity"], smoke->id, used);
    EXPECT_TRUE(problems.empty()) << join(problems);

    // A probe-only report takes parity values after opening and has no raw buffer hash.
    options.decode = false;
    const Json probe_only =
        Json::parse(probe::build_report(*registry, cached_file(*smoke), options, {}).document.dump());
    EXPECT_EQ(probe_only["parity_stage"], "after_open");
    EXPECT_FALSE(probe_only["parity"].contains("raw_data_sha256"));
    EXPECT_EQ(probe_only["metadata"]["stage"], "after_open");
}

// A decoder that reports another version of the file than the one the report hashed, as if the
// file had been replaced between hashing and probing (or decoding). Review finding ROBU-5.
class ChangedFileDecoder final : public raw::RawDecoder {
public:
    explicit ChangedFileDecoder(bool changes_before_decode) : changes_before_decode_(changes_before_decode) {}

    [[nodiscard]] std::string_view id() const noexcept override { return "changed-file"; }

    [[nodiscard]] Expected<raw::ProbeResult> probe(const std::filesystem::path& path,
                                                   const raw::ProbeOptions&,
                                                   core::CancellationToken) const override {
        auto identity = core::stat_file(path);
        if (!identity) {
            return Unexpected(std::move(identity.error()));
        }
        raw::ProbeResult result;
        result.metadata.source.path = path;
        result.metadata.source.identity = *identity;
        if (!changes_before_decode_) {
            result.metadata.source.identity.last_write_time += 1;  // changed before the probe
        }
        return result;
    }

    [[nodiscard]] Expected<raw::DecodedRaw> decode(const std::filesystem::path& path,
                                                   const raw::DecodeOptions&,
                                                   core::CancellationToken) const override {
        auto identity = core::stat_file(path);
        if (!identity) {
            return Unexpected(std::move(identity.error()));
        }
        raw::DecodedRaw decoded;
        decoded.metadata.stage = raw::MetadataStage::AfterUnpack;
        decoded.metadata.source.identity = *identity;
        decoded.metadata.source.identity.size_bytes += 1;  // a different version was decoded
        return decoded;
    }

private:
    bool changes_before_decode_;
};

TEST(ProbeReport, EveryStepMustSeeTheHashedFileVersion) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "lucent-probe-report-identity-test.bin";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "any bytes: the fake decoder does not read them";
    }
    for (const bool changes_before_decode : {false, true}) {
        SCOPED_TRACE(changes_before_decode ? "changed before decoding" : "changed before probing");
        raw::DecoderRegistry registry;
        ASSERT_TRUE(registry.add(std::make_unique<ChangedFileDecoder>(changes_before_decode), 0));
        probe::ReportOptions options;
        options.decode = true;
        const probe::Report report = probe::build_report(registry, path, options, {});
        EXPECT_FALSE(report.ok);
        const Json doc = Json::parse(report.document.dump());
        EXPECT_EQ(doc["status"], "error");
        EXPECT_EQ(doc["error"]["code"], "source_changed") << doc.dump(2);
        EXPECT_EQ(doc["error"]["where"], "probe.report");
        EXPECT_TRUE(doc["parity"].is_null());
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ProbeReport, FailuresAreReportedNotThrown) {
    auto registry = raw::create_default_registry();
    ASSERT_TRUE(registry);
    const probe::Report report =
        probe::build_report(*registry, fixture_root() / "cache" / "no-such-fixture" / "missing.dng", {}, {});
    EXPECT_FALSE(report.ok);
    const Json doc = Json::parse(report.document.dump());
    EXPECT_EQ(doc["status"], "error");
    EXPECT_EQ(doc["error"]["code"], "not_found");
    EXPECT_FALSE(doc["error"]["where"].get<std::string>().empty());
    EXPECT_TRUE(doc["parity"].is_null());
    EXPECT_TRUE(doc["file_size"].is_null());
    EXPECT_FALSE(doc.contains("file_sha256"));
}

}  // namespace
}  // namespace lucent::integration
