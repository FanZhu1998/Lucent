// "Copy into library folder" (import.hpp): verified copies registered as Copied items with their
// origin and hash, mirrored folders, originals untouched (blueprint release gate "Original
// preservation": source hashes unchanged after import), no repeated copies of the same photo but
// new photos with reused names copied, adoption of items already referenced (also when their file
// changed), failures reported per file, no unregistered copies left behind, and the storage kind
// of registered files.

#include <algorithm>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include <lucent/application/import.hpp>
#include <lucent/core/storage.hpp>

#include "app_test_support.hpp"
#include "win32_paths.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;
namespace rts = raw::test_support;

// The on-disk spelling of `path` (the importer stores canonical paths; the temporary folder may be
// spelled with short names).
std::filesystem::path on_disk(const std::filesystem::path& path) {
    auto resolved = detail::canonical_path(path, "test");
    EXPECT_TRUE(resolved) << path.string();
    return resolved ? *resolved : path;
}

// The regular files below `folder` (none when it does not exist).
std::vector<std::filesystem::path> files_under(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> files;
    if (!std::filesystem::exists(folder)) {
        return files;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(folder)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    return files;
}

// A library that refuses every add() (a catalog write error), delegating everything else.
class RefusingLibrary final : public Library {
public:
    explicit RefusingLibrary(Library& inner) : inner_(inner) {}
    std::size_t size() const override { return inner_.size(); }
    std::vector<SourceItem> items() const override { return inner_.items(); }
    std::optional<SourceItem> find(ItemId id) const override { return inner_.find(id); }
    std::optional<ItemId> find_source(const std::filesystem::path& path, const core::FileIdentity& identity,
                                      std::uint32_t subimage) const override {
        return inner_.find_source(path, identity, subimage);
    }
    Expected<std::vector<AddOutcome>> add(std::vector<SourceItem>) override {
        return fail(ErrorCode::IoError, "the catalog could not be written", "test.library.add");
    }
    Status set_flags(ItemId id, const ItemFlags& flags) override { return inner_.set_flags(id, flags); }
    Status refresh(const SourceItem& fresh) override { return inner_.refresh(fresh); }
    Status remove(std::span<const ItemId> ids) override { return inner_.remove(ids); }
    Subscription subscribe(LibraryObserver observer) override { return inner_.subscribe(std::move(observer)); }
    LibraryCapabilities capabilities() const override { return inner_.capabilities(); }

private:
    Library& inner_;
};

class ImportCopyTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry_ = ts::default_registry();
        fixture_ = ts::CatalogFixture::open(dir_.path() / L"data\\lucent.catalog");
        ASSERT_NE(fixture_.library, nullptr);
        std::filesystem::create_directories(library_folder());
    }

    std::filesystem::path card() const { return dir_.path() / L"card"; }
    std::filesystem::path library_folder() const { return dir_.path() / L"Pictures"; }

    // The originals of the card and the SHA-256 and identity of each.
    struct Original {
        core::Digest256 sha256{};
        core::FileIdentity identity;
    };
    std::map<std::wstring, Original> snapshot_card() const {
        std::map<std::wstring, Original> files;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(card())) {
            if (entry.is_regular_file()) {
                files[on_disk(entry.path()).wstring()] =
                    Original{ts::file_sha256(entry.path()), *core::stat_file(entry.path())};
            }
        }
        return files;
    }

    Expected<ImportReport> import_with_copy(const ImportRequest& base = {}) {
        ImportRequest request = base;
        if (request.files.empty() && request.folders.empty()) {
            request.folders = {{card(), true}};
        }
        request.copy_to = library_folder();
        return import_sources(*fixture_.library, *registry_, request);
    }

    void write_card() {
        ts::write_dng(dir_, L"card\\DCIM\\100CANON\\IMG_0001.dng", ts::dng_with_preview());
        ts::write_dng(dir_, L"card\\DCIM\\101CANON\\IMG_0001.dng", rts::SyntheticDngSpec{.orientation = 6});
        dir_.write_text(L"card\\DCIM\\100CANON\\IMG_0001.xmp", "<xmp/>");  // ignored, never copied
    }

    rts::ScratchDir dir_;
    std::shared_ptr<const raw::DecoderRegistry> registry_;
    ts::CatalogFixture fixture_;
};

