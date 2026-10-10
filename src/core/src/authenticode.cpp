#include "lucent/core/authenticode.hpp"

// clang-format off: the WinTrust headers need the types of <windows.h> and <wincrypt.h>.
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
// clang-format on

#include <array>
#include <format>
#include <new>
#include <span>
#include <string>
#include <vector>

#include "lucent/core/hash.hpp"
#include "lucent/core/text.hpp"

namespace lucent::core {

namespace {

constexpr std::string_view kWhere = "core.authenticode";
constexpr std::string_view kHexDigits = "0123456789abcdef";

std::string hex_of(std::span<const std::uint8_t> bytes) {
    std::string text(bytes.size() * 2, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        text[2 * i] = kHexDigits[bytes[i] >> 4];
        text[2 * i + 1] = kHexDigits[bytes[i] & 0x0F];
    }
    return text;
}

std::string certificate_name(PCCERT_CONTEXT certificate, DWORD flags) {
    const DWORD length = ::CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, flags, nullptr, nullptr, 0);
    if (length <= 1) {
        return {};
    }
    std::wstring name(length, L'\0');
    ::CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, flags, nullptr, name.data(), length);
    name.resize(length - 1);  // drop the terminating NUL
    return wide_to_utf8_lossy(name);
}

AuthenticodeSigner describe_signer(PCCERT_CONTEXT certificate) {
    AuthenticodeSigner signer;
    signer.subject = certificate_name(certificate, 0);
    signer.issuer = certificate_name(certificate, CERT_NAME_ISSUER_FLAG);
    std::array<std::uint8_t, 20> sha1{};
    DWORD size = static_cast<DWORD>(sha1.size());
    if (::CertGetCertificateContextProperty(certificate, CERT_SHA1_HASH_PROP_ID, sha1.data(), &size) &&
        size == sha1.size()) {
        signer.sha1 = hex_of(sha1);
    }
    const std::span<const std::byte> encoded(reinterpret_cast<const std::byte*>(certificate->pbCertEncoded),
                                             certificate->cbCertEncoded);
    if (Expected<Digest256> digest = sha256(encoded)) {
        signer.sha256 = to_hex(*digest);
    }
    return signer;
}

}  // namespace

std::string describe_trust_result(std::int32_t result) {
    const auto code = static_cast<std::uint32_t>(result);
    std::string_view meaning;
    switch (result) {
        case kTrustSucceeded:
            return "trusted";
        case kTrustNoSignature:
            meaning = "not signed";
            break;
        case kTrustBadDigest:
            meaning = "the file was changed after it was signed";
            break;
        case kTrustUntrustedRoot:
            meaning = "the certificate's root is not trusted on this computer";
            break;
        case kTrustExpired:
            meaning = "the certificate expired and the signature has no timestamp";
            break;
        case kTrustDistrusted:
            meaning = "the certificate is explicitly distrusted";
            break;
        default:
            meaning = "the signature does not verify";
            break;
    }
    return std::format("{} (WinVerifyTrust {:#010x})", meaning, code);
}

Expected<AuthenticodeCheck> check_authenticode(const std::filesystem::path& path, void* file) {
    const std::wstring& native = path.native();
    if (native.empty() || native.find(L'\0') != std::wstring::npos) {
        return fail(ErrorCode::InvalidArgument, "the file to check has no valid path", std::string(kWhere));
    }
    try {
        WINTRUST_FILE_INFO file_info{};
        file_info.cbStruct = sizeof(file_info);
        file_info.pcwszFilePath = native.c_str();
        file_info.hFile = file != nullptr ? static_cast<HANDLE>(file) : nullptr;

        GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        WINTRUST_DATA data{};
        data.cbStruct = sizeof(data);
        data.dwUIChoice = WTD_UI_NONE;
        data.fdwRevocationChecks = WTD_REVOKE_NONE;
        data.dwUnionChoice = WTD_CHOICE_FILE;
        data.pFile = &file_info;
        data.dwStateAction = WTD_STATEACTION_VERIFY;
        data.dwProvFlags = WTD_REVOCATION_CHECK_NONE | WTD_CACHE_ONLY_URL_RETRIEVAL;

        AuthenticodeCheck check;
        // INVALID_HANDLE_VALUE as the window: no user interface at all.
        check.result = static_cast<std::int32_t>(
            ::WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data));
        if (data.hWVTStateData != nullptr) {
            if (CRYPT_PROVIDER_DATA* provider = ::WTHelperProvDataFromStateData(data.hWVTStateData)) {
                if (CRYPT_PROVIDER_SGNR* signer = ::WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0)) {
                    check.timestamped = signer->csCounterSigners > 0;
                    CRYPT_PROVIDER_CERT* certificate = ::WTHelperGetProvCertFromChain(signer, 0);
                    if (certificate != nullptr && certificate->pCert != nullptr) {
                        check.signer = describe_signer(certificate->pCert);
                    }
                }
            }
        }
        data.dwStateAction = WTD_STATEACTION_CLOSE;
        ::WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
        return check;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot check the file's signature", std::string(kWhere));
    }
}

}  // namespace lucent::core
