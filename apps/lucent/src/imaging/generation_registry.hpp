#pragma once

// GenerationRegistry: the current thumbnail and preview generation of every library item, shared
// between the UI thread (LibraryModel writes it before announcing a change) and the image
// providers on Qt Quick's image-loader thread (which read it to reject superseded requests and
// discard superseded results).
//
// Thread-safety: all member functions may be called concurrently from any thread.

#include <optional>
#include <shared_mutex>
#include <unordered_map>

#include "services/image_source.hpp"
#include "services/service_types.hpp"

namespace lucent::ui {

class GenerationRegistry {
public:
    // Records the current generations of `item` (kNoImage when it has no such image).
    void set(ItemId item, Generation thumbnail, Generation preview);
    void remove(ItemId item);
    void clear();

    // The current generation of `kind` for `item`; std::nullopt for an unknown item.
    [[nodiscard]] std::optional<Generation> current(ItemId item, ImageKind kind) const;

    // True when `generation` is the current, available generation of `kind` for `item`.
    [[nodiscard]] bool is_current(ItemId item, ImageKind kind, Generation generation) const;

private:
    struct Entry {
        Generation thumbnail = kNoImage;
        Generation preview = kNoImage;
    };

    mutable std::shared_mutex mutex_;
    // Only looked up by key, never iterated, so its order cannot affect behaviour.
    std::unordered_map<ItemId, Entry> entries_;
};

}  // namespace lucent::ui
