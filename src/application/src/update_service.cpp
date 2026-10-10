#include "lucent/application/update_service.hpp"

#include <windows.h>

#include <algorithm>
#include <format>
#include <new>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <lucent/core/log.hpp>
#include <lucent/core/storage.hpp>
#include <lucent/core/text.hpp>
#include <lucent/core/win32_error.hpp>

namespace lucent::app {

namespace {

constexpr std::string_view kLog = "app.update";
constexpr std::string_view kWhereNotConfigured = "app.update.not_configured";
constexpr std::string_view kWhereChannel = "app.update.manifest.channel";
constexpr std::string_view kWhereDownload = "app.update.download";
constexpr std::string_view kWhereAuthenticode = "app.update.authenticode";
constexpr std::string_view kWherePrepare = "app.update.prepare";
constexpr std::string_view kWhereLaunch = "app.update.launch";

constexpr std::wstring_view kTemporaryPrefix = L"~lucent-update-";
constexpr std::wstring_view kTemporarySuffix = L".partial";
constexpr std::wstring_view kInstallerPrefix = L"Lucent-Setup-";
constexpr std::size_t kHashChunkBytes = std::size_t{1} << 20;

// Owner of a Win32 file handle.
class FileHandle {
public:
    FileHandle() noexcept = default;
    explicit FileHandle(HANDLE handle) noexcept : handle_(handle == INVALID_HANDLE_VALUE ? nullptr : handle) {}
    ~FileHandle() { reset(); }
    FileHandle(FileHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    FileHandle& operator=(FileHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }
    [[nodiscard]] HANDLE release() noexcept { return std::exchange(handle_, nullptr); }
    void reset() noexcept {
        if (handle_ != nullptr) {
            ::CloseHandle(handle_);
            handle_ = nullptr;
        }
    }

private:
    HANDLE handle_ = nullptr;
};

Error file_error(std::string_view what, const std::filesystem::path& path, DWORD code) {
    return core::win32_error(code, std::string(kWhereDownload),
                             std::format("{} {}", what, core::path_to_utf8_lossy(path)));
}

std::wstring installer_file_name(const core::SemanticVersion& version) {
    return std::wstring(kInstallerPrefix) + core::utf8_to_wide_lossy(version.to_string()) + L"-x64.exe";
}

std::wstring random_suffix() {
    std::random_device random;
    return std::format(L"{:08x}{:08x}", random(), random());
}

// SHA-256 and size of an open file, read from its start.
Expected<std::pair<std::uint64_t, core::Digest256>> hash_handle(HANDLE file, const core::CancellationToken& cancel,
                                                                const std::filesystem::path& path) {
    LARGE_INTEGER zero{};
    if (!::SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) {
        return Unexpected(file_error("cannot read", path, ::GetLastError()));
    }
    std::vector<std::byte> buffer(kHashChunkBytes);
    core::Sha256 hasher;
    std::uint64_t total = 0;
    for (;;) {
        if (Status cancelled = cancel.check(kWhereDownload); !cancelled) {
            return Unexpected(std::move(cancelled.error()));
        }
        DWORD read = 0;
        if (!::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            return Unexpected(file_error("cannot read", path, ::GetLastError()));
        }
        if (read == 0) {
            break;
        }
        hasher.update(std::span<const std::byte>(buffer.data(), read));
        total += read;
    }
    Expected<core::Digest256> digest = hasher.finish();
    if (!digest) {
        return Unexpected(std::move(digest.error()));
    }
    return std::pair{total, *digest};
}

// Opens `path` for reading with writers, renamers and deleters excluded (FILE_SHARE_READ only), so
// the file object verified through this handle is the one at `path` until the handle is closed. The
// handle itself has no write or DELETE access: other readers (Setup reading its own file, the
// loader) open the file normally, and an image section can be created while it is held.
Expected<FileHandle> open_held(const std::filesystem::path& path) {
    FileHandle file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file) {
        return Unexpected(file_error("cannot open", path, ::GetLastError()));
    }
    return file;
}

// Deletes a download that failed its checks (after its handle was closed).
void remove_refused(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::remove(path, error) && error) {
        core::log_warn(kLog, "a refused update download could not be removed: {} ({})", core::path_to_utf8_lossy(path),
                       error.message());
    }
}

bool matches_thumbprint(const core::AuthenticodeSigner& signer, const std::vector<std::string>& thumbprints) {
    return std::any_of(thumbprints.begin(), thumbprints.end(), [&](const std::string& thumbprint) {
        return thumbprint.size() == 40 ? thumbprint == signer.sha1 : thumbprint == signer.sha256;
    });
}

std::wstring quoted(const std::filesystem::path& path) {
    return L"\"" + path.native() + L"\"";
}

}  // namespace

