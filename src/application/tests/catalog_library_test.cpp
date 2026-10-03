// CatalogLibrary: the persistent library over a real catalog file. Persistence across a restart
// without touching the sources, the Library contract (duplicates, atomicity, observers), content
// hashes and duplicates, source states, relocation, import records and read-only catalogs.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include <lucent/application/catalog_library.hpp>
#include <lucent/application/import.hpp>

#include "app_test_support.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;
namespace rts = raw::test_support;

class CatalogLibraryTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry_ = ts::default_registry();
        catalog_path_ = dir_.path() / L"data" / L"lucent.catalog";
        open();
    }

    void open() {
        fixture_ = ts::CatalogFixture::open(catalog_path_);
        ASSERT_NE(fixture_.library, nullptr);
    }

    void reopen() {
        fixture_.close();
        open();
    }

    CatalogLibrary& library() { return *fixture_.library; }

    ImportReport import_folder(const std::filesystem::path& folder) {
        auto report = import_sources(library(), *registry_, ImportRequest{.folders = {{folder, true}}});
        EXPECT_TRUE(report) << (report ? std::string() : report.error().describe());
        return report ? *report : ImportReport{};
    }

    // Hashes the file of `item` and records it.
    void hash(const SourceItem& item) {
        auto hashed = core::hash_file(item.path, {});
        ASSERT_TRUE(hashed);
        ASSERT_TRUE(library().set_content_hash(item.id, hashed->digest, hashed->identity));
    }

    rts::ScratchDir dir_;
    std::filesystem::path catalog_path_;
    std::shared_ptr<const raw::DecoderRegistry> registry_;
    ts::CatalogFixture fixture_;
};

TEST_F(CatalogLibraryTest, ItemsAndFlagsSurviveARestartWithoutTouchingTheSources) {
    ts::write_dng(dir_, L"card\\a.dng");
    ts::write_dng(dir_, L"card\\sub\\b.dng", rts::SyntheticDngSpec{.orientation = 6});
    dir_.write_text(L"card\\broken.dng", "not a raw file");
    const ImportReport report = import_folder(dir_.path() / L"card");
    ASSERT_EQ(report.added.size(), 3u);
    const std::vector<SourceItem> before = library().items();
    const SourceItem a = *ts::item_named(library(), L"a.dng");
    ASSERT_TRUE(library().set_flags(a.id, ItemFlags{PickFlag::Picked, 4}));
    hash(a);

    // Close, then take the sources away: loading must not look at them (a check would mark them
    // Missing), and must still list everything as it was.
    reopen();
    std::filesystem::rename(dir_.path() / L"card", dir_.path() / L"card-unplugged");
    reopen();

    const std::vector<SourceItem> after = library().items();
    ASSERT_EQ(after.size(), before.size());
    for (std::size_t i = 0; i < after.size(); ++i) {
        SCOPED_TRACE(after[i].path.string());
        EXPECT_EQ(after[i].id, before[i].id);
        EXPECT_EQ(after[i].path, before[i].path);
        EXPECT_EQ(after[i].subimage, before[i].subimage);
        EXPECT_EQ(after[i].identity, before[i].identity);
        EXPECT_EQ(after[i].support.status, before[i].support.status);
        EXPECT_EQ(after[i].probe.has_value(), before[i].probe.has_value());
        EXPECT_EQ(after[i].source_state, SourceState::Present);
        EXPECT_EQ(after[i].revision, 1u);
        if (after[i].probe) {
            EXPECT_EQ(after[i].probe->camera_make, before[i].probe->camera_make);
            EXPECT_EQ(after[i].probe->camera_model, before[i].probe->camera_model);
            EXPECT_EQ(after[i].probe->stored_size, before[i].probe->stored_size);
            EXPECT_EQ(after[i].probe->orientation, before[i].probe->orientation);
        }
    }
    const SourceItem restored = *ts::item_named(library(), L"a.dng");
    EXPECT_EQ(restored.flags, (ItemFlags{PickFlag::Picked, 4}));
    ASSERT_TRUE(restored.sha256);
    const SourceItem broken = *ts::item_named(library(), L"broken.dng");
    EXPECT_EQ(broken.support.status, SupportStatus::Unsupported);
    EXPECT_FALSE(broken.probe.has_value());
    ASSERT_FALSE(broken.support.reasons.empty());
    EXPECT_EQ(broken.support.reasons.front().kind, SupportReasonKind::ProbeFailed);
    EXPECT_EQ(ts::item_named(library(), L"b.dng")->probe->orientation, image::Orientation::Rotate90Cw);
}

