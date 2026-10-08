// Import rules on synthetic DNG trees: recursion, extension hints and probing, duplicates,
// output / Lucent directory exclusion, junction loops, depth limit, multi-image files, support
// statuses, batching, progress, cancellation and failure paths.

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <format>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <lucent/application/import.hpp>
#include <lucent/core/hash.hpp>

#include "app_test_support.hpp"
#include "win32_paths.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;
namespace rts = raw::test_support;

class ImportTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto registry = raw::create_default_registry();
        ASSERT_TRUE(registry);
        registry_ = std::make_unique<raw::DecoderRegistry>(std::move(*registry));
    }
    void TearDown() override {
        for (const auto& link : junctions_) {
            ts::remove_junction(link);
        }
    }

    std::filesystem::path dng(const std::filesystem::path& name, const rts::SyntheticDngSpec& spec = {}) {
        return ts::write_dng(dir_, name, spec);
    }

    Expected<ImportReport> run(const ImportRequest& request, const ImportProgressCallback& progress = {},
                               core::CancellationToken cancel = {}) {
        return import_sources(library_, *registry_, request, progress, std::move(cancel));
    }

    void junction(const std::filesystem::path& link, const std::filesystem::path& target) {
        ASSERT_TRUE(ts::create_junction(link, target)) << "cannot create a junction";
        junctions_.push_back(link);
    }

    std::vector<std::wstring> item_names() const {
        std::vector<std::wstring> names;
        for (const SourceItem& item : library_.items()) {
            names.push_back(item.path.filename().wstring());
        }
        return names;
    }

    static std::size_t count(const ImportReport& report, SkipReason reason) {
        return static_cast<std::size_t>(
            std::count_if(report.skipped.begin(), report.skipped.end(),
                          [&](const SkippedPath& s) { return s.reason == reason; }));
    }

    ts::ScratchDir dir_;
    SessionLibrary library_;
    std::unique_ptr<raw::DecoderRegistry> registry_;
    std::vector<std::filesystem::path> junctions_;
};

TEST_F(ImportTest, FoldersImportTheirOwnFilesOrTheWholeTree) {
    dng(L"in\\b.dng");
    dng(L"in\\a.DNG");
    dng(L"in\\sub\\c.dng");
    dng(L"in\\sub\\deeper\\d.dng");

    auto flat = run(ImportRequest{.folders = {{dir_.path() / L"in", false}}});
    ASSERT_TRUE(flat) << flat.error().describe();
    EXPECT_EQ(flat->added.size(), 2u);
    EXPECT_EQ(item_names(), (std::vector<std::wstring>{L"a.DNG", L"b.dng"}));  // sorted discovery
    EXPECT_EQ(flat->compatible_unverified, 2u);

    auto tree = run(ImportRequest{.folders = {{dir_.path() / L"in", true}}});
    ASSERT_TRUE(tree) << tree.error().describe();
    EXPECT_EQ(tree->added.size(), 2u);  // c and d; a and b are duplicates
    EXPECT_EQ(count(*tree, SkipReason::Duplicate), 2u);
    for (const SkippedPath& s : tree->skipped) {
        if (s.reason == SkipReason::Duplicate) {
            EXPECT_TRUE(s.existing);
        }
    }
    EXPECT_EQ(tree->directories_scanned, 3u);
    EXPECT_EQ(library_.size(), 4u);

    const SourceItem first = library_.items().front();
    EXPECT_TRUE(first.path.is_absolute());
    EXPECT_EQ(first.support.status, SupportStatus::CompatibleUnverified);
    ASSERT_TRUE(first.probe);
    EXPECT_EQ(first.probe->stored_size, (image::Size{64, 48}));
    EXPECT_NE(first.identity.size_bytes, 0u);
}

