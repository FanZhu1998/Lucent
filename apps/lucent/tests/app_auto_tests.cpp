// W8: the editor's Auto through AppEditService on the real application services (AutoService, the
// recipe service over a catalog) with a dark synthetic DNG: a run reports its progress and its end,
// writes an automatic revision that the session shows (state, history, frame) with the summary of its
// analysis record, and undo takes it back; a failed full-size check keeps the failed attempt in the
// history and uses a conservative one; locked controls are kept and their effect reported; a run
// cancelled before it starts writes nothing; the library flags an automatic result that recommends a
// review.

#include <QCoreApplication>
#include <QFileInfo>
#include <QThread>
#include <chrono>
#include <optional>

#include <gtest/gtest.h>

#include "adapter_test_support.hpp"
#include "adapters/app_edit_service.hpp"

namespace lucent::ui {
namespace {

using adapters::AppEditService;
using adapters::AppLibraryService;
using testing::kAdapterWaitMs;
using testing::write_dng;
namespace rts = lucent::raw::test_support;

constexpr int kAutoWaitMs = 180000;

// A dark photo with a sensor noise model (application/tests/auto_service_test.cpp): Natural brightens
// it; 256 x 192 pixels meet every metric's minimum evidence.
rts::SyntheticDngSpec dark_spec() {
    rts::SyntheticDngSpec spec = testing::dng_with_preview();
    spec.width = 256;
    spec.height = 192;
    spec.white_level = 65535;
    spec.black_level = 256;
    spec.raw_noise_profile = {2e-5, 2e-7, 2e-5, 2e-7, 2e-5, 2e-7};
    return spec;
}

class AppAutoTest : public ::testing::Test {
protected:
    void open(std::uint32_t fail_final_checks = 0) {
        adapters::BackendOptions options = testing::test_backend_options();
        options.catalog_path = dir_.path() / L"lucent\\data\\lucent.catalog";
        options.batches = false;  // not needed here
        options.automatic.fail_final_checks = fail_final_checks;
        auto backend = adapters::Backend::create(options);
        ASSERT_TRUE(backend) << backend.error().describe();
        backend_ = *backend;
        ASSERT_NE(backend_->automatic(), nullptr);
        library_ = std::make_unique<AppLibraryService>(backend_);
        edits_ = std::make_unique<AppEditService>(backend_, library_->render_event_sink());
        QObject::connect(library_.get(), &LibraryService::items_added, library_.get(),
                         [this](const QList<LibraryItem>& items) { added_ << items; });
        QObject::connect(library_.get(), &LibraryService::item_changed, library_.get(),
                         [this](const LibraryItem& item) { items_[item.id] = item; });
        QObject::connect(library_.get(), &LibraryService::import_finished, library_.get(),
                         [this](const ImportSummary&) { ++imports_; });
        QObject::connect(edits_.get(), &EditService::state_changed, edits_.get(),
                         [this](const EditState& state) { states_ << state; });
        QObject::connect(edits_.get(), &EditService::frame_ready, edits_.get(),
                         [this](const EditFramePtr& frame) { frames_ << frame; });
        QObject::connect(edits_.get(), &EditService::history_changed, edits_.get(),
                         [this](ItemId, const QList<EditHistoryEntry>& entries) { history_ = entries; });
        QObject::connect(edits_.get(), &EditService::command_failed, edits_.get(),
                         [this](ItemId, const QString& message) { failures_ << message; });
        QObject::connect(edits_.get(), &EditService::auto_progress, edits_.get(), [this](const AutoProgress& p) {
            off_ui_thread_ |= QThread::currentThread() != QCoreApplication::instance()->thread();
            progress_ << p;
        });
        QObject::connect(edits_.get(), &EditService::auto_finished, edits_.get(), [this](const AutoOutcome& o) {
            off_ui_thread_ |= QThread::currentThread() != QCoreApplication::instance()->thread();
            outcomes_ << o;
        });
        QObject::connect(edits_.get(), &EditService::auto_summary_changed, edits_.get(),
                         [this](ItemId, const AutoSummary& summary) { summaries_ << summary; });
    }

    void TearDown() override {
        edits_.reset();
        library_.reset();
        backend_.reset();
    }

