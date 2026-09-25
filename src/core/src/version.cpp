#include "lucent/core/version.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <limits>
#include <system_error>

#include "lucent/core/text.hpp"

#include "build_config.hpp"  // generated into ${CMAKE_CURRENT_BINARY_DIR}/generated

#ifndef LUCENT_BUILD_TYPE
#error "LUCENT_BUILD_TYPE must be defined by the build system (see src/core/CMakeLists.txt)"
#endif

namespace lucent::core {

namespace {

constexpr const char* kSemverWhere = "core.version.parse_semver";
constexpr const char* kContractWhere = "core.version.parse_contract";

bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

bool is_identifier_char(char c) noexcept {
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

bool all_digits(std::string_view text) noexcept {
    return !text.empty() && std::all_of(text.begin(), text.end(), is_digit);
}

// Splits on '.', keeping empty pieces so they can be rejected.
std::vector<std::string_view> split_dots(std::string_view text) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t dot = text.find('.', start);
        if (dot == std::string_view::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, dot - start));
        start = dot + 1;
    }
}

// Parses a numeric identifier: digits only, no leading zero unless it is "0", fits in T.
template <class T>
bool parse_number(std::string_view text, T& value) noexcept {
    if (!all_digits(text) || (text.size() > 1 && text.front() == '0')) {
        return false;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc() && end == text.data() + text.size();
}

// Compares two numeric identifiers of any length without overflow.
std::weak_ordering compare_numeric(std::string_view a, std::string_view b) noexcept {
    // Parsed values never have leading zeros, but hand-built values might; ignore them.
    const auto strip = [](std::string_view s) {
        const std::size_t first = s.find_first_not_of('0');
        return first == std::string_view::npos ? std::string_view() : s.substr(first);
    };
    a = strip(a);
    b = strip(b);
    if (a.size() != b.size()) {
        return a.size() <=> b.size();
    }
    return a.compare(b) <=> 0;
}

std::weak_ordering compare_identifier(std::string_view a, std::string_view b) noexcept {
    const bool a_numeric = all_digits(a);
    const bool b_numeric = all_digits(b);
    if (a_numeric && b_numeric) {
        return compare_numeric(a, b);
    }
    if (a_numeric != b_numeric) {
        return a_numeric ? std::weak_ordering::less : std::weak_ordering::greater;  // numeric < alphanumeric
    }
    return a.compare(b) <=> 0;  // ASCII order (char_traits<char> compares as unsigned char)
}

std::string join_dots(const std::vector<std::string>& parts) {
    std::string text;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            text.push_back('.');
        }
        text += parts[i];
    }
    return text;
}

Status check_major(ContractVersion found, ContractVersion supported, std::string_view contract_name) {
    if (found.major != supported.major) {
        return fail(
            ErrorCode::VersionMismatch,
            std::format("{} version {} is not readable by this build, which reads versions {}.0 to {}; a "
                        "different major version requires an explicit migration",
                        contract_name, found.to_string(), supported.major, supported.to_string()),
            "core.version.check_readable");
    }
    return ok();
}

}  // namespace

// ---- SemanticVersion ------------------------------------------------------------------------

Expected<SemanticVersion> SemanticVersion::parse(std::string_view text) {
    const auto invalid = [text](std::string_view reason) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("invalid semantic version '{}': {}", sanitize_utf8(text), reason),
                    kSemverWhere);
    };

    // Build metadata starts at the first '+'; the pre-release at the first '-' before it
    // (the version core never contains '-', identifiers may).
    std::string_view rest = text;
    std::string_view build_text;
    bool has_build = false;
    if (const std::size_t plus = rest.find('+'); plus != std::string_view::npos) {
        build_text = rest.substr(plus + 1);
        rest = rest.substr(0, plus);
        has_build = true;
    }
    std::string_view prerelease_text;
    bool has_prerelease = false;
    if (const std::size_t dash = rest.find('-'); dash != std::string_view::npos) {
        prerelease_text = rest.substr(dash + 1);
        rest = rest.substr(0, dash);
        has_prerelease = true;
    }

    SemanticVersion version;
    const std::vector<std::string_view> core = split_dots(rest);
    if (core.size() != 3) {
        return invalid("expected MAJOR.MINOR.PATCH");
    }
    if (!parse_number(core[0], version.major) || !parse_number(core[1], version.minor) ||
        !parse_number(core[2], version.patch)) {
        return invalid(
            "MAJOR, MINOR and PATCH must be decimal numbers without leading zeros that fit in 64 bits");
    }

    if (has_prerelease) {
        for (const std::string_view identifier : split_dots(prerelease_text)) {
            if (identifier.empty()) {
                return invalid("empty pre-release identifier");
            }
            if (!std::all_of(identifier.begin(), identifier.end(), is_identifier_char)) {
                return invalid("pre-release identifiers may contain only [0-9A-Za-z-]");
            }
            if (all_digits(identifier) && identifier.size() > 1 && identifier.front() == '0') {
                return invalid("numeric pre-release identifiers must not have leading zeros");
            }
            version.prerelease.emplace_back(identifier);
        }
    }
    if (has_build) {
        for (const std::string_view identifier : split_dots(build_text)) {
            if (identifier.empty()) {
                return invalid("empty build metadata identifier");
            }
            if (!std::all_of(identifier.begin(), identifier.end(), is_identifier_char)) {
                return invalid("build metadata identifiers may contain only [0-9A-Za-z-]");
            }
            version.build.emplace_back(identifier);
        }
    }
    return version;
}

