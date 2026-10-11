// Recipe documents (recipe.hpp): RFC 8785 canonical form of "lucent.develop" v3, exact round trips
// (masks, geometry, chroma denoising and lens corrections included), strict reading of version-1 and
// version-2 documents (upgraded on read: no lens correction) and version-3 documents, VersionMismatch
// for newer ones, paste categories and parts.

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
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

// The canonical text of default ("as shot") settings: every member of version 3, sorted, no
// whitespace (automatic lens corrections, ADR 0023).
constexpr std::string_view kDefaultDocument =
    R"({"base_look":{"id":"neutral","version":1},"chroma_denoise":{"radius":8,"strength":0},"contrast":0,)"
    R"("exposure_ev":0,"geometry":{"crop":{"bottom":1,"left":0,"right":1,"top":0},"flip_horizontal":false,)"
    R"("quarter_turns":0,"straighten_degrees":0},"highlights":0,"lens_correction":{"distortion":true,)"
    R"("lateral_chromatic_aberration":true,"profile":null,"vignetting":true},"local_adjustments":[],"locks":[],)"
    R"("operations":{"chroma_denoise":1,"color":1,"contrast":1,"exposure":1,"geometry":2,"lens_correction":1,)"
    R"("local_adjustments":1,"local_tone":1,"tone_curve":1,"white_balance":1},"saturation":0,"schema":"lucent.develop",)"
    R"("shadows":0,"tone_curve":[[0,0],[1,1]],"version":3,"vibrance":0,"white_balance":{"mode":"as_shot"}})";

// The canonical text of default settings with geometry version 1 (the whole visible area).
constexpr std::string_view kGeometry1DefaultDocument =
    R"({"base_look":{"id":"neutral","version":1},"chroma_denoise":{"radius":8,"strength":0},"contrast":0,)"
    R"("exposure_ev":0,"geometry":{"crop":{"bottom":1,"left":0,"right":1,"top":0},"flip_horizontal":false,)"
    R"("quarter_turns":0,"straighten_degrees":0},"highlights":0,"lens_correction":{"distortion":true,)"
    R"("lateral_chromatic_aberration":true,"profile":null,"vignetting":true},"local_adjustments":[],"locks":[],)"
    R"("operations":{"chroma_denoise":1,"color":1,"contrast":1,"exposure":1,"geometry":1,"lens_correction":1,)"
    R"("local_adjustments":1,"local_tone":1,"tone_curve":1,"white_balance":1},"saturation":0,"schema":"lucent.develop",)"
    R"("shadows":0,"tone_curve":[[0,0],[1,1]],"version":3,"vibrance":0,"white_balance":{"mode":"as_shot"}})";

// What the as-shot documents of version 1 (geometry 1) and version 2 (geometry 2 or 1) convert to:
// the same settings with lens-correction operation version 0 (none), written as version 3.
constexpr std::string_view kVersion2DefaultUpgradedDocument =
    R"({"base_look":{"id":"neutral","version":1},"chroma_denoise":{"radius":8,"strength":0},"contrast":0,)"
    R"("exposure_ev":0,"geometry":{"crop":{"bottom":1,"left":0,"right":1,"top":0},"flip_horizontal":false,)"
    R"("quarter_turns":0,"straighten_degrees":0},"highlights":0,"lens_correction":{"distortion":true,)"
    R"("lateral_chromatic_aberration":true,"profile":null,"vignetting":true},"local_adjustments":[],"locks":[],)"
    R"("operations":{"chroma_denoise":1,"color":1,"contrast":1,"exposure":1,"geometry":2,"lens_correction":0,)"
    R"("local_adjustments":1,"local_tone":1,"tone_curve":1,"white_balance":1},"saturation":0,"schema":"lucent.develop",)"
    R"("shadows":0,"tone_curve":[[0,0],[1,1]],"version":3,"vibrance":0,"white_balance":{"mode":"as_shot"}})";
constexpr std::string_view kGeometry1UpgradedDocument =
    R"({"base_look":{"id":"neutral","version":1},"chroma_denoise":{"radius":8,"strength":0},"contrast":0,)"
    R"("exposure_ev":0,"geometry":{"crop":{"bottom":1,"left":0,"right":1,"top":0},"flip_horizontal":false,)"
    R"("quarter_turns":0,"straighten_degrees":0},"highlights":0,"lens_correction":{"distortion":true,)"
    R"("lateral_chromatic_aberration":true,"profile":null,"vignetting":true},"local_adjustments":[],"locks":[],)"
    R"("operations":{"chroma_denoise":1,"color":1,"contrast":1,"exposure":1,"geometry":1,"lens_correction":0,)"
    R"("local_adjustments":1,"local_tone":1,"tone_curve":1,"white_balance":1},"saturation":0,"schema":"lucent.develop",)"
    R"("shadows":0,"tone_curve":[[0,0],[1,1]],"version":3,"vibrance":0,"white_balance":{"mode":"as_shot"}})";

// The canonical text a version-1 build wrote for default settings (ADR 0010; identity 671dbaa8...).
constexpr std::string_view kVersion1DefaultDocument =
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

// Every version-2 member away from its default: three masks (one per kind), geometry, denoising.
engine::DevelopSettings edited_v2() {
    engine::DevelopSettings s = edited();
    s.chroma_denoise = engine::ChromaDenoiseSetting{0.7, 6.5};
    engine::LocalAdjustment radial;
    radial.id = 7;
    radial.opacity = 0.8;
    radial.shape = engine::RadialGradientMask{{0.4, 0.55}, 0.2, 0.125, -30.0, 0.35};
    radial.controls.exposure_ev = 0.5;
    radial.controls.saturation = -0.2;
    radial.controls.temperature = 0.3;
    engine::LocalAdjustment linear;
    linear.id = 2;
    linear.invert = true;
    linear.shape = engine::LinearGradientMask{{0.5, -0.1}, {0.5, 0.45}, 0.6};
    linear.controls.highlights = -0.5;
    linear.controls.contrast = 0.25;
    engine::LocalAdjustment brush;
    brush.id = 3;
    brush.enabled = false;
    engine::BrushMask strokes;
    strokes.strokes.push_back(engine::BrushStroke{engine::BrushMode::Add, 0.03, 0.4, 0.9,
                                                  {{0.1, 0.2}, {0.15, 0.25}, {0.2, 0.31}}});
    strokes.strokes.push_back(engine::BrushStroke{engine::BrushMode::Erase, 0.01, 1.0, 1.0, {{0.12, 0.22}}});
    brush.shape = strokes;
    brush.controls.shadows = 0.4;
    brush.controls.tint = -0.1;
    s.local_adjustments = {radial, linear, brush};
    s.geometry.straighten_degrees = -2.5;
    s.geometry.crop = engine::CropRect{0.05, 0.1, 0.9, 0.95};
    s.geometry.quarter_turns = 3;
    s.geometry.flip_horizontal = true;
    return s;
}

