// The coordinator, the worker slots and the publication protocol of BatchService (batch_service.hpp,
// "Scheduling" and "Publication"; ADR 0012).

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <new>
#include <string>
#include <thread>
#include <utility>

#include <windows.h>

#include <nlohmann/json.hpp>

#include <lucent/application/auto_develop.hpp>
#include <lucent/application/recipe.hpp>
#include <lucent/application/recipe_store.hpp>
#include <lucent/core/file.hpp>
#include <lucent/core/log.hpp>
#include <lucent/core/storage.hpp>
#include <lucent/core/text.hpp>
#include <lucent/core/win32_error.hpp>
#include <lucent/engine/develop_geometry.hpp>
#include <lucent/export/metadata.hpp>
#include <lucent/export/output_profile.hpp>
#include <lucent/workers/job_output.hpp>

#include "batch_service_impl.hpp"
#include "output_publication.hpp"
#include "win32_paths.hpp"

namespace lucent::app {

using namespace std::chrono_literals;

namespace {

// Largest part of a side the camera's image area may cut from the catalogued frame before an
// export's size gets a review note (geometry version 2; the corpus's largest border is 1.8 %).
constexpr double kCameraBorderTolerance = 0.05;

constexpr std::string_view kWhere = "app.batch";
constexpr std::string_view kWherePublish = "app.batch.publish";
constexpr auto kTick = 250ms;       // while batches have queued jobs (back-off, admission)
constexpr auto kIdleTick = 2000ms;  // otherwise; submissions, commands and finished jobs wake it at once
constexpr auto kDestinationCheckInterval = 2s;
constexpr std::uint64_t kCalibrationMinPixels = 1'000'000;
constexpr std::uint64_t kUnknownPixels = 50'000'000;  // estimate for a photo without a probe
constexpr std::uint64_t kDestinationReserveBytes = std::uint64_t{64} << 20;
constexpr std::uint64_t kFileOverheadBytes = std::uint64_t{1} << 20;
constexpr int kCatalogAttempts = 3;

using Clock = std::chrono::steady_clock;

std::string quoted(const std::filesystem::path& path) { return "'" + core::path_to_utf8_lossy(path) + "'"; }

ItemId from_catalog(catalog::ItemId id) { return ItemId{static_cast<std::uint64_t>(id.value)}; }

bool path_exists(const std::filesystem::path& path) {
    auto api = detail::api_path(path, kWhere);
    return api && ::GetFileAttributesW(api->c_str()) != INVALID_FILE_ATTRIBUTES;
}

double seconds_since(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }

catalog::FailureClass to_catalog(workers::FailureClass value) {
    return value == workers::FailureClass::Transient ? catalog::FailureClass::Transient
                                                     : catalog::FailureClass::Deterministic;
}

// An attempt of this job runs an automatic analysis first.
bool needs_analysis(const catalog::JobRecord& job) {
    return job.auto_state == catalog::AutoJobState::Pending || job.auto_state == catalog::AutoJobState::Analysed;
}

}  // namespace

// ---- Threads -----------------------------------------------------------------------------------

void BatchService::Impl::start_threads() {
    try {
        for (std::uint32_t i = 0; i < options.max_workers; ++i) {
            auto slot = std::make_unique<detail::Slot>();
            slot->index = i;
            slots.push_back(std::move(slot));
        }
        for (const auto& slot : slots) {
            detail::Slot* raw = slot.get();
            slot->thread = std::thread([this, raw] { slot_main(*raw); });
        }
        coordinator = std::thread([this] { coordinator_main(); });
    } catch (...) {
        stop_threads();
        throw;
    }
}

void BatchService::Impl::stop_threads() noexcept {
    {
        std::lock_guard lock(mutex);
        stopping = true;
        for (const auto& slot : slots) {
            if (slot->busy && slot->stop == detail::StopReason::None) {
                slot->stop = detail::StopReason::Shutdown;
                slot->cancel.cancel();
            }
        }
    }
    coordinator_cv.notify_all();
    for (const auto& slot : slots) {
        slot->cv.notify_all();
    }
    if (coordinator.joinable()) {
        coordinator.join();
    }
    for (const auto& slot : slots) {
        if (slot->thread.joinable()) {
            slot->thread.join();
        }
    }
}

void BatchService::Impl::coordinator_main() {
    // Reconcile the journal before anything is dispatched.
    try {
        if (Status recovered_status = recover(); !recovered_status) {
            // Leases left by the previous session are reclaimed once they expire (reclaim_abandoned).
            core::log_error(kWhere, "the batch journal could not be fully reconciled: {}",
                            recovered_status.error().describe());
        }
    } catch (const std::exception& e) {
        core::log_error(kWhere, "the batch journal reconciliation raised: {}", e.what());
    } catch (...) {
        core::log_error(kWhere, "the batch journal reconciliation raised an unknown exception");
    }
    {
        std::lock_guard lock(mutex);
        recovered = true;
    }
    recovered_cv.notify_all();
    auto last_reclaim = Clock::now();
    for (;;) {
        bool reclaim = false;
        {
            std::unique_lock lock(mutex);
            coordinator_cv.wait_for(lock, work_waiting ? kTick : kIdleTick,
                                    [&] { return stopping || wake_requested; });
            if (stopping) {
                return;
            }
            wake_requested = false;
            reclaim = std::exchange(reclaim_requested, false);
        }
        try {
            if (reclaim || Clock::now() - last_reclaim >= options.reclaim_interval) {
                last_reclaim = Clock::now();
                reclaim_abandoned();
            }
            dispatch();
        } catch (const std::exception& e) {
            core::log_error(kWhere, "the batch coordinator raised: {}", e.what());
        } catch (...) {
            core::log_error(kWhere, "the batch coordinator raised an unknown exception");
        }
    }
}

void BatchService::Impl::slot_main(detail::Slot& slot) {
    for (;;) {
        std::optional<detail::Assignment> work;
        bool release = false;
        {
            std::unique_lock lock(mutex);
            slot.cv.wait(lock, [&] { return stopping || slot.assignment.has_value() || slot.release_requested; });
            if (!slot.assignment && stopping) {
                break;
            }
            if (slot.assignment) {
                work = std::move(slot.assignment);
                slot.assignment.reset();
            } else {
                release = true;
            }
        }
        if (release) {
            // The coordinator needs the room this idle worker holds under the aggregate limit.
            if (slot.host) {
                slot.host->shutdown();
            }
            {
                std::lock_guard lock(mutex);
                slot.release_requested = false;
                slot.idle_commit = 0;
                slot.pid = 0;
            }
            request_wake();
            continue;
        }
        try {
            run_attempt(slot, *work);
        } catch (const std::exception& e) {
            // The job keeps its lease; reclaim_abandoned() reconciles it once the lease expires.
            core::log_error(kWhere, "a batch attempt raised: {}", e.what());
        } catch (...) {
            core::log_error(kWhere, "a batch attempt raised an unknown exception");
        }
        bool reclaim = false;
        const std::uint64_t idle = slot.host && slot.host->info().running ? slot.host->info().idle_commit_bytes : 0;
        {
            std::lock_guard lock(mutex);
            slot.busy = false;
            slot.stop = detail::StopReason::None;
            slot.running = RunningJob{};
            slot.batch = {};
            slot.owner.clear();
            slot.estimate_bytes = 0;
            slot.observed_commit = 0;
            slot.exclusive = false;
            slot.idle_commit = idle;
            reclaim = std::exchange(slot.reclaim_after, false);
        }
        if (reclaim) {
            request_reclaim();  // its owner is no longer live: reconcile the job now
        } else {
            request_wake();
        }
    }
    if (slot.host) {
        slot.host->shutdown();
    }
    std::lock_guard lock(mutex);
    slot.pid = 0;
}

std::vector<detail::Slot*> BatchService::Impl::busy_slots_of(catalog::BatchId batch) {
    std::vector<detail::Slot*> out;
    std::lock_guard lock(mutex);
    for (const auto& slot : slots) {
        if (slot->busy && slot->batch == batch) {
            out.push_back(slot.get());
        }
    }
    return out;
}

// ---- Admission ---------------------------------------------------------------------------------

std::uint64_t BatchService::Impl::estimate_bytes(std::uint64_t pixels, bool analysis) const {
    double per_pixel = calibrated && measured_bytes_per_pixel > 0.0
                           ? std::max(measured_bytes_per_pixel * options.memory.measured_margin, 1.0)
                           : options.memory.default_bytes_per_pixel;
    if (analysis) {
        // The analysis' full-resolution final check needs more than an export: its own calibration.
        per_pixel = std::max(per_pixel, measured_auto_bytes_per_pixel > 0.0
                                            ? measured_auto_bytes_per_pixel * options.memory.measured_margin
                                            : options.memory.default_auto_bytes_per_pixel);
    }
    const std::uint64_t px = pixels == 0 ? kUnknownPixels : pixels;
    const std::uint64_t base = std::max(options.memory.worker_baseline_bytes, measured_floor);
    return base + static_cast<std::uint64_t>(std::ceil(static_cast<double>(px) * per_pixel));
}

std::uint64_t BatchService::Impl::output_bytes(const detail::BatchContext& context, std::uint64_t pixels) const {
    const exporter::BitDepth depth = exporter::bit_depth_for(context.output.settings.format);
    const std::uint64_t per_pixel = depth == exporter::BitDepth::Sixteen ? 6 : 3;
    return (pixels == 0 ? kUnknownPixels : pixels) * per_pixel + kFileOverheadBytes;
}

void BatchService::Impl::raise_attention(detail::BatchContext& context, BatchAttention attention, Error error,
                                         std::optional<std::uint64_t> free_bytes, std::uint64_t needed_bytes) {
    {
        std::lock_guard lock(mutex);
        context.attention = attention;
        context.attention_error = error;
        context.attention_free_bytes = free_bytes;
        context.attention_needed_bytes = needed_bytes;
        context.destination_checked = {};
    }
    auto paused = catalog->transact([&](catalog::WriteTransaction& tx) { return tx.pause_batch(context.id, now()); });
    if (!paused) {
        core::log_warn(kWhere, "cannot pause batch {}: {}", context.id.value, paused.error().describe());
    }
    core::log_warn(kWhere, "batch {} paused: {} ({})", context.id.value, to_string(attention), error.describe());
    auto record = batch_record(context.id);
    BatchEvent event;
    event.kind = BatchEventKind::Attention;
    event.batch = context.id;
    if (record) {
        event.state = record->state;
        event.counts = record->counts;
    }
    event.attention = attention;
    event.attention_free_bytes = free_bytes;
    event.attention_needed_bytes = needed_bytes;
    event.error = std::move(error);
    emit(std::move(event));
}

bool BatchService::Impl::destination_usable(detail::BatchContext& context, std::uint64_t needed_bytes) {
    {
        std::lock_guard lock(mutex);
        if (context.destination_checked != Clock::time_point{} &&
            Clock::now() - context.destination_checked < kDestinationCheckInterval &&
            (!context.free_bytes || *context.free_bytes >= needed_bytes + kDestinationReserveBytes)) {
            return true;
        }
    }
    const std::filesystem::path& folder = context.destination.folder;
    auto id = core::stat_file(folder);
    if (!id) {
        const Error& error = id.error();
        if (error.code == ErrorCode::PermissionDenied) {
            raise_attention(context, BatchAttention::DestinationPermission,
                            Error{ErrorCode::PermissionDenied,
                                  std::format("Lucent may not open the output folder {}", quoted(folder)),
                                  std::string(detail::kWhereDestination), error.native_code});
        } else {
            raise_attention(context, BatchAttention::DestinationUnavailable,
                            Error{ErrorCode::NotFound, std::format("the output folder {} is not available", quoted(folder)),
                                  std::string(detail::kWhereDestination), error.native_code});
        }
        return false;
    }
    if (const std::optional<core::FileIdentity> recorded = folder_identity_of(context);
        recorded && !core::is_same_file(*id, *recorded)) {
        raise_attention(context, BatchAttention::DestinationReplaced,
                        Error{ErrorCode::NotFound,
                              std::format("{} is no longer the output folder chosen for this batch (it was created "
                                          "again, or another drive took its place)",
                                          quoted(folder)),
                              std::string(detail::kWhereDestinationReplaced), 0});
        return false;
    }
    const std::optional<std::uint64_t> free = free_space(folder);
    if (free && *free < needed_bytes + kDestinationReserveBytes) {
        raise_attention(context, BatchAttention::DestinationFull,
                        Error{ErrorCode::DiskFull,
                              std::format("the destination has {} MB free; the next output may need up to {} MB",
                                          *free >> 20, needed_bytes >> 20),
                              std::string(detail::kWhereDestination), 0},
                        free, needed_bytes);
        return false;
    }
    std::lock_guard lock(mutex);
    context.destination_checked = Clock::now();
    context.free_bytes = free;
    return true;
}

// ---- Dispatch ----------------------------------------------------------------------------------

std::vector<catalog::BatchRecord> BatchService::Impl::dispatchable_batches() {
    bool scanned = false;
    {
        std::lock_guard lock(mutex);
        scanned = active_scanned;
    }
    if (!scanned) {
        // The scan at recovery failed: repeat it until the set of unfinished batches is complete.
        if (Status rescanned = scan_batches(); !rescanned) {
            core::log_warn(kWhere, "cannot list batches: {}", rescanned.error().describe());
        }
    }
    std::vector<std::int64_t> ids;
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(mutex);
        ids.assign(active_batches.begin(), active_batches.end());  // ascending: oldest first
        generation = active_generation;
    }
    std::vector<catalog::BatchRecord> running;
    std::vector<std::int64_t> gone;
    for (const std::int64_t id : ids) {
        auto found = catalog->query([&](catalog::ReadTransaction& tx) { return tx.find_batch(catalog::BatchId{id}); });
        if (!found) {
            core::log_warn(kWhere, "cannot read batch {}: {}", id, found.error().describe());
            continue;
        }
        if (!*found || detail::is_finished_batch(**found)) {
            gone.push_back(id);
            continue;
        }
        if ((*found)->state == catalog::BatchState::Running && (*found)->counts.queued > 0) {
            running.push_back(std::move(**found));
        }
    }
    if (!gone.empty()) {
        std::lock_guard lock(mutex);
        if (generation == active_generation) {  // otherwise a batch may have been reopened: next time
            for (const std::int64_t id : gone) {
                active_batches.erase(id);
            }
        }
    }
    return running;
}

