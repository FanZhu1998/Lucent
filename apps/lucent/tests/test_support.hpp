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

// A BatchService that records every call and emits what the test asks (view-model tests).
class RecordingBatchService final : public BatchService {
    Q_OBJECT

public:
    [[nodiscard]] bool is_available() const override { return available; }
    [[nodiscard]] QString unavailable_reason() const override { return available ? QString() : reason; }
    quint64 request_plan(const BatchSetup& setup) override {
        plans.push_back(setup);
        return ++last_token;
    }
    quint64 submit(const BatchSetup& setup) override {
        submissions.push_back(setup);
        return ++last_token;
    }
    void cancel_submit(quint64 token) override { cancelled_submissions.push_back(token); }
    void pause(BatchId batch) override { commands.push_back({QStringLiteral("pause"), batch}); }
    void resume(BatchId batch) override { commands.push_back({QStringLiteral("resume"), batch}); }
    void use_replaced_folder(BatchId batch) override { commands.push_back({QStringLiteral("use_folder"), batch}); }
    void load_older() override { ++older_loads; }
    void cancel(BatchId batch) override { commands.push_back({QStringLiteral("cancel"), batch}); }
    void retry_failed(BatchId batch) override { commands.push_back({QStringLiteral("retry"), batch}); }
    void refresh() override { ++refreshes; }
    void request_report(BatchId batch) override { reports.push_back(batch); }
    void request_report_file(BatchId batch) override { report_files.push_back(batch); }
    [[nodiscard]] bool is_busy() const override { return busy; }

    void answer_plan(quint64 token, const BatchPlanInfo& plan) { emit plan_ready(token, plan); }
    void progress(quint64 token, const QString& text, double fraction) {
        emit submit_progress(token, text, fraction);
    }
    void finish_submit(quint64 token, BatchId batch, const QString& error = {}) {
        emit submitted(token, batch, error);
    }
    void list(const QList<BatchSnapshot>& batches) { emit batches_listed(batches); }
    void older(bool more) { emit older_batches(more); }
    void change(const BatchSnapshot& batch) { emit batch_changed(batch); }
    void answer_report(BatchId batch, const BatchReportInfo& report) { emit report_ready(batch, report); }
    void answer_report_file(BatchId batch, const QString& path, const QString& error = {}) {
        emit report_file_ready(batch, path, error);
    }
    void fail_command(BatchId batch, const QString& message) { emit command_failed(batch, message); }

    bool available = true;
    QString reason = QStringLiteral("Not available");
    bool busy = false;
    quint64 last_token = 0;
    std::vector<BatchSetup> plans;
    std::vector<BatchSetup> submissions;
    std::vector<quint64> cancelled_submissions;
    std::vector<std::pair<QString, BatchId>> commands;
    int refreshes = 0;
    int older_loads = 0;
    std::vector<BatchId> reports;
    std::vector<BatchId> report_files;
};

// An EditService that records every call and emits what the test asks (view-model tests).
class RecordingEditService final : public EditService {
    Q_OBJECT

public:
    struct Call {
        QString name;  // "open", "close", "set_transient", "commit", "apply", "set_lock", "undo", ...
        ItemId item = 0;
        quint64 token = 0;
        EditSettings settings;
        quint64 revision = 0;
        quint32 categories = 0;
        bool flag = false;
    };

    void open(ItemId item) override { record({"open", item}); }
    void close(ItemId item) override { record({"close", item}); }
    void set_full_resolution(ItemId item, bool full) override {
        record({"set_full_resolution", item, 0, {}, 0, 0, full});
    }
    void set_comparison(ItemId item, bool wanted) override { record({"set_comparison", item, 0, {}, 0, 0, wanted}); }
    void set_clipping(bool wanted) override { record({"set_clipping", 0, 0, {}, 0, 0, wanted}); }
    quint64 set_transient(ItemId item, const EditSettings& settings) override {
        return record({"set_transient", item, next_token(), settings});
    }
    quint64 commit(ItemId item) override { return record({"commit", item, next_token()}); }
    quint64 apply(ItemId item, const EditSettings& settings) override {
        return record({"apply", item, next_token(), settings});
    }
    quint64 set_lock(ItemId item, EditControl control, bool locked) override {
        return record({"set_lock", item, next_token(), {}, static_cast<quint64>(control), 0, locked});
    }
    quint64 undo(ItemId item) override { return record({"undo", item, next_token()}); }
    quint64 redo(ItemId item) override { return record({"redo", item, next_token()}); }
    quint64 select_revision(ItemId item, quint64 revision) override {
        return record({"select_revision", item, next_token(), {}, revision});
    }
    quint64 reset_to_as_shot(ItemId item) override { return record({"reset_to_as_shot", item, next_token()}); }
    void copy_settings(ItemId item, quint32 categories) override {
        record({"copy_settings", item, 0, {}, 0, categories});
    }
    [[nodiscard]] bool has_clipboard() const override { return clipboard; }
    void paste_settings(const QList<ItemId>& targets) override {
        record({"paste_settings", targets.isEmpty() ? 0 : targets.front()});
    }
    // Piecewise linear (the real curve is the renderer's monotone cubic).
    [[nodiscard]] QList<double> sample_tone_curve(const QList<QPointF>& points, int count) const override;
    [[nodiscard]] bool is_busy() const override { return busy; }

    // Calls named `name`, in order.
    [[nodiscard]] std::vector<Call> calls_named(const QString& name) const;
    [[nodiscard]] int count(const QString& name) const { return static_cast<int>(calls_named(name).size()); }

    // Emits a state answering `token` for `item` with `settings` (committed = settings).
    void answer(ItemId item, quint64 token, const EditSettings& settings, bool can_undo = false,
                bool can_redo = false);
    void emit_state(const EditState& state) { emit state_changed(state); }
    void emit_history(ItemId item, const QList<EditHistoryEntry>& entries) { emit history_changed(item, entries); }
    void emit_white_balance(ItemId item, const WhiteBalanceInfo& info) { emit white_balance_changed(item, info); }
    void emit_frame(const EditFramePtr& frame) { emit frame_ready(frame); }
    void emit_copied(ItemId item, quint32 categories) {
        clipboard = true;
        emit copied(item, categories);
    }
    void emit_pasted(const PasteReport& report) { emit pasted(report); }
    void emit_command_failed(ItemId item, const QString& message) { emit command_failed(item, message); }

    std::vector<Call> calls;
    bool clipboard = false;
    bool busy = false;

private:
    quint64 next_token() { return ++last_token_; }
    quint64 record(Call call) {
        calls.push_back(call);
        return call.token;
    }

    quint64 last_token_ = 0;
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
    RecordingEditService* edits = nullptr;
    RecordingBatchService* batches = nullptr;
};

[[nodiscard]] ScriptedServices make_scripted_services();

// A library item with a camera-preview thumbnail (generation `thumbnail`) and a preview
// (generation `preview`).
[[nodiscard]] LibraryItem make_item(ItemId id, const QString& file_name, Generation thumbnail = 1,
                                    Generation preview = 1);

// A solid image of `size`.
[[nodiscard]] QImage solid_image(QSize size);

}  // namespace lucent::ui::testing
