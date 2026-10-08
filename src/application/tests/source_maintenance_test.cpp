// Source maintenance (source_maintenance.hpp) on a catalog-backed library and synthetic DNGs:
// hashing and content duplicates, changed files found by identity and by content, presence checks
// (missing, restored, changed and re-read), relinking confirmed by SHA-256, the camera-thumbnail
// warm-up, and originals untouched throughout.

#include <algorithm>
#include <filesystem>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include <lucent/application/import.hpp>
#include <lucent/application/source_maintenance.hpp>
#include <lucent/application/thumbnail_cache.hpp>
#include <lucent/export/export.hpp>

#include "app_test_support.hpp"
#include "catalog_mapping.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;
namespace rts = raw::test_support;

class SourceMaintenanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry_ = ts::default_registry();
        fixture_ = ts::CatalogFixture::open(dir_.path() / L"data\\lucent.catalog");
        ASSERT_NE(fixture_.library, nullptr);
    }

    Library& library() { return *fixture_.library; }

    void import(const std::filesystem::path& folder) {
        auto report = import_sources(library(), *registry_, ImportRequest{.folders = {{folder, true}}});
        ASSERT_TRUE(report) << report.error().describe();
    }

    HashReport hash_all(const HashOptions& options = {}) {
        auto report = hash_sources(library(), {}, options);
        EXPECT_TRUE(report) << (report ? std::string() : report.error().describe());
        return report ? *report : HashReport{};
    }

    RefreshReport refresh_all() {
        auto report = refresh_sources(library(), *registry_, {});
        EXPECT_TRUE(report) << (report ? std::string() : report.error().describe());
        return report ? *report : RefreshReport{};
    }

    SourceItem named(const std::wstring& name) {
        auto item = ts::item_named(library(), name);
        EXPECT_TRUE(item) << "no item " << std::filesystem::path(name).string();
        return item ? *item : SourceItem{};
    }

    rts::ScratchDir dir_;
    std::shared_ptr<const raw::DecoderRegistry> registry_;
    ts::CatalogFixture fixture_;
};

TEST_F(SourceMaintenanceTest, HashingRecordsHashesAndFlagsDuplicatesWithoutChangingSources) {
    ts::write_dng(dir_, L"in\\a.dng");
    ts::write_dng(dir_, L"in\\b.dng");  // the same bytes as a.dng
    ts::write_dng(dir_, L"in\\c.dng", rts::SyntheticDngSpec{.model = "Synthetic DNA"});
    const core::Digest256 a_hash = ts::file_sha256(dir_.path() / L"in\\a.dng");
    const core::Digest256 c_hash = ts::file_sha256(dir_.path() / L"in\\c.dng");
    import(dir_.path() / L"in");

    std::vector<MaintenanceProgress> progress;
    auto report =
        hash_sources(library(), {}, {}, [&](const MaintenanceProgress& p) { progress.push_back(p); });
    ASSERT_TRUE(report) << report.error().describe();
    EXPECT_EQ(report->files, 3u);
    EXPECT_EQ(report->hashed, 3u);
    EXPECT_EQ(report->changed, 0u);
    ASSERT_FALSE(progress.empty());
    EXPECT_EQ(progress.back().phase, MaintenancePhase::Hashing);
    EXPECT_EQ(progress.back().files_done, 3u);
    EXPECT_EQ(progress.back().items_done, 3u);
    EXPECT_EQ(progress.back().bytes_done, progress.back().bytes_total);

    EXPECT_EQ(*named(L"a.dng").sha256, a_hash);
    EXPECT_EQ(*named(L"c.dng").sha256, c_hash);
    EXPECT_EQ(named(L"a.dng").content_duplicates, std::vector<ItemId>{named(L"b.dng").id});
    EXPECT_EQ(named(L"b.dng").content_duplicates, std::vector<ItemId>{named(L"a.dng").id});
    EXPECT_TRUE(named(L"c.dng").content_duplicates.empty());

    // Hashed files are skipped next time; verification re-reads them and finds them unchanged.
    EXPECT_EQ(hash_all().files, 0u);
    HashOptions verify;
    verify.verify_hashed = true;
    const HashReport verified = hash_all(verify);
    EXPECT_EQ(verified.verified, 3u);
    EXPECT_EQ(verified.changed, 0u);
    EXPECT_EQ(ts::file_sha256(dir_.path() / L"in\\a.dng"), a_hash);  // original preservation
}

