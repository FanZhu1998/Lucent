// AppEditService (W6, docs/adr/0010) on the real application services with synthetic DNGs: an edit
// session renders frames in order, a gesture (transient positions, then commit) writes exactly one
// revision, undo/redo/reset move through the history, a recipe made by a newer Lucent opens
// read-only with its reason, paste never carries a camera-specific white balance to another camera
// model and says so, before/after and the clipping overlay come from the session, and an edited
// photo's library thumbnail becomes Lucent's render of its recipe with the Edited flag. W9: frames
// carry their geometry and mask outlines, the crop tool's frames are uncropped, the mask overlay is
// the selected mask's weights, and noise reduction, masks and the crop paste by category with the
// masks and the crop kept off a photo of another shape (and said so).

#include <QCoreApplication>
#include <QFileInfo>
#include <QThread>
#include <memory>
#include <optional>

#include <gtest/gtest.h>

#include <lucent/application/recipe.hpp>
#include <lucent/catalog/types.hpp>

#include "adapter_test_support.hpp"
#include "adapters/app_edit_service.hpp"

namespace lucent::ui {
namespace {

using adapters::AppEditService;
using adapters::AppLibraryService;
using testing::kAdapterWaitMs;
using testing::request_image;
using testing::wait_completed;
using testing::write_dng;
namespace rts = lucent::raw::test_support;

class AppEditServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto backend = adapters::Backend::create(testing::test_backend_options());
        ASSERT_TRUE(backend) << backend.error().describe();
        backend_ = *backend;
        library_ = std::make_unique<AppLibraryService>(backend_);
        images_ = std::make_shared<adapters::AppImageSource>(backend_, library_->render_event_sink());
        edits_ = std::make_unique<AppEditService>(backend_, library_->render_event_sink());
        QObject::connect(library_.get(), &LibraryService::items_added, library_.get(),
                         [this](const QList<LibraryItem>& items) { added_ << items; });
        QObject::connect(library_.get(), &LibraryService::import_finished, library_.get(),
                         [this](const ImportSummary&) { ++imports_; });
        QObject::connect(edits_.get(), &EditService::state_changed, edits_.get(), [this](const EditState& state) {
            off_ui_thread_ |= QThread::currentThread() != QCoreApplication::instance()->thread();
            states_ << state;
        });
        QObject::connect(edits_.get(), &EditService::frame_ready, edits_.get(), [this](const EditFramePtr& frame) {
            off_ui_thread_ |= QThread::currentThread() != QCoreApplication::instance()->thread();
            frames_ << frame;
        });
        QObject::connect(edits_.get(), &EditService::history_changed, edits_.get(),
                         [this](ItemId, const QList<EditHistoryEntry>& entries) { history_ = entries; });
        QObject::connect(edits_.get(), &EditService::frame_failed, edits_.get(),
                         [this](ItemId, const QString& message) { frame_failures_ << message; });
        QObject::connect(edits_.get(), &EditService::command_failed, edits_.get(),
                         [this](ItemId, const QString& message) { command_failures_ << message; });
        QObject::connect(edits_.get(), &EditService::white_balance_changed, edits_.get(),
                         [this](ItemId, const WhiteBalanceInfo& info) { white_balance_ = info; });
        QObject::connect(edits_.get(), &EditService::pasted, edits_.get(),
                         [this](const PasteReport& report) { pastes_ << report; });
        QObject::connect(edits_.get(), &EditService::copied, edits_.get(), [this](ItemId, quint32) { ++copies_; });
    }

    void TearDown() override {
        // As Services: the edit service first, the library service (the event sink's owner) last.
        edits_.reset();
        images_.reset();
        library_.reset();
        backend_.reset();
    }

    ItemId import_file(const std::wstring& name, const std::string& model = "Synthetic DNG", std::uint32_t width = 64,
                       std::uint32_t height = 48) {
        rts::SyntheticDngSpec spec = testing::dng_with_preview();
        spec.model = model;
        spec.width = width;
        spec.height = height;
        const std::filesystem::path path = write_dng(dir_, name, spec);
        const int before = imports_;
        library_->import_paths({QString::fromStdWString(path.wstring())}, false);
        EXPECT_TRUE(QTest::qWaitFor([&] { return imports_ > before; }, kAdapterWaitMs));
        for (const LibraryItem& item : added_) {
            if (QFileInfo(item.file_path).fileName() == QString::fromStdWString(name)) {
                return item.id;
            }
        }
        ADD_FAILURE() << "not imported";
        return 0;
    }

