// Transactions: atomic batches, rollback on errors and exceptions, the single writer, concurrent
// readers during writes and snapshot isolation.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <format>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "catalog_test_support.hpp"

namespace lucent::catalog {
namespace {

using namespace test;

class CatalogTransactions : public ::testing::Test {
protected:
    void SetUp() override {
        catalog_ = open_or_fail(dir_ / L"lucent.catalog");
        ASSERT_NE(catalog_, nullptr);
    }

    std::uint64_t source_count() {
        Expected<std::uint64_t> count =
            catalog_->query([](ReadTransaction& tx) { return tx.count_sources(); });
        EXPECT_TRUE(count) << describe(count);
        return count ? *count : 0;
    }

    TempDir dir_;
    std::unique_ptr<Catalog> catalog_;
};

TEST_F(CatalogTransactions, AnErrorRollsBackTheWholeBatch) {
    const Status status = catalog_->write([&](WriteTransaction& tx) -> Status {
        for (std::uint64_t n = 1; n <= 3; ++n) {
            if (Expected<SourceUpsertResult> r =
                    tx.upsert_source(observation(dir_ / std::format(L"{}.cr3", n), n));
                !r) {
                return Unexpected(r.error());
            }
        }
        // The writer sees its own uncommitted rows.
        Expected<std::uint64_t> inside = tx.count_sources();
        EXPECT_EQ(*inside, 3u);
        return fail(ErrorCode::Cancelled, "the import was cancelled", "test.import");
    });
    ASSERT_FALSE(status);
    EXPECT_EQ(status.error().code, ErrorCode::Cancelled);  // the callback's error, unchanged
    EXPECT_EQ(status.error().where, "test.import");
    EXPECT_EQ(source_count(), 0u);
}

TEST_F(CatalogTransactions, ExceptionsRollBackAndBecomeErrors) {
    const Status thrown = catalog_->write([&](WriteTransaction& tx) -> Status {
        (void)tx.upsert_source(observation(dir_ / L"a.cr3", 1));
        throw std::runtime_error("boom");
    });
    ASSERT_FALSE(thrown);
    EXPECT_EQ(thrown.error().code, ErrorCode::Internal);
    EXPECT_NE(thrown.error().message.find("boom"), std::string::npos);
    const Status oom = catalog_->write([&](WriteTransaction& tx) -> Status {
        (void)tx.upsert_source(observation(dir_ / L"a.cr3", 1));
        throw std::bad_alloc();
    });
    ASSERT_FALSE(oom);
    EXPECT_EQ(oom.error().code, ErrorCode::OutOfMemory);
    EXPECT_EQ(source_count(), 0u);
    const Status read_throw = catalog_->read([](ReadTransaction&) -> Status { throw 42; });
    ASSERT_FALSE(read_throw);
    EXPECT_EQ(read_throw.error().code, ErrorCode::Internal);
    // The writer is usable afterwards.
    LUCENT_ASSERT_OK(populate(*catalog_, dir_.path(), 1, 2));
    EXPECT_EQ(source_count(), 2u);
}

TEST_F(CatalogTransactions, AFailedCallInsideACommittedTransactionLeavesNoTrace) {
    const Status status = catalog_->write([&](WriteTransaction& tx) -> Status {
        Expected<SourceUpsertResult> source = tx.upsert_source(observation(dir_ / L"a.cr3", 1));
        if (!source) {
            return Unexpected(source.error());
        }
        ItemMetadata bad = item(1);
        bad.support_reasons_json = "{broken";
        const std::vector<ItemMetadata> items = {item(0), bad};
        EXPECT_FALSE(tx.upsert_items(source->id, items));  // the caller chooses to carry on
        return ok();
    });
    LUCENT_ASSERT_OK(status);
    EXPECT_EQ(source_count(), 1u);
    EXPECT_EQ(*catalog_->query([](ReadTransaction& tx) { return tx.count_items({}); }), 0u);
}

TEST_F(CatalogTransactions, NestedWritesAreRefusedInsteadOfDeadlocking) {
    Status inner = ok();
    LUCENT_ASSERT_OK(catalog_->write([&](WriteTransaction& tx) -> Status {
        inner = catalog_->write([](WriteTransaction&) { return ok(); });
        return tx.upsert_source(observation(dir_ / L"a.cr3", 1)).map([](auto&&) {});
    }));
    ASSERT_FALSE(inner);
    EXPECT_EQ(inner.error().code, ErrorCode::Internal);
    EXPECT_EQ(source_count(), 1u);
}

TEST_F(CatalogTransactions, EmptyCallbacksAreInvalid) {
    EXPECT_EQ(catalog_->write({}).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(catalog_->read({}).error().code, ErrorCode::InvalidArgument);
}

TEST_F(CatalogTransactions, ABatchOfAThousandFilesIsOneTransaction) {
    LUCENT_ASSERT_OK(populate(*catalog_, dir_ / L"batch", 1, 1000));
    EXPECT_EQ(source_count(), 1000u);
    // All or nothing: a batch that fails at its last file leaves none of the 1,000 behind.
    const Status failed = catalog_->write([&](WriteTransaction& tx) -> Status {
        for (std::uint64_t n = 2000; n < 3000; ++n) {
            if (Expected<SourceUpsertResult> r =
                    tx.upsert_source(observation(dir_ / std::format(L"{}.cr3", n), n));
                !r) {
                return Unexpected(r.error());
            }
        }
        return fail(ErrorCode::DiskFull, "simulated", "test");
    });
    ASSERT_FALSE(failed);
    EXPECT_EQ(source_count(), 1000u);
}

TEST_F(CatalogTransactions, WritersFromManyThreadsAreSerialised) {
    constexpr int kThreads = 6;
    constexpr int kPerThread = 40;
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const std::uint64_t n = static_cast<std::uint64_t>(t * 1000 + i);
                const Status s = catalog_->write([&](WriteTransaction& tx) {
                    return tx.upsert_source(observation(dir_ / std::format(L"t{}-{}.cr3", t, i), n))
                        .map([](auto&&) {});
                });
                if (!s) {
                    ++failures;
                    ADD_FAILURE() << s.error().describe();
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(source_count(), static_cast<std::uint64_t>(kThreads * kPerThread));
}

// Readers run while a writer commits batches; every read transaction sees a whole number of
// batches, with sources and items consistent, and never fails with "busy".
TEST_F(CatalogTransactions, ConcurrentReadersSeeOnlyWholeBatches) {
    constexpr std::uint64_t kBatch = 25;
    constexpr std::uint64_t kBatches = 40;
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> reads{0};
    std::atomic<int> problems{0};

    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&] {
            while (!done.load()) {
                const Status s = catalog_->read([&](ReadTransaction& tx) -> Status {
                    Expected<std::uint64_t> sources = tx.count_sources();
                    if (!sources) {
                        return Unexpected(sources.error());
                    }
                    std::this_thread::yield();  // give the writer a chance to commit mid-transaction
                    Expected<std::uint64_t> items = tx.count_items({});
                    if (!items) {
                        return Unexpected(items.error());
                    }
                    ItemQuery query;
                    query.limit = 50;
                    Expected<ItemPage> page = tx.list_items(query);
                    if (!page) {
                        return Unexpected(page.error());
                    }
                    if (*sources % kBatch != 0 || *items != *sources ||
                        page->items.size() != std::min<std::uint64_t>(50, *items)) {
                        ++problems;
                        ADD_FAILURE()
                            << "inconsistent snapshot: " << *sources << " sources, " << *items << " items";
                    }
                    return ok();
                });
                if (!s) {
                    ++problems;
                    ADD_FAILURE() << s.error().describe();
                }
                ++reads;
            }
        });
    }
    for (std::uint64_t b = 0; b < kBatches; ++b) {
        const Status written = populate(*catalog_, dir_ / L"concurrent", 1 + b * kBatch, kBatch);
        if (!written) {
            ADD_FAILURE() << written.error().describe();
            break;
        }
    }
    done = true;
    for (std::thread& reader : readers) {
        reader.join();
    }
    EXPECT_EQ(problems.load(), 0);
    EXPECT_GT(reads.load(), 0u);
    EXPECT_EQ(source_count(), kBatch * kBatches);
}

// A read transaction keeps its snapshot while a write commits; the next read sees the commit.
TEST_F(CatalogTransactions, ReadTransactionsAreSnapshots) {
    LUCENT_ASSERT_OK(populate(*catalog_, dir_.path(), 1, 5));
    std::mutex mutex;
    std::condition_variable changed;
    bool first_read_done = false;
    bool write_done = false;
    std::uint64_t before = 0;
    std::uint64_t after = 0;
    std::thread reader([&] {
        const Status s = catalog_->read([&](ReadTransaction& tx) -> Status {
            before = *tx.count_sources();
            {
                std::unique_lock lock(mutex);
                first_read_done = true;
                changed.notify_all();
                changed.wait(lock, [&] { return write_done; });
            }
            after = *tx.count_sources();
            return ok();
        });
        EXPECT_TRUE(s) << describe(s);
    });
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return first_read_done; });
    }
    const Status written =
        populate(*catalog_, dir_.path(), 100, 3);  // commits while the reader holds its snapshot
    EXPECT_TRUE(written) << describe(written);
    {
        std::lock_guard lock(mutex);
        write_done = true;
    }
    changed.notify_all();
    reader.join();
    EXPECT_EQ(before, 5u);
    EXPECT_EQ(after, 5u);
    EXPECT_EQ(source_count(), 8u);
}

