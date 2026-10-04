// Timing of large batches (reported, with generous bounds so slow machines do not fail).
//
// The measured times are printed as "[catalog timing] ..." lines and recorded as test properties.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "catalog_test_support.hpp"

namespace lucent::catalog {
namespace {

using namespace test;
using Clock = std::chrono::steady_clock;

double milliseconds_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void report(const std::string& name, double ms) {
    std::printf("[catalog timing] %s: %.1f ms\n", name.c_str(), ms);
    ::testing::Test::RecordProperty(name, std::format("{:.1f} ms", ms));
}

// 10,000 sources with one item each, as an import commits them: one transaction, then the same
// amount in ten transactions of 1,000, then reading everything back in pages.
TEST(CatalogPerformance, TenThousandRowsInOneTransaction) {
    TempDir dir;
    std::unique_ptr<Catalog> catalog = open_or_fail(dir / L"lucent.catalog");
    ASSERT_NE(catalog, nullptr);

    constexpr std::uint64_t kRows = 10'000;
    Clock::time_point start = Clock::now();
    LUCENT_ASSERT_OK(populate(*catalog, dir / L"one", 1, kRows));
    const double one_transaction = milliseconds_since(start);
    report("10000 sources + 10000 items, 1 transaction", one_transaction);

    start = Clock::now();
    for (std::uint64_t batch = 0; batch < 10; ++batch) {
        LUCENT_ASSERT_OK(populate(*catalog, dir / L"ten", 100'000 + batch * 1000, 1000));
    }
    report("10000 sources + 10000 items, 10 transactions of 1000", milliseconds_since(start));

    for (const auto& [order, name] :
         {std::pair{ItemOrder::Path, "path"}, std::pair{ItemOrder::CaptureTime, "capture time"},
          std::pair{ItemOrder::Added, "insertion"}}) {
        start = Clock::now();
        std::uint64_t listed = 0;
        ItemQuery query;
        query.limit = 500;
        query.order = order;
        for (;;) {
            Expected<ItemPage> page =
                catalog->query([&](ReadTransaction& tx) { return tx.list_items(query); });
            LUCENT_ASSERT_OK(page);
            listed += page->items.size();
            if (!page->next) {
                break;
            }
            query.after = page->next;
        }
        report(std::format("list 20000 items by {}, pages of 500", name), milliseconds_since(start));
        EXPECT_EQ(listed, 2 * kRows);
    }

    start = Clock::now();
    Expected<std::uint64_t> duplicates = catalog->query([](ReadTransaction& tx) {
        ItemFilter filter;
        filter.duplicates_only = true;
        return tx.count_items(filter);
    });
    LUCENT_ASSERT_OK(duplicates);
    report("count duplicates among 20000 sources", milliseconds_since(start));
    EXPECT_EQ(*duplicates, 0u);

    // Generous bound: an import of 10,000 files must not be dominated by catalog commits.
    EXPECT_LT(one_transaction, 30'000.0);
}

// The cost the batching avoids: one commit per file (reported for comparison, 500 rows only).
TEST(CatalogPerformance, OneTransactionPerRowForComparison) {
    TempDir dir;
    std::unique_ptr<Catalog> catalog = open_or_fail(dir / L"lucent.catalog");
    ASSERT_NE(catalog, nullptr);
    const Clock::time_point start = Clock::now();
    for (std::uint64_t n = 1; n <= 500; ++n) {
        LUCENT_ASSERT_OK(populate(*catalog, dir.path(), n, 1));
    }
    report("500 sources + 500 items, 500 transactions", milliseconds_since(start));
}

// The batch journal at the scale of a large export: a 10,000-job batch created in one call, then
// 300 jobs driven through their whole lifecycle with one transaction per step, as a coordinator
// commits them (claim, two stage advances, four journal steps, receipt, complete: 9 commits).
TEST(CatalogPerformance, BatchJournalOfTenThousandJobs) {
    TempDir dir;
    std::unique_ptr<Catalog> catalog = open_or_fail(dir / L"lucent.catalog");
    ASSERT_NE(catalog, nullptr);
    constexpr std::int64_t kJobs = 10'000;
    LUCENT_ASSERT_OK(catalog->write([&](WriteTransaction& tx) -> Status {
        for (std::int64_t n = 1; n <= kJobs; ++n) {
            SourceObservation o = observation(dir / std::format(L"IMG_{:05}.CR3", n), static_cast<std::uint64_t>(n));
            o.sha256 = digest(static_cast<std::uint8_t>(n));
            Expected<SourceUpsertResult> source = tx.upsert_source(o);
            if (!source) {
                return Unexpected(source.error());
            }
            const ItemMetadata metadata = item();
            if (Expected<std::vector<ItemId>> ids = tx.upsert_items(source->id, std::span(&metadata, 1)); !ids) {
                return Unexpected(ids.error());
            }
        }
        return ok();
    }));
    std::vector<NewJob> jobs(static_cast<std::size_t>(kJobs));
    for (std::int64_t n = 1; n <= kJobs; ++n) {
        jobs[static_cast<std::size_t>(n - 1)].item = ItemId{n};
        jobs[static_cast<std::size_t>(n - 1)].settings_sha256 = digest(7);
    }
    NewBatch spec;
    spec.destination_json = R"({"folder":"D:/Exports"})";
    spec.export_settings_json = R"({"format":"jpeg","quality":95})";
    spec.renderer_version = "engine";
    spec.start = true;
    Clock::time_point start = Clock::now();
    Expected<CreatedBatch> created = catalog->transact([&](WriteTransaction& tx) { return tx.create_batch(spec, jobs); });
    LUCENT_ASSERT_OK(created);
    const double create_ms = milliseconds_since(start);
    report("create a batch of 10000 jobs, 1 transaction", create_ms);

    constexpr int kLifecycles = 300;
    const Timestamp now = at(1'800'000'000'000);
    const std::filesystem::path out = dir / L"exports" / L"out.jpg";
    const std::filesystem::path tmp = dir / L"exports" / L"out.jpg.tmp";
    start = Clock::now();
    for (int i = 0; i < kLifecycles; ++i) {
        ClaimRequest request;
        request.owner = "worker-0";
        request.now = now;
        Expected<std::optional<ClaimedJob>> claimed =
            catalog->transact([&](WriteTransaction& tx) { return tx.claim_next_job(request); });
        LUCENT_ASSERT_OK(claimed);
        ASSERT_TRUE(claimed->has_value());
        const JobLease lease = (*claimed)->lease;
        const auto record_step = [&](PublicationStep step) {
            PublicationEntry entry;
            entry.step = step;
            entry.final_path = out;
            entry.temp_path = tmp;
            entry.output_sha256 = digest(1);
            entry.output_size = 1000;
            return catalog->write([&](WriteTransaction& tx) { return tx.record_publication_step(lease, entry, now); });
        };
        const auto advance = [&](JobStage from) {
            return catalog->transact([&](WriteTransaction& tx) { return tx.advance_job(lease, from, now); });
        };
        LUCENT_ASSERT_OK(record_step(PublicationStep::Reserved));
        LUCENT_ASSERT_OK(record_step(PublicationStep::TempWritten));
        LUCENT_ASSERT_OK(advance(JobStage::Rendering));
        LUCENT_ASSERT_OK(record_step(PublicationStep::Verified));
        LUCENT_ASSERT_OK(advance(JobStage::Verifying));
        LUCENT_ASSERT_OK(record_step(PublicationStep::Published));
        NewReceipt receipt;
        receipt.final_path = out;
        receipt.size_bytes = 1000;
        receipt.output_sha256 = digest(1);
        LUCENT_ASSERT_OK(catalog->transact([&](WriteTransaction& tx) { return tx.record_receipt(lease, receipt, now); }));
        LUCENT_ASSERT_OK(advance(JobStage::Publishing));
    }
    const double lifecycle_ms = milliseconds_since(start);
    report(std::format("{} job lifecycles, 9 transactions each", kLifecycles), lifecycle_ms);

    start = Clock::now();
    Expected<std::optional<BatchRecord>> batch =
        catalog->query([&](ReadTransaction& tx) { return tx.find_batch(created->id); });
    LUCENT_ASSERT_OK(batch);
    report("batch report counts over 10000 jobs", milliseconds_since(start));
    ASSERT_TRUE(batch->has_value());
    EXPECT_EQ((*batch)->counts.complete, static_cast<std::uint64_t>(kLifecycles));
    EXPECT_EQ((*batch)->counts.queued, static_cast<std::uint64_t>(kJobs - kLifecycles));

    start = Clock::now();
    Expected<std::vector<PublicationInProgress>> open = catalog->query(
        [](ReadTransaction& tx) { return tx.list_publications_in_progress(std::nullopt, 100); });
    LUCENT_ASSERT_OK(open);
    report(std::format("publications in progress among {} journal entries", kLifecycles * 5),
           milliseconds_since(start));
    EXPECT_TRUE(open->empty());

    EXPECT_LT(create_ms, 30'000.0);
    EXPECT_LT(lifecycle_ms, 120'000.0);
}

}  // namespace
}  // namespace lucent::catalog