// Every member away from its default: edited_v2() and a named lens profile without vignetting.
engine::DevelopSettings edited_v3() {
    engine::DevelopSettings s = edited_v2();
    s.lens_correction.vignetting = false;
    s.lens_correction.profile = engine::LensProfileId{"Nikon", "Nikkor Z 24-70mm f/4 S", 1.0};
    return s;
}

// The settings of a version-2 document of `settings` (no lens correction): what reading converts it to.
engine::DevelopSettings without_lens(engine::DevelopSettings settings) {
    settings.operations.lens_correction = engine::kOldestLensCorrectionOperationVersion;
    settings.lens_correction = engine::LensCorrectionSetting{};
    return settings;
}

// The version-2 document of version-2 `settings` (the canonical version-3 document without the lens
// correction).
Json version2_json(const engine::DevelopSettings& settings) {
    auto document = write_recipe(settings);
    EXPECT_TRUE(document);
    Json doc = Json::parse(document ? document->json : std::string("{}"));
    doc["version"] = 2;
    doc.erase("lens_correction");
    doc["operations"].erase("lens_correction");
    return doc;
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
    // Pinned: any change of the canonical form changes every recipe identity. Version 2 (ADR 0015)
    // writes its three new members for every recipe, so the identity of default settings moved from
    // version 1's 671dbaa8... to 3104dbcc...; geometry version 2 (ADR 0017) moved it to 879de8b7...;
    // version 3 (ADR 0023) writes the lens correction for every recipe, so it moved again, to this one.
    EXPECT_EQ(core::to_hex(document->sha256),
              "61cdfb80bcacdb66015aa79380be418dbad80c70fa07026760e56418ef2d4bc2");

    engine::DevelopSettings geometry1;
    geometry1.operations.geometry = 1;
    auto old = write_recipe(geometry1);
    ASSERT_TRUE(old);
    EXPECT_EQ(old->json, kGeometry1DefaultDocument);
    EXPECT_EQ(core::to_hex(old->sha256), "282bed0a075ea6b5171b38a88bcdd8c9311186d453ab3535fd50e5343781d084");

    // The as-shot documents earlier builds wrote, which batches they froze name (recipe.hpp): the
    // version-2 ones keep their identities 879de8b7... (W10 to W12) and 3104dbcc... (W9).
    const std::array<std::string_view, 2> earlier = {
        "879de8b79743a9d3cf2db780c0e0ccb09552dc3a35d5e6c538377dd97721c8f7",
        "3104dbcc769bcd546a0a9449ec561c9e25f7c27a4290dda55f5bb14803f86738"};
    for (std::size_t i = 0; i < earlier.size(); ++i) {
        const std::string_view text = kVersion2AsShotRecipes[i];
        EXPECT_EQ(core::to_hex(*core::sha256(std::as_bytes(std::span(text.data(), text.size())))),
                  earlier[i]);
    }
}

TEST(RecipeDocument, GeometryVersionsOneAndTwoAreReadAndKept) {
    // A recipe keeps the geometry version it was made with (ADR 0017): version 1 crops within the
    // whole visible area, version 2 within the camera's image area.
    for (const std::uint32_t version : {1u, 2u}) {
        engine::DevelopSettings s;
        s.geometry.crop = engine::CropRect{0.1, 0.2, 0.9, 0.8};
        s.operations.geometry = version;
        auto document = write_recipe(s);
        ASSERT_TRUE(document) << document.error().describe();
        EXPECT_NE(document->json.find("\"geometry\":" + std::to_string(version) + ","), std::string::npos);
        auto read = read_recipe(document->json);
        ASSERT_TRUE(read) << read.error().describe();
        EXPECT_EQ(read->operations.geometry, version);
        EXPECT_EQ(*read, s);
    }
    // A newer version is refused as newer, never reinterpreted; 0 is not a version at all.
    for (const auto& [version, code] :
         {std::pair{3, ErrorCode::VersionMismatch}, std::pair{0, ErrorCode::InvalidArgument}}) {
        Json doc = Json::parse(std::string(kDefaultDocument));
        doc["operations"]["geometry"] = version;
        auto read = read_recipe(doc.dump());
        ASSERT_FALSE(read) << version;
        EXPECT_EQ(read.error().code, code) << read.error().describe();
    }
}

TEST(RecipeDocument, Version2MembersHaveAFixedCanonicalForm) {
    // Members sorted at every level, points as [x, y], integers without a fraction, booleans.
    engine::DevelopSettings s;
    engine::LocalAdjustment brush;
    brush.id = 4;
    brush.shape = engine::BrushMask{{engine::BrushStroke{engine::BrushMode::Erase, 0.25, 0.5, 1.0, {{0.5, 0.25}}}}};
    brush.controls.exposure_ev = -1.5;
    s.local_adjustments = {brush};
    s.geometry.quarter_turns = 1;
    s.chroma_denoise.strength = 0.5;
    auto document = write_recipe(s);
    ASSERT_TRUE(document);
    EXPECT_NE(document->json.find(
                  R"("local_adjustments":[{"controls":{"contrast":0,"exposure_ev":-1.5,"highlights":0,)"
                  R"("saturation":0,"shadows":0,"temperature":0,"tint":0},"enabled":true,"id":4,"invert":false,)"
                  R"("mask":{"kind":"brush","strokes":[{"flow":1,"hardness":0.5,"mode":"erase",)"
                  R"("points":[[0.5,0.25]],"radius":0.25}]},"opacity":1}])"),
              std::string::npos)
        << document->json;
    EXPECT_NE(document->json.find(R"("chroma_denoise":{"radius":8,"strength":0.5})"), std::string::npos);
    EXPECT_NE(document->json.find(R"("quarter_turns":1)"), std::string::npos);
    engine::DevelopSettings linear;
    linear.local_adjustments = {engine::LocalAdjustment{1, true, false, 1.0, engine::LinearGradientMask{}, {}}};
    linear.local_adjustments[0].controls.contrast = 0.5;
    auto linear_document = write_recipe(linear);
    ASSERT_TRUE(linear_document);
    EXPECT_NE(linear_document->json.find(R"("mask":{"end":[0.5,0.75],"feather":1,"kind":"linear","start":[0.5,0.25]})"),
              std::string::npos)
        << linear_document->json;
    engine::DevelopSettings radial;
    radial.local_adjustments = {engine::LocalAdjustment{1, true, false, 1.0, engine::RadialGradientMask{}, {}}};
    auto radial_document = write_recipe(radial);
    ASSERT_TRUE(radial_document);
    EXPECT_NE(radial_document->json.find(R"("mask":{"angle_degrees":0,"center":[0.5,0.5],"feather":0.5,)"
                                         R"("kind":"radial","radius_x":0.25,"radius_y":0.25})"),
              std::string::npos)
        << radial_document->json;
}

