#include "models/edit_history_model.hpp"

#include <QCoreApplication>
#include <QDateTime>

namespace lucent::ui {
namespace {

QString tr_text(const char* text) {
    return QCoreApplication::translate("lucent::ui::EditHistoryModel", text);
}

QString join_controls(const QList<EditControl>& controls) {
    QStringList labels;
    for (const EditControl control : controls) {
        labels << control_label(control);
    }
    return labels.join(QStringLiteral(", "));
}

// The origin as the history list names it.
QString origin_label(const QString& origin) {
    if (origin == u"reset") {
        return tr_text("Reset");
    }
    if (origin == u"paste") {
        return tr_text("Paste");
    }
    if (origin == u"auto") {
        return tr_text("Auto");
    }
    if (origin == u"import") {
        return tr_text("Imported");
    }
    if (origin == u"unknown") {
        return tr_text("Other");  // an origin a newer Lucent added
    }
    return tr_text("Edit");
}

}  // namespace

EditHistoryModel::EditHistoryModel(QObject* parent) : QAbstractListModel(parent) {}

int EditHistoryModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : count();
}

QString EditHistoryModel::title_of(const EditHistoryEntry& entry) {
    const QString changed = join_controls(entry.changed);
    if (entry.origin == u"reset") {
        return tr_text("Reset to as shot");
    }
    if (entry.origin == u"paste") {
        return changed.isEmpty() ? tr_text("Pasted settings") : tr_text("Pasted: %1").arg(changed);
    }
    if (entry.origin == u"auto") {
        if (entry.auto_failed) {
            // An attempt the full-size check rejected: kept with its reason, never used.
            return tr_text("Auto, failed the quality check");
        }
        if (entry.auto_kind == u"fallback" || entry.auto_kind == u"baseline") {
            return changed.isEmpty() ? tr_text("Auto (conservative)") : tr_text("Auto (conservative): %1").arg(changed);
        }
        return changed.isEmpty() ? tr_text("Auto") : tr_text("Auto: %1").arg(changed);
    }
    if (entry.origin == u"import") {
        return tr_text("Imported settings");
    }
    if (entry.origin == u"unknown" && changed.isEmpty()) {
        return tr_text("Change made by a newer Lucent");
    }
    if (entry.reverted_from != 0) {
        return tr_text("Back to revision %1").arg(entry.reverted_from);
    }
    if (!changed.isEmpty()) {
        return changed;
    }
    if (!entry.locks.isEmpty()) {
        return tr_text("Lock changed: %1").arg(join_controls(entry.locks));
    }
    return tr_text("Edit");
}

QVariant EditHistoryModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) {
        return {};
    }
    const Row& row = rows_[index.row()];
    const EditHistoryEntry& entry = row.entry;
    switch (role) {
    case RevisionRole:
        return QVariant::fromValue<qulonglong>(entry.revision);
    case NumberRole:
        return entry.number;
    case Qt::DisplayRole:
    case TitleRole:
        return row.as_shot ? tr_text("As shot") : title_of(entry);
    case DetailRole: {
        if (row.as_shot) {
            return tr_text("The camera's settings");
        }
        const QDateTime local = entry.created.toLocalTime();
        const QString time = local.date() == QDate::currentDate()
                                 ? locale_.toString(local.time(), QLocale::ShortFormat)
                                 : locale_.toString(local, QLocale::ShortFormat);
        return tr_text("#%1 · %2 · %3").arg(entry.number).arg(origin_label(entry.origin), time);
    }
    case OriginRole:
        return row.as_shot ? QStringLiteral("asshot") : entry.origin;
    case CurrentRole:
        return entry.current;
    case OnCurrentPathRole:
        return entry.on_current_path;
    case AccessibleLabelRole: {
        const QString title = row.as_shot ? tr_text("As shot") : title_of(entry);
        QString label = row.as_shot ? title : tr_text("Revision %1: %2").arg(entry.number).arg(title);
        if (entry.current) {
            label += tr_text(", current");
        } else if (!entry.on_current_path) {
            label += tr_text(", undone");
        }
        return label;
    }
    default:
        return {};
    }
}

QHash<int, QByteArray> EditHistoryModel::roleNames() const {
    return {
        {RevisionRole, "revision"},
        {NumberRole, "number"},
        {TitleRole, "title"},
        {DetailRole, "detail"},
        {OriginRole, "origin"},
        {CurrentRole, "current"},
        {OnCurrentPathRole, "onCurrentPath"},
        {AccessibleLabelRole, "accessibleLabel"},
    };
}

void EditHistoryModel::set_entries(const QList<EditHistoryEntry>& entries, bool as_shot_current) {
    const int old_count = count();
    const int old_current = current_row();
    beginResetModel();
    rows_.clear();
    for (auto it = entries.crbegin(); it != entries.crend(); ++it) {
        rows_.push_back(Row{*it, false});
    }
    Row as_shot;
    as_shot.as_shot = true;
    as_shot.entry.current = as_shot_current;
    as_shot.entry.on_current_path = true;  // every path starts as shot
    rows_.push_back(as_shot);
    endResetModel();
    if (count() != old_count) {
        emit countChanged();
    }
    if (current_row() != old_current) {
        emit currentRowChanged();
    }
}

void EditHistoryModel::clear() {
    if (rows_.isEmpty()) {
        return;
    }
    beginResetModel();
    rows_.clear();
    endResetModel();
    emit countChanged();
    emit currentRowChanged();
}

int EditHistoryModel::current_row() const noexcept {
    for (qsizetype i = 0; i < rows_.size(); ++i) {
        if (rows_[i].entry.current) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::optional<quint64> EditHistoryModel::revision_at(int row) const {
    if (row < 0 || row >= count()) {
        return std::nullopt;
    }
    return rows_[row].as_shot ? 0 : rows_[row].entry.revision;
}

}  // namespace lucent::ui
