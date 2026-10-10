#include "fakes/fake_services.hpp"

#include <QTimer>

namespace lucent::ui::fakes {

FakeUpdateService::FakeUpdateService(FakeTimings timings, QObject* parent) : UpdateService(parent), timings_(timings) {
    script_.check.ok = true;
    script_.check.availability = UpdateAvailability::Available;
    script_.check.version = QStringLiteral("9.9.0");
    script_.check.published = QStringLiteral("2026-10-09");
    script_.check.release_notes = QStringLiteral("Simulated release notes.\n<b>Plain text</b>, never markup.");
    script_.check.minimum_windows = QStringLiteral("10.0.17763");
    script_.check.size_bytes = 41'869'312;
    script_.download.ok = true;
    script_.download.installer = QStringLiteral("C:\\Simulated\\Lucent-Setup-9.9.0-x64.exe");
    script_.install.ok = true;
}

UpdateChannelInfo FakeUpdateService::channel() const {
    UpdateChannelInfo info;
    info.configured = script_.configured;
    info.channel = QStringLiteral("simulated");
    info.server = QStringLiteral("updates.invalid");
    info.current_version = QStringLiteral("0.1.0");
    return info;
}

void FakeUpdateService::later(std::function<void()> work) {
    busy_ = true;
    QTimer::singleShot(timings_.export_step_ms, this, [this, work = std::move(work)] {
        busy_ = false;
        work();
    });
}

void FakeUpdateService::check() {
    if (busy_ || !script_.configured) {
        return;
    }
    ++checks_;
    later([this] { emit checked(script_.check); });
}

void FakeUpdateService::download() {
    if (busy_) {
        return;
    }
    later([this] {
        emit download_progress(script_.check.size_bytes / 2, script_.check.size_bytes);
        emit download_progress(script_.check.size_bytes, script_.check.size_bytes);
        emit downloaded(script_.download);
    });
}

void FakeUpdateService::cancel() {}

void FakeUpdateService::install() {
    if (busy_) {
        return;
    }
    if (!script_.blocker.isEmpty()) {
        UpdateInstallInfo refused;
        refused.error = script_.blocker;
        refused.kept = true;
        later([this, refused] { emit install_finished(refused); });
        return;
    }
    ++installs_;
    later([this] { emit install_finished(script_.install); });
}

}  // namespace lucent::ui::fakes
