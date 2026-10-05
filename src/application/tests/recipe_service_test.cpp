// RecipeService (recipe_service.hpp): gesture coalescing (also of mask strokes and crop drags),
// undo/redo with branches, reset and revert, locks, paste rules across cameras and image shapes
// (masks, geometry, denoising), read-only recipes, persistence across a restart (version-2 members
// and version-1 revisions) and observers. Items are synthetic DNGs imported into a session library
// (memory store) or a catalog.

#include <algorithm>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/application/import.hpp>
#include <lucent/application/recipe.hpp>
#include <lucent/application/recipe_service.hpp>
#include <lucent/application/recipe_store.hpp>

#include "app_test_support.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;
namespace rts = raw::test_support;
using engine::DevelopControl;

engine::DevelopSettings with_exposure(double ev) {
    engine::DevelopSettings s;
    s.exposure_ev = ev;
    return s;
}

rts::SyntheticDngSpec camera(const std::string& model, rts::SyntheticKind kind = rts::SyntheticKind::Cfa) {
    rts::SyntheticDngSpec spec;
    spec.model = model;
    spec.kind = kind;
    return spec;
}

// A radial mask (id `id`) brightening the centre.
engine::LocalAdjustment radial_mask(std::uint32_t id, double x = 0.5, double y = 0.5) {
    engine::LocalAdjustment a;
    a.id = id;
    a.shape = engine::RadialGradientMask{{x, y}, 0.2, 0.15, 10.0, 0.5};
    a.controls.exposure_ev = 0.6;
    return a;
}

// Settings with every version-2 member: highlights and shadows, a radial and a brush mask, chroma
// denoising and a crop with a quarter turn.
engine::DevelopSettings local_edit() {
    engine::DevelopSettings s;
    s.highlights = -0.3;
    s.shadows = 0.2;
    engine::LocalAdjustment brush;
    brush.id = 2;
    brush.shape = engine::BrushMask{{engine::BrushStroke{engine::BrushMode::Add, 0.05, 0.5, 1.0,
                                                         {{0.2, 0.3}, {0.3, 0.35}, {0.4, 0.3}}}}};
    brush.controls.shadows = 0.5;
    s.local_adjustments = {radial_mask(1), brush};
    s.chroma_denoise = engine::ChromaDenoiseSetting{0.4, 6.0};
    s.geometry.crop = engine::CropRect{0.1, 0.05, 0.9, 0.95};
    s.geometry.quarter_turns = 1;
    return s;
}

std::vector<ItemId> import_files(Library& library, const raw::DecoderRegistry& registry,
                                 const std::vector<std::filesystem::path>& files) {
    auto report = import_sources(library, registry, ImportRequest{.files = files});
    EXPECT_TRUE(report) << (report ? std::string() : report.error().describe());
    if (!report) {
        return {};
    }
    EXPECT_EQ(report->added.size(), files.size());
    return report->added;
}

class RecipeServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry_ = ts::default_registry();
        store_ = create_memory_recipe_store();
        RecipeServiceOptions options;
        options.registry = registry_;
        service_ = std::make_unique<RecipeService>(library_, store_, options);
        items_ = import_files(
            library_, *registry_,
            {ts::write_dng(dir_, L"a.dng", camera("Alpha")), ts::write_dng(dir_, L"b.dng", camera("Alpha")),
             ts::write_dng(dir_, L"c.dng", camera("Beta")),
             ts::write_dng(dir_, L"m.dng", camera("Mono", rts::SyntheticKind::Mono))});
        ASSERT_EQ(items_.size(), 4u);
    }

    ItemId a() const { return items_[0]; }
    ItemId b() const { return items_[1]; }
    ItemId c() const { return items_[2]; }
    ItemId m() const { return items_[3]; }

    RecipeState state(ItemId item) const {
        auto s = service_->state(item);
        EXPECT_TRUE(s) << (s ? std::string() : s.error().describe());
        return s ? *s : RecipeState{};
    }

    ts::ScratchDir dir_;
    SessionLibrary library_;
    std::shared_ptr<const raw::DecoderRegistry> registry_;
    std::shared_ptr<RecipeStore> store_;
    std::unique_ptr<RecipeService> service_;
    std::vector<ItemId> items_;
};

TEST_F(RecipeServiceTest, AnUneditedItemShowsAsShotSettings) {
    const RecipeState s = state(a());
    EXPECT_FALSE(s.revision);
    EXPECT_EQ(s.settings, engine::DevelopSettings{});
    EXPECT_EQ(s.committed_sha256, write_recipe(engine::DevelopSettings{})->sha256);
    EXPECT_FALSE(s.can_undo);
    EXPECT_FALSE(s.can_redo);
    EXPECT_FALSE(s.read_only);
    auto current = service_->current_settings(a());
    ASSERT_TRUE(current);
    EXPECT_FALSE(current->revision);
    EXPECT_EQ(current->document.json, write_recipe(engine::DevelopSettings{})->json);
    EXPECT_EQ(service_->state(ItemId{999}).error().code, ErrorCode::NotFound);
}

TEST_F(RecipeServiceTest, AGestureIsCommittedAsOneRevision) {
    std::uint64_t generation = 0;
    for (int step = 1; step <= 20; ++step) {
        auto s = service_->set_transient(a(), with_exposure(0.05 * step));
        ASSERT_TRUE(s) << s.error().describe();
        EXPECT_TRUE(s->transient);
        EXPECT_GT(s->transient_generation, generation);
        generation = s->transient_generation;
        EXPECT_FALSE(s->revision);  // nothing written during the gesture
        EXPECT_EQ(s->settings.exposure_ev, 0.05 * step);
        EXPECT_EQ(s->settings_sha256, write_recipe(with_exposure(0.05 * step))->sha256);
    }
    EXPECT_TRUE(service_->history(a())->entries.empty());
    auto committed = service_->commit(a());
    ASSERT_TRUE(committed) << committed.error().describe();
    EXPECT_FALSE(committed->transient);
    ASSERT_TRUE(committed->revision);
    EXPECT_EQ(committed->revision_number, 1u);
    EXPECT_EQ(committed->committed, with_exposure(1.0));
    EXPECT_EQ(committed->committed_sha256, write_recipe(with_exposure(1.0))->sha256);
    auto history = service_->history(a());
    ASSERT_TRUE(history);
    ASSERT_EQ(history->entries.size(), 1u);
    EXPECT_EQ(history->entries[0].info.origin, catalog::RevisionOrigin::User);
    EXPECT_EQ(history->entries[0].changed, std::vector<DevelopControl>{DevelopControl::Exposure});
    EXPECT_EQ(history->entries[0].info.settings_sha256, committed->committed_sha256);
    EXPECT_TRUE(history->entries[0].current);

    // Committing without a gesture, or a gesture that ends where it started, writes nothing.
    ASSERT_TRUE(service_->commit(a()));
    ASSERT_TRUE(service_->set_transient(a(), with_exposure(2.0)));
    ASSERT_TRUE(service_->set_transient(a(), with_exposure(1.0)));
    auto unchanged = service_->commit(a());
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(unchanged->revision, committed->revision);
    ASSERT_TRUE(service_->set_transient(a(), with_exposure(3.0)));
    auto discarded = service_->discard_transient(a());
    ASSERT_TRUE(discarded);
    EXPECT_FALSE(discarded->transient);
    EXPECT_EQ(discarded->settings, with_exposure(1.0));
    EXPECT_EQ(service_->history(a())->entries.size(), 1u);

    // Invalid settings never become transient.
    EXPECT_EQ(service_->set_transient(a(), with_exposure(9.0)).error().code, ErrorCode::InvalidArgument);
}