std::string_view to_string(UpdateAvailability availability) noexcept {
    switch (availability) {
        case UpdateAvailability::UpToDate:
            return "up_to_date";
        case UpdateAvailability::Available:
            return "available";
        case UpdateAvailability::NeedsNewerWindows:
            return "needs_newer_windows";
    }
    return "invalid";
}

// ---- DownloadedInstaller ------------------------------------------------------------------------

DownloadedInstaller::DownloadedInstaller(DownloadedInstaller&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)),
      path_(std::move(other.path_)),
      version_(std::move(other.version_)),
      size_(other.size_),
      sha256_(other.sha256_),
      authenticode_(std::move(other.authenticode_)) {}

DownloadedInstaller& DownloadedInstaller::operator=(DownloadedInstaller&& other) noexcept {
    if (this != &other) {
        release();
        handle_ = std::exchange(other.handle_, nullptr);
        path_ = std::move(other.path_);
        version_ = std::move(other.version_);
        size_ = other.size_;
        sha256_ = other.sha256_;
        authenticode_ = std::move(other.authenticode_);
    }
    return *this;
}

DownloadedInstaller::~DownloadedInstaller() {
    release();
}

void DownloadedInstaller::release() noexcept {
    if (handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
}

// ---- UpdateService ------------------------------------------------------------------------------

struct UpdateService::Impl {
    UpdateServiceOptions options;
    WindowsVersion windows;

    // Checks the held file against the release and, when the channel names publishers, its signature.
    // `file` must have been opened with open_held().
    Status verify_held(HANDLE file, const std::filesystem::path& path, const ReleaseManifest& release,
                       const core::CancellationToken& cancel, std::optional<core::AuthenticodeCheck>& authenticode) const {
        Expected<std::pair<std::uint64_t, core::Digest256>> hashed = hash_handle(file, cancel, path);
        if (!hashed) {
            return Unexpected(std::move(hashed.error()));
        }
        if (hashed->first != release.installer.size || hashed->second != release.installer.sha256) {
            return fail(ErrorCode::Corrupt,
                        std::format("the downloaded installer ({} bytes, SHA-256 {}) is not the one the signed release "
                                    "manifest names ({} bytes, SHA-256 {})",
                                    hashed->first, core::to_hex(hashed->second), release.installer.size,
                                    core::to_hex(release.installer.sha256)),
                        std::string(kWhereDownload));
        }
        if (options.channel.publisher_thumbprints.empty()) {
            authenticode.reset();
            return ok();
        }
        Expected<core::AuthenticodeCheck> check = core::check_authenticode(path, file);
        if (!check) {
            return Unexpected(std::move(check.error()));
        }
        authenticode = *check;
        if (!check->trusted()) {
            return fail(ErrorCode::PermissionDenied,
                        std::format("the installer's Authenticode signature is not valid: {}",
                                    core::describe_trust_result(check->result)),
                        std::string(kWhereAuthenticode), check->result);
        }
        if (!check->signer || !matches_thumbprint(*check->signer, options.channel.publisher_thumbprints)) {
            return fail(ErrorCode::PermissionDenied,
                        std::format("the installer is signed by '{}' ({}), not by the publisher this build trusts",
                                    check->signer ? check->signer->subject : std::string("nobody"),
                                    check->signer ? check->signer->sha1 : std::string("-")),
                        std::string(kWhereAuthenticode));
        }
        return ok();
    }

    // Removes earlier downloads from the folder: temporary files, and installers of other releases
    // (`keep` is left for the caller to check). Failures are logged, not returned.
    void remove_earlier_downloads(const std::filesystem::path& keep) const {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(options.download_directory, error)) {
            const std::wstring name = entry.path().filename().native();
            const bool temporary = name.starts_with(kTemporaryPrefix) && name.ends_with(kTemporarySuffix);
            const bool installer = name.starts_with(kInstallerPrefix) && name.ends_with(L".exe");
            if ((!temporary && !installer) || entry.path() == keep) {
                continue;
            }
            std::error_code removed;
            if (!std::filesystem::remove(entry.path(), removed) && removed) {
                core::log_warn(kLog, "an earlier update download could not be removed: {} ({})",
                               core::path_to_utf8_lossy(entry.path()), removed.message());
            }
        }
    }
};