TEST_F(ImportTest, ExtensionsAreHintsAndTheProbeDecides) {
    dng(L"in\\photo.dng");
    dng(L"in\\renamed.bin");  // a DNG under an unknown extension: its signature gets it probed
    dir_.write_text(L"in\\notes.xyz", "not an image at all, just text");
    dir_.write_text(L"in\\preview.jpg", "jpeg-ish");
    dir_.write_text(L"in\\sidecar.xmp", "<xmp/>");
    dir_.write_text(L"in\\fake.cr2", "a text file with a RAW extension");

    auto report = run(ImportRequest{.folders = {{dir_.path() / L"in", false}}});
    ASSERT_TRUE(report) << report.error().describe();
    EXPECT_EQ(report->ignored_non_raw, 3u);  // notes.xyz (no RAW signature), preview.jpg, sidecar.xmp
    EXPECT_EQ(report->files_seen, 6u);
    ASSERT_EQ(report->added.size(), 3u);
    EXPECT_EQ(report->compatible_unverified, 2u);
    EXPECT_EQ(report->unsupported, 1u);  // fake.cr2 stays listed, with the probe's reason

    const auto items = library_.items();
    const auto fake = std::find_if(items.begin(), items.end(),
                                   [](const SourceItem& i) { return i.path.filename() == L"fake.cr2"; });
    ASSERT_NE(fake, items.end());
    EXPECT_EQ(fake->support.status, SupportStatus::Unsupported);
    EXPECT_FALSE(fake->probe);
    ASSERT_EQ(fake->support.reasons.size(), 1u);
    EXPECT_EQ(fake->support.reasons[0].kind, SupportReasonKind::ProbeFailed);
    EXPECT_EQ(fake->support.reasons[0].error_code, ErrorCode::Unsupported);

    // Files selected explicitly (Add photos, drag and drop) are probed whatever their extension,
    // but companions with a non-RAW extension that are not RAW files are skipped, not listed.
    dng(L"sel\\scan.tif");  // a DNG saved as .tif: a RAW container signature, and it decodes
    dir_.write_text(L"sel\\export.tif", std::string("II*\0", 4) + std::string(200, 'x'));  // TIFF, not RAW
    dir_.write_text(L"sel\\odd.xyz", "unknown extension, no signature");
    auto explicit_files = run(ImportRequest{.files = {dir_.path() / L"in\\preview.jpg",
                                                      dir_.path() / L"in\\sidecar.xmp",
                                                      dir_.path() / L"sel\\scan.tif",
                                                      dir_.path() / L"sel\\export.tif",
                                                      dir_.path() / L"sel\\odd.xyz"}});
    ASSERT_TRUE(explicit_files) << explicit_files.error().describe();
    EXPECT_EQ(count(*explicit_files, SkipReason::NotRawPhoto), 3u);  // preview.jpg, sidecar.xmp, export.tif
    ASSERT_EQ(explicit_files->added.size(), 2u);
    EXPECT_EQ(explicit_files->compatible_unverified, 1u);  // scan.tif
    EXPECT_EQ(explicit_files->unsupported, 1u);  // odd.xyz: an unknown extension is listed with the reason
    for (const SkippedPath& skipped : explicit_files->skipped) {
        if (skipped.path.filename() == L"export.tif") {
            ASSERT_TRUE(skipped.error);  // the decoder's refusal
            EXPECT_EQ(skipped.reason, SkipReason::NotRawPhoto);
        }
    }
}

TEST_F(ImportTest, ClassifiesExtensions) {
    EXPECT_EQ(classify_extension(L"a.CR3"), ExtensionHint::Raw);
    EXPECT_EQ(classify_extension(L"a.nef"), ExtensionHint::Raw);
    EXPECT_EQ(classify_extension(L"a.Dng"), ExtensionHint::Raw);
    EXPECT_EQ(classify_extension(L"a.JPG"), ExtensionHint::NotRaw);
    EXPECT_EQ(classify_extension(L"a.tiff"), ExtensionHint::NotRaw);
    EXPECT_EQ(classify_extension(L"a.insv"), ExtensionHint::NotRaw);
    EXPECT_EQ(classify_extension(L"a.bin"), ExtensionHint::Unknown);
    EXPECT_EQ(classify_extension(L"noextension"), ExtensionHint::Unknown);
    EXPECT_EQ(classify_extension(L"a."), ExtensionHint::Unknown);
}

