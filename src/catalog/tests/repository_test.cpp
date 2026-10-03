// Sources, items, flags, duplicates, relinking, listing and import records.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "lucent/catalog/location.hpp"

#include "catalog_test_support.hpp"

namespace lucent::catalog {
namespace {

using namespace test;

class CatalogRepository : public ::testing::Test {
protected:
    void SetUp() override {
        catalog_ = open_or_fail(dir_ / L"lucent.catalog");
        ASSERT_NE(catalog_, nullptr);
    }

    Expected<SourceUpsertResult> upsert(const SourceObservation& o) {
        return catalog_->transact([&](WriteTransaction& tx) { return tx.upsert_source(o); });
    }

    Expected<std::vector<ItemId>> upsert_items(SourceId source, std::vector<ItemMetadata> items) {
        return catalog_->transact([&](WriteTransaction& tx) { return tx.upsert_items(source, items); });
    }

    SourceRecord source(SourceId id) {
        Expected<std::optional<SourceRecord>> found =
            catalog_->query([&](ReadTransaction& tx) { return tx.find_source(id); });
        if (!found || !found->has_value()) {
            ADD_FAILURE() << "source " << id.value << " not found: " << describe(found);
            return {};
        }
        return **found;
    }

    ItemRecord item_record(ItemId id) {
        Expected<std::optional<ItemRecord>> found =
            catalog_->query([&](ReadTransaction& tx) { return tx.find_item(id); });
        if (!found || !found->has_value()) {
            ADD_FAILURE() << "item " << id.value << " not found: " << describe(found);
            return {};
        }
        return **found;
    }

    std::vector<ItemRecord> all_items(ItemQuery query) {
        std::vector<ItemRecord> items;
        for (int page = 0; page < 10'000; ++page) {
            Expected<ItemPage> result =
                catalog_->query([&](ReadTransaction& tx) { return tx.list_items(query); });
            if (!result) {
                ADD_FAILURE() << result.error().describe();
                return items;
            }
            items.insert(items.end(), result->items.begin(), result->items.end());
            if (!result->next) {
                return items;
            }
            query.after = result->next;
        }
        ADD_FAILURE() << "paging did not terminate";
        return items;
    }

