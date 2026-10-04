// The batch adapter over the real durable BatchService (W7, ADR 0012): plans with example paths
// (only the newest request is answered), a batch run by lucent-worker.exe processes whose many
// progress events reach the UI thread coalesced while the UI thread stays responsive, the report
// and its HTML file, a destination attention (disk full) shown with its text until Resume, the
// list after a restart, and a session without a catalog. Real services, synthetic DNGs.

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTimer>
#include <atomic>
#include <filesystem>
#include <memory>

#include <gtest/gtest.h>

#include "adapter_test_support.hpp"
#include "adapters/app_batch_service.hpp"
#include "adapters/batch_conversions.hpp"
#include "adapters/conversions.hpp"

namespace lucent::ui {
namespace {

using adapters::AppBatchService;
using adapters::AppLibraryService;
using testing::kAdapterWaitMs;
using testing::write_dng;
namespace rts = lucent::raw::test_support;

constexpr int kBatchWaitMs = 120000;

class BatchAdapterTest : public ::testing::Test {
protected:
    void TearDown() override { close(); }

    void open(bool with_catalog = true) {
        adapters::BackendOptions options = testing::test_backend_options();
        if (with_catalog) {
            options.catalog_path = dir_.path() / L"lucent\\data\\lucent.catalog";
        }
        options.batch.worker_executable = std::filesystem::path(LUCENT_WORKER_EXE).make_preferred();
        options.batch.test.free_space = [this](const std::filesystem::path&) -> std::optional<std::uint64_t> {
            if (full_after_first_.load() && free_calls_.fetch_add(1) > 0) {
                return std::uint64_t{1} << 20;  // 1 MiB: below what any output needs
            }
            return std::uint64_t{1} << 40;
        };
        auto services = adapters::create_application_services(options);
        ASSERT_TRUE(services) << services.error().describe();
        services_ = std::move(*services);
        library_ = dynamic_cast<AppLibraryService*>(services_.library.get());
        batches_ = dynamic_cast<AppBatchService*>(services_.batches.get());
        ASSERT_NE(library_, nullptr);
        ASSERT_NE(batches_, nullptr);
        QObject::connect(library_, &LibraryService::items_added, library_,
                         [this](const QList<LibraryItem>& items) { added_ << items; });
        QObject::connect(library_, &LibraryService::import_finished, library_,
                         [this](const ImportSummary& summary) { summaries_ << summary; });
        QObject::connect(batches_, &BatchService::plan_ready, batches_,
                         [this](quint64 token, const BatchPlanInfo& plan) { plans_.push_back({token, plan}); });
        QObject::connect(batches_, &BatchService::submitted, batches_,
                         [this](quint64 token, BatchId batch, const QString& error) {
                             submitted_.push_back({token, batch, error});
                         });
        QObject::connect(batches_, &BatchService::batch_changed, batches_, [this](const BatchSnapshot& batch) {
            ++changes_[batch.id];
            last_[batch.id] = batch;
        });
        QObject::connect(batches_, &BatchService::batches_listed, batches_,
                         [this](const QList<BatchSnapshot>& batches) { listed_.push_back(batches); });
        QObject::connect(batches_, &BatchService::report_ready, batches_,
                         [this](BatchId, const BatchReportInfo& report) { reports_.push_back(report); });
    }

    void close() {
        services_ = Services{};
        library_ = nullptr;
        batches_ = nullptr;
        listed_.clear();
    }

    // `count` different synthetic DNGs in "in" (64 x 48 and wider), imported.
    QList<ItemId> import_photos(int count) {
        for (int i = 0; i < count; ++i) {
            rts::SyntheticDngSpec spec;
            spec.width = 64 + 2 * static_cast<std::uint32_t>(i);
            write_dng(dir_, std::format(L"in\\IMG_{:04}.dng", i), spec);
        }
        const qsizetype before = summaries_.size();
        library_->start_import({path(L"in")}, ImportOptions{});
        EXPECT_TRUE(QTest::qWaitFor([&] { return summaries_.size() > before; }, kAdapterWaitMs));
        QList<ItemId> ids;
        for (const LibraryItem& item : added_) {
            ids << item.id;
        }
        return ids;
    }

    QString path(const std::wstring& relative) const {
        return QDir::toNativeSeparators(QString::fromStdWString((dir_.path() / relative).wstring()));
    }