TEST_F(ImportCopyTest, VerifiedCopiesAreRegisteredAndOriginalsStayUntouched) {
    write_card();
    const auto before = snapshot_card();
    auto report = import_with_copy();
    ASSERT_TRUE(report) << report.error().describe();
    EXPECT_EQ(report->added.size(), 2u);
    EXPECT_EQ(report->copied.size(), 2u);
    EXPECT_GT(report->bytes_copied, 0u);
    EXPECT_EQ(report->ignored_non_raw, 1u);

    // Originals: same bytes, same identity (size, modification time, file id).
    EXPECT_EQ(snapshot_card().size(), before.size());
    for (const auto& [path, original] : snapshot_card()) {
        SCOPED_TRACE(std::filesystem::path(path).string());
        EXPECT_EQ(original.sha256, before.at(path).sha256);
        EXPECT_EQ(original.identity, before.at(path).identity);
    }

    // The copies mirror the card's folders below a folder named after it, and carry their origin.
    const auto first = library_folder() / L"card\\DCIM\\100CANON\\IMG_0001.dng";
    const auto second = library_folder() / L"card\\DCIM\\101CANON\\IMG_0001.dng";
    ASSERT_TRUE(std::filesystem::exists(first));
    ASSERT_TRUE(std::filesystem::exists(second));
    for (const SourceItem& item : fixture_.library->items()) {
        SCOPED_TRACE(item.path.string());
        EXPECT_EQ(item.import_mode, ImportMode::Copied);
        ASSERT_TRUE(item.origin_path);
        EXPECT_TRUE(item.origin_path->wstring().starts_with(on_disk(card()).wstring()));
        ASSERT_TRUE(item.sha256);
        EXPECT_EQ(*item.sha256, before.at(item.origin_path->wstring()).sha256);
        EXPECT_EQ(*item.sha256, ts::file_sha256(item.path));
        EXPECT_TRUE(item.path.wstring().starts_with(on_disk(library_folder()).wstring()));
        EXPECT_EQ(item.storage, core::storage_kind(item.path));
    }
    // No temporary file is left behind.
    for (const auto& entry : std::filesystem::recursive_directory_iterator(library_folder())) {
        EXPECT_FALSE(core::is_temporary_file_name(entry.path().filename())) << entry.path().string();
    }

    // The copies survive a restart with their origin and hash.
    fixture_.close();
    fixture_ = ts::CatalogFixture::open(dir_.path() / L"data\\lucent.catalog");
    for (const SourceItem& item : fixture_.library->items()) {
        EXPECT_EQ(item.import_mode, ImportMode::Copied);
        EXPECT_TRUE(item.origin_path);
        EXPECT_TRUE(item.sha256);
    }
}

TEST_F(ImportCopyTest, AnOriginalIsCopiedOnlyOnce) {
    write_card();
    ASSERT_TRUE(import_with_copy());
    auto again = import_with_copy();
    ASSERT_TRUE(again) << again.error().describe();
    EXPECT_TRUE(again->added.empty());
    EXPECT_TRUE(again->copied.empty());
    EXPECT_EQ(
        std::count_if(again->skipped.begin(), again->skipped.end(),
                      [](const SkippedPath& s) { return s.reason == SkipReason::Duplicate && s.existing; }),
        2);
    // Exactly the two copies exist: no "_2" files.
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(library_folder())) {
        files += entry.is_regular_file() ? 1 : 0;
    }
    EXPECT_EQ(files, 2u);
}

TEST_F(ImportCopyTest, AReferencedItemMovesToItsCopyKeepingItsIdAndFlags) {
    write_card();
    auto in_place = import_sources(*fixture_.library, *registry_, ImportRequest{.folders = {{card(), true}}});
    ASSERT_TRUE(in_place);
    ASSERT_EQ(in_place->added.size(), 2u);
    const ItemId id = in_place->added.front();
    ASSERT_TRUE(fixture_.library->set_flags(id, ItemFlags{PickFlag::Picked, 3}));
    const std::filesystem::path original = fixture_.library->find(id)->path;

    auto copied = import_with_copy();
    ASSERT_TRUE(copied) << copied.error().describe();
    EXPECT_TRUE(copied->added.empty());
    EXPECT_EQ(copied->copied.size(), 2u);
    const SourceItem moved = *fixture_.library->find(id);
    EXPECT_EQ(moved.import_mode, ImportMode::Copied);
    EXPECT_EQ(moved.origin_path, original);
    EXPECT_TRUE(moved.path.wstring().starts_with(on_disk(library_folder()).wstring()));
    EXPECT_EQ(moved.flags, (ItemFlags{PickFlag::Picked, 3}));
    EXPECT_EQ(*moved.sha256, ts::file_sha256(original));
    EXPECT_EQ(fixture_.library->size(), 2u);
    EXPECT_TRUE(std::filesystem::exists(original));  // the card is untouched
}