    TempDir dir_;
    std::unique_ptr<Catalog> catalog_;
};

TEST_F(CatalogRepository, InsertsAndFindsASource) {
    SourceObservation o = observation(dir_ / L"Photos" / L"IMG_0001.CR3", 1);
    o.sha256 = digest(1);
    o.storage_kind = StorageKind::Removable;
    Expected<SourceUpsertResult> inserted = upsert(o);
    LUCENT_ASSERT_OK(inserted);
    EXPECT_EQ(inserted->outcome, SourceUpsertOutcome::Inserted);
    EXPECT_TRUE(inserted->id.valid());

    const SourceRecord stored = source(inserted->id);
    EXPECT_EQ(stored.path, o.path);
    EXPECT_EQ(stored.path_key, *make_path_key(o.path));
    EXPECT_EQ(stored.identity, o.identity);
    EXPECT_EQ(stored.sha256, o.sha256);
    EXPECT_EQ(stored.import_mode, ImportMode::Referenced);
    EXPECT_FALSE(stored.origin_path.has_value());
    EXPECT_EQ(stored.storage_kind, StorageKind::Removable);
    EXPECT_EQ(stored.state, SourceState::Present);
    EXPECT_EQ(stored.first_seen, o.seen_at);
    EXPECT_EQ(stored.last_seen, o.seen_at);

    Expected<std::optional<SourceRecord>> by_path =
        catalog_->query([&](ReadTransaction& tx) { return tx.find_source_by_path(o.path); });
    LUCENT_ASSERT_OK(by_path);
    ASSERT_TRUE(by_path->has_value());
    EXPECT_EQ((*by_path)->id, inserted->id);
    Expected<std::optional<SourceRecord>> absent = catalog_->query(
        [&](ReadTransaction& tx) { return tx.find_source_by_path(dir_ / L"Photos" / L"IMG_0002.CR3"); });
    LUCENT_ASSERT_OK(absent);
    EXPECT_FALSE(absent->has_value());
    Expected<std::optional<SourceRecord>> no_id =
        catalog_->query([&](ReadTransaction& tx) { return tx.find_source(SourceId{12345}); });
    LUCENT_ASSERT_OK(no_id);
    EXPECT_FALSE(no_id->has_value());
}

TEST_F(CatalogRepository, LargeUnsignedIdentityValuesRoundTrip) {
    SourceObservation o = observation(dir_ / L"big.dng", 1);
    o.identity.volume_serial = 0xFFFF'FFFF'FFFF'FFFEull;
    o.identity.size_bytes = 0x8000'0000'0000'0001ull;
    o.identity.file_id.fill(0xFF);
    o.identity.last_write_time = -5;
    Expected<SourceUpsertResult> inserted = upsert(o);
    LUCENT_ASSERT_OK(inserted);
    EXPECT_EQ(source(inserted->id).identity, o.identity);
    Expected<std::vector<SourceRecord>> by_id = catalog_->query([&](ReadTransaction& tx) {
        return tx.find_sources_by_file_id(o.identity.volume_serial, o.identity.file_id);
    });
    LUCENT_ASSERT_OK(by_id);
    ASSERT_EQ(by_id->size(), 1u);
}

TEST_F(CatalogRepository, UpsertOutcomesFollowTheFileIdentity) {
    SourceObservation o = observation(dir_ / L"IMG_0001.CR3", 1);
    o.sha256 = digest(1);
    const SourceId id = upsert(o)->id;

    // Same identity, no hash supplied: Unchanged, hash kept, last_seen advanced.
    SourceObservation again = o;
    again.sha256.reset();
    again.seen_at = o.seen_at + std::chrono::hours(1);
    Expected<SourceUpsertResult> unchanged = upsert(again);
    LUCENT_ASSERT_OK(unchanged);
    EXPECT_EQ(unchanged->id, id);
    EXPECT_EQ(unchanged->outcome, SourceUpsertOutcome::Unchanged);
    EXPECT_EQ(source(id).sha256, o.sha256);
    EXPECT_EQ(source(id).last_seen, again.seen_at);
    EXPECT_EQ(source(id).first_seen, o.seen_at);

    // Missing, then seen again unchanged: Restored.
    LUCENT_ASSERT_OK(catalog_->write(
        [&](WriteTransaction& tx) { return tx.set_source_state(id, SourceState::Missing, again.seen_at); }));
    Expected<SourceUpsertResult> restored = upsert(again);
    LUCENT_ASSERT_OK(restored);
    EXPECT_EQ(restored->outcome, SourceUpsertOutcome::Restored);
    EXPECT_EQ(source(id).state, SourceState::Present);

    // Modified file (new mtime and size): Replaced, stale hash cleared.
    SourceObservation modified = o;
    modified.sha256.reset();
    modified.identity.size_bytes += 10;
    modified.identity.last_write_time += 1;
    Expected<SourceUpsertResult> replaced = upsert(modified);
    LUCENT_ASSERT_OK(replaced);
    EXPECT_EQ(replaced->id, id);
    EXPECT_EQ(replaced->outcome, SourceUpsertOutcome::Replaced);
    EXPECT_EQ(source(id).identity, modified.identity);
    EXPECT_FALSE(source(id).sha256.has_value());

    // Same identity but a contradicting hash (a writer that preserved the timestamp): Replaced.
    LUCENT_ASSERT_OK(catalog_->write(
        [&](WriteTransaction& tx) { return tx.set_source_hash(id, digest(2), modified.identity); }));
    SourceObservation rewritten = modified;
    rewritten.sha256 = digest(3);
    Expected<SourceUpsertResult> contradicted = upsert(rewritten);
    LUCENT_ASSERT_OK(contradicted);
    EXPECT_EQ(contradicted->outcome, SourceUpsertOutcome::Replaced);
    EXPECT_EQ(source(id).sha256, digest(3));

    // A different file object at the same path (replaced by a copy): Replaced.
    SourceObservation other_file = rewritten;
    other_file.identity.file_id[0] ^= 0xFF;
    EXPECT_EQ(upsert(other_file)->outcome, SourceUpsertOutcome::Replaced);
}

TEST_F(CatalogRepository, CaseInsensitivePathKeysIdentifyOneSource) {
    const SourceObservation o = observation(dir_ / L"Photos" / L"IMG_0001.CR3", 1);
    const SourceId id = upsert(o)->id;
    SourceObservation respelled = o;
    respelled.path = dir_ / L"PHOTOS" / L"img_0001.cr3";
    Expected<SourceUpsertResult> same = upsert(respelled);
    LUCENT_ASSERT_OK(same);
    EXPECT_EQ(same->id, id);
    EXPECT_EQ(same->outcome, SourceUpsertOutcome::Unchanged);
    EXPECT_EQ(source(id).path, respelled.path);  // the latest on-disk spelling is kept
    Expected<std::optional<SourceRecord>> found = catalog_->query(
        [&](ReadTransaction& tx) { return tx.find_source_by_path(dir_ / L"photos" / L"Img_0001.Cr3"); });
    LUCENT_ASSERT_OK(found);
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ((*found)->id, id);
    Expected<std::uint64_t> count = catalog_->query([](ReadTransaction& tx) { return tx.count_sources(); });
    EXPECT_EQ(*count, 1u);
}

TEST_F(CatalogRepository, UnicodePathsRoundTripExactly) {
    const std::vector<std::filesystem::path> paths = {
        dir_ / L"Фото" / L"Снимок.NEF", dir_ / L"写真" / L"東京 📷.ARW", dir_ / L"e\u0301te\u0301" / L"a.dng",
        dir_ / L"\u00E9t\u00E9" / L"a.dng",  // NFC spelling of the previous folder: a different source
        dir_ / L"quote'semi;colon\"" / L"x.orf"};
    std::vector<SourceId> ids;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        SourceObservation o = observation(paths[i], i + 1);
        o.import_mode = ImportMode::Copied;
        o.origin_path = L"E:\\DCIM\\100CANON\\Ж_" + std::to_wstring(i) + L".CR3";
        Expected<SourceUpsertResult> inserted = upsert(o);
        LUCENT_ASSERT_OK(inserted);
        EXPECT_EQ(inserted->outcome, SourceUpsertOutcome::Inserted) << i;
        const SourceRecord stored = source(inserted->id);
        EXPECT_EQ(stored.path.native(), paths[i].native());
        EXPECT_EQ(stored.origin_path, o.origin_path);
        EXPECT_EQ(stored.import_mode, ImportMode::Copied);
        ids.push_back(inserted->id);
    }
    // Cyrillic case folding finds the first source under another spelling.
    Expected<std::optional<SourceRecord>> folded = catalog_->query(
        [&](ReadTransaction& tx) { return tx.find_source_by_path(dir_ / L"ФОТО" / L"снимок.nef"); });
    LUCENT_ASSERT_OK(folded);
    ASSERT_TRUE(folded->has_value());
    EXPECT_EQ((*folded)->id, ids[0]);
}

TEST_F(CatalogRepository, OriginPathRules) {
    SourceObservation copied = observation(dir_ / L"a.cr3", 1);
    copied.import_mode = ImportMode::Copied;
    Expected<SourceUpsertResult> missing_origin = upsert(copied);
    ASSERT_FALSE(missing_origin);
    EXPECT_EQ(missing_origin.error().code, ErrorCode::InvalidArgument);
    SourceObservation referenced = observation(dir_ / L"b.cr3", 2);
    referenced.origin_path = L"E:\\x.cr3";
    Expected<SourceUpsertResult> stray_origin = upsert(referenced);
    ASSERT_FALSE(stray_origin);
    EXPECT_EQ(stray_origin.error().code, ErrorCode::InvalidArgument);
    Expected<SourceUpsertResult> relative = upsert(observation(L"relative\\c.cr3", 3));
    ASSERT_FALSE(relative);
    EXPECT_EQ(relative.error().code, ErrorCode::InvalidArgument);
    SourceObservation bad_kind = observation(dir_ / L"d.cr3", 4);
    bad_kind.storage_kind = static_cast<StorageKind>(42);
    EXPECT_FALSE(upsert(bad_kind));
}

TEST_F(CatalogRepository, CopiesKeepTheIdentityOfTheirOriginal) {
    SourceObservation copied = observation(dir_ / L"Pictures" / L"IMG_0001.CR3", 1);
    copied.import_mode = ImportMode::Copied;
    copied.origin_path = L"E:\\DCIM\\100CANON\\IMG_0001.CR3";
    copied.origin_identity = identity(77, 0xCAFE'F00Dull);
    copied.origin_identity->last_write_time = -3;  // any 64-bit value round-trips
    Expected<SourceUpsertResult> inserted = upsert(copied);
    LUCENT_ASSERT_OK(inserted);
    EXPECT_EQ(source(inserted->id).origin_identity, copied.origin_identity);
    EXPECT_EQ(source(inserted->id).origin_path, copied.origin_path);

    // A later observation replaces the origin identity, also with "unknown".
    SourceObservation again = copied;
    again.origin_identity.reset();
    LUCENT_ASSERT_OK(upsert(again));
    EXPECT_FALSE(source(inserted->id).origin_identity.has_value());

    // Referenced sources have no origin identity.
    SourceObservation referenced = observation(dir_ / L"b.cr3", 2);
    referenced.origin_identity = identity(3);
    Expected<SourceUpsertResult> refused = upsert(referenced);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::InvalidArgument);
    EXPECT_FALSE(source(inserted->id).origin_identity.has_value());
}

TEST_F(CatalogRepository, HashesBelongToTheHashedVersion) {
    const SourceObservation o = observation(dir_ / L"a.cr3", 1);
    const SourceId id = upsert(o)->id;
    LUCENT_ASSERT_OK(
        catalog_->write([&](WriteTransaction& tx) { return tx.set_source_hash(id, digest(9), o.identity); }));
    EXPECT_EQ(source(id).sha256, digest(9));

    core::FileIdentity newer = o.identity;
    newer.last_write_time += 1;
    const Status stale =
        catalog_->write([&](WriteTransaction& tx) { return tx.set_source_hash(id, digest(8), newer); });
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, ErrorCode::SourceChanged);
    EXPECT_EQ(source(id).sha256, digest(9));

