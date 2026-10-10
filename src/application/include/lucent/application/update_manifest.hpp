#pragma once

// The update channel's documents (blueprint section 8: "Version 1 uses a signed full installer and a
// graphical Check for updates flow ... Verify downloaded installers ... against an authenticated
// release manifest or signature tied to the trusted publisher identity before activation; hashes
// alone do not authenticate a download"; ADR 0022).
//
// Release manifest: "lucent.release-manifest", schema version 1, a JSON object with exactly these
// members (strict: an unknown or repeated member, a missing one or a malformed value is Corrupt; a
// newer schema_version is VersionMismatch):
//
//   {"schema": "lucent.release-manifest", "schema_version": 1,
//    "product": "Lucent",                 always "Lucent"
//    "channel": "stable",                 [a-z0-9] words joined by '-', at most 32 characters
//    "version": "0.2.0",                  SemVer 2.0.0 of the release (core::SemanticVersion)
//    "published": "2026-10-09T12:00:00Z", UTC, this exact form
//    "architecture": "x64",               always "x64" in version 1
//    "minimum_os": "10.0.17763",          the oldest Windows the release runs on (major.minor.build)
//    "installer": {"url": "https://...", the full installer: an absolute http(s) URL of at most 2048
//                                         printable ASCII characters (the transport fetches https
//                                         only, http_client.hpp; tests serve loopback http)
//                  "size": 41869312,      its exact size in bytes, 1 ... kMaxInstallerBytes
//                  "sha256": "<64 lowercase hex digits>"},
//    "release_notes": "..."}              plain text (never markup), at most kMaxReleaseNotesBytes
//
// Release signature: "lucent.release-signature", schema version 1, the detached signature of the
// manifest's exact bytes, served beside it at <manifest URL>.sig:
//
//   {"schema": "lucent.release-signature", "schema_version": 1, "algorithm": "ecdsa-p256-sha256",
//    "key_id": "<64 lowercase hex digits: core::key_id() of the signing key>",
//    "signature": "<standard base64 of the 64-byte IEEE P1363 signature r || s>"}
//
// verify_release_manifest() authenticates before it interprets: it parses the signature document,
// finds the pinned key with its key id, verifies the signature over the manifest's bytes, and only
// then parses the manifest. A manifest whose signature does not verify is never read.
//
// write_release_manifest() and write_release_signature() produce the canonical form (members in the
// order above, no whitespace, LF never needed): tests use them, and the publisher's tool
// (installer\scripts\LucentUpdate.psm1) writes the same text. Readers accept any JSON formatting:
// the signature covers whatever bytes were signed.
//
// Thread-safety: pure functions.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <lucent/core/error.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/core/signature.hpp>
#include <lucent/core/version.hpp>

namespace lucent::app {

inline constexpr std::string_view kReleaseManifestSchema = "lucent.release-manifest";
inline constexpr std::uint32_t kReleaseManifestVersion = 1;
inline constexpr std::string_view kReleaseSignatureSchema = "lucent.release-signature";
inline constexpr std::uint32_t kReleaseSignatureVersion = 1;
inline constexpr std::string_view kReleaseSignatureAlgorithm = "ecdsa-p256-sha256";
inline constexpr std::string_view kReleaseProduct = "Lucent";
inline constexpr std::string_view kReleaseArchitecture = "x64";

// Size limits of what the channel serves.
inline constexpr std::uint64_t kMaxReleaseManifestBytes = 64 * 1024;
inline constexpr std::uint64_t kMaxReleaseSignatureBytes = 4 * 1024;
inline constexpr std::uint64_t kMaxReleaseNotesBytes = 16 * 1024;
inline constexpr std::uint64_t kMaxInstallerBytes = std::uint64_t{2} << 30;  // 2 GiB
inline constexpr std::size_t kMaxReleaseUrlLength = 2048;

// A Windows version: major.minor.build (RtlGetVersion's numbers).
struct WindowsVersion {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t build = 0;

    // "10.0.17763": three decimal numbers without leading zeros or signs. InvalidArgument otherwise.
    [[nodiscard]] static Expected<WindowsVersion> parse(std::string_view text);
    [[nodiscard]] std::string to_string() const;

    friend auto operator<=>(const WindowsVersion&, const WindowsVersion&) = default;
};

// The Windows version this process runs on (RtlGetVersion, which no compatibility manifest
// changes).
[[nodiscard]] WindowsVersion running_windows_version();

struct ReleaseInstaller {
    std::string url;
    std::uint64_t size = 0;
    core::Digest256 sha256{};
};

struct ReleaseManifest {
    std::string channel;
    core::SemanticVersion version;
    std::string published;  // "YYYY-MM-DDTHH:MM:SSZ"
    std::string architecture = std::string(kReleaseArchitecture);
    WindowsVersion minimum_os;
    ReleaseInstaller installer;
    std::string release_notes;
};

struct ReleaseSignature {
    std::string key_id;  // 64 lowercase hex digits
    core::EcdsaP256Signature signature{};
};

// A manifest that verified, with the key that signed it.
struct VerifiedReleaseManifest {
    ReleaseManifest manifest;
    std::string key_id;
    core::Digest256 manifest_sha256{};  // of the verified bytes
};

// True for a channel name of the form in the file comment.
[[nodiscard]] bool is_valid_channel_name(std::string_view name) noexcept;

// Strict readers (see the file comment). Errors use "app.update.manifest" and
// "app.update.signature".
[[nodiscard]] Expected<ReleaseManifest> parse_release_manifest(std::string_view json);
[[nodiscard]] Expected<ReleaseSignature> parse_release_signature(std::string_view json);

// Canonical writers. InvalidArgument for a manifest the reader would refuse.
[[nodiscard]] Expected<std::string> write_release_manifest(const ReleaseManifest& manifest);
[[nodiscard]] std::string write_release_signature(const ReleaseSignature& signature);

// Authenticates `manifest_bytes` with `signature_json` against the pinned `keys`, then parses it
// (see the file comment). Errors ("app.update.signature" unless the manifest itself is refused):
//   * the signature document is malformed: Corrupt (VersionMismatch for a newer one);
//   * no pinned key has its key id: PermissionDenied ("app.update.signature.unknown_key");
//   * the signature does not verify: PermissionDenied ("app.update.signature.mismatch");
//   * then parse_release_manifest()'s errors.
// InvalidArgument when `keys` is empty or the manifest exceeds kMaxReleaseManifestBytes.
[[nodiscard]] Expected<VerifiedReleaseManifest> verify_release_manifest(std::string_view manifest_bytes,
                                                                        std::string_view signature_json,
                                                                        std::span<const core::EcdsaP256PublicKey> keys);

}  // namespace lucent::app
