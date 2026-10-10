// "Check for updates" end to end over a loopback server (update_service.hpp; ADR 0022): a build
// without a channel never touches the network; a release manifest is authenticated with a pinned
// test key before it is believed; only newer releases are offered; the installer is downloaded to a
// temporary file, verified (size, SHA-256 and, when the channel names a publisher, Authenticode)
// through a handle that excludes writers, renamed and held; failures leave nothing behind; the
// installation waits while a batch or Auto runs, backs up first, and starts the real program it
// verified.

#include <gtest/gtest.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <lucent/application/http_client.hpp>
#include <lucent/application/update_service.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/core/signature.hpp>

#include "loopback_http_server.hpp"
#include "raw_test_support.hpp"

namespace lucent::app {
namespace {

using raw::test_support::ScratchDir;
using test::LoopbackHttpServer;

constexpr std::string_view kManifestPath = "/stable/release-manifest.json";
constexpr std::string_view kInstallerPath = "/stable/Lucent-Setup-0.2.0-x64.exe";

std::span<const std::byte> bytes_of(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

core::Digest256 digest_of(std::string_view bytes) {
    return *core::sha256(bytes_of(bytes));
}

std::vector<std::wstring> files_in(const std::filesystem::path& folder) {
    std::vector<std::wstring> names;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        names.push_back(entry.path().filename().native());
    }
    return names;
}

std::filesystem::path system_file(const wchar_t* name) {
    wchar_t system[MAX_PATH] = {};
    const UINT length = ::GetSystemDirectoryW(system, MAX_PATH);
    return std::filesystem::path(std::wstring(system, length)) / name;
}

class UpdateServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(server.listening());
        Expected<core::EcdsaP256KeyPair> generated = core::EcdsaP256KeyPair::generate();
        ASSERT_TRUE(generated);
        key.emplace(std::move(*generated));
        HttpClientOptions loopback;
        loopback.policy = UrlPolicy::LoopbackOnly;
        loopback.system_proxy = false;
        loopback.receive_timeout = std::chrono::milliseconds(10000);
        Expected<std::unique_ptr<HttpClient>> client = create_winhttp_client(loopback);
        ASSERT_TRUE(client);
        this->http = std::shared_ptr<HttpClient>(std::move(*client));
        installer_bytes = std::string(300 * 1024, 'L');
        for (std::size_t i = 0; i < installer_bytes.size(); i += 4097) {
            installer_bytes[i] = static_cast<char>(i & 0xFF);
        }
    }

    UpdateServiceOptions options(std::string_view thumbprints = {}) {
        Expected<UpdateChannel> channel =
            make_update_channel("stable", server.url(kManifestPath), core::to_hex(key->public_key()), thumbprints,
                                UrlPolicy::LoopbackOnly);
        EXPECT_TRUE(channel) << (channel ? std::string() : channel.error().describe());
        UpdateServiceOptions o;
        o.channel = channel ? *channel : UpdateChannel{};
        o.current_version = *core::SemanticVersion::parse("0.1.0");
        o.windows = WindowsVersion{10, 0, 22631};
        o.download_directory = dir.path() / L"cache" / L"updates";
        o.log_directory = dir.path() / L"cache" / L"logs";
        o.http = http;
        o.launcher = [this](const std::filesystem::path& installer, const std::wstring& command_line)
            -> Expected<std::uint32_t> {
            launched.emplace_back(installer, command_line);
            return 4242u;
        };
        return o;
    }

    std::unique_ptr<UpdateService> service(UpdateServiceOptions o) {
        Expected<std::unique_ptr<UpdateService>> created = UpdateService::create(std::move(o));
        EXPECT_TRUE(created) << (created ? std::string() : created.error().describe());
        return created ? std::move(*created) : nullptr;
    }

    ReleaseManifest manifest_for(std::string_view version, std::string_view bytes) {
        ReleaseManifest manifest;
        manifest.channel = "stable";
        manifest.version = *core::SemanticVersion::parse(version);
        manifest.published = "2026-10-09T12:00:00Z";
        manifest.minimum_os = WindowsVersion{10, 0, 17763};
        manifest.installer.url = server.url(kInstallerPath);
        manifest.installer.size = bytes.size();
        manifest.installer.sha256 = digest_of(bytes);
        manifest.release_notes = "Release notes.";
        return manifest;
    }

