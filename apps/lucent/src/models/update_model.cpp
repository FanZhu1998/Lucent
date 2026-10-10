#include "update_model.hpp"

#include <QCoreApplication>
#include <QLocale>

namespace lucent::ui {

namespace {

QString tr_update(const char* text) {
    return QCoreApplication::translate("lucent::ui::UpdateModel", text);
}

QString size_text(qint64 bytes) {
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeSIFormat);
}

}  // namespace

UpdateModel::UpdateModel(UpdateService& service, QObject* parent)
    : QObject(parent), service_(service), channel_(service.channel()) {
    state_ = channel_.configured ? Idle : NotConfigured;
    connect(&service_, &UpdateService::checked, this, &UpdateModel::on_checked);
    connect(&service_, &UpdateService::download_progress, this, &UpdateModel::on_progress);
    connect(&service_, &UpdateService::downloaded, this, &UpdateModel::on_downloaded);
    connect(&service_, &UpdateService::install_finished, this, &UpdateModel::on_installed);
    // While an update waits to be installed, whether it may start follows the batches and Auto.
    blocker_timer_.setInterval(1000);
    connect(&blocker_timer_, &QTimer::timeout, this, &UpdateModel::refresh);
}

QString UpdateModel::channel_text() const {
    if (!channel_.configured) {
        return tr_update("Updates are not configured in this build: it never contacts an update server. New "
                         "versions of Lucent come as a complete installer from its publisher; installing one keeps "
                         "your library, edits and settings.");
    }
    QString text = tr_update("Checks the “%1” channel at %2 when you ask, and only then. Updates are "
                             "verified against the publisher's signature before they are installed.")
                       .arg(channel_.channel, channel_.server);
    return text;
}

QString UpdateModel::status_text() const {
    switch (state_) {
        case NotConfigured:
            return {};
        case Idle:
            return tr_update("This is Lucent %1.").arg(channel_.current_version);
        case Checking:
            return tr_update("Checking for updates…");
        case UpToDate:
            return tr_update("Lucent %1 is up to date.").arg(channel_.current_version);
        case Available:
            return tr_update("Lucent %1 is available (published %2, %3). You have %4.")
                .arg(offered_.version, offered_.published, size_text(offered_.size_bytes), channel_.current_version);
        case NeedsNewerWindows:
            return tr_update("Lucent %1 is available but needs Windows %2 or later, so it cannot be installed here.")
                .arg(offered_.version, offered_.minimum_windows);
        case Downloading:
            if (total_ > 0) {
                return tr_update("Downloading Lucent %1… %2 of %3")
                    .arg(offered_.version, size_text(received_), size_text(total_));
            }
            return tr_update("Downloading Lucent %1…").arg(offered_.version);
        case Ready: {
            QString text = signer_.isEmpty()
                               ? tr_update("Lucent %1 is downloaded and verified.").arg(offered_.version)
                               : tr_update("Lucent %1 is downloaded and verified (signed by %2).")
                                     .arg(offered_.version, signer_);
            text += QLatin1Char(' ');
            text += install_blocker_.isEmpty()
                        ? tr_update("Installing closes Lucent, backs up your library and starts Setup.")
                        : install_blocker_;
            return text;
        }
        case Installing:
            return tr_update("Backing up the library and starting Setup…");
        case Failed:
            return error_;
    }
    return {};
}

double UpdateModel::progress() const noexcept {
    if (state_ != Downloading || total_ <= 0) {
        return -1.0;
    }
    return static_cast<double>(received_) / static_cast<double>(total_);
}

bool UpdateModel::can_check() const noexcept {
    return channel_.configured && !working() && state_ != Ready;
}

void UpdateModel::set_state(State state) {
    state_ = state;
    if (state_ == Ready) {
        install_blocker_ = service_.install_blocker();
        blocker_timer_.start();
    } else {
        blocker_timer_.stop();
    }
    emit changed();
}

void UpdateModel::check() {
    if (!can_check() || service_.busy()) {
        return;
    }
    error_.clear();
    set_state(Checking);
    service_.check();
}

void UpdateModel::download() {
    if (!can_download() || service_.busy()) {
        return;
    }
    received_ = 0;
    total_ = offered_.size_bytes;
    error_.clear();
    set_state(Downloading);
    service_.download();
}

void UpdateModel::cancel() {
    if (can_cancel()) {
        service_.cancel();
    }
}

void UpdateModel::install() {
    if (state_ != Ready || service_.busy()) {
        return;
    }
    install_blocker_ = service_.install_blocker();
    if (!install_blocker_.isEmpty()) {
        emit changed();
        return;
    }
    set_state(Installing);
    service_.install();
}

void UpdateModel::refresh() {
    if (state_ != Ready) {
        return;
    }
    const QString blocker = service_.install_blocker();
    if (blocker != install_blocker_) {
        install_blocker_ = blocker;
        emit changed();
    }
}

void UpdateModel::on_checked(const UpdateCheckInfo& info) {
    if (!info.ok) {
        error_ = info.error;
        set_state(info.cancelled ? (has_offer_ ? Available : Idle) : Failed);
        return;
    }
    offered_ = info;
    has_offer_ = info.availability == UpdateAvailability::Available;
    switch (info.availability) {
        case UpdateAvailability::UpToDate:
            set_state(UpToDate);
            break;
        case UpdateAvailability::Available:
            set_state(Available);
            break;
        case UpdateAvailability::NeedsNewerWindows:
            set_state(NeedsNewerWindows);
            break;
    }
}

void UpdateModel::on_progress(qint64 received, qint64 total) {
    if (state_ != Downloading) {
        return;
    }
    received_ = received;
    total_ = total;
    emit changed();
}

void UpdateModel::on_downloaded(const UpdateDownloadInfo& info) {
    if (!info.ok) {
        error_ = info.error;
        set_state(info.cancelled ? Available : Failed);
        return;
    }
    signer_ = info.signer;
    set_state(Ready);
}

void UpdateModel::on_installed(const UpdateInstallInfo& info) {
    if (!info.ok) {
        // Refused before Setup started (a job started meanwhile, the backup failed). A download that
        // was not handed over waits, still held and verified; otherwise it can be fetched again (an
        // earlier complete download is verified and reused).
        error_ = info.error;
        set_state(info.kept ? Ready : Failed);
        return;
    }
    emit quitRequested();
}

}  // namespace lucent::ui