std::string SemanticVersion::to_string() const {
    std::string text = std::format("{}.{}.{}", major, minor, patch);
    if (!prerelease.empty()) {
        text.push_back('-');
        text += join_dots(prerelease);
    }
    if (!build.empty()) {
        text.push_back('+');
        text += join_dots(build);
    }
    return text;
}

std::weak_ordering operator<=>(const SemanticVersion& a, const SemanticVersion& b) noexcept {
    if (const auto c = a.major <=> b.major; c != 0) {
        return c;
    }
    if (const auto c = a.minor <=> b.minor; c != 0) {
        return c;
    }
    if (const auto c = a.patch <=> b.patch; c != 0) {
        return c;
    }
    // A pre-release has lower precedence than the associated normal version.
    if (a.prerelease.empty() && b.prerelease.empty()) {
        return std::weak_ordering::equivalent;  // build metadata is ignored
    }
    if (a.prerelease.empty()) {
        return std::weak_ordering::greater;
    }
    if (b.prerelease.empty()) {
        return std::weak_ordering::less;
    }
    const std::size_t common = std::min(a.prerelease.size(), b.prerelease.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (const auto c = compare_identifier(a.prerelease[i], b.prerelease[i]); c != 0) {
            return c;
        }
    }
    return a.prerelease.size() <=> b.prerelease.size();  // more identifiers win
}

// ---- ContractVersion ------------------------------------------------------------------------

Expected<ContractVersion> ContractVersion::parse(std::string_view text) {
    const std::vector<std::string_view> parts = split_dots(text);
    ContractVersion version;
    if (parts.size() != 2 || !parse_number(parts[0], version.major) ||
        !parse_number(parts[1], version.minor)) {
        return fail(
            ErrorCode::InvalidArgument,
            std::format("invalid contract version '{}': expected MAJOR.MINOR with decimal numbers without "
                        "leading zeros that fit in 32 bits",
                        sanitize_utf8(text)),
            kContractWhere);
    }
    return version;
}

std::string ContractVersion::to_string() const {
    return std::format("{}.{}", major, minor);
}

Status check_readable(ContractVersion found, ContractVersion supported, std::string_view contract_name) {
    if (auto status = check_major(found, supported, contract_name); !status) {
        return status;
    }
    if (found.minor > supported.minor) {
        return fail(
            ErrorCode::VersionMismatch,
            std::format("{} version {} is newer than the newest version this build reads ({}); it may "
                        "contain changes this build does not understand",
                        contract_name, found.to_string(), supported.to_string()),
            "core.version.check_readable");
    }
    return ok();
}

Status check_readable_allowing_newer_minor(ContractVersion found, ContractVersion supported,
                                           std::string_view contract_name) {
    return check_major(found, supported, contract_name);
}

// ---- BuildInfo ------------------------------------------------------------------------------

BuildInfo build_info() noexcept {
    constexpr std::string_view kBuildType = LUCENT_BUILD_TYPE;
    return BuildInfo{
        LUCENT_BUILD_VERSION,
        LUCENT_BUILD_GIT_COMMIT,
        LUCENT_BUILD_COMPILER,
        kBuildType.empty() ? std::string_view("unspecified") : kBuildType,
    };
}

}  // namespace lucent::core
