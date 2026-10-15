// W13 (ADR 0023) view models of the Lens corrections panel against a recording EditService: what a
// frame's lens record shows (the match and its quality, the profile, each correction's outcome), the
// switches, the automatic profile and the profile choice as ONE revision each, corrections the file
// carries shown off and unchangeable with a note, a recipe made before lens corrections and a
// missing lens database, the profile search, the copy category (not selected by default), the mask
// tool mapping positions through the frame's lens correction, and the history's part label.

#include <QTest>
#include <cmath>
#include <memory>
#include <optional>

#include <gtest/gtest.h>

#include "models/edit_session.hpp"
#include "models/lens_corrections.hpp"
#include "models/mask_editor.hpp"
#include "test_support.hpp"

namespace lucent::ui {
namespace {

using testing::RecordingEditService;

constexpr QSize kUpright{6000, 4000};

EditLensProfile nikkor() {
    return EditLensProfile{QStringLiteral("Nikon"), QStringLiteral("Nikkor Z 24-70mm f/4 S"), 1.0};
}

LensProfileEntry entry_of(const EditLensProfile& profile, const QString& mounts, bool vignetting = true) {
    LensProfileEntry entry;
    entry.profile = profile;
    entry.mounts = mounts;
    entry.lens_type = QStringLiteral("rectilinear");
    entry.min_focal_mm = 24.0;
    entry.max_focal_mm = 70.0;
    entry.distortion = true;
    entry.chromatic_aberration = true;
    entry.vignetting = vignetting;
    return entry;
}

// The lens record of an exact match whose three corrections were applied.
std::shared_ptr<LensInfo> exact_info() {
    auto info = std::make_shared<LensInfo>();
    info->database = true;
    info->database_version = QStringLiteral("1.0.0");
    info->camera = QStringLiteral("Nikon Z 6");
    info->lens = QStringLiteral("NIKKOR Z 24-70mm f/4 S");
    info->focal_length_mm = 52.0;
    info->f_number = 6.7;
    info->match = LensMatchQuality::Exact;
    info->database_camera = QStringLiteral("Nikon Z 6");
    info->database_mount = QStringLiteral("Nikon Z");
    info->compatible_mounts = {QStringLiteral("Nikon Z"), QStringLiteral("Nikon F AF")};
    info->matched = nikkor();
    info->profile = nikkor();
    info->reasons << QStringLiteral("camera: Nikon Corporation / Nikon Z 6 identified");
    for (LensCorrectionState& state : info->corrections) {
        state.outcome = LensOutcome::Applied;
        state.detail = QStringLiteral("calibrated");
    }
    return info;
}

// A test map: the correction magnifies about the image's centre by `factor`.
class ScaledLens final : public LensPointMap {
public:
    explicit ScaledLens(double factor) : factor_(factor) {}
    QPointF upright_of(QPointF corrected) const override { return centre_ + (corrected - centre_) / factor_; }
    std::optional<QPointF> corrected_of(QPointF upright) const override {
        return centre_ + (upright - centre_) * factor_;
    }

private:
    double factor_;
    QPointF centre_{kUpright.width() / 2.0, kUpright.height() / 2.0};
};

class LensCorrectionsTest : public ::testing::Test {
protected:
    LensCorrectionsTest() : store(std::make_shared<FrameStore>()), session(service, store) {
        service.lens_database_info.available = true;
        service.lens_database_info.version = QStringLiteral("1.0.0");
        service.lens_database_info.library = QStringLiteral("Lensfun 0.3.4");
        service.lens_profiles = {
            entry_of(nikkor(), QStringLiteral("Nikon Z")),
            entry_of(
                EditLensProfile{QStringLiteral("Nikon"), QStringLiteral("Nikkor Z 24-70mm f/2.8 S"), 1.0},
                QStringLiteral("Nikon Z")),
            entry_of(EditLensProfile{QStringLiteral("Sigma"), QStringLiteral("24-70mm f/2.8 DG DN | A"), 1.0},
                     QStringLiteral("Sony E, Leica L"), false)};
    }

