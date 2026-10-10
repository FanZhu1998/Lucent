// ECDSA P-256 / SHA-256 signatures over CNG (signature.hpp): key parsing, key ids, verification of
// valid and tampered signatures and messages, and the encodings the update channel uses
// (ADR 0022).

#include "lucent/core/signature.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "lucent/core/hash.hpp"

namespace lucent::core {
namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

// RFC 6979, appendix A.2.5: the P-256 key and its deterministic SHA-256 signatures of "sample" and
// "test", an independent published reference, so verification is not only tested against
// signatures this build made itself (the values also verify with .NET's ECDsaCng).
constexpr std::string_view kRfcQx = "60fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6";
constexpr std::string_view kRfcQy = "7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299";
constexpr std::string_view kRfcSampleR = "efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716";
constexpr std::string_view kRfcSampleS = "f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8";
constexpr std::string_view kRfcTestR = "f1abb023518351cd71d881567b1ea663ed3efcf6c5132b354f28d3b0b7d38367";
constexpr std::string_view kRfcTestS = "019f4113742a2b14bd25926b49c649155f267e60d3814b4c0cc84250e46f0083";

TEST(Signature, VerifiesThePublishedRfc6979Vectors) {
    Expected<EcdsaP256PublicKey> key = parse_ecdsa_p256_public_key("04" + std::string(kRfcQx) + std::string(kRfcQy));
    ASSERT_TRUE(key) << key.error().describe();
    Expected<EcdsaP256Signature> sample =
        parse_ecdsa_p256_signature_hex(std::string(kRfcSampleR) + std::string(kRfcSampleS));
    ASSERT_TRUE(sample) << sample.error().describe();
    Expected<EcdsaP256Signature> test = parse_ecdsa_p256_signature_hex(std::string(kRfcTestR) + std::string(kRfcTestS));
    ASSERT_TRUE(test) << test.error().describe();
    EXPECT_TRUE(verify_ecdsa_p256_sha256(*key, bytes_of("sample"), *sample));
    EXPECT_TRUE(verify_ecdsa_p256_sha256(*key, bytes_of("test"), *test));

    // Each signature belongs to its own message only.
    const Status swapped = verify_ecdsa_p256_sha256(*key, bytes_of("test"), *sample);
    ASSERT_FALSE(swapped);
    EXPECT_EQ(swapped.error().code, ErrorCode::Corrupt);
    EXPECT_EQ(swapped.error().where, "core.signature.mismatch");
    EXPECT_FALSE(verify_ecdsa_p256_sha256(*key, bytes_of("samplf"), *sample));
}

TEST(Signature, SignsAndVerifiesWithAnEphemeralKey) {
    Expected<EcdsaP256KeyPair> pair = EcdsaP256KeyPair::generate();
    ASSERT_TRUE(pair) << pair.error().describe();
    const std::string message = R"({"schema":"lucent.release-manifest","version":"1.2.3"})";
    Expected<EcdsaP256Signature> signature = pair->sign(bytes_of(message));
    ASSERT_TRUE(signature) << signature.error().describe();
    EXPECT_TRUE(verify_ecdsa_p256_sha256(pair->public_key(), bytes_of(message), *signature));

    // A changed message, a changed signature and another key are refused.
    std::string changed = message;
    changed.back() = ' ';
    EXPECT_FALSE(verify_ecdsa_p256_sha256(pair->public_key(), bytes_of(changed), *signature));
    EcdsaP256Signature flipped = *signature;
    flipped[10] ^= 0x80;
    EXPECT_FALSE(verify_ecdsa_p256_sha256(pair->public_key(), bytes_of(message), flipped));
    Expected<EcdsaP256KeyPair> other = EcdsaP256KeyPair::generate();
    ASSERT_TRUE(other);
    const Status wrong_key = verify_ecdsa_p256_sha256(other->public_key(), bytes_of(message), *signature);
    ASSERT_FALSE(wrong_key);
    EXPECT_EQ(wrong_key.error().code, ErrorCode::Corrupt);

    // A key survives its hex form.
    Expected<EcdsaP256PublicKey> reparsed = parse_ecdsa_p256_public_key(to_hex(pair->public_key()));
    ASSERT_TRUE(reparsed);
    EXPECT_EQ(*reparsed, pair->public_key());
    EXPECT_EQ(to_hex(pair->public_key()).size(), 130u);
    EXPECT_EQ(to_hex(pair->public_key()).substr(0, 2), "04");
}

TEST(Signature, KeyIdsAreTheSha256OfThePoint) {
    Expected<EcdsaP256PublicKey> key = parse_ecdsa_p256_public_key("04" + std::string(kRfcQx) + std::string(kRfcQy));
    ASSERT_TRUE(key);
    const std::string id = key_id(*key);
    EXPECT_EQ(id.size(), 64u);
    Expected<Digest256> digest = sha256(std::as_bytes(std::span(key->point)));
    ASSERT_TRUE(digest);
    EXPECT_EQ(id, to_hex(*digest));
}

TEST(Signature, RefusesMalformedKeys) {
    // Wrong length, not hex, a compressed point, and a point that is not on the curve.
    for (const std::string& text :
         {std::string("04"), std::string(130, 'z'), "02" + std::string(kRfcQx) + std::string(kRfcQy),
          "04" + std::string(kRfcQx) + std::string(kRfcQx)}) {
        Expected<EcdsaP256PublicKey> key = parse_ecdsa_p256_public_key(text);
        ASSERT_FALSE(key) << text;
        EXPECT_EQ(key.error().code, ErrorCode::InvalidArgument) << text;
        EXPECT_EQ(key.error().where, "core.signature.key") << text;
    }
}

TEST(Signature, Base64RoundTripsAndIsStrict) {
    EXPECT_EQ(to_base64(std::span<const std::uint8_t>()), "");
    const std::uint8_t f[] = {'f'};
    const std::uint8_t fo[] = {'f', 'o'};
    const std::uint8_t foo[] = {'f', 'o', 'o'};
    const std::uint8_t foob[] = {'f', 'o', 'o', 'b'};
    EXPECT_EQ(to_base64(f), "Zg==");  // RFC 4648 section 10
    EXPECT_EQ(to_base64(fo), "Zm8=");
    EXPECT_EQ(to_base64(foo), "Zm9v");
    EXPECT_EQ(to_base64(foob), "Zm9vYg==");
    Expected<std::string> decoded = from_base64("Zm9vYmFy");
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, "foobar");
    decoded = from_base64("Zm9vYg==");
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, "foob");
    for (const std::string_view bad : {"Zm9", "Zm9vYg=", "Zm=vYg==", "Zm9v\nYg==", "Zh==", "Zm9vYg==Zm9v", "Zm9-"}) {
        EXPECT_FALSE(from_base64(bad)) << bad;
    }
}

}  // namespace
}  // namespace lucent::core
