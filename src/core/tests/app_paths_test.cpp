#include "lucent/core/app_paths.hpp"

#include <array>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>
#include <windows.h>

#include "test_support.hpp"

namespace lucent::core {
namespace {

using test_support::TempDir;
using test_support::write_file;

bool same_path_ignoring_case(const std::filesystem::path& a, const std::filesystem::path& b) {
    const std::wstring& x = a.native();
    const std::wstring& y = b.native();
    return ::CompareStringOrdinal(x.c_str(), static_cast<int>(x.size()), y.c_str(),
                                  static_cast<int>(y.size()), TRUE) == CSTR_EQUAL;
}

TEST(AppPaths, LayoutUnderAnExplicitRoot) {
    const AppDirectories dirs = app_directories_at(L"D:\\Portable\\Lucent");
    EXPECT_EQ(dirs.root, std::filesystem::path(L"D:\\Portable\\Lucent"));
    EXPECT_EQ(dirs.data, std::filesystem::path(L"D:\\Portable\\Lucent\\data"));
    EXPECT_EQ(dirs.cache, std::filesystem::path(L"D:\\Portable\\Lucent\\cache"));
    EXPECT_EQ(dirs.logs, std::filesystem::path(L"D:\\Portable\\Lucent\\cache\\logs"));
}

TEST(AppPaths, LocalAppDataMatchesTheEnvironment) {
    auto folder = local_app_data_folder();
    ASSERT_TRUE(folder) << folder.error().describe();
    EXPECT_TRUE(folder->is_absolute());
    EXPECT_TRUE(std::filesystem::is_directory(*folder));

    std::array<wchar_t, 32768> env{};
    const DWORD length =
        ::GetEnvironmentVariableW(L"LOCALAPPDATA", env.data(), static_cast<DWORD>(env.size()));
    if (length > 0 && length < env.size()) {
        EXPECT_TRUE(
            same_path_ignoring_case(*folder, std::filesystem::path(std::wstring(env.data(), length))));
    }

    auto dirs = default_app_directories();
    ASSERT_TRUE(dirs) << dirs.error().describe();
    EXPECT_EQ(dirs->root, *folder / L"Lucent");
    EXPECT_EQ(dirs->cache, *folder / L"Lucent" / L"cache");
    // Deliberately not created here: tests must not write into the user's real profile.
}

TEST(AppPaths, CreatesEveryDirectoryAndToleratesDeletion) {
    TempDir temp;
    const AppDirectories dirs = app_directories_at(temp / L"Profile Ü" / L"Lucent");
    ASSERT_TRUE(create_app_directories(dirs));
    for (const auto& dir : {dirs.root, dirs.data, dirs.cache, dirs.logs}) {
        EXPECT_TRUE(std::filesystem::is_directory(dir));
    }
    ASSERT_TRUE(create_app_directories(dirs));  // idempotent

    // "The application must start after cache deletion" (blueprint section 8).
    std::filesystem::remove_all(dirs.cache);
    ASSERT_TRUE(create_app_directories(dirs));
    EXPECT_TRUE(std::filesystem::is_directory(dirs.logs));
}

TEST(AppPaths, CreateFailsWhenAFileOccupiesADirectoryName) {
    TempDir temp;
    const AppDirectories dirs = app_directories_at(temp / L"Lucent");
    ASSERT_TRUE(std::filesystem::create_directories(dirs.root));
    ASSERT_TRUE(write_file(dirs.cache, "not a directory"));
    auto status = create_app_directories(dirs);
    ASSERT_FALSE(status);
    EXPECT_EQ(status.error().code, ErrorCode::AlreadyExists);
}

}  // namespace
}  // namespace lucent::core
