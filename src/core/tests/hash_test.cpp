#include "lucent/core/hash.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include "hash_internal.hpp"  // white-box seam: detail::hash_file_observed
#include "test_support.hpp"

namespace lucent::core {
namespace {

using test_support::pseudo_random_bytes;
using test_support::TempDir;
using test_support::write_file;

std::span<const std::byte> bytes_of(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

std::string hex_sha256(std::span<const std::byte> data) {
    const auto digest = sha256(data);
    return digest ? to_hex(*digest) : "error: " + digest.error().describe();
}

// FIPS 180-2 Appendix B known answers.
constexpr std::string_view kEmptyDigest = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr std::string_view kAbcDigest = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
constexpr std::string_view kTwoBlockMessage = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
constexpr std::string_view kTwoBlockDigest =
    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
constexpr std::string_view kMillionADigest =
    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";

TEST(Sha256, KnownAnswers) {
    EXPECT_EQ(hex_sha256({}), kEmptyDigest);
    EXPECT_EQ(hex_sha256(bytes_of("abc")), kAbcDigest);
    EXPECT_EQ(hex_sha256(bytes_of(kTwoBlockMessage)), kTwoBlockDigest);
    const std::string million(1'000'000, 'a');
    EXPECT_EQ(hex_sha256(bytes_of(million)), kMillionADigest);
}

TEST(Sha256, IncrementalUpdatesMatchOneShot) {
    // One million 'a' fed in uneven pieces, including empty ones.
    const std::string million(1'000'000, 'a');
    Sha256 hasher;
    std::size_t offset = 0;
    std::size_t piece = 0;
    while (offset < million.size()) {
        const std::size_t length = std::min(million.size() - offset, piece);
        hasher.update(bytes_of(std::string_view(million).substr(offset, length)));
        offset += length;
        piece = piece * 3 + 1;  // 0, 1, 4, 13, 40, ...
    }
    const auto digest = hasher.finish();
    ASSERT_TRUE(digest) << digest.error().describe();
    EXPECT_EQ(to_hex(*digest), kMillionADigest);
}

TEST(Sha256, FinishResetsForReuse) {
    Sha256 hasher;
    hasher.update(bytes_of("abc"));
    const auto first = hasher.finish();
    ASSERT_TRUE(first);
    EXPECT_EQ(to_hex(*first), kAbcDigest);

    const auto empty = hasher.finish();  // nothing added since the previous finish()
    ASSERT_TRUE(empty);
    EXPECT_EQ(to_hex(*empty), kEmptyDigest);

    hasher.update(bytes_of("ab"));
    hasher.update(bytes_of("c"));
    const auto again = hasher.finish();
    ASSERT_TRUE(again);
    EXPECT_EQ(*again, *first);
}

TEST(Sha256, MoveTransfersStateAndLeavesSourceReportingInternal) {
    Sha256 original;
    original.update(bytes_of("ab"));
    Sha256 moved = std::move(original);
    moved.update(bytes_of("c"));
    const auto digest = moved.finish();
    ASSERT_TRUE(digest);
    EXPECT_EQ(to_hex(*digest), kAbcDigest);

    original.update(bytes_of("ignored"));  // NOLINT(bugprone-use-after-move): documented as ignored
    const auto from_moved = original.finish();
    ASSERT_FALSE(from_moved);
    EXPECT_EQ(from_moved.error().code, ErrorCode::Internal);

    Sha256 assigned;
    assigned = std::move(moved);
    assigned.update(bytes_of("abc"));
    const auto reassigned = assigned.finish();
    ASSERT_TRUE(reassigned);
    EXPECT_EQ(to_hex(*reassigned), kAbcDigest);
}

TEST(Digest, HexRoundTrip) {
    const auto digest = digest_from_hex(kAbcDigest);
    ASSERT_TRUE(digest) << digest.error().describe();
    EXPECT_EQ(digest->front(), 0xba);
    EXPECT_EQ(digest->back(), 0xad);
    EXPECT_EQ(to_hex(*digest), kAbcDigest);

    // Upper case is accepted; to_hex() is canonical lower case.
    const auto upper = digest_from_hex("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
    ASSERT_TRUE(upper);
    EXPECT_EQ(*upper, *digest);
}

TEST(Digest, FromHexRejectsMalformedText) {
    const std::string valid(kAbcDigest);
    const std::string cases[] = {
        "",
        valid.substr(1),            // 63 digits
        valid + "0",                // 65 digits
        "0x" + valid.substr(2),     // prefix
        valid.substr(0, 63) + "g",  // non-hex digit
        " " + valid.substr(1),      // whitespace
    };
    for (const std::string& text : cases) {
        const auto digest = digest_from_hex(text);
        ASSERT_FALSE(digest) << text;
        EXPECT_EQ(digest.error().code, ErrorCode::InvalidArgument);
        EXPECT_EQ(digest.error().where, "core.hash.digest_from_hex");
    }
}

TEST(HashFile, MatchesInMemoryHashForAllChunkSizes) {
    const TempDir dir;
    const auto path = dir / L"Ünïcødé 写真 📷.bin";
    const auto content = pseudo_random_bytes(100'003, 11);
    ASSERT_TRUE(write_file(path, content));
    const std::string expected = hex_sha256(content);

    for (const std::size_t chunk :
         {std::size_t{7}, std::size_t{4096}, std::size_t{65536}, kDefaultHashChunkBytes}) {
        const auto result = hash_file(path, CancellationToken{}, chunk);
        ASSERT_TRUE(result) << "chunk " << chunk << ": " << result.error().describe();
        EXPECT_EQ(to_hex(result->digest), expected) << "chunk " << chunk;
    }

    const auto identity = stat_file(path);
    ASSERT_TRUE(identity);
    EXPECT_EQ(hash_file(path, CancellationToken{})->identity, *identity);
}

TEST(HashFile, MultiChunkDefaultSizeAndEmptyFile) {
    const TempDir dir;
    const auto big = dir / L"big.bin";
    const auto content = pseudo_random_bytes(3 * kDefaultHashChunkBytes + 123, 12);
    ASSERT_TRUE(write_file(big, content));
    const auto result = hash_file(big, CancellationToken{});
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_EQ(to_hex(result->digest), hex_sha256(content));
    EXPECT_EQ(result->identity.size_bytes, content.size());

    const auto empty = dir / L"empty.bin";
    ASSERT_TRUE(write_file(empty, ""));
    const auto empty_result = hash_file(empty, CancellationToken{});
    ASSERT_TRUE(empty_result) << empty_result.error().describe();
    EXPECT_EQ(to_hex(empty_result->digest), kEmptyDigest);
}

TEST(HashFile, RejectsInvalidChunkSizes) {
    const TempDir dir;
    const auto path = dir / L"a.bin";
    ASSERT_TRUE(write_file(path, "abc"));
    for (const std::size_t chunk : {std::size_t{0}, kMaxHashChunkBytes + 1}) {
        const auto result = hash_file(path, CancellationToken{}, chunk);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
    }
}

TEST(HashFile, MissingFileIsNotFound) {
    const TempDir dir;
    const auto result = hash_file(dir / L"missing.bin", CancellationToken{});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::NotFound);
}

TEST(HashFile, PreCancelledTokenReturnsCancelledBeforeOpening) {
    CancellationSource source;
    source.cancel();
    const TempDir dir;
    // The file does not exist: Cancelled (not NotFound) shows the token is checked first.
    const auto result = hash_file(dir / L"missing.bin", source.token());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::Cancelled);
    EXPECT_EQ(result.error().where, "core.hash.file");
}

TEST(HashFile, CancellationBetweenChunksStopsPromptly) {
    const TempDir dir;
    const auto path = dir / L"a.bin";
    ASSERT_TRUE(write_file(path, pseudo_random_bytes(64 * 1024, 13)));
    CancellationSource source;
    int chunks_after_cancel = 0;
    bool cancelled = false;
    const auto result = detail::hash_file_observed(path, source.token(), 4096, [&](std::uint64_t) {
        if (cancelled) {
            ++chunks_after_cancel;
        }
        source.cancel();
        cancelled = true;
    });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::Cancelled);
    EXPECT_EQ(chunks_after_cancel, 0);
}

// ---- Stable-read (SourceChanged) scenarios --------------------------------------------------
//
// Each test hashes a 64 KiB file in 4 KiB chunks and changes the file from the observer after
// the first chunk, i.e. while hash_file() holds it open. The observer runs on the hashing
// thread, so the sequence is fully deterministic.

constexpr std::size_t kFileSize = 64 * 1024;
constexpr std::size_t kChunk = 4096;
constexpr std::int64_t kTicksPerSecond = 10'000'000;

class StableRead : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = dir_ / L"source.cr3";
        content_ = pseudo_random_bytes(kFileSize, 21);
        ASSERT_TRUE(write_file(path_, content_));
    }