    // Publishes `manifest` signed with `signer` (the pinned key by default), and `bytes` as the
    // installer.
    void publish(const ReleaseManifest& manifest, std::string_view bytes, const core::EcdsaP256KeyPair* signer = nullptr) {
        const core::EcdsaP256KeyPair& by = signer != nullptr ? *signer : *key;
        const std::string text = *write_release_manifest(manifest);
        ReleaseSignature signature;
        signature.key_id = core::key_id(by.public_key());
        signature.signature = *by.sign(bytes_of(text));
        server.set(std::string(kManifestPath), {.body = text});
        server.set(std::string(kManifestPath) + ".sig", {.body = write_release_signature(signature)});
        server.set(std::string(kInstallerPath), {.body = std::string(bytes)});
    }

    LoopbackHttpServer server;
    ScratchDir dir;
    std::optional<core::EcdsaP256KeyPair> key;
    std::shared_ptr<HttpClient> http;
    std::string installer_bytes;
    std::vector<std::pair<std::filesystem::path, std::wstring>> launched;
};

// An HttpClient that must never be used.
class UnusedHttpClient final : public HttpClient {
public:
    Expected<HttpResult> get(std::string_view, std::uint64_t, const HttpSink&, core::CancellationToken) override {
        ++calls;
        return fail(ErrorCode::Internal, "the network was used", "test.http");
    }
    std::atomic<int> calls{0};
};

TEST_F(UpdateServiceTest, ABuildWithoutAChannelNeverTouchesTheNetwork) {
    auto unused = std::make_shared<UnusedHttpClient>();
    UpdateServiceOptions o;
    o.current_version = *core::SemanticVersion::parse("0.1.0");
    o.http = unused;
    o.download_directory = dir.path() / L"updates";
    std::unique_ptr<UpdateService> updates = service(std::move(o));
    ASSERT_NE(updates, nullptr);
    EXPECT_FALSE(updates->configured());
    Expected<UpdateCheck> check = updates->check({});
    ASSERT_FALSE(check);
    EXPECT_EQ(check.error().code, ErrorCode::Unsupported);
    EXPECT_EQ(check.error().where, "app.update.not_configured");
    EXPECT_EQ(unused->calls.load(), 0);
    EXPECT_TRUE(server.requests().empty());
}

TEST_F(UpdateServiceTest, ASignedNewerReleaseIsAvailableAndOlderOnesAreNeverOffered) {
    std::unique_ptr<UpdateService> updates = service(options());
    ASSERT_NE(updates, nullptr);
    publish(manifest_for("0.2.0", installer_bytes), installer_bytes);
    Expected<UpdateCheck> check = updates->check({});
    ASSERT_TRUE(check) << check.error().describe();
    EXPECT_EQ(check->availability, UpdateAvailability::Available);
    EXPECT_EQ(check->release.manifest.version.to_string(), "0.2.0");
    EXPECT_EQ(check->release.key_id, core::key_id(key->public_key()));
    EXPECT_EQ(check->current.to_string(), "0.1.0");
    EXPECT_EQ(server.request_count(kManifestPath), 1u);
    EXPECT_EQ(server.request_count(std::string(kManifestPath) + ".sig"), 1u);
    EXPECT_EQ(server.request_count(kInstallerPath), 0u) << "checking downloads no installer";

    for (const std::string_view version : {"0.1.0", "0.0.9", "0.1.0-rc.1"}) {
        publish(manifest_for(version, installer_bytes), installer_bytes);
        check = updates->check({});
        ASSERT_TRUE(check) << check.error().describe();
        EXPECT_EQ(check->availability, UpdateAvailability::UpToDate) << version;
        Expected<DownloadedInstaller> refused = updates->download(*check, {});
        ASSERT_FALSE(refused);
        EXPECT_EQ(refused.error().code, ErrorCode::InvalidArgument);
    }
    ReleaseManifest newer_windows = manifest_for("0.3.0", installer_bytes);
    newer_windows.minimum_os = WindowsVersion{10, 0, 30000};
    publish(newer_windows, installer_bytes);
    check = updates->check({});
    ASSERT_TRUE(check) << check.error().describe();
    EXPECT_EQ(check->availability, UpdateAvailability::NeedsNewerWindows);
}

