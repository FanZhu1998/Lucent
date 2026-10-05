// Batches over the camera smoke fixtures with real lucent-worker processes (W7, ADR 0012). Skipped
// when fixtures/cache is absent (tools\fetch-fixtures.ps1). Holds the full-resolution render lock.
//
// Measures and prints: the batch's wall time, every job's render / encode / write / verification
// times from its receipt, the workers' peak commit, how many workers the memory budget admitted,
// and the per-pixel estimate the scheduler calibrated.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/application/batch_service.hpp>

#include "batch_test_support.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;
namespace rts = raw::test_support;
using namespace std::chrono_literals;

std::vector<std::filesystem::path> smoke_fixtures() {
    std::vector<std::filesystem::path> files;
    for (const char* id : {"canon-eos-r5-cr3-raw", "sony-ilce-7m4-arw-compressed", "dji-fc4382-dng",
                           "fujifilm-x-t4-raf-lossless", "nikon-z-6-nef-lossless"}) {
        if (auto file = rts::fixture_file(id)) {
            files.push_back(*file);
        }
    }
    return files;
}

BatchServiceOptions fixture_options() {
    BatchServiceOptions options = ts::fast_batch_options();
    options.job_timeout = 600s;
    options.heartbeat_interval = 250ms;
    options.heartbeat_timeout = 30s;
    return options;
}

void print_receipts(ts::BatchHarness& h, catalog::BatchId id) {
    for (const catalog::ReceiptRecord& receipt : h.receipts(id)) {
        auto details = read_receipt_details(receipt.settings_json);
        ASSERT_TRUE(details);
        std::printf("    %-45ls %5u x %-5u %6.1f MB  render %5.2f s  encode %5.2f s  write %5.2f s  verify %5.2f s  "
                    "total %5.2f s  worker peak commit %6.0f MB%s\n",
                    receipt.final_path.filename().c_str(), details->width, details->height,
                    static_cast<double>(receipt.size_bytes) / 1e6, details->render_seconds, details->encode_seconds,
                    details->write_seconds, details->verify_seconds, details->total_seconds,
                    static_cast<double>(details->worker_peak_commit_bytes) / 1048576.0,
                    details->review.empty() ? "" : "  (review)");
    }
}

