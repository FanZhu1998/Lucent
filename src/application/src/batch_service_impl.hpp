#pragma once

// Private: the state of BatchService shared by batch_service.cpp (open, recovery, submission,
// commands, status), batch_scheduler.cpp (coordinator, worker slots, publication) and
// batch_report.cpp (reports). See batch_service.hpp for the contract and ADR 0012 for the design.
//
// Locking. Impl::mutex guards every mutable member marked "(mutex)". The catalog, the library,
// the recipe service, file systems and observers are never called while it is held: catalog calls
// wait for the writer lock and observers may call back into the service.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <lucent/application/batch_destination.hpp>
#include <lucent/application/batch_service.hpp>
#include <lucent/application/recipe.hpp>
#include <lucent/application/recipe_service.hpp>
#include <lucent/catalog/catalog.hpp>
#include <lucent/core/cancellation.hpp>
#include <lucent/workers/worker_host.hpp>

#include "batch_documents.hpp"
#include "observer_list.hpp"

namespace lucent::app {

namespace detail {

// A batch's documents, parsed once (immutable after load, except the folder's identity, which
// use_replaced_folder() may replace: read it with Impl::folder_identity_of) plus the coordinator's
// runtime state.
struct BatchContext {
    catalog::BatchId id;
    std::string name;
    DestinationDocument destination;
    NamePattern pattern;
    ExportDocument output;
    PolicyDocument policy;
    std::string renderer_version;
    core::Digest256 export_settings_sha256{};

    // ---- runtime (Impl::mutex) ----
    std::optional<core::FileIdentity> folder_identity;  // destination.folder_identity, maybe replaced
    BatchAttention attention = BatchAttention::None;
    std::optional<Error> attention_error;
    std::optional<std::uint64_t> attention_free_bytes;  // DestinationFull
    std::uint64_t attention_needed_bytes = 0;
    // BatchStatus facts read from the catalog, valid while the counts they were read at hold.
    std::optional<catalog::BatchCounts> facts_counts;
    catalog::BatchJobFacts facts;
    // Names known to be taken per final folder: folded folder path -> folded file names. Filled
    // by one listing per folder, then by this session's reservations and publications.
    std::map<std::wstring, std::unordered_set<std::wstring>> taken;
    // Folded final paths this batch published in this session or earlier (Overwrite never
    // replaces the batch's own outputs).
    std::unordered_set<std::wstring> own_outputs;
    bool own_outputs_loaded = false;
    std::chrono::steady_clock::time_point destination_checked{};
    std::optional<std::uint64_t> free_bytes;  // at destination_checked
    std::uint64_t completed_this_session = 0;
    double seconds_this_session = 0.0;
};

struct FrozenDocument {
    std::string json;
    core::Digest256 sha256{};
    std::uint32_t revision_number = 0;  // 0: as shot
};

struct Assignment {
    catalog::ClaimedJob claim;
    std::shared_ptr<BatchContext> context;
    std::optional<SourceItem> item;
    std::uint64_t pixels = 0;
    std::uint64_t estimate_bytes = 0;
};

enum class StopReason : std::uint8_t {
    None = 0,
    BatchCancelled = 1,  // the batch was cancelled: the attempt ends Cancelled
    Shutdown = 2,        // the service is closing: the job is given back (Queued)
    LeaseLost = 3,       // the lease was taken away or expired: the attempt is an interruption
};

struct Slot {
    std::uint32_t index = 0;
    std::thread thread;
    std::condition_variable cv;
    std::unique_ptr<workers::WorkerHost> host;  // the slot thread's own
    std::vector<std::string> host_hooks;        // test hooks of `host`

