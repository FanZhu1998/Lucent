#include <algorithm>
#include <array>
#include <condition_variable>
#include <exception>
#include <chrono>
#include <format>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <lucent/application/batch_destination.hpp>
#include <lucent/application/batch_service.hpp>
#include <lucent/application/export_service.hpp>
#include <lucent/application/recipe_service.hpp>
#include <lucent/core/log.hpp>
#include <lucent/core/text.hpp>
#include <lucent/export/encode.hpp>
#include <lucent/export/metadata.hpp>

#include "win32_paths.hpp"

namespace lucent::app {

namespace {

constexpr std::string_view kWhere = "app.export";
constexpr std::string_view kWhereName = "app.export.name";
constexpr std::string_view kWhereDestination = "app.export.destination";
constexpr std::string_view kWhereSpace = "app.export.space";
constexpr std::string_view kWhereSupport = "app.export.support";
constexpr std::size_t kMaxComponentChars = 255;
// Allowance for container overhead (headers, ICC profile, TIFF directories) in the free-space
// check.
constexpr std::uint64_t kFileOverheadBytes = std::uint64_t{1} << 20;

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// Overall progress fraction for a render that is `done` (0..1) complete.
double render_fraction(double done) {
    return 0.05 + 0.65 * std::clamp(done, 0.0, 1.0);
}

class ProgressReporter {
public:
    ProgressReporter(ItemId item, const ExportProgressCallback& callback)
        : item_(item), callback_(callback) {}