    [[nodiscard]] bool wait_idle() {
        return QTest::qWaitFor([&] { return !edits_->is_busy(); }, kAdapterWaitMs);
    }

    // The newest state of `item` that answers `token` (0: any).
    [[nodiscard]] std::optional<EditState> state_for(ItemId item, quint64 token = 0) const {
        for (auto it = states_.crbegin(); it != states_.crend(); ++it) {
            if (it->item == item && (token == 0 || it->token == token)) {
                return *it;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool wait_state(ItemId item, quint64 token) {
        return QTest::qWaitFor([&] { return state_for(item, token).has_value(); }, kAdapterWaitMs);
    }

    [[nodiscard]] bool wait_final_frame(ItemId item, quint64 token) {
        return QTest::qWaitFor(
            [&] {
                for (const EditFramePtr& frame : frames_) {
                    if (frame->item == item && frame->token == token && frame->phase == FramePhase::Final &&
                        !frame->before) {
                        return true;
                    }
                }
                return false;
            },
            kAdapterWaitMs);
    }

    rts::ScratchDir dir_;
    std::shared_ptr<adapters::Backend> backend_;
    std::unique_ptr<AppLibraryService> library_;
    std::shared_ptr<adapters::AppImageSource> images_;
    std::unique_ptr<AppEditService> edits_;
    QList<LibraryItem> added_;
    int imports_ = 0;
    QList<EditState> states_;
    QList<EditFramePtr> frames_;
    QList<EditHistoryEntry> history_;
    QStringList frame_failures_;
    QStringList command_failures_;
    std::optional<WhiteBalanceInfo> white_balance_;
    QList<PasteReport> pastes_;
    int copies_ = 0;
    bool off_ui_thread_ = false;
};

TEST_F(AppEditServiceTest, AGestureIsOneRevisionAndUndoRedoAndResetMoveThroughTheHistory) {
    const ItemId id = import_file(L"a.dng");
    edits_->open(id);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !frames_.isEmpty(); }, kAdapterWaitMs));
    ASSERT_TRUE(wait_idle());
    ASSERT_TRUE(state_for(id).has_value());
    EXPECT_EQ(state_for(id)->revision, 0u);
    EXPECT_FALSE(state_for(id)->can_undo);
    EXPECT_FALSE(state_for(id)->read_only);
    EXPECT_EQ(frames_.front()->image.size(), QSize(64, 48));
    EXPECT_FALSE(frames_.front()->info == nullptr);
    ASSERT_TRUE(QTest::qWaitFor([&] { return white_balance_.has_value(); }, kAdapterWaitMs));
    EXPECT_TRUE(white_balance_->known);

    // A drag: three positions, then the release.
    EditSettings settings = state_for(id)->settings;
    quint64 token = 0;
    for (const double ev : {0.25, 0.5, 0.75}) {
        settings.exposure_ev = ev;
        token = edits_->set_transient(id, settings);
    }
    const quint64 commit = edits_->commit(id);
    ASSERT_TRUE(wait_state(id, commit));
    ASSERT_TRUE(wait_final_frame(id, commit));
    ASSERT_TRUE(wait_idle());
    const EditState committed = *state_for(id, commit);
    EXPECT_EQ(committed.revision_number, 1u);
    EXPECT_DOUBLE_EQ(committed.committed.exposure_ev, 0.75);
    EXPECT_TRUE(committed.can_undo);
    EXPECT_TRUE(committed.edited);
    ASSERT_EQ(history_.size(), 1);  // ONE revision for the gesture
    EXPECT_EQ(history_.front().changed, QList<EditControl>{EditControl::Exposure});
    EXPECT_EQ(history_.front().origin, QStringLiteral("user"));
    // Frames come in order, never older than one already delivered, with their call's token.
    for (qsizetype i = 1; i < frames_.size(); ++i) {
        EXPECT_GT(frames_[i]->sequence, frames_[i - 1]->sequence);
        EXPECT_GE(frames_[i]->generation, frames_[i - 1]->generation);
    }
    EXPECT_LE(token, commit);

    settings.contrast = 0.3;
    const quint64 applied = edits_->apply(id, settings);
    ASSERT_TRUE(wait_state(id, applied));
    ASSERT_TRUE(wait_idle());
    EXPECT_EQ(history_.size(), 2);

