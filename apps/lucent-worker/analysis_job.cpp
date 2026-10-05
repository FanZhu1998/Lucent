#include "analysis_job.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <thread>
#include <utility>

#include <lucent/application/auto_develop.hpp>
#include <lucent/application/recipe.hpp>
#include <lucent/core/file.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/core/text.hpp>

namespace lucent::worker_app {

namespace {

using Clock = std::chrono::steady_clock;
using workers::JobStage;

constexpr std::string_view kWhere = "worker.analysis";
constexpr std::string_view kWhereRecipe = "worker.job.recipe";
constexpr std::string_view kWhereRequest = "worker.analysis.request";
constexpr std::string_view kWhereSource = "worker.job.source";

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

}  // namespace

Expected<workers::AnalysisResult> run_analysis_job(const raw::DecoderRegistry& registry,
                                                   const workers::AnalysisRequest& request,
                                                   workers::JobContext& context, const AnalysisJobOptions& options) {
    const auto started = Clock::now();
    const core::CancellationToken& cancel = context.cancel();
    context.progress(JobStage::Analysing, 0.0);
    workers::AnalysisResult result;
    result.job_id = request.job_id;

    auto settings = app::read_recipe(request.recipe.document);
    if (!settings) {
        return Unexpected(std::move(settings.error()));
    }
    auto canonical = app::write_recipe(*settings);
    if (!canonical) {
        return Unexpected(std::move(canonical.error()));
    }
    if (canonical->json != request.recipe.document || canonical->sha256 != request.recipe.sha256) {
        return fail(ErrorCode::InvalidArgument,
                    "the analysis' starting recipe is not the canonical document of its settings",
                    std::string(kWhereRecipe));
    }
    result.recipe_sha256 = canonical->sha256;
    auto auto_request = app::read_auto_request(request.request);
    if (!auto_request) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("the analysis request cannot be read: {}", auto_request.error().message),
                    std::string(kWhereRequest));
    }
    auto_request->fail_final_checks =
        std::max(auto_request->fail_final_checks, context.test_hooks().fail_final_checks);
    // Test hook render-delay: the analysis waits once before it starts, observing cancellation.
    if (const std::uint32_t ms = context.test_hooks().render_delay_ms; ms != 0) {
        const auto until = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < until && !cancel.is_cancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    if (Status same = require_identity(request.source, "before the analysis"); !same) {
        return Unexpected(std::move(same.error()));
    }
    if (request.source.sha256) {
        auto hashed = core::hash_file(request.source.path, cancel);
        if (!hashed) {
            return Unexpected(std::move(hashed.error()));
        }
        if (hashed->identity != request.source.identity || hashed->digest != *request.source.sha256) {
            return fail(ErrorCode::SourceChanged,
                        std::format("the content of '{}' is not the expected version (SHA-256 {}); re-import it",
                                    core::path_to_utf8_lossy(request.source.path), core::to_hex(hashed->digest)),
                        std::string(kWhereSource));
        }
        result.source_sha256 = hashed->digest;
    }
    if (Status c = cancel.check(kWhere); !c) {
        return Unexpected(std::move(c.error()));
    }

    app::AutoRunOptions run_options;
    run_options.analysis.develop.decode_limits = options.decode_limits;
    run_options.analysis.develop.parallel = options.parallel;
    if (request.memory_budget_bytes != 0) {
        run_options.analysis.develop.memory_budget_bytes = request.memory_budget_bytes;
    }
    run_options.allow_unsupported = request.allow_unsupported;
    auto run = app::run_automatic(registry, request.source.path, request.source.subimage, *settings, *auto_request,
                                  run_options, cancel, [&](app::AutoRunStage stage, double fraction) {
                                      context.progress(stage == app::AutoRunStage::Analysing ? JobStage::Analysing
                                                                                             : JobStage::Checking,
                                                       fraction);
                                  });
    if (!run) {
        return Unexpected(std::move(run.error()));
    }
    if (Status same = require_identity(request.source, "during the analysis"); !same) {
        return Unexpected(std::move(same.error()));
    }
    auto document = app::write_auto_run(*run);
    if (!document) {
        return Unexpected(std::move(document.error()));
    }
    if (document->size() > workers::kMaxAnalysisDocumentBytes) {
        return fail(ErrorCode::LimitExceeded,
                    std::format("the analysis document has {} bytes, above the protocol's {}", document->size(),
                                workers::kMaxAnalysisDocumentBytes),
                    std::string(kWhere));
    }
    result.document = std::move(*document);
    result.source_identity = request.source.identity;
    result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
    result.memory = workers::current_process_memory();
    context.progress(auto_request->final_check ? JobStage::Checking : JobStage::Analysing, 1.0);
    return result;
}

}  // namespace lucent::worker_app