    LensCorrections& lens() { return *session.lens(); }

    void open(const EditSettings& settings = {}, std::shared_ptr<const LensInfo> info = exact_info(),
              std::shared_ptr<const LensPointMap> map = nullptr) {
        session.set_item(7);
        EditState state;
        state.item = 7;
        state.settings = settings;
        state.committed = settings;
        service.emit_state(state);
        show_frame(std::move(info), std::move(map));
    }

    void show_frame(std::shared_ptr<const LensInfo> info, std::shared_ptr<const LensPointMap> map = nullptr) {
        auto frame = std::make_shared<EditFrame>();
        frame->item = 7;
        frame->sequence = ++sequence;
        frame->phase = FramePhase::Final;
        frame->geometry = session.settings().geometry;
        frame->full_upright_size = kUpright;
        const GeometryMap mapped = service.map_geometry(frame->geometry, kUpright, QRect());
        frame->full_output_size = mapped.output_size;
        frame->image = testing::solid_image(QSize(600, 400));
        frame->info = std::make_shared<RenderInfo>();
        frame->lens = std::move(info);
        frame->lens_map = std::move(map);
        service.emit_frame(frame);
    }

    [[nodiscard]] QVariantMap correction(int index) { return lens().corrections().at(index).toMap(); }

    RecordingEditService service;
    std::shared_ptr<FrameStore> store;
    EditSession session;
    quint64 sequence = 0;
};

TEST_F(LensCorrectionsTest, ShowsTheExactMatchAndEachSwitchIsOneRevision) {
    open();
    EXPECT_TRUE(lens().available());
    EXPECT_EQ(lens().database_version(), u"1.0.0");
    EXPECT_TRUE(lens().editable());
    EXPECT_FALSE(lens().legacy());
    EXPECT_TRUE(lens().automatic());
    EXPECT_TRUE(lens().known());
    EXPECT_EQ(lens().match_kind(), u"exact");
    EXPECT_EQ(lens().match_label(), u"Exact match");
    EXPECT_TRUE(lens().match_summary().contains(u"applied automatically"))
        << lens().match_summary().toStdString();
    EXPECT_EQ(lens().camera_text(), u"Nikon Z 6");
    EXPECT_EQ(lens().lens_text(), u"NIKKOR Z 24-70mm f/4 S · 52 mm · f/6.7");
    EXPECT_EQ(lens().profile_text(), u"Nikon Nikkor Z 24-70mm f/4 S");
    EXPECT_EQ(lens().reasons().size(), 1);
    EXPECT_TRUE(lens().carried_note().isEmpty());
    ASSERT_EQ(lens().corrections().size(), 3);
    const QStringList keys = {QStringLiteral("distortion"), QStringLiteral("chromaticAberration"),
                              QStringLiteral("vignetting")};
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(correction(i).value(QStringLiteral("key")).toString(), keys[i]);
        EXPECT_TRUE(correction(i).value(QStringLiteral("checked")).toBool());
        EXPECT_TRUE(correction(i).value(QStringLiteral("editable")).toBool());
        EXPECT_EQ(correction(i).value(QStringLiteral("status")).toString(), u"Applied");
    }

    // One switch: one revision of the settings with that correction off.
    lens().setCorrection(QStringLiteral("vignetting"), false);
    ASSERT_EQ(service.count(QStringLiteral("apply")), 1);
    EditSettings expected;
    expected.lens.vignetting = false;
    EXPECT_EQ(service.calls_named(QStringLiteral("apply")).back().settings, expected);
    EXPECT_FALSE(correction(2).value(QStringLiteral("checked")).toBool());
    // The same value again: nothing.
    lens().setCorrection(QStringLiteral("vignetting"), false);
    EXPECT_EQ(service.count(QStringLiteral("apply")), 1);
    lens().setCorrection(QStringLiteral("focus"), false);  // not a correction
    EXPECT_EQ(service.count(QStringLiteral("apply")), 1);
    lens().setCorrection(QStringLiteral("chromaticAberration"), false);
    ASSERT_EQ(service.count(QStringLiteral("apply")), 2);
    expected.lens.chromatic_aberration = false;
    EXPECT_EQ(service.calls_named(QStringLiteral("apply")).back().settings, expected);
    EXPECT_TRUE(service.calls_named(QStringLiteral("set_transient")).empty());  // never a gesture

