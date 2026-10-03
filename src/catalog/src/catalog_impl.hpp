#pragma once

// Internal access to the catalog for the implementation files and the white-box tests.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "lucent/catalog/catalog.hpp"
#include "lucent/catalog/types.hpp"

#include "schema.hpp"

namespace lucent::catalog::detail {

struct CatalogAccess {
    // Catalog::open() with an explicit migration list (tests add synthetic later versions). The
    // supported schema version is the last migration's version.
    [[nodiscard]] static Expected<std::unique_ptr<Catalog>> open(const OpenOptions& options,
                                                                 std::span<const Migration> migrations);
};

struct ItemCursorAccess {
    [[nodiscard]] static ItemCursor make(ItemOrder order, std::string text, std::int64_t number) {
        ItemCursor cursor;
        cursor.order_ = order;
        cursor.text_ = std::move(text);
        cursor.number_ = number;
        return cursor;
    }
    [[nodiscard]] static ItemOrder order(const ItemCursor& cursor) noexcept { return cursor.order_; }
    [[nodiscard]] static const std::string& text(const ItemCursor& cursor) noexcept { return cursor.text_; }
    [[nodiscard]] static std::int64_t number(const ItemCursor& cursor) noexcept { return cursor.number_; }
};

}  // namespace lucent::catalog::detail
