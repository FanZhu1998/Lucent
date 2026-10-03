#include "lucent/core/win32_error.hpp"

#include <cctype>
#include <string>

#include <gtest/gtest.h>
#include <windows.h>

namespace lucent::core {
namespace {

TEST(Win32Error, MapsDocumentedCodes) {
    const std::pair<DWORD, ErrorCode> cases[] = {
        {ERROR_FILE_NOT_FOUND, ErrorCode::NotFound},
        {ERROR_PATH_NOT_FOUND, ErrorCode::NotFound},
        {ERROR_INVALID_DRIVE, ErrorCode::NotFound},
        {ERROR_BAD_NETPATH, ErrorCode::NotFound},
        {ERROR_BAD_NET_NAME, ErrorCode::NotFound},
        {ERROR_ACCESS_DENIED, ErrorCode::PermissionDenied},
        {ERROR_SHARING_VIOLATION, ErrorCode::IoError},
        {ERROR_LOCK_VIOLATION, ErrorCode::IoError},
        {ERROR_DISK_FULL, ErrorCode::DiskFull},
        {ERROR_HANDLE_DISK_FULL, ErrorCode::DiskFull},
        {ERROR_FILE_EXISTS, ErrorCode::AlreadyExists},
        {ERROR_ALREADY_EXISTS, ErrorCode::AlreadyExists},
        {ERROR_NOT_ENOUGH_MEMORY, ErrorCode::OutOfMemory},
        {ERROR_OUTOFMEMORY, ErrorCode::OutOfMemory},
        {ERROR_OPERATION_ABORTED, ErrorCode::Cancelled},
        {ERROR_CANCELLED, ErrorCode::Cancelled},
        {ERROR_INVALID_PARAMETER, ErrorCode::IoError},  // everything else
        {ERROR_CRC, ErrorCode::IoError},
        {ERROR_NOT_READY, ErrorCode::IoError},
        {0x7FFF'FFFFu, ErrorCode::IoError},
    };
    for (const auto& [win32_code, expected] : cases) {
        EXPECT_EQ(error_code_from_win32(win32_code), expected) << "Win32 error " << win32_code;
    }
}

TEST(Win32Error, SuccessCodeIsAnInternalError) {
    EXPECT_EQ(error_code_from_win32(ERROR_SUCCESS), ErrorCode::Internal);
}

TEST(Win32Error, OnlySharingAndLockViolationsAreTransient) {
    EXPECT_TRUE(is_transient_win32_error(ERROR_SHARING_VIOLATION));
    EXPECT_TRUE(is_transient_win32_error(ERROR_LOCK_VIOLATION));
    EXPECT_FALSE(is_transient_win32_error(ERROR_ACCESS_DENIED));
    EXPECT_FALSE(is_transient_win32_error(ERROR_FILE_NOT_FOUND));
    EXPECT_FALSE(is_transient_win32_error(ERROR_SUCCESS));
}

TEST(Win32Error, AbsentVolumesAreIoErrorsThatPresenceChecksRecognise) {
    // An empty card-reader slot, a disconnected device, an unformatted volume.
    for (const DWORD code : {ERROR_NOT_READY, ERROR_NO_MEDIA_IN_DRIVE, ERROR_DEV_NOT_EXIST, ERROR_DEVICE_NOT_CONNECTED,
                             ERROR_DEVICE_REMOVED, ERROR_NO_SUCH_DEVICE, ERROR_UNRECOGNIZED_VOLUME}) {
        EXPECT_TRUE(is_volume_absent_win32_error(code)) << code;
        EXPECT_EQ(error_code_from_win32(code), ErrorCode::IoError) << code;
    }
    for (const DWORD code : {ERROR_SUCCESS, ERROR_FILE_NOT_FOUND, ERROR_ACCESS_DENIED, ERROR_SHARING_VIOLATION,
                             ERROR_CRC, ERROR_BAD_NETPATH}) {
        EXPECT_FALSE(is_volume_absent_win32_error(code)) << code;
    }
}

TEST(Win32Error, SystemMessageIsTrimmedText) {
    // The text is localised on non-English systems, so only its shape is checked.
    const std::string message = win32_message(ERROR_FILE_NOT_FOUND);
    ASSERT_FALSE(message.empty());
    EXPECT_FALSE(std::isspace(static_cast<unsigned char>(message.back())));
    EXPECT_EQ(message.find('\n'), std::string::npos);
    EXPECT_EQ(message.find('\r'), std::string::npos);
    EXPECT_EQ(message.rfind("Win32 error", 0), std::string::npos);
}

TEST(Win32Error, CodeWithoutSystemTextHasFallbackMessage) {
    EXPECT_EQ(win32_message(0x7FFF'FFFFu), "Win32 error 2147483647");
}

TEST(Win32Error, BuildsErrorWithContextAndNativeCode) {
    const Error error = win32_error(ERROR_ACCESS_DENIED, "core.test.open", "cannot open 'x.cr3'");
    EXPECT_EQ(error.code, ErrorCode::PermissionDenied);
    EXPECT_EQ(error.where, "core.test.open");
    EXPECT_EQ(error.native_code, ERROR_ACCESS_DENIED);
    EXPECT_EQ(error.message, "cannot open 'x.cr3': " + win32_message(ERROR_ACCESS_DENIED));
    EXPECT_NE(error.describe().find("(permission_denied, native 5)"), std::string::npos);
}

TEST(Win32Error, EmptyContextUsesSystemTextOnly) {
    const Error error = win32_error(ERROR_DISK_FULL, "core.test", "");
    EXPECT_EQ(error.code, ErrorCode::DiskFull);
    EXPECT_EQ(error.message, win32_message(ERROR_DISK_FULL));
}

TEST(Win32Error, LastErrorReadsTheThreadValue) {
    ::SetLastError(ERROR_FILE_NOT_FOUND);
    // Short literals stay within the small-string buffer, so building the arguments does not
    // allocate and cannot disturb the last-error value.
    const Error error = last_error("core.test", "");
    EXPECT_EQ(error.code, ErrorCode::NotFound);
    EXPECT_EQ(error.native_code, ERROR_FILE_NOT_FOUND);
    EXPECT_EQ(error.where, "core.test");
}

}  // namespace
}  // namespace lucent::core
