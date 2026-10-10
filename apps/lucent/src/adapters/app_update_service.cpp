#include "adapters/app_update_service.hpp"

#include <chrono>
#include <utility>

#include <QCoreApplication>
#include <QDir>
#include <QUrl>

#include <lucent/core/log.hpp>
#include <lucent/core/text.hpp>

namespace lucent::ui::adapters {

namespace {

constexpr std::string_view kLog = "ui.updates";

QString tr_update(const char* text) {
    return QCoreApplication::translate("lucent::ui::AppUpdateService", text);
}

QString native(const std::filesystem::path& path) {
    return QDir::toNativeSeparators(QString::fromStdWString(path.native()));
}

QString developer_text(const Error& error) {
    return QString::fromStdString(error.message);
}

}  // namespace

QString user_facing_update_error(const Error& error) {
    const std::string_view where = error.where;
    if (error.code == ErrorCode::Cancelled) {
        return tr_update("Cancelled.");
    }
    if (where == "app.update.not_configured") {
        return tr_update("Updates are not configured in this build.");
    }
    if (where == "app.update.network") {
        return error.code == ErrorCode::Timeout
                   ? tr_update("The update server did not answer in time. Try again later.")
                   : tr_update("Lucent could not reach the update server. Check the internet connection and try "
                               "again.");
    }
    if (where == "app.update.tls") {
        return tr_update("The update server's secure connection could not be verified, so nothing was "
                         "downloaded.");
    }
    if (where == "app.update.url") {
        return tr_update("The update address is not a secure (https) address, so nothing was downloaded.");
    }
    if (where == "app.update.http") {
        if (error.code == ErrorCode::NotFound) {
            return tr_update("The update server has no update information for this channel.");
        }
        if (error.code == ErrorCode::LimitExceeded || error.code == ErrorCode::Corrupt) {
            return tr_update("The update server sent something other than what was expected, so it was ignored.");
        }
        return tr_update("The update server answered with an error (%1). Try again later.").arg(error.native_code);
    }
    if (where.starts_with("app.update.signature") || where == "app.update.manifest.channel") {
        return tr_update("The update information is not signed by Lucent's publisher, so it was ignored.");
    }
    if (where == "app.update.manifest") {
        return error.code == ErrorCode::VersionMismatch
                   ? tr_update("The update information needs a newer Lucent. Download the new version from the "
                               "publisher's website.")
                   : tr_update("The update information could not be read, so it was ignored.");
    }
    if (where == "app.update.authenticode") {
        return tr_update("The downloaded installer is not signed by Lucent's publisher, so it was deleted.");
    }
    if (where == "app.update.download") {
        if (error.code == ErrorCode::Corrupt) {
            return tr_update("The downloaded installer is not the one the publisher signed, so it was deleted.");
        }
        if (error.code == ErrorCode::DiskFull) {
            return tr_update("There is not enough space on the disk to download the update.");
        }
        return tr_update("The update could not be downloaded: %1").arg(developer_text(error));
    }
    if (where == "app.update.busy") {
        return tr_update("Lucent installs updates between jobs: finish or pause the running export, or wait for "
                         "Auto, then install.");
    }
    if (where == "app.update.prepare") {
        return tr_update("The library could not be backed up before the update, so the update was not started: %1")
            .arg(developer_text(error));
    }
    if (where == "app.update.launch") {
        return tr_update("Setup could not be started: %1").arg(developer_text(error));
    }
    return tr_update("The update failed: %1").arg(developer_text(error));
}

AppUpdateService::AppUpdateService(std::shared_ptr<Backend> backend, QObject* parent)
    : UpdateService(parent), backend_(std::move(backend)), dispatcher_(std::make_shared<UiDispatcher>(this)) {}

AppUpdateService::~AppUpdateService() {
    dispatcher_->close();
    cancel_.cancel();
    if (worker_.joinable()) {
        worker_.join();
    }
}

UpdateChannelInfo AppUpdateService::channel() const {
    UpdateChannelInfo info;
    const app::UpdateService* updates = backend_->updates();
    if (updates == nullptr) {
        info.current_version = QString::fromUtf8(core::build_info().version.data(),
                                                 static_cast<qsizetype>(core::build_info().version.size()));
        return info;
    }
    info.current_version = QString::fromStdString(updates->current_version().to_string());
    if (updates->configured()) {
        info.configured = true;
        info.channel = QString::fromStdString(updates->channel().name);
        info.server = QUrl(QString::fromStdString(updates->channel().manifest_url)).host();
        info.checks_publisher = !updates->channel().publisher_thumbprints.empty();
    }
    return info;
}

void AppUpdateService::start(std::function<void(core::CancellationToken)> work) {
    if (worker_.joinable()) {
        worker_.join();  // finished: busy_ was cleared by its posted completion
    }
    cancel_ = core::CancellationSource();
    busy_ = true;
    worker_ = std::thread([work = std::move(work), token = cancel_.token()] { work(token); });
}

void AppUpdateService::check() {
    app::UpdateService* updates = backend_->updates();
    if (busy_ || updates == nullptr || !updates->configured()) {
        return;
    }
    start([this, updates, dispatcher = dispatcher_](core::CancellationToken token) {
        Expected<app::UpdateCheck> result = updates->check(token);
        dispatcher->post([this, result = std::move(result)]() mutable {
            busy_ = false;
            UpdateCheckInfo info;
            if (!result) {
                info.cancelled = result.error().code == ErrorCode::Cancelled;
                info.error = user_facing_update_error(result.error());
                last_check_.reset();
                emit checked(info);
                return;
            }
            const app::ReleaseManifest& release = result->release.manifest;
            info.ok = true;
            info.availability = static_cast<UpdateAvailability>(result->availability);
            info.version = QString::fromStdString(release.version.to_string());
            info.published = QString::fromStdString(release.published.substr(0, 10));
            info.release_notes = QString::fromStdString(release.release_notes);
            info.minimum_windows = QString::fromStdString(release.minimum_os.to_string());
            info.size_bytes = static_cast<qint64>(release.installer.size);
            last_check_ = std::move(*result);
            emit checked(info);
        });
    });
}

void AppUpdateService::download() {
    app::UpdateService* updates = backend_->updates();
    if (busy_ || updates == nullptr || !last_check_ ||
        last_check_->availability != app::UpdateAvailability::Available) {
        return;
    }
    installer_.reset();  // a new download replaces the held one
    start([this, updates, check = *last_check_, dispatcher = dispatcher_](core::CancellationToken token) {
        auto last_post = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        Expected<app::DownloadedInstaller> result =
            updates->download(check, token, [&](const app::UpdateDownloadProgress& progress) {
                const auto now = std::chrono::steady_clock::now();
                if (now - last_post < std::chrono::milliseconds(100) && progress.received < progress.total) {
                    return;
                }
                last_post = now;
                dispatcher->post([this, received = static_cast<qint64>(progress.received),
                                  total = static_cast<qint64>(progress.total)] { emit download_progress(received, total); });
            });
        std::shared_ptr<app::DownloadedInstaller> held;
        std::optional<Error> error;
        if (result) {
            held = std::make_shared<app::DownloadedInstaller>(std::move(*result));
        } else {
            error = std::move(result.error());
        }
        dispatcher->post([this, held = std::move(held), error = std::move(error)]() mutable {
            busy_ = false;
            UpdateDownloadInfo info;
            if (error) {
                info.cancelled = error->code == ErrorCode::Cancelled;
                info.error = user_facing_update_error(*error);
                emit downloaded(info);
                return;
            }
            info.ok = true;
            info.installer = native(held->path());
            if (held->authenticode() && held->authenticode()->signer) {
                info.signer = QString::fromStdString(held->authenticode()->signer->subject);
            }
            installer_ = std::move(held);
            emit downloaded(info);
        });
    });
}

void AppUpdateService::cancel() {
    if (busy_) {
        cancel_.cancel();
    }
}

QString AppUpdateService::install_blocker() const {
    const app::UpdateService* updates = backend_->updates();
    if (updates == nullptr) {
        return {};
    }
    const std::optional<Error> blocker = updates->install_blocker();
    return blocker ? user_facing_update_error(*blocker) : QString();
}

void AppUpdateService::install() {
    app::UpdateService* updates = backend_->updates();
    if (busy_ || updates == nullptr || !installer_ || !installer_->is_open()) {
        return;
    }
    // Checked here first so a refusal keeps the verified download (the service checks again).
    if (const QString blocker = install_blocker(); !blocker.isEmpty()) {
        UpdateInstallInfo info;
        info.error = blocker;
        info.kept = true;
        emit install_finished(info);
        return;
    }
    std::shared_ptr<app::DownloadedInstaller> installer = std::move(installer_);
    start([this, updates, installer, dispatcher = dispatcher_](core::CancellationToken) {
        Expected<app::InstallerLaunch> launched = updates->install(std::move(*installer));
        dispatcher->post([this, launched = std::move(launched)] {
            busy_ = false;
            UpdateInstallInfo info;
            if (launched) {
                info.ok = true;
                info.log = native(launched->log);
            } else {
                info.error = user_facing_update_error(launched.error());
            }
            emit install_finished(info);
        });
    });
}

}  // namespace lucent::ui::adapters
