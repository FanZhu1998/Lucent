#pragma once

// Test doubles and helpers for the shell's tests.

#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <QList>
#include <QString>
#include <QStringList>
#include <QtLogging>

#include "services/services.hpp"

namespace lucent::ui::testing {

// Records every warning, critical and fatal Qt message (from any thread) while it exists, and
// forwards all messages to the previous handler. Installing it is process-wide: keep one alive at a
// time.
class MessageCapture {
public:
    MessageCapture();
    ~MessageCapture();

    MessageCapture(const MessageCapture&) = delete;
    MessageCapture& operator=(const MessageCapture&) = delete;

    [[nodiscard]] QStringList warnings() const;
    void clear();

private:
    static void handle(QtMsgType type, const QMessageLogContext& context, const QString& message);

    QtMessageHandler previous_ = nullptr;
};

// A LibraryService whose signals the test emits explicitly.
class ScriptedLibraryService final : public LibraryService {
    Q_OBJECT

public:
    struct ImportRequest {
        QStringList paths;
        bool include_subfolders = false;
        QString copy_to;
    };

    [[nodiscard]] QStringList supported_suffixes() const override { return {"arw", "cr3"}; }
    void start_import(const QStringList& paths, const ImportOptions& options) override {
        requests.push_back({paths, options.include_subfolders, options.copy_to});
    }
    [[nodiscard]] QList<LibraryItem> current_items() const override { return initial; }
    void start() override { ++starts; }
    [[nodiscard]] bool can_relink() const override { return relink_supported; }
    void relink(const QString& folder, const QList<ItemId>& items) override {
        relinks.push_back({folder, items});
    }
    void set_pick(ItemId item, PickState pick) override { picks.push_back({item, pick}); }
    [[nodiscard]] bool can_back_up() const override { return backup_supported; }
    void back_up_catalog() override { ++backups; }
    [[nodiscard]] QString backup_folder() const override { return backup_location; }
    [[nodiscard]] QString library_notice() const override { return notice; }
    [[nodiscard]] bool is_importing() const override { return importing; }
    [[nodiscard]] bool is_busy() const override { return busy; }

    void begin(const QStringList& paths) {
        importing = true;
        emit import_started(paths);
    }
    void finish_relink(const RelinkSummary& summary) { emit relink_finished(summary); }
    void finish_backup(const BackupResult& result) { emit backup_finished(result); }
    void verify(const VerificationProgress& progress) { emit verification_progress(progress); }
    void add(const QList<LibraryItem>& items) { emit items_added(items); }
    void change(const LibraryItem& item) { emit item_changed(item); }
    void finish(const ImportSummary& summary) {
        importing = false;
        emit import_finished(summary);
    }
    void set_busy(bool value) {
        busy = value;
        emit busy_changed(value);
    }

    std::vector<ImportRequest> requests;
    bool importing = false;
    bool busy = false;
    // W4 operations.
    QList<LibraryItem> initial;
    int starts = 0;
    bool relink_supported = true;
    std::vector<std::pair<QString, QList<ItemId>>> relinks;
    std::vector<std::pair<ItemId, PickState>> picks;
    bool backup_supported = true;
    int backups = 0;
    QString backup_location;
    QString notice;
};

// An ExportService that records requests and emits what the test asks.
class RecordingExportService final : public ExportService {
    Q_OBJECT

public:
    void start_export(const ExportRequest& request) override { requests.push_back(request); }
    void cancel_job(JobId job) override { cancelled.push_back(job); }
    [[nodiscard]] bool is_busy() const override { return busy; }
    [[nodiscard]] QString suggested_file_name(ItemId item, const ExportOptions& options) const override {
        return QStringLiteral("item%1_Lucent%2")
            .arg(item)
            .arg(options.format == ExportFileFormat::Jpeg ? QStringLiteral(".jpg") : QStringLiteral(".tif"));
    }

    void add(const ExportJob& job) { emit job_added(job); }
    void change(const ExportJob& job) { emit job_changed(job); }

    std::vector<ExportRequest> requests;
    std::vector<JobId> cancelled;
    bool busy = false;
};

// An ImageSource that keeps every request pending until the test completes it. Thread-safe.
class ManualImageSource final : public ImageSource {
public:
    struct Pending {
        ImageRequest request;
        std::shared_ptr<ImageReply> reply;
    };

    void request_image(const ImageRequest& request, std::shared_ptr<ImageReply> reply) override {
        const std::lock_guard lock(mutex_);
        pending_.push_back({request, std::move(reply)});
    }

    [[nodiscard]] std::vector<Pending> pending() const {
        const std::lock_guard lock(mutex_);
        return pending_;
    }

    [[nodiscard]] std::size_t count() const {
        const std::lock_guard lock(mutex_);
        return pending_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<Pending> pending_;
};

// An ImageSource that completes every request before returning, with an image of `size`.
class SynchronousImageSource final : public ImageSource {
public:
    explicit SynchronousImageSource(QSize size) : size_(size) {}
    void request_image(const ImageRequest& request, std::shared_ptr<ImageReply> reply) override;

private:
    QSize size_;
};

// Services built from the scripted doubles; the raw pointers stay valid while the Services (or the
// AppController that took them) live.
struct ScriptedServices {
    Services services;
    ScriptedLibraryService* library = nullptr;
    RecordingExportService* exports = nullptr;
    std::shared_ptr<ManualImageSource> images;
};

[[nodiscard]] ScriptedServices make_scripted_services();

// A library item with a camera-preview thumbnail (generation `thumbnail`) and a preview
// (generation `preview`).
[[nodiscard]] LibraryItem make_item(ItemId id, const QString& file_name, Generation thumbnail = 1,
                                    Generation preview = 1);

// A solid image of `size`.
[[nodiscard]] QImage solid_image(QSize size);

}  // namespace lucent::ui::testing
