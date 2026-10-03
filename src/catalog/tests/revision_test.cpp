// Recipe revisions (schema v2): append, history, undo/redo and branches, immutability, validation,
// Unicode, large documents, concurrency with readers; and the v2 capture fields of items.

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "lucent/core/hash.hpp"

#include "catalog_test_support.hpp"

namespace lucent::catalog {
namespace {

using namespace test;

NewRevision revision(std::string settings, RevisionOrigin origin = RevisionOrigin::User,
                     std::string note = {}) {
    NewRevision r;
    r.origin = origin;
    r.schema_name = "lucent.develop";
    r.schema_version = 1;
    r.settings_json = std::move(settings);
    r.engine_version = "0.6.0";
    r.note = std::move(note);
    r.created_at = at(1'800'000'000'000);
    return r;
}

std::string exposure(double ev) {
    return std::format(R"({{"schema":"lucent.develop","version":1,"exposure_ev":{}}})", ev);
}

class CatalogRevisions : public ::testing::Test {
protected:
    void SetUp() override {
        catalog_ = open_or_fail(path_);
        ASSERT_NE(catalog_, nullptr);
        LUCENT_ASSERT_OK(populate(*catalog_, dir_ / L"photos", 1, 3));  // items 1, 2, 3
    }

    Expected<RevisionInfo> append(ItemId item, const NewRevision& r) {
        return catalog_->transact([&](WriteTransaction& tx) { return tx.append_revision(item, r); });
    }
    RevisionId append_ok(ItemId item, double ev) {
        Expected<RevisionInfo> info = append(item, revision(exposure(ev)));
        EXPECT_TRUE(info) << describe(info);
        return info ? info->id : RevisionId{};
    }
    RevisionHistory history(ItemId item) {
        Expected<RevisionHistory> h =
            catalog_->query([&](ReadTransaction& tx) { return tx.revision_history(item); });
        EXPECT_TRUE(h) << describe(h);
        return h ? *h : RevisionHistory{};
    }
    std::optional<RevisionId> current(ItemId item) { return history(item).current; }
    Expected<std::optional<RevisionId>> undo(ItemId item) {
        return catalog_->transact([&](WriteTransaction& tx) { return tx.undo_revision(item); });
    }
    Expected<RevisionId> redo(ItemId item) {
        return catalog_->transact([&](WriteTransaction& tx) { return tx.redo_revision(item); });
    }
    Status set_current(ItemId item, std::optional<RevisionId> to) {
        return catalog_->write([&](WriteTransaction& tx) { return tx.set_current_revision(item, to); });
    }

