#include "models/current_item.hpp"

namespace lucent::ui {

CurrentItem::CurrentItem(const LibraryModel& model, QObject* parent) : QObject(parent), model_(model) {
    connect(&model_, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex& top_left, const QModelIndex& bottom_right) {
                if (row_ >= top_left.row() && row_ <= bottom_right.row()) {
                    emit changed();
                }
            });
    // Structural changes can move or remove the followed item.
    connect(&model_, &QAbstractItemModel::rowsInserted, this, &CurrentItem::relocate);
    connect(&model_, &QAbstractItemModel::rowsRemoved, this, &CurrentItem::relocate);
    connect(&model_, &QAbstractItemModel::rowsMoved, this, &CurrentItem::relocate);
    connect(&model_, &QAbstractItemModel::modelReset, this, &CurrentItem::relocate);
    connect(&model_, &QAbstractItemModel::layoutChanged, this, &CurrentItem::relocate);
}

void CurrentItem::set_row(int row) {
    const LibraryItem* item = model_.item_at(row);
    const int new_row = item != nullptr ? row : -1;
    const ItemId new_id = item != nullptr ? item->id : 0;
    if (new_row == row_ && new_id == id_) {
        return;
    }
    row_ = new_row;
    id_ = new_id;
    emit changed();
}

void CurrentItem::relocate() {
    if (id_ == 0) {
        return;
    }
    const int new_row = model_.row_of(id_).value_or(-1);
    if (new_row == row_) {
        return;
    }
    row_ = new_row;
    if (row_ < 0) {
        id_ = 0;
    }
    emit changed();
}

QVariant CurrentItem::value(int role) const {
    if (row_ < 0) {
        return {};
    }
    return model_.data(model_.index(row_), role);
}

}  // namespace lucent::ui
