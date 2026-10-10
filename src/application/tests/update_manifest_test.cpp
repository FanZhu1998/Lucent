// The update channel's documents and configuration (update_manifest.hpp, update_channel.hpp;
// ADR 0022): the canonical manifest round-trips, the strict readers refuse what they do not know,
// the manifest is authenticated before it is read, and the build's channel configuration is checked.

#include <gtest/gtest.h>

#include <cctype>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/application/http_client.hpp>
#include <lucent/application/update_channel.hpp>
#include <lucent/application/update_manifest.hpp>
#include <lucent/core/signature.hpp>

namespace lucent::app {
namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

ReleaseManifest sample_manifest() {
    ReleaseManifest manifest;
    manifest.channel = "stable";
    manifest.version = *core::SemanticVersion::parse("0.2.0");
    manifest.published = "2026-10-09T12:00:00Z";
    manifest.minimum_os = WindowsVersion{10, 0, 17763};
    manifest.installer.url = "https://updates.example.org/lucent/Lucent-Setup-0.2.0-x64.exe";
    manifest.installer.size = 41869312;
    manifest.installer.sha256 = *core::digest_from_hex(std::string(64, 'a'));
    manifest.release_notes = "Faster previews.\nFixes \"quoted\" names, tabs\tand \\ backslashes; caf\xC3\xA9.";
    return manifest;
}

std::string signed_signature(const core::EcdsaP256KeyPair& key, std::string_view manifest) {
    ReleaseSignature signature;
    signature.key_id = core::key_id(key.public_key());
    signature.signature = *key.sign(bytes_of(manifest));
    return write_release_signature(signature);
}

TEST(UpdateManifest, TheCanonicalManifestRoundTrips) {
    const ReleaseManifest manifest = sample_manifest();
    Expected<std::string> text = write_release_manifest(manifest);
    ASSERT_TRUE(text) << text.error().describe();
    EXPECT_TRUE(text->starts_with(R"({"schema":"lucent.release-manifest","schema_version":1,"product":"Lucent",)"))
        << *text;
    EXPECT_EQ(text->find('\n'), std::string::npos) << "no whitespace: the release notes' line break is escaped";
    Expected<ReleaseManifest> read = parse_release_manifest(*text);
    ASSERT_TRUE(read) << read.error().describe();
    EXPECT_EQ(read->channel, "stable");
    EXPECT_EQ(read->version.to_string(), "0.2.0");
    EXPECT_EQ(read->published, manifest.published);
    EXPECT_EQ(read->architecture, "x64");
    EXPECT_EQ(read->minimum_os, (WindowsVersion{10, 0, 17763}));
    EXPECT_EQ(read->installer.url, manifest.installer.url);
    EXPECT_EQ(read->installer.size, manifest.installer.size);
    EXPECT_EQ(read->installer.sha256, manifest.installer.sha256);
    EXPECT_EQ(read->release_notes, manifest.release_notes);
    Expected<std::string> again = write_release_manifest(*read);
    ASSERT_TRUE(again);
    EXPECT_EQ(*again, *text);
}

TEST(UpdateManifest, TheReaderIsStrict) {
    const std::string good = *write_release_manifest(sample_manifest());
    const auto replaced = [&](std::string_view from, std::string_view to) {
        std::string text = good;
        const std::size_t at = text.find(from);
        EXPECT_NE(at, std::string::npos) << from;
        text.replace(at, from.size(), to);
        return text;
    };
    struct Case {
        std::string text;
        ErrorCode code;
    };
    const std::vector<Case> cases = {
        {"", ErrorCode::Corrupt},
        {"[]", ErrorCode::Corrupt},
        {replaced(R"("schema_version":1)", R"("schema_version":2)"), ErrorCode::VersionMismatch},
        {replaced(R"("schema_version":1)", R"("schema_version":0)"), ErrorCode::Corrupt},
        {replaced(R"("product":"Lucent")", R"("product":"Other")"), ErrorCode::Corrupt},
        {replaced(R"("channel":"stable")", R"("channel":"Stable")"), ErrorCode::Corrupt},
        {replaced(R"("version":"0.2.0")", R"("version":"v0.2.0")"), ErrorCode::Corrupt},
        {replaced(R"("published":"2026-10-09T12:00:00Z")", R"("published":"2026-10-09 12:00")"), ErrorCode::Corrupt},
        {replaced(R"("architecture":"x64")", R"("architecture":"arm64")"), ErrorCode::Corrupt},
        {replaced(R"("minimum_os":"10.0.17763")", R"("minimum_os":"10.0")"), ErrorCode::Corrupt},
        {replaced(R"("url":"https://)", R"("url":"ftp://)"), ErrorCode::Corrupt},
        {replaced(R"("size":41869312)", R"("size":0)"), ErrorCode::Corrupt},
        {replaced(R"("size":41869312)", R"("size":-1)"), ErrorCode::Corrupt},
        {replaced(R"("size":41869312)", R"("size":4.5)"), ErrorCode::Corrupt},
        {replaced(std::string(64, 'a'), std::string(64, 'A')), ErrorCode::Corrupt},
        {replaced(R"("release_notes":)", R"("extra":1,"release_notes":)"), ErrorCode::Corrupt},
        {replaced(R"("release_notes":)", R"("channel":"beta","release_notes":)"), ErrorCode::Corrupt},
        {replaced(R"(,"sha256":)", R"(,"mirror":"x","sha256":)"), ErrorCode::Corrupt},
    };
    for (const Case& c : cases) {
        Expected<ReleaseManifest> read = parse_release_manifest(c.text);
        ASSERT_FALSE(read) << c.text;
        EXPECT_EQ(read.error().code, c.code) << c.text << "\n" << read.error().describe();
        EXPECT_EQ(read.error().where, "app.update.manifest") << c.text;
    }
    // Release notes beyond the limit.
    ReleaseManifest long_notes = sample_manifest();
    long_notes.release_notes.assign(kMaxReleaseNotesBytes + 1, 'x');
    EXPECT_FALSE(write_release_manifest(long_notes));
}

TEST(UpdateManifest, AManifestIsAuthenticatedBeforeItIsRead) {
    Expected<core::EcdsaP256KeyPair> key = core::EcdsaP256KeyPair::generate();
    ASSERT_TRUE(key);
    Expected<core::EcdsaP256KeyPair> other = core::EcdsaP256KeyPair::generate();
    ASSERT_TRUE(other);
    const std::vector<core::EcdsaP256PublicKey> pinned{key->public_key()};
    const std::string manifest = *write_release_manifest(sample_manifest());
    const std::string signature = signed_signature(*key, manifest);

    Expected<VerifiedReleaseManifest> verified = verify_release_manifest(manifest, signature, pinned);
    ASSERT_TRUE(verified) << verified.error().describe();
    EXPECT_EQ(verified->key_id, core::key_id(key->public_key()));
    EXPECT_EQ(verified->manifest.version.to_string(), "0.2.0");

    // Any change of the signed bytes, even of whitespace the reader would accept, is refused.
    for (const std::string& changed : {manifest + " ", std::string(" ") + manifest,
                                       std::string(manifest).replace(manifest.find("41869312"), 8, "41869313")}) {
        Expected<VerifiedReleaseManifest> refused = verify_release_manifest(changed, signature, pinned);
        ASSERT_FALSE(refused);
        EXPECT_EQ(refused.error().code, ErrorCode::PermissionDenied);
        EXPECT_EQ(refused.error().where, "app.update.signature.mismatch");
    }
    // A key the build does not pin.
    Expected<VerifiedReleaseManifest> unknown = verify_release_manifest(manifest, signed_signature(*other, manifest), pinned);
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, ErrorCode::PermissionDenied);
    EXPECT_EQ(unknown.error().where, "app.update.signature.unknown_key");
    // A signature made by another key but claiming the pinned key's id.
    ReleaseSignature forged = *parse_release_signature(signed_signature(*other, manifest));
    forged.key_id = core::key_id(key->public_key());
    Expected<VerifiedReleaseManifest> mismatch =
        verify_release_manifest(manifest, write_release_signature(forged), pinned);
    ASSERT_FALSE(mismatch);
    EXPECT_EQ(mismatch.error().where, "app.update.signature.mismatch");
    // A signed document that is not a valid manifest is still not interpreted.
    const std::string not_a_manifest = R"({"schema":"lucent.release-manifest","schema_version":1})";
    Expected<VerifiedReleaseManifest> malformed =
        verify_release_manifest(not_a_manifest, signed_signature(*key, not_a_manifest), pinned);
    ASSERT_FALSE(malformed);
    EXPECT_EQ(malformed.error().code, ErrorCode::Corrupt);
    // No keys, a malformed or newer signature document.
    EXPECT_FALSE(verify_release_manifest(manifest, signature, {}));
    Expected<VerifiedReleaseManifest> garbage = verify_release_manifest(manifest, "signature", pinned);
    ASSERT_FALSE(garbage);
    EXPECT_EQ(garbage.error().where, "app.update.signature");
    std::string newer = signature;
    newer.replace(newer.find(R"("schema_version":1)"), 18, R"("schema_version":2)");
    Expected<VerifiedReleaseManifest> future = verify_release_manifest(manifest, newer, pinned);
    ASSERT_FALSE(future);
    EXPECT_EQ(future.error().code, ErrorCode::VersionMismatch);
}

TEST(UpdateManifest, SignatureDocumentsAreStrict) {
    Expected<core::EcdsaP256KeyPair> key = core::EcdsaP256KeyPair::generate();
    ASSERT_TRUE(key);
    const std::string good = signed_signature(*key, "x");
    ASSERT_TRUE(parse_release_signature(good));
    for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
             {R"("algorithm":"ecdsa-p256-sha256")", R"("algorithm":"rsa-sha256")"},
             {R"("key_id":")", R"("key_id":"X)"},
             {R"("signature":")", R"("signature":"AAAA)"},
             {R"("schema":"lucent.release-signature")", R"("schema":"lucent.release-manifest")"},
             {R"("signature":)", R"("extra":true,"signature":)"},
         }) {
        std::string text = good;
        text.replace(text.find(from), from.size(), to);
        EXPECT_FALSE(parse_release_signature(text)) << text;
    }
}

