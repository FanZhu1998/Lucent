// Authenticode checks (authenticode.hpp): a Microsoft-signed runtime DLL verifies and names its
// signer (the thumbprint is checked against an independent route, CryptQueryObject), an unsigned
// program is "not signed", a signed file changed after signing has a bad digest, and a check through
// an open handle reads that file object (ADR 0022).

#include "lucent/core/authenticode.hpp"

#include <gtest/gtest.h>

// clang-format off
#include <windows.h>
#include <wincrypt.h>
// clang-format on

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "test_support.hpp"

namespace lucent::core {
namespace {

using test_support::TempDir;

// The Visual C++ runtime that every Lucent build needs carries an embedded Authenticode signature
// by Microsoft (unlike most of System32, which is catalog-signed).
std::filesystem::path signed_runtime_dll() {
    wchar_t system[MAX_PATH] = {};
    const UINT length = ::GetSystemDirectoryW(system, MAX_PATH);
    return std::filesystem::path(std::wstring(system, length)) / L"vcruntime140.dll";
}

std::filesystem::path this_executable() {
    std::wstring path(32768, L'\0');
    const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return path;
}

// The SHA-1 thumbprint of the certificate that signed `path`, found without WinTrust: the embedded
// PKCS #7 message's first signer (CryptQueryObject, CMSG_SIGNER_INFO_PARAM) looked up in the
// message's own certificates.
std::string independent_sha1(const std::filesystem::path& path) {
    HCERTSTORE store = nullptr;
    HCRYPTMSG message = nullptr;
    if (!::CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(), CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                            CERT_QUERY_FORMAT_FLAG_BINARY, 0, nullptr, nullptr, nullptr, &store, &message, nullptr)) {
        return "query failed";
    }
    std::string result = "no signer";
    DWORD size = 0;
    if (::CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &size) && size > 0) {
        std::vector<std::uint8_t> buffer(size);
        if (::CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, buffer.data(), &size)) {
            const auto* signer = reinterpret_cast<const CMSG_SIGNER_INFO*>(buffer.data());
            CERT_INFO wanted{};
            wanted.Issuer = signer->Issuer;
            wanted.SerialNumber = signer->SerialNumber;
            PCCERT_CONTEXT certificate = ::CertFindCertificateInStore(
                store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, &wanted, nullptr);
            if (certificate != nullptr) {
                std::array<BYTE, 20> hash{};
                DWORD hash_size = static_cast<DWORD>(hash.size());
                if (::CryptHashCertificate(0, CALG_SHA1, 0, certificate->pbCertEncoded, certificate->cbCertEncoded,
                                           hash.data(), &hash_size)) {
                    result.clear();
                    for (const BYTE b : hash) {
                        result += "0123456789abcdef"[b >> 4];
                        result += "0123456789abcdef"[b & 0x0F];
                    }
                }
                ::CertFreeCertificateContext(certificate);
            }
        }
    }
    ::CryptMsgClose(message);
    ::CertCloseStore(store, 0);
    return result;
}

std::vector<char> read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_all(const std::filesystem::path& path, const std::vector<char>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

TEST(Authenticode, AMicrosoftSignedRuntimeVerifiesAndNamesItsSigner) {
    const std::filesystem::path dll = signed_runtime_dll();
    if (!std::filesystem::exists(dll)) {
        GTEST_SKIP() << "no Visual C++ runtime in " << dll.string();
    }
    Expected<AuthenticodeCheck> check = check_authenticode(dll);
    ASSERT_TRUE(check) << check.error().describe();
    EXPECT_TRUE(check->trusted()) << describe_trust_result(check->result);
    EXPECT_EQ(describe_trust_result(check->result), "trusted");
    ASSERT_TRUE(check->signer.has_value());
    EXPECT_NE(check->signer->subject.find("Microsoft"), std::string::npos) << check->signer->subject;
    EXPECT_FALSE(check->signer->issuer.empty());
    EXPECT_EQ(check->signer->sha1.size(), 40u);
    EXPECT_EQ(check->signer->sha256.size(), 64u);
    EXPECT_EQ(check->signer->sha1, independent_sha1(dll));
    EXPECT_TRUE(check->timestamped);
}

TEST(Authenticode, AnUnsignedProgramIsNotSigned) {
    Expected<AuthenticodeCheck> check = check_authenticode(this_executable());
    ASSERT_TRUE(check) << check.error().describe();
    EXPECT_FALSE(check->trusted());
    EXPECT_EQ(check->result, kTrustNoSignature) << describe_trust_result(check->result);
    EXPECT_FALSE(check->signer.has_value());
    EXPECT_NE(describe_trust_result(check->result).find("not signed"), std::string::npos);
}

TEST(Authenticode, ASignedFileChangedAfterSigningHasABadDigestAlsoThroughAHandle) {
    const std::filesystem::path dll = signed_runtime_dll();
    if (!std::filesystem::exists(dll)) {
        GTEST_SKIP() << "no Visual C++ runtime in " << dll.string();
    }
    TempDir dir;
    const std::filesystem::path copy = dir / L"signed copy.dll";
    std::vector<char> bytes = read_all(dll);
    ASSERT_GT(bytes.size(), 4096u);
    write_all(copy, bytes);
    Expected<AuthenticodeCheck> intact = check_authenticode(copy);
    ASSERT_TRUE(intact) << intact.error().describe();
    EXPECT_TRUE(intact->trusted()) << describe_trust_result(intact->result);

    // One byte of the signed image changed (a third into the file: code or data the hash covers).
    const std::filesystem::path changed = dir / L"changed.dll";
    bytes[bytes.size() / 3] = static_cast<char>(bytes[bytes.size() / 3] ^ 0x5A);
    write_all(changed, bytes);
    Expected<AuthenticodeCheck> check = check_authenticode(changed);
    ASSERT_TRUE(check) << check.error().describe();
    EXPECT_EQ(check->result, kTrustBadDigest) << describe_trust_result(check->result);
    EXPECT_FALSE(check->trusted());

    // Through an open handle that denies writers: the same verdicts for the same file objects.
    for (const auto& [path, trusted] : {std::pair{copy, true}, std::pair{changed, false}}) {
        HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(handle, INVALID_HANDLE_VALUE);
        Expected<AuthenticodeCheck> held = check_authenticode(path, handle);
        ::CloseHandle(handle);
        ASSERT_TRUE(held) << held.error().describe();
        EXPECT_EQ(held->trusted(), trusted) << path.string() << ": " << describe_trust_result(held->result);
    }
}

TEST(Authenticode, RefusesAnEmptyPath) {
    Expected<AuthenticodeCheck> check = check_authenticode(std::filesystem::path());
    ASSERT_FALSE(check);
    EXPECT_EQ(check.error().code, ErrorCode::InvalidArgument);
}

}  // namespace
}  // namespace lucent::core