    BatchSetup setup_for(const QList<ItemId>& items) const {
        BatchSetup setup;
        setup.items = items;
        setup.output_folder = path(L"out");
        return setup;
    }

    // Submits `setup` and returns its batch (0 when it was not created).
    BatchId submit(const BatchSetup& setup) {
        const quint64 token = batches_->submit(setup);
        EXPECT_TRUE(QTest::qWaitFor(
            [&] {
                return std::any_of(submitted_.begin(), submitted_.end(),
                                   [token](const Submitted& s) { return s.token == token; });
            },
            kBatchWaitMs));
        for (const Submitted& s : submitted_) {
            if (s.token == token) {
                EXPECT_TRUE(s.error.isEmpty()) << s.error.toStdString();
                return s.batch;
            }
        }
        return 0;
    }

    // Waits until `batch` finished and its final status (review count, output) was read.
    bool wait_finished(BatchId batch) {
        return QTest::qWaitFor(
            [&] {
                const BatchSnapshot* snapshot = batches_->snapshot(batch);
                return snapshot != nullptr && snapshot->finished_state() && snapshot->review >= 0 &&
                       !batches_->is_busy();
            },
            kBatchWaitMs);
    }

    struct Submitted {
        quint64 token = 0;
        BatchId batch = 0;
        QString error;
    };

    rts::ScratchDir dir_;
    // The destination's free space as the service sees it: plenty, or after the first query 1 MiB.
    std::atomic<bool> full_after_first_{false};
    std::atomic<int> free_calls_{0};
    Services services_;
    AppLibraryService* library_ = nullptr;
    AppBatchService* batches_ = nullptr;
    QList<LibraryItem> added_;
    QList<ImportSummary> summaries_;
    std::vector<std::pair<quint64, BatchPlanInfo>> plans_;
    std::vector<Submitted> submitted_;
    std::map<BatchId, int> changes_;
    std::map<BatchId, BatchSnapshot> last_;
    std::vector<QList<BatchSnapshot>> listed_;
    std::vector<BatchReportInfo> reports_;
};

TEST_F(BatchAdapterTest, PlansShowExamplePathsAndOnlyTheNewestRequestIsAnswered) {
    open();
    const QList<ItemId> items = import_photos(4);
    ASSERT_EQ(items.size(), 4);
    BatchSetup setup = setup_for(items);
    batches_->request_plan(setup);
    setup.name_pattern = QStringLiteral("{sequence}_{original-stem}");
    const quint64 newest = batches_->request_plan(setup);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !plans_.empty(); }, kAdapterWaitMs));
    QTest::qWait(200);
    ASSERT_EQ(plans_.size(), 1u);
    EXPECT_EQ(plans_[0].first, newest);
    const BatchPlanInfo& plan = plans_[0].second;
    ASSERT_TRUE(plan.ok) << plan.error.toStdString();
    EXPECT_EQ(plan.outputs, 4);
    EXPECT_FALSE(plan.folder_exists);  // created when the batch starts
    ASSERT_EQ(plan.examples.size(), 3);
    EXPECT_EQ(QFileInfo(plan.examples[0].destination).fileName(), QStringLiteral("0001_IMG_0000.jpg"));
    EXPECT_EQ(QFileInfo(plan.examples[0].destination).absolutePath(), QDir::fromNativeSeparators(path(L"out")));
    EXPECT_GT(plan.estimated_bytes, 0);
    EXPECT_FALSE(std::filesystem::exists(dir_.path() / L"out"));  // planning writes nothing

    // An invalid pattern is explained in the user's terms.
    setup.name_pattern = QStringLiteral("export");
    const quint64 invalid = batches_->request_plan(setup);
    ASSERT_TRUE(QTest::qWaitFor([&] { return plans_.back().first == invalid; }, kAdapterWaitMs));
    EXPECT_FALSE(plans_.back().second.ok);
    EXPECT_TRUE(plans_.back().second.error.contains(QStringLiteral("{original-stem} or {sequence}")));

    // A folder whose photos are not in the library yet.
    write_dng(dir_, L"new\\X.dng");
    BatchSetup folder = setup_for({});
    folder.input_folder = path(L"new");
    const quint64 folder_plan = batches_->request_plan(folder);
    ASSERT_TRUE(QTest::qWaitFor([&] { return plans_.back().first == folder_plan; }, kAdapterWaitMs));
    EXPECT_TRUE(plans_.back().second.folder_not_listed);
    EXPECT_TRUE(plans_.back().second.error.isEmpty());
}