void BatchService::Impl::dispatch() {
    for (std::uint32_t round = 0; round < options.max_workers; ++round) {
        detail::Slot* idle = nullptr;
        {
            std::lock_guard lock(mutex);
            if (stopping) {
                return;
            }
            // A free slot whose worker is running (it is reused) first, then any free slot that is not
            // shutting its worker down, then any free slot.
            for (int pass = 0; pass < 3 && idle == nullptr; ++pass) {
                for (const auto& slot : slots) {
                    if (slot->busy || slot->assignment || (pass < 2 && slot->release_requested) ||
                        (pass == 0 && slot->idle_commit == 0)) {
                        continue;
                    }
                    idle = slot.get();
                    break;
                }
            }
        }
        if (idle == nullptr) {
            return;
        }
        // Running batches with queued jobs, oldest first.
        const std::vector<catalog::BatchRecord> running = dispatchable_batches();
        {
            std::lock_guard lock(mutex);
            work_waiting = !running.empty();
        }
        // Interactive single exports first (the photographer is waiting for them), then the oldest
        // batch first.
        std::vector<std::pair<const catalog::BatchRecord*, std::shared_ptr<detail::BatchContext>>> ordered;
        for (const catalog::BatchRecord& record : running) {
            auto context = load_context(record);
            if (!context) {
                core::log_error(kWhere, "batch {} cannot be read and is skipped: {}", record.id.value,
                                context.error().describe());
                continue;
            }
            ordered.emplace_back(&record, *context);
        }
        std::stable_partition(ordered.begin(), ordered.end(),
                              [](const auto& entry) { return entry.second->policy.single_export; });
        bool assigned = false;
        for (const auto& [record_ptr, context_ptr] : ordered) {
            const catalog::BatchRecord& record = *record_ptr;
            const std::shared_ptr<detail::BatchContext>* context = &context_ptr;
            // A batch that keeps related shots consistent analyses every photo before any is
            // regularised: while one is pending, only pending jobs are claimed (ADR 0013).
            std::vector<catalog::AutoJobState> auto_states;
            if ((*context)->automatic != catalog::AutoMode::None && (*context)->policy.automatic.keep_consistent) {
                auto counts =
                    catalog->query([&](catalog::ReadTransaction& tx) { return tx.auto_job_counts(record.id); });
                if (!counts) {
                    continue;
                }
                if (counts->pending > 0) {
                    auto_states = {catalog::AutoJobState::Pending};
                }
            }
            catalog::JobQuery query;
            query.batch = record.id;
            query.stages = {catalog::JobStage::Queued};
            query.auto_states = auto_states;
            query.limit = 32;
            auto queued = catalog->query([&](catalog::ReadTransaction& tx) { return tx.list_jobs(query); });
            if (!queued) {
                continue;
            }
            const catalog::Timestamp t = now();
            const auto next = std::find_if(queued->begin(), queued->end(),
                                           [&](const catalog::JobRecord& job) { return job.not_before <= t; });
            if (next == queued->end()) {
                continue;
            }
            std::optional<SourceItem> item = next->item ? library.find(from_catalog(*next->item)) : std::nullopt;
            std::uint64_t pixels = item ? detail::output_pixels(*item) : 0;
            bool analysis = needs_analysis(*next);
            // An analyse-only batch writes nothing into its folder but the report.
            if ((*context)->automatic != catalog::AutoMode::AnalyzeOnly &&
                !destination_usable(**context, output_bytes(**context, pixels))) {
                continue;
            }
            const bool interactive = options.interactive_busy && options.interactive_busy();
            const auto admit = [&](std::uint64_t estimate, bool exclusive) {
                // (mutex held)
                std::uint32_t busy = 0;
                std::uint64_t reserved = 0;
                // Idle workers of other slots: what they hold counts against the aggregate limit.
                std::uint64_t idle_held = 0;
                std::vector<detail::Slot*> holders;
                for (const auto& slot : slots) {
                    if (slot->busy) {
                        ++busy;
                        reserved += std::max(slot->estimate_bytes, slot->observed_commit);
                    } else if (slot.get() != idle && slot->idle_commit > 0) {
                        idle_held += slot->idle_commit;
                        holders.push_back(slot.get());
                    }
                }
                // Asks the idle workers to leave; admission is tried again once they are gone.
                const auto release_holders = [&] {
                    for (detail::Slot* holder : holders) {
                        if (!holder->release_requested) {
                            holder->release_requested = true;
                            ++counters.idle_workers_released;
                            holder->cv.notify_all();
                        }
                    }
                    ++counters.deferred_for_memory;
                    return false;
                };
                if (interactive) {
                    ++counters.deferred_for_interactive;
                    return false;
                }
                if (exclusive) {
                    // The job's last attempt hit the aggregate limit beside other workers: it runs
                    // alone, with the other workers' memory released.
                    if (busy > 0) {
                        ++counters.deferred_for_memory;
                        return false;
                    }
                    return holders.empty() ? true : release_holders();
                }
                if (busy == 0) {
                    // One worker always runs (the engine refuses what exceeds the budget), with the
                    // room idle workers would take from it under the aggregate limit.
                    return holders.empty() || estimate + idle_held <= worker_budget ? true : release_holders();
                }
                if (busy >= options.max_workers) {
                    return false;
                }
                if ((!calibrated && !options.test.admit_before_calibration) || reserved + estimate > worker_budget) {
                    ++counters.deferred_for_memory;
                    return false;
                }
                if (reserved + estimate + idle_held > worker_budget) {
                    return release_holders();
                }
                const detail::PhysicalMemory memory = detail::physical_memory();
                if (memory.available != 0 && memory.available < estimate + options.memory.free_memory_margin_bytes) {
                    ++counters.deferred_for_memory;
                    return false;
                }
                return true;
            };
            std::string owner;
            {
                std::lock_guard lock(mutex);
                if (!admit(estimate_bytes(pixels, analysis), exclusive_jobs.contains(next->id.value))) {
                    return;
                }
                owner = std::format("{}/slot{}/{}", session, idle->index, ++attempt_counter);
            }
            catalog::ClaimRequest claim;
            claim.owner = owner;
            claim.now = t;
            claim.lease_duration = options.lease_duration;
            claim.batch = record.id;
            claim.auto_states = auto_states;
            auto claimed = catalog->transact([&](catalog::WriteTransaction& tx) { return tx.claim_next_job(claim); });
            if (!claimed) {
                core::log_warn(kWhere, "cannot claim a job of batch {}: {}", record.id.value, claimed.error().describe());
                continue;
            }
            if (!*claimed) {
                continue;
            }
            if ((*claimed)->job.id != next->id) {
                item = (*claimed)->job.item ? library.find(from_catalog(*(*claimed)->job.item)) : std::nullopt;
                pixels = item ? detail::output_pixels(*item) : 0;
                analysis = needs_analysis((*claimed)->job);
            }
            std::unique_lock lock(mutex);
            const bool exclusive = exclusive_jobs.contains((*claimed)->job.id.value);
            const std::uint64_t estimate = estimate_bytes(pixels, analysis);
            if (stopping || ((*claimed)->job.id != next->id && !admit(estimate, exclusive))) {
                // Give it back untouched (no failure is counted).
                lock.unlock();
                const catalog::JobLease lease = (*claimed)->lease;
                (void)catalog->transact([&](catalog::WriteTransaction& tx) { return tx.release_job(lease, now()); });
                return;
            }
            idle->busy = true;
            idle->owner = owner;
            idle->batch = record.id;
            idle->cancel = core::CancellationSource();
            idle->stop = detail::StopReason::None;
            // An exclusive attempt reserves the whole worker budget: nothing is admitted beside it.
            idle->exclusive = exclusive;
            idle->release_requested = false;  // its worker is used for this attempt instead
            idle->estimate_bytes = exclusive ? std::max(estimate, worker_budget) : estimate;
            if (exclusive) {
                exclusive_jobs.erase((*claimed)->job.id.value);
                ++counters.exclusive_attempts;
            }
            idle->observed_commit = 0;
            idle->running = RunningJob{};
            idle->running.job = (*claimed)->job.id;
            idle->running.item = item ? std::optional<ItemId>(item->id) : std::nullopt;
            idle->running.ordinal = (*claimed)->job.ordinal;
            idle->running.attempt = (*claimed)->lease.attempt;
            idle->running.source = item ? item->path : std::filesystem::path{};
            idle->running.slot = idle->index;
            idle->assignment =
                detail::Assignment{std::move(**claimed), *context, std::move(item), pixels, estimate, analysis};
            std::uint32_t active = 0;
            for (const auto& slot : slots) {
                active += slot->busy ? 1 : 0;
            }
            counters.peak_active_workers = std::max(counters.peak_active_workers, active);
            ++counters.attempts;
            idle->cv.notify_all();
            assigned = true;
            break;
        }
        if (!assigned) {
            return;
        }
    }
}