    TempDir dir_;
    std::filesystem::path path_ = dir_ / L"lucent.catalog";
    std::unique_ptr<Catalog> catalog_;
};

TEST_F(CatalogRevisions, ANewItemShowsAsShot) {
    const RevisionHistory h = history(ItemId{1});
    EXPECT_EQ(h.item, ItemId{1});
    EXPECT_FALSE(h.current.has_value());
    EXPECT_TRUE(h.revisions.empty());
    Expected<std::optional<RecipeRevision>> now =
        catalog_->query([](ReadTransaction& tx) { return tx.current_revision(ItemId{1}); });
    LUCENT_ASSERT_OK(now);
    EXPECT_FALSE(now->has_value());
    Expected<std::optional<RecipeRevision>> none =
        catalog_->query([](ReadTransaction& tx) { return tx.find_revision(RevisionId{12345}); });
    LUCENT_ASSERT_OK(none);
    EXPECT_FALSE(none->has_value());
}

TEST_F(CatalogRevisions, AppendStoresTheRevisionAndMakesItCurrent) {
    NewRevision first = revision("{}", RevisionOrigin::Auto, "Auto (Natural)");
    first.created_at = at(1'800'000'000'123);
    Expected<RevisionInfo> a = append(ItemId{2}, first);
    LUCENT_ASSERT_OK(a);
    EXPECT_TRUE(a->id.valid());
    EXPECT_EQ(a->item, ItemId{2});
    EXPECT_EQ(a->revision_number, 1u);
    EXPECT_FALSE(a->parent.has_value());
    EXPECT_EQ(a->origin, RevisionOrigin::Auto);
    EXPECT_EQ(a->settings_bytes, 2u);
    // SHA-256("{}"), an independent reference value.
    EXPECT_EQ(core::to_hex(a->settings_sha256),
              "44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a");

    Expected<RevisionInfo> b = append(ItemId{2}, revision(exposure(0.7)));
    LUCENT_ASSERT_OK(b);
    EXPECT_EQ(b->revision_number, 2u);
    EXPECT_EQ(b->parent, a->id);

    Expected<std::optional<RecipeRevision>> now =
        catalog_->query([](ReadTransaction& tx) { return tx.current_revision(ItemId{2}); });
    LUCENT_ASSERT_OK(now);
    ASSERT_TRUE(now->has_value());
    EXPECT_EQ((*now)->info.id, b->id);
    EXPECT_EQ((*now)->settings_json, exposure(0.7));
    EXPECT_EQ((*now)->info.schema_name, "lucent.develop");
    EXPECT_EQ((*now)->info.schema_version, 1u);
    EXPECT_EQ((*now)->info.engine_version, "0.6.0");

    Expected<std::optional<RecipeRevision>> stored =
        catalog_->query([&](ReadTransaction& tx) { return tx.find_revision(a->id); });
    LUCENT_ASSERT_OK(stored);
    ASSERT_TRUE(stored->has_value());
    EXPECT_EQ((*stored)->settings_json, "{}");
    EXPECT_EQ((*stored)->info.note, "Auto (Natural)");
    EXPECT_EQ((*stored)->info.created_at, at(1'800'000'000'123));
    EXPECT_EQ((*stored)->info.settings_sha256, a->settings_sha256);

    // The item record shows the pointer; other items are untouched and number independently.
    Expected<std::optional<ItemRecord>> item =
        catalog_->query([](ReadTransaction& tx) { return tx.find_item(ItemId{2}); });
    LUCENT_ASSERT_OK(item);
    ASSERT_TRUE(item->has_value());
    EXPECT_EQ((*item)->current_revision, b->id);
    EXPECT_FALSE(current(ItemId{1}).has_value());
    Expected<RevisionInfo> other = append(ItemId{3}, revision("[]"));
    LUCENT_ASSERT_OK(other);
    EXPECT_EQ(other->revision_number, 1u);

    // Survives a reopen.
    catalog_.reset();
    catalog_ = open_or_fail(path_);
    ASSERT_NE(catalog_, nullptr);
    const RevisionHistory h = history(ItemId{2});
    EXPECT_EQ(h.current, b->id);
    ASSERT_EQ(h.revisions.size(), 2u);
    EXPECT_EQ(h.revisions[0].id, a->id);
    EXPECT_EQ(h.revisions[1].id, b->id);
}

TEST_F(CatalogRevisions, UndoRedoAndABranchAfterUndoKeepsEveryRevision) {
    const ItemId item{1};
    const RevisionId a = append_ok(item, 0.1);
    const RevisionId b = append_ok(item, 0.2);
    const RevisionId c = append_ok(item, 0.3);

    Expected<std::optional<RevisionId>> undone = undo(item);
    LUCENT_ASSERT_OK(undone);
    EXPECT_EQ(*undone, b);
    undone = undo(item);
    LUCENT_ASSERT_OK(undone);
    EXPECT_EQ(*undone, a);
    Expected<RevisionId> redone = redo(item);
    LUCENT_ASSERT_OK(redone);
    EXPECT_EQ(*redone, b);
    LUCENT_ASSERT_OK(undo(item));  // back at a

    // A new edit after undo: a branch from a. b and c stay.
    const RevisionId d = append_ok(item, 0.4);
    RevisionHistory h = history(item);
    EXPECT_EQ(h.current, d);
    ASSERT_EQ(h.revisions.size(), 4u);
    const std::vector<std::pair<RevisionId, std::optional<RevisionId>>> expected = {
        {a, std::nullopt}, {b, a}, {c, b}, {d, a}};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(h.revisions[i].id, expected[i].first) << i;
        EXPECT_EQ(h.revisions[i].parent, expected[i].second) << i;
        EXPECT_EQ(h.revisions[i].revision_number, i + 1) << i;
    }

