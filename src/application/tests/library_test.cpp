// SessionLibrary: identity and duplicate rules, atomic batches, flags, removal, and the observer
// threading contract.

#include <atomic>
#include <chrono>
#include <format>
#include <future>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/application/library.hpp>

#include "app_test_support.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;

SourceItem item(const std::wstring& path, std::uint32_t subimage = 0, std::uint32_t count = 1) {
    SourceItem i = ts::fake_item(std::filesystem::path(path));
    i.subimage = subimage;
    i.subimage_count = count;
    return i;
}

core::FileIdentity identity(std::uint8_t id) {
    core::FileIdentity f;
    f.volume_serial = 7;
    f.file_id[0] = id;
    f.size_bytes = 100;
    return f;
}

TEST(SessionLibrary, AssignsIdsAndRevisionsInInsertionOrder) {
    SessionLibrary library;
    auto outcomes = library.add({item(L"C:\\p\\b.cr3"), item(L"C:\\p\\a.cr3"), item(L"C:\\p\\c.cr3")});
    ASSERT_TRUE(outcomes);
    ASSERT_EQ(outcomes->size(), 3u);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_TRUE((*outcomes)[i].added);
        EXPECT_EQ((*outcomes)[i].id.value, i + 1);
    }
    const auto items = library.items();
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0].path, std::filesystem::path(L"C:\\p\\b.cr3"));
    EXPECT_EQ(items[2].path, std::filesystem::path(L"C:\\p\\c.cr3"));
    EXPECT_EQ(items[1].revision, 1u);
    EXPECT_EQ(library.size(), 3u);
    EXPECT_EQ(library.find(ItemId{2})->path, std::filesystem::path(L"C:\\p\\a.cr3"));
    EXPECT_FALSE(library.find(ItemId{4}));
    EXPECT_FALSE(library.find(ItemId{0}));
}

TEST(SessionLibrary, RejectsDuplicatesByPathCaseInsensitivelyAndByFileObject) {
    SessionLibrary library;
    SourceItem original = item(L"C:\\Photos\\IMG_0001.CR3");
    original.identity = identity(1);
    ASSERT_TRUE(library.add({original}));

    SourceItem other_case = item(L"c:\\photos\\img_0001.cr3");
    SourceItem hard_link = item(L"C:\\Elsewhere\\copy.cr3");
    hard_link.identity = identity(1);  // same volume + file id: the same file object
    SourceItem other_image = item(L"C:\\Photos\\IMG_0001.CR3", 1, 2);
    SourceItem in_batch_twice = item(L"C:\\Photos\\new.cr3");
    auto outcomes = library.add({other_case, hard_link, other_image, in_batch_twice, in_batch_twice});
    ASSERT_TRUE(outcomes);
    EXPECT_FALSE((*outcomes)[0].added);
    EXPECT_EQ((*outcomes)[0].duplicate, DuplicateKind::SamePath);
    EXPECT_EQ((*outcomes)[0].id, ItemId{1});
    EXPECT_FALSE((*outcomes)[1].added);
    EXPECT_EQ((*outcomes)[1].duplicate, DuplicateKind::SameFileObject);
    EXPECT_TRUE((*outcomes)[2].added);  // another image of the same file is another photograph
    EXPECT_TRUE((*outcomes)[3].added);
    EXPECT_FALSE((*outcomes)[4].added);
    EXPECT_EQ((*outcomes)[4].id, (*outcomes)[3].id);

    EXPECT_EQ(library.find_source(L"C:\\PHOTOS\\img_0001.cr3", {}, 0), ItemId{1});
    EXPECT_EQ(library.find_source(L"D:\\x.cr3", identity(1), 0), ItemId{1});
    EXPECT_FALSE(library.find_source(L"D:\\x.cr3", identity(2), 0));
    EXPECT_FALSE(library.find_source(L"D:\\x.cr3", {}, 0));  // a zero file id never matches
}

TEST(SessionLibrary, InvalidBatchesAddNothing) {
    SessionLibrary library;
    int notifications = 0;
    Subscription s = library.subscribe([&](const LibraryChange&) { ++notifications; });
    for (SourceItem bad : {item(L"relative\\a.cr3"), item(L""), item(L"C:\\a.cr3", 2, 2)}) {
        auto outcomes = library.add({item(L"C:\\ok.cr3"), bad});
        ASSERT_FALSE(outcomes);
        EXPECT_EQ(outcomes.error().code, ErrorCode::InvalidArgument);
    }
    SourceItem bad_flags = item(L"C:\\flags.cr3");
    bad_flags.flags.rating = 9;
    EXPECT_FALSE(library.add({bad_flags}));
    EXPECT_EQ(library.size(), 0u);
    EXPECT_EQ(notifications, 0);
}