TEST_F(CatalogLibraryTest, ReimportingAfterARestartFindsTheSameItems) {
    ts::write_dng(dir_, L"in\\a.dng");
    ASSERT_EQ(import_folder(dir_.path() / L"in").added.size(), 1u);
    reopen();
    const ImportReport again = import_folder(dir_.path() / L"in");
    EXPECT_TRUE(again.added.empty());
    ASSERT_EQ(again.skipped.size(), 1u);
    EXPECT_EQ(again.skipped[0].reason, SkipReason::Duplicate);
    EXPECT_EQ(library().size(), 1u);
}

// A card that comes back: importing it again makes its Missing files Present again (they are not
// reported as duplicates), with their ids and flags.
TEST_F(CatalogLibraryTest, ReimportingAMissingFileMakesItPresentAgain) {
    ts::write_dng(dir_, L"card\\A.dng");
    ASSERT_EQ(import_folder(dir_.path() / L"card").added.size(), 1u);
    const SourceItem item = library().items().front();
    ASSERT_TRUE(library().set_flags(item.id, ItemFlags{PickFlag::Picked, 3}));
    ASSERT_TRUE(library().set_source_state(item.id, SourceState::Missing));
    reopen();

    const ImportReport again = import_folder(dir_.path() / L"card");
    EXPECT_TRUE(again.added.empty());
    EXPECT_TRUE(again.skipped.empty());
    EXPECT_EQ(again.restored, std::vector<ItemId>{item.id});
    const SourceItem after = *library().find(item.id);
    EXPECT_EQ(after.source_state, SourceState::Present);
    EXPECT_EQ(after.flags, (ItemFlags{PickFlag::Picked, 3}));
    reopen();
    EXPECT_EQ(library().find(item.id)->source_state, SourceState::Present);
}

// A card back under another drive letter (simulated by renaming its folder: the volume serial,
// file ids, sizes and times stay) before its files were hashed: importing it moves the items to
// the new paths instead of calling them duplicates, without reading the files.
TEST_F(CatalogLibraryTest, ImportingAMovedFileFollowsItsItem) {
    ts::write_dng(dir_, L"cardE\\DCIM\\IMG_0001.dng");
    ASSERT_EQ(import_folder(dir_.path() / L"cardE").added.size(), 1u);
    const SourceItem item = library().items().front();
    ASSERT_TRUE(library().set_flags(item.id, ItemFlags{PickFlag::Rejected, 0}));
    std::filesystem::rename(dir_.path() / L"cardE", dir_.path() / L"cardF");

    const ImportReport again = import_folder(dir_.path() / L"cardF");
    EXPECT_TRUE(again.added.empty());
    EXPECT_TRUE(again.skipped.empty());
    EXPECT_EQ(again.restored, std::vector<ItemId>{item.id});
    ASSERT_EQ(library().size(), 1u);
    const SourceItem after = *library().find(item.id);
    EXPECT_EQ(after.path.parent_path().parent_path().filename(), L"cardF");
    EXPECT_EQ(after.identity, item.identity);
    EXPECT_EQ(after.source_state, SourceState::Present);
    EXPECT_EQ(after.flags, (ItemFlags{PickFlag::Rejected, 0}));
    EXPECT_FALSE(after.sha256);
    reopen();
    EXPECT_EQ(library().find(item.id)->path, after.path);
}

