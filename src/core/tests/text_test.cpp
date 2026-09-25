#include "lucent/core/text.hpp"

#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace lucent::core {
namespace {

// With /utf-8, narrow literals are UTF-8 and wide literals are UTF-16.
constexpr std::string_view kUnicodeUtf8 =
    "\xC3\x9Cn\xC3\xAF"
    "c\xC3\xB8"
    "d\xC3\xA9 \xE5\x86\x99\xE7\x9C\x9F \xF0\x9F\x93\xB7";
constexpr std::wstring_view kUnicodeWide = L"Ünïcødé 写真 📷";
constexpr std::string_view kReplacement = "\xEF\xBF\xBD";  // U+FFFD

TEST(Text, SourceLiteralsAgree) {
    // Guards the test data itself: the escaped UTF-8 spells the same text as the literal.
    EXPECT_EQ(kUnicodeUtf8, std::string_view("Ünïcødé 写真 📷"));
    EXPECT_EQ(kUnicodeWide.size(), 13u);  // 11 BMP characters + 📷 as a surrogate pair
}

TEST(Text, Utf8ToWideRoundTripsUnicode) {
    const auto wide = utf8_to_wide(kUnicodeUtf8);
    ASSERT_TRUE(wide) << wide.error().describe();
    EXPECT_EQ(*wide, kUnicodeWide);

    const auto utf8 = wide_to_utf8(*wide);
    ASSERT_TRUE(utf8) << utf8.error().describe();
    EXPECT_EQ(*utf8, kUnicodeUtf8);
}

TEST(Text, EmptyStringsConvertToEmpty) {
    const auto wide = utf8_to_wide("");
    ASSERT_TRUE(wide);
    EXPECT_TRUE(wide->empty());
    const auto utf8 = wide_to_utf8(L"");
    ASSERT_TRUE(utf8);
    EXPECT_TRUE(utf8->empty());
}

TEST(Text, EmbeddedNulIsPreserved) {
    const auto wide = utf8_to_wide(std::string_view("a\0b", 3));
    ASSERT_TRUE(wide);
    EXPECT_EQ(*wide, std::wstring_view(L"a\0b", 3));
    const auto utf8 = wide_to_utf8(std::wstring_view(L"a\0b", 3));
    ASSERT_TRUE(utf8);
    EXPECT_EQ(*utf8, std::string_view("a\0b", 3));
}

TEST(Text, AcceptsBoundaryCodePoints) {
    const std::pair<std::string_view, std::wstring_view> cases[] = {
        {"\x7F", L"\x007F"},
        {"\xC2\x80", L"\x0080"},
        {"\xDF\xBF", L"\x07FF"},
        {"\xE0\xA0\x80", L"\x0800"},
        {"\xED\x9F\xBF", L"\xD7FF"},
        {"\xEE\x80\x80", L"\xE000"},
        {"\xEF\xBF\xBF", L"\xFFFF"},
        {"\xF0\x90\x80\x80", L"\xD800\xDC00"},  // U+10000
        {"\xF4\x8F\xBF\xBF", L"\xDBFF\xDFFF"},  // U+10FFFF
    };
    for (const auto& [utf8, wide] : cases) {
        EXPECT_TRUE(is_valid_utf8(utf8));
        const auto converted = utf8_to_wide(utf8);
        ASSERT_TRUE(converted) << converted.error().describe();
        EXPECT_EQ(*converted, wide);
        const auto back = wide_to_utf8(wide);
        ASSERT_TRUE(back) << back.error().describe();
        EXPECT_EQ(*back, utf8);
    }
}

TEST(Text, RejectsMalformedUtf8) {
    const std::string_view cases[] = {
        "\x80",              // lone continuation byte
        "\xBF",              // lone continuation byte
        "\xC0\x80",          // overlong NUL
        "\xC1\xBF",          // overlong 2-byte
        "\xE0\x80\x80",      // overlong 3-byte
        "\xE0\x9F\xBF",      // overlong 3-byte (just below U+0800)
        "\xF0\x80\x80\x80",  // overlong 4-byte
        "\xF0\x8F\xBF\xBF",  // overlong 4-byte (just below U+10000)
        "\xED\xA0\x80",      // encoded surrogate U+D800
        "\xED\xBF\xBF",      // encoded surrogate U+DFFF
        "\xF4\x90\x80\x80",  // U+110000, above the Unicode range
        "\xF5\x80\x80\x80",  // invalid lead byte
        "\xFF",              // invalid byte
        "\xFE",              // invalid byte
        "\xE2\x82",          // truncated 3-byte sequence
        "abc\xE2\x82\xAC"
        "d\xF0\x9F\x93",  // valid prefix, truncated tail
        "\xC3"
        "A",  // lead byte followed by ASCII
    };
    for (const std::string_view text : cases) {
        EXPECT_FALSE(is_valid_utf8(text));
        const auto converted = utf8_to_wide(text);
        ASSERT_FALSE(converted);
        EXPECT_EQ(converted.error().code, ErrorCode::InvalidArgument);
        EXPECT_EQ(converted.error().where, "core.text.utf8_to_wide");
    }
}

TEST(Text, RejectsUnpairedSurrogates) {
    const std::wstring_view cases[] = {
        L"\xD800",
        L"\xDC00",
        L"a\xD83D",  // high surrogate at the end
        L"a\xDC00"
        L"b",             // lone low surrogate
        L"\xD800\xD800",  // two highs
    };
    for (const std::wstring_view text : cases) {
        const auto converted = wide_to_utf8(text);
        ASSERT_FALSE(converted);
        EXPECT_EQ(converted.error().code, ErrorCode::InvalidArgument);
        EXPECT_EQ(converted.error().where, "core.text.wide_to_utf8");
    }
}

TEST(Text, LossyConversionsReplaceMalformedInput) {
    EXPECT_EQ(wide_to_utf8_lossy(L"a\xD800"
                                 L"b"),
              std::string("a") + std::string(kReplacement) + "b");
    EXPECT_EQ(utf8_to_wide_lossy("a\xFF"
                                 "b"),
              std::wstring(L"a\xFFFD"
                           L"b"));
    EXPECT_EQ(wide_to_utf8_lossy(kUnicodeWide), kUnicodeUtf8);
    EXPECT_EQ(utf8_to_wide_lossy(kUnicodeUtf8), kUnicodeWide);
}

TEST(Text, SanitizeReplacesMaximalSubparts) {
    const std::string r(kReplacement);
    EXPECT_EQ(sanitize_utf8(kUnicodeUtf8), kUnicodeUtf8);  // valid input unchanged
    EXPECT_EQ(sanitize_utf8("\xE2\x82"
                            "A"),
              r + "A");                                   // one U+FFFD per truncated sequence
    EXPECT_EQ(sanitize_utf8("\xF0\x9F\x93"), r);          // truncated 4-byte sequence
    EXPECT_EQ(sanitize_utf8("\xC0\x80"), r + r);          // C0 is never a valid lead
    EXPECT_EQ(sanitize_utf8("\xED\xA0\x80"), r + r + r);  // surrogates: every byte replaced
    EXPECT_EQ(sanitize_utf8("ok\xFF"
                            "ok"),
              "ok" + r + "ok");
    EXPECT_TRUE(is_valid_utf8(sanitize_utf8("\x80\x81\xF5\xE0\x80")));
}

TEST(Text, PathRoundTripsThroughUtf8) {
    const std::string utf8 = std::string("C:\\Photos\\") + std::string(kUnicodeUtf8) + ".cr3";
    const auto path = path_from_utf8(utf8);
    ASSERT_TRUE(path) << path.error().describe();
    EXPECT_EQ(path->native(), std::wstring(L"C:\\Photos\\") + std::wstring(kUnicodeWide) + L".cr3");

    const auto back = path_to_utf8(*path);
    ASSERT_TRUE(back) << back.error().describe();
    EXPECT_EQ(*back, utf8);
    EXPECT_EQ(path_to_utf8_lossy(*path), utf8);
}

TEST(Text, PathWithUnpairedSurrogateIsNotSilentlyAltered) {
    const std::filesystem::path path(std::wstring(L"C:\\x\\bad\xD800") + L".cr3");
    const auto utf8 = path_to_utf8(path);
    ASSERT_FALSE(utf8);
    EXPECT_EQ(utf8.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(utf8.error().where, "core.text.path_to_utf8");
    EXPECT_EQ(path_to_utf8_lossy(path), std::string("C:\\x\\bad") + std::string(kReplacement) + ".cr3");
}

TEST(Text, PathFromMalformedUtf8Fails) {
    const auto path = path_from_utf8("C:\\photos\\\xC0\x80.cr3");
    ASSERT_FALSE(path);
    EXPECT_EQ(path.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(path.error().where, "core.text.path_from_utf8");
}

}  // namespace
}  // namespace lucent::core
