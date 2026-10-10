#include "develop_job.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

#include <lucent/application/export_service.hpp>
#include <lucent/application/preview.hpp>
#include <lucent/application/recipe.hpp>
#include <lucent/application/source_item.hpp>
#include <lucent/application/source_renderer.hpp>
#include <lucent/core/file.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/core/text.hpp>
#include <lucent/core/win32_error.hpp>
#include <lucent/export/encode.hpp>
#include <lucent/export/export.hpp>
#include <lucent/raw/capture_metadata.hpp>
#include <lucent/workers/job_output.hpp>

#include "analysis_job.hpp"
#include "export_metadata.hpp"

namespace lucent::worker_app {

namespace {

using Clock = std::chrono::steady_clock;
using workers::JobStage;

constexpr std::string_view kWhere = "worker.job";
constexpr std::string_view kWhereRecipe = "worker.job.recipe";
constexpr std::string_view kWhereDestination = "worker.job.destination";
constexpr std::string_view kWhereSource = "worker.job.source";
constexpr std::string_view kWhereSupport = "worker.job.support";
constexpr std::string_view kWhereVerify = "worker.job.verify";
// Room for file system overhead beyond the uncompressed size (as app::export_item).
constexpr std::uint64_t kFileOverheadBytes = 1u << 20;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

std::wstring verbatim(const std::filesystem::path& path) {
    const std::wstring native = path.lexically_normal().native();
    if (!path.is_absolute() || native.starts_with(LR"(\\?\)")) {
        return native;
    }
    if (native.starts_with(LR"(\\)")) {
        return std::wstring(LR"(\\?\UNC\)") + native.substr(2);
    }
    return std::wstring(LR"(\\?\)") + native;
}

// ok() when nothing exists at `path`; AlreadyExists otherwise.
Status require_absent(const std::filesystem::path& path) {
    if (::GetFileAttributesW(verbatim(path).c_str()) != INVALID_FILE_ATTRIBUTES) {
        return fail(ErrorCode::AlreadyExists,
                    std::format("'{}' already exists; a job never replaces a file", core::path_to_utf8_lossy(path)),
                    std::string(kWhereDestination));
    }
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) {
        return ok();
    }
    return Unexpected(core::win32_error(error, std::string(kWhereDestination),
                                        std::format("cannot check '{}'", core::path_to_utf8_lossy(path))));
}

Status check_destination(const std::filesystem::path& temp_path) {
    const std::filesystem::path directory = temp_path.parent_path();
    const DWORD attributes = ::GetFileAttributesW(verbatim(directory).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return fail(ErrorCode::NotFound,
                    std::format("the output folder '{}' does not exist", core::path_to_utf8_lossy(directory)),
                    std::string(kWhereDestination));
    }
    if (auto absent = require_absent(temp_path); !absent) {
        return absent;
    }
    return require_absent(workers::job_scratch_directory(temp_path));
}

Status require_identity(const workers::JobSource& source, std::string_view when) {
    auto found = core::stat_file(source.path);
    if (!found) {
        return Unexpected(std::move(found.error()));
    }
    if (*found != source.identity) {
        return fail(ErrorCode::SourceChanged,
                    std::format("'{}' changed {} (expected {}, found {}); re-import it",
                                core::path_to_utf8_lossy(source.path), when, core::to_string(source.identity),
                                core::to_string(*found)),
                    std::string(kWhereSource));
    }
    return ok();
}

Status check_free_space(const std::filesystem::path& directory, std::uint64_t needed) {
    ULARGE_INTEGER available{};
    if (!::GetDiskFreeSpaceExW(verbatim(directory).c_str(), &available, nullptr, nullptr)) {
        return ok();  // unknown: the write itself reports DiskFull
    }
    if (available.QuadPart < needed) {
        return fail(ErrorCode::DiskFull,
                    std::format("the destination has {} bytes free; the output may need up to {}",
                                available.QuadPart, needed),
                    std::string(kWhereDestination));
    }
    return ok();
}

// Test hook render-delay: waits `ms`, returning early when the job is cancelled.
void delay(std::uint32_t ms, const core::CancellationToken& cancel) {
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < until && !cancel.is_cancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

std::string support_reasons(const raw::Interpretation& interpretation) {
    std::string text(raw::to_string(interpretation.status));
    for (const raw::InterpretationIssue& issue : interpretation.issues) {
        text += "; ";
        text += issue.message;
    }
    return text;
}

// Removes what the job created for temp_path unless dismissed: the scratch directory (created
// before the guard), and temp_path only once the job has moved its output there.
class OutputGuard {
public:
    explicit OutputGuard(std::filesystem::path temp_path) : temp_path_(std::move(temp_path)) {}
    ~OutputGuard() {
        if (!armed_) {
            return;
        }
        if (created_temp_) {
            (void)workers::remove_job_output(temp_path_);
        } else {
            (void)workers::remove_job_scratch_directory(temp_path_);
        }
    }
    OutputGuard(const OutputGuard&) = delete;
    OutputGuard& operator=(const OutputGuard&) = delete;
    void created_temp() noexcept { created_temp_ = true; }
    void dismiss() noexcept { armed_ = false; }

private:
    std::filesystem::path temp_path_;
    bool created_temp_ = false;
    bool armed_ = true;
};

#define LUCENT_TRY(target, expression)                                \
    auto target = (expression);                                       \
    if (!target) {                                                    \
        return Unexpected(std::move(target.error()));                 \
    }

#define LUCENT_TRY_STATUS(expression)                                 \
    if (auto lucent_status = (expression); !lucent_status) {          \
        return Unexpected(std::move(lucent_status.error()));          \
    }

}  // namespace

DevelopJobRunner::DevelopJobRunner(std::shared_ptr<const raw::DecoderRegistry> registry, DevelopJobOptions options)
    : registry_(std::move(registry)), options_(std::move(options)) {}

std::shared_ptr<const engine::LensDatabase> DevelopJobRunner::lens_database() {
    const std::lock_guard lock(lens_->mutex);
    if (!lens_->loaded) {
        lens_->loaded = true;
        if (options_.lens_database) {
            lens_->database = options_.lens_database();
        }
    }
    return lens_->database;
}

Expected<workers::AnalysisResult> DevelopJobRunner::analyze(const workers::AnalysisRequest& request,
                                                            workers::JobContext& context) {
    if (!registry_) {
        return fail(ErrorCode::InvalidArgument, "the worker has no decoder registry", std::string(kWhere));
    }
    AnalysisJobOptions options;
    options.decode_limits = options_.decode_limits;
    options.parallel = options_.parallel;
    return run_analysis_job(*registry_, request, context, options);
}

Expected<workers::JobResult> DevelopJobRunner::run(const workers::JobRequest& request, workers::JobContext& context) {
    const auto started = Clock::now();
    const core::CancellationToken& cancel = context.cancel();
    context.progress(JobStage::Preparing, 0.0);
    if (!registry_) {
        return fail(ErrorCode::InvalidArgument, "the worker has no decoder registry", std::string(kWhere));
    }
    workers::JobResult result;
    result.job_id = request.job_id;

    // ---- Preparing ------------------------------------------------------------------------------
    LUCENT_TRY(settings, app::read_recipe(request.recipe.document));
    LUCENT_TRY(canonical, app::write_recipe(*settings));
    if (canonical->json != request.recipe.document || canonical->sha256 != request.recipe.sha256) {
        return fail(ErrorCode::InvalidArgument,
                    "the job's recipe is not the canonical document of its settings, so its identity would not "
                    "name what is rendered",
                    std::string(kWhereRecipe));
    }
    result.recipe_sha256 = canonical->sha256;

    LUCENT_TRY_STATUS(check_destination(request.temp_path));
    LUCENT_TRY_STATUS(require_identity(request.source, "before the job"));
    if (request.source.sha256) {
        LUCENT_TRY(hashed, core::hash_file(request.source.path, cancel));
        if (hashed->identity != request.source.identity || hashed->digest != *request.source.sha256) {
            return fail(ErrorCode::SourceChanged,
                        std::format("the content of '{}' is not the expected version (SHA-256 {}); re-import it",
                                    core::path_to_utf8_lossy(request.source.path), core::to_hex(hashed->digest)),
                        std::string(kWhereSource));
        }
        result.source_sha256 = hashed->digest;
    }
    LUCENT_TRY_STATUS(cancel.check(kWhere));

    app::EngineRendererOptions renderer_options;
    renderer_options.decode_limits = options_.decode_limits;
    renderer_options.parallel = options_.parallel;
    renderer_options.lens_database = lens_database();
    if (request.memory_budget_bytes != 0) {
        renderer_options.memory_budget_bytes = request.memory_budget_bytes;
    }
    LUCENT_TRY(renderer, app::create_engine_renderer(registry_, renderer_options));
    app::SourceItem item;
    item.path = request.source.path;
    item.subimage = request.source.subimage;
    item.subimage_count = request.source.subimage + 1;
    item.identity = request.source.identity;
    item.sha256 = request.source.sha256;

    // Capture metadata: the file's own EXIF, completed from the decoder's probe. A file whose EXIF
    // cannot be read is still exported (with a note); a changed file is not.
    std::vector<std::string> metadata_notes;
    exporter::ExportMetadata metadata;
    const exporter::MetadataPolicy& policy = request.output_settings.metadata;
    if (policy.include_capture || policy.include_copyright || policy.include_gps) {
        raw::CaptureMetadata capture;
        if (auto read = raw::read_capture_metadata(request.source.path); read) {
            capture = std::move(*read);
        } else if (read.error().code == ErrorCode::SourceChanged || read.error().code == ErrorCode::OutOfMemory) {
            return Unexpected(std::move(read.error()));
        } else {
            metadata_notes.push_back(std::format("the source's EXIF was not read: {}", read.error().describe()));
        }
        raw::ProbeOptions probe_options;
        probe_options.subimage = request.source.subimage;
        probe_options.limits = options_.decode_limits;
        probe_options.describe_subimages = false;
        if (auto probe = registry_->select(request.source.path, probe_options, cancel); probe) {
            if (probe->probe.metadata.source.identity != request.source.identity) {
                return fail(ErrorCode::SourceChanged,
                            std::format("'{}' changed while its metadata was read; re-import it",
                                        core::path_to_utf8_lossy(request.source.path)),
                            std::string(kWhereSource));
            }
            raw::complete_from_decoder(capture, probe->probe.metadata);
        } else if (probe.error().code == ErrorCode::Cancelled) {
            return Unexpected(std::move(probe.error()));
        }
        for (std::string& warning : capture.warnings) {
            metadata_notes.push_back(std::move(warning));
        }
        metadata = make_export_metadata(capture, request.artist, request.copyright);
    }

    engine::StageOneRequest stage_one;
    stage_one.kind = engine::StageOneKind::Export;
    stage_one.white_balance = settings->white_balance;
    LUCENT_TRY(estimate, renderer->get()->estimate_for(item, stage_one, *settings, cancel));
    result.render.estimated_peak_bytes = estimate->export_peak;
    result.timings.prepare_seconds = seconds_since(started);
    context.progress(JobStage::Preparing, 0.05);

    // ---- Rendering and encoding -----------------------------------------------------------------
    const exporter::ExportSettings& output_settings = request.output_settings;
    const exporter::BitDepth depth = exporter::bit_depth_for(output_settings.format);
    exporter::OutputImage output;
    {
        const auto render_start = Clock::now();
        const std::uint32_t render_delay = context.test_hooks().render_delay_ms;
        auto rendered = (*renderer)->render(
            item, *settings,
            [&](double done) {
                context.progress(JobStage::Rendering, 0.05 + 0.65 * done);
                if (render_delay != 0) {
                    delay(render_delay, cancel);
                }
            },
            cancel);
        if (!rendered) {
            return Unexpected(std::move(rendered.error()));
        }
        result.timings.render_seconds = seconds_since(render_start);
        if (app::support_status_for(rendered->interpretation.status) == app::SupportStatus::Unsupported &&
            !request.allow_unsupported) {
            return fail(ErrorCode::Unsupported,
                        std::format("'{}' cannot be rendered faithfully: {}",
                                    core::path_to_utf8_lossy(request.source.path),
                                    support_reasons(rendered->interpretation)),
                        std::string(kWhereSupport));
        }
        const app::RenderSummary summary = app::summarize_render(rendered->report, rendered->interpretation);
        result.render.interpretation = summary.interpretation;
        result.render.warnings = summary.warnings;
        result.render.width = static_cast<std::uint32_t>(std::max(0, summary.rendered_size.width));
        result.render.height = static_cast<std::uint32_t>(std::max(0, summary.rendered_size.height));
        result.render.demosaic = summary.demosaic;
        result.render.camera_color = summary.camera_color;
        result.render.pipeline_version = summary.pipeline_version;
        result.render.look = summary.look;
        result.render.look_version = summary.look_version;
        result.render.clipped_raw_samples = summary.clipped_raw_samples;
        result.render.limited_display_pixels = summary.limited_display_pixels;

        context.progress(JobStage::Encoding, 0.72);
        const auto encode_start = Clock::now();
        LUCENT_TRY(encoded, exporter::encode_for_output(rendered->display.pixels.const_view(),
                                                        rendered->display.encoding, output_settings.profile, depth,
                                                        output_settings.transform, cancel));
        output = std::move(*encoded);
        result.timings.encode_seconds = seconds_since(encode_start);
    }  // the float render is released here

    // ---- Writing --------------------------------------------------------------------------------
    context.progress(JobStage::Writing, 0.8);
    const auto write_start = Clock::now();
    const std::uint64_t bytes_per_sample = depth == exporter::BitDepth::Sixteen ? 2 : 1;
    const std::uint64_t uncompressed = static_cast<std::uint64_t>(output.width) *
                                       static_cast<std::uint64_t>(output.height) * 3 * bytes_per_sample;
    LUCENT_TRY_STATUS(check_free_space(request.temp_path.parent_path(), uncompressed + kFileOverheadBytes));
    LUCENT_TRY_STATUS(workers::create_job_scratch_directory(request.temp_path));
    OutputGuard guard(request.temp_path);
    std::filesystem::path scratch_file = workers::job_scratch_directory(request.temp_path);
    scratch_file /= std::wstring(L"output") + std::wstring(app::export_extension(output_settings.format));
    LUCENT_TRY(receipt, exporter::write_verified(output, scratch_file, output_settings, metadata, cancel));
    output = exporter::OutputImage{};
    LUCENT_TRY_STATUS(cancel.check(kWhere));
    LUCENT_TRY_STATUS(workers::move_no_clobber(scratch_file, request.temp_path));
    guard.created_temp();
    LUCENT_TRY_STATUS(workers::remove_job_scratch_directory(request.temp_path));
    result.timings.write_seconds = seconds_since(write_start);

    // ---- Verifying ------------------------------------------------------------------------------
    context.progress(JobStage::Verifying, 0.95);
    const auto verify_start = Clock::now();
    LUCENT_TRY(rehashed, core::hash_file(request.temp_path, cancel));
    if (rehashed->digest != receipt->sha256 || rehashed->identity.size_bytes != receipt->byte_size) {
        return fail(ErrorCode::Corrupt,
                    std::format("'{}' does not hold the verified output after it was moved into place",
                                core::path_to_utf8_lossy(request.temp_path)),
                    std::string(kWhereVerify));
    }
    LUCENT_TRY_STATUS(require_identity(request.source, "during the job"));
    result.timings.verify_seconds = seconds_since(verify_start);
    guard.dismiss();

    workers::JobOutput& out = result.output;
    out.temp_path = request.temp_path;
    out.sha256 = receipt->sha256;
    out.size_bytes = receipt->byte_size;
    out.format = receipt->format;
    out.profile = receipt->profile;
    out.bit_depth = receipt->bit_depth;
    out.width = static_cast<std::uint32_t>(receipt->width);
    out.height = static_cast<std::uint32_t>(receipt->height);
    out.icc_sha256 = receipt->icc_sha256;
    if (receipt->format == exporter::ExportFormat::Jpeg) {
        out.chroma_subsampling = receipt->chroma_subsampling;
    }
    out.encoder = receipt->encoder;
    out.metadata_notes = receipt->metadata_notes;
    out.metadata_notes.insert(out.metadata_notes.end(), metadata_notes.begin(), metadata_notes.end());
    result.verification = workers::OutputVerification{true, true, true, true};
    result.source_identity = request.source.identity;
    result.timings.total_seconds = seconds_since(started);
    result.memory = workers::current_process_memory();
    context.progress(JobStage::Verifying, 1.0);
    return result;
}

#undef LUCENT_TRY
#undef LUCENT_TRY_STATUS

}  // namespace lucent::worker_app