// One file name that is not valid Unicode (NTFS allows unpaired surrogates) is skipped and named
// in the report; the other files of the folder are imported.
TEST_F(CatalogLibraryTest, ANameThatIsNotUnicodeIsSkippedAndTheImportGoesOn) {
    ts::write_dng(dir_, L"card\\A.dng");
    const std::wstring bad_file = std::wstring(L"card\\B") + wchar_t(0xD800) + L".dng";
    ts::write_dng(dir_, bad_file);
    ts::write_dng(dir_, L"card\\C.dng", rts::SyntheticDngSpec{.model = "Synthetic DNC"});
    const std::wstring bad_folder = std::wstring(L"card\\sub") + wchar_t(0xDC00);
    ts::write_dng(dir_, bad_folder + L"\\D.dng", rts::SyntheticDngSpec{.model = "Synthetic DND"});

    const ImportReport report = import_folder(dir_.path() / L"card");
    EXPECT_EQ(report.added.size(), 2u);
    EXPECT_EQ(library().size(), 2u);
    std::vector<std::filesystem::path> invalid;
    for (const SkippedPath& skipped : report.skipped) {
        if (skipped.reason == SkipReason::InvalidName) {
            invalid.push_back(skipped.path.filename());
            EXPECT_TRUE(skipped.error);
        }
    }
    std::sort(invalid.begin(), invalid.end());
    ASSERT_EQ(invalid.size(), 2u);
    EXPECT_EQ(invalid[0], std::filesystem::path(std::wstring(L"B") + wchar_t(0xD800) + L".dng"));
    EXPECT_EQ(invalid[1], std::filesystem::path(std::wstring(L"sub") + wchar_t(0xDC00)));

    // Selected directly, and as an import folder (the import record keeps the valid roots).
    auto selected = import_sources(library(), *registry_,
                                   ImportRequest{.files = {dir_.path() / bad_file},
                                                 .folders = {{dir_.path() / bad_folder, true}}});
    ASSERT_TRUE(selected) << selected.error().describe();
    ASSERT_EQ(selected->skipped.size(), 2u);
    EXPECT_EQ(selected->skipped[0].reason, SkipReason::InvalidName);
    EXPECT_EQ(selected->skipped[1].reason, SkipReason::InvalidName);
    EXPECT_NE(selected->record, 0u);
}

TEST_F(CatalogLibraryTest, AnotherPathToTheSameFileIsADuplicate) {
    const auto original = ts::write_dng(dir_, L"in\\a.dng");
    const auto link = dir_.path() / L"in\\hardlink.dng";
    ASSERT_TRUE(::CreateHardLinkW(link.c_str(), original.c_str(), nullptr));
    const ImportReport report =
        import_sources(library(), *registry_, ImportRequest{.files = {original, link}}).value();
    EXPECT_EQ(report.added.size(), 1u);
    ASSERT_EQ(report.skipped.size(), 1u);
    EXPECT_EQ(report.skipped[0].reason, SkipReason::Duplicate);
}

TEST_F(CatalogLibraryTest, ABatchWithAnInvalidItemAddsNothing) {
    SourceItem good = ts::fake_item(dir_.path() / L"a.dng");
    SourceItem bad = ts::fake_item(L"relative.dng");
    auto added = library().add({good, bad});
    ASSERT_FALSE(added);
    EXPECT_EQ(added.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(library().size(), 0u);
    reopen();
    EXPECT_EQ(library().size(), 0u);
}

TEST_F(CatalogLibraryTest, ContentDuplicatesAreFlaggedNotMerged) {
    ts::write_dng(dir_, L"in\\a.dng");
    ts::write_dng(dir_, L"in\\copy-of-a.dng");  // same bytes
    ts::write_dng(dir_, L"in\\other.dng", rts::SyntheticDngSpec{.model = "Synthetic DNA"});
    ASSERT_EQ(import_folder(dir_.path() / L"in").added.size(), 3u);

    std::mutex mutex;
    std::vector<LibraryChange> changes;
    Subscription subscription = library().subscribe([&](const LibraryChange& change) {
        std::lock_guard lock(mutex);
        changes.push_back(change);
    });
    for (const SourceItem& item : library().items()) {
        hash(item);
    }
    const SourceItem a = *ts::item_named(library(), L"a.dng");
    const SourceItem copy = *ts::item_named(library(), L"copy-of-a.dng");
    const SourceItem other = *ts::item_named(library(), L"other.dng");
    EXPECT_EQ(a.content_duplicates, std::vector<ItemId>{copy.id});
    EXPECT_EQ(copy.content_duplicates, std::vector<ItemId>{a.id});
    EXPECT_TRUE(other.content_duplicates.empty());
    EXPECT_EQ(library().size(), 3u);  // both stay separate items
    {
        std::lock_guard lock(mutex);
        // Hashing copy-of-a published an Updated change that also lists a (its duplicate list changed).
        const bool a_updated = std::any_of(changes.begin(), changes.end(), [&](const LibraryChange& change) {
            return change.kind == LibraryChangeKind::Updated &&
                   std::find(change.items.begin(), change.items.end(), a.id) != change.items.end() &&
                   std::find(change.items.begin(), change.items.end(), copy.id) != change.items.end();
        });
        EXPECT_TRUE(a_updated);
    }

    // Duplicates are recomputed at load, and removing one clears the other's flag.
    subscription.reset();
    reopen();
    EXPECT_EQ(ts::item_named(library(), L"a.dng")->content_duplicates, std::vector<ItemId>{copy.id});
    const ItemId removed[] = {copy.id};
    ASSERT_TRUE(library().remove(removed));
    EXPECT_TRUE(ts::item_named(library(), L"a.dng")->content_duplicates.empty());
    reopen();
    EXPECT_EQ(library().size(), 2u);
    EXPECT_TRUE(ts::item_named(library(), L"a.dng")->content_duplicates.empty());
}

TEST_F(CatalogLibraryTest, AHashOfAnotherFileVersionIsRefused) {
    const auto path = ts::write_dng(dir_, L"in\\a.dng");
    ASSERT_EQ(import_folder(dir_.path() / L"in").added.size(), 1u);
    const SourceItem item = library().items().front();
    core::FileIdentity other = item.identity;
    other.last_write_time += 1;
    const Status refused = library().set_content_hash(item.id, ts::file_sha256(path), other);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::SourceChanged);
    EXPECT_FALSE(library().find(item.id)->sha256);
}

