#include <utility>

#include <QCoreApplication>

#include "services/image_source.hpp"

namespace lucent::ui {

ImageReply::ImageReply(Listener listener) : listener_(std::move(listener)) {}

bool ImageReply::finish(QImage image) {
    if (image.isNull()) {
        return fail(QCoreApplication::translate("lucent::ui::ImageReply", "The image is empty."));
    }
    return complete(State::Finished, std::move(image), {});
}

bool ImageReply::fail(QString message) {
    return complete(State::Failed, {}, std::move(message));
}

bool ImageReply::cancel() {
    std::function<void()> handler;
    {
        // Setting the flag under the handler lock pairs with on_cancel(): a handler installed
        // concurrently either sees the flag (and runs itself) or is taken here, never neither.
        const std::lock_guard lock(cancel_mutex_);
        cancelled_.store(true, std::memory_order_release);
        handler = std::exchange(cancel_handler_, {});
    }
    const bool completed = complete(State::Cancelled, {}, {});
    if (handler) {
        handler();
    }
    return completed;
}

void ImageReply::on_cancel(std::function<void()> handler) {
    {
        const std::lock_guard lock(cancel_mutex_);
        if (!cancelled_.load(std::memory_order_acquire)) {
            cancel_handler_ = std::move(handler);
            return;
        }
    }
    if (handler) {
        handler();
    }
}

bool ImageReply::complete(State state, QImage image, QString error) {
    {
        const std::lock_guard lock(state_mutex_);
        if (state_ != State::Pending) {
            return false;
        }
        state_ = state;
        image_ = std::move(image);
        error_ = std::move(error);
    }
    // The state lock is released before notifying so that the listener (or anything it triggers)
    // may read the outcome. The listener lock orders notification against detach().
    const std::lock_guard lock(listener_mutex_);
    if (listener_) {
        listener_();
    }
    return true;
}

void ImageReply::detach() {
    const std::lock_guard lock(listener_mutex_);
    listener_ = {};
}

ImageReply::State ImageReply::state() const {
    const std::lock_guard lock(state_mutex_);
    return state_;
}

QImage ImageReply::image() const {
    const std::lock_guard lock(state_mutex_);
    return state_ == State::Finished ? image_ : QImage{};
}

QString ImageReply::error() const {
    const std::lock_guard lock(state_mutex_);
    return state_ == State::Failed ? error_ : QString{};
}

}  // namespace lucent::ui
