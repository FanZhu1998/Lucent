// Recipe documents (recipe.hpp): RFC 8785 canonical form, exact round trips, strict reading of
// version-1 documents and VersionMismatch for newer ones, paste categories.

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <lucent/application/recipe.hpp>
#include <lucent/core/hash.hpp>

namespace lucent::app {
namespace {

using Json = nlohmann::json;

// The canonical text of default ("as shot") settings: every member, sorted, no whitespace.
constexpr std::string_view kDefaultDocument =
    R"({"base_look":{"id":"neutral","version":1},"contrast":0,"exposure_ev":0,"highlights":0,"locks":[],)"
    R"("operations":{"color":1,"contrast":1,"exposure":1,"local_tone":1,"tone_curve":1,"white_balance":1},)"
    R"("saturation":0,"schema":"lucent.develop","shadows":0,"tone_curve":[[0,0],[1,1]],"version":1,)"
    R"("vibrance":0,"white_balance":{"mode":"as_shot"}})";

engine::DevelopSettings edited() {
    engine::DevelopSettings s;
    s.white_balance = engine::KelvinWhiteBalance{4321.5, -12.25};
    s.exposure_ev = 0.1;
    s.contrast = 1.0 / 3.0;
    s.highlights = -0.7;
    s.shadows = 0.45;
    s.tone_curve.points = {{0.0, 0.02}, {0.25, 0.2}, {0.5, 0.55}, {0.75, 0.8}, {1.0, 0.97}};
    s.saturation = -0.125;
    s.vibrance = 0.3;
    s.locks = {engine::DevelopControl::Exposure, engine::DevelopControl::WhiteBalance};
    return s;
}

Json canonical_json(const engine::DevelopSettings& settings) {
    auto document = write_recipe(settings);
    EXPECT_TRUE(document) << (document ? std::string() : document.error().describe());
    return Json::parse(document ? document->json : std::string("{}"));
}

ErrorCode read_error(const Json& doc) {
    auto read = read_recipe(doc.dump());
    EXPECT_FALSE(read) << "accepted: " << doc.dump();
    return read ? ErrorCode::Unknown : read.error().code;
}

TEST(RecipeDocument, NumbersUseTheEcmaScriptShortestForm) {
    const std::vector<std::pair<double, std::string>> cases = {
        {0.0, "0"},
        {-0.0, "0"},
        {1.0, "1"},
        {-2.5, "-2.5"},
        {0.1, "0.1"},
        {0.30000000000000004, "0.30000000000000004"},
        {1.0 / 3.0, "0.3333333333333333"},
        {123.456, "123.456"},
        {1e20, "100000000000000000000"},
        {1e21, "1e+21"},
        {1.5e300, "1.5e+300"},
        {1e-6, "0.000001"},
        {1e-7, "1e-7"},
        {-1.25e-9, "-1.25e-9"},
        {5e-324, "5e-324"},
        {std::numeric_limits<double>::max(), "1.7976931348623157e+308"},
        {4321.5, "4321.5"},
    };
    for (const auto& [value, expected] : cases) {
        auto text = canonical_number(value);
        ASSERT_TRUE(text);
        EXPECT_EQ(*text, expected) << value;
        // The text reads back as exactly the same double.
        EXPECT_EQ(std::strtod(text->c_str(), nullptr), value == 0.0 ? 0.0 : value);
    }
    EXPECT_FALSE(canonical_number(std::numeric_limits<double>::quiet_NaN()));
    EXPECT_FALSE(canonical_number(std::numeric_limits<double>::infinity()));
}

TEST(RecipeDocument, DefaultSettingsHaveAFixedCanonicalTextAndIdentity) {
    auto document = write_recipe(engine::DevelopSettings{});
    ASSERT_TRUE(document) << document.error().describe();
    EXPECT_EQ(document->json, kDefaultDocument);
    auto digest = core::sha256(std::as_bytes(std::span(kDefaultDocument.data(), kDefaultDocument.size())));
    ASSERT_TRUE(digest);
    EXPECT_EQ(document->sha256, *digest);
    // Pinned: any change of the canonical form changes every recipe identity.
    EXPECT_EQ(core::to_hex(document->sha256),
              "671dbaa8e8ff33f52cc5b73a20d585c96da4ad6e04317c9de5500a66649f1a7f");
}

TEST(RecipeDocument, RoundTripsEveryFieldExactly) {
    std::vector<engine::DevelopSettings> cases = {engine::DevelopSettings{}, edited()};
    engine::DevelopSettings neutral = edited();
    neutral.white_balance = engine::CameraNeutralWhiteBalance{2.0123456789, 1.0, 1.4999999999999998};
    neutral.locks.clear();
    cases.push_back(neutral);
    for (const engine::DevelopSettings& settings : cases) {
        auto document = write_recipe(settings);
        ASSERT_TRUE(document) << document.error().describe();
        auto read = read_recipe(document->json);
        ASSERT_TRUE(read) << read.error().describe() << "\n" << document->json;
        EXPECT_EQ(*read, settings);
        auto again = write_recipe(*read);
        ASSERT_TRUE(again);
        EXPECT_EQ(again->json, document->json);
        EXPECT_EQ(again->sha256, document->sha256);
    }
}

TEST(RecipeDocument, RandomSettingsRoundTripBitExactly) {
    std::mt19937_64 random(20261003);  // fixed seed: deterministic
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    for (int i = 0; i < 300; ++i) {
        engine::DevelopSettings s;
        s.exposure_ev = 5.0 * unit(random);
        s.contrast = unit(random);
        s.highlights = unit(random);
        s.shadows = unit(random);
        s.saturation = unit(random);
        s.vibrance = unit(random);
        if (i % 3 == 1) {
            s.white_balance = engine::KelvinWhiteBalance{2000.0 + 48000.0 * (unit(random) + 1.0) / 2.0,
                                                         150.0 * unit(random)};
        } else if (i % 3 == 2) {
            s.white_balance = engine::CameraNeutralWhiteBalance{1.0 + std::abs(unit(random)), 1.0,
                                                                1.0 + std::abs(unit(random))};
        }
        s.tone_curve.points = {{0.0, 0.0}, {0.5, 0.5 + 0.4 * unit(random)}, {1.0, 1.0}};
        auto document = write_recipe(s);
        ASSERT_TRUE(document) << document.error().describe();
        auto read = read_recipe(document->json);
        ASSERT_TRUE(read) << read.error().describe();
        ASSERT_EQ(*read, s) << document->json;
    }
}

TEST(RecipeDocument, AcceptsAnyJsonNumberSpellingAndOrder) {
    // Whitespace, member order and number spelling are free on input; the identity is that of the
    // canonical re-serialisation.
    const std::string text = R"({ "version": 1, "schema": "lucent.develop",
        "white_balance": {"tint": 0.0, "temperature_k": 5.5e3, "mode": "kelvin"},
        "exposure_ev": 1.0, "contrast": 0, "highlights": -0.0, "shadows": 25e-2,
        "tone_curve": [[0, 0], [1.0, 1]], "saturation": 0, "vibrance": 0,
        "base_look": {"version": 1, "id": "neutral"}, "locks": ["exposure"],
        "operations": {"white_balance": 1, "exposure": 1, "local_tone": 1, "contrast": 1,
                       "tone_curve": 1, "color": 1}})";
    auto read = read_recipe(text);
    ASSERT_TRUE(read) << read.error().describe();
    engine::DevelopSettings expected;
    expected.white_balance = engine::KelvinWhiteBalance{5500.0, 0.0};
    expected.exposure_ev = 1.0;
    expected.shadows = 0.25;
    expected.locks = {engine::DevelopControl::Exposure};
    EXPECT_EQ(*read, expected);
    EXPECT_EQ(write_recipe(*read)->sha256, write_recipe(expected)->sha256);
}