    const quint64 undone = edits_->undo(id);
    ASSERT_TRUE(wait_state(id, undone));
    EXPECT_EQ(state_for(id, undone)->revision_number, 1u);
    EXPECT_TRUE(state_for(id, undone)->can_redo);
    EXPECT_DOUBLE_EQ(state_for(id, undone)->settings.contrast, 0.0);
    const quint64 redone = edits_->redo(id);
    ASSERT_TRUE(wait_state(id, redone));
    EXPECT_EQ(state_for(id, redone)->revision_number, 2u);

    const quint64 reset = edits_->reset_to_as_shot(id);
    ASSERT_TRUE(wait_state(id, reset));
    ASSERT_TRUE(wait_idle());
    EXPECT_TRUE(state_for(id, reset)->committed.renders_as_shot());
    EXPECT_FALSE(state_for(id, reset)->edited);
    ASSERT_EQ(history_.size(), 3);
    EXPECT_EQ(history_.back().origin, QStringLiteral("reset"));

    // Selecting "as shot" moves the pointer without writing.
    const quint64 selected = edits_->select_revision(id, 0);
    ASSERT_TRUE(wait_state(id, selected));
    EXPECT_EQ(state_for(id, selected)->revision, 0u);
    EXPECT_TRUE(command_failures_.isEmpty()) << command_failures_.join('\n').toStdString();
    EXPECT_FALSE(off_ui_thread_);
}

TEST_F(AppEditServiceTest, ARecipeFromANewerLucentOpensReadOnlyWithItsReason) {
    const ItemId id = import_file(L"newer.dng");
    catalog::NewRevision revision;
    revision.origin = catalog::RevisionOrigin::User;
    revision.schema_name = std::string(app::kRecipeSchema);
    revision.schema_version = app::kRecipeSchemaVersion + 1;
    revision.settings_json = R"({"schema":"lucent.develop","version":2,"film_grain":0.4})";
    revision.created_at = catalog::current_time();
    ASSERT_TRUE(backend_->recipe_store().append(app::ItemId{id}, revision, std::nullopt));

    edits_->open(id);
    ASSERT_TRUE(QTest::qWaitFor([&] { return state_for(id).has_value() && !frame_failures_.isEmpty(); },
                                kAdapterWaitMs));
    const EditState state = *state_for(id);
    EXPECT_TRUE(state.read_only);
    EXPECT_TRUE(state.read_only_reason.contains(QStringLiteral("newer version of Lucent")))
        << state.read_only_reason.toStdString();
    EXPECT_TRUE(state.history_available);
    EXPECT_TRUE(state.can_undo);
    EXPECT_TRUE(state.edited);
    // Nothing else is rendered in its place.
    EXPECT_EQ(frame_failures_.front(), state.read_only_reason);
    EXPECT_TRUE(frames_.isEmpty());

    EditSettings settings;
    settings.exposure_ev = 1.0;
    const quint64 refused = edits_->apply(id, settings);
    ASSERT_TRUE(wait_state(id, refused));
    ASSERT_FALSE(command_failures_.isEmpty());
    ASSERT_TRUE(wait_idle());
    EXPECT_EQ(history_.size(), 1);

    // Moving away from it is possible and renders again.
    const quint64 undone = edits_->undo(id);
    ASSERT_TRUE(wait_state(id, undone));
    EXPECT_FALSE(state_for(id, undone)->read_only);
    ASSERT_TRUE(wait_final_frame(id, undone));
}

TEST_F(AppEditServiceTest, CommandsQueuedAtShutdownStillReachTheRecipes) {
    const ItemId id = import_file(L"shutdown.dng");
    edits_->open(id);
    ASSERT_TRUE(QTest::qWaitFor([&] { return state_for(id).has_value(); }, kAdapterWaitMs));
    ASSERT_TRUE(wait_idle());
    // A gesture's last positions and its commit (as EditSession::set_item(0) issues it from the
    // controller's destructor), then a typed value, all still queued when the service goes away.
    EditSettings settings = state_for(id)->settings;
    for (const double ev : {0.25, 0.5, 0.75}) {
        settings.exposure_ev = ev;
        edits_->set_transient(id, settings);
    }
    edits_->commit(id);
    settings.contrast = 0.3;
    edits_->apply(id, settings);
    edits_->close(id);
    edits_.reset();

    auto state = backend_->recipes().state(app::ItemId{id});
    ASSERT_TRUE(state) << state.error().describe();
    EXPECT_EQ(state->revision_number, 2u);
    EXPECT_FALSE(state->transient);
    EXPECT_DOUBLE_EQ(state->committed.exposure_ev, 0.75);
    EXPECT_DOUBLE_EQ(state->committed.contrast, 0.3);
}

