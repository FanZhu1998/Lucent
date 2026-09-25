// Unit tests for FileSource (RandomAccessSource over core::ReadOnlyFile).

#include <array>
#include <cstddef>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include <lucent/core/file.hpp>
#include <lucent/raw/file_source.hpp>
#include <lucent/raw/tiff_inspector.hpp>

#include "raw_test_support.hpp"
#include "synthetic_dng.hpp"

namespace lucent::raw {
namespace {

using test_support::ScratchDir;

std::vector<std::byte> numbered(std::size_t size) {
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>(i * 7 + 3);
    }
    return bytes;
}

TEST(FileSource, ReadsExactlyTheRequestedRange) {
    ScratchDir dir;
    const auto bytes = numbered(1000);
    const auto path = dir.write(L"data.bin", bytes);
    auto file = core::ReadOnlyFile::open(path);
    ASSERT_TRUE(file) << file.error().describe();
    auto source = FileSource::create(*file);
    ASSERT_TRUE(source) << source.error().describe();
    EXPECT_EQ(source->size(), 1000u);

    std::array<std::byte, 64> buffer{};
    auto read = source->read_at(100, buffer);
    ASSERT_TRUE(read);
    EXPECT_EQ(*read, 64u);
    EXPECT_TRUE(std::equal(buffer.begin(), buffer.end(), bytes.begin() + 100));

    auto tail = source->read_at(990, buffer);  // 10 bytes remain
    ASSERT_TRUE(tail);
    EXPECT_EQ(*tail, 10u);
    EXPECT_TRUE(std::equal(buffer.begin(), buffer.begin() + 10, bytes.begin() + 990));

    auto past = source->read_at(1000, buffer);
    ASSERT_TRUE(past);
    EXPECT_EQ(*past, 0u);
    auto beyond = source->read_at(UINT64_MAX, buffer);
    ASSERT_TRUE(beyond);
    EXPECT_EQ(*beyond, 0u);
    auto empty = source->read_at(0, std::span<std::byte>());
    ASSERT_TRUE(empty);
    EXPECT_EQ(*empty, 0u);
}

TEST(FileSource, ShrunkenFileIsSourceChanged) {
    ScratchDir dir;
    const auto path = dir.write(L"shrinks.bin", numbered(4096));
    auto file = core::ReadOnlyFile::open(path);
    ASSERT_TRUE(file);
    auto source = FileSource::create(*file);
    ASSERT_TRUE(source);

    // ReadOnlyFile shares write access, so another writer can truncate the file.
    HANDLE writer =
        CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(writer, INVALID_HANDLE_VALUE);
    LARGE_INTEGER size{};
    size.QuadPart = 1024;
    ASSERT_TRUE(SetFilePointerEx(writer, size, nullptr, FILE_BEGIN));
    ASSERT_TRUE(SetEndOfFile(writer));
    CloseHandle(writer);

    std::array<std::byte, 128> buffer{};
    auto read = source->read_at(2000, buffer);
    ASSERT_FALSE(read);
    EXPECT_EQ(read.error().code, ErrorCode::SourceChanged);
    EXPECT_EQ(read.error().where, "raw.file_source.read");
}

TEST(FileSource, ClosedFileFails) {
    core::ReadOnlyFile closed;
    auto source = FileSource::create(closed);
    ASSERT_FALSE(source);
    EXPECT_EQ(source.error().code, ErrorCode::Internal);
}

TEST(FileSource, InspectorReadsADngFromDisk) {
    ScratchDir dir;
    test_support::SyntheticDngSpec spec;
    spec.endian = test_support::Endian::Big;
    const auto path = dir.write(L"inspect.dng", test_support::build_synthetic_dng(spec));
    auto file = core::ReadOnlyFile::open(path);
    ASSERT_TRUE(file);
    auto source = FileSource::create(*file);
    ASSERT_TRUE(source);
    auto summary = inspect_tiff(*source);
    ASSERT_TRUE(summary) << summary.error().describe();
    EXPECT_TRUE(summary->is_dng());
    ASSERT_NE(summary->primary_raw(), nullptr);
    EXPECT_EQ(summary->primary_raw()->width, 64u);
    EXPECT_EQ(summary->primary_raw()->height, 48u);
}

}  // namespace
}  // namespace lucent::raw