TEST_F(RecipeServiceTest, UndoAndRedoMoveAlongBranchesThatAreKept) {
    auto r1 = service_->apply(a(), with_exposure(0.5));
    auto r2 = service_->apply(a(), with_exposure(1.0));
    ASSERT_TRUE(r1 && r2);
    EXPECT_TRUE(r2->can_undo);
    EXPECT_FALSE(r2->can_redo);

    auto undone = service_->undo(a());
    ASSERT_TRUE(undone) << undone.error().describe();
    EXPECT_EQ(undone->revision, r1->revision);
    EXPECT_EQ(undone->settings, with_exposure(0.5));
    EXPECT_TRUE(undone->can_redo);

    // An edit after an undo starts a branch; the undone revision stays in the history.
    auto r3 = service_->apply(a(), with_exposure(-0.5));
    ASSERT_TRUE(r3);
    EXPECT_FALSE(r3->can_redo);
    auto history = service_->history(a());
    ASSERT_TRUE(history);
    ASSERT_EQ(history->entries.size(), 3u);
    EXPECT_EQ(history->entries[1].info.id, *r2->revision);
    EXPECT_FALSE(history->entries[1].on_current_path);
    EXPECT_TRUE(history->entries[0].on_current_path);
    EXPECT_TRUE(history->entries[2].current);
    EXPECT_EQ(history->entries[2].info.parent, r1->revision);

    ASSERT_TRUE(service_->undo(a()));
    auto redone = service_->redo(a());  // the newest child: the new branch
    ASSERT_TRUE(redone);
    EXPECT_EQ(redone->revision, r3->revision);
    EXPECT_EQ(service_->redo(a()).error().code, ErrorCode::InvalidArgument);

    // The old branch can be selected again.
    auto selected = service_->select_revision(a(), r2->revision);
    ASSERT_TRUE(selected);
    EXPECT_EQ(selected->settings, with_exposure(1.0));
    EXPECT_EQ(service_->revision_settings(a(), *r1->revision).value(), with_exposure(0.5));

    // Undo walks back to "as shot", then there is nothing left to undo.
    ASSERT_TRUE(service_->undo(a()));
    auto as_shot = service_->undo(a());
    ASSERT_TRUE(as_shot);
    EXPECT_FALSE(as_shot->revision);
    EXPECT_EQ(as_shot->settings, engine::DevelopSettings{});
    EXPECT_EQ(service_->undo(a()).error().code, ErrorCode::InvalidArgument);
    EXPECT_TRUE(service_->state(a())->can_redo);
}

TEST_F(RecipeServiceTest, ResetAndRevertAreRecordedSteps) {
    auto r1 = service_->apply(a(), with_exposure(0.5));
    ASSERT_TRUE(r1);
    ASSERT_TRUE(service_->set_lock(a(), DevelopControl::Exposure, true));
    engine::DevelopSettings contrasty = with_exposure(0.5);
    contrasty.contrast = 0.4;
    contrasty.locks = {DevelopControl::Exposure};
    ASSERT_TRUE(service_->apply(a(), contrasty));

    auto reset = service_->reset_to_as_shot(a());
    ASSERT_TRUE(reset) << reset.error().describe();
    engine::DevelopSettings expected;  // as shot, locks kept
    expected.locks = {DevelopControl::Exposure};
    EXPECT_EQ(reset->settings, expected);
    auto history = service_->history(a());
    ASSERT_TRUE(history);
    EXPECT_EQ(history->entries.back().info.origin, catalog::RevisionOrigin::Reset);
    // Resetting again writes nothing.
    EXPECT_EQ(service_->reset_to_as_shot(a())->revision, reset->revision);

    auto undone = service_->undo(a());
    ASSERT_TRUE(undone);
    EXPECT_EQ(undone->settings, contrasty);

    auto reverted = service_->revert_to(a(), *r1->revision);
    ASSERT_TRUE(reverted) << reverted.error().describe();
    EXPECT_EQ(reverted->settings, with_exposure(0.5));
    history = service_->history(a());
    ASSERT_TRUE(history);
    const HistoryEntry& last = history->entries.back();
    EXPECT_TRUE(last.current);
    EXPECT_EQ(last.reverted_from, 1u);
    EXPECT_EQ(last.info.parent, undone->revision);  // a step after the current one: undo returns
    EXPECT_EQ(service_->undo(a())->settings, contrasty);
    // A revision of another item is refused.
    auto other = service_->apply(b(), with_exposure(1.0));
    ASSERT_TRUE(other);
    EXPECT_EQ(service_->revert_to(a(), *other->revision).error().code, ErrorCode::NotFound);
    EXPECT_FALSE(service_->select_revision(a(), other->revision));
}

TEST_F(RecipeServiceTest, LocksAreStoredWithTheSettings) {
    auto locked = service_->set_lock(a(), DevelopControl::WhiteBalance, true);
    ASSERT_TRUE(locked) << locked.error().describe();
    EXPECT_TRUE(locked->settings.locks.contains(DevelopControl::WhiteBalance));
    ASSERT_TRUE(locked->revision);
    auto document = service_->current_settings(a());
    ASSERT_TRUE(document);
    EXPECT_NE(document->document.json.find(R"("locks":["white_balance"])"), std::string::npos);
    auto history = service_->history(a());
    ASSERT_TRUE(history);
    EXPECT_EQ(history->entries.back().locks, std::vector<DevelopControl>{DevelopControl::WhiteBalance});
    EXPECT_TRUE(history->entries.back().changed.empty());
    // Locking again changes nothing; unlocking is another revision.
    EXPECT_EQ(service_->set_lock(a(), DevelopControl::WhiteBalance, true)->revision, locked->revision);
    auto unlocked = service_->set_lock(a(), DevelopControl::WhiteBalance, false);
    ASSERT_TRUE(unlocked);
    EXPECT_NE(unlocked->revision, locked->revision);
    EXPECT_TRUE(unlocked->settings.locks.empty());
}

