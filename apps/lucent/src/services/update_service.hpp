#pragma once

// UpdateService: the shell's view of "Check for updates" (blueprint section 8: "a signed full installer
// and a graphical Check for updates flow"; ADR 0022). The application service behind it
// (app::UpdateService) authenticates the channel's release manifest with the key compiled into the
// build, downloads and verifies the installer, and starts it. Nothing here touches the network unless
// the photographer asks (check(), download()); a build whose channel is not configured never does.
//
// The flow: check() -> checked(); for an available release download() -> download_progress() ...
// downloaded(); then install() -> install_finished(): on success Setup is running and the shell quits
// so Setup can replace the program. install() refuses (install_finished with an error) while
// install_blocker() names a reason: an export batch or Auto is running (updates are applied between
// jobs). cancel() stops a check or download in progress (its signal then reports a cancellation).
//
// Threading contract: as LibraryService (library_service.hpp): the object lives on the UI thread,
// member functions return promptly (the work runs elsewhere), and signals are emitted on the UI
// thread. One operation runs at a time: a call while another runs is ignored (busy()).
//
// Strings are user-facing unless stated otherwise.

#include <utility>

#include <QMetaType>
#include <QObject>
#include <QString>

namespace lucent::ui {

struct UpdateChannelInfo {
    bool configured = false;  // false: "Updates are not configured in this build"
    QString channel;          // "stable"
    QString server;           // the host the manifest comes from ("updates.example.org")
    QString current_version;  // this build ("0.1.0")
    bool checks_publisher = false;  // the installer's Authenticode signature is checked too
    // Not user-facing: the channel compiled into this Lucent.exe as one line (app::
    // describe_update_channel(): "update channel 'stable' (<URL>): keys ...; publishers ...",
    // "update channel: none (...)" or "update channel refused: ..."), which the developer automation
    // reports for the packaging smoke run (W12 review SEC-02). Empty: unknown.
    QString description;
};

enum class UpdateAvailability : quint8 {
    UpToDate = 1,
    Available = 2,
    NeedsNewerWindows = 3,
};

struct UpdateCheckInfo {
    bool ok = false;
    bool cancelled = false;
    QString error;  // why the check failed (user-facing)
    UpdateAvailability availability = UpdateAvailability::UpToDate;
    QString version;          // the channel's release
    QString published;        // "2026-10-09"
    QString release_notes;    // plain text
    QString minimum_windows;  // "10.0.17763"
    qint64 size_bytes = 0;    // the installer
};

struct UpdateDownloadInfo {
    bool ok = false;
    bool cancelled = false;
    QString error;
    QString installer;  // the verified file (native separators)
    QString signer;     // the installer's Authenticode signer when the build checks it; empty otherwise
    bool reused = false;  // an earlier complete download was verified and kept
};

struct UpdateInstallInfo {
    bool ok = false;      // Setup is running: the shell quits
    QString error;
    QString log;          // Setup's log file (native separators); empty without one
    bool kept = false;    // refused before anything started: the verified download can be installed later
};

class UpdateService : public QObject {
    Q_OBJECT

public:
    explicit UpdateService(QObject* parent = nullptr) : QObject(parent) {}
    ~UpdateService() override = default;

    [[nodiscard]] virtual UpdateChannelInfo channel() const = 0;
    [[nodiscard]] virtual bool busy() const = 0;
    virtual void check() = 0;
    // Downloads the release of the last successful check (Available).
    virtual void download() = 0;
    virtual void cancel() = 0;
    // Why an installation must wait now; empty when it may start.
    [[nodiscard]] virtual QString install_blocker() const = 0;
    // Installs the downloaded release (see the file comment).
    virtual void install() = 0;

signals:
    void checked(const lucent::ui::UpdateCheckInfo& info);
    void download_progress(qint64 received, qint64 total);
    void downloaded(const lucent::ui::UpdateDownloadInfo& info);
    void install_finished(const lucent::ui::UpdateInstallInfo& info);
};

// The service of a build without an update channel (and of sessions that cannot run one): channel()
// says "not configured" and every request is ignored.
class UnavailableUpdateService final : public UpdateService {
    Q_OBJECT

public:
    explicit UnavailableUpdateService(QString current_version, QObject* parent = nullptr)
        : UpdateService(parent), current_version_(std::move(current_version)) {}

    [[nodiscard]] UpdateChannelInfo channel() const override {
        UpdateChannelInfo info;
        info.current_version = current_version_;
        return info;
    }
    [[nodiscard]] bool busy() const override { return false; }
    void check() override {}
    void download() override {}
    void cancel() override {}
    [[nodiscard]] QString install_blocker() const override { return {}; }
    void install() override {}

private:
    QString current_version_;
};

}  // namespace lucent::ui

Q_DECLARE_METATYPE(lucent::ui::UpdateCheckInfo)
Q_DECLARE_METATYPE(lucent::ui::UpdateDownloadInfo)
Q_DECLARE_METATYPE(lucent::ui::UpdateInstallInfo)
