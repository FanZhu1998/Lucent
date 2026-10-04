#pragma once

// Value types of the durable batch journal (catalog schema v3, W7; blueprint section 7, "Durable,
// memory-bounded batch processing"). The operations are members of ReadTransaction and
// WriteTransaction (catalog.hpp); this header holds what they take and return.
//
// Vocabulary
//   Batch    one export run the photographer started: a selection of items, a destination
//            specification, export settings and a policy (all opaque JSON owned by the
//            application), and a state (Draft, Running, Paused, Cancelled, Completed).
//   Job      one item of a batch: the frozen recipe (revision id + settings SHA-256), the job
//            identity (compute_job_identity), its stage, attempt count, failure history, lease
//            and the temporary / final output paths.
//   Lease    the exclusive right of one owner (a coordinator-side name of a worker slot) to work on
//            a claimed job until `expires_at`. Every change made on behalf of a worker presents the
//            lease (job, owner, attempt) and is refused once the lease is lost: expired, recovered
//            by the coordinator, or superseded by a later claim (the attempt number then differs).
//   Event    an append-only record of a stage transition, for the completion report.
//   Journal  the publication journal: append-only steps of publishing one attempt's output
//            (Reserved -> TempWritten -> Verified -> Published -> ReceiptCommitted, or Abandoned
//            after reconciliation), so an interruption between any two steps can be reconciled.
//   Receipt  the durable record that a job's output was published: final path, size, SHA-256
//            and the settings it was rendered with. A job reaches Complete only with a receipt.
//
// Job stages (blueprint section 7, "Job lifecycle"):
//
//     Queued --claim--> Rendering --> Verifying --> Publishing --> Complete (needs a receipt)
//       ^  |                 |             |             |
//       |  +--> Skipped      +-------------+-------------+--> Failed (deterministic, or retries
//       |  +--> Cancelled    |   record_failure(retry) / release_job /     exhausted by policy)
//       +--------------------+   recover_job(Requeue)        --> Cancelled (cancel_job)
//
//   Rendering, Verifying and Publishing are the "leased" stages: a job in one of them has exactly
//   one lease owner. Complete, Failed, Skipped and Cancelled are terminal (retry_failed_jobs()
//   puts Failed jobs back to Queued on request).
//
// Automatic batches (schema v5, AutoMode). Their jobs also carry an AutoJobState that moves
// forward only: Pending (the recipe the analysis starts from is frozen; the identity is
// provisional) -> Analysed (record_job_analysis: the analysis document is durable before anything
// is rendered) -> Frozen (freeze_auto_job: revision, settings SHA-256 and identity are rebound,
// once, to the effective automatic revision; only then is anything published). The stages above
// are unchanged: an attempt that analyses or freezes a job holds it in Rendering under its lease,
// and a job may be released between the states (the related-shot pre-pass). A job of an
// AnalyzeOnly batch ends Complete without a receipt (complete_without_output): its revisions are
// its result. Analysis records (one per automatic revision) are kept in auto_analyses.
//
// Persistence. Enumerations are stored as the text of their to_string() spellings and error
// codes as their numeric lucent::ErrorCode values; both are persisted contracts (append only).
// Timestamps are supplied by the caller (the coordinator's clock), never read inside the catalog,
// so lease expiry is deterministic and testable.
//
// All types are plain values: safe to copy and to read concurrently.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lucent/catalog/types.hpp"
#include "lucent/core/error.hpp"
#include "lucent/core/hash.hpp"

namespace lucent::catalog {

struct BatchId {
    std::int64_t value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value > 0; }
    friend constexpr bool operator==(BatchId, BatchId) noexcept = default;
    friend constexpr auto operator<=>(BatchId, BatchId) noexcept = default;
};

struct JobId {
    std::int64_t value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value > 0; }
    friend constexpr bool operator==(JobId, JobId) noexcept = default;
    friend constexpr auto operator<=>(JobId, JobId) noexcept = default;
};

struct ReceiptId {
    std::int64_t value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value > 0; }
    friend constexpr bool operator==(ReceiptId, ReceiptId) noexcept = default;
    friend constexpr auto operator<=>(ReceiptId, ReceiptId) noexcept = default;
};