TEST_F(CatalogLibraryTest, RefreshReplacesTheIdentityAndClearsTheHash) {
    const auto path = ts::write_dng(dir_, L"in\\a.dng");
    ASSERT_EQ(import_folder(dir_.path() / L"in").added.size(), 1u);
    const SourceItem before = library().items().front();
    hash(before);
    ASSERT_TRUE(library().find(before.id)->sha256);

    Sleep(20);  // a distinct modification time
    ts::write_dng(dir_, L"in\\a.dng", rts::SyntheticDngSpec{.orientation = 3});
    const ImportReport again = import_folder(dir_.path() / L"in");
    ASSERT_EQ(again.refreshed.size(), 1u);
    const SourceItem after = *library().find(before.id);
    EXPECT_NE(after.identity, before.identity);
    EXPECT_FALSE(after.sha256);
    EXPECT_EQ(after.probe->orientation, image::Orientation::Rotate180);
    reopen();
    const SourceItem reloaded = *library().find(before.id);
    EXPECT_EQ(reloaded.identity, after.identity);
    EXPECT_FALSE(reloaded.sha256);
    EXPECT_EQ(reloaded.probe->orientation, image::Orientation::Rotate180);
}

TEST_F(CatalogLibraryTest, SourceStatesPersistAndPublishOnce) {
    ts::write_dng(dir_, L"in\\a.dng");
    ASSERT_EQ(import_folder(dir_.path() / L"in").added.size(), 1u);
    const SourceItem item = library().items().front();
    int updates = 0;
    Subscription subscription = library().subscribe(
        [&](const LibraryChange& change) { updates += change.kind == LibraryChangeKind::Updated ? 1 : 0; });
    ASSERT_TRUE(library().set_source_state(item.id, SourceState::Missing));
    ASSERT_TRUE(library().set_source_state(item.id, SourceState::Missing));  // no change
    EXPECT_EQ(updates, 1);
    subscription.reset();
    reopen();
    EXPECT_EQ(library().find(item.id)->source_state, SourceState::Missing);
}

