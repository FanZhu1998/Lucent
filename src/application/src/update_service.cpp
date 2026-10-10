#include "lucent/application/update_service.hpp"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <format>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <lucent/core/log.hpp>
#include <lucent/core/storage.hpp>
#include <lucent/core/text.hpp>
#include <lucent/core/win32_error.hpp>

#include "win32_paths.hpp"

namespace lucent::app {

namespace {

constexpr std::string_view kLog = "app.update";
constexpr std::string_view kWhereNotConfigured = "app.update.not_configured";
constexpr std::string_view kWhereChannel = "app.update.manifest.channel";
constexpr std::string_view kWhereDownload = "app.update.download";
constexpr std::string_view kWhereAuthenticode = "app.update.authenticode";
constexpr std::string_view kWherePrepare = "app.update.prepare";
constexpr std::string_view kWhereLaunch = "app.update.launch";
constexpr std::string_view kWhereFolder = "app.update.folder";
constexpr std::string_view kWhereVersion = "app.update.version";

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

Error folder_error(std::string_view what, const std::filesystem::path& path, DWORD code) {
    return core::win32_error(code, std::string(kWhereFolder),
                             std::format("{} {}", what, core::path_to_utf8_lossy(path)));
}

struct LocalFreeDeleter {
    void operator()(void* memory) const noexcept { ::LocalFree(memory); }
};
template <class T>
using LocalMemory = std::unique_ptr<std::remove_pointer_t<T>, LocalFreeDeleter>;

// The SID of the calling process's user (TokenUser) or of its token's default owner of new objects
// (TokenOwner: Administrators for an elevated administrator under some policies), copied.
Expected<std::vector<std::byte>> token_sid(HANDLE token, TOKEN_INFORMATION_CLASS which) {
    DWORD size = 0;
    ::GetTokenInformation(token, which, nullptr, 0, &size);
    if (size == 0) {
        return Unexpected(
            core::win32_error(::GetLastError(), std::string(kWhereFolder), "cannot read the process token"));
    }
    std::vector<std::byte> information(size);
    if (!::GetTokenInformation(token, which, information.data(), size, &size)) {
        return Unexpected(
            core::win32_error(::GetLastError(), std::string(kWhereFolder), "cannot read the process token"));
    }
    const PSID sid = which == TokenUser ? reinterpret_cast<TOKEN_USER*>(information.data())->User.Sid
                                        : reinterpret_cast<TOKEN_OWNER*>(information.data())->Owner;
    const DWORD length = ::GetLengthSid(sid);
    std::vector<std::byte> copy(length);
    if (!::CopySid(length, copy.data(), sid)) {
        return Unexpected(core::win32_error(::GetLastError(), std::string(kWhereFolder), "cannot copy a SID"));
    }
    return copy;
}

struct ProcessIdentity {
    std::vector<std::byte> user;
    std::vector<std::byte> default_owner;
};

Expected<ProcessIdentity> process_identity() {
    HANDLE raw = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw)) {
        return Unexpected(
            core::win32_error(::GetLastError(), std::string(kWhereFolder), "cannot open the process token"));
    }
    const FileHandle token(raw);  // closes the token handle
    Expected<std::vector<std::byte>> user = token_sid(raw, TokenUser);
    if (!user) {
        return Unexpected(std::move(user.error()));
    }
    Expected<std::vector<std::byte>> owner = token_sid(raw, TokenOwner);
    if (!owner) {
        return Unexpected(std::move(owner.error()));
    }
    return ProcessIdentity{std::move(*user), std::move(*owner)};
}

// True when `handle` (READ_CONTROL) is owned by this user, its token's default owner,
// Administrators or SYSTEM: by nobody another standard user could be.
Expected<bool> owned_by_us(HANDLE handle, const ProcessIdentity& identity, const std::filesystem::path& path) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR raw = nullptr;
    const DWORD code =
        ::GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &raw);
    if (code != ERROR_SUCCESS) {
        return Unexpected(folder_error("cannot read the owner of", path, code));
    }
    const LocalMemory<PSECURITY_DESCRIPTOR> descriptor(raw);
    if (owner == nullptr || !::IsValidSid(owner)) {
        return false;
    }
    std::vector<std::byte> user = identity.user;
    std::vector<std::byte> default_owner = identity.default_owner;
    return ::EqualSid(owner, user.data()) != FALSE || ::EqualSid(owner, default_owner.data()) != FALSE ||
           ::IsWellKnownSid(owner, WinBuiltinAdministratorsSid) != FALSE ||
           ::IsWellKnownSid(owner, WinLocalSystemSid) != FALSE;
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

