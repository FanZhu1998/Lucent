#pragma once

// Authenticode signatures of Windows programs (WinVerifyTrust). Lucent's update channel checks
// that a downloaded installer carries a valid signature by the publisher's certificate when the
// build names one (blueprint section 8; section 9: "Sign the installer and application/worker
// binaries using Authenticode"; ADR 0009 decision 13, ADR 0022).
//
// Policy of check_authenticode(): WINTRUST_ACTION_GENERIC_VERIFY_V2 without user interface, without
// revocation checking and without network retrieval (WTD_CACHE_ONLY_URL_RETRIEVAL): the digest of
// the file and the certificate chain up to a root this machine trusts. This is the check
// tools\package.ps1 and installer\tests\Test-InstalledPackage.ps1 make (LucentTestTrust). Revocation
// is not checked: a check that needs a certificate authority's server would fail for reasons that
// have nothing to do with the file, and the update channel's own signature (signature.hpp) is the
// primary authentication.
//
// Thread-safety: reentrant.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "lucent/core/error.hpp"

namespace lucent::core {

// WinVerifyTrust results (HRESULTs) callers commonly distinguish.
inline constexpr std::int32_t kTrustSucceeded = 0;
inline constexpr std::int32_t kTrustNoSignature = static_cast<std::int32_t>(0x800B0100u);   // TRUST_E_NOSIGNATURE
inline constexpr std::int32_t kTrustBadDigest = static_cast<std::int32_t>(0x80096010u);     // TRUST_E_BAD_DIGEST
inline constexpr std::int32_t kTrustUntrustedRoot = static_cast<std::int32_t>(0x800B0109u); // CERT_E_UNTRUSTEDROOT
inline constexpr std::int32_t kTrustExpired = static_cast<std::int32_t>(0x800B0101u);       // CERT_E_EXPIRED
inline constexpr std::int32_t kTrustDistrusted = static_cast<std::int32_t>(0x800B0111u);    // TRUST_E_EXPLICIT_DISTRUST

// The certificate that made the signature.
struct AuthenticodeSigner {
    std::string subject;  // simple display name (CERT_NAME_SIMPLE_DISPLAY_TYPE), UTF-8
    std::string issuer;   // the issuer's simple display name, UTF-8
    std::string sha1;     // lowercase hex SHA-1 thumbprint of the certificate (what signtool /sha1 takes)
    std::string sha256;   // lowercase hex SHA-256 of the certificate's DER encoding
};

struct AuthenticodeCheck {
    // WinVerifyTrust's result: kTrustSucceeded (0) when the signature verifies and chains to a root
    // this machine trusts; otherwise the HRESULT saying why not.
    std::int32_t result = kTrustNoSignature;
    // The signer, when the file carries a signature WinVerifyTrust could read (also when it does not
    // verify, for example with an untrusted root or a bad digest).
    std::optional<AuthenticodeSigner> signer;
    // The signature carries a counter-signature (a timestamp).
    bool timestamped = false;

    [[nodiscard]] bool trusted() const noexcept { return result == kTrustSucceeded; }
};

// Short English description of a WinVerifyTrust result for logs ("trusted", "not signed",
// "the file was changed after it was signed", "the certificate's root is not trusted", ... with the
// HRESULT).
[[nodiscard]] std::string describe_trust_result(std::int32_t result);

// Checks the Authenticode signature of the file at `path` with the policy of the file comment. With
// `file` (a Win32 HANDLE opened with read access) WinVerifyTrust reads that open file object, so the
// file the caller holds is the one checked. A file that is not signed or whose signature does not
// verify is not an error (see AuthenticodeCheck::result). Errors: InvalidArgument for an empty path
// or one with a NUL ("core.authenticode"); OutOfMemory.
[[nodiscard]] Expected<AuthenticodeCheck> check_authenticode(const std::filesystem::path& path,
                                                             void* file = nullptr);

}  // namespace lucent::core
