// Timing of large batches (reported, with generous bounds so slow machines do not fail).
//
// The measured times are printed as "[catalog timing] ..." lines and recorded as test properties.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
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

}  // namespace
}  // namespace lucent::catalog