TEST_F(SourceMaintenanceTest, AFileChangedBeforeHashingIsMarkedChangedAndReRead) {
    const auto path = ts::write_dng(dir_, L"in\\a.dng");
    import(dir_.path() / L"in");
    const SourceItem before = named(L"a.dng");

    Sleep(20);
    ts::write_dng(dir_, L"in\\a.dng", rts::SyntheticDngSpec{.orientation = 8});
    const HashReport hashed = hash_all();
    EXPECT_EQ(hashed.changed, 1u);
    EXPECT_EQ(hashed.hashed, 0u);
    EXPECT_EQ(hashed.changed_items, std::vector<ItemId>{before.id});
    EXPECT_EQ(named(L"a.dng").source_state, SourceState::Changed);
    EXPECT_FALSE(named(L"a.dng").sha256);

    // The re-read refreshes the item in place (same id, new identity and probe); then it hashes.
    auto reread = refresh_sources(library(), *registry_, hashed.changed_items);
    ASSERT_TRUE(reread) << reread.error().describe();
    EXPECT_EQ(reread->changed, 1u);
    EXPECT_EQ(reread->reprobed_items, std::vector<ItemId>{before.id});
    const SourceItem after = named(L"a.dng");
    EXPECT_EQ(after.id, before.id);
    EXPECT_EQ(after.source_state, SourceState::Present);
    EXPECT_NE(after.identity, before.identity);
    EXPECT_EQ(after.probe->orientation, image::Orientation::Rotate90Ccw);
    EXPECT_EQ(reread->unhashed_items, std::vector<ItemId>{before.id});
    EXPECT_EQ(hash_all().hashed, 1u);
    EXPECT_EQ(*named(L"a.dng").sha256, ts::file_sha256(path));
}