TEST_F(UpdateServiceTest, AManifestThatDoesNotVerifyIsRefused) {
    std::unique_ptr<UpdateService> updates = service(options());
    ASSERT_NE(updates, nullptr);
    // Signed by a key the build does not pin.
    Expected<core::EcdsaP256KeyPair> stranger = core::EcdsaP256KeyPair::generate();
    ASSERT_TRUE(stranger);
    publish(manifest_for("0.2.0", installer_bytes), installer_bytes, &*stranger);
    Expected<UpdateCheck> check = updates->check({});
    ASSERT_FALSE(check);
    EXPECT_EQ(check.error().where, "app.update.signature.unknown_key");
    // Changed after signing (the size of the installer, so a substituted installer would match it).
    publish(manifest_for("0.2.0", installer_bytes), installer_bytes);
    const std::string text = *write_release_manifest(manifest_for("0.2.0", installer_bytes));
    std::string tampered = text;
    tampered.replace(tampered.find("Release notes."), 14, "Release notez.");
    server.set(std::string(kManifestPath), {.body = tampered});
    check = updates->check({});
    ASSERT_FALSE(check);
    EXPECT_EQ(check.error().code, ErrorCode::PermissionDenied);
    EXPECT_EQ(check.error().where, "app.update.signature.mismatch");
    // A validly signed manifest of another channel.
    ReleaseManifest beta = manifest_for("0.2.0", installer_bytes);
    beta.channel = "beta";
    publish(beta, installer_bytes);
    check = updates->check({});
    ASSERT_FALSE(check);
    EXPECT_EQ(check.error().where, "app.update.manifest.channel");
    // No signature at all.
    publish(manifest_for("0.2.0", installer_bytes), installer_bytes);
    server.set(std::string(kManifestPath) + ".sig", {.status = 404, .body = "gone"});
    check = updates->check({});
    ASSERT_FALSE(check);
    EXPECT_EQ(check.error().code, ErrorCode::NotFound);
    EXPECT_EQ(check.error().where, "app.update.http");
    // A manifest larger than any real one.
    server.set(std::string(kManifestPath), {.body = std::string(kMaxReleaseManifestBytes + 1, ' ')});
    check = updates->check({});
    ASSERT_FALSE(check);
    EXPECT_EQ(check.error().code, ErrorCode::LimitExceeded);
}

