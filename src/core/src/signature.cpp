#include "lucent/core/signature.hpp"

// clang-format off: <bcrypt.h> requires the types declared by <windows.h>.
#include <windows.h>
#include <bcrypt.h>
// clang-format on

#include <algorithm>
#include <cstring>
#include <format>
#include <utility>
#include <vector>

#include "lucent/core/hash.hpp"

namespace lucent::core {

namespace {

constexpr std::string_view kWhereKey = "core.signature.key";
constexpr std::string_view kWhereVerify = "core.signature.verify";
constexpr std::string_view kWhereMismatch = "core.signature.mismatch";
constexpr std::string_view kWhereGenerate = "core.signature.generate";
constexpr std::string_view kWhereSign = "core.signature.sign";
constexpr std::string_view kWhereEncoding = "core.signature.encoding";

constexpr NTSTATUS kStatusNoMemory = static_cast<NTSTATUS>(0xC0000017L);
constexpr NTSTATUS kStatusInvalidSignature = static_cast<NTSTATUS>(0xC000A000L);

constexpr std::size_t kCoordinateBytes = 32;
constexpr std::string_view kHexDigits = "0123456789abcdef";
constexpr std::string_view kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr bool succeeded(NTSTATUS status) noexcept {
    return status >= 0;
}

Unexpected cng_failure(NTSTATUS status, std::string_view what, std::string_view where) {
    const ErrorCode code = status == kStatusNoMemory ? ErrorCode::OutOfMemory : ErrorCode::Internal;
    return fail(code, std::format("{} failed (NTSTATUS {:#010x})", what, static_cast<std::uint32_t>(status)),
                std::string(where), static_cast<std::int64_t>(static_cast<std::uint32_t>(status)));
}

// The ECDSA P-256 algorithm provider, opened once per process (thread-safe static
// initialisation) and deliberately never closed (see the file comment of signature.hpp).
struct EcdsaProvider {
    BCRYPT_ALG_HANDLE handle = nullptr;
    NTSTATUS status = 0;
};

const EcdsaProvider& ecdsa_provider() noexcept {
    static const EcdsaProvider provider = [] {
        EcdsaProvider p;
        p.status = ::BCryptOpenAlgorithmProvider(&p.handle, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0);
        if (!succeeded(p.status)) {
            p.handle = nullptr;
        }
        return p;
    }();
    return provider;
}

// Owner of a BCRYPT_KEY_HANDLE.
class KeyHandle {
public:
    KeyHandle() noexcept = default;
    explicit KeyHandle(BCRYPT_KEY_HANDLE handle) noexcept : handle_(handle) {}
    ~KeyHandle() {
        if (handle_ != nullptr) {
            ::BCryptDestroyKey(handle_);
        }
    }
    KeyHandle(KeyHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    KeyHandle& operator=(KeyHandle&& other) noexcept {
        if (this != &other) {
            if (handle_ != nullptr) {
                ::BCryptDestroyKey(handle_);
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    KeyHandle(const KeyHandle&) = delete;
    KeyHandle& operator=(const KeyHandle&) = delete;

    [[nodiscard]] BCRYPT_KEY_HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] BCRYPT_KEY_HANDLE* out() noexcept { return &handle_; }
    [[nodiscard]] BCRYPT_KEY_HANDLE release() noexcept { return std::exchange(handle_, nullptr); }

private:
    BCRYPT_KEY_HANDLE handle_ = nullptr;
};

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Exactly `out.size()` bytes from 2 * out.size() hex digits.
bool parse_hex_bytes(std::string_view hex, std::span<std::uint8_t> out) noexcept {
    if (hex.size() != out.size() * 2) {
        return false;
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int high = hex_value(hex[2 * i]);
        const int low = hex_value(hex[2 * i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out[i] = static_cast<std::uint8_t>(high * 16 + low);
    }
    return true;
}

std::string hex_of(std::span<const std::uint8_t> bytes) {
    std::string text(bytes.size() * 2, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        text[2 * i] = kHexDigits[bytes[i] >> 4];
        text[2 * i + 1] = kHexDigits[bytes[i] & 0x0F];
    }
    return text;
}

// The public key as CNG's BCRYPT_ECCPUBLIC_BLOB: the header, then X and Y.
std::vector<std::uint8_t> public_blob(const EcdsaP256PublicKey& key) {
    std::vector<std::uint8_t> blob(sizeof(BCRYPT_ECCKEY_BLOB) + 2 * kCoordinateBytes);
    BCRYPT_ECCKEY_BLOB header{};
    header.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header.cbKey = static_cast<ULONG>(kCoordinateBytes);
    std::memcpy(blob.data(), &header, sizeof(header));
    std::memcpy(blob.data() + sizeof(header), key.point.data() + 1, 2 * kCoordinateBytes);
    return blob;
}

// Imports `key` as a CNG public key; InvalidArgument when CNG refuses the point.
Expected<KeyHandle> import_public_key(const EcdsaP256PublicKey& key) {
    const EcdsaProvider& provider = ecdsa_provider();
    if (provider.handle == nullptr) {
        return cng_failure(provider.status, "opening the ECDSA P-256 provider", kWhereVerify);
    }
    if (key.point[0] != 0x04) {
        return fail(ErrorCode::InvalidArgument, "the public key is not an uncompressed point (it must start with 04)",
                    std::string(kWhereKey));
    }
    std::vector<std::uint8_t> blob = public_blob(key);
    KeyHandle handle;
    const NTSTATUS status = ::BCryptImportKeyPair(provider.handle, nullptr, BCRYPT_ECCPUBLIC_BLOB, handle.out(),
                                                  blob.data(), static_cast<ULONG>(blob.size()), 0);
    if (!succeeded(status)) {
        if (status == kStatusNoMemory) {
            return cng_failure(status, "importing the public key", kWhereKey);
        }
        return fail(ErrorCode::InvalidArgument,
                    std::format("the public key is not a valid P-256 point (NTSTATUS {:#010x})",
                                static_cast<std::uint32_t>(status)),
                    std::string(kWhereKey), static_cast<std::int64_t>(static_cast<std::uint32_t>(status)));
    }
    return handle;
}

}  // namespace

Expected<EcdsaP256PublicKey> parse_ecdsa_p256_public_key(std::string_view hex) {
    EcdsaP256PublicKey key;
    if (!parse_hex_bytes(hex, key.point)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("a P-256 public key is 130 hex digits (an uncompressed point); got {} characters",
                                hex.size()),
                    std::string(kWhereKey));
    }
    Expected<KeyHandle> imported = import_public_key(key);
    if (!imported) {
        return Unexpected(std::move(imported.error()));
    }
    return key;
}

std::string to_hex(const EcdsaP256PublicKey& key) {
    return hex_of(key.point);
}

std::string key_id(const EcdsaP256PublicKey& key) {
    Expected<Digest256> digest = sha256(std::as_bytes(std::span(key.point)));
    // SHA-256 over CNG fails only when the provider cannot be opened (out of memory at start-up);
    // an empty id then matches no key.
    return digest ? to_hex(*digest) : std::string();
}

Status verify_ecdsa_p256_sha256(const EcdsaP256PublicKey& key, std::span<const std::byte> message,
                                const EcdsaP256Signature& signature) {
    Expected<KeyHandle> handle = import_public_key(key);
    if (!handle) {
        return Unexpected(std::move(handle.error()));
    }
    Expected<Digest256> digest = sha256(message);
    if (!digest) {
        return Unexpected(std::move(digest.error()));
    }
    // BCryptVerifySignature takes non-const buffers; it does not modify them.
    Digest256 hash = *digest;
    EcdsaP256Signature copy = signature;
    const NTSTATUS status = ::BCryptVerifySignature(handle->get(), nullptr, hash.data(), static_cast<ULONG>(hash.size()),
                                                    copy.data(), static_cast<ULONG>(copy.size()), 0);
    if (succeeded(status)) {
        return ok();
    }
    if (status == kStatusInvalidSignature) {
        return fail(ErrorCode::Corrupt, "the signature does not verify with this public key",
                    std::string(kWhereMismatch), static_cast<std::int64_t>(static_cast<std::uint32_t>(status)));
    }
    return cng_failure(status, "verifying the signature", kWhereVerify);
}

Expected<EcdsaP256KeyPair> EcdsaP256KeyPair::generate() {
    const EcdsaProvider& provider = ecdsa_provider();
    if (provider.handle == nullptr) {
        return cng_failure(provider.status, "opening the ECDSA P-256 provider", kWhereGenerate);
    }
    KeyHandle handle;
    NTSTATUS status = ::BCryptGenerateKeyPair(provider.handle, handle.out(), 256, 0);
    if (succeeded(status)) {
        status = ::BCryptFinalizeKeyPair(handle.get(), 0);
    }
    if (!succeeded(status)) {
        return cng_failure(status, "generating a P-256 key pair", kWhereGenerate);
    }
    std::array<std::uint8_t, sizeof(BCRYPT_ECCKEY_BLOB) + 2 * kCoordinateBytes> blob{};
    ULONG written = 0;
    status = ::BCryptExportKey(handle.get(), nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(),
                               static_cast<ULONG>(blob.size()), &written, 0);
    if (!succeeded(status)) {
        return cng_failure(status, "exporting the public key", kWhereGenerate);
    }
    if (written != blob.size()) {
        return fail(ErrorCode::Internal, std::format("the exported public key has {} bytes", written),
                    std::string(kWhereGenerate));
    }
    EcdsaP256KeyPair pair;
    pair.public_key_.point[0] = 0x04;
    std::memcpy(pair.public_key_.point.data() + 1, blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), 2 * kCoordinateBytes);
    pair.key_ = handle.release();
    return pair;
}

EcdsaP256KeyPair::EcdsaP256KeyPair(EcdsaP256KeyPair&& other) noexcept
    : key_(std::exchange(other.key_, nullptr)), public_key_(other.public_key_) {}

EcdsaP256KeyPair& EcdsaP256KeyPair::operator=(EcdsaP256KeyPair&& other) noexcept {
    if (this != &other) {
        if (key_ != nullptr) {
            ::BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
        }
        key_ = std::exchange(other.key_, nullptr);
        public_key_ = other.public_key_;
    }
    return *this;
}

EcdsaP256KeyPair::~EcdsaP256KeyPair() {
    if (key_ != nullptr) {
        ::BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
    }
}

Expected<EcdsaP256Signature> EcdsaP256KeyPair::sign(std::span<const std::byte> message) const {
    if (key_ == nullptr) {
        return fail(ErrorCode::Internal, "the key pair was moved from", std::string(kWhereSign));
    }
    Expected<Digest256> digest = sha256(message);
    if (!digest) {
        return Unexpected(std::move(digest.error()));
    }
    Digest256 hash = *digest;
    EcdsaP256Signature signature{};
    ULONG written = 0;
    const NTSTATUS status =
        ::BCryptSignHash(static_cast<BCRYPT_KEY_HANDLE>(key_), nullptr, hash.data(), static_cast<ULONG>(hash.size()),
                         signature.data(), static_cast<ULONG>(signature.size()), &written, 0);
    if (!succeeded(status)) {
        return cng_failure(status, "signing", kWhereSign);
    }
    if (written != signature.size()) {
        return fail(ErrorCode::Internal, std::format("the signature has {} bytes", written), std::string(kWhereSign));
    }
    return signature;
}

Expected<EcdsaP256Signature> parse_ecdsa_p256_signature_hex(std::string_view hex) {
    EcdsaP256Signature signature{};
    if (!parse_hex_bytes(hex, signature)) {
        return fail(ErrorCode::InvalidArgument, "a P-256 signature is 128 hex digits (r || s)",
                    std::string(kWhereEncoding));
    }
    return signature;
}

std::string to_hex(const EcdsaP256Signature& signature) {
    return hex_of(signature);
}

std::string to_base64(std::span<const std::uint8_t> bytes) {
    std::string text;
    text.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t triple = (std::uint32_t{bytes[i]} << 16) | (std::uint32_t{bytes[i + 1]} << 8) | bytes[i + 2];
        text += kBase64Alphabet[(triple >> 18) & 0x3F];
        text += kBase64Alphabet[(triple >> 12) & 0x3F];
        text += kBase64Alphabet[(triple >> 6) & 0x3F];
        text += kBase64Alphabet[triple & 0x3F];
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1) {
        const std::uint32_t triple = std::uint32_t{bytes[i]} << 16;
        text += kBase64Alphabet[(triple >> 18) & 0x3F];
        text += kBase64Alphabet[(triple >> 12) & 0x3F];
        text += "==";
    } else if (rest == 2) {
        const std::uint32_t triple = (std::uint32_t{bytes[i]} << 16) | (std::uint32_t{bytes[i + 1]} << 8);
        text += kBase64Alphabet[(triple >> 18) & 0x3F];
        text += kBase64Alphabet[(triple >> 12) & 0x3F];
        text += kBase64Alphabet[(triple >> 6) & 0x3F];
        text += '=';
    }
    return text;
}

Expected<std::string> from_base64(std::string_view text) {
    const auto invalid = [] {
        return fail(ErrorCode::InvalidArgument, "the text is not padded standard base64", std::string(kWhereEncoding));
    };
    if (text.size() % 4 != 0) {
        return invalid();
    }
    std::string bytes;
    bytes.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        int values[4] = {0, 0, 0, 0};
        int padding = 0;
        for (int j = 0; j < 4; ++j) {
            const char c = text[i + static_cast<std::size_t>(j)];
            if (c == '=') {
                // Padding only in the last group, only in its last two places, and nothing after it.
                if (i + 4 != text.size() || j < 2) {
                    return invalid();
                }
                ++padding;
                continue;
            }
            if (padding > 0) {
                return invalid();
            }
            const std::size_t at = kBase64Alphabet.find(c);
            if (at == std::string_view::npos) {
                return invalid();
            }
            values[j] = static_cast<int>(at);
        }
        const std::uint32_t triple = (static_cast<std::uint32_t>(values[0]) << 18) |
                                     (static_cast<std::uint32_t>(values[1]) << 12) |
                                     (static_cast<std::uint32_t>(values[2]) << 6) | static_cast<std::uint32_t>(values[3]);
        // Canonical encoding: the bits that padding drops must be zero.
        if ((padding == 1 && (triple & 0xFF) != 0) || (padding == 2 && (triple & 0xFFFF) != 0)) {
            return invalid();
        }
        bytes += static_cast<char>((triple >> 16) & 0xFF);
        if (padding < 2) {
            bytes += static_cast<char>((triple >> 8) & 0xFF);
        }
        if (padding < 1) {
            bytes += static_cast<char>(triple & 0xFF);
        }
    }
    return bytes;
}

}  // namespace lucent::core