TEST_F(BatchAdapterTest, ABatchRunsWithCoalescedUpdatesAndAResponsiveUiThreadThenReports) {
    open();
    constexpr int kPhotos = 24;
    const QList<ItemId> items = import_photos(kPhotos);
    ASSERT_EQ(items.size(), kPhotos);

    // The UI thread's responsiveness while the batch runs: gaps between ticks of a 5 ms timer.
    QElapsedTimer clock;
    clock.start();
    qint64 last_tick = 0;
    qint64 largest_gap = 0;
    QTimer probe;
    probe.setInterval(5);
    QObject::connect(&probe, &QTimer::timeout, [&] {
        const qint64 now = clock.elapsed();
        largest_gap = std::max(largest_gap, now - last_tick);
        last_tick = now;
    });
    probe.start();

    const int flushes_before = batches_->flushes();
    const BatchId batch = submit(setup_for(items));
    ASSERT_NE(batch, 0u);
    QElapsedTimer run;
    run.start();
    ASSERT_TRUE(wait_finished(batch));
    const qint64 elapsed = run.elapsed();
    probe.stop();

    const BatchSnapshot& done = *batches_->snapshot(batch);
    EXPECT_EQ(done.state, BatchState::Completed);
    EXPECT_EQ(done.completed, kPhotos);
    EXPECT_EQ(done.remaining(), 0);
    EXPECT_EQ(done.failed, 0);
    EXPECT_GE(done.review, 0);
    EXPECT_EQ(done.excluded, 0);
    EXPECT_TRUE(done.running_jobs.isEmpty());
    EXPECT_LT(done.eta_seconds, 0.0);
    EXPECT_FALSE(done.last_output.isEmpty());
    EXPECT_TRUE(QFileInfo(done.report_path).isFile()) << done.report_path.toStdString();
    EXPECT_FALSE(batches_->is_busy());
    // Coalesced: the worker reports progress about every 1 % of each photo, yet the UI thread applies
    // events at most once per 200 ms, and emits about one snapshot per application.
    const int flushes = batches_->flushes() - flushes_before;
    EXPECT_LE(flushes, elapsed / 200 + 2) << "elapsed " << elapsed << " ms";
    EXPECT_LE(changes_[batch], 2 * (elapsed / 200 + 2) + 10) << "elapsed " << elapsed << " ms";
    EXPECT_LT(largest_gap, 1000) << "the UI thread was blocked";

    for (int i = 0; i < kPhotos; ++i) {
        EXPECT_TRUE(std::filesystem::is_regular_file(dir_.path() / std::format(L"out\\IMG_{:04}_Lucent_0.jpg", i)));
    }

    batches_->request_report(batch);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !reports_.empty(); }, kAdapterWaitMs));
    const BatchReportInfo& report = reports_.back();
    ASSERT_TRUE(report.ok) << report.error.toStdString();
    EXPECT_EQ(report.exported, kPhotos);
    EXPECT_EQ(report.entries.size(), kPhotos);
    EXPECT_EQ(report.entries[0].outcome, BatchOutcome::Exported);
    EXPECT_EQ(QFileInfo(report.entries[0].output).fileName(), QStringLiteral("IMG_0000_Lucent_0.jpg"));
    EXPECT_EQ(report.format, QStringLiteral("JPEG · sRGB · 8-bit"));

    // Exporting the same photos again with the default policy skips them as already exported.
    const BatchId again = submit(setup_for(items.mid(0, 2)));
    ASSERT_TRUE(wait_finished(again));
    EXPECT_EQ(batches_->snapshot(again)->skipped, 2);
    EXPECT_EQ(batches_->snapshot(again)->completed, 0);
}