    // Redo from a follows the newest branch (d), not the undone one.
    LUCENT_ASSERT_OK(undo(item));
    redone = redo(item);
    LUCENT_ASSERT_OK(redone);
    EXPECT_EQ(*redone, d);
    Expected<RevisionId> nothing = redo(item);
    ASSERT_FALSE(nothing);
    EXPECT_EQ(nothing.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(nothing.error().where, "catalog.redo_revision.nothing_to_redo");
    EXPECT_EQ(current(item), d);  // a failed redo changes nothing

    // An earlier recipe of the other branch can be made current again and undone along its links.
    LUCENT_ASSERT_OK(set_current(item, c));
    EXPECT_EQ(current(item), c);
    EXPECT_EQ(*undo(item), b);
    EXPECT_EQ(*undo(item), a);
    EXPECT_EQ(*undo(item), std::nullopt);  // as shot
    Expected<std::optional<RevisionId>> again = undo(item);
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(again.error().where, "catalog.undo_revision.nothing_to_undo");
    // Redo from as shot: the newest revision derived from as shot.
    EXPECT_EQ(*redo(item), a);

    // Reset: to as shot by moving the pointer (redo returns), or as a recorded Reset revision.
    LUCENT_ASSERT_OK(set_current(item, d));
    LUCENT_ASSERT_OK(set_current(item, std::nullopt));
    EXPECT_FALSE(current(item).has_value());
    LUCENT_ASSERT_OK(set_current(item, d));
    Expected<RevisionInfo> reset = append(item, revision("{}", RevisionOrigin::Reset));
    LUCENT_ASSERT_OK(reset);
    EXPECT_EQ(reset->parent, d);
    EXPECT_EQ(*undo(item), d);

    // Nothing was ever pruned.
    h = history(item);
    EXPECT_EQ(h.revisions.size(), 5u);
}

TEST_F(CatalogRevisions, RevisionsAreImmutableAndOnlyLeaveWithTheirItem) {
    const RevisionId a = append_ok(ItemId{1}, 1.0);
    const RevisionId b = append_ok(ItemId{1}, 2.0);
    append_ok(ItemId{2}, 3.0);
    catalog_.reset();
    {
        std::unique_ptr<detail::Connection> raw = raw_connection(path_);
        ASSERT_NE(raw, nullptr);
        LUCENT_ASSERT_OK(raw->exec("PRAGMA foreign_keys = ON", "test"));
        for (const char* change :
             {"UPDATE recipe_revisions SET settings_json = '{\"exposure_ev\":9}' WHERE id = 1",
              "UPDATE recipe_revisions SET note = 'edited' WHERE id = 1",
              "UPDATE recipe_revisions SET parent_revision_id = NULL WHERE id = 2",
              "DELETE FROM recipe_revisions WHERE id = 2", "DELETE FROM recipe_revisions",
              // The structure is enforced, not only by the repository code:
              "INSERT INTO recipe_revisions (item_id, revision_number, created_at, origin, schema_name, "
              "schema_version, settings_json, settings_sha256, engine_version) VALUES (1, 7, 0, 'user', 'x', "
              "1, "
              "'{}', zeroblob(32), '')",  // revision numbers skip
              "INSERT INTO recipe_revisions (item_id, revision_number, parent_revision_id, created_at, "
              "origin, "
              "schema_name, schema_version, settings_json, settings_sha256, engine_version) VALUES (2, 2, 1, "
              "0, "
              "'user', 'x', 1, '{}', zeroblob(32), '')",                     // parent of another item
              "UPDATE items SET current_revision_id = 1 WHERE id = 2",       // revision of another item
              "UPDATE items SET current_revision_id = 999 WHERE id = 1"}) {  // no such revision
            const Status refused = raw->exec(change, "test");
            EXPECT_FALSE(refused) << change;
        }
        EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM recipe_revisions"), 3);
        EXPECT_EQ(query_text(*raw, "SELECT settings_json FROM recipe_revisions WHERE id = 1"), exposure(1.0));
    }
    catalog_ = open_or_fail(path_);
    ASSERT_NE(catalog_, nullptr);
    EXPECT_EQ(current(ItemId{1}), b);

    // Removing an item removes its revisions; removing a source removes its items' revisions.
    LUCENT_ASSERT_OK(catalog_->write([](WriteTransaction& tx) {
        const ItemId items[] = {ItemId{1}};
        return tx.remove_items(items);
    }));
    Expected<std::optional<RecipeRevision>> gone =
        catalog_->query([&](ReadTransaction& tx) { return tx.find_revision(a); });
    LUCENT_ASSERT_OK(gone);
    EXPECT_FALSE(gone->has_value());
    LUCENT_ASSERT_OK(catalog_->write([](WriteTransaction& tx) { return tx.remove_source(SourceId{2}); }));
    catalog_.reset();
    std::unique_ptr<detail::Connection> raw = raw_connection(path_, detail::Access::ReadOnly);
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM recipe_revisions"), 0);
    EXPECT_EQ(query_int(*raw, "SELECT count(*) FROM pragma_foreign_key_check"), 0);
}

TEST_F(CatalogRevisions, ATamperedSettingsDocumentIsReportedAsCorrupt) {
    const RevisionId a = append_ok(ItemId{1}, 1.0);
    catalog_.reset();
    {
        std::unique_ptr<detail::Connection> raw = raw_connection(path_);
        ASSERT_NE(raw, nullptr);
        LUCENT_ASSERT_OK(
            raw->exec("DROP TRIGGER recipe_revisions_are_immutable;"
                      "UPDATE recipe_revisions SET settings_json = '{\"exposure_ev\":5}'",
                      "test"));
    }
    catalog_ = open_or_fail(path_);
    ASSERT_NE(catalog_, nullptr);
    Expected<std::optional<RecipeRevision>> read =
        catalog_->query([&](ReadTransaction& tx) { return tx.find_revision(a); });
    ASSERT_FALSE(read);
    EXPECT_EQ(read.error().code, ErrorCode::Corrupt) << read.error().describe();
    Expected<std::optional<RecipeRevision>> now =
        catalog_->query([](ReadTransaction& tx) { return tx.current_revision(ItemId{1}); });
    ASSERT_FALSE(now);
    EXPECT_EQ(now.error().code, ErrorCode::Corrupt);
    // The history (no documents) still lists it, so the user can go back to as shot.
    EXPECT_EQ(history(ItemId{1}).revisions.size(), 1u);
    LUCENT_EXPECT_OK(set_current(ItemId{1}, std::nullopt));
}

TEST_F(CatalogRevisions, InvalidRequestsChangeNothing) {
    const RevisionId a = append_ok(ItemId{1}, 1.0);
    const RevisionId other = append_ok(ItemId{2}, 1.0);
    const auto expect_refused = [&](const NewRevision& r, ErrorCode code, ItemId item = ItemId{1}) {
        Expected<RevisionInfo> info = append(item, r);
        ASSERT_FALSE(info);
        EXPECT_EQ(info.error().code, code) << info.error().describe();
    };
    expect_refused(revision(exposure(1)), ErrorCode::NotFound, ItemId{999});
    expect_refused(revision("{\"exposure_ev\":"), ErrorCode::InvalidArgument);  // not JSON
    expect_refused(revision("not json"), ErrorCode::InvalidArgument);
    NewRevision r = revision("{}");
    r.schema_name.clear();
    expect_refused(r, ErrorCode::InvalidArgument);
    r = revision("{}");
    r.schema_name = std::string(kMaxRevisionLabelBytes + 1, 'x');
    expect_refused(r, ErrorCode::InvalidArgument);
    r = revision("{}");
    r.schema_version = 0;
    expect_refused(r, ErrorCode::InvalidArgument);
    r = revision("{}");
    r.origin = static_cast<RevisionOrigin>(99);
    expect_refused(r, ErrorCode::InvalidArgument);
    r = revision("{}");
    r.note = "bad \xC3\x28 UTF-8";
    expect_refused(r, ErrorCode::InvalidArgument);
    r = revision("{\"name\":\"\xFF\"}");
    expect_refused(r, ErrorCode::InvalidArgument);
    r = revision("{}");
    r.engine_version = std::string(kMaxRevisionLabelBytes + 1, '1');
    expect_refused(r, ErrorCode::InvalidArgument);
    r = revision("{}");
    r.note = std::string(kMaxRevisionNoteBytes + 1, 'n');
    expect_refused(r, ErrorCode::LimitExceeded);
    r = revision("\"" + std::string(kMaxRevisionSettingsBytes - 1, 's') + "\"");  // one byte too many
    ASSERT_EQ(r.settings_json.size(), kMaxRevisionSettingsBytes + 1);
    expect_refused(r, ErrorCode::LimitExceeded);

    // Moving the pointer.
    Status moved = set_current(ItemId{1}, other);
    ASSERT_FALSE(moved);
    EXPECT_EQ(moved.error().code, ErrorCode::InvalidArgument);
    moved = set_current(ItemId{1}, RevisionId{424242});
    ASSERT_FALSE(moved);
    EXPECT_EQ(moved.error().code, ErrorCode::NotFound);
    moved = set_current(ItemId{999}, std::nullopt);
    ASSERT_FALSE(moved);
    EXPECT_EQ(moved.error().code, ErrorCode::NotFound);
    EXPECT_EQ(undo(ItemId{999}).error().code, ErrorCode::NotFound);
    EXPECT_EQ(redo(ItemId{999}).error().code, ErrorCode::NotFound);
    Expected<RevisionHistory> missing =
        catalog_->query([](ReadTransaction& tx) { return tx.revision_history(ItemId{999}); });
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);

