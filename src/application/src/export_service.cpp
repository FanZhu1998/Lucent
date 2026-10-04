#include <algorithm>
#include <array>
#include <condition_variable>
#include <exception>
#include <format>
#include <mutex>
#include <new>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <lucent/application/export_service.hpp>
#include <lucent/application/recipe_service.hpp>
#include <lucent/core/log.hpp>
#include <lucent/core/text.hpp>
#include <lucent/export/encode.hpp>

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

bool is_reserved_device_name(std::wstring_view stem) {
    const std::wstring base = detail::fold_case(stem.substr(0, stem.find(L'.')));
    static constexpr std::array<std::wstring_view, 4> kPlain = {L"CON", L"PRN", L"AUX", L"NUL"};
    if (std::find(kPlain.begin(), kPlain.end(), base) != kPlain.end()) {
        return true;
    }
    if (base.size() == 4 && (base.starts_with(L"COM") || base.starts_with(L"LPT"))) {
        const wchar_t digit = base[3];
        // Windows also reserves the superscript digits 1-3 after COM / LPT.
        return (digit >= L'0' && digit <= L'9') || digit == L'¹' || digit == L'²' || digit == L'³';
    }
    return false;
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

// ---------------------------------------------------------------------------------------------
// ExportService
// ---------------------------------------------------------------------------------------------

struct ExportService::State {
    State(Library& l, TaskExecutor& e, std::shared_ptr<const SourceRenderer> r, RecipeService* rs)
        : library(l), executor(e), renderer(std::move(r)), recipes(rs) {}

    Library& library;
    TaskExecutor& executor;
    std::shared_ptr<const SourceRenderer> renderer;
    RecipeService* recipes;

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
            result = export_item(item, request, *renderer, &context.admission(), context.lane(), progress,
                                 context.cancel());
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
                             std::shared_ptr<const SourceRenderer> renderer, RecipeService* recipes)
    : state_(std::make_shared<State>(library, executor, std::move(renderer), recipes)) {}

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