TEST(UpdateManifest, WindowsVersionsParseAndCompare) {
    Expected<WindowsVersion> version = WindowsVersion::parse("10.0.17763");
    ASSERT_TRUE(version);
    EXPECT_EQ(version->to_string(), "10.0.17763");
    EXPECT_LT(*version, (WindowsVersion{10, 0, 19041}));
    EXPECT_LT((WindowsVersion{6, 3, 9600}), *version);
    for (const std::string_view bad : {"10", "10.0", "10.0.x", "10.0.017763", "10.0.1.2", "-1.0.0", ""}) {
        EXPECT_FALSE(WindowsVersion::parse(bad)) << bad;
    }
    const WindowsVersion running = running_windows_version();
    EXPECT_GE(running, (WindowsVersion{10, 0, 0})) << running.to_string();
}

TEST(UpdateChannel, AnEmptyConfigurationIsNotConfigured) {
    Expected<UpdateChannel> channel = make_update_channel("", "", "", "");
    ASSERT_TRUE(channel);
    EXPECT_FALSE(channel->configured());
    EXPECT_TRUE(channel->keys.empty());
}

TEST(UpdateChannel, AFullConfigurationIsParsedAndChecked) {
    Expected<core::EcdsaP256KeyPair> first = core::EcdsaP256KeyPair::generate();
    Expected<core::EcdsaP256KeyPair> second = core::EcdsaP256KeyPair::generate();
    ASSERT_TRUE(first && second);
    std::string upper = core::to_hex(second->public_key());
    for (char& c : upper) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    const std::string keys = " " + core::to_hex(first->public_key()) + " ;" + upper + ";;";
    const std::string thumbprint = "ABCDEF0123456789ABCDEF0123456789ABCDEF01";
    Expected<UpdateChannel> channel =
        make_update_channel("stable", "https://updates.example.org/lucent/stable/release-manifest.json", keys,
                            thumbprint + "; " + std::string(64, 'b'));
    ASSERT_TRUE(channel) << channel.error().describe();
    EXPECT_TRUE(channel->configured());
    EXPECT_EQ(channel->name, "stable");
    EXPECT_EQ(channel->signature_url(), "https://updates.example.org/lucent/stable/release-manifest.json.sig");
    ASSERT_EQ(channel->keys.size(), 2u);
    EXPECT_EQ(channel->keys[1], second->public_key());
    ASSERT_EQ(channel->publisher_thumbprints.size(), 2u);
    EXPECT_EQ(channel->publisher_thumbprints[0], "abcdef0123456789abcdef0123456789abcdef01");
}