TEST(RecipeDocument, LensCorrectionHasAFixedCanonicalForm) {
    // Automatic: "profile": null; a named profile: its maker, model and crop factor, sorted.
    EXPECT_NE(std::string(kDefaultDocument)
                  .find(R"("lens_correction":{"distortion":true,)"
                        R"("lateral_chromatic_aberration":true,"profile":null,)"
                        R"("vignetting":true})"),
              std::string::npos);
    auto named = write_recipe(edited_v3());
    ASSERT_TRUE(named) << named.error().describe();
    EXPECT_NE(
        named->json.find(R"("lens_correction":{"distortion":true,"lateral_chromatic_aberration":true,)"
                         R"("profile":{"crop_factor":1,"maker":"Nikon","model":"Nikkor Z 24-70mm f/4 S"},)"
                         R"("vignetting":false})"),
        std::string::npos)
        << named->json;
    // A crop factor from Lensfun's floats keeps its exact double; names keep every character (escaped
    // as RFC 8785 requires).
    engine::DevelopSettings odd;
    odd.lens_correction.profile =
        engine::LensProfileId{"Fujifilm", "XF16-55mmF2.8 \"R\" LM WR", 1.5290000438690186};
    auto odd_document = write_recipe(odd);
    ASSERT_TRUE(odd_document) << odd_document.error().describe();
    EXPECT_NE(odd_document->json.find(R"("crop_factor":1.5290000438690186)"), std::string::npos);
    EXPECT_NE(odd_document->json.find(R"("model":"XF16-55mmF2.8 \"R\" LM WR")"), std::string::npos);
    auto read = read_recipe(odd_document->json);
    ASSERT_TRUE(read) << read.error().describe();
    EXPECT_EQ(*read, odd);
    // Version 0 (none) is written with the default member.
    auto none = write_recipe(without_lens(engine::DevelopSettings{}));
    ASSERT_TRUE(none);
    EXPECT_EQ(none->json, kVersion2DefaultUpgradedDocument);
}

TEST(RecipeDocument, RoundTripsEveryFieldExactly) {
    std::vector<engine::DevelopSettings> cases = {engine::DevelopSettings{}, edited(), edited_v2(),
                                                  edited_v3(), without_lens(edited_v2())};
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
    // Masks, strokes and points keep their order (they are ordered values): another order is
    // another recipe.
    engine::DevelopSettings reordered = edited_v2();
    std::swap(reordered.local_adjustments[0], reordered.local_adjustments[1]);
    EXPECT_NE(write_recipe(reordered)->sha256, write_recipe(edited_v2())->sha256);
}

TEST(RecipeDocument, RandomSettingsRoundTripBitExactly) {
    std::mt19937_64 random(20261003);  // fixed seed: deterministic
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    const auto position = [&] { return 0.5 + 0.75 * unit(random); };
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
        if (i % 2 == 1) {
            s.chroma_denoise = engine::ChromaDenoiseSetting{std::abs(unit(random)), 1.0 + 63.0 * std::abs(unit(random))};
            s.geometry.straighten_degrees = 45.0 * unit(random);
            const double left = 0.4 * std::abs(unit(random));
            const double top = 0.4 * std::abs(unit(random));
            s.geometry.crop = engine::CropRect{left, top, 1.0 - 0.4 * std::abs(unit(random)),
                                               1.0 - 0.4 * std::abs(unit(random))};
            s.geometry.quarter_turns = static_cast<std::uint32_t>(i % 4);
            s.geometry.flip_horizontal = i % 5 == 0;
            engine::LocalAdjustment radial;
            radial.id = static_cast<std::uint32_t>(i + 1);
            radial.opacity = std::abs(unit(random));
            radial.shape = engine::RadialGradientMask{{position(), position()}, 0.01 + std::abs(unit(random)),
                                                      0.01 + std::abs(unit(random)), 180.0 * unit(random),
                                                      std::abs(unit(random))};
            radial.controls.exposure_ev = 4.0 * unit(random);
            radial.controls.tint = unit(random);
            engine::LocalAdjustment brush;
            brush.id = static_cast<std::uint32_t>(i + 2);
            engine::BrushStroke stroke;
            stroke.radius = 0.001 + 0.4 * std::abs(unit(random));
            stroke.hardness = std::abs(unit(random));
            stroke.flow = std::abs(unit(random));
            for (int k = 0; k < 20; ++k) {
                stroke.points.push_back(engine::MaskPoint{position(), position()});
            }
            brush.shape = engine::BrushMask{{stroke}};
            brush.controls.saturation = unit(random);
            s.local_adjustments = {radial, brush};
        }
        auto document = write_recipe(s);
        ASSERT_TRUE(document) << document.error().describe();
        auto read = read_recipe(document->json);
        ASSERT_TRUE(read) << read.error().describe();
        ASSERT_EQ(*read, s) << document->json;
    }
}

TEST(RecipeDocument, TheLargestRecipeStaysAFewMegabytes) {
    // Every brush point the contract allows (16 masks of 4096 points each: 65 536), at positions
    // with many digits.
    engine::DevelopSettings s;
    std::mt19937_64 random(7);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    for (std::uint32_t m = 0; m < 16; ++m) {
        engine::LocalAdjustment a;
        a.id = m + 1;
        a.controls.exposure_ev = 0.5;
        engine::BrushStroke stroke;
        for (std::size_t i = 0; i < engine::kMaxStrokePoints; ++i) {
            stroke.points.push_back(engine::MaskPoint{unit(random), unit(random)});
        }
        a.shape = engine::BrushMask{{stroke}};
        s.local_adjustments.push_back(a);
    }
    ASSERT_TRUE(s.validate());
    const auto start = std::chrono::steady_clock::now();
    auto document = write_recipe(s);
    const auto written = std::chrono::steady_clock::now();
    ASSERT_TRUE(document) << document.error().describe();
    auto read = read_recipe(document->json);
    const auto done = std::chrono::steady_clock::now();
    ASSERT_TRUE(read) << read.error().describe();
    EXPECT_EQ(*read, s);
    EXPECT_LT(document->json.size(), std::size_t{4} << 20);
    std::printf("[ RECIPE   ] %zu bytes; write %.1f ms, read %.1f ms\n", document->json.size(),
                std::chrono::duration<double, std::milli>(written - start).count(),
                std::chrono::duration<double, std::milli>(done - written).count());
    // One point more is refused.
    s.local_adjustments.push_back(engine::LocalAdjustment{17, true, false, 1.0,
                                                          engine::BrushMask{{engine::BrushStroke{
                                                              engine::BrushMode::Add, 0.02, 0.5, 1.0, {{0.5, 0.5}}}}},
                                                          {}});
    EXPECT_EQ(write_recipe(s).error().code, ErrorCode::InvalidArgument);
}