TEST_F(CatalogTransactions, ReaderPoolIsBoundedAndWaitsForAFreeConnection) {
    catalog_.reset();
    OpenOptions options = options_for(dir_ / L"lucent.catalog");
    options.max_readers = 1;
    Expected<std::unique_ptr<Catalog>> opened = Catalog::open(options);
    LUCENT_ASSERT_OK(opened);
    Catalog& catalog = **opened;
    std::atomic<int> inside{0};
    std::atomic<int> max_inside{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 20; ++i) {
                LUCENT_EXPECT_OK(catalog.read([&](ReadTransaction& tx) -> Status {
                    const int now = ++inside;
                    int seen = max_inside.load();
                    while (now > seen && !max_inside.compare_exchange_weak(seen, now)) {
                    }
                    Expected<std::uint64_t> count = tx.count_sources();
                    --inside;
                    return count ? ok() : Status(Unexpected(count.error()));
                }));
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(max_inside.load(), 1);
}

TEST_F(CatalogTransactions, QueryAndTransactReturnValues) {
    Expected<SourceUpsertResult> inserted = catalog_->transact(
        [&](WriteTransaction& tx) { return tx.upsert_source(observation(dir_ / L"a.cr3", 1)); });
    LUCENT_ASSERT_OK(inserted);
    EXPECT_EQ(inserted->outcome, SourceUpsertOutcome::Inserted);
    Expected<std::optional<SourceRecord>> found =
        catalog_->query([&](ReadTransaction& tx) { return tx.find_source(inserted->id); });
    LUCENT_ASSERT_OK(found);
    EXPECT_TRUE(found->has_value());
    // A failing transact() rolls back and returns the error.
    Expected<SourceUpsertResult> failed =
        catalog_->transact([&](WriteTransaction& tx) -> Expected<SourceUpsertResult> {
            (void)tx.upsert_source(observation(dir_ / L"b.cr3", 2));
            return Unexpected(Error{ErrorCode::IoError, "disk went away", "test", 0});
        });
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, ErrorCode::IoError);
    EXPECT_EQ(source_count(), 1u);
}