// Persisted as "draft", "running", "paused", "cancelled", "completed".
enum class BatchState : std::uint8_t {
    Draft = 1,      // created; no job is dispatched until start_batch()
    Running = 2,    // queued jobs are dispatched (claim_next_job)
    Paused = 3,     // no new dispatch; leased jobs may finish their current safe stage
    Cancelled = 4,  // queued jobs were cancelled; leased jobs are asked to stop (terminal)
    Completed = 5,  // every job reached a terminal stage (retry_failed_jobs() may reopen it)
};

// Persisted as "queued", "rendering", "verifying", "publishing", "complete", "failed", "skipped",
// "cancelled".
enum class JobStage : std::uint8_t {
    Queued = 1,
    Rendering = 2,
    Verifying = 3,
    Publishing = 4,
    Complete = 5,
    Failed = 6,
    Skipped = 7,
    Cancelled = 8,
};

// Persisted as "transient", "deterministic". Transient failures (I/O, a worker crash or timeout,
// out of memory, a busy destination) may succeed when retried; deterministic ones (an unsupported
// or corrupt file, a decoder error) will not, and are bounded separately by the policy.
enum class FailureClass : std::uint8_t {
    Transient = 1,
    Deterministic = 2,
};

// Persisted as "reserved", "temp_written", "verified", "published", "receipt_committed",
// "abandoned".
enum class PublicationStep : std::uint8_t {
    Reserved = 1,          // a final name was chosen (and its no-clobber reservation made)
    TempWritten = 2,       // the temporary sibling was written, flushed and closed
    Verified = 3,          // the temporary file was reopened and verified
    Published = 4,         // the temporary file was moved to the final name (no clobber)
    ReceiptCommitted = 5,  // the receipt is in the catalog (written by record_receipt / recover_job)
    Abandoned = 6,         // reconciliation cleaned up an interrupted publication (abandon_publication)
};

// Automatic settings of a batch (schema v5, W8 application wiring; blueprint section 2, "Automatic
// batch development"). Persisted as "auto", "analyze_only"; None is stored as NULL.
enum class AutoMode : std::uint8_t {
    None = 0,         // an export batch: every job renders the recipe frozen at submission
    Auto = 1,         // each job is analysed, frozen as an automatic revision, checked and exported
    AnalyzeOnly = 2,  // each job is analysed and frozen as an automatic revision; nothing is exported
};

// Where an automatic job stands (schema v5). Persisted as "pending", "analysed", "frozen"; None
// (a job of an export batch) is stored as NULL. It only moves forward: Pending -> Analysed ->
// Frozen (a trigger refuses anything else).
enum class AutoJobState : std::uint8_t {
    None = 0,
    // Not analysed yet. The job's revision and settings_sha256 are the recipe the analysis starts
    // from (its locks and locked values), and its identity is provisional: no output is published
    // before the job is frozen.
    Pending = 1,
    // JobRecord::analysis_json holds the analysis (the application's document), persisted before
    // anything is rendered, so the related-shot pre-pass and a restart reuse it.
    Analysed = 2,
    // The job's revision, settings_sha256 and identity are those of the effective automatic
    // revision (WriteTransaction::freeze_auto_job): the export identity is bound to it.
    Frozen = 3,
};

// The outcome of the full-resolution final check of an automatic revision (schema v5).
// Persisted as "passed", "failed", "not_run".
enum class AutoCheckOutcome : std::uint8_t {
    Passed = 1,  // the revision's full-resolution render passed the hard checks
    Failed = 2,  // it failed them; a conservative fallback revision follows it
    NotRun = 3,  // not checked (an analysis kept without the final check)
};

// Spellings above; "invalid" for out-of-range values.
[[nodiscard]] std::string_view to_string(BatchState value) noexcept;
[[nodiscard]] std::string_view to_string(JobStage value) noexcept;
[[nodiscard]] std::string_view to_string(FailureClass value) noexcept;
[[nodiscard]] std::string_view to_string(PublicationStep value) noexcept;
// "none", "auto", "analyze_only".
[[nodiscard]] std::string_view to_string(AutoMode value) noexcept;
// "none", "pending", "analysed", "frozen".
[[nodiscard]] std::string_view to_string(AutoJobState value) noexcept;
[[nodiscard]] std::string_view to_string(AutoCheckOutcome value) noexcept;