    const Status missing = catalog_->write(
        [&](WriteTransaction& tx) { return tx.set_source_hash(SourceId{999}, digest(8), newer); });
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
}

TEST_F(CatalogRepository, SourceStatesAndLastSeen) {
    const SourceObservation o = observation(dir_ / L"a.cr3", 1);
    const SourceId id = upsert(o)->id;
    const Timestamp later = o.seen_at + std::chrono::hours(2);
    LUCENT_ASSERT_OK(catalog_->write(
        [&](WriteTransaction& tx) { return tx.set_source_state(id, SourceState::Missing, later); }));
    EXPECT_EQ(source(id).state, SourceState::Missing);
    EXPECT_EQ(source(id).last_seen, o.seen_at);  // last seen present, not last checked
    LUCENT_ASSERT_OK(catalog_->write(
        [&](WriteTransaction& tx) { return tx.set_source_state(id, SourceState::Changed, later); }));
    EXPECT_EQ(source(id).state, SourceState::Changed);
    LUCENT_ASSERT_OK(catalog_->write(
        [&](WriteTransaction& tx) { return tx.set_source_state(id, SourceState::Present, later); }));
    EXPECT_EQ(source(id).state, SourceState::Present);
    EXPECT_EQ(source(id).last_seen, later);

    const Status absent = catalog_->write(
        [&](WriteTransaction& tx) { return tx.set_source_state(SourceId{77}, SourceState::Missing, later); });
    ASSERT_FALSE(absent);
    EXPECT_EQ(absent.error().code, ErrorCode::NotFound);
    const Status invalid_state = catalog_->write(
        [&](WriteTransaction& tx) { return tx.set_source_state(id, static_cast<SourceState>(9), later); });
    ASSERT_FALSE(invalid_state);
    EXPECT_EQ(invalid_state.error().code, ErrorCode::InvalidArgument);

    Expected<std::vector<SourceRecord>> missing = catalog_->query([](ReadTransaction& tx) {
        return tx.list_sources(SourceQuery{{SourceState::Missing}, SourceId{}, 100});
    });
    LUCENT_ASSERT_OK(missing);
    EXPECT_TRUE(missing->empty());
}

TEST_F(CatalogRepository, DuplicateContentIsDetectedButKeptSeparate) {
    // Three files; two of them have identical content under different names.
    SourceObservation a = observation(dir_ / L"a.cr3", 1);
    SourceObservation b = observation(dir_ / L"copy of a.cr3", 2);
    SourceObservation c = observation(dir_ / L"c.cr3", 3);
    SourceObservation unhashed = observation(dir_ / L"d.cr3", 4);
    a.sha256 = digest(1);
    b.sha256 = digest(1);
    c.sha256 = digest(3);
    std::vector<SourceId> ids;
    for (const SourceObservation* o : {&a, &b, &c, &unhashed}) {
        Expected<SourceUpsertResult> r = upsert(*o);
        LUCENT_ASSERT_OK(r);
        EXPECT_EQ(r->outcome, SourceUpsertOutcome::Inserted);
        ids.push_back(r->id);
        LUCENT_ASSERT_OK(upsert_items(r->id, {item(0)}));
    }
    Expected<std::vector<SourceRecord>> same =
        catalog_->query([](ReadTransaction& tx) { return tx.find_sources_by_sha256(digest(1)); });
    LUCENT_ASSERT_OK(same);
    ASSERT_EQ(same->size(), 2u);
    EXPECT_EQ((*same)[0].id, ids[0]);
    EXPECT_EQ((*same)[1].id, ids[1]);

    ItemQuery query;
    query.filter.duplicates_only = true;
    const std::vector<ItemRecord> duplicates = all_items(query);
    ASSERT_EQ(duplicates.size(), 2u);
    std::set<SourceId> sources{duplicates[0].source_id, duplicates[1].source_id};
    EXPECT_EQ(sources, (std::set<SourceId>{ids[0], ids[1]}));
    Expected<std::uint64_t> count =
        catalog_->query([&](ReadTransaction& tx) { return tx.count_items(query.filter); });
    EXPECT_EQ(*count, 2u);
}