TEST(SessionLibrary, FlagsAndRemoval) {
    SessionLibrary library;
    ASSERT_TRUE(library.add({item(L"C:\\a.cr3"), item(L"C:\\b.cr3"), item(L"C:\\c.cr3")}));
    std::vector<LibraryChange> changes;
    Subscription s = library.subscribe([&](const LibraryChange& c) { changes.push_back(c); });

    EXPECT_EQ(library.set_flags(ItemId{9}, {}).error().code, ErrorCode::NotFound);
    EXPECT_EQ(library.set_flags(ItemId{1}, ItemFlags{PickFlag::None, 6}).error().code,
              ErrorCode::InvalidArgument);
    ASSERT_TRUE(library.set_flags(ItemId{1}, ItemFlags{}));  // unchanged: no notification
    EXPECT_TRUE(changes.empty());
    ASSERT_TRUE(library.set_flags(ItemId{1}, ItemFlags{PickFlag::Picked, 4}));
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].kind, LibraryChangeKind::Updated);
    EXPECT_EQ(library.find(ItemId{1})->revision, 2u);
    EXPECT_EQ(library.find(ItemId{1})->flags.rating, 4);

    const std::vector<ItemId> unknown = {ItemId{2}, ItemId{42}};
    EXPECT_EQ(library.remove(unknown).error().code, ErrorCode::NotFound);
    EXPECT_EQ(library.size(), 3u);  // nothing removed

    const std::vector<ItemId> twice = {ItemId{3}, ItemId{1}, ItemId{3}};
    ASSERT_TRUE(library.remove(twice));
    ASSERT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes[1].kind, LibraryChangeKind::Removed);
    EXPECT_EQ(changes[1].items, (std::vector<ItemId>{ItemId{3}, ItemId{1}}));
    EXPECT_GT(changes[1].sequence, changes[0].sequence);
    EXPECT_EQ(library.size(), 1u);
    // A removed path can be added again (with a new id).
    auto again = library.add({item(L"C:\\a.cr3")});
    ASSERT_TRUE(again);
    EXPECT_TRUE((*again)[0].added);
    EXPECT_EQ((*again)[0].id, ItemId{4});
}

TEST(SessionLibrary, RefreshReplacesTheSourceFactsOfAnItemInPlace) {
    SessionLibrary library;
    SourceItem a = item(L"C:\\p\\a.cr3");
    a.identity = identity(1);
    SourceItem b = item(L"C:\\p\\b.cr3");
    b.identity = identity(2);
    ASSERT_TRUE(library.add({a, b}));
    ASSERT_TRUE(library.set_flags(ItemId{1}, ItemFlags{PickFlag::Rejected, 1}));
    std::vector<LibraryChange> changes;
    Subscription s = library.subscribe([&](const LibraryChange& c) { changes.push_back(c); });

    SourceItem fresh = item(L"C:\\P\\A.CR3");  // the same canonical path, another spelling
    fresh.id = ItemId{1};
    fresh.identity = identity(3);
    fresh.identity.size_bytes = 200;
    fresh.support.status = SupportStatus::Limited;
    fresh.probe->camera_model = "after";

    SourceItem unknown = fresh;
    unknown.id = ItemId{9};
    EXPECT_EQ(library.refresh(unknown).error().code, ErrorCode::NotFound);
    SourceItem other_file = fresh;
    other_file.path = L"C:\\p\\c.cr3";
    EXPECT_EQ(library.refresh(other_file).error().code, ErrorCode::InvalidArgument);
    SourceItem other_image = fresh;
    other_image.subimage = 1;
    other_image.subimage_count = 2;
    EXPECT_EQ(library.refresh(other_image).error().code, ErrorCode::InvalidArgument);
    SourceItem relative = fresh;
    relative.path = L"a.cr3";
    EXPECT_EQ(library.refresh(relative).error().code, ErrorCode::InvalidArgument);
    SourceItem taken = fresh;
    taken.identity = identity(2);  // b's file object
    EXPECT_EQ(library.refresh(taken).error().code, ErrorCode::AlreadyExists);
    EXPECT_TRUE(changes.empty());  // nothing changed so far
    EXPECT_EQ(library.find(ItemId{1})->identity, identity(1));

    ASSERT_TRUE(library.refresh(fresh));
    const SourceItem after = *library.find(ItemId{1});
    EXPECT_EQ(after.path, std::filesystem::path(L"C:\\P\\A.CR3"));
    EXPECT_EQ(after.identity, fresh.identity);
    EXPECT_EQ(after.support.status, SupportStatus::Limited);
    EXPECT_EQ(after.probe->camera_model, "after");
    EXPECT_EQ(after.flags, (ItemFlags{PickFlag::Rejected, 1}));  // kept
    EXPECT_EQ(after.revision, 3u);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].kind, LibraryChangeKind::Refreshed);
    EXPECT_EQ(changes[0].items, (std::vector<ItemId>{ItemId{1}}));
    // Lookups follow the new file object; the old one no longer names the item.
    EXPECT_EQ(library.find_source(L"C:\\elsewhere\\link.cr3", identity(3), 0), ItemId{1});
    EXPECT_FALSE(library.find_source(L"C:\\elsewhere\\link.cr3", identity(1), 0));
    EXPECT_EQ(library.find_source(L"C:\\p\\a.cr3", {}, 0), ItemId{1});
    // Adding a file under the old file object is no longer a duplicate of the refreshed item.
    SourceItem reuse = item(L"C:\\q\\z.cr3");
    reuse.identity = identity(1);
    auto added = library.add({reuse});
    ASSERT_TRUE(added);
    EXPECT_TRUE((*added)[0].added);
}

