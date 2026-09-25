#include "lucent/core/version.hpp"

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace lucent::core {
namespace {

SemanticVersion semver(std::string_view text) {
    auto version = SemanticVersion::parse(text);
    EXPECT_TRUE(version) << text << ": " << (version ? "" : version.error().describe());
    return version ? *version : SemanticVersion{};
}

// ---- SemanticVersion ------------------------------------------------------------------------

TEST(SemanticVersion, ParsesSpecExamplesAndRoundTrips) {
    const std::string_view cases[] = {
        "0.0.0",
        "1.2.3",
        "10.20.30",
        "1.0.0-alpha",
        "1.0.0-alpha.1",
        "1.0.0-0.3.7",
        "1.0.0-x.7.z.92",
        "1.0.0-x-y-z.--",
        "1.0.0-alpha+001",
        "1.0.0+20130313144700",
        "1.0.0-beta+exp.sha.5114f85",
        "1.0.0+21AF26D3----117B344092BD",
        "1.0.0-0A.is.legal",
        "18446744073709551615.18446744073709551615.18446744073709551615",
    };
    for (const std::string_view text : cases) {
        const auto version = SemanticVersion::parse(text);
        ASSERT_TRUE(version) << text << ": " << version.error().describe();
        EXPECT_EQ(version->to_string(), text);
    }
}

TEST(SemanticVersion, ExposesComponents) {
    const SemanticVersion version = semver("1.22.333-rc.1+build.7");
    EXPECT_EQ(version.major, 1u);
    EXPECT_EQ(version.minor, 22u);
    EXPECT_EQ(version.patch, 333u);
    EXPECT_EQ(version.prerelease, (std::vector<std::string>{"rc", "1"}));
    EXPECT_EQ(version.build, (std::vector<std::string>{"build", "7"}));
    EXPECT_TRUE(version.is_prerelease());
    EXPECT_FALSE(semver("1.0.0+build").is_prerelease());
}

TEST(SemanticVersion, RejectsInvalidText) {
    const std::string_view cases[] = {
        "",
        "1",
        "1.2",
        "1.2.3.4",
        "01.2.3",
        "1.02.3",
        "1.2.03",
        "v1.2.3",
        " 1.2.3",
        "1.2.3 ",
        "1.2.3-",
        "1.2.3+",
        "1.2.3-+b",
        "1.2.3-01",  // numeric pre-release identifier with a leading zero
        "1.2.3-a..b",
        "1.2.3-a.",
        "1.2.3-a_b",
        "1.2.3+a..b",
        "1.2.3+a+b",
        "1.2.3-\xC3\xA9",            // non-ASCII identifier
        "18446744073709551616.0.0",  // overflows 64 bits
        "-1.2.3",
        "1.-2.3",
        "1..3",
        "a.b.c",
    };
    for (const std::string_view text : cases) {
        const auto version = SemanticVersion::parse(text);
        ASSERT_FALSE(version) << "accepted: " << text;
        EXPECT_EQ(version.error().code, ErrorCode::InvalidArgument);
        EXPECT_EQ(version.error().where, "core.version.parse_semver");
    }
    // Embedded NUL.
    EXPECT_FALSE(SemanticVersion::parse(std::string_view("1.2.3\0x", 7)));
}

TEST(SemanticVersion, BuildMetadataAllowsLeadingZeros) {
    EXPECT_TRUE(SemanticVersion::parse("1.0.0+001.0002"));
}

TEST(SemanticVersion, PrecedenceFollowsTheSpecification) {
    // SemVer 2.0.0 item 11 examples, in ascending precedence.
    const std::vector<std::string_view> ordered = {
        "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta",
        "1.0.0-beta",  "1.0.0-beta.2",  "1.0.0-beta.11",
        "1.0.0-rc.1",  "1.0.0",         "2.0.0",
        "2.1.0",       "2.1.1",
    };
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        for (std::size_t j = 0; j < ordered.size(); ++j) {
            const SemanticVersion a = semver(ordered[i]);
            const SemanticVersion b = semver(ordered[j]);
            EXPECT_EQ(a < b, i < j) << ordered[i] << " vs " << ordered[j];
            EXPECT_EQ(a == b, i == j) << ordered[i] << " vs " << ordered[j];
            EXPECT_EQ(a > b, i > j) << ordered[i] << " vs " << ordered[j];
        }
    }
}

TEST(SemanticVersion, IdentifierComparisonRules) {
    EXPECT_LT(semver("1.0.0-2"), semver("1.0.0-10"));   // numeric, not lexical
    EXPECT_LT(semver("1.0.0-999"), semver("1.0.0-a"));  // numeric below alphanumeric
    EXPECT_LT(semver("1.0.0-B"), semver("1.0.0-a"));    // ASCII order: upper case first
    EXPECT_LT(semver("1.0.0-a"), semver("1.0.0-a.0"));  // more identifiers win
    EXPECT_LT(semver("1.0.0-rc.1"), semver("1.0.0-rc.1.x"));
    EXPECT_LT(semver("1.9.0"), semver("1.10.0"));
    // Numeric identifiers beyond 64 bits still compare numerically.
    EXPECT_LT(semver("1.0.0-99999999999999999999999"), semver("1.0.0-100000000000000000000000"));
}

TEST(SemanticVersion, BuildMetadataIsIgnoredForPrecedence) {
    const SemanticVersion a = semver("1.0.0+linux");
    const SemanticVersion b = semver("1.0.0+windows.2");
    EXPECT_EQ(a, b);
    EXPECT_EQ(a <=> b, std::weak_ordering::equivalent);
    EXPECT_NE(a.to_string(), b.to_string());
    EXPECT_EQ(semver("1.0.0-rc.1+a"), semver("1.0.0-rc.1"));
    EXPECT_LT(semver("1.0.0-rc.1+zzz"), semver("1.0.0+aaa"));
}

// ---- ContractVersion ------------------------------------------------------------------------

TEST(ContractVersion, ParsesAndFormats) {
    const auto version = ContractVersion::parse("2.13");
    ASSERT_TRUE(version) << version.error().describe();
    EXPECT_EQ(version->major, 2u);
    EXPECT_EQ(version->minor, 13u);
    EXPECT_EQ(version->to_string(), "2.13");
    EXPECT_EQ(ContractVersion::parse("0.0")->to_string(), "0.0");
    EXPECT_EQ(ContractVersion::parse("4294967295.0")->major, 4294967295u);

    for (const std::string_view bad :
         {"", "2", "2.1.0", "02.1", "2.01", "2.x", "-1.0", "2.", ".1", "4294967296.0", " 2.1"}) {
        const auto parsed = ContractVersion::parse(bad);
        ASSERT_FALSE(parsed) << "accepted: " << bad;
        EXPECT_EQ(parsed.error().code, ErrorCode::InvalidArgument);
    }
}

TEST(ContractVersion, Ordering) {
    EXPECT_LT((ContractVersion{1, 9}), (ContractVersion{2, 0}));
    EXPECT_LT((ContractVersion{2, 0}), (ContractVersion{2, 1}));
    EXPECT_EQ((ContractVersion{2, 1}), (ContractVersion{2, 1}));
}

TEST(ContractVersion, CheckReadableIsConservative) {
    const ContractVersion supported{2, 3};
    EXPECT_TRUE(check_readable({2, 3}, supported, "recipe schema"));
    EXPECT_TRUE(check_readable({2, 0}, supported, "recipe schema"));  // older minor: additive changes only

    const Status newer_minor = check_readable({2, 4}, supported, "recipe schema");
    ASSERT_FALSE(newer_minor);
    EXPECT_EQ(newer_minor.error().code, ErrorCode::VersionMismatch);
    EXPECT_NE(newer_minor.error().message.find("recipe schema"), std::string::npos);
    EXPECT_NE(newer_minor.error().message.find("2.4"), std::string::npos);
    EXPECT_NE(newer_minor.error().message.find("2.3"), std::string::npos);

    for (const ContractVersion other_major : {ContractVersion{1, 9}, ContractVersion{3, 0}}) {
        const Status status = check_readable(other_major, supported, "catalog schema");
        ASSERT_FALSE(status) << other_major.to_string();
        EXPECT_EQ(status.error().code, ErrorCode::VersionMismatch);
        EXPECT_NE(status.error().message.find("catalog schema"), std::string::npos);
    }
}

TEST(ContractVersion, AdditiveMinorOptInAcceptsNewerMinorOnly) {
    const ContractVersion supported{1, 2};
    EXPECT_TRUE(check_readable_allowing_newer_minor({1, 7}, supported, "diagnostics report"));
    EXPECT_TRUE(check_readable_allowing_newer_minor({1, 0}, supported, "diagnostics report"));
    const Status other_major = check_readable_allowing_newer_minor({2, 0}, supported, "diagnostics report");
    ASSERT_FALSE(other_major);
    EXPECT_EQ(other_major.error().code, ErrorCode::VersionMismatch);
}

// ---- BuildInfo ------------------------------------------------------------------------------

TEST(BuildInfo, DescribesThisBuild) {
    const BuildInfo info = build_info();
    const auto version = SemanticVersion::parse(info.version);
    EXPECT_TRUE(version) << info.version;
    EXPECT_FALSE(info.git_commit.empty());
    EXPECT_TRUE(info.compiler.starts_with("MSVC ")) << info.compiler;
    EXPECT_FALSE(info.build_type.empty());
}

}  // namespace
}  // namespace lucent::core