TEST(RecipeDocument, AcceptsAnyJsonNumberSpellingAndOrder) {
    // Whitespace, member order and number spelling are free on input; the identity is that of the
    // canonical re-serialisation. Older documents read as their version-3 conversion.
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
    expected.operations.geometry = 1;  // a version-1 recipe renders the whole visible area (ADR 0017)
    expected.operations.lens_correction = 0;  // and corrects no lens (ADR 0023)
    EXPECT_EQ(*read, expected);
    EXPECT_EQ(read->version, 3u);
    EXPECT_EQ(write_recipe(*read)->sha256, write_recipe(expected)->sha256);

    // The same freedom for the version-2 members.
    const std::string v2 = R"({ "version": 2, "schema": "lucent.develop",
        "white_balance": {"mode": "as_shot"}, "exposure_ev": 0, "contrast": 0, "highlights": 0,
        "shadows": 0, "tone_curve": [[0, 0], [1, 1]], "saturation": 0, "vibrance": 0,
        "base_look": {"id": "neutral", "version": 1}, "locks": [],
        "geometry": {"straighten_degrees": 1.5e0, "flip_horizontal": false, "quarter_turns": 2,
                     "crop": {"top": 0.1, "left": 0, "bottom": 1E0, "right": 0.75}},
        "chroma_denoise": {"strength": 5e-1, "radius": 12.0},
        "local_adjustments": [{"opacity": 1.0, "id": 9, "invert": false, "enabled": true,
            "mask": {"start": [0.5, 0], "kind": "linear", "feather": 1, "end": [0.5, 1.0]},
            "controls": {"tint": 0, "temperature": 0, "shadows": 0, "saturation": 0,
                         "highlights": 0, "exposure_ev": -1.0, "contrast": 0}}],
        "operations": {"geometry": 1, "local_adjustments": 1, "chroma_denoise": 1, "white_balance": 1,
                       "exposure": 1, "local_tone": 1, "contrast": 1, "tone_curve": 1, "color": 1}})";
    auto read_v2 = read_recipe(v2);
    ASSERT_TRUE(read_v2) << read_v2.error().describe();
    engine::DevelopSettings expected_v2;
    expected_v2.operations.geometry = 1;  // as the document says
    expected_v2.geometry.straighten_degrees = 1.5;
    expected_v2.geometry.quarter_turns = 2;
    expected_v2.geometry.crop = engine::CropRect{0.0, 0.1, 0.75, 1.0};
    expected_v2.chroma_denoise = engine::ChromaDenoiseSetting{0.5, 12.0};
    engine::LocalAdjustment linear;
    linear.id = 9;
    linear.shape = engine::LinearGradientMask{{0.5, 0.0}, {0.5, 1.0}, 1.0};
    linear.controls.exposure_ev = -1.0;
    expected_v2.local_adjustments = {linear};
    expected_v2.operations.lens_correction = 0;  // a version-2 recipe corrects no lens (ADR 0023)
    EXPECT_EQ(*read_v2, expected_v2);

    // The same freedom for the version-3 member.
    Json v3 = Json::parse(v2);
    v3["version"] = 3;
    v3["operations"]["lens_correction"] = 1.0e0;  // an unsigned integer is required: refused
    EXPECT_EQ(read_error(v3), ErrorCode::InvalidArgument);
    const std::string v3_text = R"({ "version": 3, "schema": "lucent.develop",
        "white_balance": {"mode": "as_shot"}, "exposure_ev": 0, "contrast": 0, "highlights": 0,
        "shadows": 0, "tone_curve": [[0, 0], [1, 1]], "saturation": 0, "vibrance": 0,
        "base_look": {"id": "neutral", "version": 1}, "locks": [],
        "geometry": {"straighten_degrees": 0, "flip_horizontal": false, "quarter_turns": 0,
                     "crop": {"top": 0, "left": 0, "bottom": 1, "right": 1}},
        "chroma_denoise": {"strength": 0, "radius": 8},
        "local_adjustments": [],
        "lens_correction": {"vignetting": false, "profile": {"model": "FE 50mm f/1.2 GM", "crop_factor": 1E0,
                            "maker": "Sony"}, "lateral_chromatic_aberration": true, "distortion": true},
        "operations": {"lens_correction": 1, "geometry": 2, "local_adjustments": 1, "chroma_denoise": 1,
                       "white_balance": 1, "exposure": 1, "local_tone": 1, "contrast": 1, "tone_curve": 1,
                       "color": 1}})";
    auto read_v3 = read_recipe(v3_text);
    ASSERT_TRUE(read_v3) << read_v3.error().describe();
    engine::DevelopSettings expected_v3;
    expected_v3.lens_correction.vignetting = false;
    expected_v3.lens_correction.profile = engine::LensProfileId{"Sony", "FE 50mm f/1.2 GM", 1.0};
    EXPECT_EQ(*read_v3, expected_v3);
}