    const RevisionHistory h = history(ItemId{1});
    EXPECT_EQ(h.current, a);
    ASSERT_EQ(h.revisions.size(), 1u);
}

TEST_F(CatalogRevisions, ARolledBackWriteLeavesNoRevision) {
    const RevisionId a = append_ok(ItemId{1}, 1.0);
    const Status status = catalog_->write([&](WriteTransaction& tx) -> Status {
        if (Expected<RevisionInfo> b = tx.append_revision(ItemId{1}, revision(exposure(2))); !b) {
            return Unexpected(b.error());
        }
        return fail(ErrorCode::Cancelled, "the user closed the editor", "test");
    });
    ASSERT_FALSE(status);
    const RevisionHistory h = history(ItemId{1});
    EXPECT_EQ(h.current, a);
    EXPECT_EQ(h.revisions.size(), 1u);
    // The number is not burnt either.
    Expected<RevisionInfo> b = append(ItemId{1}, revision(exposure(2)));
    LUCENT_ASSERT_OK(b);
    EXPECT_EQ(b->revision_number, 2u);
}

TEST_F(CatalogRevisions, UnicodeNotesAndSettingsRoundTripByteForByte) {
    const std::string note =
        "Ретушь · 写真の調整 · é\xCC\x81 · \xF0\x9F\x93\xB7 · tab\there";  // NFD é, emoji
    const std::string settings = R"({"look":"ネガ風 ✓","exposure_ev":-0.3,"tone_curve":[[0,0],[1,1]]})";
    NewRevision r = revision(settings, RevisionOrigin::Paste, note);
    r.engine_version = "0.6.0-β.1";
    Expected<RevisionInfo> info = append(ItemId{3}, r);
    LUCENT_ASSERT_OK(info);
    catalog_.reset();
    catalog_ = open_or_fail(path_);
    ASSERT_NE(catalog_, nullptr);
    Expected<std::optional<RecipeRevision>> read =
        catalog_->query([&](ReadTransaction& tx) { return tx.find_revision(info->id); });
    LUCENT_ASSERT_OK(read);
    ASSERT_TRUE(read->has_value());
    EXPECT_EQ((*read)->info.note, note);
    EXPECT_EQ((*read)->settings_json, settings);
    EXPECT_EQ((*read)->info.engine_version, "0.6.0-β.1");
    EXPECT_EQ((*read)->info.origin, RevisionOrigin::Paste);
    EXPECT_EQ(history(ItemId{3}).revisions.front().note, note);
}

TEST_F(CatalogRevisions, LargeSettingsDocuments) {
    // An 8 MiB document (a recipe with many brush strokes) and one exactly at the limit.
    std::string big = "{\"strokes\":[";
    for (int i = 0; big.size() < (std::size_t{8} << 20); ++i) {
        big += std::format("{}[{},{},{}]", i == 0 ? "" : ",", i, i * 0.5, -i);
    }
    big += "]}";
    const std::string at_limit = "\"" + std::string(kMaxRevisionSettingsBytes - 2, 'a') + "\"";
    ASSERT_EQ(at_limit.size(), kMaxRevisionSettingsBytes);

    Expected<RevisionInfo> first = append(ItemId{1}, revision(big));
    LUCENT_ASSERT_OK(first);
    EXPECT_EQ(first->settings_bytes, big.size());
    Expected<RevisionInfo> second = append(ItemId{1}, revision(at_limit));
    LUCENT_ASSERT_OK(second);

    Expected<core::Digest256> expected = core::sha256(std::as_bytes(std::span(big.data(), big.size())));
    LUCENT_ASSERT_OK(expected);
    EXPECT_EQ(first->settings_sha256, *expected);

    const RevisionHistory h = history(ItemId{1});
    ASSERT_EQ(h.revisions.size(), 2u);
    EXPECT_EQ(h.revisions[0].settings_bytes, big.size());
    EXPECT_EQ(h.revisions[1].settings_bytes, at_limit.size());
    for (const auto& [id, text] :
         {std::pair<RevisionId, const std::string*>{first->id, &big}, {second->id, &at_limit}}) {
        Expected<std::optional<RecipeRevision>> read =
            catalog_->query([&](ReadTransaction& tx) { return tx.find_revision(id); });
        LUCENT_ASSERT_OK(read);
        ASSERT_TRUE(read->has_value());
        EXPECT_TRUE((*read)->settings_json == *text);
    }
}

// Readers run concurrently with a writer that appends, undoes and branches. Every read
// transaction must see one consistent committed state: numbers 1..n without gaps, parents earlier
// in the same item, the current pointer inside the history, and current_revision() agreeing with
// the history read in the same transaction.
TEST_F(CatalogRevisions, ReadersSeeConsistentHistoriesWhileTheWriterEdits) {
    constexpr int kEdits = 150;
    std::atomic<bool> done{false};
    std::atomic<int> reads{0};
    std::atomic<int> problems{0};
    std::mutex message_mutex;
    std::string first_problem;
    const auto report = [&](std::string message) {
        if (problems.fetch_add(1) == 0) {
            std::lock_guard lock(message_mutex);
            first_problem = std::move(message);
        }
    };

    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&] {
            while (!done.load()) {
                const Status status = catalog_->read([&](ReadTransaction& tx) -> Status {
                    Expected<RevisionHistory> h = tx.revision_history(ItemId{1});
                    if (!h) {
                        return Unexpected(h.error());
                    }
                    Expected<std::optional<RecipeRevision>> now = tx.current_revision(ItemId{1});
                    if (!now) {
                        return Unexpected(now.error());
                    }
                    for (std::size_t i = 0; i < h->revisions.size(); ++i) {
                        const RevisionInfo& info = h->revisions[i];
                        if (info.revision_number != i + 1) {
                            report(std::format("gap at {}", i));
                        }
                        if (info.parent && (info.parent->value >= info.id.value)) {
                            report("parent after child");
                        }
                    }
                    if (h->current.has_value() != now->has_value() ||
                        (h->current && (*now)->info.id != *h->current)) {
                        report("current pointer and current revision disagree");
                    }
                    if (h->current) {
                        bool found = false;
                        for (const RevisionInfo& info : h->revisions) {
                            found = found || info.id == *h->current;
                        }
                        if (!found) {
                            report("current revision not in the history");
                        }
                    }
                    return ok();
                });
                if (!status) {
                    report(status.error().describe());
                }
                reads.fetch_add(1);
            }
        });
    }

    for (int i = 0; i < kEdits; ++i) {
        if (i % 5 == 4) {
            LUCENT_EXPECT_OK(undo(ItemId{1}));  // the next edit branches
        } else {
            append_ok(ItemId{1}, i * 0.01);
        }
    }
    // Let every reader finish at least a few reads after the last commit.
    const int seen = reads.load();
    while (reads.load() < seen + 6) {
        std::this_thread::yield();
    }
    done.store(true);
    for (std::thread& t : readers) {
        t.join();
    }
    EXPECT_EQ(problems.load(), 0) << first_problem;
    EXPECT_EQ(history(ItemId{1}).revisions.size(), static_cast<std::size_t>(kEdits - kEdits / 5));
}