    // Every correction off: said so, and the profile shown is the exact match they would use.
    lens().setCorrection(QStringLiteral("distortion"), false);
    ASSERT_EQ(service.count(QStringLiteral("apply")), 3);
    auto off = exact_info();
    off->profile.reset();
    for (LensCorrectionState& state : off->corrections) {
        state.outcome = LensOutcome::Disabled;
    }
    show_frame(off);
    EXPECT_EQ(lens().match_summary(), u"Every lens correction is switched off for this photo.");
    EXPECT_EQ(lens().profile_text(), u"Nikon Nikkor Z 24-70mm f/4 S");
    EXPECT_EQ(correction(0).value(QStringLiteral("status")).toString(), u"Off");
}

TEST_F(LensCorrectionsTest, AProfileIsChosenOnlyByThePhotographer) {
    open();
    // Unticking "Automatic profile" opens the choice and changes nothing yet.
    lens().setAutomatic(false);
    EXPECT_TRUE(lens().picking());
    EXPECT_EQ(service.count(QStringLiteral("apply")), 0);
    EXPECT_TRUE(lens().automatic());
    // With an empty search the suggestions: the exact match, with its details from the database.
    ASSERT_EQ(lens().profiles()->count(), 1);
    const QModelIndex first = lens().profiles()->index(0);
    EXPECT_EQ(lens().profiles()->data(first, LensProfileListModel::LabelRole).toString(),
              u"Nikon Nikkor Z 24-70mm f/4 S");
    EXPECT_TRUE(lens().profiles()->data(first, LensProfileListModel::CompatibleRole).toBool());
    const QString detail = lens().profiles()->data(first, LensProfileListModel::DetailRole).toString();
    EXPECT_TRUE(detail.contains(u"Nikon Z")) << detail.toStdString();
    // The sensor the calibration was made on comes first, before the mounts (W13 closure: calibrations
    // of one lens for different sensors are told apart even when the line is elided).
    EXPECT_TRUE(detail.startsWith(u"crop factor 1")) << detail.toStdString();
    EXPECT_LT(detail.indexOf(u"crop factor"), detail.indexOf(u"Nikon Z")) << detail.toStdString();
    // A search by maker and model ("f/2.8" and "f2.8" are the same word).
    lens().setSearchText(QStringLiteral("24-70 f2.8"));
    ASSERT_EQ(lens().profiles()->count(), 2);
    EXPECT_TRUE(
        lens().profiles()->data(lens().profiles()->index(0), LensProfileListModel::CompatibleRole).toBool());
    EXPECT_FALSE(
        lens().profiles()->data(lens().profiles()->index(1), LensProfileListModel::CompatibleRole).toBool());
    // The profiles that fit the camera come first.
    lens().setSearchText(QStringLiteral("24-70"));
    ASSERT_EQ(lens().profiles()->count(), 3);
    EXPECT_EQ(
        lens().profiles()->data(lens().profiles()->index(2), LensProfileListModel::MakerRole).toString(),
        u"Sigma");
    lens().setSearchText(QStringLiteral("zeiss"));
    EXPECT_EQ(lens().profiles()->count(), 0);
    lens().setSearchText(QStringLiteral("sigma"));
    ASSERT_EQ(lens().profiles()->count(), 1);
    // Choosing one: one revision naming it; the choice closes.
    lens().chooseProfile(0);
    ASSERT_EQ(service.count(QStringLiteral("apply")), 1);
    const EditSettings chosen = service.calls_named(QStringLiteral("apply")).back().settings;
    ASSERT_TRUE(chosen.lens.profile);
    EXPECT_EQ(chosen.lens.profile->maker, u"Sigma");
    EXPECT_EQ(chosen.lens.profile->model, u"24-70mm f/2.8 DG DN | A");
    EXPECT_FALSE(lens().picking());
    EXPECT_FALSE(lens().automatic());
    lens().chooseProfile(5);  // no such row
    EXPECT_EQ(service.count(QStringLiteral("apply")), 1);
    // Back to automatic: one revision without the profile.
    lens().setAutomatic(true);
    ASSERT_EQ(service.count(QStringLiteral("apply")), 2);
    EXPECT_FALSE(service.calls_named(QStringLiteral("apply")).back().settings.lens.profile);
    EXPECT_EQ(service.calls_named(QStringLiteral("apply")).back().settings, EditSettings{});
}

TEST_F(LensCorrectionsTest, FuzzyMatchesSuggestTheirCandidatesAndApplyNothing) {
    auto info = exact_info();
    info->match = LensMatchQuality::Fuzzy;
    info->matched.reset();
    info->profile.reset();
    info->candidates = {
        nikkor(), EditLensProfile{QStringLiteral("Nikon"), QStringLiteral("Nikkor Z 24-70mm f/2.8 S"), 1.0}};
    for (LensCorrectionState& state : info->corrections) {
        state.outcome = LensOutcome::NoProfile;
    }
    open({}, info);
    EXPECT_EQ(lens().match_kind(), u"fuzzy");
    EXPECT_EQ(lens().match_label(), u"Similar profiles only");
    EXPECT_TRUE(lens().match_summary().contains(u"none is applied automatically"));
    EXPECT_TRUE(lens().profile_text().isEmpty());
    EXPECT_EQ(correction(0).value(QStringLiteral("status")).toString(), u"No profile");
    lens().openPicker();
    ASSERT_EQ(lens().profiles()->count(), 2);
    lens().chooseProfile(1);
    ASSERT_EQ(service.count(QStringLiteral("apply")), 1);
    EXPECT_EQ(service.calls_named(QStringLiteral("apply")).back().settings.lens.profile->model,
              u"Nikkor Z 24-70mm f/2.8 S");
}

TEST_F(LensCorrectionsTest, CorrectionsTheFileCarriesAreOffAndCannotBeChanged) {
    auto info = exact_info();
    info->carried_distortion = true;
    info->carried_vignetting = true;
    info->corrections[0].outcome = LensOutcome::CarriedByFile;
    info->corrections[1].outcome = LensOutcome::Withheld;
    info->corrections[2].outcome = LensOutcome::CarriedByFile;
    open({}, info);
    EXPECT_FALSE(correction(0).value(QStringLiteral("checked")).toBool());
    EXPECT_FALSE(correction(0).value(QStringLiteral("editable")).toBool());
    EXPECT_TRUE(correction(0).value(QStringLiteral("carried")).toBool());
    EXPECT_EQ(correction(0).value(QStringLiteral("status")).toString(), u"Corrected by the file");
    EXPECT_TRUE(correction(1).value(QStringLiteral("checked")).toBool());
    EXPECT_TRUE(correction(1).value(QStringLiteral("editable")).toBool());
    EXPECT_EQ(correction(1).value(QStringLiteral("status")).toString(), u"Not applied automatically");
    EXPECT_FALSE(correction(2).value(QStringLiteral("editable")).toBool());
    EXPECT_EQ(lens().carried_note(),
              u"This file already corrects distortion and vignetting itself (DNG opcodes). Lucent never "
              u"applies the "
              u"same correction twice, so those switches stay off.");
    EXPECT_TRUE(lens().match_summary().contains(u"the file corrects its lens itself"));
    lens().setCorrection(QStringLiteral("distortion"), false);
    lens().setCorrection(QStringLiteral("vignetting"), false);
    EXPECT_EQ(service.count(QStringLiteral("apply")), 0);

    // A file that corrects all three itself (the DJI Mavic 3's Hasselblad DNGs): nothing applied again.
    auto all = exact_info();
    all->carried_distortion = all->carried_chromatic_aberration = all->carried_vignetting = true;
    for (LensCorrectionState& state : all->corrections) {
        state.outcome = LensOutcome::CarriedByFile;
    }
    show_frame(all);
    EXPECT_TRUE(lens().match_summary().contains(u"already makes these corrections itself"))
        << lens().match_summary().toStdString();
    EXPECT_TRUE(lens().carried_note().contains(u"distortion, chromatic aberration and vignetting"))
        << lens().carried_note().toStdString();
    for (int i = 0; i < 3; ++i) {
        EXPECT_FALSE(correction(i).value(QStringLiteral("editable")).toBool());
    }
}

TEST_F(LensCorrectionsTest, ARecipeMadeBeforeLensCorrectionsOffersToTurnThemOn) {
    EditSettings legacy;
    legacy.lens.enabled = false;
    auto info = exact_info();
    info->enabled = false;
    for (LensCorrectionState& state : info->corrections) {
        state.outcome = LensOutcome::Disabled;
    }
    open(legacy, info);
    EXPECT_TRUE(lens().legacy());
    EXPECT_TRUE(lens().match_summary().contains(u"before Lucent had lens corrections"));
    for (int i = 0; i < 3; ++i) {
        EXPECT_FALSE(correction(i).value(QStringLiteral("checked")).toBool());
        EXPECT_FALSE(correction(i).value(QStringLiteral("editable")).toBool());
    }
    lens().setCorrection(QStringLiteral("distortion"), true);
    lens().setAutomatic(false);
    EXPECT_FALSE(lens().picking());
    EXPECT_EQ(service.count(QStringLiteral("apply")), 0);
    lens().turnOn();
    ASSERT_EQ(service.count(QStringLiteral("apply")), 1);
    EXPECT_EQ(service.calls_named(QStringLiteral("apply")).back().settings.lens, EditLensCorrection{});
    EXPECT_FALSE(lens().legacy());
    lens().turnOn();  // already on
    EXPECT_EQ(service.count(QStringLiteral("apply")), 1);
}

TEST_F(LensCorrectionsTest, WithoutALensDatabaseNothingCanBeChanged) {
    service.lens_database_info = LensDatabaseInfo{};
    service.lens_database_info.problem = QStringLiteral("The lens database failed its integrity check.");
    auto info = exact_info();
    info->database = false;
    open({}, info);
    EXPECT_FALSE(lens().available());
    EXPECT_FALSE(lens().editable());
    EXPECT_EQ(lens().unavailable_reason(), u"The lens database failed its integrity check.");
    EXPECT_EQ(lens().match_summary(), lens().unavailable_reason());
    lens().setCorrection(QStringLiteral("distortion"), false);
    lens().openPicker();
    EXPECT_FALSE(lens().picking());
    EXPECT_EQ(service.count(QStringLiteral("apply")), 0);
}

// W12 (APL-06): while the lens database still loads off the UI thread the panel says so (not as a
// problem) and offers nothing to change; when the service says it has loaded, the panel follows.
TEST_F(LensCorrectionsTest, WhileTheLensDatabaseLoadsThePanelWaitsForIt) {
    const LensDatabaseInfo loaded = service.lens_database_info;
    ASSERT_TRUE(loaded.available);
    service.lens_database_info = LensDatabaseInfo{};
    service.lens_database_info.loading = true;
    session.set_item(7);
    EditState state;
    state.item = 7;
    service.emit_state(state);
    EXPECT_TRUE(lens().loading());
    EXPECT_FALSE(lens().available());
    EXPECT_FALSE(lens().editable());
    EXPECT_TRUE(lens().unavailable_reason().contains(u"still loading")) << lens().unavailable_reason().toStdString();
    lens().setCorrection(QStringLiteral("distortion"), false);
    lens().openPicker();
    EXPECT_FALSE(lens().picking());
    EXPECT_EQ(service.count(QStringLiteral("apply")), 0);

    int changes = 0;
    QObject::connect(&lens(), &LensCorrections::changed, &lens(), [&] { ++changes; });
    service.lens_database_info = loaded;
    emit service.lens_database_changed();
    EXPECT_GE(changes, 1);
    EXPECT_FALSE(lens().loading());
    EXPECT_TRUE(lens().available());
    EXPECT_TRUE(lens().unavailable_reason().isEmpty());
    // Frames that arrive later are shown as usual.
    show_frame(exact_info());
    EXPECT_TRUE(lens().known());
    EXPECT_TRUE(lens().editable());
}

TEST_F(LensCorrectionsTest, TheCopyCategoryIsOfferedButNotSelected) {
    open();
    bool found = false;
    for (const QVariant& category : session.categories()) {
        const QVariantMap map = category.toMap();
        if (map.value(QStringLiteral("key")).toString() == u"lensCorrections") {
            found = true;
            EXPECT_EQ(map.value(QStringLiteral("label")).toString(), u"Lens corrections");
            EXPECT_FALSE(map.value(QStringLiteral("checked")).toBool());
        }
    }
    EXPECT_TRUE(found);
    session.copySettings({QStringLiteral("lensCorrections")});
    ASSERT_EQ(service.count(QStringLiteral("copy_settings")), 1);
    EXPECT_EQ(service.calls_named(QStringLiteral("copy_settings")).back().categories,
              category_bit(SettingsCategory::LensCorrections));
    EXPECT_EQ(part_label(EditPart::LensCorrections), u"Lens corrections");
    EXPECT_EQ(category_label(SettingsCategory::LensCorrections), u"Lens corrections");
}

TEST_F(LensCorrectionsTest, TheMaskToolMapsPositionsThroughTheLensCorrection) {
    // A correction that magnifies by 1.1 about the centre: a mask point (in the uncorrected image)
    // shows 10 % further from the centre, and a pointer position maps back to the uncorrected point.
    open({}, exact_info(), std::make_shared<ScaledLens>(1.1));
    const GeometryMap& map = session.frame_map();
    ASSERT_TRUE(map.valid);
    ASSERT_TRUE(map.lens);
    const std::optional<QPointF> shown = map.output_from_normalized(QPointF(0.75, 0.25));
    ASSERT_TRUE(shown);
    EXPECT_NEAR(shown->x(), 3000.0 + 1500.0 * 1.1, 1e-6);
    EXPECT_NEAR(shown->y(), 2000.0 - 1000.0 * 1.1, 1e-6);
    const std::optional<QPointF> back = map.normalized_from_output(*shown);
    ASSERT_TRUE(back);
    EXPECT_NEAR(back->x(), 0.75, 1e-12);
    EXPECT_NEAR(back->y(), 0.25, 1e-12);
    // The mask tool: displayed fractions <-> normalised positions through it.
    const std::optional<QPointF> at = session.masks()->normalized_at(0.5 + 0.11, 0.5);
    ASSERT_TRUE(at);
    EXPECT_NEAR(at->x(), 0.6, 1e-9);
    EXPECT_NEAR(at->y(), 0.5, 1e-9);
    const std::optional<QPointF> displayed = session.masks()->displayed_at(QPointF(0.6, 0.5));
    ASSERT_TRUE(displayed);
    EXPECT_NEAR(displayed->x(), 0.61, 1e-9);
    // Without a correction the geometry alone maps them.
    show_frame(exact_info());
    EXPECT_FALSE(session.frame_map().lens);
    EXPECT_NEAR(session.masks()->normalized_at(0.61, 0.5)->x(), 0.61, 1e-9);
}

}  // namespace
}  // namespace lucent::ui
