#pragma once

// Private: a list of observers with the registration contract documented in library.hpp:
//   * notify() calls every active observer synchronously on the calling thread, without holding
//     any lock of the list, so observers may re-enter the notifier or (un)subscribe;
//   * cancelling a Subscription waits until the observer is no longer running on another thread
//     (a cancellation from inside the observer itself does not wait for that call), and the
//     observer is never called afterwards;
//   * exceptions escaping an observer are caught and logged under `category`.
// Subscriptions stay safe to cancel after the list itself has been destroyed.

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <lucent/application/library.hpp>
#include <lucent/core/log.hpp>

namespace lucent::app::detail {

template <class Event>
class ObserverList {
public:
    using Observer = std::function<void(const Event&)>;

    explicit ObserverList(std::string category)
        : impl_(std::make_shared<Impl>()), category_(std::move(category)) {}

    [[nodiscard]] Subscription add(Observer observer) {
        if (!observer) {
            return Subscription{};
        }
        auto slot = std::make_shared<Slot>();
        slot->observer = std::move(observer);
        {
            std::lock_guard lock(impl_->mutex);
            impl_->slots.push_back(slot);
        }
        return Subscription([weak = std::weak_ptr<Impl>(impl_), slot] {
            {
                std::unique_lock lock(slot->mutex);
                slot->active = false;
                const auto self = std::this_thread::get_id();
                slot->idle.wait(lock, [&] {
                    return std::all_of(slot->callers.begin(), slot->callers.end(),
                                       [&](const std::thread::id& id) { return id == self; });
                });
            }
            if (auto impl = weak.lock()) {
                std::lock_guard lock(impl->mutex);
                std::erase(impl->slots, slot);
            }
        });
    }

    void notify(const Event& event) const {
        std::vector<std::shared_ptr<Slot>> slots;
        {
            std::lock_guard lock(impl_->mutex);
            slots = impl_->slots;
        }
        const auto self = std::this_thread::get_id();
        for (const std::shared_ptr<Slot>& slot : slots) {
            {
                std::lock_guard lock(slot->mutex);
                if (!slot->active) {
                    continue;
                }
                slot->callers.push_back(self);
            }
            try {
                slot->observer(event);
            } catch (const std::exception& e) {
                core::log_error(category_, "an observer threw: {}", e.what());
            } catch (...) {
                core::log_error(category_, "an observer threw an unknown exception");
            }
            {
                std::lock_guard lock(slot->mutex);
                const auto it = std::find(slot->callers.begin(), slot->callers.end(), self);
                if (it != slot->callers.end()) {
                    slot->callers.erase(it);
                }
            }
            slot->idle.notify_all();
        }
    }

private:
    struct Slot {
        Observer observer;
        std::mutex mutex;
        std::condition_variable idle;
        bool active = true;
        std::vector<std::thread::id> callers;  // threads currently inside `observer`
    };
    struct Impl {
        std::mutex mutex;
        std::vector<std::shared_ptr<Slot>> slots;
    };

    std::shared_ptr<Impl> impl_;
    std::string category_;
};

}  // namespace lucent::app::detail