// Items catalogued by a build with an older decoded-RAW contract (before W9 made DNG opcodes
// supported, the drone DNGs were stored "unsupported") are probed again by the presence check, in
// place and once.
TEST_F(SourceMaintenanceTest, OutdatedSupportStatusIsRenewedOnceInPlace) {
    const auto path = ts::write_dng(dir_, L"in\\a.dng");
    ts::write_dng(dir_, L"in\\b.dng", rts::SyntheticDngSpec{.model = "Synthetic DNA"});
    import(dir_.path() / L"in");
    hash_all();
    const SourceItem current = named(L"a.dng");
    ASSERT_EQ(current.support.status, SupportStatus::CompatibleUnverified);
    ASSERT_FALSE(probe_outdated(current));
    ASSERT_TRUE(library().set_flags(current.id, ItemFlags{PickFlag::Picked, 4}));

    // Store what a build before W9 would have: no probe contract, and Unsupported.
    SourceItem stale = named(L"a.dng");
    stale.probe->contract.reset();
    stale.support = assess_interpretation_status(raw::InterpretationStatus::RequiresUnsupportedOperation);
    ASSERT_TRUE(fixture_.catalog->write([&](catalog::WriteTransaction& tx) -> Status {
        auto record = tx.find_item(catalog::ItemId{static_cast<std::int64_t>(stale.id.value)});
        if (!record) {
            return Unexpected(std::move(record.error()));
        }
        if (!*record) {
            return fail(ErrorCode::NotFound, "the item is not in the catalog", "test");
        }
        const catalog::ItemMetadata metadata = detail::to_metadata(stale, catalog::Timestamp{});
        auto ids = tx.upsert_items((*record)->source_id, std::span(&metadata, 1));
        if (!ids) {
            return Unexpected(std::move(ids.error()));
        }
        return ok();
    }));
    const auto reopen = [&] {
        fixture_.library.reset();
        auto library = CatalogLibrary::open(fixture_.catalog);
        ASSERT_TRUE(library) << library.error().describe();
        fixture_.library = std::move(*library);
    };
    reopen();
    const SourceItem before = named(L"a.dng");
    EXPECT_EQ(before.support.status, SupportStatus::Unsupported);
    EXPECT_TRUE(probe_outdated(before));
    EXPECT_FALSE(probe_outdated(named(L"b.dng")));

    const RefreshReport report = refresh_all();
    EXPECT_EQ(report.present, 2u);
    EXPECT_EQ(report.changed, 0u);
    EXPECT_EQ(report.outdated, 1u);
    EXPECT_EQ(report.reprobed_items, std::vector<ItemId>{before.id});
    EXPECT_TRUE(report.unhashed_items.empty());  // the hash is kept: the file did not change
    const SourceItem after = named(L"a.dng");
    EXPECT_EQ(after.id, before.id);
    EXPECT_EQ(after.support.status, SupportStatus::CompatibleUnverified);
    EXPECT_FALSE(probe_outdated(after));
    EXPECT_EQ(after.flags, (ItemFlags{PickFlag::Picked, 4}));
    EXPECT_EQ(after.sha256, before.sha256);
    EXPECT_EQ(after.identity, before.identity);
    EXPECT_EQ(after.source_state, SourceState::Present);

    // Renewed for good: the catalog stores the contract now, so the next check re-reads nothing.
    reopen();
    EXPECT_FALSE(probe_outdated(named(L"a.dng")));
    EXPECT_EQ(named(L"a.dng").support.status, SupportStatus::CompatibleUnverified);
    const RefreshReport again = refresh_all();
    EXPECT_EQ(again.outdated, 0u);
    EXPECT_TRUE(again.reprobed_items.empty());
    EXPECT_EQ(ts::file_sha256(path), *after.sha256);  // the original is untouched

    // With reprobe_outdated off, an outdated item is only checked.
    SourceItem stale_b = named(L"b.dng");
    stale_b.probe->contract.reset();
    ASSERT_TRUE(fixture_.catalog->write([&](catalog::WriteTransaction& tx) -> Status {
        auto record = tx.find_item(catalog::ItemId{static_cast<std::int64_t>(stale_b.id.value)});
        if (!record || !*record) {
            return fail(ErrorCode::NotFound, "the item is not in the catalog", "test");
        }
        const catalog::ItemMetadata metadata = detail::to_metadata(stale_b, catalog::Timestamp{});
        auto ids = tx.upsert_items((*record)->source_id, std::span(&metadata, 1));
        return ids ? ok() : Status(Unexpected(std::move(ids.error())));
    }));
    reopen();
    RefreshOptions options;
    options.reprobe_outdated = false;
    auto unchanged = refresh_sources(library(), *registry_, {}, options);
    ASSERT_TRUE(unchanged) << unchanged.error().describe();
    EXPECT_EQ(unchanged->outdated, 0u);
    EXPECT_TRUE(unchanged->reprobed_items.empty());
    EXPECT_TRUE(probe_outdated(named(L"b.dng")));
}

TEST_F(SourceMaintenanceTest, ContentThatChangedUnderTheSameIdentityIsFoundByVerification) {
    const auto path = ts::write_dng(dir_, L"in\\a.dng");
    import(dir_.path() / L"in");
    hash_all();
    const SourceItem item = named(L"a.dng");
    // Rewrite one byte and restore the modification time: the identity no longer notices.
    {
        HANDLE file =
            ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        ASSERT_NE(file, INVALID_HANDLE_VALUE);
        FILETIME created{}, accessed{}, written{};
        ASSERT_TRUE(::GetFileTime(file, &created, &accessed, &written));
        LARGE_INTEGER offset{};
        offset.QuadPart = 200;
        ASSERT_TRUE(::SetFilePointerEx(file, offset, nullptr, FILE_BEGIN));
        const char byte = 0x5a;
        DWORD done = 0;
        ASSERT_TRUE(::WriteFile(file, &byte, 1, &done, nullptr));
        ASSERT_TRUE(::SetFileTime(file, nullptr, nullptr, &written));
        ::CloseHandle(file);
    }
    ASSERT_EQ(*core::stat_file(path), item.identity);
    HashOptions verify;
    verify.verify_hashed = true;
    const HashReport report = hash_all(verify);
    EXPECT_EQ(report.changed, 1u);
    EXPECT_EQ(named(L"a.dng").source_state, SourceState::Changed);
}