TEST(BatchFixtures, SmokeFixturesExportThroughWorkers) {
    const std::vector<std::filesystem::path> files = smoke_fixtures();
    if (files.empty()) {
        GTEST_SKIP() << "fixtures are not downloaded; run tools\\fetch-fixtures.ps1";
    }
    struct Snapshot {
        core::Digest256 sha256{};
        core::FileIdentity identity;
    };
    std::vector<Snapshot> before;
    for (const auto& file : files) {
        before.push_back(Snapshot{ts::file_sha256(file), *core::stat_file(file)});
    }
    ts::ScratchDir dir;
    ts::BatchHarness h(dir.path() / L"lucent", fixture_options());
    const std::vector<ItemId> ids = h.import(files);
    ASSERT_EQ(ids.size(), files.size());
    const auto out = dir.path() / L"out";

    const auto start = std::chrono::steady_clock::now();
    const catalog::BatchId id = h.run(h.request(ids, out), 900s);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ASSERT_TRUE(id.valid());
    // Photos Lucent cannot render faithfully (the DJI DNG's mandatory WarpRectilinear opcode) are
    // skipped with their reason, never exported silently (blueprint sections 2 and 3).
    std::size_t unsupported = 0;
    for (const ItemId item : ids) {
        unsupported += h.library().find(item)->support.status == SupportStatus::Unsupported ? 1 : 0;
    }
    const catalog::BatchRecord record = h.record(id);
    EXPECT_EQ(record.state, catalog::BatchState::Completed);
    EXPECT_EQ(record.counts.complete, files.size() - unsupported);

    // Every output has its photo's full size and matches its receipt.
    for (const ItemId item : ids) {
        const auto source = h.library().find(item);
        ASSERT_TRUE(source && source->probe);
        if (source->support.status == SupportStatus::Unsupported) {
            continue;
        }
        bool found = false;
        for (const catalog::ReceiptRecord& receipt : h.receipts(id)) {
            if (receipt.final_path.stem().wstring().starts_with(source->path.stem().wstring())) {
                found = true;
                auto details = read_receipt_details(receipt.settings_json);
                ASSERT_TRUE(details);
                EXPECT_EQ(static_cast<std::int32_t>(details->width), source->probe->upright_size.width);
                EXPECT_EQ(static_cast<std::int32_t>(details->height), source->probe->upright_size.height);
                EXPECT_EQ(ts::file_sha256(receipt.final_path), receipt.output_sha256);
            }
        }
        EXPECT_TRUE(found) << source->path.string();
    }
    EXPECT_TRUE(ts::leftovers(out).empty());
    // Release gate "Original preservation": the originals' content and identity are unchanged.
    for (std::size_t i = 0; i < files.size(); ++i) {
        EXPECT_EQ(ts::file_sha256(files[i]), before[i].sha256) << files[i].string();
        EXPECT_EQ(*core::stat_file(files[i]), before[i].identity) << files[i].string();
    }

    const BatchSchedulerStats stats = h.batches().stats();
    std::printf("  %zu smoke fixtures (%zu unsupported), JPEG sRGB q95: %.1f s for the batch; worker budget %.2f GiB of %.2f GiB; "
                "at most %u workers; calibrated %.1f bytes/pixel; peak worker commit %.0f MB; %llu worker starts\n",
                files.size(), unsupported, seconds, static_cast<double>(stats.worker_budget_bytes) / (1 << 30),
                static_cast<double>(stats.total_budget_bytes) / (1 << 30), stats.peak_active_workers,
                stats.bytes_per_pixel, static_cast<double>(stats.peak_worker_commit_bytes) / 1048576.0,
                static_cast<unsigned long long>(stats.worker_starts));
    print_receipts(h, id);
    RecordProperty("batch_seconds_x10", static_cast<int>(seconds * 10));
    RecordProperty("peak_worker_commit_mb", static_cast<int>(stats.peak_worker_commit_bytes >> 20));
    RecordProperty("peak_active_workers", static_cast<int>(stats.peak_active_workers));
    auto report = h.batches().report(id);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->exported, files.size() - unsupported);
    EXPECT_EQ(report->skipped, unsupported);
    for (const ReportEntry& entry : report->entries) {
        if (entry.outcome != ReportOutcome::Exported) {
            EXPECT_FALSE(entry.reason.empty());
            std::printf("    not exported: %ls: %s (%s)\n", entry.source.filename().c_str(), entry.reason.c_str(),
                        entry.error ? entry.error->describe().c_str() : "");
        }
    }
}

// Automatic settings over the smoke fixtures (ADR 0013): every supported photo is analysed in its
// worker, checked at full resolution, frozen as an automatic revision and exported with it.
TEST(BatchFixtures, SmokeFixturesAutomaticBatch) {
    const std::vector<std::filesystem::path> files = smoke_fixtures();
    if (files.empty()) {
        GTEST_SKIP() << "fixtures are not downloaded; run tools\\fetch-fixtures.ps1";
    }
    ts::ScratchDir dir;
    ts::BatchHarness h(dir.path() / L"lucent", fixture_options());
    const std::vector<ItemId> ids = h.import(files);
    ASSERT_EQ(ids.size(), files.size());
    BatchRequest request = h.request(ids, dir.path() / L"out");
    request.automatic = AutomaticSettings::Auto;
    const auto start = std::chrono::steady_clock::now();
    const catalog::BatchId id = h.run(request, 1800s);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ASSERT_TRUE(id.valid());
    std::size_t unsupported = 0;
    for (const ItemId item : ids) {
        unsupported += h.library().find(item)->support.status == SupportStatus::Unsupported ? 1 : 0;
    }
    const catalog::BatchRecord record = h.record(id);
    EXPECT_EQ(record.state, catalog::BatchState::Completed);
    EXPECT_EQ(record.counts.complete, files.size() - unsupported);
    for (const catalog::JobRecord& job : h.jobs(id)) {
        ASSERT_EQ(job.stage, catalog::JobStage::Complete) << (job.last_error ? job.last_error->message : "");
        EXPECT_EQ(job.auto_state, catalog::AutoJobState::Frozen);
        auto receipt = h.catalog().query([&](catalog::ReadTransaction& tx) { return tx.find_receipt(job.id); });
        ASSERT_TRUE(receipt && *receipt);
        EXPECT_EQ((*receipt)->identity_sha256, job.identity_sha256);
        EXPECT_EQ(ts::file_sha256((*receipt)->final_path), (*receipt)->output_sha256);
    }
    const BatchSchedulerStats stats = h.batches().stats();
    std::printf("  %zu smoke fixtures, automatic settings, JPEG: %.1f s for the batch; peak worker commit %.0f MB; "
                "at most %u workers; %llu worker starts\n",
                files.size(), seconds, static_cast<double>(stats.peak_worker_commit_bytes) / 1048576.0,
                stats.peak_active_workers, static_cast<unsigned long long>(stats.worker_starts));
    auto report = h.batches().report(id);
    ASSERT_TRUE(report);
    for (const ReportEntry& entry : report->entries) {
        if (entry.outcome != ReportOutcome::Exported) {
            std::printf("    not exported: %ls: %s\n", entry.source.filename().c_str(), entry.reason.c_str());
            continue;
        }
        ASSERT_TRUE(entry.automatic);
        std::printf("    %-40ls revision %u, confidence %.2f, attempt %u of %u, %zu flag(s)%s\n",
                    entry.source.filename().c_str(), entry.automatic->revision_number, entry.automatic->confidence,
                    entry.automatic->attempt, entry.automatic->attempts, entry.automatic->flags.size(),
                    entry.review.empty() ? "" : " (review)");
    }
    print_receipts(h, id);
    RecordProperty("auto_batch_seconds_x10", static_cast<int>(seconds * 10));
    RecordProperty("auto_peak_worker_commit_mb", static_cast<int>(stats.peak_worker_commit_bytes >> 20));
}

