#pragma once

// Helpers for the batch tests (W7): a catalog-backed library with a recipe service and a
// BatchService over the built lucent-worker.exe, an event collector, and file checks.

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>
#include <psapi.h>

#include <lucent/application/batch_service.hpp>
#include <lucent/application/import.hpp>
#include <lucent/application/recipe_service.hpp>
#include <lucent/application/recipe_store.hpp>
#include <lucent/application/source_maintenance.hpp>
#include <lucent/core/file.hpp>
#include <lucent/core/hash.hpp>

#include "app_test_support.hpp"

namespace lucent::app::test_support {

using namespace std::chrono_literals;

inline std::filesystem::path worker_exe() { return std::filesystem::path(LUCENT_WORKER_EXE).make_preferred(); }

// Short intervals so that failures and retries happen within seconds.
inline BatchServiceOptions fast_batch_options() {
    BatchServiceOptions options;
    options.worker_executable = worker_exe();
    options.heartbeat_interval = 100ms;
    options.heartbeat_timeout = 10s;
    options.cancel_grace = 3s;
    options.lease_duration = 10s;
    options.lease_renewal = 1s;
    options.retry.initial_backoff = 50ms;
    options.retry.max_backoff = 200ms;
    options.job_timeout = 120s;
    return options;
}

// Records every batch event; waits for conditions on them.
class EventLog {
public:
    void add(const BatchEvent& event) {
        {
            std::lock_guard lock(mutex_);
            events_.push_back(event);
        }
        changed_.notify_all();
    }
    [[nodiscard]] std::vector<BatchEvent> events() const {
        std::lock_guard lock(mutex_);
        return events_;
    }
    // Waits until `predicate(events)` holds; false after `timeout`.
    bool wait_for(const std::function<bool(const std::vector<BatchEvent>&)>& predicate,
                  std::chrono::milliseconds timeout = 120s) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return predicate(events_); });
    }
    bool wait_for_kind(catalog::BatchId batch, BatchEventKind kind, std::chrono::milliseconds timeout = 120s) {
        return wait_for(
            [&](const std::vector<BatchEvent>& events) {
                for (const BatchEvent& event : events) {
                    if (event.batch == batch && event.kind == kind) {
                        return true;
                    }
                }
                return false;
            },
            timeout);
    }
    [[nodiscard]] std::size_t count(catalog::BatchId batch, BatchEventKind kind) const {
        std::lock_guard lock(mutex_);
        std::size_t n = 0;
        for (const BatchEvent& event : events_) {
            n += event.batch == batch && event.kind == kind ? 1 : 0;
        }
        return n;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<BatchEvent> events_;
};

// A persistent library, recipes and a batch service in `root` (root\data\lucent.catalog).
class BatchHarness {
public:
    explicit BatchHarness(std::filesystem::path root, BatchServiceOptions options = fast_batch_options())
        : root_(std::move(root)), options_(std::move(options)) {
        registry_ = default_registry();
        open();
    }
    ~BatchHarness() { close(); }

    void open() {
        fixture_ = CatalogFixture::open(root_ / L"data\\lucent.catalog");
        ASSERT_NE(fixture_.library, nullptr);
        auto store = create_catalog_recipe_store(fixture_.catalog);
        ASSERT_TRUE(store) << store.error().describe();
        store_ = std::move(*store);
        RecipeServiceOptions recipe_options;
        recipe_options.registry = registry_;
        recipes_ = std::make_unique<RecipeService>(*fixture_.library, store_, std::move(recipe_options));
        BatchServiceOptions options = options_;
        options.registry = registry_;
        auto opened = BatchService::open(*fixture_.library, fixture_.catalog, *recipes_, std::move(options));
        ASSERT_TRUE(opened) << opened.error().describe();
        batches_ = std::move(*opened);
        subscription_ = batches_->subscribe([this](const BatchEvent& event) { log.add(event); });
        ASSERT_TRUE(batches_->wait_for_recovery(120s));
    }

    void close() {
        subscription_.reset();
        batches_.reset();
        recipes_.reset();
        store_.reset();
        fixture_.close();
    }

    void reopen() {
        close();
        open();
    }

    // Imports `files` and hashes them; returns their item ids in order.
    std::vector<ItemId> import(const std::vector<std::filesystem::path>& files, bool hash = true) {
        auto report = import_sources(library(), *registry_, ImportRequest{.files = files});
        EXPECT_TRUE(report) << (report ? std::string() : report.error().describe());
        std::vector<ItemId> ids;
        const std::vector<SourceItem> items = library().items();
        for (const std::filesystem::path& file : files) {
            for (const SourceItem& item : items) {
                if (_wcsicmp(item.path.filename().c_str(), file.filename().c_str()) == 0 && item.subimage == 0 &&
                    std::filesystem::equivalent(item.path, file)) {
                    ids.push_back(item.id);
                    break;
                }
            }
        }
        EXPECT_EQ(ids.size(), files.size());
        if (hash) {
            auto hashed = hash_sources(library(), ids);
            EXPECT_TRUE(hashed) << (hashed ? std::string() : hashed.error().describe());
        }
        return ids;
    }

