#include "lucent/application/update_manifest.hpp"

#include <windows.h>

#include <charconv>
#include <exception>
#include <format>
#include <new>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace lucent::app {

namespace {

using Json = nlohmann::json;

constexpr std::string_view kWhereManifest = "app.update.manifest";
constexpr std::string_view kWhereSignature = "app.update.signature";
constexpr std::string_view kWhereUnknownKey = "app.update.signature.unknown_key";
constexpr std::string_view kWhereMismatch = "app.update.signature.mismatch";

constexpr std::size_t kMaxChannelLength = 32;

// A reader's failure; turned into lucent::Error by the public functions.
struct DocumentError {
    ErrorCode code = ErrorCode::Corrupt;
    std::string message;
};

[[noreturn]] void corrupt(std::string message) {
    throw DocumentError{ErrorCode::Corrupt, std::move(message)};
}

// Parses one JSON value, refusing repeated member names (nlohmann keeps the last one silently).
Json parse_strict(std::string_view text, std::string_view what) {
    std::vector<std::set<std::string>> names;
    std::string repeated;
    const Json::parser_callback_t track = [&](int, Json::parse_event_t event, Json& parsed) {
        switch (event) {
            case Json::parse_event_t::object_start:
                names.emplace_back();
                break;
            case Json::parse_event_t::key:
                if (!names.empty() && parsed.is_string() && !names.back().insert(parsed.get<std::string>()).second &&
                    repeated.empty()) {
                    repeated = parsed.get<std::string>();
                }
                break;
            case Json::parse_event_t::object_end:
                if (!names.empty()) {
                    names.pop_back();
                }
                break;
            default:
                break;
        }
        return true;
    };
    Json document = Json::parse(text.begin(), text.end(), track, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        corrupt(std::format("the {} is not valid JSON", what));
    }
    if (!repeated.empty()) {
        corrupt(std::format("the {} repeats the member '{}'", what, repeated));
    }
    if (!document.is_object()) {
        corrupt(std::format("the {} is not a JSON object", what));
    }
    return document;
}

void only_members(const Json& object, std::initializer_list<std::string_view> known, std::string_view what) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool listed = false;
        for (const std::string_view key : known) {
            listed = listed || it.key() == key;
        }
        if (!listed) {
            corrupt(std::format("the {} has an unknown member '{}'", what, it.key()));
        }
    }
}

const Json& member(const Json& object, std::string_view key, std::string_view what) {
    const auto found = object.find(key);
    if (found == object.end()) {
        corrupt(std::format("the {} has no member '{}'", what, key));
    }
    return *found;
}

std::string text_member(const Json& object, std::string_view key, std::string_view what) {
    const Json& value = member(object, key, what);
    if (!value.is_string()) {
        corrupt(std::format("{}.{} is not a string", what, key));
    }
    return value.get<std::string>();
}

std::uint64_t unsigned_member(const Json& object, std::string_view key, std::string_view what) {
    const Json& value = member(object, key, what);
    if (!value.is_number_unsigned()) {
        corrupt(std::format("{}.{} is not an unsigned integer", what, key));
    }
    return value.get<std::uint64_t>();
}

// The schema name and version of a document; VersionMismatch for a newer version.
void check_schema(const Json& document, std::string_view schema, std::uint32_t version, std::string_view what) {
    if (text_member(document, "schema", what) != schema) {
        corrupt(std::format("the document is not a '{}' {}", schema, what));
    }
    const std::uint64_t found = unsigned_member(document, "schema_version", what);
    if (found == 0) {
        corrupt(std::format("the {} has schema version 0", what));
    }
    if (found > version) {
        throw DocumentError{ErrorCode::VersionMismatch,
                            std::format("the {} has schema version {}; this Lucent reads version {}", what, found,
                                        version)};
    }
}

