#pragma once

// Simulated services, a test double of the shell (they were its first back end, before the
// application services were bound in W3): they implement the UI-side service interfaces
// (services/*.hpp) without decoding anything, so the shell can be developed, demonstrated and
// tested before and independently of the application services (src/application).
//
// What is real and what is simulated:
//   - import really enumerates the given files and folders (supported suffixes only, recursion
//     on request, no symbolic-link following) and reports real file names, sizes and
//     modification times (used as the capture time);
//   - camera make/model come from the file suffix; exposure, dimensions, support status and flags
//     are deterministic inventions derived from the file name, and support details say
//     "Simulated";
//   - thumbnails and previews are painted synthetic landscapes; every item starts with a "camera
//     preview" thumbnail (generation 1) that is replaced by a "Lucent render" (generation 2) in
//     the background, except for Unsupported items, whose previews fail with their reason;
//   - exports progress on a timer and complete without writing any file;
//   - editing keeps revisions in memory with the recipe service's rules (one revision per gesture,
//     undo to the parent, redo to the newest child, recorded resets, copy and paste by category)
//     and paints frames from the landscape with exposure and contrast applied; Limited photos have
//     no Kelvin calibration (camera-neutral white balance).
//
// Threading: the library and export services live on the UI thread (the interfaces' contract);
// scanning and painting run on private thread pools. Destructors wait for their pools and complete
// every outstanding image reply.

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <QDateTime>
#include <QHash>
#include <QString>
#include <QThreadPool>
#include <QTimer>

#include "services/services.hpp"

namespace lucent::ui::fakes {

// Artificial latencies. The defaults make the asynchronous behaviour visible in the running
// application; tests use immediate().
struct FakeTimings {
    int thumbnail_delay_ms = 20;     // per thumbnail request
    int preview_delay_ms = 250;      // per preview request
    int render_interval_ms = 120;    // between simulated thumbnail replacements
    int export_step_ms = 60;         // per 10 % of a simulated export

    [[nodiscard]] static FakeTimings immediate() noexcept { return FakeTimings{0, 0, 0, 0}; }
};

// Facts about the simulated items shared by the fake services. Thread-safe.
class FakeCatalog {
public:
    struct Entry {
        QString file_name;
        int width = 0;   // upright pixels
        int height = 0;
        std::uint32_t seed = 0;  // drives the painted scene
        SupportStatus support = SupportStatus::Unknown;
        QString support_detail;
    };

    void add(ItemId item, Entry entry);
    [[nodiscard]] std::optional<Entry> find(ItemId item) const;

private:
    mutable std::mutex mutex_;
    std::map<ItemId, Entry> entries_;
};

// Deterministic 32-bit FNV-1a hash of `text`'s UTF-8 bytes (independent of Qt's per-process hash
// seed).
[[nodiscard]] std::uint32_t stable_hash(const QString& text) noexcept;

// Invents the simulated metadata of a file (see the top of this file). `id` and the file facts
// are copied into the item; generations start at 1 with a camera-preview thumbnail.
[[nodiscard]] LibraryItem make_fake_item(ItemId id, const QString& path, qint64 size_bytes,
                                         const QDateTime& modified);

class FakeLibraryService final : public LibraryService {
    Q_OBJECT

public:
    FakeLibraryService(std::shared_ptr<FakeCatalog> catalog, FakeTimings timings, QObject* parent = nullptr);
    ~FakeLibraryService() override;

    [[nodiscard]] QStringList supported_suffixes() const override;
    // Copying into a library folder is not simulated: options.copy_to is ignored.
    void start_import(const QStringList& paths, const ImportOptions& options) override;
    [[nodiscard]] bool is_importing() const override { return active_imports_ > 0; }
    [[nodiscard]] bool is_busy() const override;