TEST(RecipeDocument, RejectsUnknownMissingAndMistypedMembers) {
    const Json base = canonical_json(edited());
    {
        Json doc = base;
        doc["sharpening"] = 0.5;  // a field this version does not define
        EXPECT_EQ(read_error(doc), ErrorCode::InvalidArgument);
    }
    {
        Json doc = base;
        doc["white_balance"]["green_shift"] = 1;
        EXPECT_EQ(read_error(doc), ErrorCode::InvalidArgument);
    }
    {
        Json doc = base;
        doc["base_look"]["strength"] = 1;
        EXPECT_EQ(read_error(doc), ErrorCode::InvalidArgument);
    }
    {
        Json doc = base;
        doc["operations"]["sharpen"] = 1;  // an operation unknown to version 1: never a no-op
        EXPECT_EQ(read_error(doc), ErrorCode::InvalidArgument);
    }
    {
        Json doc = base;
        doc["operations"].erase("color");
        EXPECT_EQ(read_error(doc), ErrorCode::InvalidArgument);
    }
    {
        Json doc = base;
        doc.erase("contrast");
        EXPECT_EQ(read_error(doc), ErrorCode::InvalidArgument);
    }
    {
        Json doc = base;
        doc["exposure_ev"] = "0.5";
        EXPECT_EQ(read_error(doc), ErrorCode::InvalidArgument);
    }
    {
        Json doc = base;
        doc["version"] = 1.0;  // versions are unsigned integers
        EXPECT_EQ(read_error(doc), ErrorCode::InvalidArgument);
    }
    {
        Json doc = base;
        doc["tone_curve"][1] = Json::array({0.25, 0.2, 0.0});
        EXPECT_EQ(read_error(doc), ErrorCode::InvalidArgument);
    }
    {
        Json doc = base;
        doc["locks"] = Json::array({"exposure", "focus"});
        EXPECT_EQ(read_error(doc), ErrorCode::InvalidArgument);
    }
    {
        Json doc = base;
        doc["locks"] = Json::array({"exposure", "exposure"});
        EXPECT_EQ(read_error(doc), ErrorCode::InvalidArgument);
    }
    {
        Json doc = base;
        doc["white_balance"] = Json{{"mode", "auto"}};  // no such mode in version 1
        EXPECT_EQ(read_error(doc), ErrorCode::InvalidArgument);
    }
}