bool is_lower_hex(std::string_view text, std::size_t length) noexcept {
    if (text.size() != length) {
        return false;
    }
    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool all_digits(std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

int number_of(std::string_view digits) noexcept {
    int value = 0;
    for (const char c : digits) {
        value = value * 10 + (c - '0');
    }
    return value;
}

// "YYYY-MM-DDTHH:MM:SSZ" with plausible field values.
bool is_utc_timestamp(std::string_view text) noexcept {
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
        text[16] != ':' || text[19] != 'Z') {
        return false;
    }
    for (const std::size_t at : {0u, 5u, 8u, 11u, 14u, 17u}) {
        const std::size_t length = at == 0 ? 4 : 2;
        if (!all_digits(text.substr(at, length))) {
            return false;
        }
    }
    const int month = number_of(text.substr(5, 2));
    const int day = number_of(text.substr(8, 2));
    const int hour = number_of(text.substr(11, 2));
    const int minute = number_of(text.substr(14, 2));
    const int second = number_of(text.substr(17, 2));
    return month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour <= 23 && minute <= 59 && second <= 60;
}

// An absolute http or https URL with a host, no whitespace, control characters or quotes (the
// transport's policy decides which schemes it fetches: http_client.hpp).
bool is_plausible_url(std::string_view url) noexcept {
    if (url.size() > kMaxReleaseUrlLength) {
        return false;
    }
    std::string_view rest;
    if (url.starts_with("https://")) {
        rest = url.substr(8);
    } else if (url.starts_with("http://")) {
        rest = url.substr(7);
    } else {
        return false;
    }
    if (rest.empty() || rest.front() == '/' || rest.front() == '?' || rest.front() == '#') {
        return false;
    }
    for (const char c : url) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u >= 0x7F || c == '"' || c == '\\' || c == '<' || c == '>') {
            return false;
        }
    }
    return true;
}

std::string json_string(std::string_view text) {
    return Json(std::string(text)).dump();
}

template <class T, class F>
Expected<T> guarded(std::string_view where, F&& read) {
    try {
        return read();
    } catch (const DocumentError& error) {
        return fail(error.code, error.message, std::string(where));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while reading an update document", std::string(where));
    } catch (const std::exception& error) {
        return fail(ErrorCode::Corrupt, std::format("malformed update document: {}", error.what()), std::string(where));
    }
}

ReleaseManifest read_manifest(std::string_view json) {
    constexpr std::string_view what = "release manifest";
    if (json.size() > kMaxReleaseManifestBytes) {
        corrupt(std::format("the release manifest has {} bytes, more than {}", json.size(), kMaxReleaseManifestBytes));
    }
    const Json document = parse_strict(json, what);
    check_schema(document, kReleaseManifestSchema, kReleaseManifestVersion, what);
    only_members(document,
                 {"schema", "schema_version", "product", "channel", "version", "published", "architecture",
                  "minimum_os", "installer", "release_notes"},
                 what);
    ReleaseManifest manifest;
    if (text_member(document, "product", what) != kReleaseProduct) {
        corrupt("the release manifest is not for Lucent");
    }
    manifest.channel = text_member(document, "channel", what);
    if (!is_valid_channel_name(manifest.channel)) {
        corrupt(std::format("the release manifest names an invalid channel '{}'", manifest.channel));
    }
    Expected<core::SemanticVersion> version = core::SemanticVersion::parse(text_member(document, "version", what));
    if (!version) {
        corrupt("the release manifest's version is not a semantic version: " + version.error().message);
    }
    manifest.version = std::move(*version);
    manifest.published = text_member(document, "published", what);
    if (!is_utc_timestamp(manifest.published)) {
        corrupt(std::format("the release manifest's publication time '{}' is not YYYY-MM-DDTHH:MM:SSZ",
                            manifest.published));
    }
    manifest.architecture = text_member(document, "architecture", what);
    if (manifest.architecture != kReleaseArchitecture) {
        corrupt(std::format("the release manifest is for architecture '{}', not x64", manifest.architecture));
    }
    Expected<WindowsVersion> minimum_os = WindowsVersion::parse(text_member(document, "minimum_os", what));
    if (!minimum_os) {
        corrupt("the release manifest's minimum_os is not major.minor.build");
    }
    manifest.minimum_os = *minimum_os;

    constexpr std::string_view installer_what = "release manifest's installer";
    const Json& installer = member(document, "installer", what);
    if (!installer.is_object()) {
        corrupt("the release manifest's installer is not an object");
    }
    only_members(installer, {"url", "size", "sha256"}, installer_what);
    manifest.installer.url = text_member(installer, "url", installer_what);
    if (!is_plausible_url(manifest.installer.url)) {
        corrupt("the release manifest's installer URL is not an absolute http(s) URL");
    }
    manifest.installer.size = unsigned_member(installer, "size", installer_what);
    if (manifest.installer.size == 0 || manifest.installer.size > kMaxInstallerBytes) {
        corrupt(std::format("the installer size {} is outside 1 ... {}", manifest.installer.size, kMaxInstallerBytes));
    }
    const std::string sha256 = text_member(installer, "sha256", installer_what);
    if (!is_lower_hex(sha256, 64)) {
        corrupt("the installer's sha256 is not 64 lowercase hex digits");
    }
    Expected<core::Digest256> digest = core::digest_from_hex(sha256);
    if (!digest) {
        corrupt("the installer's sha256 is malformed");
    }
    manifest.installer.sha256 = *digest;

    manifest.release_notes = text_member(document, "release_notes", what);
    if (manifest.release_notes.size() > kMaxReleaseNotesBytes) {
        corrupt(std::format("the release notes have {} bytes, more than {}", manifest.release_notes.size(),
                            kMaxReleaseNotesBytes));
    }
    return manifest;
}