    // ---- (mutex) ----
    std::optional<Assignment> assignment;
    bool busy = false;
    std::string owner;  // the lease owner of the running attempt
    core::CancellationSource cancel;
    StopReason stop = StopReason::None;
    RunningJob running;
    catalog::BatchId batch;
    std::uint64_t estimate_bytes = 0;
    std::uint64_t observed_commit = 0;  // largest commit heartbeats reported in this attempt
    std::uint32_t pid = 0;
    bool reclaim_after = false;  // the attempt left its job leased: reconcile it once the slot is free
};

// Where the batch's output folder stands (recovery decides nothing about outputs it cannot see).
enum class Reach : std::uint8_t {
    Reachable = 1,
    Unreachable = 2,  // the folder or its drive cannot be opened
    Replaced = 3,     // another folder object is at its path
};

// A file compared with a recorded size and SHA-256.
enum class FileCheck : std::uint8_t {
    Match = 1,
    Mismatch = 2,  // absent (with its folder reachable), another size, or other bytes
    Unknown = 3,   // it could not be read (a sharing violation, an I/O error, changed while read)
};

}  // namespace detail

struct BatchService::Impl {
    Impl(Library& l, std::shared_ptr<catalog::Catalog> c, RecipeService& r, BatchServiceOptions o)
        : library(l), catalog(std::move(c)), recipes(r), options(std::move(o)) {}

    Library& library;
    std::shared_ptr<catalog::Catalog> catalog;
    RecipeService& recipes;
    BatchServiceOptions options;
    std::string session;  // random hex: lease owners of this service instance
    std::string renderer_version;
    std::uint64_t total_budget = 0;
    std::uint64_t worker_budget = 0;
    detail::ObserverList<BatchEvent> observers{"app.batch"};

    mutable std::mutex mutex;
    std::condition_variable coordinator_cv;
    bool stopping = false;          // (mutex)
    bool wake_requested = false;    // (mutex)
    bool reclaim_requested = false;  // (mutex) reconcile abandoned leases at the next wake-up
    bool work_waiting = true;       // (mutex) the last dispatch saw queued jobs of running batches
    std::thread coordinator;
    std::vector<std::unique_ptr<detail::Slot>> slots;
    // (mutex) Batches with work: parsed documents and runtime state. A batch's context is dropped
    // when it finishes (its name caches can be large) and parsed again when needed.
    mutable std::map<std::int64_t, std::shared_ptr<detail::BatchContext>> contexts;
    std::unordered_set<std::int64_t> finished_batches;  // (mutex) Finished already announced
    // (mutex) Ids of the unfinished batches (Draft, Running, Paused, or Cancelled with jobs still
    // leased): the only ones dispatch() looks at, oldest first, however many finished batches the
    // catalog holds. Filled by the scan of every batch at recovery (active_scanned; repeated by
    // dispatch() until it succeeds) and by submit / start / resume / retry and check_finished() (this
    // service is the catalog's only batch writer); only dispatch() drops finished batches from it.
    std::set<std::int64_t> active_batches;
    bool active_scanned = false;
    // (mutex) Increases whenever a batch (re)joins active_batches, so dispatch() never drops a batch
    // that was reopened (retry, start) after it read the batch as finished.
    std::uint64_t active_generation = 0;
    std::unordered_set<std::wstring> reserved_names;  // (mutex) folded final paths of running attempts
    std::uint64_t attempt_counter = 0;                // (mutex) lease owner suffixes
    std::uint64_t process_starts = 0;                 // (mutex) worker processes started
    // Calibration (mutex).
    bool calibrated = false;
    double measured_bytes_per_pixel = 0.0;
    std::uint64_t measured_floor = 0;  // largest commit of a job below the calibration size
    BatchSchedulerStats counters;      // (mutex) cumulative members only
    RecoveryReport recovery;           // (mutex)
    bool recovered = false;            // (mutex) recover() has run (on the coordinator thread)
    // (mutex) Reconciliations left for later because the output folder was out of reach or an
    // output could not be read: jobs (by id) and open publications (job id, attempt). Retried by
    // reclaim_abandoned(); logged once.
    std::set<std::int64_t> deferred_jobs;
    std::set<std::pair<std::int64_t, std::uint32_t>> deferred_publications;
    mutable std::condition_variable recovered_cv;