TEST_F(AppEditServiceTest, PasteKeepsCameraSpecificWhiteBalanceOnItsCameraAndSaysWhy) {
    const ItemId source = import_file(L"source.dng", "Synthetic A");
    const ItemId other = import_file(L"other.dng", "Synthetic B");
    edits_->open(source);
    ASSERT_TRUE(QTest::qWaitFor([&] { return state_for(source).has_value(); }, kAdapterWaitMs));
    ASSERT_TRUE(wait_idle());
    EditSettings settings = state_for(source)->settings;
    settings.white_balance = WhiteBalanceMode::CameraNeutral;
    settings.neutral = {2.0, 1.0, 1.5};
    settings.exposure_ev = 0.7;
    settings.saturation = 0.2;
    settings.set_locked(EditControl::Exposure, true);
    const quint64 applied = edits_->apply(source, settings);
    ASSERT_TRUE(wait_state(source, applied));
    ASSERT_TRUE(wait_idle());
    ASSERT_EQ(state_for(source, applied)->revision_number, 1u) << command_failures_.join('\n').toStdString();

    EXPECT_FALSE(edits_->has_clipboard());
    edits_->copy_settings(source, kAllCategoryBits);
    ASSERT_TRUE(QTest::qWaitFor([&] { return copies_ == 1; }, kAdapterWaitMs));
    EXPECT_TRUE(edits_->has_clipboard());

    edits_->close(source);
    edits_->open(other);
    ASSERT_TRUE(QTest::qWaitFor([&] { return state_for(other).has_value(); }, kAdapterWaitMs));
    edits_->paste_settings({other});
    ASSERT_TRUE(QTest::qWaitFor([&] { return pastes_.size() == 1; }, kAdapterWaitMs));
    const PasteReport& report = pastes_.front();
    EXPECT_EQ(report.targets, 1);
    EXPECT_EQ(report.changed, 1);
    EXPECT_TRUE(report.applied.contains(SettingsCategory::Exposure));
    EXPECT_TRUE(report.applied.contains(SettingsCategory::Color));
    EXPECT_FALSE(report.applied.contains(SettingsCategory::WhiteBalance));
    ASSERT_EQ(report.skipped.size(), 1);
    EXPECT_TRUE(report.skipped.front().startsWith(QStringLiteral("White balance: ")))
        << report.skipped.front().toStdString();
    EXPECT_TRUE(report.skipped.front().contains(QStringLiteral("Synthetic A"))) << report.skipped.front().toStdString();
    EXPECT_TRUE(report.errors.isEmpty());
    ASSERT_TRUE(wait_idle());
    const EditState pasted = *state_for(other);
    EXPECT_DOUBLE_EQ(pasted.committed.exposure_ev, 0.7);
    EXPECT_DOUBLE_EQ(pasted.committed.saturation, 0.2);
    EXPECT_EQ(pasted.committed.white_balance, WhiteBalanceMode::AsShot);
    EXPECT_TRUE(pasted.committed.locked(EditControl::Exposure));  // locks travel with their controls
    ASSERT_FALSE(history_.isEmpty());
    EXPECT_EQ(history_.back().origin, QStringLiteral("paste"));
}

