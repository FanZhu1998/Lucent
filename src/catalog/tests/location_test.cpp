// Catalog location rules (network refusal) and path keys.

#include "lucent/catalog/location.hpp"

#include <chrono>
#include <filesystem>
#include <string>

#include "catalog_test_support.hpp"

namespace lucent::catalog {
namespace {

using namespace test;

TEST(CatalogLocation, LocalDisksAreAccepted) {
    LUCENT_EXPECT_OK(check_catalog_storage(StorageKind::Fixed, L"C:\\Users\\a\\lucent.catalog"));
    LUCENT_EXPECT_OK(check_catalog_storage(StorageKind::Removable, L"E:\\lucent.catalog"));
}

TEST(CatalogLocation, NetworkLocationsAreRefusedWithAClearReason) {
    const Status status = check_catalog_storage(StorageKind::Network, L"\\\\nas\\photos\\lucent.catalog");
    ASSERT_FALSE(status);
    EXPECT_EQ(status.error().code, ErrorCode::Unsupported);
    EXPECT_EQ(status.error().where, "catalog.location");
    EXPECT_NE(status.error().message.find("local disk"), std::string::npos) << status.error().message;
    EXPECT_NE(status.error().message.find("network"), std::string::npos) << status.error().message;
}

TEST(CatalogLocation, UnknownDriveTypesAreRefused) {
    const Status status = check_catalog_storage(StorageKind::Unknown, L"Q:\\lucent.catalog");
    ASSERT_FALSE(status);
    EXPECT_EQ(status.error().code, ErrorCode::Unsupported);
}

TEST(CatalogLocation, InvalidStorageKindIsInvalidArgument) {
    const Status status = check_catalog_storage(static_cast<StorageKind>(77), L"C:\\x.catalog");
    ASSERT_FALSE(status);
    EXPECT_EQ(status.error().code, ErrorCode::InvalidArgument);
}

// A UNC catalog path is classified lexically (no network I/O) and refused before anything is
// created or opened.
TEST(CatalogLocation, OpenRefusesUncPathsWithoutTouchingTheNetwork) {
    const auto start = std::chrono::steady_clock::now();
    Expected<std::unique_ptr<Catalog>> catalog =
        Catalog::open(options_for(L"\\\\lucent-test-server.invalid\\share\\Lucent\\lucent.catalog"));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    ASSERT_FALSE(catalog);
    EXPECT_EQ(catalog.error().code, ErrorCode::Unsupported) << catalog.error().describe();
    EXPECT_LT(elapsed, std::chrono::seconds(2));
}

TEST(CatalogLocation, RestoreRefusesUncPaths) {
    TempDir dir;
    Expected<RestoreReport> report =
        Catalog::restore(L"\\\\lucent-test-server.invalid\\share\\lucent.catalog", dir / L"backup.bak");
    ASSERT_FALSE(report);
    EXPECT_EQ(report.error().code, ErrorCode::Unsupported) << report.error().describe();
}

TEST(CatalogLocation, DefaultPathIsUnderLocalAppData) {
    Expected<std::filesystem::path> path = default_catalog_path();
    LUCENT_ASSERT_OK(path);
    EXPECT_TRUE(path->is_absolute());
    EXPECT_EQ(path->filename(), std::filesystem::path(L"lucent.catalog"));
    EXPECT_EQ(path->parent_path().filename(), std::filesystem::path(L"data"));
    EXPECT_EQ(path->parent_path().parent_path().filename(), std::filesystem::path(L"Lucent"));
    EXPECT_EQ(default_backup_directory(*path), path->parent_path() / L"backups");
}

TEST(CatalogPathKey, IgnoresCaseAndSeparatorSpelling) {
    const Expected<std::string> a = make_path_key(L"C:\\Photos\\2026\\IMG_0001.CR3");
    LUCENT_ASSERT_OK(a);
    EXPECT_EQ(*a, "C:\\PHOTOS\\2026\\IMG_0001.CR3");
    for (const wchar_t* spelling :
         {L"c:\\photos\\2026\\img_0001.cr3", L"C:/Photos/2026/IMG_0001.cr3",
          L"C:\\Photos\\\\2026\\.\\IMG_0001.CR3", L"C:\\Photos\\x\\..\\2026\\IMG_0001.CR3",
          L"\\\\?\\C:\\Photos\\2026\\IMG_0001.CR3"}) {
        const Expected<std::string> b = make_path_key(spelling);
        LUCENT_ASSERT_OK(b);
        EXPECT_EQ(*a, *b) << core::path_to_utf8_lossy(spelling);
    }
    EXPECT_NE(*a, *make_path_key(L"C:\\Photos\\2026\\IMG_0002.CR3"));
}

TEST(CatalogPathKey, TrailingSeparatorsAndRoots) {
    EXPECT_EQ(*make_path_key(L"C:\\Photos\\"), *make_path_key(L"C:\\photos"));
    EXPECT_EQ(*make_path_key(L"C:\\"), "C:\\");
    EXPECT_EQ(*make_path_key(L"\\\\server\\share\\a.dng"),
              *make_path_key(L"\\\\?\\UNC\\Server\\Share\\A.DNG"));
}

TEST(CatalogPathKey, UnicodeCaseFoldingButNoNormalisation) {
    // Cyrillic, Greek and Latin-1 letters fold like ASCII.
    EXPECT_EQ(*make_path_key(L"C:\\Фото\\été\\σ.dng"), *make_path_key(L"C:\\ФОТО\\ÉTÉ\\Σ.DNG"));
    // CJK and emoji have no case and survive unchanged.
    const Expected<std::string> cjk = make_path_key(L"C:\\写真\\📷.cr3");
    LUCENT_ASSERT_OK(cjk);
    EXPECT_EQ(*cjk, "C:\\写真\\📷.CR3");
    // "é" precomposed (NFC) and decomposed (NFD) are different names on NTFS.
    EXPECT_NE(*make_path_key(L"C:\\\u00E9.dng"), *make_path_key(L"C:\\e\u0301.dng"));
}

TEST(CatalogPathKey, RejectsRelativeEmptyAndInvalidPaths) {
    for (const std::filesystem::path& bad :
         {std::filesystem::path(), std::filesystem::path(L"photos\\a.cr3"),
          std::filesystem::path(L"\\photos\\a.cr3"), std::filesystem::path(L"C:photos\\a.cr3"),
          std::filesystem::path(std::wstring(L"C:\\a\0b", 6))}) {
        const Expected<std::string> key = make_path_key(bad);
        ASSERT_FALSE(key) << core::path_to_utf8_lossy(bad);
        EXPECT_EQ(key.error().code, ErrorCode::InvalidArgument);
        EXPECT_EQ(key.error().where, "catalog.path_key");
    }
    // An unpaired surrogate is a valid NTFS name but not valid UTF-16: refused rather than altered.
    const Expected<std::string> surrogate = make_path_key(std::wstring(L"C:\\a") + wchar_t(0xD800) + L".dng");
    ASSERT_FALSE(surrogate);
    EXPECT_EQ(surrogate.error().code, ErrorCode::InvalidArgument);
}

}  // namespace
}  // namespace lucent::catalog