// Related shots (the Nikon Z 6 pair, taken seconds apart) analysed only, kept consistent: the
// pre-pass groups them and each is regularised towards the pair's look (ADR 0011 phase 2, ADR 0013).
TEST(BatchFixtures, RelatedNikonPairAnalysedConsistently) {
    LUCENT_REQUIRE_FIXTURE(first, "nikon-z-6-nef-compressed");
    LUCENT_REQUIRE_FIXTURE(second, "nikon-z-6-nef-lossless");
    ts::ScratchDir dir;
    ts::BatchHarness h(dir.path() / L"lucent", fixture_options());
    const std::vector<ItemId> ids = h.import({first, second});
    ASSERT_EQ(ids.size(), 2u);
    BatchRequest request = h.request(ids, dir.path() / L"out");
    request.automatic = AutomaticSettings::AnalyzeOnly;
    request.keep_consistent = true;
    const catalog::BatchId id = h.run(request, 1800s);
    ASSERT_TRUE(id.valid());
    EXPECT_EQ(h.record(id).counts.complete, 2u);
    auto report = h.batches().report(id);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->analysed, 2u);
    for (const ReportEntry& entry : report->entries) {
        ASSERT_TRUE(entry.automatic);
        std::printf("    %-40ls revision %u, confidence %.2f, group %s\n", entry.source.filename().c_str(),
                    entry.automatic->revision_number, entry.automatic->confidence,
                    entry.automatic->group ? entry.automatic->group->c_str() : "(none)");
        EXPECT_TRUE(entry.automatic->group.has_value()) << "the pair is one related group";
    }
    EXPECT_TRUE(h.receipts(id).empty());
}

TEST(BatchFixtures, CanonR5As16BitTiff) {
    LUCENT_REQUIRE_FIXTURE(path, "canon-eos-r5-cr3-raw");
    ts::ScratchDir dir;
    ts::BatchHarness h(dir.path() / L"lucent", fixture_options());
    const std::vector<ItemId> ids = h.import({path});
    BatchRequest request = h.request(ids, dir.path() / L"out");
    request.output = exporter::ExportSettings::tiff16(exporter::OutputProfile::DisplayP3);
    const auto start = std::chrono::steady_clock::now();
    const catalog::BatchId id = h.run(request, 900s);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    EXPECT_EQ(h.record(id).counts.complete, 1u);
    const BatchSchedulerStats stats = h.batches().stats();
    std::printf("  Canon EOS R5, 16-bit Display P3 TIFF: %.1f s; peak worker commit %.0f MB\n", seconds,
                static_cast<double>(stats.peak_worker_commit_bytes) / 1048576.0);
    print_receipts(h, id);
    RecordProperty("tiff_peak_worker_commit_mb", static_cast<int>(stats.peak_worker_commit_bytes >> 20));
}

}  // namespace
}  // namespace lucent::app