TEST_F(SourceMaintenanceTest, PresenceChecksMarkMissingAndRestoreReturningFiles) {
    ts::write_dng(dir_, L"card\\a.dng");
    ts::write_dng(dir_, L"other\\b.dng", rts::SyntheticDngSpec{.model = "Synthetic DNA"});
    import(dir_.path() / L"card");
    import(dir_.path() / L"other");

    std::filesystem::rename(dir_.path() / L"card", dir_.path() / L"card-out");  // card removed
    RefreshReport report = refresh_all();
    EXPECT_EQ(report.files, 2u);
    EXPECT_EQ(report.missing, 1u);
    EXPECT_EQ(report.present, 1u);
    EXPECT_EQ(report.missing_items, std::vector<ItemId>{named(L"a.dng").id});
    EXPECT_EQ(named(L"a.dng").source_state, SourceState::Missing);
    EXPECT_EQ(report.unhashed_items, std::vector<ItemId>{named(L"b.dng").id});

    std::filesystem::rename(dir_.path() / L"card-out", dir_.path() / L"card");  // card back
    report = refresh_all();
    EXPECT_EQ(report.missing, 0u);
    EXPECT_EQ(report.restored, 1u);
    EXPECT_EQ(named(L"a.dng").source_state, SourceState::Present);
}

TEST_F(SourceMaintenanceTest, RelinkUpdatesPathsOnlyOnAHashMatch) {
    ts::write_dng(dir_, L"old\\a.dng");
    ts::write_dng(dir_, L"old\\sub\\b.dng", rts::SyntheticDngSpec{.model = "Synthetic DNB"});
    ts::write_dng(dir_, L"old\\c.dng", rts::SyntheticDngSpec{.model = "Synthetic DNC"});
    ts::write_dng(dir_, L"old\\never-hashed.dng", rts::SyntheticDngSpec{.model = "Synthetic DND"});
    import(dir_.path() / L"old");
    const SourceItem never_hashed = named(L"never-hashed.dng");
    // Hash all but never-hashed.dng.
    std::vector<ItemId> to_hash;
    for (const SourceItem& item : library().items()) {
        if (item.id != never_hashed.id) {
            to_hash.push_back(item.id);
        }
    }
    ASSERT_TRUE(hash_sources(library(), to_hash));
    const core::Digest256 a_hash = *named(L"a.dng").sha256;

    // The folder is copied into new\photos (new file objects: only the content can prove that a
    // file is the same photo) and removed; c.dng is replaced by a different file of the same name
    // and size; a decoy with a.dng's name and size but other content is found (and hashed) first.
    std::filesystem::create_directories(dir_.path() / L"new");
    std::filesystem::copy(dir_.path() / L"old", dir_.path() / L"new\\photos",
                          std::filesystem::copy_options::recursive);
    std::filesystem::remove_all(dir_.path() / L"old");
    ts::write_dng(dir_, L"new\\a-decoy\\a.dng", rts::SyntheticDngSpec{.model = "Synthetic DNX"});
    ts::write_dng(dir_, L"new\\photos\\c.dng", rts::SyntheticDngSpec{.model = "Synthetic DNY"});
    const RefreshReport missing = refresh_all();
    EXPECT_EQ(missing.missing, 4u);

    std::vector<MaintenanceProgress> progress;
    auto report = relink_sources(library(), RelinkRequest{.folder = dir_.path() / L"new"},
                                 [&](const MaintenanceProgress& p) { progress.push_back(p); });
    ASSERT_TRUE(report) << report.error().describe();
    EXPECT_EQ(report->files, 4u);
    EXPECT_EQ(report->relinked, 2u);           // a.dng and sub\b.dng
    EXPECT_EQ(report->unmatched, 2u);          // c.dng (content differs) and never-hashed.dng
    EXPECT_EQ(report->unverifiable, 1u);       // never-hashed.dng
    EXPECT_EQ(report->candidates_hashed, 4u);  // the decoy, a.dng, b.dng and the new c.dng
    EXPECT_FALSE(progress.empty());

    const SourceItem a = named(L"a.dng");
    EXPECT_EQ(a.source_state, SourceState::Present);
    EXPECT_EQ(a.path.parent_path().filename(), L"photos");  // not the decoy
    EXPECT_EQ(*a.sha256, a_hash);
    EXPECT_EQ(named(L"b.dng").source_state, SourceState::Present);
    EXPECT_EQ(named(L"c.dng").source_state, SourceState::Missing);
    EXPECT_EQ(named(L"never-hashed.dng").source_state, SourceState::Missing);
    EXPECT_EQ(named(L"c.dng").path.parent_path().filename(), L"old");  // unchanged

    // Relinked files are found where they are now.
    EXPECT_EQ(refresh_all().missing, 2u);
}