TEST_F(BatchAdapterTest, AFullDestinationPausesTheBatchWithItsTextUntilResume) {
    open();
    const QList<ItemId> items = import_photos(3);
    ASSERT_EQ(items.size(), 3);
    // Enough space when the batch is created (the first query); full when its first photo would start.
    free_calls_ = 0;
    full_after_first_ = true;
    const BatchId batch = submit(setup_for(items));
    ASSERT_NE(batch, 0u);
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            const BatchSnapshot* s = batches_->snapshot(batch);
            return s != nullptr && s->attention == BatchAttention::DestinationFull;
        },
        kBatchWaitMs));
    const BatchSnapshot paused = *batches_->snapshot(batch);
    EXPECT_EQ(paused.state, BatchState::Paused);
    EXPECT_EQ(paused.completed, 0);
    EXPECT_EQ(paused.remaining(), 3);
    EXPECT_TRUE(paused.attention_message.contains(QStringLiteral("is full")));
    EXPECT_TRUE(paused.attention_message.contains(QStringLiteral("Resume")));
    EXPECT_TRUE(paused.attention_message.contains(path(L"out")));
    EXPECT_FALSE(batches_->is_busy());  // paused: nothing runs

    full_after_first_ = false;  // space was freed
    batches_->resume(batch);
    ASSERT_TRUE(wait_finished(batch));
    const BatchSnapshot& done = *batches_->snapshot(batch);
    EXPECT_EQ(done.completed, 3);
    EXPECT_EQ(done.attention, BatchAttention::None);
    EXPECT_TRUE(done.attention_message.isEmpty());
}

TEST_F(BatchAdapterTest, TheListComesBackAfterARestartWithSingleExportsAsBatchesOfOne) {
    open();
    const QList<ItemId> items = import_photos(2);
    ASSERT_EQ(items.size(), 2);
    const BatchId batch = submit(setup_for(items));
    ASSERT_TRUE(wait_finished(batch));
    // A single export through the export service is a batch of one.
    std::filesystem::create_directories(dir_.path() / L"single");
    services_.exports->start_export(ExportRequest{{items[0]}, path(L"single"), ExportOptions{}});
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            for (const auto& [id, snapshot] : last_) {
                if (snapshot.single() && snapshot.finished_state()) {
                    return true;
                }
            }
            return false;
        },
        kBatchWaitMs));
    close();

    open();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !listed_.empty(); }, kAdapterWaitMs));
    const QList<BatchSnapshot>& listed = listed_.back();
    ASSERT_EQ(listed.size(), 2);
    EXPECT_TRUE(listed[0].single());  // newest first
    EXPECT_EQ(listed[0].completed, 1);
    EXPECT_EQ(listed[1].id, batch);
    EXPECT_EQ(listed[1].completed, 2);
    EXPECT_EQ(listed[1].state, BatchState::Completed);
    EXPECT_EQ(listed[1].folder, path(L"out"));
    // Review counts (and the single export's file) are read from the reports in the background.
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            const BatchSnapshot* single = batches_->snapshot(listed[0].id);
            return single != nullptr && single->review >= 0 && !single->last_output.isEmpty();
        },
        kAdapterWaitMs));
    EXPECT_EQ(QFileInfo(batches_->snapshot(listed[0].id)->last_output).fileName(),
              QStringLiteral("IMG_0000_Lucent.jpg"));
}

TEST_F(BatchAdapterTest, WithoutACatalogBatchesAreUnavailableAndSaySo) {
    open(/*with_catalog=*/false);
    EXPECT_FALSE(batches_->is_available());
    EXPECT_FALSE(batches_->unavailable_reason().isEmpty());
    const quint64 plan = batches_->request_plan(setup_for({1}));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !plans_.empty(); }, kAdapterWaitMs));
    EXPECT_EQ(plans_[0].first, plan);
    EXPECT_FALSE(plans_[0].second.ok);
    EXPECT_EQ(plans_[0].second.error, batches_->unavailable_reason());
    const quint64 token = batches_->submit(setup_for({1}));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !submitted_.empty(); }, kAdapterWaitMs));
    EXPECT_EQ(submitted_[0].token, token);
    EXPECT_EQ(submitted_[0].batch, 0u);
    EXPECT_FALSE(submitted_[0].error.isEmpty());
}

// --- Conversions ---------------------------------------------------------------------------------