TEST_F(ImportTest, ListsTheRawExtensionsSortedAndClassifiedAsRaw) {
    const auto extensions = raw_extensions();
    ASSERT_FALSE(extensions.empty());
    EXPECT_TRUE(std::is_sorted(extensions.begin(), extensions.end()));
    EXPECT_EQ(std::adjacent_find(extensions.begin(), extensions.end()), extensions.end());
    for (const std::wstring_view extension : extensions) {
        EXPECT_FALSE(extension.empty());
        EXPECT_EQ(classify_extension(L"a." + std::wstring(extension)), ExtensionHint::Raw);
        for (const wchar_t c : extension) {
            EXPECT_FALSE(c >= L'A' && c <= L'Z');
        }
    }
    EXPECT_NE(std::find(extensions.begin(), extensions.end(), L"cr3"), extensions.end());
    EXPECT_NE(std::find(extensions.begin(), extensions.end(), L"dng"), extensions.end());
}

TEST_F(ImportTest, SameFileThroughDifferentPathsIsOneItem) {
    const auto file = dng(L"in\\one.dng");
    std::wstring upper = file.wstring();
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towupper(c)); });
    junction(dir_.path() / L"alias", dir_.path() / L"in");

    auto report = run(ImportRequest{.files = {file, upper, dir_.path() / L"alias\\one.dng"},
                                    .folders = {{dir_.path() / L"in", false}}});
    ASSERT_TRUE(report) << report.error().describe();
    EXPECT_EQ(report->added.size(), 1u);
    EXPECT_EQ(count(*report, SkipReason::Duplicate), 3u);
    EXPECT_EQ(library_.size(), 1u);
    // The item carries the canonical path, not the alias.
    auto canonical = detail::canonical_path(file, "test");
    ASSERT_TRUE(canonical);
    EXPECT_EQ(library_.items()[0].path, *canonical);
}

TEST_F(ImportTest, OutputFoldersBelowTheImportFolderAndLucentDirectoriesAreNotScanned) {
    dng(L"in\\keep.dng");
    dng(L"in\\exports\\nested_output.dng");  // output folder nested inside the input
    dng(L"in\\exports\\deeper\\also_output.dng");
    dng(L"in\\cache\\cached.dng");  // a Lucent directory
    dng(L"elsewhere\\out\\far_output.dng");
    dir_.write_text(L"in\\~lucent-0123456789abcdef.tmp", "publication temporary");
    junction(dir_.path() / L"in\\link-to-exports", dir_.path() / L"in\\exports");
    junction(dir_.path() / L"in\\link-to-far", dir_.path() / L"elsewhere\\out");

    ImportRequest request;
    request.folders = {{dir_.path() / L"in", true}};
    // Spelling differs: canonicalised.
    request.output_directories = {dir_.path() / L"IN\\EXPORTS", dir_.path() / L"elsewhere\\out"};
    request.lucent_directories = {dir_.path() / L"in\\cache"};
    auto report = run(request);
    ASSERT_TRUE(report) << report.error().describe();
    EXPECT_EQ(item_names(), (std::vector<std::wstring>{L"keep.dng"}));
    // The nested folder, the junction to it and the junction to the output folder elsewhere.
    EXPECT_EQ(count(*report, SkipReason::InOutputDirectory), 3u);
    EXPECT_EQ(count(*report, SkipReason::InLucentDirectory), 1u);
    EXPECT_EQ(count(*report, SkipReason::LucentTemporaryFile), 1u);

    // What the user selects directly is imported even inside an output folder: the folder itself
    // (it holds photos the user chose to add) and explicitly selected files.
    auto whole = run(ImportRequest{.folders = {{dir_.path() / L"in\\exports", true}},
                                   .output_directories = {dir_.path() / L"in\\exports"}});
    ASSERT_TRUE(whole) << whole.error().describe();
    EXPECT_EQ(whole->added.size(), 2u);
    EXPECT_EQ(count(*whole, SkipReason::InOutputDirectory), 0u);
    auto explicit_output = run(ImportRequest{.files = {dir_.path() / L"elsewhere\\out\\far_output.dng"},
                                             .output_directories = {dir_.path() / L"elsewhere\\out"}});
    ASSERT_TRUE(explicit_output) << explicit_output.error().describe();
    EXPECT_EQ(explicit_output->added.size(), 1u);

    // Lucent's own directories are never imported, whatever was selected.
    auto cache = run(ImportRequest{.files = {dir_.path() / L"in\\cache\\cached.dng"},
                                   .lucent_directories = {dir_.path() / L"in\\cache"}});
    ASSERT_TRUE(cache);
    EXPECT_TRUE(cache->added.empty());
    EXPECT_EQ(count(*cache, SkipReason::InLucentDirectory), 1u);
}