// A missing file's own version (same volume serial, file id, size and modification time: moved
// within its volume, or a card back under another drive letter) is relinked without reading it,
// also when it was never hashed; its items keep their ids and flags.
TEST_F(SourceMaintenanceTest, RelinkFollowsTheSameFileVersionWithoutReadingIt) {
    ts::write_dng(dir_, L"cardE\\DCIM\\IMG_0001.dng");
    ts::write_dng(dir_, L"cardE\\DCIM\\IMG_0002.dng", rts::SyntheticDngSpec{.model = "Synthetic DNB"});
    import(dir_.path() / L"cardE");
    const SourceItem unhashed = named(L"IMG_0001.dng");
    ASSERT_TRUE(library().set_flags(unhashed.id, ItemFlags{PickFlag::Picked, 5}));
    const ItemId second = named(L"IMG_0002.dng").id;
    ASSERT_TRUE(hash_sources(library(), std::vector<ItemId>{second}));
    const core::Digest256 second_hash = *named(L"IMG_0002.dng").sha256;

    std::filesystem::rename(dir_.path() / L"cardE", dir_.path() / L"cardF");  // another drive letter
    EXPECT_EQ(refresh_all().missing, 2u);
    auto report = relink_sources(library(), RelinkRequest{.folder = dir_.path() / L"cardF"});
    ASSERT_TRUE(report) << report.error().describe();
    EXPECT_EQ(report->relinked, 2u);
    EXPECT_EQ(report->unverifiable, 0u);
    EXPECT_EQ(report->candidates_hashed, 0u);  // proven by identity: nothing was read

    const SourceItem first = named(L"IMG_0001.dng");
    EXPECT_EQ(first.id, unhashed.id);
    EXPECT_EQ(first.source_state, SourceState::Present);
    EXPECT_EQ(first.path.parent_path().parent_path().filename(), L"cardF");
    EXPECT_EQ(first.flags, (ItemFlags{PickFlag::Picked, 5}));
    EXPECT_FALSE(first.sha256);  // still to be hashed (the maintenance service queues it)
    EXPECT_EQ(named(L"IMG_0002.dng").sha256, second_hash);
    EXPECT_EQ(refresh_all().missing, 0u);
}

TEST_F(SourceMaintenanceTest, RelinkOfASelectionAndOfAMissingFolder) {
    ts::write_dng(dir_, L"old\\a.dng");
    import(dir_.path() / L"old");
    hash_all();
    auto missing_folder = relink_sources(library(), RelinkRequest{.folder = dir_.path() / L"nowhere"});
    ASSERT_FALSE(missing_folder);
    EXPECT_EQ(missing_folder.error().code, ErrorCode::NotFound);

    // Nothing is missing: nothing to do.
    auto nothing = relink_sources(library(), RelinkRequest{.folder = dir_.path()});
    ASSERT_TRUE(nothing);
    EXPECT_EQ(nothing->files, 0u);
}

TEST_F(SourceMaintenanceTest, HashingCachesTheCameraThumbnailOfEachItem) {
    ts::write_dng(dir_, L"in\\with-preview.dng", ts::dng_with_preview());
    ts::write_dng(dir_, L"in\\without.dng");
    import(dir_.path() / L"in");
    auto cache = ThumbnailCache::open(ThumbnailCacheOptions{dir_.path() / L"cache\\thumbnails"});
    ASSERT_TRUE(cache);
    HashOptions options;
    options.thumbnails = std::shared_ptr<ThumbnailCache>(std::move(*cache));
    const HashReport report = hash_all(options);
    EXPECT_EQ(report.hashed, 2u);
    EXPECT_EQ(report.thumbnails_cached, 1u);
    const SourceItem item = named(L"with-preview.dng");
    const std::optional<Thumbnail> thumbnail =
        options.thumbnails->find(camera_thumbnail_key(*item.sha256, 0));
    ASSERT_TRUE(thumbnail);
    EXPECT_EQ(thumbnail->width, 96);
    EXPECT_EQ(thumbnail->height, 64);
    EXPECT_FALSE(options.thumbnails->find(camera_thumbnail_key(*named(L"without.dng").sha256, 0)));
}