    // The current snapshot of every item, in library order (for tests).
    [[nodiscard]] const std::vector<LibraryItem>& items() const noexcept { return items_; }

private:
    struct ScannedFile {
        QString path;
        qint64 size_bytes = 0;
        QDateTime modified;
    };
    struct ScanResult {
        QStringList sources;
        std::vector<ScannedFile> files;
        int ignored = 0;
        int failed = 0;
        QStringList errors;
    };

    [[nodiscard]] static ScanResult scan(const QStringList& paths, bool include_subfolders,
                                         const QStringList& suffixes, const std::atomic<bool>& stopping);
    void finish_import(const ScanResult& result);
    void render_next_thumbnail();
    void update_busy();

    std::shared_ptr<FakeCatalog> catalog_;
    FakeTimings timings_;
    QThreadPool pool_;  // one thread: requests are scanned in order
    std::atomic<bool> stopping_{false};

    std::vector<LibraryItem> items_;
    QHash<QString, std::size_t> index_by_path_;  // case-folded clean path -> index into items_
    ItemId next_id_ = 1;
    int active_imports_ = 0;
    std::deque<ItemId> render_queue_;
    QTimer render_timer_;
    bool busy_ = false;
};

class FakeImageSource final : public ImageSource {
public:
    FakeImageSource(std::shared_ptr<const FakeCatalog> catalog, FakeTimings timings);
    ~FakeImageSource() override;

    void request_image(const ImageRequest& request, std::shared_ptr<ImageReply> reply) override;

    // Paints the simulated image of `entry` (exposed for tests): a landscape whose colours depend
    // on the seed, graded differently for a camera-preview thumbnail (generation 1).
    [[nodiscard]] static QImage paint(const FakeCatalog::Entry& entry, ImageKind kind, Generation generation,
                                      QSize bounding);

private:
    std::shared_ptr<const FakeCatalog> catalog_;
    FakeTimings timings_;
    std::atomic<bool> stopping_{false};
    QThreadPool pool_;
};

class FakeExportService final : public ExportService {
    Q_OBJECT

public:
    FakeExportService(std::shared_ptr<const FakeCatalog> catalog, FakeTimings timings, QObject* parent = nullptr);
    ~FakeExportService() override;

    void start_export(const ExportRequest& request) override;
    void cancel_job(JobId job) override;
    [[nodiscard]] bool is_busy() const override { return running_ != 0 || !queue_.empty(); }
    [[nodiscard]] QString suggested_file_name(ItemId item, const ExportOptions& options) const override;

private:
    void step();
    void start_next();
    void publish(const ExportJob& job);
    void update_busy();

    std::shared_ptr<const FakeCatalog> catalog_;
    FakeTimings timings_;
    std::map<JobId, ExportJob> jobs_;
    std::deque<JobId> queue_;
    JobId running_ = 0;
    JobId next_id_ = 1;
    QTimer timer_;
    bool busy_ = false;
};

class FakeEditService final : public EditService {
    Q_OBJECT

public:
    FakeEditService(std::shared_ptr<const FakeCatalog> catalog, FakeTimings timings, QObject* parent = nullptr);

    void open(ItemId item) override;
    void close(ItemId item) override;
    void set_full_resolution(ItemId item, bool full) override;
    void set_comparison(ItemId item, bool wanted) override;
    void set_clipping(bool wanted) override;
    quint64 set_transient(ItemId item, const EditSettings& settings) override;
    quint64 commit(ItemId item) override;
    quint64 apply(ItemId item, const EditSettings& settings) override;
    quint64 set_lock(ItemId item, EditControl control, bool locked) override;
    quint64 undo(ItemId item) override;
    quint64 redo(ItemId item) override;
    quint64 select_revision(ItemId item, quint64 revision) override;
    quint64 reset_to_as_shot(ItemId item) override;
    void copy_settings(ItemId item, quint32 categories) override;
    [[nodiscard]] bool has_clipboard() const override { return clipboard_.has_value(); }
    void paste_settings(const QList<ItemId>& targets) override;
    [[nodiscard]] QList<double> sample_tone_curve(const QList<QPointF>& points, int count) const override;
    [[nodiscard]] bool is_busy() const override { return pending_ > 0; }