ReleaseSignature read_signature(std::string_view json) {
    constexpr std::string_view what = "release signature";
    if (json.size() > kMaxReleaseSignatureBytes) {
        corrupt(std::format("the release signature has {} bytes, more than {}", json.size(), kMaxReleaseSignatureBytes));
    }
    const Json document = parse_strict(json, what);
    check_schema(document, kReleaseSignatureSchema, kReleaseSignatureVersion, what);
    only_members(document, {"schema", "schema_version", "algorithm", "key_id", "signature"}, what);
    const std::string algorithm = text_member(document, "algorithm", what);
    if (algorithm != kReleaseSignatureAlgorithm) {
        corrupt(std::format("the release signature uses the algorithm '{}', not {}", algorithm,
                            kReleaseSignatureAlgorithm));
    }
    ReleaseSignature signature;
    signature.key_id = text_member(document, "key_id", what);
    if (!is_lower_hex(signature.key_id, 64)) {
        corrupt("the release signature's key_id is not 64 lowercase hex digits");
    }
    Expected<std::string> bytes = core::from_base64(text_member(document, "signature", what));
    if (!bytes || bytes->size() != signature.signature.size()) {
        corrupt("the release signature's signature is not the base64 of 64 bytes");
    }
    for (std::size_t i = 0; i < signature.signature.size(); ++i) {
        signature.signature[i] = static_cast<std::uint8_t>((*bytes)[i]);
    }
    return signature;
}

}  // namespace

Expected<WindowsVersion> WindowsVersion::parse(std::string_view text) {
    WindowsVersion version;
    std::uint32_t* parts[] = {&version.major, &version.minor, &version.build};
    std::size_t start = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        const std::size_t end = i < 2 ? text.find('.', start) : text.size();
        if (end == std::string_view::npos) {
            return fail(ErrorCode::InvalidArgument, "a Windows version is major.minor.build", std::string(kWhereManifest));
        }
        const std::string_view digits = text.substr(start, end - start);
        if (!all_digits(digits) || (digits.size() > 1 && digits.front() == '0') || digits.size() > 9) {
            return fail(ErrorCode::InvalidArgument, "a Windows version is major.minor.build", std::string(kWhereManifest));
        }
        std::from_chars(digits.data(), digits.data() + digits.size(), *parts[i]);
        start = end + 1;
    }
    return version;
}

std::string WindowsVersion::to_string() const {
    return std::format("{}.{}.{}", major, minor, build);
}

WindowsVersion running_windows_version() {
    using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
    WindowsVersion version;
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return version;
    }
    const auto rtl_get_version =
        reinterpret_cast<RtlGetVersionFunction>(reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")));
    if (rtl_get_version == nullptr) {
        return version;
    }
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtl_get_version(&info) == 0) {
        version.major = info.dwMajorVersion;
        version.minor = info.dwMinorVersion;
        version.build = info.dwBuildNumber;
    }
    return version;
}

bool is_valid_channel_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxChannelLength || name.front() == '-' || name.back() == '-') {
        return false;
    }
    char previous = '\0';
    for (const char c : name) {
        const bool word = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!word && !(c == '-' && previous != '-')) {
            return false;
        }
        previous = c;
    }
    return true;
}

Expected<ReleaseManifest> parse_release_manifest(std::string_view json) {
    return guarded<ReleaseManifest>(kWhereManifest, [&] { return read_manifest(json); });
}

Expected<ReleaseSignature> parse_release_signature(std::string_view json) {
    return guarded<ReleaseSignature>(kWhereSignature, [&] { return read_signature(json); });
}