TEST_F(RecipeServiceTest, CameraCapabilitiesComeFromTheFile) {
    auto alpha = service_->camera(a());
    ASSERT_TRUE(alpha);
    EXPECT_TRUE(alpha->probed);
    EXPECT_EQ(alpha->model, "Alpha");
    EXPECT_TRUE(alpha->kelvin_available);  // DNG ColorMatrix1
    auto mono = service_->camera(m());
    ASSERT_TRUE(mono);
    EXPECT_TRUE(mono->probed);
    EXPECT_FALSE(mono->kelvin_available);
    EXPECT_FALSE(mono->kelvin_unavailable_reason.empty());
    EXPECT_TRUE(alpha->same_camera(*service_->camera(b())));
    EXPECT_FALSE(alpha->same_camera(*service_->camera(c())));

    // Kelvin edits are refused where the calibration cannot express them.
    engine::DevelopSettings kelvin;
    kelvin.white_balance = engine::KelvinWhiteBalance{4000.0, 0.0};
    EXPECT_EQ(service_->apply(m(), kelvin).error().code, ErrorCode::Unsupported);
    EXPECT_TRUE(service_->apply(a(), kelvin));
}

TEST_F(RecipeServiceTest, PasteNeverCarriesCameraSpecificValuesAcrossModels) {
    engine::DevelopSettings source;
    source.white_balance = engine::CameraNeutralWhiteBalance{2.1, 1.0, 1.6};
    source.exposure_ev = 0.7;
    source.contrast = 0.3;
    source.saturation = 0.2;
    source.locks = {DevelopControl::WhiteBalance, DevelopControl::Exposure};
    ASSERT_TRUE(service_->apply(a(), source));

    auto clipboard =
        service_->copy(a(), {RecipeCategory::WhiteBalance, RecipeCategory::Exposure, RecipeCategory::Tone});
    ASSERT_TRUE(clipboard) << clipboard.error().describe();
    EXPECT_EQ(clipboard->camera.model, "Alpha");
    const std::vector<ItemId> targets = {b(), c(), m()};
    auto outcomes = service_->paste(*clipboard, targets);
    ASSERT_TRUE(outcomes) << outcomes.error().describe();
    ASSERT_EQ(outcomes->size(), 3u);

    // Same camera model: everything chosen is pasted, with the locks of the pasted controls.
    const PasteOutcome& same = (*outcomes)[0];
    EXPECT_FALSE(same.error);
    EXPECT_TRUE(same.skipped.empty());
    EXPECT_EQ(same.applied.size(), 3u);
    ASSERT_TRUE(same.revision);
    engine::DevelopSettings expected_b = source;
    expected_b.saturation = 0.0;  // colour was not copied
    EXPECT_EQ(state(b()).settings, expected_b);
    EXPECT_EQ(service_->history(b())->entries.back().info.origin, catalog::RevisionOrigin::Paste);

    // Another model: the camera-neutral multipliers stay behind, and that is reported.
    for (const std::size_t i : {std::size_t{1}, std::size_t{2}}) {
        const PasteOutcome& other = (*outcomes)[i];
        EXPECT_FALSE(other.error);
        ASSERT_EQ(other.skipped.size(), 1u);
        EXPECT_EQ(other.skipped[0].category, RecipeCategory::WhiteBalance);
        EXPECT_NE(other.skipped[0].reason.find("camera-neutral"), std::string::npos);
        EXPECT_EQ(other.applied,
                  (std::vector<RecipeCategory>{RecipeCategory::Exposure, RecipeCategory::Tone}));
        const engine::DevelopSettings got = state(other.item).settings;
        EXPECT_EQ(got.white_balance, engine::WhiteBalanceSetting{engine::AsShotWhiteBalance{}});
        EXPECT_FALSE(got.locks.contains(DevelopControl::WhiteBalance));
        EXPECT_TRUE(got.locks.contains(DevelopControl::Exposure));
        EXPECT_EQ(got.exposure_ev, 0.7);
        EXPECT_EQ(got.contrast, 0.3);
    }

    // Kelvin crosses camera models, but only to a camera whose calibration supports it.
    engine::DevelopSettings kelvin;
    kelvin.white_balance = engine::KelvinWhiteBalance{3800.0, 5.0};
    ASSERT_TRUE(service_->apply(a(), kelvin));
    auto wb = service_->copy(a(), {RecipeCategory::WhiteBalance});
    ASSERT_TRUE(wb);
    auto kelvin_outcomes = service_->paste(*wb, targets);
    ASSERT_TRUE(kelvin_outcomes);
    EXPECT_TRUE((*kelvin_outcomes)[0].skipped.empty());
    EXPECT_TRUE((*kelvin_outcomes)[1].skipped.empty());
    EXPECT_EQ(state(c()).settings.white_balance, kelvin.white_balance);
    ASSERT_EQ((*kelvin_outcomes)[2].skipped.size(), 1u);
    EXPECT_NE((*kelvin_outcomes)[2].skipped[0].reason.find("Kelvin"), std::string::npos);
    EXPECT_FALSE((*kelvin_outcomes)[2].revision);  // nothing else was chosen: no revision
    EXPECT_EQ(state(m()).settings.white_balance, engine::WhiteBalanceSetting{engine::AsShotWhiteBalance{}});

    // Pasting the same values again changes nothing.
    auto again = service_->paste(*wb, std::vector<ItemId>{c()});
    ASSERT_TRUE(again);
    EXPECT_FALSE((*again)[0].revision);
    // Unknown targets are reported per target.
    auto unknown = service_->paste(*wb, std::vector<ItemId>{ItemId{999}});
    ASSERT_TRUE(unknown);
    ASSERT_TRUE((*unknown)[0].error);
    EXPECT_EQ((*unknown)[0].error->code, ErrorCode::NotFound);
    EXPECT_EQ(service_->copy(a(), {}).error().code, ErrorCode::InvalidArgument);
}