void BatchService::Impl::reclaim_abandoned() {
    std::vector<std::string> live;
    {
        std::lock_guard lock(mutex);
        for (const auto& slot : slots) {
            if (slot->busy) {
                live.push_back(slot->owner);
            }
        }
    }
    catalog::ReclaimQuery query;
    query.now = now();
    query.live_owners = live;
    query.limit = 100;
    auto jobs = catalog->query([&](catalog::ReadTransaction& tx) { return tx.list_reclaimable_jobs(query); });
    if (!jobs) {
        return;
    }
    for (const catalog::JobRecord& job : *jobs) {
        if (job.lease && std::find(live.begin(), live.end(), job.lease->owner) != live.end()) {
            continue;  // its slot is still working on it and renews it
        }
        bool deferred_before = false;
        {
            std::lock_guard lock(mutex);
            deferred_before = deferred_jobs.contains(job.id.value);
        }
        if (!deferred_before) {
            core::log_warn(kWhere, "reclaiming job {} from its abandoned lease", job.id.value);
        }
        if (reconcile_job(job)) {
            check_finished(job.batch);
        }
    }
    // Publications whose clean-up waited for their output folder.
    std::set<std::pair<std::int64_t, std::uint32_t>> waiting;
    {
        std::lock_guard lock(mutex);
        waiting = deferred_publications;
    }
    if (waiting.empty()) {
        return;
    }
    std::int64_t after = 0;
    for (;;) {
        auto open = catalog->query(
            [&](catalog::ReadTransaction& tx) { return tx.list_publications_in_progress(std::nullopt, 1000, after); });
        if (!open) {
            return;
        }
        for (const catalog::PublicationInProgress& entry : *open) {
            after = std::max(after, entry.latest.id);
            if (!entry.attempt_is_live() && waiting.contains({entry.latest.job.value, entry.latest.attempt})) {
                if (close_publication(entry)) {
                    check_finished(entry.batch);
                }
            }
        }
        if (open->size() < 1000) {
            break;
        }
    }
}

// ---- One attempt ---------------------------------------------------------------------------------

namespace {

// Runs one claimed job through the worker and the publication protocol (see batch_service.hpp,
// "Publication"). One per attempt, on its slot's thread.
class Attempt {
public:
    Attempt(BatchService::Impl& s, detail::Slot& slot, detail::Assignment& a)
        : s_(s),
          slot_(slot),
          a_(a),
          lease_(a.claim.lease),
          job_(a.claim.job),
          context_(*a.context),
          started_(Clock::now()),
          last_renewal_(Clock::now()) {
        std::lock_guard lock(s_.mutex);
        token_ = slot_.cancel.token();
    }

    void run();

private:
    // ---- Helpers ----
    void point(std::string_view name) const {
        if (s_.options.test.on_point) {
            s_.options.test.on_point(name, job_.id);
        }
    }
    [[nodiscard]] detail::StopReason stop_reason() const {
        std::lock_guard lock(s_.mutex);
        return slot_.stop;
    }
    void renew_if_due(bool force = false);
    [[nodiscard]] bool journal(catalog::PublicationStep step, const std::optional<detail::OutputDigest>& digest = {});
    [[nodiscard]] Status advance(catalog::JobStage from);
    [[nodiscard]] bool choose_name(std::uint32_t first_index);
    void release_name();
    void cleanup_temp();
    void close_journal();
    void emit_finished(catalog::JobStage stage, std::optional<Error> error, std::optional<std::filesystem::path> output = {});
    void give_back();
    void cancelled();
    void lease_lost();
    // After publication, when the journal or the receipt could not be recorded: the job keeps its
    // lease and is reconciled from its journal as soon as the slot is free.
    void leave_to_recovery();
    // Ends the attempt at a safe boundary after its token was cancelled, as its stop reason says.
    void stop_here();
    // `skip`: the job ends Skipped with `error` as its reason (unsupported while decoding).
    void fail_job(catalog::FailureClass failure_class, Error error, bool retry_allowed, bool skip = false);
    // A failure: destination problems pause the batch; source problems fail; others by class.
    void diagnose(Error error, catalog::FailureClass failure_class);
    struct DestinationProblem {
        BatchAttention attention = BatchAttention::None;
        Error error;
        std::optional<std::uint64_t> free_bytes;
        std::uint64_t needed_bytes = 0;
    };
    [[nodiscard]] std::optional<DestinationProblem> destination_problem(const Error& error);
    void ensure_host();
    [[nodiscard]] workers::JobCallbacks worker_callbacks();
    [[nodiscard]] workers::JobOutcome run_worker(const workers::JobRequest& request);
    // A failed worker job: given back, interrupted, cancelled or diagnosed.
    void worker_failed(const workers::JobFailure& failure);
    // Automatic jobs (ADR 0013): the analyze job(s) and the freeze. True when the attempt goes on
    // to export (the job is frozen); false when it ended here (given back after the pre-pass,
    // completed without output, failed or stopped).
    [[nodiscard]] bool automatic_stage();
    [[nodiscard]] workers::AnalysisOutcome run_analysis(const workers::AnalysisRequest& request);
    [[nodiscard]] bool freeze(const AutoRun& run, const std::optional<std::string>& analysis_document);
    // An analyse-only job frozen by an earlier attempt: completes it without output.
    void complete_analysed();
    // Review notes of the effective automatic revision's analysis record (its review flags and what
    // the locks did), user-facing.
    void load_automatic_notes();
    [[nodiscard]] Status verify(detail::HeldOutput& held, const workers::JobResult& result,
                                detail::OutputDigest& digest);
    [[nodiscard]] Status publish(detail::HeldOutput& held);
    void calibrate(const workers::JobMetrics& metrics, const workers::MemorySnapshot* result_memory, bool fresh_process,
                   bool analysis);
    [[nodiscard]] std::vector<std::string> review_notes(const workers::JobResult& result) const;

    BatchService::Impl& s_;
    detail::Slot& slot_;
    detail::Assignment& a_;
    const catalog::JobLease lease_;
    const catalog::JobRecord job_;
    detail::BatchContext& context_;
    core::CancellationToken token_;
    const Clock::time_point started_;
    Clock::time_point last_renewal_;

