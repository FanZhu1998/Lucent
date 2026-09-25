#pragma once

// Versions: semantic versions of software and assets, versions of persisted/exchanged
// contracts, and the identity of this build (blueprint section 8: "Version these contracts
// independently ... Never interpret a newer unknown operation as a no-op").

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lucent/core/error.hpp"

namespace lucent::core {

// A Semantic Versioning 2.0.0 version (https://semver.org/spec/v2.0.0.html), used for the
// application, operation semantics, models and asset packs.
//
// Comparison operators implement SemVer precedence (spec item 11): major, minor and patch
// compare numerically; a pre-release version has lower precedence than the associated normal
// version; pre-release identifiers compare left to right, numeric identifiers numerically,
// alphanumeric identifiers in ASCII order, numeric below alphanumeric, and a longer identifier
// list wins when all preceding identifiers are equal. Build metadata is ignored for precedence,
// so the ordering is weak: 1.0.0+a == 1.0.0+b although to_string() differs. Compare to_string()
// when textual identity matters.
struct SemanticVersion {
    std::uint64_t major = 0;
    std::uint64_t minor = 0;
    std::uint64_t patch = 0;
    std::vector<std::string> prerelease;  // dot-separated identifiers after '-', may be empty
    std::vector<std::string> build;       // dot-separated identifiers after '+', may be empty

    // Strict SemVer 2.0.0 parse of the whole string: no leading 'v', no surrounding
    // whitespace, no leading zeros in numeric identifiers (build identifiers excepted), no
    // empty identifiers, identifiers limited to [0-9A-Za-z-]. Major, minor and patch must fit
    // in 64 bits. Violations are InvalidArgument.
    [[nodiscard]] static Expected<SemanticVersion> parse(std::string_view text);

    // Canonical text, e.g. "1.2.3-rc.1+build.5". parse(to_string()) reproduces the value.
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] bool is_prerelease() const noexcept { return !prerelease.empty(); }

    friend std::weak_ordering operator<=>(const SemanticVersion& a, const SemanticVersion& b) noexcept;
    friend bool operator==(const SemanticVersion& a, const SemanticVersion& b) noexcept {
        return (a <=> b) == 0;
    }
};

// Version of a persisted or exchanged contract (catalog schema, recipe schema, worker
// protocol, model manifest, camera profile database). Each contract is versioned
// independently:
//  - major: incompatible change; a reader supports exactly one major version and anything
//    else requires an explicit migration;
//  - minor: backward-compatible change within a major version; a reader for minor N reads
//    every minor <= N.
struct ContractVersion {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;

    // "<major>.<minor>" with decimal numbers without leading zeros, e.g. "2.1".
    [[nodiscard]] static Expected<ContractVersion> parse(std::string_view text);
    [[nodiscard]] std::string to_string() const;

    friend auto operator<=>(const ContractVersion&, const ContractVersion&) = default;
};

// Conservative readability check used by default for every contract. Returns ok() when
// `found` (the version recorded in the data) can be read by a reader supporting `supported`:
// same major version and found.minor <= supported.minor. Otherwise VersionMismatch, with
// `contract_name` (e.g. "recipe schema") in the message:
//  - a different major version (older or newer) needs an explicit migration;
//  - a newer minor version may contain additions this reader does not understand, and
//    silently ignoring them could change pixels, so it is rejected as well.
[[nodiscard]] Status check_readable(ContractVersion found, ContractVersion supported,
                                    std::string_view contract_name);

// Opt-in variant for contracts whose minor revisions are documented as purely additive AND
// whose readers are proven to ignore unknown additions safely (for example optional
// informational fields in a diagnostics report). Accepts any minor version with the same
// major; a different major is still VersionMismatch. Never use it for recipes, operation
// semantics or anything else that affects pixels.
[[nodiscard]] Status check_readable_allowing_newer_minor(ContractVersion found, ContractVersion supported,
                                                         std::string_view contract_name);

// Identity of this build, for logs, recipes (renderer version), diagnostics and support
// reports. All strings have static storage duration.
struct BuildInfo {
    std::string_view version;     // SemVer of the product (CMake PROJECT_VERSION), e.g. "0.1.0"
    std::string_view git_commit;  // abbreviated commit hash at configure time, or "unknown"
    std::string_view compiler;    // compiler id and version, e.g. "MSVC 19.44.35229.0"
    std::string_view build_type;  // CMake configuration, e.g. "RelWithDebInfo"
};

[[nodiscard]] BuildInfo build_info() noexcept;

}  // namespace lucent::core