    ItemId import_dark(const std::wstring& name) {
        const std::filesystem::path path = write_dng(dir_, name, dark_spec());
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

    // Opens the photo's session and waits for its first state, frame and quiet.
    void open_session(ItemId id) {
        edits_->open(id);
        ASSERT_TRUE(QTest::qWaitFor([&] { return latest(id).has_value() && !frames_.isEmpty(); }, kAdapterWaitMs));
        ASSERT_TRUE(QTest::qWaitFor([&] { return !edits_->is_busy(); }, kAdapterWaitMs));
    }

    std::optional<EditState> latest(ItemId id) const {
        for (auto it = states_.crbegin(); it != states_.crend(); ++it) {
            if (it->item == id) {
                return *it;
            }
        }
        return std::nullopt;
    }

    // Runs Auto on `id` and returns its outcome.
    AutoOutcome run(ItemId id) {
        const qsizetype before = outcomes_.size();
        edits_->run_auto(id);
        EXPECT_TRUE(edits_->is_auto_running(id));
        EXPECT_TRUE(edits_->is_auto_running());
        EXPECT_TRUE(QTest::qWaitFor([&] { return outcomes_.size() > before; }, kAutoWaitMs)) << "Auto did not end";
        return outcomes_.size() > before ? outcomes_.back() : AutoOutcome{};
    }

    bool wait_summary(const std::function<bool(const AutoSummary&)>& matches) {
        return QTest::qWaitFor([&] { return !summaries_.isEmpty() && matches(summaries_.back()); }, kAdapterWaitMs);
    }

    bool wait_review_flag(ItemId id, bool review) {
        return QTest::qWaitFor(
            [&] {
                const auto found = items_.find(id);
                return found != items_.end() && has_flag(found->second.flags, ItemFlag::ReviewRecommended) == review;
            },
            kAdapterWaitMs);
    }

    rts::ScratchDir dir_;
    std::shared_ptr<adapters::Backend> backend_;
    std::unique_ptr<AppLibraryService> library_;
    std::unique_ptr<AppEditService> edits_;
    QList<LibraryItem> added_;
    std::map<ItemId, LibraryItem> items_;
    int imports_ = 0;
    QList<EditState> states_;
    QList<EditFramePtr> frames_;
    QList<EditHistoryEntry> history_;
    QStringList failures_;
    QList<AutoProgress> progress_;
    QList<AutoOutcome> outcomes_;
    QList<AutoSummary> summaries_;
    bool off_ui_thread_ = false;
};

TEST_F(AppAutoTest, AutoWritesAnAutomaticRevisionThatTheSessionShowsAndUndoTakesBack) {
    open();
    const ItemId id = import_dark(L"dark.dng");
    open_session(id);
    ASSERT_FALSE(summaries_.isEmpty());  // the history of an unedited photo: no automatic revision
    EXPECT_EQ(summaries_.back().revision, 0u);

    const AutoOutcome outcome = run(id);
    ASSERT_TRUE(outcome.saved) << outcome.message.toStdString();
    EXPECT_TRUE(outcome.made_current);
    EXPECT_FALSE(outcome.cancelled);
    EXPECT_EQ(outcome.revision_number, 1u);
    EXPECT_EQ(outcome.revisions, 1);
    EXPECT_TRUE(outcome.message.startsWith(QStringLiteral("Auto saved its settings as revision 1.")))
        << outcome.message.toStdString();
    EXPECT_FALSE(edits_->is_auto_running());
    // Progress: queued, then the analysis and the full-size check, never backwards.
    bool analysing = false;
    bool checking = false;
    for (qsizetype i = 0; i < progress_.size(); ++i) {
        EXPECT_EQ(progress_[i].item, id);
        analysing = analysing || progress_[i].stage == AutoStage::Analysing;
        checking = checking || progress_[i].stage == AutoStage::Checking;
        if (i > 0) {
            EXPECT_GE(progress_[i].fraction, progress_[i - 1].fraction);
        }
    }
    EXPECT_EQ(progress_.front().stage, AutoStage::Queued);
    EXPECT_TRUE(analysing);
    EXPECT_TRUE(checking);
    EXPECT_FALSE(off_ui_thread_);

    // The session shows the automatic revision: its state, history, summary and frame.
    ASSERT_TRUE(QTest::qWaitFor([&] { return latest(id) && latest(id)->revision_number == 1; }, kAdapterWaitMs));
    const EditState state = *latest(id);
    EXPECT_GT(state.committed.exposure_ev, 0.0) << "a dark photo is brightened";
    EXPECT_TRUE(state.can_undo);
    ASSERT_TRUE(QTest::qWaitFor([&] { return history_.size() == 1; }, kAdapterWaitMs));
    EXPECT_EQ(history_[0].origin, QStringLiteral("auto"));
    EXPECT_EQ(history_[0].auto_kind, QStringLiteral("chosen"));
    EXPECT_FALSE(history_[0].auto_failed);
    EXPECT_TRUE(history_[0].current);
    EXPECT_TRUE(history_[0].changed.contains(EditControl::Exposure));
    ASSERT_TRUE(wait_summary([](const AutoSummary& s) { return s.revision != 0; }));
    const AutoSummary summary = summaries_.back();
    EXPECT_EQ(summary.revision, state.revision);
    EXPECT_EQ(summary.revision_number, 1u);
    EXPECT_FALSE(summary.from_batch);
    EXPECT_GT(summary.confidence, 0.0);
    EXPECT_LE(summary.confidence, 1.0);
    EXPECT_EQ(summary.attempt, 0u);
    EXPECT_TRUE(summary.changed.contains(EditControl::Exposure));
    EXPECT_TRUE(summary.locked.isEmpty());
    EXPECT_EQ(summary.review, !summary.review_reasons.isEmpty());
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            return std::any_of(frames_.cbegin(), frames_.cend(), [&](const EditFramePtr& frame) {
                return frame->revision == state.revision && frame->phase == FramePhase::Final && !frame->before;
            });
        },
        kAdapterWaitMs));
    // The library: the photo is edited, and flagged for review when the record says so.
    ASSERT_TRUE(QTest::qWaitFor([&] { return items_.contains(id) && items_[id].edited; }, kAdapterWaitMs));
    EXPECT_TRUE(wait_review_flag(id, summary.review));