TEST_F(UpdateServiceTest, TheInstallerIsDownloadedVerifiedAndHeld) {
    std::unique_ptr<UpdateService> updates = service(options());
    ASSERT_NE(updates, nullptr);
    publish(manifest_for("0.2.0", installer_bytes), installer_bytes);
    // Leftovers of earlier sessions are removed.
    const std::filesystem::path folder = dir.path() / L"cache" / L"updates";
    std::filesystem::create_directories(folder);
    std::ofstream(folder / L"~lucent-update-0123456789abcdef.partial") << "partial";
    std::ofstream(folder / L"Lucent-Setup-0.1.5-x64.exe") << "older";
    std::ofstream(folder / L"notes.txt") << "not ours";

    Expected<UpdateCheck> check = updates->check({});
    ASSERT_TRUE(check) << check.error().describe();
    std::vector<UpdateDownloadProgress> progress;
    Expected<DownloadedInstaller> installer =
        updates->download(*check, {}, [&](const UpdateDownloadProgress& p) { progress.push_back(p); });
    ASSERT_TRUE(installer) << installer.error().describe();
    EXPECT_TRUE(installer->is_open());
    EXPECT_EQ(installer->path(), folder / L"Lucent-Setup-0.2.0-x64.exe");
    EXPECT_EQ(installer->version().to_string(), "0.2.0");
    EXPECT_EQ(installer->size(), installer_bytes.size());
    EXPECT_EQ(installer->sha256(), digest_of(installer_bytes));
    EXPECT_FALSE(installer->authenticode().has_value()) << "no publisher is configured";
    ASSERT_FALSE(progress.empty());
    EXPECT_EQ(progress.back().received, installer_bytes.size());
    EXPECT_EQ(progress.back().total, installer_bytes.size());
    EXPECT_FALSE(progress.back().reused);
    EXPECT_TRUE(read_file(installer->path()) == installer_bytes) << "other readers can read the held file";
    std::vector<std::wstring> names = files_in(folder);
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::wstring>{L"Lucent-Setup-0.2.0-x64.exe", L"notes.txt"}));

    // Held: nobody can change, replace or delete the verified file.
    HANDLE writer = ::CreateFileW(installer->path().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
    EXPECT_EQ(writer, INVALID_HANDLE_VALUE);
    EXPECT_EQ(::GetLastError(), static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    if (writer != INVALID_HANDLE_VALUE) {
        ::CloseHandle(writer);
    }
    EXPECT_FALSE(::DeleteFileW(installer->path().c_str()));
    EXPECT_FALSE(::MoveFileExW(installer->path().c_str(), (folder / L"moved.exe").c_str(), 0));

    // A second download of the same release reuses the verified file without fetching it again.
    installer->release();
    Expected<DownloadedInstaller> again =
        updates->download(*check, {}, [&](const UpdateDownloadProgress& p) { progress.push_back(p); });
    ASSERT_TRUE(again) << again.error().describe();
    EXPECT_TRUE(progress.back().reused);
    EXPECT_EQ(server.request_count(kInstallerPath), 1u);
}

TEST_F(UpdateServiceTest, AnInstallerThatIsNotTheSignedOneIsRefusedAndRemoved) {
    std::unique_ptr<UpdateService> updates = service(options());
    ASSERT_NE(updates, nullptr);
    const std::filesystem::path folder = dir.path() / L"cache" / L"updates";
    const ReleaseManifest manifest = manifest_for("0.2.0", installer_bytes);

    // Same size, other bytes (a substituted installer).
    std::string other = installer_bytes;
    other[1000] = static_cast<char>(other[1000] ^ 0x01);
    publish(manifest, other);
    Expected<UpdateCheck> check = updates->check({});
    ASSERT_TRUE(check) << check.error().describe();
    Expected<DownloadedInstaller> refused = updates->download(*check, {});
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::Corrupt);
    EXPECT_EQ(refused.error().where, "app.update.download");
    EXPECT_TRUE(files_in(folder).empty());

    // More bytes than the manifest names: the transfer stops at the limit.
    publish(manifest, installer_bytes + "extra");
    check = updates->check({});
    ASSERT_TRUE(check);
    refused = updates->download(*check, {});
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::LimitExceeded);
    EXPECT_TRUE(files_in(folder).empty());

    // Fewer bytes (a complete answer that is too short).
    publish(manifest, installer_bytes.substr(0, installer_bytes.size() - 1));
    check = updates->check({});
    ASSERT_TRUE(check);
    refused = updates->download(*check, {});
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::Corrupt);
    EXPECT_TRUE(files_in(folder).empty());

    // A corrupted earlier download is not reused: it is fetched again.
    publish(manifest, installer_bytes);
    std::filesystem::create_directories(folder);
    std::ofstream(folder / L"Lucent-Setup-0.2.0-x64.exe", std::ios::binary) << other;
    check = updates->check({});
    ASSERT_TRUE(check);
    std::vector<UpdateDownloadProgress> progress;
    Expected<DownloadedInstaller> fresh =
        updates->download(*check, {}, [&](const UpdateDownloadProgress& p) { progress.push_back(p); });
    ASSERT_TRUE(fresh) << fresh.error().describe();
    EXPECT_FALSE(progress.back().reused);
    EXPECT_TRUE(read_file(fresh->path()) == installer_bytes);
}