// The download folder, prepared to download, verify and launch installers in (SEC-01, W12): created
// when missing and opened without following a link, so a folder that is itself a junction or a
// symbolic link is refused; owned by this user (owned_by_us()), so one another user created
// beforehand is refused; given a protected DACL (full control for this user, SYSTEM and
// Administrators only, inherited by everything in it), so no other user can add, replace, rename or
// delete anything in it; and resolved to its final path (every link above it resolved), the path
// every later step uses. Errors: PermissionDenied ("app.update.folder") for a link, a folder of
// another user or one whose permissions cannot be set (a volume without them: FAT, exFAT); the
// errors of creating and opening it.
struct DownloadFolder {
    FileHandle handle;           // held while the download is prepared (no FILE_SHARE_DELETE)
    std::filesystem::path path;  // its final path
    ProcessIdentity identity;
};

Expected<DownloadFolder> prepare_download_folder(const std::filesystem::path& folder) {
    if (Status created = core::create_directory_tree(folder); !created) {
        return Unexpected(std::move(created.error()));
    }
    DownloadFolder out;
    out.handle = FileHandle(::CreateFileW(folder.c_str(), READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!out.handle) {
        return Unexpected(folder_error("cannot open the update folder", folder, ::GetLastError()));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (!::GetFileInformationByHandle(out.handle.get(), &information)) {
        return Unexpected(folder_error("cannot inspect the update folder", folder, ::GetLastError()));
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return fail(ErrorCode::PermissionDenied,
                    std::format("the update folder {} is a link (a junction or symbolic link), not a folder; "
                                "installers are downloaded only into a real folder",
                                core::path_to_utf8_lossy(folder)),
                    std::string(kWhereFolder));
    }
    Expected<ProcessIdentity> identity = process_identity();
    if (!identity) {
        return Unexpected(std::move(identity.error()));
    }
    Expected<bool> ours = owned_by_us(out.handle.get(), *identity, folder);
    if (!ours) {
        return Unexpected(std::move(ours.error()));
    }
    if (!*ours) {
        return fail(ErrorCode::PermissionDenied,
                    std::format("the update folder {} belongs to another user; installers are downloaded only into "
                                "a folder of this user",
                                core::path_to_utf8_lossy(folder)),
                    std::string(kWhereFolder));
    }
    LPWSTR user_text = nullptr;
    if (!::ConvertSidToStringSidW(identity->user.data(), &user_text)) {
        return Unexpected(folder_error("cannot name this user for", folder, ::GetLastError()));
    }
    const LocalMemory<LPWSTR> user_name(user_text);
    const std::wstring sddl =
        std::format(L"D:P(A;OICI;FA;;;{})(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", std::wstring_view(user_text));
    PSECURITY_DESCRIPTOR raw = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &raw, nullptr)) {
        return Unexpected(folder_error("cannot build the permissions of", folder, ::GetLastError()));
    }
    const LocalMemory<PSECURITY_DESCRIPTOR> descriptor(raw);
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    if (!::GetSecurityDescriptorDacl(raw, &present, &dacl, &defaulted) || !present) {
        return Unexpected(folder_error("cannot build the permissions of", folder, ::GetLastError()));
    }
    // Also replaces the inherited entries of what the folder already holds.
    if (const DWORD code = ::SetSecurityInfo(out.handle.get(), SE_FILE_OBJECT,
                                             DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                                             nullptr, dacl, nullptr);
        code != ERROR_SUCCESS) {
        Error error = folder_error("cannot restrict the permissions of the update folder", folder, code);
        error.code = ErrorCode::PermissionDenied;
        return Unexpected(std::move(error));
    }
    Expected<std::filesystem::path> resolved = detail::final_path_of(out.handle.get(), folder, kWhereFolder);
    if (!resolved) {
        return Unexpected(std::move(resolved.error()));
    }
    out.path = std::move(*resolved);
    out.identity = std::move(*identity);
    return out;
}

