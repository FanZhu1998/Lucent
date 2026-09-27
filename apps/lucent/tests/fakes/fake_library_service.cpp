#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QSet>

#include "fakes/fake_services.hpp"

namespace lucent::ui::fakes {
namespace {

// Guards against pathological nesting; real folder trees are far shallower.
constexpr int kMaxFolderDepth = 64;

QString tr_fake(const char* text) {
    return QCoreApplication::translate("lucent::ui::fakes", text);
}

QString path_key(const QString& path) {
    return QDir::cleanPath(path).toCaseFolded();
}

bool is_link(const QFileInfo& info) {
    return info.isSymLink() || info.isJunction() || info.isShortcut();
}

}  // namespace

FakeLibraryService::FakeLibraryService(std::shared_ptr<FakeCatalog> catalog, FakeTimings timings, QObject* parent)
    : LibraryService(parent), catalog_(std::move(catalog)), timings_(timings) {
    pool_.setMaxThreadCount(1);
    render_timer_.setInterval(std::max(0, timings_.render_interval_ms));
    connect(&render_timer_, &QTimer::timeout, this, &FakeLibraryService::render_next_thumbnail);
}

FakeLibraryService::~FakeLibraryService() {
    // A scan in progress stops early; its queued result is dropped with this object.
    stopping_.store(true);
    pool_.waitForDone();
}

QStringList FakeLibraryService::supported_suffixes() const {
    return {QStringLiteral("3fr"), QStringLiteral("arw"), QStringLiteral("cr2"), QStringLiteral("cr3"),
            QStringLiteral("dng"), QStringLiteral("erf"), QStringLiteral("fff"), QStringLiteral("iiq"),
            QStringLiteral("mos"), QStringLiteral("mrw"), QStringLiteral("nef"), QStringLiteral("nrw"),
            QStringLiteral("orf"), QStringLiteral("pef"), QStringLiteral("raf"), QStringLiteral("rw2"),
            QStringLiteral("rwl"), QStringLiteral("sr2"), QStringLiteral("srf"), QStringLiteral("srw"),
            QStringLiteral("x3f")};
}

bool FakeLibraryService::is_busy() const {
    return active_imports_ > 0 || !render_queue_.empty();
}

void FakeLibraryService::import_paths(const QStringList& paths, bool include_subfolders) {
    ++active_imports_;
    update_busy();
    emit import_started(paths);
    const QStringList suffixes = supported_suffixes();
    pool_.start([this, paths, include_subfolders, suffixes] {
        ScanResult result = scan(paths, include_subfolders, suffixes, stopping_);
        QMetaObject::invokeMethod(
            this, [this, result = std::move(result)] { finish_import(result); }, Qt::QueuedConnection);
    });
}

FakeLibraryService::ScanResult FakeLibraryService::scan(const QStringList& paths, bool include_subfolders,
                                                        const QStringList& suffixes,
                                                        const std::atomic<bool>& stopping) {
    ScanResult result;
    result.sources = paths;
    QSet<QString> seen;

    const auto consider_file = [&](const QFileInfo& file) {
        if (!suffixes.contains(file.suffix().toLower())) {
            ++result.ignored;
            return;
        }
        const QString path = file.absoluteFilePath();
        if (seen.contains(path_key(path))) {
            return;
        }
        seen.insert(path_key(path));
        result.files.push_back(ScannedFile{path, file.size(), file.lastModified()});
    };

    // Depth-first, without following symbolic links, junctions or shortcuts (no scan loops).
    const auto scan_folder = [&](const auto& self, const QString& folder, int depth) -> void {
        if (stopping.load() || depth > kMaxFolderDepth) {
            return;
        }
        const QFileInfoList entries =
            QDir(folder).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                       QDir::Name | QDir::IgnoreCase | QDir::DirsLast);
        for (const QFileInfo& entry : entries) {
            if (stopping.load()) {
                return;
            }
            if (is_link(entry)) {
                continue;
            }
            if (entry.isDir()) {
                if (include_subfolders) {
                    self(self, entry.absoluteFilePath(), depth + 1);
                }
            } else {
                consider_file(entry);
            }
        }
    };

    for (const QString& path : paths) {
        if (stopping.load()) {
            break;
        }
        const QFileInfo info(path);
        if (!info.exists()) {
            ++result.failed;
            result.errors << tr_fake("%1 was not found.").arg(QDir::toNativeSeparators(path));
        } else if (info.isDir()) {
            if (!info.isReadable()) {
                ++result.failed;
                result.errors << tr_fake("%1 could not be read.").arg(QDir::toNativeSeparators(path));
            } else {
                scan_folder(scan_folder, info.absoluteFilePath(), 0);
            }
        } else {
            consider_file(info);
        }
    }
    std::sort(result.files.begin(), result.files.end(), [](const ScannedFile& a, const ScannedFile& b) {
        return path_key(a.path) < path_key(b.path);
    });
    return result;
}

void FakeLibraryService::finish_import(const ScanResult& result) {
    ImportSummary summary;
    summary.sources = result.sources;
    summary.ignored = result.ignored;
    summary.failed = result.failed;
    summary.errors = result.errors;

    QList<LibraryItem> added;
    for (const ScannedFile& file : result.files) {
        const QString key = path_key(file.path);
        if (index_by_path_.contains(key)) {
            ++summary.already_in_library;
            continue;
        }
        const LibraryItem item = make_fake_item(next_id_++, file.path, file.size_bytes, file.modified);
        catalog_->add(item.id, FakeCatalog::Entry{QFileInfo(item.file_path).fileName(), item.pixel_width,
                                                  item.pixel_height, stable_hash(item.file_path), item.support,
                                                  item.support_detail});
        index_by_path_.insert(key, items_.size());
        items_.push_back(item);
        render_queue_.push_back(item.id);
        added << item;
    }
    summary.added = static_cast<int>(added.size());

    if (!added.isEmpty()) {
        emit items_added(added);
    }
    --active_imports_;
    emit import_finished(summary);
    if (!render_queue_.empty() && !render_timer_.isActive()) {
        render_timer_.start();
    }
    update_busy();
}

void FakeLibraryService::render_next_thumbnail() {
    if (!render_queue_.empty()) {
        const ItemId id = render_queue_.front();
        render_queue_.pop_front();
        // Ids are assigned in order from 1 and items are never removed, so id - 1 is the index.
        const auto index = static_cast<std::size_t>(id - 1);
        if (index < items_.size() && items_[index].support != SupportStatus::Unsupported) {
            LibraryItem& item = items_[index];
            item.thumbnail_generation += 1;
            item.thumbnail_is_camera_preview = false;
            item.processing = ProcessingStatus::Processed;
            emit item_changed(item);
        }
    }
    if (render_queue_.empty()) {
        render_timer_.stop();
        update_busy();
    }
}

void FakeLibraryService::update_busy() {
    const bool busy = is_busy();
    if (busy != busy_) {
        busy_ = busy;
        emit busy_changed(busy);
    }
}

}  // namespace lucent::ui::fakes
