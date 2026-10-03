#include "services/library_service.hpp"

#include <QCoreApplication>

namespace lucent::ui {

namespace {

QString tr_text(const char* text) {
    return QCoreApplication::translate("lucent::ui::LibraryService", text);
}

}  // namespace

void LibraryService::relink(const QString& folder, const QList<ItemId>& items) {
    Q_UNUSED(items);
    RelinkSummary summary;
    summary.folder = folder;
    summary.error = tr_text("Relinking is not available for this library.");
    // Signals are never emitted from inside the requesting call (see the file comment).
    QMetaObject::invokeMethod(this, [this, summary] { emit relink_finished(summary); }, Qt::QueuedConnection);
}

void LibraryService::back_up_catalog() {
    BackupResult result;
    result.message = tr_text("This library has no catalog to back up.");
    QMetaObject::invokeMethod(this, [this, result] { emit backup_finished(result); }, Qt::QueuedConnection);
}

}  // namespace lucent::ui
