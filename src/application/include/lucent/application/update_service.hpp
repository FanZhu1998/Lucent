#pragma once

// UpdateService: "Check for updates" (blueprint section 8: "Version 1 uses a signed full installer and
// a graphical Check for updates flow ... Verify downloaded installers ... against an authenticated
// release manifest or signature tied to the trusted publisher identity before activation; hashes alone
// do not authenticate a download. Apply application updates between jobs and retain migration recovery
// information. No cloud account or network connection is required for opening, editing, or
// exporting"; ADR 0022).
//
// Offline first. Nothing here runs unless the photographer asks: there is no check at start-up and no
// background polling. A build whose channel is not configured (update_channel.hpp, the default) never
// opens a connection: check() fails with Unsupported ("app.update.not_configured").
//
// check():   GETs the channel's release manifest and its detached signature (http_client.hpp: https
//            only), authenticates the manifest against the build's pinned keys before reading it
//            (update_manifest.hpp), requires its channel to be the build's (a signed manifest of
//            another channel is refused: "app.update.manifest.channel", PermissionDenied), and
//            compares: a release not newer than this build is UpToDate (an older one is never
//            offered: no downgrades); a newer one is Available, or NeedsNewerWindows when this
//            Windows is older than its minimum_os.
// download(): fetches the installer of an Available release into the download folder (the per-user
//            cache, <cache>\updates) under a temporary name ("~lucent-update-<random>.partial", opened
//            exclusively), streaming at most the manifest's size, renames the complete file to
//            "Lucent-Setup-<version>-x64.exe" and opens it for reading with writers, renamers and
//            deleters excluded (FILE_SHARE_READ only; the handle itself can only read, so Setup and
//            the loader open the file normally), and through that handle checks
//              * the size and SHA-256 the signed manifest names ("app.update.download", Corrupt);
//              * when the channel names publisher thumbprints, an Authenticode signature that
//                verifies and chains to a trusted root (core::check_authenticode: no revocation
//                check, no network) by one of those certificates ("app.update.authenticode",
//                PermissionDenied);
//            The returned DownloadedInstaller keeps that handle: nobody can change, replace or delete
//            the file until it is launched or dropped. A failed check deletes the file. Earlier
//            downloads in the folder
//            are deleted first, except a complete one of the same release, which is checked the
//            same way and reused instead of being fetched again.
// install(): only between jobs: refuses while install_blocker() names a reason (a batch or Auto
//            running: "app.update.busy"); checks the held file's size and SHA-256 once more; runs
//            before_install (Lucent.exe: a verified pre-update backup of the catalog, "retain
//            migration recovery information"), whose error stops it ("app.update.prepare"); then
//            starts Setup ("<installer>" /LOG="<log folder>\setup-<version>.log": the graphical
//            wizard, which upgrades the installation in place, ADR 0009) and returns. The caller
//            then exits cleanly (Lucent.exe quits; Setup's Restart Manager closes a Lucent that is
//            still running). The installer itself migrates nothing: the new Lucent migrates the
//            catalog when it first opens it, after its own pre-migration backup (ADR 0008).
//
// Errors besides those above: the HTTP client's ("app.update.url", "app.update.network",
// "app.update.tls", "app.update.http"), the manifest's ("app.update.manifest", "app.update.signature*"),
// file errors in the download folder, Cancelled.
//
// Thread-safety: all members may be called concurrently from any thread; check() and download() block
// (network and disk), so user interfaces call them off their UI thread. A DownloadedInstaller is
// move-only and used from one thread at a time.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <lucent/application/http_client.hpp>
#include <lucent/application/update_channel.hpp>
#include <lucent/application/update_manifest.hpp>
#include <lucent/core/authenticode.hpp>
#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/core/version.hpp>

namespace lucent::app {

enum class UpdateAvailability : std::uint8_t {
    UpToDate = 1,           // the channel's release is not newer than this build
    Available = 2,          // a newer release this Windows can run
    NeedsNewerWindows = 3,  // a newer release whose minimum_os is newer than this Windows
};

// "up_to_date", "available", "needs_newer_windows".
[[nodiscard]] std::string_view to_string(UpdateAvailability availability) noexcept;

struct UpdateCheck {
    VerifiedReleaseManifest release;
    core::SemanticVersion current;  // this build
    WindowsVersion windows;         // this machine
    UpdateAvailability availability = UpdateAvailability::UpToDate;
};

struct UpdateDownloadProgress {
    std::uint64_t received = 0;
    std::uint64_t total = 0;
    bool reused = false;  // a complete earlier download of the release was found and checked
};

// A verified installer, held open with writers excluded until it is launched or destroyed.
class DownloadedInstaller {
public:
    DownloadedInstaller() noexcept = default;
    DownloadedInstaller(DownloadedInstaller&& other) noexcept;
    DownloadedInstaller& operator=(DownloadedInstaller&& other) noexcept;
    DownloadedInstaller(const DownloadedInstaller&) = delete;
    DownloadedInstaller& operator=(const DownloadedInstaller&) = delete;
    ~DownloadedInstaller();

