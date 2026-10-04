#pragma once

// EditHistoryModel: the revisions of the photo being edited, for the Edit view's history list
// (blueprint section 2: "undo/redo, and reset to as-shot or an earlier recipe"). Newest first, with
// a final "As shot" row (revision 0). Each row says what the revision did (its origin and the
// controls it changed), when, and whether it is the current revision or lies on the path to it (an
// undone branch is listed but not on the path).
//
// Thread-safety: UI thread only.

#include <QAbstractListModel>
#include <QList>
#include <QLocale>
#include <QtQmlIntegration/qqmlintegration.h>
#include <optional>

#include "services/edit_service.hpp"

namespace lucent::ui {

class EditHistoryModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("EditHistoryModel is provided by EditSession.history.")
    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)
    Q_PROPERTY(int currentRow READ current_row NOTIFY currentRowChanged FINAL)

public:
    enum Role {
        RevisionRole = Qt::UserRole + 1,  // qulonglong; 0 for "As shot"
        NumberRole,                       // revision number; 0 for "As shot"
        TitleRole,                        // "Exposure, Contrast", "Reset to as shot", "Pasted: Tone", ...
        DetailRole,                       // "#3 · Edit · 14:02" (number, origin, time in the locale's format)
        OriginRole,                       // "user", "reset", "paste", "auto", "import", "asshot"
        CurrentRole,
        OnCurrentPathRole,
        AccessibleLabelRole,
    };
    Q_ENUM(Role)

    explicit EditHistoryModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    // Replaces the rows (`entries` oldest first, as the service lists them); `as_shot_current`:
    // the photo has no current revision.
    void set_entries(const QList<EditHistoryEntry>& entries, bool as_shot_current);
    void clear();

    [[nodiscard]] int count() const noexcept { return static_cast<int>(rows_.size()); }
    [[nodiscard]] int current_row() const noexcept;
    // The revision of `row` (0: as shot); std::nullopt for an invalid row.
    [[nodiscard]] std::optional<quint64> revision_at(int row) const;

    // The title of an entry (exposed for tests).
    [[nodiscard]] static QString title_of(const EditHistoryEntry& entry);

    void set_locale(const QLocale& locale) { locale_ = locale; }

signals:
    void countChanged();
    void currentRowChanged();

private:
    struct Row {
        EditHistoryEntry entry;
        bool as_shot = false;
    };

    QList<Row> rows_;
    QLocale locale_;
};

}  // namespace lucent::ui