TEST(PasteShapes, SameUprightAspectWithinTheTolerance) {
    EXPECT_TRUE(same_upright_aspect({6000, 4000}, {6000, 4000}));
    EXPECT_TRUE(same_upright_aspect({6000, 4000}, {8192, 5464}));  // 3:2 sensors of other sizes
    EXPECT_TRUE(same_upright_aspect({6000, 4000}, {6048, 4024}));
    EXPECT_FALSE(same_upright_aspect({6000, 4000}, {4000, 3000}));  // 4:3
    EXPECT_FALSE(same_upright_aspect({6000, 4000}, {4000, 6000}));  // portrait
    EXPECT_FALSE(same_upright_aspect({6000, 4000}, {}));            // unknown
    EXPECT_FALSE(same_upright_aspect({}, {}));
}

TEST_F(RecipeServiceTest, MasksAndCropsArePastedOnlyToPhotosOfTheSameShape) {
    // A portrait photo (EXIF orientation 6: 48 x 64 upright) and a wide one (96 x 48) beside the
    // 64 x 48 landscape ones; the wide one has a mask of its own.
    rts::SyntheticDngSpec portrait_spec = camera("Alpha");
    portrait_spec.orientation = 6;
    rts::SyntheticDngSpec wide_spec = camera("Alpha");
    wide_spec.width = 96;
    const std::vector<ItemId> more = import_files(
        library_, *registry_,
        {ts::write_dng(dir_, L"portrait.dng", portrait_spec), ts::write_dng(dir_, L"wide.dng", wide_spec)});
    ASSERT_EQ(more.size(), 2u);
    const ItemId portrait = more[0];
    const ItemId wide = more[1];
    EXPECT_EQ(service_->camera(a())->upright_size, (image::Size{64, 48}));
    EXPECT_EQ(service_->camera(portrait)->upright_size, (image::Size{48, 64}));
    engine::DevelopSettings own;
    own.local_adjustments = {radial_mask(5, 0.25, 0.25)};
    ASSERT_TRUE(service_->apply(wide, own));

    const engine::DevelopSettings source = local_edit();
    ASSERT_TRUE(service_->apply(a(), source));
    // The default categories: everything but geometry.
    const std::set<RecipeCategory> defaults(kDefaultRecipeCategories.begin(), kDefaultRecipeCategories.end());
    auto clipboard = service_->copy(a(), defaults);
    ASSERT_TRUE(clipboard) << clipboard.error().describe();
    EXPECT_EQ(clipboard->camera.upright_size, (image::Size{64, 48}));
    const std::vector<ItemId> targets = {b(), portrait, wide};
    auto outcomes = service_->paste(*clipboard, targets);
    ASSERT_TRUE(outcomes) << outcomes.error().describe();
    ASSERT_EQ(outcomes->size(), 3u);

    // Same shape: masks and denoising with the rest of their categories; never the crop.
    const PasteOutcome& same = (*outcomes)[0];
    EXPECT_FALSE(same.error);
    EXPECT_TRUE(same.skipped.empty());
    ASSERT_TRUE(same.revision);
    engine::DevelopSettings expected = source;
    expected.geometry = engine::GeometrySetting{};
    EXPECT_EQ(state(b()).settings, expected);
    const HistoryEntry pasted = service_->history(b())->entries.back();
    EXPECT_EQ(pasted.info.origin, catalog::RevisionOrigin::Paste);
    EXPECT_EQ(pasted.parts, (std::vector<RecipePart>{RecipePart::ChromaDenoise, RecipePart::LocalAdjustments}));
    EXPECT_EQ(pasted.changed, (std::vector<DevelopControl>{DevelopControl::Highlights, DevelopControl::Shadows}));
    EXPECT_NE(pasted.info.note.find(";parts=chroma_denoise,masks"), std::string::npos) << pasted.info.note;

    // Another shape: highlights, shadows and denoising are pasted, the masks are not (reported),
    // and the target's own masks stay.
    for (const std::size_t i : {std::size_t{1}, std::size_t{2}}) {
        const PasteOutcome& other = (*outcomes)[i];
        EXPECT_FALSE(other.error);
        ASSERT_EQ(other.skipped.size(), 1u);
        EXPECT_EQ(other.skipped[0].category, RecipeCategory::Local);
        EXPECT_EQ(other.skipped[0].part, RecipePart::LocalAdjustments);
        EXPECT_NE(other.skipped[0].reason.find("masks are not pasted"), std::string::npos) << other.skipped[0].reason;
        EXPECT_NE(std::find(other.applied.begin(), other.applied.end(), RecipeCategory::Local), other.applied.end());
        const engine::DevelopSettings got = state(other.item).settings;
        EXPECT_EQ(got.highlights, source.highlights);
        EXPECT_EQ(got.shadows, source.shadows);
        EXPECT_EQ(got.chroma_denoise, source.chroma_denoise);
        EXPECT_EQ(got.local_adjustments,
                  other.item == wide ? own.local_adjustments : std::vector<engine::LocalAdjustment>{});
        EXPECT_TRUE(got.geometry.is_identity());
    }

    // Geometry is pasted only when chosen, and its crop only to the same shape.
    auto crop = service_->copy(a(), {RecipeCategory::Geometry});
    ASSERT_TRUE(crop);
    auto cropped = service_->paste(*crop, targets);
    ASSERT_TRUE(cropped);
    EXPECT_TRUE((*cropped)[0].skipped.empty());
    EXPECT_EQ(state(b()).settings.geometry, source.geometry);
    EXPECT_EQ(service_->history(b())->entries.back().parts, std::vector<RecipePart>{RecipePart::Geometry});
    for (const std::size_t i : {std::size_t{1}, std::size_t{2}}) {
        const PasteOutcome& other = (*cropped)[i];
        ASSERT_EQ(other.skipped.size(), 1u);
        EXPECT_EQ(other.skipped[0].category, RecipeCategory::Geometry);
        EXPECT_EQ(other.skipped[0].part, RecipePart::Geometry);
        EXPECT_TRUE(other.applied.empty());
        EXPECT_FALSE(other.revision);
        EXPECT_TRUE(state(other.item).settings.geometry.is_identity());
    }
    // A geometry without a crop (a quarter turn and a straighten angle) fits any photo.
    engine::DevelopSettings turned = source;
    turned.geometry = engine::GeometrySetting{};
    turned.geometry.quarter_turns = 3;
    turned.geometry.straighten_degrees = 1.5;
    ASSERT_TRUE(service_->apply(a(), turned));
    auto turn = service_->copy(a(), {RecipeCategory::Geometry});
    ASSERT_TRUE(turn);
    auto turned_outcomes = service_->paste(*turn, std::vector<ItemId>{portrait});
    ASSERT_TRUE(turned_outcomes);
    EXPECT_TRUE((*turned_outcomes)[0].skipped.empty());
    EXPECT_EQ(state(portrait).settings.geometry, turned.geometry);

    // Pasting the local category of a photo without masks removes the target's masks (same shape),
    // like any value of a pasted category.
    ASSERT_TRUE(service_->apply(c(), local_edit()));
    auto plain = service_->copy(m(), {RecipeCategory::Local});
    ASSERT_TRUE(plain);
    auto cleared = service_->paste(*plain, std::vector<ItemId>{c()});
    ASSERT_TRUE(cleared);
    EXPECT_TRUE((*cleared)[0].skipped.empty());
    EXPECT_TRUE(state(c()).settings.local_adjustments.empty());
    EXPECT_EQ(state(c()).settings.chroma_denoise, local_edit().chroma_denoise);  // detail was not copied
}