TEST_F(AppEditServiceTest, BeforeAfterAndClippingFramesComeFromTheSession) {
    const ItemId id = import_file(L"compare.dng");
    edits_->open(id);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !frames_.isEmpty(); }, kAdapterWaitMs));
    ASSERT_TRUE(wait_idle());
    EditSettings settings = state_for(id)->settings;
    settings.exposure_ev = 2.0;
    const quint64 applied = edits_->apply(id, settings);
    ASSERT_TRUE(wait_final_frame(id, applied));

    edits_->set_comparison(id, true);
    ASSERT_TRUE(QTest::qWaitFor(
        [&] { return std::any_of(frames_.cbegin(), frames_.cend(), [](const EditFramePtr& f) { return f->before; }); },
        kAdapterWaitMs));
    const auto before = std::find_if(frames_.cbegin(), frames_.cend(), [](const EditFramePtr& f) { return f->before; });
    EXPECT_EQ((*before)->revision, 0u);
    ASSERT_FALSE((*before)->image.isNull());
    // As shot is darker than the +2 EV edit.
    const auto current = std::find_if(frames_.crbegin(), frames_.crend(),
                                      [applied](const EditFramePtr& f) { return f->token == applied; });
    ASSERT_NE(current, frames_.crend());
    EXPECT_LT(qGray((*before)->image.pixel(32, 24)), qGray((*current)->image.pixel(32, 24)));
    ASSERT_TRUE(wait_idle());

    edits_->set_clipping(true);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !frames_.back()->clipping.isNull(); }, kAdapterWaitMs));
    const EditFramePtr overlay = frames_.back();
    EXPECT_FALSE(overlay->before);
    EXPECT_EQ(overlay->clipping.size(), overlay->image.size());
    EXPECT_EQ(overlay->clipping.format(), QImage::Format_ARGB32_Premultiplied);
    ASSERT_NE(overlay->info, nullptr);
    EXPECT_TRUE(overlay->info->clipping_counted);
    // +2 EV clips highlights somewhere; those pixels are red in the overlay.
    EXPECT_GT(overlay->info->clipped_highlights, 0u);
    bool red = false;
    for (int y = 0; y < overlay->clipping.height() && !red; ++y) {
        for (int x = 0; x < overlay->clipping.width() && !red; ++x) {
            const QRgb pixel = overlay->clipping.pixel(x, y);
            red = qAlpha(pixel) > 0 && qRed(pixel) > qBlue(pixel);
        }
    }
    EXPECT_TRUE(red);
}

TEST_F(AppEditServiceTest, AnEditedPhotosThumbnailIsLucentsRenderOfItsRecipe) {
    const ItemId id = import_file(L"thumb.dng");
    ASSERT_NE(library_->item(id), nullptr);
    EXPECT_TRUE(library_->item(id)->thumbnail_is_camera_preview);
    EXPECT_FALSE(library_->item(id)->edited);
    const Generation camera = library_->item(id)->thumbnail_generation;

    edits_->open(id);
    ASSERT_TRUE(QTest::qWaitFor([&] { return state_for(id).has_value(); }, kAdapterWaitMs));
    EditSettings settings = state_for(id)->settings;
    settings.exposure_ev = -1.0;
    const quint64 applied = edits_->apply(id, settings);
    ASSERT_TRUE(wait_state(id, applied));
    ASSERT_TRUE(QTest::qWaitFor([&] { return library_->item(id)->edited; }, kAdapterWaitMs));
    const LibraryItem edited = *library_->item(id);
    EXPECT_GT(edited.thumbnail_generation, camera);
    EXPECT_FALSE(edited.thumbnail_is_camera_preview);

    ASSERT_TRUE(wait_idle());
    auto thumbnail = request_image(*images_, ImageKind::Thumbnail, id, edited.thumbnail_generation, {512, 512});
    ASSERT_TRUE(wait_completed(*thumbnail));
    ASSERT_EQ(thumbnail->state(), ImageReply::State::Finished) << thumbnail->error().toStdString();
    EXPECT_EQ(thumbnail->image().size(), QSize(64, 48));

    // Back to as shot: no longer edited, and the thumbnail follows the recipe again.
    const quint64 reset = edits_->reset_to_as_shot(id);
    ASSERT_TRUE(wait_state(id, reset));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !library_->item(id)->edited; }, kAdapterWaitMs));
    EXPECT_GT(library_->item(id)->thumbnail_generation, edited.thumbnail_generation);
}