TEST_F(UpdateServiceTest, CancellingADownloadLeavesNothing) {
    std::unique_ptr<UpdateService> updates = service(options());
    ASSERT_NE(updates, nullptr);
    publish(manifest_for("0.2.0", installer_bytes), installer_bytes);
    server.set(std::string(kInstallerPath), {.body = installer_bytes, .stall_at = 128 * 1024,
                                             .stall = std::chrono::milliseconds(20000)});
    Expected<UpdateCheck> check = updates->check({});
    ASSERT_TRUE(check);
    core::CancellationSource source;
    const auto started = std::chrono::steady_clock::now();
    Expected<DownloadedInstaller> cancelled = updates->download(*check, source.token(), [&](const UpdateDownloadProgress& p) {
        if (p.received >= 64 * 1024) {
            source.cancel();
        }
    });
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::Cancelled);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(5000));
    EXPECT_TRUE(files_in(dir.path() / L"cache" / L"updates").empty());
}

TEST_F(UpdateServiceTest, APublisherThumbprintRequiresTheInstallersAuthenticodeSignature) {
    // An unsigned installer is refused when the channel names a publisher.
    std::unique_ptr<UpdateService> strict = service(options("abcdef0123456789abcdef0123456789abcdef01"));
    ASSERT_NE(strict, nullptr);
    publish(manifest_for("0.2.0", installer_bytes), installer_bytes);
    Expected<UpdateCheck> check = strict->check({});
    ASSERT_TRUE(check) << check.error().describe();
    Expected<DownloadedInstaller> refused = strict->download(*check, {});
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::PermissionDenied);
    EXPECT_EQ(refused.error().where, "app.update.authenticode");
    EXPECT_TRUE(files_in(dir.path() / L"cache" / L"updates").empty());

    // A file with a valid Microsoft signature (the Visual C++ runtime) stands in for a signed
    // installer: accepted for its signer's thumbprint, refused for any other.
    const std::filesystem::path signed_dll = system_file(L"vcruntime140.dll");
    Expected<core::AuthenticodeCheck> signature = core::check_authenticode(signed_dll);
    if (!signature || !signature->trusted() || !signature->signer) {
        GTEST_SKIP() << "no Authenticode-signed Visual C++ runtime in System32";
    }
    const std::string signed_bytes = read_file(signed_dll);
    for (const auto& [thumbprint, accepted] :
         {std::pair{signature->signer->sha1, true}, std::pair{signature->signer->sha256, true},
          std::pair{std::string("abcdef0123456789abcdef0123456789abcdef01"), false}}) {
        std::unique_ptr<UpdateService> updates = service(options(thumbprint));
        ASSERT_NE(updates, nullptr);
        publish(manifest_for("0.2.0", signed_bytes), signed_bytes);
        check = updates->check({});
        ASSERT_TRUE(check) << check.error().describe();
        Expected<DownloadedInstaller> installer = updates->download(*check, {});
        EXPECT_EQ(static_cast<bool>(installer), accepted)
            << thumbprint << ": " << (installer ? std::string("accepted") : installer.error().describe());
        if (installer) {
            ASSERT_TRUE(installer->authenticode().has_value());
            EXPECT_TRUE(installer->authenticode()->trusted());
        } else {
            EXPECT_EQ(installer.error().where, "app.update.authenticode");
        }
    }
}

TEST_F(UpdateServiceTest, InstallingWaitsForRunningWorkBacksUpAndStartsSetup) {
    std::optional<Error> blocker = Error{ErrorCode::AlreadyExists, "an export is running", "app.update.busy", 0};
    std::vector<std::string> steps;
    UpdateServiceOptions o = options();
    o.install_blocker = [&] { return blocker; };
    o.before_install = [&]() -> Status {
        steps.push_back("backup");
        return ok();
    };
    std::unique_ptr<UpdateService> updates = service(std::move(o));
    ASSERT_NE(updates, nullptr);
    publish(manifest_for("0.2.0", installer_bytes), installer_bytes);
    Expected<UpdateCheck> check = updates->check({});
    ASSERT_TRUE(check);
    Expected<DownloadedInstaller> installer = updates->download(*check, {});
    ASSERT_TRUE(installer) << installer.error().describe();
    const std::filesystem::path path = installer->path();

    ASSERT_TRUE(updates->install_blocker().has_value());
    Expected<InstallerLaunch> waiting = updates->install(std::move(*installer));
    ASSERT_FALSE(waiting);
    EXPECT_EQ(waiting.error().where, "app.update.busy");
    EXPECT_TRUE(steps.empty());
    EXPECT_TRUE(launched.empty());

    blocker.reset();
    installer = updates->download(*check, {});  // reused
    ASSERT_TRUE(installer);
    Expected<InstallerLaunch> launch = updates->install(std::move(*installer));
    ASSERT_TRUE(launch) << launch.error().describe();
    EXPECT_EQ(steps, (std::vector<std::string>{"backup"}));
    ASSERT_EQ(launched.size(), 1u);
    EXPECT_EQ(launched[0].first, path);
    const std::filesystem::path log = dir.path() / L"cache" / L"logs" / L"setup-0.2.0.log";
    EXPECT_EQ(launched[0].second, L"\"" + path.native() + L"\" /LOG=\"" + log.native() + L"\"");
    EXPECT_EQ(launch->process_id, 4242u);
    EXPECT_EQ(launch->log, log);
    // The handle is released once Setup started: the file can be deleted again.
    EXPECT_TRUE(::DeleteFileW(path.c_str()));
}