// A read transaction keeps its snapshot while the writer commits a new revision.
TEST_F(CatalogRevisions, AReaderKeepsItsSnapshotDuringAnAppend) {
    append_ok(ItemId{1}, 1.0);
    std::mutex mutex;
    std::condition_variable changed;
    bool reader_ready = false;
    bool writer_done = false;
    std::size_t before = 0;
    std::size_t during = 0;
    std::optional<RevisionId> current_during;
    std::thread reader([&] {
        const Status status = catalog_->read([&](ReadTransaction& tx) -> Status {
            Expected<RevisionHistory> h = tx.revision_history(ItemId{1});
            if (!h) {
                return Unexpected(h.error());
            }
            before = h->revisions.size();
            {
                std::unique_lock lock(mutex);
                reader_ready = true;
                changed.notify_all();
                changed.wait(lock, [&] { return writer_done; });
            }
            Expected<RevisionHistory> again = tx.revision_history(ItemId{1});
            if (!again) {
                return Unexpected(again.error());
            }
            during = again->revisions.size();
            current_during = again->current;
            return ok();
        });
        EXPECT_TRUE(status) << describe(status);
    });
    RevisionId added;
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return reader_ready; });
    }
    added = append_ok(ItemId{1}, 2.0);  // the writer is not blocked by the reader
    {
        std::lock_guard lock(mutex);
        writer_done = true;
    }
    changed.notify_all();
    reader.join();
    EXPECT_EQ(before, 1u);
    EXPECT_EQ(during, 1u);
    EXPECT_NE(current_during, added);
    EXPECT_EQ(history(ItemId{1}).revisions.size(), 2u);
}