TEST_F(CatalogRepository, HardLinksShareAFileId) {
    SourceObservation a = observation(dir_ / L"a.cr3", 1);
    SourceObservation b = observation(dir_ / L"link to a.cr3", 1);  // same identity
    const SourceId first = upsert(a)->id;
    const SourceId second = upsert(b)->id;
    EXPECT_NE(first, second);
    Expected<std::vector<SourceRecord>> linked = catalog_->query([&](ReadTransaction& tx) {
        return tx.find_sources_by_file_id(a.identity.volume_serial, a.identity.file_id);
    });
    LUCENT_ASSERT_OK(linked);
    ASSERT_EQ(linked->size(), 2u);
    EXPECT_EQ((*linked)[0].id, first);
    EXPECT_EQ((*linked)[1].id, second);
}

TEST_F(CatalogRepository, RelinkMovesAVerifiedSourceAndKeepsItsItems) {
    SourceObservation o = observation(dir_ / L"card" / L"IMG_0001.CR3", 1);
    o.sha256 = digest(1);
    const SourceId id = upsert(o)->id;
    const std::vector<ItemId> items = *upsert_items(id, {item(0)});
    LUCENT_ASSERT_OK(catalog_->write(
        [&](WriteTransaction& tx) { return tx.set_item_flags(items[0], ItemFlags{PickFlag::Picked, 4}); }));
    LUCENT_ASSERT_OK(catalog_->write(
        [&](WriteTransaction& tx) { return tx.set_source_state(id, SourceState::Missing, o.seen_at); }));

    RelinkRequest request;
    request.new_path = dir_ / L"archive" / L"2026" / L"IMG_0001.CR3";
    request.identity = identity(500, 0x42);
    request.sha256 = digest(1);
    request.storage_kind = StorageKind::Fixed;
    request.seen_at = o.seen_at + std::chrono::hours(24);

    // Wrong content.
    RelinkRequest wrong = request;
    wrong.sha256 = digest(2);
    Expected<SourceRecord> mismatch =
        catalog_->transact([&](WriteTransaction& tx) { return tx.relink_source(id, wrong); });
    ASSERT_FALSE(mismatch);
    EXPECT_EQ(mismatch.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(mismatch.error().where, "catalog.relink.content_mismatch");
    EXPECT_EQ(source(id).path, o.path);

    // Target path already belongs to another source.
    const SourceId other = upsert(observation(dir_ / L"other.cr3", 7))->id;
    RelinkRequest clash = request;
    clash.new_path = dir_ / L"OTHER.cr3";
    Expected<SourceRecord> taken =
        catalog_->transact([&](WriteTransaction& tx) { return tx.relink_source(id, clash); });
    ASSERT_FALSE(taken);
    EXPECT_EQ(taken.error().code, ErrorCode::AlreadyExists);
    (void)other;

    Expected<SourceRecord> relinked =
        catalog_->transact([&](WriteTransaction& tx) { return tx.relink_source(id, request); });
    LUCENT_ASSERT_OK(relinked);
    EXPECT_EQ(relinked->id, id);
    EXPECT_EQ(relinked->path, request.new_path);
    EXPECT_EQ(relinked->path_key, *make_path_key(request.new_path));
    EXPECT_EQ(relinked->identity, request.identity);
    EXPECT_EQ(relinked->state, SourceState::Present);
    EXPECT_EQ(relinked->storage_kind, StorageKind::Fixed);
    EXPECT_EQ(relinked->last_seen, request.seen_at);
    EXPECT_EQ(relinked->first_seen, o.seen_at);
    const ItemRecord kept = item_record(items[0]);
    EXPECT_EQ(kept.source_id, id);
    EXPECT_EQ(kept.flags, (ItemFlags{PickFlag::Picked, 4}));
    EXPECT_EQ(kept.source_path, request.new_path);
    Expected<std::optional<SourceRecord>> old_path =
        catalog_->query([&](ReadTransaction& tx) { return tx.find_source_by_path(o.path); });
    EXPECT_FALSE(old_path->has_value());

    Expected<SourceRecord> missing =
        catalog_->transact([&](WriteTransaction& tx) { return tx.relink_source(SourceId{4242}, request); });
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
}

TEST_F(CatalogRepository, RelinkingAnUnhashedSourceNeedsExplicitConsent) {
    const SourceId id = upsert(observation(dir_ / L"a.cr3", 1))->id;
    RelinkRequest request;
    request.new_path = dir_ / L"moved" / L"a.cr3";
    request.identity = identity(2);
    request.sha256 = digest(5);
    request.storage_kind = StorageKind::Fixed;
    Expected<SourceRecord> refused =
        catalog_->transact([&](WriteTransaction& tx) { return tx.relink_source(id, request); });
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().where, "catalog.relink.unverified");
    request.allow_unverified = true;
    Expected<SourceRecord> accepted =
        catalog_->transact([&](WriteTransaction& tx) { return tx.relink_source(id, request); });
    LUCENT_ASSERT_OK(accepted);
    EXPECT_EQ(accepted->sha256, digest(5));
}

