#pragma once

// AppUpdateService: the shell's UpdateService (services/update_service.hpp) over the Backend's
// app::UpdateService (application/update_service.hpp; ADR 0022).
//
// check(), download() and install() each run on a worker thread of this adapter (one at a time: a
// request while one runs is ignored), so the UI thread never waits for the network, the disk or the
// pre-update catalog backup. Results are posted to the UI thread (UiDispatcher); download progress
// is posted at most about ten times a second. The verified installer of the last download stays held
// open by this adapter (writers excluded) until install() hands it to Setup or the session ends.
// Errors are worded for the photographer here (user_facing_update_error()).
//
// Lifetime and threading: lives on the UI thread; the destructor closes the dispatcher, cancels the
// operation in progress and joins the worker thread before the Backend can go away.

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

#include <lucent/application/update_service.hpp>
#include <lucent/core/cancellation.hpp>

#include "adapters/backend.hpp"
#include "adapters/ui_dispatcher.hpp"
#include "services/update_service.hpp"

namespace lucent::ui::adapters {

// The photographer's wording of an update error (by its code and location; the developer text only
// as a last resort).
[[nodiscard]] QString user_facing_update_error(const Error& error);

class AppUpdateService final : public UpdateService {
    Q_OBJECT

public:
    explicit AppUpdateService(std::shared_ptr<Backend> backend, QObject* parent = nullptr);
    ~AppUpdateService() override;

    [[nodiscard]] UpdateChannelInfo channel() const override;
    [[nodiscard]] bool busy() const override { return busy_; }
    void check() override;
    void download() override;
    void cancel() override;
    [[nodiscard]] QString install_blocker() const override;
    void install() override;

private:
    // Runs `work` on the worker thread (after joining a finished previous one).
    void start(std::function<void(core::CancellationToken)> work);

    std::shared_ptr<Backend> backend_;
    std::shared_ptr<UiDispatcher> dispatcher_;
    std::thread worker_;
    core::CancellationSource cancel_;
    bool busy_ = false;                              // (UI thread)
    std::optional<app::UpdateCheck> last_check_;      // (UI thread) the last check with a release
    std::shared_ptr<app::DownloadedInstaller> installer_;  // (UI thread) the verified download
};

}  // namespace lucent::ui::adapters
