#include "lucent/core/file_copy.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include "lucent/core/hash.hpp"
#include "lucent/core/storage.hpp"

#include "file_copy_internal.hpp"
#include "test_support.hpp"

namespace lucent::core {
namespace {

using test_support::pseudo_random_bytes;
using test_support::read_text;
using test_support::TempDir;
using test_support::write_file;

constexpr std::size_t kChunk = std::size_t{64} << 10;  // smallest legal chunk: many chunks per file

CopyOptions small_chunks(CopyNamePolicy policy = CopyNamePolicy::MakeUnique) {
    CopyOptions options;
    options.name_policy = policy;
    options.chunk_bytes = kChunk;
    options.free_space_reserve_bytes = 0;
    return options;
}

std::vector<std::filesystem::path> files_in(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> names;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        names.push_back(entry.path().filename());
    }
    std::sort(names.begin(), names.end());
    return names;
}

int count_temporary_files(const std::filesystem::path& directory) {
    int count = 0;
    for (const auto& name : files_in(directory)) {
        count += is_temporary_file_name(name) ? 1 : 0;
    }
    return count;
}

std::string as_string(const std::vector<std::byte>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

struct Fixture {
    TempDir temp;
    std::filesystem::path source_dir = temp / L"card DCIM ✓";
    std::filesystem::path library = temp / L"Library Фото";
    std::filesystem::path source;
    std::vector<std::byte> content;

    explicit Fixture(std::size_t size = 5 * kChunk + 1234, const wchar_t* name = L"IMG_0001.CR3") {
        std::filesystem::create_directories(source_dir);
        std::filesystem::create_directories(library);
        source = source_dir / name;
        content = pseudo_random_bytes(size, 1234);
        EXPECT_TRUE(write_file(source, content));
    }

    // The source is untouched: same bytes and the same identity as at the start.
    void expect_source_intact(const FileIdentity& identity) const {
        EXPECT_EQ(read_text(source), as_string(content));
        auto now = stat_file(source);
        ASSERT_TRUE(now);
        EXPECT_EQ(*now, identity);
    }
};

// ---- success paths ----------------------------------------------------------------------------

TEST(FileCopy, CopiesVerifiesAndReportsAReceipt) {
    Fixture f;
    auto identity = stat_file(f.source);
    ASSERT_TRUE(identity);

    std::vector<CopyProgress> events;
    auto receipt = copy_file_verified(f.source, f.library, {}, small_chunks(), {},
                                      [&](const CopyProgress& p) { events.push_back(p); });
    ASSERT_TRUE(receipt) << receipt.error().describe();

    EXPECT_EQ(receipt->final_path, f.library / L"IMG_0001.CR3");
    EXPECT_EQ(read_text(receipt->final_path), as_string(f.content));
    EXPECT_EQ(receipt->size_bytes, f.content.size());
    auto independent = sha256(f.content);
    ASSERT_TRUE(independent);
    EXPECT_EQ(receipt->sha256, *independent);
    EXPECT_EQ(receipt->source_identity, *identity);
    EXPECT_FALSE(is_same_file(receipt->copy_identity, receipt->source_identity));
    EXPECT_EQ(receipt->copy_identity.size_bytes, f.content.size());
    // The source's modification time is kept.
    EXPECT_EQ(receipt->copy_identity.last_write_time, identity->last_write_time);
    f.expect_source_intact(*identity);
    EXPECT_EQ(count_temporary_files(f.library), 0);

    // Progress: both phases start at 0, are monotonic and end at the size.
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.front().phase, CopyPhase::Copying);
    EXPECT_EQ(events.front().bytes_done, 0u);
    EXPECT_EQ(events.back().phase, CopyPhase::Verifying);
    EXPECT_EQ(events.back().bytes_done, f.content.size());
    std::uint64_t last = 0;
    CopyPhase phase = CopyPhase::Copying;
    int verify_starts = 0;
    for (const CopyProgress& p : events) {
        EXPECT_EQ(p.bytes_total, f.content.size());
        if (p.phase != phase) {
            phase = p.phase;
            last = 0;
            ++verify_starts;
            EXPECT_EQ(p.bytes_done, 0u);
        }
        EXPECT_GE(p.bytes_done, last);
        last = p.bytes_done;
    }
    EXPECT_EQ(verify_starts, 1);
}

TEST(FileCopy, CopiesEmptyAndSingleChunkFilesAndRenames) {
    Fixture empty(0, L"empty.dng");
    auto receipt = copy_file_verified(empty.source, empty.library, L"renamed — 写真.dng", small_chunks(), {});
    ASSERT_TRUE(receipt) << receipt.error().describe();
    EXPECT_EQ(receipt->final_path.filename(), std::filesystem::path(L"renamed — 写真.dng"));
    EXPECT_EQ(receipt->size_bytes, 0u);
    EXPECT_EQ(to_hex(receipt->sha256), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(read_text(receipt->final_path), "");

    Fixture exact(kChunk, L"exact.nef");
    receipt = copy_file_verified(exact.source, exact.library, {}, small_chunks(), {});
    ASSERT_TRUE(receipt) << receipt.error().describe();
    EXPECT_EQ(read_text(receipt->final_path), as_string(exact.content));

    Fixture tiny(3, L"tiny.arw");
    receipt = copy_file_verified(tiny.source, tiny.library, {}, CopyOptions{}, {});  // default 1 MiB chunks
    ASSERT_TRUE(receipt) << receipt.error().describe();
    EXPECT_EQ(read_text(receipt->final_path), as_string(tiny.content));
}

TEST(FileCopy, CopiesIntoADestinationBeyondMaxPath) {
    Fixture f(2 * kChunk + 7);
    std::filesystem::path deep = f.library;
    while (deep.native().size() < 300) {
        deep /= std::wstring(40, L'l');
    }
    ASSERT_TRUE(create_directory_tree(deep));
    auto receipt = copy_file_verified(f.source, deep, {}, small_chunks(), {});
    ASSERT_TRUE(receipt) << receipt.error().describe();
    EXPECT_EQ(read_text(receipt->final_path), as_string(f.content));
    EXPECT_GT(receipt->final_path.native().size(), static_cast<std::size_t>(MAX_PATH));
}

// ---- names ------------------------------------------------------------------------------------

TEST(FileCopy, UniqueCopyNames) {
    EXPECT_EQ(unique_copy_name(L"IMG_1.CR3", 0), std::filesystem::path(L"IMG_1.CR3"));
    EXPECT_EQ(unique_copy_name(L"IMG_1.CR3", 1), std::filesystem::path(L"IMG_1.CR3"));
    EXPECT_EQ(unique_copy_name(L"IMG_1.CR3", 2), std::filesystem::path(L"IMG_1_2.CR3"));
    EXPECT_EQ(unique_copy_name(L"a.b.CR3", 3), std::filesystem::path(L"a.b_3.CR3"));
    EXPECT_EQ(unique_copy_name(L"noext", 12), std::filesystem::path(L"noext_12"));
    EXPECT_EQ(unique_copy_name(L".hidden", 2), std::filesystem::path(L".hidden_2"));
    EXPECT_EQ(unique_copy_name(L"Фото.dng", 2), std::filesystem::path(L"Фото_2.dng"));
}

TEST(FileCopy, MakeUniqueNeverOverwritesIncludingCaseVariants) {
    Fixture f;
    ASSERT_TRUE(write_file(f.library / L"img_0001.cr3", "existing one"));  // differs only in case
    ASSERT_TRUE(write_file(f.library / L"IMG_0001_2.CR3", "existing two"));

    auto receipt = copy_file_verified(f.source, f.library, {}, small_chunks(), {});
    ASSERT_TRUE(receipt) << receipt.error().describe();
    EXPECT_EQ(receipt->final_path.filename(), std::filesystem::path(L"IMG_0001_3.CR3"));
    EXPECT_EQ(read_text(f.library / L"img_0001.cr3"), "existing one");
    EXPECT_EQ(read_text(f.library / L"IMG_0001_2.CR3"), "existing two");
    EXPECT_EQ(read_text(receipt->final_path), as_string(f.content));
}

TEST(FileCopy, FailIfExistsFailsEarlyAndKeepsTheExistingFile) {
    Fixture f;
    ASSERT_TRUE(write_file(f.library / L"IMG_0001.CR3", "keep me"));
    std::vector<CopyProgress> events;
    auto receipt = copy_file_verified(f.source, f.library, {}, small_chunks(CopyNamePolicy::FailIfExists), {},
                                      [&](const CopyProgress& p) { events.push_back(p); });
    ASSERT_FALSE(receipt);
    EXPECT_EQ(receipt.error().code, ErrorCode::AlreadyExists);
    EXPECT_TRUE(events.empty());  // nothing was copied
    EXPECT_EQ(read_text(f.library / L"IMG_0001.CR3"), "keep me");
    EXPECT_EQ(count_temporary_files(f.library), 0);
}

TEST(FileCopy, AFileThatAppearsBeforePublicationIsNeverClobbered) {
    // FailIfExists: the racing file wins, the copy is discarded.
    {
        Fixture f;
        detail::CopyTestHooks hooks;
        hooks.before_publish = [](const std::filesystem::path& candidate) {
            if (!std::filesystem::exists(candidate)) {
                EXPECT_TRUE(write_file(candidate, "racer"));
            }
        };
        auto receipt = detail::copy_file_verified_with_hooks(
            f.source, f.library, {}, small_chunks(CopyNamePolicy::FailIfExists), {}, {}, hooks);
        ASSERT_FALSE(receipt);
        EXPECT_EQ(receipt.error().code, ErrorCode::AlreadyExists);
        EXPECT_EQ(read_text(f.library / L"IMG_0001.CR3"), "racer");
        EXPECT_EQ(files_in(f.library).size(), 1u);
    }
    // MakeUnique: the copy moves on to the next free name.
    {
        Fixture f;
        detail::CopyTestHooks hooks;
        int raced = 0;
        hooks.before_publish = [&](const std::filesystem::path& candidate) {
            if (raced < 2) {
                ++raced;
                EXPECT_TRUE(write_file(candidate, "racer"));
            }
        };
        auto receipt =
            detail::copy_file_verified_with_hooks(f.source, f.library, {}, small_chunks(), {}, {}, hooks);
        ASSERT_TRUE(receipt) << receipt.error().describe();
        EXPECT_EQ(receipt->final_path.filename(), std::filesystem::path(L"IMG_0001_3.CR3"));
        EXPECT_EQ(read_text(f.library / L"IMG_0001.CR3"), "racer");
        EXPECT_EQ(read_text(f.library / L"IMG_0001_2.CR3"), "racer");
        EXPECT_EQ(count_temporary_files(f.library), 0);
    }
}

TEST(FileCopy, ConcurrentCopiesOfOneNameGetDistinctNames) {
    Fixture f(3 * kChunk);
    constexpr int kThreads = 4;
    std::vector<Expected<CopyReceipt>> results(kThreads, fail(ErrorCode::Unknown, "not run"));
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back(
            [&, i] { results[i] = copy_file_verified(f.source, f.library, {}, small_chunks(), {}); });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    std::set<std::filesystem::path> names;
    for (const auto& result : results) {
        ASSERT_TRUE(result) << result.error().describe();
        names.insert(result->final_path.filename());
        EXPECT_EQ(read_text(result->final_path), as_string(f.content));
    }
    EXPECT_EQ(names.size(), static_cast<std::size_t>(kThreads));
    EXPECT_EQ(count_temporary_files(f.library), 0);
}

TEST(FileCopy, RejectsInvalidArguments) {
    Fixture f;
    for (const wchar_t* bad : {L"a\\b.cr3", L"a/b.cr3", L"..", L".", L"x:y.cr3", L"trailing.", L"trailing ",
                               L"what?.cr3", L".lucent-0123456789abcdef.tmp"}) {
        auto receipt = copy_file_verified(f.source, f.library, bad, small_chunks(), {});
        ASSERT_FALSE(receipt);
        EXPECT_EQ(receipt.error().code, ErrorCode::InvalidArgument);
    }
    for (const std::size_t chunk :
         {std::size_t{0}, std::size_t{4096}, kChunk + 512, kMaxHashChunkBytes * 2}) {
        CopyOptions options = small_chunks();
        options.chunk_bytes = chunk;
        auto receipt = copy_file_verified(f.source, f.library, {}, options, {});
        ASSERT_FALSE(receipt);
        EXPECT_EQ(receipt.error().code, ErrorCode::InvalidArgument) << chunk;
    }
    // Destination missing / not a folder; source missing / a folder.
    EXPECT_EQ(copy_file_verified(f.source, f.temp / L"nope", {}, small_chunks(), {}).error().code,
              ErrorCode::NotFound);
    EXPECT_EQ(copy_file_verified(f.source, f.source, {}, small_chunks(), {}).error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(
        copy_file_verified(f.source_dir / L"missing.cr3", f.library, {}, small_chunks(), {}).error().code,
        ErrorCode::NotFound);
    EXPECT_EQ(copy_file_verified(f.source_dir, f.library, L"x.cr3", small_chunks(), {}).error().code,
              ErrorCode::InvalidArgument);
    EXPECT_TRUE(files_in(f.library).empty());
}

TEST(FileCopy, CopyingOntoTheSourceNameInItsOwnFolderNeverTouchesIt) {
    Fixture f;
    auto identity = stat_file(f.source);
    ASSERT_TRUE(identity);
    auto refused =
        copy_file_verified(f.source, f.source_dir, {}, small_chunks(CopyNamePolicy::FailIfExists), {});
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::AlreadyExists);
    auto receipt = copy_file_verified(f.source, f.source_dir, {}, small_chunks(), {});
    ASSERT_TRUE(receipt) << receipt.error().describe();
    EXPECT_EQ(receipt->final_path.filename(), std::filesystem::path(L"IMG_0001_2.CR3"));
    f.expect_source_intact(*identity);
}

// ---- failure paths ----------------------------------------------------------------------------

TEST(FileCopy, VerificationMismatchIsCorruptAndPublishesNothing) {
    Fixture f;
    auto identity = stat_file(f.source);
    ASSERT_TRUE(identity);
    detail::CopyTestHooks hooks;
    hooks.before_write = [](std::uint64_t offset, std::span<std::byte> chunk) {
        if (offset == 2 * kChunk) {
            chunk[17] ^= std::byte{0x01};  // one flipped bit on the way to the disk
        }
    };
    auto receipt =
        detail::copy_file_verified_with_hooks(f.source, f.library, {}, small_chunks(), {}, {}, hooks);
    ASSERT_FALSE(receipt);
    EXPECT_EQ(receipt.error().code, ErrorCode::Corrupt);
    EXPECT_NE(receipt.error().message.find("verification"), std::string::npos);
    EXPECT_TRUE(files_in(f.library).empty());
    f.expect_source_intact(*identity);
}

TEST(FileCopy, InsufficientFreeSpaceFailsBeforeWriting) {
    Fixture f;
    detail::CopyTestHooks hooks;
    hooks.available_bytes = f.content.size() + 10;
    CopyOptions options = small_chunks();
    options.free_space_reserve_bytes = 11;  // one byte short
    auto receipt = detail::copy_file_verified_with_hooks(f.source, f.library, {}, options, {}, {}, hooks);
    ASSERT_FALSE(receipt);
    EXPECT_EQ(receipt.error().code, ErrorCode::DiskFull);
    EXPECT_TRUE(files_in(f.library).empty());

    options.free_space_reserve_bytes = 10;  // exactly enough
    receipt = detail::copy_file_verified_with_hooks(f.source, f.library, {}, options, {}, {}, hooks);
    ASSERT_TRUE(receipt) << receipt.error().describe();

    hooks.available_bytes = f.content.size() - 1;
    options.free_space_reserve_bytes = 0;
    receipt = detail::copy_file_verified_with_hooks(f.source, f.library, {}, options, {}, {}, hooks);
    ASSERT_FALSE(receipt);
    EXPECT_EQ(receipt.error().code, ErrorCode::DiskFull);
}

TEST(FileCopy, DiskFullDuringTheWriteCleansUp) {
    Fixture f;
    detail::CopyTestHooks hooks;
    hooks.write_fault = [](std::uint64_t offset) -> std::uint32_t {
        return offset >= 3 * kChunk ? ERROR_DISK_FULL : ERROR_SUCCESS;
    };
    auto receipt =
        detail::copy_file_verified_with_hooks(f.source, f.library, {}, small_chunks(), {}, {}, hooks);
    ASSERT_FALSE(receipt);
    EXPECT_EQ(receipt.error().code, ErrorCode::DiskFull);
    EXPECT_EQ(receipt.error().native_code, ERROR_DISK_FULL);
    EXPECT_TRUE(files_in(f.library).empty());
}

TEST(FileCopy, CancellationAtAnyPointCleansUp) {
    // Before starting.
    {
        Fixture f;
        CancellationSource source;
        source.cancel();
        auto receipt = copy_file_verified(f.source, f.library, {}, small_chunks(), source.token());
        ASSERT_FALSE(receipt);
        EXPECT_EQ(receipt.error().code, ErrorCode::Cancelled);
        EXPECT_TRUE(files_in(f.library).empty());
    }
    // In the middle of the copy.
    {
        Fixture f;
        CancellationSource source;
        detail::CopyTestHooks hooks;
        hooks.after_chunk = [&](std::uint64_t copied) {
            if (copied >= 2 * kChunk) {
                source.cancel();
            }
        };
        auto receipt = detail::copy_file_verified_with_hooks(f.source, f.library, {}, small_chunks(),
                                                             source.token(), {}, hooks);
        ASSERT_FALSE(receipt);
        EXPECT_EQ(receipt.error().code, ErrorCode::Cancelled);
        EXPECT_TRUE(files_in(f.library).empty());
    }
    // During verification (from the progress callback).
    {
        Fixture f;
        CancellationSource source;
        auto receipt = copy_file_verified(f.source, f.library, {}, small_chunks(), source.token(),
                                          [&](const CopyProgress& p) {
                                              if (p.phase == CopyPhase::Verifying && p.bytes_done > 0) {
                                                  source.cancel();
                                              }
                                          });
        ASSERT_FALSE(receipt);
        EXPECT_EQ(receipt.error().code, ErrorCode::Cancelled);
        EXPECT_TRUE(files_in(f.library).empty());
    }
}

TEST(FileCopy, SourceModifiedDuringTheCopyIsSourceChanged) {
    Fixture f;
    detail::CopyTestHooks hooks;
    hooks.after_chunk = [&](std::uint64_t copied) {
        if (copied == kChunk) {
            // Another program appends to the source (WriteFile updates the modification time).
            const HANDLE writer = ::CreateFileW(f.source.c_str(), FILE_APPEND_DATA,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            ASSERT_NE(writer, INVALID_HANDLE_VALUE);
            DWORD written = 0;
            ::WriteFile(writer, "tail", 4, &written, nullptr);
            ::CloseHandle(writer);
        }
    };
    auto receipt =
        detail::copy_file_verified_with_hooks(f.source, f.library, {}, small_chunks(), {}, {}, hooks);
    ASSERT_FALSE(receipt);
    EXPECT_EQ(receipt.error().code, ErrorCode::SourceChanged);
    EXPECT_TRUE(files_in(f.library).empty());
}

TEST(FileCopy, SourceDeletedDuringTheCopyIsSourceChanged) {
    Fixture f;
    detail::CopyTestHooks hooks;
    hooks.after_chunk = [&](std::uint64_t copied) {
        if (copied == kChunk) {
            EXPECT_TRUE(::DeleteFileW(f.source.c_str()));  // allowed: the source is shared for deletion
        }
    };
    auto receipt =
        detail::copy_file_verified_with_hooks(f.source, f.library, {}, small_chunks(), {}, {}, hooks);
    ASSERT_FALSE(receipt);
    EXPECT_EQ(receipt.error().code, ErrorCode::SourceChanged);
    EXPECT_TRUE(files_in(f.library).empty());
}

// ---- removing an unused copy -----------------------------------------------------------------

TEST(FileCopy, AnUnusedCopyIsRemovedOnlyWhileItIsTheSameFile) {
    Fixture f;
    auto identity = stat_file(f.source);
    ASSERT_TRUE(identity);
    auto receipt = copy_file_verified(f.source, f.library, {}, small_chunks(), {});
    ASSERT_TRUE(receipt) << receipt.error().describe();

    // Another version at the copy's path (rewritten by someone): kept.
    FileIdentity stale = receipt->copy_identity;
    stale.last_write_time += 1;
    const Status changed = remove_verified_copy(receipt->final_path, stale);
    ASSERT_FALSE(changed);
    EXPECT_EQ(changed.error().code, ErrorCode::SourceChanged);
    EXPECT_TRUE(std::filesystem::exists(receipt->final_path));

    // A writer holding the copy open: refused, kept.
    HANDLE writer = ::CreateFileW(receipt->final_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(writer, INVALID_HANDLE_VALUE);
    EXPECT_FALSE(remove_verified_copy(receipt->final_path, receipt->copy_identity));
    ::CloseHandle(writer);
    EXPECT_TRUE(std::filesystem::exists(receipt->final_path));

    // The copy as published: removed; the source is untouched.
    ASSERT_TRUE(remove_verified_copy(receipt->final_path, receipt->copy_identity));
    EXPECT_TRUE(files_in(f.library).empty());
    f.expect_source_intact(*identity);
    const Status gone = remove_verified_copy(receipt->final_path, receipt->copy_identity);
    ASSERT_FALSE(gone);
    EXPECT_EQ(gone.error().code, ErrorCode::NotFound);
}

}  // namespace
}  // namespace lucent::core
