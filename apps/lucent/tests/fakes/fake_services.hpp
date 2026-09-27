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
//   - exports progress on a timer and complete without writing any file.
//
// Threading: the library and export services live on the UI thread (the interfaces' contract);
// scanning and painting run on private thread pools. Destructors wait for their pools and complete
// every outstanding image reply.

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

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
    void import_paths(const QStringList& paths, bool include_subfolders) override;
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

// The complete simulated service set, with simulation_notice set.
[[nodiscard]] Services create_fake_services(FakeTimings timings = {});

}  // namespace lucent::ui::fakes
