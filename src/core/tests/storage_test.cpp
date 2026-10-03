#include "lucent/core/storage.hpp"

#include <gtest/gtest.h>

// clang-format off
#include <windows.h>
#include <winioctl.h>
// clang-format on

#include <array>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

#include "storage_internal.hpp"
#include "test_support.hpp"

namespace lucent::core {
namespace {

using detail::classify_storage;
using detail::lexical_volume_root;
using detail::StorageDeviceInfo;
using detail::VolumeRootKind;
using test_support::read_text;
using test_support::TempDir;
using test_support::write_file;

std::filesystem::path windows_directory() {
    std::array<wchar_t, MAX_PATH> buffer{};
    const UINT length = ::GetWindowsDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    return std::filesystem::path(std::wstring(buffer.data(), length));
}

// A drive letter with nothing mapped to it, or nullopt when all 26 are in use.
std::optional<wchar_t> unused_drive_letter() {
    const DWORD used = ::GetLogicalDrives();
    for (int i = 25; i >= 3; --i) {  // prefer late letters (Z:, Y:, ...)
        if ((used & (1u << i)) == 0) {
            return static_cast<wchar_t>(L'A' + i);
        }
    }
    return std::nullopt;
}

int count_temporary_files(const std::filesystem::path& directory) {
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (is_temporary_file_name(entry.path().filename())) {
            ++count;
        }
    }
    return count;
}

// ---- lexical_volume_root ----------------------------------------------------------------------

TEST(StorageKind, LexicalRootOfDrivePaths) {
    EXPECT_EQ(lexical_volume_root(L"C:\\Photos\\a.cr3").root, L"C:\\");
    EXPECT_EQ(lexical_volume_root(L"c:\\photos").kind, VolumeRootKind::DriveLetter);
    EXPECT_EQ(lexical_volume_root(L"c:\\photos").root, L"C:\\");
    EXPECT_EQ(lexical_volume_root(L"D:").root, L"D:\\");
    EXPECT_EQ(lexical_volume_root(L"e:/cards/DCIM").root, L"E:\\");
    EXPECT_EQ(lexical_volume_root(L"\\\\?\\F:\\very\\long").root, L"F:\\");
    EXPECT_EQ(lexical_volume_root(L"\\\\.\\G:\\x").root, L"G:\\");
    // Drive-relative ("C:foo") and plain relative paths have no root of their own.
    EXPECT_EQ(lexical_volume_root(L"C:foo").kind, VolumeRootKind::None);
    EXPECT_EQ(lexical_volume_root(L"photos\\a.cr3").kind, VolumeRootKind::None);
    EXPECT_EQ(lexical_volume_root(L"").kind, VolumeRootKind::None);
}

TEST(StorageKind, LexicalRootOfUncPaths) {
    auto root = lexical_volume_root(L"\\\\nas\\photos\\2026\\a.cr3");
    EXPECT_EQ(root.kind, VolumeRootKind::Unc);
    EXPECT_EQ(root.root, L"\\\\nas\\photos\\");
    EXPECT_EQ(lexical_volume_root(L"\\\\nas\\photos").root, L"\\\\nas\\photos\\");
    EXPECT_EQ(lexical_volume_root(L"//nas/photos/x").root, L"\\\\nas\\photos\\");
    root = lexical_volume_root(L"\\\\?\\UNC\\nas\\photos\\deep\\file");
    EXPECT_EQ(root.kind, VolumeRootKind::Unc);
    EXPECT_EQ(root.root, L"\\\\nas\\photos\\");
    EXPECT_EQ(lexical_volume_root(L"\\\\?\\unc\\nas\\photos").root, L"\\\\nas\\photos\\");
    // Missing server or share.
    EXPECT_EQ(lexical_volume_root(L"\\\\nas").kind, VolumeRootKind::None);
    EXPECT_EQ(lexical_volume_root(L"\\\\nas\\").kind, VolumeRootKind::None);
    EXPECT_EQ(lexical_volume_root(L"\\\\\\share").kind, VolumeRootKind::None);
}

TEST(StorageKind, LexicalRootOfVolumeGuidAndDevicePaths) {
    const std::wstring guid = L"Volume{01234567-89ab-cdef-0123-456789abcdef}";
    auto root = lexical_volume_root(L"\\\\?\\" + guid + L"\\DCIM\\a.cr3");
    EXPECT_EQ(root.kind, VolumeRootKind::Volume);
    EXPECT_EQ(root.root, L"\\\\?\\" + guid + L"\\");
    EXPECT_EQ(lexical_volume_root(L"\\\\?\\" + guid).root, L"\\\\?\\" + guid + L"\\");
    EXPECT_EQ(lexical_volume_root(L"\\\\?\\Volume{short}\\x").kind, VolumeRootKind::None);
    EXPECT_EQ(lexical_volume_root(L"\\\\.\\PhysicalDrive0").kind, VolumeRootKind::None);
    EXPECT_EQ(lexical_volume_root(L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1\\x").kind,
              VolumeRootKind::None);
}

// ---- classify_storage -------------------------------------------------------------------------

TEST(StorageKind, ClassifiesDriveTypes) {
    EXPECT_EQ(classify_storage(DRIVE_FIXED, std::nullopt), StorageKind::Fixed);
    EXPECT_EQ(classify_storage(DRIVE_RAMDISK, std::nullopt), StorageKind::Fixed);
    EXPECT_EQ(classify_storage(DRIVE_REMOVABLE, std::nullopt), StorageKind::Removable);
    EXPECT_EQ(classify_storage(DRIVE_CDROM, std::nullopt), StorageKind::Removable);
    EXPECT_EQ(classify_storage(DRIVE_REMOTE, std::nullopt), StorageKind::Network);
    EXPECT_EQ(classify_storage(DRIVE_UNKNOWN, std::nullopt), StorageKind::Unknown);
    EXPECT_EQ(classify_storage(DRIVE_NO_ROOT_DIR, std::nullopt), StorageKind::Unknown);
    EXPECT_EQ(classify_storage(99, std::nullopt), StorageKind::Unknown);
}

TEST(StorageKind, FixedVolumesOnHotPluggableBusesAreRemovable) {
    for (const STORAGE_BUS_TYPE bus : {BusTypeUsb, BusTypeSd, BusTypeMmc, BusType1394}) {
        EXPECT_EQ(classify_storage(DRIVE_FIXED, StorageDeviceInfo{static_cast<std::uint32_t>(bus), false}),
                  StorageKind::Removable)
            << "bus " << bus;
    }
    for (const STORAGE_BUS_TYPE bus : {BusTypeNvme, BusTypeSata, BusTypeSas, BusTypeRAID, BusTypeVirtual}) {
        EXPECT_EQ(classify_storage(DRIVE_FIXED, StorageDeviceInfo{static_cast<std::uint32_t>(bus), false}),
                  StorageKind::Fixed)
            << "bus " << bus;
    }
    EXPECT_EQ(classify_storage(DRIVE_FIXED, StorageDeviceInfo{BusTypeSata, true}), StorageKind::Removable);
    // The device never overrides a network or removable drive type.
    EXPECT_EQ(classify_storage(DRIVE_REMOTE, StorageDeviceInfo{BusTypeNvme, false}), StorageKind::Network);
    EXPECT_EQ(classify_storage(DRIVE_REMOVABLE, StorageDeviceInfo{BusTypeNvme, false}),
              StorageKind::Removable);
}

TEST(StorageKind, ToStringIsStable) {
    EXPECT_EQ(to_string(StorageKind::Unknown), "unknown");
    EXPECT_EQ(to_string(StorageKind::Fixed), "fixed");
    EXPECT_EQ(to_string(StorageKind::Removable), "removable");
    EXPECT_EQ(to_string(StorageKind::Network), "network");
}

// ---- storage_kind on this machine -------------------------------------------------------------

TEST(StorageKind, SystemVolumeIsFixed) {
    const std::filesystem::path windows = windows_directory();
    EXPECT_EQ(storage_kind(windows), StorageKind::Fixed);
    EXPECT_EQ(storage_kind(windows / L"does-not-exist" / L"x.cr3"), StorageKind::Fixed);
    EXPECT_EQ(storage_kind(std::filesystem::path(L"\\\\?\\" + windows.native())), StorageKind::Fixed);
    EXPECT_EQ(storage_kind(windows.root_name()), StorageKind::Fixed);

    // The same volume through its GUID path.
    std::array<wchar_t, 64> volume{};
    ASSERT_TRUE(::GetVolumeNameForVolumeMountPointW(windows.root_path().c_str(), volume.data(),
                                                    static_cast<DWORD>(volume.size())));
    EXPECT_EQ(storage_kind(std::filesystem::path(volume.data()) / L"Windows"), StorageKind::Fixed);
}

TEST(StorageKind, RelativePathUsesTheCurrentDirectoryVolume) {
    const StorageKind current = storage_kind(std::filesystem::current_path());
    EXPECT_EQ(storage_kind(L"relative\\photo.cr3"), current);
    EXPECT_NE(current, StorageKind::Unknown);
}

TEST(StorageKind, UncPathsAreNetworkWithoutTouchingTheNetwork) {
    // A server that cannot exist: classification must not wait for a redirector timeout.
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(storage_kind(L"\\\\lucent-no-such-server.invalid\\share\\a.cr3"), StorageKind::Network);
    EXPECT_EQ(storage_kind(L"\\\\?\\UNC\\lucent-no-such-server.invalid\\share\\a.cr3"), StorageKind::Network);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
}

TEST(StorageKind, UnmappedDriveAndUnusablePathsAreUnknown) {
    if (const auto letter = unused_drive_letter()) {
        EXPECT_EQ(storage_kind(std::wstring{*letter} + L":\\DCIM\\a.cr3"), StorageKind::Unknown);
    }
    EXPECT_EQ(storage_kind(L""), StorageKind::Unknown);
    EXPECT_EQ(storage_kind(L"\\\\.\\PhysicalDrive0"), StorageKind::Unknown);
}

TEST(StorageKind, SubstDriveReportsTheAliasedVolume) {
    const auto letter = unused_drive_letter();
    if (!letter) {
        GTEST_SKIP() << "no free drive letter for a SUBST mapping";
    }
    TempDir dir;
    const std::wstring device{*letter, L':'};
    // DefineDosDevice affects only this logon session and is removed below.
    ASSERT_TRUE(::DefineDosDeviceW(0, device.c_str(), dir.path().c_str()));
    const StorageKind kind = storage_kind(device + L"\\a.cr3");
    const StorageKind expected = storage_kind(dir.path());
    ::DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE, device.c_str(), dir.path().c_str());
    EXPECT_EQ(kind, expected);
}

// ---- available_disk_bytes ---------------------------------------------------------------------

TEST(StorageFiles, AvailableBytesOfAnExistingDirectory) {
    TempDir dir;
    auto available = available_disk_bytes(dir.path());
    ASSERT_TRUE(available) << available.error().describe();
    EXPECT_GT(*available, 0u);
}

TEST(StorageFiles, AvailableBytesOfAMissingDirectoryIsNotFound) {
    TempDir dir;
    auto available = available_disk_bytes(dir / L"missing" / L"deeper");
    ASSERT_FALSE(available);
    EXPECT_EQ(available.error().code, ErrorCode::NotFound);
    EXPECT_EQ(available_disk_bytes(L"").error().code, ErrorCode::InvalidArgument);
}

// ---- create_directory_tree--------------------------------------------------------------------

TEST(StorageFiles, CreatesNestedUnicodeDirectoriesIdempotently) {
    TempDir dir;
    const std::filesystem::path target = dir / L"Фото" / L"写真 ✓" / L"2026";
    ASSERT_TRUE(create_directory_tree(target));
    EXPECT_TRUE(std::filesystem::is_directory(target));
    ASSERT_TRUE(create_directory_tree(target));  // already there
    ASSERT_TRUE(create_directory_tree(dir.path()));
}

TEST(StorageFiles, CreatesDirectoriesBeyondMaxPath) {
    TempDir dir;
    std::filesystem::path target = dir.path();
    while (target.native().size() < 400) {
        target /= std::wstring(40, L'd');
    }
    ASSERT_TRUE(create_directory_tree(target));
    EXPECT_TRUE(std::filesystem::is_directory(std::filesystem::path(test_support::verbatim(target))));
}

TEST(StorageFiles, CreateDirectoriesRefusesAFileInTheWay) {
    TempDir dir;
    ASSERT_TRUE(write_file(dir / L"blocker", "x"));
    auto status = create_directory_tree(dir / L"blocker" / L"child");
    ASSERT_FALSE(status);
    EXPECT_EQ(status.error().code, ErrorCode::AlreadyExists);
    EXPECT_EQ(create_directory_tree(dir / L"blocker").error().code, ErrorCode::AlreadyExists);
    EXPECT_EQ(create_directory_tree(L"").error().code, ErrorCode::InvalidArgument);
}

TEST(StorageFiles, CreateDirectoriesOnAMissingDriveIsNotFound) {
    const auto letter = unused_drive_letter();
    if (!letter) {
        GTEST_SKIP() << "no free drive letter";
    }
    auto status = create_directory_tree(std::wstring{*letter} + L":\\Lucent\\cache");
    ASSERT_FALSE(status);
    EXPECT_EQ(status.error().code, ErrorCode::NotFound);
}

// ---- write_file_atomic ------------------------------------------------------------------------

std::span<const std::byte> bytes_of(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

TEST(StorageFiles, AtomicWriteCreatesAndReplaces) {
    TempDir dir;
    const auto target = dir / L"settings ✓.json";
    ASSERT_TRUE(write_file_atomic(target, bytes_of("first")));
    EXPECT_EQ(read_text(target), "first");
    ASSERT_TRUE(write_file_atomic(target, bytes_of("second, longer")));
    EXPECT_EQ(read_text(target), "second, longer");
    ASSERT_TRUE(write_file_atomic(target, {}, {ExistingFile::Replace, false}));
    EXPECT_EQ(read_text(target), "");
    EXPECT_EQ(count_temporary_files(dir.path()), 0);
}

TEST(StorageFiles, AtomicWriteKeepNeverClobbers) {
    TempDir dir;
    const auto target = dir / L"a.bin";
    ASSERT_TRUE(write_file(target, "original"));
    auto status = write_file_atomic(target, bytes_of("new"), {ExistingFile::Keep, true});
    ASSERT_FALSE(status);
    EXPECT_EQ(status.error().code, ErrorCode::AlreadyExists);
    EXPECT_EQ(read_text(target), "original");
    EXPECT_EQ(count_temporary_files(dir.path()), 0);
    ASSERT_TRUE(write_file_atomic(dir / L"b.bin", bytes_of("new"), {ExistingFile::Keep, true}));
    EXPECT_EQ(read_text(dir / L"b.bin"), "new");
}

TEST(StorageFiles, AtomicWriteReplacesAFileThatReadersHoldOpenWithDeleteSharing) {
    TempDir dir;
    const auto target = dir / L"entry.bin";
    ASSERT_TRUE(write_file(target, "old"));
    const HANDLE reader =
        ::CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(reader, INVALID_HANDLE_VALUE);
    auto status = write_file_atomic(target, bytes_of("new"));
    std::array<char, 8> seen{};
    DWORD read = 0;
    ASSERT_TRUE(::ReadFile(reader, seen.data(), static_cast<DWORD>(seen.size()), &read, nullptr));
    ::CloseHandle(reader);
    ASSERT_TRUE(status) << status.error().describe();
    EXPECT_EQ(std::string(seen.data(), read), "old");  // the reader keeps the old file
    EXPECT_EQ(read_text(target), "new");
}

TEST(StorageFiles, AtomicWriteFailuresLeaveNothingBehind) {
    TempDir dir;
    auto status = write_file_atomic(dir / L"missing" / L"a.bin", bytes_of("x"));
    ASSERT_FALSE(status);
    EXPECT_EQ(status.error().code, ErrorCode::NotFound);
    EXPECT_EQ(write_file_atomic(L"", bytes_of("x")).error().code, ErrorCode::InvalidArgument);

    // Replacing a directory is refused and the temporary file is removed.
    std::filesystem::create_directory(dir / L"folder");
    status = write_file_atomic(dir / L"folder", bytes_of("x"));
    ASSERT_FALSE(status);
    EXPECT_TRUE(std::filesystem::is_directory(dir / L"folder"));
    EXPECT_EQ(count_temporary_files(dir.path()), 0);
}

TEST(StorageFiles, AtomicWriteBeyondMaxPath) {
    TempDir dir;
    std::filesystem::path folder = dir.path();
    while (folder.native().size() < 300) {
        folder /= std::wstring(50, L'p');
    }
    ASSERT_TRUE(create_directory_tree(folder));
    const auto target = folder / L"long.bin";
    ASSERT_TRUE(write_file_atomic(target, bytes_of("long")));
    EXPECT_EQ(read_text(target), "long");
}

// ---- temporary names --------------------------------------------------------------------------

TEST(StorageFiles, TemporaryFileNames) {
    EXPECT_TRUE(is_temporary_file_name(L".lucent-0123456789abcdef.tmp"));
    EXPECT_TRUE(is_temporary_file_name(L".LUCENT-0123456789ABCDEF.TMP"));
    EXPECT_FALSE(is_temporary_file_name(L".lucent-0123456789abcde.tmp"));    // 15 digits
    EXPECT_FALSE(is_temporary_file_name(L".lucent-0123456789abcdeg.tmp"));   // not hex
    EXPECT_FALSE(is_temporary_file_name(L"lucent-0123456789abcdef.tmp"));    // no dot
    EXPECT_FALSE(is_temporary_file_name(L".lucent-0123456789abcdef.tmpx"));  // suffix
    EXPECT_FALSE(is_temporary_file_name(L"IMG_0001.CR3"));
    EXPECT_FALSE(is_temporary_file_name(L""));
    for (int i = 0; i < 100; ++i) {
        const std::wstring name = detail::make_temporary_file_name();
        EXPECT_TRUE(is_temporary_file_name(name)) << "generated name #" << i;
    }
    EXPECT_NE(detail::make_temporary_file_name(), detail::make_temporary_file_name());
}

}  // namespace
}  // namespace lucent::core
