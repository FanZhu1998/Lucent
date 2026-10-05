#include "adapters/app_library_service.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include <lucent/core/log.hpp>

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

// The shell's snapshot of `source`, with the paths of its content duplicates looked up in
// `library` (any thread).
LibraryItem convert(const app::SourceItem& source, const app::Library& library) {
    QStringList duplicates;
    for (const app::ItemId id : source.content_duplicates) {
        if (std::optional<app::SourceItem> twin = library.find(id)) {
            duplicates << QDir::toNativeSeparators(to_qstring(twin->path));
        }
    }
    return to_library_item(source, duplicates);
}

QByteArray identity_bytes(const core::Digest256& digest) {
    return QByteArray(reinterpret_cast<const char*>(digest.data()), static_cast<qsizetype>(digest.size()));
}

// The edited flag of a recipe state (service_types.hpp, LibraryItem::edited).
bool is_edited(const app::RecipeState& state) {
    return state.revision.has_value() && (state.recipe_unreadable || !state.committed.is_as_shot());
}

// W8: the current revision is an automatic one whose analysis record recommends a review
// (ItemFlag::ReviewRecommended). One catalog read of the item's records.
bool recommends_review(const app::RecipeService& recipes, const app::RecipeState& state) {
    if (!state.revision) {
        return false;
    }
    auto records = recipes.automatic_analyses(state.item);
    if (!records) {
        return false;
    }
    for (const catalog::AutoAnalysisRecord& record : *records) {
        if (record.revision == *state.revision) {
            return record.review;
        }
    }
    return false;
}

QString describe_library_problem(const Error& error) {
    switch (error.code) {
        case ErrorCode::VersionMismatch:
            return tr_text(
                "This library was saved by a newer version of Lucent. It is shown read-only; "
                "changes are not saved.");
        case ErrorCode::AlreadyExists:
            return tr_text("The library is open in another Lucent window. Changes made here are not saved.");
        case ErrorCode::Corrupt:
            return tr_text(
                "The library catalog is damaged. Changes made here are not saved; restore a "
                "catalog backup.");
        case ErrorCode::Unsupported:
            return tr_text("The library catalog must be on a local disk. Changes made here are not saved.");
        default:
            break;
    }
    return tr_text("The library could not be opened (%1). Changes made here are not saved.")
        .arg(describe_error(error));
}

}  // namespace