TEST_F(CatalogRevisions, AReadOnlyCatalogRefusesRevisionWrites) {
    append_ok(ItemId{1}, 1.0);
    catalog_.reset();
    OpenOptions options = options_for(path_);
    options.mode = OpenMode::ReadOnly;
    Expected<std::unique_ptr<Catalog>> reader = Catalog::open(options);
    LUCENT_ASSERT_OK(reader);
    Expected<RevisionHistory> h =
        (*reader)->query([](ReadTransaction& tx) { return tx.revision_history(ItemId{1}); });
    LUCENT_ASSERT_OK(h);
    EXPECT_EQ(h->revisions.size(), 1u);
    const Status write =
        (*reader)->write([](WriteTransaction& tx) { return tx.undo_revision(ItemId{1}).map([](auto&&) {}); });
    ASSERT_FALSE(write);
    EXPECT_EQ(write.error().code, ErrorCode::PermissionDenied);
}

// ---------------------------------------------------------------------------------------------
// Capture fields (schema v2)
// ---------------------------------------------------------------------------------------------

TEST_F(CatalogRevisions, CaptureFieldsRoundTripAndCanBeCleared) {
    ItemMetadata probed = item(1);
    probed.iso = 12800;
    probed.exposure_time_s = 1.0 / 8000;
    probed.f_number = 1.4;
    probed.focal_length_mm = 35.0;
    probed.image_count = 2;
    Expected<std::vector<ItemId>> ids = catalog_->transact(
        [&](WriteTransaction& tx) { return tx.upsert_items(SourceId{1}, std::span(&probed, 1)); });
    LUCENT_ASSERT_OK(ids);
    const auto read = [&](ItemId id) {
        Expected<std::optional<ItemRecord>> r =
            catalog_->query([&](ReadTransaction& tx) { return tx.find_item(id); });
        EXPECT_TRUE(r && r->has_value()) << describe(r);
        return r && r->has_value() ? (*r)->metadata : ItemMetadata{};
    };
    ItemMetadata stored = read(ids->front());
    EXPECT_EQ(stored.iso, 12800.0);
    EXPECT_EQ(stored.exposure_time_s, 1.0 / 8000);  // REAL is an IEEE double: exact round trip
    EXPECT_EQ(stored.f_number, 1.4);
    EXPECT_EQ(stored.focal_length_mm, 35.0);
    EXPECT_EQ(stored.image_count, 2u);
    EXPECT_EQ(stored.subimage_index, 1u);

    // A re-probe replaces them, including with "unknown".
    ItemMetadata reprobed = item(1);
    reprobed.f_number = 2.8;
    LUCENT_ASSERT_OK(catalog_->transact(
        [&](WriteTransaction& tx) { return tx.upsert_items(SourceId{1}, std::span(&reprobed, 1)); }));
    stored = read(ids->front());
    EXPECT_FALSE(stored.iso.has_value());
    EXPECT_FALSE(stored.exposure_time_s.has_value());
    EXPECT_EQ(stored.f_number, 2.8);
    EXPECT_FALSE(stored.focal_length_mm.has_value());
    EXPECT_FALSE(stored.image_count.has_value());
}

TEST_F(CatalogRevisions, InvalidCaptureFieldsAreRejected) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<ItemMetadata> bad;
    for (const double value : {0.0, -1.0, nan, inf, -inf}) {
        for (int field = 0; field < 4; ++field) {
            ItemMetadata m = item();
            std::optional<double>* const fields[] = {&m.iso, &m.exposure_time_s, &m.f_number,
                                                     &m.focal_length_mm};
            *fields[field] = value;
            bad.push_back(m);
        }
    }
    ItemMetadata zero_images = item();
    zero_images.image_count = 0;
    bad.push_back(zero_images);
    ItemMetadata beyond = item(2);
    beyond.image_count = 2;  // subimage 2 of a file with two images (0, 1)
    bad.push_back(beyond);
    for (std::size_t i = 0; i < bad.size(); ++i) {
        Expected<std::vector<ItemId>> ids = catalog_->transact(
            [&](WriteTransaction& tx) { return tx.upsert_items(SourceId{2}, std::span(&bad[i], 1)); });
        ASSERT_FALSE(ids) << i;
        EXPECT_EQ(ids.error().code, ErrorCode::InvalidArgument) << i;
    }
}

}  // namespace
}  // namespace lucent::catalog