// Another program holding SQLite's write lock (not a Lucent catalog, so not stopped by the lock
// file): the writer waits busy_timeout, then reports a transient IoError with SQLITE_BUSY.
TEST_F(CatalogTransactions, BusyTimeoutAppliesToForeignLockHolders) {
    catalog_.reset();
    OpenOptions options = options_for(dir_ / L"lucent.catalog");
    options.busy_timeout = std::chrono::milliseconds(150);
    Expected<std::unique_ptr<Catalog>> opened = Catalog::open(options);
    LUCENT_ASSERT_OK(opened);
    std::unique_ptr<detail::Connection> foreign = raw_connection(dir_ / L"lucent.catalog");
    ASSERT_NE(foreign, nullptr);
    LUCENT_ASSERT_OK(foreign->exec("BEGIN IMMEDIATE", "test"));

    const auto start = std::chrono::steady_clock::now();
    const Status blocked = (*opened)->write([](WriteTransaction&) { return ok(); });
    const auto waited = std::chrono::steady_clock::now() - start;
    ASSERT_FALSE(blocked);
    EXPECT_EQ(blocked.error().code, ErrorCode::IoError) << blocked.error().describe();
    EXPECT_EQ(blocked.error().native_code & 0xFF, 5);  // SQLITE_BUSY
    EXPECT_GE(waited, std::chrono::milliseconds(100));
    // Readers are not blocked by a writer in WAL mode.
    LUCENT_EXPECT_OK((*opened)->query([](ReadTransaction& tx) { return tx.count_sources(); }));

    LUCENT_ASSERT_OK(foreign->exec("ROLLBACK", "test"));
    LUCENT_EXPECT_OK((*opened)->write([](WriteTransaction&) { return ok(); }));
}

