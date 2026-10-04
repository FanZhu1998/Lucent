#pragma once

// Helpers for the lucent_workers unit tests: valid sample messages, an in-process worker server
// over real pipes with a scriptable runner, and a host end that reads with timeouts.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include <lucent/core/hash.hpp>
#include <lucent/workers/channel.hpp>
#include <lucent/workers/protocol.hpp>
#include <lucent/workers/worker_server.hpp>

namespace lucent::workers::test_support {

inline core::Digest256 digest_of(std::string_view text) {
    auto digest = core::sha256(std::as_bytes(std::span(text.data(), text.size())));
    EXPECT_TRUE(digest);
    return digest ? *digest : core::Digest256{};
}

inline JobRequest sample_job(std::string job_id = "job-1") {
    JobRequest job;
    job.job_id = std::move(job_id);
    job.source.path = LR"(C:\Photos\Ünïcode ✓\IMG_0001.CR3)";
    job.source.subimage = 0;
    job.source.identity.volume_serial = 0x0123456789ABCDEFull;
    for (std::size_t i = 0; i < job.source.identity.file_id.size(); ++i) {
        job.source.identity.file_id[i] = static_cast<std::uint8_t>(0xF0 + i);
    }
    job.source.identity.size_bytes = 40'632'406;
    job.source.identity.last_write_time = 133'700'000'000'000'000;
    job.recipe.document = R"({"schema":"lucent.develop","version":1})";
    job.recipe.sha256 = digest_of(job.recipe.document);
    job.temp_path = LR"(D:\Exports\~lucent-job-1.partial)";
    job.memory_budget_bytes = 3ull << 30;
    return job;
}

inline JobResult sample_result(std::string job_id = "job-1") {
    JobResult result;
    result.job_id = std::move(job_id);
    result.output.temp_path = LR"(D:\Exports\~lucent-job-1.partial)";
    result.output.sha256 = digest_of("output");
    result.output.size_bytes = 12'345'678;
    result.output.format = exporter::ExportFormat::Tiff;
    result.output.profile = exporter::OutputProfile::DisplayP3;
    result.output.bit_depth = exporter::BitDepth::Sixteen;
    result.output.width = 8192;
    result.output.height = 5464;
    result.output.icc_sha256 = digest_of("icc");
    result.output.encoder = "libtiff 4.7.0; lcms 2.17";
    result.output.metadata_notes = {"GPS dropped by policy"};
    result.verification = OutputVerification{true, true, true, true};
    result.source_identity = sample_job().source.identity;
    result.source_sha256 = digest_of("source");
    result.recipe_sha256 = digest_of("recipe");
    result.render.interpretation = raw::InterpretationStatus::OptionalOperationsSkipped;
    result.render.warnings = {"lens profile missing", "ünïcode warning"};
    result.render.width = 8192;
    result.render.height = 5464;
    result.render.demosaic = "malvar-he-cutler-2004";
    result.render.camera_color = "camera-to-xyz-dng_color_matrix";
    result.render.pipeline_version = 1;
    result.render.look = "neutral";
    result.render.look_version = 1;
    result.render.clipped_raw_samples = 17;
    result.render.limited_display_pixels = 42;
    result.render.estimated_peak_bytes = 1'234'567'890;
    result.timings = JobTimings{0.125, 2.5, 0.75, 1.0, 0.0625, 4.4375};
    result.memory = MemorySnapshot{100, 200, 300, 400};
    return result;
}

// A runner whose behaviour the test scripts. `behaviour` runs on the server's job thread.
class ScriptedRunner final : public JobRunner {
public:
    using Behaviour = std::function<Expected<JobResult>(const JobRequest&, JobContext&)>;
    explicit ScriptedRunner(Behaviour behaviour) : behaviour_(std::move(behaviour)) {}
    Expected<JobResult> run(const JobRequest& request, JobContext& context) override {
        ++calls;
        return behaviour_(request, context);
    }
    std::atomic<int> calls{0};

private:
    Behaviour behaviour_;
};

// Succeeds at once with sample_result(), after one progress report per stage.
inline Expected<JobResult> succeed(const JobRequest& request, JobContext& context) {
    context.progress(JobStage::Preparing, 0.0);
    context.progress(JobStage::Rendering, 0.5);
    context.progress(JobStage::Verifying, 1.0);
    return sample_result(request.job_id);
}

// Waits until the job is cancelled, then reports Cancelled.
inline Expected<JobResult> wait_for_cancel(const JobRequest&, JobContext& context) {
    context.progress(JobStage::Rendering, 0.1);
    while (!context.cancel().is_cancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return fail(ErrorCode::Cancelled, "cancelled", "test.runner");
}

// The host's end of an in-process server: reads on its own thread into a queue.
class HostEnd {
public:
    explicit HostEnd(std::unique_ptr<MessageChannel> channel) : channel_(std::move(channel)) {
        reader_ = std::thread([this] {
            for (;;) {
                auto received = channel_->receive();
                std::lock_guard lock(mutex_);
                if (!received || !*received) {
                    closed_ = true;
                    if (!received) {
                        failure_ = received.error();
                    }
                    changed_.notify_all();
                    return;
                }
                messages_.push_back(std::move(**received));
                changed_.notify_all();
            }
        });
    }
    ~HostEnd() {
        channel_->close_write();
        if (reader_.joinable()) {
            reader_.join();  // the worker side must be closed by the owner first
        }
    }

    MessageChannel& channel() { return *channel_; }

    // The next message that is not a heartbeat (or any message with `include_heartbeats`).
    std::optional<Message> next(std::chrono::milliseconds timeout = std::chrono::seconds(10),
                                bool include_heartbeats = false) {
        std::unique_lock lock(mutex_);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            while (!messages_.empty()) {
                Message message = std::move(messages_.front());
                messages_.pop_front();
                if (include_heartbeats || !std::holds_alternative<Heartbeat>(message)) {
                    return message;
                }
                ++heartbeats_;
            }
            if (closed_) {
                return std::nullopt;
            }
            if (!changed_.wait_until(lock, deadline, [&] { return !messages_.empty() || closed_; })) {
                return std::nullopt;
            }
        }
    }

    template <class T>
    std::optional<T> next_of(std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                                      std::chrono::steady_clock::now());
            if (left.count() <= 0) {
                return std::nullopt;
            }
            auto message = next(left, std::is_same_v<T, Heartbeat>);
            if (!message) {
                return std::nullopt;
            }
            if (auto* value = std::get_if<T>(&*message)) {
                return std::move(*value);
            }
        }
    }

    bool closed() {
        std::lock_guard lock(mutex_);
        return closed_;
    }