TEST_F(UpdateServiceTest, AFailedBackupStopsTheInstallation) {
    UpdateServiceOptions o = options();
    o.before_install = []() -> Status { return fail(ErrorCode::DiskFull, "no room for the backup", "catalog.backup"); };
    std::unique_ptr<UpdateService> updates = service(std::move(o));
    ASSERT_NE(updates, nullptr);
    publish(manifest_for("0.2.0", installer_bytes), installer_bytes);
    Expected<UpdateCheck> check = updates->check({});
    ASSERT_TRUE(check);
    Expected<DownloadedInstaller> installer = updates->download(*check, {});
    ASSERT_TRUE(installer);
    Expected<InstallerLaunch> launch = updates->install(std::move(*installer));
    ASSERT_FALSE(launch);
    EXPECT_EQ(launch.error().code, ErrorCode::DiskFull);
    EXPECT_EQ(launch.error().where, "app.update.prepare");
    EXPECT_TRUE(launched.empty());
}

TEST_F(UpdateServiceTest, TheVerifiedProgramIsStartedWhileItIsHeld) {
    // The real launcher (CreateProcessW) and a real program served as the installer: it records the
    // command line it was started with.
    const std::string program = read_file(LUCENT_UPDATE_TEST_INSTALLER_EXE);
    ASSERT_FALSE(program.empty());
    const std::filesystem::path marker = dir.path() / L"started.txt";
    ASSERT_EQ(_wputenv_s(L"LUCENT_TEST_INSTALLER_MARKER", marker.c_str()), 0);
    UpdateServiceOptions o = options();
    o.launcher = {};  // the default
    std::unique_ptr<UpdateService> updates = service(std::move(o));
    ASSERT_NE(updates, nullptr);
    publish(manifest_for("0.2.0", program), program);
    Expected<UpdateCheck> check = updates->check({});
    ASSERT_TRUE(check) << check.error().describe();
    Expected<DownloadedInstaller> installer = updates->download(*check, {});
    ASSERT_TRUE(installer) << installer.error().describe();
    Expected<InstallerLaunch> launch = updates->install(std::move(*installer));
    ASSERT_TRUE(launch) << launch.error().describe();
    EXPECT_NE(launch->process_id, 0u);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!std::filesystem::exists(marker) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    _wputenv_s(L"LUCENT_TEST_INSTALLER_MARKER", L"");
    ASSERT_TRUE(std::filesystem::exists(marker)) << "the installer did not run";
    const std::string recorded = read_file(marker);
    const std::wstring command_line(reinterpret_cast<const wchar_t*>(recorded.data()), recorded.size() / sizeof(wchar_t));
    EXPECT_NE(command_line.find(L"Lucent-Setup-0.2.0-x64.exe\""), std::wstring::npos);
    EXPECT_NE(command_line.find(L"/LOG=\""), std::wstring::npos);
}