TEST_F(CatalogLibraryTest, RelocationToAnotherFileNeedsAMatchingHash) {
    const auto path = ts::write_dng(dir_, L"in\\a.dng");
    ASSERT_EQ(import_folder(dir_.path() / L"in").added.size(), 1u);
    const SourceItem item = library().items().front();

    // A copy elsewhere: the same bytes in another file object.
    std::filesystem::create_directories(dir_.path() / L"moved");
    const auto moved = dir_.path() / L"moved\\a.dng";
    std::filesystem::copy_file(path, moved);
    std::filesystem::remove(path);
    auto hashed = core::hash_file(moved, {});
    ASSERT_TRUE(hashed);
    ASSERT_NE(hashed->identity, item.identity);
    SourceRelocation relocation{moved, hashed->identity, hashed->digest, core::StorageKind::Fixed, {}, {}, {}};

    const Status unverified = library().relocate_source(item.id, relocation);  // never hashed
    ASSERT_FALSE(unverified);
    EXPECT_EQ(unverified.error().where, "app.library.relocate.unverified");
    SourceRelocation without_hash = relocation;
    without_hash.sha256.reset();
    EXPECT_FALSE(library().relocate_source(item.id, without_hash));

    ASSERT_TRUE(library().set_content_hash(item.id, hashed->digest, item.identity));  // the same bytes
    SourceRelocation wrong = relocation;
    (*wrong.sha256)[0] ^= 0xff;
    const Status mismatch = library().relocate_source(item.id, wrong);
    ASSERT_FALSE(mismatch);
    EXPECT_EQ(mismatch.error().where, "app.library.relocate.content_mismatch");
    const Status still_unverified = library().relocate_source(item.id, without_hash);
    ASSERT_FALSE(still_unverified);
    EXPECT_EQ(still_unverified.error().where, "app.library.relocate.unverified");

    ASSERT_TRUE(library().set_source_state(item.id, SourceState::Missing));
    ASSERT_TRUE(library().relocate_source(item.id, relocation));
    const SourceItem after = *library().find(item.id);
    EXPECT_EQ(after.path, moved);
    EXPECT_EQ(after.identity, hashed->identity);
    EXPECT_EQ(after.source_state, SourceState::Present);
    EXPECT_EQ(after.storage, core::StorageKind::Fixed);
    reopen();
    EXPECT_EQ(library().find(item.id)->path, moved);
    EXPECT_EQ(library().find_source(moved, {}, 0), item.id);
    EXPECT_FALSE(library().find_source(path, {}, 0));
}

// The library's own file version found elsewhere (a rename keeps the volume serial, file id, size
// and modification time; so does a card under another drive letter) is verified by its identity:
// no hash is needed, and an unhashed item stays unhashed.
TEST_F(CatalogLibraryTest, TheSameFileVersionRelocatesWithoutAHash) {
    const auto path = ts::write_dng(dir_, L"E\\DCIM\\a.dng");
    ASSERT_EQ(import_folder(dir_.path() / L"E").added.size(), 1u);
    const SourceItem item = library().items().front();
    ASSERT_TRUE(library().set_flags(item.id, ItemFlags{PickFlag::Picked, 2}));
    ASSERT_TRUE(library().set_source_state(item.id, SourceState::Missing));
    std::filesystem::rename(dir_.path() / L"E", dir_.path() / L"F");
    const auto moved = dir_.path() / L"F\\DCIM\\a.dng";
    const core::FileIdentity now = *core::stat_file(moved);
    ASSERT_EQ(now, item.identity);

    SourceRelocation relocation;
    relocation.path = moved;
    relocation.identity = now;
    relocation.storage = core::StorageKind::Fixed;
    ASSERT_TRUE(library().relocate_source(item.id, relocation));
    reopen();
    const SourceItem after = *library().find(item.id);
    EXPECT_EQ(after.path, moved);
    EXPECT_EQ(after.source_state, SourceState::Present);
    EXPECT_EQ(after.flags, (ItemFlags{PickFlag::Picked, 2}));
    EXPECT_FALSE(after.sha256);
    EXPECT_EQ(library().find_source(L"Z:\\elsewhere.dng", now, 0), item.id);
    (void)path;
}