TEST(UpdateChannel, PartialOrMalformedConfigurationsAreRefused) {
    Expected<core::EcdsaP256KeyPair> key = core::EcdsaP256KeyPair::generate();
    ASSERT_TRUE(key);
    const std::string hex = core::to_hex(key->public_key());
    const std::string url = "https://updates.example.org/m.json";
    struct Case {
        std::string name, url, keys, thumbprints;
    };
    for (const Case& c : std::vector<Case>{
             {"stable", url, "", ""},
             {"", url, hex, ""},
             {"stable", "", hex, ""},
             {"", "", "", "abcdef0123456789abcdef0123456789abcdef01"},
             {"Stable", url, hex, ""},
             {"stable", "http://updates.example.org/m.json", hex, ""},
             {"stable", "https://user:secret@updates.example.org/m.json", hex, ""},
             {"stable", url, hex.substr(2), ""},
             {"stable", url, "04" + std::string(128, '0'), ""},
             {"stable", url, hex, "abc"},
         }) {
        Expected<UpdateChannel> channel = make_update_channel(c.name, c.url, c.keys, c.thumbprints);
        ASSERT_FALSE(channel) << c.name << " " << c.url << " " << c.keys << " " << c.thumbprints;
        EXPECT_EQ(channel.error().code, ErrorCode::InvalidArgument);
        EXPECT_EQ(channel.error().where, "app.update.channel");
    }
}

