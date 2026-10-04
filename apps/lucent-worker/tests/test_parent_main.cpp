// lucent_worker_test_parent: a minimal host process for the parent-death tests
// (worker_process_test.cpp). It starts a worker and reports on standard output, then waits until
// the test kills it:
//
//   lucent_worker_test_parent <lucent-worker.exe> <job|nojob> <idle|busy>
//
//   job | nojob   with or without the Job Object (nojob leaves only the worker's own reaction to
//                 its host ending: the closed pipe and the signalled parent handle)
//   idle          prints "pid=<worker pid>" and sleeps
//   busy          the worker runs with the hang-job hook; prints "pid=<worker pid>", dispatches a
//                 job, prints "busy" once a heartbeat names the job, and blocks in run_job()
//
// Exit codes: 2 usage, 3 the worker did not start.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include <windows.h>

#include <lucent/core/hash.hpp>
#include <lucent/workers/protocol.hpp>
#include <lucent/workers/worker_host.hpp>

int wmain(int argc, wchar_t** argv) {
    using namespace lucent::workers;
    if (argc != 4) {
        return 2;
    }
    const std::wstring_view mode = argv[2];
    const std::wstring_view activity = argv[3];
    WorkerHostOptions options;
    options.executable = argv[1];
    options.use_job_object = mode == L"job";
    options.heartbeat_interval = std::chrono::milliseconds(50);
    const bool busy = activity == L"busy";
    if (busy) {
        options.test_hooks = {"hang-job"};
    }
    WorkerHost host(options);
    if (!host.start()) {
        return 3;
    }
    std::printf("pid=%u\n", host.info().pid);
    std::fflush(stdout);
    if (!busy) {
        ::Sleep(INFINITE);
        return 0;
    }

    // hang-job blocks the job thread before the job is looked at, so any well-formed request does.
    JobRequest job;
    job.job_id = "busy";
    job.source.path = options.executable;
    job.recipe.document = R"({"test":true})";
    auto digest = lucent::core::sha256(std::as_bytes(std::span(job.recipe.document.data(), job.recipe.document.size())));
    if (!digest) {
        return 3;
    }
    job.recipe.sha256 = *digest;
    job.temp_path = std::filesystem::temp_directory_path() /
                    (L"lucent-test-parent-" + std::to_wstring(::GetCurrentProcessId()) + L".partial");
    bool announced = false;
    JobCallbacks callbacks;
    callbacks.heartbeat = [&](const Heartbeat& heartbeat) {
        if (!announced && heartbeat.job_id == "busy") {
            announced = true;
            std::printf("busy\n");
            std::fflush(stdout);
        }
    };
    (void)host.run_job(job, callbacks);
    return 0;
}
