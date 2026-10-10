#pragma once

// UpdateModel: "Check for updates" in Help > About (blueprint section 8: "a signed full installer and a
// graphical Check for updates flow"; ADR 0022). AppController owns it (`controller.updates`).
//
// States, in the order the flow takes them:
//   NotConfigured      this build has no update channel: nothing is ever fetched, and the panel says
//                      that new versions come as a complete installer from the publisher
//   Idle               configured; nothing has been checked in this session (no check runs by
//                      itself: Lucent contacts the server only when asked)
//   Checking           check() is fetching and authenticating the release manifest (cancel() stops)
//   UpToDate           the channel offers nothing newer than this build
//   Available          a newer release (version, publication date, size, release notes as plain text)
//   NeedsNewerWindows  a newer release that needs a newer Windows than this one: it is not offered
//   Downloading        download() is fetching the installer (progress; cancel() stops)
//   Ready              the installer is downloaded and verified; install() closes Lucent and starts
//                      Setup, but only between jobs: while a batch or Auto runs, `installBlocker`
//                      says so and `canInstall` is false (re-checked every second while Ready)
//   Installing         the library is being backed up and Setup started; on success quitRequested()
//                      asks the application to exit cleanly so Setup can replace it
//   Failed             the last step failed: `statusText` says why (Available stays downloadable
//                      again, a check can be repeated)
//
// QML-facing members use QML naming (lowerCamelCase); C++-only members use snake_case.
//
// Thread-safety: UI thread only.

#include <QObject>
#include <QString>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>

#include "services/update_service.hpp"

namespace lucent::ui {

class UpdateModel : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("UpdateModel is provided by AppController.updates.")
    Q_PROPERTY(bool configured READ configured CONSTANT FINAL)
    // "Checks the “stable” channel at updates.example.org when you ask, and only then."; for a build
    // without a channel, why there are no updates and where new versions come from.
    Q_PROPERTY(QString channelText READ channel_text CONSTANT FINAL)
    Q_PROPERTY(State state READ state NOTIFY changed FINAL)
    Q_PROPERTY(QString statusText READ status_text NOTIFY changed FINAL)
    Q_PROPERTY(bool working READ working NOTIFY changed FINAL)
    // The release the channel offers (Available, NeedsNewerWindows, Downloading, Ready); empty otherwise.
    Q_PROPERTY(QString version READ version NOTIFY changed FINAL)
    Q_PROPERTY(QString releaseNotes READ release_notes NOTIFY changed FINAL)
    // Downloading: 0 ... 1; -1 when unknown.
    Q_PROPERTY(double progress READ progress NOTIFY changed FINAL)
    Q_PROPERTY(bool canCheck READ can_check NOTIFY changed FINAL)
    Q_PROPERTY(bool canDownload READ can_download NOTIFY changed FINAL)
    Q_PROPERTY(bool canInstall READ can_install NOTIFY changed FINAL)
    Q_PROPERTY(bool canCancel READ can_cancel NOTIFY changed FINAL)
    // Why the downloaded update cannot be installed now (a running export or Auto); empty otherwise.
    Q_PROPERTY(QString installBlocker READ install_blocker NOTIFY changed FINAL)

public:
    enum State {
        NotConfigured = 0,
        Idle = 1,
        Checking = 2,
        UpToDate = 3,
        Available = 4,
        NeedsNewerWindows = 5,
        Downloading = 6,
        Ready = 7,
        Installing = 8,
        Failed = 9,
    };
    Q_ENUM(State)

    explicit UpdateModel(UpdateService& service, QObject* parent = nullptr);

    Q_INVOKABLE void check();
    Q_INVOKABLE void download();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void install();
    // Re-reads whether an installation may start now (the panel calls it when it opens).
    Q_INVOKABLE void refresh();

    [[nodiscard]] bool configured() const { return channel_.configured; }
    [[nodiscard]] QString channel_text() const;
    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] QString status_text() const;
    [[nodiscard]] bool working() const noexcept {
        return state_ == Checking || state_ == Downloading || state_ == Installing;
    }
    [[nodiscard]] QString version() const { return offered_.version; }
    [[nodiscard]] QString release_notes() const { return offered_.release_notes; }
    [[nodiscard]] double progress() const noexcept;
    [[nodiscard]] bool can_check() const noexcept;
    [[nodiscard]] bool can_download() const noexcept { return state_ == Available || (state_ == Failed && has_offer_); }
    [[nodiscard]] bool can_install() const noexcept { return state_ == Ready && install_blocker_.isEmpty(); }
    [[nodiscard]] bool can_cancel() const noexcept { return state_ == Checking || state_ == Downloading; }
    [[nodiscard]] QString install_blocker() const { return state_ == Ready ? install_blocker_ : QString(); }

signals:
    void changed();
    // Setup is running: the application should exit cleanly now.
    void quitRequested();

private:
    void set_state(State state);
    void on_checked(const UpdateCheckInfo& info);
    void on_progress(qint64 received, qint64 total);
    void on_downloaded(const UpdateDownloadInfo& info);
    void on_installed(const UpdateInstallInfo& info);

    UpdateService& service_;
    UpdateChannelInfo channel_;
    State state_ = NotConfigured;
    UpdateCheckInfo offered_;  // the last check's release
    bool has_offer_ = false;   // offered_ is an Available release
    QString error_;            // Failed
    QString signer_;           // Ready: the installer's Authenticode signer, when checked
    qint64 received_ = 0;
    qint64 total_ = 0;
    QString install_blocker_;
    QTimer blocker_timer_;
};

}  // namespace lucent::ui