TEST_F(CatalogRepository, TheSameFileVersionRelinksWithoutAHash) {
    // A source that was never hashed: its own file version (same volume serial, file id, size and
    // last write time) found under another path, e.g. a card under another drive letter.
    SourceObservation o = observation(dir_ / L"E" / L"IMG_0001.CR3", 1);
    const SourceId id = upsert(o)->id;
    LUCENT_ASSERT_OK(catalog_->write(
        [&](WriteTransaction& tx) { return tx.set_source_state(id, SourceState::Missing, o.seen_at); }));
    RelinkRequest request;
    request.new_path = dir_ / L"F" / L"IMG_0001.CR3";
    request.identity = o.identity;
    request.storage_kind = StorageKind::Removable;
    request.seen_at = o.seen_at + std::chrono::hours(1);
    Expected<SourceRecord> moved =
        catalog_->transact([&](WriteTransaction& tx) { return tx.relink_source(id, request); });
    LUCENT_ASSERT_OK(moved);
    EXPECT_EQ(moved->path, request.new_path);
    EXPECT_EQ(moved->state, SourceState::Present);
    EXPECT_FALSE(moved->sha256.has_value());

    // Another version without a hash cannot be verified.
    RelinkRequest other = request;
    other.new_path = dir_ / L"G" / L"IMG_0001.CR3";
    other.identity.last_write_time += 1;
    Expected<SourceRecord> refused =
        catalog_->transact([&](WriteTransaction& tx) { return tx.relink_source(id, other); });
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().where, "catalog.relink.unverified");

    // A hashed source keeps its hash on an identity-verified move, and a contradicting hash is
    // refused even for the same identity.
    LUCENT_ASSERT_OK(
        catalog_->write([&](WriteTransaction& tx) { return tx.set_source_hash(id, digest(4), o.identity); }));
    request.new_path = dir_ / L"H" / L"IMG_0001.CR3";
    Expected<SourceRecord> kept =
        catalog_->transact([&](WriteTransaction& tx) { return tx.relink_source(id, request); });
    LUCENT_ASSERT_OK(kept);
    EXPECT_EQ(kept->sha256, digest(4));
    RelinkRequest contradicting = request;
    contradicting.new_path = dir_ / L"I" / L"IMG_0001.CR3";
    contradicting.sha256 = digest(5);
    contradicting.allow_unverified = true;
    Expected<SourceRecord> mismatch =
        catalog_->transact([&](WriteTransaction& tx) { return tx.relink_source(id, contradicting); });
    ASSERT_FALSE(mismatch);
    EXPECT_EQ(mismatch.error().where, "catalog.relink.content_mismatch");
    EXPECT_EQ(source(id).path, request.new_path);
}

TEST_F(CatalogRepository, ItemsUpsertBySubimageAndKeepFlags) {
    const SourceId id = upsert(observation(dir_ / L"burst.cr3", 1))->id;
    Expected<std::vector<ItemId>> first = upsert_items(id, {item(0), item(1, "")});
    LUCENT_ASSERT_OK(first);
    ASSERT_EQ(first->size(), 2u);
    LUCENT_ASSERT_OK(catalog_->write([&](WriteTransaction& tx) {
        return tx.set_item_flags((*first)[1], ItemFlags{PickFlag::Rejected, 2});
    }));

    ItemMetadata reprobed = item(1, "2026-09-02T11:12:13");
    reprobed.support_status = SupportStatus::Limited;
    reprobed.support_reasons_json = R"([{"kind":"optional_operation","message":"opcode skipped"}])";
    reprobed.orientation = 6;
    reprobed.decoder_version = "0.23.0";
    Expected<std::vector<ItemId>> second = upsert_items(id, {reprobed, item(2)});
    LUCENT_ASSERT_OK(second);
    ASSERT_EQ(second->size(), 2u);
    EXPECT_EQ((*second)[0], (*first)[1]);  // same subimage, same item

    const ItemRecord updated = item_record((*first)[1]);
    EXPECT_EQ(updated.metadata.capture_time, reprobed.capture_time);
    EXPECT_EQ(updated.metadata.support_status, SupportStatus::Limited);
    EXPECT_EQ(updated.metadata.support_reasons_json, reprobed.support_reasons_json);
    EXPECT_EQ(updated.metadata.orientation, 6);
    EXPECT_EQ(updated.metadata.decoder_version, "0.23.0");
    EXPECT_EQ(updated.flags, (ItemFlags{PickFlag::Rejected, 2}));  // kept
    const ItemRecord untouched = item_record((*first)[0]);
    EXPECT_EQ(untouched.metadata.make, "Canon");
    EXPECT_EQ(untouched.metadata.width, 8192u);
    EXPECT_EQ(untouched.metadata.height, 5464u);
    EXPECT_EQ(untouched.flags, ItemFlags{});
    EXPECT_EQ(untouched.source_state, SourceState::Present);
    EXPECT_EQ(untouched.storage_kind, StorageKind::Fixed);
    EXPECT_FALSE(untouched.source_hashed);

    Expected<std::vector<ItemRecord>> of_source =
        catalog_->query([&](ReadTransaction& tx) { return tx.items_of_source(id); });
    LUCENT_ASSERT_OK(of_source);
    ASSERT_EQ(of_source->size(), 3u);
    EXPECT_EQ((*of_source)[2].metadata.subimage_index, 2u);
}