TEST_F(RecipeServiceTest, AMaskStrokeAndACropDragAreOneRevisionEach) {
    ASSERT_TRUE(service_->apply(a(), with_exposure(0.2)));
    const std::size_t before = service_->history(a())->entries.size();
    // A brush stroke: every pointer position adds a point to the stroke (a transient edit).
    engine::DevelopSettings s = with_exposure(0.2);
    engine::LocalAdjustment brush;
    brush.id = 1;
    brush.controls.exposure_ev = 0.4;
    brush.shape = engine::BrushMask{{engine::BrushStroke{}}};
    s.local_adjustments = {brush};
    auto& points = std::get<engine::BrushMask>(s.local_adjustments[0].shape).strokes[0].points;
    std::uint64_t generation = 0;
    for (int step = 0; step < 60; ++step) {
        points.push_back(engine::MaskPoint{0.1 + 0.01 * step, 0.5 + 0.002 * step});
        auto t = service_->set_transient(a(), s);
        ASSERT_TRUE(t) << t.error().describe();
        EXPECT_GT(t->transient_generation, generation);
        generation = t->transient_generation;
    }
    EXPECT_EQ(service_->history(a())->entries.size(), before);  // nothing written during the stroke
    auto stroke = service_->commit(a());
    ASSERT_TRUE(stroke) << stroke.error().describe();
    EXPECT_EQ(stroke->settings, s);
    auto history = service_->history(a());
    ASSERT_EQ(history->entries.size(), before + 1);
    EXPECT_TRUE(history->entries.back().changed.empty());
    EXPECT_EQ(history->entries.back().parts, std::vector<RecipePart>{RecipePart::LocalAdjustments});

    // A crop drag: one revision as well.
    for (int step = 1; step <= 25; ++step) {
        s.geometry.crop = engine::CropRect{0.004 * step, 0.0, 1.0, 1.0 - 0.004 * step};
        ASSERT_TRUE(service_->set_transient(a(), s));
    }
    ASSERT_TRUE(service_->commit(a()));
    history = service_->history(a());
    ASSERT_EQ(history->entries.size(), before + 2);
    EXPECT_EQ(history->entries.back().parts, std::vector<RecipePart>{RecipePart::Geometry});
    EXPECT_EQ(state(a()).settings.geometry.crop, (engine::CropRect{0.004 * 25, 0.0, 1.0, 1.0 - 0.004 * 25}));

    // Undo takes back the whole drag, then the whole stroke.
    ASSERT_TRUE(service_->undo(a()));
    EXPECT_TRUE(state(a()).settings.geometry.is_identity());
    EXPECT_EQ(state(a()).settings.local_adjustments, s.local_adjustments);
    ASSERT_TRUE(service_->undo(a()));
    EXPECT_EQ(state(a()).settings, with_exposure(0.2));

    // A stroke beyond the bounds of the contract is refused, not truncated.
    engine::DevelopSettings huge = s;
    auto& many = std::get<engine::BrushMask>(huge.local_adjustments[0].shape).strokes[0].points;
    many.assign(engine::kMaxStrokePoints + 1, engine::MaskPoint{0.5, 0.5});
    EXPECT_EQ(service_->set_transient(a(), huge).error().code, ErrorCode::InvalidArgument);
}

TEST_F(RecipeServiceTest, Version1SettingsAreKeptAsTheirConversion) {
    engine::DevelopSettings old = with_exposure(0.3);
    old.version = 1;
    auto transient = service_->set_transient(a(), old);
    ASSERT_TRUE(transient) << transient.error().describe();
    EXPECT_EQ(transient->settings, with_exposure(0.3));  // version 2
    auto committed = service_->commit(a());
    ASSERT_TRUE(committed);
    EXPECT_EQ(committed->committed, with_exposure(0.3));
    EXPECT_EQ(committed->committed_sha256, write_recipe(with_exposure(0.3))->sha256);
    // The same values as version 2 are no change.
    auto again = service_->apply(a(), with_exposure(0.3));
    ASSERT_TRUE(again);
    EXPECT_EQ(again->revision, committed->revision);
    EXPECT_EQ(service_->history(a())->entries.size(), 1u);
}

TEST_F(RecipeServiceTest, WithoutCalibrationKnowledgeKelvinIsNotPasted) {
    RecipeService blind(library_, create_memory_recipe_store());  // no registry
    engine::DevelopSettings kelvin;
    kelvin.white_balance = engine::KelvinWhiteBalance{5000.0, 0.0};
    kelvin.exposure_ev = 0.25;
    ASSERT_TRUE(blind.apply(a(), kelvin));
    auto clipboard = blind.copy(a(), {RecipeCategory::WhiteBalance, RecipeCategory::Exposure});
    ASSERT_TRUE(clipboard);
    auto outcomes = blind.paste(*clipboard, std::vector<ItemId>{c()});
    ASSERT_TRUE(outcomes);
    ASSERT_EQ((*outcomes)[0].skipped.size(), 1u);
    EXPECT_EQ((*outcomes)[0].skipped[0].category, RecipeCategory::WhiteBalance);
    EXPECT_EQ(blind.state(c())->settings.exposure_ev, 0.25);
}

