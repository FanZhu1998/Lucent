#include "lucent/core/cancellation.hpp"

#include <string>

namespace lucent::core {

// Memory ordering (see the header): release on the store pairs with acquire on every load.

bool CancellationToken::is_cancelled() const noexcept {
    return state_ != nullptr && state_->load(std::memory_order_acquire);
}

Status CancellationToken::check(std::string_view where) const {
    if (is_cancelled()) {
        return fail(ErrorCode::Cancelled, "operation cancelled", std::string(where));
    }
    return ok();
}

CancellationSource::CancellationSource() : state_(std::make_shared<std::atomic<bool>>(false)) {}

void CancellationSource::cancel() noexcept {
    state_->store(true, std::memory_order_release);
}

bool CancellationSource::is_cancelled() const noexcept {
    return state_->load(std::memory_order_acquire);
}

}  // namespace lucent::core