Expected<std::string> write_release_manifest(const ReleaseManifest& manifest) {
    try {
        std::string text;
        text += R"({"schema":)" + json_string(kReleaseManifestSchema);
        text += R"(,"schema_version":)" + std::to_string(kReleaseManifestVersion);
        text += R"(,"product":)" + json_string(kReleaseProduct);
        text += R"(,"channel":)" + json_string(manifest.channel);
        text += R"(,"version":)" + json_string(manifest.version.to_string());
        text += R"(,"published":)" + json_string(manifest.published);
        text += R"(,"architecture":)" + json_string(manifest.architecture);
        text += R"(,"minimum_os":)" + json_string(manifest.minimum_os.to_string());
        text += R"(,"installer":{"url":)" + json_string(manifest.installer.url);
        text += R"(,"size":)" + std::to_string(manifest.installer.size);
        text += R"(,"sha256":)" + json_string(core::to_hex(manifest.installer.sha256));
        text += R"(},"release_notes":)" + json_string(manifest.release_notes) + "}";
        // Only what the reader accepts is written.
        if (Expected<ReleaseManifest> reread = parse_release_manifest(text); !reread) {
            Error error = std::move(reread.error());
            error.code = ErrorCode::InvalidArgument;
            return Unexpected(std::move(error));
        }
        return text;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot write the release manifest", std::string(kWhereManifest));
    } catch (const std::exception& error) {
        // nlohmann refuses text that is not UTF-8.
        return fail(ErrorCode::InvalidArgument, std::format("cannot write the release manifest: {}", error.what()),
                    std::string(kWhereManifest));
    }
}

std::string write_release_signature(const ReleaseSignature& signature) {
    return std::format(
        R"({{"schema":"{}","schema_version":{},"algorithm":"{}","key_id":"{}","signature":"{}"}})",
        kReleaseSignatureSchema, kReleaseSignatureVersion, kReleaseSignatureAlgorithm, signature.key_id,
        core::to_base64(signature.signature));
}

Expected<VerifiedReleaseManifest> verify_release_manifest(std::string_view manifest_bytes,
                                                          std::string_view signature_json,
                                                          std::span<const core::EcdsaP256PublicKey> keys) {
    if (keys.empty()) {
        return fail(ErrorCode::InvalidArgument, "no update public key is configured", std::string(kWhereSignature));
    }
    if (manifest_bytes.size() > kMaxReleaseManifestBytes) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the release manifest has {} bytes, more than {}", manifest_bytes.size(),
                                kMaxReleaseManifestBytes),
                    std::string(kWhereManifest));
    }
    Expected<ReleaseSignature> signature = parse_release_signature(signature_json);
    if (!signature) {
        return Unexpected(std::move(signature.error()));
    }
    const core::EcdsaP256PublicKey* key = nullptr;
    for (const core::EcdsaP256PublicKey& candidate : keys) {
        if (core::key_id(candidate) == signature->key_id) {
            key = &candidate;
            break;
        }
    }
    if (key == nullptr) {
        return fail(ErrorCode::PermissionDenied,
                    std::format("the release manifest is signed by key {}, which this build does not trust",
                                signature->key_id),
                    std::string(kWhereUnknownKey));
    }
    const std::span<const std::byte> bytes = std::as_bytes(std::span(manifest_bytes.data(), manifest_bytes.size()));
    if (Status verified = core::verify_ecdsa_p256_sha256(*key, bytes, signature->signature); !verified) {
        if (verified.error().code == ErrorCode::Corrupt) {
            return fail(ErrorCode::PermissionDenied,
                        "the release manifest's signature does not verify: it was changed or not signed by the "
                        "publisher",
                        std::string(kWhereMismatch));
        }
        return Unexpected(std::move(verified.error()));
    }
    Expected<ReleaseManifest> manifest = parse_release_manifest(manifest_bytes);
    if (!manifest) {
        return Unexpected(std::move(manifest.error()));
    }
    Expected<core::Digest256> digest = core::sha256(bytes);
    if (!digest) {
        return Unexpected(std::move(digest.error()));
    }
    VerifiedReleaseManifest verified;
    verified.manifest = std::move(*manifest);
    verified.key_id = signature->key_id;
    verified.manifest_sha256 = *digest;
    return verified;
}

}  // namespace lucent::app