TEST(RecipeDocument, Version1DocumentsAreUpgradedOnRead) {
    // The version-1 default document reads as default settings of geometry version 1 without lens
    // corrections (kGeometry1UpgradedDocument).
    auto read = read_recipe(kVersion1DefaultDocument);
    ASSERT_TRUE(read) << read.error().describe();
    engine::DevelopSettings geometry1;
    geometry1.operations.geometry = 1;  // version-1 recipes keep rendering the whole visible area
    geometry1 = without_lens(geometry1);  // and correct no lens
    EXPECT_EQ(*read, geometry1);
    EXPECT_EQ(read->version, engine::kDevelopSettingsVersion);
    EXPECT_EQ(write_recipe(*read)->json, kGeometry1UpgradedDocument);
    EXPECT_EQ(core::to_hex(write_recipe(*read)->sha256),
              "b7b0027785caf4d4e836765961653cc5f0762369cc67866dd0087b048e96bb46");

    // A version-1 recipe of every version-1 field: the upgrade changes nothing else.
    Json v1 = canonical_json(edited());
    v1["version"] = 1;
    v1.erase("chroma_denoise");
    v1.erase("local_adjustments");
    v1.erase("geometry");
    v1.erase("lens_correction");
    v1["operations"].erase("chroma_denoise");
    v1["operations"].erase("local_adjustments");
    v1["operations"].erase("geometry");
    v1["operations"].erase("lens_correction");
    auto upgraded = read_recipe(v1.dump());
    ASSERT_TRUE(upgraded) << upgraded.error().describe();
    engine::DevelopSettings expected = edited();
    expected.operations.geometry = 1;
    expected = without_lens(expected);
    EXPECT_EQ(*upgraded, expected);
    // It has the identity of the version-3 document of the same settings.
    EXPECT_EQ(write_recipe(*upgraded)->sha256, write_recipe(expected)->sha256);

    // Version-1 settings (a value, not a document) are written as their conversion (geometry
    // version 1, no lens correction).
    engine::DevelopSettings old = edited();
    old.version = 1;
    auto written = write_recipe(old);
    ASSERT_TRUE(written) << written.error().describe();
    EXPECT_EQ(written->sha256, write_recipe(expected)->sha256);

    // A version-1 document never holds a member or operation of a later version.
    for (const char* member : {"chroma_denoise", "local_adjustments", "geometry", "lens_correction"}) {
        Json doc = v1;
        doc[member] = canonical_json(engine::DevelopSettings{})[member];
        EXPECT_EQ(read_error(doc), ErrorCode::InvalidArgument) << member;
        Json op = v1;
        op["operations"][member] = 1;
        EXPECT_EQ(read_error(op), ErrorCode::InvalidArgument) << member;
    }
}

TEST(RecipeDocument, Version2DocumentsAreUpgradedOnRead) {
    // The as-shot documents W9 to W12 wrote read as the same settings without lens corrections
    // (lens-correction operation version 0): they render exactly as then (ADR 0023).
    for (std::size_t i = 0; i < kVersion2AsShotRecipes.size(); ++i) {
        auto read = read_recipe(kVersion2AsShotRecipes[i]);
        ASSERT_TRUE(read) << read.error().describe();
        engine::DevelopSettings expected;
        expected.operations.geometry = i == 0 ? 2 : 1;
        expected = without_lens(expected);
        EXPECT_EQ(*read, expected);
        EXPECT_EQ(read->version, engine::kDevelopSettingsVersion);
        EXPECT_FALSE(engine::effective_lens_correction(*read).has_value());
        EXPECT_TRUE(read->is_as_shot());  // operation version 0 is not an edit
        EXPECT_EQ(write_recipe(*read)->json,
                  i == 0 ? kVersion2DefaultUpgradedDocument : kGeometry1UpgradedDocument);
    }
    EXPECT_EQ(core::to_hex(write_recipe(*read_recipe(kVersion2AsShotRecipes[0]))->sha256),
              "338dd4c1e250927ca54d9f17c549613dfea99f11634641e15054918f1b0ca7a5");

    // A version-2 recipe of every version-2 member: the upgrade changes nothing else.
    const Json v2 = version2_json(edited_v2());
    auto upgraded = read_recipe(v2.dump());
    ASSERT_TRUE(upgraded) << upgraded.error().describe();
    EXPECT_EQ(*upgraded, without_lens(edited_v2()));
    // Version-2 settings (a value) are written as their conversion.
    engine::DevelopSettings old = edited_v2();
    old.version = 2;
    auto written = write_recipe(old);
    ASSERT_TRUE(written) << written.error().describe();
    EXPECT_EQ(written->sha256, write_recipe(without_lens(edited_v2()))->sha256);

    // A version-2 document never holds the version-3 member or operation.
    Json member = v2;
    member["lens_correction"] = canonical_json(engine::DevelopSettings{})["lens_correction"];
    EXPECT_EQ(read_error(member), ErrorCode::InvalidArgument);
    Json operation = v2;
    operation["operations"]["lens_correction"] = 1;
    EXPECT_EQ(read_error(operation), ErrorCode::InvalidArgument);
    // A version-2 value holding a version-3 member is not valid either.
    engine::DevelopSettings mixed = edited_v2();
    mixed.version = 2;
    mixed.lens_correction.distortion = false;
    EXPECT_EQ(write_recipe(mixed).error().code, ErrorCode::InvalidArgument);
}

TEST(RecipeDocument, RejectsBadLensCorrections) {
    const Json base = canonical_json(edited_v3());
    const auto expect_error = [&](const std::function<void(Json&)>& change, ErrorCode code,
                                  const char* what) {
        Json doc = base;
        change(doc);
        EXPECT_EQ(read_error(doc), code) << what;
    };
    expect_error([](Json& d) { d.erase("lens_correction"); }, ErrorCode::InvalidArgument,
                 "no lens_correction");
    expect_error([](Json& d) { d["operations"].erase("lens_correction"); }, ErrorCode::InvalidArgument,
                 "no lens-correction operation");
    expect_error([](Json& d) { d["lens_correction"]["defringe"] = true; }, ErrorCode::InvalidArgument,
                 "an unknown lens member");
    expect_error([](Json& d) { d["lens_correction"].erase("vignetting"); }, ErrorCode::InvalidArgument,
                 "a missing switch");
    expect_error([](Json& d) { d["lens_correction"]["distortion"] = 1; }, ErrorCode::InvalidArgument,
                 "a number for a switch");
    expect_error([](Json& d) { d["lens_correction"]["profile"] = "Nikon"; }, ErrorCode::InvalidArgument,
                 "a profile that is a string");
    expect_error([](Json& d) { d["lens_correction"]["profile"]["mount"] = "Z"; }, ErrorCode::InvalidArgument,
                 "an unknown profile member");
    expect_error([](Json& d) { d["lens_correction"]["profile"].erase("crop_factor"); },
                 ErrorCode::InvalidArgument, "a profile without crop factor");
    expect_error([](Json& d) { d["lens_correction"]["profile"]["maker"] = 7; }, ErrorCode::InvalidArgument,
                 "a maker that is a number");
    expect_error([](Json& d) { d["lens_correction"]["profile"]["model"] = ""; }, ErrorCode::InvalidArgument,
                 "an empty model");
    expect_error([](Json& d) { d["lens_correction"]["profile"]["model"] = std::string(257, 'x'); },
                 ErrorCode::InvalidArgument, "a model of 257 bytes");
    expect_error([](Json& d) { d["lens_correction"]["profile"]["crop_factor"] = 0.0; },
                 ErrorCode::InvalidArgument, "a crop factor of 0");
    expect_error([](Json& d) { d["lens_correction"]["profile"]["crop_factor"] = 101; },
                 ErrorCode::InvalidArgument, "a crop factor above 100");
    // Version 0 ("none") holds the default member only.
    expect_error([](Json& d) { d["operations"]["lens_correction"] = 0; }, ErrorCode::InvalidArgument,
                 "version 0 with a named profile");
    {
        Json doc = canonical_json(engine::DevelopSettings{});
        doc["operations"]["lens_correction"] = 0;
        auto read = read_recipe(doc.dump());
        ASSERT_TRUE(read) << read.error().describe();
        EXPECT_EQ(*read, without_lens(engine::DevelopSettings{}));
    }
    // A newer lens-correction operation is never treated as none.
    expect_error([](Json& d) { d["operations"]["lens_correction"] = 2; }, ErrorCode::VersionMismatch,
                 "lens-correction operation 2");
    expect_error([](Json& d) { d["operations"]["lens_correction"] = -1; }, ErrorCode::InvalidArgument,
                 "a negative operation version");
}