    std::optional<SourceItem> item_;
    detail::FrozenDocument frozen_;
    std::optional<catalog::RevisionId> revision_;  // the job's revision (the effective automatic one once frozen)
    catalog::AutoJobState auto_state_ = catalog::AutoJobState::None;
    std::vector<std::string> automatic_notes_;  // review notes of the effective automatic revision
    std::filesystem::path final_dir_;
    std::filesystem::path final_path_;
    std::filesystem::path planned_name_;  // the pattern's own name (index 1)
    std::filesystem::path temp_path_;
    std::optional<std::wstring> reserved_key_;
    std::uint32_t name_index_ = 1;
    bool replaces_ = false;
    bool original_conflict_ = false;
    std::optional<core::FileIdentity> replaced_identity_;
    std::optional<std::filesystem::path> aside_path_;
    bool journal_open_ = false;
};

void Attempt::renew_if_due(bool force) {
    if (!force && Clock::now() - last_renewal_ < s_.options.lease_renewal) {
        return;
    }
    last_renewal_ = Clock::now();
    auto renewed = s_.catalog->transact([&](catalog::WriteTransaction& tx) {
        return tx.heartbeat_job(lease_, s_.now(), s_.options.lease_duration);
    });
    std::lock_guard lock(s_.mutex);
    if (!renewed) {
        if (renewed.error().where == "catalog.job.lease_lost") {
            if (slot_.stop == detail::StopReason::None) {
                slot_.stop = detail::StopReason::LeaseLost;
            }
            slot_.cancel.cancel();
        } else {
            core::log_warn(kWhere, "cannot renew the lease of job {}: {}", job_.id.value, renewed.error().describe());
        }
        return;
    }
    if (renewed->cancel_requested && slot_.stop == detail::StopReason::None) {
        slot_.stop = detail::StopReason::BatchCancelled;
        slot_.cancel.cancel();
    }
}

bool Attempt::journal(catalog::PublicationStep step, const std::optional<detail::OutputDigest>& digest) {
    renew_if_due();  // every step requires an unexpired lease
    catalog::PublicationEntry entry;
    entry.step = step;
    entry.temp_path = temp_path_;
    entry.final_path = final_path_;
    if (digest) {
        entry.output_sha256 = digest->sha256;
        entry.output_size = digest->size_bytes;
    }
    Status written = lucent::fail(ErrorCode::Internal, "not written", std::string(kWhere));
    for (int attempt = 0; attempt < kCatalogAttempts; ++attempt) {
        written = s_.catalog->write(
            [&](catalog::WriteTransaction& tx) { return tx.record_publication_step(lease_, entry, s_.now()); });
        if (written || written.error().code != ErrorCode::IoError) {
            break;
        }
        std::this_thread::sleep_for(50ms);
    }
    if (!written) {
        core::log_error(kWhere, "cannot record step {} of job {}: {}", catalog::to_string(step), job_.id.value,
                        written.error().describe());
        return false;
    }
    if (step == catalog::PublicationStep::Reserved) {
        journal_open_ = true;
    }
    return true;
}

Status Attempt::advance(catalog::JobStage from) {
    renew_if_due();
    Status advanced = lucent::fail(ErrorCode::Internal, "not advanced", std::string(kWhere));
    for (int attempt = 0; attempt < kCatalogAttempts; ++attempt) {
        auto stage = s_.catalog->transact([&](catalog::WriteTransaction& tx) { return tx.advance_job(lease_, from, s_.now()); });
        advanced = stage ? ok() : Status(Unexpected(stage.error()));
        if (advanced || advanced.error().code != ErrorCode::IoError) {
            break;
        }
        std::this_thread::sleep_for(50ms);
    }
    return advanced;
}

bool Attempt::choose_name(std::uint32_t first_index) {
    const std::wstring dir_key = folded_path_key(final_dir_);
    bool listed = false;
    {
        std::lock_guard lock(s_.mutex);
        listed = context_.taken.contains(dir_key);
    }
    if (!listed) {
        std::unordered_set<std::wstring> names;
        if (auto entries = detail::list_directory(final_dir_, detail::kWhereDestination)) {
            for (const detail::DirectoryEntry& entry : *entries) {
                names.insert(detail::fold_case(entry.name));
            }
        }
        std::lock_guard lock(s_.mutex);
        auto& taken = context_.taken[dir_key];
        taken.insert(names.begin(), names.end());
    }
    if (context_.destination.collisions == CollisionPolicy::Overwrite) {
        bool load = false;
        {
            std::lock_guard lock(s_.mutex);
            load = !context_.own_outputs_loaded;
        }
        if (load) {
            std::unordered_set<std::wstring> own;
            catalog::JobQuery query;
            query.batch = context_.id;
            query.stages = {catalog::JobStage::Complete};
            for (;;) {
                auto jobs = s_.catalog->query([&](catalog::ReadTransaction& tx) { return tx.list_jobs(query); });
                if (!jobs) {
                    break;
                }
                for (const catalog::JobRecord& done : *jobs) {
                    auto receipt = s_.catalog->query([&](catalog::ReadTransaction& tx) { return tx.find_receipt(done.id); });
                    if (receipt && *receipt) {
                        own.insert(folded_path_key((*receipt)->final_path));
                    }
                    query.after = done.id;
                }
                if (jobs->size() < query.limit) {
                    break;
                }
            }
            std::lock_guard lock(s_.mutex);
            context_.own_outputs.insert(own.begin(), own.end());
            context_.own_outputs_loaded = true;
        }
    }
    const OutputNameFields fields{item_->path, frozen_.revision_number, item_->subimage, item_->subimage_count,
                                  job_.ordinal};
    if (auto planned = expand_output_name(context_.pattern, fields, context_.output.settings.format, 1)) {
        planned_name_ = *planned;
    }
    for (std::uint32_t index = std::max<std::uint32_t>(1, first_index); index <= kMaxOutputNameIndex; ++index) {
        auto name = expand_output_name(context_.pattern, fields, context_.output.settings.format, index);
        if (!name) {
            return false;
        }
        const std::filesystem::path candidate = final_dir_ / *name;
        const std::wstring key = folded_path_key(candidate);
        const std::wstring name_key = detail::fold_case(name->native());
        bool taken = false;
        bool own = false;
        {
            std::lock_guard lock(s_.mutex);
            if (s_.reserved_names.contains(key)) {
                continue;  // another running attempt holds it
            }
            taken = context_.taken[dir_key].contains(name_key);
            own = context_.own_outputs.contains(key);
        }
        if (own) {
            continue;  // never replace another output of this batch
        }
        const bool on_disk = path_exists(candidate);
        bool replace = false;
        if (on_disk) {
            if (context_.destination.collisions == CollisionPolicy::UniqueName) {
                std::lock_guard lock(s_.mutex);
                context_.taken[dir_key].insert(name_key);
                continue;
            }
            // Overwrite: never an original photograph of the library.
            auto identity = core::stat_file(candidate);
            const core::FileIdentity id = identity ? *identity : core::FileIdentity{};
            if (s_.library.find_source(candidate, id, 0) || s_.library.find_source(candidate, id, 1)) {
                original_conflict_ = true;
                continue;
            }
            replace = true;
        } else if (taken) {
            // Listed when the folder was read but not found by its exact name now: a case variant in
            // a case-sensitive folder, or a file removed meanwhile. Stay on the safe side.
            continue;
        }
        std::lock_guard lock(s_.mutex);
        if (s_.reserved_names.contains(key)) {
            continue;
        }
        s_.reserved_names.insert(key);
        reserved_key_ = key;
        final_path_ = candidate;
        name_index_ = index;
        replaces_ = replace;
        return true;
    }
    return false;
}

void Attempt::release_name() {
    if (!reserved_key_) {
        return;
    }
    std::lock_guard lock(s_.mutex);
    s_.reserved_names.erase(*reserved_key_);
    reserved_key_.reset();
}

void Attempt::cleanup_temp() {
    if (temp_path_.empty()) {
        return;
    }
    if (Status removed = workers::remove_job_output(temp_path_); !removed) {
        core::log_warn(kWhere, "cannot remove {}: {}", core::path_to_utf8_lossy(temp_path_), removed.error().describe());
    }
}

void Attempt::close_journal() {
    release_name();
    if (!journal_open_) {
        return;
    }
    journal_open_ = false;
    Status closed = s_.catalog->write(
        [&](catalog::WriteTransaction& tx) { return tx.abandon_publication(job_.id, lease_.attempt, s_.now()); });
    if (!closed) {
        // Recovery closes it later (list_publications_in_progress).
        core::log_warn(kWhere, "cannot close the publication of job {}: {}", job_.id.value, closed.error().describe());
    }
}

void Attempt::emit_finished(catalog::JobStage stage, std::optional<Error> error, std::optional<std::filesystem::path> output) {
    BatchEvent event;
    event.kind = BatchEventKind::JobFinished;
    event.batch = job_.batch;
    event.job = job_.id;
    event.item = item_ ? std::optional<ItemId>(item_->id) : std::nullopt;
    event.job_stage = stage;
    event.error = std::move(error);
    event.output = std::move(output);
    if (auto record = s_.batch_record(job_.batch)) {
        event.state = record->state;
        event.counts = record->counts;
    }
    {
        std::lock_guard lock(s_.mutex);
        event.attention = context_.attention;
    }
    s_.emit(std::move(event));
}

void Attempt::give_back() {
    cleanup_temp();
    auto stage = s_.catalog->transact([&](catalog::WriteTransaction& tx) { return tx.release_job(lease_, s_.now()); });
    if (!stage && stage.error().where == "catalog.job.lease_lost") {
        lease_lost();  // expired meanwhile: an interruption
        return;
    }
    close_journal();
    emit_finished(stage ? *stage : catalog::JobStage::Queued, std::nullopt);
}

void Attempt::cancelled() {
    cleanup_temp();
    Status done = s_.catalog->write([&](catalog::WriteTransaction& tx) { return tx.cancel_job(lease_, s_.now()); });
    if (!done) {
        if (done.error().where == "catalog.job.lease_lost") {
            lease_lost();
            return;
        }
        core::log_warn(kWhere, "cannot cancel job {}: {}", job_.id.value, done.error().describe());
    }
    close_journal();
    emit_finished(catalog::JobStage::Cancelled,
                  Error{ErrorCode::Cancelled, "the batch was cancelled", std::string(kWhere), 0});
    s_.check_finished(job_.batch);
}

void Attempt::lease_lost() {
    // Only before publication (the rename): nothing of this attempt is in the output folder but its
    // temporary file, removed here. While nobody else took the lease (it expired: the computer
    // slept, or a step outlasted it), the attempt is recorded as an interruption at once (bounded by
    // max_interruptions); otherwise it is left to the job's new owner or to reconciliation.
    cleanup_temp();
    release_name();
    auto stage = s_.recover_interrupted(lease_, job_, s_.interruption_error(lease_.owner), &context_);
    if (stage) {
        close_journal();
        core::log_warn(kWhere, "job {} lost its lease during attempt {}: {}", job_.id.value, lease_.attempt,
                       catalog::to_string(*stage));
        emit_finished(*stage, s_.interruption_error(lease_.owner));
        s_.check_finished(job_.batch);
        if (*stage == catalog::JobStage::Queued) {
            s_.request_wake();
        }
        return;
    }
    core::log_warn(kWhere, "job {} lost its lease; its attempt {} was abandoned ({})", job_.id.value, lease_.attempt,
                   stage.error().describe());
    std::lock_guard lock(s_.mutex);
    slot_.reclaim_after = true;
}

void Attempt::leave_to_recovery() {
    release_name();
    std::lock_guard lock(s_.mutex);
    slot_.reclaim_after = true;
}

void Attempt::stop_here() {
    switch (stop_reason()) {
        case detail::StopReason::Shutdown:
            give_back();
            break;
        case detail::StopReason::LeaseLost:
            lease_lost();
            break;
        case detail::StopReason::None:
        case detail::StopReason::BatchCancelled:
            cancelled();
            break;
    }
}

void Attempt::fail_job(catalog::FailureClass failure_class, Error error, bool retry_allowed, bool skip) {
    cleanup_temp();
    catalog::JobFailure failure;
    failure.failure_class = failure_class;
    failure.error = error;
    failure.skip = skip;
    const BatchRetryPolicy& retry = context_.policy.retry;
    // A job the workers' aggregate limit stopped beside another worker (worker_failed() marked it
    // exclusive) is always retried once, alone, even when earlier transient failures used up its
    // retry budget: the stop was not its own failure, and alone it may well fit (COR-4, W12; ADR
    // 0012, decision 19). The mark is consumed by that attempt, so a second stop is budgeted again.
    bool exclusive_retry = false;
    if (failure_class == catalog::FailureClass::Transient && retry_allowed) {
        std::lock_guard lock(s_.mutex);
        exclusive_retry = s_.exclusive_jobs.contains(job_.id.value);
    }
    if (failure_class == catalog::FailureClass::Transient && retry_allowed &&
        (job_.transient_failures < retry.max_transient_failures || exclusive_retry)) {
        const std::uint32_t shift = std::min<std::uint32_t>(job_.transient_failures, 20);
        const auto delay = std::min<std::chrono::milliseconds>(retry.max_backoff, retry.initial_backoff * (1LL << shift));
        failure.retry_at = s_.now() + std::chrono::duration_cast<catalog::Timestamp::duration>(delay);
    }
    auto stage = s_.catalog->transact([&](catalog::WriteTransaction& tx) { return tx.record_failure(lease_, failure, s_.now()); });
    if (!stage && stage.error().where == "catalog.job.lease_lost") {
        lease_lost();
        return;
    }
    close_journal();
    if (!stage) {
        core::log_error(kWhere, "cannot record the failure of job {}: {}", job_.id.value, stage.error().describe());
        return;
    }
    {
        std::lock_guard lock(s_.mutex);
        if (*stage == catalog::JobStage::Queued) {
            ++s_.counters.retried;
        } else if (*stage == catalog::JobStage::Failed) {
            ++s_.counters.failed;
        }
    }
    core::log_info(kWhere, "job {} attempt {} ended {}: {}", job_.id.value, lease_.attempt, catalog::to_string(*stage),
                   error.describe());
    emit_finished(*stage, std::move(error));
    s_.check_finished(job_.batch);
    if (*stage == catalog::JobStage::Queued) {
        s_.request_wake();
    }
}

std::optional<Attempt::DestinationProblem> Attempt::destination_problem(const Error& error) {
    const std::filesystem::path& folder = context_.destination.folder;
    auto id = core::stat_file(folder);
    if (!id) {
        if (id.error().code == ErrorCode::PermissionDenied) {
            return DestinationProblem{BatchAttention::DestinationPermission,
                                      Error{ErrorCode::PermissionDenied,
                                            std::format("Lucent may not open the output folder {}", quoted(folder)),
                                            std::string(detail::kWhereDestination), id.error().native_code}};
        }
        return DestinationProblem{
            BatchAttention::DestinationUnavailable,
            Error{ErrorCode::NotFound, std::format("the output folder {} is not available", quoted(folder)),
                  std::string(detail::kWhereDestination), id.error().native_code}};
    }
    if (const std::optional<core::FileIdentity> recorded = s_.folder_identity_of(context_);
        recorded && !core::is_same_file(*id, *recorded)) {
        return DestinationProblem{BatchAttention::DestinationReplaced,
                                  Error{ErrorCode::NotFound,
                                        std::format("{} is no longer the output folder chosen for this batch (it was "
                                                    "created again, or another drive took its place)",
                                                    quoted(folder)),
                                        std::string(detail::kWhereDestinationReplaced), 0}};
    }
    const std::uint64_t needed = s_.output_bytes(context_, a_.pixels);
    const std::filesystem::path probe_dir = final_dir_.empty() ? folder : final_dir_;
    const std::optional<std::uint64_t> free = s_.free_space(detail::is_directory(probe_dir) ? probe_dir : folder);
    if (error.code == ErrorCode::DiskFull || (free && *free < needed + kDestinationReserveBytes)) {
        return DestinationProblem{
            BatchAttention::DestinationFull,
            Error{ErrorCode::DiskFull,
                  free ? std::format("the destination has {} MB free; the output may need up to {} MB", *free >> 20,
                                     needed >> 20)
                       : std::string("the destination is full"),
                  std::string(detail::kWhereDestination), error.native_code},
            free, needed};
    }
    if (error.code == ErrorCode::PermissionDenied) {
        if (Status writable = detail::probe_writable(probe_dir, context_.destination.temp_tag);
            !writable && writable.error().code == ErrorCode::PermissionDenied) {
            return DestinationProblem{BatchAttention::DestinationPermission,
                                      Error{ErrorCode::PermissionDenied,
                                            std::format("Lucent may no longer create files in {}", quoted(probe_dir)),
                                            std::string(detail::kWhereDestination), writable.error().native_code}};
        }
    }
    if (error.code == ErrorCode::IoError && error.native_code > 0 &&
        core::is_volume_absent_win32_error(static_cast<std::uint32_t>(error.native_code)) && !path_exists(folder)) {
        return DestinationProblem{
            BatchAttention::DestinationUnavailable,
            Error{ErrorCode::NotFound, std::format("the output folder {} is not available", quoted(folder)),
                  std::string(detail::kWhereDestination), error.native_code}};
    }
    return std::nullopt;
}

void Attempt::diagnose(Error error, catalog::FailureClass failure_class) {
    if (auto problem = destination_problem(error)) {
        s_.raise_attention(context_, problem->attention, problem->error, problem->free_bytes, problem->needed_bytes);
        give_back();
        return;
    }
    if (item_) {
        auto identity = core::stat_file(item_->path);
        if (!identity && detail::indicates_absence(identity.error())) {
            fail_job(catalog::FailureClass::Deterministic,
                 Error{ErrorCode::NotFound, std::format("the original {} cannot be found", quoted(item_->path)),
                       std::string(detail::kWhereSourceMissing), identity.error().native_code},
                 false);
            return;
        }
        if (identity && *identity != item_->identity) {
            fail_job(catalog::FailureClass::Deterministic,
                 Error{ErrorCode::SourceChanged,
                       std::format("the original {} changed after the batch was created", quoted(item_->path)),
                       std::string(detail::kWhereSourceChanged), 0},
                 false);
            return;
        }
        // The destination accepts files (checked above), so a refusal is about reading the original.
        if (error.code == ErrorCode::PermissionDenied) {
            auto readable = core::ReadOnlyFile::open(item_->path);
            if (!readable && readable.error().code == ErrorCode::PermissionDenied) {
                fail_job(catalog::FailureClass::Deterministic,
                         Error{ErrorCode::PermissionDenied,
                               std::format("Lucent may not read the original {}", quoted(item_->path)),
                               std::string(detail::kWhereSourceAccess), readable.error().native_code},
                         false);
                return;
            }
        }
    }
    if (error.code == ErrorCode::SourceChanged) {
        fail_job(catalog::FailureClass::Deterministic, std::move(error), false);
        return;
    }
    if (error.code == ErrorCode::Unsupported) {
        // Found unsupported while decoding: not exported, and no failure of the batch (as the
        // photographs left out at submission); retrying cannot help.
        fail_job(catalog::FailureClass::Deterministic, std::move(error), false, true);
        return;
    }
    fail_job(failure_class, std::move(error), failure_class == catalog::FailureClass::Transient);
}

void Attempt::ensure_host() {
    const bool fresh = !slot_.host || !slot_.host->info().running;
    if (!fresh) {
        return;
    }
    std::vector<std::string> hooks;
    {
        std::lock_guard lock(s_.mutex);
        const std::uint64_t start = s_.process_starts++;
        ++s_.counters.worker_starts;
        if (start < s_.options.test.worker_hooks_by_start.size()) {
            hooks = s_.options.test.worker_hooks_by_start[start];
        }
    }
    if (!slot_.host || hooks != slot_.host_hooks) {
        workers::WorkerHostOptions options;
        options.executable = s_.options.worker_executable;
        options.memory_limit_bytes = s_.worker_budget;
        options.group = s_.worker_group;
        options.heartbeat_interval = s_.options.heartbeat_interval;
        options.heartbeat_timeout = s_.options.heartbeat_timeout;
        options.cancel_grace = s_.options.cancel_grace;
        options.test_hooks = hooks;
        slot_.host = std::make_unique<workers::WorkerHost>(std::move(options));
        slot_.host_hooks = std::move(hooks);
    }
}

workers::JobOutcome Attempt::run_worker(const workers::JobRequest& request) {
    ensure_host();
    return slot_.host->run_job(request, worker_callbacks(), token_, s_.options.job_timeout);
}

workers::AnalysisOutcome Attempt::run_analysis(const workers::AnalysisRequest& request) {
    ensure_host();
    return slot_.host->run_analysis(request, worker_callbacks(), token_, s_.options.job_timeout);
}

workers::JobCallbacks Attempt::worker_callbacks() {
    workers::JobCallbacks callbacks;
    callbacks.progress = [&](const workers::JobProgress& progress) {
        {
            std::lock_guard lock(s_.mutex);
            slot_.running.stage = progress.stage;
            slot_.running.fraction = progress.fraction;
        }
        BatchEvent event;
        event.kind = BatchEventKind::JobProgress;
        event.batch = job_.batch;
        event.job = job_.id;
        event.item = item_ ? std::optional<ItemId>(item_->id) : std::nullopt;
        event.state = catalog::BatchState::Running;
        event.job_stage = catalog::JobStage::Rendering;
        event.worker_stage = progress.stage;
        event.fraction = progress.fraction;
        s_.emit(std::move(event));
    };
    callbacks.heartbeat = [&](const workers::Heartbeat& heartbeat) {
        {
            std::lock_guard lock(s_.mutex);
            slot_.observed_commit = std::max(slot_.observed_commit, heartbeat.memory.commit);
            if (slot_.host) {
                slot_.pid = slot_.host->info().pid;
            }
        }
        renew_if_due();
    };
    return callbacks;
}

void Attempt::worker_failed(const workers::JobFailure& failure) {
    const detail::StopReason stop = stop_reason();
    if (stop == detail::StopReason::Shutdown) {
        give_back();
        return;
    }
    if (stop == detail::StopReason::LeaseLost) {
        lease_lost();
        return;
    }
    if (failure.failure_class == workers::FailureClass::Cancelled) {
        cancelled();
        return;
    }
    Error error = failure.error;
    if (failure.fault != workers::WorkerFault::None) {
        error.message += std::format(" (worker {})", workers::to_string(failure.fault));
    }
    if (failure.fault == workers::WorkerFault::MemoryLimit || error.code == ErrorCode::LimitExceeded) {
        error.where = error.where.empty() ? std::string(detail::kWhereMemory) : error.where;
    }
    if (failure.fault == workers::WorkerFault::GroupMemoryLimit) {
        // The workers together reached the aggregate limit and this one allocated at it: alone it
        // may well fit, so its retry (transient) is admitted alone.
        std::lock_guard lock(s_.mutex);
        s_.exclusive_jobs.insert(job_.id.value);
        ++s_.counters.aggregate_limit_hits;
    }
    diagnose(std::move(error), to_catalog(failure.failure_class));
}

bool Attempt::automatic_stage() {
    const bool consistent = context_.policy.automatic.keep_consistent;
    const bool pre_pass = consistent && auto_state_ == catalog::AutoJobState::Pending;
    AutoRunRequest auto_request;
    auto_request.final_check = !pre_pass;
    if (pre_pass) {
        auto_request.anchor = context_.policy.automatic.anchors.contains(job_.ordinal);
    }
    if (auto_state_ == catalog::AutoJobState::Analysed) {
        auto plan = s_.consistency_plan(context_);
        if (!plan) {
            fail_job(catalog::FailureClass::Transient, std::move(plan.error()), true);
            return false;
        }
        if (const auto found = (*plan)->targets.find(job_.id.value); found != (*plan)->targets.end()) {
            auto_request.target = found->second;
        }
        // A caller-marked anchor keeps its starting recipe: its look sets its group's target, so it
        // is neither optimised nor replaced (related_shots.hpp). One automatic revision with those
        // settings records its role; the export identity is unchanged.
        if (consistent && context_.policy.automatic.anchors.contains(job_.ordinal) && job_.analysis_json) {
            if (auto analysed = read_auto_run(*job_.analysis_json); analysed && analysed->anchor_appearance) {
                std::optional<analysis::ConsistencyTarget> group;
                if (const auto found = (*plan)->anchors.find(job_.id.value); found != (*plan)->anchors.end()) {
                    group = found->second;
                }
                auto kept = anchor_run(*analysed, std::move(group));
                if (!kept) {
                    fail_job(catalog::FailureClass::Deterministic, std::move(kept.error()), false);
                    return false;
                }
                if (token_.is_cancelled() || stop_reason() != detail::StopReason::None) {
                    stop_here();
                    return false;
                }
                return freeze(*kept, std::nullopt);
            }
        }
    }
    auto request_text = write_auto_request(auto_request);
    if (!request_text) {
        fail_job(catalog::FailureClass::Deterministic, std::move(request_text.error()), false);
        return false;
    }
    workers::AnalysisRequest request;
    request.job_id = std::format("b{}-j{}-a{}-auto", job_.batch.value, job_.id.value, lease_.attempt);
    request.source.path = item_->path;
    request.source.subimage = item_->subimage;
    request.source.identity = item_->identity;
    request.source.sha256 = job_.source_sha256;
    request.recipe = workers::JobRecipe{frozen_.json, frozen_.sha256};
    request.request = std::move(*request_text);
    request.memory_budget_bytes = s_.worker_budget;
    request.allow_unsupported = context_.output.allow_unsupported;
    const std::uint64_t starts_before = slot_.host ? slot_.host->info().starts : 0;
    const bool host_running_before = slot_.host && slot_.host->info().running;
    workers::AnalysisOutcome outcome = run_analysis(request);
    const bool fresh_process = !host_running_before || (slot_.host && slot_.host->info().starts != starts_before);
    {
        std::lock_guard lock(s_.mutex);
        slot_.pid = slot_.host && slot_.host->info().running ? slot_.host->info().pid : 0;
    }
    if (!outcome.succeeded()) {
        worker_failed(*outcome.failure);
        return false;
    }
    point("answered");
    // Nothing is written after a cancellation (ADR 0013 item 3), also when the answer arrived just
    // before it: the recorded analysis, the revisions and the freeze all wait for a running batch.
    if (token_.is_cancelled() || stop_reason() != detail::StopReason::None) {
        stop_here();
        return false;
    }
    calibrate(outcome.metrics, &outcome.result->memory, fresh_process, true);
    {
        // The export that may follow in this attempt measures its own commit.
        std::lock_guard lock(s_.mutex);
        slot_.observed_commit = 0;
    }
    auto run = read_auto_run(outcome.result->document);
    if (!run) {
        Error error = std::move(run.error());
        error.where = std::string(detail::kWhereAutoDocument);
        fail_job(catalog::FailureClass::Deterministic, std::move(error), false);
        return false;
    }
    if (pre_pass) {
        // The durable pre-pass: the analysis is recorded and the job given back until every photo
        // of the batch is analysed (never analysed again after a restart).
        auto released = s_.catalog->transact([&](catalog::WriteTransaction& tx) -> Expected<catalog::JobStage> {
            if (Status recorded = tx.record_job_analysis(lease_, outcome.result->document, s_.now()); !recorded) {
                return Unexpected(std::move(recorded.error()));
            }
            return tx.release_job(lease_, s_.now());
        });
        if (!released) {
            if (released.error().where == "catalog.job.lease_lost") {
                lease_lost();
            } else {
                fail_job(catalog::FailureClass::Transient, std::move(released.error()), true);
            }
            return false;
        }
        point("analysed");
        emit_finished(*released, std::nullopt);
        s_.check_finished(job_.batch);
        s_.request_wake();
        return false;
    }
    if (run->check_failed()) {
        fail_job(catalog::FailureClass::Deterministic,
                 Error{ErrorCode::Internal,
                       "no automatic edit of this photo passed the full-resolution check (the as-shot render is "
                       "numerically invalid)",
                       std::string(detail::kWhereAutoCheck), 0},
                 false);
        return false;
    }
    return freeze(*run, auto_state_ == catalog::AutoJobState::Pending
                            ? std::optional<std::string>(outcome.result->document)
                            : std::nullopt);
}

bool Attempt::freeze(const AutoRun& run, const std::optional<std::string>& analysis_document) {
    auto revisions = automatic_revisions(run, job_.id, s_.now(), s_.recipes.engine_version());
    if (!revisions) {
        fail_job(catalog::FailureClass::Deterministic, std::move(revisions.error()), false);
        return false;
    }
    const catalog::ItemId item{job_.item->value};
    const bool analyze_only = context_.automatic == catalog::AutoMode::AnalyzeOnly;
    // Through the recipe service, under its lock: the effective revision becomes current only when
    // the photo still shows the job's starting revision and no gesture is pending in the editor
    // (ADR 0013 item 3), and no commit can interleave with the write.
    auto frozen = s_.recipes.write_automatic(item_->id, job_.revision, [&](bool make_current) {
        return s_.catalog->transact([&](catalog::WriteTransaction& tx) -> Expected<AutomaticAppend> {
            if (analysis_document) {
                if (Status recorded = tx.record_job_analysis(lease_, *analysis_document, s_.now()); !recorded) {
                    return Unexpected(std::move(recorded.error()));
                }
            }
            auto appended = append_automatic_revisions(tx, item, job_.revision, *revisions, make_current);
            if (!appended) {
                return Unexpected(std::move(appended.error()));
            }
            const catalog::RevisionInfo& effective = appended->revisions.back();
            catalog::AutoFreeze bind;
            bind.revision = effective.id;
            bind.settings_sha256 = effective.settings_sha256;
            if (auto identity = tx.freeze_auto_job(lease_, bind, s_.now()); !identity) {
                return Unexpected(std::move(identity.error()));
            }
            if (analyze_only) {
                if (Status done = tx.complete_without_output(lease_, s_.now()); !done) {
                    return Unexpected(std::move(done.error()));
                }
            }
            return appended;
        });
    });
    if (!frozen) {
        if (frozen.error().where == "catalog.job.lease_lost") {
            lease_lost();
        } else {
            fail_job(catalog::FailureClass::Transient, std::move(frozen.error()), true);
        }
        return false;
    }
    const catalog::RevisionInfo& effective = frozen->revisions.back();
    point("frozen");
    core::log_info(kWhere, "job {}: automatic revision {} of item {} ({} attempt(s), confidence {:.2f}){}",
                   job_.id.value, effective.revision_number, item_->id.value, frozen->revisions.size(),
                   run.confidence, frozen->made_current ? "" : "; the photo was edited meanwhile, so it stays a branch");
    revision_ = effective.id;
    auto_state_ = catalog::AutoJobState::Frozen;
    frozen_.json = revisions->back().revision.settings_json;
    frozen_.sha256 = effective.settings_sha256;
    frozen_.revision_number = effective.revision_number;
    automatic_notes_.clear();
    for (const AutoFlag& flag : run.flags) {
        if (flag.review) {
            automatic_notes_.push_back(describe_auto_flag(flag));
        }
    }
    if (!frozen->made_current) {
        automatic_notes_.emplace_back(detail::kEditedMeanwhileNote);
    }
    if (analyze_only) {
        {
            std::lock_guard lock(s_.mutex);
            ++context_.completed_this_session;
            context_.seconds_this_session += seconds_since(started_);
            ++s_.counters.completed;
        }
        emit_finished(catalog::JobStage::Complete, std::nullopt);
        s_.check_finished(job_.batch);
        return false;
    }
    return true;
}

void Attempt::complete_analysed() {
    Status done = s_.catalog->write([&](catalog::WriteTransaction& tx) { return tx.complete_without_output(lease_, s_.now()); });
    if (!done) {
        if (done.error().where == "catalog.job.lease_lost") {
            lease_lost();
        } else {
            fail_job(catalog::FailureClass::Transient, std::move(done.error()), true);
        }
        return;
    }
    emit_finished(catalog::JobStage::Complete, std::nullopt);
    s_.check_finished(job_.batch);
}

void Attempt::load_automatic_notes() {
    if (!revision_) {
        return;
    }
    // Kept as a branch (the photo was edited meanwhile): the revision's note says so, durably.
    if (auto revision = s_.catalog->query([&](catalog::ReadTransaction& tx) { return tx.find_revision(*revision_); });
        revision && *revision && is_automatic_branch_note((*revision)->info.note)) {
        automatic_notes_.emplace_back(detail::kEditedMeanwhileNote);
    }
    auto record = s_.catalog->query([&](catalog::ReadTransaction& tx) { return tx.find_auto_analysis(*revision_); });
    if (!record || !*record) {
        return;
    }
    nlohmann::json flags = nlohmann::json::parse((*record)->flags_json, nullptr, false);
    if (!flags.is_array()) {
        return;
    }
    for (const nlohmann::json& entry : flags) {
        if (!entry.is_object() || !entry.contains("code") || !entry.contains("reason") || !entry.contains("review")) {
            continue;
        }
        AutoFlag flag;
        flag.code = entry["code"].get<std::uint16_t>();
        flag.reason = entry["reason"].is_string() ? entry["reason"].get<std::string>() : std::string();
        flag.review = entry["review"].is_boolean() && entry["review"].get<bool>();
        if (flag.review) {
            automatic_notes_.push_back(describe_auto_flag(flag));
        }
    }
}

Status Attempt::verify(detail::HeldOutput& held, const workers::JobResult& result, detail::OutputDigest& digest) {
    const auto bad = [&](std::string what) {
        return lucent::fail(ErrorCode::Corrupt,
                           std::format("the output {} failed verification: {}", quoted(temp_path_), what),
                           std::string(detail::kWhereVerify));
    };
    const exporter::ExportSettings& settings = context_.output.settings;
    const workers::JobOutput& out = result.output;
    if (!result.verification.decoded || !result.verification.compared || !result.verification.rehashed ||
        !result.verification.source_unchanged) {
        return bad("the worker did not complete its verification");
    }
    if (result.recipe_sha256 != frozen_.sha256) {
        return bad("the worker rendered another recipe");
    }
    if (out.format != settings.format || out.profile != settings.profile ||
        out.bit_depth != exporter::bit_depth_for(settings.format)) {
        return bad("the worker wrote another format, profile or bit depth");
    }
    auto hashed = held.digest(token_, [this] { renew_if_due(); });
    if (!hashed) {
        return Unexpected(std::move(hashed.error()));
    }
    if (hashed->size_bytes != out.size_bytes || hashed->sha256 != out.sha256) {
        return bad("its size or SHA-256 differs from what the worker verified");
    }
    digest = *hashed;
    auto header = held.inspect_header();
    if (!header) {
        return Unexpected(std::move(header.error()));
    }
    if (header->format != settings.format) {
        return bad("the container is not the chosen format");
    }
    if (header->width != out.width || header->height != out.height || header->width != result.render.width ||
        header->height != result.render.height || header->width == 0 || header->height == 0) {
        return bad(std::format("its dimensions {} x {} differ from the render's {} x {}", header->width,
                               header->height, result.render.width, result.render.height));
    }
    const std::uint32_t bits = settings.format == exporter::ExportFormat::Jpeg ? 8 : 16;
    if (header->components != 3 || header->bits_per_sample != bits) {
        return bad(std::format("it has {} channels of {} bits", header->components, header->bits_per_sample));
    }
    auto icc = exporter::icc_profile(settings.profile);
    if (!icc) {
        return Unexpected(std::move(icc.error()));
    }
    auto expected_icc = core::sha256(std::as_bytes(*icc));
    if (!expected_icc) {
        return Unexpected(std::move(expected_icc.error()));
    }
    if (!header->icc_sha256 || *header->icc_sha256 != *expected_icc || out.icc_sha256 != *expected_icc) {
        return bad("the embedded ICC profile is not the chosen profile");
    }
    renew_if_due();
    auto metadata = exporter::read_export_metadata(held.path());
    if (!metadata) {
        return Unexpected(std::move(metadata.error()));
    }
    const exporter::MetadataPolicy& policy = settings.metadata;
    if (!policy.include_gps && metadata->gps) {
        return bad("it holds a GPS position although GPS was not chosen");
    }
    if (!policy.include_capture && (metadata->has_capture() || !metadata->camera_make.empty())) {
        return bad("it holds capture metadata although it was not chosen");
    }
    if (!policy.include_copyright && (!metadata->artist.empty() || !metadata->copyright.empty())) {
        return bad("it holds artist or copyright text although it was not chosen");
    }
    if (!policy.include_software && !metadata->software.empty()) {
        return bad("it names the software although that was not chosen");
    }
    return ok();
}

Status Attempt::publish(detail::HeldOutput& held) {
    if (replaces_) {
        auto existing = detail::HeldOutput::open_exclusive(final_path_, kWherePublish);
        if (!existing) {
            if (existing.error().code != ErrorCode::NotFound) {
                return Unexpected(std::move(existing.error()));
            }
            replaces_ = false;  // it disappeared meanwhile: a plain no-clobber publication
        } else {
            auto identity = existing->identity();
            if (!identity || !replaced_identity_ || *identity != *replaced_identity_) {
                return lucent::fail(ErrorCode::AlreadyExists,
                                   std::format("{} changed after it was checked; it is not replaced", quoted(final_path_)),
                                   std::string(kWherePublish));
            }
            const std::filesystem::path aside =
                final_dir_ / detail::aside_file_name(context_.destination.temp_tag, job_.id.value, lease_.attempt);
            if (Status moved = existing->rename_no_clobber(aside); !moved) {
                return moved;
            }
            existing->close();
            if (Status placed = held.rename_no_clobber(final_path_); !placed) {
                if (auto back = detail::HeldOutput::open_exclusive(aside, kWherePublish)) {
                    (void)back->rename_no_clobber(final_path_);
                }
                return placed;
            }
            aside_path_ = aside;
        }
    }
    if (!replaces_) {
        if (Status placed = held.rename_no_clobber(final_path_); !placed) {
            return placed;
        }
    }
    if (Status flushed = held.flush(); !flushed) {
        core::log_warn(kWhere, "cannot flush {}: {}", core::path_to_utf8_lossy(final_path_), flushed.error().describe());
    }
    return ok();
}

void Attempt::calibrate(const workers::JobMetrics& metrics, const workers::MemorySnapshot* result_memory,
                        bool fresh_process, bool analysis) {
    std::lock_guard lock(s_.mutex);
    std::uint64_t commit = slot_.observed_commit;
    if (fresh_process) {
        commit = std::max(commit, metrics.peak_commit_bytes);
    }
    if (result_memory != nullptr) {
        commit = std::max(commit, fresh_process ? result_memory->peak_commit : result_memory->commit);
    }
    s_.counters.peak_worker_commit_bytes = std::max({s_.counters.peak_worker_commit_bytes, commit, metrics.peak_commit_bytes});
    if (commit == 0) {
        return;
    }
    s_.calibrated = true;
    const std::uint64_t base = s_.options.memory.worker_baseline_bytes;
    if (a_.pixels >= kCalibrationMinPixels) {
        const double per_pixel = commit > base ? static_cast<double>(commit - base) / static_cast<double>(a_.pixels) : 0.0;
        double& measured = analysis ? s_.measured_auto_bytes_per_pixel : s_.measured_bytes_per_pixel;
        measured = std::max(measured, per_pixel);
    } else {
        s_.measured_floor = std::max(s_.measured_floor, commit);
    }
}

std::vector<std::string> Attempt::review_notes(const workers::JobResult& result) const {
    std::vector<std::string> notes;
    // A Limited photo is exported with a note naming each limitation (blueprint section 3; ADR 0018):
    // the camera support database's own words for a limitation of the camera mode, and the same
    // words for the engine's own limitations the probe showed (support rules 2).
    if (item_ && item_->support.status == SupportStatus::Limited) {
        for (const SupportReason& reason : item_->support.reasons) {
            if ((reason.kind == SupportReasonKind::CameraLimitation ||
                 reason.kind == SupportReasonKind::EngineLimitation) &&
                !reason.message.empty()) {
                notes.push_back("Limited support: " + reason.message);
            }
        }
    }
    if (result.render.interpretation != raw::InterpretationStatus::Complete) {
        if (context_.output.allow_unsupported &&
            app::support_status_for(result.render.interpretation) == SupportStatus::Unsupported) {
            notes.push_back("Exported although Lucent cannot render this camera file faithfully.");
        } else {
            // The interpretation's own reason, when the library recorded one (developer English).
            std::string detail;
            if (item_) {
                for (const SupportReason& reason : item_->support.reasons) {
                    if (reason.kind == SupportReasonKind::InterpretationIssue && reason.issue &&
                        *reason.issue == raw::InterpretationIssueKind::OptionalOperationUnsupported &&
                        !reason.message.empty()) {
                        detail = " (" + reason.message + ")";
                        break;
                    }
                }
            }
            notes.push_back("Limited support: some optional processing for this camera was not available" +
                            detail + ".");
        }
    }
    for (std::size_t i = 0; i < result.render.warnings.size() && i < 3; ++i) {
        notes.push_back("The renderer reported: " + result.render.warnings[i]);
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(result.render.width) * result.render.height;
    if (pixels > 0 && result.render.limited_display_pixels * 100 > pixels) {
        notes.push_back(std::format("{:.1f} % of the pixels are brighter than the output can show.",
                                    100.0 * static_cast<double>(result.render.limited_display_pixels) /
                                        static_cast<double>(pixels)));
    }
    if (!result.output.metadata_notes.empty()) {
        notes.push_back("Some metadata was changed or left out: " + result.output.metadata_notes.front());
    }
    if (original_conflict_) {
        notes.push_back("An original photo has the planned name, so the output was saved under a new name.");
    }
    if (name_index_ > 1) {
        notes.push_back(std::format("Saved as {} because {} already existed.",
                                    core::path_to_utf8_lossy(final_path_.filename()),
                                    core::path_to_utf8_lossy(planned_name_)));
    }
    if (replaces_) {
        notes.push_back("Replaced the existing file " + core::path_to_utf8_lossy(final_path_.filename()) + ".");
    }
    if (item_ && item_->probe && !item_->probe->upright_size.empty()) {
        // The size the recipe's crop, straightening and quarter turns give the photo as catalogued:
        // a geometry edit changes the output size by design.
        const image::Size catalogued = item_->probe->upright_size;
        image::Size expected = catalogued;
        bool camera_area = false;
        if (auto settings = read_recipe(frozen_.json)) {
            if (auto mapping = engine::GeometryMapping::create(settings->geometry, catalogued)) {
                expected = mapping->output_size();
            }
            camera_area = engine::geometry_operation_version(*settings) >= 2;
        }
        // Geometry version 2 crops and straightens within the camera's image area (ADR 0017), which
        // the catalog does not record: the output may then be smaller than the catalogued frame
        // gives by the camera's border (at most kCameraBorderTolerance of a side), never larger.
        const auto within = [&](std::int32_t full, std::uint32_t got) {
            const auto g = static_cast<std::int64_t>(got);
            return camera_area ? g <= full && static_cast<double>(g) >=
                                                  std::floor(full * (1.0 - kCameraBorderTolerance))
                               : g == full;
        };
        if (!within(expected.width, result.render.width) || !within(expected.height, result.render.height)) {
            notes.push_back(
                expected == catalogued
                    ? std::format("The output is {} x {} pixels; the photo was catalogued as {} x {}.",
                                  result.render.width, result.render.height, catalogued.width, catalogued.height)
                    : std::format("The output is {} x {} pixels; the photo was catalogued as {} x {}, which its "
                                  "crop and rotation make {} x {}.",
                                  result.render.width, result.render.height, catalogued.width, catalogued.height,
                                  expected.width, expected.height));
        }
    }
    return notes;
}

void Attempt::run() {
    {
        BatchEvent event;
        event.kind = BatchEventKind::JobStarted;
        event.batch = job_.batch;
        event.job = job_.id;
        event.item = a_.item ? std::optional<ItemId>(a_.item->id) : std::nullopt;
        event.state = catalog::BatchState::Running;
        event.job_stage = catalog::JobStage::Rendering;
        s_.emit(std::move(event));
    }
    if (stop_reason() == detail::StopReason::Shutdown) {
        give_back();
        return;
    }

    // ---- Inputs -------------------------------------------------------------------------------
    item_ = job_.item ? s_.library.find(from_catalog(*job_.item)) : std::nullopt;
    if (!item_) {
        fail_job(catalog::FailureClass::Deterministic,
             Error{ErrorCode::NotFound, "the photo was removed from the library", std::string(detail::kWhereRemoved), 0},
             false);
        return;
    }
    // AST-07: a photo that became Unsupported after the batch was created (a database update listing
    // its mode as Unsupported, a re-probe) is skipped like the ones left out at creation (blueprint
    // section 3: "skip automatic export and give a useful reason"); resumes and retries included.
    if (item_->support.status == SupportStatus::Unsupported && !context_.output.allow_unsupported) {
        std::string reasons;
        for (const SupportReason& reason : item_->support.reasons) {
            reasons += reasons.empty() ? reason.message : "; " + reason.message;
        }
        fail_job(catalog::FailureClass::Deterministic,
                 Error{ErrorCode::Unsupported, std::format("{} is not supported: {}", quoted(item_->path), reasons),
                       "app.batch.support", 0},
                 false, true);
        return;
    }
    auto frozen = s_.frozen_document(job_, item_->id);
    if (!frozen) {
        fail_job(catalog::FailureClass::Deterministic, std::move(frozen.error()), false);
        return;
    }
    frozen_ = std::move(*frozen);
    revision_ = job_.revision;
    auto_state_ = job_.auto_state;
    if (item_->source_state == SourceState::Missing) {
        fail_job(catalog::FailureClass::Deterministic,
             Error{ErrorCode::NotFound, std::format("the original {} cannot be found", quoted(item_->path)),
                   std::string(detail::kWhereSourceMissing), 0},
             false);
        return;
    }
    if (!item_->sha256 || *item_->sha256 != job_.source_sha256 || item_->source_state == SourceState::Changed) {
        fail_job(catalog::FailureClass::Deterministic,
             Error{ErrorCode::SourceChanged,
                   std::format("the original {} changed after the batch was created", quoted(item_->path)),
                   std::string(detail::kWhereSourceChanged), 0},
             false);
        return;
    }

    // ---- Automatic settings -----------------------------------------------------------------------
    if (auto_state_ == catalog::AutoJobState::Pending || auto_state_ == catalog::AutoJobState::Analysed) {
        if (!automatic_stage()) {
            return;  // the stage ended the attempt (pre-pass, analyse-only, failure, stop)
        }
    } else if (auto_state_ == catalog::AutoJobState::Frozen) {
        if (context_.automatic == catalog::AutoMode::AnalyzeOnly) {
            complete_analysed();
            return;
        }
        load_automatic_notes();
    }
    if (token_.is_cancelled()) {
        stop_here();
        return;
    }

    // ---- Reservation ----------------------------------------------------------------------------
    final_dir_ = output_directory_for(context_.destination.folder, context_.destination.mirror_subfolders,
                                      context_.destination.mirror_root, item_->path);
    // The output folder itself is never created after submission (a folder deleted meanwhile is
    // reported, not silently made again as another folder object): it must be there, as recorded.
    // Only mirrored subfolders below it are created.
    {
        auto root = core::stat_file(context_.destination.folder);
        const std::optional<core::FileIdentity> recorded = s_.folder_identity_of(context_);
        if (!root || (recorded && !core::is_same_file(*root, *recorded))) {
            diagnose(root ? Error{ErrorCode::NotFound, "the output folder was replaced",
                                  std::string(detail::kWhereDestinationReplaced), 0}
                          : root.error(),
                     catalog::FailureClass::Transient);
            return;
        }
    }
    if (!detail::same_path_text(final_dir_, context_.destination.folder)) {
        if (Status made = core::create_directory_tree(final_dir_); !made) {
            diagnose(std::move(made.error()), catalog::FailureClass::Transient);
            return;
        }
    }
    if (!choose_name(1)) {
        fail_job(catalog::FailureClass::Deterministic,
             Error{ErrorCode::LimitExceeded,
                   std::format("every output name up to index {} is taken in {}", kMaxOutputNameIndex, quoted(final_dir_)),
                   "app.batch.name", 0},
             false);
        return;
    }
    if (replaces_) {
        replaced_identity_ = core::stat_file(final_path_).value_or(core::FileIdentity{});
    }
    temp_path_ = final_dir_ / detail::temp_file_name(context_.destination.temp_tag, job_.id.value, lease_.attempt);
    (void)workers::remove_job_output(temp_path_);  // a stale file of this name is never reused
    if (!journal(catalog::PublicationStep::Reserved)) {
        give_back();
        return;
    }
    point("reserved");

    // ---- Worker ---------------------------------------------------------------------------------
    workers::JobRequest request;
    request.job_id = std::format("b{}-j{}-a{}", job_.batch.value, job_.id.value, lease_.attempt);
    request.source.path = item_->path;
    request.source.subimage = item_->subimage;
    request.source.identity = item_->identity;
    request.source.sha256 = job_.source_sha256;
    request.recipe = workers::JobRecipe{frozen_.json, frozen_.sha256};
    request.output_settings = context_.output.settings;
    request.artist = context_.output.artist;
    request.copyright = context_.output.copyright;
    request.temp_path = temp_path_;
    request.memory_budget_bytes = s_.worker_budget;
    request.allow_unsupported = context_.output.allow_unsupported;
    const std::uint64_t starts_before = slot_.host ? slot_.host->info().starts : 0;
    const bool host_running_before = slot_.host && slot_.host->info().running;
    workers::JobOutcome outcome = run_worker(request);
    const bool fresh_process = !host_running_before || (slot_.host && slot_.host->info().starts != starts_before);
    {
        std::lock_guard lock(s_.mutex);
        slot_.pid = slot_.host && slot_.host->info().running ? slot_.host->info().pid : 0;
    }
    if (!outcome.succeeded()) {
        worker_failed(*outcome.failure);
        return;
    }
    calibrate(outcome.metrics, outcome.result ? &outcome.result->memory : nullptr, fresh_process, false);
    const workers::JobResult& result = *outcome.result;
    if (!journal(catalog::PublicationStep::TempWritten)) {
        give_back();
        return;
    }
    point("temp_written");
    if (Status advanced = advance(catalog::JobStage::Rendering); !advanced) {
        if (advanced.error().where == "catalog.job.batch_cancelled") {
            cancelled();
        } else {
            lease_lost();
        }
        return;
    }

    // ---- Verification -----------------------------------------------------------------------------
    if (token_.is_cancelled()) {
        stop_here();
        return;
    }
    const auto verify_start = Clock::now();
    auto held = detail::HeldOutput::open(temp_path_, detail::kWhereVerify);
    if (!held) {
        diagnose(std::move(held.error()), catalog::FailureClass::Transient);
        return;
    }
    detail::OutputDigest digest;
    if (Status verified = verify(*held, result, digest); !verified) {
        held->close();
        if (verified.error().code == ErrorCode::Cancelled) {
            stop_here();
            return;
        }
        fail_job(catalog::FailureClass::Transient, std::move(verified.error()), true);
        return;
    }
    renew_if_due(true);
    // The name again: a file that appeared meanwhile is never replaced (UniqueName), and a file to
    // replace is identified now and revalidated at publication (Overwrite).
    if (path_exists(final_path_) && !replaces_) {
        release_name();
        const std::uint32_t next = name_index_ + 1;
        if (!choose_name(next)) {
            held->close();
            fail_job(catalog::FailureClass::Transient,
                 Error{ErrorCode::AlreadyExists, "no free output name is left", "app.batch.name", 0}, true);
            return;
        }
        if (replaces_) {
            replaced_identity_ = core::stat_file(final_path_).value_or(core::FileIdentity{});
        }
    } else if (replaces_ && !path_exists(final_path_)) {
        replaces_ = false;
    }
    const double verify_seconds = seconds_since(verify_start);
    // From here on every early exit closes the held file first: the clean-up removes the temporary
    // file by name, which the handle (no delete sharing) would refuse.
    if (!journal(catalog::PublicationStep::Verified, digest)) {
        held->close();
        give_back();
        return;
    }
    point("verified");
    if (Status advanced = advance(catalog::JobStage::Verifying); !advanced) {
        held->close();
        if (advanced.error().where == "catalog.job.batch_cancelled") {
            cancelled();
        } else {
            lease_lost();
        }
        return;
    }
    if (token_.is_cancelled()) {  // the last safe boundary before publication
        held->close();
        stop_here();
        return;
    }

    // ---- Publication ------------------------------------------------------------------------------
    point("before_rename");
    if (Status published = publish(*held); !published) {
        held->close();
        if (published.error().code == ErrorCode::AlreadyExists) {
            fail_job(catalog::FailureClass::Transient, std::move(published.error()), true);
        } else {
            diagnose(std::move(published.error()), catalog::FailureClass::Transient);
        }
        return;
    }
    point("after_rename");
    if (!journal(catalog::PublicationStep::Published, digest)) {
        // Left to recovery: the unrecorded output is reconciled once the slot is free.
        held->close();
        leave_to_recovery();
        return;
    }
    point("published");
    if (aside_path_) {
        if (Status removed = detail::remove_unused_file(*aside_path_, kWherePublish); !removed) {
            core::log_warn(kWhere, "cannot remove the replaced file {}: {}", core::path_to_utf8_lossy(*aside_path_),
                           removed.error().describe());
        }
    }

    // ---- Receipt --------------------------------------------------------------------------------
    ReceiptDetails details;
    details.revision = revision_;
    details.revision_number = frozen_.revision_number;
    details.recipe_sha256 = frozen_.sha256;
    details.export_settings_sha256 = context_.export_settings_sha256;
    details.settings = context_.output.settings;
    details.format = result.output.format;
    details.profile = result.output.profile;
    details.bit_depth = result.output.bit_depth;
    details.width = result.output.width;
    details.height = result.output.height;
    details.icc_sha256 = result.output.icc_sha256;
    details.chroma_subsampling = result.output.chroma_subsampling;
    details.encoder = result.output.encoder;
    details.metadata_notes = result.output.metadata_notes;
    details.interpretation = result.render.interpretation;
    details.warnings = result.render.warnings;
    details.demosaic = result.render.demosaic;
    details.camera_color = result.render.camera_color;
    details.pipeline_version = result.render.pipeline_version;
    details.look = result.render.look;
    details.look_version = result.render.look_version;
    details.clipped_raw_samples = result.render.clipped_raw_samples;
    details.limited_display_pixels = result.render.limited_display_pixels;
    details.estimated_peak_bytes = result.render.estimated_peak_bytes;
    details.worker_peak_commit_bytes = outcome.metrics.peak_commit_bytes;
    details.prepare_seconds = result.timings.prepare_seconds;
    details.render_seconds = result.timings.render_seconds;
    details.encode_seconds = result.timings.encode_seconds;
    details.write_seconds = result.timings.write_seconds;
    details.verify_seconds = result.timings.verify_seconds + verify_seconds;
    details.total_seconds = seconds_since(started_);
    details.attempt = lease_.attempt;
    details.name_index = name_index_;
    details.replaced_existing = replaces_;
    details.allow_unsupported = context_.output.allow_unsupported;
    details.review = review_notes(result);
    details.review.insert(details.review.end(), automatic_notes_.begin(), automatic_notes_.end());
    if (item_) {
        details.support = item_->support.status;
        if (item_->support.camera) {
            details.camera_support_version = item_->support.camera->database_version;
        }
    }
    auto details_json = write_receipt_details(details);
    catalog::NewReceipt receipt;
    receipt.final_path = final_path_;
    receipt.size_bytes = digest.size_bytes;
    receipt.output_sha256 = digest.sha256;
    receipt.settings_json = details_json ? std::move(*details_json) : std::string("{}");
    Status recorded = lucent::fail(ErrorCode::Internal, "not recorded", std::string(kWhere));
    for (int attempt = 0; attempt < kCatalogAttempts; ++attempt) {
        auto stored = s_.catalog->transact(
            [&](catalog::WriteTransaction& tx) { return tx.record_receipt(lease_, receipt, s_.now()); });
        recorded = stored ? ok() : Status(Unexpected(stored.error()));
        if (recorded || recorded.error().code != ErrorCode::IoError) {
            break;
        }
        std::this_thread::sleep_for(50ms);
    }
    held->close();
    if (!recorded) {
        core::log_error(kWhere, "cannot record the receipt of job {}: {}", job_.id.value, recorded.error().describe());
        leave_to_recovery();  // recovery completes it from the Published step
        return;
    }
    journal_open_ = false;
    point("receipt");
    if (Status advanced = advance(catalog::JobStage::Publishing); !advanced) {
        core::log_error(kWhere, "cannot complete job {}: {}", job_.id.value, advanced.error().describe());
        leave_to_recovery();  // recovery completes it from its receipt
        return;
    }
    point("complete");
    const double seconds = seconds_since(started_);
    {
        std::lock_guard lock(s_.mutex);
        const std::wstring key = folded_path_key(final_path_);
        context_.own_outputs.insert(key);
        context_.taken[folded_path_key(final_dir_)].insert(detail::fold_case(final_path_.filename().native()));
        ++context_.completed_this_session;
        context_.seconds_this_session += seconds;
        ++s_.counters.completed;
        if (context_.free_bytes) {
            *context_.free_bytes = *context_.free_bytes > digest.size_bytes ? *context_.free_bytes - digest.size_bytes : 0;
        }
    }
    release_name();
    core::log_info(kWhere, "job {} published {} ({} bytes, {:.2f} s)", job_.id.value,
                   core::path_to_utf8_lossy(final_path_), digest.size_bytes, seconds);
    emit_finished(catalog::JobStage::Complete, std::nullopt, final_path_);
    s_.check_finished(job_.batch);
}

}  // namespace

void BatchService::Impl::run_attempt(detail::Slot& slot, detail::Assignment& assignment) {
    Attempt attempt(*this, slot, assignment);
    attempt.run();
}

}  // namespace lucent::app