TEST_F(ImportCopyTest, FilesAlreadyInTheLibraryFolderAreReferencedWhereTheyAre) {
    ts::write_dng(dir_, L"Pictures\\old\\a.dng");
    auto report =
        import_sources(*fixture_.library, *registry_,
                       ImportRequest{.folders = {{library_folder(), true}}, .copy_to = library_folder()});
    ASSERT_TRUE(report) << (report ? std::string() : report.error().describe());
    ASSERT_EQ(report->added.size(), 1u);
    EXPECT_TRUE(report->copied.empty());
    const SourceItem item = fixture_.library->items().front();
    EXPECT_EQ(item.import_mode, ImportMode::Referenced);
    EXPECT_EQ(item.path.filename(), L"a.dng");
}

TEST_F(ImportCopyTest, ACopyThatCannotBeMadeIsReportedPerFile) {
    write_card();
    // A file already occupies the first copy's name and the policy forbids another name.
    dir_.write_text(L"Pictures\\card\\DCIM\\100CANON\\IMG_0001.dng", "occupied");
    ImportRequest request;
    request.copy_options.name_policy = core::CopyNamePolicy::FailIfExists;
    auto report = import_with_copy(request);
    ASSERT_TRUE(report) << report.error().describe();
    EXPECT_EQ(report->added.size(), 1u);
    const auto failed = std::find_if(report->skipped.begin(), report->skipped.end(),
                                     [](const SkippedPath& s) { return s.reason == SkipReason::CopyFailed; });
    ASSERT_NE(failed, report->skipped.end());
    ASSERT_TRUE(failed->error);
    EXPECT_EQ(failed->error->code, ErrorCode::AlreadyExists);
    EXPECT_EQ(failed->path.filename(), L"IMG_0001.dng");
    // The occupied file was not overwritten.
    EXPECT_EQ(rts::read_all(library_folder() / L"card\\DCIM\\100CANON\\IMG_0001.dng").size(), 8u);
}

TEST_F(ImportCopyTest, AMissingDestinationFailsTheImport) {
    write_card();
    ImportRequest request;
    request.folders = {{card(), true}};
    request.copy_to = dir_.path() / L"no-such-folder";
    auto report = import_sources(*fixture_.library, *registry_, request);
    ASSERT_FALSE(report);
    EXPECT_EQ(report.error().code, ErrorCode::NotFound);
    EXPECT_EQ(fixture_.library->size(), 0u);
}

// A camera that restarted its numbering (a formatted card, a second body) writes a new photo with
// the name and size of one copied before: it is copied as a new photo, never skipped.
TEST_F(ImportCopyTest, ANewPhotoWithTheNameAndSizeOfAnEarlierCopyIsCopied) {
    rts::SyntheticDngSpec first = ts::dng_with_preview();
    first.black_level = 256;
    const auto path = ts::write_dng(dir_, L"card\\DCIM\\100CANON\\IMG_0001.dng", first);
    auto r1 = import_with_copy();
    ASSERT_TRUE(r1) << r1.error().describe();
    ASSERT_EQ(r1->copied.size(), 1u);
    const core::Digest256 first_hash = ts::file_sha256(path);
    const auto first_size = std::filesystem::file_size(path);

    std::filesystem::remove(path);  // the card is formatted; the numbering starts again
    rts::SyntheticDngSpec second = ts::dng_with_preview();
    second.black_level = 300;  // other bytes, the same size
    ts::write_dng(dir_, L"card\\DCIM\\100CANON\\IMG_0001.dng", second);
    ASSERT_EQ(std::filesystem::file_size(path), first_size);
    const core::Digest256 second_hash = ts::file_sha256(path);
    ASSERT_NE(first_hash, second_hash);

    auto r2 = import_with_copy();
    ASSERT_TRUE(r2) << r2.error().describe();
    EXPECT_EQ(r2->copied.size(), 1u);
    EXPECT_EQ(r2->added.size(), 1u);
    EXPECT_TRUE(r2->skipped.empty());
    ASSERT_EQ(fixture_.library->size(), 2u);
    std::vector<core::Digest256> hashes;
    for (const SourceItem& item : fixture_.library->items()) {
        ASSERT_TRUE(item.sha256);
        EXPECT_EQ(*item.sha256, ts::file_sha256(item.path));  // each copy holds its own photo
        hashes.push_back(*item.sha256);
    }
    EXPECT_NE(std::find(hashes.begin(), hashes.end(), first_hash), hashes.end());
    EXPECT_NE(std::find(hashes.begin(), hashes.end(), second_hash), hashes.end());
    EXPECT_EQ(files_under(library_folder()).size(), 2u);
}