// A file id alone is not a file: FAT and exFAT reuse the ids of deleted files (directory
// positions). A new photo with a stale item's volume serial and file id, but another size and
// time, is a new item, while the same version at another path is still a duplicate.
TEST_F(CatalogLibraryTest, AReusedFileIdIsANewFileNotADuplicate) {
    SourceItem old_photo = ts::fake_item(L"E:\\DCIM\\100CANON\\IMG_0001.CR3");
    old_photo.identity.volume_serial = 0x1234ABCD;
    old_photo.identity.file_id[0] = 0x20;
    old_photo.identity.size_bytes = 25'000'000;
    old_photo.identity.last_write_time = 133'000'000'000'000'000;
    auto added = library().add({old_photo});
    ASSERT_TRUE(added);
    ASSERT_TRUE((*added)[0].added);
    ASSERT_TRUE(library().set_source_state((*added)[0].id, SourceState::Missing));
    reopen();

    SourceItem new_photo = ts::fake_item(L"E:\\DCIM\\100CANON\\IMG_0102.CR3");
    new_photo.identity = old_photo.identity;
    new_photo.identity.size_bytes = 26'500'000;
    new_photo.identity.last_write_time = 133'100'000'000'000'000;
    SourceItem hard_link = ts::fake_item(L"E:\\DCIM\\100CANON\\LINK.CR3");
    hard_link.identity = old_photo.identity;
    auto second = library().add({new_photo, hard_link});
    ASSERT_TRUE(second);
    EXPECT_TRUE((*second)[0].added);
    EXPECT_FALSE((*second)[1].added);
    EXPECT_EQ((*second)[1].duplicate, DuplicateKind::SameFileObject);
    EXPECT_EQ((*second)[1].id, (*added)[0].id);
    EXPECT_EQ(library().find_source(L"Z:\\x.cr3", new_photo.identity, 0), (*second)[0].id);
    EXPECT_EQ(library().find_source(L"Z:\\x.cr3", old_photo.identity, 0), (*added)[0].id);
    reopen();
    EXPECT_EQ(library().size(), 2u);
}

TEST_F(CatalogLibraryTest, CopiesRememberTheirOrigin) {
    SourceItem copy = ts::fake_item(dir_.path() / L"library\\a.dng");
    copy.identity.size_bytes = 1234;
    copy.import_mode = ImportMode::Copied;
    copy.origin_path = dir_.path() / L"card\\a.dng";
    core::FileIdentity original;
    original.volume_serial = 0xE0E0;
    original.file_id[0] = 7;
    original.size_bytes = 1234;
    original.last_write_time = 42;
    copy.origin_identity = original;
    auto added = library().add({copy});
    ASSERT_TRUE(added) << added.error().describe();
    reopen();
    EXPECT_EQ(library().find((*added)[0].id)->origin_identity, original);

    // The same original version, under any path (another drive letter).
    EXPECT_EQ(library().find_copied_from(L"F:\\DCIM\\a.dng", original), (*added)[0].id);
    // A candidate by origin path and size (case-insensitively), whatever its identity.
    core::FileIdentity other = original;
    other.file_id[0] = 8;
    const auto found = library().find_copied_from(dir_.path() / L"CARD\\A.DNG", other);
    ASSERT_TRUE(found);
    EXPECT_EQ(*found, (*added)[0].id);
    other.size_bytes = 99;
    EXPECT_FALSE(library().find_copied_from(dir_.path() / L"card\\a.dng", other));
    EXPECT_FALSE(library().find_copied_from(L"F:\\DCIM\\a.dng", other));
    EXPECT_EQ(library().find(*found)->import_mode, ImportMode::Copied);
    EXPECT_EQ(library().find(*found)->origin_path, copy.origin_path);

    SourceItem orphan = ts::fake_item(dir_.path() / L"library\\b.dng");
    orphan.import_mode = ImportMode::Copied;  // no origin
    EXPECT_FALSE(library().add({orphan}));

    // An import in place that adds another image of the copy keeps the file a copy.
    SourceItem first_image = ts::fake_item(dir_.path() / L"library\\burst.dng");
    first_image.subimage_count = 2;
    first_image.import_mode = ImportMode::Copied;
    first_image.origin_path = dir_.path() / L"card\\burst.dng";
    first_image.origin_identity = original;
    ASSERT_TRUE(library().add({first_image}));
    SourceItem second_image = ts::fake_item(dir_.path() / L"library\\burst.dng");
    second_image.subimage = 1;
    second_image.subimage_count = 2;  // Referenced, as an import in place describes it
    auto second = library().add({second_image});
    ASSERT_TRUE(second);
    ASSERT_TRUE((*second)[0].added);
    reopen();
    for (const SourceItem& item : library().items()) {
        if (item.path.filename() == L"burst.dng") {
            EXPECT_EQ(item.import_mode, ImportMode::Copied) << item.subimage;
            EXPECT_EQ(item.origin_path, first_image.origin_path);
            EXPECT_EQ(item.origin_identity, original);
        }
    }
}