TEST_F(ImportTest, ExportingNextToTheOriginalsOrIntoAParentNeverHidesThem) {
    dng(L"trip\\a.dng");
    dng(L"trip\\day2\\b.dng");
    dir_.write_text(L"trip\\a_Lucent.jpg", "an export next to its original");

    // Exported into the photos' own folder, then the folder is added (with subfolders).
    auto same = run(ImportRequest{.folders = {{dir_.path() / L"trip", true}},
                                  .output_directories = {dir_.path() / L"trip"}});
    ASSERT_TRUE(same) << same.error().describe();
    EXPECT_EQ(same->added.size(), 2u);
    EXPECT_EQ(same->ignored_non_raw, 1u);  // the export, by its extension
    EXPECT_EQ(count(*same, SkipReason::InOutputDirectory), 0u);

    // Exported into a parent folder (for example Pictures), then a new photo is added.
    dng(L"trip\\day3\\c.dng");
    auto parent = run(ImportRequest{.folders = {{dir_.path() / L"trip\\day3", false}},
                                    .output_directories = {dir_.path()}});
    ASSERT_TRUE(parent) << parent.error().describe();
    EXPECT_EQ(parent->added.size(), 1u);
    dng(L"trip\\day4\\d.dng");
    auto dropped = run(ImportRequest{.files = {dir_.path() / L"trip\\day4\\d.dng"},
                                     .output_directories = {dir_.path() / L"trip"}});
    ASSERT_TRUE(dropped) << dropped.error().describe();
    EXPECT_EQ(dropped->added.size(), 1u);
    EXPECT_EQ(library_.size(), 4u);
}

TEST_F(ImportTest, AChangedFileIsProbedAgainAndRefreshedInPlace) {
    const auto path = dng(L"in\\a.dng");
    ASSERT_TRUE(run(ImportRequest{.folders = {{dir_.path() / L"in", false}}}));
    ASSERT_EQ(library_.size(), 1u);
    const SourceItem before = library_.items()[0];
    ASSERT_TRUE(library_.set_flags(before.id, ItemFlags{PickFlag::Picked, 3}));
    std::vector<LibraryChange> changes;
    Subscription subscription = library_.subscribe([&](const LibraryChange& c) { changes.push_back(c); });

    // The file is rewritten (a metadata editor, a re-copy from the card): its identity changes.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    rts::SyntheticDngSpec changed;
    changed.model = "Rewritten";
    changed.opcode_list3 = rts::opcode_list({{9, 0x01030000, 0, rts::filler(76)}});  // now Unsupported
    dir_.write(L"in\\a.dng", rts::build_synthetic_dng(changed));

    auto report = run(ImportRequest{.folders = {{dir_.path() / L"in", false}}});
    ASSERT_TRUE(report) << report.error().describe();
    EXPECT_TRUE(report->added.empty());
    EXPECT_EQ(report->refreshed, (std::vector<ItemId>{before.id}));
    EXPECT_EQ(count(*report, SkipReason::Duplicate), 0u);
    ASSERT_EQ(library_.size(), 1u);
    const SourceItem after = *library_.find(before.id);
    EXPECT_EQ(after.identity, *core::stat_file(path));
    EXPECT_NE(after.identity, before.identity);
    ASSERT_TRUE(after.probe);
    EXPECT_EQ(after.probe->camera_model, "Rewritten");
    EXPECT_EQ(after.support.status, SupportStatus::Unsupported);
    EXPECT_EQ(after.flags, (ItemFlags{PickFlag::Picked, 3}));  // the user's flags stay
    EXPECT_EQ(after.revision, before.revision + 2);            // flags, then the refresh
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].kind, LibraryChangeKind::Refreshed);
    EXPECT_EQ(changes[0].items, (std::vector<ItemId>{before.id}));

    // Unchanged since: a duplicate again, found without probing.
    auto again = run(ImportRequest{.files = {path}});
    ASSERT_TRUE(again);
    EXPECT_TRUE(again->refreshed.empty());
    EXPECT_EQ(count(*again, SkipReason::Duplicate), 1u);
    EXPECT_EQ(changes.size(), 1u);
}