TEST_F(CatalogRepository, InvalidItemsAreRejectedWithoutPartialWrites) {
    const SourceId id = upsert(observation(dir_ / L"a.cr3", 1))->id;
    const auto expect_rejected = [&](std::vector<ItemMetadata> items, ErrorCode code) {
        Expected<std::vector<ItemId>> result = upsert_items(id, std::move(items));
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, code) << result.error().describe();
    };
    expect_rejected({item(0), item(0)}, ErrorCode::InvalidArgument);  // repeated subimage
    ItemMetadata bad = item(1);
    bad.orientation = 9;
    expect_rejected({item(0), bad}, ErrorCode::InvalidArgument);
    bad = item(1);
    bad.capture_time = "2026-13-01T00:00:00";
    expect_rejected({item(0), bad}, ErrorCode::InvalidArgument);
    bad = item(1);
    bad.capture_time = "2026:09:01 10:00:00";  // EXIF spelling is not accepted
    expect_rejected({item(0), bad}, ErrorCode::InvalidArgument);
    bad = item(1);
    bad.make = "\xC3\x28";  // malformed UTF-8
    expect_rejected({item(0), bad}, ErrorCode::InvalidArgument);
    bad = item(1);
    bad.support_status = static_cast<SupportStatus>(0);
    expect_rejected({item(0), bad}, ErrorCode::InvalidArgument);
    // Invalid JSON is caught by the CHECK constraint after item 0 was written: the savepoint
    // removes item 0 again.
    bad = item(1);
    bad.support_reasons_json = "[not json";
    expect_rejected({item(0), bad}, ErrorCode::InvalidArgument);
    Expected<std::vector<ItemId>> missing_source = catalog_->transact(
        [&](WriteTransaction& tx) { return tx.upsert_items(SourceId{999}, std::vector{item(0)}); });
    ASSERT_FALSE(missing_source);
    EXPECT_EQ(missing_source.error().code, ErrorCode::NotFound);

    Expected<std::uint64_t> count = catalog_->query([](ReadTransaction& tx) { return tx.count_items({}); });
    LUCENT_ASSERT_OK(count);
    EXPECT_EQ(*count, 0u);
}

TEST_F(CatalogRepository, FlagsAreValidated) {
    const SourceId id = upsert(observation(dir_ / L"a.cr3", 1))->id;
    const ItemId item_id = upsert_items(id, {item(0)})->front();
    for (const ItemFlags bad : {ItemFlags{PickFlag::None, 6}, ItemFlags{static_cast<PickFlag>(3), 0}}) {
        const Status status =
            catalog_->write([&](WriteTransaction& tx) { return tx.set_item_flags(item_id, bad); });
        ASSERT_FALSE(status);
        EXPECT_EQ(status.error().code, ErrorCode::InvalidArgument);
    }
    const Status missing =
        catalog_->write([&](WriteTransaction& tx) { return tx.set_item_flags(ItemId{404}, ItemFlags{}); });
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
}

TEST_F(CatalogRepository, RemovingItemsIsAllOrNothingAndSourcesCascade) {
    const SourceId a = upsert(observation(dir_ / L"a.cr3", 1))->id;
    const SourceId b = upsert(observation(dir_ / L"b.cr3", 2))->id;
    const std::vector<ItemId> a_items = *upsert_items(a, {item(0), item(1)});
    const std::vector<ItemId> b_items = *upsert_items(b, {item(0)});

    const std::vector<ItemId> with_missing = {a_items[0], ItemId{9999}};
    const Status partial =
        catalog_->write([&](WriteTransaction& tx) { return tx.remove_items(with_missing); });
    ASSERT_FALSE(partial);
    EXPECT_EQ(partial.error().code, ErrorCode::NotFound);
    EXPECT_EQ(*catalog_->query([](ReadTransaction& tx) { return tx.count_items({}); }), 3u);

    const std::vector<ItemId> one = {a_items[0]};
    LUCENT_ASSERT_OK(catalog_->write([&](WriteTransaction& tx) { return tx.remove_items(one); }));
    EXPECT_EQ(*catalog_->query([](ReadTransaction& tx) { return tx.count_items({}); }), 2u);

    LUCENT_ASSERT_OK(catalog_->write([&](WriteTransaction& tx) { return tx.remove_source(a); }));
    EXPECT_EQ(*catalog_->query([](ReadTransaction& tx) { return tx.count_items({}); }), 1u);
    EXPECT_EQ(*catalog_->query([](ReadTransaction& tx) { return tx.count_sources(); }), 1u);
    const Status again = catalog_->write([&](WriteTransaction& tx) { return tx.remove_source(a); });
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, ErrorCode::NotFound);
    (void)b_items;
}

TEST_F(CatalogRepository, ListingIsStableAcrossPagesForEveryOrder) {
    // 23 sources in shuffled insertion order, some with two subimages and some without capture time.
    std::vector<int> order(23);
    for (int i = 0; i < 23; ++i) {
        order[static_cast<std::size_t>(i)] = (i * 7) % 23;
    }
    for (const int n : order) {
        const std::filesystem::path path =
            dir_ / std::format(L"folder{}", n % 3) / std::format(L"IMG_{:04}.CR3", n);
        const SourceId id = upsert(observation(path, static_cast<std::uint64_t>(n) + 1))->id;
        std::vector<ItemMetadata> items = {
            item(0, n % 5 == 0 ? "" : std::format("2026-09-{:02}T08:00:00", 1 + n % 4))};
        if (n % 4 == 0) {
            items.push_back(item(1, "2026-09-01T08:00:00"));
        }
        LUCENT_ASSERT_OK(upsert_items(id, items));
    }
    Expected<std::uint64_t> total = catalog_->query([](ReadTransaction& tx) { return tx.count_items({}); });
    LUCENT_ASSERT_OK(total);
    ASSERT_EQ(*total, 29u);

    for (const ItemOrder ordering : {ItemOrder::Path, ItemOrder::CaptureTime, ItemOrder::Added}) {
        ItemQuery everything;
        everything.order = ordering;
        everything.limit = kMaxPageSize;
        const std::vector<ItemRecord> reference = all_items(everything);
        ASSERT_EQ(reference.size(), 29u);
        for (const std::uint32_t page_size : {1u, 4u, 7u, 29u, 30u}) {
            ItemQuery paged = everything;
            paged.limit = page_size;
            const std::vector<ItemRecord> pages = all_items(paged);
            ASSERT_EQ(pages.size(), reference.size()) << static_cast<int>(ordering) << " / " << page_size;
            for (std::size_t i = 0; i < pages.size(); ++i) {
                EXPECT_EQ(pages[i].id, reference[i].id)
                    << static_cast<int>(ordering) << " / " << page_size << " @" << i;
            }
        }
        // The reference order itself is the documented one.
        for (std::size_t i = 1; i < reference.size(); ++i) {
            const ItemRecord& a = reference[i - 1];
            const ItemRecord& b = reference[i];
            switch (ordering) {
                case ItemOrder::Path: {
                    const auto ka = std::make_tuple(*make_path_key(a.source_path), a.metadata.subimage_index);
                    const auto kb = std::make_tuple(*make_path_key(b.source_path), b.metadata.subimage_index);
                    EXPECT_LT(ka, kb);
                    break;
                }
                case ItemOrder::CaptureTime: {
                    const auto ka =
                        std::make_tuple(!a.metadata.capture_time, a.metadata.capture_time.value_or(""), a.id);
                    const auto kb =
                        std::make_tuple(!b.metadata.capture_time, b.metadata.capture_time.value_or(""), b.id);
                    EXPECT_LT(ka, kb);
                    break;
                }
                case ItemOrder::Added:
                    EXPECT_LT(a.id, b.id);
                    break;
            }
        }
    }

    // A cursor of one order cannot continue another.
    ItemQuery by_path;
    by_path.limit = 3;
    Expected<ItemPage> first = catalog_->query([&](ReadTransaction& tx) { return tx.list_items(by_path); });
    LUCENT_ASSERT_OK(first);
    ASSERT_TRUE(first->next.has_value());
    ItemQuery mixed;
    mixed.order = ItemOrder::Added;
    mixed.after = first->next;
    Expected<ItemPage> refused = catalog_->query([&](ReadTransaction& tx) { return tx.list_items(mixed); });
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::InvalidArgument);
    ItemQuery zero;
    zero.limit = 0;
    EXPECT_FALSE(catalog_->query([&](ReadTransaction& tx) { return tx.list_items(zero); }));
}