// A camera's full-size preview is reduced for the thumbnail cache (a few hundred KB instead of
// megabytes); a preview that is small already is cached as it is; what is not a JPEG is not cached.
TEST_F(SourceMaintenanceTest, CameraThumbnailsAreReducedForTheCache) {
    exporter::OutputImage image;
    image.width = 4000;
    image.height = 3000;
    image.samples8.resize(static_cast<std::size_t>(image.width) * image.height * 3);
    for (std::int32_t y = 0; y < image.height; ++y) {
        for (std::int32_t x = 0; x < image.width; ++x) {
            std::uint8_t* p = image.samples8.data() + (static_cast<std::size_t>(y) * image.width + x) * 3;
            p[0] = static_cast<std::uint8_t>(x * 255 / image.width);
            p[1] = static_cast<std::uint8_t>(y * 255 / image.height);
            p[2] = static_cast<std::uint8_t>(((x / 8 + y / 8) % 2) * 160 + (x * y) % 13);  // texture
        }
    }
    const auto jpeg_path = dir_.path() / L"preview.jpg";
    auto written = exporter::write_verified(image, jpeg_path, exporter::ExportSettings::jpeg_srgb_q95());
    ASSERT_TRUE(written) << written.error().describe();
    const std::vector<std::byte> large = rts::read_all(jpeg_path);

    raw::EmbeddedPreview preview;
    preview.format = raw::EmbeddedPreviewFormat::Jpeg;
    preview.bytes = large;
    preview.width = 4000;
    preview.height = 3000;
    const std::optional<Thumbnail> reduced = camera_thumbnail_entry(preview);
    ASSERT_TRUE(reduced);
    EXPECT_EQ(reduced->width, kCachedCameraPreviewEdge);
    EXPECT_EQ(reduced->height, 768);
    EXPECT_LT(reduced->jpeg.size(), large.size() / 8);
    EXPECT_EQ(reduced->jpeg[0], std::byte{0xFF});
    EXPECT_EQ(reduced->jpeg[1], std::byte{0xD8});

    preview.bytes = rts::jpeg_bytes(rts::kJpegRgb96x64);
    preview.width = 96;
    preview.height = 64;
    const std::optional<Thumbnail> small = camera_thumbnail_entry(preview);
    ASSERT_TRUE(small);
    EXPECT_EQ(small->jpeg, preview.bytes);
    EXPECT_EQ(small->width, 96);

    preview.bytes = std::vector<std::byte>(100, std::byte{0x42});
    EXPECT_FALSE(camera_thumbnail_entry(preview));
}

TEST_F(SourceMaintenanceTest, CancellationStopsBeforeTheNextFile) {
    ts::write_dng(dir_, L"in\\a.dng");
    ts::write_dng(dir_, L"in\\b.dng", rts::SyntheticDngSpec{.model = "Synthetic DNA"});
    import(dir_.path() / L"in");
    core::CancellationSource cancel;
    cancel.cancel();
    auto report = hash_sources(library(), {}, {}, {}, cancel.token());
    ASSERT_TRUE(report);
    EXPECT_TRUE(report->cancelled);
    EXPECT_EQ(report->hashed, 0u);
}

TEST_F(SourceMaintenanceTest, ALibraryWithoutSourceFactsIsRefused) {
    SessionLibrary session;
    auto hashed = hash_sources(session, {});
    ASSERT_FALSE(hashed);
    EXPECT_EQ(hashed.error().code, ErrorCode::Unsupported);
    auto refreshed = refresh_sources(session, *registry_, {});
    ASSERT_FALSE(refreshed);
    EXPECT_EQ(refreshed.error().code, ErrorCode::Unsupported);
}

}  // namespace
}  // namespace lucent::app