    [[nodiscard]] catalog::Timestamp now() const {
        return options.clock ? options.clock() : catalog::current_time();
    }

    // ---- batch_service.cpp ----
    // The batch's context: the cached one, or parsed from its record. Only a batch with work left is
    // cached (`contexts`); a finished batch's context is parsed for the caller alone, so looking at old
    // batches (status, reports, every single export's batch) never makes the cache grow.
    [[nodiscard]] Expected<std::shared_ptr<detail::BatchContext>> context_for(catalog::BatchId id) const;
    [[nodiscard]] Expected<std::shared_ptr<detail::BatchContext>> load_context(const catalog::BatchRecord& record,
                                                                              bool cache = true) const;
    [[nodiscard]] Expected<catalog::BatchRecord> batch_record(catalog::BatchId id) const;
    void emit(BatchEvent event) const;
    void emit_state(catalog::BatchId id, BatchEventKind kind, std::optional<Error> error = std::nullopt);
    // Emits Finished (and writes the report) once when the batch has nothing left to run.
    void check_finished(catalog::BatchId id);
    void request_wake() noexcept;
    // Reconciles abandoned leases and deferred publications at the next wake-up.
    void request_reclaim() noexcept;
    // Adds `id` to active_batches.
    void note_active(catalog::BatchId id);
    // Pages through every batch: unfinished ones join active_batches (and active_scanned is set when
    // the scan completed); `visit` sees each record (recovery's leftover sweep).
    [[nodiscard]] Status scan_batches(const std::function<void(const catalog::BatchRecord&)>& visit = {});
    [[nodiscard]] Status recover();
    // Reconciles a job whose lease belongs to no running attempt (see "Recovery"); false when it
    // was left for later (output folder out of reach, an output unreadable, a catalog error). Paths
    // a deferred reconciliation still needs (its temporary and set-aside files) are added to `keep`.
    bool reconcile_job(const catalog::JobRecord& job, std::vector<std::filesystem::path>* keep = nullptr);
    // Cleans up the open publication of an attempt that no longer holds a lease and closes it;
    // false when it was left for later (as reconcile_job).
    bool close_publication(const catalog::PublicationInProgress& open,
                           std::vector<std::filesystem::path>* keep = nullptr);
    // Requeues (or, past BatchRetryPolicy::max_interruptions, fails) the job of an interrupted
    // attempt whose lease is `lease`, recording `error`; the job's stage afterwards.
    [[nodiscard]] Expected<catalog::JobStage> recover_interrupted(const catalog::JobLease& lease,
                                                                  const catalog::JobRecord& job, Error error,
                                                                  const detail::BatchContext* context);
    // The error recorded for an attempt interrupted under `owner` (a previous session: Lucent
    // stopped; this one: the lease expired).
    [[nodiscard]] Error interruption_error(std::string_view owner) const;
    [[nodiscard]] detail::Reach reach_destination(const detail::BatchContext& context) const;
    [[nodiscard]] std::optional<core::FileIdentity> folder_identity_of(const detail::BatchContext& context) const;
    // Writes the reports of batches that finished without them (Lucent stopped in between).
    void report_unreported();
    void note_deferred_job(catalog::JobId job, std::string_view why);
    [[nodiscard]] std::optional<std::uint64_t> free_space(const std::filesystem::path& directory) const;
    // The canonical settings document the job renders (its revision's, or the as-shot defaults),
    // verified against the job's settings SHA-256.
    [[nodiscard]] Expected<detail::FrozenDocument> frozen_document(const catalog::JobRecord& job, ItemId item) const;