TEST_F(CatalogRepository, PagingNeverRepeatsOrSkipsWhenRowsChangeBetweenPages) {
    for (int n = 0; n < 10; ++n) {
        const SourceId id =
            upsert(observation(dir_ / std::format(L"m{:02}.cr3", n), static_cast<std::uint64_t>(n) + 1))->id;
        LUCENT_ASSERT_OK(upsert_items(id, {item(0)}));
    }
    ItemQuery query;
    query.limit = 4;
    Expected<ItemPage> first = catalog_->query([&](ReadTransaction& tx) { return tx.list_items(query); });
    LUCENT_ASSERT_OK(first);
    ASSERT_EQ(first->items.size(), 4u);
    // Insert one item before the cursor and one after it, and remove the next unseen one.
    for (const wchar_t* name : {L"a00.cr3", L"z99.cr3"}) {
        const SourceId id = upsert(observation(dir_ / name, 100 + name[0]))->id;
        LUCENT_ASSERT_OK(upsert_items(id, {item(0)}));
    }
    const SourceId m04 = *catalog_->query([&](ReadTransaction& tx) -> Expected<SourceId> {
        Expected<std::optional<SourceRecord>> s = tx.find_source_by_path(dir_ / L"m04.cr3");
        if (!s) {
            return Unexpected(s.error());
        }
        return (*s)->id;
    });
    LUCENT_ASSERT_OK(catalog_->write([&](WriteTransaction& tx) { return tx.remove_source(m04); }));

    std::vector<std::wstring> names;
    for (const ItemRecord& r : first->items) {
        names.push_back(r.source_path.filename().native());
    }
    query.after = first->next;
    for (;;) {
        Expected<ItemPage> page = catalog_->query([&](ReadTransaction& tx) { return tx.list_items(query); });
        LUCENT_ASSERT_OK(page);
        for (const ItemRecord& r : page->items) {
            names.push_back(r.source_path.filename().native());
        }
        if (!page->next) {
            break;
        }
        query.after = page->next;
    }
    const std::vector<std::wstring> expected = {L"m00.cr3", L"m01.cr3", L"m02.cr3", L"m03.cr3", L"m05.cr3",
                                                L"m06.cr3", L"m07.cr3", L"m08.cr3", L"m09.cr3", L"z99.cr3"};
    EXPECT_EQ(names, expected);
}

TEST_F(CatalogRepository, FiltersByStateSupportAndFlags) {
    struct Row {
        const wchar_t* name;
        SourceState state;
        SupportStatus support;
        ItemFlags flags;
    };
    const Row rows[] = {
        {L"a.cr3", SourceState::Present, SupportStatus::Supported, {PickFlag::Picked, 5}},
        {L"b.cr3", SourceState::Missing, SupportStatus::Supported, {PickFlag::None, 3}},
        {L"c.cr3", SourceState::Changed, SupportStatus::Limited, {PickFlag::Rejected, 1}},
        {L"d.cr3", SourceState::Present, SupportStatus::Unsupported, {PickFlag::Picked, 0}},
    };
    std::uint64_t n = 1;
    for (const Row& row : rows) {
        const SourceId id = upsert(observation(dir_ / row.name, n++))->id;
        ItemMetadata metadata = item(0);
        metadata.support_status = row.support;
        const ItemId item_id = upsert_items(id, {metadata})->front();
        LUCENT_ASSERT_OK(catalog_->write([&](WriteTransaction& tx) -> Status {
            if (Status s = tx.set_source_state(id, row.state, at(1)); !s) {
                return s;
            }
            return tx.set_item_flags(item_id, row.flags);
        }));
    }
    const auto names = [&](ItemFilter filter) {
        ItemQuery query;
        query.filter = std::move(filter);
        std::vector<std::wstring> out;
        for (const ItemRecord& r : all_items(query)) {
            out.push_back(r.source_path.filename().native());
        }
        return out;
    };
    EXPECT_EQ(names({.source_states = {SourceState::Missing, SourceState::Changed}}),
              (std::vector<std::wstring>{L"b.cr3", L"c.cr3"}));
    EXPECT_EQ(names({.support = {SupportStatus::Supported}}),
              (std::vector<std::wstring>{L"a.cr3", L"b.cr3"}));
    EXPECT_EQ(names({.pick = PickFlag::Picked}), (std::vector<std::wstring>{L"a.cr3", L"d.cr3"}));
    EXPECT_EQ(names({.min_rating = std::uint8_t{3}}), (std::vector<std::wstring>{L"a.cr3", L"b.cr3"}));
    EXPECT_EQ(names({.source_states = {SourceState::Present}, .support = {SupportStatus::Unsupported}}),
              (std::vector<std::wstring>{L"d.cr3"}));
    ItemQuery bad;
    bad.filter.min_rating = std::uint8_t{6};
    EXPECT_FALSE(catalog_->query([&](ReadTransaction& tx) { return tx.list_items(bad); }));
}

