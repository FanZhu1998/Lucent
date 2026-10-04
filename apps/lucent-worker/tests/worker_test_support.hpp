#pragma once

// Helpers for the lucent-worker tests: job requests for real files, an in-process JobContext for
// DevelopJobRunner, host options pointing at the built lucent-worker.exe, and file checks.

#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include <lucent/application/recipe.hpp>
#include <lucent/core/cancellation.hpp>
#include <lucent/core/file.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/engine/develop_settings.hpp>
#include <lucent/raw/decoder_registry.hpp>
#include <lucent/workers/protocol.hpp>
#include <lucent/workers/worker_host.hpp>
#include <lucent/workers/worker_server.hpp>

#include "develop_job.hpp"
#include "raw_test_support.hpp"
#include "synthetic_dng.hpp"

namespace lucent::worker_app::test_support {

namespace rts = lucent::raw::test_support;
using namespace std::chrono_literals;

inline std::filesystem::path worker_exe() { return std::filesystem::path(LUCENT_WORKER_EXE).make_preferred(); }

inline std::shared_ptr<const raw::DecoderRegistry> default_registry() {
    auto registry = raw::create_default_registry();
    EXPECT_TRUE(registry);
    return std::make_shared<const raw::DecoderRegistry>(std::move(*registry));
}

inline std::filesystem::path write_dng(const rts::ScratchDir& dir, const std::filesystem::path& name,
                                       const rts::SyntheticDngSpec& spec = {}) {
    return dir.write(name, rts::build_synthetic_dng(spec));
}

// A job for `source` as it is now, rendering `settings` into `temp_path`.
inline workers::JobRequest make_job(const std::filesystem::path& source, const std::filesystem::path& temp_path,
                                    std::string job_id = "job-1", const engine::DevelopSettings& settings = {}) {
    workers::JobRequest job;
    job.job_id = std::move(job_id);
    job.source.path = source;
    auto identity = core::stat_file(source);
    EXPECT_TRUE(identity) << (identity ? "" : identity.error().describe());
    if (identity) {
        job.source.identity = *identity;
    }
    auto recipe = app::write_recipe(settings);
    EXPECT_TRUE(recipe);
    if (recipe) {
        job.recipe = workers::JobRecipe{recipe->json, recipe->sha256};
    }
    job.temp_path = temp_path;
    return job;
}

// The JobContext the worker's server provides, for running DevelopJobRunner in-process.
class LocalContext final : public workers::JobContext {
public:
    explicit LocalContext(workers::TestHooks hooks = {}) : hooks_(std::move(hooks)) {}

    const core::CancellationToken& cancel() const noexcept override { return token_; }
    void progress(workers::JobStage stage, double fraction) override {
        std::function<void(workers::JobStage, double)> callback;
        {
            std::lock_guard lock(mutex_);
            reports.emplace_back(stage, fraction);
            callback = on_progress;
        }
        if (callback) {
            callback(stage, fraction);
        }
    }
    const workers::TestHooks& test_hooks() const noexcept override { return hooks_; }

    core::CancellationSource source;
    std::function<void(workers::JobStage, double)> on_progress;
    std::vector<std::pair<workers::JobStage, double>> reports;

private:
    workers::TestHooks hooks_;
    core::CancellationToken token_ = source.token();
    std::mutex mutex_;
};

// Options for a host of the built worker with short, test-friendly intervals.
inline workers::WorkerHostOptions host_options(std::vector<std::string> hooks = {}) {
    workers::WorkerHostOptions options;
    options.executable = worker_exe();
    options.test_hooks = std::move(hooks);
    options.heartbeat_interval = 100ms;
    options.heartbeat_timeout = 10s;
    options.cancel_grace = 5s;
    options.shutdown_grace = 5s;
    return options;
}

inline bool path_exists(const std::filesystem::path& path) {
    return ::GetFileAttributesW(rts::verbatim(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

inline core::Digest256 digest_of(std::string_view text) {
    auto digest = core::sha256(std::as_bytes(std::span(text.data(), text.size())));
    EXPECT_TRUE(digest);
    return digest ? *digest : core::Digest256{};
}

// SHA-256 of a file (fails the test when it cannot be read).
inline core::Digest256 file_digest(const std::filesystem::path& path) {
    auto hashed = core::hash_file(path, {});
    EXPECT_TRUE(hashed) << (hashed ? "" : hashed.error().describe());
    return hashed ? hashed->digest : core::Digest256{};
}

// Waits up to `timeout` for the process `pid` (opened beforehand) to end; its exit code, or nothing.
inline std::optional<DWORD> wait_for_exit(HANDLE process, std::chrono::milliseconds timeout) {
    if (::WaitForSingleObject(process, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) {
        return std::nullopt;
    }
    DWORD code = 0;
    ::GetExitCodeProcess(process, &code);
    return code;
}

}  // namespace lucent::worker_app::test_support