TEST(SessionLibrary, ObserversMayReenterAndUnsubscribeThemselves) {
    SessionLibrary library;
    std::vector<std::uint64_t> sequences;
    Subscription self;
    int self_calls = 0;
    self = library.subscribe([&](const LibraryChange&) {
        ++self_calls;
        self.reset();  // unsubscribing from inside the observer does not wait for this call
    });
    Subscription reentrant = library.subscribe([&](const LibraryChange& change) {
        sequences.push_back(change.sequence);
        if (change.kind == LibraryChangeKind::Added && library.size() == 1) {
            ASSERT_TRUE(library.add({item(L"C:\\second.cr3")}));  // nested change, delivered re-entrantly
        }
    });
    ASSERT_TRUE(library.add({item(L"C:\\first.cr3")}));
    EXPECT_EQ(self_calls, 1);
    EXPECT_EQ(sequences, (std::vector<std::uint64_t>{1, 2}));
    EXPECT_EQ(library.size(), 2u);
    ASSERT_TRUE(library.add({item(L"C:\\third.cr3")}));
    EXPECT_EQ(self_calls, 1);
}

TEST(SessionLibrary, AThrowingObserverDoesNotStopTheOthers) {
    SessionLibrary library;
    int calls = 0;
    Subscription a =
        library.subscribe([](const LibraryChange&) { throw std::runtime_error("observer failure"); });
    Subscription b = library.subscribe([&](const LibraryChange&) { ++calls; });
    ASSERT_TRUE(library.add({item(L"C:\\x.cr3")}));
    EXPECT_EQ(calls, 1);
}

TEST(SessionLibrary, ResetWaitsForAnObserverRunningOnAnotherThread) {
    SessionLibrary library;
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    std::atomic<bool> observer_finished{false};
    std::atomic<int> calls{0};
    Subscription s = library.subscribe([&](const LibraryChange&) {
        if (calls.fetch_add(1) == 0) {
            entered.set_value();
            released.wait();
            observer_finished = true;
        }
    });
    std::thread mutator([&] { ASSERT_TRUE(library.add({item(L"C:\\slow.cr3")})); });
    entered.get_future().wait();

    std::atomic<bool> reset_returned{false};
    std::thread resetter([&] {
        s.reset();
        reset_returned = true;
        EXPECT_TRUE(observer_finished.load());  // reset() returned only after the call ended
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(reset_returned.load());
    release.set_value();
    resetter.join();
    mutator.join();
    EXPECT_TRUE(reset_returned.load());

    ASSERT_TRUE(library.add({item(L"C:\\after.cr3")}));
    EXPECT_EQ(calls.load(), 1);  // never called after reset()
}

TEST(SessionLibrary, ConcurrentAddsKeepIdsUniqueAndSequencesIncreasing) {
    SessionLibrary library;
    std::mutex mutex;
    std::set<std::uint64_t> sequences;
    Subscription s = library.subscribe([&](const LibraryChange& c) {
        std::lock_guard lock(mutex);
        EXPECT_TRUE(sequences.insert(c.sequence).second);
    });
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 50; ++i) {
                // Every thread also adds the same shared paths: exactly one wins each.
                ASSERT_TRUE(library.add({item(std::format(L"C:\\t{}\\{}.cr3", t, i)),
                                         item(std::format(L"C:\\shared\\{}.cr3", i))}));
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    const auto items = library.items();
    EXPECT_EQ(items.size(), 4u * 50u + 50u);
    std::set<std::uint64_t> ids;
    for (const SourceItem& i : items) {
        EXPECT_TRUE(ids.insert(i.id.value).second);
    }
    EXPECT_EQ(sequences.size(), 200u);  // one change per add call (each added at least its own path)
    EXPECT_EQ(*sequences.rbegin(), 200u);
}

}  // namespace
}  // namespace lucent::app