TEST(UpdateChannel, TheDescriptionNamesWhatPackagingChecks) {
    // The line the packaging smoke run reads (installer\scripts\LucentPackaging.psm1,
    // ConvertFrom-UpdateChannelLogLine; W12 review SEC-02).
    EXPECT_EQ(describe_update_channel(UpdateChannel{}), "update channel: none (this build never checks for updates)");
    Expected<core::EcdsaP256KeyPair> key = core::EcdsaP256KeyPair::generate();
    ASSERT_TRUE(key);
    const std::string url = "https://updates.example.org/lucent/stable/release-manifest.json";
    Expected<UpdateChannel> unsigned_channel = make_update_channel("stable", url, core::to_hex(key->public_key()), "");
    ASSERT_TRUE(unsigned_channel);
    EXPECT_EQ(describe_update_channel(*unsigned_channel),
              "update channel 'stable' (" + url + "): keys " + core::key_id(key->public_key()) + "; publishers none");
    Expected<UpdateChannel> pinned = make_update_channel("stable", url, core::to_hex(key->public_key()),
                                                         "ABCDEF0123456789ABCDEF0123456789ABCDEF01;" + std::string(64, 'b'));
    ASSERT_TRUE(pinned);
    EXPECT_EQ(describe_update_channel(*pinned), "update channel 'stable' (" + url + "): keys " +
                                                    core::key_id(key->public_key()) +
                                                    "; publishers abcdef0123456789abcdef0123456789abcdef01," +
                                                    std::string(64, 'b'));
    BuildUpdateChannel refused;
    refused.problem = Error{ErrorCode::InvalidArgument, "a key is not a P-256 point", "app.update.channel", 0};
    EXPECT_EQ(describe_update_channel(refused).rfind("update channel refused: ", 0), 0u);
    EXPECT_EQ(describe_update_channel(build_update_channel()).rfind("update channel", 0), 0u);
}

TEST(UpdateChannel, ThisBuildHasAValidConfiguration) {
    // The default build has none; a configured build must have passed both CMake's and these checks.
    const BuildUpdateChannel& build = build_update_channel();
    EXPECT_FALSE(build.problem.has_value()) << (build.problem ? build.problem->describe() : std::string());
    if (build.channel.configured()) {
        EXPECT_FALSE(build.channel.keys.empty());
        EXPECT_TRUE(check_url(build.channel.manifest_url, UrlPolicy::HttpsOnly));
    }
}

TEST(UpdateUrls, ThePoliciesAcceptOnlyWhatTheyName) {
    EXPECT_TRUE(check_url("https://updates.example.org/x.json", UrlPolicy::HttpsOnly));
    EXPECT_TRUE(check_url("https://updates.example.org:8443/a/b?c=d", UrlPolicy::HttpsOnly));
    for (const std::string_view refused : {"http://updates.example.org/x.json", "http://127.0.0.1:8080/x",
                                           "https://user@updates.example.org/x", "ftp://updates.example.org/x",
                                           "file:///C:/Windows/notepad.exe"}) {
        Status checked = check_url(refused, UrlPolicy::HttpsOnly);
        ASSERT_FALSE(checked) << refused;
        EXPECT_EQ(checked.error().where, "app.update.url") << refused;
    }
    EXPECT_TRUE(check_url("http://127.0.0.1:8080/x", UrlPolicy::LoopbackOnly));
    EXPECT_TRUE(check_url("http://[::1]:8080/x", UrlPolicy::LoopbackOnly));
    EXPECT_TRUE(check_url("https://127.0.0.1/x", UrlPolicy::LoopbackOnly));
    for (const std::string_view refused : {"http://localhost:8080/x", "https://updates.example.org/x",
                                           "http://127.0.0.2/x", "http://10.0.0.1/x"}) {
        EXPECT_FALSE(check_url(refused, UrlPolicy::LoopbackOnly)) << refused;
    }
    EXPECT_FALSE(check_url("not a url", UrlPolicy::HttpsOnly));
    EXPECT_FALSE(check_url("", UrlPolicy::HttpsOnly));
}

}  // namespace
}  // namespace lucent::app