TEST_F(CatalogRepository, ImportRecordsRoundTrip) {
    ImportStart start;
    start.started_at = at(1'700'000'000'000);
    start.roots = {ImportRoot{dir_ / L"Фото \"quoted\" \\ back", true},
                   ImportRoot{L"E:\\DCIM\\IMG_1.CR3", false}};
    start.options_json = R"({"copy":false,"exclude":["cache"]})";
    Expected<ImportId> id = catalog_->transact([&](WriteTransaction& tx) { return tx.begin_import(start); });
    LUCENT_ASSERT_OK(id);

    Expected<std::optional<ImportRecord>> running =
        catalog_->query([&](ReadTransaction& tx) { return tx.find_import(*id); });
    LUCENT_ASSERT_OK(running);
    ASSERT_TRUE(running->has_value());
    EXPECT_EQ((*running)->outcome, ImportOutcome::Running);
    EXPECT_FALSE((*running)->finished_at.has_value());
    EXPECT_EQ((*running)->roots, start.roots);
    EXPECT_EQ((*running)->options_json, start.options_json);

    ImportFinish finish;
    finish.finished_at = at(1'700'000'060'000);
    finish.outcome = ImportOutcome::Cancelled;
    finish.counts = ImportCounts{120, 100, 3, 10, 2, 4, 1};
    LUCENT_ASSERT_OK(catalog_->write([&](WriteTransaction& tx) { return tx.finish_import(*id, finish); }));
    const ImportRecord done = **catalog_->query([&](ReadTransaction& tx) { return tx.find_import(*id); });
    EXPECT_EQ(done.outcome, ImportOutcome::Cancelled);
    EXPECT_EQ(done.finished_at, finish.finished_at);
    EXPECT_EQ(done.counts, finish.counts);
    EXPECT_EQ(done.started_at, start.started_at);

    const Status twice = catalog_->write([&](WriteTransaction& tx) { return tx.finish_import(*id, finish); });
    ASSERT_FALSE(twice);
    EXPECT_EQ(twice.error().code, ErrorCode::InvalidArgument);
    const Status unknown =
        catalog_->write([&](WriteTransaction& tx) { return tx.finish_import(ImportId{77}, finish); });
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, ErrorCode::NotFound);
    ImportFinish still_running = finish;
    still_running.outcome = ImportOutcome::Running;
    EXPECT_FALSE(catalog_->write([&](WriteTransaction& tx) { return tx.finish_import(*id, still_running); }));
}

TEST_F(CatalogRepository, ImportOptionsMustBeJsonAndInterruptedImportsAreMarked) {
    ImportStart bad;
    bad.options_json = "{not json";
    Expected<ImportId> refused =
        catalog_->transact([&](WriteTransaction& tx) { return tx.begin_import(bad); });
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::InvalidArgument);

    std::vector<ImportId> ids;
    for (int i = 0; i < 3; ++i) {
        ImportStart start;
        start.started_at = at(1000 + i);
        ids.push_back(*catalog_->transact([&](WriteTransaction& tx) { return tx.begin_import(start); }));
    }
    LUCENT_ASSERT_OK(catalog_->write([&](WriteTransaction& tx) {
        return tx.finish_import(ids[0], ImportFinish{at(2000), ImportOutcome::Completed, {}});
    }));
    Expected<std::uint64_t> marked =
        catalog_->transact([&](WriteTransaction& tx) { return tx.mark_interrupted_imports(at(5000)); });
    LUCENT_ASSERT_OK(marked);
    EXPECT_EQ(*marked, 2u);
    Expected<std::vector<ImportRecord>> recent =
        catalog_->query([](ReadTransaction& tx) { return tx.list_imports(10); });
    LUCENT_ASSERT_OK(recent);
    ASSERT_EQ(recent->size(), 3u);
    EXPECT_EQ((*recent)[0].id, ids[2]);  // newest first
    EXPECT_EQ((*recent)[0].outcome, ImportOutcome::Interrupted);
    EXPECT_EQ((*recent)[0].finished_at, at(5000));
    EXPECT_EQ((*recent)[2].outcome, ImportOutcome::Completed);
    EXPECT_FALSE(catalog_->query([](ReadTransaction& tx) { return tx.list_imports(0); }));
}

TEST_F(CatalogRepository, UnknownStoredValuesAreReportedNotGuessed) {
    const SourceId id = upsert(observation(dir_ / L"a.cr3", 1))->id;
    catalog_.reset();
    {
        std::unique_ptr<detail::Connection> raw = raw_connection(dir_ / L"lucent.catalog");
        ASSERT_NE(raw, nullptr);
        LUCENT_ASSERT_OK(raw->exec("UPDATE sources SET state = 'teleported'", "test"));
    }
    catalog_ = open_or_fail(dir_ / L"lucent.catalog");
    ASSERT_NE(catalog_, nullptr);
    Expected<std::optional<SourceRecord>> read =
        catalog_->query([&](ReadTransaction& tx) { return tx.find_source(id); });
    ASSERT_FALSE(read);
    EXPECT_EQ(read.error().code, ErrorCode::Corrupt);
    EXPECT_NE(read.error().message.find("teleported"), std::string::npos);
}

}  // namespace
}  // namespace lucent::catalog