TEST_F(CatalogLibraryTest, ImportsAreRecordedAndInterruptedOnesMarked) {
    ts::write_dng(dir_, L"in\\a.dng");
    const ImportReport report = import_folder(dir_.path() / L"in");
    ASSERT_NE(report.record, 0u);
    auto started = library().begin_import(ImportJournalStart{{{dir_.path(), false}}, std::nullopt});
    ASSERT_TRUE(started);  // never finished: a crash
    reopen();
    EXPECT_EQ(library().interrupted_imports(), 1u);
    auto records = fixture_.catalog->query([](catalog::ReadTransaction& tx) { return tx.list_imports(10); });
    ASSERT_TRUE(records);
    ASSERT_EQ(records->size(), 2u);
    EXPECT_EQ((*records)[0].outcome, catalog::ImportOutcome::Interrupted);
    EXPECT_EQ((*records)[1].outcome, catalog::ImportOutcome::Completed);
    EXPECT_EQ((*records)[1].counts.added, 1u);
    EXPECT_EQ((*records)[1].roots.size(), 1u);
}

TEST_F(CatalogLibraryTest, AReadOnlyCatalogIsListedButNotChanged) {
    ts::write_dng(dir_, L"in\\a.dng");
    ASSERT_EQ(import_folder(dir_.path() / L"in").added.size(), 1u);
    fixture_.close();
    auto catalog = ts::open_catalog(catalog_path_, catalog::OpenMode::ReadOnly);
    ASSERT_NE(catalog, nullptr);
    auto library = CatalogLibrary::open(catalog);
    ASSERT_TRUE(library);
    EXPECT_EQ((*library)->size(), 1u);
    EXPECT_FALSE((*library)->capabilities().source_maintenance);
    const SourceItem item = (*library)->items().front();
    const Status refused = (*library)->set_flags(item.id, ItemFlags{PickFlag::Rejected, 0});
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::PermissionDenied);
    EXPECT_EQ((*library)->find(item.id)->flags, ItemFlags{});
}

// Start-up cost of a larger library: the whole catalog is loaded into memory in one read
// transaction before the window shows it. Timings are printed (measurements, not gates).
TEST_F(CatalogLibraryTest, LoadsTenThousandItemsInOneReadTransaction) {
    constexpr int kItems = 10'000;
    constexpr int kBatch = 500;
    const auto add_start = std::chrono::steady_clock::now();
    for (int first = 0; first < kItems; first += kBatch) {
        std::vector<SourceItem> batch;
        for (int i = first; i < first + kBatch; ++i) {
            SourceItem item =
                ts::fake_item(dir_.path() / L"photos" / (L"IMG_" + std::to_wstring(i) + L".CR3"));
            item.identity.file_id[0] = static_cast<std::uint8_t>(i & 0xff);
            item.identity.file_id[1] = static_cast<std::uint8_t>(i >> 8);
            item.identity.size_bytes = 40'000'000;
            item.probe->camera_make = "Canon";
            item.probe->camera_model = "EOS R5";
            item.probe->capture_time_local = "2026-09-01T07:15:00";
            item.sha256 = core::Digest256{};
            item.sha256->at(0) = static_cast<std::uint8_t>(i & 0xff);
            item.sha256->at(1) = static_cast<std::uint8_t>(i >> 8);
            batch.push_back(std::move(item));
        }
        auto added = library().add(std::move(batch));  // one transaction per batch
        ASSERT_TRUE(added) << added.error().describe();
    }
    const double add_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - add_start).count();
    fixture_.close();

    const auto open_start = std::chrono::steady_clock::now();
    open();
    const double open_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - open_start).count();
    ASSERT_EQ(library().size(), static_cast<std::size_t>(kItems));
    const auto list_start = std::chrono::steady_clock::now();
    const std::vector<SourceItem> items = library().items();
    const double list_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - list_start).count();
    EXPECT_EQ(items.back().probe->camera_model, "EOS R5");
    EXPECT_TRUE(items.back().sha256);
    std::printf(
        "  %d items: added in %.2f s (batches of %d), catalog opened and loaded in %.3f s, "
        "snapshot in %.3f s\n",
        kItems, add_seconds, kBatch, open_seconds, list_seconds);
    RecordProperty("load_10000_ms", static_cast<int>(open_seconds * 1000));
}

}  // namespace
}  // namespace lucent::app