TEST(RecipeDocument, RejectsRepeatedMembersAndNonRecipes) {
    auto document = write_recipe(engine::DevelopSettings{});
    ASSERT_TRUE(document);
    std::string repeated = document->json;
    repeated.insert(1, R"("contrast":0.5,)");  // "contrast" twice
    auto read = read_recipe(repeated);
    ASSERT_FALSE(read);
    EXPECT_EQ(read.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(read.error().where, "app.recipe");

    EXPECT_EQ(read_recipe("not json").error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(read_recipe("[1, 2]").error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(read_recipe(R"({"schema":"lucent.other","version":1})").error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(read_recipe(R"({"schema":"lucent.develop","version":0})").error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(read_recipe(R"({"schema":"lucent.develop"})").error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(read_recipe(document->json + "x").error().code, ErrorCode::InvalidArgument);
}

TEST(RecipeDocument, NewerVersionsAreVersionMismatchNeverNoOps) {
    const Json base = canonical_json(edited());
    {
        // A newer schema is not interpreted at all, whatever it contains.
        Json doc = base;
        doc["version"] = 2;
        doc["masks"] = Json::array({Json{{"kind", "brush"}}});
        EXPECT_EQ(read_error(doc), ErrorCode::VersionMismatch);
    }
    {
        Json doc = base;
        doc["operations"]["exposure"] = 2;
        EXPECT_EQ(read_error(doc), ErrorCode::VersionMismatch);
    }
    {
        Json doc = base;
        doc["operations"]["exposure"] = 0;
        EXPECT_EQ(read_error(doc), ErrorCode::InvalidArgument);
    }
    {
        Json doc = base;
        doc["base_look"]["version"] = 2;
        EXPECT_EQ(read_error(doc), ErrorCode::VersionMismatch);
    }
    {
        Json doc = base;
        doc["base_look"]["id"] = "vivid";
        EXPECT_EQ(read_error(doc), ErrorCode::VersionMismatch);
    }
}

TEST(RecipeDocument, RejectsValuesOutsideTheirRanges) {
    const Json base = canonical_json(engine::DevelopSettings{});
    {
        Json doc = base;
        doc["exposure_ev"] = 5.5;
        EXPECT_EQ(read_error(doc), ErrorCode::InvalidArgument);
    }
    {
        Json doc = base;
        doc["tone_curve"] = Json::array({Json::array({0, 0.5}), Json::array({1, 0.25})});  // decreasing
        EXPECT_EQ(read_error(doc), ErrorCode::InvalidArgument);
    }
    {
        Json doc = base;
        doc["white_balance"] = Json{{"mode", "kelvin"}, {"temperature_k", 1000}, {"tint", 0}};
        EXPECT_EQ(read_error(doc), ErrorCode::InvalidArgument);
    }
    {
        Json doc = base;
        doc["white_balance"] = Json{{"mode", "camera_neutral"}, {"r", 100}, {"g", 1}, {"b", 1}};
        EXPECT_EQ(read_error(doc), ErrorCode::InvalidArgument);
    }
    // Only valid settings are written.
    engine::DevelopSettings invalid;
    invalid.contrast = 2.0;
    EXPECT_EQ(write_recipe(invalid).error().code, ErrorCode::InvalidArgument);
}

TEST(RecipeCategories, CoverEveryControlOnce) {
    std::size_t covered = 0;
    for (const RecipeCategory category : kAllRecipeCategories) {
        EXPECT_EQ(parse_recipe_category(to_string(category)), category);
        for (const engine::DevelopControl control : controls_of(category)) {
            EXPECT_EQ(category_of(control), category);
            ++covered;
        }
    }
    EXPECT_EQ(covered, kAllDevelopControls.size());
    EXPECT_EQ(controls_of(RecipeCategory::Local),
              (std::vector<engine::DevelopControl>{engine::DevelopControl::Highlights,
                                                   engine::DevelopControl::Shadows}));
    EXPECT_FALSE(parse_recipe_category("geometry"));
    EXPECT_EQ(to_string(static_cast<RecipeCategory>(42)), "invalid");
}

TEST(RecipeCategories, ChangedControlsAndCopy) {
    const engine::DevelopSettings a;
    const engine::DevelopSettings b = edited();
    EXPECT_TRUE(changed_controls(a, a).empty());
    const auto changed = changed_controls(a, b);
    EXPECT_EQ(changed.size(), 8u);  // everything but the base look
    EXPECT_EQ(changed_locks(a, b), (std::vector<engine::DevelopControl>{engine::DevelopControl::WhiteBalance,
                                                                        engine::DevelopControl::Exposure}));
    engine::DevelopSettings c = a;
    for (const engine::DevelopControl control : changed) {
        copy_value(b, c, control);
    }
    c.locks = b.locks;
    EXPECT_EQ(c, b);
}

}  // namespace
}  // namespace lucent::app