    BatchRequest request(const std::vector<ItemId>& items, const std::filesystem::path& folder) const {
        BatchRequest r;
        for (const ItemId id : items) {
            r.items.push_back(BatchItem{id, std::nullopt});
        }
        r.destination.folder = folder;
        return r;
    }

    // Submits and waits for the batch to finish (true when it did).
    catalog::BatchId run(const BatchRequest& request, std::chrono::milliseconds timeout = 180s) {
        auto submitted = batches_->submit(request);
        EXPECT_TRUE(submitted) << (submitted ? std::string() : submitted.error().describe());
        if (!submitted) {
            return {};
        }
        EXPECT_TRUE(log.wait_for_kind(submitted->id, BatchEventKind::Finished, timeout))
            << "batch " << submitted->id.value << " did not finish";
        return submitted->id;
    }

    [[nodiscard]] catalog::BatchRecord record(catalog::BatchId id) const {
        auto status = batches_->status(id);
        EXPECT_TRUE(status) << (status ? std::string() : status.error().describe());
        return status ? status->record : catalog::BatchRecord{};
    }

    [[nodiscard]] std::vector<catalog::JobRecord> jobs(catalog::BatchId id) const {
        catalog::JobQuery query;
        query.batch = id;
        auto jobs = fixture_.catalog->query([&](catalog::ReadTransaction& tx) { return tx.list_jobs(query); });
        EXPECT_TRUE(jobs);
        return jobs ? *jobs : std::vector<catalog::JobRecord>{};
    }

    [[nodiscard]] std::vector<catalog::ReceiptRecord> receipts(catalog::BatchId id) const {
        std::vector<catalog::ReceiptRecord> out;
        for (const catalog::JobRecord& job : jobs(id)) {
            auto receipt = fixture_.catalog->query([&](catalog::ReadTransaction& tx) { return tx.find_receipt(job.id); });
            if (receipt && *receipt) {
                out.push_back(**receipt);
            }
        }
        return out;
    }

    CatalogLibrary& library() { return *fixture_.library; }
    BatchService& batches() { return *batches_; }
    RecipeService& recipes() { return *recipes_; }
    catalog::Catalog& catalog() { return *fixture_.catalog; }
    const std::shared_ptr<const raw::DecoderRegistry>& registry() const { return registry_; }
    BatchServiceOptions& options() { return options_; }

    EventLog log;

private:
    std::filesystem::path root_;
    BatchServiceOptions options_;
    std::shared_ptr<const raw::DecoderRegistry> registry_;
    CatalogFixture fixture_;
    std::shared_ptr<RecipeStore> store_;
    std::unique_ptr<RecipeService> recipes_;
    std::unique_ptr<BatchService> batches_;
    Subscription subscription_;
};

// Synthetic DNGs of distinct content: `count` files "<prefix><i>.dng" in `folder`.
inline std::vector<std::filesystem::path> write_dngs(const ScratchDir& dir, const std::filesystem::path& folder,
                                                     std::size_t count, const std::wstring& prefix = L"photo",
                                                     std::size_t first = 0) {
    std::vector<std::filesystem::path> files;
    for (std::size_t i = first; i < first + count; ++i) {
        rts::SyntheticDngSpec spec;
        spec.width = 64 + 2 * static_cast<std::uint32_t>(i % 200);
        spec.height = 48 + 2 * static_cast<std::uint32_t>(i / 200);
        files.push_back(write_dng(dir, folder / (prefix + std::to_wstring(i) + L".dng"), spec));
    }
    return files;
}

// The names of the regular files and folders directly in `folder`, sorted.
inline std::vector<std::wstring> folder_entries(const std::filesystem::path& folder) {
    std::vector<std::wstring> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        names.push_back(entry.path().filename().wstring());
    }
    std::sort(names.begin(), names.end());
    return names;
}

// Leftovers of publications ("~lucent-*") directly in `folder`.
inline std::vector<std::wstring> leftovers(const std::filesystem::path& folder) {
    std::vector<std::wstring> out;
    for (const std::wstring& name : folder_entries(folder)) {
        if (name.rfind(L"~lucent-", 0) == 0) {
            out.push_back(name);
        }
    }
    return out;
}

// Private bytes of this process.
inline std::uint64_t private_bytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                sizeof(counters))) {
        return 0;
    }
    return counters.PrivateUsage;
}

}  // namespace lucent::app::test_support