// Checks that the file held through `file` is the one named `expected` in the prepared folder: its
// final path is `expected` (no link led elsewhere), it has no other name (one link) and it is ours
// (owned_by_us()). PermissionDenied ("app.update.folder") otherwise.
Status check_held_location(HANDLE file, const std::filesystem::path& expected, const ProcessIdentity& identity) {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!::GetFileInformationByHandle(file, &information)) {
        return Unexpected(folder_error("cannot inspect", expected, ::GetLastError()));
    }
    Expected<std::filesystem::path> resolved = detail::final_path_of(file, expected, kWhereFolder);
    if (!resolved) {
        return Unexpected(std::move(resolved.error()));
    }
    Expected<bool> ours = owned_by_us(file, identity, expected);
    if (!ours) {
        return Unexpected(std::move(ours.error()));
    }
    if (!detail::same_path_text(*resolved, expected) || information.nNumberOfLinks != 1 ||
        (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 || !*ours) {
        return fail(ErrorCode::PermissionDenied,
                    std::format("the downloaded installer is not this user's own file at {} (found at {}, {} "
                                "name(s){})",
                                core::path_to_utf8_lossy(expected), core::path_to_utf8_lossy(*resolved),
                                information.nNumberOfLinks, *ours ? "" : ", owned by another user"),
                    std::string(kWhereFolder));
    }
    return ok();
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
    // Installers whose Setup was started: held until the service is destroyed (Lucent exits right
    // after), so the file Setup's loader and Setup read again by name stays the verified one.
    std::mutex launched_mutex;
    std::vector<DownloadedInstaller> launched;

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

    // Removes earlier downloads from `folder`: temporary files, and installers of other releases
    // (`keep` is left for the caller to check). Failures are logged, not returned.
    void remove_earlier_downloads(const std::filesystem::path& folder, const std::filesystem::path& keep) const {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
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
        if (options.channel.configured() && !options.current_version) {
            // Without its own version a build would take every release of its channel, older ones
            // too, for an update (SEC-05, W12).
            return fail(ErrorCode::InvalidArgument,
                        "the running version is unknown, so the channel's releases cannot be compared with it",
                        std::string(kWhereVersion));
        }
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

const std::optional<core::SemanticVersion>& UpdateService::current_version() const noexcept {
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
    check.current = *options.current_version;  // create() requires it of a configured channel
    check.windows = impl_->windows;
    if (verified->manifest.version <= check.current) {
        check.availability = UpdateAvailability::UpToDate;
    } else if (impl_->windows < verified->manifest.minimum_os) {
        check.availability = UpdateAvailability::NeedsNewerWindows;
    } else {
        check.availability = UpdateAvailability::Available;
    }
    check.release = std::move(*verified);
    core::log_info(kLog, "channel '{}' offers Lucent {} (published {}, signed by key {}); this is {}: {}",
                   options.channel.name, check.release.manifest.version.to_string(), check.release.manifest.published,
                   check.release.key_id.substr(0, 16), check.current.to_string(), to_string(check.availability));
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
        Expected<DownloadFolder> folder = prepare_download_folder(options.download_directory);
        if (!folder) {
            core::log_warn(kLog, "the update folder cannot be used: {}", folder.error().describe());
            return Unexpected(std::move(folder.error()));
        }
        const std::filesystem::path final_path = folder->path / installer_file_name(release.version);
        impl_->remove_earlier_downloads(folder->path, final_path);

        DownloadedInstaller installer;
        installer.version_ = release.version;
        installer.size_ = release.installer.size;
        installer.sha256_ = release.installer.sha256;

        // A complete earlier download of this release is checked like a new one and reused.
        std::error_code exists_error;
        if (std::filesystem::exists(final_path, exists_error)) {
            Expected<FileHandle> held = open_held(final_path);
            if (held) {
                Status verified = check_held_location(held->get(), final_path, folder->identity);
                if (verified) {
                    verified = impl_->verify_held(held->get(), final_path, release, cancel, installer.authenticode_);
                }
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
            folder->path / (std::wstring(kTemporaryPrefix) + random_suffix() + std::wstring(kTemporarySuffix));
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
        // writers, renamers and deleters, at the final path of a folder only this user can change: the
        // file that is checked is the one install() launches.
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
        Status verified = check_held_location(held->get(), final_path, folder->identity);
        if (verified) {
            verified = impl_->verify_held(held->get(), final_path, release, cancel, installer.authenticode_);
        }
        if (!verified) {
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
    // The held file is still at the path that will be started: a final path (no link in it) whose
    // folders cannot be renamed while the file is held open.
    Expected<std::filesystem::path> resolved = detail::final_path_of(installer.handle_, installer.path_, kWhereLaunch);
    if (!resolved) {
        return Unexpected(std::move(resolved.error()));
    }
    if (!detail::same_path_text(*resolved, installer.path_)) {
        return fail(ErrorCode::PermissionDenied,
                    std::format("the verified installer is at {}, not at {} where it would be started",
                                core::path_to_utf8_lossy(*resolved), core::path_to_utf8_lossy(installer.path_)),
                    std::string(kWhereLaunch));
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
    if (!started) {
        installer.release();
        core::log_error(kLog, "Setup could not be started: {}", started.error().describe());
        return Unexpected(std::move(started.error()));
    }
    launch.process_id = *started;
    core::log_info(kLog, "started Setup for Lucent {} (process {}): {}", installer.version_.to_string(),
                   launch.process_id, core::path_to_utf8_lossy(installer.path_));
    {
        std::lock_guard lock(impl_->launched_mutex);
        impl_->launched.push_back(std::move(installer));
    }
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