TEST_F(RecipeServiceTest, ObserversSeeEveryChangeInOrder) {
    std::vector<RecipeChange> changes;
    Subscription subscription =
        service_->subscribe([&](const RecipeChange& change) { changes.push_back(change); });
    ASSERT_TRUE(service_->set_transient(a(), with_exposure(0.1)));
    ASSERT_TRUE(service_->set_transient(a(), with_exposure(0.2)));
    ASSERT_TRUE(service_->commit(a()));
    ASSERT_TRUE(service_->undo(a()));
    ASSERT_TRUE(service_->set_transient(a(), with_exposure(0.3)));
    ASSERT_TRUE(service_->discard_transient(a()));
    ASSERT_EQ(changes.size(), 6u);
    const std::vector<RecipeChangeKind> kinds = {RecipeChangeKind::Transient, RecipeChangeKind::Transient,
                                                 RecipeChangeKind::Committed, RecipeChangeKind::Moved,
                                                 RecipeChangeKind::Transient, RecipeChangeKind::Discarded};
    for (std::size_t i = 0; i < changes.size(); ++i) {
        EXPECT_EQ(changes[i].kind, kinds[i]) << i;
        EXPECT_EQ(changes[i].state.item, a());
        if (i > 0) {
            EXPECT_GT(changes[i].sequence, changes[i - 1].sequence);
        }
    }
    EXPECT_EQ(changes[2].origin, catalog::RevisionOrigin::User);
    EXPECT_EQ(changes[2].state.committed, with_exposure(0.2));
    subscription.reset();
    ASSERT_TRUE(service_->apply(a(), with_exposure(1.0)));
    EXPECT_EQ(changes.size(), 6u);
}

TEST_F(RecipeServiceTest, AnotherWritersRevisionIsAConflictNotAnOverwrite) {
    RecipeService other(library_, store_);
    ASSERT_TRUE(service_->state(a()));  // loaded (as shot)
    ASSERT_TRUE(other.apply(a(), with_exposure(1.0)));
    auto conflicting = service_->apply(a(), with_exposure(-1.0));
    ASSERT_FALSE(conflicting);
    EXPECT_EQ(conflicting.error().code, ErrorCode::AlreadyExists);
    // The service now shows the other writer's revision, and editing works again.
    EXPECT_EQ(state(a()).settings, with_exposure(1.0));
    EXPECT_TRUE(service_->apply(a(), with_exposure(-1.0)));
}

// ---- Catalog-backed --------------------------------------------------------------------------

// A memory store whose reads fail as a catalog's do for stored data this build cannot use: the
// current revision `corrupt` fails its SHA-256 check, or the whole history listing fails.
class DamagedStore final : public RecipeStore {
public:
    std::shared_ptr<RecipeStore> inner = create_memory_recipe_store();
    std::optional<catalog::RevisionId> corrupt;
    std::optional<Error> history_error;

    std::optional<Error> read_only_reason() const override { return inner->read_only_reason(); }
    Expected<catalog::RevisionHistory> history(ItemId item) const override {
        if (history_error) {
            return Unexpected(*history_error);
        }
        return inner->history(item);
    }
    Expected<std::optional<catalog::RecipeRevision>> current(ItemId item) const override {
        auto found = inner->current(item);
        if (found && *found && (*found)->info.id == corrupt) {
            return fail(ErrorCode::Corrupt,
                        std::format("the settings of recipe revision {} do not match their SHA-256", corrupt->value),
                        "catalog.revision");
        }
        return found;
    }
    Expected<std::optional<catalog::RecipeRevision>> find(catalog::RevisionId id) const override {
        return inner->find(id);
    }
    Expected<catalog::RevisionInfo> append(ItemId item, const catalog::NewRevision& revision,
                                           std::optional<catalog::RevisionId> expected_current) override {
        return inner->append(item, revision, expected_current);
    }
    Status set_current(ItemId item, std::optional<catalog::RevisionId> revision) override {
        return inner->set_current(item, revision);
    }
    Expected<std::optional<catalog::RevisionId>> undo(ItemId item) override { return inner->undo(item); }
    Expected<catalog::RevisionId> redo(ItemId item) override { return inner->redo(item); }
};

TEST_F(RecipeServiceTest, ADamagedCurrentRevisionOpensReadOnlyAndCanBeUndone) {
    auto damaged = std::make_shared<DamagedStore>();
    RecipeService service(library_, damaged);
    ASSERT_TRUE(service.apply(a(), with_exposure(0.5)));
    auto second = service.apply(a(), with_exposure(-3.2));
    ASSERT_TRUE(second);
    damaged->corrupt = second->revision;

    // A fresh service (as after a restart) reads the damaged current revision.
    RecipeService reopened(library_, damaged);
    auto s = reopened.state(a());
    ASSERT_TRUE(s) << s.error().describe();
    ASSERT_TRUE(s->read_only);
    EXPECT_TRUE(s->recipe_unreadable);
    EXPECT_EQ(s->read_only->code, ErrorCode::Corrupt);
    EXPECT_EQ(s->read_only->where, kRecipeUnreadableWhere);
    EXPECT_EQ(s->revision, second->revision);
    EXPECT_EQ(s->revision_number, 2u);
    EXPECT_TRUE(s->can_undo);
    // Never rendered with other settings, never edited on top of.
    auto current = reopened.current_settings(a());
    ASSERT_FALSE(current);
    EXPECT_EQ(current.error().code, ErrorCode::Corrupt);
    EXPECT_EQ(current.error().where, kRecipeUnreadableWhere);
    EXPECT_EQ(reopened.apply(a(), with_exposure(1.0)).error().code, ErrorCode::Corrupt);
    // The history still lists every revision.
    auto history = reopened.history(a());
    ASSERT_TRUE(history);
    EXPECT_EQ(history->entries.size(), 2u);
    // Undo shows the readable parent again.
    auto undone = reopened.undo(a());
    ASSERT_TRUE(undone) << undone.error().describe();
    EXPECT_FALSE(undone->read_only);
    EXPECT_EQ(undone->settings, with_exposure(0.5));
    // ... and reset to as shot is possible from the damaged one too.
    ASSERT_TRUE(reopened.redo(a()));
    EXPECT_TRUE(reopened.state(a())->recipe_unreadable);
    auto reset = reopened.reset_to_as_shot(a());
    ASSERT_TRUE(reset) << reset.error().describe();
    EXPECT_FALSE(reset->read_only);
    EXPECT_TRUE(reopened.current_settings(a()));
}

