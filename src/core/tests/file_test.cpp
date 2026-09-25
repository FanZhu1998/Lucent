#include "lucent/core/file.hpp"

#include <cstring>
#include <limits>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include "lucent/core/win32_error.hpp"

#include "test_support.hpp"

namespace lucent::core {
namespace {

using test_support::pseudo_random_bytes;
using test_support::TempDir;
using test_support::verbatim;
using test_support::write_file;

const wchar_t* const kUnicodeName = L"Ünïcødé 写真 📷.bin";

std::vector<std::byte> bytes_of(std::string_view text) {
    const auto view = std::as_bytes(std::span(text.data(), text.size()));
    return {view.begin(), view.end()};
}

TEST(ReadOnlyFile, ReadsUnicodePath) {
    const TempDir dir;
    const auto path = dir / kUnicodeName;
    const auto content = pseudo_random_bytes(1000, 1);
    ASSERT_TRUE(write_file(path, content));

    auto file = ReadOnlyFile::open(path);
    ASSERT_TRUE(file) << file.error().describe();
    EXPECT_TRUE(file->is_open());
    EXPECT_EQ(file->path(), path);

    const auto size = file->size();
    ASSERT_TRUE(size) << size.error().describe();
    EXPECT_EQ(*size, 1000u);

    std::vector<std::byte> buffer(1000);
    const auto read = file->read_at(0, buffer);
    ASSERT_TRUE(read) << read.error().describe();
    EXPECT_EQ(*read, 1000u);
    EXPECT_EQ(buffer, content);
}

TEST(ReadOnlyFile, ReadAtPositionsAndEndOfFile) {
    const TempDir dir;
    const auto path = dir / L"data.bin";
    const auto content = pseudo_random_bytes(1000, 2);
    ASSERT_TRUE(write_file(path, content));
    auto file = ReadOnlyFile::open(path, AccessHint::Random);
    ASSERT_TRUE(file) << file.error().describe();

    std::vector<std::byte> buffer(20);
    auto read = file->read_at(500, buffer);
    ASSERT_TRUE(read);
    EXPECT_EQ(*read, 20u);
    EXPECT_TRUE(std::equal(buffer.begin(), buffer.end(), content.begin() + 500));

    // Straddling end-of-file: a short read, the only case where fewer bytes are returned.
    read = file->read_at(990, buffer);
    ASSERT_TRUE(read);
    EXPECT_EQ(*read, 10u);
    EXPECT_TRUE(std::equal(buffer.begin(), buffer.begin() + 10, content.begin() + 990));

    // At and past end-of-file: zero bytes, not an error.
    read = file->read_at(1000, buffer);
    ASSERT_TRUE(read);
    EXPECT_EQ(*read, 0u);
    read = file->read_at(1'000'000'000, buffer);
    ASSERT_TRUE(read);
    EXPECT_EQ(*read, 0u);

    // Empty buffer.
    read = file->read_at(0, std::span<std::byte>());
    ASSERT_TRUE(read);
    EXPECT_EQ(*read, 0u);
}

TEST(ReadOnlyFile, RejectsOffsetsBeyondTheLargestFileOffset) {
    const TempDir dir;
    const auto path = dir / L"data.bin";
    ASSERT_TRUE(write_file(path, "abc"));
    auto file = ReadOnlyFile::open(path);
    ASSERT_TRUE(file);
    std::vector<std::byte> buffer(4);
    const auto read = file->read_at(std::numeric_limits<std::uint64_t>::max(), buffer);
    ASSERT_FALSE(read);
    EXPECT_EQ(read.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(read.error().where, "core.file.read");
}

TEST(ReadOnlyFile, ReadExactFailsAsCorruptOnTruncation) {
    const TempDir dir;
    const auto path = dir / L"short.bin";
    ASSERT_TRUE(write_file(path, "0123456789"));
    auto file = ReadOnlyFile::open(path);
    ASSERT_TRUE(file);

    std::vector<std::byte> buffer(4);
    ASSERT_TRUE(file->read_exact_at(6, buffer));
    EXPECT_EQ(buffer, bytes_of("6789"));

    const Status status = file->read_exact_at(8, buffer);
    ASSERT_FALSE(status);
    EXPECT_EQ(status.error().code, ErrorCode::Corrupt);
    EXPECT_NE(status.error().message.find("unexpected end of file"), std::string::npos);
}

TEST(ReadOnlyFile, MissingFileIsNotFound) {
    const TempDir dir;
    const auto missing = dir / L"missing.cr3";
    const auto file = ReadOnlyFile::open(missing);
    ASSERT_FALSE(file);
    EXPECT_EQ(file.error().code, ErrorCode::NotFound);
    EXPECT_EQ(file.error().native_code, ERROR_FILE_NOT_FOUND);
    EXPECT_EQ(file.error().where, "core.file.open");
    EXPECT_NE(file.error().message.find("missing.cr3"), std::string::npos);

    const auto in_missing_dir = ReadOnlyFile::open(dir / L"no-such-dir" / L"a.cr3");
    ASSERT_FALSE(in_missing_dir);
    EXPECT_EQ(in_missing_dir.error().code, ErrorCode::NotFound);
    EXPECT_EQ(in_missing_dir.error().native_code, ERROR_PATH_NOT_FOUND);
}

TEST(ReadOnlyFile, RejectsInvalidPaths) {
    const auto empty = ReadOnlyFile::open(std::filesystem::path());
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, ErrorCode::InvalidArgument);

    const TempDir dir;
    ASSERT_TRUE(write_file(dir / L"a.bin", "x"));
    // A NUL would truncate the name at the Win32 boundary and open "a.bin" instead.
    const std::filesystem::path with_nul = (dir / L"a.bin").native() + std::wstring(L"\0.evil", 6);
    const auto nul = ReadOnlyFile::open(with_nul);
    ASSERT_FALSE(nul);
    EXPECT_EQ(nul.error().code, ErrorCode::InvalidArgument);
    EXPECT_FALSE(stat_file(with_nul));
}

TEST(ReadOnlyFile, RejectsDirectoriesAndDevices) {
    const TempDir dir;
    const auto directory = ReadOnlyFile::open(dir.path());
    ASSERT_FALSE(directory);
    EXPECT_EQ(directory.error().code, ErrorCode::InvalidArgument);

    const auto device = ReadOnlyFile::open(L"NUL");
    ASSERT_FALSE(device);
    EXPECT_EQ(device.error().code, ErrorCode::Unsupported);
}

TEST(ReadOnlyFile, EmptyObjectReportsInternalError) {
    const ReadOnlyFile file;
    EXPECT_FALSE(file.is_open());
    std::vector<std::byte> buffer(1);
    EXPECT_EQ(file.size().error().code, ErrorCode::Internal);
    EXPECT_EQ(file.identity().error().code, ErrorCode::Internal);
    EXPECT_EQ(file.read_at(0, buffer).error().code, ErrorCode::Internal);
}

TEST(ReadOnlyFile, MoveTransfersOwnership) {
    const TempDir dir;
    const auto path = dir / L"a.bin";
    ASSERT_TRUE(write_file(path, "abc"));
    auto opened = ReadOnlyFile::open(path);
    ASSERT_TRUE(opened);

    ReadOnlyFile first = std::move(*opened);
    EXPECT_FALSE(opened->is_open());  // NOLINT(bugprone-use-after-move)
    ASSERT_TRUE(first.is_open());

    ReadOnlyFile second;
    second = std::move(first);
    EXPECT_FALSE(first.is_open());  // NOLINT(bugprone-use-after-move)
    ASSERT_TRUE(second.is_open());
    EXPECT_EQ(second.path(), path);
    EXPECT_EQ(*second.size(), 3u);

    second.close();
    EXPECT_FALSE(second.is_open());
    second.close();  // idempotent
}

TEST(ReadOnlyFile, ConcurrentReadersShareOneHandle) {
    const TempDir dir;
    const auto path = dir / L"shared.bin";
    const auto content = pseudo_random_bytes(256 * 1024, 3);
    ASSERT_TRUE(write_file(path, content));
    auto file = ReadOnlyFile::open(path, AccessHint::Random);
    ASSERT_TRUE(file);

    constexpr int kThreads = 4;
    constexpr std::size_t kBlock = 4096;
    std::vector<int> mismatches(kThreads, 0);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            std::vector<std::byte> buffer(kBlock);
            for (int round = 0; round < 50; ++round) {
                // Each thread walks a different offset sequence over the same handle.
                const std::size_t block =
                    (static_cast<std::size_t>(round) * 7 + static_cast<std::size_t>(t) * 13) %
                    (content.size() / kBlock);
                const auto read = file->read_at(block * kBlock, buffer);
                if (!read || *read != kBlock ||
                    !std::equal(buffer.begin(), buffer.end(),
                                content.begin() + static_cast<std::ptrdiff_t>(block * kBlock))) {
                    ++mismatches[static_cast<std::size_t>(t)];
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (int t = 0; t < kThreads; ++t) {
        EXPECT_EQ(mismatches[static_cast<std::size_t>(t)], 0) << "thread " << t;
    }
}

TEST(ReadOnlyFile, NeverBlocksWritersOtherReadersOrRenames) {
    const TempDir dir;
    const auto path = dir / L"open.bin";
    ASSERT_TRUE(write_file(path, "original"));
    auto file = ReadOnlyFile::open(path);
    ASSERT_TRUE(file);

    auto second_reader = ReadOnlyFile::open(path);
    EXPECT_TRUE(second_reader) << second_reader.error().describe();

    const HANDLE writer =
        ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_NE(writer, INVALID_HANDLE_VALUE) << "a writer must not be blocked, error " << ::GetLastError();
    if (writer != INVALID_HANDLE_VALUE) {
        ::CloseHandle(writer);
    }

    second_reader = ReadOnlyFile();  // close it so only `file` holds the file during the rename
    const auto renamed = dir / L"renamed.bin";
    EXPECT_TRUE(::MoveFileExW(path.c_str(), renamed.c_str(), 0))
        << "rename blocked, error " << ::GetLastError();
    // The open handle still reads the (renamed) file.
    std::vector<std::byte> buffer(8);
    EXPECT_TRUE(file->read_exact_at(0, buffer));
    EXPECT_EQ(buffer, bytes_of("original"));
}

TEST(ReadOnlyFile, ReportsDeletionWhileOpen) {
    const TempDir dir;
    const auto path = dir / L"doomed.bin";
    ASSERT_TRUE(write_file(path, "bytes"));
    auto file = ReadOnlyFile::open(path);
    ASSERT_TRUE(file);
    auto pending = file->is_delete_pending();
    ASSERT_TRUE(pending) << pending.error().describe();
    EXPECT_FALSE(*pending);

    ASSERT_TRUE(::DeleteFileW(path.c_str())) << ::GetLastError();
    pending = file->is_delete_pending();
    ASSERT_TRUE(pending) << pending.error().describe();
    EXPECT_TRUE(*pending);

    // The open handle still reads the original data.
    std::vector<std::byte> buffer(5);
    EXPECT_TRUE(file->read_exact_at(0, buffer));
    EXPECT_EQ(buffer, bytes_of("bytes"));
}

TEST(ReadOnlyFile, ExclusiveHolderYieldsTransientIoError) {
    const TempDir dir;
    const auto path = dir / L"locked.bin";
    ASSERT_TRUE(write_file(path, "x"));
    const HANDLE exclusive =
        ::CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(exclusive, INVALID_HANDLE_VALUE);

    const auto file = ReadOnlyFile::open(path);
    ::CloseHandle(exclusive);
    ASSERT_FALSE(file);
    EXPECT_EQ(file.error().code, ErrorCode::IoError);
    EXPECT_EQ(file.error().native_code, ERROR_SHARING_VIOLATION);
    EXPECT_TRUE(is_transient_win32_error(static_cast<std::uint32_t>(file.error().native_code)));
}

TEST(FileIdentity, HandleAndPathAgree) {
    const TempDir dir;
    const auto path = dir / kUnicodeName;
    ASSERT_TRUE(write_file(path, "identity"));
    auto file = ReadOnlyFile::open(path);
    ASSERT_TRUE(file);

    const auto by_handle = file->identity();
    const auto by_path = stat_file(path);
    ASSERT_TRUE(by_handle) << by_handle.error().describe();
    ASSERT_TRUE(by_path) << by_path.error().describe();
    EXPECT_EQ(*by_handle, *by_path);
    EXPECT_TRUE(is_same_file(*by_handle, *by_path));
    EXPECT_EQ(by_path->size_bytes, 8u);
    EXPECT_GT(by_path->last_write_time, 0);
    EXPECT_NE(by_path->file_id, (std::array<std::uint8_t, 16>{}));

    const std::regex format(R"(volume=[0-9a-f]{16} id=[0-9a-f]{32} size=8 mtime=\d+)");
    EXPECT_TRUE(std::regex_match(to_string(*by_path), format)) << to_string(*by_path);
}

TEST(FileIdentity, DistinguishesFilesAndDetectsModification) {
    const TempDir dir;
    const auto a = dir / L"a.bin";
    const auto b = dir / L"b.bin";
    ASSERT_TRUE(write_file(a, "same"));
    ASSERT_TRUE(write_file(b, "same"));
    const auto id_a = stat_file(a);
    const auto id_b = stat_file(b);
    ASSERT_TRUE(id_a && id_b);
    EXPECT_FALSE(is_same_file(*id_a, *id_b));
    EXPECT_NE(*id_a, *id_b);

    // Appending changes the content version but not the file object.
    const HANDLE writer = ::CreateFileW(a.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(writer, INVALID_HANDLE_VALUE);
    DWORD written = 0;
    ASSERT_TRUE(::WriteFile(writer, "more", 4, &written, nullptr));
    ::CloseHandle(writer);
    const auto modified = stat_file(a);
    ASSERT_TRUE(modified);
    EXPECT_TRUE(is_same_file(*id_a, *modified));
    EXPECT_NE(*id_a, *modified);
    EXPECT_EQ(modified->size_bytes, 8u);
}

TEST(FileIdentity, SurvivesRename) {
    const TempDir dir;
    const auto before_path = dir / L"before.bin";
    const auto after_path = dir / L"after.bin";
    ASSERT_TRUE(write_file(before_path, "content"));
    const auto before = stat_file(before_path);
    ASSERT_TRUE(before);
    ASSERT_TRUE(::MoveFileExW(before_path.c_str(), after_path.c_str(), 0));
    const auto after = stat_file(after_path);
    ASSERT_TRUE(after);
    EXPECT_EQ(*before, *after);

    const auto gone = stat_file(before_path);
    ASSERT_FALSE(gone);
    EXPECT_EQ(gone.error().code, ErrorCode::NotFound);
    EXPECT_EQ(gone.error().where, "core.file.stat");
}

TEST(FileIdentity, StatWorksForDirectories) {
    const TempDir dir;
    const auto identity = stat_file(dir.path());
    ASSERT_TRUE(identity) << identity.error().describe();
    EXPECT_EQ(identity->size_bytes, 0u);
}

TEST(ReadFileBytes, ReadsWholeFileWithinLimit) {
    const TempDir dir;
    const auto path = dir / kUnicodeName;
    const auto content = pseudo_random_bytes(70'000, 4);
    ASSERT_TRUE(write_file(path, content));

    const auto exact = read_file_bytes(path, content.size());
    ASSERT_TRUE(exact) << exact.error().describe();
    EXPECT_EQ(*exact, content);

    const auto empty_path = dir / L"empty.bin";
    ASSERT_TRUE(write_file(empty_path, ""));
    const auto empty = read_file_bytes(empty_path, 0);
    ASSERT_TRUE(empty);
    EXPECT_TRUE(empty->empty());
}

TEST(ReadFileBytes, LargerThanLimitIsLimitExceeded) {
    const TempDir dir;
    const auto path = dir / L"big.bin";
    ASSERT_TRUE(write_file(path, pseudo_random_bytes(1001, 5)));
    const auto bytes = read_file_bytes(path, 1000);
    ASSERT_FALSE(bytes);
    EXPECT_EQ(bytes.error().code, ErrorCode::LimitExceeded);
    EXPECT_EQ(bytes.error().where, "core.file.read_file_bytes");
    EXPECT_NE(bytes.error().message.find("1001"), std::string::npos);
}

TEST(ReadFileBytes, MissingFileIsNotFound) {
    const TempDir dir;
    const auto bytes = read_file_bytes(dir / L"missing.bin", 100);
    ASSERT_FALSE(bytes);
    EXPECT_EQ(bytes.error().code, ErrorCode::NotFound);
}

TEST(LongPaths, WorkBeyondMaxPath) {
    const TempDir dir;
    // Build a directory chain whose file path is well beyond MAX_PATH (260).
    std::filesystem::path deep = dir.path();
    for (int level = 0; level < 8; ++level) {
        deep /= std::wstring(40, static_cast<wchar_t>(L'a' + level));
        ASSERT_TRUE(::CreateDirectoryW(verbatim(deep).c_str(), nullptr)) << ::GetLastError();
    }
    const auto path = deep / kUnicodeName;
    ASSERT_GT(path.native().size(), 330u);
    ASSERT_TRUE(write_file(path, "long path content"));

    auto file = ReadOnlyFile::open(path);
    ASSERT_TRUE(file) << file.error().describe();
    std::vector<std::byte> buffer(17);
    EXPECT_TRUE(file->read_exact_at(0, buffer));
    EXPECT_EQ(buffer, bytes_of("long path content"));

    const auto identity = stat_file(path);
    ASSERT_TRUE(identity) << identity.error().describe();
    EXPECT_EQ(*identity, *file->identity());

    const auto bytes = read_file_bytes(path, 1024);
    ASSERT_TRUE(bytes) << bytes.error().describe();
    EXPECT_EQ(bytes->size(), 17u);

    // A relative path containing ".." resolves the same way Win32 would resolve it.
    const auto dotted = deep / L".." / deep.filename() / kUnicodeName;
    const auto via_dots = stat_file(dotted);
    ASSERT_TRUE(via_dots) << via_dots.error().describe();
    EXPECT_EQ(*via_dots, *identity);
}

}  // namespace
}  // namespace lucent::core