    [[nodiscard]] bool is_open() const noexcept { return handle_ != nullptr; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] const core::SemanticVersion& version() const noexcept { return version_; }
    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] const core::Digest256& sha256() const noexcept { return sha256_; }
    // The Authenticode check, when the channel names publisher thumbprints.
    [[nodiscard]] const std::optional<core::AuthenticodeCheck>& authenticode() const noexcept { return authenticode_; }
    // Closes the handle (the file stays). Idempotent.
    void release() noexcept;

private:
    friend class UpdateService;
    void* handle_ = nullptr;  // HANDLE: GENERIC_READ, FILE_SHARE_READ
    std::filesystem::path path_;
    core::SemanticVersion version_;
    std::uint64_t size_ = 0;
    core::Digest256 sha256_{};
    std::optional<core::AuthenticodeCheck> authenticode_;
};

struct InstallerLaunch {
    std::uint32_t process_id = 0;
    std::filesystem::path installer;
    std::filesystem::path log;  // empty without a log folder
};

// Starts the installer: its path and the full command line. Returns the process id.
using InstallerLauncher =
    std::function<Expected<std::uint32_t>(const std::filesystem::path& installer, const std::wstring& command_line)>;

struct UpdateServiceOptions {
    UpdateChannel channel;                  // not configured: nothing is ever fetched
    core::SemanticVersion current_version;  // this build (core::build_info().version)
    std::optional<WindowsVersion> windows;  // this machine; default running_windows_version()
    // Where installers are downloaded (created when missing; Lucent.exe: <cache>\updates).
    std::filesystem::path download_directory;
    // Where Setup writes its log (/LOG); empty: no log (Lucent.exe: <cache>\logs).
    std::filesystem::path log_directory;
    // Null: create_winhttp_client() with UrlPolicy::HttpsOnly and the system's proxy.
    std::shared_ptr<HttpClient> http;
    // Why an installation must wait now (a batch or Auto running); std::nullopt when it may start.
    std::function<std::optional<Error>()> install_blocker;
    // Runs right before Setup starts; an error stops the installation.
    std::function<Status()> before_install;
    // Starts Setup; default CreateProcessW (tests replace it).
    InstallerLauncher launcher;
};

class UpdateService {
public:
    // Errors: the HTTP client's creation (only for a configured channel without options.http).
    [[nodiscard]] static Expected<std::unique_ptr<UpdateService>> create(UpdateServiceOptions options);
    ~UpdateService();
    UpdateService(const UpdateService&) = delete;
    UpdateService& operator=(const UpdateService&) = delete;

    [[nodiscard]] bool configured() const noexcept;
    [[nodiscard]] const UpdateChannel& channel() const noexcept;
    [[nodiscard]] const core::SemanticVersion& current_version() const noexcept;

    [[nodiscard]] Expected<UpdateCheck> check(core::CancellationToken cancel) const;

    // `check` must be Available (InvalidArgument, "app.update.download", otherwise). `progress` is
    // called on the calling thread as bytes arrive (at most about every 64 KiB).
    [[nodiscard]] Expected<DownloadedInstaller> download(
        const UpdateCheck& check, core::CancellationToken cancel,
        const std::function<void(const UpdateDownloadProgress&)>& progress = {}) const;

    // The reason an installation must wait now (UpdateServiceOptions::install_blocker), if any.
    [[nodiscard]] std::optional<Error> install_blocker() const;

    // See the file comment. The installer's handle is released whatever the outcome.
    [[nodiscard]] Expected<InstallerLaunch> install(DownloadedInstaller installer) const;

    struct Impl;  // implementation detail

private:
    explicit UpdateService(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// The default InstallerLauncher: CreateProcessW without inherited handles. Errors: the Win32 error
// ("app.update.launch").
[[nodiscard]] Expected<std::uint32_t> launch_installer_process(const std::filesystem::path& installer,
                                                               const std::wstring& command_line);

}  // namespace lucent::app
