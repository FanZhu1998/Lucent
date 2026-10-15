#pragma once

// UiDispatcher: delivers work from application-service threads to a QObject's thread (the UI
// thread) without ever touching a destroyed object.
//
// The application services call back on executor worker threads (docs/adr/0007). Their callbacks
// capture a std::shared_ptr<UiDispatcher> and post() a function; the function runs later on the
// target's thread through a queued invocation.
//
// Lifetime rule: the owner calls close() at the start of its destructor (on the target's thread).
// post() and close() are serialised by a mutex, so after close() returns no thread can still be
// inside QMetaObject::invokeMethod() for the target, and later post() calls return false and drop
// the function. Invocations posted before close() are discarded by Qt when the target is
// destroyed (QObject removes its pending posted events), so a posted function runs only while its
// target is alive and may therefore capture the target's `this`.
//
// A constructor that throws runs no destructor, so no close(): the owner gives the dispatcher only
// to callbacks that its members unregister (subscription members, which wait for a call in
// progress), and registers a callback that has no such member last, after everything that can throw.
//
// Thread-safety: post() from any thread; close() on the target's thread.

#include <QMetaObject>
#include <QObject>
#include <functional>
#include <mutex>
#include <utility>

namespace lucent::ui::adapters {

class UiDispatcher {
public:
    explicit UiDispatcher(QObject* target) noexcept : target_(target) {}

    UiDispatcher(const UiDispatcher&) = delete;
    UiDispatcher& operator=(const UiDispatcher&) = delete;

    // Queues `function` to run on the target's thread. False (and `function` is destroyed on the
    // calling thread) once closed.
    bool post(std::function<void()> function) {
        const std::lock_guard lock(mutex_);
        if (target_ == nullptr) {
            return false;
        }
        return QMetaObject::invokeMethod(target_, std::move(function), Qt::QueuedConnection);
    }

    // Stops all further posting. Idempotent.
    void close() noexcept {
        const std::lock_guard lock(mutex_);
        target_ = nullptr;
    }

    [[nodiscard]] bool closed() const {
        const std::lock_guard lock(mutex_);
        return target_ == nullptr;
    }

private:
    mutable std::mutex mutex_;
    QObject* target_;
};

}  // namespace lucent::ui::adapters
