// The coordinator's handle-held verification and publication (src/output_publication.hpp, ADR
// 0012): one handle from verification to the rename, header inspection, no-clobber renames,
// exclusive opens, write probes and the leftover sweep.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include <lucent/core/file.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/export/export.hpp>
#include <lucent/export/metadata.hpp>
#include <lucent/export/output_profile.hpp>
#include <lucent/workers/job_output.hpp>

#include "app_test_support.hpp"
#include "output_publication.hpp"

namespace lucent::app {
namespace {

namespace ts = test_support;
using detail::HeldOutput;

exporter::OutputImage image(std::int32_t width, std::int32_t height, exporter::BitDepth depth,
                            exporter::OutputProfile profile) {
    exporter::OutputImage out;
    out.width = width;
    out.height = height;
    out.bit_depth = depth;
    out.profile = profile;
    const std::size_t samples = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3;
    if (depth == exporter::BitDepth::Eight) {
        out.samples8.resize(samples);
        for (std::size_t i = 0; i < samples; ++i) {
            out.samples8[i] = static_cast<std::uint8_t>((i * 7) & 0xFF);
        }
    } else {
        out.samples16.resize(samples);
        for (std::size_t i = 0; i < samples; ++i) {
            out.samples16[i] = static_cast<std::uint16_t>((i * 257) & 0xFFFF);
        }
    }
    return out;
}

std::filesystem::path write_output(const std::filesystem::path& path, const exporter::ExportSettings& settings,
                                   std::int32_t width = 40, std::int32_t height = 24) {
    const exporter::OutputImage pixels =
        image(width, height, exporter::bit_depth_for(settings.format), settings.profile);
    exporter::ExportMetadata metadata;
    metadata.camera_make = "Lucent";
    metadata.artist = "Test";
    auto receipt = exporter::write_verified(pixels, path, settings, metadata);
    EXPECT_TRUE(receipt) << (receipt ? std::string() : receipt.error().describe());
    return path;
}

core::Digest256 icc_sha(exporter::OutputProfile profile) {
    auto icc = exporter::icc_profile(profile);
    EXPECT_TRUE(icc);
    auto sha = core::sha256(std::as_bytes(*icc));
    EXPECT_TRUE(sha);
    return sha ? *sha : core::Digest256{};
}

DWORD open_error(const std::filesystem::path& path, DWORD access, DWORD share) {
    HANDLE handle = ::CreateFileW(path.c_str(), access, share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return ::GetLastError();
    }
    ::CloseHandle(handle);
    return ERROR_SUCCESS;
}

void set_age(const std::filesystem::path& path, std::chrono::hours age) {
    HANDLE handle = ::CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    FILETIME now{};
    ::GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER t{};
    t.LowPart = now.dwLowDateTime;
    t.HighPart = now.dwHighDateTime;
    t.QuadPart -= static_cast<ULONGLONG>(age.count()) * 3600ULL * 10'000'000ULL;
    FILETIME old{t.LowPart, t.HighPart};
    EXPECT_TRUE(::SetFileTime(handle, nullptr, nullptr, &old));
    ::CloseHandle(handle);
}

TEST(OutputPublication, HoldsTheFileAndInspectsAJpeg) {
    ts::ScratchDir dir;
    const auto path = write_output(dir.path() / L"a.jpg", exporter::ExportSettings::jpeg_srgb_q95());
    auto held = HeldOutput::open(path, "test");
    ASSERT_TRUE(held) << held.error().describe();

    // Nobody can modify, rename or delete it while it is held; readers can still read it.
    EXPECT_EQ(open_error(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE),
              static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    EXPECT_EQ(open_error(path, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE),
              static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    EXPECT_FALSE(::DeleteFileW(path.c_str()));
    EXPECT_TRUE(core::ReadOnlyFile::open(path));
    auto metadata = exporter::read_export_metadata(path);
    ASSERT_TRUE(metadata) << metadata.error().describe();
    EXPECT_EQ(metadata->artist, "Test");

    auto digest = held->digest({});
    ASSERT_TRUE(digest);
    auto hashed = core::hash_file(path, {});
    ASSERT_TRUE(hashed);
    EXPECT_EQ(digest->sha256, hashed->digest);
    EXPECT_EQ(digest->size_bytes, hashed->identity.size_bytes);

    auto header = held->inspect_header();
    ASSERT_TRUE(header) << header.error().describe();
    EXPECT_EQ(header->format, exporter::ExportFormat::Jpeg);
    EXPECT_EQ(header->width, 40u);
    EXPECT_EQ(header->height, 24u);
    EXPECT_EQ(header->components, 3u);
    EXPECT_EQ(header->bits_per_sample, 8u);
    ASSERT_TRUE(header->icc_sha256);
    EXPECT_EQ(*header->icc_sha256, icc_sha(exporter::OutputProfile::Srgb));
}

TEST(OutputPublication, InspectsA16BitTiff) {
    ts::ScratchDir dir;
    const auto path =
        write_output(dir.path() / L"b.tif", exporter::ExportSettings::tiff16(exporter::OutputProfile::DisplayP3), 33, 17);
    auto held = HeldOutput::open(path, "test");
    ASSERT_TRUE(held);
    auto header = held->inspect_header();
    ASSERT_TRUE(header) << header.error().describe();
    EXPECT_EQ(header->format, exporter::ExportFormat::Tiff);
    EXPECT_EQ(header->width, 33u);
    EXPECT_EQ(header->height, 17u);
    EXPECT_EQ(header->components, 3u);
    EXPECT_EQ(header->bits_per_sample, 16u);
    ASSERT_TRUE(header->icc_sha256);
    EXPECT_EQ(*header->icc_sha256, icc_sha(exporter::OutputProfile::DisplayP3));
}

TEST(OutputPublication, RejectsFilesThatAreNotOutputs) {
    ts::ScratchDir dir;
    const auto text = dir.write_text(L"not.jpg", "this is not an image");
    auto held = HeldOutput::open(text, "test");
    ASSERT_TRUE(held);
    auto header = held->inspect_header();
    ASSERT_FALSE(header);
    EXPECT_EQ(header.error().code, ErrorCode::Corrupt);
    // A JPEG cut short in its headers.
    const auto jpeg = write_output(dir.path() / L"c.jpg", exporter::ExportSettings::jpeg_srgb_q95());
    std::string bytes;
    {
        std::ifstream in(jpeg, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    const auto cut = dir.write_text(L"cut.jpg", bytes.substr(0, 20));
    auto held_cut = HeldOutput::open(cut, "test");
    ASSERT_TRUE(held_cut);
    EXPECT_FALSE(held_cut->inspect_header());
    EXPECT_FALSE(HeldOutput::open(dir.path() / L"missing.jpg", "test"));
}

TEST(OutputPublication, RenamesTheHeldFileWithoutClobbering) {
    ts::ScratchDir dir;
    const auto path = write_output(dir.path() / L"~lucent-temp.tmp", exporter::ExportSettings::jpeg_srgb_q95());
    const auto expected = core::hash_file(path, {});
    ASSERT_TRUE(expected);
    const std::string other = "someone else's file";
    const auto taken = dir.write_text(L"final.jpg", other);
    auto held = HeldOutput::open(path, "test");
    ASSERT_TRUE(held);
    auto clobber = held->rename_no_clobber(taken);
    ASSERT_FALSE(clobber);
    EXPECT_EQ(clobber.error().code, ErrorCode::AlreadyExists);
    EXPECT_EQ(ts::file_sha256(taken), *core::sha256(std::as_bytes(std::span(other.data(), other.size()))));

    const auto free_name = dir.path() / L"final_2.jpg";
    ASSERT_TRUE(held->rename_no_clobber(free_name));
    EXPECT_EQ(held->path(), free_name);
    EXPECT_TRUE(held->flush());
    held->close();
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_EQ(ts::file_sha256(free_name), expected->digest);
}

TEST(OutputPublication, ExclusiveOpensFailWhileTheFileIsInUse) {
    ts::ScratchDir dir;
    const auto path = dir.write_text(L"in-use.jpg", "bytes");
    {
        auto reader = core::ReadOnlyFile::open(path);
        ASSERT_TRUE(reader);
        auto exclusive = HeldOutput::open_exclusive(path, "test");
        ASSERT_FALSE(exclusive);
        EXPECT_EQ(exclusive.error().code, ErrorCode::IoError);
        EXPECT_FALSE(detail::remove_unused_file(path, "test"));
    }
    EXPECT_TRUE(detail::remove_unused_file(path, "test"));
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_TRUE(detail::remove_unused_file(path, "test"));  // nothing left: ok
}

TEST(OutputPublication, ProbesWritableFolders) {
    ts::ScratchDir dir;
    EXPECT_TRUE(detail::probe_writable(dir.path(), L"0badc0de"));
    EXPECT_TRUE(std::filesystem::is_empty(dir.path())) << "the probe file is removed";
    auto missing = detail::probe_writable(dir.path() / L"missing", L"0badc0de");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
}

TEST(OutputPublication, SweepsLeftoversOfInterruptedPublications) {
    ts::ScratchDir dir;
    const std::wstring tag = L"a1b2c3d4";
    const auto temp = dir.write_text(detail::temp_file_name(tag, 7, 2), "partial");
    ASSERT_TRUE(workers::create_job_scratch_directory(temp));
    dir.write_text(workers::job_scratch_directory(temp) / L"output.jpg", "scratch");
    const auto kept = dir.write_text(detail::temp_file_name(tag, 8, 1), "a running attempt's");
    const auto aside = dir.write_text(detail::aside_file_name(tag, 9, 1), "moved aside");
    const auto other_tag = dir.write_text(detail::temp_file_name(L"ffffffff", 1, 1), "another batch");
    const auto old_exporter = dir.write_text(L"~lucent-0123456789abcdef.tmp", "old");
    set_age(old_exporter, std::chrono::hours(2));
    const auto new_exporter = dir.write_text(L"~lucent-fedcba9876543210.tmp", "new");
    const auto user_file = dir.write_text(L"~lucent-notes.txt", "user data");

    const std::vector<std::wstring> tags{tag};
    const std::vector<std::filesystem::path> keep{kept};
    const detail::SweepResult swept =
        detail::sweep_publication_leftovers(dir.path(), tags, keep, std::chrono::seconds(3600));
    EXPECT_EQ(swept.removed, 4u);  // temp, its scratch folder, the aside file, the old exporter temporary
    EXPECT_FALSE(std::filesystem::exists(temp));
    EXPECT_FALSE(std::filesystem::exists(workers::job_scratch_directory(temp)));
    EXPECT_FALSE(std::filesystem::exists(aside));
    EXPECT_FALSE(std::filesystem::exists(old_exporter));
    EXPECT_TRUE(std::filesystem::exists(kept));
    EXPECT_TRUE(std::filesystem::exists(other_tag));
    EXPECT_TRUE(std::filesystem::exists(new_exporter));
    EXPECT_TRUE(std::filesystem::exists(user_file));
    // A missing folder is not an error.
    EXPECT_EQ(detail::sweep_publication_leftovers(dir.path() / L"gone", tags, {}, std::chrono::seconds(0)).removed, 0u);
}

}  // namespace
}  // namespace lucent::app