Expected<std::unique_ptr<UpdateService>> UpdateService::create(UpdateServiceOptions options) {
    try {
        auto impl = std::make_unique<Impl>();
        impl->windows = options.windows ? *options.windows : running_windows_version();
        if (!options.launcher) {
            options.launcher = &launch_installer_process;
        }
        if (options.channel.configured() && !options.http) {
            Expected<std::unique_ptr<HttpClient>> http = create_winhttp_client();
            if (!http) {
                return Unexpected(std::move(http.error()));
            }
            options.http = std::shared_ptr<HttpClient>(std::move(*http));
        }
        impl->options = std::move(options);
        return std::unique_ptr<UpdateService>(new UpdateService(std::move(impl)));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot create the update service", "app.update");
    }
}

UpdateService::UpdateService(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
UpdateService::~UpdateService() = default;

bool UpdateService::configured() const noexcept {
    return impl_->options.channel.configured();
}

const UpdateChannel& UpdateService::channel() const noexcept {
    return impl_->options.channel;
}

const core::SemanticVersion& UpdateService::current_version() const noexcept {
    return impl_->options.current_version;
}

Expected<UpdateCheck> UpdateService::check(core::CancellationToken cancel) const {
    const UpdateServiceOptions& options = impl_->options;
    if (!options.channel.configured()) {
        return fail(ErrorCode::Unsupported, "updates are not configured in this build", std::string(kWhereNotConfigured));
    }
    core::log_info(kLog, "checking channel '{}' for updates ({})", options.channel.name, options.channel.manifest_url);
    Expected<std::string> manifest = http_get_text(*options.http, options.channel.manifest_url, kMaxReleaseManifestBytes, cancel);
    if (!manifest) {
        core::log_warn(kLog, "the release manifest could not be fetched: {}", manifest.error().describe());
        return Unexpected(std::move(manifest.error()));
    }
    Expected<std::string> signature =
        http_get_text(*options.http, options.channel.signature_url(), kMaxReleaseSignatureBytes, cancel);
    if (!signature) {
        core::log_warn(kLog, "the release manifest's signature could not be fetched: {}", signature.error().describe());
        return Unexpected(std::move(signature.error()));
    }
    Expected<VerifiedReleaseManifest> verified = verify_release_manifest(*manifest, *signature, options.channel.keys);
    if (!verified) {
        core::log_warn(kLog, "the release manifest was refused: {}", verified.error().describe());
        return Unexpected(std::move(verified.error()));
    }
    if (verified->manifest.channel != options.channel.name) {
        Error error{ErrorCode::PermissionDenied,
                    std::format("the release manifest is for the channel '{}', not '{}'", verified->manifest.channel,
                                options.channel.name),
                    std::string(kWhereChannel), 0};
        core::log_warn(kLog, "{}", error.describe());
        return Unexpected(std::move(error));
    }
    UpdateCheck check;
    check.current = options.current_version;
    check.windows = impl_->windows;
    if (verified->manifest.version <= options.current_version) {
        check.availability = UpdateAvailability::UpToDate;
    } else if (impl_->windows < verified->manifest.minimum_os) {
        check.availability = UpdateAvailability::NeedsNewerWindows;
    } else {
        check.availability = UpdateAvailability::Available;
    }
    check.release = std::move(*verified);
    core::log_info(kLog, "channel '{}' offers Lucent {} (published {}, signed by key {}); this is {}: {}",
                   options.channel.name, check.release.manifest.version.to_string(), check.release.manifest.published,
                   check.release.key_id.substr(0, 16), options.current_version.to_string(), to_string(check.availability));
    return check;
}

Expected<DownloadedInstaller> UpdateService::download(
    const UpdateCheck& check, core::CancellationToken cancel,
    const std::function<void(const UpdateDownloadProgress&)>& progress) const {
    const UpdateServiceOptions& options = impl_->options;
    if (!options.channel.configured()) {
        return fail(ErrorCode::Unsupported, "updates are not configured in this build", std::string(kWhereNotConfigured));
    }
    if (check.availability != UpdateAvailability::Available) {
        return fail(ErrorCode::InvalidArgument, "no update can be installed from this check", std::string(kWhereDownload));
    }
    if (options.download_directory.empty()) {
        return fail(ErrorCode::InvalidArgument, "no download folder is configured", std::string(kWhereDownload));
    }
    try {
        const ReleaseManifest& release = check.release.manifest;
        if (Status created = core::create_directory_tree(options.download_directory); !created) {
            return Unexpected(std::move(created.error()));
        }
        const std::filesystem::path final_path = options.download_directory / installer_file_name(release.version);
        impl_->remove_earlier_downloads(final_path);

        DownloadedInstaller installer;
        installer.version_ = release.version;
        installer.size_ = release.installer.size;
        installer.sha256_ = release.installer.sha256;

        // A complete earlier download of this release is checked like a new one and reused.
        std::error_code exists_error;
        if (std::filesystem::exists(final_path, exists_error)) {
            Expected<FileHandle> held = open_held(final_path);
            if (held) {
                Status verified = impl_->verify_held(held->get(), final_path, release, cancel, installer.authenticode_);
                if (verified) {
                    core::log_info(kLog, "reusing the verified download {}", core::path_to_utf8_lossy(final_path));
                    if (progress) {
                        progress(UpdateDownloadProgress{release.installer.size, release.installer.size, true});
                    }
                    installer.path_ = final_path;
                    installer.handle_ = held->release();
                    return installer;
                }
                if (verified.error().code == ErrorCode::Cancelled) {
                    return Unexpected(std::move(verified.error()));
                }
                core::log_info(kLog, "an earlier download is not usable ({}); fetching it again",
                               verified.error().describe());
                held->reset();
                remove_refused(final_path);
            } else {
                core::log_warn(kLog, "an earlier download cannot be opened ({})", held.error().describe());
            }
        }

        const std::filesystem::path temporary =
            options.download_directory / (std::wstring(kTemporaryPrefix) + random_suffix() + std::wstring(kTemporarySuffix));
        {
            FileHandle out(::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                         FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!out) {
                return Unexpected(file_error("cannot create", temporary, ::GetLastError()));
            }
            std::uint64_t received = 0;
            Expected<HttpResult> fetched = options.http->get(
                release.installer.url, release.installer.size,
                [&](std::span<const std::byte> chunk) -> Status {
                    DWORD written = 0;
                    if (!::WriteFile(out.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &written, nullptr) ||
                        written != chunk.size()) {
                        return Unexpected(file_error("cannot write", temporary, ::GetLastError()));
                    }
                    received += chunk.size();
                    if (progress) {
                        progress(UpdateDownloadProgress{received, release.installer.size, false});
                    }
                    return ok();
                },
                cancel);
            Status flushed = ok();
            if (fetched && !::FlushFileBuffers(out.get())) {
                flushed = Unexpected(file_error("cannot flush", temporary, ::GetLastError()));
            }
            out.reset();
            if (!fetched || !flushed) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                Error error = !fetched ? std::move(fetched.error()) : std::move(flushed.error());
                core::log_warn(kLog, "the installer download failed: {}", error.describe());
                return Unexpected(std::move(error));
            }
            if (fetched->bytes != release.installer.size) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return fail(ErrorCode::Corrupt,
                            std::format("the server sent {} bytes; the release manifest names {}", fetched->bytes,
                                        release.installer.size),
                            std::string(kWhereDownload));
            }
        }

        // The complete download gets its final name, then is verified through a handle that excludes
        // writers, renamers and deleters: what is checked is what is launched.
        if (!::MoveFileExW(temporary.c_str(), final_path.c_str(), MOVEFILE_WRITE_THROUGH)) {
            const DWORD code = ::GetLastError();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return Unexpected(file_error("cannot rename the download to", final_path, code));
        }
        Expected<FileHandle> held = open_held(final_path);
        if (!held) {
            remove_refused(final_path);
            return Unexpected(std::move(held.error()));
        }
        if (Status verified = impl_->verify_held(held->get(), final_path, release, cancel, installer.authenticode_);
            !verified) {
            held->reset();
            remove_refused(final_path);
            core::log_warn(kLog, "the downloaded installer was refused: {}", verified.error().describe());
            return Unexpected(std::move(verified.error()));
        }
        core::log_info(kLog, "downloaded and verified Lucent {} ({} bytes, SHA-256 {}{}) to {}",
                       release.version.to_string(), release.installer.size, core::to_hex(release.installer.sha256),
                       installer.authenticode_ && installer.authenticode_->signer
                           ? ", signed by " + installer.authenticode_->signer->subject
                           : std::string(),
                       core::path_to_utf8_lossy(final_path));
        installer.path_ = final_path;
        installer.handle_ = held->release();
        return installer;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while downloading the update", std::string(kWhereDownload));
    }
}