    // Revisions of `item` (for tests).
    [[nodiscard]] int revision_count(ItemId item) const;

private:
    struct Revision {
        quint64 id = 0;
        quint32 number = 0;
        quint64 parent = 0;
        EditSettings settings;
        QString origin;
        QDateTime created;
        QList<EditControl> changed;
    };
    struct Item {
        std::vector<Revision> revisions;
        quint64 current = 0;
        std::optional<EditSettings> transient;
        quint64 transient_generation = 0;
        bool open = false;
        bool before = false;
    };
    struct Clipboard {
        ItemId source = 0;
        EditSettings settings;
        quint32 categories = 0;
    };

    [[nodiscard]] EditSettings committed(const Item& item) const;
    [[nodiscard]] const Revision* find(const Item& item, quint64 id) const;
    [[nodiscard]] EditState state_of(ItemId id, quint64 token) const;
    [[nodiscard]] QList<EditHistoryEntry> history_of(ItemId id) const;
    [[nodiscard]] bool kelvin_available(ItemId id) const;
    quint64 append(ItemId id, const EditSettings& settings, const QString& origin, quint64 token);
    // Emits the state (and history, frame) later, on the event loop.
    void publish(ItemId id, quint64 token, bool history, bool frame, bool settled);
    void later(std::function<void()> work);
    [[nodiscard]] QImage paint(ItemId id, const EditSettings& settings) const;

    std::shared_ptr<const FakeCatalog> catalog_;
    FakeTimings timings_;
    std::map<ItemId, Item> items_;
    std::optional<Clipboard> clipboard_;
    quint64 next_token_ = 0;
    quint64 next_revision_ = 1;
    quint64 next_sequence_ = 0;
    quint64 next_generation_ = 0;
    bool clipping_ = false;
    int pending_ = 0;
};

// Simulated batches: plans name the outputs from the pattern without touching the folder, and a
// submitted batch completes one photo per ten export steps without writing any file (Unsupported
// photos fail with their reason; folder batches are not simulated and fail at once).
class FakeBatchService final : public BatchService {
    Q_OBJECT

public:
    // `available` false simulates a session without a saved library (no batches).
    FakeBatchService(std::shared_ptr<const FakeCatalog> catalog, FakeTimings timings, bool available = true,
                     QObject* parent = nullptr);
    ~FakeBatchService() override;

    [[nodiscard]] bool is_available() const override { return available_; }
    [[nodiscard]] QString unavailable_reason() const override;
    quint64 request_plan(const BatchSetup& setup) override;
    quint64 submit(const BatchSetup& setup) override;
    void cancel_submit(quint64) override {}
    void pause(BatchId batch) override;
    void resume(BatchId batch) override;
    void cancel(BatchId batch) override;
    void retry_failed(BatchId batch) override;
    void refresh() override;
    void request_report(BatchId batch) override;
    void request_report_file(BatchId batch) override;
    [[nodiscard]] bool is_busy() const override;

private:
    struct Job {
        ItemId item = 0;
        QString source;
        QString output;
        BatchOutcome outcome = BatchOutcome::Pending;
        QString reason;
        double fraction = 0.0;
    };
    struct Batch {
        BatchSnapshot snapshot;
        std::vector<Job> jobs;
    };

    void step();
    void publish(Batch& batch);
    void later(std::function<void()> work);
    void update_busy();

    std::shared_ptr<const FakeCatalog> catalog_;
    FakeTimings timings_;
    bool available_ = true;
    std::map<BatchId, Batch> batches_;
    BatchId next_id_ = 1;
    quint64 next_token_ = 0;
    QTimer timer_;
    bool busy_ = false;
};

// The complete simulated service set, with simulation_notice set. `batches` false: a session
// without durable batches (exports are listed as in-process jobs).
[[nodiscard]] Services create_fake_services(FakeTimings timings = {}, bool batches = true);

}  // namespace lucent::ui::fakes