    void report(ExportStage stage, double fraction) {
        if (!callback_) {
            return;
        }
        fraction_ = std::max(fraction_, std::clamp(fraction, 0.0, 1.0));
        try {
            callback_(ExportProgress{item_, stage, fraction_});
        } catch (const std::exception& e) {
            core::log_error(kWhere, "an export progress callback threw: {}", e.what());
        } catch (...) {
            core::log_error(kWhere, "an export progress callback threw an unknown exception");
        }
    }

private:
    ItemId item_;
    const ExportProgressCallback& callback_;
    double fraction_ = 0.0;
};

std::string reasons_text(const SupportAssessment& support) {
    std::string text;
    for (const SupportReason& reason : support.reasons) {
        text += text.empty() ? reason.message : "; " + reason.message;
    }
    return text;
}

// ---- Exports through a batch of one (W7) --------------------------------------------------------

ExportStage export_stage_of(workers::JobStage stage) {
    switch (stage) {
        case workers::JobStage::Preparing:
            return ExportStage::Preparing;
        case workers::JobStage::Rendering:
            return ExportStage::Rendering;
        case workers::JobStage::Encoding:
            return ExportStage::Encoding;
        case workers::JobStage::Writing:
        case workers::JobStage::Verifying:
            return ExportStage::Writing;
    }
    return ExportStage::Preparing;
}

bool batch_finished(const catalog::BatchRecord& record) {
    return record.state == catalog::BatchState::Completed ||
           (record.state == catalog::BatchState::Cancelled && record.counts.leased() == 0);
}

// The settings can be reproduced from the catalog after a restart: a recipe revision, or as shot.
bool reproducible(const ExportRequest& request) {
    if (!request.settings) {
        return false;
    }
    if (request.recipe_revision) {
        return true;
    }
    auto given = write_recipe(*request.settings);
    auto as_shot = write_recipe(engine::DevelopSettings{});
    return given && as_shot && given->sha256 == as_shot->sha256;
}

}  // namespace

Expected<RecipeIdentity> recipe_identity(const engine::DevelopSettings& settings,
                                         std::optional<catalog::RevisionId> revision,
                                         std::uint32_t revision_number) {
    auto document = write_recipe(settings);
    if (!document) {
        return Unexpected(std::move(document.error()));
    }
    RecipeIdentity identity;
    identity.revision = revision;
    identity.revision_number = revision ? revision_number : 0;
    identity.schema = std::string(kRecipeSchema);
    identity.schema_version = settings.version;
    identity.settings_json = std::move(document->json);
    identity.settings_sha256 = document->sha256;
    identity.operations = settings.operations;
    identity.base_look = settings.base_look.id;
    identity.base_look_version = settings.base_look.version;
    identity.pipeline_version = engine::kDevelopPipelineVersion;
    return identity;
}

std::string_view to_string(ExportStage stage) noexcept {
    switch (stage) {
        case ExportStage::Preparing:
            return "preparing";
        case ExportStage::Rendering:
            return "rendering";
        case ExportStage::Encoding:
            return "encoding";
        case ExportStage::Writing:
            return "writing";
        case ExportStage::Finished:
            return "finished";
    }
    return "unknown";
}

std::wstring_view export_extension(exporter::ExportFormat format) noexcept {
    switch (format) {
        case exporter::ExportFormat::Jpeg:
            return L".jpg";
        case exporter::ExportFormat::Tiff:
            return L".tif";
    }
    return {};
}

Expected<std::filesystem::path> export_file_name(const std::filesystem::path& source, std::uint32_t subimage,
                                                 std::uint32_t subimage_count, exporter::ExportFormat format,
                                                 std::uint32_t index) {
    const std::wstring_view extension = export_extension(format);
    if (extension.empty()) {
        return fail(ErrorCode::InvalidArgument, "unknown export format", std::string(kWhereName));
    }
    if (index == 0) {
        return fail(ErrorCode::InvalidArgument, "name indices start at 1", std::string(kWhereName));
    }
    if (index > kMaxExportNameIndex) {
        return fail(ErrorCode::LimitExceeded,
                    std::format("name index {} is above {}", index, kMaxExportNameIndex),
                    std::string(kWhereName));
    }
    if (subimage_count == 0 || subimage >= subimage_count) {
        return fail(ErrorCode::InvalidArgument, std::format("image {} of {}", subimage, subimage_count),
                    std::string(kWhereName));
    }
    if (!source.has_filename()) {
        return fail(ErrorCode::InvalidArgument, "the source path has no file name", std::string(kWhereName));
    }
    std::wstring stem = source.stem().native();
    if (stem.empty()) {
        stem = L"image";
    }
    if (is_reserved_device_name(stem)) {
        stem.insert(stem.begin(), L'_');
    }
    std::wstring suffix;
    if (subimage_count > 1) {
        suffix += L"_img" + std::to_wstring(subimage + 1);
    }
    suffix += L"_Lucent";
    if (index >= 2) {
        suffix += L"_" + std::to_wstring(index);
    }
    suffix += extension;
    if (stem.size() + suffix.size() > kMaxComponentChars) {
        std::size_t keep = kMaxComponentChars - suffix.size();
        // Never split a surrogate pair: drop a trailing high surrogate.
        if (keep > 0 && stem[keep - 1] >= 0xD800 && stem[keep - 1] <= 0xDBFF) {
            --keep;
        }
        stem.resize(keep);
    }
    return std::filesystem::path(stem + suffix);
}

Expected<ChosenName> choose_export_name(const std::filesystem::path& directory,
                                        const std::filesystem::path& source, std::uint32_t subimage,
                                        std::uint32_t subimage_count, exporter::ExportFormat format,
                                        std::uint32_t first_index) {
    auto entries = detail::list_directory(directory, kWhereDestination);
    if (!entries) {
        return Unexpected(std::move(entries.error()));
    }
    std::unordered_set<std::wstring> taken;
    for (const detail::DirectoryEntry& entry : *entries) {
        taken.insert(detail::fold_case(entry.name));
    }
    for (std::uint32_t index = std::max<std::uint32_t>(1, first_index); index <= kMaxExportNameIndex;
         ++index) {
        auto name = export_file_name(source, subimage, subimage_count, format, index);
        if (!name) {
            return Unexpected(std::move(name.error()));
        }
        if (!taken.contains(detail::fold_case(name->native()))) {
            return ChosenName{directory / *name, index};
        }
    }
    return fail(ErrorCode::LimitExceeded,
                std::format("every export name up to index {} is taken in '{}'", kMaxExportNameIndex,
                            core::path_to_utf8_lossy(directory)),
                std::string(kWhereName));
}

Expected<ExportRecord> export_item(const SourceItem& item, const ExportRequest& request,
                                   const SourceRenderer& renderer, MemoryAdmission* admission, Lane lane,
                                   const ExportProgressCallback& progress,
                                   const core::CancellationToken& cancel) {
    const auto start = std::chrono::steady_clock::now();
    ProgressReporter reporter(item.id, progress);
    reporter.report(ExportStage::Preparing, 0.0);

    if (!request.settings) {
        return fail(ErrorCode::InvalidArgument, "the export's recipe was not resolved", "app.export.recipe");
    }
    const engine::DevelopSettings& settings = *request.settings;
    auto identity = recipe_identity(settings, request.recipe_revision, request.recipe_revision_number);
    if (!identity) {
        return Unexpected(std::move(identity.error()));
    }
    if (auto valid = request.output.validate(); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    if (request.output_directory.empty()) {
        return fail(ErrorCode::InvalidArgument, "no output directory was given",
                    std::string(kWhereDestination));
    }
    if (item.support.status == SupportStatus::Unsupported && !request.allow_unsupported) {
        return fail(ErrorCode::Unsupported,
                    std::format("'{}' is not supported: {}", core::path_to_utf8_lossy(item.path),
                                reasons_text(item.support)),
                    std::string(kWhereSupport));
    }
    if (!detail::is_directory(request.output_directory)) {
        return fail(ErrorCode::NotFound,
                    std::format("the output directory '{}' does not exist",
                                core::path_to_utf8_lossy(request.output_directory)),
                    std::string(kWhereDestination));
    }
    auto directory = detail::canonical_path(request.output_directory, kWhereDestination);
    if (!directory) {
        return Unexpected(std::move(directory.error()));
    }
    if (auto status = cancel.check(std::string(kWhere)); !status) {
        return Unexpected(std::move(status.error()));
    }

    const exporter::BitDepth depth = exporter::bit_depth_for(request.output.format);
    const std::uint64_t bytes_per_pixel = depth == exporter::BitDepth::Sixteen ? 6 : 3;
    const image::Size upright = item.probe ? item.probe->upright_size : image::Size{};
    const std::uint64_t output_bytes =
        upright.empty() ? 0 : static_cast<std::uint64_t>(upright.area()) * bytes_per_pixel;
    if (output_bytes > 0) {
        auto free_bytes = detail::free_disk_bytes(*directory, kWhereSpace);
        if (free_bytes && *free_bytes < output_bytes + kFileOverheadBytes) {
            return fail(ErrorCode::DiskFull,
                        std::format("the destination has {} bytes free; the export may need up to {}",
                                    *free_bytes, output_bytes + kFileOverheadBytes),
                        std::string(kWhereSpace));
        }
    }

    engine::StageOneRequest export_request;
    export_request.kind = engine::StageOneKind::Export;
    export_request.white_balance = settings.white_balance;
    auto estimate = renderer.estimate(item, export_request, cancel);
    if (!estimate) {
        return Unexpected(std::move(estimate.error()));
    }
    const std::uint64_t admitted_bytes = estimate->export_peak + output_bytes;
    AdmissionLease lease;
    if (admission != nullptr) {
        auto admitted = admission->acquire(admitted_bytes, lane, cancel);
        if (!admitted) {
            return Unexpected(std::move(admitted.error()));
        }
        lease = std::move(*admitted);
    }

    ExportRecord record;
    record.item = item.id;
    record.source_path = item.path;
    record.subimage = item.subimage;
    record.source_identity = item.identity;
    record.source_sha256 = item.sha256;
    record.settings_revision = request.settings_revision;
    record.settings = settings;
    record.recipe = std::move(*identity);
    record.support = item.support.status;

    reporter.report(ExportStage::Rendering, 0.05);
    const auto render_start = std::chrono::steady_clock::now();
    exporter::OutputImage output;
    {
        auto rendered = renderer.render(
            item, settings,
            [&reporter](double done) { reporter.report(ExportStage::Rendering, render_fraction(done)); },
            cancel);
        if (!rendered) {
            return Unexpected(std::move(rendered.error()));
        }
        record.render_seconds = seconds_since(render_start);
        record.render = summarize_render(rendered->report, rendered->interpretation);
        record.render.estimated_peak_bytes = admitted_bytes;

        reporter.report(ExportStage::Encoding, 0.72);
        const auto encode_start = std::chrono::steady_clock::now();
        auto encoded =
            exporter::encode_for_output(rendered->display.pixels.const_view(), rendered->display.encoding,
                                        request.output.profile, depth, request.output.transform, cancel);
        if (!encoded) {
            return Unexpected(std::move(encoded.error()));
        }
        output = std::move(*encoded);
        record.encode_seconds = seconds_since(encode_start);
    }  // the float render result is released here
    lease.release();

    reporter.report(ExportStage::Writing, 0.8);
    const auto write_start = std::chrono::steady_clock::now();
    std::uint32_t first_index = 1;
    for (std::uint32_t attempt = 1; attempt <= kMaxPublicationAttempts; ++attempt) {
        auto chosen = choose_export_name(*directory, item.path, item.subimage, item.subimage_count,
                                         request.output.format, first_index);
        if (!chosen) {
            return Unexpected(std::move(chosen.error()));
        }
        if (detail::same_path_text(chosen->path, item.path)) {
            return fail(ErrorCode::Internal, "the export name equals the source path",
                        std::string(kWhereName));
        }
        auto receipt = exporter::write_verified(output, chosen->path, request.output, cancel);
        if (receipt) {
            record.receipt = std::move(*receipt);
            record.name_index = chosen->index;
            record.publication_attempts = attempt;
            record.write_seconds = seconds_since(write_start);
            record.total_seconds = seconds_since(start);
            record.finished_at = std::chrono::system_clock::now();
            reporter.report(ExportStage::Finished, 1.0);
            return record;
        }
        if (receipt.error().code != ErrorCode::AlreadyExists) {
            return Unexpected(std::move(receipt.error()));
        }
        // Another file took the name after the listing: never overwrite, take the next one.
        first_index = chosen->index + 1;
    }
    return fail(ErrorCode::AlreadyExists,
                std::format("no free export name after {} publication attempts", kMaxPublicationAttempts),
                std::string(kWhereName));
}

namespace {

Expected<ExportRecord> export_through_batch(BatchService& batches, Library& library, const SourceItem& item,
                                            const ExportRequest& request, const ExportProgressCallback& progress,
                                            const core::CancellationToken& cancel) {
    using namespace std::chrono_literals;
    const auto start = std::chrono::steady_clock::now();
    ProgressReporter reporter(item.id, progress);
    reporter.report(ExportStage::Preparing, 0.0);
    if (!request.settings) {
        return fail(ErrorCode::InvalidArgument, "the export's recipe was not resolved", "app.export.recipe");
    }
    auto identity = recipe_identity(*request.settings, request.recipe_revision, request.recipe_revision_number);
    if (!identity) {
        return Unexpected(std::move(identity.error()));
    }
    if (auto valid = request.output.validate(); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    if (request.output_directory.empty()) {
        return fail(ErrorCode::InvalidArgument, "no output directory was given", std::string(kWhereDestination));
    }
    // The checks of export_item(), with its errors, before anything is written.
    if (item.support.status == SupportStatus::Unsupported && !request.allow_unsupported) {
        return fail(ErrorCode::Unsupported,
                    std::format("'{}' is not supported: {}", core::path_to_utf8_lossy(item.path),
                                reasons_text(item.support)),
                    std::string(kWhereSupport));
    }
    if (!detail::is_directory(request.output_directory)) {
        return fail(ErrorCode::NotFound,
                    std::format("the output directory '{}' does not exist",
                                core::path_to_utf8_lossy(request.output_directory)),
                    std::string(kWhereDestination));
    }
    if (item.source_state == SourceState::Missing) {
        return fail(ErrorCode::NotFound, std::format("'{}' is missing", core::path_to_utf8_lossy(item.path)),
                    "app.export.source");
    }
    if (item.source_state == SourceState::Changed) {
        return fail(ErrorCode::SourceChanged,
                    std::format("'{}' changed after it was added; add it again", core::path_to_utf8_lossy(item.path)),
                    "app.export.source");
    }

    BatchRequest batch;
    batch.name = "Export of " + core::path_to_utf8_lossy(item.path.filename());
    batch.items.push_back(BatchItem{item.id, FrozenRecipe{*request.settings, request.recipe_revision,
                                                          request.recipe_revision_number}});
    batch.destination.folder = request.output_directory;
    batch.destination.name_pattern = std::string(kSingleExportNamePattern);
    batch.destination.collisions = CollisionPolicy::UniqueName;
    batch.output = request.output;
    batch.existing_outputs = ExistingOutputs::ExportAgain;
    batch.allow_unsupported = request.allow_unsupported;
    batch.write_report_files = false;

    struct Shared {
        std::mutex mutex;
        std::condition_variable changed;
        std::optional<catalog::BatchId> id;
        bool finished = false;
        std::optional<ExportProgress> progress;
        std::optional<Error> attention;
    };
    auto shared = std::make_shared<Shared>();
    Subscription subscription = batches.subscribe([shared, item_id = item.id](const BatchEvent& event) {
        {
            std::lock_guard lock(shared->mutex);
            if (!shared->id || event.batch != *shared->id) {
                return;
            }
            switch (event.kind) {
                case BatchEventKind::JobProgress:
                    shared->progress =
                        ExportProgress{item_id, export_stage_of(event.worker_stage), 0.02 + 0.95 * event.fraction};
                    break;
                case BatchEventKind::Attention:
                    shared->attention = event.error.value_or(
                        Error{ErrorCode::IoError, "the destination cannot be used", "app.export.destination", 0});
                    break;
                case BatchEventKind::Finished:
                    shared->finished = true;
                    break;
                default:
                    return;
            }
        }
        shared->changed.notify_all();
    });
    auto submitted = batches.submit(batch, {}, cancel);
    if (!submitted) {
        return Unexpected(std::move(submitted.error()));
    }
    const catalog::BatchId id = submitted->id;
    {
        std::lock_guard lock(shared->mutex);
        shared->id = id;
    }
    bool cancel_sent = false;
    std::optional<Error> attention;
    for (std::uint32_t tick = 1;; ++tick) {
        std::optional<ExportProgress> step;
        bool finished = false;
        {
            std::unique_lock lock(shared->mutex);
            shared->changed.wait_for(lock, 200ms, [&] {
                return shared->finished || shared->progress || (shared->attention && !attention) ||
                       (cancel.is_cancelled() && !cancel_sent);
            });
            finished = shared->finished;
            step = std::exchange(shared->progress, std::nullopt);
            if (shared->attention && !attention) {
                attention = shared->attention;
            }
        }
        if (step) {
            reporter.report(step->stage, step->fraction);
        }
        if (finished) {
            break;
        }
        if ((cancel.is_cancelled() || attention) && !cancel_sent) {
            // A single export does not wait for a full disk or a reconnected drive: it fails, as in W3.
            cancel_sent = true;
            if (Status cancelled = batches.cancel(id); !cancelled) {
                core::log_warn(kWhere, "cannot cancel batch {}: {}", id.value, cancelled.error().describe());
            }
        }
        if (tick % 10 == 0) {  // a backstop for a missed event
            if (auto status = batches.status(id); status && batch_finished(status->record)) {
                break;
            }
        }
    }
    subscription.reset();
    auto report = batches.report(id);
    if (!report) {
        return Unexpected(std::move(report.error()));
    }
    const ReportEntry* entry = nullptr;
    for (const ReportEntry& candidate : report->entries) {
        if (candidate.item == item.id) {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr || entry->outcome != ReportOutcome::Exported || !entry->output || !entry->details) {
        if (attention) {
            return Unexpected(std::move(*attention));
        }
        if (cancel.is_cancelled() || (entry != nullptr && entry->outcome == ReportOutcome::Cancelled)) {
            return fail(ErrorCode::Cancelled, "the export was cancelled", std::string(kWhere));
        }
        if (entry != nullptr && entry->error) {
            return Unexpected(*entry->error);
        }
        return fail(ErrorCode::Internal, "the export's batch ended without a result", std::string(kWhere));
    }
    const ReceiptDetails& details = *entry->details;
    ExportRecord record;
    record.item = item.id;
    record.source_path = item.path;
    record.subimage = item.subimage;
    record.source_identity = item.identity;
    record.source_sha256 = item.sha256;
    if (std::optional<SourceItem> now = library.find(item.id); now && now->sha256) {
        record.source_sha256 = now->sha256;  // hashed for the batch when it had no hash yet
    }
    record.settings_revision = request.settings_revision;
    record.settings = *request.settings;
    record.recipe = std::move(*identity);
    record.support = item.support.status;
    exporter::ExportReceipt& receipt = record.receipt;
    receipt.path = *entry->output;
    receipt.format = details.format;
    receipt.profile = details.profile;
    receipt.bit_depth = details.bit_depth;
    receipt.width = static_cast<std::int32_t>(details.width);
    receipt.height = static_cast<std::int32_t>(details.height);
    receipt.byte_size = entry->size_bytes;
    receipt.sha256 = entry->sha256.value_or(core::Digest256{});
    receipt.icc_sha256 = details.icc_sha256;
    receipt.settings = details.settings;
    receipt.chroma_subsampling = details.chroma_subsampling.value_or(exporter::ChromaSubsampling::Yuv444);
    receipt.encoder = details.encoder;
    if (auto metadata = exporter::read_export_metadata(receipt.path)) {
        receipt.metadata = std::move(*metadata);
    }
    receipt.metadata_notes = details.metadata_notes;
    record.name_index = details.name_index;
    record.publication_attempts = details.attempt;
    RenderSummary& render = record.render;
    render.pipeline_version = details.pipeline_version;
    render.look = details.look;
    render.look_version = details.look_version;
    render.interpretation = details.interpretation;
    render.warnings = details.warnings;
    render.rendered_size =
        image::Size{static_cast<std::int32_t>(details.width), static_cast<std::int32_t>(details.height)};
    render.demosaic = details.demosaic;
    render.camera_color = details.camera_color;
    render.clipped_raw_samples = details.clipped_raw_samples;
    render.limited_display_pixels = details.limited_display_pixels;
    render.estimated_peak_bytes = details.estimated_peak_bytes;
    render.render_seconds = details.render_seconds;
    record.render_seconds = details.render_seconds;
    record.encode_seconds = details.encode_seconds;
    record.write_seconds = details.write_seconds + details.verify_seconds;
    record.total_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    record.finished_at = std::chrono::system_clock::now();
    reporter.report(ExportStage::Finished, 1.0);
    return record;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// ExportService
// ---------------------------------------------------------------------------------------------

struct ExportService::State {
    State(Library& l, TaskExecutor& e, std::shared_ptr<const SourceRenderer> r, RecipeService* rs, BatchService* b)
        : library(l), executor(e), renderer(std::move(r)), recipes(rs), batches(b) {}

    Library& library;
    TaskExecutor& executor;
    std::shared_ptr<const SourceRenderer> renderer;
    RecipeService* recipes;
    BatchService* batches;

    std::mutex mutex;
    std::condition_variable idle;
    bool shut_down = false;
    std::uint64_t outstanding = 0;
    std::vector<TaskHandle> active;

    void run(const TaskContext& context, const SourceItem& item, const ExportRequest& request,
             const ExportProgressCallback& progress, const ExportCompletion& completion,
             std::uint64_t task_id) {
        Expected<ExportRecord> result =
            fail(ErrorCode::Cancelled, "the export was cancelled", std::string(kWhere));
        try {
            if (batches != nullptr && library.capabilities().persistent && reproducible(request)) {
                result = export_through_batch(*batches, library, item, request, progress, context.cancel());
            } else {
                result = export_item(item, request, *renderer, &context.admission(), context.lane(), progress,
                                     context.cancel());
            }
        } catch (const std::bad_alloc&) {
            result = fail(ErrorCode::OutOfMemory, "the export ran out of memory", std::string(kWhere));
        } catch (const std::exception& e) {
            result = fail(ErrorCode::Internal, std::format("the export raised: {}", e.what()),
                          std::string(kWhere));
        }
        {
            std::lock_guard lock(mutex);
            std::erase_if(active, [task_id](const TaskHandle& h) { return h.id() == task_id; });
        }
        try {
            completion(item.id, result);
        } catch (const std::exception& e) {
            core::log_error(kWhere, "an export completion threw: {}", e.what());
        } catch (...) {
            core::log_error(kWhere, "an export completion threw an unknown exception");
        }
        {
            std::lock_guard lock(mutex);
            --outstanding;
        }
        idle.notify_all();
    }
};

ExportService::ExportService(Library& library, TaskExecutor& executor,
                             std::shared_ptr<const SourceRenderer> renderer, RecipeService* recipes,
                             BatchService* batches)
    : state_(std::make_shared<State>(library, executor, std::move(renderer), recipes, batches)) {}

ExportService::~ExportService() {
    std::unique_lock lock(state_->mutex);
    state_->shut_down = true;
    for (const TaskHandle& handle : state_->active) {
        handle.cancel();
    }
    state_->idle.wait(lock, [&] { return state_->outstanding == 0; });
}

Expected<TaskHandle> ExportService::submit(const ExportRequest& request, ExportProgressCallback progress,
                                           ExportCompletion completion) {
    if (!completion) {
        return fail(ErrorCode::InvalidArgument, "the export completion is empty", std::string(kWhere));
    }
    if (!state_->renderer) {
        return fail(ErrorCode::InvalidArgument, "the export service has no renderer", std::string(kWhere));
    }
    if (auto valid = request.output.validate(); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    std::optional<SourceItem> item = state_->library.find(request.item);
    if (!item) {
        return fail(ErrorCode::NotFound, std::format("no library item {}", request.item.value),
                    std::string(kWhere));
    }
    // Freeze the recipe now: a later edit never changes a queued export.
    ExportRequest frozen = request;
    if (frozen.settings) {
        if (auto valid = frozen.settings->validate(); !valid) {
            return Unexpected(std::move(valid.error()));
        }
    } else if (state_->recipes != nullptr) {
        auto current = state_->recipes->current_settings(request.item);
        if (!current) {
            return Unexpected(std::move(current.error()));
        }
        frozen.settings = std::move(current->settings);
        frozen.recipe_revision = current->revision;
        frozen.recipe_revision_number = current->revision_number;
    } else {
        frozen.settings = engine::DevelopSettings{};
    }
    try {
        std::lock_guard lock(state_->mutex);
        if (state_->shut_down) {
            return fail(ErrorCode::Cancelled, "the export service is shutting down", std::string(kWhere));
        }
        // The task learns its own id from a shared cell filled in once the handle exists.
        auto task_id = std::make_shared<std::uint64_t>(0);
        auto submitted = state_->executor.submit(
            Lane::Background, [state = state_, item = std::move(*item), request = std::move(frozen),
                               progress = std::move(progress), completion = std::move(completion),
                               task_id](TaskContext& context) {
                std::uint64_t id = 0;
                {
                    std::lock_guard lock(state->mutex);
                    id = *task_id;
                }
                state->run(context, item, request, progress, completion, id);
            });
        if (!submitted) {
            return Unexpected(std::move(submitted.error()));
        }
        *task_id = submitted->id();
        state_->active.push_back(*submitted);
        ++state_->outstanding;
        return *submitted;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot queue the export", std::string(kWhere));
    }
}

}  // namespace lucent::app
