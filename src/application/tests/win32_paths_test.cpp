// Private Win32 path helpers: canonical paths through junctions and case differences, case
// folding, containment, sorted listings, long paths and free space.

#include "win32_paths.hpp"

#include <cwctype>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "app_test_support.hpp"

namespace lucent::app::detail {
namespace {

namespace ts = test_support;

TEST(Win32Paths, CanonicalPathResolvesCaseAndJunctions) {
    ts::ScratchDir dir;
    const auto file = dir.write_text(L"Real\\Photo.CR3", "x");
    auto exact = canonical_path(file, "test");
    ASSERT_TRUE(exact) << exact.error().describe();

    // Different case of every component resolves to the on-disk spelling.
    std::wstring upper = file.wstring();
    for (wchar_t& c : upper) {
        c = static_cast<wchar_t>(::towupper(c));
    }
    auto from_upper = canonical_path(upper, "test");
    ASSERT_TRUE(from_upper);
    EXPECT_EQ(from_upper->native(), exact->native());

    // Through a junction.
    const auto link = dir.path() / L"Link";
    ASSERT_TRUE(ts::create_junction(link, dir.path() / L"Real"));
    auto through_link = canonical_path(link / L"photo.cr3", "test");
    ASSERT_TRUE(through_link) << through_link.error().describe();
    EXPECT_EQ(through_link->native(), exact->native());
    ts::remove_junction(link);

    auto missing = canonical_path(dir.path() / L"missing.cr3", "test");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
    EXPECT_EQ(canonical_path(std::filesystem::path{}, "test").error().code, ErrorCode::InvalidArgument);
}

TEST(Win32Paths, LongPathsWork) {
    ts::ScratchDir dir;
    std::filesystem::path relative;
    while (dir.path().native().size() + relative.native().size() < 300) {
        relative /= L"a-rather-long-folder-name";
    }
    const std::filesystem::path deep = dir.path() / relative;
    const auto file = dir.write_text(relative / L"photo.dng", "x");
    auto canonical = canonical_path(file, "test");
    ASSERT_TRUE(canonical) << canonical.error().describe();
    EXPECT_GT(canonical->native().size(), 300u);
    EXPECT_FALSE(canonical->native().starts_with(L"\\\\?\\"));
    auto entries = list_directory(deep, "test");
    ASSERT_TRUE(entries) << entries.error().describe();
    ASSERT_EQ(entries->size(), 1u);
    EXPECT_TRUE(detail::is_directory(deep));
}

TEST(Win32Paths, FoldingAndContainment) {
    EXPECT_EQ(fold_case(L"img_0001.Cr3"), L"IMG_0001.CR3");
    EXPECT_EQ(fold_case(L"\u00e9t\u00e9"), L"\u00c9T\u00c9");
    EXPECT_TRUE(same_path_text(L"C:\\Photos\\A.jpg", L"c:\\photos\\a.JPG"));

    EXPECT_TRUE(is_same_or_within(L"C:\\In\\Out", L"c:\\in\\out"));
    EXPECT_TRUE(is_same_or_within(L"C:\\In\\Out\\x.jpg", L"C:\\In\\Out"));
    EXPECT_TRUE(is_same_or_within(L"C:\\In\\Out\\x.jpg", L"C:\\In\\Out\\"));
    EXPECT_FALSE(is_same_or_within(L"C:\\In\\Outside\\x.jpg", L"C:\\In\\Out"));  // component boundary
    EXPECT_FALSE(is_same_or_within(L"C:\\In", L"C:\\In\\Out"));
    EXPECT_TRUE(is_same_or_within(L"C:\\anything", L"C:\\"));
    EXPECT_FALSE(is_same_or_within(L"C:\\x", L""));
}

TEST(Win32Paths, ListingIsSortedByFoldedName) {
    ts::ScratchDir dir;
    dir.write_text(L"b.cr3", "x");
    dir.write_text(L"A.cr3", "x");
    dir.write_text(L"c\\inner.cr3", "x");
    auto entries = list_directory(dir.path(), "test");
    ASSERT_TRUE(entries) << entries.error().describe();
    ASSERT_EQ(entries->size(), 3u);
    EXPECT_EQ((*entries)[0].name, L"A.cr3");
    EXPECT_EQ((*entries)[1].name, L"b.cr3");
    EXPECT_EQ((*entries)[2].name, L"c");
    EXPECT_TRUE((*entries)[2].directory);
    EXPECT_FALSE((*entries)[0].directory);

    auto missing = list_directory(dir.path() / L"missing", "test");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
}

TEST(Win32Paths, FreeSpace) {
    ts::ScratchDir dir;
    auto free_bytes = free_disk_bytes(dir.path(), "test");
    ASSERT_TRUE(free_bytes) << free_bytes.error().describe();
    EXPECT_GT(*free_bytes, 0u);
    EXPECT_FALSE(free_disk_bytes(L"Q:\\surely\\missing", "test"));
}

}  // namespace
}  // namespace lucent::app::detail
