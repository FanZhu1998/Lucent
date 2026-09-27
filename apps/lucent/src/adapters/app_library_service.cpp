#include "adapters/app_library_service.hpp"

#include <QCoreApplication>
#include <QFileInfo>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include "adapters/conversions.hpp"

namespace lucent::ui::adapters {
namespace {

// Items per library commit: small enough that the first photos appear quickly in a large folder,
// large enough that the contact sheet is not re-laid out for every file.
constexpr std::uint32_t kImportBatchSize = 16;
constexpr auto kProgressInterval = std::chrono::milliseconds(100);

QString tr_text(const char* text) {
    return QCoreApplication::translate("lucent::ui::adapters", text);
}

}  // namespace

AppLibraryService::AppLibraryService(std::shared_ptr<Backend> backend, QObject* parent)
    : LibraryService(parent),
      backend_(std::move(backend)),
      dispatcher_(std::make_shared<UiDispatcher>(this)) {
    app::Library* library = &backend_->library();
    // Runs on the thread that committed the change (the import's worker). Snapshots are converted
    // there; `this` is only used by the posted function, on the UI thread.
    subscription_ =
        library->subscribe([this, dispatcher = dispatcher_, library](const app::LibraryChange& change) {
            if (change.kind == app::LibraryChangeKind::Removed) {
                return;  // nothing removes items in W3; the shell has no removal yet
            }
            QList<LibraryItem> items;
            items.reserve(static_cast<qsizetype>(change.items.size()));
            for (const app::ItemId id : change.items) {
                if (std::optional<app::SourceItem> source = library->find(id)) {
                    items << to_library_item(*source);
                }
            }
            if (items.isEmpty()) {
                return;
            }
            const app::LibraryChangeKind kind = change.kind;
            dispatcher->post([this, items, kind] {
                if (kind == app::LibraryChangeKind::Added) {
                    add_items(items);
                } else if (kind == app::LibraryChangeKind::Refreshed) {
                    refresh_items(items);
                } else {
                    update_items(items);
                }
            });
        });
}

AppLibraryService::~AppLibraryService() {
    dispatcher_->close();
    subscription_.reset();
    running_task_.cancel();
}

QStringList AppLibraryService::supported_suffixes() const {
    QStringList suffixes;
    for (const std::wstring_view extension : app::raw_extensions()) {
        suffixes << QString::fromWCharArray(extension.data(), static_cast<qsizetype>(extension.size()));
    }
    return suffixes;
}

void AppLibraryService::import_paths(const QStringList& paths, bool include_subfolders) {
    queue_.push_back(PendingImport{paths, include_subfolders});
    ++active_imports_;
    update_busy();
    emit import_started(paths);
    start_next_import();
}

RenderEventSink AppLibraryService::render_event_sink() {
    // `this` is only used by the posted function, which runs on the UI thread while it lives.
    return [this, dispatcher = dispatcher_](RenderEvent event) {
        dispatcher->post([this, event = std::move(event)] { apply(event); });
    };
}

const LibraryItem* AppLibraryService::item(ItemId item) const {
    const auto found = items_.constFind(item);
    return found == items_.cend() ? nullptr : &*found;
}

void AppLibraryService::start_next_import() {
    if (running_ || queue_.empty()) {
        return;
    }
    PendingImport request = queue_.front();
    queue_.pop_front();
    running_ = true;

    app::Library* library = &backend_->library();
    const raw::DecoderRegistry* registry = &backend_->registry();
    std::vector<std::filesystem::path> outputs = backend_->output_folders().snapshot();
    // The task uses the library and the registry by reference: the Backend keeps both alive until
    // its executor has run every task. `this` is only used by posted functions (UI thread).
    auto task = [this, request, library, registry, outputs,
                 dispatcher = dispatcher_](app::TaskContext& context) {
        app::ImportRequest import;
        for (const QString& path : request.paths) {
            std::filesystem::path native = to_path(path);
            std::error_code error;
            if (std::filesystem::is_directory(native, error)) {
                import.folders.push_back(app::ImportFolder{std::move(native), request.include_subfolders});
            } else {
                import.files.push_back(std::move(native));  // import reports it when it is missing
            }
        }
        import.output_directories = outputs;
        import.batch_size = kImportBatchSize;

        std::chrono::steady_clock::time_point last_post{};
        std::optional<app::ImportPhase> last_phase;
        const app::ImportProgressCallback progress = [&](const app::ImportProgress& update) {
            const auto now = std::chrono::steady_clock::now();
            if (update.phase == app::ImportPhase::Done ||
                (last_phase == update.phase && now - last_post < kProgressInterval)) {
                return;
            }
            last_post = now;
            last_phase = update.phase;
            dispatcher->post([this, ui = to_ui_progress(update)] { emit import_progress(ui); });
        };
        auto report = std::make_shared<const Expected<app::ImportReport>>(
            app::import_sources(*library, *registry, import, progress, context.cancel()));
        dispatcher->post([this, request, report] { finish_import(request, *report); });
    };
    auto submitted = backend_->executor().submit(app::Lane::Background, std::move(task));
    if (!submitted) {
        // Shutting down: still finish the request, asynchronously, as the contract requires.
        auto report = std::make_shared<const Expected<app::ImportReport>>(Unexpected(submitted.error()));
        dispatcher_->post([this, request, report] { finish_import(request, *report); });
        return;
    }
    running_task_ = *submitted;
}

void AppLibraryService::finish_import(const PendingImport& request,
                                      const Expected<app::ImportReport>& report) {
    running_ = false;
    running_task_ = {};

    ImportSummary summary;
    if (report) {
        summary = summarize_import(request.paths, *report);
        std::vector<app::ItemId> listed = report->added;
        listed.insert(listed.end(), report->refreshed.begin(), report->refreshed.end());
        for (const app::ItemId id : listed) {
            const LibraryItem* added = item(id.value);
            if (added == nullptr ||
                (added->support != SupportStatus::Unsupported && added->support != SupportStatus::Limited)) {
                continue;
            }
            const QString status =
                added->support == SupportStatus::Unsupported ? tr_text("Unsupported") : tr_text("Limited");
            summary.notes << tr_text("%1 — %2: %3")
                                 .arg(QFileInfo(added->file_path).fileName(), status, added->support_detail);
        }
    } else {
        summary.sources = request.paths;
        summary.failed = static_cast<int>(request.paths.size());
        summary.errors << describe_error(report.error());
    }
    --active_imports_;
    emit import_finished(summary);
    start_next_import();
    update_busy();
}

void AppLibraryService::add_items(const QList<LibraryItem>& items) {
    QList<LibraryItem> fresh;
    fresh.reserve(items.size());
    QList<LibraryItem> changed;
    for (const LibraryItem& item : items) {
        const auto found = items_.find(item.id);
        if (found == items_.end()) {
            items_.insert(item.id, item);
            fresh << item;
        } else {
            merge_library_facts(*found, item);
            changed << *found;
        }
    }
    if (!fresh.isEmpty()) {
        emit items_added(fresh);
    }
    for (const LibraryItem& item : changed) {
        emit item_changed(item);
    }
}

void AppLibraryService::update_items(const QList<LibraryItem>& items) {
    for (const LibraryItem& item : items) {
        const auto found = items_.find(item.id);
        if (found == items_.end()) {
            continue;
        }
        merge_library_facts(*found, item);
        emit item_changed(*found);
    }
}

void AppLibraryService::refresh_items(const QList<LibraryItem>& items) {
    for (const LibraryItem& fresh : items) {
        const auto found = items_.find(fresh.id);
        if (found == items_.end()) {
            continue;
        }
        LibraryItem& item = *found;
        // Everything the shell derived from the old file is stale; new generations make Qt Quick
        // request new pixels (its caches are keyed by the image URL).
        item.render = nullptr;
        item.processing = ProcessingStatus::NotProcessed;
        item.processing_detail.clear();
        merge_library_facts(item, fresh);
        item.thumbnail_generation = std::max(item.thumbnail_generation, kCameraPreviewGeneration) + 1;
        item.thumbnail_is_camera_preview = false;
        item.preview_generation = std::max<Generation>(item.preview_generation, 1) + 1;
        emit item_changed(item);
    }
}

void AppLibraryService::apply(const RenderEvent& event) {
    const auto found = items_.find(event.item);
    if (found == items_.end()) {
        return;
    }
    LibraryItem& item = *found;
    const LibraryItem before = item;
    switch (event.kind) {
        case RenderEvent::Kind::CameraPreviewUnavailable:
            if (item.thumbnail_is_camera_preview) {
                item.thumbnail_generation = std::max(item.thumbnail_generation, kCameraPreviewGeneration) + 1;
                item.thumbnail_is_camera_preview = false;
            }
            break;
        case RenderEvent::Kind::PreviewStarted:
            if (item.processing != ProcessingStatus::Processed) {
                item.processing = ProcessingStatus::Processing;
                item.processing_detail.clear();
            }
            break;
        case RenderEvent::Kind::PreviewAbandoned:
            if (item.processing == ProcessingStatus::Processing) {
                item.processing = ProcessingStatus::NotProcessed;
            }
            break;
        case RenderEvent::Kind::PreviewFailed:
            item.processing = ProcessingStatus::Failed;
            item.processing_detail = event.message;
            break;
        case RenderEvent::Kind::PreviewDelivered:
            item.processing = ProcessingStatus::Processed;
            item.processing_detail.clear();
            if (event.render && (event.primary || !item.render)) {
                item.render = event.render;
                if (event.render->rendered_width > 0 && event.render->rendered_height > 0) {
                    item.pixel_width = event.render->rendered_width;
                    item.pixel_height = event.render->rendered_height;
                }
            }
            if (event.lucent_thumbnail && item.thumbnail_is_camera_preview) {
                item.thumbnail_generation = std::max(item.thumbnail_generation, kCameraPreviewGeneration) + 1;
                item.thumbnail_is_camera_preview = false;
            }
            break;
    }
    const bool changed = item.processing != before.processing ||
                         item.processing_detail != before.processing_detail || item.render != before.render ||
                         item.pixel_width != before.pixel_width || item.pixel_height != before.pixel_height ||
                         item.thumbnail_generation != before.thumbnail_generation ||
                         item.thumbnail_is_camera_preview != before.thumbnail_is_camera_preview;
    if (changed) {
        emit item_changed(item);
    }
}

void AppLibraryService::update_busy() {
    const bool busy = is_busy();
    if (busy != busy_) {
        busy_ = busy;
        emit busy_changed(busy);
    }
}

}  // namespace lucent::ui::adapters
