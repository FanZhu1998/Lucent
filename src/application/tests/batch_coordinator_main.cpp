// lucent_batch_test_coordinator: a coordinator process for the recovery tests (batch_recovery_test.cpp).
//
//   lucent_batch_test_coordinator.exe <catalog> <output folder> <kill point> [<pattern> <collisions>]
//
// Opens the catalog prepared by the test (items imported and hashed), opens a BatchService over it,
// submits one batch of every library item into <output folder> and ends the whole process with
// TerminateProcess (exit code 77, no clean-up of any kind, like a crash or a power cut seen from the
// journal) when the publication protocol of the batch's first job reaches <kill point> ("reserved",
// "temp_written", "verified", "before_rename", "after_rename", "published", "receipt" or
// "complete"). Exit code 0: the batch finished without reaching the point; 1: a setup error.

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <lucent/application/batch_service.hpp>
#include <lucent/application/catalog_library.hpp>
#include <lucent/application/recipe_service.hpp>
#include <lucent/application/recipe_store.hpp>
#include <lucent/catalog/catalog.hpp>
#include <lucent/core/text.hpp>
#include <lucent/raw/decoder_registry.hpp>

int wmain(int argc, wchar_t** argv) {
    using namespace lucent;
    if (argc < 4) {
        std::fprintf(stderr, "usage: catalog output kill-point [pattern collisions]\n");
        return 1;
    }
    const std::filesystem::path catalog_path = argv[1];
    const std::filesystem::path output = argv[2];
    const std::string kill_at = core::wide_to_utf8_lossy(argv[3]);
    const std::string pattern = argc > 4 ? core::wide_to_utf8_lossy(argv[4]) : std::string(app::kDefaultNamePattern);
    const std::string collisions = argc > 5 ? core::wide_to_utf8_lossy(argv[5]) : std::string("unique_name");

    catalog::OpenOptions open;
    open.path = catalog_path;
    auto opened = catalog::Catalog::open(open);
    if (!opened) {
        std::fprintf(stderr, "catalog: %s\n", opened.error().describe().c_str());
        return 1;
    }
    std::shared_ptr<catalog::Catalog> catalog(std::move(*opened));
    auto library = app::CatalogLibrary::open(catalog);
    if (!library) {
        return 1;
    }
    auto store = app::create_catalog_recipe_store(catalog);
    if (!store) {
        return 1;
    }
    app::RecipeService recipes(**library, *store);

    app::BatchServiceOptions options;
    options.worker_executable = std::filesystem::path(LUCENT_WORKER_EXE).make_preferred();
    options.heartbeat_interval = std::chrono::milliseconds(100);
    options.lease_duration = std::chrono::seconds(10);
    options.lease_renewal = std::chrono::seconds(1);
    std::once_flag killed;
    std::optional<catalog::JobId> first_job;
    std::mutex mutex;
    options.test.on_point = [&](std::string_view point, catalog::JobId job) {
        {
            std::lock_guard lock(mutex);
            if (!first_job) {
                first_job = job;
            }
            if (*first_job != job) {
                return;
            }
        }
        if (point == kill_at) {
            std::fprintf(stdout, "killing at %s of job %lld\n", kill_at.c_str(), static_cast<long long>(job.value));
            std::fflush(stdout);
            ::TerminateProcess(::GetCurrentProcess(), 77);
        }
    };
    options.max_workers = 1;  // the first job reaches every point before the second starts
    auto batches = app::BatchService::open(**library, catalog, recipes, std::move(options));
    if (!batches) {
        std::fprintf(stderr, "batches: %s\n", batches.error().describe().c_str());
        return 1;
    }
    std::mutex done_mutex;
    std::condition_variable done_changed;
    bool done = false;
    lucent::app::Subscription subscription = (*batches)->subscribe([&](const app::BatchEvent& event) {
        if (event.kind == app::BatchEventKind::Finished) {
            {
                std::lock_guard lock(done_mutex);
                done = true;
            }
            done_changed.notify_all();
        }
    });
    app::BatchRequest request;
    for (const app::SourceItem& item : (*library)->items()) {
        request.items.push_back(app::BatchItem{item.id, std::nullopt});
    }
    request.destination.folder = output;
    request.destination.name_pattern = pattern;
    auto policy = app::collision_policy_from_string(collisions);
    if (!policy) {
        return 1;
    }
    request.destination.collisions = *policy;
    auto submitted = (*batches)->submit(request);
    if (!submitted) {
        std::fprintf(stderr, "submit: %s\n", submitted.error().describe().c_str());
        return 1;
    }
    std::unique_lock lock(done_mutex);
    if (!done_changed.wait_for(lock, std::chrono::minutes(3), [&] { return done; })) {
        return 1;
    }
    return 0;
}
