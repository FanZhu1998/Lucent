#include "lucent/core/error.hpp"

#include <string>
#include <type_traits>

#include <gtest/gtest.h>

namespace lucent {
namespace {

constexpr auto value_of(ErrorCode code) {
    return static_cast<std::underlying_type_t<ErrorCode>>(code);
}

// Numeric values are persisted in job records and batch reports; they must never change.
static_assert(value_of(ErrorCode::Unknown) == 1);
static_assert(value_of(ErrorCode::InvalidArgument) == 2);
static_assert(value_of(ErrorCode::NotFound) == 3);
static_assert(value_of(ErrorCode::AlreadyExists) == 4);
static_assert(value_of(ErrorCode::PermissionDenied) == 5);
static_assert(value_of(ErrorCode::IoError) == 6);
static_assert(value_of(ErrorCode::DiskFull) == 7);
static_assert(value_of(ErrorCode::Corrupt) == 8);
static_assert(value_of(ErrorCode::Unsupported) == 9);
static_assert(value_of(ErrorCode::LimitExceeded) == 10);
static_assert(value_of(ErrorCode::OutOfMemory) == 11);
static_assert(value_of(ErrorCode::Cancelled) == 12);
static_assert(value_of(ErrorCode::Timeout) == 13);
static_assert(value_of(ErrorCode::VersionMismatch) == 14);
static_assert(value_of(ErrorCode::Internal) == 15);
static_assert(value_of(ErrorCode::SourceChanged) == 16);

TEST(ErrorModel, EveryCodeHasADistinctStableName) {
    const std::pair<ErrorCode, std::string_view> expected[] = {
        {ErrorCode::Unknown, "unknown"},
        {ErrorCode::InvalidArgument, "invalid_argument"},
        {ErrorCode::NotFound, "not_found"},
        {ErrorCode::AlreadyExists, "already_exists"},
        {ErrorCode::PermissionDenied, "permission_denied"},
        {ErrorCode::IoError, "io_error"},
        {ErrorCode::DiskFull, "disk_full"},
        {ErrorCode::Corrupt, "corrupt"},
        {ErrorCode::Unsupported, "unsupported"},
        {ErrorCode::LimitExceeded, "limit_exceeded"},
        {ErrorCode::OutOfMemory, "out_of_memory"},
        {ErrorCode::Cancelled, "cancelled"},
        {ErrorCode::Timeout, "timeout"},
        {ErrorCode::VersionMismatch, "version_mismatch"},
        {ErrorCode::Internal, "internal"},
        {ErrorCode::SourceChanged, "source_changed"},
    };
    for (const auto& [code, name] : expected) {
        EXPECT_EQ(to_string(code), name) << "code " << value_of(code);
    }
}

TEST(ErrorModel, UnknownNumericValueFallsBackToUnknown) {
    EXPECT_EQ(to_string(static_cast<ErrorCode>(999)), "unknown");
}

TEST(ErrorModel, DescribeIncludesLocationCodeAndNativeCode) {
    const Error error{ErrorCode::NotFound, "no such file", "core.file.open", 2};
    EXPECT_EQ(error.describe(), "core.file.open: no such file (not_found, native 2)");
}

TEST(ErrorModel, DescribeOmitsEmptyLocationAndZeroNativeCode) {
    const Error error{ErrorCode::SourceChanged, "source file changed while it was being read", "", 0};
    EXPECT_EQ(error.describe(), "source file changed while it was being read (source_changed)");
}

TEST(ErrorModel, FailProducesUnexpectedValue) {
    const Expected<int> result = fail(ErrorCode::LimitExceeded, "too big", "core.test", 7);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::LimitExceeded);
    EXPECT_EQ(result.error().message, "too big");
    EXPECT_EQ(result.error().where, "core.test");
    EXPECT_EQ(result.error().native_code, 7);
}

TEST(ErrorModel, OkIsSuccessfulStatus) {
    const Status status = ok();
    EXPECT_TRUE(status.has_value());
}

}  // namespace
}  // namespace lucent