    // Undo returns to the recipe before Auto: as shot, without a summary.
    const quint64 undone = edits_->undo(id);
    ASSERT_TRUE(QTest::qWaitFor([&] { return latest(id) && latest(id)->token == undone; }, kAdapterWaitMs));
    EXPECT_EQ(latest(id)->revision, 0u);
    EXPECT_TRUE(latest(id)->can_redo);
    ASSERT_TRUE(wait_summary([](const AutoSummary& s) { return s.revision == 0; }));
    EXPECT_TRUE(wait_review_flag(id, false));
    EXPECT_TRUE(failures_.isEmpty()) << failures_.join('\n').toStdString();
}

TEST_F(AppAutoTest, AFailedFullSizeCheckKeepsTheAttemptAndUsesAConservativeOne) {
    open(/*fail_final_checks=*/1);
    const ItemId id = import_dark(L"fallback.dng");
    open_session(id);
    const AutoOutcome outcome = run(id);
    ASSERT_TRUE(outcome.saved) << outcome.message.toStdString();
    EXPECT_EQ(outcome.revisions, 2);
    EXPECT_EQ(outcome.revision_number, 2u);
    EXPECT_TRUE(outcome.review);
    EXPECT_TRUE(outcome.message.contains(QStringLiteral("more conservative"))) << outcome.message.toStdString();

    // Both attempts are in the history; the failed one is named so, the conservative one is current.
    ASSERT_TRUE(QTest::qWaitFor([&] { return history_.size() == 2; }, kAdapterWaitMs));
    EXPECT_TRUE(history_[0].auto_failed);
    EXPECT_FALSE(history_[0].current);
    EXPECT_FALSE(history_[1].auto_failed);
    EXPECT_TRUE(history_[1].current);
    EXPECT_TRUE(history_[1].auto_kind == u"fallback" || history_[1].auto_kind == u"baseline")
        << history_[1].auto_kind.toStdString();
    EXPECT_EQ(history_[0].parent_number, history_[1].parent_number);  // siblings: both from as shot
    ASSERT_TRUE(wait_summary([](const AutoSummary& s) { return s.revision != 0; }));
    const AutoSummary summary = summaries_.back();
    EXPECT_EQ(summary.attempt, 1u);
    EXPECT_TRUE(summary.review);
    EXPECT_TRUE(summary.fallback_detail.contains(QStringLiteral("failed the full-resolution check")))
        << summary.fallback_detail.toStdString();
    EXPECT_TRUE(summary.review_reasons.contains(summary.baseline
                                                    ? QStringLiteral("Every automatic edit failed Lucent's full-size "
                                                                     "quality check, so Auto kept the camera's "
                                                                     "rendering for the controls it sets.")
                                                    : QStringLiteral("The first automatic edit failed a quality check, "
                                                                     "so a more conservative one was used.")));
    EXPECT_TRUE(wait_review_flag(id, true));
}