TEST_F(AppEditServiceTest, FramesCarryGeometryAndMasksAndTheCropToolSeesTheWholeImage) {
    const ItemId id = import_file(L"geometry.dng");
    edits_->open(id);
    ASSERT_TRUE(QTest::qWaitFor([&] { return state_for(id).has_value() && !frames_.isEmpty(); }, kAdapterWaitMs));
    ASSERT_TRUE(wait_idle());
    EXPECT_EQ(frames_.back()->full_upright_size, QSize(64, 48));
    EXPECT_EQ(frames_.back()->full_output_size, QSize(64, 48));

    EditSettings settings = state_for(id)->settings;
    settings.geometry.crop = EditCrop{0.25, 0.0, 0.75, 1.0};
    settings.geometry.quarter_turns = 1;
    EditMask radial;
    radial.id = 3;
    radial.kind = MaskKind::Radial;
    radial.center = QPointF(0.5, 0.5);
    radial.radius_x = 0.2;
    radial.radius_y = 0.2;
    radial.controls.exposure_ev = 1.0;
    settings.masks << radial;
    settings.chroma_denoise.strength = 0.5;
    const quint64 applied = edits_->apply(id, settings);
    ASSERT_TRUE(wait_final_frame(id, applied)) << command_failures_.join('\n').toStdString();
    ASSERT_TRUE(wait_idle());
    const EditState state = *state_for(id);
    EXPECT_EQ(state.committed.geometry, settings.geometry);
    ASSERT_EQ(state.committed.masks.size(), 1);
    EXPECT_EQ(state.committed.masks.front(), radial);
    EXPECT_DOUBLE_EQ(state.committed.chroma_denoise.strength, 0.5);
    ASSERT_FALSE(history_.isEmpty());
    EXPECT_TRUE(history_.back().parts.contains(EditPart::Geometry));
    EXPECT_TRUE(history_.back().parts.contains(EditPart::Masks));
    EXPECT_TRUE(history_.back().parts.contains(EditPart::ChromaDenoise));

    // The cropped, turned frame: 32 x 48 of the straightened image becomes 48 x 32.
    const auto final_frame = [&](quint64 token) {
        for (auto it = frames_.crbegin(); it != frames_.crend(); ++it) {
            if ((*it)->token == token && !(*it)->before && (*it)->phase == FramePhase::Final) {
                return *it;
            }
        }
        return EditFramePtr();
    };
    const EditFramePtr cropped = final_frame(applied);
    ASSERT_NE(cropped, nullptr);
    EXPECT_EQ(cropped->image.size(), QSize(48, 32));
    EXPECT_EQ(cropped->full_output_size, QSize(48, 32));
    EXPECT_EQ(cropped->geometry, settings.geometry);
    EXPECT_FALSE(cropped->uncropped);
    ASSERT_EQ(cropped->masks.size(), 1);
    EXPECT_EQ(cropped->masks.front().id, 3u);
    EXPECT_EQ(cropped->masks.front().kind, MaskKind::Radial);
    ASSERT_FALSE(cropped->masks.front().polylines.isEmpty());
    // The ellipse's points, as fractions of the frame, agree with the engine's mapping.
    const GeometryMap map = edits_->map_geometry(cropped->geometry, cropped->full_upright_size);
    ASSERT_TRUE(map.valid);
    const QPointF centre = map.normalized_to_output.map(QPointF(0.5, 0.5));
    double mean_x = 0.0;
    const QPolygonF& ellipse = cropped->masks.front().polylines.front();
    for (qsizetype i = 0; i + 1 < ellipse.size(); ++i) {
        mean_x += ellipse[i].x();
    }
    mean_x /= double(ellipse.size() - 1);
    EXPECT_NEAR(mean_x, centre.x() / map.output_size.width(), 1e-3);

    // The crop tool: the same settings rendered without the crop (turned, 48 x 64).
    edits_->set_crop_editing(id, true);
    ASSERT_TRUE(QTest::qWaitFor([&] { return frames_.back()->uncropped && frames_.back()->phase == FramePhase::Final; },
                                kAdapterWaitMs));
    EXPECT_EQ(frames_.back()->image.size(), QSize(48, 64));
    EXPECT_TRUE(frames_.back()->geometry.crop.is_full());
    EXPECT_EQ(frames_.back()->geometry.quarter_turns, 1);
    ASSERT_TRUE(wait_idle());
    EXPECT_EQ(state_for(id)->committed.geometry, settings.geometry);  // the recipe keeps its crop
    edits_->set_crop_editing(id, false);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !frames_.back()->uncropped && frames_.back()->image.size() == QSize(48, 32); },
                                kAdapterWaitMs));
    ASSERT_TRUE(wait_idle());

    // The mask overlay: the newest frame again, with the radial mask's weights (strongest inside).
    edits_->set_mask_overlay(id, 3);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !frames_.back()->mask_overlay.isNull(); }, kAdapterWaitMs));
    const EditFramePtr tinted = frames_.back();
    EXPECT_EQ(tinted->mask_overlay_id, 3u);
    EXPECT_EQ(tinted->mask_overlay.size(), tinted->image.size());
    const QPoint middle(qRound(centre.x()), qRound(centre.y()));
    EXPECT_GT(qAlpha(tinted->mask_overlay.pixel(middle)), 100);
    EXPECT_EQ(qAlpha(tinted->mask_overlay.pixel(0, 0)), 0);
    ASSERT_TRUE(wait_idle());
}