private:
    std::unique_ptr<MessageChannel> channel_;
    std::thread reader_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Message> messages_;
    bool closed_ = false;
    std::optional<Error> failure_;
    int heartbeats_ = 0;
};

// An in-process worker server over a new channel pair, run on its own thread.
class ServerHarness {
public:
    explicit ServerHarness(ScriptedRunner::Behaviour behaviour, WorkerServerOptions options = {})
        : runner_(std::move(behaviour)), options_(std::move(options)) {
        auto pair = create_channel_pair();
        EXPECT_TRUE(pair);
        worker_ = std::move(pair->worker);
        host_ = std::make_unique<HostEnd>(std::move(pair->host));
        server_ = std::thread([this] {
            exit_code_ = run_worker_server(*worker_, runner_, options_);
            std::lock_guard lock(mutex_);
            done_ = true;
            changed_.notify_all();
        });
    }
    ~ServerHarness() {
        host_->channel().close_write();
        if (server_.joinable()) {
            server_.join();
        }
        worker_.reset();  // the host reader sees the end
        host_.reset();
    }

    HostEnd& host() { return *host_; }
    ScriptedRunner& runner() { return runner_; }

    HostHello hello(std::uint32_t heartbeat_ms = 50) const {
        HostHello h;
        h.build = current_build_identity();
        h.heartbeat_ms = heartbeat_ms;
        h.host_pid = ::GetCurrentProcessId();
        return h;
    }

    // Sends host_hello and returns the worker's answer (WorkerHello or JobError).
    std::optional<Message> handshake(HostHello hello) {
        EXPECT_TRUE(host_->channel().send(hello));
        return host_->next();
    }

    std::optional<WorkerExitCode> wait_done(std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, timeout, [&] { return done_; })) {
            return std::nullopt;
        }
        return exit_code_;
    }

private:
    ScriptedRunner runner_;
    WorkerServerOptions options_;
    std::unique_ptr<MessageChannel> worker_;
    std::unique_ptr<HostEnd> host_;
    std::thread server_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool done_ = false;
    WorkerExitCode exit_code_ = WorkerExitCode::Internal;
};

}  // namespace lucent::workers::test_support