TEST_F(AppAutoTest, LockedControlsAreKeptAndWhatTheyPreventedIsReported) {
    open();
    const ItemId id = import_dark(L"locked.dng");
    open_session(id);
    EditSettings settings = latest(id)->settings;
    settings.exposure_ev = -1.0;
    settings.set_locked(EditControl::Exposure, true);
    const quint64 applied = edits_->apply(id, settings);
    ASSERT_TRUE(QTest::qWaitFor([&] { return latest(id) && latest(id)->token == applied; }, kAdapterWaitMs));
    ASSERT_EQ(latest(id)->revision_number, 1u);

    const AutoOutcome outcome = run(id);
    ASSERT_TRUE(outcome.saved) << outcome.message.toStdString();
    ASSERT_TRUE(QTest::qWaitFor([&] { return latest(id) && latest(id)->revision_number == 2; }, kAdapterWaitMs));
    EXPECT_DOUBLE_EQ(latest(id)->committed.exposure_ev, -1.0);
    EXPECT_TRUE(latest(id)->committed.locked(EditControl::Exposure));
    ASSERT_TRUE(wait_summary([](const AutoSummary& s) { return s.revision != 0; }));
    const AutoSummary summary = summaries_.back();
    EXPECT_EQ(summary.locked, QList<EditControl>{EditControl::Exposure});
    EXPECT_TRUE(summary.locks_prevented_targets);
    ASSERT_FALSE(summary.lock_notes.isEmpty());
    EXPECT_TRUE(summary.lock_notes.front().startsWith(QStringLiteral("Exposure is locked")))
        << summary.lock_notes.front().toStdString();
    EXPECT_TRUE(summary.review);
    EXPECT_FALSE(summary.changed.contains(EditControl::Exposure));

    // Undo returns to the locked edit Auto started from.
    const quint64 undone = edits_->undo(id);
    ASSERT_TRUE(QTest::qWaitFor([&] { return latest(id) && latest(id)->token == undone; }, kAdapterWaitMs));
    EXPECT_EQ(latest(id)->revision_number, 1u);
}

TEST_F(AppAutoTest, ARunCancelledBeforeItStartsWritesNothing) {
    open();
    const ItemId id = import_dark(L"cancel.dng");
    open_session(id);
    const qsizetype before = outcomes_.size();
    edits_->run_auto(id);
    edits_->run_auto(id);  // one run per photo: ignored
    edits_->cancel_auto(id);
    ASSERT_TRUE(QTest::qWaitFor([&] { return outcomes_.size() > before; }, kAutoWaitMs));
    QTest::qWait(100);
    ASSERT_EQ(outcomes_.size(), before + 1);
    const AutoOutcome outcome = outcomes_.back();
    EXPECT_TRUE(outcome.cancelled);
    EXPECT_FALSE(outcome.saved);
    EXPECT_EQ(outcome.message, QStringLiteral("Auto was cancelled; nothing was changed."));
    EXPECT_FALSE(edits_->is_auto_running());
    auto state = backend_->recipes().state(app::ItemId{id});
    ASSERT_TRUE(state);
    EXPECT_FALSE(state->revision.has_value());
}

}  // namespace
}  // namespace lucent::ui
