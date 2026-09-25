// Unit tests for the mapping of LibRaw return codes to Lucent error codes.

#include <cerrno>

#include <gtest/gtest.h>
#include <libraw.h>

#include <lucent/raw/libraw_decoder.hpp>

namespace lucent::raw {
namespace {

TEST(LibRawErrorMapping, EveryLibRawCodeHasADocumentedCategory) {
    struct Case {
        int code;
        ErrorCode expected;
    };
    const Case cases[] = {
        {LIBRAW_UNSPECIFIED_ERROR, ErrorCode::Unknown},
        {LIBRAW_FILE_UNSUPPORTED, ErrorCode::Unsupported},
        {LIBRAW_REQUEST_FOR_NONEXISTENT_IMAGE, ErrorCode::NotFound},
        {LIBRAW_OUT_OF_ORDER_CALL, ErrorCode::Internal},
        {LIBRAW_NO_THUMBNAIL, ErrorCode::NotFound},
        {LIBRAW_UNSUPPORTED_THUMBNAIL, ErrorCode::Unsupported},
        {LIBRAW_INPUT_CLOSED, ErrorCode::Internal},
        {LIBRAW_NOT_IMPLEMENTED, ErrorCode::Unsupported},
        {LIBRAW_REQUEST_FOR_NONEXISTENT_THUMBNAIL, ErrorCode::NotFound},
        {LIBRAW_UNSUFFICIENT_MEMORY, ErrorCode::OutOfMemory},
        {LIBRAW_DATA_ERROR, ErrorCode::Corrupt},
        {LIBRAW_IO_ERROR, ErrorCode::IoError},
        {LIBRAW_CANCELLED_BY_CALLBACK, ErrorCode::Cancelled},
        {LIBRAW_BAD_CROP, ErrorCode::Internal},
        {LIBRAW_TOO_BIG, ErrorCode::LimitExceeded},
        {LIBRAW_MEMPOOL_OVERFLOW, ErrorCode::LimitExceeded},
        {LIBRAW_SUCCESS, ErrorCode::Internal},
        {ENOENT, ErrorCode::NotFound},
        {EACCES, ErrorCode::IoError},
        {-123456, ErrorCode::Unknown},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(libraw_error_code(c.code), c.expected) << "LibRaw code " << c.code;
    }
}

TEST(LibRawErrorMapping, TextComesFromLibRaw) {
    EXPECT_EQ(libraw_error_text(LIBRAW_FILE_UNSUPPORTED), LibRaw::strerror(LIBRAW_FILE_UNSUPPORTED));
    EXPECT_FALSE(libraw_error_text(LIBRAW_IO_ERROR).empty());
    EXPECT_FALSE(libraw_error_text(-123456).empty());
}

TEST(LibRawBuild, ReportsTheLinkedVersion) {
    const LibRawBuildInfo info = libraw_build_info();
    EXPECT_EQ(info.version, LibRaw::version());
    EXPECT_EQ(info.version_number, static_cast<std::uint32_t>(LIBRAW_VERSION));
    // The vcpkg build Lucent pins is 0.22.x with zlib and libjpeg (DNG deflate and lossy codecs).
    EXPECT_EQ(info.version_number >> 8, (0u << 8) | 22u);
    EXPECT_NE(std::find(info.capabilities.begin(), info.capabilities.end(), "zlib"), info.capabilities.end());
    EXPECT_NE(std::find(info.capabilities.begin(), info.capabilities.end(), "jpeg"), info.capabilities.end());
    // No Adobe DNG SDK: DNG opcode lists are never applied by LibRaw in this build.
    EXPECT_EQ(std::find(info.capabilities.begin(), info.capabilities.end(), "dngsdk"),
              info.capabilities.end());
}

}  // namespace
}  // namespace lucent::raw
