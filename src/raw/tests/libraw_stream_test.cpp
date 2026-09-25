// White-box tests of the private LibRaw datastream (src/libraw_stream.hpp): it must reproduce the
// behaviour of LibRaw's own Windows file stream and record read failures and cancellation.

#include "libraw_stream.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include <lucent/core/cancellation.hpp>
#include <lucent/core/file.hpp>

#include "raw_test_support.hpp"

namespace lucent::raw::detail {
namespace {

using test_support::ScratchDir;

class LibRawStream : public ::testing::Test {
protected:
    // Opens a stream over a file holding `content`.
    LibRawFileStream& open(const std::string& content, core::CancellationToken cancel = {}) {
        path_ = dir_.write_text(L"stream.bin", content);
        auto file = core::ReadOnlyFile::open(path_);
        EXPECT_TRUE(file) << file.error().describe();
        file_ = std::move(*file);
        stream_ = std::make_unique<LibRawFileStream>(file_, content.size(), std::move(cancel));
        return *stream_;
    }

    ScratchDir dir_;
    std::filesystem::path path_;
    core::ReadOnlyFile file_;
    std::unique_ptr<LibRawFileStream> stream_;
};

TEST_F(LibRawStream, ReadReturnsCompleteItemsAndAdvancesByBytes) {
    LibRawFileStream& s = open("0123456789");
    std::array<char, 16> buffer{};
    EXPECT_EQ(s.read(buffer.data(), 1, 4), 4);
    EXPECT_EQ(std::string(buffer.data(), 4), "0123");
    EXPECT_EQ(s.tell(), 4);
    // 3 items of 4 bytes requested, 6 bytes left: one complete item, position advances by 6.
    EXPECT_EQ(s.read(buffer.data(), 4, 3), 1);
    EXPECT_EQ(std::string(buffer.data(), 6), "456789");
    EXPECT_EQ(s.tell(), 10);
    EXPECT_TRUE(s.read_past_end());
    EXPECT_EQ(s.eof(), 1);
    EXPECT_EQ(s.read(buffer.data(), 1, 1), 0);
    EXPECT_EQ(s.read(buffer.data(), 0, 5), 0);
    EXPECT_FALSE(s.read_error());
}

TEST_F(LibRawStream, SeekDoesNotClampAndGetCharReturnsZeroAtEnd) {
    LibRawFileStream& s = open("ABC");
    EXPECT_EQ(s.get_char(), 'A');
    EXPECT_EQ(s.seek(10, SEEK_SET), 0);
    EXPECT_EQ(s.tell(), 10);
    EXPECT_EQ(s.get_char(), 0);  // LibRaw's Windows stream returns 0, not EOF
    EXPECT_EQ(s.seek(-1, SEEK_END), 0);
    EXPECT_EQ(s.get_char(), 'C');
    EXPECT_EQ(s.seek(5, SEEK_END), 0);  // a positive offset lands exactly at the end
    EXPECT_EQ(s.tell(), 3);
    EXPECT_EQ(s.seek(-10, SEEK_CUR), 0);
    EXPECT_EQ(s.tell(), -7);
    EXPECT_EQ(s.get_char(), 0);
    std::array<char, 4> buffer{};
    EXPECT_EQ(s.read(buffer.data(), 1, 4), 0);  // negative positions read nothing
    EXPECT_EQ(s.size(), 3);
}

TEST_F(LibRawStream, GetsCopiesOneLineAndNeedsSizeBytesAvailable) {
    LibRawFileStream& s = open("first line\nsecond\nend");
    std::array<char, 32> line{};
    ASSERT_NE(s.gets(line.data(), 16), nullptr);
    EXPECT_STREQ(line.data(), "first line\n");
    EXPECT_EQ(s.tell(), 11);
    // Fewer than 16 bytes remain: LibRaw's buffered stream fails, and so does this one.
    EXPECT_EQ(s.gets(line.data(), 16), nullptr);
    ASSERT_NE(s.gets(line.data(), 5), nullptr);
    EXPECT_STREQ(line.data(), "seco");  // size - 1 characters, then one more is consumed
    EXPECT_EQ(s.tell(), 16);
    std::array<char, 1> one{'x'};
    EXPECT_EQ(s.gets(one.data(), 1), one.data());
    EXPECT_EQ(one[0], '\0');
    EXPECT_EQ(s.gets(one.data(), 0), nullptr);
}

TEST_F(LibRawStream, ScanfOneParsesAndSkipsToTheNextSeparator) {
    // As in LibRaw, the position moves from the token start to the next separator.
    LibRawFileStream& s = open("42 3.5 rest of the stream with enough bytes");
    int value = 0;
    EXPECT_EQ(s.scanf_one("%d", &value), 1);
    EXPECT_EQ(value, 42);
    EXPECT_EQ(s.tell(), 2);
    float real = 0;
    EXPECT_EQ(s.scanf_one("%f", &real), 1);
    EXPECT_FLOAT_EQ(real, 3.5f);
    EXPECT_EQ(s.tell(), 6);
    int bad = 7;
    EXPECT_EQ(s.scanf_one("%d", &bad), -1);  // "rest" is not a number
    EXPECT_EQ(bad, 7);
    s.seek(-10, SEEK_END);
    EXPECT_EQ(s.scanf_one("%d", &value), -1);  // fewer than 24 bytes remain
}

TEST_F(LibRawStream, LargeReadsBypassTheBuffer) {
    std::string content(LibRawFileStream::kBufferBytes * 3 + 17, '\0');
    for (std::size_t i = 0; i < content.size(); ++i) {
        content[i] = static_cast<char>(i * 13 + 5);
    }
    LibRawFileStream& s = open(content);
    std::vector<char> buffer(content.size());
    EXPECT_EQ(s.get_char(), static_cast<unsigned char>(content[0]));
    EXPECT_EQ(s.read(buffer.data(), 1, buffer.size()), static_cast<int>(content.size() - 1));
    EXPECT_EQ(std::memcmp(buffer.data(), content.data() + 1, content.size() - 1), 0);
    EXPECT_TRUE(s.read_past_end());
}

TEST_F(LibRawStream, CancellationStopsReading) {
    core::CancellationSource cancel;
    std::string content(LibRawFileStream::kBufferBytes + 100, 'x');
    LibRawFileStream& s = open(content, cancel.token());
    std::array<char, 8> buffer{};
    EXPECT_EQ(s.read(buffer.data(), 1, 8), 8);
    cancel.cancel();
    // The next read observes the cancellation.
    s.seek(static_cast<INT64>(LibRawFileStream::kBufferBytes + 10), SEEK_SET);
    EXPECT_EQ(s.read(buffer.data(), 1, 8), 0);
    EXPECT_TRUE(s.cancel_observed());
    s.seek(0, SEEK_SET);
    EXPECT_EQ(s.read(buffer.data(), 1, 8), 0);  // nothing more is read once cancelled
}

TEST_F(LibRawStream, ShrinkingFileIsRecordedAsSourceChanged) {
    std::string content(LibRawFileStream::kBufferBytes * 2, 'y');
    LibRawFileStream& s = open(content);
    HANDLE writer =
        CreateFileW(path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(writer, INVALID_HANDLE_VALUE);
    LARGE_INTEGER size{};
    size.QuadPart = 1000;
    ASSERT_TRUE(SetFilePointerEx(writer, size, nullptr, FILE_BEGIN));
    ASSERT_TRUE(SetEndOfFile(writer));
    CloseHandle(writer);

    std::array<char, 64> buffer{};
    s.seek(5000, SEEK_SET);
    EXPECT_EQ(s.read(buffer.data(), 1, buffer.size()), 0);
    ASSERT_TRUE(s.read_error());
    EXPECT_EQ(s.read_error()->code, ErrorCode::SourceChanged);
    EXPECT_FALSE(s.read_past_end());  // the stream's own size was not exceeded
}

}  // namespace
}  // namespace lucent::raw::detail