    // ---- batch_scheduler.cpp ----
    void start_threads();
    void stop_threads() noexcept;
    void coordinator_main();
    // The running batches with queued jobs among active_batches, oldest first (finished ones are
    // dropped from the set).
    [[nodiscard]] std::vector<catalog::BatchRecord> dispatchable_batches();
    void dispatch();
    void reclaim_abandoned();
    void slot_main(detail::Slot& slot);
    void run_attempt(detail::Slot& slot, detail::Assignment& assignment);
    [[nodiscard]] std::uint64_t estimate_bytes(std::uint64_t pixels) const;  // (mutex) held by caller
    [[nodiscard]] std::uint64_t output_bytes(const detail::BatchContext& context, std::uint64_t pixels) const;
    // Checks the batch's folder; pauses the batch with an attention when it is not usable.
    [[nodiscard]] bool destination_usable(detail::BatchContext& context, std::uint64_t needed_bytes);
    void raise_attention(detail::BatchContext& context, BatchAttention attention, Error error,
                         std::optional<std::uint64_t> free_bytes = std::nullopt, std::uint64_t needed_bytes = 0);
    [[nodiscard]] std::vector<detail::Slot*> busy_slots_of(catalog::BatchId batch);

    // ---- batch_report.cpp ----
    [[nodiscard]] Expected<BatchReport> build_report(catalog::BatchId id) const;
    [[nodiscard]] Expected<ReportFiles> write_report_files(catalog::BatchId id);
};

namespace detail {

// Nothing of the batch will run any more: Completed, or Cancelled with no job still leased.
[[nodiscard]] inline bool is_finished_batch(const catalog::BatchRecord& record) noexcept {
    return record.state == catalog::BatchState::Completed ||
           (record.state == catalog::BatchState::Cancelled && record.counts.leased() == 0);
}

// Lowercase hex of `bytes` random bytes (BCryptGenRandom).
[[nodiscard]] std::string random_hex(std::size_t bytes);

// The total and available physical memory (GlobalMemoryStatusEx); zeros when unknown.
struct PhysicalMemory {
    std::uint64_t total = 0;
    std::uint64_t available = 0;
};
[[nodiscard]] PhysicalMemory physical_memory() noexcept;

// The pixels of an item's output (upright size from its probe; 0 when unknown).
[[nodiscard]] std::uint64_t output_pixels(const SourceItem& item) noexcept;

// `path` compared with `size` and `sha256` (stat, then a stable hash read).
[[nodiscard]] FileCheck check_file(const std::filesystem::path& path, std::uint64_t size, const core::Digest256& sha256,
                                   const core::CancellationToken& cancel = {});

// User-facing English for a job's outcome (report.hpp's reason and action).
struct Wording {
    std::string reason;
    std::string action;
};
[[nodiscard]] Wording describe_failure(const Error& error);

// Where values recorded by the scheduler (stable; tests and the report read them).
inline constexpr std::string_view kWhereSourceMissing = "app.batch.source.missing";
inline constexpr std::string_view kWhereSourceChanged = "app.batch.source.changed";
inline constexpr std::string_view kWhereSourceAccess = "app.batch.source.access";
inline constexpr std::string_view kWhereRemoved = "app.batch.source.removed";
inline constexpr std::string_view kWhereExisting = "app.batch.existing";
inline constexpr std::string_view kWhereInterrupted = "app.batch.interrupted";
inline constexpr std::string_view kWhereLeaseExpired = "app.batch.interrupted.lease";
inline constexpr std::string_view kWhereInterruptedTooOften = "app.batch.interrupted.limit";
inline constexpr std::string_view kWhereVerify = "app.batch.verify";
inline constexpr std::string_view kWhereRecipe = "app.batch.recipe";
inline constexpr std::string_view kWhereOutputLost = "app.batch.output_lost";
inline constexpr std::string_view kWhereDestination = "app.batch.destination";
inline constexpr std::string_view kWhereDestinationReplaced = "app.batch.destination.replaced";
inline constexpr std::string_view kWhereMemory = "app.batch.memory";

}  // namespace detail

}  // namespace lucent::app