TEST(CatalogSqliteErrors, ResultCodesMapToLucentCategories) {
    // Numeric SQLite result codes (sqlite3.h is private to the catalog library).
    EXPECT_EQ(detail::error_code_from_sqlite(5), ErrorCode::IoError);             // BUSY
    EXPECT_EQ(detail::error_code_from_sqlite(6), ErrorCode::IoError);             // LOCKED
    EXPECT_EQ(detail::error_code_from_sqlite(517), ErrorCode::IoError);           // BUSY_SNAPSHOT
    EXPECT_EQ(detail::error_code_from_sqlite(11), ErrorCode::Corrupt);            // CORRUPT
    EXPECT_EQ(detail::error_code_from_sqlite(26), ErrorCode::Corrupt);            // NOTADB
    EXPECT_EQ(detail::error_code_from_sqlite(13), ErrorCode::DiskFull);           // FULL
    EXPECT_EQ(detail::error_code_from_sqlite(7), ErrorCode::OutOfMemory);         // NOMEM
    EXPECT_EQ(detail::error_code_from_sqlite(3082), ErrorCode::OutOfMemory);      // IOERR_NOMEM
    EXPECT_EQ(detail::error_code_from_sqlite(8), ErrorCode::PermissionDenied);    // READONLY
    EXPECT_EQ(detail::error_code_from_sqlite(2067), ErrorCode::AlreadyExists);    // CONSTRAINT_UNIQUE
    EXPECT_EQ(detail::error_code_from_sqlite(1555), ErrorCode::AlreadyExists);    // CONSTRAINT_PRIMARYKEY
    EXPECT_EQ(detail::error_code_from_sqlite(787), ErrorCode::NotFound);          // CONSTRAINT_FOREIGNKEY
    EXPECT_EQ(detail::error_code_from_sqlite(275), ErrorCode::InvalidArgument);   // CONSTRAINT_CHECK
    EXPECT_EQ(detail::error_code_from_sqlite(1299), ErrorCode::InvalidArgument);  // CONSTRAINT_NOTNULL
    EXPECT_EQ(detail::error_code_from_sqlite(18), ErrorCode::LimitExceeded);      // TOOBIG
    EXPECT_EQ(detail::error_code_from_sqlite(9), ErrorCode::Cancelled);           // INTERRUPT
    EXPECT_EQ(detail::error_code_from_sqlite(1), ErrorCode::Internal);            // ERROR (SQL)
    EXPECT_EQ(detail::error_code_from_sqlite(21), ErrorCode::Internal);           // MISUSE
    EXPECT_EQ(detail::error_code_from_sqlite(10), ErrorCode::IoError);            // IOERR
    EXPECT_EQ(detail::error_code_from_sqlite(14), ErrorCode::IoError);            // CANTOPEN
    EXPECT_EQ(detail::error_code_from_sqlite(0), ErrorCode::Internal);            // OK is never an error
}

TEST_F(CatalogTransactions, FullDurabilityCommitsNormally) {
    catalog_.reset();
    OpenOptions options = options_for(dir_ / L"lucent.catalog");
    options.durability = Durability::Full;
    options.integrity_check = IntegrityCheck::Full;
    Expected<std::unique_ptr<Catalog>> opened = Catalog::open(options);
    LUCENT_ASSERT_OK(opened);
    LUCENT_ASSERT_OK(populate(**opened, dir_.path(), 1, 20));
    EXPECT_EQ(*(*opened)->query([](ReadTransaction& tx) { return tx.count_sources(); }), 20u);
}

TEST_F(CatalogTransactions, CheckpointTruncatesTheWal) {
    LUCENT_ASSERT_OK(populate(*catalog_, dir_.path(), 1, 200));
    const std::filesystem::path wal = dir_ / L"lucent.catalog-wal";
    ASSERT_TRUE(std::filesystem::exists(wal));
    EXPECT_GT(std::filesystem::file_size(wal), 0u);
    LUCENT_ASSERT_OK(catalog_->checkpoint());
    EXPECT_EQ(std::filesystem::file_size(wal), 0u);
    EXPECT_EQ(source_count(), 200u);
}

TEST_F(CatalogTransactions, ClosingCheckpointsAndRemovesTheWal) {
    LUCENT_ASSERT_OK(populate(*catalog_, dir_.path(), 1, 50));
    catalog_.reset();
    EXPECT_FALSE(std::filesystem::exists(dir_ / L"lucent.catalog-wal"));
    std::unique_ptr<detail::Connection> raw =
        raw_connection(dir_ / L"lucent.catalog", detail::Access::ReadOnly);
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM sources"), 50);
}

}  // namespace
}  // namespace lucent::catalog