TEST_F(RecipeServiceTest, AHistoryThatCannotBeListedLeavesTheCurrentRecipeUsable) {
    auto damaged = std::make_shared<DamagedStore>();
    RecipeService service(library_, damaged);
    ASSERT_TRUE(service.apply(a(), with_exposure(0.5)));
    damaged->history_error = Error{ErrorCode::VersionMismatch,
                                   "recipe_revisions.revision_number holds '0', a value written by a newer Lucent",
                                   "catalog.query", 0};

    RecipeService reopened(library_, damaged);
    auto s = reopened.state(a());
    ASSERT_TRUE(s) << s.error().describe();
    EXPECT_FALSE(s->read_only);
    EXPECT_EQ(s->settings, with_exposure(0.5));
    auto current = reopened.current_settings(a());
    ASSERT_TRUE(current);
    EXPECT_EQ(current->settings, with_exposure(0.5));
    EXPECT_EQ(reopened.history(a()).error().code, ErrorCode::VersionMismatch);
    // Store failures that are not about stored data still fail.
    damaged->history_error = Error{ErrorCode::IoError, "disk I/O error", "catalog.query", 0};
    RecipeService failing(library_, damaged);
    EXPECT_EQ(failing.state(a()).error().code, ErrorCode::IoError);
}

class CatalogRecipeTest : public ::testing::Test {
protected:
    std::filesystem::path catalog_path() const { return dir_.path() / L"catalog" / L"lucent.catalog"; }

    void open() {
        fixture_ = ts::CatalogFixture::open(catalog_path());
        ASSERT_TRUE(fixture_.library);
        auto store = create_catalog_recipe_store(fixture_.catalog);
        ASSERT_TRUE(store);
        RecipeServiceOptions options;
        options.registry = registry_;
        service_ = std::make_unique<RecipeService>(*fixture_.library, *store, options);
    }

    void close() {
        service_.reset();
        fixture_.close();
    }

    ts::ScratchDir dir_;
    std::shared_ptr<const raw::DecoderRegistry> registry_ = ts::default_registry();
    ts::CatalogFixture fixture_;
    std::unique_ptr<RecipeService> service_;
};

TEST_F(CatalogRecipeTest, EditsAndHistorySurviveARestart) {
    open();
    const std::vector<ItemId> items =
        import_files(*fixture_.library, *registry_, {ts::write_dng(dir_, L"photos\\a.dng")});
    ASSERT_EQ(items.size(), 1u);
    const ItemId item = items[0];
    engine::DevelopSettings first = with_exposure(0.4);
    first.white_balance = engine::KelvinWhiteBalance{4500.0, 3.0};
    ASSERT_TRUE(service_->apply(item, first));
    engine::DevelopSettings second = first;
    second.tone_curve.points = {{0.0, 0.05}, {0.5, 0.6}, {1.0, 1.0}};
    second.locks = {DevelopControl::ToneCurve};
    ASSERT_TRUE(service_->apply(item, second));
    ASSERT_TRUE(service_->undo(item));
    engine::DevelopSettings branch = first;
    branch.vibrance = 0.35;
    ASSERT_TRUE(service_->apply(item, branch));
    // A pending gesture is not a revision: it does not survive.
    ASSERT_TRUE(service_->set_transient(item, with_exposure(3.0)));
    const RecipeState before = *service_->state(item);
    const RecipeHistory history_before = *service_->history(item);
    close();

    open();
    const RecipeState after = *service_->state(item);
    EXPECT_FALSE(after.transient);
    EXPECT_EQ(after.revision, before.revision);
    EXPECT_EQ(after.settings, branch);
    EXPECT_EQ(after.committed_sha256, before.committed_sha256);
    EXPECT_TRUE(after.can_undo);
    const RecipeHistory history_after = *service_->history(item);
    ASSERT_EQ(history_after.entries.size(), history_before.entries.size());
    for (std::size_t i = 0; i < history_after.entries.size(); ++i) {
        const HistoryEntry& x = history_before.entries[i];
        const HistoryEntry& y = history_after.entries[i];
        EXPECT_EQ(x.info.id, y.info.id);
        EXPECT_EQ(x.info.parent, y.info.parent);
        EXPECT_EQ(x.info.settings_sha256, y.info.settings_sha256);
        EXPECT_EQ(x.info.note, y.info.note);
        EXPECT_EQ(x.on_current_path, y.on_current_path);
    }
    // The kept branch is still there after the restart.
    auto redo_target = service_->revision_settings(item, history_after.entries[1].info.id);
    ASSERT_TRUE(redo_target);
    EXPECT_EQ(*redo_target, second);
    close();
}

TEST_F(CatalogRecipeTest, MasksGeometryAndDenoisingSurviveARestart) {
    open();
    const std::vector<ItemId> items =
        import_files(*fixture_.library, *registry_, {ts::write_dng(dir_, L"photos\\a.dng")});
    ASSERT_EQ(items.size(), 1u);
    const ItemId item = items[0];
    ASSERT_TRUE(service_->apply(item, local_edit()));
    engine::DevelopSettings moved = local_edit();
    moved.local_adjustments[0] = radial_mask(1, 0.6, 0.4);  // the mask dragged elsewhere
    ASSERT_TRUE(service_->apply(item, moved));
    const RecipeState before = *service_->state(item);
    close();

    open();
    const RecipeState after = *service_->state(item);
    EXPECT_FALSE(after.read_only);
    EXPECT_EQ(after.settings, moved);
    EXPECT_EQ(after.committed_sha256, before.committed_sha256);
    EXPECT_EQ(after.committed_sha256, write_recipe(moved)->sha256);
    auto history = service_->history(item);
    ASSERT_TRUE(history);
    ASSERT_EQ(history->entries.size(), 2u);
    EXPECT_EQ(history->entries[0].parts,
              (std::vector<RecipePart>{RecipePart::ChromaDenoise, RecipePart::LocalAdjustments, RecipePart::Geometry}));
    EXPECT_EQ(history->entries[1].parts, std::vector<RecipePart>{RecipePart::LocalAdjustments});
    EXPECT_EQ(history->entries[1].info.schema_version, kRecipeSchemaVersion);
    auto first = service_->revision_settings(item, history->entries[0].info.id);
    ASSERT_TRUE(first);
    EXPECT_EQ(*first, local_edit());
    close();
}