    // Runs hash_file_observed with `action` executed once, after the first chunk.
    Expected<FileHashResult> hash_with_change(const std::function<void()>& action) {
        bool done = false;
        return detail::hash_file_observed(path_, CancellationToken{}, kChunk, [&](std::uint64_t) {
            if (!done) {
                done = true;
                action();
            }
        });
    }

    static void expect_source_changed(const Expected<FileHashResult>& result) {
        ASSERT_FALSE(result) << "the change was not detected";
        EXPECT_EQ(result.error().code, ErrorCode::SourceChanged) << result.error().describe();
        EXPECT_EQ(result.error().where, "core.hash.file");
        EXPECT_NE(result.error().message.find("source file changed while it was being read"),
                  std::string::npos);
    }

    // Opens the source for writing alongside hash_file's read handle.
    HANDLE open_writer(DWORD access) const {
        return ::CreateFileW(path_.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    static bool set_last_write_time(const std::filesystem::path& path, std::int64_t ticks) {
        const HANDLE handle = ::CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return false;
        }
        FILETIME time;
        time.dwLowDateTime = static_cast<DWORD>(static_cast<std::uint64_t>(ticks) & 0xFFFF'FFFFu);
        time.dwHighDateTime = static_cast<DWORD>(static_cast<std::uint64_t>(ticks) >> 32);
        const bool ok = ::SetFileTime(handle, nullptr, nullptr, &time) != FALSE;
        ::CloseHandle(handle);
        return ok;
    }

    TempDir dir_;
    std::filesystem::path path_;
    std::vector<std::byte> content_;
};

TEST_F(StableRead, UnchangedFileSucceeds) {
    const auto result = hash_with_change([] {});
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_EQ(to_hex(result->digest), hex_sha256(content_));
}

TEST_F(StableRead, AppendDuringReadIsDetected) {
    bool changed = false;
    const auto result = hash_with_change([&] {
        const HANDLE writer = open_writer(FILE_APPEND_DATA);
        DWORD written = 0;
        changed = writer != INVALID_HANDLE_VALUE && ::WriteFile(writer, "tail", 4, &written, nullptr);
        ::CloseHandle(writer);
    });
    ASSERT_TRUE(changed);
    expect_source_changed(result);
    // Caught by the handle check, before the path is consulted.
    EXPECT_NE(result.error().message.find("it was modified"), std::string::npos) << result.error().message;
}

TEST_F(StableRead, TruncationDuringReadIsDetected) {
    bool changed = false;
    const auto result = hash_with_change([&] {
        const HANDLE writer = open_writer(GENERIC_WRITE);
        LARGE_INTEGER position{};
        position.QuadPart = 10'000;
        changed = writer != INVALID_HANDLE_VALUE &&
                  ::SetFilePointerEx(writer, position, nullptr, FILE_BEGIN) && ::SetEndOfFile(writer);
        ::CloseHandle(writer);
    });
    ASSERT_TRUE(changed);
    expect_source_changed(result);
    EXPECT_NE(result.error().message.find("truncated"), std::string::npos) << result.error().message;
}

TEST_F(StableRead, SameSizeRewriteWithNewTimestampIsDetected) {
    const auto before = stat_file(path_);
    ASSERT_TRUE(before);
    bool changed = false;
    const auto result = hash_with_change([&] {
        // Rewrite a block the hasher has not read yet, keeping the size. The timestamp is set
        // explicitly so the test does not depend on the clock advancing between writes.
        const HANDLE writer = open_writer(GENERIC_WRITE);
        LARGE_INTEGER position{};
        position.QuadPart = 32 * 1024;
        DWORD written = 0;
        const char patch[4] = {'e', 'd', 'i', 't'};
        changed = writer != INVALID_HANDLE_VALUE &&
                  ::SetFilePointerEx(writer, position, nullptr, FILE_BEGIN) &&
                  ::WriteFile(writer, patch, 4, &written, nullptr);
        ::CloseHandle(writer);
        changed = changed && set_last_write_time(path_, before->last_write_time + 10 * kTicksPerSecond);
    });
    ASSERT_TRUE(changed);
    expect_source_changed(result);
}

TEST_F(StableRead, ReplacementWithIdenticalSizeAndTimestampIsDetectedByFileId) {
    // The "atomic save" pattern used by editors: the original is renamed away (it is neither
    // modified nor deleted, so the handle checks pass) and a new file with the same bytes and
    // the same last write time takes its name. Only the file id differs, so this exercises the
    // re-stat-by-path check specifically.
    const auto before = stat_file(path_);
    ASSERT_TRUE(before);
    bool replaced = false;
    const auto result = hash_with_change([&] {
        replaced = ::MoveFileExW(path_.c_str(), (dir_ / L"source.cr3.bak").c_str(), 0) &&
                   write_file(path_, content_) && set_last_write_time(path_, before->last_write_time);
    });
    ASSERT_TRUE(replaced) << ::GetLastError();
    const auto replacement = stat_file(path_);
    ASSERT_TRUE(replacement);
    ASSERT_EQ(replacement->size_bytes, before->size_bytes);
    ASSERT_EQ(replacement->last_write_time, before->last_write_time);
    ASSERT_FALSE(is_same_file(*replacement, *before));
    expect_source_changed(result);
    EXPECT_NE(result.error().message.find("different or modified file"), std::string::npos)
        << result.error().message;
}

TEST_F(StableRead, DeletionDuringReadIsDetectedThroughTheHandle) {
    bool removed = false;
    const auto result = hash_with_change([&] { removed = ::DeleteFileW(path_.c_str()) != FALSE; });
    ASSERT_TRUE(removed) << ::GetLastError();
    expect_source_changed(result);
    EXPECT_NE(result.error().message.find("it was deleted"), std::string::npos) << result.error().message;
}

TEST_F(StableRead, RenameAwayDuringReadIsDetected) {
    bool renamed = false;
    const auto result = hash_with_change(
        [&] { renamed = ::MoveFileExW(path_.c_str(), (dir_ / L"moved.cr3").c_str(), 0) != FALSE; });
    ASSERT_TRUE(renamed) << ::GetLastError();
    expect_source_changed(result);
    EXPECT_NE(result.error().message.find("removed or renamed"), std::string::npos) << result.error().message;
}

}  // namespace
}  // namespace lucent::core
