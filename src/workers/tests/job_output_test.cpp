// A job's output files: scratch directory, no-clobber move, cleanup (job_output.hpp).

#include <filesystem>
#include <string>

#include <gtest/gtest.h>
#include <windows.h>

#include <lucent/workers/job_output.hpp>

#include "raw_test_support.hpp"

namespace lucent::workers {
namespace {

namespace rts = lucent::raw::test_support;

bool path_exists(const std::filesystem::path& path) {
    return ::GetFileAttributesW(rts::verbatim(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string content(const std::filesystem::path& path) {
    std::string text;
    HANDLE file = ::CreateFileW(rts::verbatim(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                                nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return text;
    }
    char buffer[256];
    DWORD got = 0;
    while (::ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        text.append(buffer, got);
    }
    ::CloseHandle(file);
    return text;
}

void write(const std::filesystem::path& path, const std::string& text) {
    HANDLE file = ::CreateFileW(rts::verbatim(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    ASSERT_NE(file, INVALID_HANDLE_VALUE);
    DWORD written = 0;
    ASSERT_TRUE(::WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr));
    ::CloseHandle(file);
}

TEST(WorkerJobOutput, ScratchDirectoryIsNamedAfterTheTempPath) {
    EXPECT_EQ(job_scratch_directory(LR"(D:\out\~job-1.partial)"), std::filesystem::path(LR"(D:\out\~job-1.partial.parts)"));
}

TEST(WorkerJobOutput, ScratchDirectoryIsCreatedOnce) {
    rts::ScratchDir dir;
    const auto temp = dir.path() / L"job.partial";
    ASSERT_TRUE(create_job_scratch_directory(temp));
    EXPECT_TRUE(path_exists(job_scratch_directory(temp)));
    auto again = create_job_scratch_directory(temp);
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, ErrorCode::AlreadyExists);
    auto missing = create_job_scratch_directory(dir.path() / L"absent" / L"job.partial");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
}

TEST(WorkerJobOutput, MovesNeverReplaceAnything) {
    rts::ScratchDir dir;
    const auto from = dir.path() / L"from.jpg";
    const auto to = dir.path() / L"to.jpg";
    write(from, "new");
    write(to, "existing");
    auto refused = move_no_clobber(from, to);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::AlreadyExists);
    EXPECT_EQ(content(to), "existing");
    EXPECT_EQ(content(from), "new");

    const auto free_name = dir.path() / L"free.jpg";
    ASSERT_TRUE(move_no_clobber(from, free_name));
    EXPECT_FALSE(path_exists(from));
    EXPECT_EQ(content(free_name), "new");
}

TEST(WorkerJobOutput, RemovalTakesTheTempFileAndTheScratchDirectory) {
    rts::ScratchDir dir;
    const auto temp = dir.path() / L"job.partial";
    EXPECT_TRUE(remove_job_output(temp));  // nothing there: ok

    write(temp, "partial");
    ASSERT_TRUE(create_job_scratch_directory(temp));
    write(job_scratch_directory(temp) / L"~lucent-0123456789abcdef.tmp", "a");
    write(job_scratch_directory(temp) / L"output.jpg", "b");
    ::SetFileAttributesW(rts::verbatim(job_scratch_directory(temp) / L"output.jpg").c_str(), FILE_ATTRIBUTE_READONLY);
    ASSERT_TRUE(remove_job_output(temp));
    EXPECT_FALSE(path_exists(temp));
    EXPECT_FALSE(path_exists(job_scratch_directory(temp)));
}

TEST(WorkerJobOutput, SubdirectoriesAreNeverFollowedOrRemoved) {
    rts::ScratchDir dir;
    const auto temp = dir.path() / L"job.partial";
    ASSERT_TRUE(create_job_scratch_directory(temp));
    const auto inner = job_scratch_directory(temp) / L"inner";
    ASSERT_TRUE(::CreateDirectoryW(rts::verbatim(inner).c_str(), nullptr));
    write(inner / L"keep.txt", "keep");
    write(job_scratch_directory(temp) / L"output.tif", "x");
    auto removed = remove_job_scratch_directory(temp);
    EXPECT_FALSE(removed);
    EXPECT_TRUE(path_exists(inner / L"keep.txt"));
    EXPECT_FALSE(path_exists(job_scratch_directory(temp) / L"output.tif"));
}

TEST(WorkerJobOutput, LongPathsWork) {
    rts::ScratchDir dir;
    std::filesystem::path deep = dir.path();
    while (deep.native().size() < 300) {
        deep /= std::wstring(40, L'd');
        rts::make_directories(deep);
    }
    const auto temp = deep / L"job.partial";
    ASSERT_TRUE(create_job_scratch_directory(temp));
    write(job_scratch_directory(temp) / L"output.jpg", "x");
    ASSERT_TRUE(move_no_clobber(job_scratch_directory(temp) / L"output.jpg", temp));
    ASSERT_TRUE(remove_job_output(temp));
    EXPECT_FALSE(path_exists(temp));
    EXPECT_FALSE(path_exists(job_scratch_directory(temp)));
}

}  // namespace
}  // namespace lucent::workers