TEST_F(CatalogRecipeTest, AVersion1RevisionIsReadAsItsUpgrade) {
    open();
    const std::vector<ItemId> items =
        import_files(*fixture_.library, *registry_, {ts::write_dng(dir_, L"photos\\a.dng")});
    ASSERT_EQ(items.size(), 1u);
    const ItemId item = items[0];
    close();

    // A revision a version-1 build wrote (its canonical version-1 text).
    {
        auto catalog = ts::open_catalog(catalog_path());
        ASSERT_TRUE(catalog);
        catalog::NewRevision revision;
        revision.schema_name = "lucent.develop";
        revision.schema_version = 1;
        revision.settings_json =
            R"({"base_look":{"id":"neutral","version":1},"contrast":0,"exposure_ev":0.5,"highlights":0,)"
            R"("locks":["exposure"],"operations":{"color":1,"contrast":1,"exposure":1,"local_tone":1,)"
            R"("tone_curve":1,"white_balance":1},"saturation":0,"schema":"lucent.develop","shadows":0,)"
            R"("tone_curve":[[0,0],[1,1]],"version":1,"vibrance":0,"white_balance":{"mode":"as_shot"}})";
        revision.engine_version = "0.8.0";
        revision.created_at = catalog::current_time();
        ASSERT_TRUE(catalog->write([&](catalog::WriteTransaction& tx) -> Status {
            auto appended = tx.append_revision(catalog::ItemId{static_cast<std::int64_t>(item.value)}, revision);
            if (!appended) {
                return Unexpected(appended.error());
            }
            return ok();
        }));
    }

    open();
    engine::DevelopSettings expected = with_exposure(0.5);
    expected.locks = {DevelopControl::Exposure};
    const RecipeState s = *service_->state(item);
    EXPECT_FALSE(s.read_only);
    EXPECT_EQ(s.settings, expected);
    EXPECT_EQ(s.settings.version, engine::kDevelopSettingsVersion);
    // Its identity is the version-2 one of the same settings (previews and exports agree with it).
    EXPECT_EQ(s.committed_sha256, write_recipe(expected)->sha256);
    auto current = service_->current_settings(item);
    ASSERT_TRUE(current);
    EXPECT_NE(current->document.json.find(R"("version":2)"), std::string::npos);
    // An edit on top of it is a version-2 revision with a mask.
    engine::DevelopSettings masked = expected;
    masked.local_adjustments = {radial_mask(1)};
    auto edited = service_->apply(item, masked);
    ASSERT_TRUE(edited) << edited.error().describe();
    auto history = service_->history(item);
    ASSERT_TRUE(history);
    ASSERT_EQ(history->entries.size(), 2u);
    EXPECT_EQ(history->entries[0].info.schema_version, 1u);
    EXPECT_EQ(history->entries[1].info.schema_version, 2u);
    EXPECT_EQ(history->entries[1].parts, std::vector<RecipePart>{RecipePart::LocalAdjustments});
    close();
}

TEST_F(CatalogRecipeTest, ARecipeFromANewerLucentOpensReadOnly) {
    open();
    const std::vector<ItemId> items =
        import_files(*fixture_.library, *registry_, {ts::write_dng(dir_, L"photos\\a.dng")});
    ASSERT_EQ(items.size(), 1u);
    const ItemId item = items[0];
    ASSERT_TRUE(service_->apply(item, with_exposure(0.5)));
    close();

    // A newer Lucent appended a revision with an operation version this build does not know.
    {
        auto catalog = ts::open_catalog(catalog_path());
        ASSERT_TRUE(catalog);
        auto text = write_recipe(with_exposure(0.75));
        ASSERT_TRUE(text);
        std::string newer = text->json;
        const std::string from = R"("exposure":1)";
        newer.replace(newer.find(from), from.size(), R"("exposure":2)");
        catalog::NewRevision revision;
        revision.schema_name = "lucent.develop";
        revision.schema_version = 1;
        revision.settings_json = newer;
        revision.engine_version = "9.0.0";
        revision.created_at = catalog::current_time();
        ASSERT_TRUE(catalog->write([&](catalog::WriteTransaction& tx) -> Status {
            auto appended =
                tx.append_revision(catalog::ItemId{static_cast<std::int64_t>(item.value)}, revision);
            if (!appended) {
                return Unexpected(appended.error());
            }
            return ok();
        }));
    }

    open();
    const RecipeState s = *service_->state(item);
    ASSERT_TRUE(s.read_only);
    EXPECT_TRUE(s.recipe_unreadable);
    EXPECT_EQ(s.read_only->code, ErrorCode::VersionMismatch);
    // Never rendered or exported with other settings, never edited on top of.
    EXPECT_EQ(service_->current_settings(item).error().code, ErrorCode::VersionMismatch);
    EXPECT_EQ(service_->apply(item, with_exposure(1.0)).error().code, ErrorCode::VersionMismatch);
    EXPECT_EQ(service_->set_lock(item, DevelopControl::Exposure, true).error().code,
              ErrorCode::VersionMismatch);
    EXPECT_EQ(service_->copy(item, {RecipeCategory::Exposure}).error().code, ErrorCode::VersionMismatch);
    EXPECT_EQ(service_->history(item)->entries.size(), 2u);
    // Moving away from it is possible: undo shows the readable revision again.
    auto undone = service_->undo(item);
    ASSERT_TRUE(undone) << undone.error().describe();
    EXPECT_FALSE(undone->read_only);
    EXPECT_EQ(undone->settings, with_exposure(0.5));
    close();
}

TEST_F(CatalogRecipeTest, AReadOnlyCatalogRefusesEveryChange) {
    open();
    const std::vector<ItemId> items =
        import_files(*fixture_.library, *registry_, {ts::write_dng(dir_, L"photos\\a.dng")});
    ASSERT_EQ(items.size(), 1u);
    ASSERT_TRUE(service_->apply(items[0], with_exposure(0.5)));
    close();

    auto catalog = ts::open_catalog(catalog_path(), catalog::OpenMode::ReadOnly);
    ASSERT_TRUE(catalog);
    auto library = CatalogLibrary::open(catalog);
    ASSERT_TRUE(library);
    auto store = create_catalog_recipe_store(catalog);
    ASSERT_TRUE(store);
    RecipeService service(**library, *store);
    auto s = service.state(items[0]);
    ASSERT_TRUE(s);
    EXPECT_EQ(s->settings, with_exposure(0.5));
    ASSERT_TRUE(s->read_only);
    EXPECT_FALSE(s->recipe_unreadable);
    EXPECT_TRUE(service.current_settings(items[0]));  // still renderable
    EXPECT_EQ(service.apply(items[0], with_exposure(1.0)).error().code, ErrorCode::PermissionDenied);
    EXPECT_EQ(service.undo(items[0]).error().code, ErrorCode::PermissionDenied);
}

}  // namespace
}  // namespace lucent::app