// The same original found again is not copied twice: under another drive letter (its identity
// recorded with the copy), or with another identity but the same bytes (its content hash).
TEST_F(ImportCopyTest, TheSameOriginalIsRecognisedByItsIdentityOrItsContent) {
    write_card();
    ASSERT_TRUE(import_with_copy());
    ASSERT_EQ(files_under(library_folder()).size(), 2u);

    // The card under another drive letter: the same volume serial, file ids, sizes and times.
    std::filesystem::rename(card(), dir_.path() / L"card-F");
    ImportRequest request;
    request.folders = {{dir_.path() / L"card-F", true}};
    auto other_letter = import_with_copy(request);
    ASSERT_TRUE(other_letter) << other_letter.error().describe();
    EXPECT_TRUE(other_letter->copied.empty());
    EXPECT_EQ(std::count_if(other_letter->skipped.begin(), other_letter->skipped.end(),
                            [](const SkippedPath& s) { return s.reason == SkipReason::Duplicate && s.existing; }),
              2);
    std::filesystem::rename(dir_.path() / L"card-F", card());

    // The same bytes with another modification time: the hash shows it was copied already.
    const auto original = card() / L"DCIM\\100CANON\\IMG_0001.dng";
    HANDLE handle = ::CreateFileW(original.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  0, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    FILETIME later{};
    later.dwHighDateTime = 0x01DC0000u;
    ASSERT_TRUE(::SetFileTime(handle, nullptr, nullptr, &later));
    ::CloseHandle(handle);
    auto touched = import_with_copy();
    ASSERT_TRUE(touched) << touched.error().describe();
    EXPECT_TRUE(touched->copied.empty());
    EXPECT_TRUE(touched->added.empty());
    EXPECT_EQ(files_under(library_folder()).size(), 2u);
    EXPECT_EQ(fixture_.library->size(), 2u);
}

// A referenced card photo whose file was replaced (a reused card) is refreshed and moved to the
// verified copy of the new file when "Copy into library folder" is asked for: it is not left
// referencing the card.
TEST_F(ImportCopyTest, AChangedReferencedOriginalIsRefreshedAndMovedToItsCopy) {
    const auto path = ts::write_dng(dir_, L"card\\DCIM\\100CANON\\IMG_0001.dng", ts::dng_with_preview());
    auto in_place = import_sources(*fixture_.library, *registry_, ImportRequest{.folders = {{card(), true}}});
    ASSERT_TRUE(in_place);
    ASSERT_EQ(in_place->added.size(), 1u);
    const ItemId id = in_place->added.front();
    ASSERT_TRUE(fixture_.library->set_flags(id, ItemFlags{PickFlag::Picked, 1}));

    std::filesystem::remove(path);
    rts::SyntheticDngSpec other = ts::dng_with_preview(6);
    other.black_level = 300;
    ts::write_dng(dir_, L"card\\DCIM\\100CANON\\IMG_0001.dng", other);
    const core::Digest256 new_hash = ts::file_sha256(path);

    auto copied = import_with_copy();
    ASSERT_TRUE(copied) << copied.error().describe();
    EXPECT_EQ(copied->refreshed, std::vector<ItemId>{id});
    EXPECT_EQ(copied->copied, std::vector<ItemId>{id});
    EXPECT_TRUE(copied->added.empty());
    ASSERT_EQ(fixture_.library->size(), 1u);
    const SourceItem item = *fixture_.library->find(id);
    EXPECT_EQ(item.import_mode, ImportMode::Copied);
    EXPECT_TRUE(item.path.wstring().starts_with(on_disk(library_folder()).wstring()));
    EXPECT_EQ(item.origin_path, on_disk(path));
    EXPECT_EQ(item.sha256, new_hash);
    EXPECT_EQ(ts::file_sha256(item.path), new_hash);
    EXPECT_EQ(item.probe->orientation, image::Orientation::Rotate90Cw);  // the new photo's probe
    EXPECT_EQ(item.flags, (ItemFlags{PickFlag::Picked, 1}));
    EXPECT_EQ(files_under(library_folder()).size(), 1u);
}

// A copy that was deleted from the library folder is made again from its original, and the
// library's item moves to the new copy (its id and flags kept).
TEST_F(ImportCopyTest, AMissingCopyIsMadeAgain) {
    write_card();
    auto first = import_with_copy();
    ASSERT_TRUE(first) << first.error().describe();
    const ItemId id = first->copied.front();
    ASSERT_TRUE(fixture_.library->set_flags(id, ItemFlags{PickFlag::Rejected, 0}));
    const SourceItem before = *fixture_.library->find(id);
    std::filesystem::remove(before.path);
    ASSERT_TRUE(fixture_.library->set_source_state(id, SourceState::Missing));

    auto again = import_with_copy();
    ASSERT_TRUE(again) << again.error().describe();
    EXPECT_EQ(again->copied, std::vector<ItemId>{id});
    EXPECT_TRUE(again->added.empty());
    const SourceItem after = *fixture_.library->find(id);
    EXPECT_EQ(after.source_state, SourceState::Present);
    EXPECT_TRUE(std::filesystem::exists(after.path));
    EXPECT_EQ(after.sha256, before.sha256);
    EXPECT_EQ(after.flags, (ItemFlags{PickFlag::Rejected, 0}));
    EXPECT_EQ(fixture_.library->size(), 2u);
    EXPECT_EQ(files_under(library_folder()).size(), 2u);
}

// Copies that do not become library files are removed again: a selected TIFF that is not a RAW
// photo is not copied at all, and the copies of a batch the library refuses are deleted.
TEST_F(ImportCopyTest, NoUnregisteredCopyIsLeftBehind) {
    dir_.write_text(L"card\\export.tif", std::string("II*\0", 4) + std::string(200, 'x'));
    ImportRequest selected;
    selected.files = {card() / L"export.tif"};
    auto not_raw = import_with_copy(selected);
    ASSERT_TRUE(not_raw) << not_raw.error().describe();
    ASSERT_EQ(not_raw->skipped.size(), 1u);
    EXPECT_EQ(not_raw->skipped[0].reason, SkipReason::NotRawPhoto);
    EXPECT_EQ(not_raw->skipped[0].path, on_disk(card() / L"export.tif"));  // the original, not a copy
    EXPECT_TRUE(files_under(library_folder()).empty());

    write_card();
    RefusingLibrary refusing(*fixture_.library);
    ImportRequest request;
    request.folders = {{card(), true}};
    request.copy_to = library_folder();
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto refused = import_sources(refusing, *registry_, request);
        ASSERT_FALSE(refused);
        EXPECT_EQ(refused.error().code, ErrorCode::IoError);
        EXPECT_TRUE(files_under(library_folder()).empty()) << "attempt " << attempt;
    }
    EXPECT_EQ(fixture_.library->size(), 0u);
    // The originals are untouched, and a later import copies them normally.
    auto later = import_with_copy();
    ASSERT_TRUE(later) << later.error().describe();
    EXPECT_EQ(later->copied.size(), 2u);
    EXPECT_EQ(files_under(library_folder()).size(), 2u);
}

TEST_F(ImportCopyTest, ReferencedImportsRecordTheStorageOfTheirFiles) {
    write_card();
    auto report = import_sources(*fixture_.library, *registry_, ImportRequest{.folders = {{card(), true}}});
    ASSERT_TRUE(report);
    for (const SourceItem& item : fixture_.library->items()) {
        EXPECT_EQ(item.storage, core::storage_kind(item.path));
        EXPECT_EQ(item.import_mode, ImportMode::Referenced);
        EXPECT_FALSE(item.origin_path);
    }
    // The scratch folder is on a fixed disk in the test environment; nothing removable is reported.
    if (core::storage_kind(card()) == core::StorageKind::Fixed) {
        EXPECT_EQ(report->removable_items, 0u);
        EXPECT_TRUE(report->removable_locations.empty());
    }
}

}  // namespace
}  // namespace lucent::app