AppLibraryService::AppLibraryService(std::shared_ptr<Backend> backend, QObject* parent)
    : LibraryService(parent),
      backend_(std::move(backend)),
      dispatcher_(std::make_shared<UiDispatcher>(this)) {
    app::Library* library = &backend_->library();
    // Runs on the thread that committed the change (an import, the maintenance service, a pick).
    // Snapshots are converted there; `this` is only used by the posted function, on the UI thread.
    subscription_ =
        library->subscribe([this, dispatcher = dispatcher_, library](const app::LibraryChange& change) {
            if (change.kind == app::LibraryChangeKind::Removed) {
                return;  // nothing removes items from the shell yet
            }
            QList<LibraryItem> items;
            items.reserve(static_cast<qsizetype>(change.items.size()));
            for (const app::ItemId id : change.items) {
                if (std::optional<app::SourceItem> source = library->find(id)) {
                    items << convert(*source, *library);
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
    // The library as loaded (the catalog's items; none for a session library). Changes committed
    // between the subscription and this snapshot are applied again when their posts arrive, which
    // is harmless (add_items treats known ids as updates).
    for (const app::SourceItem& source : library->items()) {
        LibraryItem item = convert(source, *library);
        initial_order_.push_back(item.id);
        items_.insert(item.id, std::move(item));
    }
    backend_->set_maintenance_sink([this, dispatcher = dispatcher_](const app::MaintenanceEvent& event) {
        dispatcher->post([this, event] { on_maintenance(event); });
    });
    // Committed recipes (an edit, undo/redo, a paste) change what the thumbnail must show. Runs on
    // the thread that made the change; `this` is only used by the posted function.
    recipe_subscription_ = backend_->recipes().subscribe(
        [this, dispatcher = dispatcher_, recipes = &backend_->recipes()](const app::RecipeChange& change) {
            if (change.kind != app::RecipeChangeKind::Committed && change.kind != app::RecipeChangeKind::Moved) {
                return;
            }
            const app::RecipeState& state = change.state;
            // A revision the photographer made is never automatic: only automatic revisions and moves
            // through the history (onto or off one) need its record.
            const bool review = (change.kind == app::RecipeChangeKind::Moved ||
                                 change.origin == catalog::RevisionOrigin::Auto) &&
                                recommends_review(*recipes, state);
            dispatcher->post([this, item = state.item.value, edited = is_edited(state),
                              identity = identity_bytes(state.committed_sha256),
                              readable = !state.recipe_unreadable, review] {
                apply_recipe(item, edited, identity, readable, review);
            });
        });
}

AppLibraryService::~AppLibraryService() {
    dispatcher_->close();
    backend_->set_maintenance_sink({});
    recipe_subscription_.reset();
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

QList<LibraryItem> AppLibraryService::current_items() const {
    QList<LibraryItem> items;
    items.reserve(static_cast<qsizetype>(initial_order_.size()));
    for (const ItemId id : initial_order_) {
        if (const LibraryItem* found = item(id)) {
            items << *found;
        }
    }
    return items;
}

void AppLibraryService::start() {
    if (started_) {
        return;
    }
    started_ = true;
    scan_recipes();
    app::MaintenanceService* maintenance = backend_->maintenance();
    if (maintenance == nullptr) {
        return;
    }
    if (Status queued = maintenance->check_all(); queued) {
        verifying_ = true;
        update_busy();
    } else {
        core::log_warn("ui.library", "the library check could not start: {}", queued.error().describe());
    }
}

void AppLibraryService::recheck_sources() {
    app::MaintenanceService* maintenance = backend_->maintenance();
    if (!started_ || maintenance == nullptr) {
        return;  // start() checks everything anyway
    }
    if (Status queued = maintenance->check_all(); queued) {
        verifying_ = true;
        update_busy();
    } else {
        core::log_warn("ui.library", "the library check could not start: {}", queued.error().describe());
    }
}

void AppLibraryService::start_import(const QStringList& paths, const ImportOptions& options) {
    queue_.push_back(PendingImport{paths, options});
    ++active_imports_;
    update_busy();
    emit import_started(paths);
    start_next_import();
}

bool AppLibraryService::can_relink() const {
    return backend_->maintenance() != nullptr;
}

void AppLibraryService::relink(const QString& folder, const QList<ItemId>& items) {
    app::MaintenanceService* maintenance = backend_->maintenance();
    if (maintenance == nullptr) {
        LibraryService::relink(folder, items);
        return;
    }
    app::RelinkRequest request;
    request.folder = to_path(folder);
    request.include_subfolders = true;
    for (const ItemId id : items) {
        request.items.push_back(app::ItemId{id});
    }
    ++pending_operations_;
    update_busy();
    const Status queued = maintenance->relink(
        std::move(request),
        [this, dispatcher = dispatcher_, folder](const Expected<app::RelinkReport>& report) {
            dispatcher->post([this, summary = summarize_relink(folder, report)] {
                operation_finished();
                emit relink_finished(summary);
            });
        });
    if (!queued) {
        dispatcher_->post([this, summary = summarize_relink(folder, Unexpected(queued.error()))] {
            operation_finished();
            emit relink_finished(summary);
        });
    }
}

void AppLibraryService::set_pick(ItemId item_id, PickState pick) {
    const LibraryItem* current = item(item_id);
    if (current == nullptr || current->pick == pick) {
        return;
    }
    app::Library* library = &backend_->library();
    const app::ItemFlags flags{static_cast<app::PickFlag>(pick), static_cast<std::uint8_t>(current->rating)};
    // A catalog write: never on the UI thread. The library's Updated change brings item_changed.
    auto submitted =
        backend_->executor().submit(app::Lane::Quick, [library, item_id, flags](app::TaskContext&) {
            if (Status written = library->set_flags(app::ItemId{item_id}, flags); !written) {
                core::log_warn("ui.library", "could not save the pick of item {}: {}", item_id,
                               written.error().describe());
            }
        });
    if (!submitted) {
        core::log_warn("ui.library", "could not save the pick of item {}: {}", item_id,
                       submitted.error().describe());
    }
}

bool AppLibraryService::can_back_up() const {
    return backend_->catalog() != nullptr && !backend_->catalog()->is_read_only();
}

void AppLibraryService::back_up_catalog() {
    if (!can_back_up()) {
        LibraryService::back_up_catalog();
        return;
    }
    std::shared_ptr<catalog::Catalog> catalog = backend_->catalog();
    ++pending_operations_;
    update_busy();
    // The catalog is kept alive by the Backend until the executor has run every task.
    auto task = [catalog = catalog.get(), dispatcher = dispatcher_, this](app::TaskContext&) {
        auto backup = catalog->create_backup();
        BackupResult result;
        if (backup) {
            result.ok = true;
            result.path = QDir::toNativeSeparators(to_qstring(backup->path));
        } else {
            core::log_warn("ui.library", "the catalog backup failed: {}", backup.error().describe());
            result.message = describe_error(backup.error());
        }
        dispatcher->post([this, result] {
            operation_finished();
            emit backup_finished(result);
        });
    };
    if (auto submitted = backend_->executor().submit(app::Lane::Background, std::move(task)); !submitted) {
        BackupResult result;
        result.message = describe_error(submitted.error());
        dispatcher_->post([this, result] {
            operation_finished();
            emit backup_finished(result);
        });
    }
}

QString AppLibraryService::backup_folder() const {
    const std::shared_ptr<catalog::Catalog>& catalog = backend_->catalog();
    return catalog ? QDir::toNativeSeparators(to_qstring(catalog->backup_directory())) : QString();
}

QString AppLibraryService::library_notice() const {
    const std::optional<Error>& problem = backend_->library_problem();
    return problem ? describe_library_problem(*problem) : QString();
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
    std::vector<std::filesystem::path> own = backend_->lucent_directories();
    // The task uses the library and the registry by reference: the Backend keeps both alive until
    // its executor has run every task. `this` is only used by posted functions (UI thread).
    auto task = [this, request, library, registry, outputs, own,
                 dispatcher = dispatcher_](app::TaskContext& context) {
        app::ImportRequest import;
        for (const QString& path : request.paths) {
            std::filesystem::path native = to_path(path);
            std::error_code error;
            if (std::filesystem::is_directory(native, error)) {
                import.folders.push_back(
                    app::ImportFolder{std::move(native), request.options.include_subfolders});
            } else {
                import.files.push_back(std::move(native));  // import reports it when it is missing
            }
        }
        import.output_directories = outputs;
        import.lucent_directories = own;
        import.batch_size = kImportBatchSize;
        if (!request.options.copy_to.isEmpty()) {
            import.copy_to = to_path(request.options.copy_to);
        }

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
        summary.copy_to = QDir::toNativeSeparators(request.options.copy_to);
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
        // Files found again (back, or moved) may never have been hashed; hashed ones are skipped.
        listed.insert(listed.end(), report->restored.begin(), report->restored.end());
        queue_hashing(listed);
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

void AppLibraryService::queue_hashing(const std::vector<app::ItemId>& items) {
    app::MaintenanceService* maintenance = backend_->maintenance();
    if (maintenance == nullptr || items.empty()) {
        return;
    }
    if (Status queued = maintenance->hash(items); queued) {
        verifying_ = true;
        update_busy();
    } else {
        core::log_warn("ui.library", "hashing could not be queued: {}", queued.error().describe());
    }
}

void AppLibraryService::on_maintenance(const app::MaintenanceEvent& event) {
    if (event.kind == app::MaintenanceEvent::Kind::Idle) {
        verifying_ = false;
        emit verification_progress(VerificationProgress{false, static_cast<int>(event.hash_items_done),
                                                        static_cast<int>(event.hash_items_total)});
        update_busy();
        return;
    }
    const app::MaintenanceProgress& progress = event.progress;
    VerificationProgress ui;
    ui.active = true;
    switch (progress.phase) {
        case app::MaintenancePhase::Checking:
        case app::MaintenancePhase::Reprobing:
            ui.done = static_cast<int>(progress.items_done);
            ui.total = static_cast<int>(progress.items_total);
            break;
        case app::MaintenancePhase::Hashing:
            ui.done = static_cast<int>(event.hash_items_done);
            ui.total = static_cast<int>(event.hash_items_total);
            break;
        case app::MaintenancePhase::Scanning:
        case app::MaintenancePhase::Matching:
            return;  // a relink: reported by relink_finished
    }
    verifying_ = true;
    emit verification_progress(ui);
    update_busy();
}

void AppLibraryService::operation_finished() {
    pending_operations_ = std::max(0, pending_operations_ - 1);
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

void AppLibraryService::scan_recipes() {
    if (backend_->catalog() == nullptr || items_.isEmpty()) {
        return;  // a session library starts without revisions
    }
    std::vector<app::ItemId> ids;
    ids.reserve(static_cast<std::size_t>(items_.size()));
    for (const ItemId id : initial_order_) {
        ids.push_back(app::ItemId{id});
    }
    app::RecipeService* recipes = &backend_->recipes();
    // Reading every item's current revision is a catalog query per item: on a worker, after the
    // window is up. The recipe service outlives the executor's tasks (backend.hpp).
    auto task = [this, recipes, ids = std::move(ids), dispatcher = dispatcher_](app::TaskContext& context) {
        for (const app::ItemId id : ids) {
            if (context.cancel().is_cancelled()) {
                return;
            }
            auto state = recipes->state(id);
            if (!state || !state->revision) {
                continue;
            }
            dispatcher->post([this, item = id.value, edited = is_edited(*state),
                              identity = identity_bytes(state->committed_sha256),
                              readable = !state->recipe_unreadable, review = recommends_review(*recipes, *state)] {
                apply_recipe(item, edited, identity, readable, review);
            });
        }
    };
    if (auto submitted = backend_->executor().submit(app::Lane::Quick, std::move(task)); !submitted) {
        core::log_warn("ui.library", "the edited photos could not be looked up: {}", submitted.error().describe());
    }
}

void AppLibraryService::apply_recipe(ItemId item_id, bool edited, const QByteArray& identity, bool readable,
                                     bool review) {
    const auto found = items_.find(item_id);
    if (found == items_.end()) {
        return;
    }
    LibraryItem& item = *found;
    const LibraryItem before = item;
    item.edited = edited;
    item.flags = review ? (item.flags | flag_bit(ItemFlag::ReviewRecommended))
                        : (item.flags & ~flag_bit(ItemFlag::ReviewRecommended));
    if (readable) {
        const auto known = recipe_identity_.constFind(item_id);
        const bool changed = known == recipe_identity_.cend() ? edited : *known != identity;
        recipe_identity_.insert(item_id, identity);
        if (changed && item.thumbnail_generation != kNoImage) {
            // Lucent's render of the new recipe (a new URL: Qt Quick requests the new pixels).
            item.thumbnail_generation = std::max(item.thumbnail_generation, kCameraPreviewGeneration) + 1;
            item.thumbnail_is_camera_preview = false;
        }
    }
    if (item.edited != before.edited || item.thumbnail_generation != before.thumbnail_generation ||
        item.thumbnail_is_camera_preview != before.thumbnail_is_camera_preview || item.flags != before.flags) {
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