TEST(BatchConversionsTest, TheSetupBecomesTheRequestWithExplicitMetadataChoices) {
    BatchSetup setup;
    setup.items = {4, 5};
    setup.output_folder = QStringLiteral("D:/Out");
    setup.output.format = ExportFileFormat::Tiff16;
    setup.output.color_space = ExportColorSpace::DisplayP3;
    setup.name_pattern = QStringLiteral("{sequence}");
    setup.mirror_subfolders = true;
    setup.collisions = BatchCollisions::Overwrite;
    setup.existing = BatchExisting::ExportAgain;
    setup.include_capture = false;
    setup.artist = QStringLiteral("  Ada  ");
    app::BatchRequest request = adapters::to_batch_request(setup);
    ASSERT_EQ(request.items.size(), 2u);
    EXPECT_EQ(request.items[1].item.value, 5u);
    EXPECT_FALSE(request.items[0].recipe.has_value());  // frozen by the service at submission
    EXPECT_FALSE(request.folder.has_value());
    EXPECT_EQ(request.destination.folder, std::filesystem::path(L"D:/Out"));
    EXPECT_EQ(request.destination.name_pattern, "{sequence}");
    EXPECT_TRUE(request.destination.mirror_subfolders);
    EXPECT_EQ(request.destination.collisions, app::CollisionPolicy::Overwrite);
    EXPECT_EQ(request.existing_outputs, app::ExistingOutputs::ExportAgain);
    EXPECT_EQ(request.output.format, exporter::ExportFormat::Tiff);
    EXPECT_EQ(request.output.profile, exporter::OutputProfile::DisplayP3);
    EXPECT_FALSE(request.output.metadata.include_capture);
    EXPECT_TRUE(request.output.metadata.include_copyright);
    EXPECT_FALSE(request.output.metadata.include_gps);  // never unless chosen
    EXPECT_EQ(request.artist, std::optional<std::string>("Ada"));
    EXPECT_FALSE(request.copyright.has_value());  // empty: the camera's own value
    EXPECT_FALSE(request.allow_unsupported);
    EXPECT_TRUE(request.write_report_files);

    setup.include_gps = true;
    setup.include_rights = false;
    setup.input_folder = QStringLiteral("D:/Shoot");
    request = adapters::to_batch_request(setup);
    EXPECT_TRUE(request.output.metadata.include_gps);
    EXPECT_FALSE(request.output.metadata.include_copyright);
    EXPECT_FALSE(request.artist.has_value());
    ASSERT_TRUE(request.folder.has_value());
    EXPECT_EQ(request.folder->path, std::filesystem::path(L"D:/Shoot"));
}

TEST(BatchConversionsTest, AttentionsAndExclusionsAreWordedForThePhotographer) {
    using adapters::describe_attention;
    EXPECT_TRUE(describe_attention(BatchAttention::None, QStringLiteral("D:/Out")).isEmpty());
    EXPECT_EQ(describe_attention(BatchAttention::DestinationFull, QStringLiteral("D:/Out")),
              QStringLiteral(
                  "The drive of D:\\Out is full, so the batch is paused. Free some space on it, then choose "
                  "Resume."));
    EXPECT_TRUE(describe_attention(BatchAttention::DestinationUnavailable, QStringLiteral("E:\\Card"))
                    .startsWith(QStringLiteral("E:\\Card is not available")));
    EXPECT_TRUE(describe_attention(BatchAttention::DestinationPermission, QStringLiteral("D:\\Out"))
                    .contains(QStringLiteral("permissions")));

    EXPECT_TRUE(adapters::describe_exclusion(Error{ErrorCode::Unsupported, "x", "app.batch.support", 0})
                    .startsWith(QStringLiteral("Unsupported")));
    EXPECT_TRUE(adapters::describe_exclusion(Error{ErrorCode::NotFound, "x", "app.batch.source.missing", 0})
                    .contains(QStringLiteral("relink")));
    EXPECT_TRUE(adapters::describe_exclusion(Error{ErrorCode::SourceChanged, "x", "app.batch.source.changed", 0})
                    .contains(QStringLiteral("again")));

    const BatchPlanInfo empty_folder =
        adapters::to_plan_failure(Error{ErrorCode::InvalidArgument, "x", "app.batch.empty", 0}, true, {});
    EXPECT_TRUE(empty_folder.folder_not_listed);
    EXPECT_TRUE(empty_folder.error.isEmpty());
    const BatchPlanInfo empty_selection =
        adapters::to_plan_failure(Error{ErrorCode::InvalidArgument, "x", "app.batch.empty", 0}, false, {});
    EXPECT_EQ(empty_selection.error, QStringLiteral("Nothing in the selection can be exported."));
    EXPECT_TRUE(adapters::describe_pattern_problem(QStringLiteral("{original-stem}_{x}"))
                    .contains(QStringLiteral("{x}")));
    EXPECT_TRUE(adapters::describe_pattern_problem(QStringLiteral("{original-stem}_Lucent_{revision}")).isEmpty());
}

}  // namespace
}  // namespace lucent::ui