// Complete, Failed, Skipped and Cancelled.
[[nodiscard]] constexpr bool is_terminal(JobStage stage) noexcept {
    return stage == JobStage::Complete || stage == JobStage::Failed || stage == JobStage::Skipped ||
           stage == JobStage::Cancelled;
}
// Rendering, Verifying and Publishing: the job has a lease owner.
[[nodiscard]] constexpr bool is_leased(JobStage stage) noexcept {
    return stage == JobStage::Rendering || stage == JobStage::Verifying || stage == JobStage::Publishing;
}

// Limits (UTF-8 bytes unless stated). Inputs above them are LimitExceeded, except error texts,
// which are truncated at a code-point boundary (recording a failure must not fail on its message).
inline constexpr std::size_t kMaxBatchJobs = 1'000'000;
inline constexpr std::size_t kMaxBatchTextBytes = 4096;               // name, selection description
inline constexpr std::size_t kMaxBatchJsonBytes = std::size_t{1} << 20;  // each JSON document, 1 MiB
inline constexpr std::size_t kMaxJobLabelBytes = 256;                 // lease owner, renderer version
inline constexpr std::size_t kMaxErrorTextBytes = 4096;               // error message, where
inline constexpr std::chrono::milliseconds kMinLeaseDuration{1'000};
inline constexpr std::chrono::milliseconds kMaxLeaseDuration{60 * 60 * 1'000};

// ---------------------------------------------------------------------------------------------
// Job identity
// ---------------------------------------------------------------------------------------------

// The inputs of a job's identity (blueprint section 7: "source content hash + subimage + recipe
// revision/hash + renderer and asset versions + export settings"). Two jobs with the same
// identity produce the same output bytes; a receipt with the identity of a new job is evidence
// that its output already exists (after the file is verified against the receipt).
struct JobIdentityInput {
    core::Digest256 source_sha256{};    // content hash of the source file
    std::uint32_t subimage = 0;         // image of the file
    core::Digest256 settings_sha256{};  // SHA-256 of the frozen develop settings document
    std::string renderer_version;       // engine / renderer build that renders the job
    std::string assets_json;            // versions of the assets the render uses (application JSON)
    core::Digest256 export_settings_sha256{};  // SHA-256 of the batch's export settings JSON text
};

// SHA-256 over a versioned, length-prefixed encoding of the input ("lucent.job-identity" v1;
// each text field is preceded by its byte length as 8 little-endian bytes, the subimage is
// 4 little-endian bytes). Text is hashed as given: callers that want equal settings to give equal
// identities serialise their JSON canonically. Fails only when hashing fails.
[[nodiscard]] Expected<core::Digest256> compute_job_identity(const JobIdentityInput& input);

// ---------------------------------------------------------------------------------------------
// Batches
// ---------------------------------------------------------------------------------------------

struct NewBatch {
    std::string name;                   // shown in the batch list; 0 ... 4096 bytes
    std::string selection_description;  // what was selected ("12 picked photos in Iceland 2026")
    std::string destination_json;       // destination specification (folder, naming, collisions)
    std::string export_settings_json;   // format, profile, size, metadata; part of every identity
    std::string policy_json = "{}";     // retry, memory and failure policy of the coordinator
    std::string renderer_version;       // part of every job identity; 1 ... 256 bytes
    Timestamp created_at{};
    bool start = false;  // create Running instead of Draft
    // Automatic settings (schema v5): with Auto or AnalyzeOnly every job starts AutoJobState::Pending.
    AutoMode automatic = AutoMode::None;

    // InvalidArgument ("catalog.batch") for text that is not valid UTF-8 or an empty renderer
    // version; LimitExceeded above the limits. JSON validity is checked when stored.
    [[nodiscard]] Status validate() const;
};

// One job of a new batch.
struct NewJob {
    ItemId item;
    // The frozen recipe: the revision the job renders (std::nullopt: the as-shot settings) and the
    // SHA-256 of the settings document the job will render. When a revision is given, the hash
    // must equal that revision's settings_sha256 (InvalidArgument otherwise), so a recipe cannot
    // change between freezing and dispatch unnoticed.
    std::optional<RevisionId> revision;
    core::Digest256 settings_sha256{};
    std::string assets_json = "{}";  // asset versions of this job (camera / lens profiles, models)
};

struct CreatedBatch {
    BatchId id;
    std::vector<JobId> jobs;  // in the order of the NewJob span; ordinals 0, 1, 2, ...
};

// Jobs of a batch per stage, computed from the jobs (never stored, so it cannot drift).
struct BatchCounts {
    std::uint64_t queued = 0;
    std::uint64_t rendering = 0;
    std::uint64_t verifying = 0;
    std::uint64_t publishing = 0;
    std::uint64_t complete = 0;
    std::uint64_t failed = 0;
    std::uint64_t skipped = 0;
    std::uint64_t cancelled = 0;

    [[nodiscard]] constexpr std::uint64_t leased() const noexcept { return rendering + verifying + publishing; }
    [[nodiscard]] constexpr std::uint64_t pending() const noexcept { return queued + leased(); }
    [[nodiscard]] constexpr std::uint64_t total() const noexcept {
        return pending() + complete + failed + skipped + cancelled;
    }
    friend bool operator==(const BatchCounts&, const BatchCounts&) = default;
};

struct BatchRecord {
    BatchId id;
    std::string name;
    std::string selection_description;
    std::string destination_json;
    std::string export_settings_json;
    core::Digest256 export_settings_sha256{};
    std::string policy_json;
    std::string renderer_version;
    BatchState state = BatchState::Draft;
    Timestamp created_at{};
    Timestamp updated_at{};
    std::optional<Timestamp> started_at;   // first start_batch() (or creation with start)
    std::optional<Timestamp> finished_at;  // Completed, or Cancelled with no leased job left
    // When its completion was handled (report files written, or none wanted): mark_batch_reported.
    // std::nullopt while not finished, or finished but not handled yet (schema v4).
    std::optional<Timestamp> reported_at;
    std::uint64_t job_count = 0;
    BatchCounts counts;
    AutoMode automatic = AutoMode::None;  // schema v5
};

// Jobs of an automatic batch that are not terminal (queued or leased), by AutoJobState
// (ReadTransaction::auto_job_counts): the related-shot pre-pass waits while any is Pending.
struct AutoJobCounts {
    std::uint64_t pending = 0;
    std::uint64_t analysed = 0;
    std::uint64_t frozen = 0;
    // Jobs of the batch, in any stage, whose analysis document is recorded.
    std::uint64_t documents = 0;
    friend bool operator==(const AutoJobCounts&, const AutoJobCounts&) = default;
};

// Facts about a batch's jobs beyond their stages (ReadTransaction::batch_job_facts).
struct BatchJobFacts {
    // Failed jobs that retry_failed_jobs() requeues: those without a receipt (a job whose receipt
    // committed but whose output was lost cannot publish a second one: export the photo again).
    std::uint64_t failed_retryable = 0;
    // Receipts of Complete jobs whose settings_json holds a non-empty JSON array at the path the
    // caller named (the application's review notes).
    std::uint64_t flagged_receipts = 0;
    // Complete jobs without a receipt (an analyse-only batch's) whose effective automatic revision's
    // analysis record recommends a review (schema v5).
    std::uint64_t flagged_analyses = 0;
};

// ---------------------------------------------------------------------------------------------
// Jobs
// ---------------------------------------------------------------------------------------------

// The last failure (or skip reason) recorded for a job.
struct JobError {
    std::optional<FailureClass> failure_class;  // std::nullopt for a skip or cancellation reason
    ErrorCode code = ErrorCode::Unknown;        // a code this build does not know reads as Unknown
    std::string where;
    std::string message;
    std::int64_t native_code = 0;
    Timestamp at{};
};

// Proof of a claim, presented with every change made on behalf of the worker.
struct JobLease {
    JobId job;
    std::string owner;
    std::uint32_t attempt = 0;  // the job's attempt number when it was claimed (1, 2, ...)
    Timestamp expires_at{};
    friend bool operator==(const JobLease&, const JobLease&) = default;
};

struct JobRecord {
    JobId id;
    BatchId batch;
    std::uint32_t ordinal = 0;   // position in the batch (dispatch order)
    std::optional<ItemId> item;  // std::nullopt once the item was removed from the catalog
    std::uint32_t subimage = 0;
    // The frozen revision; std::nullopt for as-shot settings, or once the item (and with it the
    // revision) was removed.
    std::optional<RevisionId> revision;
    core::Digest256 settings_sha256{};
    core::Digest256 source_sha256{};
    std::string assets_json;
    core::Digest256 identity_sha256{};
    JobStage stage = JobStage::Queued;
    std::uint32_t attempts = 0;  // claims so far
    std::uint32_t transient_failures = 0;
    std::uint32_t deterministic_failures = 0;
    // Attempts that ended without an outcome (recover_job with JobRecovery::interruption): not
    // counted in transient_failures (schema v4).
    std::uint32_t interruptions = 0;
    std::optional<JobError> last_error;
    std::optional<JobLease> lease;  // set exactly in the leased stages
    std::optional<Timestamp> heartbeat_at;
    Timestamp not_before{};  // a queued job is not claimed before this time (retry back-off)
    std::optional<std::filesystem::path> temp_path;   // latest temporary output (journal)
    // Latest reserved / published name (journal); for a job skipped because its output already
    // exists, that output (skip_job).
    std::optional<std::filesystem::path> final_path;
    Timestamp created_at{};
    Timestamp updated_at{};
    // Automatic settings (schema v5): the job's state and, from Analysed on, the analysis document
    // (opaque application JSON).
    AutoJobState auto_state = AutoJobState::None;
    std::optional<std::string> analysis_json;
};

// Filter of list_jobs(): jobs in id order (which is ordinal order within a batch).
struct JobQuery {
    std::optional<BatchId> batch;  // all batches when unset
    std::vector<JobStage> stages;  // all stages when empty
    JobId after;                   // ids greater than this (0: from the start)
    std::uint32_t limit = 1000;    // 1 ... kMaxPageSize
    // Only jobs in one of these automatic states (schema v5; AutoJobState::None: jobs of export
    // batches); all when empty.
    std::vector<AutoJobState> auto_states;
};

struct ClaimRequest {
    std::string owner;  // 1 ... 256 bytes; unique per worker slot and coordinator session
    Timestamp now{};
    std::chrono::milliseconds lease_duration{60'000};  // kMinLeaseDuration ... kMaxLeaseDuration
    std::optional<BatchId> batch;                       // only this batch when set
    // Only jobs in one of these automatic states (empty: any). The coordinator claims the Pending
    // jobs of a batch that keeps related shots consistent before any analysed one.
    std::vector<AutoJobState> auto_states;
};

struct ClaimedJob {
    JobLease lease;
    JobRecord job;  // as it is after the claim (stage Rendering)
};

struct LeaseRenewal {
    Timestamp expires_at{};
    BatchState batch_state = BatchState::Running;
    // The batch was cancelled: stop at the next safe boundary and call cancel_job() (a job that has
    // published its output finishes with record_receipt and Complete instead).
    bool cancel_requested = false;
};

// A failure reported by the worker or coordinator for a leased job.
struct JobFailure {
    FailureClass failure_class = FailureClass::Transient;
    Error error;
    // Requeue the job, not to be claimed before this time; std::nullopt: the job fails (the
    // coordinator's policy decided that it must not be retried). The policy reads the counts in
    // JobRecord (attempts, transient_failures, deterministic_failures).
    std::optional<Timestamp> retry_at;
    // The job is Skipped instead of Failed, `error` being the reason (for example a photograph
    // found unsupported while decoding: it was not exported, but that is no failure of the batch
    // and retrying cannot help). Counted as a failure of its class; retry_at is ignored.
    bool skip = false;
};

// ---------------------------------------------------------------------------------------------
// Publication journal and receipts
// ---------------------------------------------------------------------------------------------

struct PublicationEntry {
    PublicationStep step = PublicationStep::Reserved;  // Reserved ... Published (not the last two)
    std::optional<std::filesystem::path> temp_path;    // required from TempWritten on
    std::optional<std::filesystem::path> final_path;   // required for every step
    std::optional<core::Digest256> output_sha256;      // required from Verified on
    std::optional<std::uint64_t> output_size;          // required from Verified on
};

struct PublicationRecord {
    std::int64_t id = 0;  // increases with every entry
    JobId job;
    std::uint32_t attempt = 0;
    PublicationStep step = PublicationStep::Reserved;
    Timestamp at{};
    std::optional<std::filesystem::path> temp_path;
    std::optional<std::filesystem::path> final_path;
    std::optional<core::Digest256> output_sha256;
    std::optional<std::uint64_t> output_size;
};

struct NewReceipt {
    std::filesystem::path final_path;
    std::uint64_t size_bytes = 0;
    core::Digest256 output_sha256{};
    std::string settings_json = "{}";  // what the output was rendered with (report / audit)
};

struct ReceiptRecord {
    ReceiptId id;
    JobId job;
    core::Digest256 identity_sha256{};
    std::filesystem::path final_path;
    std::uint64_t size_bytes = 0;
    core::Digest256 output_sha256{};
    std::string settings_json;
    Timestamp created_at{};
};

// ---------------------------------------------------------------------------------------------
// Events and reconciliation
// ---------------------------------------------------------------------------------------------

struct JobEvent {
    std::int64_t id = 0;  // increases with every event
    JobId job;
    BatchId batch;
    Timestamp at{};
    std::optional<JobStage> from;  // std::nullopt for the creation event
    JobStage to = JobStage::Queued;
    std::uint32_t attempt = 0;
    std::optional<std::string> lease_owner;
    std::optional<JobError> error;  // the failure or reason that caused the transition
};

struct EventQuery {
    std::optional<BatchId> batch;
    std::optional<JobId> job;
    std::int64_t after = 0;  // event ids greater than this
    std::uint32_t limit = 1000;
};

// Leased jobs whose owner can no longer be trusted to finish them.
struct ReclaimQuery {
    Timestamp now{};
    // When set, leases of owners not in this list are reclaimable even before they expire (at
    // start-up: an empty list, since no worker of the previous session survives). When unset,
    // only expired leases are returned.
    std::optional<std::vector<std::string>> live_owners;
    std::optional<BatchId> batch;
    std::uint32_t limit = 1000;
};

enum class RecoveryAction : std::uint8_t {
    Requeue = 1,   // back to Queued (Cancelled when the batch was cancelled); counts as a failure
    Fail = 2,      // Failed with the given error
    Complete = 3,  // Complete: the output was verified against the receipt (or the given one)
};

struct JobRecovery {
    RecoveryAction action = RecoveryAction::Requeue;
    Timestamp now{};
    // Requeue / Fail: why (for example Timeout "the worker stopped responding").
    Error error{ErrorCode::Timeout, "the worker's lease was abandoned", "workers.lease"};
    FailureClass failure_class = FailureClass::Transient;
    std::optional<Timestamp> retry_at;  // Requeue: not before this time (default: now)
    // Complete without a receipt in the catalog: the receipt to write (the caller verified the
    // published file against the journal's output hash).
    std::optional<NewReceipt> receipt;
    // The owner is known to be gone (its worker process exited, or a previous session): the lease
    // may be recovered before it expires. Otherwise an unexpired lease is refused.
    bool owner_gone = false;
    // Requeue / Fail: the attempt was interrupted (its coordinator stopped, or the lease expired)
    // rather than failed: JobRecord::interruptions is incremented instead of the counter of
    // failure_class, so the retry budget of genuine failures is kept.
    bool interruption = false;
};

// The latest journal entry of an attempt whose publication neither committed a receipt nor was
// abandoned.
struct PublicationInProgress {
    PublicationRecord latest;
    BatchId batch;
    JobStage job_stage = JobStage::Queued;
    std::uint32_t job_attempts = 0;
    // The job is still leased for this attempt: its worker may still be publishing.
    [[nodiscard]] bool attempt_is_live() const noexcept {
        return is_leased(job_stage) && job_attempts == latest.attempt;
    }
};

// ---------------------------------------------------------------------------------------------
// Automatic settings (schema v5)
// ---------------------------------------------------------------------------------------------

// Largest analysis document of a job and record document of an automatic revision (UTF-8 bytes).
inline constexpr std::size_t kMaxAnalysisJsonBytes = std::size_t{2} << 20;  // 2 MiB

// What WriteTransaction::freeze_auto_job binds an analysed job to.
struct AutoFreeze {
    RevisionId revision;                // the effective automatic revision (origin Auto, the job's item)
    core::Digest256 settings_sha256{};  // its settings_sha256 (checked)
    // The final analysis (after related-shot regularisation and the final check); std::nullopt keeps
    // the analysed one.
    std::optional<std::string> analysis_json;
};

struct AutoAnalysisId {
    std::int64_t value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value > 0; }
    friend constexpr bool operator==(AutoAnalysisId, AutoAnalysisId) noexcept = default;
    friend constexpr auto operator<=>(AutoAnalysisId, AutoAnalysisId) noexcept = default;
};

// The analysis record of one automatic revision (blueprint section 8: "policy/search version ...
// and diagnostic results"; never an opaque "auto enhanced" flag). The versions, the attempt, the
// outcome of the final check, the confidence and whether a review is recommended are columns, so
// they can be queried; the flags with their reasons and the compact diagnostics are the
// application's JSON documents.
struct NewAutoAnalysis {
    RevisionId revision;        // origin Auto, one record per revision
    std::optional<JobId> job;   // the batch job that produced it (std::nullopt: the interactive "Auto")
    std::string policy;         // e.g. "natural"; 1 ... 256 bytes
    std::uint32_t policy_version = 0;            // >= 1
    std::uint32_t search_version = 0;            // >= 1
    std::uint32_t metric_contract_version = 0;   // >= 1
    // The related-shot consistency version when the revision was regularised within a group.
    std::optional<std::uint32_t> consistency_version;
    std::uint32_t develop_pipeline_version = 0;  // >= 1
    std::uint32_t develop_settings_version = 0;  // >= 1
    // 0: the optimiser's choice; n >= 1: the n-th conservative fallback after a failed final check.
    std::uint32_t attempt = 0;
    AutoCheckOutcome final_check = AutoCheckOutcome::NotRun;
    double confidence = 0.0;    // [0, 1]
    bool review = false;        // a review is recommended (low confidence, a fallback, locks, ...)
    std::string flags_json = "[]";   // a JSON array (the application's review flags with reasons)
    std::string record_json = "{}";  // the application's record document
    Timestamp created_at{};

    // InvalidArgument ("catalog.auto_analysis") for an empty or overlong policy, a version of 0, a
    // confidence outside [0, 1], an unknown outcome or text that is not UTF-8; LimitExceeded above
    // kMaxAnalysisJsonBytes. JSON validity is checked when stored.
    [[nodiscard]] Status validate() const;
};

struct AutoAnalysisRecord {
    AutoAnalysisId id;
    RevisionId revision;
    ItemId item;
    std::optional<JobId> job;
    std::string policy;
    std::uint32_t policy_version = 0;
    std::uint32_t search_version = 0;
    std::uint32_t metric_contract_version = 0;
    std::optional<std::uint32_t> consistency_version;
    std::uint32_t develop_pipeline_version = 0;
    std::uint32_t develop_settings_version = 0;
    std::uint32_t attempt = 0;
    AutoCheckOutcome final_check = AutoCheckOutcome::NotRun;
    double confidence = 0.0;
    bool review = false;
    std::string flags_json;
    std::string record_json;
    Timestamp created_at{};
};

}  // namespace lucent::catalog