TEST_F(AppEditServiceTest, NoiseReductionMasksAndTheCropPasteByCategoryOnlyOntoTheSameShape) {
    const ItemId source = import_file(L"shape-a.dng");
    const ItemId same = import_file(L"shape-b.dng");
    const ItemId square = import_file(L"shape-c.dng", "Synthetic DNG", 64, 64);
    edits_->open(source);
    ASSERT_TRUE(QTest::qWaitFor([&] { return state_for(source).has_value(); }, kAdapterWaitMs));
    ASSERT_TRUE(wait_idle());
    EditSettings settings = state_for(source)->settings;
    settings.chroma_denoise = EditDenoise{0.6, 6.0};
    settings.geometry.crop = EditCrop{0.1, 0.1, 0.9, 0.9};
    EditMask linear;
    linear.id = 1;
    linear.kind = MaskKind::Linear;
    linear.feather = 1.0;
    linear.controls.exposure_ev = -0.5;
    settings.masks << linear;
    settings.highlights = -0.4;
    const quint64 applied = edits_->apply(source, settings);
    ASSERT_TRUE(wait_state(source, applied));
    ASSERT_TRUE(wait_idle());
    ASSERT_EQ(state_for(source, applied)->revision_number, 1u) << command_failures_.join('\n').toStdString();

    edits_->copy_settings(source, kAllCategoryBits);
    ASSERT_TRUE(QTest::qWaitFor([&] { return copies_ == 1; }, kAdapterWaitMs));
    edits_->close(source);

    // A photo of the same shape takes everything.
    edits_->open(same);
    ASSERT_TRUE(QTest::qWaitFor([&] { return state_for(same).has_value(); }, kAdapterWaitMs));
    edits_->paste_settings({same});
    ASSERT_TRUE(QTest::qWaitFor([&] { return pastes_.size() == 1; }, kAdapterWaitMs));
    EXPECT_TRUE(pastes_.back().skipped.isEmpty()) << pastes_.back().skipped.join('\n').toStdString();
    for (const SettingsCategory category :
         {SettingsCategory::Local, SettingsCategory::Masks, SettingsCategory::Detail, SettingsCategory::Geometry}) {
        EXPECT_TRUE(pastes_.back().applied.contains(category)) << int(category);
    }
    ASSERT_TRUE(wait_idle());
    const EditState pasted = *state_for(same);
    EXPECT_EQ(pasted.committed.chroma_denoise, settings.chroma_denoise);
    EXPECT_EQ(pasted.committed.geometry, settings.geometry);
    EXPECT_EQ(pasted.committed.masks, settings.masks);
    edits_->close(same);

    // A square photo: the masks and the crop would land elsewhere, so they stay off and the report
    // says why; highlights and the noise reduction are pasted.
    edits_->open(square);
    ASSERT_TRUE(QTest::qWaitFor([&] { return state_for(square).has_value(); }, kAdapterWaitMs));
    edits_->paste_settings({square});
    ASSERT_TRUE(QTest::qWaitFor([&] { return pastes_.size() == 2; }, kAdapterWaitMs));
    const PasteReport& report = pastes_.back();
    ASSERT_EQ(report.skipped.size(), 2) << report.skipped.join('\n').toStdString();
    EXPECT_TRUE(report.skipped.join(' ').contains(QStringLiteral("Masks: not pasted because this photo has another shape")))
        << report.skipped.join('\n').toStdString();
    EXPECT_TRUE(report.skipped.join(' ').contains(QStringLiteral("crop and rotation were not pasted")))
        << report.skipped.join('\n').toStdString();
    ASSERT_TRUE(wait_idle());
    const EditState other = *state_for(square);
    EXPECT_DOUBLE_EQ(other.committed.highlights, -0.4);
    EXPECT_EQ(other.committed.chroma_denoise, settings.chroma_denoise);
    EXPECT_TRUE(other.committed.masks.isEmpty());
    EXPECT_TRUE(other.committed.geometry.is_identity());
}

}  // namespace
}  // namespace lucent::ui
