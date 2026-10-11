// Lens-profile corrections (lens_correction.hpp, develop_settings.hpp v3, develop.hpp stage two;
// ADR 0023): DevelopSettings version 3 and its upgrades, matching and planning on a synthetic
// Lensfun database, the model and its round trips against Lensfun's own lfModifier (the reference
// implementation of the formulas), masks across toggles, version-2 renders, determinism.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <lensfun/lensfun.h>

#include <lucent/engine/develop.hpp>
#include <lucent/engine/lens_correction.hpp>

#include "develop_test_support.hpp"
#include "dng_opcode_test_support.hpp"
#include "lens_test_support.hpp"

namespace lucent::engine {
namespace {

namespace ts = test_support;

// ---- Helpers --------------------------------------------------------------------------------------

raw::RawMetadata lens_metadata(const std::string& camera_model, const std::string& lens_model, float focal,
                               std::optional<float> f_number, std::optional<float> f35 = std::nullopt,
                               const std::string& lens_make = {}) {
    raw::RawMetadata m;
    ts::set_lens_identity(m, "Lucent", camera_model, lens_model, focal, f_number, f35, lens_make);
    return m;
}

LensQuery query_of(const std::string& camera_model, const std::string& lens_model, float focal = 35.0f,
                   std::optional<float> f_number = 4.0f, std::optional<float> f35 = std::nullopt,
                   const std::string& lens_make = {}) {
    return lens_query(lens_metadata(camera_model, lens_model, focal, f_number, f35, lens_make));
}

LensCorrectionPlan plan_of(const raw::RawMetadata& m, const std::optional<LensCorrectionSetting>& setting,
                           image::Size upright, image::Orientation orientation = image::Orientation::Normal,
                           bool mono = false) {
    auto db = ts::synthetic_lens_database();
    EXPECT_TRUE(db);
    auto plan = db->plan(m, setting, upright, orientation, mono);
    EXPECT_TRUE(plan) << (plan ? std::string{} : plan.error().describe());
    return plan ? *plan : LensCorrectionPlan{};
}

LensCorrectionOutcome outcome(const LensCorrectionPlan& p, LensCorrectionKind k) {
    return p.decision(k).outcome;
}

std::string describe(const LensMatch& m) {
    std::string out = std::string(to_string(m.kind));
    for (const std::string& r : m.reasons) {
        out += "\n  " + r;
    }
    return out;
}

// Lensfun itself, on the same XML: the reference of the model.
class Reference {
public:
    explicit Reference(const char* xml) {
        EXPECT_EQ(db_.Load("reference.xml", xml, std::strlen(xml)), LF_NO_ERROR);
    }
    [[nodiscard]] const lfLens* lens(std::string_view model, double crop) const {
        for (const lfLens* const* l = db_.GetLenses(); l != nullptr && *l != nullptr; ++l) {
            if (lf_mlstr_get((*l)->Model) == model && std::abs((*l)->CropFactor - crop) < 0.01) {
                return *l;
            }
        }
        ADD_FAILURE() << "no reference lens " << model;
        return nullptr;
    }

private:
    lfDatabase db_;
};

// An analytic test scene in Lensfun pixel coordinates (pixel centres at integers): smooth, coloured,
// with features of about 30 pixels.
double scene(std::int32_t channel, double u, double v) {
    const double base = 0.45 + 0.3 * std::sin(2.0 * std::numbers::pi * u / 37.0) *
                                   std::cos(2.0 * std::numbers::pi * v / 29.0);
    const double tint = 0.08 * std::sin(2.0 * std::numbers::pi * (u + v) / 53.0);
    return base + (channel == 0 ? tint : channel == 2 ? -tint : 0.0);
}

image::ImageBuffer<float> make_image(
    image::Size size, std::int32_t channels,
    const std::function<double(std::int32_t, std::int32_t, std::int32_t)>& value) {
    auto buffer = image::ImageBuffer<float>::create(size.width, size.height, channels);
    EXPECT_TRUE(buffer);
    for (std::int32_t y = 0; y < size.height; ++y) {
        for (std::int32_t x = 0; x < size.width; ++x) {
            for (std::int32_t c = 0; c < channels; ++c) {
                buffer->at(x, y, c) = static_cast<float>(value(x, y, c));
            }
        }
    }
    return std::move(*buffer);
}

// ---- DevelopSettings version 3 ------------------------------------------------------------------

TEST(LensSettings, DefaultsAreVersion3AutomaticWithEveryCorrection) {
    const DevelopSettings s;
    EXPECT_EQ(s.version, 3u);
    EXPECT_EQ(kDevelopSettingsVersion, 3u);
    EXPECT_EQ(s.operations.lens_correction, kLensCorrectionOperationVersion);
    EXPECT_TRUE(s.lens_correction.distortion);
    EXPECT_TRUE(s.lens_correction.lateral_chromatic_aberration);
    EXPECT_TRUE(s.lens_correction.vignetting);
    EXPECT_FALSE(s.lens_correction.profile.has_value());
    EXPECT_TRUE(s.validate());
    EXPECT_TRUE(s.is_as_shot());
    EXPECT_EQ(lens_correction_operation_version(s), 1u);
    ASSERT_TRUE(effective_lens_correction(s).has_value());
    EXPECT_EQ(*effective_lens_correction(s), LensCorrectionSetting{});
}

TEST(LensSettings, Version2ValuesCorrectNothingAndUpgradeToVersion0) {
    DevelopSettings v2;
    v2.version = 2;
    v2.exposure_ev = 0.4;
    ASSERT_TRUE(v2.validate()) << v2.validate().error().describe();
    EXPECT_EQ(lens_correction_operation_version(v2), 0u);
    EXPECT_FALSE(effective_lens_correction(v2).has_value());
    auto up = upgrade_develop_settings(v2);
    ASSERT_TRUE(up) << up.error().describe();
    EXPECT_EQ(up->version, 3u);
    EXPECT_EQ(up->operations.lens_correction, 0u);
    EXPECT_EQ(up->lens_correction, LensCorrectionSetting{});
    EXPECT_EQ(up->exposure_ev, 0.4);
    EXPECT_EQ(up->operations.geometry, v2.operations.geometry);
    EXPECT_TRUE(up->validate());
    EXPECT_FALSE(effective_lens_correction(*up).has_value());
    // Upgrading is idempotent and keeps an as-shot recipe as shot.
    DevelopSettings as_shot;
    as_shot.version = 2;
    auto up_as_shot = upgrade_develop_settings(as_shot);
    ASSERT_TRUE(up_as_shot);
    EXPECT_TRUE(up_as_shot->is_as_shot());
    auto again = upgrade_develop_settings(*up_as_shot);
    ASSERT_TRUE(again);
    EXPECT_EQ(*again, *up_as_shot);
}

TEST(LensSettings, Version1ValuesUpgradeThroughVersion2) {
    DevelopSettings v1;
    v1.version = 1;
    v1.contrast = 0.2;
    ASSERT_TRUE(v1.validate());
    EXPECT_EQ(lens_correction_operation_version(v1), 0u);
    auto up = upgrade_develop_settings(v1);
    ASSERT_TRUE(up) << up.error().describe();
    EXPECT_EQ(up->version, 3u);
    EXPECT_EQ(up->operations.geometry, kOldestGeometryOperationVersion);
    EXPECT_EQ(up->operations.lens_correction, kOldestLensCorrectionOperationVersion);
    EXPECT_TRUE(up->validate());
}

TEST(LensSettings, OlderValuesCannotHoldALensCorrection) {
    for (const std::uint32_t version : {1u, 2u}) {
        DevelopSettings s;
        s.version = version;
        s.lens_correction.distortion = false;
        ts::expect_error(s.validate(), ErrorCode::InvalidArgument, "engine.develop.settings");
        ts::expect_error(upgrade_develop_settings(s), ErrorCode::InvalidArgument, "engine.develop.settings");
        DevelopSettings named;
        named.version = version;
        named.lens_correction.profile = LensProfileId{"Lucent", "Lucent 50mm f/1.8", 1.0};
        ts::expect_error(named.validate(), ErrorCode::InvalidArgument, "engine.develop.settings");
    }
}

TEST(LensSettings, OperationVersion0KeepsTheDefaultSettingAndNewerVersionsAreRefused) {
    DevelopSettings s;
    s.operations.lens_correction = 0;
    EXPECT_TRUE(s.validate());
    EXPECT_FALSE(effective_lens_correction(s).has_value());
    EXPECT_TRUE(s.is_as_shot());
    s.lens_correction.vignetting = false;
    ts::expect_error(s.validate(), ErrorCode::InvalidArgument, "engine.develop.settings");
    DevelopSettings newer;
    newer.operations.lens_correction = 2;
    ts::expect_error(newer.validate(), ErrorCode::VersionMismatch, "engine.develop.settings");
    DevelopSettings v4;
    v4.version = 4;
    ts::expect_error(v4.validate(), ErrorCode::VersionMismatch, "engine.develop.settings");
}

TEST(LensSettings, ProfileReferencesAreBounded) {
    DevelopSettings s;
    s.lens_correction.profile = LensProfileId{"Lucent", "Lucent 50mm f/1.8", 1.0};
    EXPECT_TRUE(s.validate());
    for (const LensProfileId& bad :
         {LensProfileId{"", "x", 1.0}, LensProfileId{"Lucent", "", 1.0}, LensProfileId{"Lucent", "a\nb", 1.0},
          LensProfileId{"Lucent", std::string(kMaxLensProfileNameBytes + 1, 'x'), 1.0},
          LensProfileId{"Lucent", "x", 0.0}, LensProfileId{"Lucent", "x", std::nan("")},
          LensProfileId{"Lucent", "x", 1000.0}}) {
        s.lens_correction.profile = bad;
        ts::expect_error(s.validate(), ErrorCode::InvalidArgument, "engine.develop.settings");
    }
}

TEST(LensSettings, AChangedLensCorrectionIsAnEdit) {
    DevelopSettings s;
    s.lens_correction.distortion = false;
    EXPECT_FALSE(s.is_as_shot());
    DevelopSettings named;
    named.lens_correction.profile = LensProfileId{"Lucent", "Lucent 50mm f/1.8", 1.0};
    EXPECT_FALSE(named.is_as_shot());
}

// ---- Names and matching ---------------------------------------------------------------------------

TEST(LensMatching, NameWordsFollowLensfunsRules) {
    using W = std::vector<std::string>;
    EXPECT_EQ(lens_name_words("XF16-55mmF2.8 R LM WR"), (W{"16", "2.8", "55", "lm", "mmf", "r", "wr", "xf"}));
    EXPECT_EQ(lens_name_words("Sony FE 50mm f/2.5 G"), (W{"2.5", "50", "fe", "g", "mm", "sony"}));
    EXPECT_EQ(lens_name_words("FE 50mm F2.5 G"), (W{"2.5", "50", "fe", "g", "mm"}));
    EXPECT_EQ(lens_name_words("50mm F1.4 DG HSM | Art 014"),
              (W{"014", "1.4", "50", "art", "dg", "hsm", "mm"}));
    EXPECT_EQ(lens_name_words("  Nikkor   Z 24-70mm f/4 S "), (W{"24", "4", "70", "mm", "nikkor", "s", "z"}));
    EXPECT_EQ(lens_name_words("1.4x+ *"), (W{"*", "+", "1.4", "x"}));
}

TEST(LensMatching, AnExactNameOnAnIdentifiedCameraIsExact) {
    auto db = ts::synthetic_lens_database();
    ASSERT_TRUE(db);
    const LensMatch m = db->match(query_of("Test Camera", "24-70mm F2.8 Test"));
    ASSERT_EQ(m.kind, LensMatchKind::Exact) << describe(m);
    ASSERT_TRUE(m.lens);
    EXPECT_EQ(*m.lens, (LensProfileId{"Lucent", "Lucent 24-70mm f/2.8 Test", 1.0}));
    ASSERT_TRUE(m.camera);
    EXPECT_EQ(m.camera->mount, "Lucent Test Mount");
    EXPECT_EQ(m.camera->crop_factor, 1.0);
    // Spelling: case and runs of white space do not matter (Lensfun's comparison).
    LensQuery q = query_of("TEST   camera", "24-70MM f2.8 test");
    q.camera_make = q.camera_normalized_make = "LUCENT";
    EXPECT_EQ(db->match(q).kind, LensMatchKind::Exact) << describe(db->match(q));
}

TEST(LensMatching, WeakMatchesAreNeverExact) {
    auto db = ts::synthetic_lens_database();
    ASSERT_TRUE(db);
    // Every word of the file's name occurs in two database names, neither is the same name.
    const LensMatch subset = db->match(query_of("Test Camera", "24-70mm F2.8"));
    EXPECT_EQ(subset.kind, LensMatchKind::Fuzzy) << describe(subset);
    EXPECT_FALSE(subset.lens);
    EXPECT_GE(subset.candidates.size(), 2u);
    // Two makers' lenses of the same name, and the file names no maker.
    const LensMatch two = db->match(query_of("Test Camera", "50mm F1.8", 50.0f, 2.0f));
    EXPECT_EQ(two.kind, LensMatchKind::Fuzzy) << describe(two);
    // With the maker the ambiguity is gone.
    const LensMatch one =
        db->match(query_of("Test Camera", "50mm F1.8", 50.0f, 2.0f, std::nullopt, "Lucent"));
    EXPECT_EQ(one.kind, LensMatchKind::Exact) << describe(one);
    // The file names another lens maker than the database's.
    const LensMatch maker =
        db->match(query_of("Test Camera", "24-70mm F2.8 Test", 35.0f, 4.0f, std::nullopt, "Canon"));
    EXPECT_EQ(maker.kind, LensMatchKind::Fuzzy) << describe(maker);
    // An unknown camera: the lens is known but not the mount or the image's sensor.
    LensQuery unknown = query_of("Test Camera", "24-70mm F2.8 Test");
    unknown.camera_make = unknown.camera_normalized_make = "Nobody";
    unknown.camera_model = unknown.camera_normalized_model = "X1";
    EXPECT_EQ(db->match(unknown).kind, LensMatchKind::Fuzzy) << describe(db->match(unknown));
    // A crop mode: the 35 mm equivalent focal length says 1.5, the camera's sensor is 1.0.
    const LensMatch crop = db->match(query_of("Test Camera", "24-70mm F2.8 Test", 50.0f, 4.0f, 75.0f));
    EXPECT_EQ(crop.kind, LensMatchKind::Fuzzy) << describe(crop);
    // Within the tolerance the 35 mm equivalent agrees.
    EXPECT_EQ(db->match(query_of("Test Camera", "24-70mm F2.8 Test", 50.0f, 4.0f, 52.0f)).kind,
              LensMatchKind::Exact);
    // The lens's focal range disagrees with the file's.
    LensQuery range = query_of("Test Camera", "24-70mm F2.8 Test");
    range.lens_min_focal_mm = 24.0;
    range.lens_max_focal_mm = 105.0;
    EXPECT_EQ(db->match(range).kind, LensMatchKind::Fuzzy) << describe(db->match(range));
    range.lens_max_focal_mm = 70.0;
    EXPECT_EQ(db->match(range).kind, LensMatchKind::Exact) << describe(db->match(range));
}

TEST(LensMatching, UnknownLensesAndMissingNamesAreNone) {
    auto db = ts::synthetic_lens_database();
    ASSERT_TRUE(db);
    EXPECT_EQ(db->match(query_of("Test Camera", "Zeiss Otus 85mm f/1.4", 85.0f)).kind, LensMatchKind::None);
    EXPECT_EQ(db->match(query_of("Test Camera", "")).kind, LensMatchKind::None);
    LensQuery nothing;
    EXPECT_EQ(db->match(nothing).kind, LensMatchKind::None);
}

TEST(LensMatching, AFixedLensCameraMatchesItsLens) {
    auto db = ts::synthetic_lens_database();
    ASSERT_TRUE(db);
    const LensMatch m = db->match(query_of("Test Compact", "", 4.5f, 2.0f));
    ASSERT_EQ(m.kind, LensMatchKind::Exact) << describe(m);
    EXPECT_EQ(m.lens->model, "Test Compact fixed lens");
}

// ---- Plans ----------------------------------------------------------------------------------------

TEST(LensPlans, AnExactMatchAppliesEveryCoveredCorrection) {
    const raw::RawMetadata m = lens_metadata("Test Camera", "24-70mm F2.8 Test", 35.0f, 4.0f);
    const LensCorrectionPlan p = plan_of(m, LensCorrectionSetting{}, image::Size{600, 400});
    EXPECT_EQ(p.operation_version, 1u);
    EXPECT_TRUE(p.automatic);
    ASSERT_TRUE(p.profile);
    EXPECT_EQ(p.profile->model, "Lucent 24-70mm f/2.8 Test");
    for (const auto k : {LensCorrectionKind::Distortion, LensCorrectionKind::LateralChromaticAberration,
                         LensCorrectionKind::Vignetting}) {
        EXPECT_EQ(outcome(p, k), LensCorrectionOutcome::Applied) << p.decision(k).detail;
    }
    ASSERT_TRUE(p.geometry);
    ASSERT_TRUE(p.geometry->distortion);
    EXPECT_EQ(p.geometry->distortion->model, LensDistortionModel::PTLens);
    // 35 mm is a calibrated focal length: its terms exactly.
    EXPECT_NEAR(p.geometry->distortion->terms[0], 0.004, 1e-7);
    ASSERT_TRUE(p.geometry->tca);
    EXPECT_EQ(p.geometry->tca->model, LensTcaModel::Poly3);
    ASSERT_TRUE(p.vignetting);
    EXPECT_EQ(p.vignetting->calibration.distance_m, kLensDefaultFocusDistanceM);
    EXPECT_GT(p.geometry->scale, 0.9);
    EXPECT_LT(p.geometry->scale, 1.1);
    EXPECT_EQ(p.crop_factor, 1.0);
    EXPECT_NE(p.revision(), 0u);
}

TEST(LensPlans, CoverageDecidesEachCorrection) {
    // 18 mm lies outside the zoom's calibrated 24 .. 70 mm.
    const LensCorrectionPlan wide = plan_of(lens_metadata("Test Camera", "24-70mm F2.8 Test", 18.0f, 4.0f),
                                            LensCorrectionSetting{}, image::Size{600, 400});
    for (const auto k : {LensCorrectionKind::Distortion, LensCorrectionKind::LateralChromaticAberration,
                         LensCorrectionKind::Vignetting}) {
        EXPECT_EQ(outcome(wide, k), LensCorrectionOutcome::NoCalibration) << wide.decision(k).detail;
    }
    EXPECT_FALSE(wide.any_applied());
    // f/1.4 lies outside the calibrated f/2.8 .. f/8: vignetting only.
    const LensCorrectionPlan fast = plan_of(lens_metadata("Test Camera", "24-70mm F2.8 Test", 50.0f, 1.4f),
                                            LensCorrectionSetting{}, image::Size{600, 400});
    EXPECT_EQ(outcome(fast, LensCorrectionKind::Distortion), LensCorrectionOutcome::Applied);
    EXPECT_EQ(outcome(fast, LensCorrectionKind::Vignetting), LensCorrectionOutcome::NoCalibration);
    // An unknown f-number.
    const LensCorrectionPlan blind =
        plan_of(lens_metadata("Test Camera", "24-70mm F2.8 Test", 50.0f, std::nullopt),
                LensCorrectionSetting{}, image::Size{600, 400});
    EXPECT_EQ(outcome(blind, LensCorrectionKind::Vignetting), LensCorrectionOutcome::NoCalibration);
    EXPECT_NE(blind.decision(LensCorrectionKind::Vignetting).detail.find("unknown"), std::string::npos);
    // A prime's calibration holds at its focal length whatever the file records.
    const LensCorrectionPlan prime =
        plan_of(lens_metadata("Test Camera", "50mm F1.8", 51.6f, 2.0f, std::nullopt, "Lucent"),
                LensCorrectionSetting{}, image::Size{600, 400});
    EXPECT_EQ(outcome(prime, LensCorrectionKind::Distortion), LensCorrectionOutcome::Applied)
        << prime.decision(LensCorrectionKind::Distortion).detail;
    // Disabled corrections stay off.
    LensCorrectionSetting only_vignetting;
    only_vignetting.distortion = false;
    only_vignetting.lateral_chromatic_aberration = false;
    const LensCorrectionPlan v = plan_of(lens_metadata("Test Camera", "24-70mm F2.8 Test", 35.0f, 4.0f),
                                         only_vignetting, image::Size{600, 400});
    EXPECT_EQ(outcome(v, LensCorrectionKind::Distortion), LensCorrectionOutcome::Disabled);
    EXPECT_FALSE(v.geometry);
    EXPECT_TRUE(v.vignetting);
}

TEST(LensPlans, WeakMatchesLeaveEveryCorrectionOffUnlessAProfileIsChosen) {
    const raw::RawMetadata m = lens_metadata("Test Camera", "24-70mm F2.8", 35.0f, 4.0f);
    const LensCorrectionPlan automatic = plan_of(m, LensCorrectionSetting{}, image::Size{600, 400});
    EXPECT_EQ(automatic.match.kind, LensMatchKind::Fuzzy);
    EXPECT_FALSE(automatic.any_applied());
    for (const LensCorrectionDecision& d : automatic.decisions) {
        EXPECT_EQ(d.outcome, LensCorrectionOutcome::NoProfile) << d.detail;
    }
    LensCorrectionSetting named;
    named.profile = LensProfileId{"Lucent", "Lucent 24-70mm f/2.8 Test II", 1.0};
    const LensCorrectionPlan chosen = plan_of(m, named, image::Size{600, 400});
    EXPECT_FALSE(chosen.automatic);
    EXPECT_EQ(outcome(chosen, LensCorrectionKind::Distortion), LensCorrectionOutcome::Applied);
    // That profile has no TCA or vignetting calibration.
    EXPECT_EQ(outcome(chosen, LensCorrectionKind::LateralChromaticAberration),
              LensCorrectionOutcome::NoCalibration);
    EXPECT_EQ(outcome(chosen, LensCorrectionKind::Vignetting), LensCorrectionOutcome::NoCalibration);
    // A profile the database does not hold.
    named.profile = LensProfileId{"Lucent", "Lucent 35mm f/1.4", 1.0};
    const LensCorrectionPlan missing = plan_of(m, named, image::Size{600, 400});
    EXPECT_EQ(outcome(missing, LensCorrectionKind::Distortion), LensCorrectionOutcome::NoProfile);
    // A profile calibrated on a smaller sensor than the image's.
    named.profile = LensProfileId{"Lucent", "Lucent 18mm f/4 Wide", 1.5};
    const LensCorrectionPlan small =
        plan_of(lens_metadata("Test Camera", "18mm F4", 18.0f, 4.0f), named, image::Size{600, 400});
    EXPECT_EQ(outcome(small, LensCorrectionKind::Distortion), LensCorrectionOutcome::NoCalibration)
        << small.decision(LensCorrectionKind::Distortion).detail;
    // A named profile in a crop mode uses the file's crop factor.
    named.profile = LensProfileId{"Lucent", "Lucent 24-70mm f/2.8 Test", 1.0};
    const LensCorrectionPlan crop = plan_of(
        lens_metadata("Test Camera", "24-70mm F2.8 Test", 50.0f, 4.0f, 75.0f), named, image::Size{600, 400});
    EXPECT_EQ(crop.crop_factor, 1.5);
    EXPECT_EQ(outcome(crop, LensCorrectionKind::Distortion), LensCorrectionOutcome::Applied);
}

TEST(LensPlans, CorrectionsTheFileCarriesAreNeverAppliedTwice) {
    // A DNG whose OpcodeList3 already corrects distortion (equal warp sets) and vignetting.
    raw::DecodedRaw d = ts::scene_mosaic(
        64, 48, [](std::int32_t, std::int32_t) { return std::array<double, 3>{0.2, 0.2, 0.2}; });
    raw::DngFixVignetteRadial vignette;
    vignette.k = {0.3, 0.0, 0.0, 0.0, 0.0};
    ts::attach_dng(
        d, {ts::TestOpcode{raw::DngOpcodeListId::List3, 1, ts::warp({{0.99, 0.01, 0.0, 0.0, 0.0, 0.0}})},
            ts::TestOpcode{raw::DngOpcodeListId::List3, 3, vignette}});
    ts::set_lens_identity(d.metadata, "Lucent", "Test Camera", "24-70mm F2.8 Test", 35.0f, 4.0f);
    const CarriedLensCorrections carried = carried_lens_corrections(d.metadata);
    ASSERT_TRUE(carried.distortion);
    ASSERT_FALSE(carried.lateral_chromatic_aberration);
    ASSERT_TRUE(carried.vignetting);
    // Automatic: the carried ones are recorded as such, the rest withheld (no profile is mixed in).
    const LensCorrectionPlan automatic = plan_of(d.metadata, LensCorrectionSetting{}, image::Size{64, 48});
    EXPECT_EQ(automatic.match.kind, LensMatchKind::Exact);
    EXPECT_EQ(outcome(automatic, LensCorrectionKind::Distortion), LensCorrectionOutcome::CarriedByFile);
    EXPECT_EQ(outcome(automatic, LensCorrectionKind::Vignetting), LensCorrectionOutcome::CarriedByFile);
    EXPECT_EQ(outcome(automatic, LensCorrectionKind::LateralChromaticAberration),
              LensCorrectionOutcome::Withheld);
    EXPECT_FALSE(automatic.any_applied());
    // A chosen profile: only what the file does not carry.
    LensCorrectionSetting named;
    named.profile = LensProfileId{"Lucent", "Lucent 24-70mm f/2.8 Test", 1.0};
    const LensCorrectionPlan chosen = plan_of(d.metadata, named, image::Size{64, 48});
    EXPECT_EQ(outcome(chosen, LensCorrectionKind::Distortion), LensCorrectionOutcome::CarriedByFile);
    EXPECT_EQ(outcome(chosen, LensCorrectionKind::Vignetting), LensCorrectionOutcome::CarriedByFile);
    EXPECT_EQ(outcome(chosen, LensCorrectionKind::LateralChromaticAberration),
              LensCorrectionOutcome::Applied);
    ASSERT_TRUE(chosen.geometry);
    EXPECT_FALSE(chosen.geometry->distortion);
    EXPECT_TRUE(chosen.geometry->tca);
    EXPECT_FALSE(chosen.vignetting);
    // Per-plane warp sets correct lateral CA as well.
    raw::DecodedRaw e = ts::scene_mosaic(
        64, 48, [](std::int32_t, std::int32_t) { return std::array<double, 3>{0.2, 0.2, 0.2}; });
    ts::attach_dng(
        e, {ts::TestOpcode{
               raw::DngOpcodeListId::List3, 1,
               ts::warp({{0.99, 0.01, 0, 0, 0, 0}, {0.995, 0.01, 0, 0, 0, 0}, {1.0, 0.01, 0, 0, 0, 0}})}});
    ts::set_lens_identity(e.metadata, "Lucent", "Test Camera", "24-70mm F2.8 Test", 35.0f, 4.0f);
    const LensCorrectionPlan tca = plan_of(e.metadata, named, image::Size{64, 48});
    EXPECT_EQ(outcome(tca, LensCorrectionKind::LateralChromaticAberration),
              LensCorrectionOutcome::CarriedByFile);
    EXPECT_EQ(outcome(tca, LensCorrectionKind::Vignetting), LensCorrectionOutcome::Applied);
}

// A lens-shading GainMap of one Bayer phase (pitch 2), as phones write them: it starts at row and
// column 0 or 1 and stops one row and column short of the image's bottom and right edges.
raw::DngGainMap phase_map(std::uint32_t top, std::uint32_t left, std::uint32_t bottom, std::uint32_t right) {
    raw::DngGainMap g;
    g.area.rect = raw::DngOpcodeRect{top, left, bottom, right};
    g.area.plane = 0;
    g.area.planes = 1;
    g.area.row_pitch = 2;
    g.area.col_pitch = 2;
    g.points_v = 1;
    g.points_h = 1;
    g.map_planes = 1;
    g.gains = {1.0f};
    return g;
}

TEST(LensPlans, PerBayerPhaseGainMapsAreCarriedVignetting) {
    const auto flat = [](std::int32_t, std::int32_t) { return std::array<double, 3>{0.2, 0.2, 0.2}; };
    raw::DecodedRaw d = ts::scene_mosaic(64, 48, flat);
    ts::attach_dng(d, {ts::TestOpcode{raw::DngOpcodeListId::List2, 9, phase_map(0, 0, 47, 63)},
                       ts::TestOpcode{raw::DngOpcodeListId::List2, 9, phase_map(0, 1, 47, 63)},
                       ts::TestOpcode{raw::DngOpcodeListId::List2, 9, phase_map(1, 0, 47, 63)},
                       ts::TestOpcode{raw::DngOpcodeListId::List2, 9, phase_map(1, 1, 47, 63)}});
    const CarriedLensCorrections carried = carried_lens_corrections(d.metadata);
    EXPECT_TRUE(carried.vignetting);
    EXPECT_FALSE(carried.distortion);
    EXPECT_EQ(carried.sources.size(), 4u);
    // A map over a part of the image is not a correction of the lens's vignetting.
    raw::DecodedRaw part = ts::scene_mosaic(64, 48, flat);
    ts::attach_dng(part, {ts::TestOpcode{raw::DngOpcodeListId::List2, 9, phase_map(0, 0, 24, 63)}});
    EXPECT_FALSE(carried_lens_corrections(part.metadata).vignetting);
}

TEST(LensPlans, VersionZeroMonochromeAndNoDatabase) {
    const raw::RawMetadata m = lens_metadata("Test Camera", "24-70mm F2.8 Test", 35.0f, 4.0f);
    const LensCorrectionPlan none = plan_of(m, std::nullopt, image::Size{600, 400});
    EXPECT_EQ(none.operation_version, 0u);
    for (const LensCorrectionDecision& d : none.decisions) {
        EXPECT_EQ(d.outcome, LensCorrectionOutcome::Disabled);
    }
    const LensCorrectionPlan mono =
        plan_of(m, LensCorrectionSetting{}, image::Size{600, 400}, image::Orientation::Normal, true);
    EXPECT_EQ(outcome(mono, LensCorrectionKind::LateralChromaticAberration),
              LensCorrectionOutcome::NotApplicable);
    EXPECT_EQ(outcome(mono, LensCorrectionKind::Distortion), LensCorrectionOutcome::Applied);
    auto no_db = plan_lens_correction(nullptr, m, LensCorrectionSetting{}, image::Size{600, 400},
                                      image::Orientation::Normal, false);
    ASSERT_TRUE(no_db);
    EXPECT_FALSE(no_db->any_applied());
    EXPECT_EQ(no_db->decisions[0].outcome, LensCorrectionOutcome::NoProfile);
}

// W13 closure (LNS-1): a crop mode the camera records (Canon's APS-C crop of a full-frame body records
// no 35 mm equivalent focal length) is never corrected with the whole sensor's profile automatically;
// a named profile is planned for the cropped sensor.
TEST(LensPlans, ACropModeTheCameraRecordsIsNeverCorrectedAutomatically) {
    raw::RawMetadata m = lens_metadata("Test Camera", "24-70mm F2.8 Test", 50.0f, 4.0f);
    raw::SensorCrop crop;
    crop.factor = 1.6;
    crop.basis = "Canon AspectInfo (maker note 0x009a) AspectRatio 13 \"3:2 (APS-C crop)\"";
    m.sensor_crop = crop;
    const LensQuery q = lens_query(m);
    EXPECT_TRUE(q.sensor_crop);
    ASSERT_TRUE(q.sensor_crop_factor);
    EXPECT_DOUBLE_EQ(*q.sensor_crop_factor, 1.6);
    const LensCorrectionPlan automatic = plan_of(m, LensCorrectionSetting{}, image::Size{600, 400});
    EXPECT_EQ(automatic.match.kind, LensMatchKind::Fuzzy) << describe(automatic.match);
    EXPECT_FALSE(automatic.any_applied());
    const bool told = std::any_of(automatic.match.reasons.begin(), automatic.match.reasons.end(),
                                  [](const std::string& r) { return r.find("crop of its sensor") != std::string::npos; });
    EXPECT_TRUE(told) << describe(automatic.match);
    // The same file without the record is the whole sensor, as before (and the reasons say so).
    raw::RawMetadata whole = m;
    whole.sensor_crop.reset();
    EXPECT_EQ(plan_of(whole, LensCorrectionSetting{}, image::Size{600, 400}).match.kind, LensMatchKind::Exact);
    // A named profile is planned for the cropped sensor: the camera's crop factor times the crop's.
    LensCorrectionSetting named;
    named.profile = LensProfileId{"Lucent", "Lucent 24-70mm f/2.8 Test", 1.0};
    const LensCorrectionPlan chosen = plan_of(m, named, image::Size{600, 400});
    EXPECT_DOUBLE_EQ(chosen.crop_factor, 1.6);
    EXPECT_NE(chosen.crop_factor_basis.find("sensor crop"), std::string::npos) << chosen.crop_factor_basis;
    EXPECT_EQ(outcome(chosen, LensCorrectionKind::Distortion), LensCorrectionOutcome::Applied);
    // A recorded 35 mm equivalent gives the crop factor itself.
    raw::RawMetadata with_35mm = m;
    with_35mm.lens.focal_length_35mm_equivalent_mm = 75.0f;
    EXPECT_DOUBLE_EQ(plan_of(with_35mm, named, image::Size{600, 400}).crop_factor, 1.5);
    // A crop of unknown size: the image's crop factor is unknown, nothing is applied.
    raw::RawMetadata unknown = m;
    unknown.sensor_crop->factor.reset();
    const LensCorrectionPlan unsized = plan_of(unknown, named, image::Size{600, 400});
    EXPECT_FALSE(unsized.any_applied());
    EXPECT_EQ(outcome(unsized, LensCorrectionKind::Distortion), LensCorrectionOutcome::NoProfile);
}

// W13 closure (LNS-4): a prime's calibration describes its own focal length; a teleconverter's
// (1.4x or more) is not corrected with the bare lens's calibration.
TEST(LensPlans, APrimeIsNotCorrectedAtATeleconvertersFocalLength) {
    const LensCorrectionPlan converted =
        plan_of(lens_metadata("Test Camera", "50mm F1.8", 70.0f, 2.8f, std::nullopt, "Lucent"),
                LensCorrectionSetting{}, image::Size{600, 400});
    ASSERT_EQ(converted.match.kind, LensMatchKind::Exact) << describe(converted.match);
    for (const auto k : {LensCorrectionKind::Distortion, LensCorrectionKind::LateralChromaticAberration,
                         LensCorrectionKind::Vignetting}) {
        EXPECT_EQ(outcome(converted, k), LensCorrectionOutcome::NoCalibration) << converted.decision(k).detail;
        EXPECT_NE(converted.decision(k).detail.find("teleconverter"), std::string::npos)
            << converted.decision(k).detail;
    }
    EXPECT_FALSE(converted.any_applied());
    // Within 5 % (a file that records 51.6 mm for the 50 mm prime) the prime's calibration applies,
    // vignetting included: it is interpolated for the prime's calibrated focal length.
    const LensCorrectionPlan near =
        plan_of(lens_metadata("Test Camera", "50mm F1.8", 51.6f, 1.8f, std::nullopt, "Lucent"),
                LensCorrectionSetting{}, image::Size{600, 400});
    for (const auto k : {LensCorrectionKind::Distortion, LensCorrectionKind::LateralChromaticAberration,
                         LensCorrectionKind::Vignetting}) {
        EXPECT_EQ(outcome(near, k), LensCorrectionOutcome::Applied) << near.decision(k).detail;
    }
    ASSERT_TRUE(near.vignetting);
    EXPECT_EQ(near.vignetting->calibration.focal_mm, 50.0);
    // A file that records no focal length: the prime's own.
    raw::RawMetadata unrecorded = lens_metadata("Test Camera", "50mm F1.8", 50.0f, 1.8f, std::nullopt, "Lucent");
    unrecorded.capture.focal_length_mm.reset();
    EXPECT_EQ(outcome(plan_of(unrecorded, LensCorrectionSetting{}, image::Size{600, 400}),
                      LensCorrectionKind::Distortion),
              LensCorrectionOutcome::Applied);
}

// W13 closure (LNS-2): without a focus distance, vignetting calibrated at several distances is
// corrected with the calibrated distance that corrects the image's corners least.
TEST(LensPlans, VignettingThatDependsOnTheFocusDistanceCorrectsLeast) {
    const LensCorrectionPlan p =
        plan_of(lens_metadata("Test Camera", "100mm F2.8 Macro", 100.0f, 2.8f, std::nullopt, "Lucent"),
                LensCorrectionSetting{}, image::Size{600, 400});
    ASSERT_EQ(p.match.kind, LensMatchKind::Exact) << describe(p.match);
    ASSERT_EQ(outcome(p, LensCorrectionKind::Vignetting), LensCorrectionOutcome::Applied)
        << p.decision(LensCorrectionKind::Vignetting).detail;
    ASSERT_TRUE(p.vignetting);
    const LensVignettingCalibration& c = p.vignetting->calibration;
    EXPECT_NEAR(c.distance_m, 0.3, 1e-6);  // the 0.3 m entry (Lensfun stores floats)
    EXPECT_NEAR(c.terms[0], -0.21, 1e-6);
    EXPECT_EQ(c.calibrated_distances_m.size(), 2u);
    EXPECT_NE(p.decision(LensCorrectionKind::Vignetting).detail.find("corrects the image's corners least"),
              std::string::npos)
        << p.decision(LensCorrectionKind::Vignetting).detail;
    // Its corner gain is the smaller one: the infinity calibration would brighten the corners more.
    LensVignettingCorrection infinity = *p.vignetting;
    infinity.calibration.terms = {-0.52, 0.08, -0.02};
    const image::PointD corner{0.0, 0.0};
    EXPECT_LT(p.vignetting->gain_at(corner), infinity.gain_at(corner));
    // A profile calibrated at one distance keeps Lensfun's "infinity" interpolation.
    const LensCorrectionPlan single =
        plan_of(lens_metadata("Test Camera", "24-70mm F2.8 Test", 35.0f, 4.0f), LensCorrectionSetting{},
                image::Size{600, 400});
    ASSERT_TRUE(single.vignetting);
    EXPECT_EQ(single.vignetting->calibration.distance_m, kLensDefaultFocusDistanceM);
}

// W13 closure (LNS-3): settings that ask for a correction a missing dependency prevents say so.
TEST(LensPlans, AMissingDependencyIsReportedNotSilent) {
    const raw::RawMetadata m = lens_metadata("Test Camera", "24-70mm F2.8 Test", 35.0f, 4.0f);
    EXPECT_FALSE(plan_of(m, LensCorrectionSetting{}, image::Size{600, 400}).missing_dependency);
    LensCorrectionSetting named;
    named.profile = LensProfileId{"Lucent", "Lucent 35mm f/1.4", 1.0};
    const LensCorrectionPlan gone = plan_of(m, named, image::Size{600, 400});
    ASSERT_TRUE(gone.missing_dependency);
    EXPECT_NE(gone.missing_dependency->find("not in the lens database"), std::string::npos) << *gone.missing_dependency;
    auto no_db = plan_lens_correction(nullptr, m, LensCorrectionSetting{}, image::Size{600, 400},
                                      image::Orientation::Normal, false);
    ASSERT_TRUE(no_db);
    ASSERT_TRUE(no_db->missing_dependency);
    // Settings that ask for nothing miss nothing: version 0, or every correction switched off.
    auto old = plan_lens_correction(nullptr, m, std::nullopt, image::Size{600, 400}, image::Orientation::Normal, false);
    ASSERT_TRUE(old);
    EXPECT_FALSE(old->missing_dependency);
    LensCorrectionSetting off;
    off.distortion = off.lateral_chromatic_aberration = off.vignetting = false;
    auto none = plan_lens_correction(nullptr, m, off, image::Size{600, 400}, image::Orientation::Normal, false);
    ASSERT_TRUE(none);
    EXPECT_FALSE(none->missing_dependency);
    // A profile miss of the automatic match is not a missing dependency (the blueprint allows it).
    EXPECT_FALSE(plan_of(lens_metadata("Test Camera", "Zeiss Otus 85mm f/1.4", 85.0f, 4.0f), LensCorrectionSetting{},
                         image::Size{600, 400})
                     .missing_dependency);
}

// W13 closure (LNS-5): Lensfun's process-wide state is guarded by one lock, so several databases can
// be loaded, searched, planned with and destroyed on several threads at once.
TEST(LensDatabaseThreads, SeveralDatabasesWorkConcurrently) {
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&failures, t] {
            for (int round = 0; round < 6; ++round) {
                const std::vector<LensDatabaseFile> files{{"synthetic.xml", ts::kSyntheticLensDatabase}};
                auto db = LensDatabase::load(files, LensDatabaseIdentity{"test", "0.0.1", "threads"});
                if (!db || (*db)->info().lenses == 0) {
                    ++failures;
                    continue;
                }
                for (int i = 0; i < 20; ++i) {
                    const float focal = 24.0f + static_cast<float>((i * 7 + t * 3 + round) % 46);
                    const LensQuery q = query_of("Test Camera", "24-70mm F2.8 Test", focal, 4.0f);
                    if ((*db)->match(q).kind != LensMatchKind::Exact) {
                        ++failures;
                    }
                    auto plan = (*db)->plan(lens_metadata("Test Camera", "24-70mm F2.8 Test", focal, 4.0f),
                                            LensCorrectionSetting{}, image::Size{300, 200},
                                            image::Orientation::Normal, false);
                    if (!plan || !plan->any_applied()) {
                        ++failures;
                    }
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(failures.load(), 0);
}

// ---- The model against Lensfun ---------------------------------------------------------------------

struct ModelCase {
    const char* camera;
    const char* lens_name;  // the file's lens name
    const char* lens_make;
    const char* db_model;  // the reference lens
    double db_crop;
    float focal;
    float f_number;
    image::Size stored;
    image::Orientation orientation;
};

const std::vector<ModelCase>& model_cases() {
    static const std::vector<ModelCase> cases = {
        {"Test Camera",
         "24-70mm F2.8 Test",
         "",
         "Lucent 24-70mm f/2.8 Test",
         1.0,
         35.0f,
         4.0f,
         {600, 400},
         image::Orientation::Normal},
        {"Test Camera",
         "24-70mm F2.8 Test",
         "",
         "Lucent 24-70mm f/2.8 Test",
         1.0,
         29.0f,
         2.8f,
         {601, 399},
         image::Orientation::Rotate90Cw},
        {"Test Camera",
         "50mm F1.8",
         "Lucent",
         "Lucent 50mm f/1.8",
         1.0,
         50.0f,
         1.8f,
         {450, 300},
         image::Orientation::Normal},
        {"Test Camera APS-C",
         "18mm F4 Wide",
         "Lucent",
         "Lucent 18mm f/4 Wide",
         1.5,
         18.0f,
         4.0f,
         {480, 320},
         image::Orientation::Rotate180},
        {"Test Four Thirds",
         "12mm F2",
         "Lucent",
         "Lucent 12mm f/2",
         2.0,
         12.0f,
         2.0f,
         {400, 300},
         image::Orientation::Rotate90Ccw},
    };
    return cases;
}

TEST(LensModel, SourcePositionsAndGainsAreLensfuns) {
    const Reference reference(ts::kSyntheticLensDatabase);
    for (const ModelCase& c : model_cases()) {
        SCOPED_TRACE(c.db_model);
        const raw::RawMetadata m =
            lens_metadata(c.camera, c.lens_name, c.focal, c.f_number, std::nullopt, c.lens_make);
        const image::Size upright = image::oriented_size(c.stored, c.orientation);
        const LensCorrectionPlan p = plan_of(m, LensCorrectionSetting{}, upright, c.orientation);
        ASSERT_EQ(p.match.kind, LensMatchKind::Exact) << describe(p.match);
        ASSERT_TRUE(p.geometry);
        const lfLens* lens = reference.lens(c.db_model, c.db_crop);
        ASSERT_NE(lens, nullptr);
        const LensGeometryMap& map = *p.geometry;
        EXPECT_EQ(map.frame.size, c.stored);
        int flags = LF_MODIFY_DISTORTION | (map.tca ? LF_MODIFY_TCA : 0);
        // Lensfun's automatic scale for the same callbacks.
        {
            lfModifier automatic(lens, static_cast<float>(p.crop_factor), c.stored.width, c.stored.height);
            ASSERT_EQ(automatic.Initialize(lens, LF_PF_F32, c.focal, c.f_number, 1000.0f, 1.0f, lens->Type,
                                           flags, false),
                      flags);
            EXPECT_EQ(map.lensfun_scale, static_cast<double>(automatic.GetAutoScale(false)));
        }
        if (lens->CenterX == 0.0f && lens->CenterY == 0.0f) {
            // Lensfun's scale fills a frame symmetric about the lens centre.
            EXPECT_EQ(map.scale, map.lensfun_scale);
        } else {
            // Lensfun's eight test points take the frame to be symmetric about the lens centre: the
            // scale is raised (slightly) so that the whole boundary reads inside the image.
            EXPECT_GE(map.scale, map.lensfun_scale);
            EXPECT_LT(map.scale, map.lensfun_scale * 1.01);
        }
        lfModifier modifier(lens, static_cast<float>(p.crop_factor), c.stored.width, c.stored.height);
        const int wanted = flags | (map.scale != 1.0 ? LF_MODIFY_SCALE : 0);
        ASSERT_EQ(modifier.Initialize(lens, LF_PF_F32, c.focal, c.f_number, 1000.0f,
                                      static_cast<float>(map.scale), lens->Type, wanted, false),
                  wanted);
        lfModifier vignetting(lens, static_cast<float>(p.crop_factor), c.stored.width, c.stored.height);
        ASSERT_TRUE(p.vignetting);
        ASSERT_EQ(vignetting.Initialize(lens, LF_PF_F32, c.focal, c.f_number, 1000.0f, 1.0f, lens->Type,
                                        LF_MODIFY_VIGNETTING, false),
                  LF_MODIFY_VIGNETTING);
        double worst_position = 0.0;
        double worst_gain = 0.0;
        for (int j = 0; j <= 12; ++j) {
            for (int i = 0; i <= 12; ++i) {
                // Lensfun pixel coordinates (centres at integers) of the stored image, corners included.
                const double xl = (c.stored.width - 1) * i / 12.0;
                const double yl = (c.stored.height - 1) * j / 12.0;
                float res[6];
                ASSERT_TRUE(modifier.ApplySubpixelGeometryDistortion(static_cast<float>(xl),
                                                                     static_cast<float>(yl), 1, 1, res));
                const image::PointD upright_point =
                    image::map_to_oriented(image::PointD{xl + 0.5, yl + 0.5}, c.stored, c.orientation);
                for (std::uint32_t ch = 0; ch < 3; ++ch) {
                    const image::PointD s =
                        image::map_from_oriented(map.source_of(upright_point, ch), c.stored, c.orientation);
                    worst_position = std::max({worst_position, std::abs(s.x - 0.5 - res[2 * ch]),
                                               std::abs(s.y - 0.5 - res[2 * ch + 1])});
                }
                alignas(16) float pixel[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                ASSERT_TRUE(vignetting.ApplyColorModification(
                    pixel, static_cast<float>(xl), static_cast<float>(yl), 1, 1, LF_CR_1(INTENSITY),
                    static_cast<int>(4 * sizeof(float))));
                const double gain = p.vignetting->gain_at(upright_point);
                worst_gain = std::max(worst_gain, std::abs(gain / pixel[0] - 1.0));
            }
        }
        std::cout << "[lens model] " << c.db_model << " " << c.stored.width << "x" << c.stored.height
                  << ": worst position difference to lfModifier " << worst_position << " px, gain "
                  << worst_gain << " relative, scale " << map.scale << "\n";
        EXPECT_LT(worst_position, 0.01);
        EXPECT_LT(worst_gain, 2e-6);
    }
}

// Why Lucent evaluates Lensfun's model itself: lfModifier's row API steps its normalised position by
// adding NormScale in float, so along a 45 MP row the position drifts from the one a single-point
// call (or Lucent's double evaluation) gives. Measured, not gated (it is Lensfun's behaviour).
TEST(LensModel, LensfunsRowApiDriftsAlongALongRow) {
    const Reference reference(ts::kSyntheticLensDatabase);
    const lfLens* lens = reference.lens("Lucent 24-70mm f/2.8 Test", 1.0);
    ASSERT_NE(lens, nullptr);
    constexpr int kWidth = 8192;
    constexpr int kHeight = 5464;
    lfModifier modifier(lens, 1.0f, kWidth, kHeight);
    ASSERT_EQ(modifier.Initialize(lens, LF_PF_F32, 24.0f, 2.8f, 1000.0f, 1.0f, lens->Type,
                                  LF_MODIFY_DISTORTION, false),
              LF_MODIFY_DISTORTION);
    std::vector<float> row(static_cast<std::size_t>(kWidth) * 2);
    const float y = 100.0f;
    ASSERT_TRUE(modifier.ApplyGeometryDistortion(0.0f, y, kWidth, 1, row.data()));
    double worst = 0.0;
    for (int x = 0; x < kWidth; x += 7) {
        float one[2];
        ASSERT_TRUE(modifier.ApplyGeometryDistortion(static_cast<float>(x), y, 1, 1, one));
        worst = std::max({worst, std::abs(static_cast<double>(row[2 * x]) - one[0]),
                          std::abs(static_cast<double>(row[2 * x + 1]) - one[1])});
    }
    std::cout << "[lensfun] a " << kWidth << "-pixel row of ApplyGeometryDistortion drifts up to " << worst
              << " px from single-point calls\n";
    SUCCEED();
}

TEST(LensModel, TheInverseAndTheMappingRoundTrip) {
    for (const ModelCase& c : model_cases()) {
        SCOPED_TRACE(c.db_model);
        const raw::RawMetadata m =
            lens_metadata(c.camera, c.lens_name, c.focal, c.f_number, std::nullopt, c.lens_make);
        const image::Size upright = image::oriented_size(c.stored, c.orientation);
        const LensCorrectionPlan p = plan_of(m, LensCorrectionSetting{}, upright, c.orientation);
        ASSERT_TRUE(p.geometry);
        double worst = 0.0;
        for (int j = 0; j <= 10; ++j) {
            for (int i = 0; i <= 10; ++i) {
                const image::PointD q{upright.width * i / 10.0, upright.height * j / 10.0};
                const auto back = p.geometry->corrected_of(p.geometry->source_of(q, 1));
                ASSERT_TRUE(back);
                worst = std::max({worst, std::abs(back->x - q.x), std::abs(back->y - q.y)});
            }
        }
        EXPECT_LT(worst, 1e-6);
        // Through GeometryMapping at a proxy scale, with a crop and a turn.
        GeometrySetting g;
        g.crop = CropRect{0.1, 0.05, 0.9, 0.95};
        g.quarter_turns = 1;
        const image::Size proxy{upright.width / 2, upright.height / 2};
        auto mapping =
            GeometryMapping::create(g, proxy, image::PixelRect::from_size(proxy), *p.geometry, upright);
        ASSERT_TRUE(mapping);
        EXPECT_FALSE(mapping->identity());
        double worst_mapping = 0.0;
        for (const image::PointD o :
             {image::PointD{3.5, 7.25}, image::PointD{40.0, 20.0}, image::PointD{80.5, 100.5}}) {
            const image::PointD u = mapping->upright_from_output(o);
            const image::PointD again = mapping->output_from_upright(u);
            worst_mapping = std::max({worst_mapping, std::abs(again.x - o.x), std::abs(again.y - o.y)});
        }
        EXPECT_LT(worst_mapping, 1e-6);
    }
}

TEST(LensModel, TheAutomaticScaleFillsTheFrame) {
    for (const ModelCase& c : model_cases()) {
        SCOPED_TRACE(c.db_model);
        const raw::RawMetadata m =
            lens_metadata(c.camera, c.lens_name, c.focal, c.f_number, std::nullopt, c.lens_make);
        const image::Size upright = image::oriented_size(c.stored, c.orientation);
        const LensCorrectionPlan p = plan_of(m, LensCorrectionSetting{}, upright, c.orientation);
        ASSERT_TRUE(p.geometry);
        // Every boundary point of the corrected frame takes its content from inside the image
        // (Lensfun checks eight points and keeps a 1 per mille margin; this checks every pixel).
        double beyond = 0.0;
        const auto check = [&](double x, double y) {
            for (std::uint32_t ch = 0; ch < 3; ++ch) {
                const image::PointD s = p.geometry->source_of(image::PointD{x, y}, ch);
                beyond = std::max({beyond, -s.x, s.x - upright.width, -s.y, s.y - upright.height});
            }
        };
        for (int x = 0; x <= upright.width; ++x) {
            check(x, 0.0);
            check(x, upright.height);
        }
        for (int y = 0; y <= upright.height; ++y) {
            check(0.0, y);
            check(upright.width, y);
        }
        EXPECT_LE(beyond, 0.05) << "a corrected edge reads " << beyond << " px beyond the image";
    }
}

// ---- Round trips: Lensfun distorts, Lucent corrects -------------------------------------------------

struct RoundTrip {
    double worst = 0.0;
    double mean = 0.0;
    std::uint64_t compared = 0;
};

// Compares the corrected image with the scene at every pixel whose sources lie at least `margin`
// pixels inside the image.
RoundTrip compare(const image::ImageBuffer<float>& corrected, const LensCorrectionPlan& p, double margin) {
    RoundTrip r;
    const image::Size size = corrected.size();
    double sum = 0.0;
    for (std::int32_t y = 0; y < size.height; ++y) {
        for (std::int32_t x = 0; x < size.width; ++x) {
            bool inside = true;
            if (p.geometry) {
                for (std::uint32_t ch = 0; ch < 3; ++ch) {
                    const image::PointD s = p.geometry->source_of(image::PointD{x + 0.5, y + 0.5}, ch);
                    inside = inside && s.x >= margin && s.y >= margin && s.x <= size.width - margin &&
                             s.y <= size.height - margin;
                }
            }
            if (!inside) {
                continue;
            }
            for (std::int32_t c = 0; c < corrected.channels(); ++c) {
                const double e =
                    std::abs(corrected.at(x, y, c) - scene(corrected.channels() == 1 ? 1 : c, x, y));
                r.worst = std::max(r.worst, e);
                sum += e;
                ++r.compared;
            }
        }
    }
    r.mean = r.compared > 0 ? sum / static_cast<double>(r.compared) : 0.0;
    return r;
}

TEST(LensRoundTrips, DistortionThroughLensfunsReverseModifier) {
    const Reference reference(ts::kSyntheticLensDatabase);
    const image::Size size{300, 200};
    LensCorrectionSetting distortion_only;
    distortion_only.lateral_chromatic_aberration = false;
    distortion_only.vignetting = false;
    for (const ModelCase& c : model_cases()) {
        if (c.orientation != image::Orientation::Normal) {
            continue;  // the round trip is about the model; orientations are covered above
        }
        SCOPED_TRACE(c.db_model);
        const raw::RawMetadata m =
            lens_metadata(c.camera, c.lens_name, c.focal, c.f_number, std::nullopt, c.lens_make);
        const LensCorrectionPlan p = plan_of(m, distortion_only, size);
        ASSERT_TRUE(p.geometry);
        const lfLens* lens = reference.lens(c.db_model, c.db_crop);
        lfModifier reverse(lens, static_cast<float>(p.crop_factor), size.width, size.height);
        const int flags = LF_MODIFY_DISTORTION | LF_MODIFY_SCALE;
        ASSERT_EQ(reverse.Initialize(lens, LF_PF_F32, c.focal, c.f_number, 1000.0f,
                                     static_cast<float>(p.geometry->scale), lens->Type, flags, true),
                  flags);
        // The distorted photograph: each sensor pixel shows the scene at Lensfun's undistorted position.
        image::ImageBuffer<float> photo =
            make_image(size, 3, [&](std::int32_t x, std::int32_t y, std::int32_t ch) {
                float res[2];
                reverse.ApplyGeometryDistortion(static_cast<float>(x), static_cast<float>(y), 1, 1, res);
                return scene(ch, res[0], res[1]);
            });
        auto record = apply_lens_correction(p, photo, size);
        ASSERT_TRUE(record) << record.error().describe();
        EXPECT_TRUE(record->resampled);
        const RoundTrip r = compare(photo, p, 3.0);
        std::cout << "[distortion round trip] " << c.db_model << ": worst " << r.worst << ", mean " << r.mean
                  << " over " << r.compared << " samples\n";
        EXPECT_GT(r.compared, 100000u);
        EXPECT_LT(r.worst, 4e-3);
        EXPECT_LT(r.mean, 4e-4);
    }
}

TEST(LensRoundTrips, LateralChromaticAberrationAndDistortionPerChannel) {
    const Reference reference(ts::kSyntheticLensDatabase);
    const image::Size size{240, 160};
    LensCorrectionSetting geometry_only;
    geometry_only.vignetting = false;
    for (const ModelCase& c : model_cases()) {
        if (c.orientation != image::Orientation::Normal) {
            continue;
        }
        SCOPED_TRACE(c.db_model);
        const raw::RawMetadata m =
            lens_metadata(c.camera, c.lens_name, c.focal, c.f_number, std::nullopt, c.lens_make);
        const LensCorrectionPlan p = plan_of(m, geometry_only, size);
        ASSERT_TRUE(p.geometry);
        ASSERT_TRUE(p.geometry->tca);
        const lfLens* lens = reference.lens(c.db_model, c.db_crop);
        lfModifier forward(lens, static_cast<float>(p.crop_factor), size.width, size.height);
        const int flags = LF_MODIFY_DISTORTION | LF_MODIFY_TCA | LF_MODIFY_SCALE;
        ASSERT_EQ(forward.Initialize(lens, LF_PF_F32, c.focal, c.f_number, 1000.0f,
                                     static_cast<float>(p.geometry->scale), lens->Type, flags, false),
                  flags);
        // Lensfun's forward model inverted numerically per channel: the corrected position whose
        // channel `ch` source is the sensor pixel (x, y).
        const auto undistorted = [&](double x, double y, std::int32_t ch) {
            double u = x;
            double v = y;
            for (int k = 0; k < 30; ++k) {
                float res[6];
                forward.ApplySubpixelGeometryDistortion(static_cast<float>(u), static_cast<float>(v), 1, 1,
                                                        res);
                u += x - res[2 * ch];
                v += y - res[2 * ch + 1];
            }
            return image::PointD{u, v};
        };
        image::ImageBuffer<float> photo =
            make_image(size, 3, [&](std::int32_t x, std::int32_t y, std::int32_t ch) {
                const image::PointD q = undistorted(x, y, ch);
                return scene(ch, q.x, q.y);
            });
        auto record = apply_lens_correction(p, photo, size);
        ASSERT_TRUE(record) << record.error().describe();
        const RoundTrip r = compare(photo, p, 3.0);
        std::cout << "[TCA round trip] " << c.db_model << ": worst " << r.worst << ", mean " << r.mean
                  << "\n";
        EXPECT_LT(r.worst, 4e-3);
        EXPECT_LT(r.mean, 4e-4);
    }
}

TEST(LensRoundTrips, VignettingThroughLensfunsReverseModifier) {
    const Reference reference(ts::kSyntheticLensDatabase);
    const image::Size size{300, 200};
    LensCorrectionSetting vignetting_only;
    vignetting_only.distortion = false;
    vignetting_only.lateral_chromatic_aberration = false;
    for (const ModelCase& c : model_cases()) {
        if (c.orientation != image::Orientation::Normal) {
            continue;
        }
        SCOPED_TRACE(c.db_model);
        const raw::RawMetadata m =
            lens_metadata(c.camera, c.lens_name, c.focal, c.f_number, std::nullopt, c.lens_make);
        const LensCorrectionPlan p = plan_of(m, vignetting_only, size);
        ASSERT_FALSE(p.geometry);
        ASSERT_TRUE(p.vignetting);
        const lfLens* lens = reference.lens(c.db_model, c.db_crop);
        lfModifier reverse(lens, static_cast<float>(p.crop_factor), size.width, size.height);
        ASSERT_EQ(reverse.Initialize(lens, LF_PF_F32, c.focal, c.f_number, 1000.0f, 1.0f, lens->Type,
                                     LF_MODIFY_VIGNETTING, true),
                  LF_MODIFY_VIGNETTING);
        // Lensfun darkens the scene as the lens would, row by row on interleaved RGB floats.
        image::ImageBuffer<float> photo = make_image(
            size, 3, [&](std::int32_t x, std::int32_t y, std::int32_t ch) { return scene(ch, x, y); });
        std::vector<float> row(static_cast<std::size_t>(size.width) * 3 + 4);
        for (std::int32_t y = 0; y < size.height; ++y) {
            std::copy(photo.row(y), photo.row(y) + size.width * 3, row.begin());
            // One pixel per call: Lensfun's row loop accumulates its position in float.
            for (std::int32_t x = 0; x < size.width; ++x) {
                alignas(16) float px[4] = {row[3 * x], row[3 * x + 1], row[3 * x + 2], 0.0f};
                reverse.ApplyColorModification(px, static_cast<float>(x), static_cast<float>(y), 1, 1,
                                               LF_CR_3(RED, GREEN, BLUE),
                                               static_cast<int>(4 * sizeof(float)));
                photo.at(x, y, 0) = px[0];
                photo.at(x, y, 1) = px[1];
                photo.at(x, y, 2) = px[2];
            }
        }
        auto record = apply_lens_correction(p, photo, size);
        ASSERT_TRUE(record) << record.error().describe();
        EXPECT_FALSE(record->resampled);
        const RoundTrip r = compare(photo, p, 0.0);
        std::cout << "[vignetting round trip] " << c.db_model << ": worst " << r.worst << "\n";
        EXPECT_LT(r.worst, 2e-6);
    }
}

TEST(LensRoundTrips, AProxyIsCorrectedLikeTheFullImage) {
    // The proxy is the box downscale of the photograph; its correction against the box downscale of
    // the full-resolution correction (they do not commute exactly: interpolation and averaging).
    const image::Size full{600, 400};
    const raw::RawMetadata m = lens_metadata("Test Camera", "24-70mm F2.8 Test", 24.0f, 2.8f);
    const LensCorrectionPlan p = plan_of(m, LensCorrectionSetting{}, full);
    ASSERT_TRUE(p.geometry);
    image::ImageBuffer<float> photo = make_image(
        full, 3,
        [&](std::int32_t x, std::int32_t y, std::int32_t ch) { return scene(ch, x * 0.5, y * 0.5); });
    const image::Size proxy = proxy_size(full, 300);
    auto small = image::ImageBuffer<float>::create(proxy.width, proxy.height, 3);
    ASSERT_TRUE(small);
    ASSERT_TRUE(box_downscale(photo.const_view(), small->view()));
    ASSERT_TRUE(apply_lens_correction(p, photo, full));
    ASSERT_TRUE(apply_lens_correction(p, *small, full));
    auto reference_small = image::ImageBuffer<float>::create(proxy.width, proxy.height, 3);
    ASSERT_TRUE(reference_small);
    ASSERT_TRUE(box_downscale(photo.const_view(), reference_small->view()));
    double worst = 0.0;
    double sum = 0.0;
    std::uint64_t n = 0;
    for (std::int32_t y = 4; y < proxy.height - 4; ++y) {
        for (std::int32_t x = 4; x < proxy.width - 4; ++x) {
            for (std::int32_t c = 0; c < 3; ++c) {
                const double e = std::abs(small->at(x, y, c) - reference_small->at(x, y, c));
                worst = std::max(worst, e);
                sum += e;
                ++n;
            }
        }
    }
    std::cout << "[proxy] corrected proxy against the downscaled correction: worst " << worst << ", mean "
              << sum / static_cast<double>(n) << " (values 0.1 .. 0.9)\n";
    EXPECT_LT(worst, 0.02);
    EXPECT_LT(sum / static_cast<double>(n), 2e-3);
}

// ---- Stage two ---------------------------------------------------------------------------------------

// A textured scene with a lens identity the synthetic database matches exactly at 24 mm f/2.8 (the
// strongest calibration: about 3 pixels of distortion at the corners of 240 x 160).
raw::DecodedRaw lens_scene(std::int32_t width = 240, std::int32_t height = 160) {
    raw::DecodedRaw d = ts::scene_mosaic(width, height, [](std::int32_t x, std::int32_t y) {
        const double b = 0.08 + 0.12 * (0.5 + 0.5 * std::sin(0.21 * x) * std::cos(0.17 * y));
        return std::array<double, 3>{0.9 * b, b, 0.7 * b};
    });
    ts::set_lens_identity(d.metadata, "Lucent", "Test Camera", "24-70mm F2.8 Test", 24.0f, 2.8f);
    return d;
}

StageOneImage stage_one_of(raw::DecodedRaw&& d, std::int32_t proxy = 0) {
    StageOneRequest request;
    request.proxy_long_edge = proxy;
    auto one = render_stage_one(std::move(d), request, DevelopOptions{});
    EXPECT_TRUE(one) << one.error().describe();
    return std::move(*one);
}

DevelopOptions with_database(bool parallel = true) {
    DevelopOptions o;
    o.lens_database = ts::synthetic_lens_database();
    o.parallel = parallel;
    return o;
}

DevelopSettings masked_settings() {
    DevelopSettings s;
    s.exposure_ev = 0.2;
    s.chroma_denoise.strength = 0.5;
    s.chroma_denoise.radius = 3.0;
    LocalAdjustment radial;
    radial.id = 1;
    RadialGradientMask r;
    r.center = {0.8, 0.75};
    r.radius_x = 0.05;
    r.radius_y = 0.05;
    r.feather = 1.0;
    radial.shape = r;
    radial.controls.exposure_ev = 1.0;
    LocalAdjustment brush;
    brush.id = 2;
    BrushStroke stroke;
    stroke.points = {{0.15, 0.2}};
    stroke.radius = 0.04;
    stroke.hardness = 0.0;
    brush.shape = BrushMask{{stroke}};
    brush.controls.exposure_ev = -1.0;
    s.local_adjustments = {radial, brush};
    s.geometry.crop = CropRect{0.02, 0.03, 0.97, 0.98};
    return s;
}

TEST(LensStageTwo, RecordsTheCorrectionFirstAndAppliesIt) {
    const StageOneImage one = stage_one_of(lens_scene());
    auto with = render_stage_two(one, DevelopSettings{}, with_database());
    ASSERT_TRUE(with) << with.error().describe();
    ASSERT_EQ(with->report.operations.size(), 13u);
    EXPECT_EQ(with->report.operations.front().operation, DevelopOperation::LensCorrection);
    EXPECT_TRUE(with->report.operations.front().applied);
    EXPECT_EQ(with->report.operations[1].operation, DevelopOperation::WhiteBalance);
    const LensCorrectionRecord& lens = with->report.lens_correction;
    EXPECT_TRUE(lens.plan.any_applied());
    EXPECT_TRUE(lens.resampled);
    ASSERT_TRUE(lens.plan.database);
    EXPECT_EQ(lens.plan.database->asset_id, "lens-database-test");
    auto without = render_stage_two(one, DevelopSettings{}, DevelopOptions{});
    ASSERT_TRUE(without);
    EXPECT_FALSE(without->report.lens_correction.plan.any_applied());
    EXPECT_FALSE(without->report.operations.front().applied);
    EXPECT_GT(ts::max_abs_difference(with->display.pixels, without->display.pixels), 1e-3);
    // The report's mapping includes the correction.
    auto mapping = geometry_mapping(with->report);
    ASSERT_TRUE(mapping);
    EXPECT_TRUE(mapping->lens().has_value());
    auto plain = geometry_mapping(without->report);
    ASSERT_TRUE(plain);
    EXPECT_FALSE(plain->lens().has_value());
    // The overload with options plans the same correction.
    auto planned = geometry_mapping(DevelopSettings{}, one, with_database());
    ASSERT_TRUE(planned);
    ASSERT_TRUE(planned->lens());
    EXPECT_EQ(planned->revision(), mapping->revision());
}

TEST(LensStageTwo, Version2SettingsRenderBitIdenticallyWithADatabase) {
    DevelopSettings v2 = masked_settings();
    v2.version = 2;
    ASSERT_TRUE(v2.validate()) << v2.validate().error().describe();
    auto up = upgrade_develop_settings(v2);
    ASSERT_TRUE(up);
    for (const std::int32_t proxy : {0, 120}) {
        SCOPED_TRACE(proxy);
        const StageOneImage one = stage_one_of(lens_scene(), proxy);
        auto plain = render_stage_two(one, v2, DevelopOptions{});
        auto with_db = render_stage_two(one, v2, with_database());
        auto upgraded = render_stage_two(one, *up, with_database());
        ASSERT_TRUE(plain && with_db && upgraded);
        EXPECT_TRUE(ts::bit_identical(plain->display.pixels, with_db->display.pixels));
        EXPECT_TRUE(ts::bit_identical(plain->display.pixels, upgraded->display.pixels));
        EXPECT_EQ(with_db->report.lens_correction.plan.operation_version, 0u);
        EXPECT_FALSE(with_db->report.operations.front().applied);
    }
    // The export render too (it consumes stage one).
    auto exported = render_develop(lens_scene(), v2, DevelopOptions{});
    auto exported_db = render_develop(lens_scene(), v2, with_database());
    ASSERT_TRUE(exported && exported_db);
    EXPECT_TRUE(ts::bit_identical(exported->display.pixels, exported_db->display.pixels));
}

// The output position of a radial mask's effect: the weighted centroid of how much brighter the
// masked render is, around `around` (output pixels).
image::PointD effect_centroid(const image::ImageBuffer<float>& masked, const image::ImageBuffer<float>& plain,
                              image::PointD around, double window) {
    double sx = 0.0;
    double sy = 0.0;
    double sw = 0.0;
    for (std::int32_t y = 0; y < masked.height(); ++y) {
        for (std::int32_t x = 0; x < masked.width(); ++x) {
            if (std::abs(x + 0.5 - around.x) > window || std::abs(y + 0.5 - around.y) > window) {
                continue;
            }
            const double w = std::max(0.0, static_cast<double>(masked.at(x, y, 1)) - plain.at(x, y, 1));
            sx += w * (x + 0.5);
            sy += w * (y + 0.5);
            sw += w;
        }
    }
    return sw > 0.0 ? image::PointD{sx / sw, sy / sw} : image::PointD{0.0, 0.0};
}

TEST(LensStageTwo, MasksStayOnTheirContentWhenACorrectionIsToggled) {
    // A flat scene, so the effect of the mask is its weight alone.
    raw::DecodedRaw flat_scene = ts::scene_mosaic(
        240, 160, [](std::int32_t, std::int32_t) { return std::array<double, 3>{0.1, 0.12, 0.09}; });
    ts::set_lens_identity(flat_scene.metadata, "Lucent", "Test Camera", "24-70mm F2.8 Test", 24.0f, 2.8f);
    const StageOneImage one = stage_one_of(std::move(flat_scene));
    DevelopSettings base;
    base.lens_correction.vignetting = false;  // keep the scene flat: vignetting would add its own gradient
    DevelopSettings masked = base;
    LocalAdjustment radial;
    radial.id = 1;
    RadialGradientMask r;
    r.center = {0.92, 0.88};  // about 1.5 pixels of distortion here at 24 mm
    r.radius_x = 0.03;
    r.radius_y = 0.03;
    r.feather = 1.0;
    radial.shape = r;
    radial.controls.exposure_ev = 1.0;
    masked.local_adjustments = {radial};
    DevelopSettings masked_off = masked;
    masked_off.lens_correction.distortion = false;
    masked_off.lens_correction.lateral_chromatic_aberration = false;
    DevelopSettings base_off = base;
    base_off.lens_correction = masked_off.lens_correction;

    const DevelopOptions options = with_database();
    auto on = render_stage_two(one, masked, options);
    auto on_plain = render_stage_two(one, base, options);
    auto off = render_stage_two(one, masked_off, options);
    auto off_plain = render_stage_two(one, base_off, options);
    ASSERT_TRUE(on && on_plain && off && off_plain);
    ASSERT_TRUE(on->report.lens_correction.plan.geometry);
    ASSERT_FALSE(off->report.lens_correction.plan.geometry);
    auto map_on = geometry_mapping(on->report);
    auto map_off = geometry_mapping(off->report);
    ASSERT_TRUE(map_on && map_off);
    const image::PointD expected_on = map_on->output_from_normalized(r.center);
    const image::PointD expected_off = map_off->output_from_normalized(r.center);
    const image::PointD got_on =
        effect_centroid(on->display.pixels, on_plain->display.pixels, expected_on, 14.0);
    const image::PointD got_off =
        effect_centroid(off->display.pixels, off_plain->display.pixels, expected_off, 14.0);
    std::cout << "[masks] corrected: mask centre expected (" << expected_on.x << ", " << expected_on.y
              << "), found (" << got_on.x << ", " << got_on.y << "); uncorrected: expected ("
              << expected_off.x << ", " << expected_off.y << "), found (" << got_off.x << ", " << got_off.y
              << ")\n";
    // The correction moves the content (and so the mask) by more than a pixel here ...
    EXPECT_GT(std::hypot(expected_on.x - expected_off.x, expected_on.y - expected_off.y), 1.0);
    // ... and the mask follows it.
    EXPECT_LT(std::hypot(got_on.x - expected_on.x, got_on.y - expected_on.y), 0.25);
    EXPECT_LT(std::hypot(got_off.x - expected_off.x, got_off.y - expected_off.y), 0.25);
}

TEST(LensStageTwo, RendersAreDeterministic) {
    const StageOneImage one = stage_one_of(lens_scene());
    const DevelopSettings s = masked_settings();
    auto a = render_stage_two(one, s, with_database(true));
    auto b = render_stage_two(one, s, with_database(true));
    auto serial = render_stage_two(one, s, with_database(false));
    ASSERT_TRUE(a && b && serial);
    ASSERT_TRUE(a->report.lens_correction.plan.any_applied());
    EXPECT_TRUE(ts::bit_identical(a->display.pixels, b->display.pixels));
    EXPECT_TRUE(ts::bit_identical(a->display.pixels, serial->display.pixels));
    // The export render equals stage two on the export stage one.
    auto exported = render_develop(lens_scene(), s, with_database());
    StageOneRequest request;
    request.kind = StageOneKind::Export;
    request.white_balance = s.white_balance;
    auto export_one = render_stage_one(lens_scene(), request, DevelopOptions{});
    ASSERT_TRUE(exported && export_one);
    auto again = render_stage_two(*export_one, s, with_database());
    ASSERT_TRUE(again);
    EXPECT_TRUE(ts::bit_identical(exported->display.pixels, again->display.pixels));
}

TEST(LensStageTwo, TheDenoisedImageCacheKnowsTheCorrection) {
    const StageOneImage one = stage_one_of(lens_scene(), 120);
    DevelopSettings s = masked_settings();
    DevelopOptions options = with_database();
    options.stage_two_cache = std::make_shared<StageTwoCache>();
    auto first = render_stage_two(one, s, options);
    s.lens_correction.distortion = false;
    auto second = render_stage_two(one, s, options);
    ASSERT_TRUE(first && second);
    EXPECT_EQ(options.stage_two_cache->stats().hits, 0u);
    auto uncached = render_stage_two(one, s, with_database());
    ASSERT_TRUE(uncached);
    EXPECT_TRUE(ts::bit_identical(second->display.pixels, uncached->display.pixels));
    auto third = render_stage_two(one, s, options);
    ASSERT_TRUE(third);
    EXPECT_EQ(options.stage_two_cache->stats().hits, 1u);
    EXPECT_TRUE(ts::bit_identical(third->display.pixels, uncached->display.pixels));
}

TEST(LensStageTwo, APartOfTheFrameIsNotCorrected) {
    StageOneImage one = stage_one_of(lens_scene());
    // An analysis crop: a sub-rectangle at full resolution (scale 1, the full size kept).
    StageOneImage part;
    part.pixels = std::move(*image::ImageBuffer<float>::create(64, 64, 3));
    for (std::int32_t y = 0; y < 64; ++y) {
        const float* source = one.pixels.row(y + 10) + 30 * 3;
        std::copy(source, source + 64 * 3, part.pixels.row(y));
    }
    part.encoding = one.encoding;
    part.full_size = one.full_size;
    part.scale = 1.0;
    part.metadata = one.metadata;
    part.normalization = one.normalization;
    part.white_balance = one.white_balance;
    part.sample_multipliers = one.sample_multipliers;
    part.demosaic = one.demosaic;
    EXPECT_FALSE(is_whole_frame(part));
    EXPECT_TRUE(is_whole_frame(one));
    auto r = render_stage_two(part, DevelopSettings{}, with_database());
    ASSERT_TRUE(r) << r.error().describe();
    EXPECT_FALSE(r->report.lens_correction.plan.any_applied());
    EXPECT_EQ(r->report.lens_correction.plan.decision(LensCorrectionKind::Distortion).outcome,
              LensCorrectionOutcome::NotApplicable);
}

// The estimate plans the correction with the options' database, as the render does.
TEST(LensStageTwo, TheExportEstimateCountsTheCorrectedImage) {
    const raw::DecodedRaw d = lens_scene(600, 400);
    StageOneRequest request;
    request.kind = StageOneKind::Export;
    DevelopSettings v2;
    v2.version = 2;
    const DevelopOptions database = with_database();
    auto old = estimate_develop_memory(d.metadata, request, v2, database);
    auto current = estimate_develop_memory(d.metadata, request, DevelopSettings{}, database);
    auto no_database = estimate_develop_memory(d.metadata, request, DevelopSettings{});
    DevelopSettings vignetting_only;
    vignetting_only.lens_correction.distortion = false;
    vignetting_only.lens_correction.lateral_chromatic_aberration = false;
    auto in_place = estimate_develop_memory(d.metadata, request, vignetting_only, database);
    ASSERT_TRUE(old && current && no_database && in_place);
    // The exact match resamples: the corrected RGB image is counted next to stage one's kept image.
    const std::uint64_t rgb = std::uint64_t{600} * 400 * 3 * sizeof(float);
    EXPECT_GE(current->export_peak, current->stage_one_bytes + rgb);
    EXPECT_GE(current->export_peak, old->export_peak);
    // Version-2 settings correct nothing, nothing is corrected without a database, and vignetting
    // alone does not resample: stage one's RGB image is reused in place.
    EXPECT_LT(old->export_peak, current->stage_one_bytes + rgb);
    EXPECT_EQ(no_database->export_peak, old->export_peak);
    EXPECT_EQ(in_place->export_peak, old->export_peak);
    // Without settings any profile of a database may resample.
    auto unknown = estimate_develop_memory(d.metadata, request, database);
    ASSERT_TRUE(unknown);
    EXPECT_GE(unknown->export_peak, unknown->stage_one_bytes + rgb);
}

// W13 closure (APL-03): the lens-corrected image depends on the stage-one image and the plan only, so
// an edit session's renders keep it (StageTwoCache::LensEntry) and every later frame reads it instead of
// resampling again, bit-identically.
TEST(LensStageTwo, TheLensCorrectedImageIsKeptAndReused) {
    const StageOneImage one = stage_one_of(lens_scene(), 120);
    ASSERT_NE(one.id, 0u);
    DevelopOptions options = with_database();
    options.stage_two_cache = std::make_shared<StageTwoCache>();
    DevelopSettings s;
    s.exposure_ev = 0.3;
    auto first = render_stage_two(one, s, options);
    ASSERT_TRUE(first) << first.error().describe();
    EXPECT_TRUE(first->report.lens_correction.resampled);
    EXPECT_FALSE(first->report.lens_correction.reused);
    const StageTwoCache::Stats made = options.stage_two_cache->stats();
    EXPECT_EQ(made.lens_misses, 1u);
    EXPECT_EQ(made.bytes, one.pixels.byte_size());  // no denoising: the lens-corrected image alone
    for (const double ev : {0.3, -0.7, 1.1}) {
        SCOPED_TRACE(ev);
        s.exposure_ev = ev;
        s.contrast = ev * 0.2;  // within [-1, 1]
        auto cached = render_stage_two(one, s, options);
        auto plain = render_stage_two(one, s, with_database());
        ASSERT_TRUE(cached && plain);
        EXPECT_TRUE(cached->report.lens_correction.reused);
        EXPECT_EQ(cached->report.lens_correction.outside_samples, plain->report.lens_correction.outside_samples);
        EXPECT_TRUE(ts::bit_identical(cached->display.pixels, plain->display.pixels));
    }
    EXPECT_EQ(options.stage_two_cache->stats().lens_hits, 3u);
    // Another plan (a correction switched off) makes a new one; denoising is kept beside it.
    s.lens_correction.lateral_chromatic_aberration = false;
    s.chroma_denoise.strength = 0.5;
    auto other = render_stage_two(one, s, options);
    ASSERT_TRUE(other);
    EXPECT_FALSE(other->report.lens_correction.reused);
    EXPECT_EQ(options.stage_two_cache->stats().bytes, one.pixels.byte_size() + other->display.pixels.byte_size());
    // A cache too small for the image corrects every render's rows, with the same pixels.
    DevelopOptions small = with_database();
    small.stage_two_cache = std::make_shared<StageTwoCache>(one.pixels.byte_size() / 2);
    auto unkept = render_stage_two(one, s, small);
    auto again = render_stage_two(one, s, small);
    ASSERT_TRUE(unkept && again);
    EXPECT_FALSE(again->report.lens_correction.reused);
    EXPECT_TRUE(ts::bit_identical(unkept->display.pixels, other->display.pixels));
    EXPECT_EQ(small.stage_two_cache->stats().bytes, 0u);
}

// A cache with room for one image keeps the lens-corrected one, which serves every render.
TEST(LensStageTwo, TheLensCorrectedImageHasPriorityInTheCache) {
    const StageOneImage one = stage_one_of(lens_scene(), 120);
    DevelopOptions options = with_database();
    options.stage_two_cache = std::make_shared<StageTwoCache>(one.pixels.byte_size() * 3 / 2);
    DevelopSettings s;
    s.chroma_denoise.strength = 0.5;
    auto first = render_stage_two(one, s, options);
    ASSERT_TRUE(first);
    StageTwoCache::Stats stats = options.stage_two_cache->stats();
    EXPECT_EQ(stats.bytes, one.pixels.byte_size());  // the denoised image did not fit beside it
    s.exposure_ev = 0.5;
    auto second = render_stage_two(one, s, options);
    ASSERT_TRUE(second);
    EXPECT_TRUE(second->report.lens_correction.reused);
}

TEST(LensModel, TheThreeChannelsAtOnceAreThePerChannelModel) {
    for (const ModelCase& mc : model_cases()) {
        SCOPED_TRACE(mc.db_model);
        const raw::RawMetadata m =
            lens_metadata(mc.camera, mc.lens_name, mc.focal, mc.f_number, std::nullopt, mc.lens_make);
        const LensCorrectionPlan p = plan_of(m, LensCorrectionSetting{}, mc.stored, image::Orientation::Normal);
        if (!p.geometry) {
            continue;
        }
        for (double x = -1.4; x <= 1.4; x += 0.07) {
            for (double y = -1.0; y <= 1.0; y += 0.09) {
                const std::array<image::PointD, 3> all = p.geometry->normalized_sources(image::PointD{x, y});
                for (std::uint32_t c = 0; c < 3; ++c) {
                    const image::PointD one_channel = p.geometry->normalized_source(image::PointD{x, y}, c);
                    ASSERT_EQ(std::bit_cast<std::uint64_t>(all[c].x), std::bit_cast<std::uint64_t>(one_channel.x));
                    ASSERT_EQ(std::bit_cast<std::uint64_t>(all[c].y), std::bit_cast<std::uint64_t>(one_channel.y));
                }
            }
        }
    }
}

// A small highlights/shadows brush near a corner: the base layer covers its support mapped into the
// corrected image (as the render maps it), not the whole image.
TEST(LensStageTwo, TheEstimateMapsTheMasksSupportsThroughTheCorrection) {
    const raw::DecodedRaw d = lens_scene(600, 400);
    DevelopSettings brushed;
    LocalAdjustment a;
    a.id = 1;
    BrushStroke stroke;
    stroke.points = {{0.9, 0.88}};
    stroke.radius = 0.02;
    stroke.hardness = 0.5;
    a.shape = BrushMask{{stroke}};
    a.controls.shadows = 0.4;
    brushed.local_adjustments = {a};
    ASSERT_TRUE(brushed.validate());
    StageOneRequest interactive;
    auto plain = estimate_develop_memory(d.metadata, interactive, brushed);
    auto corrected = estimate_develop_memory(d.metadata, interactive, brushed, with_database());
    ASSERT_TRUE(plain && corrected);
    const std::uint64_t plane = std::uint64_t{600} * 400 * sizeof(float);
    EXPECT_LT(corrected->stage_two_bytes, plain->stage_two_bytes + plane / 2);
    EXPECT_GT(corrected->stage_two_bytes + plane / 2, plain->stage_two_bytes);
    // The render's own check uses the same mapping: it renders within that estimate.
    const StageOneImage one = stage_one_of(lens_scene(600, 400));
    DevelopOptions tight = with_database();
    tight.memory_budget_bytes = corrected->stage_two_bytes + 4096;
    auto r = render_stage_two(one, brushed, tight);
    ASSERT_TRUE(r) << r.error().describe();
    EXPECT_TRUE(r->report.lens_correction.plan.geometry.has_value());
}

}  // namespace
}  // namespace lucent::engine