TEST(RecipeDocument, RejectsUnknownMissingAndMistypedMembers) {
    const Json base = canonical_json(edited_v2());
    const auto expect_invalid = [&](const std::function<void(Json&)>& change, const char* what) {
        Json doc = base;
        change(doc);
        EXPECT_EQ(read_error(doc), ErrorCode::InvalidArgument) << what;
    };
    expect_invalid([](Json& d) { d["sharpening"] = 0.5; }, "a field this version does not define");
    expect_invalid([](Json& d) { d["white_balance"]["green_shift"] = 1; }, "white balance member");
    expect_invalid([](Json& d) { d["base_look"]["strength"] = 1; }, "base look member");
    expect_invalid([](Json& d) { d["operations"]["sharpen"] = 1; }, "an unknown operation: never a no-op");
    expect_invalid([](Json& d) { d["operations"].erase("color"); }, "a missing operation");
    expect_invalid([](Json& d) { d["operations"].erase("geometry"); }, "a missing version-2 operation");
    expect_invalid([](Json& d) { d.erase("contrast"); }, "a missing member");
    expect_invalid([](Json& d) { d["exposure_ev"] = "0.5"; }, "a string number");
    expect_invalid([](Json& d) { d["version"] = 1.0; }, "versions are unsigned integers");
    expect_invalid([](Json& d) { d["tone_curve"][1] = Json::array({0.25, 0.2, 0.0}); }, "a curve point");
    expect_invalid([](Json& d) { d["locks"] = Json::array({"exposure", "focus"}); }, "an unknown lock");
    expect_invalid([](Json& d) { d["locks"] = Json::array({"exposure", "exposure"}); }, "a lock twice");
    expect_invalid([](Json& d) { d["white_balance"] = Json{{"mode", "auto"}}; }, "an unknown mode");
    // Version 2: a version-2 document without its new members is refused.
    expect_invalid([](Json& d) { d.erase("chroma_denoise"); }, "no chroma_denoise");
    expect_invalid([](Json& d) { d.erase("local_adjustments"); }, "no local_adjustments");
    expect_invalid([](Json& d) { d.erase("geometry"); }, "no geometry");
    // Unknown, missing and mistyped members of the new ones.
    expect_invalid([](Json& d) { d["chroma_denoise"]["mode"] = "luma"; }, "chroma_denoise member");
    expect_invalid([](Json& d) { d["chroma_denoise"].erase("radius"); }, "chroma_denoise radius");
    expect_invalid([](Json& d) { d["geometry"]["perspective"] = 0; }, "geometry member");
    expect_invalid([](Json& d) { d["geometry"]["crop"]["width"] = 1; }, "crop member");
    expect_invalid([](Json& d) { d["geometry"]["quarter_turns"] = 1.0; }, "quarter turns with a fraction");
    expect_invalid([](Json& d) { d["geometry"]["quarter_turns"] = -1; }, "negative quarter turns");
    expect_invalid([](Json& d) { d["geometry"]["flip_horizontal"] = 1; }, "a number for a flag");
    expect_invalid([](Json& d) { d["local_adjustments"] = Json::object(); }, "masks not an array");
    expect_invalid([](Json& d) { d["local_adjustments"][0]["feather"] = 0.5; }, "adjustment member");
    expect_invalid([](Json& d) { d["local_adjustments"][0].erase("opacity"); }, "adjustment opacity");
    expect_invalid([](Json& d) { d["local_adjustments"][0]["id"] = 7.0; }, "a mask id with a fraction");
    expect_invalid([](Json& d) { d["local_adjustments"][0]["enabled"] = "true"; }, "a string flag");
    expect_invalid([](Json& d) { d["local_adjustments"][0]["controls"]["clarity"] = 0; }, "a control");
    expect_invalid([](Json& d) { d["local_adjustments"][0]["controls"].erase("tint"); }, "a missing control");
    expect_invalid([](Json& d) { d["local_adjustments"][0]["mask"]["kind"] = "lasso"; }, "an unknown kind");
    expect_invalid([](Json& d) { d["local_adjustments"][0]["mask"]["start"] = Json::array({0, 0}); },
                   "a linear member on a radial mask");
    expect_invalid([](Json& d) { d["local_adjustments"][0]["mask"]["center"] = Json::array({0.5}); },
                   "a point of one number");
    expect_invalid([](Json& d) { d["local_adjustments"][1]["mask"].erase("feather"); }, "a linear feather");
    expect_invalid([](Json& d) { d["local_adjustments"][2]["mask"]["strokes"][0]["mode"] = "smudge"; },
                   "an unknown brush mode");
    expect_invalid([](Json& d) { d["local_adjustments"][2]["mask"]["strokes"][0]["pressure"] = 1; },
                   "a stroke member");
    expect_invalid([](Json& d) { d["local_adjustments"][2]["mask"]["strokes"][0]["points"][0] = "a"; },
                   "a point that is not an array");
    expect_invalid([](Json& d) { d["local_adjustments"][2]["mask"]["strokes"] = Json::object(); },
                   "strokes not an array");
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
    // Repeated inside a mask too.
    auto masked = write_recipe(edited_v2());
    ASSERT_TRUE(masked);
    std::string repeated_mask = masked->json;
    const std::size_t at = repeated_mask.find(R"("kind":"radial")");
    ASSERT_NE(at, std::string::npos);
    repeated_mask.insert(at, R"("feather":0.1,)");
    EXPECT_EQ(read_recipe(repeated_mask).error().code, ErrorCode::InvalidArgument);

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
    const Json base = canonical_json(edited_v2());
    {
        // A newer schema is not interpreted at all, whatever it contains.
        Json doc = base;
        doc["version"] = 4;
        doc["perspective"] = Json::array({Json{{"kind", "keystone"}}});
        EXPECT_EQ(read_error(doc), ErrorCode::VersionMismatch);
    }
    for (const char* operation :
         {"exposure", "chroma_denoise", "local_adjustments", "geometry", "lens_correction"}) {
        Json doc = base;
        // A newer definition of the operation (the geometry reads versions 1 and 2, ADR 0017; the lens
        // correction 0 and 1, ADR 0023).
        doc["operations"][operation] = std::string_view(operation) == "geometry" ? 3 : 2;
        EXPECT_EQ(read_error(doc), ErrorCode::VersionMismatch) << operation;
        if (std::string_view(operation) != "lens_correction") {  // 0 is the lens correction's "none"
            doc["operations"][operation] = 0;
            EXPECT_EQ(read_error(doc), ErrorCode::InvalidArgument) << operation;
        }
    }
    {
        // A newer mask definition is VersionMismatch even when its shapes would also be new.
        Json doc = base;
        doc["operations"]["local_adjustments"] = 2;
        doc["local_adjustments"][0]["mask"]["kind"] = "segment";
        EXPECT_EQ(read_error(doc), ErrorCode::VersionMismatch);
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
    const Json masked = canonical_json(edited_v2());
    const auto expect_invalid = [](const Json& from, const std::function<void(Json&)>& change, const char* what) {
        Json doc = from;
        change(doc);
        EXPECT_EQ(read_error(doc), ErrorCode::InvalidArgument) << what;
    };
    expect_invalid(base, [](Json& d) { d["exposure_ev"] = 5.5; }, "exposure");
    expect_invalid(base,
                   [](Json& d) { d["tone_curve"] = Json::array({Json::array({0, 0.5}), Json::array({1, 0.25})}); },
                   "a decreasing curve");
    expect_invalid(base, [](Json& d) { d["white_balance"] = Json{{"mode", "kelvin"}, {"temperature_k", 1000}, {"tint", 0}}; },
                   "kelvin");
    expect_invalid(base,
                   [](Json& d) { d["white_balance"] = Json{{"mode", "camera_neutral"}, {"r", 100}, {"g", 1}, {"b", 1}}; },
                   "camera neutral");
    expect_invalid(base, [](Json& d) { d["chroma_denoise"]["strength"] = 1.5; }, "denoise strength");
    expect_invalid(base, [](Json& d) { d["chroma_denoise"]["radius"] = 0.5; }, "denoise radius");
    expect_invalid(base, [](Json& d) { d["geometry"]["straighten_degrees"] = 46; }, "straighten");
    expect_invalid(base, [](Json& d) { d["geometry"]["quarter_turns"] = 4; }, "quarter turns");
    expect_invalid(base, [](Json& d) { d["geometry"]["crop"]["left"] = 0.8; d["geometry"]["crop"]["right"] = 0.2; },
                   "an unordered crop");
    expect_invalid(base, [](Json& d) { d["geometry"]["crop"]["bottom"] = 1.5; }, "a crop beyond the image");
    expect_invalid(masked, [](Json& d) { d["local_adjustments"][1]["id"] = 7; }, "a repeated mask id");
    expect_invalid(masked, [](Json& d) { d["local_adjustments"][1]["id"] = 0; }, "mask id 0");
    expect_invalid(masked, [](Json& d) { d["local_adjustments"][0]["opacity"] = 1.5; }, "opacity");
    expect_invalid(masked, [](Json& d) { d["local_adjustments"][0]["controls"]["exposure_ev"] = 4.5; },
                   "local exposure");
    expect_invalid(masked, [](Json& d) { d["local_adjustments"][0]["mask"]["center"] = Json::array({2.5, 0.5}); },
                   "a centre beyond one image size");
    expect_invalid(masked, [](Json& d) { d["local_adjustments"][1]["mask"]["end"] = d["local_adjustments"][1]["mask"]["start"]; },
                   "a gradient without length");
    expect_invalid(masked, [](Json& d) { d["local_adjustments"][2]["mask"]["strokes"][0]["points"] = Json::array(); },
                   "a stroke without points");
    expect_invalid(masked, [](Json& d) { d["local_adjustments"][2]["mask"]["strokes"][0]["radius"] = 0.0; },
                   "a brush radius of 0");
    expect_invalid(masked,
                   [](Json& d) {
                       Json many = Json::array();
                       for (std::uint32_t i = 1; i <= engine::kMaxLocalAdjustments + 1; ++i) {
                           Json a = d["local_adjustments"][0];
                           a["id"] = i;
                           many.push_back(a);
                       }
                       d["local_adjustments"] = many;
                   },
                   "more masks than allowed");
    expect_invalid(masked,
                   [](Json& d) {
                       Json points = Json::array();
                       for (std::size_t i = 0; i <= engine::kMaxStrokePoints; ++i) {
                           points.push_back(Json::array({0.5, 0.5}));
                       }
                       d["local_adjustments"][2]["mask"]["strokes"][0]["points"] = points;
                   },
                   "more stroke points than allowed");
    // Only valid settings are written.
    engine::DevelopSettings invalid;
    invalid.contrast = 2.0;
    EXPECT_EQ(write_recipe(invalid).error().code, ErrorCode::InvalidArgument);
    engine::DevelopSettings invalid_geometry;
    invalid_geometry.geometry.quarter_turns = 7;
    EXPECT_EQ(write_recipe(invalid_geometry).error().code, ErrorCode::InvalidArgument);
}

TEST(RecipeCategories, CoverEveryControlAndPartOnce) {
    std::size_t controls = 0;
    std::size_t parts = 0;
    for (const RecipeCategory category : kAllRecipeCategories) {
        EXPECT_EQ(parse_recipe_category(to_string(category)), category);
        for (const engine::DevelopControl control : controls_of(category)) {
            EXPECT_EQ(category_of(control), category);
            ++controls;
        }
        for (const RecipePart part : parts_of(category)) {
            EXPECT_EQ(category_of(part), category);
            ++parts;
        }
        EXPECT_FALSE(controls_of(category).empty() && parts_of(category).empty()) << to_string(category);
    }
    EXPECT_EQ(controls, kAllDevelopControls.size());
    EXPECT_EQ(parts, kAllRecipeParts.size());
    // The masks and the geometry are categories of their own; denoising is detail.
    EXPECT_EQ(controls_of(RecipeCategory::Local),
              (std::vector<engine::DevelopControl>{engine::DevelopControl::Highlights,
                                                   engine::DevelopControl::Shadows}));
    EXPECT_TRUE(parts_of(RecipeCategory::Local).empty());
    EXPECT_EQ(parts_of(RecipeCategory::Masks), std::vector<RecipePart>{RecipePart::LocalAdjustments});
    EXPECT_TRUE(controls_of(RecipeCategory::Masks).empty());
    EXPECT_EQ(parts_of(RecipeCategory::Detail), std::vector<RecipePart>{RecipePart::ChromaDenoise});
    EXPECT_EQ(parts_of(RecipeCategory::Geometry), std::vector<RecipePart>{RecipePart::Geometry});
    EXPECT_TRUE(controls_of(RecipeCategory::Geometry).empty());
    EXPECT_EQ(parts_of(RecipeCategory::LensCorrections), std::vector<RecipePart>{RecipePart::LensCorrection});
    EXPECT_TRUE(controls_of(RecipeCategory::LensCorrections).empty());
    EXPECT_EQ(parse_recipe_category("lens_corrections"), RecipeCategory::LensCorrections);
    EXPECT_EQ(to_string(RecipePart::LensCorrection), "lens_correction");
    EXPECT_EQ(parse_recipe_category("geometry"), RecipeCategory::Geometry);
    EXPECT_EQ(parse_recipe_category("detail"), RecipeCategory::Detail);
    EXPECT_EQ(parse_recipe_category("masks"), RecipeCategory::Masks);
    EXPECT_EQ(to_string(static_cast<RecipeCategory>(42)), "invalid");
    // The masks, the geometry and the lens corrections are the categories not pasted by default.
    EXPECT_EQ(kDefaultRecipeCategories.size(), kAllRecipeCategories.size() - 3);
    for (const RecipeCategory category : kDefaultRecipeCategories) {
        EXPECT_NE(category, RecipeCategory::Geometry);
        EXPECT_NE(category, RecipeCategory::Masks);
        EXPECT_NE(category, RecipeCategory::LensCorrections);
    }
    // Part names (revision notes).
    for (const RecipePart part : kAllRecipeParts) {
        EXPECT_EQ(parse_recipe_part(to_string(part)), part);
    }
    EXPECT_EQ(to_string(RecipePart::LocalAdjustments), "masks");
    EXPECT_FALSE(parse_recipe_part("local_adjustments"));
}

TEST(RecipeCategories, ChangedControlsPartsAndCopy) {
    const engine::DevelopSettings a;
    const engine::DevelopSettings b = edited_v3();
    EXPECT_TRUE(changed_controls(a, a).empty());
    EXPECT_TRUE(changed_parts(a, a).empty());
    const auto changed = changed_controls(a, b);
    EXPECT_EQ(changed.size(), 8u);  // everything but the base look
    EXPECT_EQ(changed_parts(a, b),
              (std::vector<RecipePart>{RecipePart::ChromaDenoise, RecipePart::LocalAdjustments,
                                       RecipePart::Geometry, RecipePart::LensCorrection}));
    EXPECT_EQ(changed_locks(a, b), (std::vector<engine::DevelopControl>{engine::DevelopControl::WhiteBalance,
                                                                        engine::DevelopControl::Exposure}));
    engine::DevelopSettings c = a;
    for (const engine::DevelopControl control : changed) {
        copy_value(b, c, control);
    }
    c.locks = b.locks;
    EXPECT_NE(c, b);
    for (const RecipePart part : changed_parts(a, b)) {
        EXPECT_FALSE(same_value(c, b, part));
        copy_value(b, c, part);
        EXPECT_TRUE(same_value(c, b, part));
    }
    EXPECT_EQ(c, b);
}

TEST(RecipeCategories, TheGeometryPartCarriesItsOperationVersion) {
    // A crop of geometry version 1 is relative to the whole visible area, one of version 2 to the
    // camera's image area (ADR 0017): pasting the geometry pastes its version too, and a version
    // change alone is a geometry change.
    engine::DevelopSettings old_crop;
    old_crop.operations.geometry = 1;
    old_crop.geometry.crop = engine::CropRect{0.1, 0.1, 0.9, 0.9};
    engine::DevelopSettings target;  // version 2
    EXPECT_FALSE(same_value(target, old_crop, RecipePart::Geometry));
    copy_value(old_crop, target, RecipePart::Geometry);
    EXPECT_EQ(target.operations.geometry, 1u);
    EXPECT_EQ(target.geometry, old_crop.geometry);
    EXPECT_TRUE(same_value(target, old_crop, RecipePart::Geometry));

    engine::DevelopSettings v2;
    engine::DevelopSettings v1 = v2;
    v1.operations.geometry = 1;
    EXPECT_EQ(changed_parts(v1, v2), std::vector<RecipePart>{RecipePart::Geometry});
}

TEST(RecipeCategories, TheLensCorrectionPartCarriesItsOperationVersion) {
    // Version 0 ("none": made before lens corrections) and the automatic defaults are different
    // values; pasting the part pastes the version, and version 0 leaves the member at its default.
    const engine::DevelopSettings automatic;
    const engine::DevelopSettings none = without_lens(automatic);
    EXPECT_FALSE(same_value(automatic, none, RecipePart::LensCorrection));
    EXPECT_EQ(changed_parts(automatic, none), std::vector<RecipePart>{RecipePart::LensCorrection});
    engine::DevelopSettings target = edited_v3();  // a named profile
    copy_value(none, target, RecipePart::LensCorrection);
    EXPECT_EQ(target.operations.lens_correction, 0u);
    EXPECT_EQ(target.lens_correction, engine::LensCorrectionSetting{});
    EXPECT_TRUE(target.validate());
    EXPECT_TRUE(same_value(target, none, RecipePart::LensCorrection));
    copy_value(edited_v3(), target, RecipePart::LensCorrection);
    EXPECT_EQ(target.operations.lens_correction, 1u);
    EXPECT_EQ(target.lens_correction, edited_v3().lens_correction);
    // An unused member of version 0 is not a difference (only what renders is compared).
    engine::DevelopSettings stale = none;
    stale.lens_correction.vignetting = false;  // invalid as a value, but same_value looks at what renders
    EXPECT_TRUE(same_value(stale, none, RecipePart::LensCorrection));
}

}  // namespace
}  // namespace lucent::app