TEST_F(ImportTest, JunctionLoopsAreScannedOnce) {
    dng(L"in\\a.dng");
    dng(L"in\\sub\\b.dng");
    junction(dir_.path() / L"in\\sub\\loop", dir_.path() / L"in");        // back to the root
    junction(dir_.path() / L"in\\sub\\again", dir_.path() / L"in\\sub");  // to itself

    auto report = run(ImportRequest{.folders = {{dir_.path() / L"in", true}}});
    ASSERT_TRUE(report) << report.error().describe();
    EXPECT_EQ(report->added.size(), 2u);
    EXPECT_EQ(count(*report, SkipReason::DirectoryCycle), 2u);
    EXPECT_EQ(report->directories_scanned, 2u);
    EXPECT_EQ(library_.size(), 2u);
}

TEST_F(ImportTest, DepthLimitIsReported) {
    dng(L"in\\l1\\l2\\l3\\deep.dng");
    dng(L"in\\l1\\shallow.dng");
    ImportRequest request;
    request.folders = {{dir_.path() / L"in", true}};
    request.max_depth = 1;
    auto report = run(request);
    ASSERT_TRUE(report) << report.error().describe();
    EXPECT_EQ(item_names(), (std::vector<std::wstring>{L"shallow.dng"}));
    EXPECT_EQ(count(*report, SkipReason::DepthLimit), 1u);
}

TEST_F(ImportTest, SupportStatusComesFromTheContractAssessment) {
    rts::SyntheticDngSpec mandatory;
    mandatory.opcode_list3 = rts::opcode_list({{9, 0x01030000, 0, rts::filler(76)}});  // GainMap, mandatory
    dng(L"in\\mandatory.dng", mandatory);
    rts::SyntheticDngSpec optional;
    optional.opcode_list1 = rts::opcode_list({{4, 0x01030000, 1, rts::filler(8)}});  // optional
    dng(L"in\\optional.dng", optional);
    dng(L"in\\plain.dng");

    auto report = run(ImportRequest{.folders = {{dir_.path() / L"in", false}}});
    ASSERT_TRUE(report) << report.error().describe();
    EXPECT_EQ(report->compatible_unverified, 1u);
    EXPECT_EQ(report->limited, 1u);
    EXPECT_EQ(report->unsupported, 1u);
    for (const SourceItem& item : library_.items()) {
        const std::wstring name = item.path.filename().wstring();
        if (name == L"mandatory.dng") {
            EXPECT_EQ(item.support.status, SupportStatus::Unsupported);
            EXPECT_EQ(item.support.interpretation, raw::InterpretationStatus::RequiresUnsupportedOperation);
            ASSERT_FALSE(item.support.reasons.empty());
            EXPECT_EQ(item.support.reasons[0].issue,
                      raw::InterpretationIssueKind::MandatoryOperationUnsupported);
        } else if (name == L"optional.dng") {
            EXPECT_EQ(item.support.status, SupportStatus::Limited);
        } else {
            EXPECT_EQ(item.support.status, SupportStatus::CompatibleUnverified);
            EXPECT_TRUE(item.support.reasons.empty());
        }
    }
}

TEST_F(ImportTest, MultiImageFilesListEveryPhotograph) {
    rts::SyntheticDngSpec two;
    two.raw_images = 2;
    dng(L"in\\two.dng", two);
    auto report = run(ImportRequest{.folders = {{dir_.path() / L"in", false}}});
    ASSERT_TRUE(report) << report.error().describe();
    const auto items = library_.items();
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].subimage, 0u);
    EXPECT_EQ(items[1].subimage, 1u);
    EXPECT_EQ(items[0].subimage_count, 2u);
    // Lucent cannot tell what two raw IFDs of one DNG are: never presented as supported.
    for (const SourceItem& item : items) {
        EXPECT_EQ(item.support.status, SupportStatus::Unsupported);
    }
}

