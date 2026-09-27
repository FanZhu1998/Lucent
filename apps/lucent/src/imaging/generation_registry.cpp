#include "imaging/generation_registry.hpp"

#include <mutex>

namespace lucent::ui {

void GenerationRegistry::set(ItemId item, Generation thumbnail, Generation preview) {
    const std::unique_lock lock(mutex_);
    entries_[item] = Entry{thumbnail, preview};
}

void GenerationRegistry::remove(ItemId item) {
    const std::unique_lock lock(mutex_);
    entries_.erase(item);
}

void GenerationRegistry::clear() {
    const std::unique_lock lock(mutex_);
    entries_.clear();
}

std::optional<Generation> GenerationRegistry::current(ItemId item, ImageKind kind) const {
    const std::shared_lock lock(mutex_);
    const auto found = entries_.find(item);
    if (found == entries_.end()) {
        return std::nullopt;
    }
    return kind == ImageKind::Thumbnail ? found->second.thumbnail : found->second.preview;
}

bool GenerationRegistry::is_current(ItemId item, ImageKind kind, Generation generation) const {
    if (generation == kNoImage) {
        return false;
    }
    const auto current_generation = current(item, kind);
    return current_generation && *current_generation == generation;
}

}  // namespace lucent::ui
