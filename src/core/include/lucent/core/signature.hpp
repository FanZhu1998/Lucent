#pragma once

// Digital signatures over Windows CNG: ECDSA on the NIST P-256 curve with SHA-256 (FIPS 186-4,
// SEC 1). Lucent verifies the signed release manifests of its update channel with a public key
// compiled into the build (blueprint section 8: "Verify downloaded installers ... against an
// authenticated release manifest or signature tied to the trusted publisher identity"; ADR 0022).
//
// Encodings (the same as the publisher's tooling, installer\scripts\LucentUpdate.psm1, which signs
// through .NET's ECDsaCng on the same CNG provider):
//   * a public key is the uncompressed SEC 1 point 0x04 || X || Y (65 bytes; 130 hex digits);
//   * a signature is the IEEE P1363 form r || s (64 bytes, each a big-endian 32-byte integer), the
//     form CNG's BCryptSignHash produces and BCryptVerifySignature takes;
//   * the signed value is SHA-256 of the message bytes exactly as they are (no canonicalisation).
// A key is named by its key id: the lowercase hex SHA-256 of its 65-byte point.
//
// Key pairs. EcdsaP256KeyPair generates an ephemeral key pair in memory (BCryptGenerateKeyPair);
// it is never persisted and its private key cannot be exported. Tests and tools use it to sign;
// Lucent itself only verifies. Release keys live outside the repository, in the publisher's key
// storage provider (ADR 0022).
//
// Thread-safety: functions are reentrant; the CNG algorithm provider is opened once per process
// and shared (never closed, so verification stays valid during static destruction). An
// EcdsaP256KeyPair may be used from one thread at a time.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "lucent/core/error.hpp"

namespace lucent::core {

// An ECDSA P-256 public key: the uncompressed SEC 1 point 0x04 || X || Y.
struct EcdsaP256PublicKey {
    std::array<std::uint8_t, 65> point{};

    friend bool operator==(const EcdsaP256PublicKey&, const EcdsaP256PublicKey&) = default;
};

// An ECDSA P-256 signature in IEEE P1363 form: r || s.
using EcdsaP256Signature = std::array<std::uint8_t, 64>;

// Parses 130 hex digits (either case, no prefix or separators) of an uncompressed point. The point
// must start with 04 and lie on the curve (CNG imports it). InvalidArgument otherwise
// ("core.signature.key").
[[nodiscard]] Expected<EcdsaP256PublicKey> parse_ecdsa_p256_public_key(std::string_view hex);

// 130 lowercase hex digits.
[[nodiscard]] std::string to_hex(const EcdsaP256PublicKey& key);

// The key id: lowercase hex SHA-256 of the 65-byte point (64 digits).
[[nodiscard]] std::string key_id(const EcdsaP256PublicKey& key);

// Verifies that `signature` is an ECDSA P-256 signature of SHA-256(`message`) made with the private
// key of `key`. Errors: Corrupt ("core.signature.mismatch") when it does not verify; InvalidArgument
// ("core.signature.key") for a point CNG refuses; Internal or OutOfMemory for CNG failures (the
// NTSTATUS in native_code).
[[nodiscard]] Status verify_ecdsa_p256_sha256(const EcdsaP256PublicKey& key, std::span<const std::byte> message,
                                              const EcdsaP256Signature& signature);

// An ephemeral ECDSA P-256 key pair (see the file comment). Move-only.
class EcdsaP256KeyPair {
public:
    // Errors: Internal or OutOfMemory when CNG cannot generate the key ("core.signature.generate").
    [[nodiscard]] static Expected<EcdsaP256KeyPair> generate();

    EcdsaP256KeyPair(EcdsaP256KeyPair&& other) noexcept;
    EcdsaP256KeyPair& operator=(EcdsaP256KeyPair&& other) noexcept;
    EcdsaP256KeyPair(const EcdsaP256KeyPair&) = delete;
    EcdsaP256KeyPair& operator=(const EcdsaP256KeyPair&) = delete;
    ~EcdsaP256KeyPair();

    [[nodiscard]] const EcdsaP256PublicKey& public_key() const noexcept { return public_key_; }

    // Signs SHA-256(`message`). Errors: Internal or OutOfMemory ("core.signature.sign").
    [[nodiscard]] Expected<EcdsaP256Signature> sign(std::span<const std::byte> message) const;

private:
    EcdsaP256KeyPair() noexcept = default;

    void* key_ = nullptr;  // BCRYPT_KEY_HANDLE
    EcdsaP256PublicKey public_key_;
};

// Hex helpers for signatures (128 digits, either case on input; lowercase on output) and standard
// base64 (RFC 4648 section 4, with padding, no line breaks or whitespace). InvalidArgument for
// malformed text ("core.signature.encoding").
[[nodiscard]] Expected<EcdsaP256Signature> parse_ecdsa_p256_signature_hex(std::string_view hex);
[[nodiscard]] std::string to_hex(const EcdsaP256Signature& signature);
[[nodiscard]] std::string to_base64(std::span<const std::uint8_t> bytes);
[[nodiscard]] Expected<std::string> from_base64(std::string_view text);

}  // namespace lucent::core