TEST_F(ImportTest, CommitsInBatchesAndReportsProgress) {
    for (int i = 0; i < 5; ++i) {
        dng(std::format(L"in\\{}.dng", i));
    }
    std::vector<LibraryChange> changes;
    Subscription s = library_.subscribe([&](const LibraryChange& c) { changes.push_back(c); });
    std::vector<ImportProgress> progress;
    ImportRequest request;
    request.folders = {{dir_.path() / L"in", false}};
    request.batch_size = 2;
    auto report = run(request, [&](const ImportProgress& p) { progress.push_back(p); });
    ASSERT_TRUE(report) << report.error().describe();
    EXPECT_EQ(changes.size(), 3u);  // 2 + 2 + 1
    ASSERT_FALSE(progress.empty());
    EXPECT_EQ(progress.front().phase, ImportPhase::Scanning);
    EXPECT_EQ(progress.back().phase, ImportPhase::Done);
    EXPECT_EQ(progress.back().probed, 5u);
    EXPECT_EQ(progress.back().added, 5u);
    EXPECT_EQ(progress.back().candidates, 5u);
    EXPECT_TRUE(progress.back().current.empty());
    const auto probing = std::count_if(progress.begin(), progress.end(), [](const ImportProgress& p) {
        return p.phase == ImportPhase::Probing;
    });
    EXPECT_EQ(probing, 5);
}

TEST_F(ImportTest, CancellationStopsAndKeepsCommittedItems) {
    for (int i = 0; i < 4; ++i) {
        dng(std::format(L"in\\{}.dng", i));
    }
    core::CancellationSource before;
    before.cancel();
    auto none = run(ImportRequest{.folders = {{dir_.path() / L"in", false}}}, {}, before.token());
    ASSERT_TRUE(none);
    EXPECT_TRUE(none->cancelled);
    EXPECT_TRUE(none->added.empty());

    core::CancellationSource during;
    ImportRequest request;
    request.folders = {{dir_.path() / L"in", false}};
    request.batch_size = 1;
    auto some = run(
        request,
        [&](const ImportProgress& p) {
            if (p.phase == ImportPhase::Probing && p.probed == 2) {
                during.cancel();
            }
        },
        during.token());
    ASSERT_TRUE(some) << some.error().describe();
    EXPECT_TRUE(some->cancelled);
    EXPECT_EQ(some->added.size(), 2u);
    EXPECT_EQ(library_.size(), 2u);
}

TEST_F(ImportTest, ReportsMissingAndMisusedPaths) {
    const auto file = dng(L"in\\a.dng");
    auto report = run(ImportRequest{.files = {dir_.path() / L"missing.dng", dir_.path() / L"in"},
                                    .folders = {{dir_.path() / L"no-such-folder", true}, {file, false}}});
    ASSERT_TRUE(report) << report.error().describe();
    EXPECT_TRUE(report->added.empty());
    EXPECT_EQ(count(*report, SkipReason::NotFound), 2u);
    EXPECT_EQ(count(*report, SkipReason::NotAFileOrFolder), 2u);

    ImportRequest bad;
    bad.batch_size = 0;
    EXPECT_EQ(run(bad).error().code, ErrorCode::InvalidArgument);
}

TEST_F(ImportTest, NeverWritesToSources) {
    const auto file = dng(L"in\\a.dng");
    const auto before = core::hash_file(file, {});
    ASSERT_TRUE(before);
    ASSERT_TRUE(run(ImportRequest{.folders = {{dir_.path() / L"in", true}}}));
    const auto after = core::hash_file(file, {});
    ASSERT_TRUE(after);
    EXPECT_EQ(before->digest, after->digest);
    EXPECT_EQ(before->identity, after->identity);
}

TEST(ImportNames, AreStable) {
    EXPECT_EQ(to_string(SkipReason::Duplicate), "duplicate");
    EXPECT_EQ(to_string(SkipReason::DirectoryCycle), "directory_cycle");
    EXPECT_EQ(to_string(SkipReason::NotAFileOrFolder), "not_a_file_or_folder");
    EXPECT_EQ(to_string(SkipReason::NotRawPhoto), "not_raw_photo");
}

}  // namespace
}  // namespace lucent::app