// The publisher's tooling (installer\scripts\LucentUpdate.psm1, .NET's ECDsaCng) and Lucent's reader
// (C++, BCrypt) agree: a manifest the tooling writes and signs with an ephemeral key verifies here,
// is byte for byte the canonical text Lucent writes for the same release, and names the installer.
TEST(UpdateInterop, ThePublishersToolingProducesWhatLucentVerifies) {
    wchar_t system[MAX_PATH] = {};
    const UINT length = ::GetSystemDirectoryW(system, MAX_PATH);
    const std::filesystem::path powershell =
        std::filesystem::path(std::wstring(system, length)) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
    if (!std::filesystem::exists(powershell)) {
        GTEST_SKIP() << "Windows PowerShell is not available";
    }
    ScratchDir dir;
    const std::filesystem::path module =
        std::filesystem::path(LUCENT_SOURCE_DIR) / L"installer" / L"scripts" / L"LucentUpdate.psm1";
    const std::filesystem::path script = dir.path() / L"publish.ps1";
    std::ofstream(script, std::ios::binary) << R"ps(param([string]$Module, [string]$Work)
$ErrorActionPreference = 'Stop'
Import-Module $Module -Force
$key = [System.Security.Cryptography.CngKey]::Create([System.Security.Cryptography.CngAlgorithm]::ECDsaP256)
try {
    $installer = Join-Path $Work 'Lucent-Setup-0.2.0-x64.exe'
    [System.IO.File]::WriteAllBytes($installer, [byte[]](0..255))
    $notes = "Notes with `"quotes`", a tab`tand a line`nbreak, caf" + [char]0xE9 + ' \ done.'
    New-LucentReleaseManifest -Installer $installer -InstallerUrl 'https://updates.example.org/l/Lucent-Setup-0.2.0-x64.exe' `
        -Channel stable -ReleaseNotes $notes -Published ([datetime]::new(2026, 10, 9, 12, 0, 0, [DateTimeKind]::Utc)) `
        -OutFile (Join-Path $Work 'release-manifest.json') | Out-Null
    Protect-LucentReleaseManifest -Manifest (Join-Path $Work 'release-manifest.json') -Key $key | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $Work 'public-key.txt'), (Get-LucentUpdatePublicKey -Key $key).PublicKey)
}
finally { $key.Dispose() }
)ps";
    std::wstring command = L"\"" + powershell.native() + L"\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" +
                           script.native() + L"\" -Module \"" + module.native() + L"\" -Work \"" + dir.path().native() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    ASSERT_TRUE(::CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                 nullptr, &startup, &process));
    const DWORD waited = ::WaitForSingleObject(process.hProcess, 120000);
    DWORD exit_code = 1;
    ::GetExitCodeProcess(process.hProcess, &exit_code);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    ASSERT_EQ(waited, WAIT_OBJECT_0);
    ASSERT_EQ(exit_code, 0u) << "the publishing script failed";

    const std::string manifest = read_file(dir.path() / L"release-manifest.json");
    const std::string signature = read_file(dir.path() / L"release-manifest.json.sig");
    Expected<core::EcdsaP256PublicKey> key = core::parse_ecdsa_p256_public_key(read_file(dir.path() / L"public-key.txt"));
    ASSERT_TRUE(key) << key.error().describe();
    const std::vector<core::EcdsaP256PublicKey> pinned{*key};
    Expected<VerifiedReleaseManifest> verified = verify_release_manifest(manifest, signature, pinned);
    ASSERT_TRUE(verified) << verified.error().describe();
    EXPECT_EQ(verified->manifest.version.to_string(), "0.2.0");
    EXPECT_EQ(verified->manifest.channel, "stable");
    EXPECT_EQ(verified->manifest.installer.size, 256u);
    EXPECT_EQ(verified->manifest.installer.sha256, digest_of(read_file(dir.path() / L"Lucent-Setup-0.2.0-x64.exe")));
    EXPECT_EQ(verified->manifest.release_notes, "Notes with \"quotes\", a tab\tand a line\nbreak, caf\xC3\xA9 \\ done.");
    Expected<std::string> canonical = write_release_manifest(verified->manifest);
    ASSERT_TRUE(canonical);
    EXPECT_EQ(*canonical, manifest) << "both implementations write the same canonical text";
}

}  // namespace
}  // namespace lucent::app