std::optional<Error> UpdateService::install_blocker() const {
    if (impl_->options.install_blocker) {
        return impl_->options.install_blocker();
    }
    return std::nullopt;
}

Expected<InstallerLaunch> UpdateService::install(DownloadedInstaller installer) const {
    const UpdateServiceOptions& options = impl_->options;
    if (!installer.is_open()) {
        return fail(ErrorCode::InvalidArgument, "the installer is not held open", std::string(kWhereLaunch));
    }
    if (std::optional<Error> blocker = install_blocker()) {
        core::log_info(kLog, "installation postponed: {}", blocker->describe());
        return Unexpected(std::move(*blocker));
    }
    // Once more, through the held handle, right before it is used.
    Expected<std::pair<std::uint64_t, core::Digest256>> hashed =
        hash_handle(static_cast<HANDLE>(installer.handle_), core::CancellationToken{}, installer.path_);
    if (!hashed) {
        return Unexpected(std::move(hashed.error()));
    }
    if (hashed->first != installer.size_ || hashed->second != installer.sha256_) {
        return fail(ErrorCode::Corrupt, "the downloaded installer changed after it was verified",
                    std::string(kWhereDownload));
    }
    if (options.before_install) {
        if (Status prepared = options.before_install(); !prepared) {
            Error error = std::move(prepared.error());
            error.message = "the installation was not started: " + error.message;
            error.where = std::string(kWherePrepare);
            core::log_error(kLog, "{}", error.describe());
            return Unexpected(std::move(error));
        }
    }
    InstallerLaunch launch;
    launch.installer = installer.path_;
    std::wstring command_line = quoted(installer.path_);
    if (!options.log_directory.empty()) {
        if (Status created = core::create_directory_tree(options.log_directory); created) {
            launch.log = options.log_directory /
                         (L"setup-" + core::utf8_to_wide_lossy(installer.version_.to_string()) + L".log");
            command_line += L" /LOG=" + quoted(launch.log);
        }
    }
    Expected<std::uint32_t> started = options.launcher(installer.path_, command_line);
    installer.release();
    if (!started) {
        core::log_error(kLog, "Setup could not be started: {}", started.error().describe());
        return Unexpected(std::move(started.error()));
    }
    launch.process_id = *started;
    core::log_info(kLog, "started Setup for Lucent {} (process {}): {}", installer.version_.to_string(),
                   launch.process_id, core::path_to_utf8_lossy(installer.path_));
    return launch;
}

Expected<std::uint32_t> launch_installer_process(const std::filesystem::path& installer,
                                                 const std::wstring& command_line) {
    std::wstring mutable_command = command_line;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::filesystem::path folder = installer.parent_path();
    if (!::CreateProcessW(installer.c_str(), mutable_command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                          folder.empty() ? nullptr : folder.c_str(), &startup, &process)) {
        return Unexpected(core::win32_error(::GetLastError(), std::string(kWhereLaunch),
                                            "cannot start " + core::path_to_utf8_lossy(installer)));
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return static_cast<std::uint32_t>(process.dwProcessId);
}

}  // namespace lucent::app
